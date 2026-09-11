//=============================================================================//
//
// Purpose: S21 Crash-Safe Detour Registry -- implementation
//
//=============================================================================//
#include "core/stdafx.h"
#include "core/detour_registry.h"
#include "thirdparty/detours/include/idetour.h"

#include <typeinfo>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_set>

// File-only boot traces (message.log under -devsdk/-logfiles; never console).
extern void SDK_LogDevFile(const char* fmt, ...);

//-----------------------------------------------------------------------------
// Global state
//-----------------------------------------------------------------------------
static std::vector<DetourEntry> g_DetourRegistry;
static std::unordered_set<std::string> g_DisabledClasses;

//-----------------------------------------------------------------------------
// State -> string
//-----------------------------------------------------------------------------
const char* DetourState_ToString(DetourState state)
{
	switch (state)
	{
	case DetourState::NotAttempted:     return "NotAttempted";
	case DetourState::SkippedByConfig:  return "SkippedByConfig";
	case DetourState::SkippedByFlag:    return "SkippedByFlag";
	case DetourState::ScanFailed:       return "ScanFailed";
	case DetourState::ScanSucceeded:    return "ScanSucceeded";
	case DetourState::ValidationFailed: return "ValidationFailed";
	case DetourState::Validated:        return "Validated";
	case DetourState::HookFailed:       return "HookFailed";
	case DetourState::Hooked:           return "Hooked";
	case DetourState::RuntimeCrash:     return "RuntimeCrash";
	case DetourState::Disabled:         return "Disabled";
	}
	return "???";
}

//-----------------------------------------------------------------------------
// Extract class name from typeid.name.
// On MSVC: "class VCVar" -> "VCVar"
// Returns a static buffer per call (not thread-safe, used single-threaded).
//-----------------------------------------------------------------------------
static const char* ExtractClassName(IDetour* instance)
{
	// typeid stores strings in read-only memory. The pointer is valid for
	// the lifetime of the program, so we can cache it directly.
	const char* raw = typeid(*instance).name();
	if (!raw)
		return "<unknown>";

	// Strip "class " prefix if present
	if (strncmp(raw, "class ", 6) == 0)
		return raw + 6;
	if (strncmp(raw, "struct ", 7) == 0)
		return raw + 7;
	return raw;
}

//-----------------------------------------------------------------------------
// Build the registry from g_DetourVec
//-----------------------------------------------------------------------------
void DetourRegistry_Build()
{
	g_DetourRegistry.clear();
	g_DetourRegistry.reserve(g_DetourVec.size());

	for (IDetour* instance : g_DetourVec)
	{
		DetourEntry entry;
		entry.name           = ExtractClassName(instance);
		entry.instance       = instance;
		entry.state          = DetourState::NotAttempted;
		entry.exceptionCode  = 0;
		entry.failureReason  = nullptr;
		entry.scanTimeUs     = 0;
		entry.hookTimeUs     = 0;

		if (g_DisabledClasses.count(entry.name) > 0)
		{
			entry.state = DetourState::SkippedByConfig;
			entry.failureReason = "Disabled in sdk_detours.cfg";
		}

		g_DetourRegistry.push_back(entry);
	}

	SDK_LogDevFile("DetourRegistry: built %zu entries\n", g_DetourRegistry.size());
}

//-----------------------------------------------------------------------------
// Load config file: one class per line, format: <name> <enabled|disabled|skip>
// Lines starting with # are comments.
// Missing file -> all classes default to enabled.
//-----------------------------------------------------------------------------
void DetourRegistry_LoadConfig(const char* configPath)
{
	FILE* fp = nullptr;
	fopen_s(&fp, configPath, "r");
	if (!fp)
	{
		SDK_LogDevFile("DetourRegistry: config not found (%s), defaulting to all enabled\n", configPath);
		return;
	}

	char line[256];
	int loaded = 0;
	while (fgets(line, sizeof(line), fp))
	{
		// Strip leading whitespace
		char* p = line;
		while (*p == ' ' || *p == '\t') ++p;

		// Skip comments and blank lines
		if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
			continue;

		// Parse: <name> <state>
		char name[64] = {0};
		char state[16] = {0};
		if (sscanf_s(p, "%63s %15s", name, (unsigned)sizeof(name), state, (unsigned)sizeof(state)) != 2)
			continue;

		if (_stricmp(state, "disabled") == 0 || _stricmp(state, "skip") == 0)
		{
			g_DisabledClasses.insert(name);
			loaded++;
		}
		// "enabled" is the default, no-op
	}
	fclose(fp);

	SDK_LogDevFile("DetourRegistry: loaded config, %d classes disabled\n", loaded);
}

//-----------------------------------------------------------------------------
// Find entry by name
//-----------------------------------------------------------------------------
DetourEntry* DetourRegistry_Find(const char* name)
{
	for (DetourEntry& e : g_DetourRegistry)
		if (strcmp(e.name, name) == 0)
			return &e;
	return nullptr;
}

size_t DetourRegistry_CountByState(DetourState state)
{
	size_t count = 0;
	for (const DetourEntry& e : g_DetourRegistry)
		if (e.state == state)
			count++;
	return count;
}

size_t DetourRegistry_Size()
{
	return g_DetourRegistry.size();
}

const std::vector<DetourEntry>& DetourRegistry_GetAll()
{
	return g_DetourRegistry;
}

//-----------------------------------------------------------------------------
// Formatted report
//-----------------------------------------------------------------------------
void DetourRegistry_PrintReport()
{
	int hooked = 0, skipped = 0, failed = 0;

	for (const DetourEntry& e : g_DetourRegistry)
	{
		switch (e.state)
		{
		case DetourState::Hooked:
			hooked++;
			break;
		case DetourState::SkippedByConfig:
		case DetourState::SkippedByFlag:
			skipped++;
			break;
		case DetourState::ScanFailed:
		case DetourState::ValidationFailed:
		case DetourState::HookFailed:
		case DetourState::RuntimeCrash:
			failed++;
			SDK_LogDevFile("  %-32s  %-20s  %s\n",
				e.name,
				DetourState_ToString(e.state),
				e.failureReason ? e.failureReason : "");
			break;
		default:
			break;
		}
	}

	SDK_LogDevFile("Detour summary: %d hooked, %d skipped, %d failed\n", hooked, skipped, failed);

	// Slowest classes by scan+hook. A boot-time regression shows up here as one
	// name moving to the top, which the per-phase totals alone cannot attribute.
	uint64_t scanUs = 0, hookUs = 0;
	for (const DetourEntry& e : g_DetourRegistry)
	{
		scanUs += e.scanTimeUs;
		hookUs += e.hookTimeUs;
	}
	SDK_LogDevFile("Detour timing: scan %llu us, hook %llu us, total %llu us\n",
		(unsigned long long)scanUs, (unsigned long long)hookUs,
		(unsigned long long)(scanUs + hookUs));

	std::vector<const DetourEntry*> byTime;
	byTime.reserve(g_DetourRegistry.size());
	for (const DetourEntry& e : g_DetourRegistry)
		byTime.push_back(&e);
	std::sort(byTime.begin(), byTime.end(),
		[](const DetourEntry* a, const DetourEntry* b)
		{
			return (a->scanTimeUs + a->hookTimeUs) > (b->scanTimeUs + b->hookTimeUs);
		});

	const size_t nSlow = (byTime.size() < 8) ? byTime.size() : 8;
	for (size_t i = 0; i < nSlow; ++i)
	{
		const DetourEntry* const e = byTime[i];
		const uint64_t us = e->scanTimeUs + e->hookTimeUs;
		if (us == 0)
			break;
		SDK_LogDevFile("  slowest[%zu] %-32s %llu us (scan %llu, hook %llu)\n",
			i + 1, e->name, (unsigned long long)us,
			(unsigned long long)e->scanTimeUs, (unsigned long long)e->hookTimeUs);
	}
}
