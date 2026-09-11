//=============================================================================//
//
// Purpose: Reject invalid per-entity animation-data descriptors before the
// nearby-entity job builder dereferences them. Return 0 = skip this frame.
//
//=============================================================================//
#include "core/stdafx.h"
#include "core/logdef.h"
#include "tier0/dbg.h"
#include "tier0/memaddr.h"
#include "tier0/module.h"
#include "tier1/cvar.h"
#include "anim_desc.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Consumer allocates blobCapacity+0x70 and copies blobUsed from blobData.
static constexpr ptrdiff_t DESC_OFF_BLOBDATA = 0x08; // const void*
static constexpr ptrdiff_t DESC_OFF_CAPACITY = 0x44; // u16, allocation size
static constexpr ptrdiff_t DESC_OFF_USED     = 0x46; // u16, bytes to copy
// Nearby-job consume slots. +0x70 is always called; +0x78 is called if nonzero.
static constexpr ptrdiff_t DESC_OFF_FN_ALWAYS = 0x70;
static constexpr ptrdiff_t DESC_OFF_FN_OPT    = 0x78;

// Highest canonical user-mode address on x64.
static constexpr unsigned __int64 USER_ADDR_LIMIT = 0x00007FFFFFFFFFFFULL;
// Below this is the null page and the reserved low range; never a heap object.
static constexpr unsigned __int64 USER_ADDR_FLOOR = 0x0000000000010000ULL;

static ConVar sdk_anim_desc_guard("sdk_anim_desc_guard", "1", FCVAR_RELEASE,
	"Reject invalid per-entity animation-data descriptors before the "
	"nearby-entity job builder dereferences them. 0 = pass-through.");

typedef __int64(__fastcall* PFN_GetEntityAnimDesc)(__int64* /*entity*/);
static PFN_GetEntityAnimDesc v_GetEntityAnimDesc = nullptr;

static int s_logBudget = 32;

// Cheap range rejection. No VirtualQuery -- that froze the client pump.
static bool IsPlausibleAddress(const unsigned __int64 addr)
{
	if (addr < USER_ADDR_FLOOR || addr > USER_ADDR_LIMIT)
		return false;

	// Unset sentinel (-1) or a zero-extended 32-bit value; neither is a
	// user-mode heap/module pointer.
	if (static_cast<unsigned __int32>(addr) == 0xFFFFFFFFu)
		return false;
	if ((addr >> 32) == 0)
		return false;

	return true;
}

// Blob fields must agree. SEH covers an unmapped address that passed the range test.
static bool HasConsistentBlob(const __int64 desc)
{
	__try
	{
		const unsigned __int16 capacity =
			*reinterpret_cast<const unsigned __int16*>(desc + DESC_OFF_CAPACITY);
		const unsigned __int16 used =
			*reinterpret_cast<const unsigned __int16*>(desc + DESC_OFF_USED);

		if (used > capacity)
			return false;

		if (used > 0 &&
			!*reinterpret_cast<const void* const*>(desc + DESC_OFF_BLOBDATA))
			return false;

		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

// [desc+0x70] is always called; [desc+0x78] is called if nonzero. -1 is nonzero.
static bool HasCallableSlots(const __int64 desc)
{
	const unsigned __int64 always =
		*reinterpret_cast<const unsigned __int64*>(desc + DESC_OFF_FN_ALWAYS);
	if (!IsPlausibleAddress(always))
		return false;

	const unsigned __int64 opt =
		*reinterpret_cast<const unsigned __int64*>(desc + DESC_OFF_FN_OPT);
	if (opt != 0 && !IsPlausibleAddress(opt))
		return false;

	return true;
}

static __int64 __fastcall Hook_GetEntityAnimDesc(__int64* entity)
{
	if (!v_GetEntityAnimDesc)
		return 0;

	const __int64 desc = v_GetEntityAnimDesc(entity);

	// Zero already means "no data" to every caller; leave it alone.
	if (!desc || !sdk_anim_desc_guard.GetBool())
		return desc;

	if (IsPlausibleAddress(static_cast<unsigned __int64>(desc)) &&
		HasCallableSlots(desc) &&
		HasConsistentBlob(desc))
		return desc;

	if (s_logBudget > 0)
	{
		--s_logBudget;

		const void* vtable = nullptr;
		__try { vtable = entity ? *reinterpret_cast<void* const*>(entity) : nullptr; }
		__except (EXCEPTION_EXECUTE_HANDLER) { vtable = nullptr; }

		Warning(eDLL_T::CLIENT,
			"[ANIM-DESC-GUARD] rejected descriptor %p for entity %p (vtable %p) "
			"-- entity skipped this frame\n",
			reinterpret_cast<void*>(desc), entity, vtable);
	}

	return 0;
}

void VAnimDescGuardS21::GetAdr(void) const
{
	LogFunAdr("GetEntityAnimDesc", v_GetEntityAnimDesc);
}

void VAnimDescGuardS21::GetFun(void) const
{
	// Unique on the leading virtual call followed by the reload of the entity
	// pointer and the second dispatch through a 32-bit vtable displacement.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC ?? 48 8B 01 48 8B D9 FF 90 ?? ?? ?? ?? "
		"48 8B 13 48 8B CB 84 C0 74 ?? FF 92 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? "
		"48 8B 80")
		.GetPtr(v_GetEntityAnimDesc);

	if (!v_GetEntityAnimDesc)
		Warning(eDLL_T::CLIENT,
			"[ANIM-DESC-GUARD] pattern UNRESOLVED -- guard NOT installed\n");
}

void VAnimDescGuardS21::Detour(const bool bAttach) const
{
	if (v_GetEntityAnimDesc)
		DetourSetup(&v_GetEntityAnimDesc, &Hook_GetEntityAnimDesc, bAttach);
}
