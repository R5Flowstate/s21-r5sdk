//=============================================================================//
//
// Purpose: Snapshot dump/census (FLATN, MB-PACK, bonefollow, ring watch).
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier1/strtools.h"
#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/detours.h"
#include "engine/server/server.h"
#include "engine/networkstringtable.h"
#include "snapshot_diag.h"
#include "snapshot_dump.h"

#include <TlHelp32.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>

//-----------------------------------------------------------------------------
// CreatePhysicsFollower: owner + bone index; out is follower EHANDLE.
//-----------------------------------------------------------------------------
static ConVar sdk_log_bonefollower_spawn("sdk_log_bonefollower_spawn", "0",
	FCVAR_DEVELOPMENTONLY,
	"Log every phys_bone_follower creation (owner entity/model + chosen bone) "
	"for the client Crash-B / missing-model divergence hunt. Default 1 (on).");

static char __fastcall Hook_CreatePhysicsFollower(int64_t a1, int64_t ownerEnt,
	int* out, int boneIndex)
{
	const char ret = v_CreatePhysicsFollower(a1, ownerEnt, out, boneIndex);

	if (!sdk_log_bonefollower_spawn.GetBool())
		return ret;

	// Convar-gated cold diag; closed-form null chain (no empty SEH swallow).
	uint32_t ownerRefH = 0;
	int16_t ownerModelIdx = 0;
	const char* ownerModel = "?";
	int numBones = 0;
	int bfCount = 0;
	char boneName[128] = "?";

	if (ownerEnt)
	{
		ownerRefH = *reinterpret_cast<const uint32_t*>(ownerEnt + 0x8);
		ownerModelIdx = *reinterpret_cast<const int16_t*>(ownerEnt + 0xDE);

		// CStudioHdr wrapper @ entity+4056 (S3 dedi CBaseAnimating path).
		const uint64_t csh = *reinterpret_cast<const uint64_t*>(ownerEnt + 4056);
		const uint64_t hdr = csh
			? *reinterpret_cast<const uint64_t*>(csh + 8)
			: 0;

		if (hdr)
		{
			ownerModel = reinterpret_cast<const char*>(hdr + 0x10);
			numBones = *reinterpret_cast<const int32_t*>(hdr + 0xA0);
			bfCount = *reinterpret_cast<const int32_t*>(hdr + 0x1E8);

			if (boneIndex >= 0 && boneIndex < numBones)
			{
				const int32_t boneTableOff =
					*reinterpret_cast<const int32_t*>(hdr + 0xA4);
				if (boneTableOff > 0)
				{
					const uint8_t* bonePtr = reinterpret_cast<const uint8_t*>(hdr)
						+ boneTableOff
						+ 184ull * static_cast<uint32_t>(boneIndex);
					const int32_t sznameindex =
						*reinterpret_cast<const int32_t*>(bonePtr);
					const char* pName = reinterpret_cast<const char*>(bonePtr)
						+ sznameindex;
					if (pName)
						V_strncpy(boneName, pName, sizeof(boneName));
				}
			}
		}
	}

	const uint32_t followerH = out ? static_cast<uint32_t>(out[1]) : 0xFFFFFFFFu;

	Warning(eDLL_T::SERVER,
		"[BONEFOLLOW-SPAWN] owner_refH=0x%08X owner_mdlIdx=%d owner_model='%s' "
		"boneIndex=%d boneName='%s' numbones=%d bfCount=%d ret=%d follower_h=0x%08X\n",
		ownerRefH, ownerModelIdx, ownerModel, boneIndex, boneName,
		numBones, bfCount, static_cast<int>(ret), followerH);

	return ret;
}

//-----------------------------------------------------------------------------
// DR0 write watch on ring descriptor entry[0]+8 after Snapshot_Initialize binds it.
//-----------------------------------------------------------------------------
static ConVar sdk_snap_ring_watch("sdk_snap_ring_watch", "0",
	FCVAR_DEVELOPMENTONLY,
	"Arm a hardware write watch on snapshot ring descriptor entry[0]+8 after "
	"Snapshot_Initialize binds it; log each writer's RIP. 0 = off.");

static volatile LONG         s_snapWatchArmed = 0;
static uintptr_t             s_snapWatchAddr = 0;
static std::atomic<uint32_t> s_snapWatchHits{ 0 };

static LONG CALLBACK SnapWatch_Veh(EXCEPTION_POINTERS* p)
{
	if (!p || !p->ExceptionRecord || !p->ContextRecord
		|| p->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
		return EXCEPTION_CONTINUE_SEARCH;

	CONTEXT* const ctx = p->ContextRecord;
	if ((ctx->Dr6 & 0x1) == 0)
		return EXCEPTION_CONTINUE_SEARCH;

	ctx->Dr6 = 0;
	const uint32_t n = s_snapWatchHits.fetch_add(1, std::memory_order_relaxed);
	if (n < 16)
	{
		const uintptr_t rip = static_cast<uintptr_t>(ctx->Rip);
		uintptr_t modBase = 0;
		HMODULE hm = nullptr;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
			| GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(rip), &hm) && hm)
			modBase = reinterpret_cast<uintptr_t>(hm);
		uint64_t val = 0;
		if (s_snapWatchAddr)
			val = *reinterpret_cast<volatile uint64_t*>(s_snapWatchAddr);
		Warning(eDLL_T::SERVER,
			"[SNAP-WATCH] hit#%u write to %p: rip=%p base=%p rva=0x%llX "
			"tid=%lu newval=0x%llX\n",
			n + 1, (void*)s_snapWatchAddr, (void*)rip, (void*)modBase,
			(unsigned long long)(modBase ? rip - modBase : rip),
			GetCurrentThreadId(), (unsigned long long)val);
	}
	if (n >= 15)
	{
		ctx->Dr0 = 0;
		ctx->Dr7 &= ~0xF0003ull;
	}
	return EXCEPTION_CONTINUE_EXECUTION;
}

// Arms from a helper thread (a thread cannot SetThreadContext on itself), and
// keeps rescanning for ~20s so worker threads spawned mid-load get the watch
// too.
static DWORD WINAPI SnapWatch_ArmThread(LPVOID)
{
	const uintptr_t addr = s_snapWatchAddr;
	const DWORD pid = GetCurrentProcessId();
	const DWORD self = GetCurrentThreadId();
	int armedTotal = 0;

	for (int pass = 0; pass < 40; ++pass)
	{
		if (s_snapWatchHits.load(std::memory_order_relaxed) >= 16)
			break;

		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap == INVALID_HANDLE_VALUE)
			break;
		THREADENTRY32 te{};
		te.dwSize = sizeof(te);
		int armedPass = 0;
		if (Thread32First(snap, &te))
		{
			do
			{
				if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
					continue;
				HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT
					| THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
					FALSE, te.th32ThreadID);
				if (!th)
					continue;
				if (SuspendThread(th) != static_cast<DWORD>(-1))
				{
					CONTEXT tctx{};
					tctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
					if (GetThreadContext(th, &tctx)
						&& tctx.Dr0 != static_cast<DWORD64>(addr))
					{
						tctx.Dr0 = addr;
						// L0=1, RW0=01 (write), LEN0=10 (8 bytes)
						tctx.Dr7 = (tctx.Dr7 & ~0xF0003ull)
							| 0x1 | (0x1ull << 16) | (0x2ull << 18);
						if (SetThreadContext(th, &tctx))
							++armedPass;
					}
					ResumeThread(th);
				}
				CloseHandle(th);
			} while (Thread32Next(snap, &te));
		}
		CloseHandle(snap);
		armedTotal += armedPass;
		if (pass == 0)
			Warning(eDLL_T::SERVER,
				"[SNAP-WATCH] armed DR0=%p on %d thread(s), rescanning 20s\n",
				(void*)addr, armedPass);
		Sleep(500);
	}
	Warning(eDLL_T::SERVER,
		"[SNAP-WATCH] arm sweep done (%d total arms, %u hits)\n",
		armedTotal, s_snapWatchHits.load(std::memory_order_relaxed));
	return 0;
}

static void SnapWatch_Arm(uintptr_t addr)
{
	if (!addr || InterlockedCompareExchange(&s_snapWatchArmed, 1, 0) != 0)
		return;
	s_snapWatchAddr = addr;
	AddVectoredExceptionHandler(1, SnapWatch_Veh);
	HANDLE h = CreateThread(nullptr, 0, SnapWatch_ArmThread, nullptr, 0, nullptr);
	if (h)
		CloseHandle(h);
}

// Encode-descriptor blob at mgr+32960. hdr at +8*cid; desc at +1216+4*i.
static constexpr ptrdiff_t kSgeDescBlobOff    = 32960;
static constexpr ptrdiff_t kSgeDescEntriesOff = 1216;

static uintptr_t SGE_DescBlob(void)
{
	const uintptr_t mgr = SnapshotRing_Mgr();
	return mgr ? (mgr + kSgeDescBlobOff) : 0;
}

static void SGE_DescriptorAudit(const char* reason)
{
	const uintptr_t base = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
	const uintptr_t blob = SGE_DescBlob();
	if (!base || !blob)
	{
		Warning(eDLL_T::SERVER, "[DESC-AUDIT] (%s) blob unreachable (mgr null)\n",
			reason ? reason : "?");
		return;
	}

	const int nClasses = S3_RdD(base + kS3_RVA_NumServerClasses);
	if (nClasses <= 0 || nClasses > 1024)
	{
		Warning(eDLL_T::SERVER, "[DESC-AUDIT] (%s) nClasses=%d out of range\n",
			reason ? reason : "?", nClasses);
		return;
	}

	const uintptr_t byId = base + kS3_RVA_ServerClassesByID;
	int scanned = 0, badClasses = 0, detailLines = 0;
	uint64_t totalFields = 0, totalWidth = 0;
	for (int cid = 0; cid < nClasses; ++cid)
	{
		const uintptr_t sc = S3_RdQ(byId + static_cast<uintptr_t>(cid) * 8);
		if (!sc || !S3_PtrLooksHeap(sc))
			continue;
		const uintptr_t st = S3_RdQ(sc + kS3_SC_SendTable);
		const uintptr_t pc = st ? S3_RdQ(st + kS3_ST_Precalc) : 0;
		if (!pc || !S3_PtrLooksHeap(pc))
			continue;
		const uintptr_t flatArr = S3_RdQ(pc + 0x08);
		const int flatN = S3_RdD(pc + kS3_PC_FlatCount);
		if (!flatArr || !S3_PtrLooksHeap(flatArr) || flatN <= 0 || flatN > 4096)
			continue;
		++scanned;

		uint32_t hdr = 0, bDwords = 0;
		SGE_TryReadU32(blob + 8ull * static_cast<uint32_t>(cid), hdr);
		SGE_TryReadU32(blob + 8ull * static_cast<uint32_t>(cid) + 4, bDwords);
		const uint32_t bFields = hdr & 0x7FFu;
		const uint32_t bBase   = hdr >> 11;

		int typeMm = 0, bitsMm = 0;
		uint32_t sumWidth = 0;
		const int walkN = (static_cast<int>(bFields) < flatN)
			? static_cast<int>(bFields) : flatN;
		for (int i = 0; i < walkN; ++i)
		{
			const uintptr_t sp = S3_RdQ(flatArr + 8ull * static_cast<uint32_t>(i));
			if (!sp || !S3_PtrLooksHeap(sp))
				break;
			uint32_t spType = 0, spBits = 0, spWidth = 0;
			SGE_TryReadU32(sp, spType);
			SGE_TryReadU32(sp + 4, spBits);           // SendProp.m_nBits
			SGE_TryReadU32(sp + 76, spWidth);         // SendProp static pack width (dwords)
			sumWidth += spWidth;
			uint32_t desc = 0;
			SGE_TryReadU32(blob + kSgeDescEntriesOff
				+ 4ull * (bBase + static_cast<uint32_t>(i)), desc);
			const uint32_t dType = desc >> 24;
			const uint32_t dBits = (desc >> 16) & 0xFFu;
			const bool tMm = (dType != spType);
			// Array descriptors carry the ELEMENT's nBits; skip the bit compare.
			const bool bMm = (!tMm && spType != 5 && dBits != (spBits & 0xFFu));
			if ((tMm || bMm) && detailLines < 24)
			{
				++detailLines;
				char pn[64] = {};
				uint64_t nmp = 0;
				if (SGE_TryReadU64(sp + 0x40, nmp) && nmp)
					S3_RdStr(static_cast<uintptr_t>(nmp), pn, sizeof(pn));
				Warning(eDLL_T::SERVER,
					"[DESC-AUDIT]   cid=%d flat=%d '%s' spType=%u spBits=%u "
					"descType=%u descBits=%u desc=0x%08X\n",
					cid, i, pn, spType, spBits, dType, dBits, desc);
			}
			if (tMm) ++typeMm;
			if (bMm) ++bitsMm;
		}

		totalFields += bFields;
		totalWidth += bDwords;
		const bool countMm = (bFields != static_cast<uint32_t>(flatN));
		const bool dwordMm = (bDwords != sumWidth);
		if (countMm || dwordMm || typeMm || bitsMm)
		{
			++badClasses;
			char cn[64] = {};
			uint64_t nmp = 0;
			if (SGE_TryReadU64(st + 0x4B8, nmp) && nmp)
				S3_RdStr(static_cast<uintptr_t>(nmp), cn, sizeof(cn));
			Warning(eDLL_T::SERVER,
				"[DESC-AUDIT] cid=%d %s: blobFields=%u flatN=%d blobDwords=%u "
				"sumWidth=%u typeMm=%d bitsMm=%d\n",
				cid, cn, bFields, flatN, bDwords, sumWidth, typeMm, bitsMm);
		}
	}
	Warning(eDLL_T::SERVER,
		"[DESC-AUDIT] (%s) scanned=%d classes, mismatched=%d "
		"totalFields=%llu totalWidth=%llu\n",
		reason ? reason : "?", scanned, badClasses,
		(unsigned long long)totalFields, (unsigned long long)totalWidth);
}

static void CC_SGE_DescAudit_f(const CCommand& args)
{
	SGE_DescriptorAudit("concommand");
}
static ConCommand sdk_desc_audit("sdk_desc_audit", CC_SGE_DescAudit_f,
	"Audit the SGE encode-descriptor blob against the live SendTable flattens.",
	FCVAR_DEVELOPMENTONLY | FCVAR_GAMEDLL | FCVAR_CHEAT);

// Read-only: CPlayerDecoy idx >= live flat count. Cap 10 reports.
static void S21Bridge_ScanDecoyPropIndices(int64_t frameObj,
	int* propIdxCursor, int serverClassId)
{
	static constexpr uint16_t kServerPropEndMarker = 2047;

	static std::atomic<uint32_t> s_scanLog{0};
	if (s_scanLog.load(std::memory_order_relaxed) >= 10)
		return;

	const S21Bridge_ClassMeta meta = S21Bridge_LookupClassMeta("CPlayerDecoy");
	if (meta.classId < 0 || meta.propCount == 0)
		return;
	if (serverClassId != meta.classId || frameObj == 0 || !propIdxCursor)
		return;

	const uint64_t arr = *reinterpret_cast<uint64_t*>(frameObj + 114816);
	if (!arr || !S3_PtrLooksHeap(arr))
		return;

	const int start = *propIdxCursor;
	if (start < 0 || start > 8192)
		return;

	uint16_t* const indices = reinterpret_cast<uint16_t*>(arr);
	int nProps = 0, nImpossible = 0, firstBad = -1, firstBadSlot = -1;
	for (int i = start; i < start + 512; ++i)
	{
		const uint16_t idx = indices[i];

		if (idx == kServerPropEndMarker)
			break;
		++nProps;
		if (idx >= meta.propCount)
		{
			if (firstBad < 0)
			{
				firstBad = idx;
				firstBadSlot = i;
			}
			++nImpossible;
		}
	}

	if (nImpossible > 0)
	{
		const uint32_t n = s_scanLog.fetch_add(1);
		if (n < 10)
			Warning(eDLL_T::SERVER,
				"[DECOY-IDX-SCAN] #%u cursor=%d props=%d impossible=%d "
				"first idx=%d at slot=%d (valid 0..%u)\n",
				n + 1, start, nProps, nImpossible, firstBad, firstBadSlot,
				(unsigned)(meta.propCount - 1));
	}
}

// Residual closed-hunt class dumps (ParticleSystem / Ziprail / etc.) -- default off.
// Whole-wire audit: dump EVERY class's encode precalc so the client's '*' dump
// can be diffed class-by-class. ~2000 lines per boot, so it stays opt-in.
static ConVar bridge_flatn_dump_all("bridge_flatn_dump_all", "0", FCVAR_DEVELOPMENTONLY,
	"One-shot [FLATN-DUMP] of EVERY server class's encode precalc at FLATN-SCAN. "
	"Pair with the client's +bridge_dump_recvflat * and wire_flat_diff.py.");

static ConVar bridge_flatn_dump_residual("bridge_flatn_dump_residual", "0", FCVAR_DEVELOPMENTONLY,
	"One-shot [FLATN-DUMP] of residual encode-precalc classes at FLATN-SCAN. Default 0.");

// Default ON: always dump DT_Player encode flat once so next-run CPlayer
// divergence can be lockstep-diffed against client [RECV-FLAT] DT_Player.
static ConVar bridge_flatn_dump_player("bridge_flatn_dump_player", "0", FCVAR_DEVELOPMENTONLY,
	"One-shot [FLATN-DUMP] of DT_Player encode precalc at first entity-prop write. "
	"Pair with client bridge_dump_recvflat DT_Player (default). 0 = count-only [FLATN-CMP].");

// One line per server class (flatN + nameFnv). Cheap whole-wire census; pair with
// client bridge_flatn_cl_all. Full prop bodies still need bridge_flatn_dump_all.
static ConVar bridge_flatn_all("bridge_flatn_all", "0", FCVAR_DEVELOPMENTONLY,
	"1 = one-shot [FLATN-ALL] line per server class at FLATN-SCAN (cid/table/flatN/nameFnv). "
	"0 = off. ~N lines for N classes; not a full prop dump.");

// FNV-1a 64 of "name\\0" for every flat prop -- cross-side fingerprint without
// re-reading 1k lines. Client prints the same under [FLATN-CL].
static uint64_t Flatn_NameFnv64(uintptr_t flatArr, int flatN)
{
	uint64_t h = 14695981039346656037ull;
	for (int i = 0; i < flatN; ++i)
	{
		const uintptr_t sp = *reinterpret_cast<uintptr_t*>(
			flatArr + static_cast<uintptr_t>(i) * 8);
		const char* pn = (sp && S3_PtrLooksHeap(sp))
			? *reinterpret_cast<const char**>(sp + 0x40) : nullptr;
		const char* s = pn ? pn : "";
		for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p)
		{
			h ^= *p;
			h *= 1099511628211ull;
		}
		h ^= 0;
		h *= 1099511628211ull;
	}
	return h;
}

// Find first flat index whose varname equals want (exact). -1 if missing.
static int Flatn_FindPropIdx(uintptr_t flatArr, int flatN, const char* want)
{
	if (!want || !flatArr || flatN <= 0)
		return -1;
	for (int i = 0; i < flatN; ++i)
	{
		const uintptr_t sp = *reinterpret_cast<uintptr_t*>(
			flatArr + static_cast<uintptr_t>(i) * 8);
		if (!sp || !S3_PtrLooksHeap(sp))
			continue;
		const char* pn = *reinterpret_cast<const char**>(sp + 0x40);
		if (pn && strcmp(pn, want) == 0)
			return i;
	}
	return -1;
}

// Live CPlayer/DT_Player encode report. No hardcoded 1167/1293 scare text --
// client nProps is measured on the client under [FLATN-CL]/[RECV-FLAT].
static void Flatn_ReportPlayerEncode(int cid, const char* nm, uintptr_t st,
	uintptr_t pc, uintptr_t flatArr, int flatN)
{
	const uint64_t nameFnv = (flatArr && flatN > 0)
		? Flatn_NameFnv64(flatArr, flatN) : 0;
	const int idxHealth = Flatn_FindPropIdx(flatArr, flatN, "m_iHealth");
	const int idxOrigin = Flatn_FindPropIdx(flatArr, flatN, "m_vecAbsOrigin");
	const int idxLife   = Flatn_FindPropIdx(flatArr, flatN, "m_lifeState");
	const int idxTeam   = Flatn_FindPropIdx(flatArr, flatN, "m_iTeamNum");

	// Instance-baseline blob for this class (key is class id as decimal string).
	int ibLen = -1;
	int ibBytes = -1;
	if (g_pServer)
	{
		__try
		{
			// CNetworkStringTable* (server.h); same surface as IB-DUMP-DEDI.
			CNetworkStringTable* const ib = g_pServer->GetInstanceBaselineTable();
			if (ib)
			{
				char key[16];
				snprintf(key, sizeof(key), "%d", cid);
				const int nStrings = ib->GetNumStrings();
				for (int i = 0; i < nStrings && i < 512; ++i)
				{
					const char* const k = ib->GetString(i);
					if (!k || strcmp(k, key) != 0)
						continue;
					int len = 0;
					(void)ib->GetStringUserData(i, &len);
					ibLen = len;
					// Engine stores bit length in some paths, byte length in others;
					// print both interpretations so next-run can match client.
					ibBytes = (len > 0) ? ((len + 7) / 8) : 0;
					if (len > 0 && len < 8192 && (len % 8) == 0)
						ibBytes = len; // likely already bytes
					break;
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			ibLen = -2;
		}
	}

	Warning(eDLL_T::SERVER,
		"[FLATN-CMP] LIVE encode cid=%d table='%s' flatN=%d nameFnv=0x%016llX "
		"st=0x%p pc=0x%p flatArr=0x%p "
		"idx m_iHealth=%d m_vecAbsOrigin=%d m_lifeState=%d m_iTeamNum=%d "
		"ib key='%d' len=%d ibBytes~=%d "
		"-- pair with client [FLATN-CL]/[RECV-FLAT] DT_Player; "
		"MATCH only if flatN and nameFnv agree both sides\n",
		cid, nm ? nm : "?", flatN, static_cast<unsigned long long>(nameFnv),
		reinterpret_cast<void*>(st), reinterpret_cast<void*>(pc),
		reinterpret_cast<void*>(flatArr),
		idxHealth, idxOrigin, idxLife, idxTeam,
		cid, ibLen, ibBytes);

	// Head + tail names (cheap orientation without full dump).
	if (flatArr && flatN > 0)
	{
		char line[1024];
		int used = snprintf(line, sizeof(line), "[FLATN-CMP] head:");
		const int headN = (flatN < 12) ? flatN : 12;
		for (int i = 0; i < headN && used > 0 && used < (int)sizeof(line) - 48; ++i)
		{
			const uintptr_t sp = *reinterpret_cast<uintptr_t*>(
				flatArr + static_cast<uintptr_t>(i) * 8);
			const char* pn = (sp && S3_PtrLooksHeap(sp))
				? *reinterpret_cast<const char**>(sp + 0x40) : nullptr;
			used += snprintf(line + used, sizeof(line) - static_cast<size_t>(used),
				" %d='%s'", i, pn ? pn : "?");
		}
		Warning(eDLL_T::SERVER, "%s\n", line);
		if (flatN > 12)
		{
			used = snprintf(line, sizeof(line), "[FLATN-CMP] tail:");
			for (int i = flatN - 8; i < flatN && used > 0 && used < (int)sizeof(line) - 48; ++i)
			{
				const uintptr_t sp = *reinterpret_cast<uintptr_t*>(
					flatArr + static_cast<uintptr_t>(i) * 8);
				const char* pn = (sp && S3_PtrLooksHeap(sp))
					? *reinterpret_cast<const char**>(sp + 0x40) : nullptr;
				used += snprintf(line + used, sizeof(line) - static_cast<size_t>(used),
					" %d='%s'", i, pn ? pn : "?");
			}
			Warning(eDLL_T::SERVER, "%s\n", line);
		}
	}

	// Historical note only (not a live client measurement).
	if (flatN == 1167)
	{
		Warning(eDLL_T::SERVER,
			"[FLATN-CMP] NOTE: flatN=1167 is the historical S3-era / firehose-artifact "
			"count; do NOT treat client as 1293 unless [FLATN-CL] says so this run\n");
	}
}

static void Flatn_DumpProps(const char* nm, int cid, uintptr_t flatArr, int flatN)
{
	Warning(eDLL_T::SERVER,
		"[FLATN-DUMP] %s (cid=%d) ENCODE-time precalc flatN=%d -- listing flat props:\n",
		nm ? nm : "<null>", cid, flatN);
	for (int di = 0; di < flatN; ++di)
	{
		const uintptr_t dsp = *reinterpret_cast<uintptr_t*>(
			flatArr + static_cast<uintptr_t>(di) * 8);
		const char* dpn = dsp ? *reinterpret_cast<const char**>(dsp + 0x40) : nullptr;
		const int dty   = dsp ? *reinterpret_cast<int*>(dsp + 0x00) : -1;
		const int dnb   = dsp ? *reinterpret_cast<int*>(dsp + 0x04) : -1;
		const int dfl   = dsp ? *reinterpret_cast<int*>(dsp + 0x58) : 0;
		const int doff  = dsp ? *reinterpret_cast<int*>(dsp + 0x78) : -1;
		Warning(eDLL_T::SERVER,
			"[FLATN-DUMP]   [%4d] ty=%d nBits=%d fl=0x%X off=0x%X s='%s'\n",
			di, dty, dnb, dfl, doff, dpn ? dpn : "<null>");
	}
	Warning(eDLL_T::SERVER, "[FLATN-DUMP] %s end (flatN=%d)\n", nm ? nm : "<null>", flatN);
}

//-----------------------------------------------------------------------------
// uint16 index array at frameObj+114816, from *propIdxCursor, terminated by 2047.
//-----------------------------------------------------------------------------
static ConVar sdk_mb_pack_diag("sdk_mb_pack_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[MB-PACK] Census whether m_mantleBoostState is staged in the changed-prop stream. 0 = off.");

static void S21Bridge_MantleBoostPackProbe(int64_t frameObj,
	int* propIdxCursor, int serverClassId)
{
	if (!sdk_mb_pack_diag.GetBool())
		return;

	static constexpr uint16_t kServerPropEndMarker = 2047;

	// Resolved once: CPlayer's class id and the flat index of the prop.
	static int s_playerCid = -2;   // -2 = not attempted, -1 = unresolvable
	static int s_mbIdx     = -1;

	if (s_playerCid == -2)
	{
		s_playerCid = -1;
		const S21Bridge_ClassMeta meta = S21Bridge_LookupClassMeta("CPlayer");
		const uintptr_t fb = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
		if (meta.classId >= 0 && fb)
		{
			const uintptr_t sc = *reinterpret_cast<uintptr_t*>(
				fb + kS3_RVA_ServerClassesByID + static_cast<uintptr_t>(meta.classId) * 8);
			const uintptr_t st = (sc && S3_PtrLooksHeap(sc))
				? *reinterpret_cast<uintptr_t*>(sc + kS3_SC_SendTable) : 0;
			const uintptr_t pc = (st && S3_PtrLooksHeap(st))
				? *reinterpret_cast<uintptr_t*>(st + kS3_ST_Precalc) : 0;
			const uintptr_t flatArr = (pc && S3_PtrLooksHeap(pc))
				? *reinterpret_cast<uintptr_t*>(pc + 0x08) : 0;
			const int flatN = (pc && S3_PtrLooksHeap(pc))
				? *reinterpret_cast<int*>(pc + kS3_PC_FlatCount) : 0;
			if (flatArr && S3_PtrLooksHeap(flatArr) && flatN > 0 && flatN <= 4096)
			{
				s_mbIdx = Flatn_FindPropIdx(flatArr, flatN, "m_mantleBoostState");
				s_playerCid = meta.classId;
			}
		}
		Warning(eDLL_T::SERVER,
			"[MB-PACK] probe armed: CPlayer cid=%d m_mantleBoostState flatIdx=%d "
			"(cid<0 or idx<0 = probe inert)\n", s_playerCid, s_mbIdx);
	}

	if (s_playerCid < 0 || s_mbIdx < 0 || serverClassId != s_playerCid ||
		frameObj == 0 || !propIdxCursor)
		return;

	const uintptr_t arr = *reinterpret_cast<uintptr_t*>(frameObj + 114816);
	if (!arr || !S3_PtrLooksHeap(arr))
		return;

	const int start = *propIdxCursor;
	if (start < 0 || start > 8192)
		return;

	const uint16_t* const indices = reinterpret_cast<const uint16_t*>(arr);
	int nProps = 0, maxIdx = -1;
	bool bFound = false;
	for (int i = start; i < start + 512; ++i)
	{
		const uint16_t idx = indices[i];
		if (idx == kServerPropEndMarker)
			break;
		++nProps;
		if (idx > maxIdx)
			maxIdx = idx;
		if (idx == static_cast<uint16_t>(s_mbIdx))
			bFound = true;
	}

	static uint32_t s_bodies = 0;
	static uint32_t s_hits   = 0;
	++s_bodies;
	if (bFound)
		++s_hits;

	// The first staging is the whole answer to "encode or decode", so say it once,
	// loudly. After that a periodic census keeps the negative case falsifiable --
	// silence would read the same as "the probe never ran".
	if (bFound && s_hits == 1)
	{
		Warning(eDLL_T::SERVER,
			"[MB-PACK] STAGED: idx=%d is in the changed-prop stream (body #%u, "
			"props=%d maxIdx=%d) -- the encode side is fine, hunt the client decode\n",
			s_mbIdx, s_bodies, nProps, maxIdx);
	}
	else if ((s_bodies % 500) == 0)
	{
		Warning(eDLL_T::SERVER,
			"[MB-PACK] census: %u CPlayer bodies, %u carried idx=%d; last body "
			"props=%d maxIdx=%d\n", s_bodies, s_hits, s_mbIdx, nProps, maxIdx);
	}
}

void SnapshotDump_OnWriteEntityProps(int64_t frameObj, int* propIdxCursor, int serverClassId)
{
	S21Bridge_MantleBoostPackProbe(frameObj, propIdxCursor, serverClassId);
	S21Bridge_ScanDecoyPropIndices(frameObj, propIdxCursor, serverClassId);

	if (!(bridge_flatn_dump_all.GetBool() || bridge_flatn_dump_player.GetBool()
		|| bridge_flatn_dump_residual.GetBool() || bridge_flatn_all.GetBool()))
		return;

	static volatile LONG s_flatScanDone = 0;
	if (serverClassId < 0 || serverClassId >= 4096
		|| InterlockedCompareExchange(&s_flatScanDone, 1, 0) != 0)
		return;

	const uintptr_t fb = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
	int scanned = 0, bad = 0;
	int playerCid = -1;
	int playerFlatN = -1;
	Warning(eDLL_T::SERVER,
		"[FLATN-SCAN] scanning all server-class encode precalcs for 13b model-index props "
		"+ LIVE DT_Player [FLATN-CMP]...\n");
	for (int cid = 0; fb && cid < 1024; ++cid)
	{
		const uintptr_t sc = *reinterpret_cast<uintptr_t*>(
			fb + kS3_RVA_ServerClassesByID + static_cast<uintptr_t>(cid) * 8);
		if (!sc || !S3_PtrLooksHeap(sc)) continue;
		const uintptr_t st = *reinterpret_cast<uintptr_t*>(sc + kS3_SC_SendTable);
		if (!st || !S3_PtrLooksHeap(st)) continue;
		const char* nm = *reinterpret_cast<const char**>(st + kS3_ST_NetTableName);
		const uintptr_t pc = *reinterpret_cast<uintptr_t*>(st + kS3_ST_Precalc);
		if (!pc || !S3_PtrLooksHeap(pc)) continue;
		const uintptr_t flatArr = *reinterpret_cast<uintptr_t*>(pc + 0x08);
		const int flatN = *reinterpret_cast<int*>(pc + kS3_PC_FlatCount);
		if (!flatArr || !S3_PtrLooksHeap(flatArr) || flatN <= 0 || flatN > 4096) continue;
		++scanned;

		const bool isPlayer = (nm && strcmp(nm, "DT_Player") == 0);
		if (isPlayer)
		{
			playerCid = cid;
			playerFlatN = flatN;
			Flatn_ReportPlayerEncode(cid, nm, st, pc, flatArr, flatN);
		}

		if (bridge_flatn_all.GetBool() && nm)
		{
			const uint64_t nameFnv = Flatn_NameFnv64(flatArr, flatN);
			Warning(eDLL_T::SERVER,
				"[FLATN-ALL] cid=%d table='%s' flatN=%d nameFnv=0x%016llX\n",
				cid, nm, flatN, static_cast<unsigned long long>(nameFnv));
		}

		const bool dumpAlways = bridge_flatn_dump_all.GetBool()
			|| (isPlayer && bridge_flatn_dump_player.GetBool());
		const bool dumpResidual = bridge_flatn_dump_residual.GetBool() &&
			((nm && (strcmp(nm, "DT_ParticleSystem") == 0 ||
					 strcmp(nm, "DT_TriggerSlipSphere") == 0 ||
					 strcmp(nm, "DT_TEEffectDispatch") == 0 ||
					 strcmp(nm, "DT_Ziprail") == 0 ||
					 strcmp(nm, "DT_Zipline") == 0 ||
					 strcmp(nm, "DT_Team") == 0 ||
					 strcmp(nm, "DT_EffectData") == 0)) ||
			 cid == 65 || cid == 120 || cid == 21 || cid == 82);
		if (dumpAlways || dumpResidual)
			Flatn_DumpProps(nm, cid, flatArr, flatN);

		for (int fi = 0; fi < flatN; ++fi)
		{
			const uintptr_t sp = *reinterpret_cast<uintptr_t*>(flatArr + static_cast<uintptr_t>(fi) * 8);
			if (!sp) continue;
			const char* pn = *reinterpret_cast<const char**>(sp + 0x40);
			const int nb = *reinterpret_cast<int*>(sp + 0x04);
			if (pn && nb == 13 && strstr(pn, "ModelIndex"))
			{
				++bad;
				Warning(eDLL_T::SERVER,
					"[FLATN-SCAN] *** STILL 13b *** class='%s' [%d] prop='%s'\n",
					nm ? nm : "?", fi, pn);
			}
		}
	}
	Warning(eDLL_T::SERVER,
		"[FLATN-SCAN] done: %d classes scanned, %d model-index prop(s) still at 13b "
		"(each drifts vs the client's 14b decode -> OOB); "
		"DT_Player cid=%d flatN=%d; [FLATN-ALL] lines=%s\n",
		scanned, bad, playerCid, playerFlatN,
		bridge_flatn_all.GetBool() ? "on" : "off");
	if (playerCid < 0)
	{
		Warning(eDLL_T::SERVER,
			"[FLATN-CMP] WARN: DT_Player sendtable not found in server-class list "
			"(no LIVE encode flatN this scan)\n");
	}

	(void)frameObj;
}

void SnapshotDump_OnFirstPack(void)
{
	SGE_DescriptorAudit("first-pack");
}

void SnapshotDump_OnRingInit(uintptr_t ring, uintptr_t entry0)
{
	if (sdk_snap_ring_watch.GetBool() && ring && entry0)
		SnapWatch_Arm(ring + 8);
}

void VSnapshotDump::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 41 56 41 57 48 81 EC "
		"?? ?? ?? ?? 48 83 BA")
		.GetPtr(v_CreatePhysicsFollower);

	if (v_CreatePhysicsFollower)
		Msg(eDLL_T::SERVER,
			"[BONEFOLLOW-SPAWN] CreatePhysicsFollower resolved: %p\n",
			(void*)v_CreatePhysicsFollower);
	else
		Warning(eDLL_T::SERVER,
			"[BONEFOLLOW-SPAWN] CreatePhysicsFollower pattern unresolved\n");
}

void VSnapshotDump::Detour(const bool bAttach) const
{
	if (v_CreatePhysicsFollower)
		DetourSetup(&v_CreatePhysicsFollower, &Hook_CreatePhysicsFollower, bAttach);
	else if (bAttach)
		Warning(eDLL_T::SERVER,
			"[BONEFOLLOW-SPAWN] CreatePhysicsFollower pattern unresolved -- "
			"spawn dump NOT active.\n");
}
