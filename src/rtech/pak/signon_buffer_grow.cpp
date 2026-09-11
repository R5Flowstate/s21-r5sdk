//=============================================================================//
//
// Purpose: Relocate SignonInfo bf_write onto a +-2GB heap buffer. Destination half only
// (per-table scratch is stringtable_baseline_grow.cpp). Wire m_nDataBytes is 22-bit (0x3FFFFF).
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier1/convar.h"
#include "public/tier0/memaddr.h"
#include "public/tier0/tier0_iface.h"
#include "thirdparty/detours/include/detours.h"
#include "signon_buffer_grow.h"
#include "game/shared/heap_canary.h"

#include <cstdint>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

//-----------------------------------------------------------------------------
// 22-bit m_nDataBytes tops out at 0x3FFFFF; 0x400000 wraps to 0 on the wire.
//-----------------------------------------------------------------------------
static ConVar sdk_signon_buffer_size("sdk_signon_buffer_size", "4194303",
	FCVAR_DEVELOPMENTONLY | FCVAR_REPLICATED,
	"Capacity in bytes of the SignonInfo bf_write buffer (default engine cap "
	"is 786432). Range [786432, 4194303]; out-of-range values skip the patch.",
	true, 786432.0f, true, 4194303.0f);

//-----------------------------------------------------------------------------
// RVAs. r5apex.exe == r5apex_ds.exe at S3.
//-----------------------------------------------------------------------------
static constexpr uintptr_t kImageBase = 0x140000000ull;

// LEA rax, [rip+disp32] (48 8D 05 disp32), disp32 at +3.
static constexpr uintptr_t kLeaSite       = 0x140304c3bull;
// mov dword [rsp+disp8], imm32 (C7 44 24 28 imm32), imm32 at +4.
static constexpr uintptr_t kMovDwordSite  = 0x140304c78ull;
// mov qword [rsp+disp8], imm32 (48 C7 44 24 2C imm32), imm32 at +5.
static constexpr uintptr_t kMovQwordSite  = 0x140304c80ull;

static constexpr uint32_t kStockBytes = 0x000C0000;  // 786432
static constexpr uint32_t kStockBits  = 0x00600000;  // 6291456

static uint8_t* s_pNewSignonBuffer = nullptr;

//-----------------------------------------------------------------------------
static inline uint8_t* RvaToRuntime(uintptr_t preferredVa, uintptr_t actualBase)
{
	return reinterpret_cast<uint8_t*>(actualBase + (preferredVa - kImageBase));
}

static bool WriteBytes(void* addr, const void* data, size_t len)
{
	DWORD oldProt = 0;
	if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::ENGINE,
			"[signon-buffer] VirtualProtect failed @ %p (gle=%lu)\n",
			addr, GetLastError());
		return false;
	}
	memcpy(addr, data, len);
	VirtualProtect(addr, len, oldProt, &oldProt);
	FlushInstructionCache(GetCurrentProcess(), addr, len);
	return true;
}

//-----------------------------------------------------------------------------
// DataBlock sender diag: log bf_write cursor at entry. 14-byte FF25 trampoline.
//-----------------------------------------------------------------------------
static constexpr uintptr_t kDataBlockSendSite = 0x1403082E0ull;
// ConVar int value at *(ptr)+0x6C.
static constexpr uintptr_t kNetDataBlockConvarPtr = 0x141844F78ull;

using DataBlockSendFn = void (*)(uintptr_t pClient, uintptr_t pBfWrite);
static DataBlockSendFn s_origDataBlockSend = nullptr;
static uintptr_t s_netDataBlockConvarPtrRuntime = 0;

static void __fastcall DataBlockSendDiag(uintptr_t pClient, uintptr_t pBfWrite)
{
	static int s_logCount = 0;
	if (s_logCount++ < 30 && pBfWrite)
	{
		const uint32_t iCurBit = *reinterpret_cast<uint32_t*>(pBfWrite + 16);
		const uintptr_t pData  = *reinterpret_cast<uintptr_t*>(pBfWrite + 0);
		const uint32_t nBytes  = *reinterpret_cast<uint32_t*>(pBfWrite + 8);
		const uint32_t nBits   = *reinterpret_cast<uint32_t*>(pBfWrite + 12);
		const uint8_t  bOver   = *reinterpret_cast<uint8_t*>(pBfWrite + 20);

		int dbEnabled = -1;
		if (s_netDataBlockConvarPtrRuntime)
		{
			const uintptr_t cvarStruct = *reinterpret_cast<uintptr_t*>(s_netDataBlockConvarPtrRuntime);
			if (cvarStruct)
				dbEnabled = *reinterpret_cast<int*>(cvarStruct + 0x6C);
		}

		Msg(eDLL_T::ENGINE,
			"[signon-diag] DataBlockSend entry: client=%p bfwrite=%p "
			"m_pData=%p m_nDataBytes=%u m_nDataBits=%u m_iCurBit=%u "
			"m_bOverflowed=%u net_data_block_enabled=%d (#%d)\n",
			(void*)pClient, (void*)pBfWrite, (void*)pData,
			nBytes, nBits, iCurBit, bOver, dbEnabled, s_logCount);
	}

	// Slot 0, signon==8: dump early post-spawn bytes.
	static int s_wireDumpCount = 0;
	constexpr int kMaxWireDumps = 12;
	if (s_wireDumpCount < kMaxWireDumps && pClient && pBfWrite)
	{
		const int slot     = *reinterpret_cast<int*>(pClient + 0x10);
		const int signon   = *reinterpret_cast<int*>(pClient + 0x3B0);
		if (slot == 0 && signon == 8)
		{
			const int deltaAck   = *reinterpret_cast<int*>(pClient + 0x3C8);
			const int strTblAck  = *reinterpret_cast<int*>(pClient + 0x3CC);
			const int lastSnap   = *reinterpret_cast<int*>(pClient + 0x3D4);
			const int waitTick   = *reinterpret_cast<int*>(pClient + 0x59C);

			const uintptr_t pData = *reinterpret_cast<uintptr_t*>(pBfWrite + 0);
			const uint32_t iBit   = *reinterpret_cast<uint32_t*>(pBfWrite + 16);
			const uint32_t numBytes = (iBit + 7) / 8;

			const uint32_t dumpBytes = numBytes < 384u ? numBytes : 384u;

			const int seq = ++s_wireDumpCount;
			Warning(eDLL_T::SERVER,
				"[WIRE-DUMP] #%d slot=%d signon=%d deltaAck=%d strTblAck=%d "
				"lastSnap=%d waitTick=%d  bytes=%u (showing %u)\n",
				seq, slot, signon, deltaAck, strTblAck,
				lastSnap, waitTick, numBytes, dumpBytes);

			if (pData && dumpBytes > 0)
			{
				const uint8_t* p = reinterpret_cast<const uint8_t*>(pData);
				constexpr uint32_t kRow = 32;
				char rowbuf[kRow * 3 + 8];
				for (uint32_t off = 0; off < dumpBytes; off += kRow)
				{
					const uint32_t end =
						(off + kRow > dumpBytes) ? dumpBytes : (off + kRow);
					uint32_t pos = 0;
					for (uint32_t i = off; i < end; ++i)
					{
						pos += static_cast<uint32_t>(
							snprintf(rowbuf + pos, sizeof(rowbuf) - pos,
								"%02X ", p[i]));
						if (pos >= sizeof(rowbuf)) break;
					}
					Warning(eDLL_T::SERVER,
						"[WIRE-DUMP]   %04X: %s\n", off, rowbuf);
				}
			}
		}
	}

	if (s_origDataBlockSend)
		s_origDataBlockSend(pClient, pBfWrite);
}

static void SignonBufferGrow_InstallDiag(uintptr_t actualBase)
{
	static bool s_installed = false;
	if (s_installed) return;

	s_netDataBlockConvarPtrRuntime =
		actualBase + (kNetDataBlockConvarPtr - kImageBase);

	uint8_t* target = reinterpret_cast<uint8_t*>(
		actualBase + (kDataBlockSendSite - kImageBase));

	// Detour transaction is already active. Patch first 7 prologue bytes (before the RIP-rel load).
	void* tramp = nullptr;
	{
		const intptr_t window = 0x7FFFFFFFLL - 4096;
		const SIZE_T stride = 64ull * 1024ull * 1024ull;
		const uintptr_t targetU = reinterpret_cast<uintptr_t>(target);
		for (intptr_t off = stride; off <= window && !tramp; off += stride)
		{
			for (int sign = 0; sign < 2 && !tramp; ++sign)
			{
				const intptr_t signed_off = sign ? -off : off;
				const uintptr_t hint = targetU + signed_off;
				const uintptr_t aligned = hint & ~static_cast<uintptr_t>(0xFFFF);
				void* candidate = VirtualAlloc(
					reinterpret_cast<LPVOID>(aligned),
					4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
				if (candidate) tramp = candidate;
			}
		}
	}
	if (!tramp)
	{
		Warning(eDLL_T::ENGINE,
			"[signon-diag] could not allocate trampoline; skipping diag hook\n");
		return;
	}

	uint8_t* tb = reinterpret_cast<uint8_t*>(tramp);

	// BlockA: indirect jmp to the diag handler.
	tb[0] = 0xFF; tb[1] = 0x25;
	*reinterpret_cast<uint32_t*>(tb + 2) = 0;
	*reinterpret_cast<uint64_t*>(tb + 6) =
		reinterpret_cast<uint64_t>(&DataBlockSendDiag);

	// BlockB at +32.
	constexpr size_t kBlockBOffset = 32;
	uint8_t* blockB = tb + kBlockBOffset;
	// Original 7 bytes.
	memcpy(blockB, target, 7);
	// jmp back to target+7.
	blockB[7]  = 0xFF; blockB[8] = 0x25;
	*reinterpret_cast<uint32_t*>(blockB + 9) = 0;
	*reinterpret_cast<uint64_t*>(blockB + 13) =
		reinterpret_cast<uint64_t>(target + 7);

	s_origDataBlockSend = reinterpret_cast<DataBlockSendFn>(blockB);

	// Bail if the prologue does not match.
	static const uint8_t kExpectedPrologue[7] = {
		0x40, 0x53, 0x57, 0x48, 0x83, 0xEC, 0x38
	};
	if (memcmp(target, kExpectedPrologue, 7) != 0)
	{
		Warning(eDLL_T::ENGINE,
			"[signon-diag] target prologue mismatch @ %p (got "
			"%02X %02X %02X %02X %02X %02X %02X); skipping hook\n",
			target, target[0], target[1], target[2], target[3],
			target[4], target[5], target[6]);
		return;
	}

	// E9 rel32 to BlockA + 90 90.
	const int64_t rel = reinterpret_cast<int64_t>(tb) -
						(reinterpret_cast<int64_t>(target) + 5);
	if (rel < INT32_MIN || rel > INT32_MAX)
	{
		Warning(eDLL_T::ENGINE,
			"[signon-diag] trampoline out of rel32 range (delta=%lld); skipping\n",
			(long long)rel);
		return;
	}
	uint8_t patchBuf[7];
	patchBuf[0] = 0xE9;
	*reinterpret_cast<int32_t*>(patchBuf + 1) = static_cast<int32_t>(rel);
	patchBuf[5] = 0x90;
	patchBuf[6] = 0x90;

	if (!WriteBytes(target, patchBuf, 7))
	{
		Warning(eDLL_T::ENGINE,
			"[signon-diag] failed to patch target @ %p; diag hook not installed\n",
			target);
		return;
	}

	s_installed = true;
	Msg(eDLL_T::ENGINE,
		"[signon-diag] hook installed: target=%p tramp=%p diag=%p\n",
		target, tramp, (void*)&DataBlockSendDiag);
}

void VSignonBufferGrow::Detour(const bool bAttach) const
{
	if (!bAttach) return;

	const int targetBytes = sdk_signon_buffer_size.GetInt();
	if (targetBytes < (int)kStockBytes || targetBytes > 0x3FFFFF)
	{
		Warning(eDLL_T::ENGINE,
			"[signon-buffer] target size %d out of range [%u, %u]; skipping. "
			"Stock 786432-byte buffer retained.\n",
			targetBytes, kStockBytes, 0x3FFFFFu);
		return;
	}
	if (targetBytes == (int)kStockBytes)
	{
		Msg(eDLL_T::ENGINE,
			"[signon-buffer] target equals stock cap; nothing to patch.\n");
		return;
	}

	const uintptr_t actualBase =
		reinterpret_cast<uintptr_t>(GetModuleHandleA(NULL));
	if (!actualBase)
	{
		Warning(eDLL_T::ENGINE,
			"[signon-buffer] GetModuleHandleA(NULL) returned 0; aborting.\n");
		return;
	}

	// Allocate within +-2GB of the LEA. VirtualAlloc hints, 64MB stride.
	if (!s_pNewSignonBuffer)
	{
		const SIZE_T allocSize = static_cast<SIZE_T>(targetBytes) + HeapCanary::kTailBytes;
		const uintptr_t leaSite = actualBase + (kLeaSite - kImageBase);
		// Leave room for the buffer inside the +-2GB window.
		const intptr_t window = 0x7FFFFFFFLL - static_cast<intptr_t>(allocSize);
		const SIZE_T stride = 64ull * 1024ull * 1024ull;  // 64MB

		void* region = nullptr;
		// Alternate +/- 64MB hops from the LEA site.
		for (intptr_t off = stride; off <= window && !region; off += stride)
		{
			for (int sign = 0; sign < 2 && !region; ++sign)
			{
				const intptr_t signed_off = sign ? -off : off;
				const uintptr_t hint = leaSite + signed_off;
				// 64KB allocation granularity.
				const uintptr_t aligned = hint & ~static_cast<uintptr_t>(0xFFFF);
				void* candidate = VirtualAlloc(
					reinterpret_cast<LPVOID>(aligned),
					allocSize,
					MEM_RESERVE | MEM_COMMIT,
					PAGE_READWRITE);
				if (!candidate) continue;
				// Entire buffer must sit within +-2GB of the LEA.
				const intptr_t lo = static_cast<intptr_t>(
					reinterpret_cast<uintptr_t>(candidate)) -
					static_cast<intptr_t>(leaSite);
				const intptr_t hi = lo + static_cast<intptr_t>(allocSize);
				if (lo >= INT32_MIN && hi <= INT32_MAX)
				{
					region = candidate;
				}
				else
				{
					VirtualFree(candidate, 0, MEM_RELEASE);
				}
			}
		}
		if (!region)
		{
			Warning(eDLL_T::ENGINE,
				"[signon-buffer] could not allocate %d bytes within +-2GB of "
				"LEA %p; aborting (stock buffer retained).\n",
				targetBytes, (void*)leaSite);
			return;
		}
		memset(region, 0, allocSize);
		s_pNewSignonBuffer = reinterpret_cast<uint8_t*>(region);
		HeapCanary::RegisterTail("signon-buffer", s_pNewSignonBuffer,
			static_cast<size_t>(targetBytes));
	}

	// Validate all three sites before writing.
	uint8_t* pLea = RvaToRuntime(kLeaSite, actualBase);
	uint8_t* pMovD = RvaToRuntime(kMovDwordSite, actualBase);
	uint8_t* pMovQ = RvaToRuntime(kMovQwordSite, actualBase);

	const uint32_t curMovDImm = *reinterpret_cast<uint32_t*>(pMovD + 4);
	const uint32_t curMovQImm = *reinterpret_cast<uint32_t*>(pMovQ + 5);
	const int32_t stockLeaDisp = *reinterpret_cast<int32_t*>(pLea + 3);

	bool valid = true;
	if (pLea[0] != 0x48 || pLea[1] != 0x8D || pLea[2] != 0x05)
	{
		Warning(eDLL_T::ENGINE,
			"[signon-buffer] LEA signature mismatch @ %p "
			"(got %02X %02X %02X, expected 48 8D 05); aborting\n",
			pLea, pLea[0], pLea[1], pLea[2]);
		valid = false;
	}
	if (pMovD[0] != 0xC7 || pMovD[1] != 0x44 || pMovD[2] != 0x24 || curMovDImm != kStockBytes)
	{
		Warning(eDLL_T::ENGINE,
			"[signon-buffer] mov-dword signature mismatch @ %p "
			"(got %02X %02X %02X imm=0x%X, expected C7 44 24 imm=0x%X)\n",
			pMovD, pMovD[0], pMovD[1], pMovD[2], curMovDImm, kStockBytes);
		valid = false;
	}
	if (pMovQ[0] != 0x48 || pMovQ[1] != 0xC7 || pMovQ[2] != 0x44 || pMovQ[3] != 0x24
		|| curMovQImm != kStockBits)
	{
		Warning(eDLL_T::ENGINE,
			"[signon-buffer] mov-qword signature mismatch @ %p "
			"(got %02X %02X %02X %02X imm=0x%X, expected 48 C7 44 24 imm=0x%X)\n",
			pMovQ, pMovQ[0], pMovQ[1], pMovQ[2], pMovQ[3], curMovQImm, kStockBits);
		valid = false;
	}

	const uintptr_t newBufAddr =
		reinterpret_cast<uintptr_t>(s_pNewSignonBuffer);
	const int64_t leaDelta = static_cast<int64_t>(newBufAddr) -
		static_cast<int64_t>(reinterpret_cast<uintptr_t>(pLea + 7));
	if (leaDelta < INT32_MIN || leaDelta > INT32_MAX)
	{
		Warning(eDLL_T::ENGINE,
			"[signon-buffer] LEA delta %lld out of int32 range @ %p; aborting\n",
			(long long)leaDelta, pLea);
		valid = false;
	}

	if (!valid)
	{
		VirtualFree(s_pNewSignonBuffer, 0, MEM_RELEASE);
		s_pNewSignonBuffer = nullptr;
		Warning(eDLL_T::ENGINE,
			"[signon-buffer] expansion OFF -- preflight failed, stock buffer retained\n");
		return;
	}

	const int32_t newLeaDisp = static_cast<int32_t>(leaDelta);
	const uint32_t newMovD = static_cast<uint32_t>(targetBytes);
	const uint32_t newMovQ = static_cast<uint32_t>(targetBytes) * 8u;

	bool leaOk = WriteBytes(pLea + 3, &newLeaDisp, 4);
	bool movDOk = leaOk && WriteBytes(pMovD + 4, &newMovD, 4);
	bool movQOk = movDOk && WriteBytes(pMovQ + 5, &newMovQ, 4);

	if (!leaOk || !movDOk || !movQOk)
	{
		// Roll back any sites that landed.
		if (leaOk)
			WriteBytes(pLea + 3, &stockLeaDisp, 4);
		if (movDOk)
			WriteBytes(pMovD + 4, &kStockBytes, 4);
		// Restore movD if movQ failed after it.
		if (movDOk && !movQOk)
			WriteBytes(pMovD + 4, &kStockBytes, 4);

		VirtualFree(s_pNewSignonBuffer, 0, MEM_RELEASE);
		s_pNewSignonBuffer = nullptr;
		Warning(eDLL_T::ENGINE,
			"[signon-buffer] expansion OFF -- write failure (lea=%d movD=%d movQ=%d); "
			"rolled back, stock buffer retained\n",
			leaOk ? 1 : 0, movDOk ? 1 : 0, movQOk ? 1 : 0);
		return;
	}

	Msg(eDLL_T::ENGINE,
		"[signon-buffer] patch ACTIVE: 3/3 sites; new buffer @ %p, capacity=%d bytes (%d bits)\n",
		(void*)s_pNewSignonBuffer, targetBytes, targetBytes * 8);

	// DataBlock sender diag.
	SignonBufferGrow_InstallDiag(actualBase);
}
