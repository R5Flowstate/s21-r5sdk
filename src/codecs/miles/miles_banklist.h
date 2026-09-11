//=============================================================================//
//
// Purpose: S21 client -- append disk Miles banks the packed banks.rson omits.
//
//=============================================================================//
#ifndef MILES_BANKLIST_S21_H
#define MILES_BANKLIST_S21_H

#include "thirdparty/detours/include/idetour.h"

inline __int64(__fastcall* v_MilesShared_LoadBanksListFromFile)(char*, __int64, __int64, __int64) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VMilesBankListS21 : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("MilesShared_LoadBanksListFromFile", v_MilesShared_LoadBanksListFromFile);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // MILES_BANKLIST_S21_H
