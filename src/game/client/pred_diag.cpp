//=============================================================================//
//
// Purpose: Per-field client/server prediction-error spew (S21 client).
// See pred_diag.h. Walks the flattened networked prediction datamap.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "datamap.h"            // fieldtype_t (FIELD_FLOAT, FIELD_EHANDLE,...)
#include "game/client/pred_diag.h"
#include "game/client/pred_authority.h"
#include "engine/client/clientstate.h"      // CClientState ([PRED-ACKSRC] reference values)
#include "engine/client/cl_splitscreen.h"   // GetBaseLocalClient ([PRED-ACKSRC])
#include "engine/client/net_bridge_internal.h"
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <unordered_map>

//-----------------------------------------------------------------------------
// sdk_pred_diff: 0=off, 1=all predictables, -N=entity index N only.
//-----------------------------------------------------------------------------
static ConVar sdk_pred_diff(
	"sdk_pred_diff", "0", FCVAR_DEVELOPMENTONLY,
	"Spew per-field client/server prediction divergences. 0 = off (and the "
	"PostNetworkDataReceived detour is not even attached). "
	"1 = all predictable entities, -N = watch entity index N only.",
	false, 0.f, false, 0.f, nullptr, nullptr);

static ConVar sdk_pred_diag("sdk_pred_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Master enable for ALL prediction diagnostics (diff/verdict/census/depth/acksrc/"
	"acktick/kcensus/prederr/pair-scan). 0 silences the whole suite in one flip; the "
	"production mask/repair fixes are unaffected. Default 0.");

static inline bool PredDiag_On(void) { return sdk_pred_diag.GetBool(); }

static ConVar sdk_pred_diff_file("sdk_pred_diff_file", "1", FCVAR_DEVELOPMENTONLY,
	"Route [PRED-DIFF]/[PRED-CMD] to platform\\pred_diff.log (deduplicated) instead of "
	"warning.log. 0 = warning channel (legacy, high volume).");
static ConVar sdk_pred_diff_repeat("sdk_pred_diff_repeat", "256", FCVAR_DEVELOPMENTONLY,
	"Heartbeat: re-log an unchanged but still-diverging field once per N repeats "
	"(~N/20 seconds at 20Hz). Lower = more lines, higher = smaller file.");

// nAcked is a per-snapshot count, not an absolute command number.
static ConVar sdk_pred_depth("sdk_pred_depth", "0", FCVAR_DEVELOPMENTONLY,
	"Log the bridge local player's per-snapshot ack COUNT (commands_acknowledged) + "
	"ack-step per ack, windowed ([PRED-DEPTH]). nAcked is a count, not a command number. Default 0.");

static ConVar sdk_pred_acksrc("sdk_pred_acksrc", "0", FCVAR_DEVELOPMENTONLY,
	"Log the RAW commands_acknowledged (a3) the native prediction derivation feeds the "
	"dispatch, windowed ([PRED-ACKSRC]), vs the snapshot cmdTick + outgoing cmd. Settles "
	"whether a3 is ~7 (healthy) or ~0/neg (broken) for the bridge. Default 0.");

static ConVar sdk_acktick_snap("sdk_acktick_snap", "0", FCVAR_DEVELOPMENTONLY,
	"[ACKTICK-SNAP] per-snapshot dump (in PostNetworkDataReceived, ent 1) of the wire server tick + "
	"acked command tick + nAcked with per-snapshot deltas. dSvr is the CONTROL: dCmd==dSvr = coarse/"
	"skipped snapshots (not a count bug); dCmd lumpy while dSvr=+1 = genuinely lumpy acked-command "
	"count. DIAGNOSTIC DEFAULT 1 (auto-captures; revert to 0 after the run).");

static ConVar sdk_pred_verdict("sdk_pred_verdict", "0", FCVAR_DEVELOPMENTONLY,
	"[PRED-VERDICT] log the native PNR had-errors verdict rate + the differing fields on "
	"each error ack (ent 1, capped). Attributes the prediction-error icon to exact fields. "
	"1=on. Default 0 (ship); enable with the pred-diag suite for a hunt.");

static ConVar sdk_pair_scan("sdk_pair_scan", "0", FCVAR_DEVELOPMENTONLY,
	"[PAIR-SCAN] per-ack scan: which predicted record (nLatestCmd - k, k=0..7) matches "
	"the server m_localOrigin. Windowed histogram of k. Measures the prediction pairing "
	"offset in command units. Default 0.");

static ConVar sdk_pred_kcensus("sdk_pred_kcensus", "0", FCVAR_DEVELOPMENTONLY,
	"[K-CENSUS] per-ack scan: which predicted record (ack-k, k=0..15) matches the arrived "
	"server value, PER FIELD (origin/velocity/groundNormal/nextAttack/zoom). Windowed "
	"per-field k histograms. Read-only measurement (PART 15d). Default 0.");

static ConVar sdk_prederr_probe("sdk_prederr_probe", "0", FCVAR_DEVELOPMENTONLY,
	"[PREDERR] Hook the PATH-B prediction-error check . Windowed log for"
	"ent 1: the command number a2 used for the network-record lookup, the predicted origin, "
	"and how often the >0.5 correction (smoothing-record free) fired. Settles whether a2 is "
	"a sane command number or the broken ack feeding a stale record. Default 0.");


static FILE* s_predFile = nullptr;
static std::unordered_map<uint64_t, std::pair<uint64_t, uint32_t>> s_predSeen; // key -> (valueHash, repeatCount)

static inline uint64_t PredFnv(uint64_t h, const char* s)
{
	for (; *s; ++s) { h ^= (uint8_t)(*s); h *= 1099511628211ull; }
	return h;
}

static void PredDiag_WriteFile(const char* line)
{
	if (!s_predFile)
		s_predFile = fopen("platform\\pred_diff.log", "a");
	if (s_predFile)
	{
		fputs(line, s_predFile);
		fflush(s_predFile);
	}
}

// Emit one [PRED-DIFF] field line, deduplicated. `valfmt`+args = the "pred=.. srv=.."
// value portion only; suppressed unless the value changed or the heartbeat elapsed.
static void PredDiag_Emit(int ent, const char* name, const char* valfmt, ...)
{
	char vbuf[384];
	va_list ap; va_start(ap, valfmt);
	vsnprintf(vbuf, sizeof(vbuf), valfmt, ap);
	va_end(ap);

	if (!sdk_pred_diff_file.GetBool())
	{
		Warning(eDLL_T::CLIENT, "[PRED-DIFF] (%d) %s  %s\n", ent, name, vbuf);
		return;
	}

	const uint64_t key = PredFnv(1469598103934665603ull ^ ((uint64_t)(uint32_t)ent * 0x9E3779B1ull), name);
	const uint64_t vh = PredFnv(1469598103934665603ull, vbuf);
	const uint32_t hb = (uint32_t)sdk_pred_diff_repeat.GetInt();

	auto it = s_predSeen.find(key);
	if (it == s_predSeen.end())
		s_predSeen[key] = std::make_pair(vh, 0u);              // first sight -> log
	else if (it->second.first == vh)
	{
		if (hb == 0 || (++it->second.second % hb) != 0u)
			return;                                           // identical & within heartbeat -> suppress
	}
	else { it->second.first = vh; it->second.second = 0u; }   // value changed -> log

	char line[512];
	snprintf(line, sizeof(line), "[PRED-DIFF] (%d) %s  %s\n", ent, name, vbuf);
	PredDiag_WriteFile(line);
}

static void PredDiag_SpewFieldDiffs(void* pEntity, unsigned int nCmd, int nEntIndex)
{
	if (!EntField<unsigned char>(pEntity, ENT_PREDICTED_FLAG))
		return; // no prediction data allocated

	void* pPredState = C_BaseEntity__GetPredictedEntityState(pEntity, nCmd);
	if (!pPredState)
		return;
	const uint8_t* pPredicted = EntField<uint8_t*>(pPredState, STATE_SERIALIZED_DATA);

	const uintptr_t states = EntField<uintptr_t>(pEntity, ENT_SAVED_STATES);
	if (!states)
		return;
	const uintptr_t originalData = states + STATES_ORIGINALDATA;
	if (!*reinterpret_cast<unsigned char*>(originalData)) // originalData.active
		return;
	const uint8_t* pServer = *reinterpret_cast<uint8_t**>(originalData + STATE_SERIALIZED_DATA);
	if (!pPredicted || !pServer)
		return;

	// GetPredDescMap is a virtual (C_BaseEntity vtable slot 14)
	void* (*GetPredDescMap)(void*) = reinterpret_cast<void* (*)(void*)>(
		(*reinterpret_cast<void***>(pEntity))[VIDX_GetPredDescMap]);
	const uintptr_t dmap = reinterpret_cast<uintptr_t>(GetPredDescMap(pEntity));
	if (!dmap)
		return;
	const uintptr_t opt = *reinterpret_cast<uintptr_t*>(dmap + DMAP_OPTIMIZED);
	if (!opt)
		return;

	const uintptr_t info = opt + (uintptr_t)OPT_INFO_STRIDE * PC_NETWORKED_ONLY;
	const uintptr_t fields = *reinterpret_cast<uintptr_t*>(info + INFO_FLAT_FIELDS);
	const int count = *reinterpret_cast<int*>(info + INFO_FLAT_COUNT);
	if (!fields || count <= 0 || count > 4096) // safety bound on corrupt data
		return;

	if (nEntIndex == 1)
	{
		if (!sdk_pred_diff_file.GetBool())
			Warning(eDLL_T::CLIENT, "[PRED-CMD] (1) latestCmd=%u\n", nCmd);
		else
		{
			// latestCmd increments every ack (never dedups); sample 1-in-32 to keep it small.
			static uint32_t s_cmdSample = 0;
			if (((++s_cmdSample) & 0x1F) == 0)
			{
				char line[64];
				snprintf(line, sizeof(line), "[PRED-CMD] (1) latestCmd=%u\n", nCmd);
				PredDiag_WriteFile(line);
			}
		}
	}

	for (int i = 0; i < count; ++i)
	{
		const uintptr_t td = fields + (uintptr_t)i * TD_STRIDE;
		const uint32_t flags = *reinterpret_cast<uint32_t*>(td + TD_FLAGS);
		if (flags & (FTYPEDESC_SKIP | FTYPEDESC_GRAPPLE))
			continue;

		const int type = *reinterpret_cast<int*>(td + TD_FIELDTYPE);
		const int off = *reinterpret_cast<int*>(td + TD_FLATOFFSET1);
		const int n16 = *reinterpret_cast<uint16_t*>(td + TD_FIELDSIZE);
		const int cnt = n16 ? n16 : 1;
		// The engine widens FIELD_TIME by half a server tick before it compares;
		// spewing on the raw fieldTolerance reports drift the engine forgives.
		const float tol = PredNative_Tolerance(type,
			*reinterpret_cast<float*>(td + TD_TOLERANCE));
		const char* name = *reinterpret_cast<const char**>(td + TD_FIELDNAME);
		if (!name) name = "<noname>";

		const uint8_t* p = pPredicted + off;
		const uint8_t* s = pServer + off;

		switch (type)
		{
		case FIELD_FLOAT:
		case FIELD_TIME:
			for (int e = 0; e < cnt; ++e)
			{
				const float pf = reinterpret_cast<const float*>(p)[e];
				const float sf = reinterpret_cast<const float*>(s)[e];
				if (fabsf(pf - sf) > tol)
					PredDiag_Emit(nEntIndex, name, "pred=%.6g srv=%.6g d=%.6g", pf, sf, pf - sf);
			}
			break;
		case FIELD_VECTOR:
		{
			const float* pv = reinterpret_cast<const float*>(p);
			const float* sv = reinterpret_cast<const float*>(s);
			if (fabsf(pv[0] - sv[0]) > tol || fabsf(pv[1] - sv[1]) > tol || fabsf(pv[2] - sv[2]) > tol)
				PredDiag_Emit(nEntIndex, name, "pred=(%.3f %.3f %.3f) srv=(%.3f %.3f %.3f)",
					pv[0], pv[1], pv[2], sv[0], sv[1], sv[2]);
			break;
		}
		case FIELD_QUATERNION:
		{
			const float* pq = reinterpret_cast<const float*>(p);
			const float* sq = reinterpret_cast<const float*>(s);
			if (fabsf(pq[0] - sq[0]) > tol || fabsf(pq[1] - sq[1]) > tol ||
				fabsf(pq[2] - sq[2]) > tol || fabsf(pq[3] - sq[3]) > tol)
				PredDiag_Emit(nEntIndex, name, "quaternion differs");
			break;
		}
		case FIELD_INTEGER:
			for (int e = 0; e < cnt; ++e)
			{
				const int pi = reinterpret_cast<const int*>(p)[e];
				const int si = reinterpret_cast<const int*>(s)[e];
				if (pi != si)
					PredDiag_Emit(nEntIndex, name, "pred=%d srv=%d", pi, si);
			}
			break;
		case FIELD_SHORT:
			for (int e = 0; e < cnt; ++e)
			{
				const short ps = reinterpret_cast<const int16_t*>(p)[e];
				const short ss = reinterpret_cast<const int16_t*>(s)[e];
				if (ps != ss)
					PredDiag_Emit(nEntIndex, name, "pred=%d srv=%d", ps, ss);
			}
			break;
		case FIELD_BOOLEAN:
		case FIELD_CHARACTER:
			for (int e = 0; e < cnt; ++e)
				if (p[e] != s[e])
					PredDiag_Emit(nEntIndex, name, "pred=%u srv=%u", p[e], s[e]);
			break;
		case FIELD_EHANDLE:
			for (int e = 0; e < cnt; ++e)
			{
				const uint32_t ph = reinterpret_cast<const uint32_t*>(p)[e];
				const uint32_t sh = reinterpret_cast<const uint32_t*>(s)[e];
				if (ph == sh)
					continue;
				// res=1 means the two handles resolve to the SAME entity, which the
				// engine treats as equal -- the row is raw drift, not a rebase driver.
				const void* const pe = PredNative_ResolveEHandle(ph);
				const void* const se = PredNative_ResolveEHandle(sh);
				PredDiag_Emit(nEntIndex, name, "pred=h%08X srv=h%08X res=%d",
					ph, sh, (pe == se) ? 1 : 0);
			}
			break;
		case FIELD_COLOR32:
			if (memcmp(p, s, (size_t)cnt * 4) != 0)
				PredDiag_Emit(nEntIndex, name, "color32 differs");
			break;
		case FIELD_STRING:
			if (strcmp(reinterpret_cast<const char*>(p), reinterpret_cast<const char*>(s)) != 0)
				PredDiag_Emit(nEntIndex, name, "pred='%s' srv='%s'",
					reinterpret_cast<const char*>(p), reinterpret_cast<const char*>(s));
			break;
		default:
			break;
		}
	}
}

static void PredDiag_PairScan(void* pEntity, unsigned int nCmd)
{
	if (!EntField<unsigned char>(pEntity, ENT_PREDICTED_FLAG))
		return;

	const uintptr_t states = EntField<uintptr_t>(pEntity, ENT_SAVED_STATES);
	if (!states)
		return;
	const uintptr_t originalData = states + STATES_ORIGINALDATA;
	if (!*reinterpret_cast<unsigned char*>(originalData)) // originalData.active
		return;
	const uint8_t* pServer = *reinterpret_cast<uint8_t**>(originalData + STATE_SERIALIZED_DATA);
	if (!pServer)
		return;

	// Resolve + cache m_localOrigin's flat offset from the optimized pred datamap.
	static int s_nOriginFlatOff = -2; // -2 = unresolved, -1 = not found
	if (s_nOriginFlatOff == -2)
	{
		s_nOriginFlatOff = -1;
		void* (*GetPredDescMap)(void*) = reinterpret_cast<void* (*)(void*)>(
			(*reinterpret_cast<void***>(pEntity))[VIDX_GetPredDescMap]);
		const uintptr_t dmap = reinterpret_cast<uintptr_t>(GetPredDescMap(pEntity));
		const uintptr_t opt = dmap ? *reinterpret_cast<uintptr_t*>(dmap + DMAP_OPTIMIZED) : 0;
		if (opt)
		{
			const uintptr_t info = opt + (uintptr_t)OPT_INFO_STRIDE * PC_NETWORKED_ONLY;
			const uintptr_t fields = *reinterpret_cast<uintptr_t*>(info + INFO_FLAT_FIELDS);
			const int count = *reinterpret_cast<int*>(info + INFO_FLAT_COUNT);
			for (int i = 0; fields && i < count && i < 4096; ++i)
			{
				const uintptr_t td = fields + (uintptr_t)i * TD_STRIDE;
				const char* name = *reinterpret_cast<const char**>(td + TD_FIELDNAME);
				if (name && !_stricmp(name, "m_localOrigin"))
				{
					s_nOriginFlatOff = *reinterpret_cast<int*>(td + TD_FLATOFFSET1);
					break;
				}
			}
		}
		Warning(eDLL_T::CLIENT, "[PAIR-SCAN] m_localOrigin flatOffset=%d\n", s_nOriginFlatOff);
	}
	if (s_nOriginFlatOff < 0 || !C_BaseEntity__GetPredictedEntityState)
		return;

	const float* sv = reinterpret_cast<const float*>(pServer + s_nOriginFlatOff);

	// Scan k = 0..7 for the record whose origin matches the server state.
	int nMatchK = -1;
	for (unsigned int k = 0; k <= 7 && k < nCmd; ++k)
	{
		void* const rec = C_BaseEntity__GetPredictedEntityState(pEntity, nCmd - k);
		if (!rec)
			continue;
		const uint8_t* pRec = EntField<uint8_t*>(rec, STATE_SERIALIZED_DATA);
		if (!pRec)
			continue;
		const float* pr = reinterpret_cast<const float*>(pRec + s_nOriginFlatOff);
		if (fabsf(pr[0] - sv[0]) < 0.01f && fabsf(pr[1] - sv[1]) < 0.01f && fabsf(pr[2] - sv[2]) < 0.01f)
		{
			nMatchK = (int)k;
			break;
		}
	}

	// Windowed histogram: k buckets 0..7 + none(-1). Also track the cmd step per ack
	// (cmds-per-snapshot) so the k verdict can be read against the rate.
	static int s_n = 0, s_hk[9] = {}; // [0..7]=k, [8]=none
	static unsigned s_lastCmd = 0; static long s_stepSum = 0; static int s_stepN = 0;
	++s_n;
	++s_hk[nMatchK < 0 ? 8 : nMatchK];
	if (s_lastCmd && nCmd > s_lastCmd) { s_stepSum += (long)(nCmd - s_lastCmd); ++s_stepN; }
	s_lastCmd = nCmd;
	if (s_n >= 256)
	{
		Warning(eDLL_T::CLIENT,
			"[PAIR-SCAN] n=%d k{0=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7=%d none=%d} avgCmdStep=%.2f latest=%u\n",
			s_n, s_hk[0], s_hk[1], s_hk[2], s_hk[3], s_hk[4], s_hk[5], s_hk[6], s_hk[7], s_hk[8],
			s_stepN ? (double)s_stepSum / s_stepN : 0.0, nCmd);
		s_n = 0; memset(s_hk, 0, sizeof(s_hk)); s_stepSum = 0; s_stepN = 0;
	}
}

struct KCensusField_s { const char* name; bool isVec3; float eps; };
static const KCensusField_s s_kCensusFields[] =
{
	{ "m_localOrigin",       true,  0.01f  },
	{ "m_vecVelocity",       true,  0.1f   },
	{ "m_groundNormal",      true,  0.001f },
	{ "m_flNextAttack",      false, 1e-4f  },
	{ "m_zoomBaseTime",      false, 1e-4f  },
	{ "m_zoomBaseFrac",      false, 1e-4f  },
	{ "m_zoomFullStartTime", false, 1e-4f  },
};
static constexpr int K_CENSUS_NFIELDS = sizeof(s_kCensusFields) / sizeof(s_kCensusFields[0]);

// Per-datamap flat-offset cache (player ent + predicted weapon carry different
// datamaps). Cap 8, linear scan -- new dmaps are rare (one per predictable class).
struct KCensusDmapEntry_s { uintptr_t dmap; int off[K_CENSUS_NFIELDS]; };
static KCensusDmapEntry_s s_kCensusDmaps[8] = {};
static int s_kCensusDmapCount = 0;

// Windowed per-field histograms. Buckets [0..15]=k, [16]=no-match(anyRec but none
// matched), [17]=no-record(no k had a valid record). Reset together on dump.
static int s_kCensusHist[K_CENSUS_NFIELDS][18] = {};
static int s_kCensusSamples[K_CENSUS_NFIELDS] = {};
static unsigned s_kCensusLastCmd = 0;
static long     s_kCensusStepSum = 0;
static int      s_kCensusStepN = 0;

static const int* PredDiag_KCensusOffsets(void* pEntity)
{
	void* (*GetPredDescMap)(void*) = reinterpret_cast<void* (*)(void*)>(
		(*reinterpret_cast<void***>(pEntity))[VIDX_GetPredDescMap]);
	const uintptr_t dmap = reinterpret_cast<uintptr_t>(GetPredDescMap(pEntity));
	if (!dmap)
		return nullptr;

	for (int i = 0; i < s_kCensusDmapCount; ++i)
	{
		if (s_kCensusDmaps[i].dmap == dmap)
			return s_kCensusDmaps[i].off;
	}

	if (s_kCensusDmapCount >= 8)
		return nullptr; // cache full -- unexpected number of distinct predictable classes

	KCensusDmapEntry_s& entry = s_kCensusDmaps[s_kCensusDmapCount];
	entry.dmap = dmap;
	for (int f = 0; f < K_CENSUS_NFIELDS; ++f)
		entry.off[f] = -1;

	const uintptr_t opt = *reinterpret_cast<uintptr_t*>(dmap + DMAP_OPTIMIZED);
	if (opt)
	{
		const uintptr_t info = opt + (uintptr_t)OPT_INFO_STRIDE * PC_NETWORKED_ONLY;
		const uintptr_t fields = *reinterpret_cast<uintptr_t*>(info + INFO_FLAT_FIELDS);
		const int count = *reinterpret_cast<int*>(info + INFO_FLAT_COUNT);
		for (int i = 0; fields && i < count && i < 4096; ++i)
		{
			const uintptr_t td = fields + (uintptr_t)i * TD_STRIDE;
			const char* name = *reinterpret_cast<const char**>(td + TD_FIELDNAME);
			if (!name)
				continue;
			for (int f = 0; f < K_CENSUS_NFIELDS; ++f)
			{
				if (entry.off[f] < 0 && !_stricmp(name, s_kCensusFields[f].name))
				{
					entry.off[f] = *reinterpret_cast<int*>(td + TD_FLATOFFSET1);
					break;
				}
			}
		}
	}

	Warning(eDLL_T::CLIENT,
		"[K-CENSUS] dmap=0x%llX offs org=%d vel=%d gnorm=%d natk=%d zbt=%d zbf=%d zfst=%d\n",
		(unsigned long long)dmap, entry.off[0], entry.off[1], entry.off[2], entry.off[3],
		entry.off[4], entry.off[5], entry.off[6]);

	++s_kCensusDmapCount;
	return entry.off;
}

static void PredDiag_KCensus(void* pEntity, unsigned int nCmd)
{
	// Refresh originalData; without this the read is one ack stale.
	__try
	{
		if (PFN_PredSaveData pSave = PredAuth_SaveData())
			pSave(pEntity, "kcensus", (int)nCmd, -1);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return;
	}

	if (!EntField<unsigned char>(pEntity, ENT_PREDICTED_FLAG))
		return;

	const uintptr_t states = EntField<uintptr_t>(pEntity, ENT_SAVED_STATES);
	if (!states)
		return;
	const uintptr_t originalData = states + STATES_ORIGINALDATA;
	if (!*reinterpret_cast<unsigned char*>(originalData)) // originalData.active
		return;
	const uint8_t* pServer = *reinterpret_cast<uint8_t**>(originalData + STATE_SERIALIZED_DATA);
	if (!pServer)
		return;

	const int* off = PredDiag_KCensusOffsets(pEntity);
	if (!off)
		return;

	for (int f = 0; f < K_CENSUS_NFIELDS; ++f)
	{
		if (off[f] < 0)
			continue;

		const KCensusField_s& fld = s_kCensusFields[f];
		const float* sv = reinterpret_cast<const float*>(pServer + off[f]);

		bool anyRec = false;
		int  nMatchK = -1;
		for (unsigned int k = 0; k <= 15 && k < nCmd; ++k)
		{
			void* const rec = C_BaseEntity__GetPredictedEntityState(pEntity, nCmd - k);
			if (!rec)
				continue;
			const uint8_t* pRec = EntField<uint8_t*>(rec, STATE_SERIALIZED_DATA);
			if (!pRec)
				continue;
			anyRec = true;

			const float* pr = reinterpret_cast<const float*>(pRec + off[f]);
			bool bMatch;
			if (fld.isVec3)
			{
				bMatch = fabsf(pr[0] - sv[0]) < fld.eps &&
					fabsf(pr[1] - sv[1]) < fld.eps &&
					fabsf(pr[2] - sv[2]) < fld.eps;
			}
			else
			{
				bMatch = fabsf(pr[0] - sv[0]) < fld.eps;
			}

			if (bMatch)
			{
				nMatchK = (int)k;
				break;
			}
		}

		const int bucket = (nMatchK >= 0) ? nMatchK : (anyRec ? 16 : 17);
		++s_kCensusHist[f][bucket];
		++s_kCensusSamples[f];
	}

	if (s_kCensusLastCmd && nCmd > s_kCensusLastCmd)
	{
		s_kCensusStepSum += (long)(nCmd - s_kCensusLastCmd);
		++s_kCensusStepN;
	}
	s_kCensusLastCmd = nCmd;

	// Window dump keyed off field 0 (m_localOrigin): every predictable feeds the
	// same ack cadence, so any resolved field's sample count would do.
	if (s_kCensusSamples[0] >= 512)
	{
		Warning(eDLL_T::CLIENT, "[K-CENSUS] window n=%d avgCmdStep=%.2f latest=%u\n",
			s_kCensusSamples[0],
			s_kCensusStepN ? (double)s_kCensusStepSum / s_kCensusStepN : 0.0, nCmd);

		for (int f = 0; f < K_CENSUS_NFIELDS; ++f)
		{
			const int* h = s_kCensusHist[f];
			const int eight_plus = h[8] + h[9] + h[10] + h[11] + h[12] + h[13] + h[14] + h[15];
			Warning(eDLL_T::CLIENT,
				"[K-CENSUS] %-20s n=%d k{0=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7=%d 8+=%d} none=%d norec=%d\n",
				s_kCensusFields[f].name, s_kCensusSamples[f],
				h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], eight_plus, h[16], h[17]);
		}

		memset(s_kCensusHist, 0, sizeof(s_kCensusHist));
		memset(s_kCensusSamples, 0, sizeof(s_kCensusSamples));
		s_kCensusStepSum = 0;
		s_kCensusStepN = 0;
	}
}

struct PredClass_s
{
	bool clock, landing, originF, velF, zipline, punch, other, groundEnt;
	bool weaponFlags, meleeTime;
	bool zoom, edge, animevt;
	const char* otherName;   // first unclassified field (engine string, stable)
	float dOrgX, dOrgY, dOrgZ;
	int   sTd;               // pred - srv m_lastUCmdSimulationTicks
	float tBd;               // pred - srv timeBase
	int   mtP, mtS;          // m_MoveType pred/srv
	float fvP, fvS;          // m_flFallVelocity pred/srv
};
static PredClass_s s_predClass;

// [PRED-BOARD] cross-hook forced0 tally written by PredDiag_PnrCensus (delta==0
// free-rebase rule). Read by the verdict window scoreboard. No locking: main
// prediction thread only.
static uint32_t s_boardForced0 = 0;
static uint32_t s_boardPnrCalls = 0;

// "=pred/srv" for the scalar and vector types the native compare honours; empty
// for the rest so the field list stays readable.
static void PredDiag_FormatPair(int type, const uint8_t* p, const uint8_t* s, char* out, size_t outLen)
{
	out[0] = '\0';
	switch (type)
	{
	case FIELD_FLOAT: case FIELD_TIME:
		snprintf(out, outLen, "=%.3f/%.3f",
			*reinterpret_cast<const float*>(p), *reinterpret_cast<const float*>(s));
		break;
	case FIELD_VECTOR:
	{
		const float* a = reinterpret_cast<const float*>(p);
		const float* b = reinterpret_cast<const float*>(s);
		snprintf(out, outLen, "=(%.2f %.2f %.2f)/(%.2f %.2f %.2f)", a[0], a[1], a[2], b[0], b[1], b[2]);
		break;
	}
	case FIELD_INTEGER: case FIELD_COLOR32:
		snprintf(out, outLen, "=%d/%d",
			*reinterpret_cast<const int*>(p), *reinterpret_cast<const int*>(s));
		break;
	case FIELD_EHANDLE:
		snprintf(out, outLen, "=h%08X/h%08X",
			*reinterpret_cast<const uint32_t*>(p), *reinterpret_cast<const uint32_t*>(s));
		break;
	case FIELD_SHORT:
		snprintf(out, outLen, "=%d/%d",
			*reinterpret_cast<const int16_t*>(p), *reinterpret_cast<const int16_t*>(s));
		break;
	case FIELD_BOOLEAN: case FIELD_CHARACTER:
		snprintf(out, outLen, "=%d/%d", p[0], s[0]);
		break;
	default:
		break;
	}
}

static int PredDiag_CollectDiffNames(void* pEntity, unsigned int nCmd, char* out, size_t outLen)
{
	out[0] = '\0';
	memset(&s_predClass, 0, sizeof(s_predClass));
	if (!EntField<unsigned char>(pEntity, ENT_PREDICTED_FLAG))
		return -1;
	void* pPredState = C_BaseEntity__GetPredictedEntityState(pEntity, nCmd);
	if (!pPredState)
		return -1;
	const uint8_t* pPredicted = EntField<uint8_t*>(pPredState, STATE_SERIALIZED_DATA);
	const uintptr_t states = EntField<uintptr_t>(pEntity, ENT_SAVED_STATES);
	if (!pPredicted || !states)
		return -1;
	const uintptr_t originalData = states + STATES_ORIGINALDATA;
	if (!*reinterpret_cast<unsigned char*>(originalData))
		return -1;
	const uint8_t* pServer = *reinterpret_cast<uint8_t**>(originalData + STATE_SERIALIZED_DATA);
	if (!pServer)
		return -1;

	void* (*GetPredDescMap)(void*) = reinterpret_cast<void* (*)(void*)>(
		(*reinterpret_cast<void***>(pEntity))[VIDX_GetPredDescMap]);
	const uintptr_t dmap = reinterpret_cast<uintptr_t>(GetPredDescMap(pEntity));
	if (!dmap) return -1;
	const uintptr_t opt = *reinterpret_cast<uintptr_t*>(dmap + DMAP_OPTIMIZED);
	if (!opt) return -1;
	const uintptr_t info   = opt + (uintptr_t)OPT_INFO_STRIDE * PC_NETWORKED_ONLY;
	const uintptr_t fields = *reinterpret_cast<uintptr_t*>(info + INFO_FLAT_FIELDS);
	const int count        = *reinterpret_cast<int*>(info + INFO_FLAT_COUNT);
	if (!fields || count <= 0 || count > 4096) return -1;

	int nDiff = 0; size_t o = 0;
	for (int i = 0; i < count; ++i)
	{
		const uintptr_t td = fields + (uintptr_t)i * TD_STRIDE;
		const uint32_t flags = *reinterpret_cast<uint32_t*>(td + TD_FLAGS);
		if (flags & (FTYPEDESC_SKIP | FTYPEDESC_GRAPPLE))
			continue;
		const int type = *reinterpret_cast<int*>(td + TD_FIELDTYPE);
		const int off = *reinterpret_cast<int*>(td + TD_FLATOFFSET1);
		const int n16 = *reinterpret_cast<uint16_t*>(td + TD_FIELDSIZE);
		const int cnt = n16 ? n16 : 1;
		const float tol = *reinterpret_cast<float*>(td + TD_TOLERANCE);
		const uint8_t* p = pPredicted + off;
		const uint8_t* s = pServer + off;

		// Mirror the engine's own compare exactly (see the contract banner in
		// pred_authority.h) -- a field the native check forgives is not a rebase
		// driver and must not enter nDiff or the family ranking built off it.
		if (PredNative_Compare(type, p, s, cnt, tol) != PRED_CMP_ERROR)
			continue;
		++nDiff;
		const char* name = *reinterpret_cast<const char**>(td + TD_FIELDNAME);
		if (!name) name = "<noname>";

		// [PRED-CLASS] classify every diffing field (not just the printed 6).
		{
			PredClass_s& c = s_predClass;
			if (!strcmp(name, "m_currentFramePlayer.timeBase") ||
				!strcmp(name, "m_flNextAttack") ||
				!strcmp(name, "m_lastUCmdSimulationTicks") ||
				!strcmp(name, "m_lastUCmdSimulationRemainderTime") ||
				!strcmp(name, "m_meleePressTime") ||
				!strcmp(name, "m_raiseFromMeleeEndTime") ||
				!strcmp(name, "attackStartTime") ||
				!strcmp(name, "attackHitEntityTime") ||
				!strcmp(name, "attackLastHitNonWorldEntity") ||
				!strcmp(name, "m_airMoveBlockPlaneTime"))
				c.clock = true;
			else if (!strcmp(name, "m_flFallVelocity") ||
				!strcmp(name, "m_groundNormal") ||
				!strcmp(name, "m_hGroundEntity") ||
				!strcmp(name, "m_fFlags") ||
				!strcmp(name, "m_vecMaxs"))
			{
				c.landing = true;
				if (!strcmp(name, "m_hGroundEntity")) c.groundEnt = true;
			}
			else if (!strcmp(name, "m_localOrigin"))
				c.originF = true;
			else if (!strcmp(name, "m_vecVelocity") || !strcmp(name, "m_vecAbsVelocity"))
				c.velF = true;
			else if (strstr(name, "ipline") || strstr(name, "ziplineViewOffset"))
				c.zipline = true;
			else if (strstr(name, "Punch") || strstr(name, "stepSmoothing") ||
				strstr(name, "localGravityRotation") || strstr(name, "viewConeAngle"))
				c.punch = true;
			else if (!strcmp(name, "m_bZooming") || strstr(name, "m_zoom") ||
				strstr(name, "zoomBase") || strstr(name, "zoomFull"))
				c.zoom = true;
			else if (strstr(name, "predictedAnimEvent"))
				c.animevt = true;
			else if (!strcmp(name, "predictableFlags") ||
				!strcmp(name, "activeWeapons") ||
				!strcmp(name, "m_selectedOffhands") ||
				strstr(name, "selectedOffhandsPending") ||
				strstr(name, "showActiveWeapon3p") ||
				strstr(name, "weaponGettingSwitchedOut") ||
				strstr(name, "m_ammoPoolCount") ||
				!strcmp(name, "m_fIsSprinting") ||
				!strcmp(name, "m_bIsStickySprinting") ||
				!strcmp(name, "m_fStickySprintMinTime") ||
				strstr(name, "playAnimationNext") ||
				!strcmp(name, "m_forceStance"))
				c.edge = true;
			else if (!strcmp(name, "m_weaponDisabledFlags"))
				c.weaponFlags = true;
			else if (!strcmp(name, "m_raiseFromMeleeEndTime"))
				c.meleeTime = true;
			else
			{
				c.other = true;
				if (!c.otherName) c.otherName = name;
			}
		}

		if (nDiff <= 8)
		{
			char szPair[96];
			PredDiag_FormatPair(type, p, s, szPair, sizeof(szPair));
			const int wrote = snprintf(out + o, outLen > o ? outLen - o : 0, "%s%s%s",
				(nDiff > 1) ? ", " : "", name, szPair);
			if (wrote > 0) o += (size_t)wrote;
		}
	}

	// Correction-magnitude extras (flat offsets latched like the unfed mask).
	{
		struct DetailF { const char* name; int off; };
		static DetailF s_det[] = {
			{ "m_localOrigin",            -1 },  // vector
			{ "m_currentFrame.viewOffset",-1 },  // vector
			{ "m_duckState",              -1 },  // int
			{ "m_fFlags",                 -1 },  // int
			{ "m_flFallVelocity",         -1 },  // float
			{ "m_vecVelocity",            -1 },  // vector [K-SCAN: speed-mismatch hunt]
			{ "m_fIsSprinting",           -1 },  // small int
			{ "m_currentFrameLocalPlayer.m_vecPunchBase_Angle",    -1 },  // vector
			{ "m_currentFrameLocalPlayer.m_vecPunchBase_AngleVel", -1 },  // vector
			{ "m_currentFramePlayer.timeBase",        -1 },  // float (abs time)
			{ "m_flNextAttack",                       -1 },  // float (abs time)
			{ "m_lastUCmdSimulationTicks",            -1 },  // int
			{ "m_lastUCmdSimulationRemainderTime",    -1 },  // float
			{ "m_groundNormal",                       -1 },  // vector
			{ "m_weaponDisabledFlags",                -1 },  // char bitmask: 1=main, 2=altHand, 4=utility
			{ "m_weaponDelayEnableTime",              -1 },  // float (abs time)
			{ "m_weaponPermission",                   -1 },  // int enum (WEAPON_ENABLE = 0 expected)
			{ "m_currentFrameLocalPlayer.m_vecPunchWeapon_Angle",    -1 },  // vector
			// Punch weapon velocity is three scalars; the bare vector name never resolves.
			{ "m_currentFrameLocalPlayer.m_vecPunchWeapon_AngleVel.x", -1 },
			{ "m_currentFrameLocalPlayer.m_vecPunchWeapon_AngleVel.y", -1 },
			{ "m_currentFrameLocalPlayer.m_vecPunchWeapon_AngleVel.z", -1 },
			{ "m_MoveType", -1 },  // int  [21]
		};
		static uintptr_t s_detDmap = 0;
		if (s_detDmap != dmap)
		{
			for (auto& d : s_det) d.off = -1;
			for (int i = 0; i < count; ++i)
			{
				const uintptr_t td = fields + (uintptr_t)i * TD_STRIDE;
				const char* name = *reinterpret_cast<const char**>(td + TD_FIELDNAME);
				if (!name) continue;
				for (auto& d : s_det)
					if (d.off < 0 && strcmp(name, d.name) == 0)
						d.off = *reinterpret_cast<int*>(td + TD_FLATOFFSET1);
			}
			s_detDmap = dmap;
		}
		if (s_det[0].off >= 0)
		{
			const float* po = reinterpret_cast<const float*>(pPredicted + s_det[0].off);
			const float* so = reinterpret_cast<const float*>(pServer + s_det[0].off);
			const float dx = po[0]-so[0], dy = po[1]-so[1], dz = po[2]-so[2];
			float dvz = 0.0f;
			if (s_det[1].off >= 0)
				dvz = reinterpret_cast<const float*>(pPredicted + s_det[1].off)[2]
				    - reinterpret_cast<const float*>(pServer + s_det[1].off)[2];
			const int dp = (s_det[2].off >= 0) ? *reinterpret_cast<const int*>(pPredicted + s_det[2].off) : -1;
			const int ds = (s_det[2].off >= 0) ? *reinterpret_cast<const int*>(pServer   + s_det[2].off) : -1;
			const int fp = (s_det[3].off >= 0) ? *reinterpret_cast<const int*>(pPredicted + s_det[3].off) : -1;
			const int fs = (s_det[3].off >= 0) ? *reinterpret_cast<const int*>(pServer   + s_det[3].off) : -1;
			const float vp = (s_det[4].off >= 0) ? *reinterpret_cast<const float*>(pPredicted + s_det[4].off) : 0.0f;
			const float vs = (s_det[4].off >= 0) ? *reinterpret_cast<const float*>(pServer   + s_det[4].off) : 0.0f;

			float dk[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
			dk[0] = sqrtf(dx*dx + dy*dy + dz*dz);
			for (int k = 1; k <= 3; ++k)
			{
				void* st = C_BaseEntity__GetPredictedEntityState(pEntity, nCmd - (unsigned int)k);
				if (!st) break;
				const uint8_t* pk = EntField<uint8_t*>(st, STATE_SERIALIZED_DATA);
				if (!pk) break;
				const float* ko = reinterpret_cast<const float*>(pk + s_det[0].off);
				const float kx = ko[0]-so[0], ky = ko[1]-so[1], kz = ko[2]-so[2];
				dk[k] = sqrtf(kx*kx + ky*ky + kz*kz);
			}
			float fk[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
			for (int k = 1; k <= 3; ++k)
			{
				void* st = C_BaseEntity__GetPredictedEntityState(pEntity, nCmd + (unsigned int)k);
				if (!st) break;
				const uint8_t* pk = EntField<uint8_t*>(st, STATE_SERIALIZED_DATA);
				if (!pk) break;
				const float* ko = reinterpret_cast<const float*>(pk + s_det[0].off);
				const float kx = ko[0]-so[0], ky = ko[1]-so[1], kz = ko[2]-so[2];
				fk[k] = sqrtf(kx*kx + ky*ky + kz*kz);
			}
			float velP = -1.0f, velS = -1.0f; int sprP = -1, sprS = -1;
			if (s_det[5].off >= 0)
			{
				const float* a = reinterpret_cast<const float*>(pPredicted + s_det[5].off);
				const float* b = reinterpret_cast<const float*>(pServer   + s_det[5].off);
				velP = sqrtf(a[0]*a[0] + a[1]*a[1]);
				velS = sqrtf(b[0]*b[0] + b[1]*b[1]);
			}
			if (s_det[6].off >= 0)
			{
				sprP = *reinterpret_cast<const uint8_t*>(pPredicted + s_det[6].off);
				sprS = *reinterpret_cast<const uint8_t*>(pServer   + s_det[6].off);
			}
			float kS = -1.0f; int nStamp = 0;
			{
				static uintptr_t s_pSnapG = 0;
				if (!s_pSnapG)
					s_pSnapG = NetObs_SnapshotBaseLerpFramePtrAddr();
				if (s_pSnapG)
				{
					const uintptr_t snap = *reinterpret_cast<const uintptr_t*>(s_pSnapG);
					nStamp = snap ? *reinterpret_cast<const int*>(snap + 64) : 0;
					if (nStamp > 0)
					{
						void* st = C_BaseEntity__GetPredictedEntityState(pEntity, (unsigned int)nStamp);
						const uint8_t* pk = st ? EntField<uint8_t*>(st, STATE_SERIALIZED_DATA) : nullptr;
						if (pk)
						{
							const float* ko = reinterpret_cast<const float*>(pk + s_det[0].off);
							const float kx = ko[0]-so[0], ky = ko[1]-so[1], kz = ko[2]-so[2];
							kS = sqrtf(kx*kx + ky*ky + kz*kz);
						}
					}
				}
			}
			// [PUNCH round #21] pred vs srv punch spring state (base angle + velocity).
			float pbP[3] = {0,0,0}, pbS[3] = {0,0,0}, pvP[3] = {0,0,0}, pvS[3] = {0,0,0};
			if (s_det[7].off >= 0)
			{
				memcpy(pbP, pPredicted + s_det[7].off, 12);
				memcpy(pbS, pServer    + s_det[7].off, 12);
			}
			if (s_det[8].off >= 0)
			{
				memcpy(pvP, pPredicted + s_det[8].off, 12);
				memcpy(pvS, pServer    + s_det[8].off, 12);
			}
			// The punch WEAPON pair -- the field that actually diverges on class=P.
			float pwP[3] = {0,0,0}, pwS[3] = {0,0,0};
			float pwvP[3] = {0,0,0}, pwvS[3] = {0,0,0};
			if (s_det[17].off >= 0)
			{
				memcpy(pwP, pPredicted + s_det[17].off, 12);
				memcpy(pwS, pServer    + s_det[17].off, 12);
			}
			for (int c = 0; c < 3; ++c)
			{
				const int off = s_det[18 + c].off;
				if (off < 0)
					continue;
				pwvP[c] = *reinterpret_cast<const float*>(pPredicted + off);
				pwvS[c] = *reinterpret_cast<const float*>(pServer    + off);
			}
			// [FINAL round #23] clock family + weapon timer + ground normal, pred/srv.
			float tbP=0, tbS=0, naP=0, naS=0, rmP=0, rmS=0, gnP=1, gnS=1;
			int stP=0, stS=0;
			if (s_det[9].off  >= 0) { tbP = *reinterpret_cast<const float*>(pPredicted + s_det[9].off);  tbS = *reinterpret_cast<const float*>(pServer + s_det[9].off);  }
			if (s_det[10].off >= 0) { naP = *reinterpret_cast<const float*>(pPredicted + s_det[10].off); naS = *reinterpret_cast<const float*>(pServer + s_det[10].off); }
			if (s_det[11].off >= 0) { stP = *reinterpret_cast<const int*>(pPredicted + s_det[11].off);   stS = *reinterpret_cast<const int*>(pServer + s_det[11].off);   }
			if (s_det[12].off >= 0) { rmP = *reinterpret_cast<const float*>(pPredicted + s_det[12].off); rmS = *reinterpret_cast<const float*>(pServer + s_det[12].off); }
			if (s_det[13].off >= 0) { gnP = reinterpret_cast<const float*>(pPredicted + s_det[13].off)[2]; gnS = reinterpret_cast<const float*>(pServer + s_det[13].off)[2]; }
			// [WEAPON-DISABLE #34c] pred/srv raw bitmask, decoded per-slot below.
			uint8_t wdfP = 0, wdfS = 0;
			if (s_det[14].off >= 0)
			{
				wdfP = *reinterpret_cast<const uint8_t*>(pPredicted + s_det[14].off);
				wdfS = *reinterpret_cast<const uint8_t*>(pServer   + s_det[14].off);
			}
			// [WEAPON-DISABLE #34d] the first gate's own two inputs, pred/srv.
			float wdeP = 0, wdeS = 0; int wpmP = -1, wpmS = -1;
			if (s_det[15].off >= 0) { wdeP = *reinterpret_cast<const float*>(pPredicted + s_det[15].off); wdeS = *reinterpret_cast<const float*>(pServer + s_det[15].off); }
			if (s_det[16].off >= 0) { wpmP = *reinterpret_cast<const int*>(pPredicted + s_det[16].off);   wpmS = *reinterpret_cast<const int*>(pServer + s_det[16].off);   }

			int mtP = -1, mtS = -1;
			if (s_det[21].off >= 0)
			{
				mtP = *reinterpret_cast<const int*>(pPredicted + s_det[21].off);
				mtS = *reinterpret_cast<const int*>(pServer + s_det[21].off);
			}

			// [PRED-CLASS] magnitudes for the verdict-side classifier.
			s_predClass.dOrgX = dx; s_predClass.dOrgY = dy; s_predClass.dOrgZ = dz;
			s_predClass.sTd = stP - stS;
			s_predClass.tBd = tbP - tbS;
			s_predClass.mtP = mtP;
			s_predClass.mtS = mtS;
			s_predClass.fvP = vp;
			s_predClass.fvS = vs;

			const int wroteDet = snprintf(out + o, outLen > o ? outLen - o : 0,
				"} dOrg=%.3f(%.2f %.2f %.2f) srvOrg=(%.3f %.3f %.3f) predOrg=(%.3f %.3f %.3f) k1=%.3f k2=%.3f k3=%.3f f1=%.3f f2=%.3f f3=%.3f kS=%.3f stamp=%d vel=%.0f/%.0f spr=%d/%d dVOfsZ=%.2f duck=%d/%d flags=%X/%X fallV=%.0f/%.0f pB=(%.3f %.3f %.3f)/(%.3f %.3f %.3f) pV=(%.2f %.2f %.2f)/(%.2f %.2f %.2f) pW=(%.4f %.4f %.4f)/(%.4f %.4f %.4f) pWV=(%.3f %.3f %.3f)/(%.3f %.3f %.3f) tB=%.6f/%.6f nA=%.6f/%.6f sT=%d/%d rm=%.6f/%.6f gNz=%.4f/%.4f wdf=%02X/%02X(m%d/%d a%d/%d u%d/%d) wde=%.4f/%.4f wpm=%d/%d {",
				dk[0], dx, dy, dz, so[0], so[1], so[2], po[0], po[1], po[2],
				dk[1], dk[2], dk[3], fk[1], fk[2], fk[3], kS, nStamp, velP, velS, sprP, sprS,
				dvz, dp, ds, fp, fs, vp, vs,
				pbP[0], pbP[1], pbP[2], pbS[0], pbS[1], pbS[2],
				pvP[0], pvP[1], pvP[2], pvS[0], pvS[1], pvS[2],
				pwP[0], pwP[1], pwP[2], pwS[0], pwS[1], pwS[2],
				pwvP[0], pwvP[1], pwvP[2], pwvS[0], pwvS[1], pwvS[2],
				tbP, tbS, naP, naS, stP, stS, rmP, rmS, gnP, gnS,
				wdfP, wdfS,
				(wdfP & 1) ? 1 : 0, (wdfS & 1) ? 1 : 0,
				(wdfP & 2) ? 1 : 0, (wdfS & 2) ? 1 : 0,
				(wdfP & 4) ? 1 : 0, (wdfS & 4) ? 1 : 0,
				wdeP, wdeS, wpmP, wpmS);
			if (wroteDet > 0) o += (size_t)wroteDet;

			// Knockback windows gate CategorizePosition on both engines, so a landing
			// tick can only be judged next to the windows and the clock they are held
			// against. beginTime/endTime are the nested CKnockBack slot names.
			struct KbSlot { int begin, end; };
			static KbSlot s_kb[4];
			static int s_kbN = 0;
			static int s_dodgeOff = -1;
			static uintptr_t s_kbDmap = 0;
			if (s_kbDmap != dmap)
			{
				s_kbN = 0; s_dodgeOff = -1;
				int pendingBegin = -1;
				for (int i = 0; i < count; ++i)
				{
					const uintptr_t td = fields + (uintptr_t)i * TD_STRIDE;
					const char* name = *reinterpret_cast<const char**>(td + TD_FIELDNAME);
					if (!name) continue;
					const int off = *reinterpret_cast<int*>(td + TD_FLATOFFSET1);
					if (!strcmp(name, "beginTime"))
						pendingBegin = off;
					else if (!strcmp(name, "endTime") && pendingBegin >= 0 && s_kbN < 4)
					{
						s_kb[s_kbN].begin = pendingBegin;
						s_kb[s_kbN].end = off;
						++s_kbN;
						pendingBegin = -1;
					}
					else if (!strcmp(name, "m_dodgingInAir"))
						s_dodgeOff = off;
				}
				s_kbDmap = dmap;
			}
			{
				const float ct = PredNative_CurTime();
				int wrote = snprintf(out + o, outLen > o ? outLen - o : 0, " ct=%.4f td=%+.4f dodge=%d/%d",
					ct, TimeDomain_LastDelta(),
					(s_dodgeOff >= 0) ? pPredicted[s_dodgeOff] : -1,
					(s_dodgeOff >= 0) ? pServer[s_dodgeOff] : -1);
				if (wrote > 0) o += (size_t)wrote;
				for (int k = 0; k < s_kbN; ++k)
				{
					const float bp = *reinterpret_cast<const float*>(pPredicted + s_kb[k].begin);
					const float ep = *reinterpret_cast<const float*>(pPredicted + s_kb[k].end);
					const float bs = *reinterpret_cast<const float*>(pServer + s_kb[k].begin);
					const float es = *reinterpret_cast<const float*>(pServer + s_kb[k].end);
					if (ep <= 0.0f && es <= 0.0f)
						continue;
					wrote = snprintf(out + o, outLen > o ? outLen - o : 0, " kb%d=%.3f-%.3f/%.3f-%.3f%s",
						k, bp, ep, bs, es,
						((bp <= ct && ct <= ep) != (bs <= ct && ct <= es)) ? "!" : "");
					if (wrote > 0) o += (size_t)wrote;
				}
			}
		}
	}
	return nDiff;
}

//-----------------------------------------------------------------------------
// Field-diff then original. Buffers read pre-call.
//-----------------------------------------------------------------------------
static bool h_C_BaseEntity__PostNetworkDataReceived(void* pEntity, __int64 nLatestCmd, int nAcked)
{
	// Ack-count latch for kick residual class join ([KICK-ROW-HIST]/[KICK-ROW-RES]).
	if (nAcked > 0)
		PredAuth_NoteAckCount(static_cast<int>(nLatestCmd), nAcked);

	// [SHOT-IDX-FANOUT] before mask/census/native compare -- the wire only
	// carries S3's pre-split m_shotCount; fan it into m_shotIndexForSpread so
	// the compare, the originalData refresh and any rebase see the fed value.
	if (nAcked > 0 && pEntity)
		PredAuth_ShotIndexFanout(pEntity);

	// Mask before spew and native compare.
	if ((sdk_pred_unfed_mask.GetBool() ||
		sdk_pred_clock_rebase.GetBool() ||
		sdk_pred_teleport_adopt.GetBool()) && nAcked > 0 && pEntity &&
		C_BaseEntity__GetPredictedEntityState)
	{
		__try {
			PredAuth_Apply(pEntity, (unsigned int)nLatestCmd);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			static uint32_t s_nFaults = 0;
			if (++s_nFaults <= 8)
				Warning(eDLL_T::CLIENT, "[PRED-UNFED] exception #%u -- mask skipped this ack\n", s_nFaults);
		}

		// [PRED-CENSUS] name the residual offenders on the WEAPON/VIEWMODEL predictables
		// (post-mask), the ones this ent-1-gated verdict path is blind to. Self-gated +
		// SEH-guarded internally; must run AFTER PredAuth_Apply so masked fields are equalized.
		if (PredDiag_On())
			PredAuth_CensusObserve(pEntity, (unsigned int)nLatestCmd);
	}

	const int nMode = PredDiag_On() ? sdk_pred_diff.GetInt() : 0;
	if (nMode != 0 && nAcked > 0 && pEntity)
	{
		const int nEntIndex = EntField<int>(pEntity, ENT_ENTINDEX);
		if (nMode > 0 || nEntIndex == -nMode)
			PredDiag_SpewFieldDiffs(pEntity, (unsigned int)nLatestCmd, nEntIndex);
	}

	// [ZIP-ALPHA] rail parameter, both sides, local player only. After the mask so
	// the record it reads is the one the native compare is about to see.
	if (nAcked > 0 && pEntity && EntField<int>(pEntity, ENT_ENTINDEX) == 1)
		PredAuth_ZipAlphaProbe(pEntity, (unsigned int)nLatestCmd);

	// [PAIR-SCAN] stamp-vs-state offset in command units (local player only).
	if (PredDiag_On() && sdk_pair_scan.GetBool() && nAcked > 0 && pEntity &&
		EntField<int>(pEntity, ENT_ENTINDEX) == 1)
	{
		PredDiag_PairScan(pEntity, (unsigned int)nLatestCmd);
	}

	// [K-CENSUS] PART 15d -- per-field k census, all predictables (no entindex gate).
	if (PredDiag_On() && sdk_pred_kcensus.GetBool() && nAcked > 0 && pEntity && C_BaseEntity__GetPredictedEntityState)
		PredDiag_KCensus(pEntity, (unsigned int)nLatestCmd);

	// [PRED-DEPTH] per-snapshot ack-count distribution + cadence (not a depth).
	if (PredDiag_On() && sdk_pred_depth.GetBool() && nAcked > 0 && pEntity &&
		EntField<int>(pEntity, ENT_ENTINDEX) == 1)
	{
		struct PredAckAccum { int n; int h[8]; int ackStep[8]; long ackSum; int lastAcked; };
		static PredAckAccum a = { 0, {0}, {0}, 0, -1 };
		const int ackCount = nAcked;
		a.n++;
		a.h[ackCount < 0 ? 0 : (ackCount < 7 ? ackCount : 7)]++;
		a.ackSum += ackCount;
		if (a.lastAcked >= 0)
		{
			const int astep = nAcked - a.lastAcked;
			a.ackStep[astep < 0 ? 0 : (astep < 7 ? astep : 7)]++;
		}
		a.lastAcked = nAcked;
		if (a.n >= 256)
		{
			Warning(eDLL_T::CLIENT,
				"[PRED-DEPTH] n=%d avgAckCount=%.2f ackCount{<0=%d 0=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6+=%d} "
				"ackStep{<0=%d 0=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6+=%d} latest=%d\n",
				a.n, static_cast<double>(a.ackSum) / a.n,
				a.h[0], a.h[1], a.h[2], a.h[3], a.h[4], a.h[5], a.h[6], a.h[7],
				a.ackStep[0], a.ackStep[1], a.ackStep[2], a.ackStep[3], a.ackStep[4], a.ackStep[5], a.ackStep[6], a.ackStep[7],
				static_cast<int>(nLatestCmd));
			a = PredAckAccum{ 0, {0}, {0}, 0, -1 };
		}
	}

	if (PredDiag_On() && sdk_acktick_snap.GetBool() && pEntity && nAcked > 0 &&
		EntField<int>(pEntity, ENT_ENTINDEX) == 1)
	{
		CClientState* const cl = GetBaseLocalClient();
		const int svrTick = cl ? cl->m_nServerTick : -1;
		const int latest  = static_cast<int>(nLatestCmd);
		static int s_lastSvr = 0x7FFFFFFF, s_lastAck = 0x7FFFFFFF, s_lastLat = 0x7FFFFFFF;
		const int dSvr = (svrTick >= 0 && s_lastSvr != 0x7FFFFFFF) ? (svrTick - s_lastSvr) : 0;
		const int dAck = (s_lastAck != 0x7FFFFFFF) ? (nAcked - s_lastAck) : 0;
		const int dLat = (s_lastLat != 0x7FFFFFFF) ? (latest - s_lastLat) : 0;
		if (svrTick >= 0) s_lastSvr = svrTick;
		s_lastAck = nAcked; s_lastLat = latest;
		static unsigned s_ackN = 0;
		if (s_ackN++ < 6000)
			Warning(eDLL_T::CLIENT,
				"[ACKTICK-SNAP] svrTick=%d (dSvr=%d) ackCount=%d (dAck=%d) latest=%d (dLat=%d)\n",
				svrTick, dSvr, nAcked, dAck, latest, dLat);
	}

	char szDiffs[2560]; int nDiffs = -2;
	const bool bVerdictProbe = PredDiag_On() && sdk_pred_verdict.GetBool() && nAcked > 0 && pEntity &&
		C_BaseEntity__GetPredictedEntityState && EntField<int>(pEntity, ENT_ENTINDEX) == 1;
	if (bVerdictProbe)
	{
		// SaveData slot -1 refreshes originalData from live members (otherwise the previous ack's copy).
		__try {
			if (PFN_PredSaveData pSave = PredAuth_SaveData())
				pSave(pEntity, "sdkverdict", static_cast<int>(nLatestCmd), -1);
			nDiffs = PredDiag_CollectDiffNames(pEntity, (unsigned int)nLatestCmd, szDiffs, sizeof(szDiffs));
		} __except (EXCEPTION_EXECUTE_HANDLER) { nDiffs = -3; }
	}

	const bool bHadErrors = C_BaseEntity__PostNetworkDataReceived(pEntity, nLatestCmd, nAcked);

	// [FORCED0-NEUT] tally real entity field errors for the parent Prediction_Dispatch.
	if (PredAuth_DispatchActive() && bHadErrors)
		PredAuth_NoteEntityError();

	if (bVerdictProbe)
	{
		static uint32_t s_nAcks = 0, s_nErr = 0, s_nErrLines = 0;

		// [PRED-CLASS]/[PRED-BOARD] window accumulators + clock-recency + top other-names.
		static int s_clockAge = 999;
		struct ClassAccum {
			uint32_t nClock, nLand, nLandCofire, nLandIndep, nOriginPure, nZip, nPunch, nOther, nGroundEnt;
			uint32_t nLandZup, nLandZdn;   // sign of landing dOrgZ (pred - srv)
			float landZAbsSum, landZAbsMax;
			// [WEAPON-DISABLE #34]
			uint32_t nWeaponFlags, nMeleeTime, nDisableBothSameAck, nDisableCofire, nDisableIndep;
			// [PRED-BOARD]
			uint32_t nZoom, nEdge, nAnimevt, nVel;
		};
		static ClassAccum s_cls = {};
		// Top residual "other" field names this window (engine string ptrs -- stable).
		struct OtherHit { const char* name; uint32_t n; };
		static OtherHit s_otherTop[12] = {};
		auto noteOther = [&](const char* name)
		{
			if (!name) return;
			for (auto& h : s_otherTop)
			{
				if (h.name == name) { ++h.n; return; }
				if (!h.name) { h.name = name; h.n = 1; return; }
			}
			// Full: replace lowest count if this is new (rare).
			uint32_t lo = UINT32_MAX; int loI = 0;
			for (int i = 0; i < 12; ++i)
				if (s_otherTop[i].n < lo) { lo = s_otherTop[i].n; loI = i; }
			s_otherTop[loI].name = name; s_otherTop[loI].n = 1;
		};

		++s_nAcks;
		if (s_clockAge < 999) ++s_clockAge;

		if (bHadErrors)
		{
			++s_nErr;
			char szClass[80] = "?";
			if (nDiffs > 0)
			{
				const PredClass_s& c = s_predClass;
				if (c.clock) s_clockAge = 0;

				snprintf(szClass, sizeof(szClass), "%s%s%s%s%s%s%s%s%s%s%s%s%s",
					c.clock ? "C" : "", c.landing ? "L" : "", c.originF ? "O" : "",
					c.velF ? "V" : "", c.zipline ? "Z" : "", c.punch ? "P" : "",
					c.zoom ? "A" : "", c.edge ? "E" : "", c.animevt ? "N" : "",
					c.weaponFlags ? "D" : "", c.meleeTime ? "M" : "",
					c.other ? "?" : "", c.groundEnt ? "G" : "");
				if (!szClass[0]) snprintf(szClass, sizeof(szClass), "-");

				if (c.clock) s_cls.nClock++;
				if (c.landing)
				{
					s_cls.nLand++;
					// co-fire = a clock error in this ack or within the previous 4.
					if (c.clock || s_clockAge <= 4) s_cls.nLandCofire++;
					else                            s_cls.nLandIndep++;
					if (c.originF)
					{
						const float az = c.dOrgZ < 0 ? -c.dOrgZ : c.dOrgZ;
						s_cls.landZAbsSum += az;
						if (az > s_cls.landZAbsMax) s_cls.landZAbsMax = az;
						if (c.dOrgZ > 0) s_cls.nLandZup++; else if (c.dOrgZ < 0) s_cls.nLandZdn++;
					}
				}
				else if (c.originF && !c.zipline)
					s_cls.nOriginPure++;
				if (c.velF) s_cls.nVel++;
				if (c.zipline) s_cls.nZip++;
				if (c.punch) s_cls.nPunch++;
				if (c.zoom) s_cls.nZoom++;
				if (c.edge) s_cls.nEdge++;
				if (c.animevt) s_cls.nAnimevt++;
				if (c.other)
				{
					s_cls.nOther++;
					noteOther(c.otherName);
				}
				if (c.groundEnt) s_cls.nGroundEnt++;
				// [WEAPON-DISABLE #34] track co-fire with EACH OTHER (shared melee-recovery-
				// timing root?) and with clock (sub-tick TIME-boundary flip on a field the
				// engine's fieldTolerance does NOT cover, since it is FIELD_INTEGER not FLOAT).
				if (c.weaponFlags) s_cls.nWeaponFlags++;
				if (c.meleeTime)   s_cls.nMeleeTime++;
				if (c.weaponFlags && c.meleeTime) s_cls.nDisableBothSameAck++;
				if (c.weaponFlags || c.meleeTime)
				{
					if (c.clock || s_clockAge <= 4) s_cls.nDisableCofire++;
					else                            s_cls.nDisableIndep++;
				}
			}

			// FRESH = collected after our own SaveData(-1) refresh -> byte-identical to
			// what the native compare read. (A post-orig collect is useless: the orig's
			// closing SaveData(slot 0) rebases record[cmd] to the members -> always 0.)
			if (++s_nErrLines <= 400)
				Warning(eDLL_T::CLIENT,
					"[PRED-VERDICT] ERR ack#%u cmd=%u nDiff=%d class=%s sTd=%d tBd=%+.4f dOrgZ=%+.3f clkAge=%d%s%s mt=%d/%d fallVel=%.2f/%.2f FRESH fields{%s}\n",
					s_nAcks, (unsigned int)nLatestCmd, nDiffs, szClass,
					s_predClass.sTd, s_predClass.tBd, s_predClass.dOrgZ,
					s_clockAge > 900 ? -1 : s_clockAge,
					s_predClass.otherName ? " other=" : "",
					s_predClass.otherName ? s_predClass.otherName : "",
					s_predClass.mtP, s_predClass.mtS,
					s_predClass.fvP, s_predClass.fvS,
					(nDiffs > 0) ? szDiffs : "<none-collected>");
		}
		if (s_nAcks >= 512)
		{
			const uint32_t f0 = s_boardForced0;
			const uint32_t pnrN = s_boardPnrCalls;
			s_boardForced0 = 0;
			s_boardPnrCalls = 0;

			// [PRED-BOARD] one-line residual scoreboard (daily-driver: no grepping required).
			// err% of local-player PNRs; family counts are multi-hot per err-ack (sum can
			// exceed errAcks). forced0 = delta==0 free-rebases from PNR-CENSUS this window.
			Warning(eDLL_T::CLIENT,
				"[PRED-BOARD] acks=%u err=%u (%.1f%%) land=%u(indep=%u) origin=%u vel=%u "
				"clock=%u punch=%u zoom=%u edge=%u animevt=%u zip=%u disable=%u other=%u "
				"forced0=%u/%u repair=%d unfed=%d\n",
				s_nAcks, s_nErr, s_nAcks ? 100.0 * s_nErr / s_nAcks : 0.0,
				s_cls.nLand, s_cls.nLandIndep, s_cls.nOriginPure, s_cls.nVel,
				s_cls.nClock, s_cls.nPunch, s_cls.nZoom, s_cls.nEdge, s_cls.nAnimevt,
				s_cls.nZip, s_cls.nWeaponFlags + s_cls.nMeleeTime, s_cls.nOther,
				f0, pnrN,
				sdk_pred_baseline_repair.GetInt(),
				sdk_pred_unfed_mask.GetInt());
			Warning(eDLL_T::CLIENT, "[PRED-VERDICT] window: acks=%u errAcks=%u (%.1f%%)\n",
				s_nAcks, s_nErr, 100.0 * s_nErr / s_nAcks);
			Warning(eDLL_T::CLIENT,
				"[PRED-CLASS] window: clock=%u landing=%u (cofire=%u indep=%u) originPure=%u "
				"zip=%u punch=%u zoom=%u edge=%u animevt=%u other=%u groundEnt=%u "
				"landZ{up=%u dn=%u avgAbs=%.2f max=%.2f} "
				"weaponFlags=%u meleeTime=%u bothSameAck=%u disable(cofire=%u indep=%u)\n",
				s_cls.nClock, s_cls.nLand, s_cls.nLandCofire, s_cls.nLandIndep, s_cls.nOriginPure,
				s_cls.nZip, s_cls.nPunch, s_cls.nZoom, s_cls.nEdge, s_cls.nAnimevt,
				s_cls.nOther, s_cls.nGroundEnt,
				s_cls.nLandZup, s_cls.nLandZdn,
				(s_cls.nLandZup + s_cls.nLandZdn) ? s_cls.landZAbsSum / (s_cls.nLandZup + s_cls.nLandZdn) : 0.f,
				s_cls.landZAbsMax,
				s_cls.nWeaponFlags, s_cls.nMeleeTime, s_cls.nDisableBothSameAck,
				s_cls.nDisableCofire, s_cls.nDisableIndep);
			// Top residual other= names (the next Shape-A candidates).
			char topBuf[512]; size_t to = 0; topBuf[0] = '\0';
			// Simple sort-by-count descending (12 entries).
			for (int pass = 0; pass < 11; ++pass)
				for (int i = 0; i < 11 - pass; ++i)
					if (s_otherTop[i].n < s_otherTop[i + 1].n)
					{
						const OtherHit t = s_otherTop[i];
						s_otherTop[i] = s_otherTop[i + 1];
						s_otherTop[i + 1] = t;
					}
			for (int i = 0; i < 12 && s_otherTop[i].name && s_otherTop[i].n > 0; ++i)
			{
				const int w = snprintf(topBuf + to, sizeof(topBuf) > to ? sizeof(topBuf) - to : 0,
					"%s%s=%u", to ? " " : "", s_otherTop[i].name, s_otherTop[i].n);
				if (w > 0) to += (size_t)w;
			}
			if (topBuf[0])
				Warning(eDLL_T::CLIENT, "[PRED-BOARD] top-other: %s\n", topBuf);

			// [PRED-AUTH] regression alarm: zoom/punch/animevt are fully table-covered,
			// so any nonzero count here means a mask miss / renamed field / dmap cache full.
			PredAuth_BoardAlarm(s_cls.nZoom, s_cls.nPunch, s_cls.nAnimevt);

			s_nAcks = 0; s_nErr = 0; s_nErrLines = 0;
			s_cls = ClassAccum{};
			for (auto& h : s_otherTop) { h.name = nullptr; h.n = 0; }
		}
	}
	return bHadErrors;
}

static ConVar sdk_pnr_census("sdk_pnr_census", "0", FCVAR_DEVELOPMENTONLY,
	"[PNR-CENSUS] count CPrediction-level PostNetworkDataReceived calls: the commands_acknowledged "
	"delta, the native delta==0 forced-error rule, and the had-errors verdict. Read-only. Default 0.");

static void PredDiag_PnrCensus(__int64 a1, unsigned int a2, int a3, char a4, int errAfter, int entErrs, int neutered)
{
	__try
	{
		static uint32_t s_n = 0, s_d0 = 0, s_dneg = 0, s_errAcks = 0, s_neut = 0;
		static long     s_sumD = 0;

		++s_n;
		++s_boardPnrCalls;
		const bool bForced0 = (a3 == 0 && a4);
		if (bForced0) { ++s_d0; ++s_boardForced0; }
		if (a3 < 0) ++s_dneg;
		if (errAfter != 0) ++s_errAcks;
		if (neutered) ++s_neut;
		if (a3 > 0) s_sumD += a3;

		if (bForced0 && (s_d0 <= 24 || (s_d0 % 512) == 0))
		{
			Warning(eDLL_T::CLIENT,
				"[PNR-CENSUS] ack=%u d=%d chg=%d errAfter=%d entErr=%d neut=%d FORCED0\n",
				a2, a3, (int)a4, errAfter, entErrs, neutered);
		}

		if (s_n >= 256)
		{
			Warning(eDLL_T::CLIENT,
				"[PNR-CENSUS] n=%d d0=%d dneg=%d errAcks=%d neut=%d avgD=%.2f lastAck=%u\n",
				s_n, s_d0, s_dneg, s_errAcks, s_neut,
				s_n ? (double)s_sumD / s_n : 0.0, a2);
			s_n = 0; s_d0 = 0; s_dneg = 0; s_errAcks = 0; s_neut = 0; s_sumD = 0;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

static __int64 h_Prediction_Dispatch(__int64 a1, unsigned int a2, int a3, char a4)
{
	if (PredDiag_On() && sdk_pred_acksrc.GetBool())
	{
		// 9 buckets: [<0, 0, 1, 2, 3, 4, 5, 6, 7+]
		struct AckSrcAccum { int n; int h[9]; long ackSum; int ackMin; int ackMax; };
		static AckSrcAccum s = { 0, {0}, 0, 0x7FFFFFFF, -0x7FFFFFFF };
		const int ack = a3;
		s.n++;
		const int b = ack < 0 ? 0 : (ack < 7 ? ack + 1 : 8);
		s.h[b]++;
		s.ackSum += ack;
		if (ack < s.ackMin) s.ackMin = ack;
		if (ack > s.ackMax) s.ackMax = ack;
		if (s.n >= 256)
		{
			CClientState* const cl = GetBaseLocalClient();
			const int cmdTick = (cl && cl->m_CurrFrameSnapshot)
				? cl->m_CurrFrameSnapshot->m_TickUpdate.m_nCommandTick : -1;
			const int outNr = cl ? cl->m_nOutgoingCommandNr : -1;
			Warning(eDLL_T::CLIENT,
				"[PRED-ACKSRC] n=%d avgAck=%.2f ack{<0=%d 0=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7+=%d} "
				"min=%d max=%d | latest=%u cmdTick=%d outNr=%d\n",
				s.n, static_cast<double>(s.ackSum) / s.n,
				s.h[0], s.h[1], s.h[2], s.h[3], s.h[4], s.h[5], s.h[6], s.h[7], s.h[8],
				s.ackMin, s.ackMax, a2, cmdTick, outNr);
			s = AckSrcAccum{ 0, {0}, 0, 0x7FFFFFFF, -0x7FFFFFFF };
		}
	}
	// [FORCED0-NEUT] arm entity-error tally for the duration of the native dispatch
	// (entity PNR hooks accumulate via PredAuth_NoteEntityError).
	PredAuth_DispatchBegin();
	const __int64 r = Prediction_Dispatch(a1, a2, a3, a4);
	const PredAuthDispatchResult_s res = PredAuth_DispatchEnd(a1, a2, a3, a4);

	// [PNR-CENSUS] pure observer, after neut so counts reflect post-fix verdict.
	if (PredDiag_On() && sdk_pnr_census.GetBool() && a1)
		PredDiag_PnrCensus(a1, a2, a3, a4, res.errAfter, res.entErrs, res.neutered);

	return r;
}

static __int64 __fastcall h_PredErrorCheck(__int64 a1, unsigned int a2, __int64 a3, __int64 a4)
{
	bool      doLog = false;
	float     px = 0.0f, py = 0.0f, pz = 0.0f;
	uintptr_t recBefore = 0;

	if (PredDiag_On() && sdk_prederr_probe.GetBool() && a4)
	{
		__try
		{
			if (EntField<int>(reinterpret_cast<void*>(a4), ENT_ENTINDEX) == 1)
			{
				doLog     = true;
				px        = *reinterpret_cast<float*>(a4 + 312);
				py        = *reinterpret_cast<float*>(a4 + 316);
				pz        = *reinterpret_cast<float*>(a4 + 320);
				recBefore = *reinterpret_cast<uintptr_t*>(a4 + 3472);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { doLog = false; }
	}

	const __int64 r = PredErrorCheck(a1, a2, a3, a4);

	if (doLog)
	{
		__try
		{
			const uintptr_t recAfter  = *reinterpret_cast<uintptr_t*>(a4 + 3472);
			const bool      recZero   = (recAfter == 0);

			const int      g3 = *reinterpret_cast<uint8_t*> (a4 + 2032);   // prediction-ctx flag
			const uint32_t g5 = *reinterpret_cast<uint32_t*>(a1 + 1176);   // recvprop list count
			const int      g6 = *reinterpret_cast<uint8_t*> (a1 + 1144);   // list-built latch
			int g4 = 0;                                                    // record exists?
			if (C_BaseEntity__GetPredictedEntityState)
				g4 = C_BaseEntity__GetPredictedEntityState(reinterpret_cast<void*>(a4), a2) ? 1 : 0;

			// EXACT per-command error (only computable once the list is built): delta =
			// |predicted(a4+312) - record's m_vecNetworkOrigin|. MUST use the record (per-command),
			// NOT live wire (which sits the prediction lead ahead -> large even when perfectly predicted).
			float delta = -1.0f;
			if (g5 && g4 && C_BaseEntity__GetPredictedEntityState)
			{
				void* const rec = C_BaseEntity__GetPredictedEntityState(reinterpret_cast<void*>(a4), a2);
				if (rec)
				{
					const uintptr_t recData = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(rec) + 8);
					const uintptr_t listPtr = *reinterpret_cast<uintptr_t*>(a1 + 1152);
					if (recData && listPtr)
					{
						const uintptr_t firstProp = *reinterpret_cast<uintptr_t*>(listPtr);
						if (firstProp)
						{
							const int    fieldOff = *reinterpret_cast<int*>(firstProp + 116);
							const float* net = reinterpret_cast<const float*>(recData + fieldOff);
							const float  dx = net[0] - px, dy = net[1] - py, dz = net[2] - pz;
							delta = sqrtf(dx * dx + dy * dy + dz * dz);
						}
					}
				}
			}

			static int      s_n = 0, s_recZeroN = 0, s_dValid = 0, s_dGt = 0;
			static int      s_g3 = 0, s_g4 = 0, s_g6 = 0; static uint32_t s_g5max = 0;
			static unsigned s_a2min = 0xFFFFFFFFu, s_a2max = 0;
			static double   s_dSum = 0.0; static float s_dMin = 1e9f, s_dMax = 0.0f;
			++s_n;
			if (a2 < s_a2min) s_a2min = a2;
			if (a2 > s_a2max) s_a2max = a2;
			if (recZero) ++s_recZeroN;
			if (g3) ++s_g3; if (g4) ++s_g4; if (g6) ++s_g6; if (g5 > s_g5max) s_g5max = g5;
			if (delta >= 0.0f) { ++s_dValid; s_dSum += delta; if (delta < s_dMin) s_dMin = delta; if (delta > s_dMax) s_dMax = delta; if (delta > 0.5f) ++s_dGt; }
			if (s_n >= 256)
			{
				Warning(eDLL_T::CLIENT,
					"[PREDERR] n=%d recZero=%d | gates{g3ctx=%d g4rec=%d g6built=%d g5cnt=%u} | "
					"delta{n=%d >0.5=%d min=%.3f avg=%.3f max=%.2f} | a2{min=%u max=%u} predicted=(%.1f %.1f %.1f)\n",
					s_n, s_recZeroN, s_g3, s_g4, s_g6, s_g5max,
					s_dValid, s_dGt, s_dValid ? (double)s_dMin : 0.0, s_dValid ? s_dSum / s_dValid : 0.0, (double)s_dMax,
					s_a2min, s_a2max, px, py, pz);
				s_n = 0; s_recZeroN = 0; s_dValid = 0; s_dGt = 0; s_g3 = 0; s_g4 = 0; s_g6 = 0; s_g5max = 0;
				s_a2min = 0xFFFFFFFFu; s_a2max = 0; s_dSum = 0.0; s_dMin = 1e9f; s_dMax = 0.0f;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	return r;
}

//-----------------------------------------------------------------------------
// IDetour implementation
//-----------------------------------------------------------------------------
void VPredDiag::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 81 EC A0 00 00 00 41 8B D8 41 B9 FF FF FF FF")
		.GetPtr(C_BaseEntity__PostNetworkDataReceived);

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 4C 8B 99 F8 07 00 00 44 8B CA 41 80 BB E0 2E 00 00 00 74 48")
		.GetPtr(C_BaseEntity__GetPredictedEntityState);

	Module_FindPattern(g_GameDll,
		"44 89 44 24 18 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 81 EC C0 00 00 00 48 8D 6C 24 40")
		.GetPtr(Prediction_Dispatch);

	// [PREDERR] -- PATH-B prediction-error check (unique; rip-disp wildcarded).
	Module_FindPattern(g_GameDll,
		"40 53 55 41 56 48 83 EC 60 48 8B E9 4D 8B F1 48 8B 0D ?? ?? ?? ?? 8B DA 48 8B 01 FF 90 80 06 00 00 84 C0 0F 84")
		.GetPtr(PredErrorCheck);
}

///////////////////////////////////////////////////////////////////////////////
void VPredDiag::Detour(const bool bAttach) const
{
	if (sdk_pred_diff.GetInt() == 0 && !sdk_pred_unfed_mask.GetBool() && !sdk_pred_verdict.GetBool() &&
		!sdk_pred_clock_rebase.GetBool() && !sdk_pred_teleport_adopt.GetBool())
		return;

	if (C_BaseEntity__PostNetworkDataReceived && C_BaseEntity__GetPredictedEntityState)
	{
		DetourSetup(&C_BaseEntity__PostNetworkDataReceived,
			&h_C_BaseEntity__PostNetworkDataReceived, bAttach);
	}
	else if (bAttach)
	{
		Warning(eDLL_T::CLIENT, "[PRED-DIFF] disabled: pattern resolve failed "
			"(PNR=%p GPS=%p)\n",
			reinterpret_cast<void*>(C_BaseEntity__PostNetworkDataReceived),
			reinterpret_cast<void*>(C_BaseEntity__GetPredictedEntityState));
	}

	// [PRED-ACKSRC] Attach the dispatch hook alongside (sdk_pred_acksrc gates the log
	// inside). Separate guard so a resolve failure here doesn't disable the field diff.
	if (Prediction_Dispatch)
	{
		DetourSetup(&Prediction_Dispatch, &h_Prediction_Dispatch, bAttach);
	}
	else if (bAttach)
	{
		Warning(eDLL_T::CLIENT, "[PRED-ACKSRC] disabled: dispatch pattern resolve failed\n");
	}

	// [PREDERR] Attach the PATH-B error-check hook (sdk_prederr_probe gates the log inside).
	if (PredErrorCheck)
	{
		DetourSetup(&PredErrorCheck, &h_PredErrorCheck, bAttach);
	}
	else if (bAttach)
	{
		Warning(eDLL_T::CLIENT, "[PREDERR] disabled: error-check pattern resolve failed\n");
	}
}
///////////////////////////////////////////////////////////////////////////////
