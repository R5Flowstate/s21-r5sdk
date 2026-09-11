//=============================================================================//
//
// Purpose: Client stub for CBaseEntity::GetScriptInstance
//
//=============================================================================//
#include "core/stdafx.h"
#if defined(CLIENT_DLL)
#include "game/server/baseentity.h"
const HSCRIPT CBaseEntity::GetScriptInstance()
{
	return nullptr;
}
#endif
