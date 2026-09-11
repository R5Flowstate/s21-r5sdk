//=============================================================================//
//
// Purpose: Cafe dedicated object-placement / weapon-class natives
//
//=============================================================================//
#ifndef VSCRIPT_SERVER_PLACEMENT_H
#define VSCRIPT_SERVER_PLACEMENT_H

#include "vscript/languages/squirrel_re/include/squirrel.h"

class CSquirrelVM;
class CPlayer;
class Vector3D;
struct ScriptClassDescriptor_t;

void Script_RegisterDedicatedWeaponNatives(void);
void Script_UpdateDedicatedPlayerDecoySignature(void);
void ServerScript_UpdateHeldObjectPlacement(CPlayer* player, int commandNumber = 0);
int ServerScript_ClassifyPortalDir(const Vector3D& normal);
bool ServerScript_TraceForMoverBlocking(const Vector3D& portalExitPos,
	const Vector3D& surfaceNormal, CPlayer* owner);

SQRESULT Script_IsMoverOrChildOfMover(HSQUIRRELVM v);
SQRESULT Script_SetEnableScriptAnimModifier(HSQUIRRELVM v);
SQRESULT Script_SetTier(HSQUIRRELVM v);
SQRESULT Script_SetHasVaultKey(HSQUIRRELVM v);
SQRESULT Script_SetLootGrabDist(HSQUIRRELVM v);
SQRESULT Script_GetLootGrabDist(HSQUIRRELVM v);
SQRESULT Script_SetIsVendingMachine(HSQUIRRELVM v);
SQRESULT Script_IsVendingMachine(HSQUIRRELVM v);
SQRESULT Script_IsLinkedBox(HSQUIRRELVM v);
SQRESULT Script_IncrementPlayersGrabbingLoot(HSQUIRRELVM v);
SQRESULT Script_DecrementPlayersGrabbingLoot(HSQUIRRELVM v);
SQRESULT Script_SetImpactEffectColorID(HSQUIRRELVM v);
SQRESULT Script_SetLootIndex(HSQUIRRELVM v);
SQRESULT Script_GetLootIndex(HSQUIRRELVM v);
SQRESULT Script_SetAreContentsTaken(HSQUIRRELVM v);
SQRESULT Script_GetAreContentsTaken(HSQUIRRELVM v);

#endif // VSCRIPT_SERVER_PLACEMENT_H
