//=============================================================================//
//
// Purpose: S21 crash-safe detour registry: scan, validate, hook, SEH per step.
//
//=============================================================================//
#ifndef CORE_DETOUR_REGISTRY_H
#define CORE_DETOUR_REGISTRY_H

#include <cstdint>
#include <vector>
#include <string>

class IDetour;

//-----------------------------------------------------------------------------
// State machine for each detour class
//-----------------------------------------------------------------------------
enum class DetourState : uint8_t
{
	NotAttempted     = 0,
	SkippedByConfig  = 1,  // Disabled in sdk_detours.cfg
	SkippedByFlag    = 2,
	ScanFailed       = 3,
	ScanSucceeded    = 4,
	ValidationFailed = 5,
	Validated        = 6,
	HookFailed       = 7,
	Hooked           = 8,
	RuntimeCrash     = 9,
	Disabled         = 10,
};

const char* DetourState_ToString(DetourState state);

//-----------------------------------------------------------------------------
// Per-class tracking entry
//-----------------------------------------------------------------------------
struct DetourEntry
{
	const char* name;
	IDetour*    instance;
	DetourState state;
	uint32_t    exceptionCode;
	const char* failureReason;
	uint64_t    scanTimeUs;
	uint64_t    hookTimeUs;
};

//-----------------------------------------------------------------------------
// Registry API
//-----------------------------------------------------------------------------

void DetourRegistry_Build();
void DetourRegistry_LoadConfig(const char* configPath);
void DetourRegistry_PrintReport();
DetourEntry* DetourRegistry_Find(const char* name);
size_t DetourRegistry_CountByState(DetourState state);
size_t DetourRegistry_Size();
const std::vector<DetourEntry>& DetourRegistry_GetAll();

#endif // CORE_DETOUR_REGISTRY_H
