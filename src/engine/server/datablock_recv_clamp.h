//=============================================================================//
//
// Purpose: Clamp ClientDataBlockReceiver::ProcessDataBlock against the
// fixed 0xC0004 scratch so a wire transferSize cannot drive an OOB write.
//
//=============================================================================//
#ifndef ENGINE_SERVER_DATABLOCK_RECV_CLAMP_H
#define ENGINE_SERVER_DATABLOCK_RECV_CLAMP_H

#include "thirdparty/detours/include/idetour.h"

class VDataBlockRecvClamp : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // ENGINE_SERVER_DATABLOCK_RECV_CLAMP_H
