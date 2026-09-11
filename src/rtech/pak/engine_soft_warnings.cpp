//=============================================================================//
//
// Purpose: Quiet the weapon-mod "Unrecognized entry" logger CALL.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "engine_soft_warnings.h"

#include <cstdint>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Both weapon-settings parser clones: CALL logger, then JMP back to the loop.
static const uintptr_t kUnrecognizedEntryCalls[] = {
	0x1408e5352ull,
	0x140c75dd2ull,
};
static constexpr uintptr_t kImageBase = 0x140000000ull;

void VEngineSoftWarnings::Detour(const bool bAttach) const
{
	if (!bAttach) return;

	HMODULE hExe = GetModuleHandleA(NULL);
	if (!hExe)
	{
		Warning(eDLL_T::ENGINE,
			"[engine-soft-warn] GetModuleHandleA returned NULL; aborting.\n");
		return;
	}
	const uintptr_t base = reinterpret_cast<uintptr_t>(hExe);

	for (uintptr_t preferredVa : kUnrecognizedEntryCalls)
	{
		uint8_t* p = reinterpret_cast<uint8_t*>(base + (preferredVa - kImageBase));
		if (p[0] != 0xE8)
		{
			Warning(eDLL_T::ENGINE,
				"[engine-soft-warn] CALL signature mismatch @ %p "
				"(got %02X, expected E8); not patching.\n", p, p[0]);
			continue;
		}
		DWORD oldProt = 0;
		if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProt))
		{
			Warning(eDLL_T::ENGINE,
				"[engine-soft-warn] VirtualProtect on CALL @ %p failed "
				"(gle=%lu)\n", p, GetLastError());
			continue;
		}
		memset(p, 0x90, 5);
		VirtualProtect(p, 5, oldProt, &oldProt);
		FlushInstructionCache(GetCurrentProcess(), p, 5);
	}

	// Key bindings RSON bypass (dedi-only). The engine loads controller/
	// keyboard binding RSON assets and calls Engine_Error if missing.
	// These are purely client/UI input -- dedi never uses them. The code
	// at has 3 blocks with pattern: test rbx,rbx / JNZ +14
	// (skip error if found). When not found, falls through to fatal error
	// then dereferences NULL. Patch JNZ → JMP to skip entire block.
	struct KeyBindPatch { uintptr_t jnzAddr; uint8_t newDisp; const char* name; };
	static const KeyBindPatch kKeyBinds[] = {
		{ 0x140237065ull, 0x7D, "xone" },
		{ 0x140237165ull, 0x7D, "ps4" },
		{ 0x140237265ull, 0x56, "keyboard" },
	};
	int kbPatched = 0;
	for (const auto& kb : kKeyBinds)
	{
		uint8_t* p = reinterpret_cast<uint8_t*>(base + (kb.jnzAddr - kImageBase));
		if (p[0] != 0x75)
		{
			Warning(eDLL_T::ENGINE,
				"[engine-soft-warn] keybind %s: expected 0x75 at %p, "
				"got 0x%02X; skipped\n", kb.name, p, p[0]);
			continue;
		}
		DWORD oldP = 0;
		if (!VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &oldP))
			continue;
		p[0] = 0xEB;          // JNZ → JMP
		p[1] = kb.newDisp;    // adjusted displacement
		VirtualProtect(p, 2, oldP, &oldP);
		FlushInstructionCache(GetCurrentProcess(), p, 2);
		kbPatched++;
	}
	if (kbPatched)
		Msg(eDLL_T::ENGINE,
			"[engine-soft-warn] key-bindings RSON bypass: %d/%zu sites "
			"patched (dedi skips controller/keyboard init)\n",
			kbPatched, sizeof(kKeyBinds) / sizeof(kKeyBinds[0]));
}
