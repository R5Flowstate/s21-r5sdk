//=============================================================================//
//
// Purpose: S21 melee-over-offhand rule on the dedi melee test.
//
// With an offhand selected in the main hand, the S21 client allows melee only
// when that weapon sets offhand_cancelled_by_melee, and an offhand in the
// attack state no longer blocks melee through the button-press protection.
// The dedi test only knows the older charge / interrupt-resumeable rule.
//
//=============================================================================//
#ifndef OFFHAND_MELEE_CANCEL_H
#define OFFHAND_MELEE_CANCEL_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VOffhandMeleeCancel : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // OFFHAND_MELEE_CANCEL_H
