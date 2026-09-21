//=============================================================================//
//
// Purpose: Heap shadow for S21 modelprecache idx 8192..16383.
//
//=============================================================================//
#ifndef MDL_PRECACHE_CLIENT_GROW_S21_H
#define MDL_PRECACHE_CLIENT_GROW_S21_H

#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier0/memaddr.h"
#include "thirdparty/detours/include/idetour.h"

// Resolved at GetFun time (Phase A) so the IDetour framework's pattern
// scan reports a clean failure on any miss before Detour runs.
inline __int64(__fastcall* v_CL_GetModelByIndex_Grow)(void* mi, int idx) = nullptr;
inline char*  (__fastcall* v_CL_StringChanged_Grow) (__int64 a1, __int64 table,
													  int idx, const char* name,
													  __int64 userdata,
													  __int64 userdataLen) = nullptr;
inline __int64(__fastcall* v_CL_ClientStateClear_Grow)(void* clstate) = nullptr;
inline __int64(__fastcall* v_CL_SetModelByIndex_Grow)(unsigned int idx) = nullptr;
inline void** v_CL_ModelPrecacheStringTableLoc = nullptr;

// Stats for the dump probe. Returns base + count + entries_populated. Safe to
// call before / after the detour attaches: when not attached, returns nullptr +
// 0 / 0 and the caller skips the shadow walk.
struct MdlPrecacheShadowStats
{
	const uint8_t* pBase;       // heap-backed shadow array (or null)
	uint32_t       shadowBase;  // first idx covered
	uint32_t       shadowEnd;   // one past last idx covered
	uint32_t       populated;   // entries with non-NULL model_t*
};
// 16-byte precache item slot for idx 8192..16383, nullptr outside the shadow.
uint8_t* MdlPrecacheShadow_Slot(uint32_t idx);
void MdlPrecacheShadow_GetStats(MdlPrecacheShadowStats* out);

class VModelPrecacheClientGrowS21 : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CModelInfoClient::GetModelByIndex_S21", v_CL_GetModelByIndex_Grow);
		LogFunAdr("CModelPrecache::OnStringChanged_S21",   v_CL_StringChanged_Grow);
		LogFunAdr("CClientState::Clear_S21",               v_CL_ClientStateClear_Grow);
		LogFunAdr("CL_SetModelByIndex_S21",               v_CL_SetModelByIndex_Grow);
		LogVarAdr("modelprecache stringtable",            v_CL_ModelPrecacheStringTableLoc);
	}
	virtual void GetFun(void) const
	{

		// cmp [rip+disp32], 0; jz; lea eax,[rdx+1]; cmp eax,1; jbe;
		// movsxd rdx,edx; lea rax,[rip+disp32]
		Module_FindPattern(g_GameDll,
			"48 83 3D ?? ?? ?? ?? 00 74 ?? 8D 42 01 83 F8 01 76 ?? 48 63 D2 48 8D 05")
			.GetPtr(v_CL_GetModelByIndex_Grow);

		// Writer (modelprecache OnStringChanged) prologue
		// 48 89 5C 24 18 48 89 7C 24 20 41 56 48 83 EC 60 48 8B FA 4D 63 F0
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 18 48 89 7C 24 20 41 56 48 83 EC 60 48 8B FA 4D 63 F0")
			.GetPtr(v_CL_StringChanged_Grow);

		// CClientState::Clear prologue; memset(items, 0, 0x20000) at this+0x21348
		Module_FindPattern(g_GameDll,
			"40 57 48 83 EC 20 48 89 6C 24 38 48 8B F9 33 ED "
			"C7 81 B8 00 00 00 FF FF FF FF")
			.GetPtr(v_CL_ClientStateClear_Grow);

		Module_FindPattern(g_GameDll,
			"40 53 48 81 EC ?? ?? ?? ?? 48 63 D9")
			.GetPtr(v_CL_SetModelByIndex_Grow);
		if (v_CL_SetModelByIndex_Grow)
		{
			v_CL_ModelPrecacheStringTableLoc =
				CMemory(reinterpret_cast<const void*>(v_CL_SetModelByIndex_Grow))
					.Offset(0x0C)
					.ResolveRelativeAddress(3, 7)
					.RCast<void**>();
		}
	}
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // MDL_PRECACHE_CLIENT_GROW_S21_H
