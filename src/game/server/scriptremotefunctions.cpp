//=============================================================================//
//
// Purpose: Script remote-function server path
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/module.h"
#include "tier0/platform.h"
#include "public/tier0/memaddr.h"
#include "tier1/bitbuf.h"
#include "tier1/convar.h"
#include "thirdparty/detours/include/detours.h"
#include "common/netmessages.h"
#include "engine/client/client.h"
#include "engine/server/server.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "game/server/baseentity.h"
#include "game/server/util_server.h"
#include "engine/server/snapshot_diag.h"
#include "engine/host_state.h"
#include "game/shared/scriptremotefunctions_server.h"

// C2S receive-side bound: per-client rate + optional name deny-list.
static ConVar sv_scriptremote_c2s_rate("sv_scriptremote_c2s_rate", "64", FCVAR_RELEASE,
	"Max C2S ScriptRemote calls per client per window. 0 = unlimited (lab only).");
static ConVar sv_scriptremote_c2s_rate_window("sv_scriptremote_c2s_rate_window", "1.0", FCVAR_RELEASE,
	"Seconds for sv_scriptremote_c2s_rate window (clamped 0.1..60).");
static ConVar sv_scriptremote_c2s_flood_kick("sv_scriptremote_c2s_flood_kick", "1", FCVAR_RELEASE,
	"1 = Disconnect on C2S ScriptRemote flood; 0 = drop the call only.");
static ConVar sv_scriptremote_c2s_denylist("sv_scriptremote_c2s_denylist", "", FCVAR_RELEASE,
	"Space-separated registered function names blocked for C2S ScriptRemote (exact match).");
static ConVar sv_scriptremote_c2s_log("sv_scriptremote_c2s_log", "0", FCVAR_DEVELOPMENTONLY,
	"Log C2S ScriptRemote accepts and security rejects ([C2S-SR]).");
static ConVar sv_scriptremote_c2s_error_kick("sv_scriptremote_c2s_error_kick", "1", FCVAR_RELEASE,
	"1 = Disconnect a client after 3 C2S ScriptRemote VM errors in the rate window.");

struct ScriptRemoteC2SRateSlot_t
{
	uint64_t uKey;
	bool bUsed;
	double flWindowStart;
	int nCount;
	int nErrors;
};

// Loopback clients share id64 0; salt their ephemeral UserID into a stable key.
static constexpr uint64_t kScriptRemoteC2SLoopbackSalt = 0x9E3779B97F4A7C15ULL;

static ScriptRemoteC2SRateSlot_t s_scriptRemoteC2SRate[256];

static uint64_t ScriptRemoteC2S_RateKey(CClient* pClient)
{
	const PlatformUserId_t uId64 = pClient->GetPlatformUserId();
	if (uId64 != 0)
		return static_cast<uint64_t>(uId64);
	return (kScriptRemoteC2SLoopbackSalt ^ static_cast<uint64_t>(pClient->GetUserID()));
}

// Open-address linear probe; -1 when full (caller fails open, never kicks).
static int ScriptRemoteC2S_RateSlot(CClient* pClient)
{
	const uint64_t uKey = ScriptRemoteC2S_RateKey(pClient);
	int nSlot = static_cast<int>(uKey & 255);
	int safety = 0;
	while (safety++ < 256)
	{
		ScriptRemoteC2SRateSlot_t& slot = s_scriptRemoteC2SRate[nSlot];
		if (!slot.bUsed || slot.uKey == uKey)
		{
			slot.bUsed = true;
			slot.uKey = uKey;
			return nSlot;
		}
		nSlot = (nSlot + 1) & 255;
	}
	return -1;
}

// Returns false if this call should be dropped (and kick already issued if configured).
static bool ScriptRemoteC2S_CheckRate(CClient* pClient)
{
	const int nLimit = sv_scriptremote_c2s_rate.GetInt();
	if (nLimit <= 0)
		return true; // lab unlimited

	double flWindow = static_cast<double>(sv_scriptremote_c2s_rate_window.GetFloat());
	if (flWindow < 0.1)
		flWindow = 0.1;
	else if (flWindow > 60.0)
		flWindow = 60.0;

	const int nSlot = ScriptRemoteC2S_RateSlot(pClient);
	if (nSlot < 0)
	{
		static volatile LONG s_nRateFullLog = 0;
		if (InterlockedIncrement(&s_nRateFullLog) <= 8)
			Warning(eDLL_T::SERVER, "[C2S-SR] rate table full -- fail-open call (client #%d)\n",
				pClient->GetUserID());
		return true;
	}
	ScriptRemoteC2SRateSlot_t& slot = s_scriptRemoteC2SRate[nSlot];
	const double flNow = Plat_FloatTime();

	if (slot.flWindowStart <= 0.0 || (flNow - slot.flWindowStart) >= flWindow)
	{
		slot.flWindowStart = flNow;
		slot.nCount = 0;
		slot.nErrors = 0;
	}

	++slot.nCount;
	if (slot.nCount <= nLimit)
		return true;

	Warning(eDLL_T::SERVER,
		"[C2S-SR] flood client #%d slot=%d count=%d limit=%d window=%.2fs\n",
		pClient->GetUserID(), nSlot, slot.nCount, nLimit, flWindow);

	if (sv_scriptremote_c2s_flood_kick.GetBool())
	{
		pClient->Disconnect(Reputation_t::REP_MARK_BAD, "#DISCONNECT_SCRIPTREMOTE_FLOOD");
	}
	return false;
}

static bool ScriptRemoteC2S_NoteScriptError(CClient* pClient)
{
	if (!pClient)
		return false;

	const int nSlot = ScriptRemoteC2S_RateSlot(pClient);
	if (nSlot < 0)
	{
		static volatile LONG s_nErrFullLog = 0;
		if (InterlockedIncrement(&s_nErrFullLog) <= 8)
			Warning(eDLL_T::SERVER, "[C2S-SR] rate table full -- fail-open error note (client #%d)\n",
				pClient->GetUserID());
		return false;
	}
	ScriptRemoteC2SRateSlot_t& slot = s_scriptRemoteC2SRate[nSlot];
	const double flNow = Plat_FloatTime();
	double flWindow = static_cast<double>(sv_scriptremote_c2s_rate_window.GetFloat());
	if (flWindow < 0.1)
		flWindow = 0.1;
	else if (flWindow > 60.0)
		flWindow = 60.0;

	if (slot.flWindowStart <= 0.0 || (flNow - slot.flWindowStart) >= flWindow)
	{
		slot.flWindowStart = flNow;
		slot.nCount = 0;
		slot.nErrors = 0;
	}

	++slot.nErrors;
	if (slot.nErrors < 3)
		return false;

	Warning(eDLL_T::SERVER,
		"[C2S-SR] VM-error flood client #%d slot=%d errors=%d\n",
		pClient->GetUserID(), nSlot, slot.nErrors);

	if (sv_scriptremote_c2s_error_kick.GetBool())
		pClient->Disconnect(Reputation_t::REP_MARK_BAD, "#DISCONNECT_SCRIPTREMOTE_ERROR");
	return true;
}

static void ScriptRemoteC2S_CancelHostShutdown(const char* pszName,
	const HostStates_t iStateBefore, const HostStates_t iNextBefore)
{
	if (!g_pHostState)
		return;
	if (iStateBefore == HostStates_t::HS_GAME_SHUTDOWN
		|| iNextBefore == HostStates_t::HS_GAME_SHUTDOWN)
		return;
	if (g_pHostState->m_iCurrentState != HostStates_t::HS_GAME_SHUTDOWN
		&& g_pHostState->m_iNextState != HostStates_t::HS_GAME_SHUTDOWN)
		return;

	g_pHostState->m_iCurrentState = iStateBefore;
	g_pHostState->m_iNextState = iNextBefore;
	Warning(eDLL_T::SERVER,
		"[C2S-SR] cancelled host shutdown scheduled by '%s'\n",
		pszName ? pszName : "?");
}

static bool ScriptRemoteC2S_IsDeniedName(const char* pszName)
{
	if (!pszName || !pszName[0])
		return false;

	const char* pszList = sv_scriptremote_c2s_denylist.GetString();
	if (!pszList || !pszList[0])
		return false;

	// Tokenize on space/tab/comma without allocating.
	const char* p = pszList;
	while (*p)
	{
		while (*p == ' ' || *p == '\t' || *p == ',')
			++p;
		if (!*p)
			break;

		const char* pTok = p;
		while (*p && *p != ' ' && *p != '\t' && *p != ',')
			++p;

		const size_t nTok = static_cast<size_t>(p - pTok);
		const size_t nName = strlen(pszName);
		if (nTok == nName && V_strncmp(pTok, pszName, static_cast<int>(nTok)) == 0)
			return true;
	}
	return false;
}

// FindFunction allocates a handle per call and never frees it. Key on the
// live HSQUIRRELVM; store the name so a re-register cannot reuse a stale index.
struct ScriptRemoteC2SFnCacheEntry_t
{
	uint64_t uEpoch;
	HSCRIPT hFunc;
	char szName[128];
};

static HSQUIRRELVM s_hC2SFnCacheVM = nullptr;
static uint64_t s_uC2SFnCacheEpoch = 1;
static ScriptRemoteC2SFnCacheEntry_t s_c2sFnCache[SCRIPT_REMOTE_SERVER_MAX_FUNCTIONS];

void ScriptRemoteC2S_DropFnCache(void)
{
	++s_uC2SFnCacheEpoch;
	if (s_uC2SFnCacheEpoch == 0)
		s_uC2SFnCacheEpoch = 1;
	s_hC2SFnCacheVM = nullptr;
	memset(s_c2sFnCache, 0, sizeof(s_c2sFnCache));
}

void ScriptRemoteC2S_LevelShutdown(void)
{
	ScriptRemoteC2S_DropFnCache();
}

static HSCRIPT ScriptRemoteC2S_ResolveFunction(const char* pszName, uint16_t nIndex)
{
	if (!g_pServerScript || !pszName || !pszName[0])
		return nullptr;

	const HSQUIRRELVM hVMNow = g_pServerScript->GetVM();
	if (!hVMNow)
		return nullptr;

	if (s_hC2SFnCacheVM != hVMNow)
	{
		ScriptRemoteC2S_DropFnCache();
		s_hC2SFnCacheVM = hVMNow;
	}

	if (nIndex >= SCRIPT_REMOTE_SERVER_MAX_FUNCTIONS)
		return g_pServerScript->FindFunction(pszName, nullptr, nullptr);

	ScriptRemoteC2SFnCacheEntry_t& e = s_c2sFnCache[nIndex];
	if (e.hFunc && e.uEpoch == s_uC2SFnCacheEpoch && strcmp(e.szName, pszName) == 0)
		return e.hFunc;

	const HSCRIPT hFunc = g_pServerScript->FindFunction(pszName, nullptr, nullptr);
	if (hFunc)
	{
		e.uEpoch = s_uC2SFnCacheEpoch;
		e.hFunc = hFunc;
		V_strncpy(e.szName, pszName, sizeof(e.szName));
	}
	else
	{
		e.hFunc = nullptr;
		e.szName[0] = '\0';
	}
	return hFunc;
}

bool ScriptRemoteServer_ProcessMessage(CClient* pClient, NET_ScriptMessage* pMsg)
{
	if (!pClient || !pMsg)
		return false;

	if (!pClient->IsActive())
	{
		Warning(eDLL_T::SERVER, "[C2S-SR] drop from inactive client #%d\n",
			pClient->GetUserID());
		return false;
	}

	bf_read& in = pMsg->m_DataIn;

	const uint32_t nFuncIndex = in.ReadUBitLong(SCRIPT_REMOTE_FUNC_INDEX_BITS);

	const uint32_t nSeqLo = in.ReadUBitLong(31);
	const uint32_t nRelSeq = nSeqLo | (in.ReadOneBit() ? (1u << 31) : 0u);
	if (nRelSeq != 0)
	{
		CClientExtended* const pExt = pClient->GetClientExtended();
		if (pExt && !pExt->AcceptBridgeRelSeq(nRelSeq))
		{
			if (sv_scriptremote_c2s_log.GetBool())
				Msg(eDLL_T::SERVER, "[C2S-SR] drop dup seq=%u client #%d\n",
					nRelSeq, pClient->GetUserID());
			return true;
		}
	}

	if (!ScriptRemoteC2S_CheckRate(pClient))
		return false;

	const ScriptRemoteFuncDesc_t* pFunc = ScriptRemoteServer_GetFunctionByIndex(static_cast<uint16_t>(nFuncIndex));
	if (!pFunc)
	{
		Warning(eDLL_T::SERVER, "[C2S-SR] invalid function index %u (client #%d) -- rejected\n",
			nFuncIndex, pClient->GetUserID());
		return false;
	}

	if (ScriptRemoteC2S_IsDeniedName(pFunc->szName))
	{
		Warning(eDLL_T::SERVER, "[C2S-SR] deny-list blocked '%s' (client #%d)\n",
			pFunc->szName, pClient->GetUserID());
		return false;
	}

	// Log emission blocks the frame thread, and this path is client-driven up to
	// sv_scriptremote_c2s_rate calls per second per client -- silent when off.
	const bool bSrLog = sv_scriptremote_c2s_log.GetBool();
	if (bSrLog)
	{
		Msg(eDLL_T::SERVER, "[C2S-SR] '%s' (index=%u, params=%d) client #%d\n",
			pFunc->szName, nFuncIndex, pFunc->nParamCount, pClient->GetUserID());
	}

	ScriptVariant_t scriptArgs[SCRIPT_REMOTE_SERVER_MAX_PARAMS + 1];
	int nScriptArgCount = 0;

	CPlayer* pPlayer = UTIL_PlayerByIndex(pClient->GetHandle());
	if (!pPlayer || !pPlayer->IsConnected())
	{
		Warning(eDLL_T::SERVER, "[C2S-SR] no player entity for client #%d -- rejected\n", pClient->GetUserID());
		return false;
	}

	const HSCRIPT hPlayerScript = pPlayer->GetScriptInstance();
	if (!hPlayerScript)
	{
		Warning(eDLL_T::SERVER, "[C2S-SR] player has no script instance (client #%d) -- rejected\n", pClient->GetUserID());
		return false;
	}

	scriptArgs[nScriptArgCount++] = hPlayerScript;

	for (int i = 0; i < pFunc->nParamCount; i++)
	{
		// [SR-DIAG] per-param decode trace -- pinpoints the over-read on overflowing funcs.
		if (bSrLog)
		{
			const ScriptRemoteParamDesc_t& dp = pFunc->params[i];
			const int dbgBits = (dp.type == ScriptRemoteParamType_e::SRP_INT)
				? ScriptRemote_IntBitsForRange(dp.intMin, dp.intMax) : -1;
			Msg(eDLL_T::SERVER, "[SR-DIAG] '%s' param[%d] type=%d intMin=%d intMax=%d intBits=%d bitsReadSoFar=%d\n",
				pFunc->szName, i, (int)dp.type, dp.intMin, dp.intMax, dbgBits, (int)in.GetNumBitsRead());
		}

		if (!ScriptRemote_DecodeParam(in, pFunc->params[i],
			scriptArgs, nScriptArgCount,
			SCRIPT_REMOTE_SERVER_MAX_PARAMS + 1,
			"ScriptRemoteServer", pFunc->szName, i))
		{
			// Fail closed: no partial ExecuteFunction.
			Warning(eDLL_T::SERVER, "[C2S-SR] '%s' param %d decode failed (client #%d) -- rejected\n",
				pFunc->szName, i, pClient->GetUserID());
			return false;
		}
	}

	if (in.IsOverflowed())
	{
		Warning(eDLL_T::SERVER, "[C2S-SR] '%s' (index=%u, params=%d) buffer overflowed (client #%d) -- rejected\n",
			pFunc->szName, nFuncIndex, pFunc->nParamCount, pClient->GetUserID());
		return false;
	}

	// Payload is byte-padded: after a correct decode, 0..7 bits remain.
	// Framing divergence is a protocol violation -- fail closed, do not execute.
	const int nBitsLeft = in.GetNumBitsLeft();
	if (nBitsLeft < 0 || nBitsLeft > 7)
	{
		Warning(eDLL_T::SERVER, "[C2S-SR] '%s' (index=%u, params=%d): consumed %d bits, %d left (expected 0-7 pad) -- rejected\n",
			pFunc->szName, nFuncIndex, pFunc->nParamCount, (int)in.GetNumBitsRead(), nBitsLeft);
		return false;
	}

	if (!g_pServerScript)
	{
		Warning(eDLL_T::SERVER, "[C2S-SR] no server VM -- rejected\n");
		return false;
	}

	HSCRIPT hFunc = ScriptRemoteC2S_ResolveFunction(pFunc->szName, pFunc->nIndex);
	if (!hFunc)
	{
		Warning(eDLL_T::SERVER, "[C2S-SR] '%s' not found in VM -- rejected\n", pFunc->szName);
		return false;
	}

	// ClientCallback_PlayerSwitchedWeapons already switched locally; suppress the mirror.
	const bool bClientSwitchFollow = pFunc->szName
		&& strcmp(pFunc->szName, "ClientCallback_PlayerSwitchedWeapons") == 0;
	if (bClientSwitchFollow)
		WeapSelMirror_SetSuppress(true);

	const HostStates_t iStateBefore = g_pHostState
		? g_pHostState->m_iCurrentState : HostStates_t::HS_RUN;
	const HostStates_t iNextBefore = g_pHostState
		? g_pHostState->m_iNextState : HostStates_t::HS_RUN;

	const ScriptStatus_t status = g_pServerScript->ExecuteFunction(
		hFunc, scriptArgs, nScriptArgCount, nullptr, nullptr);

	if (bClientSwitchFollow)
		WeapSelMirror_SetSuppress(false);

	if (status != SCRIPT_DONE)
	{
		Warning(eDLL_T::SERVER, "[C2S-SR] '%s' SCRIPT_ERROR (client #%d) -- dropped\n",
			pFunc->szName, pClient->GetUserID());
		ScriptRemoteC2S_CancelHostShutdown(pFunc->szName, iStateBefore, iNextBefore);
		ScriptRemoteC2S_NoteScriptError(pClient);
		return false;
	}

	return true;
}

//=============================================================================//
// Dual-path: original first, then forward on the S2C lane when enabled.
//=============================================================================//

typedef __int64(__fastcall* Bridge_RemoteFuncSend_t)(__int64 a1, __int64 a2, unsigned __int8 a3, char a4);
static Bridge_RemoteFuncSend_t v_Bridge_RemoteFuncSend = nullptr;

// slot1 (base+16) via tagged resolver, not default-index sq_getentity.
typedef __int64(__fastcall* ScriptGetEntityFromTagged_t)(__int64 vm, __int64 pTaggedValue, __int64 pTypeDesc);
static ScriptGetEntityFromTagged_t v_ScriptGetEntityFromTagged = nullptr;
static void* s_pEntTypeDesc = nullptr;

static ConVar bridge_s2c_scriptremote("bridge_s2c_scriptremote", "1", FCVAR_RELEASE | FCVAR_GAMEDLL,
	"[BRIDGE-S2C-SR] Forward the dedi's native S->C remote function calls "
	"(Remote_CallFunction_NonReplay/_Replay/_UI) to the S21 client on the "
	"name-carried Bridge S2C ScriptRemote lane. 1 = on (default; the native "
	"S3 SVC_UserMessage send still runs but stays suppressed client-side as "
	"today), 0 = off.");

static ConVar bridge_s2c_scriptremote_log("bridge_s2c_scriptremote_log", "0", FCVAR_DEVELOPMENTONLY | FCVAR_GAMEDLL,
	"Log every S->C remote function this dedi forwards on the bridge lane ([S2C-SR-SEND] name/argc).");

// Raw SQObject tag values from squirrel.h tagSQObjectType bits.
static constexpr int32_t kSqTag_Null   = 0x01000001;
static constexpr int32_t kSqTag_Int    = 0x05000002;
static constexpr int32_t kSqTag_Float  = 0x05000004;
static constexpr int32_t kSqTag_Bool   = 0x01000008;
static constexpr int32_t kSqTag_String = 0x08000010;
static constexpr int32_t kSqTag_Entity = 0x0A400000;   // R5 entity/instance tag
static constexpr int32_t kSqTag_Vector = 0x00040000;   // R5 vector; 3 floats inline at slot+4/+8/+12

// Raw 16:16 EHANDLE (index|serial). Do not re-pack to 14:10. Null is 0xFFFFFFFF
// (0 is worldspawn).
static uint32_t Bridge_S2C_GetNetEHandle(void* pEnt)
{
	if (!pEnt)
		return 0xFFFFFFFFu;
	return *reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(pEnt) + 8);
}

// One 16-byte tagged SQObject slot: type@0, pad/vecX@4, val@8.
struct BridgeS2CSqSlot_t
{
	int32_t type;
	float   pad;
	union { int32_t i; float f; void* ptr; } val;
};
static_assert(sizeof(BridgeS2CSqSlot_t) == 16, "BridgeS2CSqSlot_t must mirror the 16-byte SQObject layout");

// POD-only result of the raw SQVM extraction below. No members with
// non-trivial destructors (see the __try/C2712 note on the extractor).
struct BridgeS2CExtracted_t
{
	char name[BRIDGE_S2C_SCRIPTREMOTE_MAX_NAME_LEN + 1];
	int nameLen;
	CClient* pTargetClient;
	int argCount;
	BridgeS2CScriptRemoteArg_t args[BRIDGE_S2C_SCRIPTREMOTE_MAX_ARGS];
	char strScratch[BRIDGE_S2C_SCRIPTREMOTE_MAX_ARGS][BRIDGE_S2C_SCRIPTREMOTE_MAX_STRING_LEN + 1];
};

// POD-only leaf so the walk can sit in SEH __try (C2712). Fail closed.
static bool Bridge_S2C_ExtractCall(__int64 a1, __int64 a2, BridgeS2CExtracted_t& out)
{
	out.pTargetClient = nullptr;

	__try
	{
		const uint8_t* const base = *reinterpret_cast<const uint8_t* const*>(a2 + 88);
		if (!base)
			return false;

		// slot2 (base+32) is the function name, OT_STRING.
		const BridgeS2CSqSlot_t* const nameSlot =
			reinterpret_cast<const BridgeS2CSqSlot_t*>(base + 32);
		if (nameSlot->type != kSqTag_String || !nameSlot->val.ptr)
		{
			Warning(eDLL_T::SERVER, "[BRIDGE-S2C-SR] send hook: slot2 is not OT_STRING (bug -- native already validated this)\n");
			return false;
		}

		// SQString: 64-byte header, char buffer follows (same 'ptr+64'
		// convention the native itself uses for its own name read).
		const char* const pszName = reinterpret_cast<const char*>(nameSlot->val.ptr) + 64;
		const size_t nameLenRaw = strnlen(pszName, BRIDGE_S2C_SCRIPTREMOTE_MAX_NAME_LEN + 1);
		if (nameLenRaw == 0 || nameLenRaw > static_cast<size_t>(BRIDGE_S2C_SCRIPTREMOTE_MAX_NAME_LEN))
		{
			Warning(eDLL_T::SERVER, "[BRIDGE-S2C-SR] send hook: name length %zu out of bounds\n", nameLenRaw);
			return false;
		}
		memcpy(out.name, pszName, nameLenRaw);
		out.name[nameLenRaw] = '\0';
		out.nameLen = static_cast<int>(nameLenRaw);

		// slot1 (base+16) via tagged resolver, not v_sq_getentity.
		if (!v_ScriptGetEntityFromTagged || !s_pEntTypeDesc)
		{
			Warning(eDLL_T::SERVER, "[BRIDGE-S2C-SR] send hook: entity resolver not resolved -- dropping\n");
			return false;
		}
		alignas(16) unsigned char taggedTarget[16];
		memcpy(taggedTarget, base + 16, sizeof(taggedTarget));
		void* const pEnt = reinterpret_cast<void*>(v_ScriptGetEntityFromTagged(
			a1, reinterpret_cast<__int64>(taggedTarget), reinterpret_cast<__int64>(s_pEntTypeDesc)));
		if (!pEnt)
		{
			Warning(eDLL_T::SERVER, "[BRIDGE-S2C-SR] send hook: '%s' arg0 is not an entity\n", out.name);
			return false;
		}
		// IsPlayer via vtbl slot 93 (offset 744) -- same check the native runs.
		unsigned char* const pEntVtbl = *reinterpret_cast<unsigned char**>(pEnt);
		const auto pfnIsPlayer =
			*reinterpret_cast<unsigned char(__fastcall**)(void*)>(pEntVtbl + 744);
		if (!pfnIsPlayer(pEnt))
		{
			Warning(eDLL_T::SERVER, "[BRIDGE-S2C-SR] send hook: '%s' target is not a player\n", out.name);
			return false;
		}
		CPlayer* const pTargetPlayer = reinterpret_cast<CPlayer*>(pEnt);

		CClient* const pTargetClient = g_pServer->GetClient(pTargetPlayer->GetEdict() - 1);
		if (!pTargetClient || !pTargetClient->IsActive())
		{
			Warning(eDLL_T::SERVER, "[BRIDGE-S2C-SR] send hook: '%s' target client not active\n", out.name);
			return false;
		}

		// argCount = *(DWORD*)(a2+120) - *(DWORD*)(a2+84) - 3 -- the native's own param-loop bound.
		const int32_t topIdx = *reinterpret_cast<const int32_t*>(a2 + 120);
		const int32_t baseIdx = *reinterpret_cast<const int32_t*>(a2 + 84);
		int argCount = topIdx - baseIdx - 3;

		if (argCount < 0)
			argCount = 0;

		if (argCount > BRIDGE_S2C_SCRIPTREMOTE_MAX_ARGS)
		{
			Warning(eDLL_T::SERVER, "[BRIDGE-S2C-SR] send hook: '%s' arg count %d truncated to %d\n",
				out.name, argCount, BRIDGE_S2C_SCRIPTREMOTE_MAX_ARGS);
			argCount = BRIDGE_S2C_SCRIPTREMOTE_MAX_ARGS;
		}

		for (int i = 0; i < argCount; i++)
		{
			const BridgeS2CSqSlot_t* const slot =
				reinterpret_cast<const BridgeS2CSqSlot_t*>(base + 48 + 16 * i);
			BridgeS2CScriptRemoteArg_t& a = out.args[i];

			switch (slot->type)
			{
			case kSqTag_Bool:
				a.type = BridgeS2CScriptRemoteType_e::BOOL;
				a.b = (slot->val.i != 0);
				break;
			case kSqTag_Int:
				a.type = BridgeS2CScriptRemoteType_e::INT;
				a.i = slot->val.i;
				break;
			case kSqTag_Float:
				a.type = BridgeS2CScriptRemoteType_e::FLOAT;
				a.f = slot->val.f;
				break;
			case kSqTag_Vector:
			{
				// R5 vector: 3 floats stored INLINE at slot+4, +8, +12 (the
				// value union starts at +8, so x sits in the pad slot at +4).
				const float* const v = reinterpret_cast<const float*>(
					reinterpret_cast<const unsigned char*>(slot) + 4);
				a.type = BridgeS2CScriptRemoteType_e::VECTOR;
				a.vec[0] = v[0];
				a.vec[1] = v[1];
				a.vec[2] = v[2];
				break;
			}
			case kSqTag_String:
			{
				if (!slot->val.ptr)
				{
					Warning(eDLL_T::SERVER, "[BRIDGE-S2C-SR] send hook: '%s' arg %d null string -- dropping call\n",
						out.name, i);
					return false;
				}
				const char* const pszArg = reinterpret_cast<const char*>(slot->val.ptr) + 64;
				const size_t argLen = strnlen(pszArg, BRIDGE_S2C_SCRIPTREMOTE_MAX_STRING_LEN + 1);
				if (argLen > static_cast<size_t>(BRIDGE_S2C_SCRIPTREMOTE_MAX_STRING_LEN))
				{
					Warning(eDLL_T::SERVER, "[BRIDGE-S2C-SR] send hook: '%s' arg %d string too long (%zu) -- dropping call\n",
						out.name, i, argLen);
					return false;
				}
				memcpy(out.strScratch[i], pszArg, argLen);
				out.strScratch[i][argLen] = '\0';
				a.type = BridgeS2CScriptRemoteType_e::STRING;
				a.str = out.strScratch[i];
				a.strLen = static_cast<int>(argLen);
				break;
			}
			case kSqTag_Entity:
			{
				// Entity arg: resolve via the same helper, send networkable EHandle as ENTITY (0x28).
				alignas(16) unsigned char taggedArg[16];
				memcpy(taggedArg, slot, sizeof(taggedArg));
				void* const pArgEnt = (v_ScriptGetEntityFromTagged && s_pEntTypeDesc)
					? reinterpret_cast<void*>(v_ScriptGetEntityFromTagged(
						a1, reinterpret_cast<__int64>(taggedArg), reinterpret_cast<__int64>(s_pEntTypeDesc)))
					: nullptr;
				a.type = BridgeS2CScriptRemoteType_e::ENTITY;
				a.ehandle = pArgEnt ? Bridge_S2C_GetNetEHandle(pArgEnt) : 0xFFFFFFFFu;
				break;
			}
			case kSqTag_Null:
				// Null arg encodes as entity handle 0 (client null HSCRIPT).
				a.type = BridgeS2CScriptRemoteType_e::ENTITY;
				a.ehandle = 0xFFFFFFFFu;
				break;
			default:
				Warning(eDLL_T::SERVER,
					"[BRIDGE-S2C-SR] send hook: '%s' arg %d unhandled/unrecognized SQ tag 0x%X -- dropping call "
					"(vector/entity/typed_entity variadic args are UNVERIFIED for this lane, see reconciliation doc)\n",
					out.name, i, static_cast<unsigned int>(slot->type));
				return false;
			}
		}

		out.pTargetClient = pTargetClient;
		out.argCount = argCount;
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Warning(eDLL_T::SERVER, "[BRIDGE-S2C-SR] send hook: exception while reading the SQVM call-context -- dropped "
			"(native SVC_UserMessage send already completed above)\n");
		return false;
	}
}

static __int64 __fastcall Hook_Bridge_RemoteFuncSend(__int64 a1, __int64 a2, unsigned __int8 a3, char a4)
{
	// Extract before the original: the native pops the live SQVM stack.
	const bool bWant = bridge_s2c_scriptremote.GetBool();
	BridgeS2CExtracted_t extracted;
	const bool bExtracted = bWant && Bridge_S2C_ExtractCall(a1, a2, extracted);

	const __int64 result = v_Bridge_RemoteFuncSend(a1, a2, a3, a4);

	if (result == -1 || !bExtracted)
	{
		if (bWant && result == -1)
			Warning(eDLL_T::SERVER, "[BRIDGE-S2C-SR] send hook: original native returned -1 for '%s' -- bridge send skipped\n",
				bExtracted ? extracted.name : "<extraction failed>");
		return result;
	}

	NET_ScriptMessage msg;
	msg.InitWrite();
	msg.m_bIsTyped = false;

	// Stamp this frame's snapshot tick; the reliable stream lands ahead of the snapshot.
	const uint32_t snapshotTick = static_cast<uint32_t>(Bridge_GetWireSnapshotTick());

	if (!Bridge_S2C_ScriptRemote_EncodeFrame(extracted.name, extracted.nameLen, a4 != 0, a3,
		snapshotTick, extracted.argCount, extracted.args, msg.m_DataOut))
	{
		return result;
	}

	extracted.pTargetClient->SendNetMsgEx(&msg, false, true, false);

	if (bridge_s2c_scriptremote_log.GetBool())
		Msg(eDLL_T::SERVER, "[S2C-SR-SEND] '%s' argc=%d isUI=%d tick=%u\n",
			extracted.name, extracted.argCount, a4 != 0, snapshotTick);

	return result;
}

void VScriptRemoteS2CBridge::GetAdr(void) const
{
	LogFunAdr("Bridge_RemoteFuncSend", v_Bridge_RemoteFuncSend);
}

void VScriptRemoteS2CBridge::GetFun(void) const
{
	// Shared S3 impl behind Remote_CallFunction_NonReplay/_Replay/_UI.

	// Landmark: function prologue through first vmCtx deref + near-call into the SQ entity-resolve helper.
	CMemory fn = Module_FindPattern(g_GameDll,
		"44 88 4C 24 ?? 44 88 44 24 ?? 48 89 4C 24 ?? 55 41 54 41 56 48 8D AC 24 ?? ?? ?? ?? "
		"48 81 EC ?? ?? ?? ?? 48 8B 42 ?? 4C 8D 05 ?? ?? ?? ?? 4C 8B F2 48 8D 54 24 ?? "
		"0F 10 40 ?? 0F 11 44 24 ?? E8");
	fn.GetPtr(v_Bridge_RemoteFuncSend);

	if (!v_Bridge_RemoteFuncSend)
	{
		Warning(eDLL_T::SERVER, "[BRIDGE-S2C-SR] Remote_CallFunction shared-impl pattern unresolved -- S->C bridge lane disabled\n");
		return;
	}

	// Entity type-descriptor lea at pattern +0x27; entity-resolver near-call at +0x3F.
	s_pEntTypeDesc = fn.Offset(0x27).ResolveRelativeAddress(0x3, 0x7).RCast<void*>();
	v_ScriptGetEntityFromTagged = fn.Offset(0x3F).FollowNearCall().RCast<ScriptGetEntityFromTagged_t>();

	Msg(eDLL_T::SERVER, "[BRIDGE-S2C-SR] resolved: send=%p entResolver=%p entTypeDesc=%p\n",
		reinterpret_cast<void*>(v_Bridge_RemoteFuncSend),
		reinterpret_cast<void*>(v_ScriptGetEntityFromTagged), s_pEntTypeDesc);

	if (!v_ScriptGetEntityFromTagged || !s_pEntTypeDesc)
		Warning(eDLL_T::SERVER, "[BRIDGE-S2C-SR] entity resolver/type-desc navigation failed -- S->C bridge lane disabled\n");
}

void VScriptRemoteS2CBridge::Detour(const bool bAttach) const
{
	if (v_Bridge_RemoteFuncSend)
	{
		DetourSetup(&v_Bridge_RemoteFuncSend, &Hook_Bridge_RemoteFuncSend, bAttach);
	}
	else if (bAttach)
	{
		Warning(eDLL_T::SERVER, "[BRIDGE-S2C-SR] DISABLED -- Remote_CallFunction shared-impl not resolved\n");
	}
}

