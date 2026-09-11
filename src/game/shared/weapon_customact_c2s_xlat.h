//=============================================================================
//
// Purpose: S21->S3 weaponCustomActivity ids before Weapon_ExecuteCustomActivityCmd.
// Two LTCG clones exist; hook both. Live clone reads ucmd+0x3A.
//
//=============================================================================
#ifndef WEAPON_CUSTOMACT_C2S_XLAT_H
#define WEAPON_CUSTOMACT_C2S_XLAT_H

#include "thirdparty/detours/include/idetour.h"

class VWeaponCustomActC2SXlat : public IDetour
{
	virtual void GetAdr(void) const { }
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

// Resolves the S3 activity id by name and calls the original trampoline so an already-S3 id is not re-translated.
bool WeaponCustomAct_ServerExecuteByName(void* pPlayer, const char* activityName);

#endif // WEAPON_CUSTOMACT_C2S_XLAT_H
