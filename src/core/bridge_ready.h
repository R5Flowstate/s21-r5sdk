//=============================================================================//
//
// Purpose: Boot-time subsystem install ledger -- one place to see zeros.
//
//=============================================================================//
#ifndef CORE_BRIDGE_READY_H
#define CORE_BRIDGE_READY_H

#include "tier0/dbg.h"

//-----------------------------------------------------------------------------
// Fixed-size install ledger (no allocation).
//-----------------------------------------------------------------------------
struct BridgeReadyRow_t
{
	const char* pszName;
	int         nInstalled;
	int         nExpectedAtLeast;
};

static constexpr int BRIDGE_READY_MAX_ROWS = 64;

inline BridgeReadyRow_t s_bridgeReadyRows[BRIDGE_READY_MAX_ROWS];
inline int              s_nBridgeReadyRows = 0;

//-----------------------------------------------------------------------------
// Records one subsystem's install result.
//-----------------------------------------------------------------------------
inline void BridgeReady_Report(const char* pszName, int nInstalled, int nExpectedAtLeast)
{
	if (s_nBridgeReadyRows >= BRIDGE_READY_MAX_ROWS)
	{
		Warning(eDLL_T::COMMON,
			"[BRIDGE-READY] row cap (%d) -- dropped '%s' (installed=%d expected>=%d)\n",
			BRIDGE_READY_MAX_ROWS,
			pszName ? pszName : "?",
			nInstalled,
			nExpectedAtLeast);
		return;
	}

	BridgeReadyRow_t& row = s_bridgeReadyRows[s_nBridgeReadyRows++];
	row.pszName           = pszName ? pszName : "?";
	row.nInstalled        = nInstalled;
	row.nExpectedAtLeast  = nExpectedAtLeast;

	// Loud at report time so late install passes cannot hide a zero behind
	// an earlier end-of-boot Print.
	if (nInstalled < nExpectedAtLeast)
	{
		Warning(eDLL_T::COMMON,
			"[BRIDGE-READY] SHORT '%s': installed=%d expected>=%d\n",
			row.pszName,
			nInstalled,
			nExpectedAtLeast);
	}
}

//-----------------------------------------------------------------------------
// Prints reported-row summary; loud banner when any install came up short.
//-----------------------------------------------------------------------------
inline void BridgeReady_Print(const char* pszProduct)
{
	const eDLL_T channel =
		(pszProduct && (pszProduct[0] == 'c' || pszProduct[0] == 'C'))
			? eDLL_T::CLIENT
			: eDLL_T::SERVER;

	int nShort = 0;
	for (int i = 0; i < s_nBridgeReadyRows; ++i)
	{
		if (s_bridgeReadyRows[i].nInstalled < s_bridgeReadyRows[i].nExpectedAtLeast)
			++nShort;
	}

	if (nShort <= 0)
		return;

	Msg(channel,
		"[BRIDGE-READY] %s: %d subsystem(s) reported | %d short\n",
		pszProduct ? pszProduct : "?",
		s_nBridgeReadyRows,
		nShort);

	Warning(channel,
		"[BRIDGE-READY] ************************************************************\n");
	Warning(channel,
		"[BRIDGE-READY] ** INSTALL SHORT -- %d subsystem(s) below expected floor **\n",
		nShort);
	for (int i = 0; i < s_nBridgeReadyRows; ++i)
	{
		const BridgeReadyRow_t& row = s_bridgeReadyRows[i];
		if (row.nInstalled >= row.nExpectedAtLeast)
			continue;

		Warning(channel,
			"[BRIDGE-READY] **   %s: installed=%d expected>=%d\n",
			row.pszName,
			row.nInstalled,
			row.nExpectedAtLeast);
	}
	Warning(channel,
		"[BRIDGE-READY] ************************************************************\n");
}

#endif // CORE_BRIDGE_READY_H
