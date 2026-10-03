//=============================================================================//
//
// Purpose: S21 client -- append disk Miles banks the packed banks.rson omits.
//
//=============================================================================//
#ifndef MILES_BANKLIST_S21_H
#define MILES_BANKLIST_S21_H

#include "thirdparty/detours/include/idetour.h"

inline __int64(__fastcall* v_MilesShared_LoadBanksListFromFile)(char*, __int64, __int64, __int64) = nullptr;
inline char(__fastcall* v_ClientSoundMiles_Initialize)(void) = nullptr;
inline const char** s_ppszMilesLanguageLatch = nullptr;

// One mod bank per boot (BankIndex 2, served after custom). Resolved once,
// shared by the bank-list append and the project sidecar serve.
bool MilesBankDisk_ResolveModBank(void);
const char* MilesBankDisk_ModBankName(void);
const char* MilesBankDisk_ModProjectPath(void);
bool MilesBankDisk_ShouldServeProject(const char* const pszPath);

///////////////////////////////////////////////////////////////////////////////
class VMilesBankListS21 : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("MilesShared_LoadBanksListFromFile", v_MilesShared_LoadBanksListFromFile);
		LogFunAdr("ClientSoundMiles_Initialize", v_ClientSoundMiles_Initialize);
		LogVarAdr("miles_language_latch", s_ppszMilesLanguageLatch);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // MILES_BANKLIST_S21_H
