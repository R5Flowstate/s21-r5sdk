//=============================================================================//
//
// Purpose: One-shot per-slot mod policy check after the client is fully in.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier1/convar.h"
#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/detours.h"
#include "mod_policy_gate.h"

#include "engine/host_state.h"
#include "engine/client/client.h"
#include "engine/server/server.h"
#include "engine/server/vengineserver_impl.h"
#include "pluginsystem/modsystem.h"

#include <cstring>

static uint8_t s_nPhase[MAX_PLAYERS] = {};
static uint32_t s_nPhaseUserId[MAX_PLAYERS] = {};
static uint16_t s_nEmptyWait[MAX_PLAYERS] = {};

enum
{
	MODPOLICY_PHASE_IDLE = 0,
	MODPOLICY_PHASE_DONE = 1
};

// Host frames to wait for a late sdk_mods userinfo key after SIGNONSTATE_FULL.
static constexpr uint16_t kModPolicyEmptyRetryMax = 90;

void ModPolicyGate_ResetAll(void)
{
	memset(s_nPhase, 0, sizeof(s_nPhase));
	memset(s_nPhaseUserId, 0, sizeof(s_nPhaseUserId));
	memset(s_nEmptyWait, 0, sizeof(s_nEmptyWait));
}

static bool ModPolicy_NormalizeId(char* const pszOut, const size_t nOut, const char* const pszIn)
{
	if (!pszOut || nOut < 2)
		return false;

	pszOut[0] = '\0';
	if (!pszIn || !pszIn[0])
		return false;

	size_t n = 0;
	for (; pszIn[n]; ++n)
	{
		if (n + 1 >= nOut)
			return false;

		const char ch = pszIn[n];
		pszOut[n] = (ch == '.') ? '_' : ch;
	}

	pszOut[n] = '\0';
	return true;
}

static bool ModPolicy_IdsEqual(const char* const pszLeft, const char* const pszRight)
{
	char szLeft[33]; // 32-char id cap + NUL
	char szRight[33];
	if (!ModPolicy_NormalizeId(szLeft, sizeof(szLeft), pszLeft))
		return false;
	if (!ModPolicy_NormalizeId(szRight, sizeof(szRight), pszRight))
		return false;

	return V_stricmp(szLeft, szRight) == 0;
}

static bool ModPolicy_IdInList(const CUtlVector<CUtlString>& list, const char* const pszId)
{
	if (!pszId || !pszId[0])
		return false;

	FOR_EACH_VEC(list, i)
	{
		if (ModPolicy_IdsEqual(list[i].String(), pszId))
			return true;
	}

	return false;
}

static bool ModPolicy_IsAttestChar(const char ch)
{
	if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))
		return true;
	if (ch >= '0' && ch <= '9')
		return true;

	return ch == '.' || ch == '_' || ch == ',' || ch == '+' || ch == '#' || ch == '-';
}

static void ModPolicy_Reject(CClient* const pClient, const int nSlot, const char* const pszReason)
{
	Warning(eDLL_T::SERVER, "[MOD-POLICY] reject slot=%d reason=%s\n", nSlot, pszReason);
	pClient->Disconnect(REP_MARK_BAD, "#SDK_MODS_POLICY");
}

static bool ModPolicy_EvaluateSlot(CClient* const pClient, const int nSlot, const uint16_t nRetry)
{
	if (!g_pEngineServer)
	{
		if (nRetry < kModPolicyEmptyRetryMax)
			return false;

		ModPolicy_Reject(pClient, nSlot, "absent");
		return true;
	}

	const int nClientIndex = static_cast<int>(pClient->GetHandle());
	if (nClientIndex < 1)
	{
		if (nRetry < kModPolicyEmptyRetryMax)
			return false;

		ModPolicy_Reject(pClient, nSlot, "absent");
		return true;
	}

	const char* const pszRaw = g_pEngineServer->GetClientConVarValue(nClientIndex, "sdk_mods");
	if (!pszRaw)
	{
		if (nRetry < kModPolicyEmptyRetryMax)
			return false;

		ModPolicy_Reject(pClient, nSlot, "absent");
		return true;
	}
	if (!pszRaw[0])
	{
		if (nRetry < kModPolicyEmptyRetryMax)
			return false;

		ModPolicy_Reject(pClient, nSlot, "empty");
		return true;
	}

	const size_t nLen = strnlen(pszRaw, static_cast<size_t>(MOD_ATTESTATION_MAX_LEN) + 1);
	if (nLen >= MOD_ATTESTATION_MAX_LEN)
	{
		ModPolicy_Reject(pClient, nSlot, "too_long");
		return true;
	}

	for (size_t i = 0; i < nLen; ++i)
	{
		if (!ModPolicy_IsAttestChar(pszRaw[i]))
		{
			ModPolicy_Reject(pClient, nSlot, "bad_charset");
			return true;
		}
	}

	CUtlVector<CUtlString> ids;
	bool bTruncated = false;
	if (!ModSystem_ParseAttestation(pszRaw, ids, bTruncated))
	{
		ModPolicy_Reject(pClient, nSlot, "parse");
		return true;
	}

	CUtlVector<CUtlString> required;
	CUtlVector<CUtlString> allowed;
	ModPolicy_GetEffectiveRequired(required);
	ModPolicy_GetEffectiveAllowed(allowed);

	FOR_EACH_VEC(required, i)
	{
		if (!ModPolicy_IdInList(ids, required[i].String()))
		{
			ModPolicy_Reject(pClient, nSlot, "missing_required");
			return true;
		}
	}

	if (sv_modPolicy.GetInt() == 2 && bTruncated)
	{
		Warning(eDLL_T::SERVER, "[MOD-POLICY] reject slot=%d reason=truncated_allowlist count=%d\n",
			nSlot, ids.Count());
		pClient->Disconnect(REP_MARK_BAD, "#SDK_MODS_POLICY");
		return true;
	}

	if (sv_modPolicy.GetInt() == 2)
	{
		FOR_EACH_VEC(ids, i)
		{
			const char* const pszId = ids[i].String();
			if (ModPolicy_IdInList(required, pszId) || ModPolicy_IdInList(allowed, pszId))
				continue;

			ModPolicy_Reject(pClient, nSlot, "not_allowed");
			return true;
		}
	}

	DevMsg(eDLL_T::SERVER, "[MOD-POLICY] accept slot=%d\n", nSlot);
	return true;
}

void ModPolicyGate_OnFrame(void)
{
	if (!g_pServer)
		return;

	const int nPolicy = sv_modPolicy.GetInt();
	const int nMax = g_pServer->GetMaxClients();
	const int nSlots = (nMax < MAX_PLAYERS) ? nMax : MAX_PLAYERS;

	for (int i = 0; i < nSlots; ++i)
	{
		CClient* const pClient = g_pServer->GetClient(i);
		if (!pClient || !pClient->IsConnected() || pClient->IsFakeClient())
		{
			s_nPhase[i] = MODPOLICY_PHASE_IDLE;
			s_nPhaseUserId[i] = 0;
			s_nEmptyWait[i] = 0;
			continue;
		}

		if (s_nPhaseUserId[i] != static_cast<uint32_t>(pClient->GetUserID()))
		{
			s_nPhase[i] = MODPOLICY_PHASE_IDLE;
			s_nEmptyWait[i] = 0;
		}

		if (nPolicy == 0)
			continue;

		if (s_nPhase[i] != MODPOLICY_PHASE_IDLE)
			continue;

		if (!pClient->IsActive())
			continue;

		s_nPhaseUserId[i] = static_cast<uint32_t>(pClient->GetUserID());
		if (!ModPolicy_EvaluateSlot(pClient, i, s_nEmptyWait[i]))
		{
			++s_nEmptyWait[i];
			continue;
		}

		s_nPhase[i] = MODPOLICY_PHASE_DONE;
		s_nEmptyWait[i] = 0;
	}
}

static void Hook_CHostState_State_GameShutDown(CHostState* thisptr)
{
	ModPolicyGate_ResetAll();
	v_CHostState_State_GameShutDown(thisptr);
}

//-----------------------------------------------------------------------------
void VModPolicyGate::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 57 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ??")
		.GetPtr(v_CHostState_State_GameShutDown);

	if (!v_CHostState_State_GameShutDown)
		Warning(eDLL_T::SERVER, "[MOD-POLICY] CHostState::State_GameShutDown pattern unresolved\n");
}

void VModPolicyGate::Detour(const bool bAttach) const
{
	if (v_CHostState_State_GameShutDown)
		DetourSetup(&v_CHostState_State_GameShutDown, &Hook_CHostState_State_GameShutDown, bAttach);
}
