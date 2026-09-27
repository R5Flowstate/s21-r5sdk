//=============================================================================//
//
// Purpose: Instant stand / crouch on the server CPlayer, the way the S21
// client's leap and drag-revive natives switch stance within one command.
//
//=============================================================================//
#ifndef PLAYER_STANCE_H
#define PLAYER_STANCE_H

#include "thirdparty/detours/include/idetour.h"

// Stance flag, duck state, eye height, hull height and collision bounds.
void PlayerStance_SetInstant(void* pPlayer, bool bDucked);

///////////////////////////////////////////////////////////////////////////////
class VPlayerStance : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const { }
};
///////////////////////////////////////////////////////////////////////////////

#endif // PLAYER_STANCE_H
