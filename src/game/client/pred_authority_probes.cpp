//=============================================================================//
//
// Purpose: Default-off prediction-authority probes. Self-gated; read-only.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "datamap.h"
#include "const.h"
#include "game/client/pred_authority.h"
#include "game/client/pred_authority_probes.h"
#include "game/client/pred_diag.h"

// [PRED-CENSUS] per-kind offender census (see PredAuth_CensusObserve). Default 0 for ship;
// enable for weapon/viewmodel predictable hunts the ent-1 [PRED-VERDICT] cannot see.
ConVar sdk_pred_ent_census("sdk_pred_ent_census", "0", FCVAR_DEVELOPMENTONLY,
	"[PRED-CENSUS] tally the field NAMES that still diverge post-mask on WEAPON/VIEWMODEL "
	"predictables (the ones ent-1 [PRED-VERDICT] cannot see), dumped per-kind. Finds the "
	"remaining server-authoritative offenders without guessing. 0=off (default), 1=on.");

//-----------------------------------------------------------------------------
// Kick decay stamps vs latestPredictedTime (or m_attackTimeThisFrame while firing).
//-----------------------------------------------------------------------------
static bool KickRow_IsWatched(const char* name)
{
	if (!name)
		return false;
	static const char* const kNames[] = {
		"m_kickPatternScaleBase",
		"m_kickScaleBasePitch",
		"m_kickScaleBaseYaw",
		"m_kickSpreadHipfire",
		"m_lastPrimaryAttackTime",
		"m_attackTimeThisFrame",
		"m_kickTime",
		"m_kickSpringHeatBaseTime",
	};
	for (const char* n : kNames)
	{
		if (!strcmp(name, n))
			return true;
	}
	return false;
}

// Residual class for d=pred-srv (lag2 ~-2 multi-step; frac ~-2.83 decay;
// hard |d|>=3; rev pred-ahead / firstshot reset).
enum KickRowClass_t
{
	KICKCLS_NOISE = 0,
	KICKCLS_LAG1,
	KICKCLS_LAG2,
	KICKCLS_FRAC,
	KICKCLS_HARD,
	KICKCLS_REV,
};

static KickRowClass_t KickRow_Classify(float d, float ad)
{
	if (d > 0.05f)
		return KICKCLS_REV;
	if (ad >= 3.0f)
		return KICKCLS_HARD;
	if (ad >= 2.5f)
		return KICKCLS_FRAC;
	if (ad >= 1.5f)
		return KICKCLS_LAG2;
	if (ad >= 0.5f)
		return KICKCLS_LAG1;
	return KICKCLS_NOISE;
}

static const char* KickRow_ClassName(KickRowClass_t c)
{
	switch (c)
	{
	case KICKCLS_LAG1: return "lag1";
	case KICKCLS_LAG2: return "lag2";
	case KICKCLS_FRAC: return "frac";
	case KICKCLS_HARD: return "hard";
	case KICKCLS_REV:  return "rev";
	default:           return "noise";
	}
}

const char* PredAuth_KickRowClassName(float d, float ad)
{
	return KickRow_ClassName(KickRow_Classify(d, ad));
}

// Pre-equalize histogram: equalized vs residual classes.
// Measure-only -- does N=2 collapse pure lag2 without hiding hard/rev?
struct KickRowHist_s
{
	uint32_t nTot;
	uint32_t nEq1;
	uint32_t nEq2;
	uint32_t nEqOther;
	uint32_t nResLag2;
	uint32_t nResFrac;
	uint32_t nResHard;
	uint32_t nResRev;
	uint32_t nResOther;
};
static KickRowHist_s s_kickHist = {};

void PredAuth_KickRowHistNote(float d, float ad, float flKickLag, bool bEqualized)
{
	if (!bridge_kick_row_tap.GetBool())
		return;

	static float s_histKickLag = -1.0f;
	if (s_histKickLag != flKickLag)
	{
		s_histKickLag = flKickLag;
		s_kickHist = KickRowHist_s{};
	}
	++s_kickHist.nTot;
	if (bEqualized)
	{
		if (ad <= 1.5f)
			++s_kickHist.nEq1;
		else if (ad <= 2.5f)
			++s_kickHist.nEq2;
		else
			++s_kickHist.nEqOther;
	}
	else
	{
		const KickRowClass_t cls = KickRow_Classify(d, ad);
		if (cls == KICKCLS_REV)
			++s_kickHist.nResRev;
		else if (cls == KICKCLS_HARD)
			++s_kickHist.nResHard;
		else if (cls == KICKCLS_FRAC)
			++s_kickHist.nResFrac;
		else if (cls == KICKCLS_LAG2)
			++s_kickHist.nResLag2;
		else
			++s_kickHist.nResOther;
	}

	// Window dump: enough samples for a spray mag under remote RTT.
	if (s_kickHist.nTot >= 64 && (s_kickHist.nTot % 64u) == 0)
	{
		Warning(eDLL_T::CLIENT,
			"[KICK-ROW-HIST] n=%u N=%.0f eq{d1=%u d2=%u other=%u} "
			"res{lag2=%u frac=%u hard=%u rev=%u other=%u} "
			"ackCount=%d latest=%d\n",
			s_kickHist.nTot, flKickLag,
			s_kickHist.nEq1, s_kickHist.nEq2, s_kickHist.nEqOther,
			s_kickHist.nResLag2, s_kickHist.nResFrac, s_kickHist.nResHard,
			s_kickHist.nResRev, s_kickHist.nResOther,
			PredAuth_PnrAckCount(), PredAuth_PnrLatest());
	}
}

// Census tables. Field-name pointers are stable keys.
struct CensusHit_s { const char* name; uint32_t n; };
enum { kCensusKinds = 2, kCensusSlots = 32 };
static CensusHit_s s_censusTop[kCensusKinds][kCensusSlots] = {};
static CensusHit_s s_censusFirst[kCensusKinds][kCensusSlots] = {};  // first-in-flat-order only
static CensusHit_s s_censusUnder[kCensusKinds][kCensusSlots] = {};  // differ but inside tolerance
static uint32_t    s_censusObs[kCensusKinds] = {};
static uint32_t    s_censusErrObs[kCensusKinds] = {};

static void PredAuth_CensusNoteRow(CensusHit_s* const row, const char* name)
{
	if (!name)
		return;
	for (int i = 0; i < kCensusSlots; ++i)
	{
		if (row[i].name == name) { ++row[i].n; return; }
		if (!row[i].name)        { row[i].name = name; row[i].n = 1; return; }
	}
	// full: replace the lowest-count slot with this new name.
	uint32_t lo = UINT32_MAX; int loI = 0;
	for (int i = 0; i < kCensusSlots; ++i)
		if (row[i].n < lo) { lo = row[i].n; loI = i; }
	row[loI].name = name; row[loI].n = 1;
}

//-----------------------------------------------------------------------------
// After PredAuth_Apply: residual weapon/viewmodel diffs. Members are server values at PNR.
//-----------------------------------------------------------------------------
void PredAuth_CensusObserve(void* pEntity, unsigned int nCmd)
{
	if (!sdk_pred_ent_census.GetBool() || !pEntity || !C_BaseEntity__GetPredictedEntityState)
		return;

	const int kind = PredAuth_GetDmapKind(pEntity);
	if (kind != AUTH_SCOPE_WEAPON && kind != AUTH_SCOPE_VIEWMODEL)
		return; // player is already covered by ent-1 [PRED-VERDICT]/[PRED-BOARD]
	const int ki = (kind == AUTH_SCOPE_WEAPON) ? 0 : 1;

	__try
	{
		if (!EntField<unsigned char>(pEntity, ENT_PREDICTED_FLAG))
			return;
		void* pPredState = C_BaseEntity__GetPredictedEntityState(pEntity, nCmd);
		if (!pPredState)
			return;
		const uint8_t* pPredicted = EntField<uint8_t*>(pPredState, STATE_SERIALIZED_DATA);
		const uintptr_t states = EntField<uintptr_t>(pEntity, ENT_SAVED_STATES);
		if (!pPredicted || !states)
			return;

		// Refresh originalData from the live members (idempotent with the orig's own
		// SaveData(-1)) so the census reads exactly what the native compare will read.
		if (PFN_PredSaveData pSave = PredAuth_SaveData())
			pSave(pEntity, "sdkcensus", static_cast<int>(nCmd), -1);

		const uintptr_t originalData = states + STATES_ORIGINALDATA;
		if (!*reinterpret_cast<unsigned char*>(originalData))
			return;
		const uint8_t* pServer = *reinterpret_cast<uint8_t**>(originalData + STATE_SERIALIZED_DATA);
		if (!pServer)
			return;

		void* (*GetPredDescMap)(void*) = reinterpret_cast<void* (*)(void*)>(
			(*reinterpret_cast<void***>(pEntity))[VIDX_GetPredDescMap]);
		const uintptr_t dmap = reinterpret_cast<uintptr_t>(GetPredDescMap(pEntity));
		if (!dmap)
			return;
		const uintptr_t opt = *reinterpret_cast<uintptr_t*>(dmap + DMAP_OPTIMIZED);
		if (!opt)
			return;
		const uintptr_t info   = opt + (uintptr_t)OPT_INFO_STRIDE * PC_NETWORKED_ONLY;
		const uintptr_t fields = *reinterpret_cast<uintptr_t*>(info + INFO_FLAT_FIELDS);
		const int count        = *reinterpret_cast<int*>(info + INFO_FLAT_COUNT);
		if (!fields || count <= 0 || count > 4096)
			return;

		bool bBlamed = false;
		for (int i = 0; i < count; ++i)
		{
			const uintptr_t td = fields + (uintptr_t)i * TD_STRIDE;
			const uint32_t flags = *reinterpret_cast<uint32_t*>(td + TD_FLAGS);
			if (flags & (FTYPEDESC_SKIP | FTYPEDESC_GRAPPLE))
				continue;
			const int type = *reinterpret_cast<int*>(td + TD_FIELDTYPE);
			const int off  = *reinterpret_cast<int*>(td + TD_FLATOFFSET1);
			const int n16  = *reinterpret_cast<uint16_t*>(td + TD_FIELDSIZE);
			const int cnt  = n16 ? n16 : 1;
			const float tol = *reinterpret_cast<float*>(td + TD_TOLERANCE);
			const uint8_t* p = pPredicted + off;
			const uint8_t* s = pServer + off;

			const PredNativeCmp_t cmp = PredNative_Compare(type, p, s, cnt, tol);
			if (cmp == PRED_CMP_SAME)
				continue;

			const char* name = *reinterpret_cast<const char**>(td + TD_FIELDNAME);
			if (!name)
				name = "<noname>";

			// Drift the engine forgives is still drift -- bucket it, never drop it.
			if (cmp == PRED_CMP_BELOW_TOL)
			{
				PredAuth_CensusNoteRow(s_censusUnder[ki], name);
				continue;
			}

			// The native compare BREAKS at its first error, so only the first
			// diverging field in flat order is ever the engine's stated reason
			// for the rebase. Tally that separately from the full offender set.
			if (!bBlamed)
			{
				bBlamed = true;
				++s_censusErrObs[ki];
				PredAuth_CensusNoteRow(s_censusFirst[ki], name);
			}
			PredAuth_CensusNoteRow(s_censusTop[ki], name);

			// Values, not just names: the row is derived offline as
			// floor(m_kickPatternScaleBase / viewkick_scale_valuePerShot).
			// Post-equalize residual only -- join class + per-snapshot ack count.
			if (ki == 0 && (type == FIELD_FLOAT || type == FIELD_TIME)
				&& bridge_kick_row_tap.GetBool() && KickRow_IsWatched(name))
			{
				static uint32_t s_nKickRow = 0;
				if (++s_nKickRow <= 600)
				{
					const float fs = *reinterpret_cast<const float*>(s);
					const float fp = *reinterpret_cast<const float*>(p);
					const float d = fp - fs;
					const float ad = fabsf(d);
					Warning(eDLL_T::CLIENT,
						"[KICK-ROW] cmd=%u %s srv=%.4f pred=%.4f d=%+.4f "
						"class=%s ackCount=%d latest=%d\n",
						nCmd, name, fs, fp, d,
						KickRow_ClassName(KickRow_Classify(d, ad)),
						PredAuth_PnrAckCount(), PredAuth_PnrLatest());
				}
			}
		}

		if (++s_censusObs[ki] >= 512)
		{
			const char* const pszKind = (ki == 0) ? "weapon" : "viewmodel";
			CensusHit_s* const rows[3] =
				{ s_censusTop[ki], s_censusFirst[ki], s_censusUnder[ki] };
			// blamed = what the engine would actually have rebased on; offenders =
			// every field that diverged; belowTol = drift the engine forgives.
			const char* const pszTag[3] = { "offenders", "blamed", "belowTol" };

			for (int r = 0; r < 3; ++r)
			{
				CensusHit_s* const row = rows[r];
				for (int a = 0; a < kCensusSlots - 1; ++a)
					for (int b = 0; b < kCensusSlots - 1 - a; ++b)
						if (row[b].n < row[b + 1].n)
						{
							const CensusHit_s t = row[b]; row[b] = row[b + 1]; row[b + 1] = t;
						}

				char buf[640]; size_t o = 0; buf[0] = '\0';
				for (int i = 0; i < kCensusSlots && i < 16 && row[i].name && row[i].n > 0; ++i)
				{
					const int w = snprintf(buf + o, sizeof(buf) > o ? sizeof(buf) - o : 0,
						"%s%s=%u", o ? " " : "", row[i].name, row[i].n);
					if (w > 0) o += (size_t)w;
				}
				if (r == 0)
					Warning(eDLL_T::CLIENT,
						"[PRED-CENSUS] kind=%s obs=%u errObs=%u tick=%.4f post-mask %s{%s}\n",
						pszKind, s_censusObs[ki], s_censusErrObs[ki],
						PredNative_TickInterval(), pszTag[r], buf[0] ? buf : "none");
				else if (buf[0])
					Warning(eDLL_T::CLIENT, "[PRED-CENSUS] kind=%s %s{%s}\n",
						pszKind, pszTag[r], buf);

				for (int i = 0; i < kCensusSlots; ++i) { row[i].name = nullptr; row[i].n = 0; }
			}

			s_censusObs[ki] = 0;
			s_censusErrObs[ki] = 0;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Rail alpha both sides. Speed not networked; FTYPEDESC_SKIP on m_slidingZiplineAlpha.
static ConVar bridge_zip_alpha_probe("bridge_zip_alpha_probe", "0", FCVAR_DEVELOPMENTONLY,
	"[ZIP-ALPHA] per-ack dump of the zipline rail parameter (pred vs server) plus the "
	"derived origin, while the local player is on a zipline. 0=off (default), 1=on.");

static ConVar bridge_zip_path_probe("bridge_zip_path_probe", "0", FCVAR_DEVELOPMENTONLY,
	"[ZIP-PATH] per-ack hang-vector compare of pred vs server against the shared "
	"rest-position polyline (same rail path?). 0=off (default), 1=on.");

struct ZipAlphaCache_t
{
	uintptr_t dmap;
	int slidP, slidS;      // m_slidingZiplineAlpha record / originalData offsets
	int mntP,  mntS;       // m_mountingZiplineAlpha
	int stateP, stateS;    // m_ziplineState
	int orgP,  orgS;       // m_localOrigin
	int dirP,  dirS;       // m_ziplinePathDirection
	int slidLive;          // live member (flatOffset[0]) of the sliding alpha
	int zipLive;           // live member of m_activeZipline (EHANDLE)
};
static ZipAlphaCache_t s_zipCache = {};

// S21 zipline entity: networked rest polyline both engines share for non-ziprail rides.
static constexpr ptrdiff_t kZipRestPositions = 0x0ECC; // m_ziplineRestPositions Vector[32]
static constexpr ptrdiff_t kZipRestCount     = 0x0F8C; // m_numZiplineRestPositions int

static bool ZipPath_ReadRestPoints(void* pZipEnt, float pts[32][3], int* outCount)
{
	if (!pZipEnt || !pts || !outCount)
		return false;
	const int n = *reinterpret_cast<const int*>(
		reinterpret_cast<const uint8_t*>(pZipEnt) + kZipRestCount);
	if (n < 2 || n > 32)
		return false;
	const float* src = reinterpret_cast<const float*>(
		reinterpret_cast<const uint8_t*>(pZipEnt) + kZipRestPositions);
	for (int i = 0; i < n; ++i)
	{
		pts[i][0] = src[i * 3 + 0];
		pts[i][1] = src[i * 3 + 1];
		pts[i][2] = src[i * 3 + 2];
	}
	*outCount = n;
	return true;
}

// Alpha is a chord-length fraction of the rest polyline, not an index.
static bool ZipPath_EvalPolylineAtAlpha(const float pts[32][3], int count, float alpha, float out[3],
	float* pTotal = nullptr)
{
	if (!pts || !out || count < 2 || count > 32)
		return false;

	float total = 0.f;
	float segLen[31];
	const int nSeg = count - 1;
	for (int s = 0; s < nSeg; ++s)
	{
		const float dx = pts[s + 1][0] - pts[s][0];
		const float dy = pts[s + 1][1] - pts[s][1];
		const float dz = pts[s + 1][2] - pts[s][2];
		segLen[s] = sqrtf(dx * dx + dy * dy + dz * dz);
		total += segLen[s];
	}
	if (pTotal)
		*pTotal = total;

	if (total <= 0.f)
	{
		out[0] = pts[0][0];
		out[1] = pts[0][1];
		out[2] = pts[0][2];
		return true;
	}

	float a = alpha;
	if (a < 0.f) a = 0.f;
	else if (a > 1.f) a = 1.f;
	float remain = a * total;

	out[0] = pts[count - 1][0];
	out[1] = pts[count - 1][1];
	out[2] = pts[count - 1][2];
	for (int s = 0; s < nSeg; ++s)
	{
		const float sl = segLen[s];
		if (remain <= sl || s == nSeg - 1)
		{
			const float t = (sl > 0.f) ? (remain / sl) : 0.f;
			const float tc = (t < 0.f) ? 0.f : ((t > 1.f) ? 1.f : t);
			out[0] = pts[s][0] + (pts[s + 1][0] - pts[s][0]) * tc;
			out[1] = pts[s][1] + (pts[s + 1][1] - pts[s][1]) * tc;
			out[2] = pts[s][2] + (pts[s + 1][2] - pts[s][2]) * tc;
			return true;
		}
		remain -= sl;
	}
	return true;
}

void PredAuth_ZipAlphaProbe(void* pEntity, unsigned int nCmd)
{
	if (!bridge_zip_alpha_probe.GetBool() || !pEntity || !C_BaseEntity__GetPredictedEntityState)
		return;

	__try
	{
		if (!EntField<unsigned char>(pEntity, ENT_PREDICTED_FLAG))
			return;

		void* const pPredState = C_BaseEntity__GetPredictedEntityState(pEntity, nCmd);
		if (!pPredState)
			return;
		const uint8_t* const pPredicted = EntField<uint8_t*>(pPredState, STATE_SERIALIZED_DATA);

		const uintptr_t states = EntField<uintptr_t>(pEntity, ENT_SAVED_STATES);
		if (!states)
			return;
		const uintptr_t originalData = states + STATES_ORIGINALDATA;
		if (!*reinterpret_cast<unsigned char*>(originalData))
			return;
		const uint8_t* const pServer = *reinterpret_cast<uint8_t**>(originalData + STATE_SERIALIZED_DATA);
		if (!pPredicted || !pServer)
			return;

		void* (*GetPredDescMap)(void*) = reinterpret_cast<void* (*)(void*)>(
			(*reinterpret_cast<void***>(pEntity))[VIDX_GetPredDescMap]);
		const uintptr_t dmap = reinterpret_cast<uintptr_t>(GetPredDescMap(pEntity));
		if (!dmap)
			return;

		if (s_zipCache.dmap != dmap)
		{
			ZipAlphaCache_t c = {};
			c.dmap = dmap;
			PredAuth_ResolveFlatOff(dmap, "m_slidingZiplineAlpha",  &c.slidLive, &c.slidS);
			c.slidP = c.slidS;
			PredAuth_ResolveFlatOff(dmap, "m_mountingZiplineAlpha", nullptr, &c.mntS);
			c.mntP = c.mntS;
			PredAuth_ResolveFlatOff(dmap, "m_ziplineState",         nullptr, &c.stateS);
			c.stateP = c.stateS;
			PredAuth_ResolveFlatOff(dmap, "m_localOrigin",          nullptr, &c.orgS);
			c.orgP = c.orgS;
			PredAuth_ResolveFlatOff(dmap, "m_ziplinePathDirection", nullptr, &c.dirS);
			c.dirP = c.dirS;
			PredAuth_ResolveFlatOff(dmap, "m_activeZipline", &c.zipLive, nullptr);
			s_zipCache = c;

			Warning(eDLL_T::CLIENT,
				"[ZIP-ALPHA] resolved on dmap=%p: slide=%d/live%d mount=%d state=%d org=%d dir=%d\n",
				reinterpret_cast<void*>(dmap), c.slidS, c.slidLive, c.mntS, c.stateS, c.orgS, c.dirS);
		}

		const ZipAlphaCache_t& c = s_zipCache;
		if (c.slidS < 0 || c.stateS < 0 || c.orgS < 0)
			return;

		const int stateP = *reinterpret_cast<const int*>(pPredicted + c.stateP);
		const int stateS = *reinterpret_cast<const int*>(pServer + c.stateS);
		if (stateP == 0 && stateS == 0)
			return;   // not on a rail on either side

		const float slidP = *reinterpret_cast<const float*>(pPredicted + c.slidP);
		const float slidS = *reinterpret_cast<const float*>(pServer + c.slidS);
		const float mntP  = (c.mntS >= 0) ? *reinterpret_cast<const float*>(pPredicted + c.mntP) : -1.f;
		const float mntS  = (c.mntS >= 0) ? *reinterpret_cast<const float*>(pServer + c.mntS) : -1.f;
		const float slidLive = (c.slidLive >= 0)
			? *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(pEntity) + c.slidLive) : -1.f;

		const float* const op = reinterpret_cast<const float*>(pPredicted + c.orgP);
		const float* const os = reinterpret_cast<const float*>(pServer + c.orgS);

		// d(alpha) per ack on each side is the rate; the two are only comparable
		// because both are normalized to the same rail.
		static float s_lastP = -1.f, s_lastS = -1.f;
		static unsigned s_lastCmd = 0;
		const float dP = (s_lastP >= 0.f && nCmd > s_lastCmd) ? (slidP - s_lastP) : 0.f;
		const float dS = (s_lastS >= 0.f && nCmd > s_lastCmd) ? (slidS - s_lastS) : 0.f;

		const float dOrg = fmaxf(fmaxf(fabsf(op[0] - os[0]), fabsf(op[1] - os[1])), fabsf(op[2] - os[2]));

		Warning(eDLL_T::CLIENT,
			"[ZIP-ALPHA] cmd=%u st=%d/%d slide=%.6f/%.6f d=%+.6f rate=%+.6f/%+.6f "
			"mount=%.4f/%.4f live=%.6f dOrg=%.3f predOrg=(%.2f %.2f %.2f) srvOrg=(%.2f %.2f %.2f)\n",
			nCmd, stateP, stateS, slidP, slidS, slidP - slidS, dP, dS,
			mntP, mntS, slidLive, dOrg, op[0], op[1], op[2], os[0], os[1], os[2]);

		// Hang vectors cancel the fixed player-under-rail offset; equal hangP/hangS
		// means both engines walk the same rest polyline at their own alphas.
		if (bridge_zip_path_probe.GetBool())
		{
			static uint32_t s_pathFaults = 0;
			do
			{
				if (c.zipLive < 0)
				{
					if (++s_pathFaults <= 4)
						Warning(eDLL_T::CLIENT,
							"[ZIP-PATH] handle invalid (m_activeZipline offset unresolved) #%u\n",
							s_pathFaults);
					break;
				}

				const uint32_t zipHandle = *reinterpret_cast<const uint32_t*>(
					reinterpret_cast<const uint8_t*>(pEntity) + c.zipLive);
				if (zipHandle == 0xFFFFFFFFu)
				{
					if (++s_pathFaults <= 4)
						Warning(eDLL_T::CLIENT,
							"[ZIP-PATH] handle invalid (0xFFFFFFFF) #%u\n", s_pathFaults);
					break;
				}

				// Resolve through the raw EntInfo array, not g_pClientEntityList --
				// that factory interface is a truncated vtable stub and is null here.
				const int zipEnt = static_cast<int>(zipHandle & ENT_ENTRY_MASK);
				void* const pZipEnt = const_cast<void*>(PredNative_ResolveEHandle(zipHandle));
				if (!pZipEnt)
				{
					if (++s_pathFaults <= 4)
						Warning(eDLL_T::CLIENT,
							"[ZIP-PATH] entity does not resolve (ent=%d) #%u\n",
							zipEnt, s_pathFaults);
					break;
				}

				float pts[32][3];
				int nPts = 0;
				if (!ZipPath_ReadRestPoints(pZipEnt, pts, &nPts))
				{
					if (++s_pathFaults <= 4)
						Warning(eDLL_T::CLIENT,
							"[ZIP-PATH] rest array unusable (ent=%d) #%u\n",
							zipEnt, s_pathFaults);
					break;
				}

				float railP[3], railS[3], pathLen = 0.f;
				if (!ZipPath_EvalPolylineAtAlpha(pts, nPts, slidP, railP, &pathLen)
					|| !ZipPath_EvalPolylineAtAlpha(pts, nPts, slidS, railS))
				{
					if (++s_pathFaults <= 4)
						Warning(eDLL_T::CLIENT,
							"[ZIP-PATH] rest array unusable (polyline eval failed ent=%d n=%d) #%u\n",
							zipEnt, nPts, s_pathFaults);
					break;
				}

				const float hangP[3] = { op[0] - railP[0], op[1] - railP[1], op[2] - railP[2] };
				const float hangS[3] = { os[0] - railS[0], os[1] - railS[1], os[2] - railS[2] };
				const float dHang = fmaxf(
					fmaxf(fabsf(hangP[0] - hangS[0]), fabsf(hangP[1] - hangS[1])),
					fabsf(hangP[2] - hangS[2]));

				Warning(eDLL_T::CLIENT,
					"[ZIP-PATH] cmd=%u ent=%d n=%d alpha=%.6f/%.6f pathLen=%.1f dArc=%.3f dHang=%.3f "
					"hangP=(%.2f %.2f %.2f) hangS=(%.2f %.2f %.2f) "
					"railS=(%.2f %.2f %.2f) first=(%.1f %.1f %.1f) last=(%.1f %.1f %.1f)\n",
					nCmd, zipEnt, nPts, slidP, slidS, pathLen, (slidP - slidS) * pathLen, dHang,
					hangP[0], hangP[1], hangP[2],
					hangS[0], hangS[1], hangS[2],
					railS[0], railS[1], railS[2],
					pts[0][0], pts[0][1], pts[0][2],
					pts[nPts - 1][0], pts[nPts - 1][1], pts[nPts - 1][2]);
			} while (0);
		}

		s_lastP = slidP; s_lastS = slidS; s_lastCmd = nCmd;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		static uint32_t s_nFaults = 0;
		if (++s_nFaults <= 4)
			Warning(eDLL_T::CLIENT, "[ZIP-ALPHA] exception #%u -- probe skipped this ack\n", s_nFaults);
	}
}
