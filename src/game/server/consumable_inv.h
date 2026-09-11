//=============================================================================//
//
// Purpose: u16 full-type shadow for S3 m_consumableInventory {u8 type,u8 count}
// at CPlayer+0x5FAC -- S21 loot indices exceed 255.
//
//=============================================================================//
#ifndef CONSUMABLE_INV_BRIDGE_H
#define CONSUMABLE_INV_BRIDGE_H

#include "thirdparty/detours/include/idetour.h"

// Full S21 loot type for a player's consumable slot (0..31). The shadow caches
// only the high byte, so it is trusted just while its low byte still matches
// what the engine actually stores; anything else returns nativeType unchanged.
uint16_t ConsumableInv_ResolveType(const void* player, int slot, uint16_t nativeType);

// Same lookup keyed on the entity index. The snapshot encoder never hands an
// array-element proxy the entity -- it passes the array base -- so the wire side
// must resolve through the objectID the pack loop already carries.
uint16_t ConsumableInv_ResolveTypeByEnt(int entIndex, int slot, uint16_t nativeType);

void ConsumableInv_LevelShutdown();

///////////////////////////////////////////////////////////////////////////////
class VConsumableInvBridge : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // CONSUMABLE_INV_BRIDGE_H
