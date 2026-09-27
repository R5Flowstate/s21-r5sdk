//=============================================================================//
//
// Purpose: Drag revive (Newcastle passive) natives on the dedicated server.
// The S21 client animates the drag from the three networked fields these set.
//
//=============================================================================//
#ifndef DRAG_REVIVE_H
#define DRAG_REVIVE_H

struct ScriptClassDescriptor_t;

void DragRevive_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct);

#endif // DRAG_REVIVE_H
