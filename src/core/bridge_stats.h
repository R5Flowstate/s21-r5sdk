//=============================================================================//
//
// Purpose: Session scoreboard -- always-on cheap counters for authority splits.
//
//=============================================================================//
#ifndef CORE_BRIDGE_STATS_H
#define CORE_BRIDGE_STATS_H

#include "tier0/dbg.h"

#include <cstdint>

//-----------------------------------------------------------------------------
// Counter ids. Parallel s_bridgeStatNames[] must stay 1:1 with this order.
//-----------------------------------------------------------------------------
enum class BridgeStat_e : int
{
	WEAPSTATE_XLAT_TRANSLATED = 0,
	WEAPSTATE_XLAT_PASSTHROUGH,
	COUNT
};

static const char* const s_bridgeStatNames[] =
{
	"WEAPSTATE_XLAT_TRANSLATED",
	"WEAPSTATE_XLAT_PASSTHROUGH",
};

static_assert(
	sizeof(s_bridgeStatNames) / sizeof(s_bridgeStatNames[0])
		== static_cast<size_t>(BridgeStat_e::COUNT),
	"s_bridgeStatNames must match BridgeStat_e::COUNT");

inline int64_t s_bridgeStats[static_cast<int>(BridgeStat_e::COUNT)] = {};

//-----------------------------------------------------------------------------
// Hot path: plain increment. No formatting, allocation, or locking.
//-----------------------------------------------------------------------------
inline void BridgeStat_Bump(BridgeStat_e stat)
{
	const int i = static_cast<int>(stat);
	if (i < 0 || i >= static_cast<int>(BridgeStat_e::COUNT))
		return;
	++s_bridgeStats[i];
}

inline void BridgeStat_Add(BridgeStat_e stat, int64_t n)
{
	const int i = static_cast<int>(stat);
	if (i < 0 || i >= static_cast<int>(BridgeStat_e::COUNT))
		return;
	s_bridgeStats[i] += n;
}

//-----------------------------------------------------------------------------
// Dump non-zero rows; always report how many stayed at zero.
//-----------------------------------------------------------------------------
inline void BridgeStats_Print(const char* pszReason)
{
	// Counters exist in both products; route to the log channel of whichever this is.
#if defined(CLIENT_DLL)
	const eDLL_T channel = eDLL_T::CLIENT;
#else
	const eDLL_T channel = eDLL_T::SERVER;
#endif // CLIENT_DLL

	const int nCount = static_cast<int>(BridgeStat_e::COUNT);
	int nZero = 0;

	Msg(channel, "[BRIDGE-STATS] session=%s\n",
		pszReason ? pszReason : "?");

	for (int i = 0; i < nCount; ++i)
	{
		const int64_t n = s_bridgeStats[i];
		if (n == 0)
		{
			++nZero;
			continue;
		}
		Msg(channel, "[BRIDGE-STATS]   %-32s %lld\n",
			s_bridgeStatNames[i],
			static_cast<long long>(n));
	}

	Msg(channel, "[BRIDGE-STATS]   (%d counters at zero)\n", nZero);
}

inline void BridgeStats_PrintAndReset(const char* pszReason)
{
	BridgeStats_Print(pszReason);

	const int nCount = static_cast<int>(BridgeStat_e::COUNT);
	for (int i = 0; i < nCount; ++i)
		s_bridgeStats[i] = 0;
}

#endif // CORE_BRIDGE_STATS_H
