//=============================================================================//
//
// Purpose: Non-physics script mover traversal (Newcastle Mobile Shield) on the
// dedicated server: the S21 script API for a mover that drives itself over
// ground toward a goal, stepping over, sliding along and stopping at ledges.
//
//=============================================================================//
#ifndef SCRIPT_MOVER_TRAVERSAL_H
#define SCRIPT_MOVER_TRAVERSAL_H

#include "thirdparty/detours/include/idetour.h"

// Advances every mover in traversal mode by one tick; called before the
// engine's entity think so the issued segment plays this frame.
void ScriptMoverTraversal_Frame(void);
void ScriptMoverTraversal_LevelShutdown(void);

///////////////////////////////////////////////////////////////////////////////
class VScriptMoverTraversal : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // SCRIPT_MOVER_TRAVERSAL_H
