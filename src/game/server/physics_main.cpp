//====== Copyright � 1996-2005, Valve Corporation, All rights reserved. =======//
//
// Purpose: Physics simulation for non-havok/ipion objects
//
// $NoKeywords: $
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "edict.h"
#include "player.h"
#include "physics_main.h"
#include "engine/server/server.h"
#include "engine/client/client.h"
#include "game/server/util_server.h"
#include "game/server/hitbox_debug.h"
#include "game/server/headglitch_detect.h"

static ConVar sv_simulateBots("sv_simulateBots", "1", FCVAR_RELEASE, "Simulate user commands for bots on the server.");

//-----------------------------------------------------------------------------
// Purpose: Runs the command simulation for fake players
//-----------------------------------------------------------------------------
void Physics_RunBotSimulation(bool bSimulating)
{
	if (!sv_simulateBots.GetBool())
		return;

	for (int i = 0; i < g_ServerGlobalVariables->maxClients; i++)
	{
		const CClient* const pClient = g_pServer->GetClient(i);

		if (pClient->IsActive() && pClient->IsFakeClient())
		{
			CPlayer* const pPlayer = UTIL_PlayerByIndex(pClient->GetHandle());
			if (!pPlayer)
				continue;

			const edict_t nEdict = pPlayer->GetEdict();
			if (nEdict < 1 || nEdict == FL_EDICT_INVALID)
				continue;

			pPlayer->RunNullCommand();
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Runs the main physics simulation loop against all entities ( except players )
//-----------------------------------------------------------------------------
void Physics_RunThinkFunctions(bool bSimulating)
{
	Physics_RunBotSimulation(bSimulating);
	HitboxDebug_DrawFrame();
	HeadGlitch_Frame();
	v_Physics_RunThinkFunctions(bSimulating);
}

///////////////////////////////////////////////////////////////////////////////
void VPhysics_Main::Detour(const bool bAttach) const
{
	DetourSetup(&v_Physics_RunThinkFunctions, &Physics_RunThinkFunctions, bAttach);
}
