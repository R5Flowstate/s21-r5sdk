//=============================================================================//
//
// Purpose: Dest-map loadscreen on first connect; defer dest world paks until
// the loadscreen has presented. Strip default-slot lobby paks on +connect.
//
//=============================================================================//
#ifndef PAK_LOBBY_WORLD_S21_H
#define PAK_LOBBY_WORLD_S21_H

#include "thirdparty/detours/include/idetour.h"

class VPakLobbyWorldS21 : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

bool PakLobby_DirectMapLoad(void);
bool PakLobby_InPakWait(void);
void PakLobby_OnPollReceive(void);
void PakLobby_OnSessionReset(void);

#endif // PAK_LOBBY_WORLD_S21_H
