//=============================================================================//
//
// Purpose: Address validation for detour targets
//
// Sanity-checks resolved pattern addresses before attempting DetourAttach.
// Catches the "pattern matched a valid-but-wrong function" failure mode.
//
//=============================================================================//
#ifndef CORE_DETOUR_VALIDATOR_H
#define CORE_DETOUR_VALIDATOR_H

#include <cstdint>

struct DetourValidationResult
{
	bool        valid;
	const char* reason;  // Static string describing the failure
};

//-----------------------------------------------------------------------------
// Validate a single address as a candidate function target.
// Checks
// - Non-null
// - Inside game module bounds
// - Inside.text section
// - Has enough trampoline room (16+ bytes before section end)
// - Starts with a valid x64 function prologue
//-----------------------------------------------------------------------------
DetourValidationResult DetourValidator_ValidateFunctionAddress(uintptr_t addr);

//-----------------------------------------------------------------------------
// Quick check: is this byte sequence a valid x64 function prologue?
// Accepts common MSVC prologues
// mov [rsp+X], rbx/rsi/rdi/r12-r15
// push rbx/rbp/rsi/rdi/r12-r15
// sub rsp, imm
// mov rax, rsp (save stack)
// jmp rel32 (thunk)
//-----------------------------------------------------------------------------
bool DetourValidator_IsValidPrologue(const uint8_t* code);

//-----------------------------------------------------------------------------
// Check if an address is inside the game module's.text section
//-----------------------------------------------------------------------------
bool DetourValidator_IsInTextSection(uintptr_t addr);

#endif // CORE_DETOUR_VALIDATOR_H
