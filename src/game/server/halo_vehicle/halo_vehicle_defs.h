//=============================================================================//
//
// Purpose: parses a Halo vehicle definition from its JSON text.
//
//=============================================================================//
#ifndef HALO_VEHICLE_DEFS_H
#define HALO_VEHICLE_DEFS_H

#include "halo_vehicle_sim.h"
#include <cstddef>

static constexpr size_t HALO_DEF_MAX_JSON_BYTES = 256 * 1024;

// Fills def from text; on failure returns false with a reason in pszError.
// The result has passed HaloSim_ValidateDef.
bool HaloDefs_Parse(const char* pszText, size_t nTextSize, HaloVehicleDef& def,
	char* pszError, size_t nErrorSize);

#endif // HALO_VEHICLE_DEFS_H
