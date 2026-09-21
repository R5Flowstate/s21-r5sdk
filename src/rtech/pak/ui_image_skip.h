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
