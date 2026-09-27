//=============================================================================//
//
// Purpose: Raise the Alter tactical maximum wall depth from 20 m to 30 m so
// the client hologram matches the dedi placement and the Void Passage range.
//
//=============================================================================//
#include "core/stdafx.h"
#include "game/client/phase_door_depth.h"

// 787.40155^2 (20 m) and 1181.1^2 (30 m), exact IEEE-754 bits.
static constexpr uint32_t PHASE_DOOR_DEPTH_20M_SQR_BITS = 0x49175E12u;
static constexpr uint32_t PHASE_DOOR_DEPTH_30M_SQR_BITS = 0x49AA49E1u;

static void PhaseDoorDepth_SetBits(const uint32_t wantBits)
{
	if (!g_pPhaseDoorMaxDepthSqr)
		return;

	uint32_t* const pBits = reinterpret_cast<uint32_t*>(g_pPhaseDoorMaxDepthSqr);
	if (*pBits == wantBits)
		return;

	if (*pBits != PHASE_DOOR_DEPTH_20M_SQR_BITS && *pBits != PHASE_DOOR_DEPTH_30M_SQR_BITS)
	{
		Warning(eDLL_T::CLIENT,
			"[PHASE-DOOR-DEPTH] max depth holds 0x%08X, leaving it alone\n", *pBits);
		return;
	}

	DWORD oldProt = 0;
	if (!VirtualProtect(pBits, sizeof(uint32_t), PAGE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::CLIENT, "[PHASE-DOOR-DEPTH] VirtualProtect failed, depth untouched\n");
		return;
	}
	*pBits = wantBits;
	VirtualProtect(pBits, sizeof(uint32_t), oldProt, &oldProt);

	Msg(eDLL_T::CLIENT, "[PHASE-DOOR-DEPTH] max wall depth -> %s\n",
		wantBits == PHASE_DOOR_DEPTH_30M_SQR_BITS ? "30 m" : "20 m");
}

void VPhaseDoorDepth::GetFun(void) const
{
	// Exit search depth gate: addss / comiss xmm2, [depthSqr] / jbe / mov rax / mov r13b, 1.
	const CMemory p = Module_FindPattern(g_GameDll,
		"F3 0F 58 D0 0F 2F 15 ?? ?? ?? ?? 76 23 48 8B 05 ?? ?? ?? ?? 41 B5 01");
	if (!p)
	{
		Warning(eDLL_T::CLIENT, "[PHASE-DOOR-DEPTH] depth gate unresolved -- stays at 20 m\n");
		return;
	}

	float* const pDepth = p.Offset(0x4).ResolveRelativeAddress(0x3, 0x7).RCast<float*>();
	if (!pDepth)
	{
		Warning(eDLL_T::CLIENT, "[PHASE-DOOR-DEPTH] depth constant unresolved -- stays at 20 m\n");
		return;
	}

	const uint32_t bits = *reinterpret_cast<const uint32_t*>(pDepth);
	if (bits != PHASE_DOOR_DEPTH_20M_SQR_BITS && bits != PHASE_DOOR_DEPTH_30M_SQR_BITS)
	{
		Warning(eDLL_T::CLIENT,
			"[PHASE-DOOR-DEPTH] depth constant is 0x%08X, not 20 m -- stays unpatched\n", bits);
		return;
	}
	g_pPhaseDoorMaxDepthSqr = pDepth;
}

void VPhaseDoorDepth::Detour(const bool bAttach) const
{
	PhaseDoorDepth_SetBits(bAttach ? PHASE_DOOR_DEPTH_30M_SQR_BITS : PHASE_DOOR_DEPTH_20M_SQR_BITS);
}
