#if defined(CLIENT_DLL)
#ifndef WEAPON_SCRIPT_VARS_H
#define WEAPON_SCRIPT_VARS_H

// No client-side weapon script-var surface. The S21 client binds these natives
// itself and the SDK registration path that reached this half no longer exists.

#endif // WEAPON_SCRIPT_VARS_H

#else // !CLIENT_DLL
#ifndef WEAPON_SCRIPT_VARS_H
#define WEAPON_SCRIPT_VARS_H

#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "thirdparty/detours/include/idetour.h"

struct ScriptClassDescriptor_t;
class CSquirrelVM;

void WeaponScriptVars_RegisterWeaponFuncs(ScriptClassDescriptor_t* weaponStruct);
void WeaponScriptVars_RegisterEntityFuncs(ScriptClassDescriptor_t* entityStruct);
void WeaponScriptVars_RegisterWeaponTypeDisableFuncs(ScriptClassDescriptor_t* entityStruct);
void WeaponScriptVars_RegisterWPTConstants(CSquirrelVM* s);
void WeaponScriptVars_RegisterS21EWeaponVarAliases(CSquirrelVM* s);
float WeaponScriptVars_GetScriptFloat0(void* pWeapon);
void WeaponScriptVars_SetScriptFloat0(void* pWeapon, float value);
void WeaponScriptVars_LevelShutdown();
void WeaponScriptVars_PhaseShift_LevelShutdown();
void WeaponScriptVars_WeaponLockedSet_LevelShutdown();
// PhaseShiftBegin belongs to the base-combat-character class in both engines;
// registering it on the player class narrows it and breaks every NPC call site.
void WeaponScriptVars_RegisterPhaseShiftOverride(ScriptClassDescriptor_t* combatCharStruct);
void WeaponScriptVars_RegisterLaserSightOverride(ScriptClassDescriptor_t* playerStruct);
// Syncs laserSightColor / laserSightColorCustomized userinfo into DT_Player each usercmd.
void LaserSightColorBridge_Think(void* pPlayer);
// Offhand natives live on the combat-character descriptor (players and NPCs).
// isServerStruct=true gates Give/Take/GetOffhandWeapons; false registers GetOffhandWeapon only.
void WeaponScriptVars_RegisterOffhandOverrides(ScriptClassDescriptor_t* combatCharStruct,
                                               bool isServerStruct);
// CancelOffhandWeapon is player-class in S21, so it registers separately.
void WeaponScriptVars_RegisterOffhandPlayerOverrides(ScriptClassDescriptor_t* playerStruct,
                                                     bool isServerStruct);
void WeaponScriptVars_RegisterWeaponLockedSetSetter(ScriptClassDescriptor_t* weaponStruct);

// Wire accessors: appended DT_WeaponX slots alias live m_modVars, so state lives in sidecars.
int WeaponScriptVars_WireGetLockedSet(void* pWeapon);
int WeaponScriptVars_WireGetTargetingLaserEnabled(void* pWeapon);
int WeaponScriptVars_WireGetGenericHighlightContext(void* pEntity, int genericType);
int WeaponScriptVars_WireGetHighlightFocused(void* pEntity);
void WeaponScriptVars_RegisterInfiniteAmmoFuncs(ScriptClassDescriptor_t* weaponStruct);
void WeaponScriptVars_RegisterInfiniteAmmoSetter(ScriptClassDescriptor_t* weaponStruct);
void WeaponScriptVars_InfiniteAmmo_LevelShutdown();

// Cross-thread-safe: called from the snapshot pack value proxy (transmit
// job worker) and the server ammo detours. Sidecar scan, no locks.
int WeaponScriptVars_GetInfiniteAmmoState(const void* pWeapon);

// Per-player WPT_ disabled bitmask from the handle-keyed map. 0 if unset.
uint32_t WeaponScriptVars_GetDisabledFlagsForEntity(const void* pEntity);
void WeaponScriptVars_FlushDisableHoldFlags(void);

// Engine: CBaseEntity::SetRadius(float) - sets collision cylinder radius
inline void(*v_CBaseEntity_SetRadius)(void* entity, float radius) = nullptr;

// PhaseShiftBegin: client writes +0x1790/+0x1794, server +0x15B4/+0x15B8.
// Dispatch by VM context -- the wrong half writes an unrelated field and times stay 0.
inline void(*v_PhaseShiftBegin_Client)(void* entity, float warmup, float duration) = nullptr;
inline void(*v_PhaseShiftBegin_Server)(void* entity, float warmup, float duration) = nullptr;

// Offhand Script_* wrappers: slots 0..5 tail-call the native; 6/7 go through OffhandSlotsExt.
// Get is split by VM side; Give/Take are server-only.
inline SQRESULT(*v_Script_GetOffhandWeapon_Server)(HSQUIRRELVM v) = nullptr;
inline SQRESULT(*v_Script_GetOffhandWeapon_Client)(HSQUIRRELVM v) = nullptr;
inline SQRESULT(*v_Script_GiveOffhandWeapon_Server)(HSQUIRRELVM v) = nullptr;
inline SQRESULT(*v_Script_TakeOffhandWeapon_Server)(HSQUIRRELVM v) = nullptr;

// PushEntity is the only safe OT_ENTITY push. GetScriptInstance is an HScriptHandle,
// not an SQInstance*; a hand-rolled SQObject UAFs at pInstance+0x50.
inline void(*v_CSquirrelVM_PushEntity_Client)(HSQUIRRELVM v, void* pEntity) = nullptr;
inline void(*v_CSquirrelVM_PushEntity_Server)(HSQUIRRELVM v, void* pEntity) = nullptr;

// Spawn-only primitive (no slot-store). playerPlus1104/1116 are m_OffhandSlotScratch.
inline void*(*v_CPlayer_SpawnOffhandWeapon)(
	const char* weaponClassName, uint32_t modsBitfield,
	void* playerPlus1104, void* playerPlus1116, void* player,
	char unused_a6, char unused_a7) = nullptr;

// Mods-array -> bitfield. Mutates the VM stack; server copies only (client twins exist).
inline void*(*v_OffhandWeaponNameToDef)(const char* weaponClassName) = nullptr;
inline char(*v_OffhandComputeModsBitfield)(HSQUIRRELVM v, unsigned int argIdx,
	void* weaponDef, uint32_t* outBitfield) = nullptr;

// Post-spawn bind order: team color, owner, post-spawn (twice), owner-dirty.
// DestroyEntity takes pWeapon+0x40 (IHandleEntity).
inline void(*v_CWeapon_InitTeamColor)(void* pWeapon, uint32_t teamId) = nullptr;
inline int (*v_CWeapon_BindToOwner)(void* pWeapon, void* pOwner, char setDefaults) = nullptr;
inline void(*v_CWeapon_PostSpawnInit)(void* pWeapon) = nullptr;
inline void(*v_CWeapon_OwnerDirtyReplicate)(void* pWeapon, void* pOwner) = nullptr;
inline void(*v_CWeapon_DestroyEntity)(void* pHandleEntity) = nullptr;

// TrySelectOffhand: resolved global offhand command byte
inline uint8_t* g_pOffhandCommandByte = nullptr;

// Engine weapon-sound list. Off-path destroy leaves a stale CWeaponX* for the tick walk.
// g_pWeaponListMem is &m_pMemory; g_pWeaponListCount is &m_Size at +0x18.
inline void*** g_pWeaponListMem = nullptr;
inline int* g_pWeaponListCount = nullptr;

// Engine: CWeaponX::HolsterInternal(weapon, doFastHolster) -- S3 2-arg form.
// Backs Script_WeaponHolster / Script_WeaponFastHolster.
inline char(*v_WeaponX_HolsterInternal)(void* pWeapon, bool bDoFastHolster) = nullptr;

// Engine: CBaseCombatCharacter::Weapon_SetSelectedOffhandCleared(character, activeSlot).
// Backs the engine's own ClearOffhand script native; clears the selected offhand
// for one active inventory slot and restores the last non-offhand weapon to hand.
inline __int64(*v_Weapon_SetSelectedOffhandCleared)(void* pCharacter, unsigned int activeSlot) = nullptr;

// Expanded WeaponType enum table (adds "gadget" at index 9)
void WeaponScriptVars_PatchWeaponTypeTable();

// Adds WPT_VIEWHANDS (bit 8) and WPT_SURVIVAL (bit 9) to the engine flag-name table.
void WeaponScriptVars_PatchWeaponTypeFlagsTable();

///////////////////////////////////////////////////////////////////////////////
class VWeaponScriptVars : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CBaseEntity::SetRadius", v_CBaseEntity_SetRadius);
#ifndef DEDICATED
		LogFunAdr("PhaseShiftBegin_Client", v_PhaseShiftBegin_Client);
#endif
		LogFunAdr("PhaseShiftBegin_Server", v_PhaseShiftBegin_Server);
		LogFunAdr("Script_GetOffhandWeapon_Server",  v_Script_GetOffhandWeapon_Server);
#ifndef DEDICATED
		LogFunAdr("Script_GetOffhandWeapon_Client",  v_Script_GetOffhandWeapon_Client);
#endif
		LogFunAdr("Script_GiveOffhandWeapon_Server", v_Script_GiveOffhandWeapon_Server);
		LogFunAdr("Script_TakeOffhandWeapon_Server", v_Script_TakeOffhandWeapon_Server);
#ifndef DEDICATED
		LogFunAdr("CSquirrelVM::PushEntity_Client",  v_CSquirrelVM_PushEntity_Client);
#endif
		LogFunAdr("CSquirrelVM::PushEntity_Server",  v_CSquirrelVM_PushEntity_Server);
		LogFunAdr("CPlayer::SpawnOffhandWeapon",     v_CPlayer_SpawnOffhandWeapon);
		LogFunAdr("CWeapon::InitTeamColor",          v_CWeapon_InitTeamColor);
		LogFunAdr("CWeapon::BindToOwner",            v_CWeapon_BindToOwner);
		LogFunAdr("CWeapon::PostSpawnInit",          v_CWeapon_PostSpawnInit);
		LogFunAdr("CWeapon::OwnerDirtyReplicate",    v_CWeapon_OwnerDirtyReplicate);
		LogFunAdr("CWeapon::DestroyEntity",          v_CWeapon_DestroyEntity);
		LogFunAdr("CWeaponX::HolsterInternal",       v_WeaponX_HolsterInternal);
		LogFunAdr("Weapon_SetSelectedOffhandCleared", v_Weapon_SetSelectedOffhandCleared);
		LogVarAdr("g_pOffhandCommandByte", g_pOffhandCommandByte);
		LogVarAdr("g_pWeaponListMem", g_pWeaponListMem);
		LogVarAdr("g_pWeaponListCount", g_pWeaponListCount);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 57 48 83 EC 30 F3 0F 10 81 10 0D 00 00")
			.GetPtr(v_CBaseEntity_SetRadius);

		// Server PhaseShiftBegin: dirty-flag lock-or + store at +0x15B4.
		Module_FindPattern(g_GameDll,
			"4C 8B 05 ?? ?? ?? ?? 48 8B D1 F3 0F 10 99 B4 15 00 00 41 B9 00 02 00 00 F3 41 0F 58 48 28")
			.GetPtr(v_PhaseShiftBegin_Server);

#ifndef DEDICATED
		// Client PhaseShiftBegin: store at +0x1790. Dedi does not resolve this.
		Module_FindPattern(g_GameDll,
			"48 8B 05 ?? ?? ?? ?? F3 0F 58 48 28 F3 0F 11 89 90 17 00 00")
			.GetPtr(v_PhaseShiftBegin_Client);
#endif

		// Keep the trailing rel32: the shared prefix matches both VM-side twins.
		Module_FindPattern(g_GameDll,
			"40 53 48 83 EC 20 48 8D 54 24 38 48 8B D9 E8 ?? ?? ?? ?? 84 C0 74 53 48 8B 43 58 81 78 10 02 00 00 05 75 05 8B 50 18 EB 05 F3 0F 2C 50 18 48 8B 4C 24 38 E8 38 3D E3 FF")
			.GetPtr(v_Script_GetOffhandWeapon_Server);
#ifndef DEDICATED
		Module_FindPattern(g_GameDll,
			"40 53 48 83 EC 20 48 8D 54 24 38 48 8B D9 E8 ?? ?? ?? ?? 84 C0 74 53 48 8B 43 58 81 78 10 02 00 00 05 75 05 8B 50 18 EB 05 F3 0F 2C 50 18 48 8B 4C 24 38 E8 D8 19 12 00")
			.GetPtr(v_Script_GetOffhandWeapon_Client);
#endif

		// PushEntity is the E8 at GetOffhandWeapon+0x3E (same offset on both sides).
#ifndef DEDICATED
		if (v_Script_GetOffhandWeapon_Client)
		{
			CMemory callSite(reinterpret_cast<uintptr_t>(
				v_Script_GetOffhandWeapon_Client) + 0x3E);
			v_CSquirrelVM_PushEntity_Client =
				callSite.FollowNearCallSelf().RCast<
					void(*)(HSQUIRRELVM, void*)>();
		}
#endif
		if (v_Script_GetOffhandWeapon_Server)
		{
			CMemory callSite(reinterpret_cast<uintptr_t>(
				v_Script_GetOffhandWeapon_Server) + 0x3E);
			v_CSquirrelVM_PushEntity_Server =
				callSite.FollowNearCallSelf().RCast<
					void(*)(HSQUIRRELVM, void*)>();
		}

		// Give is the 1-arg VM wrapper, not the 2-arg inner (that expects player in rcx).
		// Trailing rel32 E8 BC 91 FC FF separates it from Get/Take.
		Module_FindPattern(g_GameDll,
			"40 53 48 83 EC 20 48 8D 54 24 38 48 8B D9 E8 ?? ?? ?? ?? 84 C0 74 29 48 8B 4C 24 38 48 8B D3 E8 BC 91 FC FF")
			.GetPtr(v_Script_GiveOffhandWeapon_Server);

		// Take: jz 74 45 vs Get's 74 53; keep trailing E8 E8 A3 FB FF or it collides with Get.
		Module_FindPattern(g_GameDll,
			"40 53 48 83 EC 20 48 8D 54 24 38 48 8B D9 E8 ?? ?? ?? ?? 84 C0 74 45 48 8B 43 58 81 78 10 02 00 00 05 75 05 8B 50 18 EB 05 F3 0F 2C 50 18 48 8B 4C 24 38 E8 E8 A3 FB FF")
			.GetPtr(v_Script_TakeOffhandWeapon_Server);

		// CPlayer::SpawnOffhandWeapon -- isolated spawn primitive extracted
		// from the Give path. 0x80-byte stack frame with the canonical
		// 5-spill + 2-char-arg prologue. See header decl for full signature.
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 41 57 48 81 EC 80 00 00 00 4D 8B F9")
			.GetPtr(v_CPlayer_SpawnOffhandWeapon);

		// Post-spawn helpers between spawn and slot-store (mirrored for slots 6/7).
		Module_FindPattern(g_GameDll,
			"48 89 74 24 20 57 48 83 EC 40 8B 81 94 05 00 00 8B F2 48 8B F9 3B C2 0F 84 38 01 00 00")
			.GetPtr(v_CWeapon_InitTeamColor);
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 20 45 33 E4 41 0F B6 E8 4C 8B F2")
			.GetPtr(v_CWeapon_BindToOwner);
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 20 41 56 48 83 EC 20 44 8B 81 C8 1A 00 00 48 8B D9 48 89 74 24 38 8B B1 D0 1A 00 00 48 89 7C 24 40 33 FF 85")
			.GetPtr(v_CWeapon_PostSpawnInit);
		Module_FindPattern(g_GameDll,
			"48 8B C4 48 89 58 10 57 48 81 EC A0 00 00 00 0F 29 70 E8 48 8B D9 48 89 70 08 48 8B CA 48 8B 02 48 8B F2")
			.GetPtr(v_CWeapon_OwnerDirtyReplicate);
		Module_FindPattern(g_GameDll,
			"48 85 C9 0F 84 4A 01 00 00 57 48 83 EC 20 4C 8B 49 08 48 8B F9 48 8B 05")
			.GetPtr(v_CWeapon_DestroyEntity);

		// Server mods helpers; trailing operand separates them from the client twins.
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 48 85 C9 74 7E 0F B7 1D D0 E2 71 01 BD FF FF 00 00")
			.GetPtr(v_OffhandWeaponNameToDef);
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 4C 89 4C 24 20 57 41 54 41 55 41 56 41 57 48 83 EC 40 33 F6 4D 8B E8 44 8B FE 44 8B E2 48 8B D9 E8 1C C0 3E 00")
			.GetPtr(v_OffhandComputeModsBitfield);

		// CWeaponX::HolsterInternal(weapon, doFastHolster).
		// S3's 2-arg variant (no fastHolsterScale; the twin is 3-arg).
		// Prologue reads owner handle at weapon+0x11F0. Verified unique (1 hit)

		Module_FindPattern(g_GameDll,
			"48 89 5C 24 10 48 89 6C 24 18 56 48 83 EC 30 48 8B D9 0F B6 F2 8B 89 F0 11 00 00 8B C1 83 F9 FF")
			.GetPtr(v_WeaponX_HolsterInternal);
		if (!v_WeaponX_HolsterInternal)
			Warning(eDLL_T::SERVER, "[WeaponScriptVars] CWeaponX::HolsterInternal pattern unresolved -- weapon.Holster()/FastHolster() script bindings will no-op\n");

		// C half: 48 63 DA and vtable+0xA60. C_ half is 8B DA / +0x850 -- do not attach that.
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 48 8B 01 48 8B F9 48 63 DA 48 8D 34 9D ?? ?? ?? ?? FF 90 60 0A 00 00")
			.GetPtr(v_Weapon_SetSelectedOffhandCleared);
		if (!v_Weapon_SetSelectedOffhandCleared)
			Warning(eDLL_T::SERVER, "[WeaponScriptVars] Weapon_SetSelectedOffhandCleared pattern unresolved -- player.CancelOffhandWeapon() will holster only\n");
	}
	virtual void GetVar(void) const
	{
		// TrySelectOffhand: resolve offhand command byte from ActivateOffhandWeaponByIndex
		CMemory wrapper = Module_FindPattern(g_GameDll,
			"83 FA ?? 77 ?? 48 8B 05 ?? ?? ?? ?? 48 8D 0D");
		if (wrapper)
		{
			CMemory leaInstr = wrapper.Offset(12);
			uint8_t* pGlobal = reinterpret_cast<uint8_t*>(
				leaInstr.ResolveRelativeAddress(3, 7).GetPtr());
			if (pGlobal)
				g_pOffhandCommandByte = pGlobal + 0x180;
		}

		// Weapon-list CUtlVector: mem and count must be 0x18 apart.
		CMemory listBlock = Module_FindPattern(g_GameDll,
			"83 7B 08 FF 74 ?? 8B 15 ?? ?? ?? ?? 8B CF 85 D2 7E ?? 4C 8B 05 ?? ?? ?? ?? "
			"49 8B C0 48 39 18 74 ?? FF C1 48 83 C0 08 3B CA 7C F1");
		if (listBlock)
		{
			void*** const pMem = listBlock.Offset(18).ResolveRelativeAddress(3, 7).RCast<void***>();
			int* const pCount = listBlock.Offset(6).ResolveRelativeAddress(2, 6).RCast<int*>();

			if (pMem && pCount && reinterpret_cast<uintptr_t>(pCount) -
				reinterpret_cast<uintptr_t>(pMem) == 0x18)
			{
				g_pWeaponListMem = pMem;
				g_pWeaponListCount = pCount;
			}
			else
			{
				Warning(eDLL_T::SERVER, "[WeaponScriptVars] weapon-list globals failed the "
					"CUtlVector layout check (mem=%p count=%p) -- extended offhand slots "
					"will refuse to destroy their weapons.\n",
					reinterpret_cast<void*>(pMem), reinterpret_cast<void*>(pCount));
			}
		}
		else
		{
			Warning(eDLL_T::SERVER, "[WeaponScriptVars] weapon-list pattern unresolved -- "
				"extended offhand slots will refuse to destroy their weapons.\n");
		}
	}
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const
	{
		if (bAttach)
		{
			WeaponScriptVars_PatchWeaponTypeTable();
			WeaponScriptVars_PatchWeaponTypeFlagsTable();
		}
	}
};
///////////////////////////////////////////////////////////////////////////////

#endif // WEAPON_SCRIPT_VARS_H
#endif // CLIENT_DLL
