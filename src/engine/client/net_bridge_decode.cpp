//=============================================================================//
//
// Purpose: net_bridge -- recvtable / prop-apply decode + camo + classname hooks.
//
//=============================================================================//
#include "core/stdafx.h"
#include "engine/client/net_observer.h"
#include "engine/client/net_bridge_internal.h"

#include "engine/cmd.h"
#include "engine/net.h"
#include "engine/net_chan.h"
#include "engine/host.h"
#include "engine/client/clientstate.h"
#include "engine/client/client.h"
#include "public/tier1/cmd.h"
#include "public/bspflags.h"
#include "tier1/cvar.h"
#include "tier0/commandline.h"
#include "tier0/memvalidate.h"

#include "game/client/mantle_boost.h"
#include "game/client/pred_authority.h"

#include <cstdint>
#include <cstring>
#include <string>

#pragma warning(disable: 4456 4459)

// Forward: defined below PropDecodeDispatch; used by SNDC-LINK / RestoreSndc preflights.
static bool S21_IsReadablePtr(const void* p, size_t bytes);

// SendProp in the decoder flat precalc (CRecvDecoder+0x18), 0x88 layout.
// +0x28 is m_nElements, not the name; the name lives at +0x40.
constexpr uint32_t SP_VARNAME = 0x40;

//-----------------------------------------------------------------------------
// Peek nBits of a bf_read without consuming them. Nothing here writes the cursor.
//-----------------------------------------------------------------------------
static bool DT_PeekFixedBits(uintptr_t bitbuf, int nBits, int* pOut)
{
	if (bitbuf < 0x10000 || nBits <= 0 || nBits > 32 || !pOut)
		return false;

	__try
	{
		if (*reinterpret_cast<const unsigned char*>(bitbuf + 0x08))
			return false;   // buffer already overflowed -- nothing trustworthy left

		const uint32_t nCache = *reinterpret_cast<const uint32_t*>(bitbuf + 0x20);
		const int      nLeft  = *reinterpret_cast<const int*>(bitbuf + 0x24);
		if (nLeft < 0 || nLeft > 32)
			return false;

		const uint32_t nMask = (nBits == 32) ? 0xFFFFFFFFu : ((1u << nBits) - 1u);

		if (nLeft >= nBits)
		{
			*pOut = static_cast<int>(nCache & nMask);
			return true;
		}

		const uint32_t* const pNext = *reinterpret_cast<const uint32_t* const*>(bitbuf + 0x28);
		const uint32_t* const pEnd  = *reinterpret_cast<const uint32_t* const*>(bitbuf + 0x30);
		if (!pNext || pNext >= pEnd)
			return false;   // value straddles the end of the buffer

		const uint32_t nLo = (nLeft == 0)
			? 0u : (nCache & ((1u << nLeft) - 1u));
		*pOut = static_cast<int>((nLo | (*pNext << nLeft)) & nMask);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

//-----------------------------------------------------------------------------
// DT_Player wire props with no S21 RecvProp. Peek before skip; re-resolve on decoder change.
//-----------------------------------------------------------------------------
static uintptr_t s_mbDecoder      = 0;
static uintptr_t s_mbSendProp     = 0;
static uintptr_t s_duckRemSendProp = 0;

static void Bridge_LatchWirePeekProps(__int64 decoder)
{
	s_mbDecoder       = static_cast<uintptr_t>(decoder);
	s_mbSendProp      = 0;
	s_duckRemSendProp = 0;

	uint8_t** spA = nullptr;
	int       nPre = 0;
	__try
	{
		nPre = *reinterpret_cast<int*>(decoder + 0x20);
		spA  = *reinterpret_cast<uint8_t***>(decoder + 0x18);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		spA = nullptr;
	}

	if (!spA || nPre <= 0 || nPre > 4096)
		nPre = 0;   // fall through to the announce -- a silent bail reads as "never arrived"

	for (int i = 0; i < nPre; ++i)
	{
		if (s_mbSendProp && s_duckRemSendProp)
			break;
		__try
		{
			uint8_t* const sp = spA[i];
			const char* const pn = sp
				? *reinterpret_cast<const char**>(sp + SP_VARNAME) : nullptr;
			if (!pn)
				continue;
			if (!s_mbSendProp && strcmp(pn, "m_mantleBoostState") == 0)
				s_mbSendProp = reinterpret_cast<uintptr_t>(sp);
			else if (!s_duckRemSendProp && strcmp(pn, "m_nDuckTransitionTimeMsecs") == 0)
				s_duckRemSendProp = reinterpret_cast<uintptr_t>(sp);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	// Every re-latch, not just the first: a rebuilt decoder that resolves to
	// nothing would otherwise silently switch the capture off for the rest of the
	// session, and look exactly like a prop that never arrives.
	static int s_nLatches = 0;
	if (++s_nLatches <= 8)
		Warning(eDLL_T::ENGINE,
			"[WIRE-PEEK] m_mantleBoostState %s, m_nDuckTransitionTimeMsecs %s in the "
			"DT_Player decoder %p (flatN=%d) (#%d)\n",
			s_mbSendProp ? "resolved" : "NOT FOUND",
			s_duckRemSendProp ? "resolved" : "NOT FOUND",
			reinterpret_cast<void*>(decoder), nPre, s_nLatches);
}

// Opt-in per-prop decode diagnostics (PROP-ORDER / PROP-AUDIT bitCost). Default off
// so the hot dispatch path is bounds + identity + orig only.
static ConVar bridge_prop_dispatch_diag("bridge_prop_dispatch_diag", "0",
	FCVAR_DEVELOPMENTONLY | FCVAR_CLIENTDLL,
	"1 = enable PROP-ORDER + PROP-AUDIT bitCost probes on every prop dispatch. "
	"0 = off (default, lightweight).");

// S21 builds omit the iClass bounds check before m_pServerClasses; guard restores it.
static ConVar bridge_delta_class_guard("bridge_delta_class_guard", "1",
	FCVAR_RELEASE | FCVAR_CLIENTDLL,
	"1 = validate the class index recovered from the delta buffer before the "
	"engine indexes m_pServerClasses with it (default). 0 = off.");

ConVar bridge_decode_seh("bridge_decode_seh", "0",
	FCVAR_DEVELOPMENTONLY | FCVAR_CLIENTDLL,
	"1 = wrap native RecvTable/dispatch/CopyNew/svc_Snapshot in SEH. 0 = ship (no swallow).");

// One-shot dump of the client's decoder precalc flat list (what wire indices
// address). Diff against the dedi's [FLATN-DUMP] for the same table. Empty /
// 0 disables; enable with +bridge_dump_recvflat or the ConVar at runtime under -devsdk.
static ConVar bridge_dump_recvflat("bridge_dump_recvflat", "",
	FCVAR_DEVELOPMENTONLY | FCVAR_CLIENTDLL,
	"One-shot [RECV-FLAT] dump of the client decoder precalc on first decode. "
	"Comma-separated table names, or 0/empty to disable. Boot: +bridge_dump_recvflat.");

// One-line [FLATN-CL] count+fingerprint for DT_Player so next-run can MATCH
// dedi [FLATN-CMP] nameFnv. Off by default; independent of full [RECV-FLAT].
static ConVar bridge_flatn_cl("bridge_flatn_cl", "0",
	FCVAR_DEVELOPMENTONLY | FCVAR_CLIENTDLL,
	"1 = one-shot [FLATN-CL] LIVE client decoder flatN+nameFnv for DT_Player on first decode. "
	"0 = off. Independent of bridge_dump_recvflat full dump.");

// One line per RecvTable the client actually decodes (flatN+nameFnv). Pair with
// dedi [FLATN-ALL]. Full prop bodies still need bridge_dump_recvflat *.
static ConVar bridge_flatn_cl_all("bridge_flatn_cl_all", "0",
	FCVAR_DEVELOPMENTONLY | FCVAR_CLIENTDLL,
	"1 = one-shot [FLATN-CL-ALL] line per table on first decode (table/flatN/nameFnv). "
	"0 = off. Only tables that appear on the wire get a line.");

// Opt-in per-prop [WIRE-PAIR] row dump. Census summary (nPre/nRcv + mismatch
// count) is always on; this only gates the per-index send/recv name lines.
static ConVar sdk_wire_pair_rows("sdk_wire_pair_rows", "0",
	FCVAR_DEVELOPMENTONLY | FCVAR_CLIENTDLL,
	"1 = dump per-prop [WIRE-PAIR] send/recv name rows. "
	"0 = off (default; census summary is always on).");

// [SNAP-PROF] per-inject entity-create/decode counters and timing.
// Declared extern in net_bridge_internal.h; reset before each inject
// (SnapProf_Reset); read after ProcessMessages for the [SNAP-PROF] log.
long long g_snapProfCreateN    = 0;
long long g_snapProfDecodeN    = 0;
long long g_snapProfCreateTicks = 0;
bool      g_snapProfOn         = false;

void SnapProf_Reset(void)
{
	g_snapProfCreateN     = 0;
	g_snapProfDecodeN     = 0;
	g_snapProfCreateTicks = 0;
	g_snapProfOn          = true;
}

// Real VirtualQuery syscalls (readable-cache misses) and caught exceptions
// on the per-prop dispatch path. Reset with SnapBudget_Reset for [SNAP-BUDGET].
volatile long g_dispVqCalls = 0;
volatile long g_dispSehHits = 0;

SnapBudgetEntry_t g_snapBudget[SNAPB_COUNT] = {};
bool              g_snapBudgetOn = false;

void SnapBudget_Reset(void)
{
	for (int i = 0; i < SNAPB_COUNT; ++i)
	{
		g_snapBudget[i].m_nTotalTicks = 0;
		g_snapBudget[i].m_nOrigTicks  = 0;
		g_snapBudget[i].m_nCalls      = 0;
	}
	g_dispVqCalls = 0;
	g_dispSehHits = 0;
}

// -- SVC_DatatableChecksum::Process. Native handler
// 1. lookup table by name
typedef char    (__fastcall *PFN_SVC_DatatableChecksum_Process)(void* a1, void* a2);

static PFN_SVC_DatatableChecksum_Process s_origDatatableChkProc   = nullptr;

// Default ON: S3 <-> S21 table CRCs differ by construction on this bridge.
static ConVar bridge_dt_checksum_swallow("bridge_dt_checksum_swallow", "1",
	FCVAR_RELEASE | FCVAR_CLIENTDLL,
	"1 = swallow SVC_DatatableChecksum mismatch with a loud rate-limited Warning "
	"(default; S3 <-> S21 table CRCs differ by construction). "
	"0 = call native Process, which disconnects on mismatch.");

// OOB prop index storm: Host_Error after N hits per window (default). 0 = entity-abort only.
static ConVar bridge_prop_oob_disconnect_limit("bridge_prop_oob_disconnect_limit", "32",
	FCVAR_RELEASE | FCVAR_CLIENTDLL,
	"Host_Error after this many OOB prop indices per bridge_prop_oob_window_sec "
	"(default 32). 0 = abort entity only (lab). Boot: +bridge_prop_oob_disconnect_limit 0");

static ConVar bridge_prop_oob_window_sec("bridge_prop_oob_window_sec", "10", FCVAR_RELEASE,
	"Sliding window for the OOB prop disconnect limit (seconds). 0 = count over the "
	"whole session (legacy).");

// Hook on DataTable_SetupReceiveTableFromSendTable.
// Diagnostic: logs every call so we can see if the engine finds our stubs.
typedef char (__fastcall *PFN_DataTable_SetupRecv)(__int64 a1, __int64 a2, __int64 a3, __int64 a4);

static PFN_DataTable_SetupRecv s_origDataTableSetupRecv = nullptr;

// Synthesize a ClientClass for CScriptNetData_SNDC_GLOBAL_NON_REWIND into g_pClientClassHead.
static bool s_linkedNonRewindClientClass = false;

static uint8_t s_nrClientClassBuf[48] = {};
static constexpr int kNonRewindClassSize = 0xD50;

static void Bridge_LinkNonRewindClientClass()
{
	if (s_linkedNonRewindClientClass)
		return;
	s_linkedNonRewindClientClass = true;

	const uintptr_t base = NetObs_GetExeModuleBase();
	if (!base) return;

	uintptr_t* pHead = (uintptr_t*)NetObs_NonRewindClientClassHeadAddr();

	uintptr_t recvTable = NetObs_NonRewindRecvTableAddr();
	uintptr_t nameStr   = NetObs_NonRewindNameAddr();

	if (!S21_IsReadablePtr(reinterpret_cast<const void*>(nameStr), 40))
	{
		Warning(eDLL_T::CLIENT,
			"[SNDC-LINK] name string unreadable at %p -- skipping\n", (void*)nameStr);
		return;
	}
	{
		const char* cn = reinterpret_cast<const char*>(nameStr);
		if (!cn || strncmp(cn, "CScriptNetData_SNDC_GLOBAL_NON_REWIND", 37) != 0)
		{
			Warning(eDLL_T::CLIENT,
				"[SNDC-LINK] name string mismatch at %p -- skipping\n", (void*)nameStr);
			return;
		}
	}

	// Get a donor vtable from the first ClientClass in the linked list.
	// CL_CopyNewEntity calls vtable methods on the ClientClass; a NULL vtable
	// silently fails entity creation (entity never instantiated, all deltas lost).
	uintptr_t donorVtable = 0;
	if (S21_IsReadablePtr(pHead, sizeof(uintptr_t)))
	{
		const uintptr_t firstCC = *pHead;
		if (firstCC && S21_IsReadablePtr(reinterpret_cast<const void*>(firstCC), sizeof(uintptr_t)))
			donorVtable = *reinterpret_cast<uintptr_t*>(firstCC); // +0x00 = vtable
	}

	if (!S21_IsReadablePtr(pHead, sizeof(uintptr_t)))
	{
		Warning(eDLL_T::CLIENT, "[SNDC-LINK] ClientClass head unreadable -- skipping\n");
		return;
	}

	memset(s_nrClientClassBuf, 0, sizeof(s_nrClientClassBuf));
	*(uintptr_t*)(s_nrClientClassBuf + 0x00) = donorVtable;  // ClientClass vtable
	*(uintptr_t*)(s_nrClientClassBuf + 0x10) = nameStr;      // m_pNetworkName
	*(uintptr_t*)(s_nrClientClassBuf + 0x18) = recvTable;    // m_pRecvTable
	*(uintptr_t*)(s_nrClientClassBuf + 0x20) = *pHead;       // m_pNext
	*(int*)(s_nrClientClassBuf + 0x28)       = -1;           // m_ClassID
	*(int*)(s_nrClientClassBuf + 0x2C)       = kNonRewindClassSize; // class size is last Recv array end, not m_bools offset

	*pHead = (uintptr_t)s_nrClientClassBuf;

	Warning(eDLL_T::ENGINE,
		"[SNDC-LINK] synthesized ClientClass for CScriptNetData_SNDC_GLOBAL_NON_REWIND "
		"(RecvTable=0x%p)\n", (void*)recvTable);
}

static char __fastcall Hook_DataTable_SetupRecv(__int64 a1, __int64 a2, __int64 a3, __int64 a4)
{
	Bridge_LinkNonRewindClientClass();

	// a1 = SendTable*. S21 name is at *(char**)(a1 + 0x4B8); 1208 is a rare alt layout.
	const char* name = nullptr;
	if (a1 && S21_IsReadablePtr(reinterpret_cast<const void*>(a1 + 0x4B8), sizeof(const char*)))
		name = *reinterpret_cast<const char**>(a1 + 0x4B8);
	if (!name && a1 && S21_IsReadablePtr(reinterpret_cast<const void*>(a1 + 1208), sizeof(const char*)))
		name = *reinterpret_cast<const char**>(a1 + 1208);

	char result = s_origDataTableSetupRecv(a1, a2, a3, a4);

	BridgeStubDiag("[DT-SETUP] '%s' a2=%lld result=%d\n",
		name ? name : "?", (long long)a2, (int)result);

	return result;
}


// Forward declarations for cross-hook communication.
// THREAD-LOCAL: S21 runs DT decode on two threads; process-global shared context shears concurrent prop streams.
static thread_local __int64 s_currentDecodeRecvTable = 0;


static thread_local int s_decodeOobProp = 0;
static thread_local int s_copyNewOob = 0;

static void Bridge_PropStreamIntegrityFail(__int64 bitbuf, const char* reason,
	const char* tableName, int propIdx, long eventN);

typedef void (__fastcall *PFN_BitbufSeek)(__int64 bitbuf, int bits);

static PFN_BitbufSeek s_bitbufSeek = nullptr;


// -- entity property decode/merge (RecvTable_DecodeZeros equivalent).
// First arg = RecvTable*. *(RecvTable + 0x4C0) = m_data.decoder (CRecvDecoder*).
// S3-only entity classes have no decoder -> NULL -> crash at [rsi+18h].
typedef __int64 (__fastcall *PFN_RecvTableDecode)(__int64, __int64, __int64, __int64);

static PFN_RecvTableDecode s_origRecvTableDecode = nullptr;

// CreateDecoders -- matches SendProps to RecvProps by name.
// S3->S21 renames cause props to go unmatched. Fix: rename S21 RecvProp names
// to S3 equivalents before the original runs, so the name matcher finds them.
typedef __int64 (__fastcall *PFN_CreateDecoders)(__int64, __int64, __int64, __int64);

static PFN_CreateDecoders s_origCreateDecoders = nullptr;


// [BIND-CENSUS] For every SendProp the dedi ships, report whether it paired with
// a client RecvProp or landed on the unmatched dummy (bits consumed, member never
// written). Read-only walk of the ClientClass SendProp list.
static ConVar sdk_bind_census("sdk_bind_census", "0", FCVAR_DEVELOPMENTONLY,
	"[BIND-CENSUS] When 1, dump wire-pairing once after CreateDecoders (re-arms "
	"when the ClientClass count changes). 0=off (default).");

// A name the dedi unbinds on purpose (dt_extend's __skip_ prefix). Counted, but
// never reported -- an expected unbind is not a finding.
static bool BindCensus_IsDeliberateSkip(const char* name)
{
	return name && strncmp(name, "__skip_", 7) == 0;
}

// "[0007]" -- an array element. Every array names its elements the same way, so
// the name repeats across unrelated arrays in one flat list and says nothing
// about duplicate registration.
static bool BindCensus_IsArrayElementName(const char* name)
{
	if (!name || name[0] != '[')
		return false;
	const char* p = name + 1;
	if (*p < '0' || *p > '9')
		return false;
	while (*p >= '0' && *p <= '9')
		++p;
	return p[0] == ']' && p[1] == '\0';
}

// Count wire slots that carry `name` and whether the first one paired.
static int BindCensus_CountName(uintptr_t sendArr, uintptr_t recvArr, int nWalk,
	const void* dummy, const char* name, int* outFirstIdx, bool* outFirstBound)
{
	int n = 0;
	if (outFirstIdx) *outFirstIdx = -1;
	if (outFirstBound) *outFirstBound = false;

	for (int i = 0; i < nWalk; i++)
	{
		const uintptr_t sp = *reinterpret_cast<uintptr_t*>(sendArr + (uintptr_t)i * 8);
		if (!sp || !S21_IsReadablePtr(reinterpret_cast<const void*>(sp), 0x48))
			continue;
		const char* sn = *reinterpret_cast<const char**>(sp + 0x40);
		if (!sn || !S21_IsReadablePtr(sn, 1) || strcmp(sn, name) != 0)
			continue;

		if (++n == 1)
		{
			const uintptr_t rp = *reinterpret_cast<uintptr_t*>(recvArr + (uintptr_t)i * 8);
			if (outFirstIdx) *outFirstIdx = i;
			if (outFirstBound)
				*outFirstBound = (rp && rp != reinterpret_cast<uintptr_t>(dummy));
		}
	}
	return n;
}

static void BindCensus_Dump(const char* filter)
{
	const void* const dummy = NetBridge_GlobalDummyRecvProp();

	const uintptr_t countAddr = NetObs_ClientClassCountAddr();
	const uintptr_t arrayAddr = NetObs_ClientClassArrayAddr();
	if (!S21_IsReadablePtr(reinterpret_cast<const void*>(countAddr), sizeof(int))
		|| !S21_IsReadablePtr(reinterpret_cast<const void*>(arrayAddr), sizeof(uintptr_t)))
	{
		Warning(eDLL_T::CLIENT, "[BIND-CENSUS] ClientClass registry unreadable\n");
		return;
	}

	const int ccCount = *reinterpret_cast<int*>(countAddr);
	const uintptr_t ccArray = *reinterpret_cast<uintptr_t*>(arrayAddr);
	if (!ccArray || ccCount <= 0 || ccCount > 0x4000)
	{
		Warning(eDLL_T::CLIENT, "[BIND-CENSUS] ClientClass count invalid (%d)\n", ccCount);
		return;
	}
	if (!S21_IsReadablePtr(reinterpret_cast<const void*>(ccArray),
		static_cast<size_t>(ccCount) * 0x20))
	{
		Warning(eDLL_T::CLIENT, "[BIND-CENSUS] ClientClass array unreadable\n");
		return;
	}

	int nClasses = 0;
	int nPropsTotal = 0;
	int nUnmatched = 0;
	int nClassesAffected = 0;
	int nEmitted = 0;
	int nDuplicate = 0;

	for (int idx = 0; idx < ccCount; idx++)
	{
		const uintptr_t slot = ccArray + (uintptr_t)idx * 0x20;
		if (!S21_IsReadablePtr(reinterpret_cast<const void*>(slot), sizeof(uintptr_t)))
			continue;
		const uintptr_t classDesc = *reinterpret_cast<uintptr_t*>(slot);
		if (!classDesc)
			continue;
		if (!S21_IsReadablePtr(reinterpret_cast<const void*>(classDesc), 0x20))
			continue;

		const uintptr_t namePtr   = *reinterpret_cast<uintptr_t*>(classDesc + 0x10);
		const uintptr_t recvTable = *reinterpret_cast<uintptr_t*>(classDesc + 0x18);
		if (!recvTable)
			continue;
		if (!S21_IsReadablePtr(reinterpret_cast<const void*>(recvTable + 0x4C0), sizeof(uintptr_t)))
			continue;

		char cname[128];
		DF_CopyStr(namePtr, cname, (int)sizeof(cname));

		const uintptr_t decoder = *reinterpret_cast<uintptr_t*>(recvTable + 0x4C0);
		if (!decoder)
			continue;
		if (!S21_IsReadablePtr(reinterpret_cast<const void*>(decoder + 0x18), 0x10))
			continue;
		if (!S21_IsReadablePtr(reinterpret_cast<const void*>(decoder + 0x4080), 0x20))
			continue;

		const int nProps = *reinterpret_cast<int*>(decoder + 0x20);
		const uintptr_t sendArr = *reinterpret_cast<uintptr_t*>(decoder + 0x18);
		const uintptr_t recvArr = *reinterpret_cast<uintptr_t*>(decoder + 0x4080);
		const int nRecv = *reinterpret_cast<int*>(decoder + 0x4098);
		if (nProps <= 0 || nProps > 8192 || !sendArr)
			continue;
		if (nRecv < 0 || nRecv > 8192 || !recvArr)
			continue;
		if (!S21_IsReadablePtr(reinterpret_cast<const void*>(sendArr),
			static_cast<size_t>(nProps) * 8))
			continue;
		if (!S21_IsReadablePtr(reinterpret_cast<const void*>(recvArr),
			static_cast<size_t>(nRecv) * 8))
			continue;

		++nClasses;
		const int nWalk = (nProps < nRecv) ? nProps : nRecv;
		int classUnmatched = 0;
		int classSkips = 0;

		// Pass 1 counts so the rollup lands ABOVE the rows and survives the row
		// cap -- a wholesale-unmatched class must never be hidden by truncation.
		for (int i = 0; i < nWalk; i++)
		{
			const uintptr_t sp = *reinterpret_cast<uintptr_t*>(sendArr + (uintptr_t)i * 8);
			const uintptr_t rp = *reinterpret_cast<uintptr_t*>(recvArr + (uintptr_t)i * 8);
			if (!sp)
				continue;
			if (!S21_IsReadablePtr(reinterpret_cast<const void*>(sp), 0x48))
				continue;

			++nPropsTotal;
			if (rp && rp != reinterpret_cast<uintptr_t>(dummy))
				continue;

			char pn[96];
			DF_CopyStr(*reinterpret_cast<uintptr_t*>(sp + 0x40), pn, (int)sizeof(pn));
			if (BindCensus_IsDeliberateSkip(pn))
			{
				++classSkips;
				continue;
			}
			++classUnmatched;
		}

		nUnmatched += classUnmatched;
		if (classUnmatched > 0)
			++nClassesAffected;

		if (classUnmatched > 0 || classSkips > 0)
		{
			Warning(eDLL_T::CLIENT,
				"[BIND-CENSUS] %s: %d/%d unmatched (send=%d recv=%d skips=%d)\n",
				cname, classUnmatched, nWalk, nProps, nRecv, classSkips);
		}

		for (int i = 0; i < nWalk && classUnmatched > 0; i++)
		{
			const uintptr_t sp = *reinterpret_cast<uintptr_t*>(sendArr + (uintptr_t)i * 8);
			const uintptr_t rp = *reinterpret_cast<uintptr_t*>(recvArr + (uintptr_t)i * 8);
			if (!sp)
				continue;
			if (!S21_IsReadablePtr(reinterpret_cast<const void*>(sp), 0x48))
				continue;
			if (rp && rp != reinterpret_cast<uintptr_t>(dummy))
				continue;

			const int spType = *reinterpret_cast<int*>(sp + 0x00);
			const uintptr_t spNamePtr = *reinterpret_cast<uintptr_t*>(sp + 0x40);
			char pn[96];
			DF_CopyStr(spNamePtr, pn, (int)sizeof(pn));

			if (BindCensus_IsDeliberateSkip(pn))
				continue;
			if (filter && *filter && !strstr(pn, filter))
				continue;

			int firstIdx = -1;
			bool firstBound = false;
			const int nSame = BindCensus_IsArrayElementName(pn)
				? 1
				: BindCensus_CountName(sendArr, recvArr, nWalk, dummy,
					pn, &firstIdx, &firstBound);
			if (nSame > 1)
				++nDuplicate;

			// First 512 lines, then every 64th -- a pathological unmatched set
			// must not flood the log. The rollup above stays exact either way.
			const bool emit = (nEmitted < 512)
				|| ((nEmitted - 512) % 64 == 0);
			if (emit)
			{
				if (nSame > 1)
				{
					Warning(eDLL_T::CLIENT,
						"[BIND-CENSUS] UNMATCHED %s.%s ty=%d idx=%d DUPLICATE x%d "
						"(first idx=%d %s)\n",
						cname, pn, spType, i, nSame, firstIdx,
						firstBound ? "BOUND" : "unmatched");
				}
				else
				{
					Warning(eDLL_T::CLIENT,
						"[BIND-CENSUS] UNMATCHED %s.%s ty=%d idx=%d\n",
						cname, pn, spType, i);
				}
			}
			++nEmitted;
		}
	}

	Warning(eDLL_T::CLIENT,
		"[BIND-CENSUS] %d classes, %d wire props, %d UNMATCHED across %d classes, "
		"%d DUPLICATE%s%s\n",
		nClasses, nPropsTotal, nUnmatched, nClassesAffected, nDuplicate,
		(filter && *filter) ? " filter=" : "", (filter && *filter) ? filter : "");
}

static void CC_BridgeBindCensus_f(const CCommand& args)
{
	BindCensus_Dump(args.ArgC() > 1 ? args.Arg(1) : nullptr);
}
static ConCommand bridge_bind_census("bridge_bind_census", CC_BridgeBindCensus_f,
	"[BIND-CENSUS] Dump wire-pairing: every SendProp vs its RecvProp slot (dummy/null = unmatched). Usage: bridge_bind_census [name filter]",
	FCVAR_DEVELOPMENTONLY);

static __int64 __fastcall Hook_CreateDecoders(__int64 a1, __int64 a2, __int64 a3, __int64 a4)
{

	__int64 cdRet = s_origCreateDecoders(a1, a2, a3, a4);

	// One-shot auto dump when sdk_bind_census is on; re-arm when the class
	// count changes so a changelevel produces a fresh census.
	if (sdk_bind_census.GetBool())
	{
		static int s_censusLatchedCount = -1;
		const uintptr_t countAddr = NetObs_ClientClassCountAddr();
		int ccCount = 0;
		if (S21_IsReadablePtr(reinterpret_cast<const void*>(countAddr), sizeof(int)))
			ccCount = *reinterpret_cast<int*>(countAddr);
		if (ccCount > 0 && ccCount != s_censusLatchedCount)
		{
			s_censusLatchedCount = ccCount;
			BindCensus_Dump(nullptr);
		}
	}
	return cdRet;
}

// -- builds the prop-to-offset mapping table.
// For decoder-less classes (no matching SendTable from bridge dedi), the function
typedef void (*PFN_BuildPropOffsetTable)(void);

static PFN_BuildPropOffsetTable s_origBuildPropOffsetTable = nullptr;

// Sized to cover the worst-case flattened class. The engine's flat decoder
// reads offset entries from this mapping; a class with N flattened props
static uint8_t s_emptyPropMapping[256 * 1024] = {};

static void __fastcall Hook_BuildPropOffsetTable()
{
	s_origBuildPropOffsetTable();

	uintptr_t base = NetObs_GetExeModuleBase();
	if (!base) return;

	const int classCount = *reinterpret_cast<int*>(NetObs_ClientClassCountAddr());
	uint64_t* table = reinterpret_cast<uint64_t*>(NetObs_PropMappingTableAddr());
	int swept = 0;

	for (int i = 0; i < classCount && i < 8192; i++)
	{
		if (!table[i])
		{
			table[i] = reinterpret_cast<uint64_t>(s_emptyPropMapping);
			++swept;
		}
	}

	if (swept > 0)
		SDK_Log("[NET-OBS] BuildPropOffsetTable: filled %d NULL mapping entries with empty mapping\n", swept);
}

// SEH filter: capture the inner exception RIP + fault address so crash lines
// name the exact faulting instruction (-rebased).
static thread_local void* s_decCrashRip = nullptr;
static thread_local unsigned long s_decCrashCode = 0;
static thread_local uintptr_t s_decCrashFault = 0;
static int Bridge_DecodeCrashFilter(EXCEPTION_POINTERS* ep)
{
	s_decCrashRip = nullptr; s_decCrashCode = 0; s_decCrashFault = 0;
	if (ep && ep->ExceptionRecord)
	{
		s_decCrashRip  = ep->ExceptionRecord->ExceptionAddress;
		s_decCrashCode = ep->ExceptionRecord->ExceptionCode;
		if (ep->ExceptionRecord->NumberParameters >= 2)
			s_decCrashFault = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
	}
	return EXCEPTION_EXECUTE_HANDLER;
}

// The two bf_reads RecvTable_MergeDeltas walks: a2 = the class instance
// baseline (private per class, from the instancebaseline string table), a3 =
static thread_local __int64 s_mergeFromBuf = 0;
static thread_local __int64 s_mergeToBuf   = 0;

// FNV-1a 64 over "name\\0" for every flat prop -- matches dedi Flatn_NameFnv64.
static uint64_t Bridge_FlatNameFnv64(uint8_t** spA, int nPre)
{
	uint64_t h = 14695981039346656037ull;
	for (int i = 0; i < nPre; ++i)
	{
		const char* pn = nullptr;
		__try
		{
			uint8_t* sp = spA ? spA[i] : nullptr;
			if (sp)
				pn = *reinterpret_cast<const char**>(sp + 0x40);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			pn = nullptr;
		}
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

static int Bridge_FindRecvPropIdx(uint8_t** spA, int nPre, const char* want)
{
	if (!want || !spA || nPre <= 0)
		return -1;
	for (int i = 0; i < nPre; ++i)
	{
		const char* pn = nullptr;
		__try
		{
			uint8_t* sp = spA[i];
			if (sp)
				pn = *reinterpret_cast<const char**>(sp + 0x40);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			pn = nullptr;
		}
		if (pn && strcmp(pn, want) == 0)
			return i;
	}
	return -1;
}

// Always-on one-line LIVE client count for DT_Player (and full dump for listed tables).
static void Bridge_MaybeDumpRecvFlat(const char* tableName, __int64 decoder)
{
	if (!tableName || !tableName[0] || !decoder)
		return;

	int nPre = 0;
	uint8_t** spA = nullptr;
	__try
	{
		nPre = *reinterpret_cast<int*>(decoder + 0x20);
		spA = *reinterpret_cast<uint8_t***>(decoder + 0x18);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Warning(eDLL_T::ENGINE, "[RECV-FLAT] %s: fault reading decoder precalc\n", tableName);
		return;
	}

	// [FLATN-CL-ALL] one-line census for every table the client decodes.
	if (bridge_flatn_cl_all.GetBool() && spA && nPre > 0 && nPre <= 4096)
	{
		static const char* s_allSeen[512] = {};
		static int s_allSeenN = 0;
		bool seen = false;
		for (int i = 0; i < s_allSeenN; ++i)
		{
			if (s_allSeen[i] == tableName)
			{
				seen = true;
				break;
			}
		}
		if (!seen && s_allSeenN < 512)
		{
			s_allSeen[s_allSeenN++] = tableName;
			const uint64_t nameFnv = Bridge_FlatNameFnv64(spA, nPre);
			Warning(eDLL_T::ENGINE,
				"[FLATN-CL-ALL] table='%s' flatN=%d nameFnv=0x%016llX\n",
				tableName, nPre, static_cast<unsigned long long>(nameFnv));
		}
	}

	// [WIRE-PAIR] Opt-in one-shot census. Index-parallel pairing of
	// decoder+0x18 vs +0x4080 is UNVERIFIED. Offsets from the [OOB-SHAPE] block.
	if (sdk_wire_pair_rows.GetBool())
	{
		static const char* s_pairSeen[512] = {};
		static int s_pairSeenN = 0;
		bool pairSeen = false;
		for (int i = 0; i < s_pairSeenN; ++i)
		{
			if (s_pairSeen[i] == tableName)
			{
				pairSeen = true;
				break;
			}
		}
		if (!pairSeen && s_pairSeenN < 512 && spA && nPre > 0 && nPre <= 4096)
		{
			s_pairSeen[s_pairSeenN++] = tableName;

			int nRcv = 0;
			uint8_t** rpA = nullptr;
			__try
			{
				nRcv = *reinterpret_cast<int*>(decoder + 0x4098);
				rpA = *reinterpret_cast<uint8_t***>(decoder + 0x4080);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				nRcv = 0;
				rpA = nullptr;
			}

			if (!rpA || nRcv <= 0)
			{
				Warning(eDLL_T::ENGINE,
					"[WIRE-PAIR] %s: decoder RecvProp array unreadable "
					"(rpA=%p nRcv=%d) -- census skipped\n",
					tableName, reinterpret_cast<void*>(rpA), nRcv);
			}
			else
			{
				if (sdk_wire_pair_rows.GetBool())
				{
					const int nCalib = (nPre < 8) ? nPre : 8;
					for (int i = 0; i < nCalib; ++i)
					{
						const char* sn = nullptr;
						const char* rn = nullptr;
						__try
						{
							uint8_t* sp = (S21_IsReadablePtr(spA + i, sizeof(void*)))
								? spA[i] : nullptr;
							// SendProp name is +0x40 (not +0x28).
							if (sp && S21_IsReadablePtr(sp, 0x48))
							{
								sn = *reinterpret_cast<const char**>(sp + 0x40);
								if (sn && !S21_IsReadablePtr(sn, 1)) sn = nullptr;
							}
							uint8_t* rp = (i < nRcv &&
								S21_IsReadablePtr(rpA + i, sizeof(void*)))
								? rpA[i] : nullptr;
							// RecvProp name is +0x28.
							if (rp && S21_IsReadablePtr(rp, 0x30))
							{
								rn = *reinterpret_cast<const char**>(rp + 0x28);
								if (rn && !S21_IsReadablePtr(rn, 1)) rn = nullptr;
							}
						}
						__except (EXCEPTION_EXECUTE_HANDLER)
						{
							sn = nullptr;
							rn = nullptr;
						}
						Warning(eDLL_T::ENGINE,
							"[WIRE-PAIR] %s [%4d] s='%s' r='%s'\n",
							tableName, i,
							sn ? sn : "<null>",
							rn ? rn : "<null>");
					}
				}

				int nMismatch = 0;
				int nListed = 0;
				const int nWalk = (nPre < nRcv) ? nPre : nRcv;
				for (int i = 0; i < nWalk; ++i)
				{
					const char* sn = nullptr;
					const char* rn = nullptr;
					__try
					{
						uint8_t* sp = (S21_IsReadablePtr(spA + i, sizeof(void*)))
							? spA[i] : nullptr;
						if (sp && S21_IsReadablePtr(sp, 0x48))
						{
							sn = *reinterpret_cast<const char**>(sp + 0x40);
							if (sn && !S21_IsReadablePtr(sn, 1)) sn = nullptr;
						}
						uint8_t* rp = (S21_IsReadablePtr(rpA + i, sizeof(void*)))
							? rpA[i] : nullptr;
						if (rp && S21_IsReadablePtr(rp, 0x30))
						{
							rn = *reinterpret_cast<const char**>(rp + 0x28);
							if (rn && !S21_IsReadablePtr(rn, 1)) rn = nullptr;
						}
					}
					__except (EXCEPTION_EXECUTE_HANDLER)
					{
						sn = nullptr;
						rn = nullptr;
					}
					if (!sn || !rn || strcmp(sn, rn) == 0)
						continue;
					++nMismatch;
					const bool dumpRows = sdk_wire_pair_rows.GetBool() ||
						(tableName && strcmp(tableName, "DT_ScriptMover") == 0);
					if (dumpRows && nListed < 64)
					{
						++nListed;
						Warning(eDLL_T::ENGINE,
							"[WIRE-PAIR] %s [%4d] s='%s' r='%s'\n",
							tableName, i, sn, rn);
					}
				}
				if (sdk_wire_pair_rows.GetBool() && nMismatch > 64)
				{
					Warning(eDLL_T::ENGINE,
						"[WIRE-PAIR] %s: ... and %d more\n",
						tableName, nMismatch - 64);
				}
				if (nMismatch > 0)
				{
					Warning(eDLL_T::ENGINE,
						"[WIRE-PAIR] %s: %d of %d indices have mismatched send/recv names "
						"(nRcv=%d)%s\n",
						tableName, nMismatch, nPre, nRcv,
						(nPre != nRcv) ? " -- nPre!=nRcv, arrays are not parallel; census void" : "");
				}
			}
		}
	}

	// [FLATN-CL] detailed LIVE summary for DT_Player -- independent of full dump.
	if (bridge_flatn_cl.GetBool() && strcmp(tableName, "DT_Player") == 0)
	{
		static volatile LONG s_flatnClLogged = 0;
		if (InterlockedCompareExchange(&s_flatnClLogged, 1, 0) == 0)
		{
			if (!spA || nPre <= 0 || nPre > 4096)
			{
				Warning(eDLL_T::ENGINE,
					"[FLATN-CL] LIVE DT_Player decoder bad flatN=%d arr=%p\n",
					nPre, reinterpret_cast<void*>(spA));
			}
			else
			{
				const uint64_t nameFnv = Bridge_FlatNameFnv64(spA, nPre);
				const int idxHealth = Bridge_FindRecvPropIdx(spA, nPre, "m_iHealth");
				const int idxOrigin = Bridge_FindRecvPropIdx(spA, nPre, "m_vecAbsOrigin");
				const int idxLife   = Bridge_FindRecvPropIdx(spA, nPre, "m_lifeState");
				const int idxTeam   = Bridge_FindRecvPropIdx(spA, nPre, "m_iTeamNum");
				Warning(eDLL_T::ENGINE,
					"[FLATN-CL] LIVE DT_Player decoder flatN=%d nameFnv=0x%016llX "
					"dec=0x%p arr=0x%p "
					"idx m_iHealth=%d m_vecAbsOrigin=%d m_lifeState=%d m_iTeamNum=%d "
					"-- pair with dedi [FLATN-CMP]; MATCH if flatN and nameFnv agree\n",
					nPre, static_cast<unsigned long long>(nameFnv),
					reinterpret_cast<void*>(decoder), reinterpret_cast<void*>(spA),
					idxHealth, idxOrigin, idxLife, idxTeam);

				char line[1024];
				int used = snprintf(line, sizeof(line), "[FLATN-CL] head:");
				const int headN = (nPre < 12) ? nPre : 12;
				for (int i = 0; i < headN && used > 0 && used < (int)sizeof(line) - 48; ++i)
				{
					const char* pn = nullptr;
					__try
					{
						uint8_t* sp = spA[i];
						if (sp)
							pn = *reinterpret_cast<const char**>(sp + 0x40);
					}
					__except (EXCEPTION_EXECUTE_HANDLER) { pn = nullptr; }
					used += snprintf(line + used, sizeof(line) - static_cast<size_t>(used),
						" %d='%s'", i, pn ? pn : "?");
				}
				Warning(eDLL_T::ENGINE, "%s\n", line);
				if (nPre > 12)
				{
					used = snprintf(line, sizeof(line), "[FLATN-CL] tail:");
					for (int i = nPre - 8; i < nPre && used > 0 && used < (int)sizeof(line) - 48; ++i)
					{
						const char* pn = nullptr;
						__try
						{
							uint8_t* sp = spA[i];
							if (sp)
								pn = *reinterpret_cast<const char**>(sp + 0x40);
						}
						__except (EXCEPTION_EXECUTE_HANDLER) { pn = nullptr; }
						used += snprintf(line + used, sizeof(line) - static_cast<size_t>(used),
							" %d='%s'", i, pn ? pn : "?");
					}
					Warning(eDLL_T::ENGINE, "%s\n", line);
				}
				if (nPre == 1167)
				{
					Warning(eDLL_T::ENGINE,
						"[FLATN-CL] NOTE: client flatN=1167 (same as historical dedi count) -- "
						"gap closed if dedi [FLATN-CMP] also 1167 and nameFnv matches\n");
				}
				else if (nPre == 1293)
				{
					Warning(eDLL_T::ENGINE,
						"[FLATN-CL] NOTE: client flatN=1293 (historical S21 decoder count) -- "
						"if dedi flatN!=1293 the CPlayer wire map is divergent\n");
				}
			}
		}
	}

	const char* cfg = nullptr;
	if (CommandLine()->CheckParm("+bridge_dump_recvflat", &cfg) && cfg && cfg[0])
		/* launch-arg wins */;
	else
		cfg = bridge_dump_recvflat.GetString();
	if (!cfg || !cfg[0] || strcmp(cfg, "0") == 0)
		return;

	// cfg is comma-separated names, "1" = DT_Player only, or "*" = every
	// table that decodes -- the whole-wire audit, diffed against the dedi's
	// [FLATN-DUMP] by tools/wire_flat_diff.py.
	bool want = false;
	if (strcmp(cfg, "*") == 0)
		want = true;
	else if (strcmp(cfg, "1") == 0)
		want = (strcmp(tableName, "DT_Player") == 0);
	else
	{
		const char* p = cfg;
		while (*p)
		{
			while (*p == ' ' || *p == ',')
				++p;
			if (!*p)
				break;
			const char* start = p;
			while (*p && *p != ',' && *p != ' ')
				++p;
			const size_t n = static_cast<size_t>(p - start);
			if (n > 0 && n == strlen(tableName) &&
				_strnicmp(start, tableName, n) == 0)
			{
				want = true;
				break;
			}
		}
	}
	if (!want)
		return;

	// One dump per table name for the process lifetime.
	static const char* s_dumped[256] = {};
	static int s_dumpedN = 0;
	for (int i = 0; i < s_dumpedN; ++i)
		if (s_dumped[i] == tableName)
			return;
	if (s_dumpedN >= 256)
		return;
	s_dumped[s_dumpedN++] = tableName;

	if (!spA || nPre <= 0 || nPre > 4096)
	{
		Warning(eDLL_T::ENGINE,
			"[RECV-FLAT] %s: bad precalc flatN=%d arr=%p\n",
			tableName, nPre, reinterpret_cast<void*>(spA));
		return;
	}

	Warning(eDLL_T::ENGINE,
		"[RECV-FLAT] %s decoder precalc flatN=%d -- listing flat props:\n",
		tableName, nPre);
	for (int i = 0; i < nPre; ++i)
	{
		int ty = -1, nb = -1, fl = 0, off = -1;
		const char* pn = nullptr;
		__try
		{
			uint8_t* sp = spA[i];
			if (sp)
			{
				ty = *reinterpret_cast<int*>(sp + 0x00);
				nb = *reinterpret_cast<int*>(sp + 0x04);
				fl = *reinterpret_cast<int*>(sp + 0x58);
				off = *reinterpret_cast<int*>(sp + 0x78);
				pn = *reinterpret_cast<const char**>(sp + 0x40);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			ty = -1; nb = -1; fl = 0; off = -1; pn = nullptr;
		}
		Warning(eDLL_T::ENGINE,
			"[RECV-FLAT]   [%4d] ty=%d nBits=%d fl=0x%X off=0x%X s='%s'\n",
			i, ty, nb, fl, off, pn ? pn : "<null>");
	}
	Warning(eDLL_T::ENGINE, "[RECV-FLAT] %s end (flatN=%d)\n", tableName, nPre);
}

static __int64 __fastcall Hook_RecvTableDecode(__int64 recvTable, __int64 a2, __int64 a3, __int64 a4)
{
	SNAPB_T0(_sbT);
	// Install apply-pipe hooks on the first decode call (Hook_RecvTableDecode is every DT decode).
	static volatile LONG s_proxyInstallTried = 0;
	if (InterlockedCompareExchange(&s_proxyInstallTried, 1, 0) == 0)
	{
		// Fail loud: empty SEH would swallow a broken proxy attach.
		Bridge_InstallApplyPipeHooks();
	}

	const char* tableName = nullptr;
	if (recvTable)
		tableName = *reinterpret_cast<const char**>(recvTable + 0x4C8);


	__int64 decoder = *(__int64*)(recvTable + 0x4C0);
	if (!decoder)
	{
		static int s_nullDecoderCount = 0;
		const int nNull = ++s_nullDecoderCount;
		if (nNull <= 50)
			Warning(eDLL_T::ENGINE,
				"[NET-OBS] NULL decoder for '%s' -- overflow wire (#%d)\n",
				tableName ? tableName : "?", nNull);
		Bridge_PropStreamIntegrityFail(a3, "NULL_DECODER",
			tableName, -1, nNull);
		SNAPB_ADD_TOTAL(SNAPB_RTDECODE, _sbT);
		return 0;
	}

	// Resolve the appended mantle-boost prop entry for this decoder (see the
	// Bridge_LatchWirePeekProps banner). The name compare only runs while the
	// decoder is one we have not scanned yet.
	if (tableName && static_cast<uintptr_t>(decoder) != s_mbDecoder &&
		strcmp(tableName, "DT_Player") == 0)
		Bridge_LatchWirePeekProps(decoder);

	s_currentDecodeRecvTable = recvTable;
	// Despite the name this is RecvTable_MergeDeltas(table, fromBuf, toBuf, out):
	// a2 is the class instance baseline, a3 the snapshot wire buffer.
	s_mergeFromBuf = a2;
	s_mergeToBuf   = a3;

	// Optional [RECV-FLAT] one-shot (bridge_dump_recvflat / +bridge_dump_recvflat).
	// DEC-MATCH bound-count + VirtualQuery walk retired -- log noise only.
	{
		static const char* s_flatSeen[160] = {};
		static int s_flatSeenN = 0;
		bool seen = false;
		for (int i = 0; i < s_flatSeenN; ++i)
			if (s_flatSeen[i] == tableName) { seen = true; break; }
		if (!seen && s_flatSeenN < 160 && tableName)
		{
			s_flatSeen[s_flatSeenN++] = tableName;
			Bridge_MaybeDumpRecvFlat(tableName, decoder);
		}
	}

	__int64 result = 0;
	{
		SNAPB_T0(_sbO);
		if (bridge_decode_seh.GetBool())
		{
			__try { result = s_origRecvTableDecode(recvTable, a2, a3, a4); }
			__except(Bridge_DecodeCrashFilter(GetExceptionInformation()))
			{
				Bridge_PropStreamIntegrityFail(a3, "DECODE_CRASH",
					tableName, -1, 0);
				result = 0;
			}
		}
		else
		{
			result = s_origRecvTableDecode(recvTable, a2, a3, a4);
		}
		SNAPB_ADD_ORIG(SNAPB_RTDECODE, _sbO);
	}

	// OOB / no-consume paths force bitbuf overflow and set s_decodeOobProp
	// without SEH; still fail the merge rather than return success.
	if (s_decodeOobProp)
		result = 0;

	s_currentDecodeRecvTable = 0;
	SNAPB_ADD_TOTAL(SNAPB_RTDECODE, _sbT);
	return result;
}

// DPT_String wire decoder: 9-bit length, copy payload to engine staging buffer, return ptr.
// Fires when string props (e.g. m_iSignifierName) are on the wire; silence means no string bits or bit misalignment.
bool IsBadRecvPropPtr(uintptr_t p)
{
	return p < 0x10000ULL ||
	       p < 0x0000010000000000ULL ||
	       (p & 0xFFFF800000000000ULL) != 0 ||
	       (p & 7ULL) != 0;
}

// -- RecvTable_Decode (the main entity decode function).
// Called from CL_CopyNewEntity after (DecodeZeros).
typedef int (__fastcall *PFN_RecvTableDecodeMain)(
	__int64, int, int, __int64*, __int64, unsigned __int8, int*, int,
	_QWORD*, _BYTE*, char*);

static PFN_RecvTableDecodeMain s_origRecvTableDecodeMain = nullptr;

// Per-call decode trace counters (see [RTD] log below).
static volatile long s_rtdTraceCtr = 0;


// OOB-prop snapshot-drop counter. File-scope so the 1Hz [SNAP-LEDGER] can read
// it (was a function-local static inside Hook_PropDecodeDispatch).
static volatile long s_snapOobDrops = 0;
static volatile long s_oobDisconnectFired = 0;

// Mark bf_read overflow so the merge loop cannot interpret desynced bits as
// further prop indices. Cursor layout: +0x08 = overflow flag (see DT_PeekFixedBits).
static void Bridge_BitbufForceOverflow(__int64 bitbuf)
{
	if (bitbuf < 0x10000)
		return;
	__try
	{
		*reinterpret_cast<unsigned char*>(bitbuf + 0x08) = 1;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
}

// Fail closed on prop-stream integrity loss: overflow + optional Host_Error.
// eventN is the lifetime total (log-only); disconnect uses the sliding window.
static void Bridge_PropStreamIntegrityFail(__int64 bitbuf, const char* reason,
	const char* tableName, int propIdx, long eventN)
{
	Bridge_BitbufForceOverflow(bitbuf);
	s_decodeOobProp = 1;
	s_copyNewOob = 1;

	int limit = bridge_prop_oob_disconnect_limit.GetInt();
	const char* limArg = nullptr;
	if (CommandLine()->CheckParm("+bridge_prop_oob_disconnect_limit", &limArg) && limArg)
		limit = atoi(limArg);

	const float flWindowSec = bridge_prop_oob_window_sec.GetFloat();
	static double s_oobWindowStart = 0.0;
	static long s_oobWindowCount = 0;

	const double flNow = Plat_FloatTime();
	if (flWindowSec > 0.0f
		&& (s_oobWindowStart <= 0.0 || (flNow - s_oobWindowStart) >= (double)flWindowSec))
	{
		s_oobWindowStart = flNow;
		s_oobWindowCount = 0;
	}
	++s_oobWindowCount;

	if (limit > 0 && s_oobWindowCount >= limit
		&& InterlockedCompareExchange(&s_oobDisconnectFired, 1, 0) == 0)
	{
		Warning(eDLL_T::ENGINE,
			"[NET-OBS] prop stream integrity fail (%s) table='%s' idx=%d "
			"(lifetime=#%ld window=#%ld / %g s) -- disconnecting (limit=%d)\n",
			reason ? reason : "?", tableName ? tableName : "?", propIdx,
			eventN, s_oobWindowCount, (double)flWindowSec, limit);
		Host_Error("[NET-OBS] prop stream integrity fail (%s) table='%s' idx=%d "
			"(lifetime=#%ld window=#%ld)",
			reason ? reason : "?", tableName ? tableName : "?", propIdx,
			eventN, s_oobWindowCount);
		return;
	}

	// Rate-limited shear notice when under the disconnect threshold.
	static long s_oobWarnN = 0;
	const long nWarn = ++s_oobWarnN;
	if (nWarn <= 8 || (nWarn % 64) == 0)
	{
		Warning(eDLL_T::ENGINE,
			"[NET-OBS] prop stream shear (%s) table='%s' idx=%d "
			"(lifetime=#%ld window=#%ld / %g s, limit=%d)\n",
			reason ? reason : "?", tableName ? tableName : "?", propIdx,
			eventN, s_oobWindowCount, (double)flWindowSec, limit);
	}
}

// --- entity crumb ring (cross-entity desync locator) ------------
// The per-prop crumb ring resets per entity, so it can only show a desync that
struct DecodeEntCrumb
{
	int       snap;
	int       entIdx;
	char      ty;
	int       cid;
	long long startPos;
	long long endPos;
	int       ret;
};
static DecodeEntCrumb s_entCrumbs[128];
static int            s_entCrumbHead  = 0;
static int            s_entCrumbCount = 0;

// Current-entity context for prop dispatch (which has no entity index of its own).
static thread_local bool s_dtTraceOn   = false;
static thread_local int  s_curDecEntIdx = -1;
static thread_local char s_curDecTy     = '?';
static thread_local int  s_curDecCid    = -1;

// CL_CopyNewEntity: MergeDeltas first, Decode second; only Decode publishes s_curDec*.
// Merge-loop reports lag one entity; s_curNewEnt* is set before the native call.
static thread_local int s_curNewEntSlot = -1;
static thread_local int s_curNewEntCid  = -1;


// Crash-time crumb dump. The enter-PVS SPAWN can heap-corrupt the engine
// allocator WITHOUT tripping the prop-index OOB guard. When the C2S SignonState
void Bridge_DumpDecodeCrumbsOnCrash(const char* tag)
{
	const char* t = tag ? tag : "?";
	Warning(eDLL_T::ENGINE,
		"[NET-OBS] decode-crumb dump on crash [%s]: ents=%d trace=%d -> bridge_trace.log\n",
		t, s_entCrumbCount, s_dtTraceOn ? 1 : 0);
	BridgeTrace_Log("[NET-OBS] ===== decode-crumb dump on crash [%s] =====\n", t);

	if (s_entCrumbCount > 0)
	{
		BridgeTrace_Log(
			"[NET-OBS] last %d entities decoded before crash (anomalous wireBits = desync origin):\n",
			s_entCrumbCount);
		const int es = (s_entCrumbHead - s_entCrumbCount + 128) & 127;
		for (int i = 0; i < s_entCrumbCount; ++i)
		{
			const DecodeEntCrumb& e = s_entCrumbs[(es + i) & 127];
			const long long wb =
				(e.startPos >= 0 && e.endPos >= 0) ? (e.endPos - e.startPos) : -1;
			BridgeTrace_Log(
				"[NET-OBS]   ent snap#%d idx=%d ty=%c class=%d(%s) startBit=%lld endBit=%lld wireBits=%lld ret=%d\n",
				e.snap, e.entIdx, e.ty, e.cid, DT_ClassName(e.cid),
				e.startPos, e.endPos, wb, e.ret);
		}
	}
	BridgeTrace_Log("[NET-OBS] ===== end decode-crumb dump [%s] =====\n", t);
}

static int __fastcall Hook_RecvTableDecodeMain(
	__int64 a1, int a2, int a3, __int64* a4,
	__int64 a5, unsigned __int8 a6, int* a7, int a8,
	_QWORD* a9, _BYTE* a10, char* a11)
{
	SNAPB_T0(_sbT);

	const char rtdTy = a4 ? (a5 ? 'D' : 'P') : (a5 ? 'E' : '?');

	// DT decode trace: sample the wire bitbuf cursor before the entity decode.
	// Level 1 (default): arm the in-memory crumb ring + the [NET-OBS] OOB
	const int  dtTraceLvl = 0;
	const bool dtTrace    = dtTraceLvl > 0;
	const bool dtVerbose  = dtTraceLvl >= 2;

	// Publish this entity's context for the prop dispatch hook's crumb ring.
	s_dtTraceOn    = dtTrace;
	s_curDecEntIdx = a3;                 // a3 here is the ENTITY index
	s_curDecTy     = rtdTy;
	s_curDecCid    = a7 ? *a7 : -1;

	uintptr_t dtCurPre = 0; int dtAvailPre = 0; bool dtHaveCur = false;
	if (dtTrace && a5)  // pre-cursor feeds the always-on entity crumb ring + verbose [DT-TRACE]
		dtHaveCur = DT_ReadCursor((uintptr_t)a5, dtCurPre, dtAvailPre);

	// Per-prop trace: arm for delta-to-existing (ty=D). Hook_PropDecodeDispatch logs while s_ptActive.
	const bool ptArm = dtTrace && (rtdTy == 'D' || rtdTy == 'E');
	if (ptArm)
	{
		s_ptActive  = true;
		s_ptEntIdx  = a3;
		s_ptPropN   = 0;
		s_ptLastIdx = -1;
		s_ptPrevPos = -1;
		s_ptEmitLines = dtVerbose && (s_ptTotalLines < 200000);
		s_ptCrumbHead = 0;
		s_ptCrumbCount = 0;
	}
	// Cleared per decode, and deliberately so: on the enter-PVS path
	// CL_CopyNewEntity runs RecvTable_MergeDeltas FIRST, so an OOB raised there
	s_decodeOobProp = 0;

	// On both delta paths (ty=D CopyExisting, ty=P Preserve) the caller hands in
	// iClass = -1 and the class is recovered from the PREVIOUS snapshot's packed
	if (bridge_delta_class_guard.GetBool() && a7 && *a7 < 0 && a4 && a1
		&& static_cast<unsigned>(a3) < 0x4000u)
	{
		int       recovered   = -1;
		int       nClasses    = 0;
		uintptr_t classArr    = 0;
		uintptr_t clientClass = 0;
		bool      probed      = false;
		const uintptr_t fromBuf = static_cast<uintptr_t>(a4[0]);
		if (fromBuf)
		{
			const uint32_t off = *reinterpret_cast<const uint32_t*>(
				a1 + 88 + 8ull * static_cast<unsigned>(a3)) & 0x7FFFFF;
			// a4 is ByteBuf: buf@0, bufLength@8, readPos@0xC. Callers stamp
			// length with the packed-field arena (8 MiB, same as the 23-bit mask).
			const uint32_t packedLen = *reinterpret_cast<const uint32_t*>(
				reinterpret_cast<const char*>(a4) + 8);
			if (packedLen >= sizeof(uint16_t) && packedLen <= 0x800000u
				&& off <= packedLen - sizeof(uint16_t))
			{
				recovered = *reinterpret_cast<const uint16_t*>(fromBuf + off);
				probed = true;
			}
		}
		if (probed)
		{
			nClasses = *reinterpret_cast<const int*>(NetObs_ClientClassCountAddr());
			classArr = *reinterpret_cast<const uintptr_t*>(NetObs_ClientClassArrayAddr());
			if (classArr && nClasses > 0 && nClasses <= 4096
				&& recovered >= 0 && recovered < nClasses)
			{
				clientClass = *reinterpret_cast<const uintptr_t*>(
					classArr + 32ull * static_cast<unsigned>(recovered));
			}

			if (!clientClass)
			{
				static volatile long s_deltaClassBad = 0;
				const long n = InterlockedIncrement(&s_deltaClassBad);
				if (n <= 20 || (n % 256) == 0)
				{
					Warning(eDLL_T::ENGINE,
						"[DELTA-CLASS] ty=%c ent=%d recovered cid=%d (nClasses=%d arr=%p cc=%p) "
						"-- entity dropped, snapshot resyncs (#%ld)\n",
						rtdTy, a3, recovered, nClasses, (void*)classArr,
						(void*)clientClass, n);
				}
				if (a10) *a10 = 0;
				if (a11) *a11 = 0;
				SNAPB_ADD_TOTAL(SNAPB_RTDECODEMAIN, _sbT);
				return -1;
			}
		}
	}

	int result = 0;
	{
		SNAPB_T0(_sbO);
		if (bridge_decode_seh.GetBool())
		{
			__try { result = s_origRecvTableDecodeMain(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11); }
			__except(Bridge_DecodeCrashFilter(GetExceptionInformation()))
			{
				if (a11) *a11 = 1;
				result = -1;
				static long s_decCrashN = 0;
				const long n = InterlockedIncrement(&s_decCrashN);
				if (n <= 20 || (n % 200) == 0)
				{
					const int cid = a7 ? *a7 : -1;
					Warning(eDLL_T::ENGINE,
						"[NET-OBS] RecvTable_Decode crashed ent=%d cid=%d(%s) ty=%c code=0x%08lX rip=%p fault=%p -- snapshot dropped (#%ld)\n",
						a3, cid, DT_ClassName(cid), rtdTy, (unsigned long)s_decCrashCode,
						s_decCrashRip, (void*)s_decCrashFault, n);
				}
			}
		}
		else
		{
			result = s_origRecvTableDecodeMain(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
		}
		SNAPB_ADD_ORIG(SNAPB_RTDECODEMAIN, _sbO);
	}
	if (ptArm) s_ptActive = false;


	if (s_decodeOobProp)
	{
		if (a11) *a11 = 1;
		result = -1;
	}

	// Per-entity class + wire bits consumed. ty=D and ty=E: a5 is the snapshot wire bitbuf.
	if (dtTrace)
	{
		const int cid = a7 ? *a7 : -1;

		const long long startPos = dtHaveCur ? ((long long)dtCurPre * 8 - dtAvailPre) : -1;
		long long endPos = -1;
		if (dtHaveCur) {
			uintptr_t c1 = 0; int b1 = 0;
			if (DT_ReadCursor((uintptr_t)a5, c1, b1))
				endPos = (long long)c1 * 8 - b1;
		}
		const long long wireBits = (startPos >= 0 && endPos >= 0) ? (endPos - startPos) : -1;


		// Record into the cross-entity crumb ring (always-on, in-memory).
		s_entCrumbs[s_entCrumbHead] = { s_dtSnap, a3, rtdTy, cid, startPos, endPos, result };
		s_entCrumbHead = (s_entCrumbHead + 1) & 127;
		if (s_entCrumbCount < 128) ++s_entCrumbCount;

		// [DT-TRACE] per-entity bit accounting -- verbose only (one line per
		// decoded entity is a firehose). File-only via BridgeTrace_Log.
		if (dtVerbose)
			BridgeTrace_Log(
				"[DT-TRACE] snap#%d entdecode idx=%d ty=%c class=%d(%s) wireBits=%lld ret=%d\n",
				s_dtSnap, a3, rtdTy, cid, DT_ClassName(cid), wireBits, result);

	}

	// --- per-call decode trace (post-call: result + resolved classID known) ---
	// type: P=preserve (a4 set, a5 null), D=delta-to-existing (a4 set, a5 set),
	{
		// The ty=='D' clause fires for EVERY delta entity with no upper bound, so
		// this is an unbounded per-delta disk write in steady-state gameplay -- gate
		// it behind the trace convar so the default playable path pays no per-entity I/O.
		const long n = InterlockedIncrement(&s_rtdTraceCtr);
		// sdk_dt_decode_trace ConVar removed (net_bridge_core.cpp) -- [RTD] trace
		// permanently unarmed, no per-entity I/O in the default playable path.
		(void)n;
	}

	SNAPB_ADD_TOTAL(SNAPB_RTDECODEMAIN, _sbT);
	return result;
}

// CEntityReadInfo: +0 from frame, +8 to frame, +16 update state, +20 old
// entity, +24 new entity, +61 as-delta; frame +48 is rawTick. A state >= 5 is
// a parse failure the worker frees without advancing the delta ack.
typedef char (__fastcall *PFN_ClParsePacketEntities)(__int64 a1, __int64* a2);
static PFN_ClParsePacketEntities s_origClParsePacketEntities = nullptr;

static char __fastcall Hook_ClParsePacketEntities(__int64 a1, __int64* a2)
{
	const char result = s_origClParsePacketEntities(a1, a2);
	if (!a2)
		return result;
	const uintptr_t r = reinterpret_cast<uintptr_t>(a2);
	const int state = *reinterpret_cast<const int*>(r + 16);
	if (state < 5)
		return result;
	static long s_dropN = 0;
	const long n = InterlockedIncrement(&s_dropN);
	if (n <= 20 || (n % 200) == 0)
	{
		const uintptr_t fromFrame = static_cast<uintptr_t>(a2[0]);
		const uintptr_t toFrame = static_cast<uintptr_t>(a2[1]);
		Warning(eDLL_T::ENGINE,
			"[SNAP-DROP] ParsePacketEntities state=%d oldEnt=%d newEnt=%d asDelta=%d toTick=%d fromTick=%d "
			"lastDecode ent=%d cid=%d(%s) ty=%c -- delta ack will not advance (#%ld)\n",
			state,
			*reinterpret_cast<const int*>(r + 20),
			*reinterpret_cast<const int*>(r + 24),
			*reinterpret_cast<const uint8_t*>(r + 61),
			toFrame ? *reinterpret_cast<const int*>(toFrame + 48) : -1,
			fromFrame ? *reinterpret_cast<const int*>(fromFrame + 48) : -1,
			s_curDecEntIdx, s_curDecCid, DT_ClassName(s_curDecCid),
			s_curDecTy ? s_curDecTy : '?', n);
	}
	return result;
}

// -- per-prop decode dispatch. Called from 's
// merge loop for each property. First arg = entry from decoder's prop
typedef __int64 (__fastcall *PFN_PropDecodeDispatch)(__int64 a1, __int64 a2, int a3, __int64 a4, __int64 a5);

static PFN_PropDecodeDispatch s_origPropDecodeDispatch = nullptr;


// -- reads the next changed-prop flat index from the snapshot
// wire (Huffman-coded delta index) inside the delta decoder.
typedef __int64 (__fastcall *PFN_ReadPropIdx)(__int64 *a1);

static PFN_ReadPropIdx s_origReadPropIdx = nullptr;

static __int64 __fastcall Hook_ReadPropIdx(__int64 *a1)
{

	if (!s_ptActive || !a1)
		return s_origReadPropIdx ? s_origReadPropIdx(a1) : 0;

	uintptr_t bb = 0, cp = 0; int av = 0;
	long long posBefore = -1;
	// a1 already null-checked above; PROP-TRACE-only peek.
	bb = (uintptr_t)*a1;
	if (bb && DT_ReadCursor(bb, cp, av))
		posBefore = (long long)cp * 8 - av;

	const __int64 idx = s_origReadPropIdx ? s_origReadPropIdx(a1) : 0;

	long long posAfter = -1;
	if (bb && DT_ReadCursor(bb, cp, av))
		posAfter = (long long)cp * 8 - av;


	if (!s_ptActive)
		return idx;

	const long long prevValBits =
		(s_ptPropN > 0 && posBefore >= 0 && s_ptPrevPos >= 0)
		? (posBefore - s_ptPrevPos) : -1;
	const long long idxBits =
		(posAfter >= 0 && posBefore >= 0) ? (posAfter - posBefore) : -1;

	s_ptCrumbs[s_ptCrumbHead] = { s_ptPropN, idx, posBefore, idxBits, prevValBits };
	s_ptCrumbHead = (s_ptCrumbHead + 1) & 31;
	if (s_ptCrumbCount < 32)
		++s_ptCrumbCount;

	// [PROP-TRACE] SCOPED to CWorld (ent=0) + the player (ent=1). Without this the
	// budget (s_ptTotalLines) is burned by the connect-time flood of every other
	if (s_ptEmitLines && (s_ptEntIdx == 0 || s_ptEntIdx == 1))
	{
		BridgeTrace_Log(
			"[PROP-TRACE] snap#%d ent=%d #%d idx=%lld pos=%lld idxBits=%lld prevValBits=%lld\n",
			s_dtSnap, s_ptEntIdx, s_ptPropN, idx, posBefore, idxBits, prevValBits);
		++s_ptTotalLines;
	}

	s_ptPrevPos = posAfter;
	++s_ptPropN;
	return idx;
}

// Wild SendProp pointers from a desync can look numerically plausible; range checks miss them.
static bool S21_IsReadablePtr(const void* p, size_t bytes)
{
	return Mem_IsReadableCached(p, bytes, &g_dispVqCalls);
}

// [TE-IDXSEQ] per-decode prop-index recorder for DT_TE* tables (, the explosion-event
// OOB). The OOB'd wire-time TEExplosion FULL decode reads indices 14..27 = EXACTLY a second
thread_local int  g_teIdxSeq[64];
thread_local int  g_teIdxSeqCount = 0;
void NetObsTe_ResetIdxSeq(void) { g_teIdxSeqCount = 0; }

// -bridgediaglogs gates the heavy one-shot dumps (hundreds of lines per table).
static bool Bridge_DiagLogsRequested()
{
	static int s_v = -1;
	if (s_v < 0)
	{
		const char* cl = GetCommandLineA();
		s_v = (cl && strstr(cl, "-bridgediaglogs")) ? 1 : 0;
	}
	return s_v != 0;
}

static __int64 __fastcall Hook_PropDecodeDispatch(__int64 a1, __int64 a2, int a3, __int64 a4, __int64 a5)
{
	SNAPB_T0(_sbT);
	// Hot path: only bounds identity. s_currentDecodeRecvTable is set by our
	// RecvTableDecode wrapper -- plain reads, no per-prop SEH / name walk.
	// Prop names are resolved only on error/diag paths below (not every leaf).
	const char* tableName = nullptr;
	int     decoderPropCount = -1;
	__int64 dpPrecalc        = 0;   // decoder+0x18 = the flattened SendProp* array the caller indexes
	if (s_currentDecodeRecvTable)
	{
		tableName = *reinterpret_cast<const char**>(s_currentDecodeRecvTable + 0x4C8);
		const __int64 decoder = *reinterpret_cast<__int64*>(s_currentDecodeRecvTable + 0x4C0);
		if (decoder)
		{
			decoderPropCount = *reinterpret_cast<int*>(decoder + 0x20);
			dpPrecalc        = *reinterpret_cast<__int64*>(decoder + 0x18);
		}
	}

	// a1 is the SendProp pointer. A desync feeds WILD pointers here (observed 4,
	// 0x600000000, 0x8000000000); dereferencing (*(int*)a1, or the original dispatch's
	bool a1WasNull;
	if (dpPrecalc && a3 >= 0 && (decoderPropCount <= 0 || a3 < decoderPropCount))
	{
		// Identity compare only -- no SEH/VQ. Corrupt precalc is an OOB/null path.
		const __int64 dpExpect = *reinterpret_cast<__int64*>(dpPrecalc + 8LL * a3);
		a1WasNull = (!a1 || a1 != dpExpect);
	}
	else if (decoderPropCount > 0 && a3 >= decoderPropCount)
	{
		a1WasNull = true;   // OOB index: never deref a1; the OOB guard below drops the snapshot
	}
	else
	{
		// Rare: dispatch outside a wrapped RecvTableDecode (no precalc context).
		a1WasNull = (!a1 || !S21_IsReadablePtr((const void*)a1, 8));
	}

	// [TE-IDXSEQ] record this dispatch's prop index while decoding a DT_TE* table
	// (buffer reset per event by Hook_EvtWireDecode; printed on its post-call line).
	if (tableName && tableName[0] == 'D' && tableName[1] == 'T' && tableName[2] == '_' &&
	    tableName[3] == 'T' && tableName[4] == 'E' && g_teIdxSeqCount < 64)
		g_teIdxSeq[g_teIdxSeqCount++] = a3;

	// Diagnostic only (feeds bridge_dispatch_stats): a per-prop linear scan of up
	// to 2048 counters = O(props * distinct-props) per snapshot. Verbose-only


	if (decoderPropCount > 0 && a3 >= decoderPropCount)
	{
		const long oobN = InterlockedIncrement(&s_snapOobDrops);
		// Overflow the bitbuf BEFORE any further merge-loop reads; without
		// this a return-0 continues the walk on a sheared cursor.
		Bridge_PropStreamIntegrityFail(a4, "OOB_PROP_INDEX",
			tableName, a3, oobN);
		// First OOB: dump the instancebaseline table. Enter-PVS baseline decode went out of range.
		if (oobN == 1 && Bridge_DiagLogsRequested())
		{
			Bridge_DumpInstanceBaseline();
			char btag[80];
			snprintf(btag, sizeof(btag), "OOB [%s] idx=%d", tableName ? tableName : "?", a3);
			Bridge_DumpBodyBitsRing(btag);
		}

		// m_Precalc.m_nProps is the client's parsed SendTable; m_Props.m_Size is the RecvProp list.
		{
			static const char* s_shapeSeen[8] = {};
			static int s_shapeSeenN = 0;
			bool seen = false;
			for (int i = 0; i < s_shapeSeenN; ++i)
				if (s_shapeSeen[i] == tableName) { seen = true; break; }
			if (!seen && s_shapeSeenN < 8 && s_currentDecodeRecvTable)
			{
				s_shapeSeen[s_shapeSeenN++] = tableName;
				const __int64 dec0 =
					*reinterpret_cast<__int64*>(s_currentDecodeRecvTable + 0x4C0);
				if (dec0)
				{
					const int nPre = *reinterpret_cast<int*>(dec0 + 0x20);
					const int nRcv = *reinterpret_cast<int*>(dec0 + 0x4098);
					Warning(eDLL_T::ENGINE,
						"[OOB-SHAPE] [%s] wireIdx=%d precalc.nProps=%d recvProps.size=%d entCrumbs=%d\n",
						tableName ? tableName : "?", a3, nPre, nRcv, s_entCrumbCount);

					// Flat lists index by index: the precalc SendProp array is what
					// the wire index addresses, the RecvProp array is where it lands.
					const bool dumpFlat = Bridge_DiagLogsRequested();
					uint8_t** spA = *reinterpret_cast<uint8_t***>(dec0 + 0x18);
					uint8_t** rpA = *reinterpret_cast<uint8_t***>(dec0 + 0x4080);
					const int nMax = (nPre > nRcv ? nPre : nRcv);
					for (int i = 0; dumpFlat && i < nMax && i < 512; ++i)
					{
						const char* sn = nullptr; int sty = -1, snb = -1;
						const char* rn = nullptr; int rty = -1;
						uint8_t* sp = (spA && i < nPre &&
							S21_IsReadablePtr(spA + i, sizeof(void*))) ? spA[i] : nullptr;
						if (sp && S21_IsReadablePtr(sp, 0x48))
						{
							sty = *reinterpret_cast<int*>(sp + 0x00);
							snb = *reinterpret_cast<int*>(sp + 0x04);
							sn  = *reinterpret_cast<const char**>(sp + 0x40);
							if (sn && !S21_IsReadablePtr(sn, 1)) sn = nullptr;
						}
						uint8_t* rp = (rpA && i < nRcv &&
							S21_IsReadablePtr(rpA + i, sizeof(void*))) ? rpA[i] : nullptr;
						if (rp && S21_IsReadablePtr(rp, 0x30))
						{
							rty = *reinterpret_cast<int*>(rp + 0x00);
							rn  = *reinterpret_cast<const char**>(rp + 0x28);
							if (rn && !S21_IsReadablePtr(rn, 1)) rn = nullptr;
						}
						Warning(eDLL_T::ENGINE,
							"[OOB-FLAT] %s [%4d] send ty=%2d nb=%3d '%s' | recv ty=%2d '%s'\n",
							tableName ? tableName : "?", i, sty, snb,
							sn ? sn : (sp ? "?" : "<none>"),
							rty, rn ? rn : (rp ? "?" : "<none>"));
					}
				}
			}
		}
		if (oobN <= 50 || (oobN % 1000) == 0)
		{
			const char* tn = tableName ? tableName : "?";
			// Throttled console breadcrumb; full desync detail goes to bridge_trace.log.
			const char* src =
				(a4 && a4 == s_mergeFromBuf) ? "BASE" :
				(a4 && a4 == s_mergeToBuf)   ? "WIRE" : "?";
			Warning(eDLL_T::ENGINE,
				"[NET-OBS] OOB prop index [%s] idx=%d nProps=%d src=%s ent=%d cid=%d(%s) expTbl='%s' prev=(%d,%d,%c) (#%ld); detail -> bridge_trace.log\n",
				tn, a3, decoderPropCount, src, s_curNewEntSlot, s_curNewEntCid,
				DT_ClassName(s_curNewEntCid), DT_TableName(s_curNewEntCid),
				s_curDecEntIdx, s_curDecCid, s_curDecTy, oobN);
			BridgeTrace_Log(
				"[NET-OBS] OOB prop index [%s] idx=%d nProps=%d src=%s ent=%d cid=%d(%s) prev=(%d,%d,%c) (#%ld)\n",
				tn, a3, decoderPropCount, src, s_curNewEntSlot, s_curNewEntCid,
				DT_ClassName(s_curNewEntCid), s_curDecEntIdx, s_curDecCid,
				s_curDecTy, oobN);
			if (s_ptCrumbCount > 0) // crumb dump for ANY OOB'd table
			{
				BridgeTrace_Log(
					"[NET-OBS] last %s prop-index reads before OOB:\n", tn);
				const int start = (s_ptCrumbHead - s_ptCrumbCount + 32) & 31;
				for (int i = 0; i < s_ptCrumbCount; ++i)
				{
					const PropTraceCrumb& c = s_ptCrumbs[(start + i) & 31];
					BridgeTrace_Log(
						"[NET-OBS]   crumb #%d idx=%lld pos=%lld idxBits=%lld prevValBits=%lld\n",
						c.propN, c.idx, c.pos, c.idxBits, c.prevValBits);
				}
			}
			if (s_entCrumbCount > 0) // cross-entity ring: find the upstream desync origin
			{
				BridgeTrace_Log(
					"[NET-OBS] last %d entities decoded before OOB (anomalous wireBits = desync origin):\n",
					s_entCrumbCount);
				const int es = (s_entCrumbHead - s_entCrumbCount + 128) & 127;
				for (int i = 0; i < s_entCrumbCount; ++i)
				{
					const DecodeEntCrumb& e = s_entCrumbs[(es + i) & 127];
					const long long wb =
						(e.startPos >= 0 && e.endPos >= 0) ? (e.endPos - e.startPos) : -1;
					BridgeTrace_Log(
						"[NET-OBS]   ent snap#%d idx=%d ty=%c class=%d(%s) startBit=%lld endBit=%lld wireBits=%lld ret=%d\n",
						e.snap, e.entIdx, e.ty, e.cid, DT_ClassName(e.cid),
						e.startPos, e.endPos, wb, e.ret);
				}
			}
		}
		// No silent continue on a sheared cursor: overflow is set, s_decodeOobProp
		// aborts RecvTableDecodeMain, and repeated OOB Host_Errors (limit > 0).
		SNAPB_ADD_TOTAL(SNAPB_DISPATCH, _sbT);
		return 0;
	}

	if (a1WasNull)
	{
		// a1 is the prop entry from the decoder's flat array (CSendTablePrecalc.m_Props
		// at CRecvDecoder+0x18). NULL means the precalc has no entry for this
		static uintptr_t* s_propDecodeFnTable = nullptr;
		if (!s_propDecodeFnTable)
		{
			const uintptr_t base = reinterpret_cast<uintptr_t>(
				(HMODULE)NetObs_GetExeModuleBase());
			if (base)
				s_propDecodeFnTable = reinterpret_cast<uintptr_t*>(
					NetObs_PropDecodeFnTableAddr());
		}

		// Closed-form: same precalc entry the VQ-KILL identity path already resolved.
		__int64 sendProp = 0;
		if (dpPrecalc && a3 >= 0)
			sendProp = *reinterpret_cast<__int64*>(dpPrecalc + 8LL * a3);

		// DT_Player.m_mantleBoostState, for a table that genuinely gets no slot:
		// peek the value out before the skip below discards it. Strictly read-only
		if (s_mbSendProp && static_cast<uintptr_t>(sendProp) == s_mbSendProp &&
			MantleBoostClient_AuthoritativeEnabled())
		{
			int nValue = 0;
			if (DT_PeekFixedBits(static_cast<uintptr_t>(a4), 32, &nValue) && nValue != 0)
				MantleBoostClient_OnAuthoritativeState(s_curDecEntIdx, nValue);
		}

		// m_nDuckTransitionTimeMsecs, same peek contract: S21 keeps the quantity
		// under another name in another table, so it can only arrive this way.
		if (s_duckRemSendProp && static_cast<uintptr_t>(sendProp) == s_duckRemSendProp)
		{
			int nValue = 0;
			if (DT_PeekFixedBits(static_cast<uintptr_t>(a4), 32, &nValue))
				PredAuth_OnDuckRemainderWire(s_curDecEntIdx, nValue);
		}

		// Critical diagnostic: log every NULL-slot dispatch where the recovered
		// SendProp is m_iSignifierName. Tells us which classes the dedi sends
		if (sendProp)
		{
			const char* sigPropName = *reinterpret_cast<const char**>(sendProp + SP_VARNAME);
			if (sigPropName && strcmp(sigPropName, "m_iSignifierName") == 0)
			{
				static volatile LONG s_nullSigCount = 0;
				const LONG nIdx = InterlockedIncrement(&s_nullSigCount);
				if (nIdx <= 50)
				{
					const char* tn = tableName ? tableName : "?";
					Warning(eDLL_T::ENGINE,
						"[NET-OBS] NULL-SLOT m_iSignifierName dispatch table='%s' idx=%d sendProp=0x%llX (#%ld)\n",
						tn, a3, (unsigned long long)sendProp, nIdx);
				}
			}
		}

		if (sendProp && (uintptr_t)sendProp < 0x800000000000ULL && s_propDecodeFnTable)
		{
			const int propType = *reinterpret_cast<int*>(sendProp);

			// PropType range is small (0..N-1, typically <16). Bound for safety.
			if (propType >= 0 && propType < 32)
			{
				typedef void(__fastcall *PFN_TypeDecode)(__int64 sendProp, __int64 bitbuf);
				PFN_TypeDecode decode = reinterpret_cast<PFN_TypeDecode>(
					s_propDecodeFnTable[6 * propType]);

				if (decode)
				{
					__try { decode(sendProp, a4); }
					__except(EXCEPTION_EXECUTE_HANDLER)
					{
						static int s_decodeCrash = 0;
						if (++s_decodeCrash <= 20)
							Warning(eDLL_T::ENGINE,
								"[NET-OBS] type-decode CRASH idx=%d propType=%d (#%d)\n",
								a3, propType, s_decodeCrash);
					}

					static int s_consumedCount = 0;
					if (++s_consumedCount <= 20 || (s_consumedCount % 5000) == 0)
					{
						const char* tn = tableName ? tableName : "?";
						Warning(eDLL_T::ENGINE,
							"[NET-OBS] consumed bits for NULL prop [%s] idx=%d type=%d (#%d)\n",
							tn, a3, propType, s_consumedCount);
					}
					// Type-specific fallback is not the native dispatch; no ORIG pair.
					SNAPB_ADD_TOTAL(SNAPB_DISPATCH, _sbT);
					return 0;
				}
			}
		}

		// Fallback: SendProp recovery failed or bad type -- cannot consume bits
		// safely. Abort the stream (overflow + entity fail); never continue on a
		// sheared cursor (that cascades into garbage prop indices).
		static int s_nullPropCount = 0;
		const int nNull = ++s_nullPropCount;
		if (nNull <= 20 || (nNull % 1000) == 0)
		{
			const char* tn = tableName ? tableName : "?";
			Warning(eDLL_T::ENGINE,
				"[NET-OBS] UNMATCHED prop [%s] idx=%d sendProp=0x%llX (#%d) -- abort stream (no consume)\n",
				tn, a3, (unsigned long long)sendProp, nNull);
		}
		const long failN = InterlockedIncrement(&s_snapOobDrops);
		Bridge_PropStreamIntegrityFail(a4, "NULL_PROP_NO_CONSUME",
			tableName, a3, failN);
		SNAPB_ADD_TOTAL(SNAPB_DISPATCH, _sbT);
		return 0;
	}
	const bool diag = bridge_prop_dispatch_diag.GetBool();
	long long auditBefore = -1;
	if (diag && a4)
	{
		uintptr_t cc = 0; int aa = 0;
		if (DT_ReadCursor((uintptr_t)a4, cc, aa))
			auditBefore = (long long)cc * 8 - aa;
	}

	// [PROP-ORDER] Detect backwards / huge-forward deltas (staging OOB class).
	if (diag && a5)
	{
		const int prevIdx = *reinterpret_cast<int*>(a5 + 16);
		if (a3 < prevIdx || (long long)a3 - prevIdx > 256)
		{
			static volatile LONG s_orderN = 0;
			const LONG on = InterlockedIncrement(&s_orderN);
			if (on <= 200 || (on % 2000) == 0)
			{
				const char* tn = tableName ? tableName : "?";
				Warning(eDLL_T::ENGINE,
					"[PROP-ORDER] ANOMALY ent=%d class=%d(%s) tbl=%s a3=%d prevIdx=%d delta=%lld (#%ld)\n",
					s_curDecEntIdx, s_curDecCid, DT_ClassName(s_curDecCid),
					tn, a3, prevIdx, (long long)a3 - prevIdx, on);
			}
		}
	}

	// dt_extend's appended props occupy the tail of DT_Player's flat list, and
	// m_mantleBoostState is one of them. It reaches the client in the instance
	if (s_mbSendProp && static_cast<uintptr_t>(a1) == s_mbSendProp &&
		MantleBoostClient_AuthoritativeEnabled())
	{
		int nValue = 0;
		const bool bPeeked = DT_PeekFixedBits(static_cast<uintptr_t>(a4), 32, &nValue);

		// The dispatch is the only place that can tell "the prop never arrived"
		// apart from "it arrived and the handler dropped it", and those need
		static int s_mbHits = 0;
		if (++s_mbHits <= 20)
			Warning(eDLL_T::ENGINE, "[MB-WIRE] dispatch ent=%d idx=%d peek=%d value=%d (#%d)\n",
				s_curDecEntIdx, a3, bPeeked ? 1 : 0, nValue, s_mbHits);
		else if ((s_mbHits % 250) == 0)
			Warning(eDLL_T::ENGINE, "[MB-WIRE] census: %d dispatches so far (latest value=%d)\n",
				s_mbHits, nValue);

		if (bPeeked && nValue != 0)
			MantleBoostClient_OnAuthoritativeState(s_curDecEntIdx, nValue);
	}

	if (s_duckRemSendProp && static_cast<uintptr_t>(a1) == s_duckRemSendProp)
	{
		int nValue = 0;
		if (DT_PeekFixedBits(static_cast<uintptr_t>(a4), 32, &nValue))
			PredAuth_OnDuckRemainderWire(s_curDecEntIdx, nValue);
	}

	__int64 result = 0;
	{
		SNAPB_T0(_sbO);
		if (bridge_decode_seh.GetBool())
		{
			__try { result = s_origPropDecodeDispatch(a1, a2, a3, a4, a5); }
			__except(EXCEPTION_EXECUTE_HANDLER)
			{
				InterlockedIncrement(&g_dispSehHits);
				Bridge_PropStreamIntegrityFail(a4, "DISPATCH_CRASH",
					tableName, a3, g_dispSehHits);
			}
		}
		else
		{
			result = s_origPropDecodeDispatch(a1, a2, a3, a4, a5);
		}
		SNAPB_ADD_ORIG(SNAPB_DISPATCH, _sbO);
	}

	if (diag && auditBefore >= 0 && a4)
	{
		long long auditAfter = -1;
		{ uintptr_t cc = 0; int aa = 0; if (DT_ReadCursor((uintptr_t)a4, cc, aa)) auditAfter = (long long)cc * 8 - aa; }
		const long long bitCost = (auditAfter >= 0) ? (auditAfter - auditBefore) : -1;
		if (bitCost > 2048)
		{
			int roff = -1, ptype = -1; const char* rname = nullptr;
			if (a1) ptype = *reinterpret_cast<int*>(a1);
			if (s_currentDecodeRecvTable)
			{
				const __int64 dec  = *reinterpret_cast<__int64*>(s_currentDecodeRecvTable + 0x4C0);
				const __int64 rArr = dec ? *reinterpret_cast<__int64*>(dec + 0x4080) : 0;
				const __int64 rp   = rArr ? *reinterpret_cast<__int64*>(rArr + 8LL * a3) : 0;
				if (rp) { roff = *reinterpret_cast<int*>(rp + 0x04); rname = *reinterpret_cast<const char**>(rp + 0x28); }
			}
			static volatile LONG s_auditN = 0;
			const LONG an = InterlockedIncrement(&s_auditN);
			if (an <= 200 || (an % 2000) == 0)
				Warning(eDLL_T::ENGINE,
					"[PROP-AUDIT] WIDE ent=%d class=%d(%s) prop='%s' a3=%d type=%d roff=0x%X bitCost=%lld (#%ld)\n",
					s_curDecEntIdx, s_curDecCid, DT_ClassName(s_curDecCid),
					rname ? rname : "?", a3, ptype, (unsigned)roff, bitCost, an);
		}
	}
	SNAPB_ADD_TOTAL(SNAPB_DISPATCH, _sbT);
	return result;
}

// -- CL_CopyNewEntity. Creates a new entity from the snapshot
// bitstream. Crashes at +0x1A5 when m_pServerClasses[iClass].m_pClientClass
typedef char (__fastcall *PFN_CL_CopyNewEntity)(__int64 a1, int* a2, int a3, char* a4);

static PFN_CL_CopyNewEntity s_origCL_CopyNewEntity = nullptr;

// SEH wrapper over the native CL_CopyNewEntity. Stub ClientClass entries
// for the 18 S3-only classes mean every classID has a non-NULL
static char __fastcall Hook_CL_CopyNewEntity_Body(__int64 a1, int* a2, int a3, char* a4)
{
	SNAPB_T0(_sbT);
	// Diagnostic entry log -- first 10 calls only -- so we can tell if the
	// hook is actually firing for the 32 low-slot bridge entities (slots
	++g_snapProfCreateN;
	const int hookSlot = *(int*)(a1 + 0x18);
	const int hookCID  = a2 ? *a2 : -1;
	if (static_cast<unsigned>(hookSlot) >= MAX_EDICTS)
	{
		static volatile LONG s_copyNewSlotBad = 0;
		if (InterlockedIncrement(&s_copyNewSlotBad) <= 8)
		{
			Warning(eDLL_T::ENGINE,
				"[COPYNEW-SLOT] slot=%d -- entity dropped\n", hookSlot);
		}
		if (a4) *a4 = 0;
		return 0;
	}
	// Published BEFORE the native call: RecvTable_MergeDeltas runs inside it and
	// has no entity context of its own, so this is the only place an OOB raised
	// from the merge loop can be attributed to the right entity.
	s_curNewEntSlot = hookSlot;
	s_curNewEntCid  = hookCID;
	s_copyNewOob = 0;
	const int hookHdrCtr = *(int*)(a1 + 0x20);

	// Wire class index reaches the native unchecked (S21 omits the iClass bound
	// check); drop out-of-range creates before the engine indexes its table.
	{
		const int nCopyClasses = *reinterpret_cast<const int*>(NetObs_ClientClassCountAddr());
		const uintptr_t copyClassArr = *reinterpret_cast<const uintptr_t*>(NetObs_ClientClassArrayAddr());
		uintptr_t copyClientClass = 0;
		if (copyClassArr && nCopyClasses > 0 && nCopyClasses <= 4096
			&& hookCID >= 0 && hookCID < nCopyClasses)
		{
			copyClientClass = *reinterpret_cast<const uintptr_t*>(
				copyClassArr + 32ull * static_cast<unsigned>(hookCID));
		}
		if (!copyClientClass)
		{
			static volatile LONG s_copyNewClassBad = 0;
			if (InterlockedIncrement(&s_copyNewClassBad) <= 8)
			{
				Warning(eDLL_T::ENGINE,
					"[COPYNEW-CLASS] slot=%d cid=%d out of range (nClasses=%d) -- entity dropped\n",
					hookSlot, hookCID, nCopyClasses);
			}
			if (a4) *a4 = 0;
			return 0;
		}
	}

	// Capture slot->classID so Hook_PropApplyLoop's post-apply block can
	// resolve the proper className for entity+0x478. Stored as (classID + 1)
	if (a1 && a2)
	{
		const int slotIdx = *reinterpret_cast<int*>(a1 + 0x18);
		const int cid = *a2;
		if ((unsigned)slotIdx < 16384 && cid >= 0)
			s_slotClassID[slotIdx] = cid + 1;
	}

	char result = 0;
	LARGE_INTEGER _cnT0 = {}, _cnT1 = {};
	const bool timeCreate = g_snapBudgetOn || g_snapProfOn;
	if (timeCreate)
		QueryPerformanceCounter(&_cnT0);
	{
		SNAPB_T0(_sbO);
		if (bridge_decode_seh.GetBool())
		{
			__try { result = s_origCL_CopyNewEntity(a1, a2, a3, a4); }
			__except(EXCEPTION_EXECUTE_HANDLER)
			{
				Bridge_PropStreamIntegrityFail(s_mergeToBuf, "COPYNEW_CRASH",
					DT_TableName(hookCID), hookCID, 0);
				if (a4) *a4 = 0;
				result = 0;
			}
		}
		else
		{
			result = s_origCL_CopyNewEntity(a1, a2, a3, a4);
		}
		SNAPB_ADD_ORIG(SNAPB_COPYNEWENT, _sbO);
	}
	if (s_copyNewOob)
	{
		if (a4) *a4 = 0;
		result = 0;
	}
	if (timeCreate)
	{
		QueryPerformanceCounter(&_cnT1);
		g_snapProfCreateTicks += (_cnT1.QuadPart - _cnT0.QuadPart);
	}

	if ((!result || (a4 && *a4)) && Bridge_DiagFirehoseEnabled())
	{
		Warning(eDLL_T::ENGINE,
			"[NET-OBS-HOOK] CL_CopyNewEntity FAIL classID=%d slot=%d "
			"class='%s' table='%s' ret=%d errFlag=%d hdrCtr=%d\n",
			hookCID, hookSlot,
			DT_ClassName(hookCID),
			DT_TableName(hookCID),
			(int)result, a4 ? (int)*a4 : -1,
			hookHdrCtr);
	}

	// EnterPVS decode with the wrong wire-bit count desyncs the rest of the snapshot.
	{
		static volatile long s_cneCtr = 0;
		const long n = InterlockedIncrement(&s_cneCtr);
		// Capped: enough to identify entities at connect without per-entity I/O cost
		// in the hot spawn path (raised to 8000 for the full-class-map join; reverted).
		const bool lateCreate = (n > 600);
		static volatile long s_cneLate = 0;
		if (n <= 600)
			SDK_Log("[CNE] #%ld slot=%d cid=%d -> ret=%d errFlag=%d\n",
				n, *(int*)(a1 + 0x18), a2 ? *a2 : -1, (int)result, a4 ? (int)*a4 : -1);
		else if (lateCreate)
		{
			const long lateN = InterlockedIncrement(&s_cneLate);
			if (Bridge_DiagFirehoseEnabled() && lateN <= 4096)
				SDK_Log("[CNE-LATE] #%ld slot=%d cid=%d -> ret=%d errFlag=%d\n",
					n, *(int*)(a1 + 0x18), a2 ? *a2 : -1, (int)result, a4 ? (int)*a4 : -1);
		}
	}
	SNAPB_ADD_TOTAL(SNAPB_COPYNEWENT, _sbT);
	return result;
}

static char __fastcall Hook_CL_CopyNewEntity(__int64 a1, int* a2, int a3, char* a4)
{
	return Hook_CL_CopyNewEntity_Body(a1, a2, a3, a4);
}

// Plain-C helper so the calling hook can hold C++ objects (std::string,
// std::unordered_set) without triggering C2712 (SEH and object unwinding
// can't coexist in the same function).
static void SVC_DatatableChecksum_SafeReadName(const char* name, char* out, int outSize)
{
	if (outSize <= 0) return;
	out[0] = '\0';
	if (!name) return;
	// Cold log-only: ODP preflight then plain ASCII sanitize (no SEH probe).
	const int maxCopy = outSize - 1;
	if (maxCopy <= 0) return;
	if (!S21_IsReadablePtr(name, (size_t)maxCopy))
		return;
	int i = 0;
	for (; i < maxCopy; ++i)
	{
		const char c = name[i];
		if (c == '\0') break;
		out[i] = (c >= 0x20 && c < 0x7F) ? c : '?';
	}
	out[i] = '\0';
}

static bool Bridge_DtChecksumSwallowEnabled(void)
{
	const char* val = nullptr;
	if (CommandLine()->CheckParm("+bridge_dt_checksum_swallow", &val))
		return !val || val[0] != '0';
	return bridge_dt_checksum_swallow.GetBool();
}

// SVC_DatatableChecksum::Process detour. Name field is inline at msg+40.
// Default swallow: S3 <-> S21 table CRCs differ by construction.
// bridge_dt_checksum_swallow 0 calls native Process (hard disconnect on mismatch).
static char __fastcall Hook_SVC_DatatableChecksum_Process(void* a1, void* a2)
{
	char nameBuf[64];
	nameBuf[0] = '\0';
	if (a2)
		SVC_DatatableChecksum_SafeReadName((const char*)((char*)a2 + 40),
			nameBuf, (int)sizeof(nameBuf));

	if (Bridge_DtChecksumSwallowEnabled())
	{
		static volatile long s_swallowN = 0;
		const long n = InterlockedIncrement(&s_swallowN);
		if (n <= 16 || (n % 64) == 0)
		{
			Warning(eDLL_T::ENGINE,
				"[NET-OBS] swallow SVC_DatatableChecksum name='%s' (#%ld) "
				"-- bridge_dt_checksum_swallow=1 (S3/S21 CRC mismatch expected)\n",
				nameBuf[0] ? nameBuf : "<empty>", n);
		}
		return 1;
	}

	if (!s_origDatatableChkProc)
	{
		static volatile long s_noOrigN = 0;
		if (InterlockedIncrement(&s_noOrigN) <= 8)
		{
			Warning(eDLL_T::ENGINE,
				"[NET-OBS] SVC_DatatableChecksum::Process orig missing name='%s' "
				"-- fail-closed reject\n",
				nameBuf[0] ? nameBuf : "<empty>");
		}
		return 0;
	}

	{
		static volatile long s_nativeN = 0;
		const long n = InterlockedIncrement(&s_nativeN);
		if (n <= 8 || (n % 64) == 0)
		{
			Msg(eDLL_T::ENGINE,
				"[NET-OBS] SVC_DatatableChecksum::Process native name='%s' (#%ld)\n",
				nameBuf[0] ? nameBuf : "<empty>", n);
		}
	}

	return s_origDatatableChkProc(a1, a2);
}


static void InstallDecodeHooks_S21()
{
	int nHooked = 0;
	// --- CreateDecoders prop rename pre-hook ---
	// S3->S21 renames (m_parentAttachmentIndex -> m_parentAttachment, etc.)
	{
		const uintptr_t cdAddr = NetObs_Sym(NetObsSym_t::ClientDataDecode);
		s_origCreateDecoders = (PFN_CreateDecoders)cdAddr;
		if (AttachInTxn(reinterpret_cast<PVOID*>(&s_origCreateDecoders),
		              reinterpret_cast<PVOID>(&Hook_CreateDecoders),
		              "CreateDecoders"))
		{
			SDK_Log("[NET-OBS] hooked CreateDecoders for prop rename\n");
			++nHooked;
		}
	}

	// --- BuildPropOffsetTable post-sweep ---
	// After the engine builds the prop-to-offset mapping table, any decoder-less
	{
		const uintptr_t buildAddr = NetObs_Sym(NetObsSym_t::BuildEntityDecode);
		s_origBuildPropOffsetTable = (PFN_BuildPropOffsetTable)buildAddr;
		if (AttachInTxn(reinterpret_cast<PVOID*>(&s_origBuildPropOffsetTable),
		              reinterpret_cast<PVOID>(&Hook_BuildPropOffsetTable),
		              "BuildPropOffsetTable"))
		{
			SDK_Log("[NET-OBS] hooked BuildPropOffsetTable for NULL mapping sweep\n");
			++nHooked;
		}
	}

	// --- CL_CopyNewEntity NULL ClientClass guard ---
	// Crashes at +0x1A5 when m_pServerClasses[iClass].m_pClientClass is NULL
	// (S3 entity class with no S21 equivalent). Guard before calling original.
	{
		const uintptr_t copyNewAddr = NetObs_Sym(NetObsSym_t::CopyNewEntity);
		s_origCL_CopyNewEntity = (PFN_CL_CopyNewEntity)copyNewAddr;
		if (AttachInTxn(reinterpret_cast<PVOID*>(&s_origCL_CopyNewEntity),
		              reinterpret_cast<PVOID>(&Hook_CL_CopyNewEntity),
		              "CL_CopyNewEntity"))
		{
			SDK_Log("[NET-OBS] hooked CL_CopyNewEntity for NULL ClientClass guard\n");
			++nHooked;
		}
	}

	// --- RecvTable decode NULL decoder guard ---
	// : entity property decode crashes when RecvTable.m_data.decoder
	{
		const uintptr_t recvDecodeAddr = NetObs_Sym(NetObsSym_t::RecvDecode);
		s_origRecvTableDecode = (PFN_RecvTableDecode)recvDecodeAddr;
		if (AttachInTxn(reinterpret_cast<PVOID*>(&s_origRecvTableDecode),
		              reinterpret_cast<PVOID>(&Hook_RecvTableDecode),
		              "RecvTableDecode"))
		{
			SDK_Log("[NET-OBS] hooked RecvTableDecode for NULL decoder guard\n");
			++nHooked;
		}
	}

	// --- Per-prop decode dispatch NULL guard ---
	// Called from 's merge loop for each property. First arg is
	{
		const uintptr_t propDispatchAddr = NetObs_Sym(NetObsSym_t::PropDispatch);
		s_origPropDecodeDispatch = (PFN_PropDecodeDispatch)propDispatchAddr;
		if (AttachInTxn(reinterpret_cast<PVOID*>(&s_origPropDecodeDispatch),
		              reinterpret_cast<PVOID>(&Hook_PropDecodeDispatch),
		              "PropDecodeDispatch"))
		{
			SDK_Log("[NET-OBS] hooked PropDecodeDispatch for NULL prop guard\n");
			++nHooked;
		}

		// PROP-TRACE: per-changed-prop wire trace inside the delta decoder.
		// reads the next changed-prop index. DX12 equivalent
		// is shifted into the S21 DX12 net decoder block at.
		s_origReadPropIdx = (PFN_ReadPropIdx)
			NetObs_Sym(NetObsSym_t::ReadPropIdx);
		if (AttachInTxn(reinterpret_cast<PVOID*>(&s_origReadPropIdx),
		              reinterpret_cast<PVOID>(&Hook_ReadPropIdx),
		              "ReadPropIdx"))
		{
			SDK_Log("[NET-OBS] hooked ReadPropIdx for PROP-TRACE\n");
			++nHooked;
		}

		s_bitbufSeek = (PFN_BitbufSeek)NetObs_Sym(NetObsSym_t::BitbufSeek);
		SDK_Log("[NET-OBS] resolved bitbuf seek at %p\n", (void*)s_bitbufSeek);
	}

	// --- RecvTable_Decode SEH guard ---
	{
		const uintptr_t decodeMainAddr = NetObs_Sym(NetObsSym_t::DecodeMain);
		s_origRecvTableDecodeMain = (PFN_RecvTableDecodeMain)decodeMainAddr;
		if (AttachInTxn(reinterpret_cast<PVOID*>(&s_origRecvTableDecodeMain),
		              reinterpret_cast<PVOID>(&Hook_RecvTableDecodeMain),
		              "RecvTableDecodeMain"))
		{
			SDK_Log("[NET-OBS] hooked RecvTableDecodeMain with SEH guard\n");
			++nHooked;
		}
	}

	{
		s_origClParsePacketEntities = reinterpret_cast<PFN_ClParsePacketEntities>(
			NetObs_Sym(NetObsSym_t::ClParsePacketEntities));
		if (!s_origClParsePacketEntities)
			Warning(eDLL_T::ENGINE, "[SNAP-DROP] ClParsePacketEntities unresolved -- dropped-snapshot probe off\n");
		else if (AttachInTxn(reinterpret_cast<PVOID*>(&s_origClParsePacketEntities),
		                     reinterpret_cast<PVOID>(&Hook_ClParsePacketEntities),
		                     "ClParsePacketEntities"))
			++nHooked;
	}


	// SVC_DatatableChecksum::Process integrity gate. Default swallows mismatch
	// (S3/S21 CRC differ by construction). Fail-closed: +bridge_dt_checksum_swallow 0.
	{
		uintptr_t base = NetObs_GetExeModuleBase();
		if (base)
		{
			// Hook DataTable_SetupReceiveTableFromSendTable for stub diagnostics.
			s_origDataTableSetupRecv = reinterpret_cast<PFN_DataTable_SetupRecv>(
				NetObs_Sym(NetObsSym_t::DataTableSetupRecv));
			if (AttachInTxn(reinterpret_cast<PVOID*>(&s_origDataTableSetupRecv),
			              reinterpret_cast<PVOID>(&Hook_DataTable_SetupRecv),
			              "DataTable_SetupRecv"))
			{
				SDK_Log("[NET-OBS] hooked DataTable_SetupReceiveTableFromSendTable \n");
				++nHooked;
			}
			else
			{
				s_origDataTableSetupRecv = nullptr;
			}


			s_origDatatableChkProc = reinterpret_cast<PFN_SVC_DatatableChecksum_Process>(
				NetObs_Sym(NetObsSym_t::SvcDatatableChecksumProcess));
			if (AttachInTxn(reinterpret_cast<PVOID*>(&s_origDatatableChkProc),
			              reinterpret_cast<PVOID>(&Hook_SVC_DatatableChecksum_Process),
			              "SVC_DatatableChecksum::Process"))
			{
				SDK_Log("[NET-OBS] hooked SVC_DatatableChecksum::Process "
					"(orig at %p) -- native hard-fail default; "
					"lab escape bridge_dt_checksum_swallow\n",
					(void*)s_origDatatableChkProc);
				++nHooked;
			}
			else
			{
				s_origDatatableChkProc = nullptr;
			}
		}
	}

	(void)nHooked;
}

void VNetDecodeDiagS21::Detour(const bool bAttach) const
{
	if (bAttach)
	{
		InstallDecodeHooks_S21();
		Bridge_InstallEarlyPipeHooks();
		Bridge_PumpGate_Bootstrap();
	}
}
