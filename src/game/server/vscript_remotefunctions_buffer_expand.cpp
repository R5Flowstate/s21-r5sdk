//=============================================================================//
//
// Purpose: Expand the engine's Remote_RegisterClientFunction buffer.
//
// See vscript_remotefunctions_buffer_expand.h for the architecture overview.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier0/memory_patch.h"
#include "tier1/convar.h"
#include "vscript_remotefunctions_buffer_expand.h"
#include "game/shared/heap_canary.h"

#include <cstdint>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

//-----------------------------------------------------------------------------
// Runtime gate. Disable + restart to revert to stock S3 behavior
// (16 KB / 256-entry cap, fatal overflow when S21 scripts register >256
// client functions).
//-----------------------------------------------------------------------------
static ConVar sdk_remote_func_buffer_expand("sdk_remote_func_buffer_expand", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"S21-parity Remote_RegisterClientFunction buffer expansion (256 KB / "
	"2048 entries vs S3's 16 KB / 256). Required to load S21 scripts that "
	"register >256 client functions. Disable + restart for stock S3 limits.");

//-----------------------------------------------------------------------------
// S3 layout constants (where things live in r5apex_ds.exe's static.bss).
// All RVAs are module_base =.
//-----------------------------------------------------------------------------
namespace s3 {
	constexpr uint32_t kBufferBaseRVA = 0x027B28E0; // unk_1427B28E0
	constexpr uint32_t kBufferSize    = 0x4000;     // 16 KB
	constexpr uint32_t kEntryCap      = 0x100;      // 256 entries

	// Cap-check immediates inside 
	// 140e41b72 B8 00 40 00 00 mov eax, 4000h
	// 140e41b9a 41 BB 00 01 00 00 mov r11d, 100h
	constexpr uint32_t kByteCapImmRVA  = 0x00E41B73; // first byte of imm32
	constexpr uint32_t kEntryCapImmRVA = 0x00E41B9C; // first byte of imm32

	// 7-byte `LEA reg, [rip+disp32]` sites. disp_off = 3 (REX + opcode + ModRM).
	// Targets are buffer base (+0), base+4 (entry-payload start), base+5 (args).
	struct LeaSite { uint32_t rva; uint32_t targetOffset; const char* tag; };

	static constexpr LeaSite kLeaSites[] = {
		// === In (writer) ===
		{ 0x00E41BAA, 0, "writer.header (lea r11, base)" },
		{ 0x00E41BD4, 4, "writer.name   (lea rax, base+4)" },
		{ 0x00E41C04, 5, "writer.args   (lea rcx, base+5)" },
		// === In (CRC walker) ===
		{ 0x00E41F29, 0, "crc.walker    (lea rbx, base)" },
		// === In (variant entry) ===
		{ 0x00E4399C, 0, "variant.entry (lea rsi, base)" },
		// === In (clear/reset path) ===
		{ 0x00E4F0E5, 0, "reset.path    (lea rcx, base)" },
		// === In (diagnostic / dump) ===
		{ 0x00E51E26, 0, "diag.dump     (lea rdx, base)" },
	};
}

namespace ours {
	constexpr uint32_t kBufferSize = 0x40000; // 256 KB
	constexpr uint32_t kEntryCap   = 0x800;   // 2048 entries
}

//-----------------------------------------------------------------------------
// Public globals
//-----------------------------------------------------------------------------
uint8_t* g_pSdkRemoteFuncBuffer  = nullptr;
uint8_t* g_pOrigRemoteFuncBuffer = nullptr;

//-----------------------------------------------------------------------------
// Per-site validation + patch / unpatch.
//-----------------------------------------------------------------------------
static bool ApplyLeaSite(uintptr_t moduleBase, uintptr_t origBase, uintptr_t newBase,
						 const s3::LeaSite& site, bool patchInForward)
{
	// 7-byte LEA: REX (1) + opcode (1) + ModRM (1) + disp32 (4). disp_off = 3.
	uint8_t* pInstr = (uint8_t*)(moduleBase + site.rva);
	uint8_t* pDisp  = pInstr + 3;
	uintptr_t nextRIP = (uintptr_t)pDisp + 4;

	int32_t curDisp = *(int32_t*)pDisp;
	uintptr_t resolved = nextRIP + (intptr_t)curDisp;

	uintptr_t expected = patchInForward
		? (origBase + site.targetOffset)
		: (newBase  + site.targetOffset);

	if (resolved != expected)
	{
		Warning(eDLL_T::ENGINE,
			"[REMOTE-FUNC-EXPAND] LEA MISMATCH '%s' RVA=0x%X resolved=0x%llX expected=0x%llX (disp=0x%X)\n",
			site.tag, site.rva, (unsigned long long)resolved,
			(unsigned long long)expected, (unsigned)curDisp);
		return false;
	}

	uintptr_t newTarget = patchInForward
		? (newBase  + site.targetOffset)
		: (origBase + site.targetOffset);
	int64_t newDisp64 = (int64_t)newTarget - (int64_t)nextRIP;
	if (newDisp64 > INT32_MAX || newDisp64 < INT32_MIN)
	{
		Warning(eDLL_T::ENGINE,
			"[REMOTE-FUNC-EXPAND] LEA RANGE OVERFLOW '%s' RVA=0x%X newDisp=0x%llX\n",
			site.tag, site.rva, (unsigned long long)newDisp64);
		return false;
	}

	int32_t newDisp = (int32_t)newDisp64;
	if (!Mem_PatchCode(pDisp, &newDisp, 4))
	{
		Warning(eDLL_T::ENGINE,
			"[REMOTE-FUNC-EXPAND] LEA VirtualProtect failed '%s' RVA=0x%X\n",
			site.tag, site.rva);
		return false;
	}
	return true;
}

static bool ApplyImm32(uintptr_t moduleBase, uint32_t immRVA,
					   uint32_t expectedVal, uint32_t newVal, const char* tag,
					   bool patchInForward)
{
	uint8_t* pImm = (uint8_t*)(moduleBase + immRVA);
	uint32_t curImm = *(uint32_t*)pImm;

	uint32_t want   = patchInForward ? expectedVal : newVal;
	uint32_t target = patchInForward ? newVal      : expectedVal;
	if (curImm != want)
	{
		Warning(eDLL_T::ENGINE,
			"[REMOTE-FUNC-EXPAND] imm32 MISMATCH '%s' RVA=0x%X got=0x%X expected=0x%X\n",
			tag, immRVA, curImm, want);
		return false;
	}
	if (!Mem_PatchCode(pImm, &target, 4))
	{
		Warning(eDLL_T::ENGINE,
			"[REMOTE-FUNC-EXPAND] imm32 VirtualProtect failed '%s' RVA=0x%X\n",
			tag, immRVA);
		return false;
	}
	return true;
}

//-----------------------------------------------------------------------------
// VRemoteFuncBufferExpand
//-----------------------------------------------------------------------------
void VRemoteFuncBufferExpand::GetAdr(void) const
{
	if (g_pSdkRemoteFuncBuffer)
	{
		Msg(eDLL_T::ENGINE,
			"[REMOTE-FUNC-EXPAND] addresses: orig=0x%p new=0x%p (size=0x%X, cap=%u)\n",
			(void*)g_pOrigRemoteFuncBuffer, (void*)g_pSdkRemoteFuncBuffer,
			ours::kBufferSize, ours::kEntryCap);
	}
}

void VRemoteFuncBufferExpand::Detour(const bool bAttach) const
{
	const uintptr_t moduleBase = g_GameDll.GetModuleBase();

	if (bAttach)
	{
		if (!sdk_remote_func_buffer_expand.GetBool())
		{
			Msg(eDLL_T::ENGINE,
				"[REMOTE-FUNC-EXPAND] disabled by sdk_remote_func_buffer_expand=0 -- "
				"stock S3 limits (16 KB / 256 entries; S21 scripts will fail to register "
				"all client functions)\n");
			return;
		}

		g_pOrigRemoteFuncBuffer = (uint8_t*)(moduleBase + s3::kBufferBaseRVA);

		g_pSdkRemoteFuncBuffer = Mem_AllocNearModule(g_GameDll, ours::kBufferSize + HeapCanary::kTailBytes);
		if (!g_pSdkRemoteFuncBuffer)
		{
			Warning(eDLL_T::ENGINE,
				"[REMOTE-FUNC-EXPAND] FAILED to allocate %u-byte buffer within +-2GB of "
				"module base 0x%llX -- stock S3 limits in effect, S21 client-function "
				"registration will fail past entry 256\n",
				ours::kBufferSize, (unsigned long long)moduleBase);
			return;
		}
		HeapCanary::RegisterTail("remote-func-buffer", g_pSdkRemoteFuncBuffer, ours::kBufferSize);

		// VirtualAlloc with MEM_COMMIT already zeroes the region. Engine BSS
		// buffer starts zero, so the freshly-allocated heap buffer matches.
		Msg(eDLL_T::ENGINE,
			"[REMOTE-FUNC-EXPAND] buffer at 0x%p (size=0x%X, dist=%lld bytes from module)\n",
			(void*)g_pSdkRemoteFuncBuffer, ours::kBufferSize,
			(long long)((intptr_t)g_pSdkRemoteFuncBuffer - (intptr_t)moduleBase));

		const uintptr_t origBase = (uintptr_t)g_pOrigRemoteFuncBuffer;
		const uintptr_t newBase  = (uintptr_t)g_pSdkRemoteFuncBuffer;

		// All-or-nothing: reverse any applied LEA/imm on failure (ADIG pattern).
		const size_t nLea = sizeof(s3::kLeaSites) / sizeof(s3::kLeaSites[0]);
		size_t leaApplied = 0;
		bool allOk = true;

		for (size_t i = 0; i < nLea; ++i)
		{
			if (!ApplyLeaSite(moduleBase, origBase, newBase, s3::kLeaSites[i], true))
			{
				allOk = false;
				break;
			}
			++leaApplied;
		}

		bool byteCapOk = false;
		bool entryCapOk = false;
		if (allOk)
		{
			byteCapOk = ApplyImm32(moduleBase, s3::kByteCapImmRVA,
				s3::kBufferSize, ours::kBufferSize,
				"byte cap (mov eax,4000h)", true);
			if (!byteCapOk)
				allOk = false;
		}
		if (allOk)
		{
			entryCapOk = ApplyImm32(moduleBase, s3::kEntryCapImmRVA,
				s3::kEntryCap, ours::kEntryCap,
				"entry cap (mov r11d,100h)", true);
			if (!entryCapOk)
				allOk = false;
		}

		if (!allOk)
		{
			for (size_t i = 0; i < leaApplied; ++i)
				ApplyLeaSite(moduleBase, origBase, newBase, s3::kLeaSites[i], false);
			if (byteCapOk)
			{
				ApplyImm32(moduleBase, s3::kByteCapImmRVA,
					s3::kBufferSize, ours::kBufferSize,
					"byte cap (mov eax,4000h)", false);
			}
			if (entryCapOk)
			{
				ApplyImm32(moduleBase, s3::kEntryCapImmRVA,
					s3::kEntryCap, ours::kEntryCap,
					"entry cap (mov r11d,100h)", false);
			}

			VirtualFree(g_pSdkRemoteFuncBuffer, 0, MEM_RELEASE);
			g_pSdkRemoteFuncBuffer  = nullptr;
			g_pOrigRemoteFuncBuffer = nullptr;

			Warning(eDLL_T::ENGINE,
				"[REMOTE-FUNC-EXPAND] expansion OFF -- site failure after %zu/%zu LEA "
				"(byte cap %s, entry cap %s); rolled back all applied patches, "
				"stock S3 limits restored (16 KB / 256 entries)\n",
				leaApplied, nLea,
				byteCapOk ? "OK" : "FAIL",
				entryCapOk ? "OK" : "FAIL");
			return;
		}

		Msg(eDLL_T::ENGINE,
			"[REMOTE-FUNC-EXPAND] LEA sites: %zu OK (of %zu); imm32 caps: byte=OK entry=OK\n",
			leaApplied, nLea);
		Msg(eDLL_T::ENGINE,
			"[REMOTE-FUNC-EXPAND] expansion ACTIVE -- buffer 0x4000 -> 0x%X, "
			"cap 256 -> %u; registration room for ~%u client functions\n",
			ours::kBufferSize, ours::kEntryCap, ours::kEntryCap);
	}
	else
	{
		if (!g_pSdkRemoteFuncBuffer)
			return;

		const uintptr_t origBase = (uintptr_t)g_pOrigRemoteFuncBuffer;
		const uintptr_t newBase  = (uintptr_t)g_pSdkRemoteFuncBuffer;

		for (const s3::LeaSite& site : s3::kLeaSites)
			ApplyLeaSite(moduleBase, origBase, newBase, site, false);

		ApplyImm32(moduleBase, s3::kByteCapImmRVA,
				   s3::kBufferSize, ours::kBufferSize,
				   "byte cap (mov eax,4000h)", false);
		ApplyImm32(moduleBase, s3::kEntryCapImmRVA,
				   s3::kEntryCap, ours::kEntryCap,
				   "entry cap (mov r11d,100h)", false);

		VirtualFree(g_pSdkRemoteFuncBuffer, 0, MEM_RELEASE);
		g_pSdkRemoteFuncBuffer  = nullptr;
		g_pOrigRemoteFuncBuffer = nullptr;

		Msg(eDLL_T::ENGINE,
			"[REMOTE-FUNC-EXPAND] expansion REVERTED -- stock S3 layout restored, "
			"heap buffer freed\n");
	}
}
