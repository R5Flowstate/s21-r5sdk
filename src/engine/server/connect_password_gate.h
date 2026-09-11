//=============================================================================//
//
// Purpose: Challenge-bind the C2S_CONNECT password tag, then restore the static tag.
//
//=============================================================================//
#ifndef ENGINE_CONNECT_PASSWORD_GATE_H
#define ENGINE_CONNECT_PASSWORD_GATE_H
#include "thirdparty/detours/include/idetour.h"

inline int64_t(*v_CServer_ConnectClient)(int64_t a1, int64_t a2) = nullptr;

// Rewrites the incoming password tag to the challenge-bound form in place.
// Called from CServer::ConnectClient. VServer owns the only ConnectClient
// attach; this class resolves the same target for verification only.
void ConnectPasswordGate_FilterTag(void* pChallenge);

///////////////////////////////////////////////////////////////////////////////
class VConnectPasswordGate : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CServer::ConnectClient", v_CServer_ConnectClient);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // !ENGINE_CONNECT_PASSWORD_GATE_H
