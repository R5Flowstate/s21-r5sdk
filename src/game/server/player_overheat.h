#ifndef PLAYER_OVERHEAT_H
#define PLAYER_OVERHEAT_H

//=============================================================================//
//
// Purpose: S3 native gap-fills for ballistic player overheat. Backed by
// bridge-owned appended DT_BaseCombatCharacter props with no engine writer --
// pure SDK state that scripts own end-to-end.
//
//=============================================================================//

struct ScriptClassDescriptor_t;

void PlayerOverheat_RegisterCombatCharacterFuncs(ScriptClassDescriptor_t* combatCharStruct);
void PlayerOverheat_RegisterWeaponFuncs(ScriptClassDescriptor_t* weaponStruct);

// Per-shot accumulate + script overheat event when the meter tops out.
// Driven from Hook_CWeaponX_PrimaryAttack in weapon_heat.cpp.
void PlayerOverheat_OnWeaponFired(void* pWeapon);

// No per-player cache beyond the weapon-config map (class-name keyed, not
// entity-keyed). No-op unless that shape changes.
void PlayerOverheat_LevelShutdown(void);

#endif // PLAYER_OVERHEAT_H
