//=============================================================================//
//
// Purpose: S21 server entity/player script natives the S3 dedicated server
// lacks: never-crush movers, velocity at a point, Dissolve with optional
// arguments, local gravity, and the bleedout bookkeeping natives.
//
//=============================================================================//
#ifndef ENTITY_SCRIPT_EXT_H
#define ENTITY_SCRIPT_EXT_H

#include "thirdparty/detours/include/idetour.h"
#include "mathlib/vector.h"

struct ScriptClassDescriptor_t;
class CSquirrelVM;

void EntityScriptExt_RegisterEntityFunctions(ScriptClassDescriptor_t* entityStruct);
void EntityScriptExt_RegisterPlayerFunctions(ScriptClassDescriptor_t* playerStruct);
void EntityScriptExt_RegisterServerFunctions(CSquirrelVM* s);
void EntityScriptExt_RegisterScriptConstants(CSquirrelVM* s);

// Renames every engine binding of pszName on the descriptor so a later
// AddFunction under that name is the one the script compiler sees.
void EntityScriptExt_RenameBinding(ScriptClassDescriptor_t* pDesc, const char* pszName, const char* pszNewName);

// Linear velocity plus the rotation of the entity and its move parents, at a world point.
Vector3D EntityScriptExt_AbsVelocityAtPoint(void* pEntity, const Vector3D& point);

///////////////////////////////////////////////////////////////////////////////
class VEntityScriptExt : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // ENTITY_SCRIPT_EXT_H
