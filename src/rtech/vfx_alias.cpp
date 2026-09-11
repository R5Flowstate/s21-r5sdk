//=============================================================================//
//
// Purpose: Detour to guard + name the skin VFX-alias NULL
// settings deref (weapon-inspect crash). See vfx_alias.h.
//
//=============================================================================//

#include "core/stdafx.h"
#include "core/logdef.h"
#include "tier0/dbg.h"
#include "tier0/memaddr.h"
#include "tier0/module.h"
#include "tier1/cvar.h"
#include "vfx_alias.h"

// On by default: prevent the NULL-settings-root deref in the skin VFX-alias
// resolver and name the offending cosmetic. 0 = pass-through (let it crash).
static ConVar sdk_vfx_alias_guard(
	"sdk_vfx_alias_guard", "1", FCVAR_RELEASE,   // default ON: shield the weapon-inspect NULL-settings deref
	"Guard the weapon-inspect skin VFX-alias resolver : when"
	"the skin's settings asset is not loaded ( == NULL), skip the"
	"alias apply and log the offending skinId/asset instead of AV-crashing. "
	"0 = pass-through.");

// Entity field offsets used by (verified from the ).
static constexpr ptrdiff_t ENT_OFF_VFX_EHANDLE = 0x1CF4; // owner EHANDLE (serial+index)
static constexpr ptrdiff_t ENT_OFF_VFX_NETID   = 0xD8;   // uint16 id, a1 vs owner compare
static constexpr ptrdiff_t ENT_OFF_VFX_SKINID  = 0xD5C;  // uint32 skin/cosmetic id
static constexpr ptrdiff_t DESC_OFF_NAME       = 0x10;   // const char* settings asset name

// 4th arg is saved then reused as a loop counter; forward it to preserve ABI.
typedef void (__fastcall* PFN_VfxAliasResolve)(int64_t /*a1*/, char /*a2*/, int64_t /*a3*/, uint32_t /*a4*/);
static PFN_VfxAliasResolve v_VfxAliasResolve = nullptr;

// skinId -> settings descriptor. descriptor+0x10 = name string.
typedef int64_t (__fastcall* PFN_SettingsDescById)(uint32_t /*skinId*/);
static PFN_SettingsDescById v_SettingsDescById = nullptr;

// String -> settings handle. The engine picks the tagged variant when the
// low 2 bits of the string pointer are set (interned form).
typedef int64_t (__fastcall* PFN_SettingsHash)(const char* /*str*/);
static PFN_SettingsHash v_SettingsHashStr = nullptr;       // (untagged)
static PFN_SettingsHash v_SettingsHashStrTagged = nullptr; // (tagged)

// Settings/RSON find-root. Returns NULL when not loaded; a2
// is an optional out-handle (engine passes NULL here, rdx=0).
typedef int64_t (__fastcall* PFN_SettingsFindRoot)(int64_t /*handle*/, uint32_t* /*outId*/);
static PFN_SettingsFindRoot v_SettingsFindRoot = nullptr;

//: base of the EHANDLE table, 0x20-byte entries (+0 = entity
// ptr, +8 = serial). g_pLocaleStr: &Locale, the asset-name fallback string.
static uint8_t*    g_pEHandleTable = nullptr;
static const char* g_pLocaleStr    = nullptr;

static void __fastcall Hook_VfxAliasResolve(int64_t a1, char a2, int64_t a3, uint32_t a4)
{
	// Fast pass-through: disabled, the gated branch isn't taken (a2 & 8), or
	// any engine primitive failed to resolve -- never silently change behavior.
	if (!sdk_vfx_alias_guard.GetBool() || (a2 & 8) == 0 || !a1 ||
		!g_pEHandleTable || !v_SettingsFindRoot || !v_SettingsDescById ||
		!v_SettingsHashStr || !v_SettingsHashStrTagged || !g_pLocaleStr)
	{
		v_VfxAliasResolve(a1, a2, a3, a4);
		return;
	}

	// Replicate the engine prologue (read-only): EHANDLE (a1+0x1CF4) -> owner.
	int64_t owner = 0;
	const uint32_t handle = *reinterpret_cast<const uint32_t*>(a1 + ENT_OFF_VFX_EHANDLE);
	if (handle != 0xFFFFFFFFu)
	{
		const uint64_t idx = static_cast<uint16_t>(handle);
		const uint8_t* const ent = g_pEHandleTable + (idx << 5); // 0x20-byte stride
		if (*reinterpret_cast<const uint32_t*>(ent + 8) == (handle >> 16))
			owner = *reinterpret_cast<const int64_t*>(ent);
	}

	// The settings resolve is only reached when the engine's id-match holds
	// (a1+0xD8 == owner+0xD8). If it won't, run the original untouched.
	if (!owner ||
		*reinterpret_cast<const uint16_t*>(a1 + ENT_OFF_VFX_NETID) !=
		*reinterpret_cast<const uint16_t*>(owner + ENT_OFF_VFX_NETID))
	{
		v_VfxAliasResolve(a1, a2, a3, a4);
		return;
	}

	// skinId -> descriptor -> asset-name string (or &Locale when no descriptor).
	const uint32_t skinId = *reinterpret_cast<const uint32_t*>(owner + ENT_OFF_VFX_SKINID);
	const int64_t  desc   = v_SettingsDescById(skinId);
	const char*    assetName = g_pLocaleStr;
	if (desc)
		assetName = *reinterpret_cast<const char* const*>(desc + DESC_OFF_NAME);

	// Hash exactly as the engine does, then probe the settings root. The crash
	// reduces to this single condition (the engine's retry re-hashes the same
	// deterministic string), so a NULL here means the engine WILL deref NULL+8.
	const int64_t settingsHandle = (reinterpret_cast<uintptr_t>(assetName) & 3)
		? v_SettingsHashStrTagged(assetName)
		: v_SettingsHashStr(assetName);

	if (v_SettingsFindRoot(settingsHandle, nullptr) != 0)
	{
		// Asset present -- safe to run the real resolver.
		v_VfxAliasResolve(a1, a2, a3, a4);
		return;
	}

	// Bad state: skip the vfx-alias apply (no output has been written at the
	// crash site) and name the offending cosmetic so the dedi side can be fixed.
	static volatile LONG s_logCount = 0;
	const LONG n = InterlockedIncrement(&s_logCount);
	if (n <= 200)
	{
		const uintptr_t pName = reinterpret_cast<uintptr_t>(assetName);
		const bool unreadable = (pName & 3) != 0 ||
			pName < 0x10000ULL || pName > 0x7FFFFFFFFFFFULL;
		Warning(eDLL_T::CLIENT,
			"[VFX-ALIAS-GUARD] #%ld NULL settings root -- skipping skin vfx-alias apply. "
			"skinId=%u desc=%p asset='%s' ent=%p owner=%p\n",
			n, skinId, reinterpret_cast<void*>(desc),
			unreadable ? "<tagged/unreadable>" : assetName,
			reinterpret_cast<void*>(a1), reinterpret_cast<void*>(owner));
	}
}

void VVfxAliasNullGuardS21::GetAdr(void) const
{
	LogFunAdr("VfxAliasResolve", v_VfxAliasResolve);
	LogFunAdr("SettingsDescById", v_SettingsDescById);
	LogFunAdr("SettingsHashStr",  v_SettingsHashStr);
	LogFunAdr("SettingsHashStrTagged", v_SettingsHashStrTagged);
	LogFunAdr("SettingsFindRoot", v_SettingsFindRoot);
	LogVarAdr("EHandleTable",   reinterpret_cast<void*>(g_pEHandleTable));
	LogVarAdr("Locale (asset-name fallback)",     reinterpret_cast<const void*>(g_pLocaleStr));
}

void VVfxAliasNullGuardS21::GetFun(void) const
{
	// Unique prologue: test dl,8; jz; home-slot saves; push rdi; sub rsp,70h.

	CMemory base = Module_FindPattern(g_GameDll,
		"F6 C2 08 0F 84 ?? ?? ?? ?? 44 89 4C 24 ?? 4C 89 44 24 ?? 57 48 83 EC 70 "
		"48 8B 01 48 8B F9 48 89 9C 24 ?? ?? ?? ?? 33 DB FF 90 58 01 00 00");

	if (!base.GetPtr())
	{
		Warning(eDLL_T::CLIENT, "[VFX-ALIAS-GUARD] pattern UNRESOLVED\n");
		return;
	}
	base.GetPtr(v_VfxAliasResolve);

	// Navigate to the engine primitives this function uses (single anchor)
	// +0x40 lea rdx, (EHANDLE table base)
	// +0x72 lea rdi, Locale (fallback asset-name string)
	// +0x83 call (skinId -> settings descriptor)
	// +0x95 lea rbx, (string hash, untagged)
	// +0x9C lea r14, (string hash, tagged/interned)
	// +0xB4 call (settings find-root; NULL = not loaded)
	g_pEHandleTable = base.Offset(0x40).ResolveRelativeAddress(0x3, 0x7).RCast<uint8_t*>();
	g_pLocaleStr    = base.Offset(0x72).ResolveRelativeAddress(0x3, 0x7).RCast<const char*>();
	base.Offset(0x83).FollowNearCall().GetPtr(v_SettingsDescById);
	base.Offset(0x95).ResolveRelativeAddress(0x3, 0x7).GetPtr(v_SettingsHashStr);
	base.Offset(0x9C).ResolveRelativeAddress(0x3, 0x7).GetPtr(v_SettingsHashStrTagged);
	base.Offset(0xB4).FollowNearCall().GetPtr(v_SettingsFindRoot);

	if (!g_pEHandleTable || !g_pLocaleStr || !v_SettingsDescById ||
		!v_SettingsHashStr || !v_SettingsHashStrTagged || !v_SettingsFindRoot)
		Warning(eDLL_T::CLIENT,
			"[VFX-ALIAS-GUARD] primitive navigation incomplete "
			"(ehTbl=%p locale=%p desc=%p hash=%p hashT=%p findRoot=%p) -- guard disabled\n",
			reinterpret_cast<void*>(g_pEHandleTable), reinterpret_cast<const void*>(g_pLocaleStr),
			reinterpret_cast<void*>(v_SettingsDescById), reinterpret_cast<void*>(v_SettingsHashStr),
			reinterpret_cast<void*>(v_SettingsHashStrTagged), reinterpret_cast<void*>(v_SettingsFindRoot));
}

void VVfxAliasNullGuardS21::Detour(const bool bAttach) const
{
	if (v_VfxAliasResolve)
		DetourSetup(&v_VfxAliasResolve, &Hook_VfxAliasResolve, bAttach);
}
