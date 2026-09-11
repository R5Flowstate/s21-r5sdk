//===========================================================================//
//
// Purpose: Fix for zipline entity validation crash on S17+ converted maps
//
// NULL entity from failed zipline handle lookup reaches type validation and AV at +0xB1C.
// Common on S17+ converted maps with missing/uninitialized zipline refs.
//
//===========================================================================//

#include "core/stdafx.h"
#include "game/client/c_zipline.h"

//-----------------------------------------------------------------------------
// Purpose: Hooked entity validation function with NULL pointer protection
// Input: a1 - Entity pointer (can be NULL!)
// Output: Validation result (nullptr if NULL or invalid)
//-----------------------------------------------------------------------------
static void* ZiplineEntityValidation_Hook(_DWORD* a1)
{
	// NULL check to prevent crash at a1[711]
	if (!a1)
	{
		// Log once to avoid spam
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			Warning(eDLL_T::CLIENT,
				"ZiplineEntityValidation: NULL entity pointer passed, "
				"skipping validation to prevent crash\n");
			s_bWarned = true;
		}
		return nullptr;  // Return NULL (invalid entity)
	}

	// Valid pointer, call original function
	return v_ZiplineEntityValidation(a1);
}

