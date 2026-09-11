//====== Copyright © 1996-2005, Valve Corporation, All rights reserved. =======//
//
// Purpose
//
// $NoKeywords: $
//=============================================================================//
#include "core/stdafx.h"
#include "movehelper_server.h"

//-----------------------------------------------------------------------------
// Purpose: Gets the server movehelper
//-----------------------------------------------------------------------------
IMoveHelper* MoveHelperServer()
{
	return s_MoveHelperServer;
}

CMoveHelperServer* s_MoveHelperServer = nullptr;

// ProcessImpacts reads host at +8 with no null check. The helper is a process-wide
// singleton; PhysicsSimulate / a nested mini-RunCommand both SetHost(NULL) on it,
// so a bot RunNullCommand can reach ProcessImpacts with host already cleared.
static constexpr ptrdiff_t MOVEHELPER_OFF_HOST = 8;

static void Hook_CMoveHelperServer_ProcessImpacts(IMoveHelper* thisp)
{
	if (!thisp)
		return;

	IHandleEntity* const pHost = *reinterpret_cast<IHandleEntity* const*>(
		reinterpret_cast<const unsigned char*>(thisp) + MOVEHELPER_OFF_HOST);
	if (!pHost)
	{
		static int s_nNullHostSkip = 0;
		if (s_nNullHostSkip < 16)
		{
			Warning(eDLL_T::SERVER,
				"[MOVEHELP] ProcessImpacts skipped null host (#%d)\n",
				++s_nNullHostSkip);
		}
		return;
	}

	CMoveHelperServer__ProcessImpacts(thisp);
}

void VMoveHelperServer::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"40 53 57 48 83 EC 38 48 8B 79 08 48 8B D9 F6 87 50 03 00 00 44")
		.GetPtr(CMoveHelperServer__ProcessImpacts);

	if (!CMoveHelperServer__ProcessImpacts)
		Warning(eDLL_T::SERVER,
			"[MOVEHELP] CMoveHelperServer::ProcessImpacts pattern unresolved\n");
}

void VMoveHelperServer::Detour(const bool bAttach) const
{
	if (CMoveHelperServer__ProcessImpacts)
		DetourSetup(&CMoveHelperServer__ProcessImpacts, &Hook_CMoveHelperServer_ProcessImpacts, bAttach);
}
