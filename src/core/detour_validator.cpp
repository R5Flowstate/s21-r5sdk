//=============================================================================//
//
// Purpose: Address validation for detour targets -- implementation
//
//=============================================================================//
#include "core/stdafx.h"
#include "core/detour_validator.h"
#include "tier0/module.h"

//-----------------------------------------------------------------------------
// Cached.text section bounds -- populated lazily on first call
//-----------------------------------------------------------------------------
static uintptr_t s_TextBase = 0;
static uintptr_t s_TextEnd  = 0;
static bool      s_TextResolved = false;

static void ResolveTextSection()
{
	if (s_TextResolved)
		return;
	s_TextResolved = true;

	// FindSectionByName is non-throwing; missing/invalid .text leaves base/end 0.
	const CModule::ModuleSections_t* const textSection = g_GameDll.FindSectionByName(".text");
	if (textSection && textSection->IsSectionValid())
	{
		s_TextBase = static_cast<uintptr_t>(textSection->m_pSectionBase);
		s_TextEnd  = s_TextBase + textSection->m_nSectionSize;
	}
}

//-----------------------------------------------------------------------------
// Section membership check
//-----------------------------------------------------------------------------
bool DetourValidator_IsInTextSection(uintptr_t addr)
{
	ResolveTextSection();
	if (s_TextBase == 0)
		return false;  // Can't validate without section info, fail safe
	return addr >= s_TextBase && addr < s_TextEnd;
}

//-----------------------------------------------------------------------------
// Common MSVC x64 prologues (save-reg, sub rsp, jmp thunk).
//-----------------------------------------------------------------------------
bool DetourValidator_IsValidPrologue(const uint8_t* code)
{
	if (!code)
		return false;

	const uint8_t b0 = code[0];
	const uint8_t b1 = code[1];
	const uint8_t b2 = code[2];

	// REX.W + mov reg [rsp+X]
	if (b0 == 0x48 && b1 == 0x89)
	{
		// ModR/M byte must be ?? XXX 100 (SIB byte follows, indicating [rsp+disp])
		// Valid patterns: 44, 4C, 54, 5C, 64, 6C, 74, 7C
		if (b2 == 0x44 || b2 == 0x4C || b2 == 0x54 || b2 == 0x5C ||
		    b2 == 0x64 || b2 == 0x6C || b2 == 0x74 || b2 == 0x7C)
			return true;
	}

	// REX.R + mov [rsp+X], r8-r15
	if (b0 == 0x4C && b1 == 0x89)
	{
		if (b2 == 0x44 || b2 == 0x4C || b2 == 0x54 || b2 == 0x5C ||
		    b2 == 0x64 || b2 == 0x6C || b2 == 0x74 || b2 == 0x7C)
			return true;
	}

	// sub rsp, imm8
	if (b0 == 0x48 && b1 == 0x83 && b2 == 0xEC)
		return true;

	// sub rsp, imm32
	if (b0 == 0x48 && b1 == 0x81 && b2 == 0xEC)
		return true;

	// mov rax, rsp (save stack, common with large frames)
	if (b0 == 0x48 && b1 == 0x8B && b2 == 0xC4)
		return true;

	// push with REX prefix: 40 53, 40 55, 40 56, 40 57
	if (b0 == 0x40 && (b1 == 0x53 || b1 == 0x55 || b1 == 0x56 || b1 == 0x57))
		return true;

	// push r12-r15: 41 54, 41 55, 41 56, 41 57
	if (b0 == 0x41 && (b1 == 0x54 || b1 == 0x55 || b1 == 0x56 || b1 == 0x57))
		return true;

	// push without REX
	if (b0 == 0x53 || b0 == 0x55 || b0 == 0x56 || b0 == 0x57)
		return true;

	// jmp rel32 (thunk/forwarded function)
	if (b0 == 0xE9)
		return true;

	// jmp [rip+disp32] (import/trampoline)
	if (b0 == 0xFF && b1 == 0x25)
		return true;

	// MSVC arg-guard: test r64,r64 then jz. Opcode 48 85 / 4D 85 / 4C 85 / 49 85.
	if (b0 == 0x48 && b1 == 0x85 && b2 >= 0xC0)
		return true;
	// 4D 85 ?? - test r8..r15, r8..r15
	if (b0 == 0x4D && b1 == 0x85 && b2 >= 0xC0)
		return true;
	// 4C 85 ?? - test r/m64, r8..r15
	if (b0 == 0x4C && b1 == 0x85 && b2 >= 0xC0)
		return true;
	// 49 85 ?? - test r8..r15, r64
	if (b0 == 0x49 && b1 == 0x85 && b2 >= 0xC0)
		return true;

	// MSVC large-frame: mov r11, rsp (NET_ReceiveDatagram). Rejecting it unhooks VNet.
	if (b0 == 0x4C && b1 == 0x8B && b2 == 0xDC)
		return true;
	// 4C 8B D9 / DA / DB = mov r11, rcx/rdx/rbx (less common entry variants)
	if (b0 == 0x4C && b1 == 0x8B && (b2 == 0xD9 || b2 == 0xDA || b2 == 0xDB))
		return true;
	// 48 8B D9 / DA / F9 = mov rbx/rdx/rdi, rcx/rdx/...
	if (b0 == 0x48 && b1 == 0x8B && (b2 == 0xD9 || b2 == 0xDA || b2 == 0xDB
		|| b2 == 0xF9 || b2 == 0xF1 || b2 == 0xE9 || b2 == 0xE5))
		return true;
	// 40 55 = push rbp with REX (already covered partially by 40 5x)
	// 48 8B EC = mov rbp, rsp
	if (b0 == 0x48 && b1 == 0x8B && b2 == 0xEC)
		return true;
	// 44 89 44 24 ?? = mov [rsp+X], r8d (PredDiag Prediction_Dispatch style)
	if (b0 == 0x44 && b1 == 0x89 && (b2 == 0x44 || b2 == 0x4C || b2 == 0x54
		|| b2 == 0x5C || b2 == 0x64 || b2 == 0x6C || b2 == 0x74 || b2 == 0x7C))
		return true;
	// 89 54 24 ?? = mov [rsp+X], edx
	if (b0 == 0x89 && (b1 == 0x44 || b1 == 0x4C || b1 == 0x54 || b1 == 0x5C
		|| b1 == 0x64 || b1 == 0x6C || b1 == 0x74 || b1 == 0x7C))
		return true;
	// 66 89 44 24 ?? = mov [rsp+X], r16 (16-bit home spill)
	if (b0 == 0x66 && b1 == 0x89 && (b2 == 0x44 || b2 == 0x4C || b2 == 0x54
		|| b2 == 0x5C || b2 == 0x64 || b2 == 0x6C || b2 == 0x74 || b2 == 0x7C))
		return true;

	// Reject int3 / nop / null at function start.
	return false;
}

//-----------------------------------------------------------------------------
// Full address validation
//-----------------------------------------------------------------------------
DetourValidationResult DetourValidator_ValidateFunctionAddress(uintptr_t addr)
{
	DetourValidationResult r;
	r.valid = false;
	r.reason = nullptr;

	if (addr == 0)
	{
		r.reason = "null address";
		return r;
	}

	// Must be inside the game module (rough check)
	const uintptr_t base = g_GameDll.GetModuleBase();
	const uintptr_t end  = base + g_GameDll.GetModuleSize();
	if (addr < base || addr >= end)
	{
		r.reason = "outside game module bounds";
		return r;
	}

	// Must be inside the.text section
	if (!DetourValidator_IsInTextSection(addr))
	{
		r.reason = "not in .text section";
		return r;
	}

	// Must have enough room for a trampoline (Detours needs ~16 bytes)
	if (addr + 16 >= end)
	{
		r.reason = "insufficient trampoline room";
		return r;
	}

	// Prologue bytes are plain-readable mapped .text after the gates above.
	const uint8_t* code = reinterpret_cast<const uint8_t*>(addr);
	if (!DetourValidator_IsValidPrologue(code))
	{
		r.reason = "invalid function prologue";
		return r;
	}

	r.valid = true;
	return r;
}
