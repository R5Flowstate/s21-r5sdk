//=============================================================================//
//
// Purpose: Lift the S21 client FOV ceiling so the 120 the menu slider already
// offers (video.res maxValue 1.6875) actually sticks. Two sites, both behind
// cl_fov_120: the shared 115.0f render dword, and the text-entry clamp that
// hardcodes 110 and snaps the box back. See fov_limit.h.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "game/client/fov_limit.h"
#include <cstdlib>

// 115.0f -> 120.0f, exact IEEE-754 bits.
static constexpr uint32_t FOV_CAP_115_BITS = 0x42E60000u;
static constexpr uint32_t FOV_CAP_120_BITS = 0x42F00000u;

static constexpr float FOV_MIN_DEG = 70.0f;
static constexpr float FOV_MAX_DEG = 120.0f;
static constexpr float FOV_SCALE_PER_DEG = 0.01375f; // 0.0275 slider step per 2 deg

static constexpr uint8_t FOV_TEXT_MAX_110 = 110;
static constexpr uint8_t FOV_TEXT_MAX_120 = 120;

// VideoOptions_FOVTextChanged: mov ecx, 110 / cmp ebx, 110. +1 and +0x4E
// are the immediates. Formula already maps 120 -> 1.6875; only the clamp
// blocked it. Unique on the DX11 client.
static uint8_t* s_pFovTextMaxImm = nullptr;
static uint8_t* s_pFovTextCmpImm = nullptr;

//-----------------------------------------------------------------------------
// Applies the wanted ceiling to the shared dword. Bails loudly on anything
// unexpected instead of writing blind.
//-----------------------------------------------------------------------------
static void FOVLimit_SetCapBits(const uint32_t wantBits)
{
	if (!g_pFovRenderCap)
		return;

	uint32_t* const pBits = reinterpret_cast<uint32_t*>(g_pFovRenderCap);
	if (*pBits == wantBits)
		return;

	if (*pBits != FOV_CAP_115_BITS && *pBits != FOV_CAP_120_BITS)
	{
		Warning(eDLL_T::CLIENT,
			"[FOV-LIMIT] render cap holds 0x%08X, leaving it alone\n", *pBits);
		return;
	}

	DWORD oldProt = 0;
	if (!VirtualProtect(pBits, sizeof(uint32_t), PAGE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::CLIENT, "[FOV-LIMIT] VirtualProtect failed, cap untouched\n");
		return;
	}
	*pBits = wantBits;
	VirtualProtect(pBits, sizeof(uint32_t), oldProt, &oldProt);

	Msg(eDLL_T::CLIENT, "[FOV-LIMIT] FOV render cap -> %s\n",
		wantBits == FOV_CAP_120_BITS ? "120" : "115");
}

static void FOVLimit_PatchTextImm(uint8_t* const pImm, const uint8_t want)
{
	if (!pImm || *pImm == want)
		return;
	if (*pImm != FOV_TEXT_MAX_110 && *pImm != FOV_TEXT_MAX_120)
	{
		Warning(eDLL_T::CLIENT,
			"[FOV-LIMIT] text clamp holds %u, leaving it alone\n", *pImm);
		return;
	}

	DWORD oldProt = 0;
	if (!VirtualProtect(pImm, 1, PAGE_EXECUTE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::CLIENT, "[FOV-LIMIT] VirtualProtect failed, text clamp untouched\n");
		return;
	}
	*pImm = want;
	VirtualProtect(pImm, 1, oldProt, &oldProt);
}

static void FOVLimit_SetTextMax(const uint8_t want)
{
	if (!s_pFovTextMaxImm || !s_pFovTextCmpImm)
		return;
	if (*s_pFovTextMaxImm == want && *s_pFovTextCmpImm == want)
		return;

	FOVLimit_PatchTextImm(s_pFovTextMaxImm, want);
	FOVLimit_PatchTextImm(s_pFovTextCmpImm, want);
	if (*s_pFovTextMaxImm == want && *s_pFovTextCmpImm == want)
		Msg(eDLL_T::CLIENT, "[FOV-LIMIT] FOV text clamp -> %u\n", want);
}

static void FOVLimit_Apply(void);

static void FOVLimit_Changed_f(IConVar* var, const char* pOldValue, float flOldValue, ChangeUserData_t pUserData)
{
	(void)var;
	(void)pOldValue;
	(void)flOldValue;
	(void)pUserData;
	FOVLimit_Apply();
}

static ConVar cl_fov_120("cl_fov_120", "1", FCVAR_RELEASE,
	"Raise FOV to 120 (render cap 115 and text-entry clamp 110). 0 = retail.",
	FOVLimit_Changed_f);

static void FOVLimit_Apply(void)
{
	const bool bOn = cl_fov_120.GetBool();
	FOVLimit_SetCapBits(bOn ? FOV_CAP_120_BITS : FOV_CAP_115_BITS);
	FOVLimit_SetTextMax(bOn ? FOV_TEXT_MAX_120 : FOV_TEXT_MAX_110);
}

//-----------------------------------------------------------------------------
// Console command: fov_deg
//-----------------------------------------------------------------------------
static void CC_FOV_SetDegrees_f(const CCommand& args)
{
	if (args.ArgC() != 2)
	{
		Msg(eDLL_T::CLIENT, "Usage: fov_deg <70-120>\n");
		return;
	}

	float deg = static_cast<float>(atof(args.Arg(1)));
	if (deg != deg || deg < FOV_MIN_DEG)
		deg = FOV_MIN_DEG;
	if (deg > FOV_MAX_DEG)
		deg = FOV_MAX_DEG;

	ConVar* const pFovScale = g_pCVar ? g_pCVar->FindVar("cl_fovScale") : nullptr;
	if (!pFovScale)
	{
		Warning(eDLL_T::CLIENT, "[FOV-LIMIT] cl_fovScale not found\n");
		return;
	}

	const float scale = 1.0f + (deg - FOV_MIN_DEG) * FOV_SCALE_PER_DEG;
	pFovScale->SetValue(scale);
	Msg(eDLL_T::CLIENT, "[FOV-LIMIT] FOV %.0f (cl_fovScale %.4f)\n", deg, scale);
}

static ConCommand fov_deg("fov_deg", CC_FOV_SetDegrees_f,
	"Set FOV in degrees (70-120); writes cl_fovScale directly, bypassing the menu slider.",
	FCVAR_CLIENTDLL);

void VFOVLimit::GetFun(void) const
{
	// First-person clamp chain (cmp/cmov/movd/cvtdq2ps/addss/movaps/minss).
	// 0x17 = minss against the shared cap; disp32 bytes wildcarded.
	const CMemory p1 = Module_FindPattern(g_GameDll,
		"3B FE 0F 4F F7 66 0F 6E CE 0F 5B C9 F3 0F 58 0D ?? ?? ?? ?? "
		"0F 28 C1 F3 0F 5D 05 ?? ?? ?? ??");
	// Zoom-delta tail (addss/movaps/minss/subss). 0x0B = minss, same deal.
	const CMemory p2 = Module_FindPattern(g_GameDll,
		"F3 0F 58 0D ?? ?? ?? ?? 0F 28 C1 F3 0F 5D 05 ?? ?? ?? ?? F3 0F 5C C3");

	if (!p1 || !p2)
	{
		Warning(eDLL_T::CLIENT,
			"[FOV-LIMIT] cap anchors unresolved -- 120 cap not installed\n");
	}
	else
	{
		float* const d1 = p1.Offset(0x17).ResolveRelativeAddress(0x4, 0x8).RCast<float*>();
		float* const d2 = p2.Offset(0x0B).ResolveRelativeAddress(0x4, 0x8).RCast<float*>();

		if (!d1 || !d2 || d1 != d2)
		{
			Warning(eDLL_T::CLIENT,
				"[FOV-LIMIT] cap anchors diverged -- 120 cap not installed\n");
		}
		else
		{
			const uint32_t capBits = *reinterpret_cast<const uint32_t*>(d1);
			if (capBits != FOV_CAP_115_BITS && capBits != FOV_CAP_120_BITS)
			{
				Warning(eDLL_T::CLIENT,
					"[FOV-LIMIT] shared cap is not 115.0f -- 120 cap not installed\n");
			}
			else
			{
				g_pFovRenderCap = d1;
			}
		}
	}

	CMemory text = Module_FindPattern(g_GameDll,
		"B9 6E 00 00 00 8B D8 B8 46 00 00 00 3B D0 0F 4C D0 3B D1 0F 4C CA");
	if (!text)
	{
		text = Module_FindPattern(g_GameDll,
			"B9 78 00 00 00 8B D8 B8 46 00 00 00 3B D0 0F 4C D0 3B D1 0F 4C CA");
	}
	if (!text
		|| (text.Offset(0x4C).GetValue<uint8_t>() != 0x83)
		|| (text.Offset(0x4D).GetValue<uint8_t>() != 0xFB))
	{
		Warning(eDLL_T::CLIENT,
			"[FOV-LIMIT] text clamp unresolved -- box still snaps to 110\n");
		return;
	}
	s_pFovTextMaxImm = text.Offset(1).RCast<uint8_t*>();
	s_pFovTextCmpImm = text.Offset(0x4E).RCast<uint8_t*>();
}

void VFOVLimit::Detour(const bool bAttach) const
{
	if (!bAttach)
	{
		FOVLimit_SetCapBits(FOV_CAP_115_BITS);
		FOVLimit_SetTextMax(FOV_TEXT_MAX_110);
		return;
	}
	FOVLimit_Apply();
}
