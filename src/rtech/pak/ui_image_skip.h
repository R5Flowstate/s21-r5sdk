//=============================================================================//
//
// Purpose: Load a pak without paying for its UI images in the atlas.
//
//=============================================================================//
#pragma once
#include "thirdparty/detours/include/idetour.h"

// Marks a pak name so its uiia assets load as 1x1 stubs (one atlas tile each)
// instead of their full tile grid. Cleared by UIImageSkip_UnmarkPak.
bool UIImageSkip_MarkPak(const char* pszPakName);
void UIImageSkip_UnmarkPak(const char* pszPakName);

inline void (__fastcall *v_UIImage_Load_S21)(unsigned char* hdr, unsigned char* data,
	int pakHandle, void* asset) = nullptr;

// Resolves a RUI image by name (or precomputed GUID) to its atlas index; falls
// back to the "missing" image, whose index it caches in g_pUIImageMissingIdx_S21.
inline __int64 (__fastcall *v_UIImage_Resolve_S21)(void* ctx, const char* pszName, unsigned __int64 guid) = nullptr;
inline short* g_pUIImageMissingIdx_S21 = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VUIImageMissLog : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("UIImage_Resolve_S21", v_UIImage_Resolve_S21);
		LogVarAdr("UIImage_MissingIdx_S21", g_pUIImageMissingIdx_S21);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
class VUIImageSkipS21 : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("UIImage_Load_S21", v_UIImage_Load_S21);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////
