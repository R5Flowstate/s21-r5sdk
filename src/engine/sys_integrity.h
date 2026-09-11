//=============================================================================//
//
// Purpose: S21 client -- disarm the engine EOS/EAC integrity net (Origin/auth
// weapon path). Default on for this product; set 0 to leave stock armed.
//
//=============================================================================//
#ifndef INTEGRITY_NEUTER_S21_H
#define INTEGRITY_NEUTER_S21_H

#include "thirdparty/detours/include/idetour.h"

class VIntegrityNeuterS21 : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

// True when -sdk_private or -devsdk is on the command line.
bool SDK_IsPrivateLabMode(void);

// True when integrity neuter is active (sdk_integrity_neuter 1). Default on.
bool SDK_IntegrityNeuterActive(void);

// Re-fill AmmoPoolType_t / eWeaponCooldownType from disk if registry or
// dataStruct was wiped (changelevel teardown). Safe no-op when already full.
void WeapConst_EnsurePopulated_S21(void);

#endif // INTEGRITY_NEUTER_S21_H
