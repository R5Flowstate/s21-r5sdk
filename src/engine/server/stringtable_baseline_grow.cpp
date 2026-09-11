//=============================================================================//
//
// Purpose: Grow WriteBaselines scratch past the 512 KB literal.
// Ceiling 4190208 -- SVC_CreateStringTable decompresses into 4 MiB minus 4 KB.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier1/convar.h"
#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/detours.h"
#include "stringtable_baseline_grow.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static constexpr uint32_t kStockScratchBytes = 0x80000;   // 524288
static constexpr uint32_t kMaxScratchBytes   = 4190208;   // client decompress cap

static ConVar sdk_stringtable_baseline_scratch(
	"sdk_stringtable_baseline_scratch", "2097152",
	FCVAR_DEVELOPMENTONLY | FCVAR_GAMEDLL,
	"Bytes of scratch CClient::SendServerInfo gives each string table to "
	"serialise its baselines into (engine literal is 524288). Must be a "
	"multiple of 4; out-of-range values skip the patch and keep the stock "
	"buffer.",
	true, float(kStockScratchBytes), true, float(kMaxScratchBytes));

//-----------------------------------------------------------------------------
// SVC_CreateStringTable field offsets, all set by WriteBaselines itself.
//-----------------------------------------------------------------------------
static constexpr size_t kMsgNumEntries = 44;
static constexpr size_t kMsgDataBits   = 148;
static constexpr size_t kMsgCurBit     = 152;
static constexpr size_t kMsgOverflow   = 156;

// CNetworkStringTable::m_pszTableName
static constexpr size_t kTableName = 16;

//-----------------------------------------------------------------------------
// WriteBaselines size literals: raw/compressed scratch, nBytes cap, compressor output.
//-----------------------------------------------------------------------------
struct ScratchSite
{
	uint32_t    offset;
	uint32_t    immOffset;
	const char* prefix;
	uint32_t    prefixLen;
};

static const ScratchSite kScratchSites[] = {
	{ 0x043, 1, "\xBA",                         1 },
	{ 0x06D, 1, "\xBA",                         1 },
	{ 0x0EF, 2, "\x41\xB9",                     2 },
	{ 0x125, 7, "\x48\xC7\x85\x18\x01\x00\x00", 7 },
};

static uint32_t g_scratchBytes = kStockScratchBytes;

static bool WriteBytes(void* addr, const void* data, size_t len)
{
	DWORD oldProt = 0;
	if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::SERVER,
			"[STBL-BASELINE] VirtualProtect failed @ %p (gle=%lu)\n",
			addr, GetLastError());
		return false;
	}
	memcpy(addr, data, len);
	DWORD restored = 0;
	VirtualProtect(addr, len, oldProt, &restored);
	FlushInstructionCache(GetCurrentProcess(), addr, len);
	return true;
}

//-----------------------------------------------------------------------------
// First pass logs every table; later passes log overflow or >3/4 cap only.
//-----------------------------------------------------------------------------
static bool Hook_CNetworkStringTable_WriteBaselines(void* pTable, void* pMsg,
	void* pBuf, int nBytes)
{
	const bool ret = v_CNetworkStringTable_WriteBaselines(pTable, pMsg, pBuf,
		nBytes);

	if (!pTable || !pMsg)
		return ret;

	const uint8_t* const msg = reinterpret_cast<const uint8_t*>(pMsg);
	const int numEntries = *reinterpret_cast<const int*>(msg + kMsgNumEntries);
	const int capBits    = *reinterpret_cast<const int*>(msg + kMsgDataBits);
	const int curBits    = *reinterpret_cast<const int*>(msg + kMsgCurBit);
	const bool overflow  = *reinterpret_cast<const uint8_t*>(msg + kMsgOverflow) != 0;

	const char* const name =
		*reinterpret_cast<const char* const*>(
			reinterpret_cast<const uint8_t*>(pTable) + kTableName);

	// One full set of tables is worth having on record; after that only a
	// table that overflowed or crossed three quarters of the cap earns a line.
	static constexpr uint32_t kFullReportLines = 64;
	static std::atomic<uint32_t> s_callN{ 0 };
	const uint32_t callN = s_callN.fetch_add(1, std::memory_order_relaxed);
	const bool bHot = overflow || !ret ||
		(capBits > 0 && curBits > (capBits - (capBits >> 2)));

	if (callN < kFullReportLines || bHot)
		Warning(eDLL_T::SERVER,
			"[STBL-BASELINE]%s table='%s' entries=%d used=%d/%d bits "
			"(%d/%d bytes)%s\n",
			overflow ? " OVERFLOW" : "",
			name ? name : "<null>", numEntries, curBits, capBits,
			(curBits + 7) >> 3, capBits >> 3,
			ret ? "" : " -- writer reported short");

	if (overflow)
		Warning(eDLL_T::SERVER,
			"[STBL-BASELINE] '%s' did NOT fit %d bytes -- its baselines are "
			"TRUNCATED on the wire. Raise sdk_stringtable_baseline_scratch "
			"above %u and restart.\n",
			name ? name : "<null>", capBits >> 3, g_scratchBytes);

	return ret;
}

void VStringTableBaselineGrow::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D 9A 88 00 00 00 "
		"49 63 C1 4C 89 03 48 83 E0 FC 89 43 08")
		.GetPtr(v_CNetworkStringTable_WriteBaselines);
}

void VStringTableBaselineGrow::Detour(const bool bAttach) const
{
	if (v_CNetworkStringTable_WriteBaselines)
	{
		const LONG r = bAttach
			? DetourAttach(
				reinterpret_cast<void**>(&v_CNetworkStringTable_WriteBaselines),
				reinterpret_cast<void*>(&Hook_CNetworkStringTable_WriteBaselines))
			: DetourDetach(
				reinterpret_cast<void**>(&v_CNetworkStringTable_WriteBaselines),
				reinterpret_cast<void*>(&Hook_CNetworkStringTable_WriteBaselines));
		if (bAttach)
			Msg(eDLL_T::SERVER, "[s21-bridge] DetourAttach "
				"CNetworkStringTable::WriteBaselines result=0x%lX "
				"(target=0x%p)\n",
				r, (void*)v_CNetworkStringTable_WriteBaselines);
	}
	else
	{
		Warning(eDLL_T::SERVER,
			"[STBL-BASELINE] WriteBaselines pattern unresolved -- per-table "
			"fill will NOT be reported; a truncated baseline stays silent.\n");
	}

	if (!bAttach)
		return;

	const int targetBytes = sdk_stringtable_baseline_scratch.GetInt();
	if (targetBytes == static_cast<int>(kStockScratchBytes))
	{
		Msg(eDLL_T::SERVER,
			"[STBL-BASELINE] scratch left at the engine's %u bytes.\n",
			kStockScratchBytes);
		return;
	}
	if (targetBytes < static_cast<int>(kStockScratchBytes) ||
		targetBytes > static_cast<int>(kMaxScratchBytes) ||
		(targetBytes & 3) != 0)
	{
		Warning(eDLL_T::SERVER,
			"[STBL-BASELINE] target %d out of range [%u, %u] or not "
			"4-byte aligned; skipping. Stock %u-byte scratch retained.\n",
			targetBytes, kStockScratchBytes, kMaxScratchBytes,
			kStockScratchBytes);
		return;
	}

	CMemory fn = Module_FindPattern(g_GameDll,
		"48 89 7C 24 20 48 89 54 24 10 55 41 54 41 55 41 56 41 57 48 8D AC "
		"24 20 FF FF FF 48 81 EC E0 01 00 00");
	if (!fn)
	{
		Warning(eDLL_T::SERVER,
			"[STBL-BASELINE] container WriteBaselines pattern not found -- "
			"scratch stays at %u bytes and oversized tables will still be "
			"truncated.\n", kStockScratchBytes);
		return;
	}

	uint8_t* const base = reinterpret_cast<uint8_t*>(fn.GetPtr());

	// Preflight every site before touching any of them; a partially patched
	// set would size the allocations and the cap differently.
	for (const ScratchSite& s : kScratchSites)
	{
		uint8_t* const p = base + s.offset;
		if (memcmp(p, s.prefix, s.prefixLen) != 0 ||
			*reinterpret_cast<const uint32_t*>(p + s.immOffset) != kStockScratchBytes)
		{
			Warning(eDLL_T::SERVER,
				"[STBL-BASELINE] site +0x%X mismatch @ %p (imm=0x%X, expected "
				"0x%X); no sites patched.\n",
				s.offset, p,
				*reinterpret_cast<const uint32_t*>(p + s.immOffset),
				kStockScratchBytes);
			return;
		}
	}

	const uint32_t newBytes = static_cast<uint32_t>(targetBytes);
	int applied = 0;
	for (const ScratchSite& s : kScratchSites)
	{
		if (!WriteBytes(base + s.offset + s.immOffset, &newBytes, 4))
			break;
		++applied;
	}

	if (applied != static_cast<int>(ARRAYSIZE(kScratchSites)))
	{
		for (int i = 0; i < applied; ++i)
			WriteBytes(base + kScratchSites[i].offset +
				kScratchSites[i].immOffset, &kStockScratchBytes, 4);
		Warning(eDLL_T::SERVER,
			"[STBL-BASELINE] only %d/%d sites took; rolled back to the stock "
			"%u-byte scratch.\n",
			applied, static_cast<int>(ARRAYSIZE(kScratchSites)),
			kStockScratchBytes);
		return;
	}

	g_scratchBytes = newBytes;
	Msg(eDLL_T::SERVER,
		"[STBL-BASELINE] scratch %u -> %u bytes (%u bits of bf_write cap) "
		"at %p, %d/%d sites.\n",
		kStockScratchBytes, newBytes, newBytes * 8u, base,
		applied, static_cast<int>(ARRAYSIZE(kScratchSites)));
}
