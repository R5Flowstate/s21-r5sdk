#pragma once
//=============================================================================//
//
// Purpose: Client-side zipline NULL-entity validation guards
//
//=============================================================================//
#include "core/stdafx.h"

//-----------------------------------------------------------------------------
// Purpose: NULL entity handle still hits zipline type check at +0xB1C.
// Hook validation (RVA 0xFD2940) and lookup (RVA 0xFCCE10); guard before the check.
// Crash: AV at r5apex.exe+0xFD2944 when lookup returns NULL into validation.
// -----------------------------------------------------------------------------

// Original function pointer - Entity validation at RVA 0xFD2940
// Signature: sub rsp, 28h; cmp dword ptr [rcx+0B1Ch], 1
// Checks entity[711] to validate entity type
inline void* (*v_ZiplineEntityValidation)(_DWORD* a1);

// Caller function pointer - Zipline entity lookup at RVA 0xFCCE10
// This function sets rcx = 0 when entity handle is invalid, then calls validation
inline void* (*v_ZiplineEntityLookup)(__int64 a1, __int64 a2, __int64 a3, _DWORD* a4);

