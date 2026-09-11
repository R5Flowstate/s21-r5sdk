//=============================================================================//
//
// Purpose: Fixes the Pathfinder grapple-rope hand-origin bug. See
// grapple_rope_diag.h for the cause and the three fix sites.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "game/client/grapple_rope_diag.h"

static ConVar sdk_grapple_anim_type_fix("sdk_grapple_anim_type_fix", "1", FCVAR_RELEASE,
	"Correct the S3->S21 m_playAnimationType ordinal drift (wire value 5 -> "
	"local 6) that causes the grapple rope to render from the chest instead "
	"of the hand. 0 = off (bug reproduces). 1 = on (default).");

static volatile LONG s_grappleAnimFixCount = 0;

//-----------------------------------------------------------------------------
// Shared correction: player+0x2FA8 (m_playAnimationType) 5 -> 6, guarded on
// player+0x2D60 (the same field each original function already treats as its
// own "is this an active grapple" signal) being nonzero.
//-----------------------------------------------------------------------------
static inline void CorrectGrappleAnimTypeDrift(void* player, const char* tag)
{
	if (!sdk_grapple_anim_type_fix.GetBool() || !player)
		return;

	// Live player entity from engine hooks -- null-checked once, plain fields (D2).
	uint8_t* p = reinterpret_cast<uint8_t*>(player);
	int32_t* pPlayAnimType = reinterpret_cast<int32_t*>(p + 0x2FA8);
	const uint8_t gate1 = *(p + 0x2D60);

	if (*pPlayAnimType == 5 && gate1 != 0)
	{
		*pPlayAnimType = 6;

		const LONG n = InterlockedIncrement(&s_grappleAnimFixCount);
		if (n <= 10 || (n % 2000) == 0)
		{
			DevMsg(eDLL_T::CLIENT, "[GRAPPLE-ANIM-FIX] (%s) player=%p corrected m_playAnimationType 5->6 (n=%ld)\n",
				tag, player, n);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: the sparse gating/reselect helper (S21).
//-----------------------------------------------------------------------------
static char GrappleAnimSelectSequence_Hook(void* player, void* a2, char a3)
{
	CorrectGrappleAnimTypeDrift(player, "select");
	return v_GrappleAnimSelectSequence(player, a2, a3);
}

//-----------------------------------------------------------------------------
// Purpose: the actual per-frame gate (S21) the rope renderer
// checks every frame.
//-----------------------------------------------------------------------------
static bool IsPlayingGrappleAnimation_Client_Hook(void* player)
{
	CorrectGrappleAnimTypeDrift(player, "gate");
	return v_IsPlayingGrappleAnimation_Client(player);
}

//-----------------------------------------------------------------------------
// Purpose: the per-tick movement/think function (S21) whose
// switch(m_playAnimationType) misroutes case 5 to case 4's handler.
//-----------------------------------------------------------------------------
static int64_t MovePostThinkAnimSwitch_Hook(void* player)
{
	CorrectGrappleAnimTypeDrift(player, "switch");
	return v_MovePostThinkAnimSwitch(player);
}

///////////////////////////////////////////////////////////////////////////////
void VGrappleRopeDiag::GetFun(void) const
{

	Module_FindPattern(g_GameDll,
		"48 8B C4 48 89 58 ?? 48 89 78 ?? 55 48 8D 68 ?? 48 81 EC 00 01 00 00 80 B9 60 2D 00 00 00")
		.GetPtr(v_GrappleAnimSelectSequence);

	if (!v_GrappleAnimSelectSequence)
		Warning(eDLL_T::CLIENT, "[GRAPPLE-ANIM-FIX] GrappleAnimSelectSequence pattern unresolved\n");

	// IsPlayingGrappleAnimation_Client (S21). Verified via

	// offset (`cmp dword ptr [reg+2FA8h], 6`).
	Module_FindPattern(g_GameDll,
		"83 B9 A8 2F 00 00 06 75 ?? 8B 91 04 37 00 00 4C 8D 15 ?? ?? ?? ?? 33 C0 83 FA FF 74 ?? 44 0F B7 C2 49 C1 E0 05 C1 EA 10")
		.GetPtr(v_IsPlayingGrappleAnimation_Client);

	if (!v_IsPlayingGrappleAnimation_Client)
		Warning(eDLL_T::CLIENT, "[GRAPPLE-ANIM-FIX] IsPlayingGrappleAnimation_Client pattern unresolved\n");

	// Per-tick movement/think function (S21). Verified via

	Module_FindPattern(g_GameDll,
		"48 8B C4 55 56 48 8D A8 C8 FD FF FF 48 81 EC 28 03 00 00 48 89 58 08 48 89 78 10 48 8B F9 4C 89 68 E8 4C 89 70 E0 4C 89 78 D8")
		.GetPtr(v_MovePostThinkAnimSwitch);

	if (!v_MovePostThinkAnimSwitch)
		Warning(eDLL_T::CLIENT, "[GRAPPLE-ANIM-FIX] MovePostThinkAnimSwitch pattern unresolved\n");
}

///////////////////////////////////////////////////////////////////////////////
void VGrappleRopeDiag::Detour(const bool bAttach) const
{
	if (v_GrappleAnimSelectSequence)
	{
		DetourSetup(&v_GrappleAnimSelectSequence, &GrappleAnimSelectSequence_Hook, bAttach);
	}
	else if (bAttach)
	{
		Warning(eDLL_T::CLIENT, "[GRAPPLE-ANIM-FIX] disabled: GrappleAnimSelectSequence pattern resolve failed\n");
	}

	if (v_IsPlayingGrappleAnimation_Client)
	{
		DetourSetup(&v_IsPlayingGrappleAnimation_Client, &IsPlayingGrappleAnimation_Client_Hook, bAttach);
	}
	else if (bAttach)
	{
		Warning(eDLL_T::CLIENT, "[GRAPPLE-ANIM-FIX] disabled: IsPlayingGrappleAnimation_Client pattern resolve failed\n");
	}

	if (v_MovePostThinkAnimSwitch)
	{
		DetourSetup(&v_MovePostThinkAnimSwitch, &MovePostThinkAnimSwitch_Hook, bAttach);
	}
	else if (bAttach)
	{
		Warning(eDLL_T::CLIENT, "[GRAPPLE-ANIM-FIX] disabled: MovePostThinkAnimSwitch pattern resolve failed\n");
	}
}
