#include "cliententitylist.h"

// Note: 'entList' points directly at the vtable member
// of CClientEntityList; sub classed data is truncated.
static IClientNetworkable* ClientEntityList_GetClientNetworkable(IClientEntityList* const entList, const int entNum)
{
	// entNum is used to index into m_EntPtrArray, which is of size
	// NUM_ENT_ENTRIES. However, both the lower and upper bounds
	// checks were missing; check it here.
	if (entNum < 0 || entNum >= NUM_ENT_ENTRIES)
	{
		Assert(0);
		return nullptr;
	}

	return v_ClientEntityList_GetClientNetworkable(entList, entNum);
}

// Note: 'entList' points directly at the vtable member
// of CClientEntityList; sub classed data is truncated.
static IClientEntity* ClientEntityList_GetClientEntity(IClientEntityList* const entList, const int entNum)
{
	// Numbers < -2 will be used to index into the array as follows
	// m_EntPtrArray[ (MAX_EDICTS-2) - entNum ]. However, the code
	// doesn't have a clamp for underflows; check it here. -1 cases
	// are ignored here as they already are handled correctly.
	if (entNum < -(MAX_EDICTS - 2))
	{
		Assert(0);
		return nullptr;
	}

	// m_EntPtrArray is as large as NUM_ENT_ENTRIES, but there is no
	// overflow clamp; check it here.
	if (entNum >= NUM_ENT_ENTRIES)
	{
		Assert(0);
		return nullptr;
	}

	return v_ClientEntityList_GetClientEntity(entList, entNum);
}


//-----------------------------------------------------------------------------
// Purpose: a global list of all the entities in the game. All iteration through
// entities is done through this object.
//-----------------------------------------------------------------------------
CClientEntityList* g_clientEntityList = nullptr;

// CEntInfo: stride 0x20, +0x00 entity base, +0x08 serial.
static constexpr size_t kEntInfoStride = 0x20;
static constexpr ptrdiff_t kEntInfoOffSerial = 0x08;

static const uint8_t* ClientEntityList_EntInfoArray(void)
{
	static const uint8_t* s_pArray = nullptr;
	static bool s_bTried = false;
	if (s_bTried)
		return s_pArray;
	s_bTried = true;
	// RequestBodygroupUpdate indexes the array with the owner handle (+0x1560).
	const CMemory fn = Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 8B 81 60 15 00 00 48 8B D9 83 F8 FF "
		"0F 84 ?? ?? ?? ?? 0F B7 C8 48 C1 E1 05 48 89 74 24 ?? 48 8D 35");
	if (fn.IsValid())
	{
		const CMemory lea = fn.FindPattern("48 8D 35");
		if (lea.IsValid())
			s_pArray = lea.ResolveRelativeAddress(3, 7).RCast<const uint8_t*>();
	}
	if (!s_pArray)
		Warning(eDLL_T::CLIENT, "[ENTLIST] S21 entity info array unresolved -- index lookups return null\n");
	return s_pArray;
}

void* ClientEntityList_EntityAt(const int entNum, const int serial)
{
	if (entNum < 0 || entNum >= NUM_ENT_ENTRIES)
		return nullptr;
	const uint8_t* const pArray = ClientEntityList_EntInfoArray();
	if (!pArray)
		return nullptr;
	const uint8_t* const pSlot = pArray + static_cast<size_t>(entNum) * kEntInfoStride;
	if (serial >= 0 && *reinterpret_cast<const uint32_t*>(pSlot + kEntInfoOffSerial) != static_cast<uint32_t>(serial))
		return nullptr;
	return *reinterpret_cast<void* const*>(pSlot);
}
