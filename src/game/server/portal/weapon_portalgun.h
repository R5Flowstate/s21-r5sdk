//=============================================================================//
//
// Purpose: Portal gun server natives and gun state. Placement itself lives in
// portal_placement; the networked gun props from the reference are kept in a
// server-only sidecar (no S21 DT_WeaponX RecvProp exists for them).
//
//=============================================================================//
#ifndef WEAPON_PORTALGUN_H
#define WEAPON_PORTALGUN_H

class CSquirrelVM;

struct ScriptClassDescriptor_t;
void PortalGun_RegisterWeaponFuncs(ScriptClassDescriptor_t* weaponStruct);
void PortalGun_LevelShutdown(void);
// Death sweep, called from the existing player usercmd hook.
void Portal_OnPlayerRunCommand(void* pPlayer);

#endif // WEAPON_PORTALGUN_H
