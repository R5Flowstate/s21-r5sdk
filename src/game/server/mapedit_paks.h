//=============================================================================//
//
// Purpose: DEDI extra map-pack natives for the map editor. Loads another
// map's server pak so its props can be precached and spawned mid-level.
//
//=============================================================================//
#ifndef MAPEDIT_PAKS_H
#define MAPEDIT_PAKS_H

#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/idetour.h"

class CSquirrelVM;

void MapEditPaks_RegisterServerFunctions(CSquirrelVM* pVM);
void MapEditPaks_LevelShutdown(void);

// Engine helper that lifts the post-load precache lock around one PrecacheModel.
inline char (*v_Server_PrecacheModelLate)(const char* pszModel) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VMapEditPaks : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("Server_PrecacheModelLate", v_Server_PrecacheModelLate);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const { }
};
///////////////////////////////////////////////////////////////////////////////

#endif // MAPEDIT_PAKS_H
