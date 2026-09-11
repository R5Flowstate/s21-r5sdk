//=============================================================================//
//
// Purpose: Observation hook on CClient::UpdateAcknowledgedFramecount.
//
//=============================================================================//
#ifndef ENGINE_SERVER_SNAPSHOT_ACK_DIAG_H
#define ENGINE_SERVER_SNAPSHOT_ACK_DIAG_H

#include "thirdparty/detours/include/idetour.h"

inline char (*v_CClient_UpdateAckFramecount)(int64_t, int, int) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VAckDiag : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CClient::UpdateAcknowledgedFramecount", v_CClient_UpdateAckFramecount);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // ENGINE_SERVER_SNAPSHOT_ACK_DIAG_H
