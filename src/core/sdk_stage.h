//=============================================================================//
//
// Purpose: run one init stage behind the single SEH boundary in the SDK.
// A faulting stage is skipped; nothing else in src/ may use SEH.
//
//=============================================================================//
#ifndef CORE_SDK_STAGE_H
#define CORE_SDK_STAGE_H

#include "tier0/dbg.h"
#include "core/bridge_ready.h"

typedef void (*PFN_SdkStage)(void);

// Plain function pointer, never a capturing lambda: an object with a destructor
// in this scope makes MSVC reject the __try with C2712.
inline bool SdkStage_Run(const char* const pszStage, const PFN_SdkStage pfnStage)
{
	if (!pszStage || !pfnStage)
		return false;

	unsigned long nCode = 0;
	__try
	{
		pfnStage();
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		nCode = static_cast<unsigned long>(GetExceptionCode());
	}

	if (nCode != 0)
	{
		Warning(eDLL_T::COMMON,
			"[SDK-STAGE] '%s' faulted 0x%08lX -- stage skipped, boot continues\n",
			pszStage, nCode);
		BridgeReady_Report(pszStage, 0, 1);
		return false;
	}

	BridgeReady_Report(pszStage, 1, 1);
	return true;
}

#endif // CORE_SDK_STAGE_H
