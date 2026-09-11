//=============================================================================//
//
// Purpose: Reject UDP split fragments whose index*splitSize write would
// miss the reassembly buffer before native reassembly copies.
//
//=============================================================================//
#ifndef ENGINE_SPLITPACKET_RECV_CLAMP_H
#define ENGINE_SPLITPACKET_RECV_CLAMP_H

#include "thirdparty/detours/include/idetour.h"

bool SplitPacketRecvClamp_Enabled(void);
bool SplitPacket_WireWouldOOB(const unsigned char* p, int n);
void SplitPacketRecvClamp_Drop(const char* why, int a, int b);

class VSplitPacketRecvClamp : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // ENGINE_SPLITPACKET_RECV_CLAMP_H
