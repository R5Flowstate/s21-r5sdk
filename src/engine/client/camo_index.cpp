//=============================================================================//
//
// Purpose: Hostile / S3 camo index must not reach table[8*idx].
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "thirdparty/detours/include/detours.h"
#include "camo_index.h"

static constexpr int32_t kCamoField = 0xD64;
static constexpr int32_t kDrawInfoCamo = 0x5C;
static constexpr size_t kCamoCountOff = 0x10;
static constexpr uint64_t kCamoCountCap = 0x10000;

static int32_t Camo_ClampIndex(const int32_t idx)
{
	if (idx <= 0)
		return 0;
	if (!s_pCamoSkins)
		return 0;
	const uintptr_t obj = *s_pCamoSkins;
	if (!obj)
		return 0;
	const uint64_t n = *reinterpret_cast<const uint64_t*>(obj + kCamoCountOff);
	if (n == 0 || n > kCamoCountCap || static_cast<uint64_t>(idx) >= n)
		return 0;
	return idx;
}

static int32_t __fastcall Hook_CamoIndexGetter(void* pEnt)
{
	if (!pEnt)
		return 0;
	int32_t* const p = reinterpret_cast<int32_t*>(
		reinterpret_cast<uintptr_t>(pEnt) + kCamoField);
	const int32_t v = Camo_ClampIndex(*p);
	if (v != *p)
		*p = v;
	return v;
}

static __int64 __fastcall Hook_CamoDrawSubmit(void* a1, void* a2, uint8_t* drawInfo)
{
	if (drawInfo)
	{
		int32_t* const p = reinterpret_cast<int32_t*>(drawInfo + kDrawInfoCamo);
		*p = Camo_ClampIndex(*p);
	}
	return v_CamoDrawSubmit(a1, a2, drawInfo);
}

void VCamoIndexClamp::Detour(const bool bAttach) const
{
	if (v_CamoIndexGetter)
		DetourSetup(&v_CamoIndexGetter, &Hook_CamoIndexGetter, bAttach);
	if (v_CamoDrawSubmit)
		DetourSetup(&v_CamoDrawSubmit, &Hook_CamoDrawSubmit, bAttach);
	if (bAttach && v_CamoIndexGetter && v_CamoDrawSubmit)
		Msg(eDLL_T::CLIENT, "[CAMO-RECV] getter and draw-submit clamped\n");
}
