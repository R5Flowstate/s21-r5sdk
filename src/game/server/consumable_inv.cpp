//=============================================================================//
//
// Purpose: full-width S21 consumable types behind the native byte-truncated
// inventory (see consumable_inv.h). Mid-function capture on Set/Get only.
//
//=============================================================================//
#include "core/stdafx.h"


#include "consumable_inv.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/detours.h"

#include <atomic>

// ---------------------------------------------------------------------------
// Layout (S3 native)
// ---------------------------------------------------------------------------
static constexpr ptrdiff_t kConsumableInvOff = 0x5FAC;
static constexpr ptrdiff_t kEntIndexOff      = 0x58;
static constexpr int kConsumableInvSlots     = 32;
static constexpr int kConsumableInvStride    = 2;

// Consumables live on players only, which are always low-index entities. An
// index past this degrades to stock truncation rather than growing a map.
static constexpr int kMaxEntIndex = 256;

// Full S21 type per (entity index, slot). Lock-free and allocation free because
// the wire proxy reads it from the snapshot encode path. Count is never widened:
// the native byte already holds every reachable stack size.
static std::atomic<uint16_t> s_fullType[kMaxEntIndex][kConsumableInvSlots];

static inline uint8_t* NativeSlot(const void* player, int slot)
{
	return const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(player))
		+ kConsumableInvOff + slot * kConsumableInvStride;
}

static inline int EntIndex(const void* player)
{
	const uint16_t idx = *reinterpret_cast<const uint16_t*>(
		reinterpret_cast<const uint8_t*>(player) + kEntIndexOff);

	if (idx == 0xFFFF || idx >= kMaxEntIndex)
		return -1;

	return static_cast<int>(idx);
}

static void ShadowStore(const void* player, int slot, uint16_t fullType)
{
	if (!player || slot < 0 || slot >= kConsumableInvSlots)
		return;

	const int ent = EntIndex(player);
	if (ent < 0)
		return;

	s_fullType[ent][slot].store(fullType, std::memory_order_relaxed);
}

static void ShadowClearRow(const void* player)
{
	if (!player)
		return;

	const int ent = EntIndex(player);
	if (ent < 0)
		return;

	for (int i = 0; i < kConsumableInvSlots; ++i)
		s_fullType[ent][i].store(0, std::memory_order_relaxed);
}

void ConsumableInv_LevelShutdown()
{
	for (int e = 0; e < kMaxEntIndex; ++e)
	{
		for (int i = 0; i < kConsumableInvSlots; ++i)
			s_fullType[e][i].store(0, std::memory_order_relaxed);
	}
}

uint16_t ConsumableInv_ResolveTypeByEnt(int ent, int slot, uint16_t nativeType)
{
	if (ent < 0 || ent >= kMaxEntIndex || slot < 0 || slot >= kConsumableInvSlots)
		return nativeType;

	const uint16_t full = s_fullType[ent][slot].load(std::memory_order_relaxed);

	// Shadow high byte only; require low byte still matches native or degrade to truncation.
	if (full && (full & 0xFF) == (nativeType & 0xFF))
		return full;

	return nativeType;
}

uint16_t ConsumableInv_ResolveType(const void* player, int slot, uint16_t nativeType)
{
	if (!player)
		return nativeType;

	return ConsumableInv_ResolveTypeByEnt(EntIndex(player), slot, nativeType);
}

// ---------------------------------------------------------------------------
// Engine natives
// ---------------------------------------------------------------------------
static int64_t (*v_ConsumableInventory_Set)(void* player, void* vm) = nullptr;
static int64_t (*v_ConsumableInventory_Get)(void* player, void* vm) = nullptr;
static int64_t (*v_ConsumableInventory_Add)(void* player, int type, int slot) = nullptr;

// ---------------------------------------------------------------------------
// Mid-function capture points (Microsoft x64 ABI, called from the trampolines)
// ---------------------------------------------------------------------------
static void __fastcall ConsumableInv_OnSetStore(void* player, uint64_t slot, uint32_t type)
{
	if (slot >= static_cast<uint64_t>(kConsumableInvSlots))
		return;

	// A type past u16 cannot be cached; leaving the entry at 0 falls back to the
	// stock byte rather than serving a wrapped value.
	if (type > 0xFFFF)
		return;

	ShadowStore(player, static_cast<int>(slot), static_cast<uint16_t>(type));
}

// Return value replaces the truncated byte Get was about to push.
static uint32_t __fastcall ConsumableInv_OnGetType(void* player, uint32_t slot, uint32_t nativeType)
{
	if (slot >= static_cast<uint32_t>(kConsumableInvSlots))
		return nativeType;

	return ConsumableInv_ResolveType(player, static_cast<int>(slot),
		static_cast<uint16_t>(nativeType));
}

// Dedicated RX page. Do not VirtualProtect a static -- protection is page-granular.
static uint8_t* s_codePage = nullptr;
static size_t   s_codeUsed = 0;
static constexpr size_t kCodePageSize = 4096;

static uint8_t* CodeAlloc(size_t len)
{
	if (!s_codePage)
	{
		// Executable from the first byte: a site is patched to call into this
		// page as soon as its trampoline lands, so the page must never be
		// non-executable while a live call site points at it.
		s_codePage = reinterpret_cast<uint8_t*>(VirtualAlloc(nullptr, kCodePageSize,
			MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));

		if (!s_codePage)
		{
			Warning(eDLL_T::SERVER, "[CONSUMABLEINV-FULL] trampoline page alloc failed\n");
			return nullptr;
		}
	}

	if (s_codeUsed + len > kCodePageSize)
		return nullptr;

	uint8_t* const out = s_codePage + s_codeUsed;
	s_codeUsed += len;
	return out;
}

static void CodeSeal(void)
{
	if (!s_codePage)
		return;

	DWORD oldProt = 0;
	VirtualProtect(s_codePage, kCodePageSize, PAGE_EXECUTE_READ, &oldProt);
	FlushInstructionCache(GetCurrentProcess(), s_codePage, kCodePageSize);
}

// Save every volatile the callback may clobber, reserve the 32 bytes of home
// space the callee spills into, and land the inner call on a 16-byte boundary:
// site RSP%16 == 0, minus the call (8), minus 6 pushes (48), minus 0x28 == 0.
static const uint8_t kSaveVolatile[] = {
	0x51,                     // push rcx
	0x52,                     // push rdx
	0x41, 0x50,               // push r8
	0x41, 0x51,               // push r9
	0x41, 0x52,               // push r10
	0x41, 0x53,               // push r11
	0x48, 0x83, 0xEC, 0x28    // sub  rsp, 28h
};

static const uint8_t kRestoreVolatile[] = {
	0x48, 0x83, 0xC4, 0x28,   // add  rsp, 28h
	0x41, 0x5B,               // pop  r11
	0x41, 0x5A,               // pop  r10
	0x41, 0x59,               // pop  r9
	0x41, 0x58,               // pop  r8
	0x5A,                     // pop  rdx
	0x59                      // pop  rcx
};

struct CodeWriter_t
{
	uint8_t* m_pBuf;
	int      m_nLen;

	inline void Emit(const void* src, size_t len)
	{
		memcpy(m_pBuf + m_nLen, src, len);
		m_nLen += static_cast<int>(len);
	}
	inline void Call(const void* fn)
	{
		const uint8_t movRax[] = { 0x48, 0xB8 };
		const uint64_t imm = reinterpret_cast<uint64_t>(fn);
		const uint8_t callRax[] = { 0xFF, 0xD0 };
		Emit(movRax, sizeof(movRax));
		Emit(&imm, sizeof(imm));
		Emit(callRax, sizeof(callRax));
	}
};

// Set store site, 19 bytes, unique. EDX is live (helper(vm, 2) three bytes past).
static const char* const kSetStorePattern =
	"43 88 B4 46 AC 5F 00 00 48 8B CB 43 88 BC 46 AD 5F 00 00";
static constexpr int kSetStoreLen = 19;

// Get type site, 16 bytes. Not unique; bounded scan inside Get. movzx eax, bl is the truncated type.
static const char* const kGetTypePattern =
	"8B 4F 78 0F B6 C3 4C 89 7D E8 89 45 E8 8D 41 01";
static constexpr int kGetTypeLen = 16;

static uint8_t  s_setStoreOrig[kSetStoreLen]{};
static uint8_t* s_setStoreSite = nullptr;

static uint8_t  s_getTypeOrig[kGetTypeLen]{};
static uint8_t* s_getTypeSite = nullptr;

static bool WriteCode(uint8_t* site, const uint8_t* bytes, int len)
{
	DWORD oldProt = 0;
	if (!VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::SERVER, "[CONSUMABLEINV-FULL] VirtualProtect(%p, %d) failed\n", site, len);
		return false;
	}

	memcpy(site, bytes, len);
	VirtualProtect(site, len, oldProt, &oldProt);
	FlushInstructionCache(GetCurrentProcess(), site, len);
	return true;
}

static bool InstallSetStorePatch(uint8_t* site)
{
	if (!site || s_setStoreSite)
		return s_setStoreSite != nullptr;

	uint8_t tramp[96]{};
	CodeWriter_t w{ tramp, 0 };
	const uint8_t argSetup[] = {
		0x4C, 0x89, 0xF1,   // mov rcx, r14  (player)
		0x4C, 0x89, 0xC2,   // mov rdx, r8   (slot)
		0x41, 0x89, 0xF0    // mov r8d, esi  (full type)
	};
	const uint8_t ret[] = { 0xC3 };

	w.Emit(kSaveVolatile, sizeof(kSaveVolatile));
	w.Emit(argSetup, sizeof(argSetup));
	w.Call(reinterpret_cast<const void*>(&ConsumableInv_OnSetStore));
	w.Emit(kRestoreVolatile, sizeof(kRestoreVolatile));
	w.Emit(site + 0, 8);    // replay: mov [r14+r8*2+5FACh], sil
	w.Emit(site + 11, 8);   // replay: mov [r14+r8*2+5FADh], dil
	w.Emit(ret, sizeof(ret));

	uint8_t* const pTramp = CodeAlloc(w.m_nLen);
	if (!pTramp)
	{
		Warning(eDLL_T::SERVER, "[CONSUMABLEINV-FULL] Set trampoline alloc failed\n");
		return false;
	}
	memcpy(pTramp, tramp, w.m_nLen);

	// The site's own `mov rcx, rbx` is kept after the call so the following
	// engine helper still gets the vm in RCX.
	uint8_t patch[kSetStoreLen];
	CodeWriter_t p{ patch, 0 };
	p.Call(pTramp);
	p.Emit(site + 8, 3);
	while (p.m_nLen < kSetStoreLen)
		patch[p.m_nLen++] = 0x90;

	memcpy(s_setStoreOrig, site, kSetStoreLen);
	if (!WriteCode(site, patch, kSetStoreLen))
		return false;

	s_setStoreSite = site;
	return true;
}

static bool InstallGetTypePatch(uint8_t* site)
{
	if (!site || s_getTypeSite)
		return s_getTypeSite != nullptr;

	uint8_t tramp[96]{};
	CodeWriter_t w{ tramp, 0 };
	const uint8_t argSetup[] = {
		0x4C, 0x89, 0xF1,         // mov   rcx, r14  (player)
		0x8B, 0xD6,               // mov   edx, esi  (slot)
		0x44, 0x0F, 0xB6, 0xC3    // movzx r8d, bl   (native type byte)
	};
	const uint8_t ret[] = { 0xC3 };

	w.Emit(kSaveVolatile, sizeof(kSaveVolatile));
	w.Emit(argSetup, sizeof(argSetup));
	w.Call(reinterpret_cast<const void*>(&ConsumableInv_OnGetType)); // -> eax
	w.Emit(kRestoreVolatile, sizeof(kRestoreVolatile));
	w.Emit(site + 0, 3);    // replay: mov ecx, [rdi+78h] (the callback clobbered it)
	w.Emit(site + 6, 10);   // replay: the two stores + lea, minus the movzx
	w.Emit(ret, sizeof(ret));

	uint8_t* const pTramp = CodeAlloc(w.m_nLen);
	if (!pTramp)
	{
		Warning(eDLL_T::SERVER, "[CONSUMABLEINV-FULL] Get trampoline alloc failed\n");
		return false;
	}
	memcpy(pTramp, tramp, w.m_nLen);

	uint8_t patch[kGetTypeLen];
	CodeWriter_t p{ patch, 0 };
	p.Call(pTramp);
	while (p.m_nLen < kGetTypeLen)
		patch[p.m_nLen++] = 0x90;

	memcpy(s_getTypeOrig, site, kGetTypeLen);
	if (!WriteCode(site, patch, kGetTypeLen))
		return false;

	s_getTypeSite = site;
	return true;
}

static void RemoveMidFunctionPatches(void)
{
	if (s_setStoreSite)
	{
		WriteCode(s_setStoreSite, s_setStoreOrig, kSetStoreLen);
		s_setStoreSite = nullptr;
	}
	if (s_getTypeSite)
	{
		WriteCode(s_getTypeSite, s_getTypeOrig, kGetTypeLen);
		s_getTypeSite = nullptr;
	}
}

// ---------------------------------------------------------------------------
// Function-level hooks
// ---------------------------------------------------------------------------
static int64_t __fastcall Hook_ConsumableInventory_Set(void* player, void* vm)
{
	// Stock's first act is to zero every slot's count byte. Drop the shadow row
	// with it so a slot this call leaves empty cannot serve a stale full type.
	ShadowClearRow(player);
	return v_ConsumableInventory_Set(player, vm);
}

static int64_t __fastcall Hook_ConsumableInventory_Add(void* player, int type, int slotArg)
{
	if (!player)
		return v_ConsumableInventory_Add(player, type, slotArg);

	int target = slotArg;
	if (target < 0)
	{
		// slot < 0 with a full inventory: stock indexes OOB. Refuse.
		target = -1;
		for (int i = 0; i < kConsumableInvSlots; ++i)
		{
			if (NativeSlot(player, i)[1] == 0)
			{
				target = i;
				break;
			}
		}

		if (target < 0)
		{
			static bool s_warnedFull = false;
			if (!s_warnedFull)
			{
				s_warnedFull = true;
				Warning(eDLL_T::SERVER,
					"[CONSUMABLEINV-FULL] Add(type=%d) refused: inventory full and no slot given\n",
					type);
			}
			return -1;
		}
	}

	if (target >= kConsumableInvSlots || type < 0)
		return v_ConsumableInventory_Add(player, type, slotArg); // stock emits the range error

	const uint8_t* const p = NativeSlot(player, target);

	// Stock compares truncated type byte; feed truncated once the shadow matches so stacking works.
	int stockType = type;
	if (p[1] != 0 &&
		ConsumableInv_ResolveType(player, target, p[0]) == static_cast<uint16_t>(type))
	{
		stockType = type & 0xFF;
	}

	const int64_t result = v_ConsumableInventory_Add(player, stockType, target);

	if (NativeSlot(player, target)[1] != 0 && type <= 0xFFFF)
		ShadowStore(player, target, static_cast<uint16_t>(type));

	return result;
}

// ---------------------------------------------------------------------------
// Detour class
// ---------------------------------------------------------------------------
void VConsumableInvBridge::GetAdr(void) const
{
	LogFunAdr("CPlayer::Script_ConsumableInventory_Set", v_ConsumableInventory_Set);
	LogFunAdr("CPlayer::Script_ConsumableInventory_Get", v_ConsumableInventory_Get);
	LogFunAdr("CPlayer::Script_ConsumableInventory_Add", v_ConsumableInventory_Add);
	LogVarAdr("ConsumableInventory_SetStoreSite", s_setStoreSite);
	LogVarAdr("ConsumableInventory_GetTypeSite", s_getTypeSite);
}

void VConsumableInvBridge::GetFun(void) const
{
	// Script_ConsumableInventory_Set
	Module_FindPattern(g_GameDll,
		"40 53 41 54 41 55 41 56 41 57 48 83 EC ?? 45 33 ED 48 8B DA")
		.GetPtr(v_ConsumableInventory_Set);
	if (!v_ConsumableInventory_Set)
		Warning(eDLL_T::SERVER, "[CONSUMABLEINV-FULL] Set pattern unresolved\n");

	// Script_ConsumableInventory_Add
	Module_FindPattern(g_GameDll, "44 8B D2 4C 8B C9 41 83 F8")
		.GetPtr(v_ConsumableInventory_Add);
	if (!v_ConsumableInventory_Add)
		Warning(eDLL_T::SERVER, "[CONSUMABLEINV-FULL] Add pattern unresolved\n");

	// Script_ConsumableInventory_Get
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 55 41 56 41 57 48 8B EC 48 83 EC ?? "
		"48 8B FA 4C 8B F1")
		.GetPtr(v_ConsumableInventory_Get);
	if (!v_ConsumableInventory_Get)
		Warning(eDLL_T::SERVER, "[CONSUMABLEINV-FULL] Get pattern unresolved\n");

	// Both capture points are located from inside their own function, so a
	// pattern that also occurs elsewhere cannot bind the wrong site.
	if (v_ConsumableInventory_Set)
	{
		uint8_t* const site = CMemory(reinterpret_cast<uintptr_t>(v_ConsumableInventory_Set))
			.FindPattern(kSetStorePattern, CMemory::Direction::DOWN, 0x600).RCast<uint8_t*>();

		if (!site || !InstallSetStorePatch(site))
			Warning(eDLL_T::SERVER,
				"[CONSUMABLEINV-FULL] Set store capture failed -- script Set leaves loot "
				"indices >= 256 truncated\n");
	}

	if (v_ConsumableInventory_Get)
	{
		uint8_t* const site = CMemory(reinterpret_cast<uintptr_t>(v_ConsumableInventory_Get))
			.FindPattern(kGetTypePattern, CMemory::Direction::DOWN, 0x200).RCast<uint8_t*>();

		if (!site || !InstallGetTypePatch(site))
			Warning(eDLL_T::SERVER,
				"[CONSUMABLEINV-FULL] Get type capture failed -- script Get returns "
				"truncated loot indices\n");
	}

	CodeSeal();
}

void VConsumableInvBridge::Detour(const bool bAttach) const
{
	if (!bAttach)
		RemoveMidFunctionPatches();

	if (v_ConsumableInventory_Set)
		DetourSetup(&v_ConsumableInventory_Set, &Hook_ConsumableInventory_Set, bAttach);
	if (v_ConsumableInventory_Add)
		DetourSetup(&v_ConsumableInventory_Add, &Hook_ConsumableInventory_Add, bAttach);
}

