//=============================================================================//
//
// Purpose: [TITAN] client diagnostics, see titan_diag.h. The soul and NPC
// classes are followed from their ClientClass creation function; every entity
// is re-read through the engine's entity handle table, so a stale slot is
// never dereferenced.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/memvalidate.h"
#include "tier1/cvar.h"
#include "public/client_class.h"
#include "engine/client/net_bridge_internal.h"
#include "rtech/pak/rpak_observe.h"
#include "game/shared/titan_gate.h"
#include "game/client/classvar_natives.h"
#include "game/client/titan_diag.h"

static ConVar bridge_titan_diag("bridge_titan_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[TITAN] log titan soul and titan NPC creation, the replicated fields on them, and the local "
	"player's soul and cockpit links. Polls once per second and prints on change. 0 = off.");

// C_TitanSoul recv offsets; the class size (0xD00) is checked against the ClientClass before use.
static constexpr ptrdiff_t SOUL_OFF_TITAN         = 0x970;  // m_titan (EHANDLE)
static constexpr ptrdiff_t SOUL_OFF_STANCE        = 0xCE8;  // m_stance
static constexpr ptrdiff_t SOUL_OFF_DOOMED        = 0xCEC;  // m_doomed
static constexpr ptrdiff_t SOUL_OFF_SETTINGS_GUID = 0xCF0;  // m_playerSettingsNum, the settings asset guid
static constexpr ptrdiff_t SOUL_OFF_EJECTING      = 0xCF9;  // m_bEjecting
static constexpr int       SOUL_CLASS_SIZE        = 0xD00;
static constexpr int       NPC_CLASS_SIZE         = 0x1E50; // C_NPC_Titan

// C_BaseCombatCharacter::m_titanSoul and C_Player::m_hCockpitProp (EHANDLEs); pl.currentClass is the
// settings asset guid the class settings are rebuilt from.
static constexpr ptrdiff_t BCC_OFF_TITAN_SOUL = 0x19D4;
static constexpr ptrdiff_t PLAYER_OFF_COCKPIT = 0x34A8;
static constexpr ptrdiff_t PLAYER_OFF_CURRENT_CLASS = 0x24D0;

// SettingsHeader::name.
static constexpr ptrdiff_t SETTINGS_HEADER_OFF_NAME = 0x10;

static constexpr int    MAX_TRACKED = 64;
static constexpr int    MAX_LOG_LINES = 512;
static constexpr int    MAX_UNSEEN_POLLS = 8;
static constexpr double POLL_INTERVAL = 1.0;
static constexpr uint32_t INVALID_HANDLE_VALUE_U32 = 0xFFFFFFFFu;

enum TitanDiagKind_e
{
	TDK_SOUL,
	TDK_NPC,
};

struct TitanDiagTracked_t
{
	volatile LONG nReady;
	int      nKind;
	int      nEntNum;
	int      nSerial;
	// Poll state.
	bool     bSeen;
	int      nUnseenPolls;
	uint32_t hLink;       // soul: m_titan, npc: m_titanSoul
	uint64_t nGuid;
	int      nStance;
	int      nFlags;
};

typedef void* (__fastcall *CreateClientClassFn_t)(int nEntNum, int nSerial);

static TitanDiagTracked_t s_tracked[MAX_TRACKED];
static volatile LONG s_nNextTracked = 0;
static int s_nLogged = 0;
static bool s_bArmed = false;
static bool s_bArmOk = false;
static bool s_bPollFaulted = false;
static double s_flNextPoll = 0.0;

// Entity handle table: 32-byte entries, entity pointer at +0, serial at +8.
static const uint8_t* s_pEntTable = nullptr;

static CreateClientClassFn_t s_pfnCreateSoul = nullptr;
static CreateClientClassFn_t s_pfnCreateNpc = nullptr;

// Local player state as last printed.
static bool     s_bLocalSeen = false;
static bool     s_bLocalTitan = false;
static uint32_t s_hLocalSoul = INVALID_HANDLE_VALUE_U32;
static uint32_t s_hLocalCockpit = INVALID_HANDLE_VALUE_U32;
static uint64_t s_nLocalGuid = 0;

// Client-only entity slot bitmap: 128 x 64 bits, a full bitmap is the 'Too many client-only enities' fatal.
static const uint64_t* s_pClientOnlyBits = nullptr;
static constexpr int CLIENT_ONLY_WORDS = 128;
static constexpr ptrdiff_t ENT_OFF_MODELINDEX = 0xD8;
static int s_nLastSlotsUsed = -1;
static int s_nLastCockpitIdx = 0x7FFFFFFF;

template <typename T>
static inline T TitanDiag_Read(const void* const pBase, const ptrdiff_t nOff)
{
	return *reinterpret_cast<const T*>(reinterpret_cast<uintptr_t>(pBase) + nOff);
}

static bool TitanDiag_CanLog(void)
{
	if (s_nLogged < MAX_LOG_LINES)
	{
		++s_nLogged;
		return true;
	}
	if (s_nLogged == MAX_LOG_LINES)
	{
		++s_nLogged;
		Warning(eDLL_T::CLIENT, "[TITAN] cl log budget of %d lines used -- further changes are not printed\n", MAX_LOG_LINES);
	}
	return false;
}

static void* TitanDiag_EntityBySlot(const int nEntNum, const uint32_t nSerial)
{
	if (!s_pEntTable || nEntNum < 0 || nEntNum > 0xFFFF)
		return nullptr;

	const uint8_t* const pEntry = s_pEntTable + static_cast<size_t>(nEntNum) * 32;
	if (TitanDiag_Read<uint32_t>(pEntry, 8) != nSerial)
		return nullptr;
	return TitanDiag_Read<void*>(pEntry, 0);
}

static void* TitanDiag_HandleToEntity(const uint32_t hHandle)
{
	if (hHandle == INVALID_HANDLE_VALUE_U32)
		return nullptr;
	return TitanDiag_EntityBySlot(static_cast<int>(hHandle & 0xFFFFu), hHandle >> 16);
}

static void TitanDiag_NoteCreated(const int nKind, const int nEntNum, const int nSerial, const void* const pResult)
{
	if (!bridge_titan_diag.GetBool())
		return;

	const LONG nIdx = (InterlockedIncrement(&s_nNextTracked) - 1) % MAX_TRACKED;
	TitanDiagTracked_t& t = s_tracked[nIdx];
	t.nReady = 0;
	t.nKind = nKind;
	t.nEntNum = nEntNum;
	t.nSerial = nSerial;
	t.bSeen = false;
	t.nUnseenPolls = 0;
	t.hLink = INVALID_HANDLE_VALUE_U32;
	t.nGuid = 0;
	t.nStance = 0;
	t.nFlags = 0;
	t.nReady = 1;

	if (TitanDiag_CanLog())
		Msg(eDLL_T::CLIENT, "[TITAN] cl create %s ent=%d serial=%d networkable=%p\n",
			nKind == TDK_SOUL ? "C_TitanSoul" : "C_NPC_Titan", nEntNum, nSerial, pResult);
}

static void* __fastcall Hook_CreateTitanSoul(int nEntNum, int nSerial)
{
	void* const pResult = s_pfnCreateSoul(nEntNum, nSerial);
	TitanDiag_NoteCreated(TDK_SOUL, nEntNum, nSerial, pResult);
	return pResult;
}

static void* __fastcall Hook_CreateNpcTitan(int nEntNum, int nSerial)
{
	void* const pResult = s_pfnCreateNpc(nEntNum, nSerial);
	TitanDiag_NoteCreated(TDK_NPC, nEntNum, nSerial, pResult);
	return pResult;
}

static bool TitanDiag_HookClass(ClientClass* const pClass, const int nExpectSize, const CreateClientClassFn_t pfnHook,
	CreateClientClassFn_t& pfnOrig, const char* const pszName)
{
	if (!pClass)
	{
		Warning(eDLL_T::CLIENT, "[TITAN] cl ClientClass '%s' not found -- its creation is not logged\n", pszName);
		return false;
	}
	if (pClass->m_ClassSize != nExpectSize)
	{
		Warning(eDLL_T::CLIENT, "[TITAN] cl ClientClass '%s' size 0x%X, expected 0x%X -- not hooked\n",
			pszName, pClass->m_ClassSize, nExpectSize);
		return false;
	}
	if (!pClass->m_pCreateFn)
		return false;

	pfnOrig = reinterpret_cast<CreateClientClassFn_t>(pClass->m_pCreateFn);
	pClass->m_pCreateFn = reinterpret_cast<CreateClientClassFn>(pfnHook);
	return true;
}

static bool TitanDiag_Arm(void)
{
	if (s_bArmed)
		return s_bArmOk;
	s_bArmed = true;

	// Both handle accessors load the same table; the offsets in the patterns are the
	// m_hCockpitProp and m_titanSoul reads this file relies on.
	const CMemory cockpit = Module_FindPattern(g_GameDll,
		"8B 81 A8 34 00 00 83 F8 FF 74 ?? 0F B7 C8 48 8D 15 ?? ?? ?? ?? 48 C1 E1 05 C1 E8 10 39 44 11 08");
	const CMemory soul = Module_FindPattern(g_GameDll,
		"8B 81 D4 19 00 00 83 F8 FF 74 ?? 0F B7 D8 48 8D 15 ?? ?? ?? ?? 48 C1 E3 05 C1 E8 10 39 44 13 08");
	const uint8_t* const pTableA = cockpit ? cockpit.Offset(14).ResolveRelativeAddress(3, 7).RCast<const uint8_t*>() : nullptr;
	const uint8_t* const pTableB = soul ? soul.Offset(14).ResolveRelativeAddress(3, 7).RCast<const uint8_t*>() : nullptr;
	if (!pTableA || pTableA != pTableB)
	{
		Warning(eDLL_T::CLIENT, "[TITAN] cl entity handle table unresolved (cockpit=%p soul=%p) -- diag off\n", pTableA, pTableB);
		return false;
	}
	s_pEntTable = pTableA;

	// Client-only index allocator: lea r9 at +7 loads the slot bitmap.
	const CMemory clientOnly = Module_FindPattern(g_GameDll,
		"48 81 EC 28 02 00 00 4C 8D 0D ?? ?? ?? ?? 33 D2 49 8B C1 48 8D 0D ?? ?? ?? ?? 66 0F 1F 44 00 00 4C 8B 00 49 83 F8 FF");
	s_pClientOnlyBits = clientOnly ? clientOnly.Offset(7).ResolveRelativeAddress(3, 7).RCast<const uint64_t*>() : nullptr;
	if (!s_pClientOnlyBits)
		Warning(eDLL_T::CLIENT, "[TITAN] cl client-only slot bitmap unresolved -- slot census off\n");

	ClientClass* pSoulClass = nullptr;
	ClientClass* pNpcClass = nullptr;
	const uintptr_t pHeadAddr = NetObs_NonRewindClientClassHeadAddr();
	if (pHeadAddr && Mem_IsReadable(reinterpret_cast<const void*>(pHeadAddr), sizeof(void*)))
	{
		ClientClass* pClass = *reinterpret_cast<ClientClass* const*>(pHeadAddr);
		for (int nSafety = 0; pClass && nSafety < 4096 && Mem_IsReadable(pClass, sizeof(ClientClass)); ++nSafety)
		{
			const char* const pszName = pClass->m_pNetworkName;
			if (pszName && Mem_IsReadable(pszName, 16))
			{
				if (!strcmp(pszName, "CTitanSoul"))
					pSoulClass = pClass;
				else if (!strcmp(pszName, "CNPC_Titan"))
					pNpcClass = pClass;
			}
			pClass = pClass->m_pNext;
		}
	}

	const bool bSoul = TitanDiag_HookClass(pSoulClass, SOUL_CLASS_SIZE, &Hook_CreateTitanSoul, s_pfnCreateSoul, "CTitanSoul");
	const bool bNpc = TitanDiag_HookClass(pNpcClass, NPC_CLASS_SIZE, &Hook_CreateNpcTitan, s_pfnCreateNpc, "CNPC_Titan");

	Msg(eDLL_T::CLIENT, "[TITAN] cl diag armed: handle table %p, soul class %p (%s), npc class %p (%s); "
		"entities created before this point are not logged\n",
		s_pEntTable, pSoulClass, bSoul ? "hooked" : "off", pNpcClass, bNpc ? "hooked" : "off");

	s_bArmOk = true;
	return true;
}

static void TitanDiag_CopyName(char* const pszOut, const size_t nOut, const uint64_t nGuid)
{
	pszOut[0] = '\0';
	if (!nGuid)
		return;

	const void* const pHeader = Pak_FindInstalledHead_S21(nGuid);
	if (!pHeader || !Mem_IsReadable(pHeader, SETTINGS_HEADER_OFF_NAME + sizeof(void*)))
		return;

	const char* const pszName = TitanDiag_Read<const char*>(pHeader, SETTINGS_HEADER_OFF_NAME);
	if (!pszName || !Mem_IsReadable(pszName, 2))
		return;

	size_t i = 0;
	for (; i + 1 < nOut && Mem_IsReadable(pszName + i, 1) && pszName[i]; ++i)
		pszOut[i] = pszName[i];
	pszOut[i] = '\0';
}

static void TitanDiag_PollLocal(void)
{
	void* const pLocal = ClassVar_LocalPlayer();
	if (!pLocal)
	{
		if (s_bLocalSeen && TitanDiag_CanLog())
			Msg(eDLL_T::CLIENT, "[TITAN] cl local player gone\n");
		s_bLocalSeen = false;
		return;
	}

	const bool bTitan = TitanGate_ReadIsTitanPlayer(pLocal);
	const uint32_t hSoul = TitanDiag_Read<uint32_t>(pLocal, BCC_OFF_TITAN_SOUL);
	const uint32_t hCockpit = TitanDiag_Read<uint32_t>(pLocal, PLAYER_OFF_COCKPIT);
	const uint64_t nGuid = TitanDiag_Read<uint64_t>(pLocal, PLAYER_OFF_CURRENT_CLASS);
	if (s_bLocalSeen && bTitan == s_bLocalTitan && hSoul == s_hLocalSoul && hCockpit == s_hLocalCockpit
		&& nGuid == s_nLocalGuid)
		return;

	s_bLocalSeen = true;
	s_bLocalTitan = bTitan;
	s_hLocalSoul = hSoul;
	s_hLocalCockpit = hCockpit;
	s_nLocalGuid = nGuid;
	if (TitanDiag_CanLog())
	{
		char szName[96];
		TitanDiag_CopyName(szName, sizeof(szName), nGuid);
		Msg(eDLL_T::CLIENT, "[TITAN] cl local player=%p titan=%d GetTitanSoul=%08X->%p cockpit=%08X->%p settings=%016llX asset='%s'\n",
			pLocal, bTitan ? 1 : 0, hSoul, TitanDiag_HandleToEntity(hSoul), hCockpit, TitanDiag_HandleToEntity(hCockpit),
			static_cast<unsigned long long>(nGuid), szName[0] ? szName : "(not in the client asset table)");
	}
}

static void TitanDiag_PollSoul(TitanDiagTracked_t& t, const void* const pEnt)
{
	const uint32_t hTitan = TitanDiag_Read<uint32_t>(pEnt, SOUL_OFF_TITAN);
	const uint64_t nGuid = TitanDiag_Read<uint64_t>(pEnt, SOUL_OFF_SETTINGS_GUID);
	const int nStance = TitanDiag_Read<int>(pEnt, SOUL_OFF_STANCE);
	const int nFlags = (TitanDiag_Read<uint8_t>(pEnt, SOUL_OFF_DOOMED) ? 1 : 0)
		| (TitanDiag_Read<uint8_t>(pEnt, SOUL_OFF_EJECTING) ? 2 : 0);
	if (t.bSeen && hTitan == t.hLink && nGuid == t.nGuid && nStance == t.nStance && nFlags == t.nFlags)
		return;

	t.hLink = hTitan;
	t.nGuid = nGuid;
	t.nStance = nStance;
	t.nFlags = nFlags;
	if (!TitanDiag_CanLog())
		return;

	char szName[96];
	TitanDiag_CopyName(szName, sizeof(szName), nGuid);
	Msg(eDLL_T::CLIENT, "[TITAN] cl soul ent=%d ptr=%p m_titan=%08X->%p settings=%016llX asset='%s' stance=%d doomed=%d ejecting=%d\n",
		t.nEntNum, pEnt, hTitan, TitanDiag_HandleToEntity(hTitan), static_cast<unsigned long long>(nGuid),
		szName[0] ? szName : "(not in the client asset table)", nStance, nFlags & 1, (nFlags >> 1) & 1);
}

static void TitanDiag_PollNpc(TitanDiagTracked_t& t, const void* const pEnt)
{
	const uint32_t hSoul = TitanDiag_Read<uint32_t>(pEnt, BCC_OFF_TITAN_SOUL);
	if (t.bSeen && hSoul == t.hLink)
		return;

	t.hLink = hSoul;
	if (TitanDiag_CanLog())
		Msg(eDLL_T::CLIENT, "[TITAN] cl npc_titan ent=%d ptr=%p GetTitanSoul=%08X->%p\n",
			t.nEntNum, pEnt, hSoul, TitanDiag_HandleToEntity(hSoul));
}

// Cockpit rebuilt each frame shows as a changing handle with a stable model index; a leak as slots climbing.
static void TitanDiag_PollSlots(void)
{
	int nUsed = 0;
	if (s_pClientOnlyBits)
		for (int i = 0; i < CLIENT_ONLY_WORDS; ++i)
			nUsed += static_cast<int>(__popcnt64(s_pClientOnlyBits[i]));

	int nCockpitIdx = -1;
	void* const pLocal = ClassVar_LocalPlayer();
	const void* const pCockpit = pLocal ? TitanDiag_HandleToEntity(TitanDiag_Read<uint32_t>(pLocal, PLAYER_OFF_COCKPIT)) : nullptr;
	if (pCockpit)
		nCockpitIdx = TitanDiag_Read<int16_t>(pCockpit, ENT_OFF_MODELINDEX);

	const bool bSlotsMoved = s_nLastSlotsUsed < 0 || nUsed - s_nLastSlotsUsed >= 16 || s_nLastSlotsUsed - nUsed >= 16;
	if (!bSlotsMoved && nCockpitIdx == s_nLastCockpitIdx)
		return;
	const int nDelta = s_nLastSlotsUsed < 0 ? 0 : nUsed - s_nLastSlotsUsed;
	s_nLastSlotsUsed = nUsed;
	s_nLastCockpitIdx = nCockpitIdx;
	if (TitanDiag_CanLog())
		Msg(eDLL_T::CLIENT, "[TITAN] cl client-only slots=%d/%d (%+d since last) cockpit=%p modelIndex=%d\n",
			nUsed, CLIENT_ONLY_WORDS * 64, nDelta, pCockpit, nCockpitIdx);
}

static void TitanDiag_Poll(void)
{
	TitanDiag_PollLocal();
	TitanDiag_PollSlots();

	for (int i = 0; i < MAX_TRACKED; ++i)
	{
		TitanDiagTracked_t& t = s_tracked[i];
		if (!t.nReady)
			continue;

		const void* const pEnt = TitanDiag_EntityBySlot(t.nEntNum, static_cast<uint32_t>(t.nSerial));
		if (!pEnt)
		{
			// The entity registers after its create function returns; give it a few polls.
			if (t.bSeen || ++t.nUnseenPolls > MAX_UNSEEN_POLLS)
			{
				if (TitanDiag_CanLog())
					Msg(eDLL_T::CLIENT, "[TITAN] cl %s ent=%d serial=%d %s\n",
						t.nKind == TDK_SOUL ? "soul" : "npc_titan", t.nEntNum, t.nSerial,
						t.bSeen ? "removed" : "never registered");
				t.nReady = 0;
			}
			continue;
		}

		if (t.nKind == TDK_SOUL)
			TitanDiag_PollSoul(t, pEnt);
		else
			TitanDiag_PollNpc(t, pEnt);
		t.bSeen = true;
	}
}

void TitanDiag_OnFrame(void)
{
	if (!bridge_titan_diag.GetBool())
		return;

	const double flNow = Plat_StallClockSeconds();
	if (flNow < s_flNextPoll)
		return;
	s_flNextPoll = flNow + POLL_INTERVAL;

	if (!TitanDiag_Arm() || s_bPollFaulted)
		return;

	__try
	{
		TitanDiag_Poll();
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		s_bPollFaulted = true;
		Warning(eDLL_T::CLIENT, "[TITAN] cl poll faulted -- diag stopped for this session\n");
	}
}
