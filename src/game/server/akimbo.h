#ifndef AKIMBO_BRIDGE_H
#define AKIMBO_BRIDGE_H

//=============================================================================//
//
// Purpose: Akimbo (dual pistol) dedi authors for S21 client props / toggle
//
//=============================================================================//

#ifndef CLIENT_DLL

#include "thirdparty/detours/include/idetour.h"

struct ScriptClassDescriptor_t;

enum AkimboState_e { AKIMBO_STATE_NONE = 0, AKIMBO_STATE_SINGLE = 1, AKIMBO_STATE_OFFHAND = 2, AKIMBO_STATE_ACTIVE = 3 };

bool AkimboBridge_IsAkimboWeapon(void* pWeapon);
void* AkimboBridge_GetOtherWeapon(void* pWeapon);
bool AkimboBridge_IsAlthand(void* pWeapon);
bool AkimboBridge_IsDisabled(void* pWeapon);
void AkimboBridge_UpdateState(void* pPlayer);

bool AkimboBridge_CanActivateAlthand(void* pPlayer, void* pWeapon);
void AkimboBridge_OnSetActiveWeapon(void* pPlayer, unsigned int hand, void* pWeapon);

void AkimboBridge_Think(void* pPlayer, void* pUserCmd);
void AkimboBridge_RegisterWeaponFuncs(ScriptClassDescriptor_t* weaponStruct);
void AkimboBridge_RegisterPlayerFuncs(ScriptClassDescriptor_t* playerStruct);
void AkimboBridge_LevelShutdown();

///////////////////////////////////////////////////////////////////////////////
class VAkimboBridge : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const;
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // !CLIENT_DLL

#endif // AKIMBO_BRIDGE_H
