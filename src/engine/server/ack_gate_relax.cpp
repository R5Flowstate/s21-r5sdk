//=============================================================================//
//
// Purpose: NOP UpdateAcknowledgedFramecount's inner jl so late S21 acks land.
// Outer a3 > waitTick gate stays.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "public/tier0/memaddr.h"
#include "ack_gate_relax.h"

#include <cstdint>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static bool AckGateRelax_WriteBytes(void* addr, const void* data, size_t len)
{
	DWORD oldProt = 0;
	if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::SERVER,
			"[ACK-GATE-RELAX] VirtualProtect failed @ %p (gle=%lu)\n",
			addr, GetLastError());
		return false;
	}
	memcpy(addr, data, len);
	DWORD restored = 0;
	VirtualProtect(addr, len, oldProt, &restored);
	FlushInstructionCache(GetCurrentProcess(), addr, len);
	return true;
}

void VAckGateRelax::Detour(const bool bAttach) const
{
	if (!bAttach) return;

	// Locate the cmp r8d,eax immediately preceding the strict-equality jl.
	// The jl bytes (7C 38) sit at site+3.
	CMemory site = Module_FindPattern(g_GameDll,
		"44 3B C0 7C 38 C7 81 9C 05 00 00 FF FF FF FF");
	if (!site)
	{
		Warning(eDLL_T::SERVER,
			"[ACK-GATE-RELAX] gate pattern not found; engine ack gate "
			"REMAINS strict. S21 async late acks will be dropped -- do "
			"NOT rely on bridged ack handling without "
			"this patch active.\n");
		return;
	}

	uint8_t* p  = reinterpret_cast<uint8_t*>(site.GetPtr());
	uint8_t* jl = p + 3;

	// Must be 7C 38 (jl rel8 +0x38).
	if (jl[0] != 0x7C || jl[1] != 0x38)
	{
		Warning(eDLL_T::SERVER,
			"[ACK-GATE-RELAX] jl signature mismatch @ %p "
			"(got %02X %02X, expected 7C 38); patch skipped.\n",
			jl, jl[0], jl[1]);
		return;
	}

	const uint8_t nop[2] = { 0x90, 0x90 };
	if (!AckGateRelax_WriteBytes(jl, nop, sizeof(nop)))
	{
		Warning(eDLL_T::SERVER,
			"[ACK-GATE-RELAX] failed to NOP jl @ %p\n", jl);
		return;
	}

	Msg(eDLL_T::SERVER,
		"[ACK-GATE-RELAX] patched 7C 38 -> 90 90 at %p (cmp @ %p) -- "
		"S21 async late acks now accepted natively.\n",
		jl, p);
}
