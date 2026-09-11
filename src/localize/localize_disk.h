//=============================================================================//
//
// Purpose: Load localization from disk language tables; skip localization_*.rpak.
//
//=============================================================================//

#ifndef LOCALIZE_DISK_S21_H
#define LOCALIZE_DISK_S21_H

#include "thirdparty/detours/include/idetour.h"

// CLocalize::QueueLocalizationPak(this) -- loads localization_%s.rpak.
inline bool (__fastcall *v_QueueLocalizationPak_S21)(void* thisptr) = nullptr;

// CLocalize::WaitForLocalizationPakToLoad(this)
inline void (__fastcall *v_WaitForLocalizationPak_S21)(void* thisptr) = nullptr;

// CLocalize::ReloadLocalizationFiles(this)
inline void (__fastcall *v_ReloadLocalizationFiles_S21)(void* thisptr) = nullptr;

// DetectLanguage -> const char* ("english",...)
inline const char* (__fastcall *v_DetectLanguage_S21)(void) = nullptr;

// Override, else DetectLanguage, else "english". Same names as g_LanguageNames.
const char* Localize_GetCurrentLanguage(void);

// rtech HashName (same as FindIndex) -- 64-bit; human keys in language.txt files.
inline unsigned __int64 (__fastcall *v_LocHashAligned_S21)(const char* str) = nullptr;
inline unsigned __int64 (__fastcall *v_LocHashUnaligned_S21)(const char* str) = nullptr;

// s_localizationPaks[14]
inline int* g_pLocalizationPaks_S21 = nullptr;

// sLocAssets: count @ base, LocalizationAsset* array @ base+8
inline unsigned int* g_pLocAssetCount_S21 = nullptr;
inline void** g_ppLocAssets_S21 = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VLocalizeDiskS21 : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("QueueLocalizationPak_S21", v_QueueLocalizationPak_S21);
		LogFunAdr("WaitForLocalizationPak_S21", v_WaitForLocalizationPak_S21);
		LogFunAdr("ReloadLocalizationFiles_S21", v_ReloadLocalizationFiles_S21);
		LogFunAdr("DetectLanguage_S21", v_DetectLanguage_S21);
		LogFunAdr("LocHashAligned_S21", v_LocHashAligned_S21);
		LogFunAdr("LocHashUnaligned_S21", v_LocHashUnaligned_S21);
		LogVarAdr("s_localizationPaks_S21", g_pLocalizationPaks_S21);
		LogVarAdr("sLocAssetCount_S21", g_pLocAssetCount_S21);
		LogVarAdr("sLocAssets_S21", g_ppLocAssets_S21);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // LOCALIZE_DISK_S21_H
