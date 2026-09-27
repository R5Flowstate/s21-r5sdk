//=============================================================================//
//
// Purpose: realm hygiene for script-built entities and world effects.
//
// The base entity constructor places every entity in all realms. Scripts are
// expected to narrow that, and many ability paths never do, so a deployable
// built mid-fight shows up in every other realm. An entity still in all
// realms when a script gives it an owner or a parent adopts that entity's
// realms; an explicit script realm always wins because it is no longer the
// all-realms default.
//
// World-space temp entities (StartParticleEffectInWorld) go to every player.
// StartParticleEffectInWorldForRealms arms a realm mask that the shared temp
// entity suppress check applies to the recipient list.
//
//=============================================================================//
#include "core/stdafx.h"


#include "tier1/cvar.h"
#include "tier0/memaddr.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "game/server/util_server.h"
#include "game/server/vscript_server_natives.h"
#include "realm_adopt.h"
#include "weapon_realm_follow.h"

static constexpr ptrdiff_t RA_ENT_OFF_REALMSBITMASK = 0xAE8; // u64 m_realmsBitMask
static constexpr ptrdiff_t RA_ENT_OFF_EDICTINDEX    = 88;    // u16 edict index word
static constexpr uint64_t  RA_ALL_REALMS            = ~0ull; // base constructor default

// CRecipientFilter: recipient vector data at +0x10, count at +0x28, 8-byte
// entries whose first int is the player's entity index.
static constexpr ptrdiff_t RA_FILTER_OFF_RECIPIENTS = 0x10;
static constexpr ptrdiff_t RA_FILTER_OFF_COUNT      = 0x28;

static ConVar bridge_realm_adopt("bridge_realm_adopt", "1", FCVAR_RELEASE,
	"An entity still in all realms adopts the realms of the entity a script "
	"makes its owner (SetOwner) or parent (SetParent)");

static ConVar bridge_realm_fx_filter("bridge_realm_fx_filter", "1", FCVAR_RELEASE,
	"StartParticleEffectInWorldForRealms only reaches players sharing a realm "
	"with its realm entity");

static ConVar bridge_realm_adopt_diag("bridge_realm_adopt_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Log realm adoptions and filtered world effects ([REALM-ADOPT], rate limited)");

static SQRESULT (*v_Script_SetOwner)(HSQUIRRELVM v) = nullptr;
static SQRESULT (*v_Script_SetParent)(HSQUIRRELVM v) = nullptr;
static bool (*v_TempEnt_SuppressEvents)(void* pSystem, void* pFilter) = nullptr;

static thread_local uint64_t s_armedTempEntMask = 0;

uint64_t RealmAdopt_GetRealmsBitMask(const void* const pEntity)
{
	if (!pEntity)
		return 0;
	return *reinterpret_cast<const uint64_t*>(
		reinterpret_cast<uintptr_t>(pEntity) + RA_ENT_OFF_REALMSBITMASK);
}

static bool RealmAdopt_IsPlayer(void* const pEntity)
{
	const int16_t edictIdx = *reinterpret_cast<const int16_t*>(
		reinterpret_cast<uintptr_t>(pEntity) + RA_ENT_OFF_EDICTINDEX);
	return edictIdx >= 1 && UTIL_PlayerByIndex(edictIdx) == pEntity;
}

//-----------------------------------------------------------------------------
// Purpose: move an all-realms entity into the realms of source.
//-----------------------------------------------------------------------------
static void RealmAdopt_FromSource(void* const pEntity, void* const pSource, const char* const pszVia)
{
	if (!pEntity || !pSource || pEntity == pSource || !bridge_realm_adopt.GetBool())
		return;

	if (RealmAdopt_GetRealmsBitMask(pEntity) != RA_ALL_REALMS)
		return;

	const uint64_t sourceMask = RealmAdopt_GetRealmsBitMask(pSource);
	if (sourceMask == 0 || sourceMask == RA_ALL_REALMS)
		return;

	if (RealmAdopt_IsPlayer(pEntity))
		return;

	WeaponRealmFollow_SetRealmsBitMask(reinterpret_cast<__int64>(pEntity), sourceMask);

	if (bridge_realm_adopt_diag.GetBool())
	{
		static int s_nLogged = 0;
		if (s_nLogged++ < 64 || (s_nLogged % 256) == 0)
			Msg(eDLL_T::SERVER, "[REALM-ADOPT] %s ent=%p realms 0x%llX (n=%d)\n", pszVia, pEntity,
				static_cast<unsigned long long>(sourceMask), s_nLogged);
	}
}

static SQRESULT Hook_Script_SetOwner(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		pEntity = nullptr;
	void* const pOwner = ServerScript_EntityPtrFromStackIdx(v, 2);

	const SQRESULT result = v_Script_SetOwner(v);
	if (result != SQ_ERROR)
		RealmAdopt_FromSource(pEntity, pOwner, "owner");

	return result;
}

static SQRESULT Hook_Script_SetParent(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		pEntity = nullptr;
	void* const pParent = ServerScript_EntityPtrFromStackIdx(v, 2);

	const SQRESULT result = v_Script_SetParent(v);
	if (result != SQ_ERROR)
		RealmAdopt_FromSource(pEntity, pParent, "parent");

	return result;
}

void RealmAdopt_ArmTempEntRealms(const uint64_t mask)
{
	s_armedTempEntMask = bridge_realm_fx_filter.GetBool() ? mask : 0;
}

//-----------------------------------------------------------------------------
// Purpose: every temp entity passes through here with its finished recipient
// list; drop the players an armed realm mask excludes.
//-----------------------------------------------------------------------------
static bool Hook_TempEnt_SuppressEvents(void* const pSystem, void* const pFilter)
{
	const uint64_t mask = s_armedTempEntMask;
	s_armedTempEntMask = 0;

	if (mask && pFilter)
	{
		const uintptr_t filter = reinterpret_cast<uintptr_t>(pFilter);
		uint64_t* const pEntries = *reinterpret_cast<uint64_t**>(filter + RA_FILTER_OFF_RECIPIENTS);
		int* const pCount = reinterpret_cast<int*>(filter + RA_FILTER_OFF_COUNT);

		if (pEntries && *pCount > 0)
		{
			const int nCount = *pCount;
			int nKept = 0;
			for (int i = 0; i < nCount; ++i)
			{
				const int entIndex = *reinterpret_cast<const int*>(&pEntries[i]);
				void* const pPlayer = UTIL_PlayerByIndex(entIndex);
				if (pPlayer && (RealmAdopt_GetRealmsBitMask(pPlayer) & mask) == 0)
					continue;
				pEntries[nKept++] = pEntries[i];
			}
			*pCount = nKept;

			if (bridge_realm_adopt_diag.GetBool() && nKept != nCount)
			{
				static int s_nLogged = 0;
				if (s_nLogged++ < 64 || (s_nLogged % 256) == 0)
					Msg(eDLL_T::SERVER, "[REALM-ADOPT] world fx realms 0x%llX: %d of %d recipients kept\n",
						static_cast<unsigned long long>(mask), nKept, nCount);
			}
		}
	}

	return v_TempEnt_SuppressEvents(pSystem, pFilter);
}

//-----------------------------------------------------------------------------
// Purpose: resolve a script binding from its registration. The server class
// registrar stores the binding with the first `lea reg, <.text>` after the
// lea of its name string.
//-----------------------------------------------------------------------------
static void* RealmAdopt_BindingAfterNameLea(const CMemory nameLea)
{
	const CModule::ModuleSections_t* const pText = g_GameDll.FindSectionByName(".text");
	if (!nameLea || !pText)
		return nullptr;

	const uintptr_t textBase = pText->m_pSectionBase;
	const uintptr_t textEnd = textBase + pText->m_nSectionSize;

	for (ptrdiff_t off = 7; off < 0x30; ++off)
	{
		const CMemory at = nameLea.Offset(off);
		const uint8_t* const p = reinterpret_cast<const uint8_t*>(at.GetPtr());
		if (p[0] != 0x48 || p[1] != 0x8D || (p[2] != 0x05 && p[2] != 0x0D))
			continue;

		const uintptr_t target = at.ResolveRelativeAddress(0x3, 0x7).GetPtr();
		if (target >= textBase && target < textEnd)
			return reinterpret_cast<void*>(target);
	}
	return nullptr;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void VRealmAdopt::GetAdr(void) const
{
	LogFunAdr("Script_SetOwner", v_Script_SetOwner);
	LogFunAdr("Script_SetParent", v_Script_SetParent);
	LogFunAdr("TempEnt_SuppressEvents", v_TempEnt_SuppressEvents);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void VRealmAdopt::GetFun(void) const
{
	// SetOwner's binding and its registration name are unique to the server
	// class; the client twin registers no such name.
	const CMemory ownerLea = g_GameDll.FindString("SetScriptOwnerEntity", 1, true);
	v_Script_SetOwner = reinterpret_cast<SQRESULT (*)(HSQUIRRELVM)>(RealmAdopt_BindingAfterNameLea(ownerLea));

	// "ScriptSetParent" is registered by both twins; the server one sits in
	// the same registrar as SetScriptOwnerEntity.
	if (ownerLea)
	{
		CMemory bestLea;
		ptrdiff_t bestDist = PTRDIFF_MAX;
		for (ptrdiff_t n = 1; n <= 4; ++n)
		{
			const CMemory lea = g_GameDll.FindString("ScriptSetParent", n, true);
			if (!lea)
				break;
			const ptrdiff_t dist = std::abs(static_cast<ptrdiff_t>(lea.GetPtr() - ownerLea.GetPtr()));
			if (dist < bestDist)
			{
				bestDist = dist;
				bestLea = lea;
			}
		}
		if (bestLea && bestDist < 0x4000)
			v_Script_SetParent = reinterpret_cast<SQRESULT (*)(HSQUIRRELVM)>(RealmAdopt_BindingAfterNameLea(bestLea));
	}

	// Recipient-filter suppress check every temp entity runs right before
	// dispatch: applies prediction rules, then suppresses an empty filter.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 83 79 24 00 48 8B DA 7F ?? 48 8B 51 18 48 85 D2 74 ?? "
		"80 7B 31 00 75 ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B 03 48 8B CB FF 50 20")
		.GetPtr(v_TempEnt_SuppressEvents);

	if (!v_Script_SetOwner || !v_Script_SetParent)
		Warning(eDLL_T::SERVER, "[REALM-ADOPT] SetOwner/SetParent binding unresolved -- "
			"script entities keep the all-realms default\n");
	if (!v_TempEnt_SuppressEvents)
		Warning(eDLL_T::SERVER, "[REALM-ADOPT] temp entity suppress check unresolved -- "
			"world effects reach every realm\n");
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void VRealmAdopt::Detour(const bool bAttach) const
{
	if (v_Script_SetOwner)
		DetourSetup(&v_Script_SetOwner, &Hook_Script_SetOwner, bAttach);
	if (v_Script_SetParent)
		DetourSetup(&v_Script_SetParent, &Hook_Script_SetParent, bAttach);
	if (v_TempEnt_SuppressEvents)
		DetourSetup(&v_TempEnt_SuppressEvents, &Hook_TempEnt_SuppressEvents, bAttach);
}
