//=============================================================================//
//
// Purpose: Engine SDK feature strings a mod may require through its
//          mod.vdf "RequiresFeatures" block. A required feature the build
//          does not list refuses the whole mod at load.
//
//=============================================================================//

#ifndef SDK_FEATURES_H
#define SDK_FEATURES_H

#include "tier1/strtools.h" // V_stricmp

static const char* const s_sdkFeatures[] =
{
#if defined(SDK_WIP)
	"portal",
	"halo_vehicles",
#endif // SDK_WIP
	nullptr
};

inline bool SDK_HasFeature(const char* pszFeature)
{
	if (!pszFeature || !pszFeature[0])
		return false;

	for (size_t i = 0; s_sdkFeatures[i]; ++i)
	{
		if (V_stricmp(pszFeature, s_sdkFeatures[i]) == 0)
			return true;
	}

	return false;
}

#endif // SDK_FEATURES_H
