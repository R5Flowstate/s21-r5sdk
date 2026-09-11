//=============================================================================//
//
// Purpose: net_bridge -- S2C ProcessMessages (one TU).
//
//=============================================================================//
#include "core/stdafx.h"
#include "engine/client/net_observer.h"
#include "engine/client/net_bridge_internal.h"
#include "engine/client/net_bridge_skip.h"
#include "core/bridge_stats.h"
#include "engine/client/bridge_join_auth.h"
#include "engine/client/bridge_connect_password.h"
#include "engine/sys_integrity.h"
#include "rtech/pak/pak_lobby_world.h"
#include "engine/mdl_precache_client_grow.h"
#include "tier0/memvalidate.h"
#include "tier0/commandline.h"
#include "tier1/lzss.h"

#include "engine/cmd.h"
#include "engine/net.h"
#include "engine/net_chan.h"
#include "engine/debugoverlay.h"
#include "engine/client/clientstate.h"
#include "engine/client/client.h"
#include "engine/client/cl_rcon.h"
#include "engine/client/cl_rcon_launcher.h"
#include "engine/server/sv_rcon.h"
#include "ebisusdk/EbisuSDK.h"
#include "public/tier1/cmd.h"
#include "public/bspflags.h"
#include "public/globalvars_base.h"

#include <mutex>
#include "tier1/cvar.h"
#include "rtech/playlists/playlists.h"

#include "game/shared/activity.h"
#include "game/shared/activity_s3_to_s21_client.h"

#include "game/client/c_baseentity.h"
#include "game/client/mantle_boost.h"
#include "game/client/pred_authority.h"
#include "game/client/hud_basechat.h"
#include "game/shared/heap_canary.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/vsquirrel_s21.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#pragma comment(lib, "psapi.lib")
#include <ctime>
#include <cmath>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <string>

// Client global vars (curTime/tickCount) for the [ANIM-WATCH] clock-domain probe.
// Defined in cdll_engine_int.cpp; same extern pattern as status_effects_sdk.cpp.
extern CGlobalVarsBase* gpGlobals;

// Silence C4456/C4459 on this TU: inner locals intentionally shadow outer names.
// C4244 stays enabled.
#pragma warning(disable: 4456 4459)
#include "engine/client/net_bridge_split.h"


static std::string S21Bridge_GetConnectPersona()
{
	if (g_PersonaName && g_PersonaName[0] != '\0')
		return std::string(g_PersonaName);

	return "unnamed";
}
//-----------------------------------------------------------------------------
// [SKIP-TRANSFER] 1Hz rollup of S2C messages whose type maps to no S21 handler.
//-----------------------------------------------------------------------------
static std::atomic<uint64_t> g_skipLastDumpQpc{0};
static std::atomic<uint32_t> g_skipTotalCount{0};
static std::atomic<uint64_t> g_skipTotalBits{0};
static std::atomic<uint32_t> g_skipPerCmdCount[128];
static std::atomic<uint64_t> g_skipPerCmdBits[128];

static void Bridge_RecordSkipTransfer(int s3cmd, int64_t bitsLeft)
{
	const uint64_t bits = (bitsLeft > 0) ? static_cast<uint64_t>(bitsLeft) : 0;
	if (s3cmd >= 0 && s3cmd < 128)
	{
		g_skipPerCmdCount[s3cmd].fetch_add(1, std::memory_order_relaxed);
		g_skipPerCmdBits[s3cmd].fetch_add(bits, std::memory_order_relaxed);
	}
	g_skipTotalCount.fetch_add(1, std::memory_order_relaxed);
	g_skipTotalBits.fetch_add(bits, std::memory_order_relaxed);

	LARGE_INTEGER now, freq;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&freq);
	if (freq.QuadPart <= 0) return;
	uint64_t prev = g_skipLastDumpQpc.load(std::memory_order_relaxed);
	if (prev == 0)
	{
		g_skipLastDumpQpc.store(static_cast<uint64_t>(now.QuadPart),
			std::memory_order_relaxed);
		return;
	}
	const uint64_t deltaMs = (static_cast<uint64_t>(now.QuadPart) - prev)
		* 1000ULL / static_cast<uint64_t>(freq.QuadPart);
	if (deltaMs < 1000) return;
	if (!g_skipLastDumpQpc.compare_exchange_strong(prev,
		static_cast<uint64_t>(now.QuadPart), std::memory_order_relaxed))
		return;

	const uint32_t total     = g_skipTotalCount.exchange(0, std::memory_order_relaxed);
	const uint64_t totalBits = g_skipTotalBits.exchange(0, std::memory_order_relaxed);
	if (total == 0) return;

	char detail[1024] = {};
	int len = 0;
	for (int c = 0; c < 128; ++c)
	{
		const uint32_t cnt = g_skipPerCmdCount[c].exchange(0, std::memory_order_relaxed);
		const uint64_t b   = g_skipPerCmdBits[c].exchange(0, std::memory_order_relaxed);
		if (cnt > 0 && len < static_cast<int>(sizeof(detail)) - 64)
		{
			int n = snprintf(detail + len, sizeof(detail) - len,
				" s3cmd%d=%ux/%lluB", c, cnt, b / 8);
			if (n > 0) len += n;
		}
	}
	Warning(eDLL_T::CLIENT,
		"[SKIP-TRANSFER] /1s: total=%ux/%lluB %s\n",
		total, totalBits / 8, detail);
}
static bool        s_dumpClassInventoryAfterClassInfo = false;
// One in-flight split per slot; a fragment lost on the wire must never pin
// the reassembler to its request while every later split gets skipped.
static SplitPacket s_splitSlots[4];
static volatile LONG s_splitFrags = 0, s_splitDone = 0, s_splitAbandoned = 0, s_splitBadCount = 0;

void S21Bridge_ResetSplitReassembly(void)
{
	memset(s_splitSlots, 0, sizeof(s_splitSlots));
}

static constexpr int S3_NETMSG_TYPE_MAX = 127;
static void S21Bridge_CIDiag_WalkAndStubPatch(void);
static void S21Bridge_CIDiag_WriteEmergencyDump(const char* tag, EXCEPTION_POINTERS* ep);

// Structural SendTable verdict shared with the signon pre-scan, evaluated at
// the point of use. Pure cursor over [nDataStart, nDataEnd): enforces every
// ceiling the native table parser assumes (name termination, pType < 11,
// nProps <= 1023, which also bounds leaves for one table). Verdict-only: no
// stub building, no byte writes. The pre-scan keeps its own walk (global leaf
// budget + stub side effects); this gate closes the interleave hole the scan
// cannot prove absent.
static bool FailInjectTable(const char* sTag, const char* sTbl, const char* sWhy)
{
	static volatile LONG s_gateReject = 0;
	if (InterlockedIncrement(&s_gateReject) <= 8)
		Warning(eDLL_T::ENGINE, "[BRIDGE-ST:%s] inject-table reject '%s': %s\n",
			sTag, sTbl && sTbl[0] ? sTbl : "?", sWhy);
	return false;
}

static bool S21Bridge_ValidateSendTableBody(const uint8_t* pData, int nTotalBits,
	int nDataStart, int nDataEnd, const char* sTag)
{
	char tbl[256] = {};
	if (!pData || nDataStart < 0 || nDataEnd > nTotalBits || nDataStart >= nDataEnd)
		return FailInjectTable(sTag, tbl, "bad range");
	int pos = nDataStart;
	bool nameTerm = false;
	for (int i = 0; i < 256; i++)
	{
		if (pos + 8 > nDataEnd)
			return FailInjectTable(sTag, tbl, "unterminated table name");
		const uint32_t ch = S21Bridge_Skip_PeekUBits(pData, nTotalBits, pos, 8);
		pos += 8;
		tbl[i] = (char)ch;
		if (ch == 0)
		{
			nameTerm = true;
			break;
		}
	}
	if (!nameTerm)
		return FailInjectTable(sTag, tbl, "unterminated table name");
	if (pos + 10 > nDataEnd)
		return FailInjectTable(sTag, tbl, "truncated nProps");
	const uint32_t nProps = S21Bridge_Skip_PeekUBits(pData, nTotalBits, pos, 10);
	pos += 10;
	if (nProps > 1023)
		return FailInjectTable(sTag, tbl, "nProps");
	for (uint32_t pi = 0; pi < nProps; pi++)
	{
		if (pos + 5 > nDataEnd)
			return FailInjectTable(sTag, tbl, "truncated prop type");
		const uint32_t pType = S21Bridge_Skip_PeekUBits(pData, nTotalBits, pos, 5);
		pos += 5;
		if (pType >= 11)
			return FailInjectTable(sTag, tbl, "prop type");
		bool pNameTerm = false;
		for (int i = 0; i < 256; i++)
		{
			if (pos + 8 > nDataEnd)
				return FailInjectTable(sTag, tbl, "unterminated prop name");
			const uint32_t ch = S21Bridge_Skip_PeekUBits(pData, nTotalBits, pos, 8);
			pos += 8;
			if (ch == 0)
			{
				pNameTerm = true;
				break;
			}
		}
		if (!pNameTerm)
			return FailInjectTable(sTag, tbl, "unterminated prop name");
		if (pos + 16 + 8 > nDataEnd)
			return FailInjectTable(sTag, tbl, "truncated prop flags");
		const uint32_t pFlags = S21Bridge_Skip_PeekUBits(pData, nTotalBits, pos, 16);
		pos += 16 + 8;
		if (pType == 10 || (pFlags & 0x40))
		{
			bool dtTerm = false;
			for (int i = 0; i < 256; i++)
			{
				if (pos + 8 > nDataEnd)
					return FailInjectTable(sTag, tbl, "unterminated datatable name");
				const uint32_t ch = S21Bridge_Skip_PeekUBits(pData, nTotalBits, pos, 8);
				pos += 8;
				if (ch == 0)
				{
					dtTerm = true;
					break;
				}
			}
			if (!dtTerm)
				return FailInjectTable(sTag, tbl, "unterminated datatable name");
		}
		else if (pType == 5)
		{
			if (pos + 10 > nDataEnd)
				return FailInjectTable(sTag, tbl, "truncated array count");
			pos += 10;
		}
		else
		{
			if (pos + 32 + 32 + 7 > nDataEnd)
				return FailInjectTable(sTag, tbl, "truncated prop fields");
			pos += 32 + 32 + 7;
		}
	}
	return true;
}

// Validator skip alphabet. Keep in sync with skipNonSendTable below: every
// case there must appear here. A signon type in this set with a malformed
// body is hostile or corrupt, so the scan rejects; anything else keeps the
// historic fail-open break (inject discards the rest of the transfer at
// dispatch, so nothing past an unknown type runs).
static bool S21Bridge_IsValidatorSkipType(uint32_t msgType)
{
	switch (msgType)
	{
	case 3:
	case 4:
	case 5:
	case 6:
	case 7:
	case 9:
	case 10:
	case 11:
	case 12:
	case 13:
	case 16:
	case 22:
	case 23:
	case 24:
	case 32:
	case 34:
	case 37:
	case 41:
	case 42:
	case 43:
	case 44:
		return true;
	default:
		return false;
	}
}

bool S21Bridge_PreprocessSendTablesInBuffer(uint8_t* data, int dataSize, const char* tag)
{
	if (!data || dataSize <= 0)
		return true;

	S21Bridge_ExtractRecvTableNames();

	const int totalBits = dataSize * 8;
	int bitPos = 0;
	int matched = 0;
	int stubsBuilt = 0;
	int noDecoder = 0;
	int caseFixed = 0;
	int messages = 0;
	int sendTables = 0;
	int leafTotal = 0;
	const char* logTag = tag ? tag : "?";

	static volatile LONG s_preScanLogs = 0;
	const LONG scanLogN = InterlockedIncrement(&s_preScanLogs);
	if (scanLogN <= 40)
	{
		const uint32_t first7 = (dataSize > 0) ? (data[0] & 0x7Fu) : 0;
		SDK_Log("[BRIDGE-ST:%s] scan start: bytes=%d first7=%u first16=%02X %02X %02X %02X %02X %02X %02X %02X\n",
			logTag, dataSize, first7,
			dataSize > 0 ? data[0] : 0, dataSize > 1 ? data[1] : 0,
			dataSize > 2 ? data[2] : 0, dataSize > 3 ? data[3] : 0,
			dataSize > 4 ? data[4] : 0, dataSize > 5 ? data[5] : 0,
			dataSize > 6 ? data[6] : 0, dataSize > 7 ? data[7] : 0);
	}

	auto peekBits = [&](int nBits) -> uint32_t {
		uint32_t val = 0;
		if (bitPos < 0 || nBits <= 0)
			return 0;
		for (int b = 0; b < nBits && (bitPos + b) < totalBits; b++)
		{
			const int bp = bitPos + b;
			if (bp < 0)
				return 0;
			if (data[bp / 8] & (1 << (bp % 8)))
				val |= (1u << b);
		}
		return val;
	};
	auto readBits = [&](int nBits) -> uint32_t {
		uint32_t val = peekBits(nBits);
		bitPos += nBits;
		return val;
	};
	auto readString = [&](char* out, int maxLen) {
		for (int i = 0; i < maxLen - 1; i++)
		{
			if (bitPos + 8 > totalBits) { out[i] = 0; break; }
			uint32_t ch = readBits(8);
			out[i] = (char)ch;
			if (ch == 0) break;
		}
		out[maxLen - 1] = 0;
	};
	auto writeByteAtBit = [&](int startBit, int byteIndex, uint8_t ch) {
		const int bp = startBit + byteIndex * 8;
		for (int b = 0; b < 8; b++)
		{
			const int bitIdx = bp + b;
			if (bitIdx >= totalBits)
				break;
			if (ch & (1 << b))
				data[bitIdx / 8] |= (1 << (bitIdx % 8));
			else
				data[bitIdx / 8] &= ~(1 << (bitIdx % 8));
		}
	};
	auto peekBitsAt = [&](int pos, int nBits) -> uint32_t {
		uint32_t val = 0;
		if (pos < 0 || nBits <= 0)
			return 0;
		for (int b = 0; b < nBits && (pos + b) < totalBits; b++)
		{
			const int bp = pos + b;
			if (data[bp / 8] & (1 << (bp % 8)))
				val |= (1u << b);
		}
		return val;
	};
	auto rejectSendTable = [&](const char* tbl, const char* reason) -> bool {
		static volatile LONG s_stReject = 0;
		const LONG n = InterlockedIncrement(&s_stReject);
		if (n <= 8)
			Warning(eDLL_T::ENGINE,
				"[BRIDGE-ST:%s] rejected SendTable '%s': %s\n",
				logTag, tbl && tbl[0] ? tbl : "?", reason);
		return false;
	};

	auto skipCappedString = [&](int maxLen) -> bool {
		return S21Bridge_Skip_CappedString(data, totalBits, bitPos, maxLen);
	};

	auto skipCappedStringStrict = [&](int maxLen) -> bool {
		return S21Bridge_Skip_CappedStringStrict(data, totalBits, bitPos, maxLen);
	};

	auto skipNonSendTable = [&](uint32_t msgType) -> bool {
		return S21Bridge_SkipNonSendTable(data, totalBits, bitPos, msgType);
	};

	int skippedNonST = 0;
	while (bitPos + 7 <= totalBits)
	{
		const uint32_t msgType = readBits(7);
		if (msgType == 0)
			continue;
		if (msgType == 1)
			break;
		++messages;
		if (msgType != 8)
		{
			static volatile LONG s_nonStLogs = 0;
			const LONG n = InterlockedIncrement(&s_nonStLogs);
			if (n <= 40)
				SDK_Log("[BRIDGE-ST:%s] skip non-SendTable msg=%u bit=%d/%d\n",
					logTag, msgType, bitPos - 7, totalBits);
			// Split authority: unknown types keep the historic fail-open break
			// (inject discards the rest of the transfer at dispatch, so
			// nothing past this point runs). Known types with malformed
			// bodies are hostile or corrupt -- reject the buffer.
			if (msgType > (uint32_t)S3_NETMSG_TYPE_MAX)
				break;
			if (!skipNonSendTable(msgType))
			{
				if (S21Bridge_IsValidatorSkipType(msgType))
				{
					// Telemetry: known types with malformed bodies fail open
					// here (a reject once bounced a legit join on tail-garbage
					// msg6). The inject-time table gate is the fail-closed
					// point. Claimed length separates huge-legit blobs from
					// misalignment garbage.
					static volatile LONG s_skipMalformed = 0;
					if (InterlockedIncrement(&s_skipMalformed) <= 8)
					{
						const uint32_t claimed = (msgType == 6 && bitPos + 32 <= totalBits)
							? peekBitsAt(bitPos, 32) : 0;
						Warning(eDLL_T::ENGINE,
							"[BRIDGE-ST:%s] KNOWN-MALFORMED msg=%u bit=%d/%d claimed=%u messages=%d sendTables=%d skippedNonST=%d -- fail-open break\n",
							logTag, msgType, bitPos, totalBits, claimed, messages, sendTables, skippedNonST);
					}
				}
				break;
			}
			++skippedNonST;
			continue;
		}
		++sendTables;

		const int needsDecoderBitPos = bitPos;
		const uint32_t needsDecoder = readBits(1);
		const uint32_t dataLen = readBits(32);
		const int dataStart = bitPos;
		const int dataEnd = dataStart + (int)dataLen;
		if (dataStart < 0 || dataStart > totalBits
			|| dataLen > static_cast<uint32_t>(totalBits - dataStart))
		{
			BridgeStubDiag("[BRIDGE-ST:%s] truncated SendTable payload len=%u bit=%d/%d\n",
				logTag, dataLen, dataStart, totalBits);
			break;
		}

		char tblName[256] = {};
		int parsePos = dataStart;
		if (needsDecoder == 1)
		{
			readString(tblName, 256);
			parsePos = bitPos;
		}
		else
		{
			bool tblNameTerm = false;
			for (int i = 0; i < 256; i++)
			{
				if (parsePos + 8 > dataEnd)
					return rejectSendTable("?", "unterminated table name");
				const uint32_t ch = peekBitsAt(parsePos, 8);
				parsePos += 8;
				tblName[i] = (char)ch;
				if (ch == 0)
				{
					tblNameTerm = true;
					break;
				}
			}
			if (!tblNameTerm)
				return rejectSendTable("?", "unterminated table name");
			tblName[255] = 0;
		}

		if (parsePos + 10 > dataEnd)
			return rejectSendTable(tblName, "truncated nProps");
		const uint32_t nProps = peekBitsAt(parsePos, 10);
		parsePos += 10;
		if (nProps > 1023)
			return rejectSendTable(tblName, "nProps");

		for (uint32_t pi = 0; pi < nProps; pi++)
		{
			if (parsePos + 5 > dataEnd)
				return rejectSendTable(tblName, "truncated prop type");
			const uint32_t pType = peekBitsAt(parsePos, 5);
			parsePos += 5;
			if (pType >= 11)
				return rejectSendTable(tblName, "prop type");

			bool nameTerm = false;
			for (int i = 0; i < 256; i++)
			{
				if (parsePos + 8 > dataEnd)
					return rejectSendTable(tblName, "unterminated prop name");
				const uint32_t ch = peekBitsAt(parsePos, 8);
				parsePos += 8;
				if (ch == 0)
				{
					nameTerm = true;
					break;
				}
			}
			if (!nameTerm)
				return rejectSendTable(tblName, "unterminated prop name");

			if (parsePos + 16 + 8 > dataEnd)
				return rejectSendTable(tblName, "truncated prop flags");
			const uint32_t pFlags = peekBitsAt(parsePos, 16);
			parsePos += 16;
			(void)peekBitsAt(parsePos, 8);
			parsePos += 8;

			if (pType == 10 || (pFlags & 0x40))
			{
				bool dtTerm = false;
				for (int i = 0; i < 256; i++)
				{
					if (parsePos + 8 > dataEnd)
						return rejectSendTable(tblName, "unterminated datatable name");
					const uint32_t ch = peekBitsAt(parsePos, 8);
					parsePos += 8;
					if (ch == 0)
					{
						dtTerm = true;
						break;
					}
				}
				if (!dtTerm)
					return rejectSendTable(tblName, "unterminated datatable name");
			}
			else if (pType == 5)
			{
				if (parsePos + 10 > dataEnd)
					return rejectSendTable(tblName, "truncated array count");
				parsePos += 10;
			}
			else
			{
				if (parsePos + 32 + 32 + 7 > dataEnd)
					return rejectSendTable(tblName, "truncated prop fields");
				parsePos += 32 + 32 + 7;
			}

			if (pType != 10)
			{
				++leafTotal;
				if (leafTotal > 65536)
					return rejectSendTable(tblName, "leaf props");
			}
		}

		if (needsDecoder == 1)
		{
			bool has = S21Bridge_HasRecvTable(tblName);
			if (!has)
			{
				const char* nearMatch = nullptr;
				for (const auto& rn : s_s21RecvTableNames)
				{
					if (_stricmp(rn.c_str(), tblName) == 0)
					{
						nearMatch = rn.c_str();
						break;
					}
				}

				if (nearMatch && strlen(nearMatch) == strlen(tblName))
				{
					const int nameBitStart = needsDecoderBitPos + 1 + 32;
					for (int ci = 0; nearMatch[ci]; ci++)
						writeByteAtBit(nameBitStart, ci, (uint8_t)nearMatch[ci]);
					++matched;
					++caseFixed;
					static int s_relCaseLog = 0;
					if (++s_relCaseLog <= 80)
						SDK_Log("[BRIDGE-ST:%s] CASE-FIX '%s' -> '%s'\n",
							logTag, tblName, nearMatch);
				}
				else if (!S21Bridge_FindStub(tblName))
				{
					if (!S21Bridge_CanRecordStub(tblName))
					{
						static int s_relStubRefuse = 0;
						if (++s_relStubRefuse <= 8)
							Warning(eDLL_T::ENGINE,
								"[BRIDGE-ST:%s] refusing stub '%s'\n",
								logTag, tblName);
						++matched;
					}
					else
					{
					int parseStart = bitPos;
					uintptr_t stubRT = S21Bridge_BuildStubRecvTable(
						tblName, data, totalBits, bitPos);
					bitPos = parseStart;
					if (stubRT)
					{
						S21Bridge_RecordStub(tblName, stubRT);
						++stubsBuilt;
						static int s_relStubLog = 0;
						if (++s_relStubLog <= 200)
							SDK_Log("[BRIDGE-ST:%s] STUB '%s' rt=%p\n",
								logTag, tblName, (void*)stubRT);
					}
					else
					{
						Warning(eDLL_T::ENGINE,
							"[BRIDGE-ST:%s] failed to build RecvTable stub for '%s'\n",
							logTag, tblName);
						if (needsDecoderBitPos >= 0 && needsDecoderBitPos < totalBits)
							data[needsDecoderBitPos / 8] &= ~(1 << (needsDecoderBitPos % 8));
					}
					}
				}
				else
				{
					++matched;
				}
			}
			else
			{
				++matched;
			}
		}
		else
		{
			++noDecoder;
		}

		bitPos = dataEnd;
	}

	if (sendTables > 0)
		s_dbSawSendTables = true;

	if (scanLogN <= 40 || matched || stubsBuilt || noDecoder || caseFixed || skippedNonST)
	{
		SDK_Log("[BRIDGE-ST:%s] scan done: messages=%d sendTables=%d skippedNonST=%d matched=%d stubs=%d noDecoder=%d caseFixed=%d bit=%d/%d sawSendTables=%d\n",
			logTag, messages, sendTables, skippedNonST, matched, stubsBuilt, noDecoder,
			caseFixed, bitPos, totalBits, s_dbSawSendTables ? 1 : 0);
	}

	if (matched || stubsBuilt || noDecoder || caseFixed)
	{
		// Map REL-path counters onto the same CI-ST summary shape (flipped ~= stubsBuilt).
		S21Bridge_CIDiag_STSummary(logTag, matched, stubsBuilt + caseFixed, noDecoder);
		S21Bridge_RegisterStubsInList();
		if (S21Bridge_CIDiag_On())
			S21Bridge_CIDiag_Gate("REL_ST_postRegister");
	}

	return true;
}

static double    s_avgRtt          = 0.0;          // smoothed round-trip time (seconds)
static uint32_t  s_prevAckSeq      = 0;
volatile long    s_bridgeReportedRttMs = -1;
// rolling 1-second accumulators
static double    s_flowWinStart   = 0.0;           // window start (GetTickCount64 seconds)
static int       s_inPkts         = 0;             // incoming packets this window
static long long s_inBytes        = 0;             // incoming bytes this window
static long long s_inExpected     = 0;             // incoming packets expected (incl. gaps)
static long long s_inLost         = 0;             // incoming packets missing (S2C seq gaps)
static long long s_inChokeAcc     = 0;             // incoming choke (dedi's choked count)
static int       s_inPrevSeq      = -1;            // last incoming sequence (gap detection)
static uint32_t  s_outRelayWin    = 0;             // s_c2sSeqCounter at window start
static uint32_t  s_outAckWin      = 0;             // dedi ack (relay-seq space) at window start
static bool      s_outWinInit     = false;

static char s_szAppliedPlaylist[64] = {};
static int s_nPlaylistNamesApplied = 0;

void S21Bridge_FlowStatsReset(void)
{
	s_avgRtt = 0.0;
	s_prevAckSeq = 0;
	s_flowWinStart = 0.0;
	s_inPkts = 0;
	s_inBytes = 0;
	s_inExpected = 0;
	s_inLost = 0;
	s_inChokeAcc = 0;
	s_inPrevSeq = -1;
	s_outRelayWin = 0;
	s_outAckWin = 0;
	s_outWinInit = false;
	s_flowStat[0] = {};
	s_flowStat[1] = {};
	InterlockedExchange(&s_bridgeReportedRttMs, -1);
	s_szAppliedPlaylist[0] = '\0';
	s_nPlaylistNamesApplied = 0;
}
static uint8_t* s_dbScratchBuffer   = nullptr;      // Reassembly scratch buffer
static ConVar sdk_bridge_defer_signon("sdk_bridge_defer_signon", "1", FCVAR_RELEASE,
    "Run the bridge signon-DataBlock processing (OnDataBlockComplete -> ProcessMessages "
    "-> engine SetSignonState/SPAWN) on the MAIN thread via Cbuf_Execute instead of the "
    "net thread. Breaks the net-thread vs BSP-load (CStaticProp::Init) rpak-lock race that "
    "intermittently hangs or crashes the canyonlands load. Default 1 (ON). 0 = legacy "
    "synchronous net-thread processing (reproduces the race).");

// Per-block DataBlock ACKs so the dedi resends only missing fragments.
// 0 = ack-on-complete.
static ConVar sdk_bridge_db_incremental_ack("sdk_bridge_db_incremental_ack", "1", FCVAR_RELEASE,
    "Bridge sends incremental per-block DataBlock ACKs (P-bitmap) as fragments arrive so the "
    "dedi resends only missing fragments -- collapses the ~10s/snapshot large-DataBlock stall. "
    "0 = legacy (ACK only on full completion).");
static uint8_t  s_deferredSignon3Buf[1024] = {};
// S2C ScriptRemote: S3 net_ScriptMessage(68) isTyped=0, inject by wire-carried name.
// Bypasses the native 600-cap receive queue; local-entry + argCount gates still apply.
static ConVar bridge_s2c_scriptremote("bridge_s2c_scriptremote", "1", FCVAR_RELEASE,
	"Inject the dedi(S3)-originated name-carried ScriptRemote S->C RPC (S3 net_ScriptMessage(68), "
	"isTyped=0) via CSquirrelVM::ExecuteFunction. 1 = ON (default, product). 0 = OFF. "
	"Entity args resolve at release time, so all arg types inject. Parse+log always runs.");
// Per-call receipt log for the S2C lane. Off by default: the lane carries every
// ServerCallback_* the dedi emits, so this is only wanted while bisecting a
// "the server called it but the HUD never moved" report.
ConVar bridge_net_flow_diag("bridge_net_flow_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[PKT-1S]/[S2C-SNAP]/[WT-PROBE] per-second packet flow and per-snapshot ack probes. 0 = off.");
static ConVar bridge_s2c_scriptremote_log("bridge_s2c_scriptremote_log", "0", FCVAR_DEVELOPMENTONLY,
	"Log every name-carried S->C ScriptRemote the client executes ([S2C-SR-EXEC] name/argc/rv).");
// Native S21 never runs a remote at parse time: stamp with snapshot tick and
// drain from ExecuteCallQueue when that tick is current.
static ConVar bridge_s2c_scriptremote_tickgate("bridge_s2c_scriptremote_tickgate", "1", FCVAR_RELEASE,
	"Hold each dedi S->C ScriptRemote call until the snapshot it was stamped with is "
	"current, matching the native receive queue. 1 = on (default, product). "
	"0 = run at packet-parse time (repaints read stale state).");
static ConVar bridge_s2c_scriptremote_release("bridge_s2c_scriptremote_release", "1", FCVAR_RELEASE,
	"S2C ScriptRemote release tick. 1 = interpolation base (native ordering, default); "
	"2 = latest fully applied snapshot tick. Parse-time remains diagnostic-only via "
	"bridge_s2c_scriptremote_tickgate.",
	true, 1.f, true, 2.f);

// If the snapshot tick never reaches a queued stamp, force-release after this
// many drains rather than lose the call. Every forced release is a loud warning.
static ConVar bridge_s2c_scriptremote_max_hold("bridge_s2c_scriptremote_max_hold", "64", FCVAR_RELEASE,
	"Max engine drain passes a tick-gated S->C ScriptRemote call may wait before it is "
	"released regardless of tick. Each forced release warns.");

// Arrival-to-current latency probe: how long a received snapshot waits before its
// wire tick becomes the client's current snapshot tick (interp-depth question).
static ConVar bridge_lerp_depth_probe("bridge_lerp_depth_probe", "0", FCVAR_DEVELOPMENTONLY,
	"Measure snapshot arrival-to-current latency (ms). Logs [LERP-DEPTH] every 64 samples.");
static LerpDepthSlot_s s_lerpDepthRing[kLerpDepthRingSize];
static std::atomic<uint32_t> s_lerpDepthWrite{ 0 };
static LONGLONG s_lerpDepthQpcFreq = 0;
static int      s_lerpDepthWinN = 0;
static double   s_lerpDepthWinSum = 0.0;
static double   s_lerpDepthWinMin = 0.0;
static double   s_lerpDepthWinMax = 0.0;

static void S21Bridge_LerpDepth_OnSnapshotAccepted(uint32_t wireTick)
{
	if (!bridge_lerp_depth_probe.GetBool())
		return;

	LARGE_INTEGER now = {};
	QueryPerformanceCounter(&now);
	const uint32_t slot = s_lerpDepthWrite.fetch_add(1, std::memory_order_relaxed)
		% (uint32_t)kLerpDepthRingSize;
	LerpDepthSlot_s& e = s_lerpDepthRing[slot];
	e.tick.store(wireTick, std::memory_order_relaxed);
	e.qpc.store((uint64_t)now.QuadPart, std::memory_order_relaxed);
	e.ready.store(1, std::memory_order_release);
}

static void S21Bridge_LerpDepth_OnCurrentTick(uint32_t curTick)
{
	if (!bridge_lerp_depth_probe.GetBool())
		return;

	if (s_lerpDepthQpcFreq <= 0)
	{
		LARGE_INTEGER f = {};
		QueryPerformanceFrequency(&f);
		s_lerpDepthQpcFreq = f.QuadPart;
		if (s_lerpDepthQpcFreq <= 0)
			return;
	}

	LARGE_INTEGER now = {};
	QueryPerformanceCounter(&now);

	for (int i = 0; i < kLerpDepthRingSize; ++i)
	{
		LerpDepthSlot_s& e = s_lerpDepthRing[i];
		if (e.ready.load(std::memory_order_acquire) == 0)
			continue;

		const uint32_t t = e.tick.load(std::memory_order_relaxed);
		if (static_cast<int32_t>(curTick - t) < 0)
			continue;

		const uint64_t q = e.qpc.load(std::memory_order_relaxed);
		e.ready.store(0, std::memory_order_relaxed);

		const double ms = 1000.0 * (double)((uint64_t)now.QuadPart - q)
			/ (double)s_lerpDepthQpcFreq;
		if (s_lerpDepthWinN == 0)
		{
			s_lerpDepthWinMin = ms;
			s_lerpDepthWinMax = ms;
			s_lerpDepthWinSum = ms;
		}
		else
		{
			if (ms < s_lerpDepthWinMin) s_lerpDepthWinMin = ms;
			if (ms > s_lerpDepthWinMax) s_lerpDepthWinMax = ms;
			s_lerpDepthWinSum += ms;
		}
		++s_lerpDepthWinN;

		if (s_lerpDepthWinN >= 64)
		{
			Warning(eDLL_T::CLIENT,
				"[LERP-DEPTH] n=64 min=%.2f avg=%.2f max=%.2f curTick=%u\n",
				s_lerpDepthWinMin,
				s_lerpDepthWinSum / 64.0,
				s_lerpDepthWinMax,
				curTick);
			s_lerpDepthWinN = 0;
			s_lerpDepthWinSum = 0.0;
			s_lerpDepthWinMin = 0.0;
			s_lerpDepthWinMax = 0.0;
		}
	}
}
// [SEC] S2C ScriptRemote rate budget (native receive-queue anti-desync is ~600).
static ConVar bridge_s2c_scriptremote_rate_max("bridge_s2c_scriptremote_rate_max", "600", FCVAR_RELEASE,
	"[SEC] Max S2C ScriptRemote injects accepted per wall-second (native ~600 queue parity). "
	"0 = unlimited (lab only). Fail closed when exceeded.");
// Optional explicit name allowlist. Empty = registered-local-entry only (default, product).
static ConVar bridge_s2c_scriptremote_allow("bridge_s2c_scriptremote_allow", "", FCVAR_RELEASE,
	"[SEC] Extra comma-separated S2C ScriptRemote name allowlist. Empty = registered "
	"client remotes plus the compiled UI allowlist. Non-empty = name must also appear "
	"here. Fail closed on miss. Wire isUI is ignored unless the name is UI-allowlisted.");


// Stamp m_nOutSequenceNrAck in the engine's sequence space, not the bridge relay counter.
static ConVar bridge_ack_xlate("bridge_ack_xlate", "1", FCVAR_RELEASE,
	"S21 bridge: translate the dedi's bridge-space C2S ack into the engine's own outgoing "
	"sequence space before stamping m_nOutSequenceNrAck. Default 1.");


// svc_Snapshot re-encode counters (written by Hook_ProcessPacket's svc_Snapshot path)
static volatile LONG s_snapReencodeTotal = 0;
static volatile LONG s_snapReencodeDelta = 0;
static volatile LONG s_snapReencodeFull  = 0;



// FILE-SCOPE scratch: the message pump uses __try/__except; a function-local
// with a destructor would force object unwinding (C2712).
static std::vector<uint8_t> g_snapBufVec;
static volatile LONG s_snapRfbOk         = 0;
static volatile LONG s_snapRfbFail       = 0;
static volatile LONG s_snapRfbCrash      = 0;
static volatile LONG s_snapProcOk        = 0;
static volatile LONG s_snapProcFail      = 0;
static volatile LONG s_snapProcCrash     = 0;

//-----------------------------------------------------------------------------
// Transcoder fire counters, dumped periodically. Per-format logs are lifetime-capped.
//-----------------------------------------------------------------------------
static volatile LONG s_pmCallCount         = 0;   // S21Bridge_ProcessMessages calls
static volatile LONG s_pmIterTotal         = 0;   // total dispatch-loop iterations
static volatile LONG s_pmS3TypeFires[128]  = {};  // per-S3-type fire count
static volatile LONG s_ppEntryCount        = 0;   // Hook_ProcessPacket entries
static volatile LONG s_ppBadNonce          = 0;   // dropped by nonce magic check
static volatile LONG s_ppSubchanCalled     = 0;   // subchannel data parsed
static volatile LONG s_ppSubchanFailed     = 0;   // subchannel parse returned false
static volatile LONG s_ppPmCalled          = 0;   // ProcessMessages run on unreliable
static volatile LONG s_ppPmSkippedNoBits   = 0;   // unreliable body too small
static volatile LONG s_ppPmSkippedSubFail  = 0;   // gated off by !subChannelOk
static LONG          s_pmLastHistDumpCall  = 0;   // cadence anchor for dump

// s_S3ToS21 covers indices 0..68 (SDK-added types through net_ScriptMessage).
// The wire type field is NETMSG_TYPE_BITS (7) so legal values are 0..127. Types
static constexpr int S3_NETMSG_TABLE_MAX = 68;

static ConVar bridge_desync_dump("bridge_desync_dump", "0", FCVAR_DEVELOPMENTONLY,
	"Hex-dump the transfer bytes around a bitstream desync (impossible net-message type).");

// S3 svc_DLCNotifyOwnership body: 64-bit entitlement bitfield + N*16 balance bits.
// N is NOT on the wire -- both S3 ends use Host_GetEntitlementBalances->m_Size.
static ConVar bridge_s3_dlc_balance_count("bridge_s3_dlc_balance_count", "151", FCVAR_RELEASE,
	"S3 svc_DLCNotifyOwnership balance count for body-skip (not applied). "
	"Must match dedi Host_GetEntitlementBalances size. -1 = auto when transfer "
	"remainder is pure DLC (+ optional 1-bit pad).");

// Body-skip success counters (lifetime) -- prove SKIP-TRANSFER is gone for known types.
static std::atomic<uint32_t> g_bodySkipOk[128];
static std::atomic<uint32_t> g_bodySkipFail[128];

static void Bridge_RecordBodySkip(int s3cmd, bool ok)
{
	if (s3cmd < 0 || s3cmd >= 128)
		return;
	if (ok)
		g_bodySkipOk[s3cmd].fetch_add(1, std::memory_order_relaxed);
	else
		g_bodySkipFail[s3cmd].fetch_add(1, std::memory_order_relaxed);
}

static const char* S21Bridge_S3TypeName(int s3type)
{
	switch (s3type)
	{
		case 0:  return "net_NOP";
		case 1:  return "net_Disconnect";
		case 2:  return "(reserved)";
		case 3:  return "net_StringCmd";
		case 4:  return "net_SetConVar";
		case 5:  return "net_SignonState [BRIDGE]";
		case 6:  return "net_MTXUserMsg";
		case 7:  return "svc_ServerInfo [BRIDGE]";
		case 8:  return "svc_SendTable";
		case 9:  return "svc_ClassInfo";
		case 10: return "svc_SetPause [BODY-SKIP]";
		case 11: return "svc_Playlists";
		case 12: return "svc_CreateStringTable [BRIDGE]";
		case 13: return "svc_UpdateStringTable";
		case 14: return "svc_VoiceData";
		case 15: return "svc_DurangoVoiceData";
		case 16: return "svc_Print [BRIDGE -2]";
		case 17: return "svc_Sounds";
		case 18: return "svc_FixAngle";
		case 19: return "svc_CrosshairAngle";
		case 20: return "svc_GrantClientSidePickup";
		case 21: return "(gap in S3)";
		case 22: return "svc_ServerTick [BRIDGE -2]";
		case 23: return "svc_PersistenceDefFile";
		case 24: return "svc_UseCachedPersistenceDefFile";
		case 25: return "svc_PersistenceBaseline";
		case 26: return "svc_PersistenceUpdateVar";
		case 27: return "svc_PersistenceNotifySaved";
		case 28: return "svc_DLCNotifyOwnership [BODY-SKIP]";
		case 29: return "svc_MatchmakingETAs";
		case 30: return "svc_MatchmakingStatus";
		case 31: return "svc_MTXUserInfo";
		case 32: return "svc_PlaylistChange [BRIDGE -2]";
		case 33: return "svc_SetTeam";
		case 34: return "svc_PlaylistOverrides [BODY-SKIP]";
		case 35: return "svc_AntiCheat";
		case 36: return "svc_AntiCheatChallenge [BODY-SKIP]";
		case 37: return "svc_UserMessage [BRIDGE]";
		case 38: return "(gap in S3)";
		case 39: return "(gap in S3)";
		case 40: return "svc_Snapshot [BRIDGE]";
		case 41: return "svc_TempEntities";
		case 42: return "svc_Menu [BODY-SKIP]";
		case 43: return "svc_CmdKeyValues [BODY-SKIP]";
		case 44: return "svc_DatatableChecksum [BRIDGE -2]";
		case 45: return "clc_ClientInfo";
		case 46: return "clc_Move";
		case 47: return "clc_VoiceData";
		case 48: return "clc_DurangoVoiceData";
		case 49: return "(gap in S3)";
		case 50: return "clc_FileCRCCheck [SKIP]";
		case 51: return "(gap in S3)";
		case 52: return "clc_LoadingProgress";
		case 53: return "clc_PersistenceRequestSave [SKIP]";
		case 54: return "clc_PersistenceClientToken";
		case 55: return "clc_SetClientEntitlements";
		case 56: return "clc_SetPlaylistVarOverride [SKIP]";
		case 57: return "clc_ClaimClientSidePickup";
		case 58: return "(gap in S3)";
		case 59: return "clc_CmdKeyValues [SKIP]";
		case 60: return "clc_ClientTick";
		case 61: return "clc_ClientSayText";
		case 62: return "clc_PINTelemetryData [SKIP]";
		case 63: return "clc_AntiCheat";
		case 64: return "clc_AntiCheatChallenge [SKIP]";
		case 65: return "clc_GamepadMsg";
		case 66: return "svc_SetClassVar [SDK]";
		case 67: return "svc_SystemSayText [SDK]";
		case 70: return "svc_ChatBuilder [SDK]";
		case 68: return "net_ScriptMessage [SDK, S2C lane]";
		case 69: return "svc_DebugOverlay [SDK]";
		default:
			if (s3type < 0 || s3type > S3_NETMSG_TYPE_MAX)
				return "(impossible - desync)";
			if (s3type > S3_NETMSG_TABLE_MAX)
				return "(unmapped > table)";
			return "(unknown)";
	}
}

static void S21Bridge_PpPmHistogramDump()
{
    SDK_Log("[BRIDGE-HIST] ===== TRANSCODER DIAGNOSTICS (lifetime counts) =====\n");
    SDK_Log("[BRIDGE-HIST] PP: entry=%ld badNonce=%ld subOk=%ld subFail=%ld\n",
        s_ppEntryCount, s_ppBadNonce, s_ppSubchanCalled, s_ppSubchanFailed);
    SDK_Log("[BRIDGE-HIST] PP: PMcalled=%ld PMskipNoBits=%ld PMskipSubFail=%ld\n",
        s_ppPmCalled, s_ppPmSkippedNoBits, s_ppPmSkippedSubFail);
    SDK_Log("[BRIDGE-HIST] PM: calls=%ld iters=%ld snap=(re=%ld full=%ld delta=%ld rfbOk=%ld rfbFail=%ld procOk=%ld procFail=%ld)\n",
        s_pmCallCount, s_pmIterTotal,
        s_snapReencodeTotal, s_snapReencodeFull, s_snapReencodeDelta,
        s_snapRfbOk, s_snapRfbFail, s_snapProcOk, s_snapProcFail);
    for (int i = 0; i < 128; i++)
    {
        const LONG n = s_pmS3TypeFires[i];
        if (n > 0)
            SDK_Log("[BRIDGE-HIST]   s3type[%3d] = %7ld  %s\n",
                i, n, S21Bridge_S3TypeName(i));
    }
    // Body-skip health: known suppress types that must continue the transfer.
    {
        char skipLine[512];
        int len = 0;
        for (int i = 0; i < 128; i++)
        {
            const uint32_t ok = g_bodySkipOk[i].load(std::memory_order_relaxed);
            const uint32_t fail = g_bodySkipFail[i].load(std::memory_order_relaxed);
            if ((ok || fail) && len < (int)sizeof(skipLine) - 48)
            {
                int n = snprintf(skipLine + len, sizeof(skipLine) - len,
                    " s3=%d ok=%u fail=%u", i, ok, fail);
                if (n > 0) len += n;
            }
        }
        if (len > 0)
            SDK_Log("[BRIDGE-HIST] body-skip:%s\n", skipLine);
    }
    SDK_Log("[BRIDGE-HIST] ===== END =====\n");
}

static void GenerateDeltas_RecycleDummyPool(void)
{
	InterlockedExchange(&s_gdDummyPoolNext, 0);
}
void S21Bridge_FlushS2CScriptRemote(const char* reason);
static ConVar bridge_playlist_override_allow("bridge_playlist_override_allow", "name,map_name", FCVAR_RELEASE,
	"Comma-separated playlist var names the server may override. Empty = none.");

// [SEH-ADDR] Capture faulting address + exception code for the __except body.
static thread_local void* s_lastCrashAddr = nullptr;
static thread_local unsigned long s_lastCrashCode = 0;
static int S21Bridge_CrashFilter(EXCEPTION_POINTERS* ep)
{
    if (ep && ep->ExceptionRecord)
    {
        s_lastCrashAddr = ep->ExceptionRecord->ExceptionAddress;
        s_lastCrashCode = ep->ExceptionRecord->ExceptionCode;
    }
    else
    {
        s_lastCrashAddr = nullptr;
        s_lastCrashCode = 0;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// Stage 1 -> 2. Called from PollReceive on S2C_CHALLENGE (ffffffff 49).
// Builds the S3 C2S_CONNECT bitstream and sends it. Pre-activates the bridge
// so the server's follow-up netchan packets flow through the normal drain.
static void S21Bridge_OnS2CChallenge(uint32_t challenge)
{
    if (s_hsStage != BridgeHsStage::ChallengeSent) return;

    SDK_Log("[NET-OBS] HANDSHAKE: S2C_CHALLENGE received, challenge=0x%08X\n", challenge);
    const uint64_t bridgeNucleusID = S21Bridge_GetConnectNucleusID();
    const std::string bridgePersona = S21Bridge_GetConnectPersona();
    SDK_Log("[NET-OBS] HANDSHAKE: using nucleusId=%llu persona='%s' (Origin) password=%d\n",
        (unsigned long long)bridgeNucleusID, bridgePersona.c_str(),
        Bridge_GetConnectPasswordTag()[0] ? 1 : 0);

    // ─── Build S3-format C2S_CONNECT via bitstream writer ───
    // Build S3 C2S_CONNECT payload for online-auth token ConVars.
    // Wire must stay under net_maxroutable (1200).
    static constexpr int kC2SConnectPayloadBytes = 1152; // worst-case full token ~1018B body
    static constexpr int kC2SConnectWireHeader = 5;      // 0xFFFFFFFF + C2S_CONNECT
    uint32_t bsDwords[kC2SConnectPayloadBytes / 4] = {};
    int bsBitPos = 0;
    bool bsOverflow = false;
    int bsOverflowNeedBytes = 0;
    auto bsWriteBits = [&](uint64_t value, int numBits) {
        if (bsOverflow || numBits <= 0)
            return;
        const int capBits = kC2SConnectPayloadBytes * 8;
        if (bsBitPos + numBits > capBits)
        {
            bsOverflow = true;
            bsOverflowNeedBytes = (bsBitPos + numBits + 7) / 8;
            return;
        }
        uint64_t val = value & ((numBits >= 64) ? ~0ULL : ((1ULL << numBits) - 1));
        int remaining = numBits;
        while (remaining > 0) {
            int dIdx = bsBitPos / 32;
            int bitOff = bsBitPos % 32;
            int space = 32 - bitOff;
            int take = (remaining < space) ? remaining : space;
            uint32_t mask = (take >= 32) ? 0xFFFFFFFFU : ((1U << take) - 1);
            bsDwords[dIdx] |= (uint32_t)(val & mask) << bitOff;
            bsBitPos += take;
            val >>= take;
            remaining -= take;
        }
    };
    auto bsWriteU32 = [&](uint32_t v) { bsWriteBits(v, 32); };
    auto bsWriteU8  = [&](uint8_t v)  { bsWriteBits(v, 8); };
    auto bsWriteI64 = [&](uint64_t v) { bsWriteBits(v & 0xFFFFFFFF, 32); bsWriteBits(v >> 32, 32); };
    auto bsWriteStr = [&](const char* s) {
        if (!s) s = "";
        for (const char* p = s; *p; ++p) bsWriteU8((uint8_t)*p);
        bsWriteU8(0);
    };
    auto bsWriteBit = [&](int v) { bsWriteBits(v & 1, 1); };
    // LIVE ConVar string at send time; empty if unresolved (must still emit the key).
    auto liveCVarStr = [](const char* name) -> const char* {
        if (g_pCVar)
        {
            ConVar* const liveVar = g_pCVar->FindVar(name);
            if (liveVar)
            {
                const char* const liveStr = liveVar->GetString();
                if (liveStr)
                    return liveStr;
            }
        }
        return "";
    };

    bsWriteU32(529);                    // protocol
    bsWriteU32(2001);                   // authChallenge
    bsWriteU32(challenge);              // challenge echo
    bsWriteU32(0);                      // reservation
    bsWriteU8(2);                       // authProtocol (< 8)
    bsWriteI64(bridgeNucleusID);        // platform user id field (S3 C2S layout; value = Nucleus)
    bsWriteStr(bridgePersona.c_str());  // persona name
    // Challenge-bound HMAC tag; the dedi recomputes it from sv_password + the
    // challenge it issued.
    char wirePwTag[24];
    Bridge_WirePasswordTag(challenge, wirePwTag, sizeof(wirePwTag));
    bsWriteStr(wirePwTag);
    bsWriteI64(0);                      // nonce1
    bsWriteI64(0);                      // nonce2
    bsWriteU32(0);                      // clientToken
    // sendtableHash: CRC64 of scripts/entitlements.rson on the S3 server.
    static constexpr uint64_t kS3ServerSendTableCRC = 0xA28012AACDFB8F7EULL;
    bsWriteI64(kS3ServerSendTableCRC);  // field 12: sendtableHash (CRC64)

    // Field 13 is 64 bits (not 512). Wrong width shifts later fields and fails connect with a hash-like desync reject.
    bsWriteI64(0); // field 13: signature blob (8 bytes = 64 bits, zeroed)

    // DLC count must match the server's entitlement slot count so later markers stay aligned.
    // Emit the same count of zero u16 entries as the S3 server expects.
    static constexpr uint8_t  kS3ServerDLCCount = 31;
    bsWriteU8(kS3ServerDLCCount);                 // field 14: DLC count
    for (int i = 0; i < kS3ServerDLCCount; ++i)
        bsWriteBits(0, 16);                       // field 14b: N × u16 entries (all 0)
    bsWriteBit(0); bsWriteBit(0); bsWriteBit(0); bsWriteBit(0); // 4 flag bits
    bsWriteBit(1);                      // hasAuthString = 1
    bsWriteStr("");                     // authString
    bsWriteU32(74565);                  // marker (0x12345)
    // plugin/ConVar list: nucleus pair required for hasAuthString path; token
    // trio so Authenticate can FindKey before the post-CONNACCEPT userinfo dump.
    bsWriteU8(5);                       // pluginCount
    // Per-plugin format (from S3 client)
    // 6 bits: plugin-name-table index (0 = name not in table, full string follows)
    bsWriteBits(0, 6); // plugin-name-table index = 0 -> string follows
    bsWriteStr("nucleus_id"); bsWriteStr("0");
    bsWriteBits(0, 6); // plugin-name-table index = 0 -> string follows
    bsWriteStr("nucleus_pid"); bsWriteStr("0");
    bsWriteBits(0, 6);
    bsWriteStr("cl_onlineAuthToken"); bsWriteStr(liveCVarStr("cl_onlineAuthToken"));
    bsWriteBits(0, 6);
    bsWriteStr("cl_onlineAuthTokenSignature1"); bsWriteStr(liveCVarStr("cl_onlineAuthTokenSignature1"));
    bsWriteBits(0, 6);
    bsWriteStr("cl_onlineAuthTokenSignature2"); bsWriteStr(liveCVarStr("cl_onlineAuthTokenSignature2"));

    if (bsOverflow)
    {
        Warning(eDLL_T::CLIENT,
            "S21Bridge_OnS2CChallenge: C2S_CONNECT bitstream overflow "
            "(required %d bytes, available %d) -- connect abandoned\n",
            bsOverflowNeedBytes > 0 ? bsOverflowNeedBytes : (bsBitPos + 7) / 8,
            kC2SConnectPayloadBytes);
        return;
    }

    int totalBytes = (bsBitPos + 7) / 8;
    if (totalBytes < 256) totalBytes = 256; // server requires min 256B payload

    const int pktLen = kC2SConnectWireHeader + totalBytes;
    if (pktLen > 1200)
    {
        Warning(eDLL_T::CLIENT,
            "S21Bridge_OnS2CChallenge: C2S_CONNECT exceeds net_maxroutable "
            "(required %d bytes, available 1200) -- connect abandoned\n",
            pktLen);
        return;
    }

    uint8_t connectPkt[kC2SConnectWireHeader + kC2SConnectPayloadBytes] = {};
    connectPkt[0] = 0xFF; connectPkt[1] = 0xFF;
    connectPkt[2] = 0xFF; connectPkt[3] = 0xFF;
    connectPkt[4] = 0x41;               // C2S_CONNECT
    memcpy(connectPkt + kC2SConnectWireHeader, bsDwords, totalBytes);

    // Pre-activate the bridge BEFORE sending CONNECT. The server starts
    // streaming subchannel fragments immediately after ACK'ing CONNECT; if
    s_bridgeActive = true;
    S21Bridge_StartKeepaliveThread();

    // The bridge has many SDK SEH wrappers (RecvTable_Decode on a bad delta,
    // svc_Snapshot Process, etc.) that catch & recover from crashes the stock
    { extern bool g_bCrashHandlerNoFatal; g_bCrashHandlerNoFatal = true; }

    // Diagnostic: dump the wire bytes around the sendtableHash field.
    // The S3 parser reads u32 protocol/authChallenge/challenge/reservation
    SDK_Log("[NET-OBS] HANDSHAKE: C2S_CONNECT bytes[60..76]="
            " %02X %02X %02X %02X | %02X %02X %02X %02X %02X %02X %02X %02X |"
            " %02X %02X %02X %02X (hash should be LE 7E 8F FB CD AA 12 80 A2)\n",
        connectPkt[60], connectPkt[61], connectPkt[62], connectPkt[63],
        connectPkt[64], connectPkt[65], connectPkt[66], connectPkt[67],
        connectPkt[68], connectPkt[69], connectPkt[70], connectPkt[71],
        connectPkt[72], connectPkt[73], connectPkt[74], connectPkt[75]);

    const int sent = sendto(s_bridgeSocket,
        reinterpret_cast<const char*>(connectPkt), pktLen, 0,
        reinterpret_cast<const sockaddr*>(&s_bridgeDest), sizeof(s_bridgeDest));

    SDK_Log("[NET-OBS] HANDSHAKE: sent C2S_CONNECT %d/%d bytes (payload=%d), bridge pre-activated\n",
        sent, pktLen, totalBytes);

    s_hsStage         = BridgeHsStage::ConnectSent;
    s_hsStageDeadline = GetTickCount64() + 15000;
}

// Stage timeout tick. Runs every PollReceive. Resets Idle if deadline passed.
static void S21Bridge_HandshakeTick()
{
    if (s_hsStage == BridgeHsStage::Idle) return;
    if (GetTickCount64() <= s_hsStageDeadline) return;

    if (s_hsStage == BridgeHsStage::ConnectSent && !s_connAcceptDone)
    {
        const char* pszMap = g_bridgeConnMapName[0] ? g_bridgeConnMapName : "mp_lobby";
        Warning(eDLL_T::ENGINE,
            "[BRIDGE] CONNACCEPT not seen; synthesizing CONNECTED for '%s'\n", pszMap);
        S21Bridge_OnConnAccept(pszMap, "");
        return;
    }

    SDK_Log("[NET-OBS] HANDSHAKE: stage %d timed out, resetting\n", (int)s_hsStage);

    if (!s_bridgeActive && s_bridgeSocket != INVALID_SOCKET)
    {
        // Haven't pre-activated yet -- safe to tear socket down so the next
        // engine retry can start clean. After pre-activation the socket is
        // needed for the active bridge; don't touch it here.
        closesocket(s_bridgeSocket);
        s_bridgeSocket = INVALID_SOCKET;
    }
    s_hsStage = BridgeHsStage::Idle;
    Bridge_NotifyConnectSessionEnded();
    PakLobby_OnSessionReset();
    MantleBoostClient_OnSessionReset();
}

// Copy a maybe-bad C string under SEH into a fixed buffer.
static void S21Bridge_CIDiag_SafeStr(uintptr_t p, char* out, int outLen)
{
	if (!out || outLen <= 0)
		return;
	out[0] = '\0';
	if (p < 0x10000ull || p > 0x00007FFFFFFFFFFFull)
	{
		strncpy(out, "?", (size_t)outLen - 1);
		out[outLen - 1] = '\0';
		return;
	}
	__try
	{
		const char* s = reinterpret_cast<const char*>(p);
		int i = 0;
		for (; i < outLen - 1; ++i)
		{
			const char c = s[i];
			out[i] = c;
			if (!c)
				break;
		}
		out[outLen - 1] = '\0';
		if (i == outLen - 1)
			out[outLen - 1] = '\0';
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		strncpy(out, "<fault>", (size_t)outLen - 1);
		out[outLen - 1] = '\0';
	}
}

static void S21Bridge_CIDiag_WriteEmergencyDump(const char* tag, EXCEPTION_POINTERS* ep)
{
	if (!S21Bridge_CIDiag_On() || !s_origMiniDumpWriteDump || !ep)
		return;

	static volatile LONG s_classInfoDumpOnce = 0;
	const bool bClassInfo = (tag && strcmp(tag, "ClassInfo_Process") == 0);
	if (bClassInfo && InterlockedCompareExchange(&s_classInfoDumpOnce, 1, 0) != 0)
		return;

	char path[MAX_PATH] = {};
	extern std::string g_LogSessionDirectory;
	if (!g_LogSessionDirectory.empty())
		_snprintf_s(path, _TRUNCATE, "%s/ci_crash_%s.dmp", g_LogSessionDirectory.c_str(), tag ? tag : "x");
	else
		_snprintf_s(path, _TRUNCATE, "platform/logs/client/ci_crash_%s.dmp", tag ? tag : "x");

	const HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		Warning(eDLL_T::ENGINE, "[CI-WALK] MiniDump CreateFile failed path='%s' err=%lu\n",
			path, GetLastError());
		return;
	}

	// MINIDUMP_EXCEPTION_INFORMATION layout: ThreadId, ExceptionPointers, ClientPointers
	struct {
		DWORD ThreadId;
		EXCEPTION_POINTERS* ExceptionPointers;
		BOOL ClientPointers;
	} mei = {};
	mei.ThreadId = GetCurrentThreadId();
	mei.ExceptionPointers = ep;
	mei.ClientPointers = FALSE;

	// MiniDumpWithDataSegs | MiniDumpWithThreadInfo. HandleData is omitted:
	// this can run from ProcessMessages while s_pmMutex is held.
	const DWORD dumpType = 0x00000001u | 0x00001000u;
	const BOOL ok = s_origMiniDumpWriteDump(
		GetCurrentProcess(), GetCurrentProcessId(), hFile, dumpType, &mei, nullptr, nullptr);
	CloseHandle(hFile);

	Warning(eDLL_T::ENGINE, "[CI-WALK] emergency MiniDump %s path='%s'\n",
		ok ? "OK" : "FAIL", path);
	SDK_Log("[CI-WALK] emergency MiniDump %s path='%s'\n", ok ? "OK" : "FAIL", path);
	S21Bridge_CIDiag_Flush();
}

// SEH-safe ClassInfo inventory walk + stub ClientClass second pass.
// Replaces the unguarded [CLASS-MAP] loop that silent-killed on district reload.
static void S21Bridge_CIDiag_WalkAndStubPatch(void)
{
	int nClasses = -1;
	uintptr_t srvClasses = 0;
	S21Bridge_CIDiag_ReadClassMeta(&nClasses, &srvClasses);

	SDK_Log("[CI-WALK] BEGIN nClasses=%d arr=%p\n", nClasses, (void*)srvClasses);
	S21Bridge_CIDiag_Flush();

	if (!srvClasses || nClasses <= 0 || nClasses > 4096)
	{
		Warning(eDLL_T::ENGINE,
			"[CI-WALK] ABORT bad registry nClasses=%d arr=%p\n", nClasses, (void*)srvClasses);
		SDK_Log("[CLASS-MAP] === S3->S21 Class Inventory: %d classes (BAD REGISTRY) ===\n", nClasses);
		S21Bridge_CIDiag_Flush();
		return;
	}

	SDK_Log("[CLASS-MAP] === S3->S21 Class Inventory: %d classes ===\n", nClasses);
	S21Bridge_CIDiag_Flush();

	int matched = 0, unmatched = 0, faults = 0;
	for (int ci = 0; ci < nClasses; ++ci)
	{
		// Announce index BEFORE any load so a hard kill still leaves a breadcrumb.
		SDK_Log("[CI-WALK] enter ci=%d\n", ci);
		S21Bridge_CIDiag_Flush();

		uintptr_t clientClass = 0;
		uintptr_t namePtr = 0;
		uintptr_t dtPtr = 0;
		char className[96] = {};
		char dtName[96] = {};
		int slotSeh = 0;

		__try
		{
			const uintptr_t entry = srvClasses + 32ull * (uintptr_t)ci;
			clientClass = *reinterpret_cast<uintptr_t*>(entry + 0);
			namePtr = *reinterpret_cast<uintptr_t*>(entry + 8);
			dtPtr = *reinterpret_cast<uintptr_t*>(entry + 16);
		}
		__except (S21Bridge_CIDiag_WriteEmergencyDump("slot_load", GetExceptionInformation()),
			EXCEPTION_EXECUTE_HANDLER)
		{
			slotSeh = 1;
			++faults;
			Warning(eDLL_T::ENGINE,
				"[CI-WALK] SEH load slot ci=%d code -- dump written\n", ci);
			SDK_Log("[CI-WALK] SEH load slot ci=%d entry=%p\n",
				ci, (void*)(srvClasses + 32ull * (uintptr_t)ci));
			S21Bridge_CIDiag_Flush();
			continue;
		}

		S21Bridge_CIDiag_SafeStr(namePtr, className, (int)sizeof(className));
		S21Bridge_CIDiag_SafeStr(dtPtr, dtName, (int)sizeof(dtName));

		if (slotSeh)
			continue;

		if (clientClass)
		{
			++matched;
			SDK_Log("[CLASS-MAP] [%3d] OK   '%s' (DT=%s) cc=%p\n",
				ci, className, dtName, (void*)clientClass);
		}
		else
		{
			++unmatched;
			SDK_Log("[CLASS-MAP] [%3d] NULL '%s' (DT=%s) -- S3-only, no S21 ClientClass\n",
				ci, className, dtName);
		}
		// Flush every 16 entries + always on NULL (NULL set is small).
		if ((ci & 15) == 15 || !clientClass)
			S21Bridge_CIDiag_Flush();
	}

	SDK_Log("[CLASS-MAP] === Total: %d matched, %d unmatched (NULL), faults=%d ===\n",
		matched, unmatched, faults);
	S21Bridge_CIDiag_Flush();

	// Stub ClientClass second pass (same logic as pre-diag, with SEH + flush).
	int ccPatched = 0;
	int ccSkipNoStub = 0;
	int ccAllocFail = 0;
	for (int ci = 0; ci < nClasses; ++ci)
	{
		uintptr_t existing = 0;
		uintptr_t dtPtr = 0;
		__try
		{
			const uintptr_t entry = srvClasses + 32ull * (uintptr_t)ci;
			existing = *reinterpret_cast<uintptr_t*>(entry + 0);
			dtPtr = *reinterpret_cast<uintptr_t*>(entry + 16);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			continue;
		}
		if (existing)
			continue;

		char dtName[96] = {};
		S21Bridge_CIDiag_SafeStr(dtPtr, dtName, (int)sizeof(dtName));
		if (!dtName[0] || dtName[0] == '?' || strcmp(dtName, "<fault>") == 0)
			continue;

		const uintptr_t stubRT = S21Bridge_FindStub(dtName);
		if (!stubRT)
		{
			++ccSkipNoStub;
			SDK_Log("[CI-WALK] stub-cc skip no-stub ci=%d dt='%s'\n", ci, dtName);
			continue;
		}

		uint8_t* cc = (uint8_t*)VirtualAlloc(
			nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!cc)
		{
			++ccAllocFail;
			Warning(eDLL_T::ENGINE, "[CI-WALK] VirtualAlloc(cc) failed ci=%d dt='%s'\n", ci, dtName);
			continue;
		}
		memset(cc, 0, 64);
		*reinterpret_cast<const char**>(cc + 0x10) = _strdup(dtName);
		*reinterpret_cast<uintptr_t*>(cc + 0x18) = stubRT;

		__try
		{
			*reinterpret_cast<uintptr_t*>(srvClasses + 32ull * (uintptr_t)ci) = (uintptr_t)cc;
			++ccPatched;
			SDK_Log("[CI-WALK] stub-cc [%3d] '%s' cc=%p rt=%p\n",
				ci, dtName, (void*)cc, (void*)stubRT);
		}
		__except (S21Bridge_CIDiag_WriteEmergencyDump("stub_cc_write", GetExceptionInformation()),
			EXCEPTION_EXECUTE_HANDLER)
		{
			Warning(eDLL_T::ENGINE,
				"[CI-WALK] SEH writing stub ClientClass ci=%d dt='%s'\n", ci, dtName);
			VirtualFree(cc, 0, MEM_RELEASE);
		}
	}

	SDK_Log("[CLASS-MAP] stub-ClientClass: patched %d NULL entries (skipNoStub=%d allocFail=%d)\n",
		ccPatched, ccSkipNoStub, ccAllocFail);
	SDK_Log("[CI-WALK] END matched=%d unmatched=%d faults=%d patched=%d\n",
		matched, unmatched, faults, ccPatched);
	S21Bridge_CIDiag_Flush();
}

// Engine packet scratch handed to us via netpacket_s+0x30. The S21 packet pump
// allocates 262215 bytes, 16-aligns and offsets by 16 before storing the
// pointer (alloc-fail path: a flat 262176), so 256 KB is the usable envelope.
static constexpr size_t kS21EnginePacketCapacity = 262144;
static S21BfReadInit_fn s_S21BfReadInit = nullptr;

uintptr_t S21_GetExeBase()
{
	static uintptr_t s_base = 0;
	if (!s_base)
	{
		s_base = NetObs_GetExeModuleBase();
	}
	return s_base;
}

static void S21_ResolveBfReadFunctions()
{
	if (s_S21BfReadInit) return;
	uintptr_t base = S21_GetExeBase();
	if (!base) return;
	// DX11 / DX12 = bf_read constructor(buf, data, size).
	// DX12 is a red-black tree fixup; calling it here was the
	// CreateDecoders crash at during ClassInfo.
	s_S21BfReadInit = (S21BfReadInit_fn)NetObs_Sym(NetObsSym_t::BfReadInit);
}


//-----------------------------------------------------------------------------
// S21 bf_read helpers -- operate directly on the 64-byte struct
//-----------------------------------------------------------------------------
static bool S21BR_IsOverflowed(const uint8_t* s21)
{
	return *(const uint8_t*)(s21 + S21BR_OVERFLOW) != 0;
}

static int64_t S21BR_GetBitsRead(const uint8_t* s21)
{
	auto pData   = *(const uint8_t* const*)(s21 + S21BR_PDATA);
	auto pDataIn = *(const uint8_t* const*)(s21 + S21BR_PDATAIN);
	auto nBytes  = *(const int64_t*)(s21 + S21BR_DATABYTES);
	auto avail   = *(const int32_t*)(s21 + S21BR_BITSAVAIL);
	if (!pData) return 0;
	// Match ProcessMessages formula: 8*((nBytes&3) + 4*((pDataIn-pData)>>2)) - avail
	return 8 * ((int64_t)(nBytes & 3) + 4 * ((int64_t)(pDataIn - pData) >> 2)) - avail;
}

static int64_t S21BR_GetBitsLeft(const uint8_t* s21)
{
	auto totalBits = *(const int64_t*)(s21 + S21BR_DATABITS);
	int64_t bitsRead = S21BR_GetBitsRead(s21);
	if (bitsRead >= totalBits) return 0;
	return totalBits - bitsRead;
}

// Seek to absolute bit position in the S21 bf_read (kept for future use)
[[maybe_unused]] static void S21BR_Seek(uint8_t* s21, int64_t bitPos)
{
	auto pData    = *(const uint8_t**)(s21 + S21BR_PDATA);
	auto pBufEnd  = *(const uint8_t**)(s21 + S21BR_PBUFEND);
	auto totalBits = *(const int64_t*)(s21 + S21BR_DATABITS);

	if (!pData || bitPos < 0 || bitPos > totalBits)
	{
		*(uint8_t*)(s21 + S21BR_OVERFLOW) = 1;
		return;
	}

	int64_t wordIdx    = bitPos / 32;
	int     bitInWord  = (int)(bitPos % 32);
	const uint8_t* wordPtr = pData + wordIdx * 4;

	if (wordPtr >= pBufEnd)
	{
		// At or past the end -- position at end, mark as consumed
		*(uint32_t*)(s21 + S21BR_INBUFWORD) = 0;
		*(int32_t*)(s21 + S21BR_BITSAVAIL)  = 0;
		*(const uint8_t**)(s21 + S21BR_PDATAIN) = pBufEnd;
		return;
	}

	uint32_t word = *(const uint32_t*)wordPtr;
	*(uint32_t*)(s21 + S21BR_INBUFWORD) = word >> bitInWord;
	*(int32_t*)(s21 + S21BR_BITSAVAIL)  = 32 - bitInWord;
	*(const uint8_t**)(s21 + S21BR_PDATAIN) = wordPtr + 4;
}

// Read N bits (N <= 32) from S21 bf_read. Returns value, advances state.
static uint32_t S21BR_ReadUBits(uint8_t* s21, int numBits)
{
	if (numBits <= 0) return 0;
	if (S21BR_IsOverflowed(s21)) return 0;

	auto& inBufWord = *(uint32_t*)(s21 + S21BR_INBUFWORD);
	auto& bitsAvail = *(int32_t*)(s21 + S21BR_BITSAVAIL);
	auto& pDataIn   = *(const uint8_t**)(s21 + S21BR_PDATAIN);
	auto  pBufEnd   = *(const uint8_t**)(s21 + S21BR_PBUFEND);

	uint32_t mask = (numBits >= 32) ? 0xFFFFFFFFu : ((1u << numBits) - 1);

	if (bitsAvail >= numBits)
	{
		uint32_t result = inBufWord & mask;
		bitsAvail -= numBits;
		if (bitsAvail == 0)
		{
			// Load next word
			if (pDataIn < pBufEnd)
			{
				inBufWord = *(const uint32_t*)pDataIn;
				pDataIn += 4;
				bitsAvail = 32;
			}
			else
			{
				inBufWord = 0;
				bitsAvail = 1; // Match S21 behavior: set to 1 at end
				pDataIn += 4;
			}
		}
		else
		{
			inBufWord >>= numBits;
		}
		return result;
	}

	// Cross word boundary: take remaining bits from current word
	int lowBits = bitsAvail;
	uint32_t result = inBufWord; // all bitsAvail bits

	// Load next word
	if (pDataIn < pBufEnd)
	{
		uint32_t nextWord = *(const uint32_t*)pDataIn;
		pDataIn += 4;

		int highBits = numBits - lowBits;
		uint32_t highMask = (highBits >= 32) ? 0xFFFFFFFFu : ((1u << highBits) - 1);
		result |= (nextWord & highMask) << lowBits;
		inBufWord = nextWord >> highBits;
		bitsAvail = 32 - highBits;
	}
	else
	{
		*(uint8_t*)(s21 + S21BR_OVERFLOW) = 1;
		inBufWord = 0;
		return 0;
	}

	return result;
}

// Skip via ReadUBits, not Seek: S21 bf_read init for non-4-aligned buffers
// starts at pData+(nDataBytes&3), but Seek computes word positions from pData+0.
static void S21BR_SkipBits(uint8_t* s21, int64_t numBits)
{
	if (numBits <= 0) return;
	while (numBits > 32)
	{
		S21BR_ReadUBits(s21, 32);
		numBits -= 32;
	}
	if (numBits > 0)
		S21BR_ReadUBits(s21, (int)numBits);
}

// S3 net_SignonState body after [8b state][32b spawn]: [String][String][QWORD][String].
// Nothing in the body is length-prefixed, so a dropped SignonState must consume it
// or the next cmd in the transfer is read off the leftover cursor.
static void S21BR_SkipSignonBody(uint8_t* s21)
{
	for (int si = 0; si < 3; si++)
	{
		if (si == 2)
		{
			(void)S21BR_ReadUBits(s21, 32);
			(void)S21BR_ReadUBits(s21, 32);
		}
		for (int ci = 0; ci < 255; ci++)
		{
			const uint8_t ch = (uint8_t)S21BR_ReadUBits(s21, 8);
			if (ch == 0)
				break;
		}
	}
}

// S3->S21 snapshot dest writer. LSB-first bits == little-endian store when
// dest is byte-aligned. Header rewrite adds 17 bits, so FULL dest is aligned
// (336) and delta dest is 1 bit in (369 / 401).
static void Snap_WriteBits(uint8_t* dst, int cap, int& wBit, uint64_t val, int nBits)
{
	for (int b = 0; b < nBits; ++b)
	{
		const int byteIdx = wBit >> 3;
		if (dst && byteIdx < cap && ((val >> static_cast<unsigned>(b)) & 1ull))
			dst[byteIdx] |= static_cast<uint8_t>(1u << (wBit & 7));
		++wBit;
	}
}

static void Snap_WriteU32(uint8_t* dst, int cap, int& wBit, uint32_t val)
{
	const int bitOff = wBit & 7;
	const int byteIdx = wBit >> 3;
	if (!dst)
	{
		wBit += 32;
		return;
	}
	if (bitOff == 0 && byteIdx + 4 <= cap)
	{
		memcpy(dst + byteIdx, &val, 4);
		wBit += 32;
		return;
	}
	if (byteIdx + 5 <= cap)
	{
		uint64_t chunk = 0;
		memcpy(&chunk, dst + byteIdx, 5);
		chunk |= static_cast<uint64_t>(val) << bitOff;
		memcpy(dst + byteIdx, &chunk, 5);
		wBit += 32;
		return;
	}
	Snap_WriteBits(dst, cap, wBit, val, 32);
}

static void Snap_CopyPayload(uint8_t* dst, int cap, int& wBit, uint8_t* s21, uint32_t nBits)
{
	uint32_t left = nBits;
	while (left >= 32)
	{
		Snap_WriteU32(dst, cap, wBit, S21BR_ReadUBits(s21, 32));
		left -= 32;
	}
	if (left)
		Snap_WriteBits(dst, cap, wBit, S21BR_ReadUBits(s21, static_cast<int>(left)),
			static_cast<int>(left));
}

// One-shot dump of the engine INetMessage handler table (m_NetMessages).
static void S21Bridge_DumpMsgArray(CNetChan* pChan)
{
	static bool s_done = false;
	if (s_done || !pChan) return;
	s_done = true;

	void** const msgArray = S21_NC_NetMessages(pChan);
	const int    msgCount = S21_NC_NetMessageCount(pChan);
	SDK_Log("[BRIDGE-MSG-DUMP] netchan=%p msgArray=%p msgCount=%d\n",
		(void*)pChan, (void*)msgArray, msgCount);
	if (!msgArray || msgCount <= 0 || msgCount > 256) return;

	for (int i = 0; i < msgCount; ++i)
	{
		void* const msg = msgArray[i];
		if (!msg)
		{
			SDK_Log("[BRIDGE-MSG-DUMP] slot[%d] = NULL\n", i);
			continue;
		}
		int type = -2;
		void* vtbl = nullptr;
		__try
		{
			vtbl = *reinterpret_cast<void**>(msg);
			// vtable[7] (offset +0x38) = GetType -> int (per

			// 7-bit type written by WriteUBitLong(...,7)).
			using GetTypeFn = int(__fastcall*)(void*);
			GetTypeFn fn = reinterpret_cast<GetTypeFn>(reinterpret_cast<void**>(vtbl)[7]);
			type = fn(msg);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { type = -3; }
		SDK_Log("[BRIDGE-MSG-DUMP] slot[%d] msg=%p vtbl=%p type=%d\n",
			i, msg, vtbl, type);
	}
	SDK_Log("[BRIDGE-MSG-DUMP] END (use this to build the authoritative S21 type table)\n");
}

//-----------------------------------------------------------------------------
// Synthetic S21 bf_read over a UM payload. bitsAvail (+0x24) is min(32, len*8).
//-----------------------------------------------------------------------------
static bool S21Bridge_InitUmBitbuf(uint8_t* bf, uint8_t* scratch, size_t scratchCap,
	const uint8_t* payload, uint32_t lenBytes)
{
	if (!bf || !scratch || !payload || lenBytes == 0)
		return false;
	const size_t padded = (static_cast<size_t>(lenBytes) + 3u) & ~static_cast<size_t>(3);
	if (padded + 4 > scratchCap)
		return false;

	memset(scratch, 0, padded + 4);
	memcpy(scratch, payload, lenBytes);
	memset(bf, 0, 64);

	const uint64_t numBits = (uint64_t)lenBytes * 8;
	const uint64_t numBytes = (uint64_t)lenBytes;
	memcpy(bf + 0x10, &numBits, 8);
	memcpy(bf + 0x18, &numBytes, 8);

	uint32_t firstDword = 0;
	memcpy(&firstDword, scratch, lenBytes >= 4 ? 4 : lenBytes);
	memcpy(bf + 0x20, &firstDword, 4);

	const uint32_t bitsAvail = (lenBytes >= 4) ? 32u : (lenBytes * 8u);
	memcpy(bf + 0x24, &bitsAvail, 4);

	const uint64_t dataStart = (uint64_t)(uintptr_t)scratch;
	const uint64_t dataNext = dataStart + (lenBytes >= 4 ? 4 : lenBytes);
	const uint64_t dataEnd = dataStart + lenBytes;
	memcpy(bf + 0x28, &dataNext, 8);
	memcpy(bf + 0x30, &dataEnd, 8);
	memcpy(bf + 0x38, &dataStart, 8);
	return true;
}

// Invoke registered __MsgFunc_SayText with a bitreader on the usermessage payload.
static char S21Bridge_CallNativeSayText(const uint8_t* payload, uint32_t lenBytes)
{
	if (!Chat_AllowsPlayerChat())
		return 0;

	// payload[0] is the sender slot. Dropping here rather than inside the S21
	// handler keeps the mute working for senders this client holds no entity
	// for, which is every player outside its own realm.
	if (payload && lenBytes >= 1 && Chat_IsSlotMuted((int)payload[0]))
		return 0;

	typedef char(__fastcall* PFN_SayText)(void*);
	const PFN_SayText fn = (PFN_SayText)NetObs_Sym(NetObsSym_t::SayText);
	if (!fn || !payload || lenBytes == 0)
		return -1;

	uint8_t bf[64];
	uint8_t scratch[4104];
	if (!S21Bridge_InitUmBitbuf(bf, scratch, sizeof(scratch), payload, lenBytes))
		return -1;

	char r = -3;
	__try { r = fn(bf); }
	__except (EXCEPTION_EXECUTE_HANDLER) { r = -3; }
	return r;
}

// Same direct-handler path as SayText, for PlayerNotifyDidDamage.
static __int64 S21Bridge_CallNativeDidDamage(const uint8_t* payload, uint32_t lenBytes)
{
	if (!s_origPlayerDidDamageParse || !payload || lenBytes == 0)
		return -1;

	uint8_t bf[64];
	uint8_t scratch[4104];
	if (!S21Bridge_InitUmBitbuf(bf, scratch, sizeof(scratch), payload, lenBytes))
		return -1;

	__int64 r = -3;
	__try { r = s_origPlayerDidDamageParse((__int64)bf); }
	__except (EXCEPTION_EXECUTE_HANDLER) { r = -3; }
	return r;
}

// Direct registered UM handler call (RemoteBulletFired / RemoteWeaponReload).
static __int64 S21Bridge_CallNativeUmHandler(void* fn, const uint8_t* payload, uint32_t lenBytes)
{
	if (!fn || !payload || lenBytes == 0)
		return -1;

	uint8_t bf[64];
	uint8_t scratch[4104];
	if (!S21Bridge_InitUmBitbuf(bf, scratch, sizeof(scratch), payload, lenBytes))
		return -1;

	typedef __int64(__fastcall* PFN_Generic)(__int64);
	__int64 r = -3;
	__try { r = reinterpret_cast<PFN_Generic>(fn)((__int64)bf); }
	__except (EXCEPTION_EXECUTE_HANDLER) { r = -3; }
	return r;
}

// C_BaseScriptRemoteFunctions singleton -> dict (+0x20). Local entries table
// (dict+0x30 pElements / dict+0x48 count) is the 328-byte-stride LOCAL CUtlVector -- distinct
static constexpr ptrdiff_t SCRIPTREMOTE_DICT_OFF      = 0x20;
static constexpr ptrdiff_t SCRIPTREMOTE_DICT_STATE    = 0x50;
static constexpr ptrdiff_t SCRIPTREMOTE_DICT_ELEMENTS = 0x30;
static constexpr ptrdiff_t SCRIPTREMOTE_DICT_COUNT    = 0x48;
static constexpr size_t    SCRIPTREMOTE_LOCAL_STRIDE  = 0x148; // 328 bytes
static constexpr ptrdiff_t SCRIPTREMOTE_ENT_SCRIPTHOOK = 0x0;
static constexpr ptrdiff_t SCRIPTREMOTE_ENT_ARGDATA    = 0x14;
static constexpr ptrdiff_t SCRIPTREMOTE_ENT_ARGSIZE    = 0x114;
static constexpr ptrdiff_t SCRIPTREMOTE_ENT_PARAMCOUNT = 0x118;
static constexpr ptrdiff_t SCRIPTREMOTE_ENT_FUNCSTRING = 0x120;

// MSVC C2712: a function with __try cannot hold a local whose type requires unwinding.

// Reads the dict header + scans m_localEntries for a strcmp name match. Entirely SEH-guarded,
// entirely POD locals -- safe to __try in this function.
static bool S21Bridge_SR_ResolveLocalEntry(const char* name, uint32_t nameLen,
	void** pOutScriptHook, uint32_t* pOutParamCount, uint8_t* pOutArgData, uint32_t* pOutArgSize)
{
	const uintptr_t singleton = NetObs_Sym(NetObsSym_t::ScriptRemoteSingleton);
	if (!singleton)
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] module base not resolved -- drop '%.*s'\n", (int)nameLen, name);
		return false;
	}

	const uintptr_t dict = singleton + SCRIPTREMOTE_DICT_OFF;

	uint32_t state = 0, count = 0;
	uint8_t* pElements = nullptr;
	void*    matchScriptHook = nullptr;
	uint32_t matchParamCount = 0;
	uint32_t matchArgSize = 0;
	uint8_t  matchArgData[256] = {};
	bool     sawAnyFuncString = false;
	bool     matched = false;
	bool     unpopulated = false;

	__try
	{
		state     = *reinterpret_cast<uint32_t*>(dict + SCRIPTREMOTE_DICT_STATE);
		count     = *reinterpret_cast<uint32_t*>(dict + SCRIPTREMOTE_DICT_COUNT);
		pElements = *reinterpret_cast<uint8_t**>(dict + SCRIPTREMOTE_DICT_ELEMENTS);

		if (state == 2 && pElements && count > 0 && count <= 8192)
		{
			for (uint32_t slot = 0; slot < count; ++slot)
			{
				uint8_t* entry = pElements + (size_t)slot * SCRIPTREMOTE_LOCAL_STRIDE;
				const char* funcStr = *reinterpret_cast<const char**>(entry + SCRIPTREMOTE_ENT_FUNCSTRING);
				if (!funcStr)
					continue;
				sawAnyFuncString = true;

				// Bounded strlen -- never scan past a reasonable cap on a foreign pointer.
				size_t slen = 0;
				while (slen < 256 && funcStr[slen]) ++slen;
				if (slen != nameLen || slen >= 256)
					continue;
				if (memcmp(funcStr, name, nameLen) != 0)
					continue;

				void* scriptHook = *reinterpret_cast<void**>(entry + SCRIPTREMOTE_ENT_SCRIPTHOOK);
				// Sentinel transcribed from OnReceiveRemoteFunction: unpopulated entries carry
				// scriptHook in {0,1}. Unsigned (x-2) <= (UINTPTR_MAX-4) excludes both.
				if ((uintptr_t)scriptHook - 2 > (uintptr_t)0xFFFFFFFFFFFFFFFCull)
				{
					unpopulated = true;
					break;
				}

				matchScriptHook = scriptHook;
				matchParamCount = *reinterpret_cast<uint32_t*>(entry + SCRIPTREMOTE_ENT_PARAMCOUNT);
				matchArgSize = *reinterpret_cast<uint32_t*>(entry + SCRIPTREMOTE_ENT_ARGSIZE);
				memcpy(matchArgData, entry + SCRIPTREMOTE_ENT_ARGDATA, sizeof(matchArgData));
				matched = true;
				break;
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] fault reading/scanning the dict -- drop '%.*s'\n", (int)nameLen, name);
		return false;
	}

	{
		static bool s_loggedDict = false;
		if (!s_loggedDict)
		{
			s_loggedDict = true;
			Msg(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] dict %s singleton=%p state=%u count=%u\n",
				NetObs_IsDx12Exe() ? "dx12" : "dx11",
				reinterpret_cast<void*>(singleton), state, count);
		}
	}

	if (state != 2)
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] dict not ready (state=%u) -- drop '%.*s'\n", state, (int)nameLen, name);
		return false;
	}
	if (!pElements || count == 0 || count > 8192)
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] local entries table invalid (count=%u) -- drop '%.*s'\n", count, (int)nameLen, name);
		return false;
	}
	if (unpopulated)
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] entry found but unpopulated (scriptHook sentinel) for '%.*s'\n", (int)nameLen, name);
		return false;
	}
	if (!matched)
	{
		if (!sawAnyFuncString)
			Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] dictionary has entries but no funcString data -- "
				"scriptremotefunctions_saveFuncName may be 0\n");
		Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] no local entry for \"%.*s\"\n", (int)nameLen, name);
		return false;
	}

	*pOutScriptHook = matchScriptHook;
	*pOutParamCount = matchParamCount;
	if (pOutArgData)
		memcpy(pOutArgData, matchArgData, sizeof(matchArgData));
	if (pOutArgSize)
		*pOutArgSize = matchArgSize;
	return true;
}

// Resolves a wire EHandle to the entity's HSCRIPT instance. SEH-guarded (foreign entity
// pointer, foreign vtable call); POD locals only.
static HSCRIPT S21Bridge_SR_ResolveEntityScript(uint32_t eh)
{
	if (eh == 0xFFFFFFFFu)
		return nullptr;
	const uintptr_t entPtr = NetObs_ResolveEHandle(eh);
	HSCRIPT hScript = nullptr;
	// Guard: unresolved GetScriptInstance is execute-at-0; VEH fires before __try.
	if (entPtr && v_C_BaseEntity__GetScriptInstance)
	{
		__try { hScript = v_C_BaseEntity__GetScriptInstance(reinterpret_cast<C_BaseEntity*>(entPtr)); }
		__except (EXCEPTION_EXECUTE_HANDLER) { hScript = nullptr; }
	}
	return hScript;
}

// Invokes CSquirrelVM::ExecuteFunction. SEH-guarded; the ScriptVariant_t array is owned by
// the caller (passed as a pointer, not a local object here), so this leaf has no dtor-object
// of its own and may freely use __try.
static ScriptStatus_t S21Bridge_SR_CallExecute(CSquirrelVM* vm, HSCRIPT scriptHook,
	ScriptVariant_t* args, uint32_t argCount, const char* name, uint32_t nameLen)
{
	ScriptStatus_t rv = SCRIPT_ERROR;
	__try
	{
		rv = vm->ExecuteFunction(scriptHook, args, argCount, nullptr, nullptr);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] ExecuteFunction faulted for \"%.*s\"\n", (int)nameLen, name);
		return SCRIPT_ERROR;
	}
	return rv;
}

//-----------------------------------------------------------------------------
// [MB] Fire a client-VM code callback taking a single entity argument, e.g.
// CodeCallback_OnPlayerMantleBoosted(player). Reuses the proven FindFunction ->
//-----------------------------------------------------------------------------
static HSCRIPT MB_ResolveEntityScriptInstance(void* pEnt)
{
	HSCRIPT h = nullptr;
	__try { h = v_C_BaseEntity__GetScriptInstance(reinterpret_cast<C_BaseEntity*>(pEnt)); }
	__except (EXCEPTION_EXECUTE_HANDLER) { h = nullptr; }
	return h;
}

void NetBridge_FireClientPlayerCallback(const char* funcName, void* pPlayerEnt)
{
	if (!funcName || !pPlayerEnt)
		return;

	CSquirrelVM* const vm = g_pClientScript;
	if (!CSquirrelVM_IsAlive_S21(vm) || !CSquirrelVM__FindFunction || !CSquirrelVM__ExecuteFunction)
		return;

	// Resolve GetScriptInstance with the S21 pattern; the SDK header still uses a stale S3 one.
	if (!v_C_BaseEntity__GetScriptInstance)
	{
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 57 48 83 EC ?? 80 B9 ?? ?? ?? ?? ?? 48 8B F9 75 ?? 48 8B 01 FF 90 ?? ?? ?? ??")
			.GetPtr(v_C_BaseEntity__GetScriptInstance);
	}
	if (!v_C_BaseEntity__GetScriptInstance)
		return;

	const HSCRIPT hInstance = MB_ResolveEntityScriptInstance(pPlayerEnt);
	if (!hInstance)
	{
		static bool s_warnedNoInst = false;
		if (!s_warnedNoInst)
		{
			s_warnedNoInst = true;
			Warning(eDLL_T::CLIENT, "[MB] FX skipped: GetScriptInstance returned null\n");
		}
		return;
	}

	HSCRIPT hFunc = vm->FindFunction(funcName, nullptr, nullptr);
	if (!hFunc)
	{
		static bool s_warnedMiss = false;
		if (!s_warnedMiss)
		{
			s_warnedMiss = true;
			Warning(eDLL_T::CLIENT, "[MB] client callback '%s' not found in the client VM -- is "
				"client/cl_bridge_mantle_boost.gnut deployed + listed in scripts.rson? (FX skipped)\n", funcName);
		}
		return;
	}

	ScriptVariant_t sv;
	sv.m_flags = 0;              // non-owning -- do not free our stack HSCRIPT
	sv.m_type = FIELD_HSCRIPT;
	sv.m_hScript = hInstance;
	S21Bridge_SR_CallExecute(vm, hFunc, &sv, 1, funcName, static_cast<uint32_t>(strlen(funcName)));

	// Do NOT free hFunc. verified (native FindFunction): the
	// returned handle is a 24-byte object allocated by the ENGINE's IMemAlloc singleton
}

// Single producer (packet parse) / single consumer (the engine's drain pass).
// The two never touch the same slot: the producer only writes at s_s2cDeferTail
static BridgeS2CDeferred_t   s_s2cDefer[BRIDGE_S2C_DEFER_MAX];
static std::atomic<uint32_t> s_s2cDeferHead{ 0 };   // next entry to consider running
static std::atomic<uint32_t> s_s2cDeferTail{ 0 };   // next slot to fill

// Signed backwards jump of this many snapshot ticks means the client tick
// space restarted (changelevel); every held stamp is unreachable.
static constexpr int32_t kS2CTickRestartBackJump = 64;
static uint32_t s_s2cLastDrainTick = 0;

// Defined with the drain below -- the parse path must know whether anything will
// ever come back for what it queues.
static bool S21Bridge_S2CTickGateArmed(void);

static uint32_t s_s2cAppliedTick = 0;
static bool s_s2cAppliedTickValid = false;
static uint32_t s_s2cLastAcceptedTick = 0;
static bool s_s2cLastAcceptedValid = false;

void S21Bridge_S2CScriptRemote_ResetAppliedTick(void)
{
	s_s2cAppliedTickValid = false;
	s_s2cAppliedTick = 0;
	s_s2cLastAcceptedValid = false;
	s_s2cLastAcceptedTick = 0;
}

void S21Bridge_S2CScriptRemote_OnSnapshotAccepted(uint32_t wireTick)
{
	if (wireTick == 0)
		return;
	s_s2cLastAcceptedTick = wireTick;
	s_s2cLastAcceptedValid = true;
}

bool S21Bridge_S2CScriptRemote_TickNearAccepted(uint32_t tick)
{
	if (!s_s2cLastAcceptedValid || tick == 0)
		return false;
	const int32_t d = static_cast<int32_t>(tick - s_s2cLastAcceptedTick);
	return d >= -4 && d <= 4;
}

void S21Bridge_S2CScriptRemote_OnSnapshotApplied(uint32_t wireTick)
{
	if (!s_s2cAppliedTickValid
		|| static_cast<int32_t>(wireTick - s_s2cAppliedTick) > 0)
	{
		s_s2cAppliedTick = wireTick;
		s_s2cAppliedTickValid = true;
	}
}

static bool S21Bridge_S2CAppliedTick(uint32_t* pTick)
{
	if (!s_s2cAppliedTickValid || !pTick)
		return false;
	*pTick = s_s2cAppliedTick;
	return true;
}

void S21Bridge_FlushS2CScriptRemote(const char* reason)
{
	S21Bridge_S2CScriptRemote_ResetAppliedTick();
	const uint32_t tail = s_s2cDeferTail.load(std::memory_order_acquire);
	const uint32_t head = s_s2cDeferHead.load(std::memory_order_relaxed);
	const uint32_t n = tail - head;
	if (n == 0)
		return;
	s_s2cDeferHead.store(tail, std::memory_order_relaxed);
	Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] flushed %u held call(s) -- %s\n",
		n, reason ? reason : "reset");
}

static inline uint32_t S21Bridge_S2CQueueDepth(void)
{
	return s_s2cDeferTail.load(std::memory_order_acquire) -
		s_s2cDeferHead.load(std::memory_order_relaxed);
}

// [SEC] Per-second S2C ScriptRemote rate budget (native ~600 queue parity).
static bool S21Bridge_S2CRateAllow(const char* name, uint32_t nameLen)
{
	const int maxPerSec = bridge_s2c_scriptremote_rate_max.GetInt();
	if (maxPerSec <= 0)
		return true;

	const ULONGLONG nowMs = GetTickCount64();
	static ULONGLONG s_winStartMs = 0;
	static int s_winCount = 0;
	if (s_winStartMs == 0 || (nowMs - s_winStartMs) >= 1000ull)
	{
		s_winStartMs = nowMs;
		s_winCount = 0;
	}
	if (s_winCount >= maxPerSec)
	{
		static uint32_t s_rateDropLog = 0;
		if (++s_rateDropLog <= 8 || (s_rateDropLog % 200) == 0)
			Warning(eDLL_T::ENGINE, "[SEC][BRIDGE-S2C-SR] rate budget exceeded (%d/s) -- drop '%.*s'\n",
				maxPerSec, (int)nameLen, name);
		return false;
	}
	++s_winCount;
	return true;
}

static bool S21Bridge_NameInCommaList(const char* list, const char* name, uint32_t nameLen)
{
	if (!list || !list[0] || !name || nameLen == 0)
		return false;
	const char* p = list;
	while (*p)
	{
		while (*p == ' ' || *p == '\t' || *p == ',')
			++p;
		if (!*p)
			break;
		const char* start = p;
		while (*p && *p != ',')
			++p;
		size_t tokLen = (size_t)(p - start);
		while (tokLen > 0 && (start[tokLen - 1] == ' ' || start[tokLen - 1] == '\t'))
			--tokLen;
		if (tokLen == nameLen && memcmp(start, name, nameLen) == 0)
			return true;
	}
	return false;
}

static bool S21Bridge_S2CIsUiAllowed(const char* name, uint32_t nameLen)
{
	static const char* const kUi[] = {
		"ControlSpawnMenu_SetLoadoutAndLegendSelectMenuIsEnabled",
		"ControlSpawnMenu_UpdatePlayerLoadout",
		"Control_RemoveAllButtonSpawnIcons",
		"ForceOpenRewardPacks",
		"LoadoutSelectionMenu_CloseLoadoutMenu",
		"LoadoutSelectionMenu_OpenLoadoutMenu",
		"SCBUI_PlayerConnectedOrDisconnected",
		"ServerCallback_GotBPFromPremier",
		"ServerCallback_MilestoneEvent_MilestoneRewardCeremonyIsDue",
		"ServerCallback_OpenSurvivalExitMenu",
		"ServerCallback_UpdatePlayerLastLoggedInTimestamp",
		"ServerToUI_CharacterLockRejected",
		"ServerToUI_EntitlementInitialized",
		"ServerToUI_GRX_QueuedRewardsGiven",
		"ServerToUI_PROTO_YouAreGreenLightedForGRX",
		"ServerToUI_Ranked_NotifyRankedPeriodScoreChanged",
		"SurvivalMenu_AckAction",
		"UI_OpenControlSpawnMenu",
	};
	for (size_t i = 0; i < ARRAYSIZE(kUi); ++i)
	{
		if (strlen(kUi[i]) == nameLen && memcmp(kUi[i], name, nameLen) == 0)
			return true;
	}
	return false;
}

static bool S21Bridge_S2CIsDenied(const char* name, uint32_t nameLen)
{
	static const char* const kDeny[] = {
		"DisablePrecacheErrors",
		"RestorePrecacheErrors",
		"ScriptCallback_UnlockAchievement",
		"ServerCallback_GamemodeSelectorInitialize",
		"ServerCallback_OpenDevMenu",
		"ServerCallback_SetPlaylistById",
	};
	for (size_t i = 0; i < ARRAYSIZE(kDeny); ++i)
	{
		if (strlen(kDeny[i]) == nameLen && memcmp(kDeny[i], name, nameLen) == 0)
			return true;
	}
	return false;
}

static bool S21Bridge_S2CAllowlistOk(const char* name, uint32_t nameLen)
{
	if (S21Bridge_S2CIsDenied(name, nameLen))
	{
		Warning(eDLL_T::ENGINE, "[SEC][BRIDGE-S2C-SR] name '%.*s' denied -- drop\n",
			(int)nameLen, name);
		return false;
	}
	const char* list = bridge_s2c_scriptremote_allow.GetString();
	if (!list || !list[0])
		return true;
	if (S21Bridge_NameInCommaList(list, name, nameLen))
		return true;
	Warning(eDLL_T::ENGINE, "[SEC][BRIDGE-S2C-SR] name '%.*s' not on allowlist -- drop\n",
		(int)nameLen, name);
	return false;
}

static uint8_t S21Bridge_S2CResolveIsUI(const char* name, uint32_t nameLen, uint8_t wireIsUI)
{
	(void)wireIsUI;
	return S21Bridge_S2CIsUiAllowed(name, nameLen) ? 1 : 0;
}

// m_argData is the packed registration blob (type byte + range payload), not
// one type tag per argument. Sizes match GetRemoteFunctionArgumentTypes:
// float/vector +12, int +8, typed_entity +cstring, itemflavor +5/+9.
struct S21Bridge_SRRanges
{
	int      intMin[16];
	int      intMax[16];
	float    floatMin[16];
	float    floatMax[16];
	uint32_t floatBits[16];
};

static bool S21Bridge_SR_ExtractArgTypes(const uint8_t* argData, uint32_t argSize,
	uint32_t paramCount, uint8_t* outTypes, uint32_t outCap, S21Bridge_SRRanges* outRanges)
{
	if (!argData || !outTypes || paramCount > outCap || argSize > 256)
		return false;
	if (paramCount == 0)
		return argSize == 0;

	uint32_t pos = 0;
	for (uint32_t i = 0; i < paramCount; ++i)
	{
		if (pos >= argSize)
			return false;
		const uint8_t t = argData[pos++];
		outTypes[i] = t;

		uint32_t skip = 0;
		switch (t)
		{
		case 1: // float: min/max/bits as 3 dwords
		case 3: // vector: same
			skip = 12;
			if (outRanges)
			{
				outRanges->floatMin[i] = *reinterpret_cast<const float*>(argData + pos);
				outRanges->floatMax[i] = *reinterpret_cast<const float*>(argData + pos + 4);
				outRanges->floatBits[i] = *reinterpret_cast<const uint32_t*>(argData + pos + 8);
			}
			break;
		case 5: // int: min/max
			skip = 8;
			if (outRanges)
			{
				outRanges->intMin[i] = *reinterpret_cast<const int*>(argData + pos);
				outRanges->intMax[i] = *reinterpret_cast<const int*>(argData + pos + 4);
			}
			break;
		case 6:    // bool
		case 0x22: // string
		case 0x28: // entity
			break;
		case 0x29: // typed_entity + classname
		{
			uint32_t slen = 0;
			while (pos + slen < argSize && argData[pos + slen])
				++slen;
			if (pos + slen >= argSize)
				return false;
			skip = slen + 1;
			break;
		}
		case 0x2A: // itemflavor: disc + one or two ints
			if (pos >= argSize)
				return false;
			skip = argData[pos] ? 9u : 5u;
			break;
		default:
			return false;
		}
		if (pos + skip > argSize)
			return false;
		pos += skip;
	}
	return pos == argSize;
}

// A FindFunction handle is a copy of the closure object; it dies with the
// Squirrel VM at level change while the CSquirrelVM wrapper lives on. The
// HSQUIRRELVM allocation is recycled, so an entry also carries the VM
// generation stamped by CSquirrelVM_Init_S21 -- that is what actually
// separates the new VM from the dead one at the same address.
struct S21Bridge_SRFuncCacheEntry
{
	char           szName[80];
	HSCRIPT        hFunc;
	CSquirrelVM*   pVm;
	HSQUIRRELVM    hVM;
	unsigned int   nGeneration;
};
static S21Bridge_SRFuncCacheEntry s_srFnCache[32];
static size_t s_srFnCacheCount = 0;

static void S21Bridge_SRFuncCache_DropDeadVM(CSquirrelVM* vm, HSQUIRRELVM hNow)
{
	size_t kept = 0;
	for (size_t i = 0; i < s_srFnCacheCount; ++i)
	{
		if (s_srFnCache[i].pVm == vm &&
			(s_srFnCache[i].hVM != hNow ||
			 s_srFnCache[i].nGeneration != g_nS21ScriptVMGeneration))
			continue;
		if (kept != i)
			s_srFnCache[kept] = s_srFnCache[i];
		++kept;
	}
	if (kept != s_srFnCacheCount)
		Msg(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] dropped %u cached function handle(s) from a torn-down VM\n",
			static_cast<unsigned>(s_srFnCacheCount - kept));
	s_srFnCacheCount = kept;
}

static void S21Bridge_InjectScriptRemote(const char* name, uint32_t nameLen, uint8_t isUI,
	uint32_t argCount, const BridgeS2CScriptRemoteArg* args)
{
	static bool s_announced = false;
	if (!s_announced)
	{
		s_announced = true;
		Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] injector active (first call) -- name-carried "
			"S->C ScriptRemote via CSquirrelVM::ExecuteFunction (bridge_s2c_scriptremote)\n");
	}

	// [SEC] Fail closed: denylist + optional extra allowlist, then registered+schema.
	// Wire isUI does not pick the VM -- only compiled UI-allowlisted names do.
	if (!S21Bridge_S2CAllowlistOk(name, nameLen))
		return;
	isUI = S21Bridge_S2CResolveIsUI(name, nameLen, isUI);

	// Lazily resolve C_BaseEntity::GetScriptInstance; the SDK header still uses a stale S3 pattern.
	if (!v_C_BaseEntity__GetScriptInstance)
	{
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 57 48 83 EC ?? 80 B9 ?? ?? ?? ?? ?? 48 8B F9 75 ?? 48 8B 01 FF 90 ?? ?? ?? ??")
			.GetPtr(v_C_BaseEntity__GetScriptInstance);
		if (v_C_BaseEntity__GetScriptInstance)
			Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] resolved C_BaseEntity::GetScriptInstance @ %p (S21) -- entity args live\n",
				reinterpret_cast<void*>(v_C_BaseEntity__GetScriptInstance));
	}

	void*    matchScriptHook = nullptr;
	uint32_t matchParamCount = 0;
	uint32_t matchArgSize = 0;
	uint8_t  matchArgData[256] = {};
	if (!S21Bridge_SR_ResolveLocalEntry(name, nameLen, &matchScriptHook, &matchParamCount, matchArgData, &matchArgSize))
		return;

	// [SEC] Schema/argc: wire argc must match registered param count (fail closed).
	if (argCount != matchParamCount)
	{
		Warning(eDLL_T::ENGINE, "[SEC][BRIDGE-S2C-SR] arg count mismatch for '%.*s': wire=%u native=%u -- drop\n",
			(int)nameLen, name, argCount, matchParamCount);
		return;
	}

	uint8_t nativeTypes[16] = {};
	S21Bridge_SRRanges ranges = {};
	if (!S21Bridge_SR_ExtractArgTypes(matchArgData, matchArgSize, matchParamCount, nativeTypes, 16, &ranges))
	{
		Warning(eDLL_T::ENGINE,
			"[SEC][BRIDGE-S2C-SR] native arg schema unreadable for '%.*s' (argSize=%u argc=%u) -- drop\n",
			(int)nameLen, name, matchArgSize, matchParamCount);
		return;
	}

	for (uint32_t i = 0; i < argCount; ++i)
	{
		if (nativeTypes[i] != args[i].typeTag)
		{
			Warning(eDLL_T::ENGINE,
				"[SEC][BRIDGE-S2C-SR] arg type mismatch for '%.*s': arg[%u] wire=0x%02X native=0x%02X -- drop\n",
				(int)nameLen, name, i, args[i].typeTag, nativeTypes[i]);
			return;
		}

		const BridgeS2CScriptRemoteArg& a = args[i];
		// A registered [min,max] is the quantization domain only while the arg is
		// packed into fewer than 32 bits. At 32 bits the sender writes a raw float
		// and never consults the range, so enforcing it here drops traffic the
		// sender considered valid -- e.g. any world position past the 32000 that
		// eleven remotes declare, on maps that reach MAX_MAP_BOUNDS 61000.
		const bool bRawFloat = (ranges.floatBits[i] >= 32);
		bool bRangeOk = true;
		switch (nativeTypes[i])
		{
		case 5:
			bRangeOk = (a.i >= ranges.intMin[i] && a.i <= ranges.intMax[i]);
			break;
		case 1:
			bRangeOk = std::isfinite(a.f)
				&& (bRawFloat || (a.f >= ranges.floatMin[i] && a.f <= ranges.floatMax[i]));
			break;
		case 3:
			bRangeOk = std::isfinite(a.vec[0]) && std::isfinite(a.vec[1]) && std::isfinite(a.vec[2])
				&& (bRawFloat
					|| (a.vec[0] >= ranges.floatMin[i] && a.vec[0] <= ranges.floatMax[i]
					 && a.vec[1] >= ranges.floatMin[i] && a.vec[1] <= ranges.floatMax[i]
					 && a.vec[2] >= ranges.floatMin[i] && a.vec[2] <= ranges.floatMax[i]));
			break;
		default:
			break;
		}
		if (!bRangeOk)
		{
			Warning(eDLL_T::ENGINE, "[SEC][BRIDGE-S2C-SR] arg %u out of range for '%.*s' -- drop\n",
				i, (int)nameLen, name);
			return;
		}
	}

	CSquirrelVM* const vm = isUI ? g_pUIScript : g_pClientScript;
	if (!CSquirrelVM_IsAlive_S21(vm))
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] target VM (isUI=%u) not initialized -- drop '%.*s'\n", isUI, (int)nameLen, name);
		return;
	}

	if (argCount > 16)
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] argCount %u exceeds ScriptVariant_t[16] bound -- drop '%.*s'\n", argCount, (int)nameLen, name);
		return;
	}

	// Resolve the target function by NAME via the SDK's proven FindFunction ->
	// ExecuteFunction pattern (every working call site does this: the C->S path,
	if (!CSquirrelVM__FindFunction || !CSquirrelVM__ExecuteFunction)
	{
		static bool s_warnedPtrs = false;
		if (!s_warnedPtrs)
		{
			s_warnedPtrs = true;
			Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] VM-call pointers UNRESOLVED (FindFunction=%p ExecuteFunction=%p) -- cannot inject\n",
				reinterpret_cast<void*>(CSquirrelVM__FindFunction), reinterpret_cast<void*>(CSquirrelVM__ExecuteFunction));
		}
		return;
	}

	const HSQUIRRELVM hVMNow = CSquirrelVM_GetHVM_S21(vm);
	S21Bridge_SRFuncCache_DropDeadVM(vm, hVMNow);

	HSCRIPT hFunc = nullptr;
	for (size_t i = 0; i < s_srFnCacheCount; ++i)
	{
		if (s_srFnCache[i].pVm == vm && s_srFnCache[i].hVM == hVMNow &&
			s_srFnCache[i].nGeneration == g_nS21ScriptVMGeneration &&
			strcmp(s_srFnCache[i].szName, name) == 0)
		{
			hFunc = s_srFnCache[i].hFunc;
			break;
		}
	}

	if (!hFunc)
	{
		hFunc = vm->FindFunction(name, nullptr, nullptr);
		if (hFunc)
		{
			if (s_srFnCacheCount >= 32)
			{
				memmove(&s_srFnCache[0], &s_srFnCache[1], sizeof(s_srFnCache[0]) * 31);
				s_srFnCacheCount = 31;
			}
			S21Bridge_SRFuncCacheEntry& e = s_srFnCache[s_srFnCacheCount++];
			V_strncpy(e.szName, name, sizeof(e.szName));
			e.hFunc = hFunc;
			e.pVm = vm;
			e.hVM = hVMNow;
			e.nGeneration = g_nS21ScriptVMGeneration;
		}
		else
		{
			for (size_t i = 0; i < s_srFnCacheCount; ++i)
			{
				if (s_srFnCache[i].pVm == vm && strcmp(s_srFnCache[i].szName, name) == 0)
				{
					if (i + 1 < s_srFnCacheCount)
						memmove(&s_srFnCache[i], &s_srFnCache[i + 1], sizeof(s_srFnCache[0]) * (s_srFnCacheCount - i - 1));
					--s_srFnCacheCount;
					break;
				}
			}

			static int s_ffMiss = 0;
			if (++s_ffMiss <= 20)
				Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] FindFunction('%.*s') null on the %s VM -- drop\n",
					(int)nameLen, name, isUI ? "UI" : "client");
			return;
		}
	}

	// Entity/typed_entity args resolve via S21Bridge_SR_ResolveEntityScript (EHandle 16:16,
	// serial-checked) to a valid client HSCRIPT; the cross-binary S3->S21 EHandle round-trips

	// Build ScriptVariant_t args. NOTE (deviation from the frozen spec, verified
	// against this repo's public/vscript/ivscript.h): the wire typeTag values 1/3/5/6/0x22
	ScriptVariant_t svArgs[16];
	char strScratch[16][256];
	int  strScratchUsed = 0;

	for (uint32_t i = 0; i < argCount; ++i)
	{
		const BridgeS2CScriptRemoteArg& a = args[i];
		ScriptVariant_t& sv = svArgs[i];
		sv.m_flags = 0; // non-owning -- do not let ExecuteFunction try to free our stack buffers

		switch (a.typeTag)
		{
		case 1: // float
			sv.m_type = FIELD_FLOAT;
			sv.m_float = a.f;
			break;
		case 3: // vector
			sv.m_type = FIELD_VECTOR;
			sv.m_Vec3D = Vector3D(a.vec[0], a.vec[1], a.vec[2]);
			break;
		case 5: // int
			sv.m_type = FIELD_INTEGER;
			sv.m_int = a.i;
			break;
		case 6: // bool
			sv.m_type = FIELD_BOOLEAN;
			sv.m_bool = (a.i != 0);
			break;
		case 0x22: // string
		{
			sv.m_type = FIELD_CSTRING;
			if (strScratchUsed >= 16)
			{
				Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] too many string args for '%.*s' -- drop\n", (int)nameLen, name);
				return;
			}
			char* scratch = strScratch[strScratchUsed++];
			const uint32_t copyLen = (a.strLen < 255) ? a.strLen : 255;
			memcpy(scratch, a.strBuf, copyLen);
			scratch[copyLen] = '\0';
			sv.m_pszString = scratch;
			break;
		}
		case 0x28: // entity (raw EHandle)
		case 0x29: // typed_entity (raw EHandle; the type-name signifier is schema-local, never on wire)
			sv.m_type = FIELD_HSCRIPT;
			sv.m_hScript = S21Bridge_SR_ResolveEntityScript(a.eh); // null on stale/invalid handle -- target must null-check
			break;
		default:
			Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] unrecognized arg typeTag 0x%02X at index %u for '%.*s' -- drop whole call\n",
				a.typeTag, i, (int)nameLen, name);
			return;
		}
	}

	(void)matchScriptHook; // registration gate only; the actual call uses the FindFunction handle
	const ScriptStatus_t rv = S21Bridge_SR_CallExecute(vm, hFunc, svArgs, argCount, name, nameLen);
	if (bridge_s2c_scriptremote_log.GetBool())
		Msg(eDLL_T::ENGINE, "[S2C-SR-EXEC] '%.*s' argc=%u vm=%s rv=%d\n",
			(int)nameLen, name, argCount, isUI ? "UI" : "CL", (int)rv);
	if (rv == SCRIPT_ERROR)
		Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] ExecuteFunction failed for \"%.*s\" rv=%d\n", (int)nameLen, name, (int)rv);
}

static ConVar bridge_playlist_apply_early("bridge_playlist_apply_early", "1", FCVAR_RELEASE,
	"Select the server's playlist on the client as soon as signon carries it, instead of "
	"waiting for the native SIGNONSTATE_NEW handler (fixes stale load-screen mode/map name).");

// S21 CClientState::SetSignonState case 3 does:
// lea rcx, [msg+0x90]; the playlist-name string field
static void* S21Bridge_ResolvePlaylistSetCurrent(void)
{
	static void* s_pfn = nullptr;
	static bool s_resolved = false;
	if (s_resolved)
		return s_pfn;
	s_resolved = true;

	const uint8_t* pJnz = reinterpret_cast<const uint8_t*>(
		NetObs_Sym(NetObsSym_t::PatchSite_PlaylistValidation));
	if (!pJnz)
		return nullptr;

	const uint8_t* pCall = pJnz - 7; // 5-byte E8 call + 2-byte test al,al
	if (pCall[0] != 0xE8)
	{
		Warning(eDLL_T::ENGINE,
			"[BRIDGE-PM] playlist setter call site mismatch (expected E8, got %02X) -- "
			"load-screen mode/map name will lag one match behind\n", pCall[0]);
		return nullptr;
	}

	const int32_t rel = *reinterpret_cast<const int32_t*>(pCall + 1);
	s_pfn = reinterpret_cast<void*>(const_cast<uint8_t*>(pCall + 5 + rel));
	SDK_Log("[BRIDGE-PM] resolved Playlists_SetCurrentByName @ %p\n", s_pfn);
	return s_pfn;
}

// The 64-byte current-playlist-name buffer the selector writes, reached from the
// `lea rcx, [rip+disp32]` at Playlists_SetCurrentByName+0x24 (the strncpy dest on
static char* S21Bridge_ResolvePlaylistNameBuf(void)
{
	static char* s_pBuf = nullptr;
	static bool s_resolved = false;
	if (s_resolved)
		return s_pBuf;
	s_resolved = true;

	const uint8_t* pFn = reinterpret_cast<const uint8_t*>(S21Bridge_ResolvePlaylistSetCurrent());
	if (!pFn)
		return nullptr;

	const uint8_t* pLea = pFn + 0x24;
	if (pLea[0] != 0x48 || pLea[1] != 0x8D || pLea[2] != 0x0D)
	{
		Warning(eDLL_T::ENGINE,
			"[BRIDGE-PM] playlist name-buffer lea mismatch (%02X %02X %02X) -- "
			"GetCurrentPlaylistName unavailable on the client\n", pLea[0], pLea[1], pLea[2]);
		return nullptr;
	}

	const int32_t disp = *reinterpret_cast<const int32_t*>(pLea + 3);
	s_pBuf = reinterpret_cast<char*>(const_cast<uint8_t*>(pLea + 7 + disp));
	SDK_Log("[BRIDGE-PM] resolved current-playlist name buffer @ %p\n", s_pBuf);
	return s_pBuf;
}

// Client-side stand-in for v_Playlists_GetCurrent, which is permanently null in
// client.dll: VPlaylists is REGISTERed only in init_server.cpp, and its S3
const char* S21Bridge_GetCurrentPlaylistName(void)
{
	const char* pszBuf = S21Bridge_ResolvePlaylistNameBuf();
	return pszBuf ? pszBuf : "";
}

//-----------------------------------------------------------------------------
// Apply the dedi's playlist name when the wire carries it (native selects at SignonState 3).
//-----------------------------------------------------------------------------
static void S21Bridge_ApplyServerPlaylist(const char* pszPlaylist)
{
	if (!pszPlaylist || !pszPlaylist[0])
		return;

	const size_t nLen = V_strlen(pszPlaylist);
	bool bNameOk = (nLen >= 1 && nLen <= 63);
	if (bNameOk)
	{
		for (size_t i = 0; i < nLen; ++i)
		{
			const unsigned char c = static_cast<unsigned char>(pszPlaylist[i]);
			if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
				|| (c >= '0' && c <= '9') || c == '_'))
			{
				bNameOk = false;
				break;
			}
		}
	}
	if (!bNameOk)
	{
		static volatile LONG s_plReject = 0;
		if (InterlockedIncrement(&s_plReject) == 1)
			Warning(eDLL_T::ENGINE, "[BRIDGE-PM] rejected playlist name\n");
		return;
	}

	if (!bridge_playlist_apply_early.GetBool())
		return;

	typedef char (*PFN_PlaylistsSetCurrentByName)(const char* pszName);
	const PFN_PlaylistsSetCurrentByName pfnSet =
		reinterpret_cast<PFN_PlaylistsSetCurrentByName>(S21Bridge_ResolvePlaylistSetCurrent());
	if (!pfnSet)
		return;

	if (!_stricmp(s_szAppliedPlaylist, pszPlaylist))
		return;

	if (s_nPlaylistNamesApplied >= 8)
		return;

	char result = 0;
	__try { result = pfnSet(pszPlaylist); }
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-PM] crash selecting playlist '%s'\n", pszPlaylist);
		return;
	}

	// 0 = the name is not in the client's playlist table (the dedi's file lands via
	// svc_Playlists; before it does, the label falls back to the previous match).
	if (result)
	{
		strncpy(s_szAppliedPlaylist, pszPlaylist, sizeof(s_szAppliedPlaylist) - 1);
		s_szAppliedPlaylist[sizeof(s_szAppliedPlaylist) - 1] = '\0';
		++s_nPlaylistNamesApplied;
	}

	// Sanity-check the playlist name buffer after select; mismatch means the name derivation is wrong for this build.
	const char* pszNow = S21Bridge_GetCurrentPlaylistName();
	if (result && pszNow[0] && strncmp(pszNow, pszPlaylist, 63) != 0)
		Warning(eDLL_T::ENGINE, "[BRIDGE-PM] name buffer reads '%s' after selecting '%s'"
			" -- buffer derivation is wrong for this build\n", pszNow, pszPlaylist);

	SDK_Log("[BRIDGE-PM] ApplyServerPlaylist '%s' result=%d now='%s'\n",
		pszPlaylist, (int)result, pszNow);
}

// ProcessMessages: read S3 IDs from the bitstream, translate, dispatch S21 handlers.
static constexpr int kBridgeS2CStringMax = 256;

static bool Bridge_ReadS2CString(uint8_t* const s21buf, char* const pOut, const int nOutMax)
{
	for (int i = 0; i < nOutMax; i++)
	{
		if (S21BR_GetBitsLeft(s21buf) < 8)
			return false;

		const uint8_t c = (uint8_t)S21BR_ReadUBits(s21buf, 8);
		pOut[i] = (char)c;

		if (c == 0)
			return !S21BR_IsOverflowed(s21buf);
	}

	return false; // never terminated within the cap -- the stream is sheared
}

static bool S21Bridge_SetCVarNameAccepted(const char* const name)
{
	if (!name || !name[0] || !g_pCVar)
		return false;

	static const char* const kBlocked[] = {
		"sdk_", "bridge_", "net_", "cl_", "r_", "mat_", "fps_",
		"host_", "con_", "exec", "bind", "alias"
	};
	for (size_t i = 0; i < sizeof(kBlocked) / sizeof(kBlocked[0]); ++i)
	{
		if (_strnicmp(name, kBlocked[i], V_strlen(kBlocked[i])) == 0)
			return false;
	}

	ConVar* const var = g_pCVar->FindVar(name);
	if (!var)
		return false;
	// Names the retail client does not flag REPLICATED but the dedi must still drive.
	static const char* const kReplicatedByServer[] = {
		"mp_gamemode", "sv_lobbyType", "sv_players",
		"sv_forceChatToTeamOnly", "tether_maxvel"
	};
	for (size_t i = 0; i < sizeof(kReplicatedByServer) / sizeof(kReplicatedByServer[0]); ++i)
	{
		if (_stricmp(name, kReplicatedByServer[i]) == 0)
			return !var->IsFlagSet(FCVAR_CHEAT);
	}
	if (!var->IsFlagSet(FCVAR_REPLICATED))
		return false;
	// REPLICATED is retail's own "server enforces on clients" mark; DEVELOPMENTONLY
	// and HIDDEN only hide console visibility, so they do not veto the push.
	if (var->IsFlagSet(FCVAR_CHEAT))
		return false;
	return true;
}

static void S21Bridge_LogDroppedSetCVar(const char* const name)
{
	static volatile LONG s_drop = 0;
	const LONG n = InterlockedIncrement(&s_drop);
	if (n <= 32)
		Warning(eDLL_T::ENGINE, "[BRIDGE-SETCVAR] dropped '%s'\n", name ? name : "?");
}

// SignonState bodies may end before their last string; running out of bits is
// an empty string, a full window with no NUL is a sheared stream.
static bool Bridge_ReadSignonString(uint8_t* const s21buf, char* const pOut, const int nOutMax)
{
	for (int i = 0; i < nOutMax; i++)
	{
		if (S21BR_GetBitsLeft(s21buf) < 8)
		{
			pOut[i] = '\0';
			return true;
		}

		const uint8_t c = (uint8_t)S21BR_ReadUBits(s21buf, 8);
		pOut[i] = (char)c;

		if (c == 0)
			return true;
	}

	return false;
}

// Held calls released per overflow. A batch keeps a long burst from paying the
// release cost on every single call past the ceiling.
#define BRIDGE_S2C_RELEASE_BATCH 32u

// Inject the oldest held calls in send order, freeing room at the tail.
static void S21Bridge_S2CReleaseOldest(uint32_t count)
{
	const uint32_t tail = s_s2cDeferTail.load(std::memory_order_acquire);
	uint32_t head = s_s2cDeferHead.load(std::memory_order_relaxed);

	for (uint32_t n = 0; n < count && head != tail; ++n)
	{
		BridgeS2CDeferred_t& slot = s_s2cDefer[head % BRIDGE_S2C_DEFER_MAX];

		// Advance before inject: a re-entrant drain must not re-run this entry.
		s_s2cDeferHead.store(++head, std::memory_order_relaxed);

		CSquirrelVM* const targetVm = slot.isUI ? g_pUIScript : g_pClientScript;
		const HSQUIRRELVM hNow = CSquirrelVM_GetHVM_S21(targetVm);
		if (!hNow || hNow != slot.hVM)
			continue;

		S21Bridge_InjectScriptRemote(slot.name, slot.nameLen, slot.isUI, slot.argCount, slot.args);
	}
}

// Consumes exactly its own message body off s21buf; the caller continues after.
static void S21Bridge_HandleS2CScriptRemote(uint8_t* s21buf)
{
	const uint32_t isTyped = S21BR_ReadUBits(s21buf, 1);
	const uint32_t byteLen = S21BR_ReadUBits(s21buf, 16);

	if (isTyped != 0)
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] unexpected isTyped=1 frame on the S2C lane (byteLen=%u) -- skipping\n", byteLen);
		S21BR_SkipBits(s21buf, (int64_t)byteLen * 8);
		return;
	}
	if (byteLen == 0 || byteLen > 1024)
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] frame byteLen %u out of bounds -- skipping\n", byteLen);
		S21BR_SkipBits(s21buf, (int64_t)byteLen * 8);
		return;
	}

	uint8_t frame[1024];
	for (uint32_t i = 0; i < byteLen; ++i)
		frame[i] = (uint8_t)S21BR_ReadUBits(s21buf, 8);

	uint32_t off = 0;
	auto take8 = [&](uint8_t& out) -> bool {
		if (off + 1 > byteLen) return false;
		out = frame[off]; off += 1; return true;
	};
	auto takeBytes = [&](void* dst, uint32_t n) -> bool {
		if (off + n > byteLen) return false;
		memcpy(dst, &frame[off], n); off += n; return true;
	};

	bool ok = true;
	uint32_t magic = 0, snapshotTick = 0;
	uint8_t isUI = 0, replayMode = 0, nameLen = 0;
	ok = ok && takeBytes(&magic, 4);

	if (ok && magic == BRIDGE_S2C_MANTLEBOOST_MAGIC)
	{
		int32_t nValue = 0;
		if (byteLen != 8 || !takeBytes(&nValue, 4))
		{
			Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-MB] verdict frame byteLen %u invalid -- dropping frame\n", byteLen);
			return;
		}
		if (MantleBoostClient_AuthoritativeEnabled())
			MantleBoostClient_OnAuthoritativeState((nValue >> 7) & 0x7F, nValue);
		return;
	}

	ok = ok && takeBytes(&snapshotTick, 4);
	ok = ok && take8(isUI);
	ok = ok && take8(replayMode);
	ok = ok && take8(nameLen);

	if (!ok || magic != BRIDGE_S2C_SCRIPTREMOTE_MAGIC || nameLen == 0 || nameLen > 64)
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] frame magic/header invalid (ok=%d magic=0x%08X nameLen=%u) -- dropping frame\n",
			ok ? 1 : 0, magic, nameLen);
		return;
	}

	char nameBuf[65] = {};
	if (!takeBytes(nameBuf, nameLen))
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] frame truncated reading name -- dropping frame\n");
		return;
	}
	nameBuf[nameLen] = '\0';
	bool nameOk = true;
	for (uint8_t ni = 0; ni < nameLen; ++ni)
	{
		const unsigned char ch = static_cast<unsigned char>(nameBuf[ni]);
		if (ch < 32 || ch > 126)
		{
			nameOk = false;
			break;
		}
	}
	if (!nameOk)
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] frame name not printable -- dropping frame\n");
		return;
	}

	uint8_t argCount = 0;
	if (!take8(argCount) || argCount > 16)
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] invalid argCount (%u) for '%s' -- dropping frame\n", argCount, nameBuf);
		return;
	}

	BridgeS2CScriptRemoteArg parsedArgs[16] = {};
	bool argOk = true;
	for (uint32_t i = 0; i < argCount && argOk; ++i)
	{
		uint8_t typeTag = 0;
		if (!take8(typeTag)) { argOk = false; break; }
		parsedArgs[i].typeTag = typeTag;
		switch (typeTag)
		{
		case 1:    argOk = takeBytes(&parsedArgs[i].f, 4); break;                 // float
		case 3:    argOk = takeBytes(&parsedArgs[i].vec, 12); break;              // vector (x,y,z)
		case 5:    argOk = takeBytes(&parsedArgs[i].i, 4); break;                 // int
		case 6:                                                                    // bool
		{
			uint8_t b = 0;
			argOk = take8(b);
			parsedArgs[i].i = (b != 0) ? 1 : 0;
			break;
		}
		case 0x22:                                                                  // string
		{
			uint8_t slen = 0;
			argOk = take8(slen);
			if (argOk)
			{
				parsedArgs[i].strLen = slen;
				argOk = takeBytes(parsedArgs[i].strBuf, slen);
			}
			break;
		}
		case 0x28: // entity
		case 0x29: // typed_entity
			argOk = takeBytes(&parsedArgs[i].eh, 4);
			break;
		default:
			Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] unknown wire typeTag 0x%02X at arg %u for '%s' -- dropping frame\n",
				typeTag, i, nameBuf);
			argOk = false;
			break;
		}
	}

	if (!argOk)
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] frame arg parse failed for '%s' -- dropping frame\n", nameBuf);
		return;
	}
	if (off != byteLen)
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] frame length mismatch (consumed=%u declared=%u) for '%s' -- dropping frame\n",
			off, byteLen, nameBuf);
		return;
	}

	if (!bridge_s2c_scriptremote.GetBool())
		return;

	if (!S21Bridge_S2CAllowlistOk(nameBuf, nameLen))
		return;
	isUI = S21Bridge_S2CResolveIsUI(nameBuf, nameLen, isUI);

	if (!S21Bridge_S2CRateAllow(nameBuf, nameLen))
		return;

	if (!bridge_s2c_scriptremote_tickgate.GetBool() || !S21Bridge_S2CTickGateArmed())
	{
		S21Bridge_InjectScriptRemote(nameBuf, nameLen, isUI, argCount, parsedArgs);
		return;
	}

	if (S21Bridge_S2CQueueDepth() >= BRIDGE_S2C_DEFER_MAX)
	{
		// Release the oldest held calls early rather than let this one overtake
		// them. A burst that outruns the queue must still arrive in send order:
		// a batch protocol whose header runs after its body reads as corrupt
		// state, not as a late frame.
		static uint32_t s_nOverflow = 0;
		if (++s_nOverflow <= 8)
			Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] hold queue full (%d) -- releasing %u held call(s) early to keep '%s' in order\n",
				BRIDGE_S2C_DEFER_MAX, BRIDGE_S2C_RELEASE_BATCH, nameBuf);
		S21Bridge_S2CReleaseOldest(BRIDGE_S2C_RELEASE_BATCH);
	}

	CSquirrelVM* const targetVm = isUI ? g_pUIScript : g_pClientScript;
	const HSQUIRRELVM hVM = CSquirrelVM_GetHVM_S21(targetVm);
	if (!hVM)
	{
		static uint32_t s_nDeadEnqueue = 0;
		if (++s_nDeadEnqueue <= 8 || (s_nDeadEnqueue % 200) == 0)
			Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] target VM dead at enqueue -- drop '%s'\n", nameBuf);
		return;
	}

	const uint32_t tail = s_s2cDeferTail.load(std::memory_order_relaxed);
	BridgeS2CDeferred_t& slot = s_s2cDefer[tail % BRIDGE_S2C_DEFER_MAX];
	memcpy(slot.name, nameBuf, nameLen);
	slot.name[nameLen] = '\0';
	slot.nameLen = nameLen;
	slot.isUI = isUI;
	slot.argCount = argCount;
	slot.snapshotTick = snapshotTick;
	slot.holdPasses = 0;
	slot.hVM = hVM;
	for (uint32_t i = 0; i < argCount; ++i)
		slot.args[i] = parsedArgs[i];

	// Publish last: the drain pass must never see a half-written slot.
	s_s2cDeferTail.store(tail + 1, std::memory_order_release);
}

//-----------------------------------------------------------------------------
// Run S->C ScriptRemote calls whose snapshot tick has arrived (engine ExecuteCallQueue).
//-----------------------------------------------------------------------------
static void S21Bridge_DrainS2CScriptRemote(uint32_t snapshotTick)
{
	const uint32_t tail = s_s2cDeferTail.load(std::memory_order_acquire);
	uint32_t head = s_s2cDeferHead.load(std::memory_order_relaxed);

	// Tick space restarted (changelevel): held stamps can never be reached.
	if (head != tail &&
		static_cast<int32_t>(s_s2cLastDrainTick - snapshotTick) >= kS2CTickRestartBackJump)
	{
		const uint32_t n = tail - head;
		s_s2cDeferHead.store(tail, std::memory_order_relaxed);
		Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] tick space restart (last=%u now=%u) -- dropped %u held call(s)\n",
			s_s2cLastDrainTick, snapshotTick, n);
		s_s2cLastDrainTick = snapshotTick;
		return;
	}
	s_s2cLastDrainTick = snapshotTick;

	if (head == tail)
		return;

	const uint32_t maxHold = static_cast<uint32_t>(bridge_s2c_scriptremote_max_hold.GetInt());

	while (head != tail)
	{
		BridgeS2CDeferred_t& slot = s_s2cDefer[head % BRIDGE_S2C_DEFER_MAX];

		// Signed compare: ticks wrap, and a stamp from before a level change is
		// "already due", not 2^31 ticks in the future.
		const bool bDue = static_cast<int32_t>(snapshotTick - slot.snapshotTick) >= 0;

		if (!bDue)
		{
			if (++slot.holdPasses < maxHold)
				break;

			Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] '%s' held %u drains without reaching stamp "
				"(stamp=%u snapshotTick=%u) -- releasing anyway\n",
				slot.name, slot.holdPasses, slot.snapshotTick, snapshotTick);
		}

		// Advance before inject/drop: re-entrant drain must not re-run this entry.
		s_s2cDeferHead.store(++head, std::memory_order_relaxed);

		CSquirrelVM* const targetVm = slot.isUI ? g_pUIScript : g_pClientScript;
		const HSQUIRRELVM hNow = CSquirrelVM_GetHVM_S21(targetVm);
		if (!hNow || hNow != slot.hVM)
		{
			static uint32_t s_nVmDrop = 0;
			if (++s_nVmDrop <= 8 || (s_nVmDrop % 200) == 0)
				Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] dropping '%s' -- target VM was torn down (queued=%p now=%p)\n",
					slot.name, reinterpret_cast<void*>(slot.hVM), reinterpret_cast<void*>(hNow));
			continue;
		}

		S21Bridge_InjectScriptRemote(slot.name, slot.nameLen, slot.isUI, slot.argCount, slot.args);
	}
}
static void S21Bridge_LerpDepth_OnCurrentTick(uint32_t curTick);

static void __fastcall Hook_SR_ExecuteCallQueue(int64_t thisptr, int snapshotTick)
{
	v_SR_ExecuteCallQueue(thisptr, snapshotTick);

	const uint32_t engineTick = static_cast<uint32_t>(snapshotTick);
	uint32_t releaseTick = engineTick;

	if (bridge_s2c_scriptremote_release.GetInt() == 2)
	{
		uint32_t appliedTick = 0;
		if (S21Bridge_S2CAppliedTick(&appliedTick)
			&& static_cast<int32_t>(appliedTick - engineTick) > 0)
			releaseTick = appliedTick;
	}

	S21Bridge_DrainS2CScriptRemote(releaseTick);
	S21Bridge_LerpDepth_OnCurrentTick(engineTick);
}

void VScriptRemoteS2CGate::GetFun(void) const
{
	// C_BaseScriptRemoteFunctions::ExecuteCallQueue. Landmark is the inlined EHandle pair check.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC ?? 8B 05 ?? ?? ?? ?? 8B DA 83 F8 ?? 0F 84 ?? ?? ?? ?? "
		"44 0F B7 C0 4C 8D 0D ?? ?? ?? ?? 0F B7 05 ?? ?? ?? ?? 49 C1 E0 ?? "
		"43 39 44 08 ?? 0F 85 ?? ?? ?? ?? 4B 83 3C 08 ?? 0F 84")
		.GetPtr(v_SR_ExecuteCallQueue);

	if (!v_SR_ExecuteCallQueue)
		Warning(eDLL_T::ENGINE, "[BRIDGE-S2C-SR] ExecuteCallQueue pattern UNRESOLVED -- S->C calls "
			"would be held forever; the tick gate is disabled and they run at parse time\n");
}

void VScriptRemoteS2CGate::Detour(const bool bAttach) const
{
	if (v_SR_ExecuteCallQueue)
		DetourSetup(&v_SR_ExecuteCallQueue, &Hook_SR_ExecuteCallQueue, bAttach);
}

// True only when the gate can actually release what it holds. Without the hook
// there is no drain, so holding would be a silent stall -- the parse path falls
// back to running calls inline instead.
static bool S21Bridge_S2CTickGateArmed(void)
{
	return v_SR_ExecuteCallQueue != nullptr;
}

// Single-writer gate for every file-scope translation buffer ProcessMessages touches.
static std::recursive_mutex s_pmMutex;

static bool S21Bridge_ProcessMessages_Locked(CNetChan* pChan, bf_read* s3buf);
static volatile LONG s_pendingValidatorDisconnect = 0;

void S21Bridge_RequestValidatorDisconnect(const char* tag)
{
	Warning(eDLL_T::ENGINE,
		"[BRIDGE-ST] rejected SendTables (%s) -- disconnect%s\n",
		tag ? tag : "?",
		PakLobby_InPakWait() ? " (deferred, in pak wait)" : "");
	if (PakLobby_InPakWait())
	{
		InterlockedExchange(&s_pendingValidatorDisconnect, 1);
		return;
	}
	Cbuf_AddText(ECommandTarget_t::CBUF_FIRST_PLAYER, "disconnect",
		cmd_source_t::kCommandSrcCode);
}

void S21Bridge_FlushValidatorDisconnect(void)
{
	if (InterlockedCompareExchange(&s_pendingValidatorDisconnect, 0, 1) == 1)
	{
		Cbuf_AddText(ECommandTarget_t::CBUF_FIRST_PLAYER, "disconnect",
			cmd_source_t::kCommandSrcCode);
	}
}

static void S21Bridge_LogSkipTransfer(int s3cmd, int64_t bitsLeft,
	int lastCmd, int64_t lastStartBit, int64_t lastEndBit, long long lastOrdinal,
	const char* why)
{
	Warning(eDLL_T::ENGINE,
		"[BRIDGE-PM] SKIP-TRANSFER: S3 msg %d (%s) %s -- discarding %lld bits "
		"(rest of transfer lost) signon=%d prev=#%lld S3 %d (%s) bits %lld..%lld\n",
		s3cmd, S21Bridge_S3TypeName(s3cmd), why ? why : "no handler",
		(long long)bitsLeft, s_lastSentSignonState,
		lastOrdinal, lastCmd,
		lastCmd >= 0 ? S21Bridge_S3TypeName(lastCmd) : "<none>",
		(long long)lastStartBit, (long long)lastEndBit);
}

static bool S21Bridge_SignonOpen(void)
{
	return s_lastSentSignonState < 8;
}

bool S21Bridge_ProcessMessages(CNetChan* pChan, bf_read* s3buf)
{
	std::lock_guard<std::recursive_mutex> pmLock(s_pmMutex);
	return S21Bridge_ProcessMessages_Locked(pChan, s3buf);
}

// Last message starts in the payload being parsed, printed when a read fails.
struct PmTrail_t { int s3cmd; int64_t startBit; };
static PmTrail_t     s_pmTrail[12];
static int           s_pmTrailN = 0;
static const uint8_t* s_pmTrailBase = nullptr;
static int            s_pmTrailBytes = 0;

static void PmTrail_Dump(void)
{
	char line[512]; int len = 0;
	const int first = s_pmTrailN > 12 ? s_pmTrailN - 12 : 0;
	for (int i = first; i < s_pmTrailN && len < 400; ++i)
	{
		const PmTrail_t& t = s_pmTrail[i % 12];
		len += snprintf(line + len, sizeof(line) - len, " %d@%lld", t.s3cmd, (long long)t.startBit);
	}
	char hex[80]; int hl = 0;
	for (int i = 0; i < 24 && i < s_pmTrailBytes && hl < 72; ++i)
		hl += snprintf(hex + hl, sizeof(hex) - hl, "%02X", s_pmTrailBase[i]);
	Warning(eDLL_T::ENGINE, "[BRIDGE-PM] payload trail (s3cmd@bit):%s | bytes %s\n", line, hex);
}

static bool S21Bridge_ProcessMessages_Locked(CNetChan* pChan, bf_read* s3buf)
{
	void** msgArray = S21_NC_NetMessages(pChan);
	const int msgCount = S21_NC_NetMessageCount(pChan);

	if (!msgArray || msgCount <= 0)
	{
		Warning(eDLL_T::ENGINE, "S21Bridge_ProcessMessages: no message handlers registered\n");
		return true;
	}

	// Resolve S21 bf_read functions on first call
	S21_ResolveBfReadFunctions();
	if (!s_S21BfReadInit)
	{
		Warning(eDLL_T::ENGINE, "S21Bridge_ProcessMessages: failed to resolve S21 bf_read init\n");
		return true;
	}

	// Extract data from S3 bf_read
	const uint8_t* baseData = (const uint8_t*)s3buf->GetBasePointer();
	const int s3TotalBytes = (int)s3buf->TotalBytesAvailable();
	const int startBit = (int)s3buf->GetNumBitsRead();

	if (!baseData || s3TotalBytes < 4)
		return true;

	// Construct S21 bf_read (64 bytes) using the engine's own initializer
	alignas(16) uint8_t s21buf[S21BR_SIZE];
	s_S21BfReadInit(s21buf, baseData, (uint64_t)s3TotalBytes);
	s_pmTrailN = 0; s_pmTrailBase = baseData; s_pmTrailBytes = s3TotalBytes;

	// Skip to current S3 bit position (for partially-consumed buffers)
	// Use SkipBits (ReadUBits loop) instead of Seek to avoid alignment issues
	if (startBit > 0)
		S21BR_SkipBits(s21buf, startBit);

	// Per-call transfer tracing.
	// Logs every iteration's (cmd, bitsRead, bitsLeft) so we can localize
	// where a misaligned format-bridge consumes the wrong bit count.
	const LONG pmCallRaw = InterlockedIncrement(&s_pmCallCount);
	const long long pmCallNum = (long long)pmCallRaw;
	if (pmCallRaw <= 20)
		SDK_Log("[BRIDGE-PM] === TRANSFER START call=%lld bytes=%d startBit=%d ===\n",
			pmCallNum, s3TotalBytes, startBit);

	// Periodic comprehensive histogram dump. Cadence: every 50 calls.
	// At post-spawn rate of ~30-100 PM calls/sec, this prints every ~0.5-2s.
	if (pmCallRaw - s_pmLastHistDumpCall >= 25)
	{
		s_pmLastHistDumpCall = pmCallRaw;
		S21Bridge_PpPmHistogramDump();
	}

	// Previous message in THIS transfer -- reported when a corrupt type is read so the
	// desync can be attributed to the message that consumed the wrong bit count.
	int      prevCmd         = -1;
	int64_t  prevMsgStartBit = -1;
	long long msgOrdinal     = 0;

	while (S21BR_GetBitsLeft(s21buf) >= NETMSG_TYPE_BITS)
	{
		static long long s_loopIter = 0;
		s_loopIter++;
		int64_t bitsReadNow = S21BR_GetBitsRead(s21buf);
		int64_t bitsLeftNow = S21BR_GetBitsLeft(s21buf);

		int cmd = (int)S21BR_ReadUBits(s21buf, NETMSG_TYPE_BITS);
		if (S21BR_IsOverflowed(s21buf))
		{
			Warning(eDLL_T::ENGINE, "S21Bridge_ProcessMessages: bitstream overflow\n");
			s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
			return false;
		}

		// Per-S3-type lifetime fire counter (read by histogram dump).
		InterlockedIncrement(&s_pmIterTotal);
		if (cmd >= 0 && cmd < 128)
			InterlockedIncrement(&s_pmS3TypeFires[cmd]);

		// Log every non-NOP message type at the top of the loop, capped at the
		// first 50 iterations to keep the giant signon transfer's log volume
		// in check.
		if (cmd != 0 && s_loopIter <= 50)
			SDK_Log("[BRIDGE-PM] LOOP iter=%lld cmd=%d bitsRead=%lld bitsLeft=%lld\n",
				s_loopIter, cmd, bitsReadNow, bitsLeftNow);

		if (cmd == 0) continue;  // net_NOP
		if (cmd == 1)            // net_Disconnect: reason string, then ConnectionClosing
		{
			// A sheared cursor landing on type 1 with no body still queues
			// disconnect. A real net_Disconnect always carries at least a NUL.
			if (S21BR_GetBitsLeft(s21buf) < 8)
			{
				Warning(eDLL_T::ENGINE,
					"[BRIDGE-PM] truncated S2C net_Disconnect (bitsLeft=%lld)\n",
					(long long)S21BR_GetBitsLeft(s21buf));
				s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
				return false;
			}

			char szReason[1024];
			size_t n = 0;
			while (n < sizeof(szReason) - 1 && S21BR_GetBitsLeft(s21buf) >= 8)
			{
				const char ch = static_cast<char>(S21BR_ReadUBits(s21buf, 8));
				if (ch == '\0')
					break;
				szReason[n++] = ch;
			}
			szReason[n] = '\0';
			Warning(eDLL_T::ENGINE, "[BRIDGE-PM] server closed the connection: \"%s\"\n", szReason);
			Cbuf_AddText(ECommandTarget_t::CBUF_FIRST_PLAYER, "disconnect", cmd_source_t::kCommandSrcCode);
			s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
			return true;
		}

		// Stage previous-message identity before roll-forward so a desync on the
		// current type can name the message that sheared the stream.
		const int       lastCmd      = prevCmd;
		const int64_t   lastStartBit = prevMsgStartBit;
		const int64_t   lastEndBit   = bitsReadNow;
		const long long lastOrdinal  = msgOrdinal;

		prevCmd         = cmd;
		prevMsgStartBit = bitsReadNow;
		++msgOrdinal;

		// Translate S3 ID -> S21 ID
		const int s3cmd = cmd;
		const int s21cmd = S21Bridge_TranslateNetMsgType(cmd, true);

		if (s21cmd < 0) // -1 = removed in S21, -2 = format changed / S21 type TBD
		{
			// Known S3-only or format-changed messages: skip their body.
			// Without this, ALL messages after an unknown type are dropped.

			// S3 net_ScriptMessage (type 68) maps to s21cmd=-1; handle the S2C ScriptRemote frame here.
			if (s3cmd == 68)
			{
				S21Bridge_HandleS2CScriptRemote(s21buf);
				continue;
			}

			if (s3cmd == 69)
			{
				static bool s_sawOverlayMsg = false;
				if (!s_sawOverlayMsg)
				{
					s_sawOverlayMsg = true;
					Msg(eDLL_T::CLIENT, "[DBGDRAW] S2C overlay message reached the client dispatch\n");
				}

				const uint32_t byteLen = S21BR_ReadUBits(s21buf, 16);

				// Bound and buffer both come from the writer's own batch size, so
				// raising the batch can never silently exceed the reader again.
				if (byteLen == 0 || byteLen > kDebugOverlayS2CMaxBytes)
				{
					Warning(eDLL_T::CLIENT, "[DBGDRAW] S2C overlay byteLen %u out of bounds (max %d) -- abandoning frame\n",
						byteLen, kDebugOverlayS2CMaxBytes);
					break;
				}

				uint8_t payload[kDebugOverlayS2CMaxBytes];
				for (uint32_t i = 0; i < byteLen; ++i)
					payload[i] = (uint8_t)S21BR_ReadUBits(s21buf, 8);

				// Reads return zero once the reader has overflowed, so a truncated
				// datagram would apply a zero-filled tail as overlays at the origin.
				if (S21BR_IsOverflowed(s21buf))
				{
					Warning(eDLL_T::CLIENT, "[DBGDRAW] S2C overlay payload truncated -- dropping\n");
					break;
				}

				DebugOverlay_ApplyS2CPayload(payload, static_cast<int>(byteLen));
				continue;
			}

			if (s3cmd == kChatBuilderS2CType) // svc_ChatBuilder -- decoded client-side
			{
				if (S21BR_GetBitsLeft(s21buf) < 16)
					break;

				const uint32_t byteLen = S21BR_ReadUBits(s21buf, 16);

				if (byteLen < kChatBuilderStreamHeaderBytes || byteLen > kChatBuilderMaxWireBytes ||
					S21BR_GetBitsLeft(s21buf) < (int64_t)byteLen * 8)
				{
					Warning(eDLL_T::CLIENT, "[CHATBUILDER] S2C byteLen %u out of bounds (max %d) -- abandoning frame\n",
						byteLen, kChatBuilderMaxWireBytes);
					break;
				}

				uint8_t payload[kChatBuilderMaxWireBytes];
				for (uint32_t i = 0; i < byteLen; ++i)
					payload[i] = (uint8_t)S21BR_ReadUBits(s21buf, 8);

				if (S21BR_IsOverflowed(s21buf))
				{
					Warning(eDLL_T::CLIENT, "[CHATBUILDER] S2C payload truncated -- dropping\n");
					break;
				}

				ChatBuilder_ApplyS2CPayload(payload, static_cast<int>(byteLen));
				continue;
			}

			if (s3cmd == 66 || s3cmd == 67) // svc_SetClassVar / svc_SystemSayText
			{
				// Both are two strings; SystemSayText adds a trailing admin bit.
				// Without a body read here the generic drop below discards every
				// message left in the transfer, not just this one.
				char szFirst[kBridgeS2CStringMax];
				char szSecond[kBridgeS2CStringMax];

				if (!Bridge_ReadS2CString(s21buf, szFirst, sizeof(szFirst)) ||
					!Bridge_ReadS2CString(s21buf, szSecond, sizeof(szSecond)))
				{
					Warning(eDLL_T::CLIENT, "[BRIDGE-PM] S3 msg %d string body unreadable -- abandoning frame\n", s3cmd);
					break;
				}

				if (s3cmd == 67)
				{
					const bool bAdminMsg = S21BR_ReadUBits(s21buf, 1) != 0;

					if (S21BR_IsOverflowed(s21buf))
						break;

					if (g_ppHudChat && *g_ppHudChat)
						(*g_ppHudChat)->PrintSystemMsg(szFirst, szSecond, bAdminMsg);
				}

				continue;
			}

			if (s3cmd == 22) // svc_ServerTick -- FORMAT BRIDGE
			{
				// S3 wire: 32b tick + 32b tick2 + 16b compute + 16b stdDev + 1b bool + 8b byte + 32b field = 137 bits
				// S21 wire (slot 24): 32b tick + 32b tick2 + 16b compute + 16b stdDev + 16b word + 8b byte + 32b field = 152 bits
				// Difference: S3 has 1-bit bool, S21 has 16-bit word at same position
				const uint32_t s3Tick1     = S21BR_ReadUBits(s21buf, 32);
				const uint32_t s3Tick2     = S21BR_ReadUBits(s21buf, 32);
				const uint32_t s3Compute   = S21BR_ReadUBits(s21buf, 16);
				const uint32_t s3StdDev    = S21BR_ReadUBits(s21buf, 16);
				const uint32_t s3Bool      = S21BR_ReadUBits(s21buf, 1);
				const uint32_t s3Byte      = S21BR_ReadUBits(s21buf, 8);
				const uint32_t s3Last      = S21BR_ReadUBits(s21buf, 32);

				// Build S21 ServerTick format (152 bits)
				static uint8_t s21TickBuf[128];
				memset(s21TickBuf, 0, sizeof(s21TickBuf));
				int wBit = 0;
				auto writeBitsTK = [&](uint64_t val, int nBits) {
					for (int b = 0; b < nBits; b++) {
						if ((val >> b) & 1)
							s21TickBuf[wBit / 8] |= (1 << (wBit % 8));
						wBit++;
					}
				};
				writeBitsTK(s3Tick1, 32);
				writeBitsTK(s3Tick2, 32);
				writeBitsTK(s3Compute, 16);
				writeBitsTK(s3StdDev, 16);
				writeBitsTK(s3Bool, 16);  // S21: 16-bit word (S3 was 1-bit bool)
				writeBitsTK(s3Byte, 8);
				writeBitsTK(s3Last, 32);

				int totalBytes = (wBit + 7) / 8;
				alignas(16) uint8_t tkS21Buf[S21BR_SIZE];
				s_S21BfReadInit(tkS21Buf, s21TickBuf, (uint64_t)totalBytes);

				const int s21TickType = 24; // CORRECT: slot 24 is ServerTick (NOT 25!)
				if (s21TickType < msgCount)
				{
					void* tkMsg = msgArray[s21TickType];
					if (tkMsg)
					{
						void** tkVtbl = *reinterpret_cast<void***>(tkMsg);
						__try { reinterpret_cast<void(*)(void*)>(tkVtbl[8])(tkMsg); }
						__except(EXCEPTION_EXECUTE_HANDLER) {}
						bool tkReadOk = false;
						__try { tkReadOk = reinterpret_cast<bool(*)(void*, void*)>(tkVtbl[4])(tkMsg, tkS21Buf); }
						__except(EXCEPTION_EXECUTE_HANDLER) {}
						if (tkReadOk) {
							__try { reinterpret_cast<bool(*)(void*)>(tkVtbl[3])(tkMsg); }
							__except(EXCEPTION_EXECUTE_HANDLER) {}
						}
						// Counter-driven log: first 5 + every 500th. Lifetime count
						// lives in s_pmS3TypeFires[22] (see histogram dump).
						static volatile LONG s_tkLog = 0;
						const LONG tkN = InterlockedIncrement(&s_tkLog);
						if (tkN <= 5 || (tkN % 500) == 0)
							SDK_Log("[BRIDGE-PM] ServerTick bridge #%ld: read=%d tick=%u\n",
								tkN, tkReadOk?1:0, s3Tick1);
					}
				}
				continue;
			}

			if (s3cmd == 17) // svc_Sounds -- FORMAT BRIDGE
			{
				// Dedi sound_bridge broadcasts bridge-native svc_Sounds (EmitSoundOnEntity
				// send is CSOMET-dead on dedicated). Envelope
				const uint32_t sndTick = S21BR_ReadUBits(s21buf, 32);
				const uint32_t sndPayloadBits = S21BR_ReadUBits(s21buf, 8);

				static uint8_t s21SoundBuf[64];
				memset(s21SoundBuf, 0, sizeof(s21SoundBuf));
				int sndWBit = 0;
				auto writeBitsSnd = [&](uint64_t val, int nBits) {
					for (int b = 0; b < nBits; b++) {
						if ((val >> b) & 1)
							s21SoundBuf[sndWBit / 8] |= (1 << (sndWBit % 8));
						sndWBit++;
					}
				};

				writeBitsSnd(sndTick, 32);
				writeBitsSnd(sndPayloadBits, 8);

				const uint32_t sndPayloadBitsClamped =
					(sndPayloadBits <= (sizeof(s21SoundBuf) - 5) * 8) ? sndPayloadBits : 0;
				for (uint32_t b = 0; b < sndPayloadBitsClamped; b++)
					writeBitsSnd(S21BR_ReadUBits(s21buf, 1), 1);

				int sndTotalBytes = (sndWBit + 7) / 8;
				alignas(16) uint8_t soundS21Buf[S21BR_SIZE];
				s_S21BfReadInit(soundS21Buf, s21SoundBuf, (uint64_t)sndTotalBytes);

				const int s21SoundType = 20; // svc_Sounds
				if (s21SoundType < msgCount)
				{
					void* sndMsg = msgArray[s21SoundType];
					if (sndMsg)
					{
						void** sndVtbl = *reinterpret_cast<void***>(sndMsg);
						__try { reinterpret_cast<void(*)(void*)>(sndVtbl[8])(sndMsg); }
						__except (EXCEPTION_EXECUTE_HANDLER) {}

						bool sndReadOk = false;
						__try { sndReadOk = reinterpret_cast<bool(*)(void*, void*)>(sndVtbl[4])(sndMsg, soundS21Buf); }
						__except (EXCEPTION_EXECUTE_HANDLER) {}

						if (sndReadOk)
						{
							__try { reinterpret_cast<bool(*)(void*)>(sndVtbl[3])(sndMsg); }
							__except (EXCEPTION_EXECUTE_HANDLER) {}
						}
					}
				}
				continue;
			}

			if (s3cmd == 32) // svc_PlaylistChange -- FORMAT BRIDGE
			{
				// S3 wire: null-term string (playlist name, max 64)
				// S21 wire: 7-bit playlist index -- no name string. Apply by name instead.
				char plcName[65] = {};
				for (int ci = 0; ci < 64; ci++)
				{
					uint8_t ch = (uint8_t)S21BR_ReadUBits(s21buf, 8);
					plcName[ci] = (char)ch;
					if (ch == 0) break;
				}
				// Load-screen chrome (name/map_name) reads GetCurrentPlaylistVarString on
				// the CLIENT. Without this, client stays on lobby default (survival_dev
				// Free Roam / Developer) while dedi runs movement_recorder.
				S21Bridge_ApplyServerPlaylist(plcName);
				continue;
			}

			if (s3cmd == 34) // svc_PlaylistOverrides -- REMOVED in S21, decoded by the bridge
			{
				// S3 wire: 32b(lenBits) + payload(lenBits bits). The payload is
				// Playlist_WriteOverridesToBuffer's output:
				const int32_t lenBits = (int32_t)S21BR_ReadUBits(s21buf, 32);
				if (lenBits <= 0 || lenBits >= 0x100000)
				{
					Warning(eDLL_T::CLIENT,
						"[BRIDGE-PLO] bad payload length %d -- aborting remaining messages "
						"in this packet (no re-align target)\n", lenBits);
					break;
				}

				const int64_t payloadStartBit = S21BR_GetBitsRead(s21buf);
				const uint32_t count = S21BR_ReadUBits(s21buf, 8);

				long accepted = 0;
				bool truncated = false;

				if (count > (uint32_t)S21BR_PLO_MAX_ENTRIES)
				{
					Warning(eDLL_T::CLIENT, "[BRIDGE-PLO] server sent %u overrides (max %d) -- ignoring\n",
						count, S21BR_PLO_MAX_ENTRIES);
					truncated = true;
				}

				// Publish an empty table first: a reader racing this decode must never see
				// a half-rewritten entry paired with the old count.
				s_nPlaylistOverrides = 0;

				for (uint32_t i = 0; i < count && !truncated; i++)
				{
					const uint32_t nameLen = S21BR_ReadUBits(s21buf, 8);
					if (nameLen >= (uint32_t)S21BR_PLO_NAME_SIZE)
					{
						Warning(eDLL_T::CLIENT, "[BRIDGE-PLO] entry %u name length %u out of range -- "
							"discarding the rest\n", i, nameLen);
						truncated = true;
						break;
					}

					char szName[S21BR_PLO_NAME_SIZE];
					for (uint32_t c = 0; c < nameLen; c++)
						szName[c] = (char)S21BR_ReadUBits(s21buf, 8);
					szName[nameLen] = '\0';

					const uint32_t valueLen = S21BR_ReadUBits(s21buf, 8);
					if (valueLen >= (uint32_t)S21BR_PLO_VALUE_SIZE)
					{
						Warning(eDLL_T::CLIENT, "[BRIDGE-PLO] entry %u value length %u out of range -- "
							"discarding the rest\n", i, valueLen);
						truncated = true;
						break;
					}

					char szValue[S21BR_PLO_VALUE_SIZE];
					for (uint32_t c = 0; c < valueLen; c++)
						szValue[c] = (char)S21BR_ReadUBits(s21buf, 8);
					szValue[valueLen] = '\0';

					// An overflowed reader returns zeroes forever; anything decoded past
					// that point is invented, not received.
					if (S21BR_IsOverflowed(s21buf))
					{
						Warning(eDLL_T::CLIENT, "[BRIDGE-PLO] bitstream overflowed at entry %u -- "
							"discarding the rest\n", i);
						truncated = true;
						break;
					}

					if (!szName[0])
						continue;

					bool printable = true;
					for (uint32_t c = 0; c < nameLen && printable; c++)
					{
						const unsigned char ch = static_cast<unsigned char>(szName[c]);
						if (ch < 32 || ch > 126)
							printable = false;
					}
					for (uint32_t c = 0; c < valueLen && printable; c++)
					{
						const unsigned char ch = static_cast<unsigned char>(szValue[c]);
						if (ch < 32 || ch > 126)
							printable = false;
					}
					if (!printable)
					{
						Warning(eDLL_T::CLIENT,
							"[BRIDGE-PLO] entry %u dropped -- non-printable name/value\n", i);
						continue;
					}
					if (!S21Bridge_NameInCommaList(bridge_playlist_override_allow.GetString(),
						szName, nameLen))
					{
						static int s_ploDenyLog = 0;
						if (++s_ploDenyLog <= 8)
							Warning(eDLL_T::CLIENT,
								"[BRIDGE-PLO] override '%s' not on allowlist -- drop\n", szName);
						continue;
					}

					memcpy(s_playlistOverrides[accepted].m_szName, szName, nameLen + 1);
					memcpy(s_playlistOverrides[accepted].m_szValue, szValue, valueLen + 1);
					accepted++;
				}

				s_nPlaylistOverrides = accepted;

				// The length prefix is authoritative -- honour it even on a clean parse, so
				// a payload longer than the entries we read cannot shear the stream.
				S21BR_Seek(s21buf, payloadStartBit + lenBits);

				static long s_ploLastCount = -1;
				if (accepted != s_ploLastCount || truncated)
				{
					s_ploLastCount = accepted;
					SDK_Log("[BRIDGE-PLO] applied %ld override(s) from %d payload bits%s\n",
						accepted, lenBits, truncated ? " (TRUNCATED)" : "");
					for (long li = 0; li < accepted; li++)
						SDK_Log("[BRIDGE-PLO]   %s = %s\n",
							s_playlistOverrides[li].m_szName, s_playlistOverrides[li].m_szValue);
				}
				continue;
			}

			if (s3cmd == 10) // svc_SetPause -- REMOVED in S21
			{
				// S3 wire: 1 bit (m_bPaused)
				// No S21 equivalent. Read S3 bit correctly.
				const uint32_t paused = S21BR_ReadUBits(s21buf, 1);
				static int s_spLog = 0;
				if (++s_spLog <= 3)
					SDK_Log("[BRIDGE-PM] SetPause consumed: paused=%u (removed in S21)\n", paused);
				continue;
			}

			if (s3cmd == 16) // svc_Print -- FORMAT BRIDGE
			{
				// S3 wire format: null-terminated string (WriteString)
				// S21 wire format: 4-bit prefix + null-terminated string (ReadBits(4) + char loop)
				// Bridge: read S3 string, build S21 buffer with 4-bit prefix, dispatch to S21:20

				// Read S3 null-terminated string from bitstream
				char printText[2048];
				int pi = 0;
				for (; pi < 2047; pi++)
				{
					uint8_t ch = (uint8_t)S21BR_ReadUBits(s21buf, 8);
					printText[pi] = (char)ch;
					if (ch == 0) break;
				}
				printText[pi] = 0;

				// Build S21 format: 4-bit prefix (0) + null-terminated string
				static uint8_t s21PrintBuf[4096];
				memset(s21PrintBuf, 0, sizeof(s21PrintBuf));
				int wBit = 0;
				auto writeBitsP = [&](uint64_t val, int nBits) {
					for (int b = 0; b < nBits; b++)
					{
						if ((val >> b) & 1)
							s21PrintBuf[wBit / 8] |= (1 << (wBit % 8));
						wBit++;
					}
				};

				writeBitsP(0, 4); // S21 prefix (4 bits, value 0)
				for (int ci = 0; ci <= pi; ci++) // include null terminator
					writeBitsP((uint8_t)printText[ci], 8);

				int totalBytes = (wBit + 7) / 8;

				// Build S21 bf_read for the translated buffer
				alignas(16) uint8_t printS21Buf[S21BR_SIZE];
				s_S21BfReadInit(printS21Buf, s21PrintBuf, (uint64_t)totalBytes);

				// S21 type 19 = svc_Print (-verified: slot[19] RFB reads 4-bit prefix + string)
				const int s21PrintType = 19;
				if (s21PrintType < msgCount)
				{
					void* printMsg = msgArray[s21PrintType];
					if (printMsg)
					{
						void** printVtbl = *reinterpret_cast<void***>(printMsg);
						__try { reinterpret_cast<void(*)(void*)>(printVtbl[8])(printMsg); }
						__except(EXCEPTION_EXECUTE_HANDLER) {}

						bool printReadOk = false;
						__try { printReadOk = reinterpret_cast<bool(*)(void*, void*)>(printVtbl[4])(printMsg, printS21Buf); }
						__except(EXCEPTION_EXECUTE_HANDLER) {}

						if (printReadOk)
						{
							__try { reinterpret_cast<bool(*)(void*)>(printVtbl[3])(printMsg); }
							__except(EXCEPTION_EXECUTE_HANDLER) {}
						}

						static int s_printLog = 0;
						if (++s_printLog <= 3)
							SDK_Log("[BRIDGE-PM] Print bridge: read=%d len=%d '%.80s'\n",
								printReadOk ? 1 : 0, pi, printText);
					}
				}
				continue;
			}

			if (s3cmd == 35) // svc_AntiCheat -- never native ProcessAntiCheat
			{
				const int64_t left = S21BR_GetBitsLeft(s21buf);
				const int64_t skip = (left < 8192) ? left : 8192;
				if (skip > 0)
					S21BR_SkipBits(s21buf, skip);
				Bridge_RecordBodySkip(35, true);
				static int s_skipLog35 = 0;
				if (++s_skipLog35 <= 3)
					SDK_Log("[BRIDGE-PM] body-skip svc_AntiCheat (%lld bits)\n",
						(long long)skip);
				continue;
			}

			if (s3cmd == 36) // svc_AntiCheatChallenge (removed in S21)
			{
				// S3 wire format: WriteLong(m_nLength) + WriteBits(data, m_nLength)
				const int32_t lenBits = (int32_t)S21BR_ReadUBits(s21buf, 32);
				if (lenBits > 0 && lenBits < 0x100000)
					S21BR_SkipBits(s21buf, lenBits);
				Bridge_RecordBodySkip(36, true);
				static int s_skipLog36 = 0;
				if (++s_skipLog36 <= 3)
					SDK_Log("[BRIDGE-PM] body-skip svc_AntiCheatChallenge (%d bits)\n", lenBits);
				continue;
			}

			if (s3cmd == 63) // clc_AntiCheat -- never native ProcessAntiCheat
			{
				const int64_t left = S21BR_GetBitsLeft(s21buf);
				const int64_t skip = (left < 8192) ? left : 8192;
				if (skip > 0)
					S21BR_SkipBits(s21buf, skip);
				Bridge_RecordBodySkip(63, true);
				static int s_skipLog63 = 0;
				if (++s_skipLog63 <= 3)
					SDK_Log("[BRIDGE-PM] body-skip clc_AntiCheat (%lld bits)\n",
						(long long)skip);
				continue;
			}

			if (s3cmd == 28) // svc_DLCNotifyOwnership -- BODY-SKIP (do not apply)
			{
				// S3 WriteToBuffer: WriteBits(m_bitfield, 64) + N * WriteBits(u16, 16).
				// N = dedi Host_GetEntitlementBalances->m_Size -- NOT on the wire.
				// Wrong N shears the rest of the transfer; fail closed to SKIP-TRANSFER.
				const int64_t bitsAtStart = S21BR_GetBitsLeft(s21buf);
				if (bitsAtStart < 64)
				{
					Bridge_RecordBodySkip(28, false);
					Warning(eDLL_T::CLIENT,
						"[BRIDGE-PM] DLCNotify body-skip fail: only %lld bits left (need 64b bitfield)\n",
						(long long)bitsAtStart);
					// fall through to SKIP-TRANSFER
				}
				else
				{
					int nBal = bridge_s3_dlc_balance_count.GetInt();
					if (nBal < 0)
					{
						// Auto: pure-DLC transfer (or 1-bit pad). Else keep last good / 151.
						const int64_t afterBf = bitsAtStart - 64;
						const int rem = (int)(afterBf % 16);
						if (rem == 0 || rem == 1)
							nBal = (int)(afterBf / 16);
						else
							nBal = 151;
					}
					if (nBal < 0 || nBal > 512)
						nBal = 151;

					const int64_t bodyBits = 64 + (int64_t)nBal * 16;
					if (bitsAtStart < bodyBits)
					{
						Bridge_RecordBodySkip(28, false);
						Warning(eDLL_T::CLIENT,
							"[BRIDGE-PM] DLCNotify body-skip fail: bitsLeft=%lld need=%lld (N=%d) -- "
							"set bridge_s3_dlc_balance_count to dedi balance count\n",
							(long long)bitsAtStart, (long long)bodyBits, nBal);
						// fall through to SKIP-TRANSFER
					}
					else
					{
						S21BR_SkipBits(s21buf, bodyBits);
						Bridge_RecordBodySkip(28, true);
						static int s_skipLog28 = 0;
						if (++s_skipLog28 <= 5 || (s_skipLog28 % 200) == 0)
							SDK_Log("[BRIDGE-PM] body-skip svc_DLCNotifyOwnership N=%d bodyBits=%lld leftAfter=%lld\n",
								nBal, (long long)bodyBits, (long long)S21BR_GetBitsLeft(s21buf));
						continue;
					}
				}
			}

			if (s3cmd == 42) // svc_Menu -- BODY-SKIP (dedi rarely emits; never apply KV)
			{
				// S3 Write: u16 dialogType + u16 lenBytes + lenBytes raw (capped 4096).
				if (S21BR_GetBitsLeft(s21buf) < 32)
				{
					Bridge_RecordBodySkip(42, false);
					// fall through
				}
				else
				{
					const uint32_t menuType = S21BR_ReadUBits(s21buf, 16);
					const uint32_t lenBytes = S21BR_ReadUBits(s21buf, 16);
					if (lenBytes > 4096 || S21BR_GetBitsLeft(s21buf) < (int64_t)lenBytes * 8)
					{
						Bridge_RecordBodySkip(42, false);
						Warning(eDLL_T::CLIENT,
							"[BRIDGE-PM] Menu body-skip fail: type=%u lenBytes=%u bitsLeft=%lld\n",
							menuType, lenBytes, (long long)S21BR_GetBitsLeft(s21buf));
						// fall through -- stream may already be bad
					}
					else
					{
						if (lenBytes > 0)
							S21BR_SkipBits(s21buf, (int64_t)lenBytes * 8);
						Bridge_RecordBodySkip(42, true);
						static int s_skipLog42 = 0;
						if (++s_skipLog42 <= 3)
							SDK_Log("[BRIDGE-PM] body-skip svc_Menu type=%u lenBytes=%u\n",
								menuType, lenBytes);
						continue;
					}
				}
			}

			if (s3cmd == 43) // svc_CmdKeyValues -- BODY-SKIP (removed in S21)
			{
				// S3 Write: u32 lenBytes + lenBytes raw KeyValues blob.
				if (S21BR_GetBitsLeft(s21buf) < 32)
				{
					Bridge_RecordBodySkip(43, false);
				}
				else
				{
					const uint32_t lenBytes = S21BR_ReadUBits(s21buf, 32);
					if (lenBytes > 0x100000 || S21BR_GetBitsLeft(s21buf) < (int64_t)lenBytes * 8)
					{
						Bridge_RecordBodySkip(43, false);
						Warning(eDLL_T::CLIENT,
							"[BRIDGE-PM] CmdKeyValues body-skip fail: lenBytes=%u bitsLeft=%lld\n",
							lenBytes, (long long)S21BR_GetBitsLeft(s21buf));
					}
					else
					{
						if (lenBytes > 0)
							S21BR_SkipBits(s21buf, (int64_t)lenBytes * 8);
						Bridge_RecordBodySkip(43, true);
						static int s_skipLog43 = 0;
						if (++s_skipLog43 <= 3)
							SDK_Log("[BRIDGE-PM] body-skip svc_CmdKeyValues lenBytes=%u\n", lenBytes);
						continue;
					}
				}
			}

			if (s3cmd == 44) // svc_DatatableChecksum -- FORMAT BRIDGE
			{
				// Bridge mutates SendTables so the dedi checksum can never match the
				// S21 client; consume the S3 body and skip dispatch (hostile parse path).
				const uint32_t dtLo = S21BR_ReadUBits(s21buf, 32);
				const uint32_t dtHi = S21BR_ReadUBits(s21buf, 32);

				// NUL-terminated string, S3 caps it at 512 bytes. Bound the walk on
				// bitsLeft as well -- a desynced or hostile stream may never present a NUL.
				char dtName[512];
				uint32_t dtLen = 0;
				bool dtTerminated = false;
				while (dtLen < sizeof(dtName) - 1 && S21BR_GetBitsLeft(s21buf) >= 8)
				{
					const uint8_t c = (uint8_t)S21BR_ReadUBits(s21buf, 8);
					if (c == 0) { dtTerminated = true; break; }
					dtName[dtLen++] = (char)c;
				}
				dtName[dtLen] = '\0';

				static int s_dtcLog = 0;
				if (++s_dtcLog <= 5 || !dtTerminated)
					SDK_Log("[BRIDGE-PM] DatatableChecksum: 0x%08X%08X name='%s' len=%u term=%d\n",
						dtHi, dtLo, dtName, dtLen, dtTerminated ? 1 : 0);

				if (!dtTerminated)
					Warning(eDLL_T::CLIENT,
						"[BRIDGE-PM] DatatableChecksum: unterminated string (len=%u) -- stream may already be desynced\n",
						dtLen);

				continue;
			}

			// No handler: type outside 0..127 is bitstream desync; types 0..127 with
			// no body-skip/format-bridge are unmapped. Either way break -- no safe
			{
				const int64_t bitsLeft = S21BR_GetBitsLeft(s21buf);
				const long long totalBits = (long long)s3TotalBytes * 8;

				if (s3cmd < 0 || s3cmd > S3_NETMSG_TYPE_MAX)
				{
					static int s_desyncLog = 0;
					const bool bLoud = S21Bridge_SignonOpen() || s_desyncLog < 5;
					if (bLoud)
					{
						++s_desyncLog;
						if (lastCmd < 0)
						{
							Warning(eDLL_T::CLIENT,
								"[BRIDGE-DESYNC] impossible S3 type %d at bit %lld/%lld (max %d) -- bitstream "
								"desync, aborting transfer (%lld bits dropped) signon=%d. Previous msg: <none, first in transfer>\n",
								s3cmd, (long long)bitsReadNow, totalBits, S3_NETMSG_TYPE_MAX,
								(long long)bitsLeft, s_lastSentSignonState);
						}
						else
						{
							Warning(eDLL_T::CLIENT,
								"[BRIDGE-DESYNC] impossible S3 type %d at bit %lld/%lld (max %d) -- bitstream "
								"desync, aborting transfer (%lld bits dropped) signon=%d. Previous msg #%lld: S3 %d (%s) "
								"bits %lld..%lld (%lld wide).\n",
								s3cmd, (long long)bitsReadNow, totalBits, S3_NETMSG_TYPE_MAX,
								(long long)bitsLeft, s_lastSentSignonState,
								lastOrdinal, lastCmd, S21Bridge_S3TypeName(lastCmd),
								(long long)lastStartBit, (long long)lastEndBit,
								(long long)(lastEndBit - lastStartBit));
						}

						if (bridge_desync_dump.GetBool() && baseData && s3TotalBytes > 0)
						{
							const int64_t dumpAnchorBit =
								(lastCmd >= 0 && lastStartBit >= 0) ? lastStartBit : bitsReadNow;
							int dumpStart = (int)(dumpAnchorBit / 8) - 32;
							if (dumpStart < 0)
								dumpStart = 0;
							int dumpEnd = dumpStart + 64;
							if (dumpEnd > s3TotalBytes)
								dumpEnd = s3TotalBytes;
							for (int off = dumpStart; off < dumpEnd; off += 16)
							{
								char line[128];
								int n = V_snprintf(line, sizeof(line), "[BRIDGE-DESYNC] bytes @%d:", off);
								const int rowEnd = (off + 16 < dumpEnd) ? (off + 16) : dumpEnd;
								for (int b = off; b < rowEnd && n > 0 && n < (int)sizeof(line) - 4; ++b)
									n += V_snprintf(line + n, sizeof(line) - n, " %02X", baseData[b]);
								Warning(eDLL_T::CLIENT, "%s\n", line);
							}
						}
					}

					Bridge_RecordSkipTransfer(s3cmd, bitsLeft);
					const int64_t endPos = S21BR_GetBitsRead(s21buf) + bitsLeft;
					S21BR_Seek(s21buf, endPos);
					s3buf->Seek((int)endPos);
					break;
				}

				// Branch B -- valid 7-bit type, no body-skip/handler: first hit is Warning
				// so a newly-unmapped type (incl. historical s3cmd72) surfaces immediately.
				static int s_suppHist[128] = {};
				if (s3cmd >= 0 && s3cmd < 128)
				{
					const int n = s_suppHist[s3cmd]++;
					if (S21Bridge_SignonOpen() || n < 3)
					{
						S21Bridge_LogSkipTransfer(s3cmd, bitsLeft,
							lastCmd, lastStartBit, lastEndBit, lastOrdinal,
							"has no handler");
					}
				}
				else
				{
					S21Bridge_LogSkipTransfer(s3cmd, bitsLeft,
						lastCmd, lastStartBit, lastEndBit, lastOrdinal,
						"has no handler");
				}
				Bridge_RecordSkipTransfer(s3cmd, bitsLeft);
				s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
				break;
			}
		}

		// S21 direct array index lookup
		if (s21cmd < 0 || s21cmd >= msgCount)
		{
			static int s_rangeHist[128] = {};
			if (s21cmd >= 0 && s21cmd < 128 && s_rangeHist[s21cmd]++ < 3)
				SDK_Log("[BRIDGE-PM] S21 msg %d out of range (max=%d)\n", s21cmd, msgCount);
			Bridge_RecordSkipTransfer(s3cmd, S21BR_GetBitsLeft(s21buf));
			s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
			return true;
		}

		void* netMsg = msgArray[s21cmd];
		if (!netMsg)
		{
			static int s_nullHist[128] = {};
			if (s21cmd >= 0 && s21cmd < 128 && s_nullHist[s21cmd]++ < 3)
				SDK_Log("[BRIDGE-PM] S21 msg %d has no handler (NULL slot)\n", s21cmd);
			Bridge_RecordSkipTransfer(s3cmd, S21BR_GetBitsLeft(s21buf));
			s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
			return true;
		}

		// Log translated messages
		{
			static long long s_transLog = 0;
			if (++s_transLog <= 30 || (s_transLog % 500) == 0)
				SDK_Log("[BRIDGE-PM] #%lld: S3:%d -> S21:%d\n", s_transLog, s3cmd, s21cmd);
		}

		void** msgVtbl = *reinterpret_cast<void***>(netMsg);

		// Snapshot bf_read state BEFORE handler for diagnostics
		const int64_t preReadBits = S21BR_GetBitsRead(s21buf);
		s_pmTrail[s_pmTrailN % 12] = { s3cmd, preReadBits };
		++s_pmTrailN;

		// Per-message format bridges for S3->S21 wire format differences.
		// Messages with identical wire formats use the generic S21 vtable
		if (s3cmd == 41) // svc_TempEntities -- FORMAT BRIDGE (S21 type 48)
		{
			// S3 wire (after 7-bit type): [32 m_tick][10 m_nNumEntries][22 m_nLength][m_nLength-bit blob]
			// S21 wire (msg 48): [32 m_tick][10 m_nNumEntries][24 m_nLength][m_nLength-bit blob]
			const uint32_t teTick = S21BR_ReadUBits(s21buf, 32);
			const uint32_t teNum  = S21BR_ReadUBits(s21buf, 10);
			const uint32_t teLen  = S21BR_ReadUBits(s21buf, 22);

			const int64_t teBitsLeft = S21BR_GetBitsLeft(s21buf);
			if ((int64_t)teLen > teBitsLeft)
			{
				// Garbage length (stream desync) -- cannot safely continue.
				static int s_teBadLen = 0;
				if (++s_teBadLen <= 10)
					Warning(eDLL_T::ENGINE,
						"[BRIDGE-PM] TempEntities(41->48) bad length=%u > bitsLeft=%lld -- aborting packet\n",
						teLen, (long long)teBitsLeft);
				s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
				return true;
			}

			static uint8_t s_teBuf[65536];
			// teByteLen includes the 66 header bits and the 2 pad bytes -- both in the cap.
			const uint32_t teCapBits = (uint32_t)((sizeof(s_teBuf) - 2) * 8) - 66;
			if (teLen > teCapBits)
			{
				// Too large to re-encode here -- skip this TE message cleanly
				// (keep the stream aligned) and keep processing the packet.
				static int s_teTooBig = 0;
				if (++s_teTooBig <= 10)
					Warning(eDLL_T::ENGINE,
						"[BRIDGE-PM] TempEntities(41->48) length=%u exceeds cap=%u -- skipping (effects lost)\n",
						teLen, teCapBits);
				S21BR_SkipBits(s21buf, (int64_t)teLen);
				continue;
			}

			const uint32_t teByteLen = (66u + teLen + 7u) / 8u + 2u;
			memset(s_teBuf, 0, teByteLen);
			int wBit = 0;
			auto teWrite = [&](uint64_t val, int nBits) {
				for (int b = 0; b < nBits; b++) {
					if ((val >> b) & 1ull) s_teBuf[wBit >> 3] |= (uint8_t)(1u << (wBit & 7));
					++wBit;
				}
			};
			teWrite(teTick, 32);
			teWrite(teNum, 10);
			teWrite(teLen, 24);  // S21 length field is 24 bits (value unchanged)
			// Copy the opaque blob bit-for-bit. Reading from s21buf also advances
			// it past the blob, so the loop stays aligned to the next message.
			uint32_t teRem = teLen;
			while (teRem > 0) {
				const int chunk = teRem >= 32 ? 32 : (int)teRem;
				const uint32_t v = S21BR_ReadUBits(s21buf, chunk);
				teWrite(v, chunk);
				teRem -= (uint32_t)chunk;
			}

			const int teTotalBytes = (wBit + 7) / 8;
			alignas(16) uint8_t teS21Buf[S21BR_SIZE];
			s_S21BfReadInit(teS21Buf, s_teBuf, (uint64_t)teTotalBytes);

			__try { reinterpret_cast<void(*)(void*)>(msgVtbl[8])(netMsg); }
			__except(EXCEPTION_EXECUTE_HANDLER) {}
			bool teReadOk = false;
			__try { teReadOk = reinterpret_cast<bool(*)(void*, void*)>(msgVtbl[4])(netMsg, teS21Buf); }
			__except(EXCEPTION_EXECUTE_HANDLER) {}
			if (teReadOk)
				__try { reinterpret_cast<bool(*)(void*)>(msgVtbl[3])(netMsg); }
				__except(EXCEPTION_EXECUTE_HANDLER) {}

			static volatile LONG s_teLog = 0;
			const LONG teN = InterlockedIncrement(&s_teLog);
			if (teN <= 10 || (teN % 500) == 0)
				SDK_Log("[BRIDGE-PM] TempEntities bridge #%ld: tick=%u num=%u len=%u read=%d\n",
					teN, teTick, teNum, teLen, teReadOk ? 1 : 0);
			continue;
		}

		if (s3cmd == 23) // svc_PersistenceDefFile
		{
			// S3 wire format (after 7-bit type)
			// S21 expects

			// Parse S3 fields from the bitstream
			const uint64_t pdefVersion = ((uint64_t)S21BR_ReadUBits(s21buf, 32))
				| ((uint64_t)S21BR_ReadUBits(s21buf, 32) << 32);
			const uint32_t pdefDataLen = S21BR_ReadUBits(s21buf, 16);

			SDK_Log("[BRIDGE-PM] PersistenceDefFile: version=%llu (0x%llX), dataLen=%u\n",
				(unsigned long long)pdefVersion, (unsigned long long)pdefVersion, pdefDataLen);

			if (pdefDataLen == 0 || pdefDataLen > 0x28000)
			{
				Warning(eDLL_T::ENGINE, "S21Bridge: pdef data length %u out of range\n", pdefDataLen);
				S21BR_SkipBits(s21buf, (int64_t)pdefDataLen * 8);
				continue;
			}

			// Read the BZ2 data bytes from the S3 bitstream into temp buffer
			uint8_t* pdefBZ2 = (uint8_t*)malloc(pdefDataLen + 16);
			if (!pdefBZ2)
			{
				S21BR_SkipBits(s21buf, (int64_t)pdefDataLen * 8);
				continue;
			}
			for (uint32_t i = 0; i < pdefDataLen; i++)
				pdefBZ2[i] = (uint8_t)S21BR_ReadUBits(s21buf, 8);

			// Load compressed pdef into client globals, decompress, parse, set loaded flag.
			// Avoids bf_read path issues and version mismatch problems.
			uintptr_t exeBase = S21_GetExeBase();
			uint8_t* pGlobalPdef = reinterpret_cast<uint8_t*>(NetObs_PdefCompressedBufferAddr());
			memcpy(pGlobalPdef, pdefBZ2, pdefDataLen);
			free(pdefBZ2);

			// Store compressed size
			uint64_t* pPdefCompSize = reinterpret_cast<uint64_t*>(NetObs_PdefCompressedSizeAddr());
			*pPdefCompSize = (uint64_t)pdefDataLen;

			// BZ2 decompress: BZ2_bzBuffToBuffDecompress(dest, &destLen, src, srcLen, small, verbosity)
			static uint8_t s_pdefDecomp[163840]; // 160KB buffer
			uint32_t decompLen = 163840;
			bool pdefOk = false;
			__try
			{
				// Call BZ2_bzBuffToBuffDecompress directly
				typedef int (*BZ2_fn)(void*, uint32_t*, void*, uint32_t, int, int);
				BZ2_fn pBZ2 = (BZ2_fn)NetObs_Bz2DecompressAddr();
				int bz2Ret = pBZ2(s_pdefDecomp, &decompLen, pGlobalPdef, pdefDataLen, 1, 0);
				if (bz2Ret < 0)
				{
					Warning(eDLL_T::ENGINE, "S21Bridge: BZ2 pdef decompress error %d (src=%u dst=%u)\n",
						bz2Ret, pdefDataLen, decompLen);
				}
				else
				{
					// Parse pdef: (data, dataLen, ?, &globals, version)
					typedef uint8_t (__fastcall *PdefParse_fn)(void* data, unsigned int len, int unk, void* globals, uint64_t version);
					PdefParse_fn pParse = (PdefParse_fn)NetObs_PdefParseAddr();
					void* pGlobals = reinterpret_cast<void*>(NetObs_PdefGlobalsAddr());

					uint8_t parseOk = pParse(s_pdefDecomp, decompLen, 0, pGlobals, pdefVersion);
					uint8_t* pLoaded = reinterpret_cast<uint8_t*>(NetObs_PdefLoadedFlagAddr());
					if (!parseOk)
					{
						Warning(eDLL_T::ENGINE, "S21Bridge: pdef parse failed -- leaving unloaded\n");
						if (pLoaded)
							*pLoaded = 0;
						s_bridgePdefReady = false;
					}
					else
					{
					if (pLoaded)
					{
						*pLoaded = 1;
						*(pLoaded - 1) = 0;
					}

					uint64_t* pVerCache = reinterpret_cast<uint64_t*>(NetObs_PdefVersionCacheAddr());
					*pVerCache = pdefVersion;
					*reinterpret_cast<uint64_t*>((uintptr_t)netMsg + 32) = pdefVersion;
					s_bridgePdefReady = true;

					SDK_Log("[BRIDGE-PM] PDEF FLAGS: exeBase=%p loaded_addr=%p loaded_val=%u verCache_addr=%p verCache_val=%llu msg+32=%llu\n",
						(void*)exeBase, (void*)pLoaded, (unsigned)(pLoaded ? *pLoaded : 0),
						(void*)pVerCache, (unsigned long long)*pVerCache,
						(unsigned long long)*reinterpret_cast<uint64_t*>((uintptr_t)netMsg + 32));

					pdefOk = true;
					}
					SDK_Log("[BRIDGE-PM] PersistenceDefFile: BZ2 ok, decompressed %u -> %u, parse=%d\n",
						pdefDataLen, decompLen, (int)parseOk);

					// Diagnostic: read persistence baseline var count and descriptor table entries.
					uint64_t* pPbCount = reinterpret_cast<uint64_t*>(NetObs_PdefPbCountAddr());
					uint64_t* pPbArray = reinterpret_cast<uint64_t*>(NetObs_PdefPbArrayAddr());
					const uint64_t pbCount = *pPbCount;
					SDK_Log("[BRIDGE-PM] PDEF DIAG: count=%llu\n", (unsigned long long)pbCount);
					// Log first 4 entries (4*4 qwords = 16 qwords)
					for (int ei = 0; ei < 4 && (uint64_t)ei < pbCount; ++ei)
					{
						uint64_t* e = pPbArray + (4 * ei);
						SDK_Log("[BRIDGE-PM] PDEF entry[%d]: ptr=0x%llX off=0x%llX extra=0x%llX,0x%llX",
							ei, (unsigned long long)e[0], (unsigned long long)e[1],
							(unsigned long long)e[2], (unsigned long long)e[3]);
						if (e[0]) {
							uint32_t* desc = reinterpret_cast<uint32_t*>(e[0]);
							SDK_Log("    ptr->[+0]=0x%X [+2]=type=%u [+10]=len=%u\n",
								desc[0], desc[2], desc[10]);
						} else {
							SDK_Log("    (null ptr)\n");
						}
					}
					// Log a few entries toward the end to confirm whole array populated
					if (pbCount > 10)
					{
						uint64_t tailIdx = pbCount - 1;
						uint64_t* e = pPbArray + (4 * tailIdx);
						SDK_Log("[BRIDGE-PM] PDEF entry[%llu](last): ptr=0x%llX off=0x%llX\n",
							(unsigned long long)tailIdx, (unsigned long long)e[0], (unsigned long long)e[1]);
					}
				}
			}
			__except(EXCEPTION_EXECUTE_HANDLER)
			{
				SDK_Log("[BRIDGE-PM] PersistenceDefFile: EXCEPTION in BZ2/parse path\n");
			}

			SDK_Log("[BRIDGE-PM] PersistenceDefFile: processed=%d, bits consumed=%lld\n",
				pdefOk ? 1 : 0, (long long)(S21BR_GetBitsRead(s21buf) - preReadBits));

			// PersistenceDefFile handled -- continue to next message
			continue;
		}
		else if (s3cmd == 24) // svc_UseCachedPersistenceDefFile
		{
			// S3 wire: [32b version low] [32b version high]
			const uint64_t cachedVer = ((uint64_t)S21BR_ReadUBits(s21buf, 32))
				| ((uint64_t)S21BR_ReadUBits(s21buf, 32) << 32);

			uint8_t*  pLoaded   = reinterpret_cast<uint8_t*>(NetObs_PdefLoadedFlagAddr());
			uint64_t* pVerCache = reinterpret_cast<uint64_t*>(NetObs_PdefVersionCacheAddr());

			if (s_bridgePdefReady && pLoaded && *pLoaded)
			{
				if (pVerCache)
					*pVerCache = cachedVer;
				SDK_Log("[BRIDGE-PM] UseCachedPersistenceDefFile: keeping bridge-parsed pdef, version=%llu\n",
					(unsigned long long)cachedVer);
			}
			else
			{
				// Native Process sets the loaded flag whether or not a definition
				// was ever parsed, so every later persistence read would decode
				// against a schema this client never received. Stay unloaded.
				if (pLoaded)
					*pLoaded = 0;
				s_bridgePdefReady = false;
				Warning(eDLL_T::ENGINE,
					"S21Bridge: dedi sent UseCachedPersistenceDefFile (version %llu) but no pdef "
					"was parsed this session -- persistence left unloaded (dedi UseCached gate patch missing?)\n",
					(unsigned long long)cachedVer);
			}
			continue;
		}
		// svc_PersistenceBaseline (s3cmd == 25) uses native dispatch.
		// S3 and S21 wire formats match once pdef schemas align (DefFile loads S3 pdef into S21 globals).
		// Types: 0=1bit 1=32bit 2=32bit 3=string 4=8bit. Fall through to generic vtable path for slot 28.
		else if (s3cmd == 37) // svc_UserMessage -- FORMAT BRIDGE
		{
			// S3 wire
			// [m_nLength bits] -- payload data (inline in bitstream)

			const uint32_t umMsgType = S21BR_ReadUBits(s21buf, 8);
			const uint32_t umLenBits = S21BR_ReadUBits(s21buf, 12);
			const uint32_t umLenBytes = (umLenBits + 7) / 8;

			// S3->S21 UserMessages_t index reconciliation: S21 dropped 4
			// legacy slots (AchievementEvent/UpdateJalopyRadar/CurrentTimescale/
			const bool umRpcFamily = (umMsgType >= 52 && umMsgType <= 54);
			uint32_t umMsgTypeS21 = umMsgType;
			if (umMsgType >= 25 && !umRpcFamily)
				umMsgTypeS21 = umMsgType - 4;

			static int s_umLog = 0;
			if (++s_umLog <= 5 || umMsgType == 2 || umMsgType == 3 || umMsgTypeS21 != umMsgType)
				SDK_Log("[BRIDGE-PM] UserMessage bridge: type=%u->%u lenBits=%u lenBytes=%u\n",
					umMsgType, umMsgTypeS21, umLenBits, umLenBytes);

			if (umLenBytes > 4096)
			{
				Warning(eDLL_T::ENGINE, "S21Bridge: UserMessage len %u bits too large\n", umLenBits);
				S21BR_SkipBits(s21buf, umLenBits);
				continue;
			}

			// Build S21-format buffer: 16b type + 16b lenBytes + payload
			uint8_t s21UmBuf[4104] = {};
			// Write S21 header (byte-aligned for simplicity since fields are 16-bit)
			s21UmBuf[0] = (uint8_t)(umMsgTypeS21 & 0xFF);
			s21UmBuf[1] = (uint8_t)((umMsgTypeS21 >> 8) & 0xFF);
			s21UmBuf[2] = (uint8_t)(umLenBytes & 0xFF);
			s21UmBuf[3] = (uint8_t)((umLenBytes >> 8) & 0xFF);

			// Copy payload bytes from S3 bitstream
			for (uint32_t i = 0; i < umLenBytes; i++)
			{
				const uint32_t bitsLeft = (umLenBits > i * 8) ? (umLenBits - i * 8) : 0;
				const uint32_t bitsToRead = (bitsLeft >= 8) ? 8 : bitsLeft;
				s21UmBuf[4 + i] = (bitsToRead > 0) ? (uint8_t)S21BR_ReadUBits(s21buf, bitsToRead) : 0;
			}

			// Inbound chat (SayText sub-message, type=3): the generic svc_UserMessage
			// dispatch (msgArray[49] RFB+Process) does NOT route the SayText sub-type
			if (umMsgType == 3 && umLenBytes >= 1)
			{
				S21Bridge_CallNativeSayText(&s21UmBuf[4], umLenBytes);
			}

			// PlayerNotifyDidDamage (raw S3 type=55, translated to S21 native slot 51)
			// same dispatch-routing bug as SayText above, proved by [DMG-DIAG-NATIVE]
			if (umMsgType == 55 && umLenBytes >= 1)
			{
				S21Bridge_CallNativeDidDamage(&s21UmBuf[4], umLenBytes);
			}

			// RemoteBulletFired / RemoteWeaponReload (raw S3 type=56/57, translated to S21
			// native slots 52/53): the cause behind all of these is that
			if (umMsgType == 56 && umLenBytes >= 1)
			{
				S21Bridge_CallNativeUmHandler((void*)s_origRemoteBulletFired, &s21UmBuf[4], umLenBytes);
			}
			if (umMsgType == 57 && umLenBytes >= 1)
			{
				S21Bridge_CallNativeUmHandler((void*)s_origRemoteWeaponReload, &s21UmBuf[4], umLenBytes);
			}

			// WeapProjFireCB (S3/S21 idx 17, fixed 66B -- no index remap; the -4 shift
			// only applies past the dropped AchievementEvent..DesiredTimescale block).
			if (umMsgType == 17 && umLenBytes >= 1)
			{
				const __int64 wpr = S21Bridge_CallNativeUmHandler(
					(void*)s_origWeapProjFireCB, &s21UmBuf[4], umLenBytes);
				static volatile LONG s_weapProjLog = 0;
				const LONG nWp = InterlockedIncrement(&s_weapProjLog);
				if (nWp <= 16 || (nWp % 200) == 0)
					Warning(eDLL_T::CLIENT,
						"[WEAPPROJ-CB] #%ld direct-call type=17 lenBytes=%u handler=%p r=%lld\n",
						nWp, umLenBytes, (void*)s_origWeapProjFireCB, (long long)wpr);
			}

			// The direct native handlers above are the ONLY dispatch path for a
			// bridged UserMessage. There is no generic fallback and there must not
			const bool umDirect =
				(umMsgType == 3 || umMsgType == 17 || umMsgType == 55
					|| umMsgType == 56 || umMsgType == 57);
			if (!umDirect)
			{
				static int s_umSkipLog = 0;
				static uint32_t s_umSkipSeen[64] = {};
				static int s_umSkipN = 0;
				bool seen = false;
				for (int i = 0; i < s_umSkipN; ++i)
				{
					if (s_umSkipSeen[i] == umMsgType) { seen = true; break; }
				}
				if (!seen && s_umSkipN < 64)
					s_umSkipSeen[s_umSkipN++] = umMsgType;
				if (!seen && s_umSkipLog < 24)
					++s_umSkipLog;
			}
			continue;
		}
		else if (s3cmd == 7) // svc_ServerInfo -- FORMAT BRIDGE
		{
			// S3 format: [16b proto][32b count][1b][1b][1b][16b][32b][16b][8b][8b]
			// [32b field11][8b field12]

			// Parse shared prefix (same in both S3 and S21)
			const uint32_t proto     = S21BR_ReadUBits(s21buf, 16);
			const uint32_t srvCount  = S21BR_ReadUBits(s21buf, 32);
			const uint32_t bDedicated= S21BR_ReadUBits(s21buf, 1);
			const uint32_t bBool2    = S21BR_ReadUBits(s21buf, 1);
			const uint32_t bBool3    = S21BR_ReadUBits(s21buf, 1);
			const uint32_t field6    = S21BR_ReadUBits(s21buf, 16);
			const uint32_t field7    = S21BR_ReadUBits(s21buf, 32);
			const uint32_t field8    = S21BR_ReadUBits(s21buf, 16);
			const uint32_t field9    = S21BR_ReadUBits(s21buf, 8);
			const uint32_t field10   = S21BR_ReadUBits(s21buf, 8);

			// S3-only fields at divergence point
			const uint32_t s3_field11 = S21BR_ReadUBits(s21buf, 32);
			const uint32_t s3_field12 = S21BR_ReadUBits(s21buf, 8);

			// S3 strings: 7x ReadString(260) + 1x ReadString(128)
			char s3Strings[8][260] = {};
			for (int si = 0; si < 7; si++)
			{
				for (int ci = 0; ci < 260; ci++)
				{
					uint8_t ch = (uint8_t)S21BR_ReadUBits(s21buf, 8);
					s3Strings[si][ci] = (char)ch;
					if (ch == 0) break;
				}
			}
			// String(128)
			for (int ci = 0; ci < 128; ci++)
			{
				uint8_t ch = (uint8_t)S21BR_ReadUBits(s21buf, 8);
				s3Strings[7][ci] = (char)ch;
				if (ch == 0) break;
			}

			// S3 flags byte
			const uint32_t flagsByte = S21BR_ReadUBits(s21buf, 8);

			// Skip bulk data if flags set (64000 x 32-bit reads)
			if (flagsByte)
				S21BR_SkipBits(s21buf, (int64_t)64000 * 32);

			for (int si = 0; si < 8; si++)
			{
				char* s = s3Strings[si];
				for (int i = 0; i < 260 && s[i]; ++i)
				{
					const unsigned char c = static_cast<unsigned char>(s[i]);
					if (c == '/' || c == '\\' || c == ':'
						|| (c == '.' && s[i + 1] == '.'))
					{
						s[0] = 0;
						break;
					}
				}
			}

			SDK_Log("[BRIDGE-PM] ServerInfo: proto=%u srvCount=%u ded=%u b2=%u b3=%u f6=%u f7=%u f8=%u f9=%u f10=%u f11=%u f12=%u flags=%u\n",
				proto, srvCount, bDedicated, bBool2, bBool3, field6, field7, field8, field9, field10, s3_field11, s3_field12, flagsByte);
			SDK_Log("[BRIDGE-PM] ServerInfo strings: [0]='%s' [1]='%s' [2]='%s' [3]='%s'\n",
				s3Strings[0], s3Strings[1], s3Strings[2], s3Strings[3]);
			SDK_Log("[BRIDGE-PM] ServerInfo strings: [4]='%s' [5]='%s' [6]='%s' [7]='%s'\n",
				s3Strings[4], s3Strings[5], s3Strings[6], s3Strings[7]);

			// Now build S21-format buffer and process via S21 vtable
			// Allocate buffer large enough for all fields
			static uint8_t s21SrvBuf[65536];
			memset(s21SrvBuf, 0, sizeof(s21SrvBuf));
			int wBit = 0;

			// Simple bit writer (LSB-first, matching engine bf_write)
			auto writeBits = [&](uint64_t val, int nBits) {
				for (int i = 0; i < nBits; i++)
				{
					if ((val >> i) & 1)
						s21SrvBuf[wBit / 8] |= (1 << (wBit % 8));
					wBit++;
				}
			};
			auto writeString = [&](const char* s, int maxLen) {
				for (int i = 0; i < maxLen; i++)
				{
					writeBits((uint8_t)s[i], 8);
					if (s[i] == 0) break;
				}
			};

			// Write S21 format: shared prefix
			writeBits(proto, 16);
			writeBits(srvCount, 32);
			writeBits(bDedicated, 1);
			writeBits(bBool2, 1);
			writeBits(bBool3, 1);
			writeBits(field6, 16);
			writeBits(field7, 32);
			writeBits(field8, 16);
			writeBits(field9, 8);
			writeBits(field10, 8);

			// S21-specific: QWORD -- server Unix timestamp.
			// S21 ProcessServerInfo reads this at struct+72
			writeBits((uint64_t)time(nullptr), 64);

			// S21-specific: 32-bit field
			writeBits(s3_field11, 32);

			// S21: 8-bit field (same as S3 field12)
			writeBits(s3_field12, 8);

			// S21: 6x String(260) -- use first 6 of S3's 7 strings
			for (int si = 0; si < 6; si++)
				writeString(s3Strings[si], 260);

			// S21: String(128) -- same as S3
			writeString(s3Strings[7], 128);

			// S21: String(9) -- new in S21, use empty string
			writeBits(0, 8); // null terminator

			// S21: flags byte
			writeBits(flagsByte, 8);

			// If flags: write bulk data (we skipped it in S3 read, write zeros)
			// In practice flags should be 0 for signon

			int totalBytes = (wBit + 7) / 8;

			// Init S21 bf_read from the translated buffer
			alignas(16) uint8_t srvS21Buf[S21BR_SIZE];
			s_S21BfReadInit(srvS21Buf, s21SrvBuf, (uint64_t)totalBytes);

			// Call S21 ReadFromBuffer + Process via vtable
			__try
			{
				reinterpret_cast<void(*)(void*)>(msgVtbl[8])(netMsg); // Init
			}
			__except(EXCEPTION_EXECUTE_HANDLER) {}

			bool srvReadOk = false;
			__try
			{
				srvReadOk = reinterpret_cast<bool(*)(void*, void*)>(msgVtbl[4])(netMsg, srvS21Buf);
			}
			__except(EXCEPTION_EXECUTE_HANDLER)
			{
				Warning(eDLL_T::ENGINE, "S21Bridge: crash in ServerInfo ReadFromBuffer\n");
			}

			bool srvProcessOk = false;
			if (srvReadOk)
			{
				__try
				{
					srvProcessOk = reinterpret_cast<bool(*)(void*)>(msgVtbl[3])(netMsg);
				}
				__except(EXCEPTION_EXECUTE_HANDLER)
				{
					Warning(eDLL_T::ENGINE, "S21Bridge: crash in ServerInfo Process\n");
				}
			}

			SDK_Log("[BRIDGE-PM] ServerInfo bridge: read=%d process=%d wBits=%d\n",
				srvReadOk ? 1 : 0, srvProcessOk ? 1 : 0, wBit);

			if (!srvProcessOk)
			{
				s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
				return false;
			}
			continue;
		}
		else if (s3cmd == 5) // net_SignonState -- FORMAT BRIDGE
		{
			// S3 format: [8b state][32b spawnCount][String][String][int64][String]
			// S21 format: [8b state][32b spawnCount][String(64)][String(32)][QWORD][String(64)]
			// Bridge: read S3 fields including strings, pass through to S21.

			// A sheared cursor landing on type 5 near the tail reads state 0
			// out of an overflowed reader, and state 0 dispatches straight
			// into S21Bridge_OnSessionEnded. Fail closed like svc_Menu.
			if (S21BR_GetBitsLeft(s21buf) < 40)
			{
				Warning(eDLL_T::ENGINE,
					"[BRIDGE-SIGNON] truncated S2C SignonState header (bitsLeft=%lld)\n",
					(long long)S21BR_GetBitsLeft(s21buf));
				s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
				return false;
			}

			const uint32_t signonState = S21BR_ReadUBits(s21buf, 8);
			const uint32_t spawnCount  = S21BR_ReadUBits(s21buf, 32);

			if (S21BR_IsOverflowed(s21buf))
			{
				Warning(eDLL_T::ENGINE,
					"[BRIDGE-SIGNON] overflow reading S2C SignonState header\n");
				s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
				return false;
			}

			// S3 CHANGELEVEL=9; S21 9 is MAYRECONNECT (drops to menu) and CHANGELEVEL is 10.
			const uint32_t s21SignonState = (signonState == 9) ? 10u : signonState;

			if (s21SignonState == 2 && s_lastSentSignonState > 2)
			{
				// A retransmit of a rung this sequence already dispatched is
				// not a rewind. Wiping the pending echo here parks BOTH
				// engines: the server never re-drives a rung above CONNECTED,
				// and a (2,-1) echo is ignored by a server already at 3+.
				if (s_signonSeqMax >= 2 &&
					((int)spawnCount < 0 || (int)spawnCount == s_signonSeqKey))
				{
					Warning(eDLL_T::ENGINE,
						"[BRIDGE-SIGNON] stale S2C CONNECTED dup dropped "
						"(floor=%d seqMax=%d spawn=%d)\n",
						s_lastSentSignonState, s_signonSeqMax, (int)spawnCount);
					S21BR_SkipSignonBody(s21buf);
					continue;
				}
				Warning(eDLL_T::ENGINE,
					"[BRIDGE-SIGNON] S2C CONNECTED rewind dropped (C2S floor was %d)\n",
					s_lastSentSignonState);
				S21Bridge_ClearPendingSignon();
				s_lastSentSignonState = -1;
				s_signonRewalk = true;
				s_rewalkTraceLeft = 400;
				s_rewalkTraceSignonLeft = 400;
				S21Bridge_ResetReliableRecv("S2C CONNECTED rewind");
				S21Bridge_QueueSignon(2, -1);
				// The rewind drops the message, not its body: the strings + qword
				// have no length prefix, and leaving them on the cursor shears the
				// next cmd.
				S21BR_SkipSignonBody(s21buf);
				continue;
			}

			if (s21SignonState >= 4)
				S21Bridge_ClearUserInfo();

			// Read S3 strings (null-terminated)
			char ssStr1[256] = {}, ssStr2[256] = {}, ssStr3[256] = {};
			bool ssOk = Bridge_ReadSignonString(s21buf, ssStr1, sizeof(ssStr1));
			ssOk = ssOk && Bridge_ReadSignonString(s21buf, ssStr2, sizeof(ssStr2));
			const uint64_t ssQword = ((uint64_t)S21BR_ReadUBits(s21buf, 32))
				| ((uint64_t)S21BR_ReadUBits(s21buf, 32) << 32);
			ssOk = ssOk && Bridge_ReadSignonString(s21buf, ssStr3, sizeof(ssStr3));

			if (!ssOk || S21BR_IsOverflowed(s21buf))
			{
				Warning(eDLL_T::ENGINE,
					"[BRIDGE-SIGNON] overflow reading S2C SignonState body "
					"(s3state=%u spawn=%u) -- dropped\n",
					signonState, spawnCount);
				s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
				return false;
			}

			SDK_Log("[BRIDGE-PM] SignonState bridge: s3state=%u -> s21state=%u spawn=%u s1='%s' s2='%s' qw=0x%llX s3='%s'\n",
				signonState, s21SignonState, spawnCount, ssStr1, ssStr2, (unsigned long long)ssQword, ssStr3);
			if (signonState == 9 || s21SignonState == 10)
			{
				Warning(eDLL_T::ENGINE,
					"[BRIDGE-SIGNON] CHANGELEVEL s3=%u -> s21=%u spawn=%u map='%s' mode='%s'\n",
					signonState, s21SignonState, spawnCount, ssStr1, ssStr2);
			}
			else if (s_signonRewalk)
			{
				Warning(eDLL_T::ENGINE,
					"[BRIDGE-SIGNON] re-walk S2C SignonState(%u) spawn=%d\n",
					s21SignonState, (int)spawnCount);
			}

			// ssStr3 is the dedi playlist name (S3 SignonState). Apply before load UI paints.
			S21Bridge_ApplyServerPlaylist(ssStr3);

			// Build S21 format buffer
			static uint8_t s21SsBuf[1024];
			memset(s21SsBuf, 0, sizeof(s21SsBuf));
			int wBit = 0;
			auto writeBitsSS = [&](uint64_t val, int nBits) {
				for (int b = 0; b < nBits; b++)
				{
					if ((val >> b) & 1)
						s21SsBuf[wBit / 8] |= (1 << (wBit % 8));
					wBit++;
				}
			};

			// S21 reads String(maxLen): the NUL must land inside the window.
			auto writeStringSS = [&](const char* s, int maxLen) {
				int i = 0;
				for (; i < maxLen - 1 && s[i] != 0; i++)
					writeBitsSS((uint8_t)s[i], 8);
				writeBitsSS(0, 8);
			};

			if (ssStr1[0] && !Bridge_IsBareMapName(ssStr1))
			{
				Warning(eDLL_T::ENGINE,
					"[BRIDGE-SIGNON] rejecting map name '%s'\n", ssStr1);
				Bridge_CopyBoundedCString("mp_lobby", 8, ssStr1, sizeof(ssStr1));
			}

			writeBitsSS(s21SignonState, 8);
			// ActivateServer bumps spawn (1->2). S21 CONNECTED rejects a
			// future server count vs the CHANGELEVEL spawn. -1 skips it.
			uint32_t outSpawn = spawnCount;
			if (s_signonRewalk && s21SignonState >= 2 && s21SignonState <= 8)
				outSpawn = 0xFFFFFFFFu;
			if (outSpawn != spawnCount)
				Warning(eDLL_T::ENGINE,
					"[BRIDGE-SIGNON] re-walk spawn %u -> -1 (state=%u)\n",
					spawnCount, s21SignonState);
			writeBitsSS(outSpawn, 32);
			writeStringSS(ssStr1, 64);   // S21: String(64) -- pass through S3 map name etc.
			writeStringSS(ssStr2, 32);   // S21: String(32)
			writeBitsSS(ssQword & 0xFFFFFFFF, 32);  // QWORD low
			writeBitsSS(ssQword >> 32, 32);          // QWORD high
			writeStringSS(ssStr3, 64);   // S21: String(64)

			int totalBytes = (wBit + 7) / 8;
			// Pad to next DWORD boundary + extra word to prevent bf_read edge overflow
			int paddedBytes = ((totalBytes + 3) & ~3) + 4;

			// Defer state=3 until SendTables/ClassInfo arrive in Transfer #2.
			if (signonState == 3 && !s_dbSawSendTables)
			{
				SDK_Log("[BRIDGE-PM] DEFERRING SignonState(3): no SendTables yet "
					"(transfer #%d). Will replay before next state dispatch.\n",
					s_dbTransferCount);
				memcpy(s_deferredSignon3Buf, s21SsBuf, sizeof(s_deferredSignon3Buf));
				s_deferredSignon3Bits = wBit;
				s_deferredSignon3Spawn = (int)spawnCount;
				s_deferredSignon3 = true;

				S21Bridge_QueueSignon(3, (int)spawnCount);
				s_lastSentSignonState = 3;
				SDK_Log("[BRIDGE-PM] queued C2S net_SignonState(3) for dedi\n");
				continue;
			}

			// Replay deferred state=3 BEFORE dispatching state=4+ so the
			// engine's case 3 body runs while currentState is still 2.
			if (s_deferredSignon3 && signonState > 3
				&& (s_deferredSignon3Spawn < 0 || s_deferredSignon3Spawn == (int)spawnCount))
			{
				SDK_Log("[BRIDGE-PM] REPLAYING deferred SignonState(3) before "
					"dispatching state=%u\n", signonState);

				int defTotalBytes = (s_deferredSignon3Bits + 7) / 8;
				int defPaddedBytes = ((defTotalBytes + 3) & ~3) + 4;

				alignas(16) uint8_t defS21Buf[S21BR_SIZE];
				s_S21BfReadInit(defS21Buf, s_deferredSignon3Buf, (uint64_t)defPaddedBytes);

				const int s21SsType3 = 6;
				if (s21SsType3 < msgCount)
				{
					void* ssMsg3 = msgArray[s21SsType3];
					if (ssMsg3)
					{
						typedef bool (__fastcall *RFB_fn)(void*, void*);
						typedef __int64 (__fastcall *Process_fn)(void*);
						RFB_fn ssRFB3 = (RFB_fn)NetObs_NetSignonStateReadFromBufferAddr();
						Process_fn ssProc3 = (Process_fn)NetObs_NetSignonStateProcessAddr();

						bool readOk3 = false;
						__try { readOk3 = ssRFB3(ssMsg3, defS21Buf); }
						__except(EXCEPTION_EXECUTE_HANDLER)
						{ Warning(eDLL_T::ENGINE, "S21Bridge: crash in deferred SignonState(3) RFB\n"); }

						bool procOk3 = false;
						if (readOk3)
						{
							__try { procOk3 = ssProc3(ssMsg3) != 0; }
							__except(EXCEPTION_EXECUTE_HANDLER)
							{ Warning(eDLL_T::ENGINE, "S21Bridge: crash in deferred SignonState(3) Process\n"); }
						}
						SDK_Log("[BRIDGE-PM] deferred SignonState(3) replay: read=%d process=%d\n",
							readOk3 ? 1 : 0, procOk3 ? 1 : 0);
					}
				}
				s_deferredSignon3 = false;
			}

			// Drop a state already dispatched for this signon sequence (changelevel-aware).
			{
				if (s21SignonState == 10)
				{
					// Second notify for the same CHANGELEVEL edge (no ordinary
					// signon state processed since the first).
					if (s_signonSeqDidCL && !s_signonSawStateSinceCL)
					{
						Warning(eDLL_T::ENGINE,
							"[BRIDGE-SIGNON] DEDUP: dropping duplicate CHANGELEVEL "
							"spawn=%u (no state since CL)\n", spawnCount);
						continue;
					}
					// Fresh changelevel: reset the floor so the low re-signon
					// states that follow are allowed. CHANGELEVEL itself does NOT
					s_signonSeqKey   = (int)spawnCount;
					// The changelevel notify still carries the OUTGOING map's
					// count; the new one only arrives with the re-signon. An echo
					s_signonSeqSpawn = -1;
					s_signonSeqMax   = -1;
					s_signonSeqDidCL = true;
					s_signonSawStateSinceCL = false;
					s_dbSawSendTables = false;
					s_deferredSignon3 = false;
					s_deferredSignon3Bits = 0;
					s_deferredSignon3Spawn = -1;
					// C2S walk is Hook_CS_SetSignonState behind a monotonic floor left at FULL.
					S21Bridge_ClearPendingSignon();
					s_lastSentSignonState = -1;
					s_signonRewalk        = true;
					s_rewalkTraceLeft     = 400;
					s_rewalkTraceSignonLeft = 400;
					SDK_Log("[BRIDGE-PM] CHANGELEVEL seq reset: sawSendTables/defer3/C2S floor cleared\n");
				}
				else
				{
					// A different spawn count (fresh connect / new map without a
					// changelevel notification) also starts a new sequence.
					if ((int)spawnCount >= 0 && (int)spawnCount != s_signonSeqKey)
					{
						s_signonSeqKey   = (int)spawnCount;
						if ((int)spawnCount >= 0)
							s_signonSeqSpawn = (int)spawnCount;
						s_signonSeqMax   = -1;
						s_signonSeqDidCL = false;
						s_deferredSignon3 = false;
						s_deferredSignon3Bits = 0;
						s_deferredSignon3Spawn = -1;
					}
					if ((int)s21SignonState <= s_signonSeqMax)
					{
						Warning(eDLL_T::ENGINE,
							"[BRIDGE-SIGNON] DEDUP: dropping SignonState(%u) -- already "
							"dispatched <=%d for spawn=%u\n",
							s21SignonState, s_signonSeqMax, spawnCount);
						continue;
					}
					// Claim this state BEFORE the (potentially 12s) dispatch so a
					// retransmit arriving mid-dispatch is recognized as a dup.
					s_signonSeqMax = (int)s21SignonState;
					s_signonSawStateSinceCL = true;
				}
			}

			if ((int)s21SignonState != s_signonEchoState)
			{
				s_signonEchoState = -1;
				s_signonEchoUntilMs = 0;
			}

			// Build S21 bf_read for translated buffer
			alignas(16) uint8_t ssS21Buf[S21BR_SIZE];
			s_S21BfReadInit(ssS21Buf, s21SsBuf, (uint64_t)paddedBytes);

			// Dispatch to S21 slot 6 (net_SignonState)
			const int s21SsType = 6;
			if (s21SsType < msgCount)
			{
				void* ssMsg = msgArray[s21SsType];
				if (ssMsg)
				{
					void** ssVtbl = *reinterpret_cast<void***>(ssMsg); (void)ssVtbl;

					// NET_SignonState: call ReadFromBuffer and Process by DIRECT RVA
					// because the vtable layout differs from the generic INetMessage layout.
					typedef bool (__fastcall *RFB_fn)(void*, void*);
					typedef __int64 (__fastcall *Process_fn)(void*);
					RFB_fn ssRFB = (RFB_fn)NetObs_NetSignonStateReadFromBufferAddr();
					Process_fn ssProc = (Process_fn)NetObs_NetSignonStateProcessAddr();

					bool ssReadOk = false;
					__try { ssReadOk = ssRFB(ssMsg, ssS21Buf); }
					__except(S21Bridge_CrashFilter(GetExceptionInformation()))
					{
						Warning(eDLL_T::ENGINE,
							"S21Bridge: crash in SignonState ReadFromBuffer: code=0x%08lX addr_va=0x%llX\n",
							s_lastCrashCode, DeathObs_ImageVA(s_lastCrashAddr));
						BridgeTrace_Log(
							"[SEH-ADDR] crash in SignonState ReadFromBuffer: code=0x%08lX addr_va=0x%llX\n",
							s_lastCrashCode, DeathObs_ImageVA(s_lastCrashAddr));
						BridgeTrace_Flush();
					}

					SDK_Log("[BRIDGE-PM] SignonState: RFB read=%d msg=%p handler=%p\n",
						ssReadOk ? 1 : 0, (void*)ssMsg,
						ssReadOk ? *(void**)((uintptr_t)ssMsg + 24) : nullptr);

					bool ssProcessOk = false;
					if (ssReadOk)
					{
						__try { ssProcessOk = ssProc(ssMsg) != 0; }
						__except(S21Bridge_CrashFilter(GetExceptionInformation()))
						{
							Warning(eDLL_T::ENGINE,
								"S21Bridge: crash in SignonState Process: code=0x%08lX addr_va=0x%llX\n",
								s_lastCrashCode, DeathObs_ImageVA(s_lastCrashAddr));
							BridgeTrace_Log(
								"[SEH-ADDR] crash in SignonState Process: code=0x%08lX addr_va=0x%llX\n",
								s_lastCrashCode, DeathObs_ImageVA(s_lastCrashAddr));
							BridgeTrace_Flush();
							// AV inside SetSignonState: dump decode crumb rings.
							__try { Bridge_DumpDecodeCrumbsOnCrash("SignonState Process"); }
							__except(EXCEPTION_EXECUTE_HANDLER) {}
						}
					}

					SDK_Log("[BRIDGE-PM] SignonState bridge: read=%d process=%d state=%u spawn=%u\n",
						ssReadOk ? 1 : 0, ssProcessOk ? 1 : 0, signonState, spawnCount);
				}
			}
			continue;
		}
		else if (s3cmd == 6) // net_MTXUserMsg -- S3 is [u32 L][L bits], S21 RFB is not
		{
			const uint32_t nBits = S21BR_ReadUBits(s21buf, 32);
			const int64_t left = S21BR_GetBitsLeft(s21buf);
			if (S21BR_IsOverflowed(s21buf) || nBits > (uint32_t)left || nBits > (1u << 20))
			{
				Warning(eDLL_T::ENGINE,
					"[BRIDGE-PM] net_MTXUserMsg skip: nBits=%u left=%lld -- dropping body\n",
					nBits, (long long)left);
				if (left > 0)
					S21BR_SkipBits(s21buf, left);
				continue;
			}
			S21BR_SkipBits(s21buf, (int64_t)nBits);
			continue;
		}
		else if (s3cmd == 12) // svc_CreateStringTable -- FORMAT BRIDGE
		{
			// CreateStringTable: S3 dataLen is 22 bits, S21 is 24. Other fields match.
			char cstName[260] = {};
			for (int ci = 0; ci < 256; ci++)
			{
				uint8_t ch = (uint8_t)S21BR_ReadUBits(s21buf, 8);
				cstName[ci] = (char)ch;
				if (ch == 0) break;
			}

			const uint32_t cstMaxEntries = S21BR_ReadUBits(s21buf, 16);

			if (cstMaxEntries == 0)
			{
				S21Bridge_LogSkipTransfer(s3cmd, S21BR_GetBitsLeft(s21buf),
					lastCmd, lastStartBit, lastEndBit, lastOrdinal,
					"CreateStringTable maxEntries 0");
				s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
				break;
			}

			int cstEntryBits = 0;
			for (uint32_t me = cstMaxEntries >> 1; me > 0; me >>= 1)
				cstEntryBits++;
			cstEntryBits++;
			const uint32_t cstNumEntries = S21BR_ReadUBits(s21buf, cstEntryBits);

			// 4. Data length -- S3 uses 22 bits (verified: writes 22)
			// This is the ONLY format difference between S3 and S21.
			const uint32_t cstDataLenBits = S21BR_ReadUBits(s21buf, 22);

			// 5. hasUserData FIRST (same order as S21 -- NOT swapped!)
			const uint32_t cstHasUserData = S21BR_ReadUBits(s21buf, 1);
			uint32_t cstUserDataSize = 0;
			uint32_t cstUserDataSizeBits = 0;
			if (cstHasUserData)
			{
				cstUserDataSize = S21BR_ReadUBits(s21buf, 12);
				cstUserDataSizeBits = S21BR_ReadUBits(s21buf, 4);
			}
			if (cstHasUserData && cstUserDataSizeBits > 14)
			{
				Warning(eDLL_T::ENGINE,
					"[BRIDGE-PM] CreateStringTable '%s' userDataSizeBits %u over 14 -- skip body signon=%d\n",
					cstName, cstUserDataSizeBits, s_lastSentSignonState);
				const uint32_t cstSkipCompressed = S21BR_ReadUBits(s21buf, 1);
				const uint32_t cstSkipFlags = S21BR_ReadUBits(s21buf, 2);
				(void)cstSkipCompressed;
				(void)cstSkipFlags;
				if (S21BR_IsOverflowed(s21buf)
					|| (cstDataLenBits != 0
						&& cstDataLenBits > (uint32_t)S21BR_GetBitsLeft(s21buf)))
				{
					S21Bridge_LogSkipTransfer(s3cmd, S21BR_GetBitsLeft(s21buf),
						lastCmd, lastStartBit, lastEndBit, lastOrdinal,
						"CreateStringTable userdata skip truncated");
					s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
					break;
				}
				S21BR_SkipBits(s21buf, cstDataLenBits);
				continue;
			}

			// 6. isCompressed SECOND (same order as S21)
			const uint32_t cstCompressed = S21BR_ReadUBits(s21buf, 1);

			// 7. 2-bit flags (S3 HAS this field too -- verified)
			const uint32_t cstFlags = S21BR_ReadUBits(s21buf, 2);

			// [SEC] s21CstBuf is 4 MiB; leave headroom for CST header bits + safety pad
			// so header+payload always fit (C10). LZSS uncomp must be strictly smaller.
			static constexpr uint32_t kS21CstBufCap = 4u * 1024u * 1024u;
			static constexpr uint32_t kS21CstHdrHeadroom = 4096u; // bytes reserved for name+fields
			static constexpr uint32_t kS21CstUncompMax = kS21CstBufCap - kS21CstHdrHeadroom;

			const uint32_t cstDataLenBytes = (cstDataLenBits + 7) / 8;
			if (S21BR_IsOverflowed(s21buf))
			{
				S21Bridge_LogSkipTransfer(s3cmd, S21BR_GetBitsLeft(s21buf),
					lastCmd, lastStartBit, lastEndBit, lastOrdinal,
					"CreateStringTable overflowed reader");
				s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
				break;
			}
			if (cstDataLenBits != 0
				&& cstDataLenBits > (uint32_t)S21BR_GetBitsLeft(s21buf))
			{
				S21Bridge_LogSkipTransfer(s3cmd, S21BR_GetBitsLeft(s21buf),
					lastCmd, lastStartBit, lastEndBit, lastOrdinal,
					"CreateStringTable dataLen exceeds leftover");
				s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
				break;
			}
			if (S21Bridge_CstCountInvalid(static_cast<int>(cstNumEntries), static_cast<int>(cstMaxEntries)))
			{
				static int s_cstNumLog = 0;
				if (++s_cstNumLog <= 8)
					Warning(eDLL_T::ENGINE,
						"[SEC] CreateStringTable '%s' numEntries=%u > maxEntries=%u -- drop\n",
						cstName, cstNumEntries, cstMaxEntries);
				S21BR_SkipBits(s21buf, cstDataLenBits);
				continue;
			}
			if (cstDataLenBytes > kS21CstUncompMax)
			{
				Warning(eDLL_T::ENGINE,
					"[BRIDGE-PM] CreateStringTable '%s' dataLen %u over cap -- skip body signon=%d\n",
					cstName, cstDataLenBits, s_lastSentSignonState);
				S21BR_SkipBits(s21buf, cstDataLenBits);
				continue;
			}

			// Read the raw data into a temp buffer (forward pass, no seek-back needed)
			uint8_t* cstRawData = (uint8_t*)malloc(cstDataLenBytes + 16);
			if (!cstRawData)
			{
				S21BR_SkipBits(s21buf, cstDataLenBits);
				continue;
			}
			memset(cstRawData, 0, cstDataLenBytes + 16);
			// Read data bit-by-bit to preserve exact alignment
			for (uint32_t i = 0; i < cstDataLenBits; i++)
			{
				uint32_t bit = S21BR_ReadUBits(s21buf, 1);
				if (bit)
					cstRawData[i / 8] |= (1 << (i % 8));
			}

			static int s_cstDiag = 0;
			if (++s_cstDiag <= 30)
				SDK_Log("[BRIDGE-PM] CreateStringTable S3: name='%s' maxEnt=%u numEnt=%u dataLen=%u comp=%u hasUD=%u udSz=%u udBits=%u flags=%u\n",
					cstName, cstMaxEntries, cstNumEntries, cstDataLenBits,
					cstCompressed, cstHasUserData, cstUserDataSize, cstUserDataSizeBits, cstFlags);

			// S3 uses LZSS compression (magic "LZSS").
			// S21 uses a different format (magic 0xFFFFFFFD).
			// Decompress S3 data here and pass as uncompressed/Oodle to S21.
			uint8_t* cstFinalData = cstRawData;
			uint32_t cstFinalDataBits = cstDataLenBits;
			uint32_t cstFinalComp = cstCompressed;
			bool cstLzssFailClosed = false;

			// Data blob format when comp=1
			// S21 checks byte 8+ for magic 0xFFFFFFFD (its own format).
			if (cstCompressed && cstDataLenBytes >= 16 &&
				*(uint32_t*)(cstRawData + 8) == 0x53535A4C) // "LZSS" at offset 8
			{
				uint32_t uncompSz = ((uint32_t*)cstRawData)[0];
				// [SEC] Fail closed on oversize / zero -- never allocate or write past s21CstBuf.
				if (uncompSz == 0 || uncompSz > kS21CstUncompMax)
				{
					Warning(eDLL_T::ENGINE, "[SEC] CreateStringTable LZSS uncompSz=%u over cap %u for '%s' -- drop\n",
						uncompSz, kS21CstUncompMax, cstName);
					cstLzssFailClosed = true;
				}
				else
				{
					uint8_t* decompBuf = (uint8_t*)malloc(uncompSz + 16);
					if (!decompBuf)
					{
						Warning(eDLL_T::ENGINE, "[SEC] CreateStringTable LZSS alloc failed '%s' -- drop\n", cstName);
						cstLzssFailClosed = true;
					}
					else
					{
						memset(decompBuf, 0, uncompSz + 16);
						// LZSS stream at offset 8: lzss_header_t { id, actualSize } + payload.
						// Prefer CLZSS::SafeUncompress (bounds output + backrefs below decompBuf).
						CLZSS lzss;
						const unsigned int got = lzss.SafeUncompress(
							cstRawData + 8, decompBuf, uncompSz, cstDataLenBytes - 8);

						// Harden: also reject if declared compressed size overruns input blob.
						const uint32_t declaredCompSz = ((uint32_t*)cstRawData)[1];
						const bool compSzOk = (declaredCompSz >= 8) &&
							(8u + declaredCompSz <= cstDataLenBytes);

						if (got == uncompSz && compSzOk)
						{
							// LZSS decompressed successfully. Now Oodle-recompress
							// for S21 format. S21 Process checks consumed==dataLen on
							typedef __int64 (*OodleLZ_Compress_fn)(
								int, const uint8_t*, __int64, uint8_t*,
								unsigned int, void*, uint64_t, void*, void*, void*);
							auto pOodleCompress = (OodleLZ_Compress_fn)NetObs_OodleLZCompressAddr();

							uint32_t oodleBufSz = uncompSz + 274;
							uint8_t* oodleBuf = (uint8_t*)malloc(oodleBufSz);
							__int64 oodleCompSz = 0;

							if (oodleBuf && pOodleCompress)
							{
								__try {
									oodleCompSz = pOodleCompress(
										8,          // Kraken
										decompBuf, uncompSz,
										oodleBuf, 1, // SuperFast
										NULL, 0, NULL, NULL, NULL);
								}
								__except (EXCEPTION_EXECUTE_HANDLER) {
									oodleCompSz = 0;
								}
							}

							if (oodleCompSz > 0)
							{
								// S21 comp=1 data layout in bf_read
								// [uncompSz:32][blobSize:32][blob:blobSize bytes]
								// blob = [0xFFFFFFFD:4B][uncompSz:4B][Oodle payload]
								uint32_t blobSize = 8 + (uint32_t)oodleCompSz;
								uint32_t totalDataBytes = 8 + blobSize;

								// [SEC] Oodle blob must still fit s21CstBuf headroom.
								if (totalDataBytes > kS21CstUncompMax)
								{
									free(oodleBuf);
									free(decompBuf);
									Warning(eDLL_T::ENGINE, "[SEC] CreateStringTable Oodle blob %u over cap for '%s' -- drop\n",
										totalDataBytes, cstName);
									cstLzssFailClosed = true;
								}
								else
								{
									uint8_t* s21CompData = (uint8_t*)malloc(totalDataBytes + 16);
									if (s21CompData)
									{
										memset(s21CompData, 0, totalDataBytes + 16);
										*(uint32_t*)(s21CompData + 0) = uncompSz;
										*(uint32_t*)(s21CompData + 4) = blobSize;
										*(uint32_t*)(s21CompData + 8) = 0xFFFFFFFD;
										*(uint32_t*)(s21CompData + 12) = uncompSz;
										memcpy(s21CompData + 16, oodleBuf, oodleCompSz);

										free(decompBuf);
										free(oodleBuf);

										cstFinalData = s21CompData;
										cstFinalDataBits = totalDataBytes * 8;
										cstFinalComp = 1;

										if (s_cstDiag <= 30)
											SDK_Log("[BRIDGE-PM] LZSS->Oodle: '%s' %u -> %u -> %u bytes\n",
												cstName, cstDataLenBytes, uncompSz,
												(uint32_t)oodleCompSz);
									}
									else
									{
										free(oodleBuf);
										// Uncompressed fallback only if it fits the rewrite buffer.
										cstFinalData = decompBuf;
										cstFinalDataBits = uncompSz * 8;
										cstFinalComp = 0;
									}
								}
							}
							else
							{
								if (oodleBuf) free(oodleBuf);
								cstFinalData = decompBuf;
								cstFinalDataBits = uncompSz * 8;
								cstFinalComp = 0;
								SDK_Log("[BRIDGE-PM] Oodle compress failed '%s', uncompressed fallback\n",
									cstName);
							}
						}
						else
						{
							free(decompBuf);
							Warning(eDLL_T::ENGINE, "[SEC] CreateStringTable LZSS SafeUncompress FAILED: "
								"'%s' got=%u expected=%u compSzOk=%d -- drop\n",
								cstName, got, uncompSz, compSzOk ? 1 : 0);
							cstLzssFailClosed = true;
						}
					}
				}
			}

			if (cstLzssFailClosed)
			{
				if (cstFinalData != cstRawData)
					free(cstFinalData);
				free(cstRawData);
				continue;
			}

			// -- Build S21 format: dataLen 22->24 bits, decompress LZSS->raw --
			// [SEC C10] Capacity-bound rewrite into s21CstBuf; fail closed on oversize.
			uint32_t cstFinalDataBytes = (cstFinalDataBits + 7) / 8;
			static uint8_t s21CstBuf[kS21CstBufCap];
			const uint32_t cstBitCap = kS21CstBufCap * 8u;

			// Estimate header+payload bits (name <= 256*8 + NUL, fields ~64, data).
			const uint32_t estBits =
				(256u * 8u) + 16u + (uint32_t)cstEntryBits + 24u + 1u +
				(cstHasUserData ? 16u : 0u) + 1u + 2u + (cstFinalDataBytes * 8u);
			if (cstFinalDataBytes > kS21CstUncompMax || estBits > cstBitCap)
			{
				Warning(eDLL_T::ENGINE, "[SEC] CreateStringTable rewrite oversize '%s' "
					"dataBytes=%u estBits=%u capBits=%u -- drop\n",
					cstName, cstFinalDataBytes, estBits, cstBitCap);
				if (cstFinalData != cstRawData)
					free(cstFinalData);
				free(cstRawData);
				continue;
			}

			memset(s21CstBuf, 0, (estBits + 7u) / 8u);
			int wBit = 0;
			bool cstWriteOvf = false;

			auto writeBitsCST = [&](uint64_t val, int nBits) {
				if (cstWriteOvf || nBits < 0 || nBits > 64)
				{
					cstWriteOvf = true;
					return;
				}
				if ((uint32_t)wBit + (uint32_t)nBits > cstBitCap)
				{
					cstWriteOvf = true;
					return;
				}
				for (int b = 0; b < nBits; b++)
				{
					if ((val >> b) & 1)
						s21CstBuf[wBit / 8] |= (1 << (wBit % 8));
					wBit++;
				}
			};
			auto writeStringCST = [&](const char* s, int maxLen) {
				for (int i = 0; i < maxLen; i++)
				{
					writeBitsCST((uint8_t)s[i], 8);
					if (s[i] == 0) break;
				}
			};

			// 1. String(256): tableName
			writeStringCST(cstName, 256);

			// 2. uint16: maxEntries (same)
			writeBitsCST(cstMaxEntries, 16);

			// 3. (log2(maxEntries)+1) bits: numEntries (same)
			writeBitsCST(cstNumEntries, cstEntryBits);

			// S21 dataLen is 24 bits (S3 was 22).
			writeBitsCST(cstFinalDataBits, 24);

			// All remaining fields are IDENTICAL between S3 and S21 -- pass through
			writeBitsCST(cstHasUserData, 1);
			if (cstHasUserData)
			{
				writeBitsCST(cstUserDataSize, 12);
				writeBitsCST(cstUserDataSizeBits, 4);
			}
			writeBitsCST(cstFinalComp, 1);
			writeBitsCST(cstFlags, 2);

			// 9. Raw data: copy from final buffer (decompressed if LZSS)
			for (uint32_t i = 0; i < cstFinalDataBytes; i++)
				writeBitsCST(cstFinalData[i], 8);

			if (cstFinalData != cstRawData)
				free(cstFinalData);
			free(cstRawData);

			if (cstWriteOvf)
			{
				Warning(eDLL_T::ENGINE, "[SEC] CreateStringTable writeBits overflow '%s' wBit=%d -- drop\n",
					cstName, wBit);
				continue;
			}

			int totalBytes = (wBit + 7) / 8;
			if (totalBytes <= 0 || (uint32_t)totalBytes > kS21CstBufCap)
			{
				Warning(eDLL_T::ENGINE, "[SEC] CreateStringTable totalBytes=%d invalid for '%s' -- drop\n",
					totalBytes, cstName);
				continue;
			}

			// Build S21 bf_read for translated buffer
			alignas(16) uint8_t cstS21Buf[S21BR_SIZE];
			s_S21BfReadInit(cstS21Buf, s21CstBuf, (uint64_t)totalBytes);

			// Dispatch to S21 slot 15 (svc_CreateStringTable)
			// Like SVC_Snapshot, the client-side message vtable may have
			const int s21CstType = 15;
			if (s21CstType < msgCount)
			{
				void* cstMsg = msgArray[s21CstType];
				if (cstMsg)
				{
					void** cstVtbl = *reinterpret_cast<void***>(cstMsg);

					static bool s_cstVtblLogged = false;
					if (!s_cstVtblLogged)
					{
						s_cstVtblLogged = true;
						SDK_Log("[BRIDGE-PM] CreateStringTable vtable diag: vtbl=%p [3]=%p [4]=%p [5]=%p\n",
							(void*)cstVtbl, cstVtbl[3], cstVtbl[4], cstVtbl[5]);
					}

					typedef bool (__fastcall *CstRFB_fn)(void*, void*);
					typedef __int64 (__fastcall *CstProc_fn)(void*);
					CstRFB_fn  cstRFB  = (CstRFB_fn)NetObs_Sym(NetObsSym_t::CustomStringTableReadFromBuffer);
					CstProc_fn cstProc = (CstProc_fn)NetObs_Sym(NetObsSym_t::CustomStringTableProcess);

					bool cstReadOk = false;
					__try { cstReadOk = cstRFB(cstMsg, cstS21Buf); }
					__except(EXCEPTION_EXECUTE_HANDLER)
					{ Warning(eDLL_T::ENGINE, "S21Bridge: crash in CreateStringTable ReadFromBuffer\n"); }

					// Pre-Process diagnostic: check if table already exists
					bool cstPreExists = false;
					void* cstHandler = nullptr;
					uintptr_t cstHandlerVtbl14 = 0;
					if (cstReadOk)
					{
						uintptr_t stCont = *(uintptr_t*)NetObs_StringTableContainerAddr();
						if (stCont)
						{
							void** cv = *(void***)stCont;
							void* existing = nullptr;
							__try { existing = reinterpret_cast<void*(*)(void*, const char*)>(cv[3])((void*)stCont, cstName); }
							__except(EXCEPTION_EXECUTE_HANDLER) {}
							cstPreExists = (existing != nullptr);
						}
						cstHandler = *(void**)((uintptr_t)cstMsg + 24);
						if (cstHandler)
						{
							void** hVtbl = *(void***)cstHandler;
							cstHandlerVtbl14 = (uintptr_t)hVtbl[14];
						}
					}

					bool cstProcessOk = false;
					if (cstReadOk)
					{
						__try { cstProcessOk = cstProc(cstMsg) != 0; }
						__except(EXCEPTION_EXECUTE_HANDLER)
						{ Warning(eDLL_T::ENGINE, "S21Bridge: crash in CreateStringTable Process\n"); }
					}

					static int s_cstLog = 0;
					if (++s_cstLog <= 30)
						SDK_Log("[BRIDGE-PM] CreateStringTable bridge: read=%d process=%d preExist=%d name='%s' maxEnt=%u numEnt=%u dataBytes=%u handler=%p vtbl14=%p\n",
							cstReadOk ? 1 : 0, cstProcessOk ? 1 : 0, cstPreExists ? 1 : 0,
							cstName, cstMaxEntries, cstNumEntries, cstDataLenBytes,
							cstHandler, (void*)cstHandlerVtbl14);

					if (cstProcessOk && strcmp(cstName, "instancebaseline") == 0)
					{
						uintptr_t stContainer = *(uintptr_t*)NetObs_StringTableContainerAddr();
						if (stContainer)
						{
							void** contVtbl = *(void***)stContainer;
							void* ibTable = nullptr;
							__try {
								ibTable = reinterpret_cast<void*(*)(void*, const char*)>(contVtbl[3])((void*)stContainer, "instancebaseline");
							} __except(EXCEPTION_EXECUTE_HANDLER) {}

							if (ibTable)
							{
								void** tblVtbl = *(void***)ibTable;
								int numEntries = 0;
								__try { numEntries = reinterpret_cast<int(*)(void*)>(tblVtbl[3])(ibTable); }
								__except(EXCEPTION_EXECUTE_HANDLER) {}

								SDK_Log("[BRIDGE-PM] instancebaseline table: numEntries=%d\n", numEntries);

								for (int ei = 0; ei < numEntries && ei < 20; ei++)
								{
									const char* entryKey = nullptr;
									__try { entryKey = reinterpret_cast<const char*(*)(void*, int)>(tblVtbl[11])(ibTable, ei); }
									__except(EXCEPTION_EXECUTE_HANDLER) {}
									SDK_Log("[BRIDGE-PM]   entry[%d] key='%s'\n", ei, entryKey ? entryKey : "(null)");
								}

								int idx0 = -1, idx116 = -1;
								__try { idx0 = reinterpret_cast<int(*)(void*, const char*)>(tblVtbl[10])(ibTable, "0"); }
								__except(EXCEPTION_EXECUTE_HANDLER) {}
								__try { idx116 = reinterpret_cast<int(*)(void*, const char*)>(tblVtbl[10])(ibTable, "116"); }
								__except(EXCEPTION_EXECUTE_HANDLER) {}
								SDK_Log("[BRIDGE-PM] instancebaseline FindStringIndex: '0'=%d '116'=%d\n", idx0, idx116);
							}
							else
							{
								SDK_Log("[BRIDGE-PM] instancebaseline table NOT FOUND in container\n");
							}
						}
					}
				}
			}
			continue;
		}
		else if (s3cmd == 40) // svc_Snapshot -- WIRE RE-ENCODE S3->S21
		{
			// S3 and S21 differ on the wire in TWO places only
			// => S3 helper = 137 bits, S21 helper = 152 bits

			// === RE-ENCODER PER-STAGE BIT TRACE (delta-aligned bug hunt) ===
			// Capture S3 input bit position at every stage so any off-by-N
			const int64_t snapStartBit = S21BR_GetBitsRead(s21buf);

			// 32-bit prefix (m_nDeltaFromTick / serverCount)
			const uint32_t serverCount = (uint32_t)S21BR_ReadUBits(s21buf, 32);
			const int64_t  rb_serverCount = S21BR_GetBitsRead(s21buf);

			// S3 ServerTickInfo: 32+32+16+16+1+8+32 = 137 bits
			const uint32_t sti_tick1    = (uint32_t)S21BR_ReadUBits(s21buf, 32);
			const uint32_t sti_tick2    = (uint32_t)S21BR_ReadUBits(s21buf, 32);
			const uint32_t sti_compute  = (uint32_t)S21BR_ReadUBits(s21buf, 16);
			const uint32_t sti_stddev   = (uint32_t)S21BR_ReadUBits(s21buf, 16);
			const uint32_t sti_connFlag = (uint32_t)S21BR_ReadUBits(s21buf, 1);  // S3 = 1 bit
			const uint32_t sti_byte     = (uint32_t)S21BR_ReadUBits(s21buf, 8);
			const uint32_t sti_cmdRun   = (uint32_t)S21BR_ReadUBits(s21buf, 32);
			const int64_t  rb_sti       = S21BR_GetBitsRead(s21buf);

			const uint32_t nMaxEntries = (uint32_t)S21BR_ReadUBits(s21buf, 15);
			const uint32_t isDelta     = (uint32_t)S21BR_ReadUBits(s21buf, 1);
			// Delta-from inner conditional (S3 + S21
			// use IDENTICAL structure)
			uint32_t nDeltaFrom      = 0xFFFFFFFF;
			uint32_t rawDeltaPresent = 0;
			uint32_t nRawDeltaFrom   = 0xFFFFFFFF;
			if (isDelta)
			{
				nDeltaFrom      = (uint32_t)S21BR_ReadUBits(s21buf, 32);
				rawDeltaPresent = (uint32_t)S21BR_ReadUBits(s21buf, 1);
				if (rawDeltaPresent)
					nRawDeltaFrom = (uint32_t)S21BR_ReadUBits(s21buf, 32);
			}
			const int64_t  rb_delta = S21BR_GetBitsRead(s21buf);

			// 112 shared header bits (identical layout in S3 and S21)
			// [1]+[14]+[1]+[1]+[14]+[32]+[1]+[32]+[1]+[1]+[14] = 112
			const uint32_t sh0 = (uint32_t)S21BR_ReadUBits(s21buf, 32);
			const uint32_t sh1 = (uint32_t)S21BR_ReadUBits(s21buf, 32);
			const uint32_t sh2 = (uint32_t)S21BR_ReadUBits(s21buf, 32);
			const uint32_t sh3 = (uint32_t)S21BR_ReadUBits(s21buf, 16);
			const int64_t  rb_shared = S21BR_GetBitsRead(s21buf);

			const uint32_t nLengthBits = (uint32_t)S21BR_ReadUBits(s21buf, 22); // S3 = 22 bits
			const int64_t  rb_nLenBits = S21BR_GetBitsRead(s21buf);
			const int64_t  snapBitsLeft = S21BR_GetBitsLeft(s21buf);
			if ((int64_t)nLengthBits > snapBitsLeft)
			{
				static int s_snapBadLen = 0;
				if (++s_snapBadLen <= 10)
					Warning(eDLL_T::ENGINE,
						"[BRIDGE-PM] Snapshot nLengthBits=%u > bitsLeft=%lld -- aborting packet\n",
						nLengthBits, (long long)snapBitsLeft);
				s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
				return true;
			}

			// Entity payload (nLengthBits bits)
			const int64_t afterHeader = S21BR_GetBitsRead(s21buf);

			// --- Build S21 wire buffer ---
			// s21DeltaBits = isDelta(1) + (nDeltaFrom(32) + rawDeltaPresent(1)
			const int64_t s21DeltaBits = isDelta
				? (1 + 32 + 1 + (rawDeltaPresent ? 32 : 0))
				: 1;
			const int64_t s21HeaderBits = 32 + 152 + 15 + s21DeltaBits + 112 + 24;
			const int64_t s21TotalBits = s21HeaderBits + nLengthBits;
			const int s21TotalBytes = (int)((s21TotalBits + 7) / 8);
			const int sbLevel = BridgeBudget_Level();
			static long s_sbSnapN = 0;
			const long sbN = (sbLevel > 0) ? InterlockedIncrement(&s_sbSnapN) : 0;
			const bool sbArm = (sbLevel > 0) && (!isDelta || BridgeBudget_Armed(sbLevel, sbN));
			LARGE_INTEGER sbFreq = {}, sbQPrep0 = {}, sbQBlit0 = {}, sbQBlit1 = {}, sbQ0 = {}, sbQ1 = {};
			unsigned long long sbT0 = 0, sbTMid = 0, sbT1 = 0;
			if (sbArm)
			{
				QueryPerformanceFrequency(&sbFreq);
				QueryPerformanceCounter(&sbQPrep0);
			}
			// Translated snapshot must fit the FULL payload. A 256 KB static buffer overruns.
			if (s21TotalBytes > 0 && (int)g_snapBufVec.size() < s21TotalBytes + 8)
				g_snapBufVec.resize((size_t)(s21TotalBytes + 8));
			uint8_t* const s_snapBuf = g_snapBufVec.empty() ? nullptr : g_snapBufVec.data();
			const int s_snapBufCap = (int)g_snapBufVec.size();
			if (s_snapBuf && s21TotalBytes > 0)
				memset(s_snapBuf, 0, (size_t)s21TotalBytes);

			int wBit = 0;
			auto writeBits = [&](uint64_t val, int nBits) {
				Snap_WriteBits(s_snapBuf, s_snapBufCap, wBit, val, nBits);
			};

			// serverCount (32)
			writeBits(serverCount, 32);
			const int wb_serverCount = wBit;

			// S21 ServerTickInfo (152 bits): 32+32+16+16+16+8+32
			writeBits(sti_tick1, 32);
			writeBits(sti_tick2, 32);
			writeBits(sti_compute, 16);
			writeBits(sti_stddev, 16);
			writeBits(sti_connFlag, 16);  // expanded from 1 bit to 16 bits
			writeBits(sti_byte, 8);
			writeBits(sti_cmdRun, 32);
			const int wb_sti = wBit;

			// nMaxEntries (15 bits, same)
			writeBits(nMaxEntries, 15);

			// isDelta + inner conditional (verbatim S3 -> S21 pass-through)
			writeBits(isDelta, 1);
			if (isDelta)
			{
				writeBits(nDeltaFrom, 32);
				writeBits(rawDeltaPresent, 1);
				if (rawDeltaPresent)
					writeBits(nRawDeltaFrom, 32);
			}
			const int wb_delta = wBit;

			// 112 shared header bits (verbatim)
			writeBits(sh0, 32);
			writeBits(sh1, 32);
			writeBits(sh2, 32);
			writeBits(sh3, 16);
			const int wb_shared = wBit;

			// nLengthBits (S21 = 24 bits, S3 was 22 - widen, same value)
			writeBits(nLengthBits, 24);
			const int wb_nLenBits = wBit;

			// Entity payload (nLengthBits bits, verbatim copy from S3).
			const int64_t rb_payloadStart = S21BR_GetBitsRead(s21buf);
			if (sbArm)
				QueryPerformanceCounter(&sbQBlit0);
			Snap_CopyPayload(s_snapBuf, s_snapBufCap, wBit, s21buf, nLengthBits);
			if (sbArm)
				QueryPerformanceCounter(&sbQBlit1);
			const int64_t rb_payloadEnd = S21BR_GetBitsRead(s21buf);
			const int wb_payload = wBit;

			const LONG snapN = InterlockedIncrement(&s_snapReencodeTotal);
			if (isDelta) InterlockedIncrement(&s_snapReencodeDelta);
			else         InterlockedIncrement(&s_snapReencodeFull);

			static int s_snapLog = 0;
			if (++s_snapLog <= 20 || (s_snapLog % 500) == 0)
				SDK_Log("[BRIDGE-PM] svc_Snapshot #%ld re-encode: "
					"S3 hdr=%lld S21 hdr=%lld delta=%u rawDelta=%u nLen=%u "
					"serverCount=%u tick=%u connFlag=%u "
					"(total=%ld full=%ld delta=%ld)\n",
					(long)snapN,
					(long long)(afterHeader - snapStartBit),
					(long long)s21HeaderBits, isDelta, rawDeltaPresent,
					nLengthBits, serverCount, sti_tick1, sti_connFlag,
					(long)s_snapReencodeTotal, (long)s_snapReencodeFull, (long)s_snapReencodeDelta);

			// === PER-STAGE BIT-WALK INSTRUMENTATION ===
			// Log read and write bit counts at every stage so any off-by-N
			if (Bridge_DiagFirehoseEnabled() && (isDelta || nLengthBits > 0))
			{
				const int64_t r_serverCount = rb_serverCount - snapStartBit;       // expect 32
				const int64_t r_sti         = rb_sti - rb_serverCount;             // expect 137
				const int64_t r_maxEntDelta = rb_delta - rb_sti;                    // expect 16 + deltaBits
				const int64_t r_shared      = rb_shared - rb_delta;                 // expect 112
				const int64_t r_nLenBits    = rb_nLenBits - rb_shared;              // expect 22
				const int64_t r_payload     = rb_payloadEnd - rb_payloadStart;      // expect nLen
				const int     w_serverCount = wb_serverCount - 0;                  // expect 32
				const int     w_sti         = wb_sti - wb_serverCount;             // expect 152
				const int     w_maxEntDelta = wb_delta - wb_sti;                   // expect 16 + deltaBits
				const int     w_shared      = wb_shared - wb_delta;                // expect 112
				const int     w_nLenBits    = wb_nLenBits - wb_shared;             // expect 24
				const int     w_payload     = wb_payload - wb_nLenBits;            // expect nLen

				const int     deltaExp      = isDelta ? (1 + 32 + 1 + (rawDeltaPresent ? 32 : 0)) : 1;
				const int     maxEntDeltaExp= 15 + deltaExp;
				const int64_t r_total       = rb_payloadEnd - snapStartBit;
				const int     w_total       = wb_payload;
				const int     w_total_exp   = 32 + 152 + 15 + deltaExp + 112 + 24 + (int)nLengthBits;
				const int64_t r_total_exp   = 32 + 137 + 15 + deltaExp + 112 + 22 + (int64_t)nLengthBits;

				if (Bridge_DiagFirehoseEnabled())
				{
					SDK_Log("[SNAP-WALK] #%ld delta=%u rawDelta=%u nLen=%u\n",
						(long)snapN, isDelta, rawDeltaPresent, nLengthBits);
					SDK_Log("[SNAP-WALK]   read  : srv=%lld(32) sti=%lld(137) maxDel=%lld(%d) shr=%lld(112) nLen=%lld(22) pay=%lld(%u)\n",
						(long long)r_serverCount, (long long)r_sti,
						(long long)r_maxEntDelta, maxEntDeltaExp,
						(long long)r_shared, (long long)r_nLenBits,
						(long long)r_payload, nLengthBits);
					SDK_Log("[SNAP-WALK]   write : srv=%d(32) sti=%d(152) maxDel=%d(%d) shr=%d(112) nLen=%d(24) pay=%d(%u)\n",
						w_serverCount, w_sti, w_maxEntDelta, maxEntDeltaExp,
						w_shared, w_nLenBits, w_payload, nLengthBits);
					SDK_Log("[SNAP-WALK]   total : read=%lld/%lld write=%d/%d  diff=%lld(17)\n",
						(long long)r_total, (long long)r_total_exp,
						w_total, w_total_exp, (long long)(w_total - r_total));

					// === BUFFER READ-BACK VALIDATION ===
					// Re-parse the s_snapBuf we just wrote and check key fields
					// match what we put in. Catches writeBits<->bit-position bugs.
					{
						bf_read vr(s_snapBuf, s21TotalBytes, (int)s21TotalBits);
						if (vr.Seek(0))
						{
							const uint32_t v_serverCount = vr.ReadUBitLong(32);
							const uint32_t v_tick1       = vr.ReadUBitLong(32);
							const uint32_t v_tick2       = vr.ReadUBitLong(32);
							const uint32_t v_compute     = vr.ReadUBitLong(16);
							const uint32_t v_stddev      = vr.ReadUBitLong(16);
							const uint32_t v_connFlag16  = vr.ReadUBitLong(16);
							const uint32_t v_byte        = vr.ReadUBitLong(8);
							const uint32_t v_cmdRun      = vr.ReadUBitLong(32);
							const uint32_t v_maxEnt      = vr.ReadUBitLong(15);
							const uint32_t v_isDelta     = vr.ReadUBitLong(1);
							uint32_t v_nDelta = 0, v_rawDP = 0, v_nRawDelta = 0;
							if (v_isDelta) {
								v_nDelta = vr.ReadUBitLong(32);
								v_rawDP  = vr.ReadUBitLong(1);
								if (v_rawDP) v_nRawDelta = vr.ReadUBitLong(32);
							}
							// Skip 112 shared
							for (int i = 0; i < 112; i++) (void)vr.ReadUBitLong(1);
							const uint32_t v_nLenBits = vr.ReadUBitLong(24);
							const int v_postHeaderBits = (int)vr.GetNumBitsRead();
							const int v_expHeaderBits  = (int)s21HeaderBits;

							const bool match =
								v_serverCount == serverCount &&
								v_tick1       == sti_tick1   &&
								v_tick2       == sti_tick2   &&
								v_compute     == sti_compute &&
								v_stddev      == sti_stddev  &&
								v_connFlag16  == sti_connFlag &&  // widened from 1-bit
								v_byte        == sti_byte    &&
								v_cmdRun      == sti_cmdRun  &&
								v_maxEnt      == nMaxEntries &&
								v_isDelta     == isDelta     &&
								(!isDelta || (v_nDelta == nDeltaFrom && v_rawDP == rawDeltaPresent &&
									(!rawDeltaPresent || v_nRawDelta == nRawDeltaFrom))) &&
								v_nLenBits    == nLengthBits &&
								v_postHeaderBits == v_expHeaderBits;
							SDK_Log("[SNAP-WALK]   verify: match=%d hdrBitsReadback=%d/%d "
								"srvCnt=%u/%u tick=%u/%u connF=%u/%u maxEnt=%u/%u "
								"isDelta=%u/%u nDelta=%u/%u nLen=%u/%u\n",
								match ? 1 : 0, v_postHeaderBits, v_expHeaderBits,
								v_serverCount, serverCount,
								v_tick1, sti_tick1,
								v_connFlag16, sti_connFlag,
								v_maxEnt, nMaxEntries,
								v_isDelta, isDelta,
								v_nDelta, nDeltaFrom,
								v_nLenBits, nLengthBits);
						}
					}
				}
			}

			// === ENTITY-PAYLOAD verified (first 8 deltas only) ===
			// Logs m_nHeaderCount (reconstructed from sharedBits[98..111]),
			if (Bridge_DiagFirehoseEnabled() && isDelta && nLengthBits > 0 && s_snapReencodeDelta <= 8)
			{
				// Reconstruct m_nHeaderCount from the 14-bit field at
				// sharedBits[98..111]. The wire order matches both S3
				const uint32_t hdrCount = (sh3 >> 2) & 0x3FFFu;

				// Parse the entity payload bits using a fresh SDK bf_read.
				// CL_ParseDeltaHeader wire format
				bf_read pr(s_snapBuf, s21TotalBytes, (int)s21TotalBits);
				if (pr.Seek((int)s21HeaderBits))
				{
					// ReadEntityIndex: 6-bit prefix, then maybe more bits
					uint32_t v6 = pr.ReadUBitLong(6);
					uint32_t prefix = v6 & 0x30;
					uint32_t entIdx = 0;
					int      idxBits = 6;
					if (prefix == 0)
						entIdx = v6;
					else if (prefix == 16)
					{
						uint32_t extra = pr.ReadUBitLong(4);
						entIdx = (v6 & 0xF) | (extra << 4);
						idxBits = 10;
					}
					else if (prefix == 32)
					{
						uint32_t extra = pr.ReadUBitLong(8);
						entIdx = (v6 & 0xF) | (extra << 4);
						idxBits = 14;
					}
					else
					{
						uint32_t extra = pr.ReadUBitLong(28);
						entIdx = (v6 & 0xF) | (extra << 4);
						idxBits = 34;
					}
					uint32_t flagA = pr.ReadUBitLong(1);
					uint32_t flagB = pr.ReadUBitLong(1);

					// Hex dump of first 32 bytes of entity payload.
					// Compute byte/bit offset within s_snapBuf where the
					// payload starts.
					int64_t payByteOff = s21HeaderBits / 8;
					int64_t payBitOff  = s21HeaderBits % 8;
					char hexBuf[100];
					int hexLen = 0;
					int dumpBytes = (int)nLengthBits / 8 + 1;
					if (dumpBytes > 32) dumpBytes = 32;
					for (int b = 0; b < dumpBytes && hexLen < (int)sizeof(hexBuf) - 4; b++)
					{
						int64_t idx = payByteOff + b;
						if (idx < s21TotalBytes)
							hexLen += snprintf(hexBuf + hexLen,
								sizeof(hexBuf) - hexLen,
								"%02X ", s_snapBuf[idx]);
					}
					if (hexLen > 0) hexBuf[hexLen - 1] = 0;

					SDK_Log("[ENT-PAYLOAD] #%ld(delta) nLen=%u hdrCount=%u "
						"firstIdx=%u(bits=%d v6=0x%02X prefix=0x%X) "
						"flagA=%u flagB=%u payOff=%lld+%lldbits payBytes[0..%d]=%s\n",
						(long)s_snapReencodeDelta, nLengthBits, hdrCount,
						entIdx, idxBits, v6, prefix, flagA, flagB,
						(long long)payByteOff, (long long)payBitOff,
						dumpBytes - 1, hexBuf);
				}
			}

			// Build S21 bf_read over the re-encoded buffer
			alignas(16) uint8_t snapS21Buf[S21BR_SIZE];
			s_S21BfReadInit(snapS21Buf, s_snapBuf, (uint64_t)s21TotalBytes);

			// S21 client vtable at is BROKEN: the linker
			// inserted an extra RTTI slot at [0], shifting every entry +1.
			{
				typedef bool (__fastcall *SnapRFB_fn)(void*, void*);
				typedef __int64 (__fastcall *SnapProc_fn)(void*);
				SnapRFB_fn snapRFB = (SnapRFB_fn)
					NetObs_Sym(NetObsSym_t::SvcSnapshotReadFromBuffer);
				SnapProc_fn snapProc = (SnapProc_fn)
					NetObs_Sym(NetObsSym_t::SnapshotProcess);

				// [SNAP-BUDGET] arm per-hook TSC stamps for this sampled snapshot only.
				if (sbArm)
				{
					SnapBudget_Reset();
					g_snapBudgetOn = true;
					QueryPerformanceCounter(&sbQ0);
					sbT0 = __rdtsc();
				}

		// Signon-table gate at point of use: the pre-scan cannot prove every
		// table was walked under interleave, so each table is structurally
		// validated here before the native RFB parses it.
		if (s3cmd == 8)
		{
			const int64_t gateTotal = (int64_t)s3TotalBytes * 8;
			const uint32_t gateLen = S21Bridge_Skip_PeekUBits(baseData, (int)gateTotal, (int)(preReadBits + 1), 32);
			const int64_t gateStart = preReadBits + 33;
			const int64_t gateEnd = gateStart + (int64_t)gateLen;
			if (preReadBits < 0 || gateStart >= gateTotal || gateEnd > gateTotal
				|| !S21Bridge_ValidateSendTableBody(baseData, (int)gateTotal, (int)gateStart, (int)gateEnd, "PM"))
			{
				s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
				S21Bridge_RequestValidatorDisconnect("PM-TABLE");
				return true;
			}
		}

		bool readOk = false;
				if (bridge_decode_seh.GetBool())
				{
					__try { readOk = snapRFB(netMsg, snapS21Buf); }
					__except(EXCEPTION_EXECUTE_HANDLER)
					{
						const LONG n = InterlockedIncrement(&s_snapRfbCrash);
						if (n <= 5)
							Warning(eDLL_T::ENGINE, "[BRIDGE-PM] svc_Snapshot ReadFromBuffer crashed (#%ld)\n", (long)n);
					}
				}
				else
					readOk = snapRFB(netMsg, snapS21Buf);

				if (sbArm)
					sbTMid = __rdtsc();

				if (readOk)
				{
					InterlockedIncrement(&s_snapRfbOk);
					bool procOk = false;
					if (bridge_decode_seh.GetBool())
					{
						__try { procOk = snapProc(netMsg) != 0; }
						__except(S21Bridge_CrashFilter(GetExceptionInformation()))
						{
							const LONG n = InterlockedIncrement(&s_snapProcCrash);
							if (n <= 5)
							{
								Warning(eDLL_T::ENGINE,
									"[BRIDGE-PM] svc_Snapshot Process crashed (#%ld): code=0x%08lX addr_va=0x%llX\n",
									(long)n, s_lastCrashCode, DeathObs_ImageVA(s_lastCrashAddr));
								BridgeTrace_Log(
									"[SEH-ADDR] svc_Snapshot Process crashed (#%ld): code=0x%08lX addr_va=0x%llX\n",
									(long)n, s_lastCrashCode, DeathObs_ImageVA(s_lastCrashAddr));
								BridgeTrace_Flush();
							}
						}
					}
					else
						procOk = snapProc(netMsg) != 0;

					// CClientState: main ack +348/+352, worker ack +356/+360, workerIsNew +364.
					{
						static long s_s2cSnapN = 0;
						const long sn = InterlockedIncrement(&s_s2cSnapN);
						if (bridge_net_flow_diag.GetBool() && (sn <= 60 || (sn % 100) == 0))
						{
							const uintptr_t cs = Bridge_ClientStatePtr();
							Warning(eDLL_T::ENGINE,
								"[S2C-SNAP] #%ld isDelta=%u tick=%u from=%u procOk=%d main{%d %d} worker{%d %d new=%d}\n",
								sn, isDelta, sti_tick1, nRawDeltaFrom, procOk ? 1 : 0,
								cs ? *reinterpret_cast<const int*>(cs + 348) : -2,
								cs ? *reinterpret_cast<const int*>(cs + 352) : -2,
								cs ? *reinterpret_cast<const int*>(cs + 356) : -2,
								cs ? *reinterpret_cast<const int*>(cs + 360) : -2,
								cs ? *reinterpret_cast<const uint8_t*>(cs + 364) : -2);
						}
					}

					if (procOk)
					{
						InterlockedIncrement(&s_snapProcOk);
						S21Bridge_S2CScriptRemote_OnSnapshotAccepted(sti_tick1);
						S21Bridge_LerpDepth_OnSnapshotAccepted(sti_tick1);
					}
					else
					{
						const LONG n = InterlockedIncrement(&s_snapProcFail);
						if (n <= 10)
							Warning(eDLL_T::ENGINE,
								"[BRIDGE-PM] svc_Snapshot Process returned false (#%ld) "
								"isDelta=%u nLen=%u tick=%u\n",
								(long)n, isDelta, nLengthBits, sti_tick1);
					}
				}
				else
				{
					const LONG n = InterlockedIncrement(&s_snapRfbFail);
					if (n <= 10)
						Warning(eDLL_T::ENGINE,
							"[BRIDGE-PM] svc_Snapshot ReadFromBuffer failed (#%ld) "
							"isDelta=%u nLen=%u tick=%u serverCount=%u\n",
							(long)n, isDelta, nLengthBits, sti_tick1, serverCount);
				}

				// Always clear the arm flag: covers readOk false, RFB crash, and Process crash.
				if (sbArm)
				{
					sbT1 = __rdtsc();
					QueryPerformanceCounter(&sbQ1);
					g_snapBudgetOn = false;

					const double windowMs = BridgeBudget_Ms(sbQ0.QuadPart, sbQ1.QuadPart, sbFreq.QuadPart);
					const double prepMs = BridgeBudget_Ms(sbQPrep0.QuadPart, sbQBlit0.QuadPart, sbFreq.QuadPart);
					const double blitMs = BridgeBudget_Ms(sbQBlit0.QuadPart, sbQBlit1.QuadPart, sbFreq.QuadPart);
					const double totalMs = BridgeBudget_Ms(sbQPrep0.QuadPart, sbQ1.QuadPart, sbFreq.QuadPart);
					const unsigned long long tscWin = (sbT1 > sbT0) ? (sbT1 - sbT0) : 0ULL;
					const double msPerTsc = (tscWin > 0) ? (windowMs / (double)tscWin) : 0.0;
					const double rfbMs = (sbTMid > sbT0)
						? ((double)(sbTMid - sbT0) * msPerTsc) : 0.0;
					const double procMs = (sbT1 > sbTMid)
						? ((double)(sbT1 - sbTMid) * msPerTsc) : 0.0;

					// Exclusive = total - orig (what the bridge itself costs).
					const long long cneEx  = g_snapBudget[SNAPB_COPYNEWENT].m_nTotalTicks  - g_snapBudget[SNAPB_COPYNEWENT].m_nOrigTicks;
					const long long rtdmEx = g_snapBudget[SNAPB_RTDECODEMAIN].m_nTotalTicks - g_snapBudget[SNAPB_RTDECODEMAIN].m_nOrigTicks;
					const long long rtdEx  = g_snapBudget[SNAPB_RTDECODE].m_nTotalTicks     - g_snapBudget[SNAPB_RTDECODE].m_nOrigTicks;
					const long long gdEx   = g_snapBudget[SNAPB_GENDELTAS].m_nTotalTicks    - g_snapBudget[SNAPB_GENDELTAS].m_nOrigTicks;
					const long long dispEx = g_snapBudget[SNAPB_DISPATCH].m_nTotalTicks     - g_snapBudget[SNAPB_DISPATCH].m_nOrigTicks;
					const double cneMs  = (double)((cneEx  > 0) ? cneEx  : 0) * msPerTsc;
					const double rtdmMs = (double)((rtdmEx > 0) ? rtdmEx : 0) * msPerTsc;
					const double rtdMs  = (double)((rtdEx  > 0) ? rtdEx  : 0) * msPerTsc;
					const double gdMs   = (double)((gdEx   > 0) ? gdEx   : 0) * msPerTsc;
					const double dispMs = (double)((dispEx > 0) ? dispEx : 0) * msPerTsc;
					const double hookTotal = cneMs + rtdmMs + rtdMs + gdMs + dispMs;

					Warning(eDLL_T::CLIENT,
						"[SNAP-BUDGET] #%ld bytes=%lld bits=%u delta=%u "
						"prep=%.2f blit=%.2f rfb=%.2f proc=%.2f total=%.2f | "
						"cne=%.2f/%ld rtdm=%.2f/%ld rtd=%.2f/%ld gd=%.2f/%ld disp=%.2f/%ld | "
						"hookTotal=%.2fms vq=%ld seh=%ld\n",
						sbN,
						(long long)s21TotalBytes,
						(unsigned)nLengthBits,
						(unsigned)isDelta,
						prepMs, blitMs, rfbMs, procMs, totalMs,
						cneMs,  (long)g_snapBudget[SNAPB_COPYNEWENT].m_nCalls,
						rtdmMs, (long)g_snapBudget[SNAPB_RTDECODEMAIN].m_nCalls,
						rtdMs,  (long)g_snapBudget[SNAPB_RTDECODE].m_nCalls,
						gdMs,   (long)g_snapBudget[SNAPB_GENDELTAS].m_nCalls,
						dispMs, (long)g_snapBudget[SNAPB_DISPATCH].m_nCalls,
						hookTotal,
						(long)g_dispVqCalls,
						(long)g_dispSehHits);
				}

			}
			continue;
		}
		else
		{
			//=============================================================
			// Generic S21 vtable path for messages with compatible formats
			//=============================================================

			void* rfbBuf = s21buf;
			alignas(16) uint8_t setcvarS21[S21BR_SIZE];
			static uint8_t s_setCvarScratch[20480];

			if (s3cmd == 4)
			{
				const uint32_t pairCount = S21BR_ReadUBits(s21buf, 8);
				if (S21BR_IsOverflowed(s21buf))
				{
					s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
					return true;
				}

				static char s_cvNames[255][256];
				static char s_cvValues[255][256];
				int kept = 0;
				const uint32_t walkCount = pairCount;
				bool bodyOk = true;

				if (pairCount > 255)
					Warning(eDLL_T::ENGINE, "[BRIDGE-SETCVAR] pairCount %u over cap -- drop\n", pairCount);

				for (uint32_t i = 0; i < walkCount; ++i)
				{
					char name[256];
					char value[256];
					if (!Bridge_ReadS2CString(s21buf, name, 256)
						|| !Bridge_ReadS2CString(s21buf, value, 256)
						|| S21BR_IsOverflowed(s21buf))
					{
						bodyOk = false;
						break;
					}

					if (pairCount > 255)
						continue;

					if (!S21Bridge_SetCVarNameAccepted(name))
					{
						S21Bridge_LogDroppedSetCVar(name);
						continue;
					}
					if (kept < 255)
					{
						V_strncpy(s_cvNames[kept], name, sizeof(s_cvNames[kept]));
						V_strncpy(s_cvValues[kept], value, sizeof(s_cvValues[kept]));
						++kept;
					}
				}

				if (!bodyOk)
				{
					s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
					return true;
				}

				if (pairCount > 255 || kept == 0)
					continue;

				// Worst case is 255 pairs of full-length strings; bound the re-encode so hostile input drops loudly instead of running past the scratch.
				int needBytes = 1;
				for (int i = 0; i < kept; ++i)
					needBytes += (int)strlen(s_cvNames[i]) + 1 + (int)strlen(s_cvValues[i]) + 1;
				if (needBytes > (int)sizeof(s_setCvarScratch))
				{
					Warning(eDLL_T::ENGINE, "[BRIDGE-SETCVAR] %d pairs need %d bytes, scratch %d -- drop\n", kept, needBytes, (int)sizeof(s_setCvarScratch));
					s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
					return true;
				}

				memset(s_setCvarScratch, 0, sizeof(s_setCvarScratch));
				int wBit = 0;
				auto writeBits = [&](uint64_t val, int nBits) {
					for (int b = 0; b < nBits; b++)
					{
						if ((val >> b) & 1)
							s_setCvarScratch[wBit / 8] |= (1 << (wBit % 8));
						wBit++;
					}
				};
				auto writeString = [&](const char* s, int maxLen) {
					for (int i = 0; i < maxLen; i++)
					{
						writeBits((uint8_t)s[i], 8);
						if (s[i] == 0)
							break;
					}
				};
				writeBits(static_cast<uint32_t>(kept), 8);
				for (int i = 0; i < kept; ++i)
				{
					writeString(s_cvNames[i], 256);
					writeString(s_cvValues[i], 256);
				}

				if (!s_S21BfReadInit)
				{
					s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
					return true;
				}
				s_S21BfReadInit(setcvarS21, s_setCvarScratch, (uint64_t)((wBit + 7) / 8));
				rfbBuf = setcvarS21;
			}

			// S21: vtable[8] = SetNetChannel/Init
			__try
			{
				reinterpret_cast<void(*)(void*)>(msgVtbl[8])(netMsg);
			}
			__except(EXCEPTION_EXECUTE_HANDLER)
			{
				Warning(eDLL_T::ENGINE, "S21Bridge_ProcessMessages: crash in vtable[8] for S21 msg %d\n", s21cmd);
				s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
				return true;
			}

			// S21: vtable[4] = ReadFromBuffer(msg, s21_bf_read*)
			if (s3cmd == 9)
				SDK_Log("[BRIDGE-PM] ClassInfo (S3:9->S21:13): ReadFromBuffer start bitsRead=%lld bitsLeft=%lld\n",
					(long long)preReadBits, (long long)S21BR_GetBitsLeft(s21buf));

			bool readOk = false;
			__try
			{
				readOk = reinterpret_cast<bool(*)(void*, void*)>(msgVtbl[4])(netMsg, rfbBuf);
			}
			__except(EXCEPTION_EXECUTE_HANDLER)
			{
				Warning(eDLL_T::ENGINE,
					"S21Bridge_ProcessMessages: crash in ReadFromBuffer for S3 msg %d (%s) -> S21 msg %d\n",
					s3cmd, S21Bridge_S3TypeName(s3cmd), s21cmd);
				s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
				return true;
			}

			{
				const int64_t postReadBits = S21BR_GetBitsRead(s21buf);
				static long long s_diagLog = 0;
				if (++s_diagLog <= 30)
					SDK_Log("[BRIDGE-PM] DIAG S3:%d->S21:%d readOk=%d bitsConsumed=%lld\n",
						s3cmd, s21cmd, readOk ? 1 : 0, (long long)(postReadBits - preReadBits));
			}

			if (!readOk)
			{
				static int s_failHist[128] = {};
				if (s21cmd >= 0 && s21cmd < 128 && s_failHist[s21cmd]++ < 3)
				{
					const int64_t failBits = S21BR_GetBitsRead(s21buf);
					Warning(eDLL_T::ENGINE,
						"S21Bridge_ProcessMessages: ReadFromBuffer FAILED for S3 msg %d (%s) -> S21 msg %d, consumed=%lld bits, left=%lld\n",
						s3cmd, S21Bridge_S3TypeName(s3cmd), s21cmd,
						(long long)(failBits - preReadBits),
						(long long)S21BR_GetBitsLeft(s21buf));
					PmTrail_Dump();
				}
				s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
				return true;
			}

			if ((s3cmd == 25 || s3cmd == 26) && !s_bridgePdefReady)
			{
				Warning(eDLL_T::ENGINE,
					"[BRIDGE-PM] Persistence %s dropped -- pdef not parsed\n",
					s3cmd == 25 ? "Baseline" : "UpdateVar");
				continue;
			}

			// SVC_ClassInfo (S3:9 -> S21:13): Process now runs properly.
			// Decoder builder lenient patch skips unmatched props.
			if (s3cmd == 9)
			{
				SDK_Log("[BRIDGE-PM] ClassInfo (S3:9->S21:13): Processing (decoders + entity init)\n");
				s_dumpClassInventoryAfterClassInfo = true;
				S21Bridge_CIDiag_Gate("ClassInfo_Process_ENTER");
			}

			// S21: vtable[3] = Process
			bool processOk = false;
			__try
			{
				processOk = reinterpret_cast<bool(*)(void*)>(msgVtbl[3])(netMsg);
			}
			__except (s3cmd == 9
				? (S21Bridge_CIDiag_WriteEmergencyDump("ClassInfo_Process", GetExceptionInformation()),
					EXCEPTION_EXECUTE_HANDLER)
				: EXCEPTION_EXECUTE_HANDLER)
			{
				Warning(eDLL_T::ENGINE, "S21Bridge_ProcessMessages: crash in Process for S21 msg %d\n", s21cmd);
				if (s3cmd == 9)
				{
					SDK_Log("[CI-GATE] ClassInfo_Process_SEH processOk=0 (exception)\n");
					S21Bridge_CIDiag_Flush();
				}
				s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
				return true;
			}

			if (s3cmd == 9)
			{
				GenerateDeltas_ClearFilled();
				GenerateDeltas_RecycleDummyPool();
				SDK_Log("[CI-GATE] ClassInfo_Process_EXIT processOk=%d\n", processOk ? 1 : 0);
				S21Bridge_CIDiag_Gate("ClassInfo_Process_EXIT");
			}

			if (!processOk)
			{
				// Log the failure with context
				uintptr_t eb = S21_GetExeBase(); (void)eb;
				void* handler = *reinterpret_cast<void**>((uintptr_t)netMsg + 24);
				int sigState = handler ? *reinterpret_cast<int*>((uintptr_t)handler + 156) : -1;
				void* strTblContainer = handler ? *reinterpret_cast<void**>((uintptr_t)handler + 1096) : nullptr;
				int expectedBits = *reinterpret_cast<int*>((uintptr_t)netMsg + 40);

				static int s_procFail = 0;
				if (++s_procFail <= 10)
					SDK_Log("[BRIDGE-PM] Process FAILED for S21:%d (s3:%d) sigState=%d strTbl=%p expectedBits=%d\n",
						s21cmd, s3cmd, sigState, strTblContainer, expectedBits);

				// Don't abort -- continue to next message. CreateStringTable Process
				// can fail if string table container isn't ready yet. The data is
				// still consumed by ReadFromBuffer, so the bitstream stays aligned.
				continue;
			}
			// After ClassInfo processes: SEH-safe inventory walk + stub ClientClass patch.
			// District same-map reload was dying mid unguarded [CLASS-MAP] printf.
			if (s_dumpClassInventoryAfterClassInfo && s3cmd == 9 && processOk)
			{
				s_dumpClassInventoryAfterClassInfo = false;
				S21Bridge_CIDiag_WalkAndStubPatch();
				// Next wire message is normally SignonState(4) (+ deferred-3
				// replay on a healthy first join). Stamp so a silent death
				// after END still leaves a post-walk breadcrumb.
				SDK_Log("[CI-WALK] POST-PATCH resume PM bitsLeft=%lld deferred3=%d sawST=%d\n",
					(long long)S21BR_GetBitsLeft(s21buf),
					s_deferredSignon3 ? 1 : 0,
					s_dbSawSendTables ? 1 : 0);
				S21Bridge_CIDiag_Flush();
			}

		} // end generic vtable path
	}

	// Post-loop: log final state
	{
		int64_t finalBits = S21BR_GetBitsRead(s21buf);
		int64_t finalLeft = S21BR_GetBitsLeft(s21buf);
		SDK_Log("[BRIDGE-PM] LOOP EXIT: bitsRead=%lld bitsLeft=%lld overflow=%d\n",
			(long long)finalBits, (long long)finalLeft, S21BR_IsOverflowed(s21buf) ? 1 : 0);
		// If there are remaining bits, log the first 32 for analysis
		if (finalLeft >= 7)
		{
			uint32_t peek = 0;
			int peekBits = (finalLeft > 32) ? 32 : (int)finalLeft;
			for (int i = 0; i < peekBits; i++)
			{
				uint32_t b = S21BR_ReadUBits(s21buf, 1);
				peek |= (b << i);
			}
			int nextType = peek & 0x7F;
			SDK_Log("[BRIDGE-PM] LOOP EXIT PEEK: next7bits=type%d raw32=0x%08X remaining_after=%lld\n",
				nextType, peek, (long long)S21BR_GetBitsLeft(s21buf));
		}
	}

	// Sync final position back to S3 bf_read
	s3buf->Seek((int)S21BR_GetBitsRead(s21buf));
	return true;
}

//-----------------------------------------------------------------------------
// [SUBCHAN-DESYNC] one-shot context. ProcessPacket fills these in right before
// calling the subchannel parser; the parser dumps them when it reads a bad
//-----------------------------------------------------------------------------
static int            s_dsyncSeq = 0, s_dsyncAck = 0, s_dsyncFlags = 0, s_dsyncChoked = 0;
static int            s_dsyncNoncePresent = 0, s_dsyncBitsBeforeNonce = 0, s_dsyncSubBitPos = 0;
static uint32_t       s_dsyncNonceMagic = 0;
static const uint8_t* s_dsyncRaw = nullptr;
static int            s_dsyncPktSize = 0;

//-----------------------------------------------------------------------------
// S3 subchannel fragment parser for reliable data reassembly.
//-----------------------------------------------------------------------------
static void S21Bridge_AckSubchanEntry(const uint32_t entrySeq)
{
	const uint32_t ack = entrySeq + 1u;
	if (ack > s_serverSubSeqRecv)
		s_serverSubSeqRecv = ack;
}

static void S21Bridge_DispatchReliableTransfer(CNetChan* pChan)
{
	if (!s_bridgeReliable.active || s_bridgeReliable.receivedSize < s_bridgeReliable.totalSize)
		return;

	const uint32_t doneSeq = s_bridgeReliable.entrySeq;
	if (S21Bridge_RewalkTrace())
		Warning(eDLL_T::ENGINE,
			"[BRIDGE-REL] re-walk: dispatching entry_seq=%u (%d bytes, compressed=%d)\n",
			doneSeq, s_bridgeReliable.totalSize, s_bridgeReliable.isCompressed ? 1 : 0);

	SDK_Log("[BRIDGE-REL] reliable transfer complete (%d bytes), processing messages\n",
		s_bridgeReliable.totalSize);

	if (s_bridgeReliable.isCompressed)
	{
		const int decompSize = s_bridgeReliable.uncompressedSize;
		if (decompSize > 0 && decompSize < 0x200000)
		{
			uint8_t* decompBuf = (uint8_t*)malloc(decompSize + 16);
			if (decompBuf)
			{
				size_t sourceLen = s_bridgeReliable.totalSize;
				unsigned int result = NET_BufferToBufferDecompress(
					s_bridgeReliable.buffer, sourceLen,
					decompBuf, decompSize);

				if (result > 0)
				{
					SDK_Log("[BRIDGE-REL] decompressed %d -> %u bytes\n",
						s_bridgeReliable.totalSize, result);
					SDK_Log("[BRIDGE-REL] preprocess decompressed transfer (%u bytes)\n", result);
					if (!S21Bridge_PreprocessSendTablesInBuffer(decompBuf, (int)result, "REL"))
					{
						S21Bridge_RequestValidatorDisconnect("REL");
					}
					else
					{
						SDK_Log("[BRIDGE-REL] ProcessMessages decompressed transfer next\n");
						bf_read reliableBuf(decompBuf, result);
						S21Bridge_ProcessMessages(pChan, &reliableBuf);
					}
				}
				else
				{
					Warning(eDLL_T::ENGINE, "S21Bridge: LZSS decompression failed (src=%d, expected=%d)\n",
						s_bridgeReliable.totalSize, decompSize);
				}
				free(decompBuf);
			}
		}
		else
		{
			Warning(eDLL_T::ENGINE, "S21Bridge: invalid decompressed size %d\n", decompSize);
		}
	}
	else
	{
		SDK_Log("[BRIDGE-REL] preprocess raw transfer (%d bytes)\n",
			s_bridgeReliable.totalSize);
		if (!S21Bridge_PreprocessSendTablesInBuffer(
			s_bridgeReliable.buffer, s_bridgeReliable.totalSize, "REL"))
		{
			S21Bridge_RequestValidatorDisconnect("REL");
		}
		else
		{
			SDK_Log("[BRIDGE-REL] ProcessMessages raw transfer next\n");
			bf_read reliableBuf(s_bridgeReliable.buffer, s_bridgeReliable.totalSize);
			S21Bridge_ProcessMessages(pChan, &reliableBuf);
		}
	}

	s_bridgeReliable.Reset();
}

static bool S21Bridge_ParseSubChannelData(CNetChan* pChan, bf_read& buf)
{
	// -verified format from S3 WriteSubChannelData
	// and ReadSubChannelData.

	const uint32_t subMagic = buf.ReadUBitLong(32);
	// S3 reader reads and discards magic -- never checks.
	// Log unexpected values for diagnostics but do NOT fail.
	if (subMagic != 0xABCDEF01)
	{
		// S3 reader reads but never checks magic, so the dedi
		// can write any value here. Log rare divergences only.
		static long long s_badSub = 0;
		if (++s_badSub <= 5 || (s_badSub % 1000) == 0)
			Warning(eDLL_T::ENGINE, "S21Bridge: subchannel magic 0x%08X (expected 0xABCDEF01) #%lld\n",
				subMagic, s_badSub);

		// One-shot dump when the cursor lands on a bad subchannel magic.
		static long long s_dsyncDump = 0;
		if (++s_dsyncDump <= 8)
		{
			const int nonceBits = s_dsyncSubBitPos - s_dsyncBitsBeforeNonce;
			Warning(eDLL_T::ENGINE,
				"[SUBCHAN-DESYNC] #%lld seq=%d ack=%d flags=0x%02X choked=%d "
				"noncePresent=%d nonceMagic=0x%08X nonceBits=%d subBitPos=%d "
				"(byte %d bit %d) bitsLeftAfterMagic=%d pktSize=%d "
				"trkServerSubSeq=%u ourSubSeq=%u\n",
				s_dsyncDump, s_dsyncSeq, s_dsyncAck, s_dsyncFlags, s_dsyncChoked,
				s_dsyncNoncePresent, s_dsyncNonceMagic, nonceBits, s_dsyncSubBitPos,
				s_dsyncSubBitPos / 8, s_dsyncSubBitPos % 8, buf.GetNumBitsLeft(),
				s_dsyncPktSize, s_serverSubSeqRecv, s_bridgeSubSeq);

			char hx[3 * 96 + 1] = {};
			const int nHex = (s_dsyncPktSize < 96) ? s_dsyncPktSize : 96;
			for (int i = 0; i < nHex; ++i)
				snprintf(hx + i * 3, sizeof(hx) - i * 3, "%02X ",
					s_dsyncRaw ? s_dsyncRaw[i] : 0);
			Warning(eDLL_T::ENGINE, "[SUBCHAN-DESYNC] #%lld raw[0..%d]: %s\n",
				s_dsyncDump, nHex, hx);
		}
	}

	const uint32_t subSeq = buf.ReadUBitLong(32);
	const uint32_t reqId = buf.ReadUBitLong(10);
	s_serverNonceCaptured = true;

	SDK_Log("[BRIDGE-REL] subchan header: magic=0x%08X seq=%u reqId=%u bitsLeft=%d\n",
		subMagic, subSeq, reqId, buf.GetNumBitsLeft());

	if (S21Bridge_RewalkTrace())
		Warning(eDLL_T::ENGINE,
			"[BRIDGE-REL] re-walk: subchan seq=%u reqId=%u floor=%u bitsLeft=%d\n",
			subSeq, reqId, s_serverSubSeqRecv, buf.GetNumBitsLeft());

	// SV_ActivateServer restarts the waiting list at entry 0 and the dedi then
	// retransmits it ~10x/s until we ack. Rebasing on every one of those copies
	if (subSeq == 0)
	{
		if (s_serverSubSeqRecv > 0 && !s_subSeqRebased)
			S21Bridge_ResetReliableRecv(s_signonRewalk ? "re-walk base-0" : "subSeq=0 list restart");
		s_subSeqRebased = true;
	}
	else
	{
		// The list moved on, so the next return to 0 is a genuine restart.
		s_subSeqRebased = false;
	}

	// req_id == 0: nonce exchange section (-verified from both Writer and Reader)
	// Format: [1-bit nonce_flag]
	if (reqId == 0)
	{
		if (subSeq != 0)
		{
			static int s_wrapLog = 0;
			if (++s_wrapLog <= 4)
				Warning(eDLL_T::ENGINE,
					"[BRIDGE-REL] subchan wrap subSeq=%u floor=%u\n",
					subSeq, s_serverSubSeqRecv);
		}
		const int nonceFlag = buf.ReadUBitLong(1);
		if (nonceFlag)
		{
			const uint32_t serverNonceHost = buf.ReadUBitLong(32);
			if (serverNonceHost != 0)
			{
				// Peer re-created its netchan (Clear): base seq restarts at 0
				// and a fresh nonce is published. Drop our receive floor so
				// entry_seq=0 is accepted again.
				if (s_serverNonce != 0 && serverNonceHost != s_serverNonce)
				{
					Warning(eDLL_T::ENGINE,
						"[BRIDGE-REL] peer re-created netchan: nonce 0x%08X -> 0x%08X, "
						"dropped subseq floor %u\n",
						s_serverNonce, serverNonceHost, s_serverSubSeqRecv);
					s_serverSubSeqRecv = 0;
					s_subSeqRebased = true;
					s_bridgeReliable.Reset();
				}
				s_serverNonce = serverNonceHost;
				s_needNonceAck = true;
				s_serverNonceCaptured = true;
				static int s_nonceCapLog = 0;
				if (++s_nonceCapLog <= 5)
					SDK_Log("[BRIDGE-REL] captured server nonce: 0x%08X (subSeq=%u)\n",
						serverNonceHost, subSeq);
			}
		}
		// nonceFlag==0: writer wrote just 1 bit=0, nothing else. Do NOT read more.
	}

	// Per-entry loop (-verified from ReadSubChannelData).
	// Each entry is followed by a 1-bit "more" flag. When more==0, loop ends.
	while (!buf.IsOverflowed())
	{
		const uint32_t entrySeq = buf.ReadUBitLong(32);
		const int isFirstFrag = buf.ReadUBitLong(1);

		int chunkSize = 0;

		if (isFirstFrag)
		{
			const int expectedSize = buf.ReadUBitLong(19);
			if (expectedSize > 0x40000)
			{
				Warning(eDLL_T::ENGINE, "S21Bridge: subchannel xfer_size too large (%d)\n", expectedSize);
				return false;
			}

			// chunkSize = min(totalSize, 560) -- -verified from Reader
			chunkSize = (expectedSize > S3_MAX_FRAGMENT_PER_PACKET)
				? S3_MAX_FRAGMENT_PER_PACKET : expectedSize;

			const int isCompressed = buf.ReadUBitLong(1);
			int uncompSize = 0;
			if (isCompressed)
				uncompSize = buf.ReadUBitLong(22);

			// A stale floor is corrected once at the subchannel header, before any entry.
			if (entrySeq < s_serverSubSeqRecv)
			{
				// s_signonRewalk stays set until the walk reaches FULL, so a
				// stalled walk must not get an unbudgeted log line per packet.
				static long long s_dupLog = 0;
				if (S21Bridge_RewalkTrace() || ++s_dupLog <= 8 || (s_dupLog % 200) == 0)
					Warning(eDLL_T::ENGINE,
						"[BRIDGE-REL] drop dup entry_seq=%u size=%d ack=%u (#%lld)\n",
						entrySeq, expectedSize, s_serverSubSeqRecv, s_dupLog);
				if (chunkSize > 0)
					buf.SeekRelative(chunkSize * 8);
				const int hasMore = buf.ReadUBitLong(1);
				if (!hasMore) break;
				continue;
			}

			// Start new reliable transfer
			if (s_bridgeReliable.active && s_bridgeReliable.receivedSize < s_bridgeReliable.totalSize)
			{
				static long long s_abandoned = 0;
				if (++s_abandoned <= 5 || (s_abandoned % 500) == 0)
					Warning(eDLL_T::ENGINE,
						"S21Bridge: subchan transfer abandoned at %d/%d bytes (#%lld)\n",
						s_bridgeReliable.receivedSize, s_bridgeReliable.totalSize, s_abandoned);
			}
			s_bridgeReliable.Reset();
			s_bridgeReliable.active = true;
			s_bridgeReliable.totalSize = expectedSize;
			s_bridgeReliable.receivedSize = 0;
			s_bridgeReliable.isCompressed = (isCompressed != 0);
			s_bridgeReliable.uncompressedSize = uncompSize;
			s_bridgeReliable.entrySeq = entrySeq;
			const int need = expectedSize + 16;
			if (s_bridgeReliable.capacity < need)
			{
				if (s_bridgeReliable.buffer)
				{
					HeapCanary::Unregister(s_bridgeReliable.buffer);
					free(s_bridgeReliable.buffer);
					s_bridgeReliable.buffer = nullptr;
					s_bridgeReliable.capacity = 0;
				}
				// +kTailBytes always: slack must exist even while canary is unarmed.
				s_bridgeReliable.buffer = (uint8_t*)malloc(
					static_cast<size_t>(need) + HeapCanary::kTailBytes);
				if (s_bridgeReliable.buffer)
				{
					s_bridgeReliable.capacity = need;
					HeapCanary::RegisterTail("bridge-reliable",
						s_bridgeReliable.buffer,
						static_cast<size_t>(need));
				}
			}
			if (!s_bridgeReliable.buffer)
			{
				s_bridgeReliable.active = false;
				return false;
			}
			SDK_Log("[BRIDGE-REL] transfer start: seq=%u size=%d compressed=%d chunk=%d\n",
				entrySeq, expectedSize, isCompressed, chunkSize);
		}
		else
		{
			// Not first fragment: [1-bit is_last]
			const int isLast = buf.ReadUBitLong(1);
			if (isLast)
			{
				// [10-bit lastChunkSize] -- Reader uses this as the data size
				chunkSize = buf.ReadUBitLong(10);
			}
			else
			{
				// Middle fragment: always 560 bytes (-verified)
				chunkSize = S3_MAX_FRAGMENT_PER_PACKET;
			}
		}

		// Read fragment data into reassembly buffer
		if (chunkSize > S3_MAX_FRAGMENT_PER_PACKET)
		{
			Warning(eDLL_T::ENGINE, "S21Bridge: subchan chunk %d > max %d\n",
				chunkSize, S3_MAX_FRAGMENT_PER_PACKET);
			return false;
		}

		if (entrySeq < s_serverSubSeqRecv)
		{
			if (chunkSize > 0)
				buf.SeekRelative(chunkSize * 8);
		}
		else if (chunkSize > 0 && s_bridgeReliable.active && s_bridgeReliable.buffer)
		{
			if (s_bridgeReliable.receivedSize + chunkSize <= s_bridgeReliable.totalSize + 16)
			{
				buf.ReadBits(s_bridgeReliable.buffer + s_bridgeReliable.receivedSize, chunkSize * 8);
				s_bridgeReliable.receivedSize += chunkSize;
				S21Bridge_AckSubchanEntry(entrySeq);
			}
			else
			{
				buf.SeekRelative(chunkSize * 8);
				Warning(eDLL_T::ENGINE, "S21Bridge: subchan buffer overflow, skipping %d bytes\n", chunkSize);
			}

			static long long s_chunkLog = 0;
			if (++s_chunkLog <= 20 || (s_chunkLog % 100) == 0)
				SDK_Log("[BRIDGE-REL] chunk: %d bytes (total %d/%d)\n",
					chunkSize, s_bridgeReliable.receivedSize, s_bridgeReliable.totalSize);
		}
		else if (chunkSize > 0)
		{
			// Native fails a continuation with no recv buffer. Skip to keep
			// the unreliable body aligned. ACK uncounted seqs so leftovers pop.
			buf.SeekRelative(chunkSize * 8);
			S21Bridge_AckSubchanEntry(entrySeq);
			static long long s_skipNoActive = 0;
			if (++s_skipNoActive <= 5 || (s_skipNoActive % 500) == 0)
				Warning(eDLL_T::ENGINE,
					"S21Bridge: subchan data with no active transfer, skipping %d bytes "
					"entry_seq=%u ack=%u (#%lld)\n",
					chunkSize, entrySeq, s_serverSubSeqRecv, s_skipNoActive);
		}

		// Complete transfers must dispatch here: the next first-frag reuses the slot.
		S21Bridge_DispatchReliableTransfer(pChan);

		// [1-bit more_entries] -- read ONCE per entry, at the END (-verified)
		const int hasMore = buf.ReadUBitLong(1);
		if (!hasMore) break;
	}

	return !buf.IsOverflowed();
}

//-----------------------------------------------------------------------------
// CNetChan::ProcessPacket: parse S3 incoming, update CNetChan, process reliable + unreliable.
//-----------------------------------------------------------------------------
void S21Bridge_Hook_ProcessPacket(CNetChan* pChan, netpacket_s* pPacket)
{
	// [PKT-IDLE]/[PIPELINE] probe + HOOK-COST timer removed.
	if (!s_bridgeActive)
	{
		CNetChan__ProcessPacket(pChan, pPacket);
		return;
	}

	// Lifetime entry counter -- proves whether the hook keeps firing post-signon.
	// (See S21Bridge_PpPmHistogramDump for full breakdown.)
	const LONG ppN = InterlockedIncrement(&s_ppEntryCount);

	// PP-side histogram trigger -- catches the case where PM never fires
	// (subchan fail / empty unreliable body), so the PM-side trigger would
	// never run. Anchored at every 100 PP entries past the first 20.
	static volatile LONG s_ppLastHistDump = 0;
	if (ppN >= 20 && ppN - s_ppLastHistDump >= 100)
	{
		s_ppLastHistDump = ppN;
		S21Bridge_PpPmHistogramDump();
	}

	// The engine frees and reallocates its CNetChan across a disconnect, so the
	// pointer is refreshed from every packet rather than captured once.
	if (s_bridgeChan != pChan)
	{
		const bool firstCapture = (s_bridgeChan == nullptr);
		s_bridgeChan = pChan;
		SDK_Log("[BRIDGE-PP] captured CNetChan=%p\n", (void*)pChan);
		if (firstCapture)
			S21Bridge_DumpMsgArray(pChan);
	}

	const int pktSize = S21_PKT_Size(pPacket);
	if (!pPacket || pktSize <= 0)
		return;

	bf_read* pBuf = S21_PKT_Message(pPacket);
	pBuf->Seek(0);
	bf_read& buf = *pBuf;

	// === Parse S3 header: [32 seq][32 ack][8-bit flags] ===
	const int sequence = buf.ReadLong();
	const int sequenceAck = buf.ReadLong();
	const int flags = buf.ReadUBitLong(8);

	if (buf.IsOverflowed())
		return;

	const int ppLevel = BridgeBudget_Level();
	static long s_ppBudgetN = 0;
	const long ppBN = (ppLevel > 0) ? InterlockedIncrement(&s_ppBudgetN) : 0;
	const bool ppArm = BridgeBudget_Armed(ppLevel, ppBN);
	LARGE_INTEGER ppFreq = {}, ppQ0 = {}, ppQHdr = {}, ppQRel = {}, ppQPm = {}, ppQ1 = {};
	if (ppArm)
	{
		QueryPerformanceFrequency(&ppFreq);
		QueryPerformanceCounter(&ppQ0);
	}

	// Format gate: S3 nonce-magic check (0xFDBAC34D / S21 variant).

	// S3 choked count (flags bit 4 = 0x10)
	int choked = 0;
	if (flags & 0x10)
		choked = buf.ReadUBitLong(8);

	// Diagnostic: hex dump first 32 bytes + bit position before nonce section
	{
		static long long s_hexLog = 0;
		if (++s_hexLog <= 15)
		{
			const int bitsRead = (int)buf.GetNumBitsRead();
			const uint8_t* raw = reinterpret_cast<const uint8_t*>(buf.GetBasePointer());
			char hexBuf[128] = {};
			for (int i = 0; i < 32 && i < pktSize; ++i)
				snprintf(hexBuf + i * 3, sizeof(hexBuf) - i * 3, "%02X ", raw ? raw[i] : 0);
			SDK_Log("[BRIDGE-PP] hex: %s\n", hexBuf);
			SDK_Log("[BRIDGE-PP] pre-nonce: bitsRead=%d flags=0x%02X choked=%d pktSize=%d\n",
				bitsRead, flags, choked, pktSize);
		}
	}

	// === Parse S3 nonce + subchannel (-verified format) ===
	bool subChannelOk = true;

	// === NONCE SECTION (: SendDatagram, ProcessPacket) ===
	// [1-bit nonce_present]
	const int dsBitsBeforeNonce = (int)buf.GetNumBitsRead(); // [SUBCHAN-DESYNC] diag
	const int noncePresent = buf.ReadUBitLong(1);
	uint32_t dsNonceMagic = 0; // [SUBCHAN-DESYNC] diag
	if (noncePresent)
	{
		const uint32_t nonceMagic = buf.ReadUBitLong(32);
		dsNonceMagic = nonceMagic;
		if (nonceMagic != S3_SUBCHAN_MAGIC_S3 && nonceMagic != S3_SUBCHAN_MAGIC_S21)
		{
			// S3 ProcessPacket returns -1 (drops packet) on bad nonce magic.
			// We must do the same -- if we skip ahead, bit reader is misaligned.
			InterlockedIncrement(&s_ppBadNonce);
			static long long s_badMagic = 0;
			if (++s_badMagic <= 10)
				Warning(eDLL_T::ENGINE, "S21Bridge: bad nonce magic 0x%08X seq=%d bitsRead=%d -- dropping packet\n",
					nonceMagic, sequence, (int)buf.GetNumBitsRead());
			return; // Drop packet like S3 ProcessPacket does
		}

		const int nonceAck = buf.ReadUBitLong(1);
		if (nonceAck)
		{
			const uint32_t ackValue = buf.ReadUBitLong(32);
			// Server ACKing our nonce = reliable data delivered.
			// CRITICAL: only process if we have PENDING reliable data.
			if (ackValue == s_bridgeNonceHost && !s_serverAckedUs && s_reliableSize > 0)
			{
				s_serverAckedUs = true;
				s_reliableSize = 0;
				static int s_ackLog = 0;
				if (++s_ackLog <= 10)
					SDK_Log("[BRIDGE-PP] nonce ACK 0x%08X -> delivered!\n", ackValue);
			}
		}

		// subchan_ack_seq + low 10 bits
		buf.ReadUBitLong(32);
		buf.ReadUBitLong(10);
	}

	// Replicate native CNetChan::ProcessPacket's last_received update.
	// Native at +: movsd [rbx+20F0h], xmm3
	{
		static double* s_pNetTime = nullptr;
		if (!s_pNetTime)
		{
			const uintptr_t base = reinterpret_cast<uintptr_t>(
				(HMODULE)NetObs_GetExeModuleBase());
			if (base)
				s_pNetTime = reinterpret_cast<double*>(
					NetObs_NetTimeAddr());
		}
		if (s_pNetTime)
		{
			*reinterpret_cast<double*>(
				reinterpret_cast<char*>(pChan) + 0x20F0) = *s_pNetTime;
		}
	}

	// Log bit position after nonce, before subchan
	{
		static long long s_posLog = 0;
		if (++s_posLog <= 15)
			SDK_Log("[BRIDGE-PP] post-nonce: bitsRead=%d nonce=%d bitsLeft=%d\n",
				(int)buf.GetNumBitsRead(), noncePresent, buf.GetNumBitsLeft());
	}

	// === WRITESUBCHANNELDATA (flags bit 0) ===
	// Only present when the server has pending reliable data to send.
	if (S21Bridge_RewalkTrace())
		Warning(eDLL_T::ENGINE,
			"[BRIDGE-PP] re-walk: S2C seq=%d ack=%d flags=0x%02X choked=%d size=%d "
			"subchan=%d bitsLeft=%d\n",
			sequence, sequenceAck, flags, choked, pktSize,
			(flags & 0x01) ? 1 : 0, (int)buf.GetNumBitsLeft());

	if (flags & 0x01)
	{
		// [SUBCHAN-DESYNC] capture context for the parser's bad-magic dump.
		s_dsyncSeq = sequence; s_dsyncAck = sequenceAck; s_dsyncFlags = flags;
		s_dsyncChoked = choked; s_dsyncNoncePresent = noncePresent;
		s_dsyncNonceMagic = dsNonceMagic; s_dsyncBitsBeforeNonce = dsBitsBeforeNonce;
		s_dsyncSubBitPos = (int)buf.GetNumBitsRead();
		s_dsyncRaw = reinterpret_cast<const uint8_t*>(buf.GetBasePointer());
		s_dsyncPktSize = pktSize;

		InterlockedIncrement(&s_ppSubchanCalled);
		subChannelOk = S21Bridge_ParseSubChannelData(pChan, buf);
		if (!subChannelOk)
			InterlockedIncrement(&s_ppSubchanFailed);

		static long long s_subLog = 0;
		if (++s_subLog <= 10 || (s_subLog % 2000) == 0)
			SDK_Log("[BRIDGE-PP] #%lld subchan parse: ok=%d bitsLeft=%d\n",
				s_subLog, subChannelOk ? 1 : 0, buf.GetNumBitsLeft());
	}

	// Update netchannel state
	if (sequence > S21_NC_InSeqNr(pChan))
		S21_NC_InSeqNr(pChan) = sequence;

	// [ACK-XLATE] stamp m_nOutSequenceNrAck in the ENGINE's sequence space (see the
	// bridge_ack_xlate banner): translate the dedi's bridge-space ack through the flush
	if (bridge_ack_xlate.GetBool())
	{
		const AckXlate_s& x = s_ackXlate[(uint32_t)sequenceAck & 0x3FF];
		if (x.bridgeSeq == (uint32_t)sequenceAck && x.engineSeq >= 0 &&
			x.engineSeq > S21_NC_OutSeqNrAck(pChan))
		{
			S21_NC_OutSeqNrAck(pChan) = x.engineSeq;
		}
		static long long s_axLog = 0;
		if (++s_axLog <= 5 || (s_axLog % 4000) == 0)
			SDK_Log("[ACK-XLATE] #%lld bridgeAck=%d -> engineAck=%d outSeq=%d (delta=%d)\n",
				s_axLog, sequenceAck, S21_NC_OutSeqNrAck(pChan), S21_NC_OutSeqNr(pChan),
				S21_NC_OutSeqNr(pChan) - S21_NC_OutSeqNrAck(pChan));
	}
	else
		S21_NC_OutSeqNrAck(pChan) = sequenceAck;

	// === Authoritative flow-stat accounting (netgraph shows REAL numbers) ===
	// The bridge replaces CNetChan::ProcessPacket, so the engine's per-packet flow
	if (bridge_net_flow_reconcile.GetBool())
	{
		if (CNetChan__FlowNewPacket)
			CNetChan__FlowNewPacket(pChan, /*FLOW_INCOMING*/ 1, sequence, sequenceAck,
				choked, /*dropped*/ 0, pktSize + 28);

		// FLOW_INCOMING: packets/bytes/choke + loss from genuine S2C sequence gaps.
		s_inPkts++;
		s_inBytes += pktSize + 28; // + UDP/IP overhead, matches the native data-rate convention
		s_inChokeAcc += choked;
		if (s_inPrevSeq >= 0 && sequence > s_inPrevSeq)
		{
			const int delta = sequence - s_inPrevSeq;   // packets since last (incl. this)
			s_inExpected += delta;
			const int gap = delta - 1 - choked;          // genuinely missing (beyond choke)
			if (gap > 0) s_inLost += gap;
		}
		if (sequence > s_inPrevSeq) s_inPrevSeq = sequence;

		// FLOW_OUTGOING baseline (relay-seq space: s_c2sSeqCounter sent vs dedi sequenceAck).
		if (!s_outWinInit)
		{
			s_outRelayWin = s_c2sSeqCounter;
			s_outAckWin   = (uint32_t)sequenceAck;
			s_outWinInit  = true;
		}

		// RTT (ping): time from relaying a C2S seq to the dedi acking it. sequenceAck is
		// the highest relay seq the dedi received, so look up when we sent it. EMA-smoothed.
		// Published continuously (not just at the 1s boundary) so ping stays live/moving.
		{
			const uint32_t ackS = (uint32_t)sequenceAck;
			if (ackS != 0 && ackS != s_prevAckSeq)
			{
				const double sendT = s_relaySendTime[ackS & 0x3FF];
				const double rtt = Bridge_NetTime() - sendT;
				if (sendT > 0.0 && rtt > 0.0 && rtt < 2.0) // sane window
				{
					s_avgRtt = (s_avgRtt <= 0.0) ? rtt : (s_avgRtt * 0.875 + rtt * 0.125);
					long ms = (long)(s_avgRtt * 1000.0 + 0.5);
					if (ms < 0)
						ms = 0;
					if (ms > 2000)
						ms = 2000;
					InterlockedExchange(&s_bridgeReportedRttMs, ms);
				}
				s_prevAckSeq = ackS;
			}
			s_flowStat[0].avglatency = (float)s_avgRtt;
			s_flowStat[1].avglatency = (float)s_avgRtt;
		}

		// Publish ~once per second.
		const double now = GetTickCount64() / 1000.0;
		if (s_flowWinStart == 0.0) s_flowWinStart = now;
		const double dt = now - s_flowWinStart;
		if (dt >= 1.0)
		{
			// FLOW_INCOMING (1)
			s_flowStat[1].avgpackets = (float)((double)s_inPkts / dt);
			s_flowStat[1].avgbytes   = (float)((double)s_inBytes / dt);
			s_flowStat[1].avgloss    = s_inExpected > 0
				? (float)((double)s_inLost / (double)s_inExpected) : 0.0f;
			s_flowStat[1].avgchoke   = (s_inPkts + s_inChokeAcc) > 0
				? (float)((double)s_inChokeAcc / (double)(s_inPkts + s_inChokeAcc)) : 0.0f;

			// FLOW_OUTGOING (0): relay sent vs dedi-acked over the window (signed delta,
			// guarded against reconnect/seq-reset). Loss = relayed-but-unacked beyond a
			// small in-flight tolerance (~RTT worth of packets).
			long long relayDelta = (long long)s_c2sSeqCounter - (long long)s_outRelayWin;
			long long ackDelta   = (long long)(uint32_t)sequenceAck - (long long)s_outAckWin;
			if (relayDelta < 0 || ackDelta < 0) { relayDelta = 0; ackDelta = 0; } // reset
			s_flowStat[0].avgpackets = (float)((double)relayDelta / dt);
			s_flowStat[0].avgbytes   = (float)((double)s_outBytesAcc / dt);
			const long long outLost = relayDelta - ackDelta - 4;
			s_flowStat[0].avgloss = (relayDelta > 0 && outLost > 0)
				? (float)((double)outLost / (double)relayDelta) : 0.0f;
			s_flowStat[0].avgchoke = 0.0f; // the relay does not choke at the netchannel layer

			for (int f = 0; f < 2; ++f)
			{
				if (s_flowStat[f].avgloss < 0.0f) s_flowStat[f].avgloss = 0.0f;
				if (s_flowStat[f].avgloss > 1.0f) s_flowStat[f].avgloss = 1.0f;
			}

			// roll the window
			s_flowWinStart = now;
			s_inPkts = 0; s_inBytes = 0; s_inExpected = 0; s_inLost = 0; s_inChokeAcc = 0;
			s_outRelayWin = s_c2sSeqCounter; s_outAckWin = (uint32_t)sequenceAck; s_outBytesAcc = 0;
		}
	}

	// Diagnostic logging
	{
		static long long s_ppCount = 0;
		if (++s_ppCount <= 10 || (s_ppCount % 2000) == 0)
		{
			SDK_Log("[BRIDGE-PP] #%lld: seq=%d ack=%d flags=0x%02X nonce=%d choked=%d bitsLeft=%d\n",
				s_ppCount, sequence, sequenceAck, flags, noncePresent, choked,
				buf.GetNumBitsLeft());
		}
	}

	if (ppArm)
		QueryPerformanceCounter(&ppQHdr);

	// === Process reassembled reliable data (if subchannel transfer complete) ===
	S21Bridge_DispatchReliableTransfer(pChan);

	if (ppArm)
		QueryPerformanceCounter(&ppQRel);

	// === Process unreliable messages ===
	if (subChannelOk && buf.GetNumBitsLeft() >= NETMSG_TYPE_BITS)
	{
		InterlockedIncrement(&s_ppPmCalled);
		S21Bridge_ProcessMessages(pChan, &buf);
	}
	else if (!subChannelOk)
	{
		// Subchannel parse mangled the bitstream; we can't trust unreliable
		// body alignment. PM is gated off entirely on this packet.
		InterlockedIncrement(&s_ppPmSkippedSubFail);
	}
	else
	{
		// Subchannel was OK but the unreliable body has < 7 bits left -- this
		// is normal during signon and idle frames. Counted so we can confirm
		// it's not the cause of post-signon dispatch silence.
		InterlockedIncrement(&s_ppPmSkippedNoBits);
	}

	if (ppArm)
		QueryPerformanceCounter(&ppQPm);

	// === Notify message handler ===
	INetChannelHandler* handler = S21_NC_MessageHandler(pChan);
	if (handler)
	{
		void** vtbl = *reinterpret_cast<void***>(handler);
		// S21 handler->vtable[5] = FlowNewPacket(handler, outSeqAck, inSeq)
		reinterpret_cast<void(*)(INetChannelHandler*, int, int)>(vtbl[5])(
			handler, sequenceAck, sequence);
		// S21 handler->vtable[6] = PacketEnd(handler)
		reinterpret_cast<void(*)(INetChannelHandler*)>(vtbl[6])(handler);
	}

	// No S->C ScriptRemote drain here: release is snapshot-tick arrival, not packet position.

	if (ppArm)
	{
		QueryPerformanceCounter(&ppQ1);
		Warning(eDLL_T::CLIENT, "[PP-BUDGET] #%ld bytes=%d seq=%d hdr=%.2f rel=%.2f pm=%.2f tail=%.2f total=%.2f\n",
			ppBN, pktSize, sequence,
			BridgeBudget_Ms(ppQ0.QuadPart, ppQHdr.QuadPart, ppFreq.QuadPart),
			BridgeBudget_Ms(ppQHdr.QuadPart, ppQRel.QuadPart, ppFreq.QuadPart),
			BridgeBudget_Ms(ppQRel.QuadPart, ppQPm.QuadPart, ppFreq.QuadPart),
			BridgeBudget_Ms(ppQPm.QuadPart, ppQ1.QuadPart, ppFreq.QuadPart),
			BridgeBudget_Ms(ppQ0.QuadPart, ppQ1.QuadPart, ppFreq.QuadPart));
	}

	return;

}

// S21->S3 translation table (for outgoing messages from S21 client, or
// incoming on S3 server). Index = S21 type ID, value = S3 type ID.
static const int s_S21ToS3[91] = {
	/* 0 */ 0, // net_NOP
	/* 1 */ 1, // net_Disconnect
	/* 2 */ 2, // (reserved)
	/* 3 */ 3, // net_StringCmd
	/* 4 */ 68,
	/* 5 */ 4, // net_SetConVar
	/* 6 */ 5, // net_SignonState
	/* 7 */ 6, // net_MTXUserMsg
	/* 8 */ -1, /* 9 */ -1, /* 10 */ -1, // gaps
	/* 11 */ 7, // svc_ServerInfo [VERIFIED: msgArray[11] RFB reads 16b proto + 32b srvCount + flags + strings + bulk blob]
	/* 12 */ 8, // svc_SendTable [VERIFIED: msgArray[12] RFB = 1b + 32b length + payload]
	/* 13 */ 9, // svc_ClassInfo [VERIFIED: msgArray[13] RFB = 16b count + per-class strings]
	/* 14 */ 11, // svc_Playlists [VERIFIED: msgArray[14] RFB = 1b compressed + 32b length + BZ2 blob (max 0x7D000)]
	/* 15 */ 12, // svc_CreateStringTable [VERIFIED: slot 15 RFB reads 24b dataLen (vs S3's 22b)]
	/* 16 */ 13, // svc_UpdateStringTable: 5b tableId + 1b count-present + [16b numEnt] + 20b dataLen; 16b exists only when the flag is set
	/* 17 */ 14, // svc_VoiceData [VERIFIED: matches s3 wire format = 8b+16b+payload]
	/* 18 */ 15, // svc_DurangoVoiceData [VERIFIED: matches s3 = 8b+16b+2b+1b+payload]
	/* 19 */ -1, // (S21-only msg at slot 19; handler[9], string-buffer reader. Was incorrectly mapped to s3=15)
	/* 20 */ 17, // svc_Sounds [: slot[20] RFB reads 32b+8b+data blob]
	/* 21 */ 18, // svc_FixAngle
	/* 22 */ 19, // svc_CrosshairAngle
	/* 23 */ 20, // svc_GrantClientSidePickup
	/* 24 */ 22, // svc_ServerTick
	/* 25 */ 23, // svc_PersistenceDefFile [VERIFIED: slot 25 vtbl = the full-send message SendServerInfo writes]
	/* 26 */ 24, // svc_UseCachedPersistenceDefFile [VERIFIED: slot 26 = UseCachedDefFile; old mapping to s3=23 DefFile was wrong]
	/* 27 */ 25, // svc_PersistenceBaseline [ verified: S21 slot 27 = PB wrapper ]
	/* 28 */ 26, // svc_PersistenceUpdateVar [verified: GetType=28, RFB ]
	/* 29 */ 27, // svc_PersistenceNotifySaved [verified: GetType=29, Process -> handler slot 31 ]
	/* 30 */ 28, // svc_DLCNotifyOwnership [verified: GetType=30, apply writes entitlement bitfield..A20]
	/* 31 */ 29, // svc_MatchmakingETAs
	/* 32 */ 30, // svc_MatchmakingStatus
	/* 33 */ 31, // svc_MTXUserInfo
	/* 34 */ 32, // svc_PlaylistChange [64-char string + name check]
	/* 35 */ 33, // svc_SetTeam [7 bits]
	/* 36 */ 34, // svc_PlaylistOverrides [15b length + 1b + blob]
	/* 37 */ 35, // svc_AntiCheat
	/* 38 */ -1, // svc_CustomMatchResponse (new, no S3 equiv)
	/* 39 */ -1, // svc_CustomMatchCreateOrJoinResp
	/* 40 */ -1, // svc_CustomMatchGetStatsResp
	/* 41 */ -1, // svc_NetHealth
	/* 42 */ -1, // svc_LiveAPI
	/* 43 */ -1, // svc_CupsLeaderboardData
	/* 44 */ -1, // svc_CupsServerResponse
	/* 45 */ -1, // svc_RelayTicket
	/* 46 */ -1, // svc_MigrateParty
	/* 47 */ -1, // svc_ConnectToDedi
	/* 48 */ 41, // svc_TempEntities -- -VERIFIED: RFB = 32b m_tick(@+0x88) + 10b m_nNumEntries(@+0x20) + 24b m_nLength + blob + Seek; GetType vtbl-> returns 48. (Old table guessed DestructibleStaticProps -- wrong.) -> S3 41.
	/* 49 */ 37, // svc_UserMessage
	/* 50 */ 40, // svc_Snapshot
	/* 51 */ -1, // S21 51 = inline-records msg (8b count + N*(10*u32) + 32b), NOT svc_TempEntities -- do not relay to S3. (TempEntities is S21 48.)
	/* 52 */ 42, // svc_Menu
	/* 53 */ 44, // svc_DatatableChecksum
	/* 54 */ -1, // svc_DeathRecap (new)
	/* 55 */ -1, // svc_PartySpectateSlotReady
	/* 56 */ -1, // svc_PlayerRanksDevtoolResult
	/* 57 */ 46, // clc_Move -- -VERIFIED: CLC_Move::vftable=, vtable[7]=GetType= returns 57. (Old table claimed slot 57 was svc_RetrieveLeaderboardDevtoolResult, which is wrong for S21.)
	/* 58 */ -1, // svc_PlayerTAGResult
	/* 59 */ -1, // svc_ZoneSkyWriting
	/* 60 */ -1, // svc_RankScoreUpdated
	/* 61 */ 45, // clc_ClientInfo (UNVERIFIED on S21 -- type may differ)
	/* 62 */ -1, // (S21 doesn't have clc_Move at 62 -- it's at type 57; verified via msgArray dump + CLC_Move::GetType)
	/* 63 */ 57, // clc_ClaimClientSidePickup (loot) -- -VERIFIED (GetType ->63; RFB 10-bit field). S3 type 57. (68, -5 shift. VoiceData's real S21 type is TBD.)
	/* 64 */ 48, // clc_DurangoVoiceData (UNVERIFIED on S21)
	/* 65 */ 60, // clc_ClientTick -- -VERIFIED: CLC_ClientTick::vftable=, vtable[7]=GetType= returns 65. (Old table claimed slot 65 was clc_LoadingProgress, which is wrong for S21.)
	/* 66 */ 61, // clc_ClientSayText (text chat) -- -VERIFIED (GetType ->0x42). numbered chat 71; S21 numbers it 66. (PersistenceClientToken's real S21 type is TBD.)
	/* 67 */ 55, // clc_SetClientEntitlements (UNVERIFIED on S21)
	/* 68 */ -1, // S21 68 is NOT ClaimClientSidePickup (that's 63). Likely clc_AntiCheat (73, -5) -- drop-intentionally. Unverified; do not relay.
	/* 69 */ -1, // (gap)
	/* 70 */ -1, // (S21 doesn't have clc_ClientTick at 70 -- it's at type 65; verified via vtable GetType)
	/* 71 */ 61, // clc_ClientSayText -- DISPROVEN: S21 71 is a BINARY msg, NOT chat. Real S21 chat type unknown; do NOT relay this as chat (corrupts the dedi packet).
	/* 72 */ -1, // (gap)
	/* 73 */ -1, // clc_AntiCheat -- never relay / never native ProcessAntiCheat
	/* 74 */ 65, // clc_GamepadMsg -- -VERIFIED (GetType ->74; RFB reads 8-bit field). S3 type 65. (No shift: 74 == S21 74.)
	/* 75 */ -1, // clc_ControllerEventMsg (new)
	/* 76 */ -1, // clc_ObserverCmdMsg
	/* 77 */ -1, // clc_RoamingCameraPosMsg
	/* 78 */ -1, // clc_CustomMatchCmd
	/* 79 */ -1, // clc_ScriptMessageChecksum
	/* 80 */ -1, // clc_BHit
	/* 81 */ -1, // clc_ExperimentList
	/* 82 */ -1, // clc_ReportClub
	/* 83 */ -1, // clc_ReportPlayerCustomerService
	/* 84 */ -1, // clc_PlayerRanksDevtool
	/* 85 */ -1, // clc_RetrieveLeaderboardDevtool
	/* 86 */ -1, // clc_MigrateMe
	/* 87 */ -1, // clc_ResetIdleTimer
	/* 88 */ -1, // clc_VoiceModEnable
	/* 89 */ -1, // clc_VoiceBan
	/* 90 */ -1, // clc_PlayerTAG
};


int S21Bridge_TranslateNetMsgType(int msgType, bool bIncoming)
{
	if (!s_bridgeActive)
		return msgType;

	if (bIncoming)
	{
		// Incoming from S3 server -> translate S3 IDs to S21 IDs
		if (msgType < 0 || msgType >= (int)ARRAYSIZE(s_S3ToS21))
			return -1;
		return s_S3ToS21[msgType];
	}
	else
	{
		// Outgoing to S3 server -> translate S21 IDs to S3 IDs
		if (msgType < 0 || msgType >= (int)ARRAYSIZE(s_S21ToS3))
			return -1;
		return s_S21ToS3[msgType];
	}
}

// Phase 2: Parse an incoming S3 S2C netchannel packet to extract subchannel
// data (server nonce, subchannel seq). This runs on every netchannel packet
static void S21Bridge_ParseS2CPacket(const uint8_t* data, int dataLen)
{
	if (dataLen < 9)
		return;

	// Read header
	const uint32_t seq = *reinterpret_cast<const uint32_t*>(data);
	const uint8_t flags = data[8];

	// Update our tracked inbound sequence number.
	if (seq > s_bridgeInSeqNr)
		s_bridgeInSeqNr = seq;

	// Check if this packet has the reliability/subchannel section
	// In S3, the flags byte is at bit offset 64. The reliability section
	int bitPos = 72; // after [32 seq][32 ack][8 flags]
	if (flags & 0x10)
		bitPos += 8; // choked count sits between flags and nonce_present

	// Helper lambdas for bit reading
	auto readBit = [&]() -> uint32_t {
		if (bitPos / 8 >= dataLen) return 0;
		uint32_t val = (data[bitPos / 8] >> (bitPos & 7)) & 1;
		bitPos++;
		return val;
	};
	auto readBits = [&](int nBits) -> uint32_t {
		uint32_t val = 0;
		for (int i = 0; i < nBits; i++)
		{
			if (bitPos / 8 >= dataLen) return val;
			val |= (uint32_t)((data[bitPos / 8] >> (bitPos & 7)) & 1) << i;
			bitPos++;
		}
		return val;
	};

	// Check nonce section: first bit after flags
	const uint32_t noncePresent = readBit();
	if (noncePresent)
	{
		const uint32_t magic = readBits(32);
		if (magic == 0xFDBAC34D)
		{
			const uint32_t hasNonce = readBit();
			if (hasNonce)
			{
				const uint32_t nonceVal = readBits(32);
				{
					static int s_s2cNonceLog = 0;
					if (++s_s2cNonceLog <= 15)
						SDK_Log("[BRIDGE-S2C] nonceACK val=0x%08X bridgeNonce=0x%08X acked=%d rel=%d\n",
							nonceVal, s_bridgeNonceHost, s_serverAckedUs ? 1 : 0, s_reliableSize);
				}
				// Server ACKing our nonce = reliable delivered.
				// Only process if we have PENDING reliable data.
				if (nonceVal == s_bridgeNonceHost && !s_serverAckedUs && s_reliableSize > 0)
				{
					s_serverAckedUs = true;
					s_reliableSize = 0;
					static int s_ackLog = 0;
					if (++s_ackLog <= 10)
						SDK_Log("[BRIDGE-S2C] nonce ACK 0x%08X -> delivered!\n", nonceVal);
				}
				else if (nonceVal == s_bridgeNonceHost)
				{
					static int s_staleLog = 0;
					if (++s_staleLog <= 3)
						SDK_Log("[BRIDGE-S2C] ignoring stale nonce ACK 0x%08X (acked=%d rel=%d)\n",
							nonceVal, s_serverAckedUs ? 1 : 0, s_reliableSize);
				}
			}
			// Read subchan_ack_seq and reqid (server ACKing our subchannel)
			/*uint32_t subAckSeq =*/ readBits(32);
			/*uint32_t subAckReq =*/ readBits(10);
		}
	}

	// Check WriteSubChannelData section
	// The subchannel magic 0xABCDEF01 follows the nonce section
	const uint32_t subMagic = readBits(32);
	if (subMagic != 0xABCDEF01)
		return; // No subchannel data in this packet

	const uint32_t subSeq = readBits(32);
	const uint32_t reqId = readBits(10);

	uint32_t serverNonceHost = 0;
	if (reqId == 0)
	{
		const uint32_t nonceFlag = readBit();
		if (nonceFlag)
			serverNonceHost = readBits(32);
	}

	if (serverNonceHost != 0 && s_serverNonce == 0)
	{
		s_serverNonce = serverNonceHost;
		s_needNonceAck = true;
		s_serverNonceCaptured = true;
		static int s_capLog = 0;
		if (++s_capLog <= 5)
			SDK_Log("[BRIDGE-S2C] captured server nonce: 0x%08X subSeq=%u reqId=%u\n",
				serverNonceHost, subSeq, reqId);
	}

	// ACK of S2C reliable is owned by ParseSubChannelData (unique
	// entry_seq complete). This parser only exists to capture nonce.
	s_serverNonceCaptured = true;

	static long long s_parseLog = 0;
	if (++s_parseLog <= 10)
		SDK_Log("[BRIDGE-S2C] parsed S2C seq=%u: nonce=0x%08X subSeqRecv=%u\n",
			seq, s_serverNonce, s_serverSubSeqRecv);
}

// Phase 4: Handle DataBlock signon fragments (S2C_DATABLOCK_FRAGMENT = 'O')
// SDK-verified format (from ServerDataBlockSender::SendDataBlock)
static const int kDBHeaderSize = 4 + 1 + 2 + 4 + 2 + 2 + 4; // = 19 bytes

static void S21Bridge_ResetDataBlock()
{
	if (!s_dbScratchBuffer)
	{
		s_dbScratchBuffer = new uint8_t[BRIDGE_DB_SCRATCH_SIZE + HeapCanary::kTailBytes];
		if (s_dbScratchBuffer)
			HeapCanary::RegisterTail("bridge-db-scratch", s_dbScratchBuffer,
				BRIDGE_DB_SCRATCH_SIZE);
	}
	memset(s_dbScratchBuffer, 0, BRIDGE_DB_SCRATCH_SIZE);
	memset(s_dbBlockStatus, 0, sizeof(s_dbBlockStatus));
	memset(s_dbBlockSizes, 0, sizeof(s_dbBlockSizes));
	s_dbTransferSize = 0;
	s_dbTotalBlocks = 0;
	s_dbBlocksReceived = 0;
	s_dbComplete = false;
}

// Incremental per-block ACK. Reports exactly which fragments we hold so the dedi's
// ServerDataBlockSender resends ONLY the missing ones instead of looping the whole unacked
static void S21Bridge_SendDataBlockBitmapAck()
{
	if (!s_origSendto || s_bridgeSocket == INVALID_SOCKET)
		return;
	if (s_dbTotalBlocks <= 0 || s_dbTotalBlocks > BRIDGE_DB_MAX_FRAGMENTS)
		return;

	uint8_t ackBuf[16 + (BRIDGE_DB_MAX_FRAGMENTS / 8)];
	memset(ackBuf, 0, sizeof(ackBuf));
	int pos = 0;

	// CONNECTIONLESS_HEADER
	const uint32_t hdr = 0xFFFFFFFF;
	memcpy(ackBuf + pos, &hdr, 4); pos += 4;

	// C2S_DATABLOCK_ACK
	ackBuf[pos++] = 0x50;

	// transferId == server currentId (NOT +1: +1 is the complete-all path)
	const uint16_t tid = s_dbTransferId;
	memcpy(ackBuf + pos, &tid, 2); pos += 2;

	// transferNr
	const uint16_t tnr = (uint16_t)s_dbTransferNr;
	memcpy(ackBuf + pos, &tnr, 2); pos += 2;

	// Bit-packed region (byte-aligned here): bit0 = flag(1), bit(1+i) = block i received.
	uint8_t* const bits = ackBuf + pos;
	int bitPos = 0;
	bits[bitPos >> 3] |= (uint8_t)(1u << (bitPos & 7)); // flag = bitmap present
	++bitPos;
	for (int i = 0; i < s_dbTotalBlocks; ++i)
	{
		if (s_dbBlockStatus[i])
			bits[bitPos >> 3] |= (uint8_t)(1u << (bitPos & 7));
		++bitPos;
	}
	pos += (bitPos + 7) >> 3;

	const int ackResult = s_origSendto(s_bridgeSocket,
		reinterpret_cast<const char*>(ackBuf), pos, 0,
		reinterpret_cast<const sockaddr*>(&s_bridgeDest),
		sizeof(s_bridgeDest));

	static long long s_bmAckLog = 0;
	if (++s_bmAckLog <= 8 || (s_bmAckLog % 200) == 0)
		SDK_Log("[BRIDGE-DB] incr bitmap ACK #%lld: id=%d nr=%d recv=%d/%d bytes=%d send=%d\n",
			s_bmAckLog, s_dbTransferId, s_dbTransferNr, s_dbBlocksReceived,
			s_dbTotalBlocks, pos, ackResult);
}

static bool S21Bridge_Handle0x4F(const uint8_t* data, int dataLen)
{
	if (dataLen < kDBHeaderSize)
		return false;
	if (data[4] != 0x4F)
		return false;

	// Parse ACTUAL S3 DataBlock header (19-byte header)
	// Binary uses WriteShort for transferId/transferNr/blockNr (not WriteByte as SDK says)
	const uint8_t* p = data + 5; // after FFFFFFFF + 4F
	uint16_t transferId;
	memcpy(&transferId, p + 0, 2);    // offset 5-6: WriteShort
	uint32_t transferSize;
	memcpy(&transferSize, p + 2, 4);  // offset 7-10: WriteLong
	uint16_t transferNr;
	memcpy(&transferNr, p + 6, 2);    // offset 11-12: WriteShort
	uint16_t blockNr;
	memcpy(&blockNr, p + 8, 2);      // offset 13-14: WriteShort
	uint32_t blockSize;
	memcpy(&blockSize, p + 10, 4);   // offset 15-18: WriteLong
	const uint8_t* blockData = p + 14; // offset 19: data starts
	const int blockDataAvail = dataLen - 19; // bytes available for block data

	// Sanity checks
	if ((int)blockSize > blockDataAvail || blockSize > BRIDGE_DB_MAX_FRAG_SIZE)
	{
		static int s_dropLog = 0;
		if (++s_dropLog <= 3)
			SDK_Log("[BRIDGE-DB] WARNING: invalid blockSize=%u (avail=%d) blockNr=%u xferSz=%u, dropping\n",
				blockSize, blockDataAvail, blockNr, transferSize);
		return false;
	}
	if (blockNr >= BRIDGE_DB_MAX_FRAGMENTS)
	{
		SDK_Log("[BRIDGE-DB] WARNING: blockNr=%d exceeds max %d\n", blockNr, BRIDGE_DB_MAX_FRAGMENTS);
		return false;
	}

	// Server retransmitting a completed transfer -- re-send our ACK.
	// B2: while a deferred payload is still queued for the main thread
	if (s_dbComplete && transferNr == s_dbTransferNr && transferId == s_dbTransferId)
	{
		if (s_pendingSignonReady)
			return true; // hold the ACK until the main thread drains this transfer
		static long long s_reackCount = 0;
		++s_reackCount;
		S21Bridge_SendDataBlockAck();
		if (s_reackCount <= 5 || (s_reackCount % 100) == 0)
			SDK_Log("[BRIDGE-DB] re-sent ACK #%lld (server retransmitting completed transfer)\n", s_reackCount);
		return true;
	}

	// Detect new transfer (different transferNr -> reset)
	if (transferNr != s_dbTransferNr || s_dbComplete)
	{
		// [SEC prio16] Fail closed on zero/oversize transferSize (scratch is BRIDGE_DB_SCRATCH_SIZE).
		if (transferSize == 0 || transferSize > (uint32_t)BRIDGE_DB_SCRATCH_SIZE)
		{
			static int s_szDrop = 0;
			if (++s_szDrop <= 8)
				Warning(eDLL_T::ENGINE, "[SEC][BRIDGE-DB] transferSize=%u over scratch %d -- drop transfer\n",
					transferSize, BRIDGE_DB_SCRATCH_SIZE);
			return false;
		}
		const int totalBlocks = (int)((transferSize + BRIDGE_DB_MAX_FRAG_SIZE - 1) / BRIDGE_DB_MAX_FRAG_SIZE);
		if (totalBlocks <= 0 || totalBlocks > BRIDGE_DB_MAX_FRAGMENTS)
		{
			static int s_blkDrop = 0;
			if (++s_blkDrop <= 8)
				Warning(eDLL_T::ENGINE, "[SEC][BRIDGE-DB] totalBlocks=%d invalid (max %d) size=%u -- drop\n",
					totalBlocks, BRIDGE_DB_MAX_FRAGMENTS, transferSize);
			return false;
		}

		SDK_Log("[BRIDGE-DB] new transfer: id=%d nr=%d size=%u (prev nr=%d)\n",
			transferId, transferNr, transferSize, s_dbTransferNr);
		S21Bridge_ResetDataBlock();
		s_dbTransferId = transferId;
		s_dbTransferNr = transferNr;
		s_dbTransferSize = (int)transferSize;
		s_dbTotalBlocks = totalBlocks;
	}

	// Reject block index outside the contiguous map for this transfer.
	if (s_dbTotalBlocks <= 0 || blockNr >= (uint16_t)s_dbTotalBlocks)
	{
		static int s_oobBlk = 0;
		if (++s_oobBlk <= 8)
			Warning(eDLL_T::ENGINE, "[SEC][BRIDGE-DB] blockNr=%u outside map 0..%d -- drop frag\n",
				blockNr, s_dbTotalBlocks - 1);
		return false;
	}

	// Skip duplicate blocks. A dup means the dedi resent a block we already hold ->
	// our prior ACK didn't reach/advance it yet, so re-send the current bitmap so it
	// stops resending what we have.
	if (s_dbBlockStatus[blockNr])
	{
		if (sdk_bridge_db_incremental_ack.GetBool() && !s_dbComplete)
			S21Bridge_SendDataBlockBitmapAck();
		return true;
	}

	// Ensure scratch is live (Reset allocates; path can race first frag).
	if (!s_dbScratchBuffer)
	{
		s_dbScratchBuffer = new uint8_t[BRIDGE_DB_SCRATCH_SIZE + HeapCanary::kTailBytes];
		if (s_dbScratchBuffer)
			HeapCanary::RegisterTail("bridge-db-scratch", s_dbScratchBuffer,
				BRIDGE_DB_SCRATCH_SIZE);
	}

	// Copy fragment data into scratch buffer -- fail closed on oversize.
	const int scratchOffset = (int)blockNr * BRIDGE_DB_MAX_FRAG_SIZE;
	if (scratchOffset < 0 ||
		scratchOffset + (int)blockSize > BRIDGE_DB_SCRATCH_SIZE ||
		scratchOffset + (int)blockSize > s_dbTransferSize + BRIDGE_DB_MAX_FRAG_SIZE)
	{
		static int s_copyDrop = 0;
		if (++s_copyDrop <= 8)
			Warning(eDLL_T::ENGINE, "[SEC][BRIDGE-DB] frag copy OOB block=%u size=%u off=%d -- drop\n",
				blockNr, blockSize, scratchOffset);
		return false;
	}

	memcpy(s_dbScratchBuffer + scratchOffset, blockData, blockSize);
	s_dbBlockStatus[blockNr] = true;
	s_dbBlockSizes[blockNr] = (uint16_t)blockSize;
	s_dbBlocksReceived++;

	// Log progress
	static long long s_fragLog = 0;
	if (++s_fragLog <= 5 || (s_fragLog % 50) == 0)
		SDK_Log("[BRIDGE-DB] block #%d: %u bytes, received %d/%d (transfer %d/%u)\n",
			blockNr, blockSize, s_dbBlocksReceived, s_dbTotalBlocks,
			transferId, transferSize);

	// Incremental per-block ACK: tell the dedi exactly which fragments we now hold so it
	// resends only the missing ones (not the whole unacked set every resend round). This
	// collapses the ~10s/snapshot large-DataBlock stall that starves the client on big maps.
	if (sdk_bridge_db_incremental_ack.GetBool() && !s_dbComplete)
		S21Bridge_SendDataBlockBitmapAck();

	// [SEC] Complete only when the contiguous map 0..total-1 is fully valid
	// (count alone can lie if status bits and count ever diverge).
	bool mapContiguous = (s_dbBlocksReceived >= s_dbTotalBlocks && s_dbTotalBlocks > 0);
	if (mapContiguous)
	{
		for (int i = 0; i < s_dbTotalBlocks; ++i)
		{
			if (!s_dbBlockStatus[i])
			{
				mapContiguous = false;
				break;
			}
		}
	}

	// [SEC] Byte coverage: every block must carry exactly its expected payload
	// (full fragment for all but the last, the remainder for the last), or the
	// gap inside [0,transferSize) ships uninitialized scratch as signon data.
	if (mapContiguous)
	{
		static int s_covDropLog = 0;

		for (int i = 0; i < s_dbTotalBlocks; ++i)
		{
			const int expected = (i == s_dbTotalBlocks - 1)
				? (s_dbTransferSize - i * BRIDGE_DB_MAX_FRAG_SIZE)
				: BRIDGE_DB_MAX_FRAG_SIZE;

			if (s_dbBlockSizes[i] != expected)
			{
				mapContiguous = false;

				if (++s_covDropLog <= 8)
					Warning(eDLL_T::ENGINE, "[SEC][BRIDGE-DB] block %d holds %u bytes, expected %d -- transfer dropped\n",
						i, s_dbBlockSizes[i], expected);
				S21Bridge_ResetDataBlock();
				return true;
			}
		}
	}

	if (mapContiguous && !s_dbComplete)
	{
		if (s_dbTransferSize <= 0 || s_dbTransferSize > BRIDGE_DB_SCRATCH_SIZE)
		{
			Warning(eDLL_T::ENGINE, "[SEC][BRIDGE-DB] complete rejected: transferSize=%d\n",
				s_dbTransferSize);
			S21Bridge_ResetDataBlock();
			return true;
		}

		if (sdk_bridge_defer_signon.GetBool())
		{
			// B2: hand the completed (still-compressed) transfer to the MAIN thread.
			// Copy out of the reassembly buffer so the net thread can keep receiving;
			// Hook_Cbuf_Execute runs OnDataBlockComplete (engine SetSignonState/SPAWN).
			if (!s_pendingSignonBuf)
			{
				s_pendingSignonBuf = static_cast<uint8_t*>(
					malloc(BRIDGE_DB_SCRATCH_SIZE + HeapCanary::kTailBytes));
				if (s_pendingSignonBuf)
					HeapCanary::RegisterTail("bridge-pending-signon",
						s_pendingSignonBuf, BRIDGE_DB_SCRATCH_SIZE);
			}
			if (s_pendingSignonReady)
			{
				return true;
			}
			if (s_pendingSignonBuf && s_dbTransferSize > 0 &&
				s_dbTransferSize <= BRIDGE_DB_SCRATCH_SIZE)
			{
				memcpy(s_pendingSignonBuf, s_dbScratchBuffer, s_dbTransferSize);
				s_pendingSignonSize = s_dbTransferSize;
				s_dbComplete = true;
				s_pendingSignonReady = true;
			}
			else
			{
				Warning(eDLL_T::ENGINE,
					"[BRIDGE-DB] pending signon alloc failed -- holding transfer, not inlining\n");
				return true;
			}
		}
		else
		{
			S21Bridge_OnDataBlockComplete(s_dbScratchBuffer, s_dbTransferSize);
		}
	}

	return true; // consumed
}

static bool S21Bridge_TryConsumeDataBlockOOB(const uint8_t* h, int recvd)
{
	const uint32_t hdr4 = recvd >= 4 ? *reinterpret_cast<const uint32_t*>(h) : 0;
	if (!(hdr4 == 0xFFFFFFFF && recvd > 4 && h[4] == 0x4F))
		return false;

	if (!S21Bridge_Handle0x4F(h, recvd))
		return true;

	if (s_bridgeChan)
	{
		static double* s_pNetTime = nullptr;
		if (!s_pNetTime)
		{
			const uintptr_t base = reinterpret_cast<uintptr_t>(
				(HMODULE)NetObs_GetExeModuleBase());
			if (base)
				s_pNetTime = reinterpret_cast<double*>(NetObs_NetTimeAddr());
		}
		if (s_pNetTime)
		{
			*reinterpret_cast<double*>(
				reinterpret_cast<char*>(s_bridgeChan) + 0x20F0) = *s_pNetTime;

			static int s_keepaliveLog = 0;
			if (++s_keepaliveLog <= 5 || (s_keepaliveLog % 5000) == 0)
				SDK_Log("[NETCHAN-KEEPALIVE] last_received=%.3f net_time=%.3f (#%d)\n",
					*s_pNetTime, *s_pNetTime, s_keepaliveLog);
		}
	}

	return true;
}

// Phase 1: Self-clocking C2S sender
// Called from PollReceive every frame. If 50ms have elapsed since the last
static void S21Bridge_SelfClockC2S()
{
	if (!s_bridgeActive || s_bridgeSocket == INVALID_SOCKET || !s_origSendto)
		return;

	// Get current time (use simple GetTickCount64 for ms precision)
	const double now = GetTickCount64() / 1000.0;
	if (s_lastBridgeC2STime == 0.0)
	{
		s_lastBridgeC2STime = now;
		return; // skip first call
	}

	// Fire on the 50ms keepalive cadence, OR immediately whenever the C2S
	// input relay has queued a clc_Move/clc_ClientTick (the hooks fill
	const double elapsed = now - s_lastBridgeC2STime;
	// Unlocked: a stale GetNumBitsWritten only costs one early/late flush attempt.
	if (elapsed < 0.050 && s_c2sPend.GetNumBitsWritten() == 0)
		return;

	// Pending C2S signon states are drained by BuildS3Packet's unreliable
	// message section -- no separate reliable queue needed.
	S21Bridge_FlushC2SNow("self-clock");
}

static volatile LONG s_pollFromQueue  = 0;
static volatile LONG s_pollFromSocket = 0;
static volatile LONG s_pollInjected   = 0;

// One line per second while packets flow: where they came from, how many
// reached the engine, and what the packet hook did with them.
static void S21Bridge_PacketFlowSummary(void)
{
	static ULONGLONG s_lastMs = 0;
	static LONG s_lastQ = 0, s_lastS = 0, s_lastI = 0, s_lastPp = 0, s_lastPm = 0;
	static LONG s_lastNoBits = 0, s_lastSubFail = 0, s_lastSnapDelta = 0, s_lastSnapFull = 0;
	const ULONGLONG nowMs = GetTickCount64();
	if (s_lastMs != 0 && nowMs - s_lastMs < 1000)
		return;
	s_lastMs = nowMs;
	const LONG q = s_pollFromQueue, s = s_pollFromSocket, i = s_pollInjected;
	const LONG pp = s_ppEntryCount, pm = s_ppPmCalled;
	const LONG nb = s_ppPmSkippedNoBits, sf = s_ppPmSkippedSubFail;
	const LONG sd = s_snapReencodeDelta, sfu = s_snapReencodeFull;
	static LONG s_lastFr = 0, s_lastDone = 0, s_lastAb = 0, s_lastBig = 0;
	const LONG fr = s_splitFrags, dn = s_splitDone, ab = s_splitAbandoned, bg = s_splitBadCount;
	int inflight = 0;
	for (const SplitPacket& sp : s_splitSlots)
		if (sp.receivedCount != 0)
			++inflight;
	Warning(eDLL_T::ENGINE,
		"[PKT-1S] poll{queue=%ld socket=%ld injected=%ld} split{frags=%ld done=%ld abandoned=%ld big=%ld inflight=%d} pp{entered=%ld pm=%ld nobits=%ld subfail=%ld} snap{delta=%ld full=%ld}\n",
		q - s_lastQ, s - s_lastS, i - s_lastI,
		fr - s_lastFr, dn - s_lastDone, ab - s_lastAb, bg - s_lastBig, inflight,
		pp - s_lastPp, pm - s_lastPm,
		nb - s_lastNoBits, sf - s_lastSubFail, sd - s_lastSnapDelta, sfu - s_lastSnapFull);
	s_lastQ = q; s_lastS = s; s_lastI = i; s_lastPp = pp; s_lastPm = pm;
	s_lastNoBits = nb; s_lastSubFail = sf; s_lastSnapDelta = sd; s_lastSnapFull = sfu;
	s_lastFr = fr; s_lastDone = dn; s_lastAb = ab; s_lastBig = bg;
}

bool S21Bridge_PollReceive(int iSocket, netpacket_s* pInpacket)
{
	if (iSocket == 0 && s_bridgeActive && bridge_net_flow_diag.GetBool())
		S21Bridge_PacketFlowSummary();
	// Only inject bridge packets for the CLIENT socket (0).
	if (iSocket != 0)
		return false;

	if (!pInpacket)
		return false;

	// The bridge socket is unconnected UDP: without a source gate, any host
	// that can reach the port can feed signon / scriptremote / datablock
	// frames straight into the parser. Accept only the connected peer.

	// Tick the async handshake deadline FIRST -- resets state to Idle if a
	// stage deadline passed so the next engine retry can start clean.
	S21Bridge_HandshakeTick();
	PakLobby_OnPollReceive();
	S21Bridge_FlushValidatorDisconnect();

	// Nothing to do unless bridge is active OR a handshake stage is in
	// progress (we need to drain the bridge socket to see S2C_CHALLENGE /
	// S2C_CONNACCEPT even before s_bridgeActive flips to true).
	if (!s_bridgeActive && s_hsStage == BridgeHsStage::Idle)
		return false;

	// Self-clocking C2S sender: only once handshake is Idle. Pre-activating
	// firing through ConnectSent would flood empty netchan seqs before CONNECTED+UserInfo.
	// flood empty netchan seqs before CONNECTED+UserInfo.
	if (s_bridgeActive && s_hsStage == BridgeHsStage::Idle)
		S21Bridge_SelfClockC2S();

	// Poll the bridge socket for new data (non-blocking).
	// DRAIN LOOP: read ALL pending packets. DataBlock fragments (0x4F) are
	if (s_bridgeSocket == INVALID_SOCKET || !s_origRecvfrom)
		return false;

	static char pollBuf[262144]; // Must hold a reassembled split packet
	static_assert(sizeof(pollBuf) <= kS21EnginePacketCapacity,
		"pollBuf must fit the engine packet scratch at netpacket_s+0x30");
	sockaddr_in6 from = {};
	int recvd = 0;
	bool gotPacket = false;

	for (int drainIter = 0; drainIter < 512; drainIter++)
	{
		// First: drain the split packet queue (pushed by Hook_sendto).
		// These are S3 server S2C split fragments that can't reach the bridge
		// socket via network loopback (destPort mismatch in listen server).
		const long tail = s_splitQueueTail;
		if (tail != s_splitQueueHead)
		{
			if (s_splitQueueGen[tail] != s_splitSessionGen)
			{
				InterlockedIncrement(&s_splitAbandoned);
				s_splitQueueTail = (tail + 1) % SPLIT_QUEUE_SIZE;
				recvd = 0;
				continue;
			}
			recvd = s_splitQueue[tail].len;
			if (recvd > 0 && recvd <= (int)sizeof(pollBuf))
				memcpy(pollBuf, s_splitQueue[tail].data, recvd);
			else
				recvd = 0;
			s_splitQueueTail = (tail + 1) % SPLIT_QUEUE_SIZE;

			if (recvd <= 0)
			{
				recvd = 0;
				continue; // bad entry, try next
			}

			// The queue only ever holds peer packets; the engine drops an
			// injected packet whose source is not the channel's address.
			from = s_bridgeDest;
			InterlockedIncrement(&s_pollFromQueue);
		}
		else
		{
			// Queue empty -- try recvfrom
			int fromLen = sizeof(from);
			recvd = s_origRecvfrom(s_bridgeSocket,
				pollBuf, sizeof(pollBuf), 0,
				reinterpret_cast<sockaddr*>(&from), &fromLen);

			// Diagnostic (rate-limited)
			{
				static long long s_pollCalls = 0;
				if (++s_pollCalls <= 10 || (s_pollCalls % 1000) == 0)
				{
					const int wsaErr = (recvd <= 0) ? WSAGetLastError() : 0;
					SDK_Log("[BRIDGE-POLL] #%lld: recvd=%d wsa=%d\n",
						s_pollCalls, recvd, wsaErr);
				}
			}

			if (recvd <= 0)
				return false; // Socket drained AND queue empty

			InterlockedIncrement(&s_pollFromSocket);
			if (!S21Bridge_FromMatchesPeer(
				reinterpret_cast<sockaddr*>(&from), fromLen))
			{
				static long long s_spoofDrops = 0;
				if (++s_spoofDrops <= 10 || (s_spoofDrops % 1000) == 0)
					Warning(eDLL_T::ENGINE,
						"[BRIDGE-POLL] dropped %d byte packet from a non-peer source (#%lld)\n",
						recvd, s_spoofDrops);
				recvd = 0;
				continue;
			}
		}

		const uint8_t* h = reinterpret_cast<const uint8_t*>(pollBuf);
		const uint32_t hdr4 = recvd >= 4 ? *reinterpret_cast<const uint32_t*>(h) : 0;

		// ── DataBlock fragments (0x4F): handle internally, CONTINUE draining ──
		if (S21Bridge_TryConsumeDataBlockOOB(h, recvd))
		{
			recvd = 0;
			continue;
		}

		// ── Split packet reassembly (NET_HEADER_FLAG_SPLITPACKET = 0xFFFFFFFE) ──
		// S3 server fragments netchan packets > MTU into multiple UDP datagrams.
		// We must reassemble all fragments before processing as a netchan packet.
		if (hdr4 == 0xFFFFFFFE && recvd >= 12)
		{
			const int32_t  reqID     = *reinterpret_cast<const int32_t*>(h + 4);
			const uint8_t  pktCount  = h[8]; // R5 format: count FIRST
			const uint8_t  pktNum    = h[9]; // then fragment index
			const uint16_t splitSize = *reinterpret_cast<const uint16_t*>(h + 10);
			const int      payload   = recvd - 12;
			InterlockedIncrement(&s_splitFrags);
			if (pktCount == 0)
			{
				InterlockedIncrement(&s_splitBadCount);
				recvd = 0;
				continue;
			}

			static long long s_splitLog = 0;
			if (++s_splitLog <= 5 || (s_splitLog % 500) == 0)
				SDK_Log("[BRIDGE-SPLIT] reqID=%d pkt=%d/%d splitSz=%d payload=%d\n",
					reqID, pktNum, pktCount, splitSize, payload);

			// The slot already holding this request, else a free one, else
			// the stalest in-flight one.
			const ULONGLONG nowMs = GetTickCount64();
			const long gen = s_splitSessionGen;
			SplitPacket* slot = nullptr;
			int slotIdx = -1;
			for (int i = 0; i < 4; ++i)
			{
				SplitPacket& s = s_splitSlots[i];
				if (s.receivedCount != 0 && s.requestID == reqID && s.packetCount == pktCount)
				{
					if (s_splitSlotGen[i] != gen)
					{
						InterlockedIncrement(&s_splitAbandoned);
						memset(&s, 0, offsetof(SplitPacket, data));
						s_splitSlotGen[i] = 0;
						continue;
					}
					slot = &s;
					slotIdx = i;
					break;
				}
			}
			if (!slot)
			{
				SplitPacket* victim = &s_splitSlots[0];
				int victimIdx = 0;
				for (int i = 0; i < 4; ++i)
				{
					SplitPacket& s = s_splitSlots[i];
					if (s.receivedCount == 0)
					{
						victim = &s;
						victimIdx = i;
						break;
					}
					if (s.lastMs < victim->lastMs)
					{
						victim = &s;
						victimIdx = i;
					}
				}
				if (victim->receivedCount != 0)
					InterlockedIncrement(&s_splitAbandoned);
				memset(victim, 0, offsetof(SplitPacket, data));
				victim->requestID   = reqID;
				victim->packetCount = pktCount;
				victim->splitSize   = splitSize;
				s_splitSlotGen[victimIdx] = gen;
				slot = victim;
				slotIdx = victimIdx;
			}
			slot->lastMs = nowMs;

			// Store fragment
			if (pktNum < pktCount)
			{
				const int offset = (int)pktNum * (int)splitSize;
				const uint64_t bit = 1ull << (pktNum & 63);
				uint64_t& word = slot->receivedMask[pktNum >> 6];
				if (offset >= 0 && offset + payload <= (int)sizeof(slot->data) && !(word & bit))
				{
					memcpy(slot->data + offset, h + 12, payload);
					word |= bit;
					++slot->receivedCount;
					// Track total size: last fragment determines end
					const int endPos = offset + payload;
					if (endPos > slot->totalSize)
						slot->totalSize = endPos;
				}
			}

			if (slot->receivedCount >= (int)pktCount)
			{
				if (slotIdx < 0 || s_splitSlotGen[slotIdx] != gen || gen != s_splitSessionGen)
				{
					InterlockedIncrement(&s_splitAbandoned);
					recvd = 0;
					continue;
				}
				// Reassembled! Copy to pollBuf for injection
				recvd = slot->totalSize;
				slot->receivedCount = 0; // consumed
				if (recvd > 0 && recvd <= (int)sizeof(pollBuf))
				{
					memcpy(pollBuf, slot->data, recvd);
					h = reinterpret_cast<const uint8_t*>(pollBuf);
					InterlockedIncrement(&s_splitDone);

					static long long s_reassLog = 0;
					if (++s_reassLog <= 10 || (s_reassLog % 200) == 0)
						SDK_Log("[BRIDGE-SPLIT] reassembled reqID=%d: %d bytes from %d fragments\n",
							reqID, recvd, pktCount);
					if (S21Bridge_TryConsumeDataBlockOOB(h, recvd))
					{
						recvd = 0;
						continue;
					}
				}
				else
				{
					SDK_Log("[BRIDGE-SPLIT] WARNING: reassembled size %d out of range\n", recvd);
					recvd = 0;
					continue;
				}
			}
			else
			{
				recvd = 0;
				continue; // More fragments needed, drain next packet
			}
		}

		// ── Found a non-DataBlock packet: classify and inject ──
		// Re-read header in case we just reassembled a split packet
		const uint32_t hdr4Final = recvd >= 4 ? *reinterpret_cast<const uint32_t*>(h) : 0;
		const bool isOOB = (hdr4Final == 0xFFFFFFFF);

		// ── Async handshake state transitions ──
		// Intercept the OOB packets the state machine is waiting on instead
		if (isOOB && recvd >= 5)
		{
			const uint8_t oobType = h[4];

			if (oobType == 0x49)
			{
				if (s_hsStage == BridgeHsStage::ChallengeSent && recvd >= 9)
				{
					const uint32_t challenge = *reinterpret_cast<const uint32_t*>(h + 5);
					S21Bridge_OnS2CChallenge(challenge);
					S21Bridge_RememberChallengeMap(
						S21Bridge_PickChallengeMap(
							reinterpret_cast<const unsigned char*>(pollBuf), recvd));
					int nOut = recvd;
					if (S21Bridge_RewritePollToDestChallenge04(
						reinterpret_cast<unsigned char*>(pollBuf),
						static_cast<int>(sizeof(pollBuf)), &nOut))
					{
						recvd = nOut;
					}
					else
					{
						recvd = 0;
						continue;
					}
				}
				else
				{
					recvd = 0;
					continue;
				}
			}
			else if (oobType == 0x4A)
			{
				if (s_hsStage == BridgeHsStage::ConnectSent)
				{
					char mapBuf[64] = {};
					char modeBuf[64] = {};
					const size_t nBody = (recvd > 5) ? static_cast<size_t>(recvd - 5) : 0;
					Bridge_CopyBoundedCString(reinterpret_cast<const char*>(h + 5), nBody, mapBuf, sizeof(mapBuf));
					const size_t nMap = strnlen(mapBuf, sizeof(mapBuf));
					const size_t nModeOff = nMap + 1;
					if (nModeOff < nBody)
						Bridge_CopyBoundedCString(reinterpret_cast<const char*>(h + 5) + nModeOff,
							nBody - nModeOff, modeBuf, sizeof(modeBuf));
					S21Bridge_OnConnAccept(mapBuf, modeBuf);
					int nOut = recvd;
					if (S21Bridge_RewritePollToDestChallenge04(
						reinterpret_cast<unsigned char*>(pollBuf),
						static_cast<int>(sizeof(pollBuf)), &nOut))
					{
						recvd = nOut;
					}
					else
					{
						recvd = 0;
						continue;
					}
				}
				else
				{
					recvd = 0;
					continue;
				}
			}
			else if (oobType == 0x4B && s_hsStage == BridgeHsStage::ConnectSent)
			{
				char szReason[128] = {};
				const size_t nBody = (recvd > 5) ? static_cast<size_t>(recvd - 5) : 0;
				if (nBody)
					Bridge_CopyBoundedCString(reinterpret_cast<const char*>(h + 5),
						nBody, szReason, sizeof(szReason));
				S21Bridge_OnConnReject(szReason[0] ? szReason : "CONNREJECT");
				recvd = 0;
				continue;
			}
			else if (oobType != 0x04 && oobType != 0x4B)
			{
				static volatile LONG s_oobDrop = 0;
				const LONG n = InterlockedIncrement(&s_oobDrop);
				if (n <= 8 || (n % 256) == 0)
					Warning(eDLL_T::ENGINE,
						"[BRIDGE-POLL] dropped OOB type 0x%02X\n", oobType);
				recvd = 0;
				continue;
			}
		}

		// Parse netchannel packets for subchannel state (nonce, seq, ack)
		if (!isOOB && recvd >= 9)
			S21Bridge_ParseS2CPacket(h, recvd);

		// Diagnostic logging
		{
			static long long s_typeLog = 0;
			if (++s_typeLog <= 30 || (s_typeLog % 5000) == 0)
			{
				const char* pktType = isOOB ? "OOB" : "netchan";
				SDK_Log("[BRIDGE-POLL] pkt #%lld: %s %d bytes hdr=%02x%02x%02x%02x %02x%02x%02x%02x\n",
					s_typeLog, pktType, recvd,
					h[0], h[1], h[2], h[3],
					recvd > 4 ? h[4] : 0, recvd > 5 ? h[5] : 0,
					recvd > 6 ? h[6] : 0, recvd > 7 ? h[7] : 0);
			}
		}

		if (s_connAcceptDone)
		{
			static int s_postAcceptInj = 0;
			if (++s_postAcceptInj <= 16)
				Msg(eDLL_T::ENGINE, "[BRIDGE] poll inject %d bytes oob=%d\n",
					recvd, isOOB ? 1 : 0);
		}

		gotPacket = true;
		break; // Exit drain loop -- inject this packet below
	}

	if (!gotPacket || recvd <= 0)
		return false;

	// ── Inject packet into S21 engine ──
	// S21 netpacket_s (0x90 stride): pData=+0x30, message=+0x38, size=+0x78,
	uint8_t* pktBase = reinterpret_cast<uint8_t*>(pInpacket);
	uint8_t* s21_pData = *reinterpret_cast<uint8_t**>(pktBase + 0x30);

	if (!s21_pData)
	{
		SDK_Log("[BRIDGE-POLL] WARNING: pData is NULL at S21 offset 0x30!\n");
		return false;
	}

	memcpy(s21_pData, pollBuf, recvd);
	*reinterpret_cast<int*>(pktBase + 0x7C) = recvd; // wiresize
	*reinterpret_cast<int*>(pktBase + 0x78) = recvd; // size

	// Set source address in S21 netadr_t layout (32 bytes, NOT S3's 24 bytes).
	// match: type(+0,4B)=3, addr(+4,16B), port(+20,2B), slot(+23,1B)
	{
		// S21 netadr_t occupies bytes 0-31 of netpacket_s
		memset(pktBase, 0, 32); // zero entire S21 netadr_t

		// type = 3 (NA_IP, used for all IPv4 connections)
		*reinterpret_cast<int*>(pktBase + 0) = 3;

		// IPv6-mapped IPv4 address:::ffff:A.B.C.D
		// in6_addr at +4: first 10 bytes = 0, bytes 10-11 = 0xFF, bytes 12-15 = IPv4
		memcpy(pktBase + 4, from.sin6_addr.u.Byte, 16); // full 16-byte in6_addr

		// port at +20 (network byte order, same as sockaddr)
		memcpy(pktBase + 20, &from.sin6_port, 2);

		// slot byte at +23 = 0 (default)
		// extra field at +24 = 0 (already zeroed)
	}

	static long long s_pollCount = 0;
	if (++s_pollCount <= 20 || (s_pollCount % 2000) == 0)
		SDK_Log("[BRIDGE-POLL] #%lld: injected %d bytes into engine\n", s_pollCount, recvd);
	InterlockedIncrement(&s_pollInjected);
	return true;
}
