//=============================================================================//
//
// Purpose: Validate an effect asset's baked child list before the pak
//          'particle effect' publish callback walks it.
//
// Each slot of the 24-byte asset header's child array is a GUID that pak fixup
// rewrites into a pointer to the child's own 32-byte effect asset, whose
// particle definition is then read at +0x18. A child whose asset was allocated
// but never published still holds uninitialised bytes there. Every slot is
// checked first; one bad slot names the parent effect in the log and drops
// that effect's whole child list, costing only its attached children.
//
//=============================================================================//
#include "core/stdafx.h"
#include "core/logdef.h"
#include "tier0/dbg.h"
#include "tier0/memaddr.h"
#include "tier0/module.h"
#include "tier1/cvar.h"
#include "efct_child_link.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// EffectAsset: childRefs, assetRefs, childRefCount, assetRefCount, pDef.
static constexpr size_t EFFECT_CHILD_REFS = 0x00;
static constexpr size_t EFFECT_ASSET_REFS = 0x08;
static constexpr size_t EFFECT_CHILD_COUNT = 0x10;
static constexpr size_t EFFECT_DEF = 0x18;

// C_ParticleSystemDefinition: the name string and the flag byte the publish
// callback reads off every child definition.
static constexpr size_t PARTICLE_DEF_NAME = 504;
static constexpr size_t PARTICLE_DEF_NAME_SET = 528;
static constexpr size_t PARTICLE_DEF_FLAGS = 992;

static constexpr unsigned __int64 USER_ADDR_FLOOR = 0x0000000000010000ULL;
static constexpr unsigned __int64 USER_ADDR_LIMIT = 0x00007FFFFFFFFFFFULL;

static ConVar sdk_efct_child_link_guard("sdk_efct_child_link_guard", "1", FCVAR_RELEASE,
	"Drop an efct's child list when a child asset is not published yet. 0 = pass-through.");

static ConVar sdk_efct_report_unresolved("sdk_efct_report_unresolved", "0", FCVAR_DEVELOPMENTONLY,
	"Report effect material/model refs that pak fixup could not resolve.");

static ConVar sdk_efct_xyz_guard("sdk_efct_xyz_guard", "1", FCVAR_RELEASE,
	"Retarget a collection XYZ/PREV pointer that cannot hold one 3-wide SIMD group (48 B) to the constant block. 0 = pass-through.");

typedef void(__fastcall* PFN_EffectPublish)(unsigned __int64, unsigned __int64, unsigned __int64);
static PFN_EffectPublish v_EffectPublish = nullptr;

typedef __int64(__fastcall* PFN_InitStorage)(unsigned __int64, unsigned __int64);
static PFN_InitStorage v_InitStorage = nullptr;

// C_ParticleCollection after InitStorage.
static constexpr size_t COL_DEF = 0x78;
static constexpr size_t COL_MEM_SIZE = 0x1D0;
static constexpr size_t COL_MEM = 0x1D8;
static constexpr size_t COL_INIT_SIZE = 0x1E0;
static constexpr size_t COL_INIT_MEM = 0x1E8;
static constexpr size_t COL_XYZ = 0x210;
static constexpr size_t COL_PREV = 0x220;
static constexpr size_t COL_STRIDE_XYZ = 0x2E8;
static constexpr size_t COL_STRIDE_PREV = 0x2F8;
static constexpr size_t COL_CONSTANT = 0x720;
static constexpr size_t CONSTANT_BYTES = 0x520;
static constexpr size_t XYZ_GROUP_BYTES = 48;

static int s_logBudget = 64;
static int s_refLogBudget = 128;

static bool IsPlausiblePointer(const unsigned __int64 p)
{
	return p >= USER_ADDR_FLOOR && p <= USER_ADDR_LIMIT && (p & 7) == 0;
}

static bool ReadQword(const unsigned __int64 at, unsigned __int64& out)
{
	if (!IsPlausiblePointer(at))
		return false;

	__try
	{
		out = *reinterpret_cast<const unsigned __int64*>(at);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

// A published child reads back a definition whose name slot and flag byte are
// both reachable; an unpublished one is whatever the pool allocator left behind.
static bool ChildIsPublished(const unsigned __int64 child)
{
	unsigned __int64 def = 0;
	if (!ReadQword(child + EFFECT_DEF, def))
		return false;
	if (!IsPlausiblePointer(def))
		return false;

	unsigned __int64 nameSet = 0;
	if (!ReadQword(def + PARTICLE_DEF_NAME_SET, nameSet))
		return false;

	unsigned __int64 scratch = 0;
	if (!ReadQword(def + PARTICLE_DEF_FLAGS, scratch))
		return false;

	// Publish reads the name string next and hands it to the dictionary, so a
	// mapped-but-meaningless def has to fail here rather than one call later.
	if (nameSet)
	{
		unsigned __int64 name = 0;
		if (!ReadQword(def + PARTICLE_DEF_NAME, name))
			return false;
		if (name < USER_ADDR_FLOOR || name > USER_ADDR_LIMIT)
			return false;
		if (!ReadQword(name & ~7ull, scratch))
			return false;
	}

	return true;
}

static const char* ParentName(const unsigned __int64 effect)
{
	unsigned __int64 def = 0;
	if (!ReadQword(effect + EFFECT_DEF, def) || !IsPlausiblePointer(def))
		return nullptr;

	unsigned __int64 nameSet = 0;
	if (!ReadQword(def + PARTICLE_DEF_NAME_SET, nameSet) || !nameSet)
		return nullptr;

	unsigned __int64 name = 0;
	if (!ReadQword(def + PARTICLE_DEF_NAME, name) || !IsPlausiblePointer(name))
		return nullptr;

	return reinterpret_cast<const char*>(name);
}

// A resolved ref is a pointer to the target's live header; anything pak fixup could not find is
// left as the raw GUID, and the S21 client's "Failed to find referenced asset" assert is compiled out.
// A 64-bit hash sets bits above the user address range essentially always, so the two are separable.
static void ReportUnresolvedAssetRefs(const unsigned __int64 effect)
{
	unsigned __int64 refs = 0;
	unsigned __int64 counts = 0;
	if (!ReadQword(effect + EFFECT_ASSET_REFS, refs)
		|| !ReadQword(effect + EFFECT_CHILD_COUNT, counts))
		return;

	const unsigned int count = static_cast<unsigned int>(counts >> 32);
	if (!count || !IsPlausiblePointer(refs))
		return;

	for (unsigned int i = 0; i < count; ++i)
	{
		unsigned __int64 slot = 0;
		if (!ReadQword(refs + 8ull * i, slot))
			return;

		if (slot && (slot > USER_ADDR_LIMIT || slot < USER_ADDR_FLOOR))
		{
			if (s_refLogBudget <= 0)
				return;

			--s_refLogBudget;
			const char* const name = ParentName(effect);
			Warning(eDLL_T::CLIENT,
				"[EFCT-REF] '%s' ref %u/%u UNRESOLVED guid=0x%016llX\n",
				name ? name : "?", i, count, slot);
		}
	}
}

static void __fastcall Hook_EffectPublish(unsigned __int64 live, unsigned __int64 incoming,
	unsigned __int64 outgoing)
{
	if (incoming && sdk_efct_report_unresolved.GetBool())
		ReportUnresolvedAssetRefs(incoming);

	if (incoming && sdk_efct_child_link_guard.GetBool())
	{
		unsigned __int64 refs = 0;
		unsigned __int64 count = 0;
		if (ReadQword(incoming + EFFECT_CHILD_REFS, refs)
			&& ReadQword(incoming + EFFECT_CHILD_COUNT, count))
		{
			count = static_cast<unsigned int>(count);
			static constexpr unsigned kEfctChildWalkCap = 4096;
			if (count > kEfctChildWalkCap)
			{
				if (s_logBudget > 0)
				{
					--s_logBudget;
					const char* const name = ParentName(incoming);
					Warning(eDLL_T::CLIENT,
						"[EFCT-LINK] '%s' child count %u walk-capped to %u\n",
						name ? name : "?", count, kEfctChildWalkCap);
				}
				count = kEfctChildWalkCap;
			}

			bool drop = !IsPlausiblePointer(refs) && count != 0;
			unsigned __int64 bad = 0;
			unsigned int badIndex = 0;

			for (unsigned int i = 0; !drop && i < count; ++i)
			{
				unsigned __int64 child = 0;
				if (!ReadQword(refs + 8ull * i, child) || !ChildIsPublished(child))
				{
					drop = true;
					bad = child;
					badIndex = i;
				}
			}

			if (drop)
			{
				if (s_logBudget > 0)
				{
					--s_logBudget;
					const char* const name = ParentName(incoming);
					Warning(eDLL_T::CLIENT,
						"[EFCT-LINK] '%s' child %u/%llu unusable (asset=0x%llX) -- dropping child list\n",
						name ? name : "?", badIndex, count, bad);
				}

				__try
				{
					*reinterpret_cast<unsigned int*>(incoming + EFFECT_CHILD_COUNT) = 0;
				}
				__except (EXCEPTION_EXECUTE_HANDLER)
				{
				}
			}
		}
	}

	if (v_EffectPublish)
		v_EffectPublish(live, incoming, outgoing);
}

static size_t ArenaRemain(const unsigned __int64 p, const unsigned __int64 base, const size_t bytes)
{
	if (!p || !base || !IsPlausiblePointer(base) || p < base)
		return 0;
	const unsigned __int64 end = base + bytes;
	if (p >= end)
		return 0;
	return static_cast<size_t>(end - p);
}

static size_t XyzRemain(const unsigned __int64 collection, const unsigned __int64 p)
{
	unsigned __int64 mem = 0;
	unsigned __int64 initMem = 0;
	unsigned __int64 constant = 0;
	int memSize = 0;
	int initSize = 0;
	ReadQword(collection + COL_MEM, mem);
	ReadQword(collection + COL_INIT_MEM, initMem);
	ReadQword(collection + COL_CONSTANT, constant);
	memSize = *reinterpret_cast<const int*>(collection + COL_MEM_SIZE);
	initSize = *reinterpret_cast<const int*>(collection + COL_INIT_SIZE);

	size_t remain = 0;
	if (memSize > 0)
		remain = ArenaRemain(p, mem, static_cast<size_t>(memSize));
	if (!remain && initSize > 0)
		remain = ArenaRemain(p, initMem, static_cast<size_t>(initSize));
	if (!remain)
		remain = ArenaRemain(p, constant, CONSTANT_BYTES);
	return remain;
}

static const char* DefName(const unsigned __int64 def)
{
	if (!IsPlausiblePointer(def))
		return nullptr;

	unsigned __int64 nameSet = 0;
	if (!ReadQword(def + PARTICLE_DEF_NAME_SET, nameSet) || !nameSet)
		return nullptr;

	unsigned __int64 name = 0;
	if (!ReadQword(def + PARTICLE_DEF_NAME, name) || !IsPlausiblePointer(name))
		return nullptr;
	return reinterpret_cast<const char*>(name);
}

// Simulate always reads [ptr+0x10] (the Y FourVectors). A 16-byte heap is one
// scalar SIMD group; retarget must be the 0x520 constant block, not a null.
static void SanitizeXyzSlot(const unsigned __int64 collection, const size_t ptrOff,
	const size_t strideOff, const unsigned __int64 constant, const char* which)
{
	unsigned __int64 ptr = 0;
	if (!ReadQword(collection + ptrOff, ptr) || !ptr)
		return;

	const size_t remain = XyzRemain(collection, ptr);
	if (remain >= XYZ_GROUP_BYTES)
		return;
	if (!constant || !IsPlausiblePointer(constant))
		return;

	*reinterpret_cast<unsigned __int64*>(collection + ptrOff) = constant;
	*reinterpret_cast<unsigned __int64*>(collection + strideOff) = 0;

	static int s_xyzLog = 64;
	if (s_xyzLog <= 0)
		return;
	--s_xyzLog;

	unsigned __int64 def = 0;
	ReadQword(collection + COL_DEF, def);
	const char* name = DefName(def);
	Warning(eDLL_T::CLIENT,
		"[EFCT-XYZ] %s remain=%zu ptr=0x%llX -> constant 0x%llX '%s'\n",
		which, remain, ptr, constant, name ? name : "?");
}

static __int64 __fastcall Hook_InitStorage(unsigned __int64 collection, unsigned __int64 pDef)
{
	const __int64 result = v_InitStorage(collection, pDef);

	if (!sdk_efct_xyz_guard.GetBool() || !collection)
		return result;

	unsigned __int64 constant = 0;
	if (!ReadQword(collection + COL_CONSTANT, constant))
		constant = 0;

	__try
	{
		SanitizeXyzSlot(collection, COL_XYZ, COL_STRIDE_XYZ, constant, "XYZ");
		SanitizeXyzSlot(collection, COL_PREV, COL_STRIDE_PREV, constant, "PREV");
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}

	return result;
}

void VEffectChildLinkGuardS21::GetAdr(void) const
{
	LogFunAdr("EffectAssetPublish", v_EffectPublish);
	LogFunAdr("ParticleInitStorage", v_InitStorage);
}

void VEffectChildLinkGuardS21::GetFun(void) const
{
	// Unique: prologue through mov rbx,r8 / mov rdi,rdx / mov rsi,rcx / test r8,r8
	Module_FindPattern(g_GameDll,
		"40 53 55 56 57 41 57 48 83 EC ?? 4C 8D 3D ?? ?? ?? ?? 49 8B D8 48 8B FA 48 8B F1 4D 85 C0")
		.GetPtr(v_EffectPublish);

	if (!v_EffectPublish)
		Warning(eDLL_T::CLIENT,
			"[EFCT-LINK] pattern UNRESOLVED -- guard NOT installed\n");

	// InitStorage: unique through cmp [rip] TrackFeature byte. Confirmed 1 hit.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 54 24 ?? 55 56 57 41 54 41 55 41 56 41 57 48 83 EC ?? 80 3D")
		.GetPtr(v_InitStorage);

	if (!v_InitStorage)
		Warning(eDLL_T::CLIENT,
			"[EFCT-XYZ] InitStorage pattern UNRESOLVED -- xyz guard NOT installed\n");
}

void VEffectChildLinkGuardS21::Detour(const bool bAttach) const
{
	if (v_EffectPublish)
		DetourSetup(&v_EffectPublish, &Hook_EffectPublish, bAttach);
	if (v_InitStorage)
		DetourSetup(&v_InitStorage, &Hook_InitStorage, bAttach);
}
