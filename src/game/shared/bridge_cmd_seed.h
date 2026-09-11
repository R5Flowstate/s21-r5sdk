//=============================================================================//
//
// Purpose: cross-build per-usercmd prediction-RNG seed parity (dedi side).
// Both sides re-derive from command_number via MD5_PseudoRandom & 0x7FFFFFFF.
//
//=============================================================================//
#ifndef BRIDGE_CMD_SEED_H
#define BRIDGE_CMD_SEED_H
#ifdef _WIN32
#pragma once
#endif

#include "thirdparty/detours/include/idetour.h"

uint32_t BridgeSeed_FromCommandNumber(const uint32_t commandNumber);

///////////////////////////////////////////////////////////////////////////////
class VBridgeCmdSeed : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const;
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // BRIDGE_CMD_SEED_H
