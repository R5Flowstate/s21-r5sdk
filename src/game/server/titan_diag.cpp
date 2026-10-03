//=============================================================================//
//
// Purpose: [TITAN] dedi diagnostics, see titan_diag.h. Walks the entity list
// once per second and prints a soul, titan NPC or player the first time it
// appears and whenever a field it tracks changes.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/memvalidate.h"
#include "tier1/cvar.h"
#include "game/server/entitylist.h"
#include "game/server/titan_diag.h"
#include "game/shared/titan_gate.h"
#include "rtech/pak/settings_disk.h"

static ConVar bridge_titan_diag("bridge_titan_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[TITAN] log titan souls, titan NPCs and titan-class players as the dedi publishes them: first "
	"appearance, the settings asset a soul names, and every change. Polls once per second. 0 = off.");

// CTitanSoul send offsets.
static constexpr ptrdiff_t SOUL_OFF_STANCE        = 0xB40;  // m_stance
static constexpr ptrdiff_t SOUL_OFF_SETTINGS_GUID = 0xB48;  // m_playerSettingsNum, the settings asset guid
static constexpr ptrdiff_t SOUL_OFF_DOOMED        = 0xB50;  // m_doomed
static constexpr ptrdiff_t SOUL_OFF_EJECTING      = 0xB52;  // m_bEjecting
static constexpr ptrdiff_t SOUL_OFF_TITAN         = 0xD00;  // m_titan (EHANDLE)

// CBaseCombatCharacter::m_titanSoul (EHANDLE); CPlayer class settings block and pl.currentClass
// (the settings asset guid the client rebuilds its class settings from).
static constexpr ptrdiff_t BCC_OFF_TITAN_SOUL      = 0x1770;
static constexpr ptrdiff_t PLAYER_OFF_CLASS_SETTINGS = 0x5F08;
static constexpr ptrdiff_t PLAYER_OFF_CURRENT_CLASS  = 0x5DF0;

// SettingsHeader::name; asset slot table follows the type registry by 0x1000.
static constexpr ptrdiff_t SETTINGS_HEADER_OFF_NAME = 0x10;
static constexpr uintptr_t PAK_ASSET_SLOT_TABLE_OFFSET = 0x1000;
static constexpr uint32_t  PAK_ASSET_SLOT_MASK = 0x3FFFFu;
static constexpr uint32_t  PAK_ASSET_SLOT_STRIDE = 32;
static constexpr uint32_t  PAK_ASSET_PROBE_MAX = 4096;

static constexpr int    MAX_TRACKED = 96;
static constexpr int    MAX_LOG_LINES = 512;
static constexpr int    MAX_ENTITY_WALK = 20000;
static constexpr double POLL_INTERVAL = 1.0;

enum TitanDiagKind_e
{
	TDK_SOUL,
	TDK_NPC,
	TDK_PLAYER,
};

struct TitanDiagEnt_t
{
	bool     bUsed;
	bool     bSeen;
	int      nKind;
	int      nIndex;
	int      nSerial;
	uint32_t hLink;         // soul: m_titan, npc/player: m_titanSoul
	uint64_t nGuid;
	int      nStance;
	int      nFlags;        // soul: bit0 doomed, bit1 ejecting; player: bit0 titan class
	const void* pSettings;  // player class settings block
};

static TitanDiagEnt_t s_tracked[MAX_TRACKED];
static int s_nLogged = 0;
static bool s_bAnnounced = false;
static double s_flNextPoll = 0.0;

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
		Warning(eDLL_T::SERVER, "[TITAN] sv log budget of %d lines used -- further changes are not printed\n", MAX_LOG_LINES);
	}
	return false;
}

static void TitanDiag_CopyName(char* const pszOut, const size_t nOut, const uint64_t nGuid)
{
	pszOut[0] = '\0';
	if (!nGuid || !g_pPakAssetTypeRegistry_S3)
		return;

	const uint8_t* const pSlots = reinterpret_cast<const uint8_t*>(g_pPakAssetTypeRegistry_S3 + PAK_ASSET_SLOT_TABLE_OFFSET);
	uint32_t nSlot = static_cast<uint32_t>(nGuid) & PAK_ASSET_SLOT_MASK;
	for (uint32_t nProbe = 0; nProbe < PAK_ASSET_PROBE_MAX; ++nProbe)
	{
		const uint8_t* const pEntry = pSlots + static_cast<size_t>(nSlot) * PAK_ASSET_SLOT_STRIDE;
		const uint64_t nSlotGuid = TitanDiag_Read<uint64_t>(pEntry, 0);
		if (nSlotGuid == 0)
			return;
		if (nSlotGuid == nGuid)
		{
			const void* const pHeader = TitanDiag_Read<const void*>(pEntry, 0x10);
			if (!pHeader || !Mem_IsReadable(pHeader, SETTINGS_HEADER_OFF_NAME + sizeof(void*)))
				return;
			const char* const pszName = TitanDiag_Read<const char*>(pHeader, SETTINGS_HEADER_OFF_NAME);
			if (!pszName || !Mem_IsReadable(pszName, 2))
				return;
			size_t i = 0;
			for (; i + 1 < nOut && Mem_IsReadable(pszName + i, 1) && pszName[i]; ++i)
				pszOut[i] = pszName[i];
			pszOut[i] = '\0';
			return;
		}
		nSlot = (nSlot + 1) & PAK_ASSET_SLOT_MASK;
	}
}

// Class name of the live entity a handle names, or null.
static const char* TitanDiag_HandleClass(const uint32_t hValue)
{
	if (hValue == 0xFFFFFFFFu)
		return nullptr;

	const CBaseHandle handle = CBaseHandle::UnsafeFromIndex(static_cast<int>(hValue));
	const CEntInfo* const pInfo = g_serverEntityList->GetEntInfoPtr(handle);
	if (!pInfo || !pInfo->m_pEntity || pInfo->m_SerialNumber != handle.GetSerialNumber())
		return nullptr;
	return STRING(pInfo->m_iClassName);
}

static TitanDiagEnt_t* TitanDiag_Slot(const int nKind, const int nIndex, const int nSerial, bool& bCreated)
{
	TitanDiagEnt_t* pFree = nullptr;
	for (int i = 0; i < MAX_TRACKED; ++i)
	{
		TitanDiagEnt_t& t = s_tracked[i];
		if (!t.bUsed)
		{
			if (!pFree)
				pFree = &t;
			continue;
		}
		if (t.nKind == nKind && t.nIndex == nIndex && t.nSerial == nSerial)
		{
			bCreated = false;
			return &t;
		}
	}

	bCreated = true;
	if (pFree)
	{
		*pFree = TitanDiagEnt_t();
		pFree->bUsed = true;
		pFree->nKind = nKind;
		pFree->nIndex = nIndex;
		pFree->nSerial = nSerial;
		pFree->hLink = 0xFFFFFFFFu;
	}
	return pFree;
}

static void TitanDiag_PollSoul(TitanDiagEnt_t& t, const void* const pEnt, const bool bCreated)
{
	const uint32_t hTitan = TitanDiag_Read<uint32_t>(pEnt, SOUL_OFF_TITAN);
	const uint64_t nGuid = TitanDiag_Read<uint64_t>(pEnt, SOUL_OFF_SETTINGS_GUID);
	const int nStance = TitanDiag_Read<int>(pEnt, SOUL_OFF_STANCE);
	const int nFlags = (TitanDiag_Read<uint8_t>(pEnt, SOUL_OFF_DOOMED) ? 1 : 0)
		| (TitanDiag_Read<uint8_t>(pEnt, SOUL_OFF_EJECTING) ? 2 : 0);
	if (!bCreated && hTitan == t.hLink && nGuid == t.nGuid && nStance == t.nStance && nFlags == t.nFlags)
		return;

	t.hLink = hTitan;
	t.nGuid = nGuid;
	t.nStance = nStance;
	t.nFlags = nFlags;
	if (!TitanDiag_CanLog())
		return;

	char szName[96];
	TitanDiag_CopyName(szName, sizeof(szName), nGuid);
	const char* const pszTitan = TitanDiag_HandleClass(hTitan);
	Msg(eDLL_T::SERVER, "[TITAN] sv soul %s ent=%d ptr=%p m_titan=%08X->%s settings=%016llX asset='%s' stance=%d doomed=%d ejecting=%d\n",
		bCreated ? "create" : "change", t.nIndex, pEnt, hTitan, pszTitan ? pszTitan : "(none)",
		static_cast<unsigned long long>(nGuid), szName[0] ? szName : "(not in the dedi asset table)",
		nStance, nFlags & 1, (nFlags >> 1) & 1);
}

static void TitanDiag_PollNpc(TitanDiagEnt_t& t, const void* const pEnt, const bool bCreated)
{
	const uint32_t hSoul = TitanDiag_Read<uint32_t>(pEnt, BCC_OFF_TITAN_SOUL);
	if (!bCreated && hSoul == t.hLink)
		return;

	t.hLink = hSoul;
	if (TitanDiag_CanLog())
	{
		const char* const pszSoul = TitanDiag_HandleClass(hSoul);
		Msg(eDLL_T::SERVER, "[TITAN] sv npc_titan %s ent=%d ptr=%p m_titanSoul=%08X->%s\n",
			bCreated ? "create" : "change", t.nIndex, pEnt, hSoul, pszSoul ? pszSoul : "(none)");
	}
}

static void TitanDiag_PollPlayer(TitanDiagEnt_t& t, const void* const pEnt, const bool bCreated)
{
	const bool bTitan = TitanGate_ReadIsTitanPlayer(pEnt);
	const uint32_t hSoul = TitanDiag_Read<uint32_t>(pEnt, BCC_OFF_TITAN_SOUL);
	const void* const pSettings = TitanDiag_Read<const void*>(pEnt, PLAYER_OFF_CLASS_SETTINGS);
	const uint64_t nGuid = TitanDiag_Read<uint64_t>(pEnt, PLAYER_OFF_CURRENT_CLASS);
	if (!bCreated && (bTitan ? 1 : 0) == t.nFlags && hSoul == t.hLink && pSettings == t.pSettings && nGuid == t.nGuid)
		return;

	const bool bTitanChanged = !bCreated && (bTitan ? 1 : 0) != t.nFlags;
	t.nFlags = bTitan ? 1 : 0;
	t.hLink = hSoul;
	t.pSettings = pSettings;
	t.nGuid = nGuid;
	// A pilot with no soul is the normal state; print a player once it is or was a titan, or links a soul.
	if (bCreated && !bTitan && hSoul == 0xFFFFFFFFu)
		return;
	if (TitanDiag_CanLog())
	{
		char szName[96];
		TitanDiag_CopyName(szName, sizeof(szName), nGuid);
		const char* const pszSoul = TitanDiag_HandleClass(hSoul);
		Msg(eDLL_T::SERVER, "[TITAN] sv player ent=%d ptr=%p titan=%d%s m_titanSoul=%08X->%s classSettings=%p settings=%016llX asset='%s'\n",
			t.nIndex, pEnt, bTitan ? 1 : 0, bTitanChanged ? " (class changed)" : "", hSoul,
			pszSoul ? pszSoul : "(none)", pSettings, static_cast<unsigned long long>(nGuid),
			szName[0] ? szName : "(not in the dedi asset table)");
	}
}

static void TitanDiag_Poll(void)
{
	if (!g_serverEntityList)
		return;

	for (int i = 0; i < MAX_TRACKED; ++i)
		s_tracked[i].bSeen = false;

	int nSafety = 0;
	for (CBaseHandle handle = g_serverEntityList->FirstHandle();
		handle.IsValid() && nSafety++ < MAX_ENTITY_WALK;
		handle = g_serverEntityList->NextHandle(handle))
	{
		const CEntInfo* const pInfo = g_serverEntityList->GetEntInfoPtr(handle);
		if (!pInfo || !pInfo->m_pEntity)
			continue;

		const char* const pszClass = STRING(pInfo->m_iClassName);
		int nKind = -1;
		if (!strcmp(pszClass, "titan_soul"))
			nKind = TDK_SOUL;
		else if (!strcmp(pszClass, "npc_titan"))
			nKind = TDK_NPC;
		else if (!strcmp(pszClass, "player"))
			nKind = TDK_PLAYER;
		if (nKind < 0)
			continue;

		bool bCreated = false;
		TitanDiagEnt_t* const pSlot = TitanDiag_Slot(nKind, handle.GetEntryIndex(), pInfo->m_SerialNumber, bCreated);
		if (!pSlot)
			continue;
		pSlot->bSeen = true;

		if (nKind == TDK_SOUL)
			TitanDiag_PollSoul(*pSlot, pInfo->m_pEntity, bCreated);
		else if (nKind == TDK_NPC)
			TitanDiag_PollNpc(*pSlot, pInfo->m_pEntity, bCreated);
		else
			TitanDiag_PollPlayer(*pSlot, pInfo->m_pEntity, bCreated);
	}

	for (int i = 0; i < MAX_TRACKED; ++i)
	{
		TitanDiagEnt_t& t = s_tracked[i];
		if (!t.bUsed || t.bSeen)
			continue;
		if (t.nKind != TDK_PLAYER && TitanDiag_CanLog())
			Msg(eDLL_T::SERVER, "[TITAN] sv %s ent=%d serial=%d removed\n",
				t.nKind == TDK_SOUL ? "soul" : "npc_titan", t.nIndex, t.nSerial);
		t.bUsed = false;
	}
}

void TitanDiag_Frame(void)
{
	if (!bridge_titan_diag.GetBool())
		return;

	const double flNow = Plat_StallClockSeconds();
	if (flNow < s_flNextPoll)
		return;
	s_flNextPoll = flNow + POLL_INTERVAL;

	if (!s_bAnnounced)
	{
		s_bAnnounced = true;
		Msg(eDLL_T::SERVER, "[TITAN] sv diag armed: settings asset registry %s\n",
			g_pPakAssetTypeRegistry_S3 ? "resolved" : "UNRESOLVED (asset names not printed)");
	}
	TitanDiag_Poll();
}
