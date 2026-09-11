#ifndef CONTEXT_ACTION_H
#define CONTEXT_ACTION_H

//=============================================================================//
//
// Purpose: S21 ContextAction_Set/Clear/IsEmoting on S3 CBaseCombatCharacter.
// Writes the existing m_contextAction sendprop; no layout growth.
//
//=============================================================================//

struct ScriptClassDescriptor_t;

void ContextAction_RegisterScriptFunctions(ScriptClassDescriptor_t* bccStruct);

#endif // CONTEXT_ACTION_H
