//=============================================================================//
//
// Purpose: S3 CPhysicsSurfaceProps remaps materialIndex > 127 to slot 0.
// S21 indexes the id directly. Digital_Water is 133.
//
//=============================================================================//
#include "core/stdafx.h"
#include "surfaceprop_id.h"

#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier1/convar.h"

static constexpr ptrdiff_t SURF_OFF_PROPS = 0x70;
static constexpr ptrdiff_t SURF_OFF_COUNT = 0x88;
static constexpr ptrdiff_t SURF_OFF_DEFAULT = 0x1CC;
static constexpr int SURF_STRIDE = 120;
static constexpr int SURF_ID_CLAMP = 127;
static constexpr int SURF_SHADOW_INDEX = 0xF000;
static constexpr ptrdiff_t SURF_OFF_FRICTION = 0x14;

static ConVar bridge_surfprop_unclamp("bridge_surfprop_unclamp", "1", FCVAR_RELEASE,
	"Use surface ids 128-255 on the dedi instead of remapping them to default. "
	"S21 already does. 0 = stock S3 clamp (A/B).");

static ConVar sdk_surfprop_id_diag("sdk_surfprop_id_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Log the first hi-id surface lookup after unclamp.");

static void(__fastcall* v_GetPhysicsProperties)(void*, int, float*, float*, float*, float*) = nullptr;
static void*(__fastcall* v_GetSurfaceData)(void*, int) = nullptr;

static char s_surfDummy[SURF_STRIDE];

static char* SurfProp_Row(void* const pThis, int idx)
{
	if (!pThis)
		return nullptr;

	if (idx == SURF_SHADOW_INDEX)
		idx = *reinterpret_cast<int*>(static_cast<char*>(pThis) + SURF_OFF_DEFAULT);
	else if (idx > SURF_ID_CLAMP && !bridge_surfprop_unclamp.GetBool())
		idx = 0;

	if (idx < 0)
		return nullptr;

	const int nCount = *reinterpret_cast<int*>(static_cast<char*>(pThis) + SURF_OFF_COUNT);
	if (idx > nCount - 1)
		return nullptr;

	char* const pProps = *reinterpret_cast<char**>(static_cast<char*>(pThis) + SURF_OFF_PROPS);
	if (!pProps)
		return nullptr;

	return pProps + SURF_STRIDE * idx;
}

static char* SurfProp_RowOrDummy(void* const pThis, int idx)
{
	char* pRow = SurfProp_Row(pThis, idx);
	if (!pRow)
		pRow = SurfProp_Row(pThis, 0);
	return pRow ? pRow : s_surfDummy;
}

static void SurfProp_NoteHi(const int idx)
{
	if (!sdk_surfprop_id_diag.GetBool() || idx <= SURF_ID_CLAMP)
		return;

	static volatile LONG s_once = 0;
	if (InterlockedCompareExchange(&s_once, 1, 0) != 0)
		return;

	Warning(eDLL_T::SERVER, "[SURFPROP-ID] FIRST FIRE id=%d unclamp=%d\n",
		idx, bridge_surfprop_unclamp.GetInt());
}

static void __fastcall Hook_GetPhysicsProperties(void* pThis, int materialIndex,
	float* density, float* thickness, float* friction, float* elasticity)
{
	SurfProp_NoteHi(materialIndex);

	char* const pRow = SurfProp_RowOrDummy(pThis, materialIndex);

	if (friction)
		*friction = *reinterpret_cast<float*>(pRow + SURF_OFF_FRICTION);
	if (elasticity)
		*elasticity = *reinterpret_cast<float*>(pRow + SURF_OFF_FRICTION + 4);
	if (density)
		*density = *reinterpret_cast<float*>(pRow + SURF_OFF_FRICTION + 8);
	if (thickness)
		*thickness = *reinterpret_cast<float*>(pRow + SURF_OFF_FRICTION + 12);
}

static void* __fastcall Hook_GetSurfaceData(void* pThis, int materialIndex)
{
	SurfProp_NoteHi(materialIndex);

	return SurfProp_RowOrDummy(pThis, materialIndex) + SURF_OFF_FRICTION;
}

void VSurfPropIdUnclamp::GetAdr(void) const
{
	LogFunAdr("CPhysicsSurfaceProps::GetPhysicsProperties", v_GetPhysicsProperties);
	LogFunAdr("CPhysicsSurfaceProps::GetSurfaceData", v_GetSurfaceData);
}

void VSurfPropIdUnclamp::GetFun(void) const
{
	Module_FindPattern(g_GameDll, "4C 8B D1 83 FA ?? 7E")
		.GetPtr(v_GetPhysicsProperties);
	Module_FindPattern(g_GameDll, "45 33 C9 83 FA ?? 7E")
		.GetPtr(v_GetSurfaceData);

	if (!v_GetPhysicsProperties || !v_GetSurfaceData)
		Warning(eDLL_T::SERVER, "[SURFPROP-ID] pattern unresolved -- hi-id surfaces stay default\n");
}

void VSurfPropIdUnclamp::Detour(const bool bAttach) const
{
	if (v_GetPhysicsProperties)
		DetourSetup(&v_GetPhysicsProperties, &Hook_GetPhysicsProperties, bAttach);
	if (v_GetSurfaceData)
		DetourSetup(&v_GetSurfaceData, &Hook_GetSurfaceData, bAttach);
}
