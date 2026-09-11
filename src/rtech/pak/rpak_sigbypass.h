//=============================================================================//
//
// Purpose: S21 RPak signature bypass. Authenticode IAT skip is allowlisted basenames only.
//
//=============================================================================//

#ifndef RPAK_SIGBYPASS_S21_H
#define RPAK_SIGBYPASS_S21_H

#include "thirdparty/detours/include/idetour.h"
#include "tier0/module.h"

inline int (__fastcall *v_Pak_VerifySignature_S21)(
	__int64 pakContext, __int64 a2, __int64 a3, __int64 a4) = nullptr;

inline uintptr_t g_WinVerifyTrustIat_S21 = 0;

///////////////////////////////////////////////////////////////////////////////
class VRPakSigBypassS21 : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("Pak_VerifySignature_S21", v_Pak_VerifySignature_S21);
		LogFunAdr("WinVerifyTrust_IAT", reinterpret_cast<void*>(g_WinVerifyTrustIat_S21));
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll,
			"48 89 4C 24 08 55 53 56 57 41 54 41 57 48 8D AC 24 08 EA FF FF "
			"B8 F8 16 00 00")
			.GetPtr(v_Pak_VerifySignature_S21);

		const CMemory iat = g_GameDll.GetImportedSymbol("WINTRUST.dll", "WinVerifyTrust", true);
		g_WinVerifyTrustIat_S21 = iat.IsValid() ? iat.GetPtr() : 0;
	}
	virtual void GetVar(void) const {}
	virtual void GetCon(void) const {}
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // RPAK_SIGBYPASS_S21_H
