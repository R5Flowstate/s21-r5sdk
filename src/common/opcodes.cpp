#if defined(CLIENT_DLL)
/*-----------------------------------------------------------------------------
 * S21 Client SDK - Runtime patches
 *
 * Binary patches applied after detour init. These are S21-specific patches
 * that cannot be done via detours (e.g., instruction-level changes).
 *
 * NOTE: EOS and Origin init are already patched by the patcher tool.
 * This file handles any additional runtime patches needed.
 *-----------------------------------------------------------------------------*/

#include "core/stdafx.h"
#include "common/opcodes.h"
#include "tier1/cmd.h"
#include "tier0/memory_patch.h"
#include "tier0/memvalidate.h"
#include "tier0/commandline.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"

extern void SDK_LogDevFile(const char* fmt, ...);

// Locate the severity-patch site. v_SQVM_CompileError is null-checked by the
// caller; Mem_InModule refuses a non-image base so FindPatternSelf never walks
// from address 0.
static uintptr_t FindSQVMCompileErrorSeveritySite()
{
	if (!v_SQVM_CompileError)
		return 0;
	if (!Mem_InModule(g_GameDll, v_SQVM_CompileError, 16))
		return 0;

	return CMemory(v_SQVM_CompileError)
		.Offset(0x0)
		.FindPatternSelf("41 B0 01", CMemory::Direction::DOWN, 400)
		.GetPtr();
}

static bool TryPatchSQVMCompileErrorSeverity()
{
	const uintptr_t site = FindSQVMCompileErrorSeveritySite();
	if (!site)
		return false;

	const uint8_t patch[3] = { 0x41, 0xB0, 0x00 };
	return Mem_PatchCode(reinterpret_cast<void*>(site), patch, sizeof(patch));
}


void RuntimePtc_Init()
{
	// Reduce script error severity from fatal (1) to warning (0). Same
	// patch as S3 SDK -- prevents script errors from crashing. On S21
	// v_SQVM_CompileError is resolved by VSquirrelS21Core's IDetour::GetFun
	// pass; if that pattern shifts on a future build the pointer is null
	// and FindPatternSelf would walk from address 0 and AV. Guard hard.
	if (!v_SQVM_CompileError)
	{
		SDK_LogDevFile("[RuntimePtc] SQVM_CompileError unresolved -- skipping severity patch\n");
	}
	else if (TryPatchSQVMCompileErrorSeverity())
	{
		SDK_LogDevFile("[RuntimePtc] SQVM_CompileError severity patch applied at %p\n",
			(void*)v_SQVM_CompileError);
	}
	else
	{
		SDK_LogDevFile("[RuntimePtc] SQVM_CompileError severity patch FAILED "
			"(pattern '41 B0 01' not present near %p)\n", (void*)v_SQVM_CompileError);
	}

	// -----------------------------------------------------------------
	// S3-on-S21 bridge: redirect engine vtable[40] -> vtable[39] in
	// C_Player::SetLocalView and C_Player::PostDataUpdate

	// On S3-hosted S21 client, only vtable[39] matches the local slot.
	// Patch three SetLocalView/PostDataUpdate sites (vtable[40]->[39],
	// one-byte disp 0x40->0x38) so LocalClientPlayer and LocalViewPlayer both arm.


	//
	// Note: Module_FindPattern uses a compile-time SigMaker, so the
	// pattern string MUST be a string literal at the call site -- can't
	// be looped via a struct (otherwise C2131: non-constexpr).
	int redirectsApplied = 0;

	// Site 1: (C_Player::SetLocalView) -- single vtable[40] call
	// followed by xor ebx,ebx; lea rbp,; cmp eax,[rdi+20h]; jnz
	{
		CMemory hit = Module_FindPattern(g_GameDll,
			"FF 90 40 01 00 00 33 DB 48 8D 2D ?? ?? ?? ?? 3B 47 20 75");
		if (hit.GetPtr())
		{
			const uint8_t before = hit.Offset(2).GetValue<uint8_t>();
			hit.Offset(2).Patch({ 0x38 });
			const uint8_t after = hit.Offset(2).GetValue<uint8_t>();
			SDK_LogDevFile("[BRIDGE-FIX] vtable[40]->vtable[39] redirect at %p (C_Player::SetLocalView) "
				"before=0x%02X after=0x%02X expected=0x38\n",
				(void*)(hit.GetPtr() + 2), before, after);
			++redirectsApplied;
		}
		else
		{
			SDK_LogDevFile("[BRIDGE-FIX] vtable[40] pattern NOT FOUND for C_Player::SetLocalView -- "
				"local-player UI gates may stay parked.\n");
		}
	}

	// Site 2: (C_Player::PostDataUpdate) first vtable[40] call.
	// Pattern includes the second adjacent call, which uniques this site.
	{
		CMemory hit = Module_FindPattern(g_GameDll,
			"FF 90 40 01 00 00 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 40 01 00 00");
		if (hit.GetPtr())
		{
			const uint8_t before = hit.Offset(2).GetValue<uint8_t>();
			hit.Offset(2).Patch({ 0x38 });
			const uint8_t after = hit.Offset(2).GetValue<uint8_t>();
			SDK_LogDevFile("[BRIDGE-FIX] vtable[40]->vtable[39] redirect at %p (C_Player::PostDataUpdate call 1) "
				"before=0x%02X after=0x%02X expected=0x38\n",
				(void*)(hit.GetPtr() + 2), before, after);
			++redirectsApplied;
		}
		else
		{
			SDK_LogDevFile("[BRIDGE-FIX] vtable[40] pattern NOT FOUND for C_Player::PostDataUpdate call 1 -- "
				"local-player UI gates may stay parked.\n");
		}
	}

	// Site 3: (C_Player::PostDataUpdate) second vtable[40] call.
	// Pattern includes lea rbx, + lea r10 + cmp [rdi+20h]; jnz,
	// uniquing this site.
	{
		CMemory hit = Module_FindPattern(g_GameDll,
			"FF 90 40 01 00 00 48 8D 1D ?? ?? ?? ?? 4C 8D 15 ?? ?? ?? ?? 3B 47 20 75");
		if (hit.GetPtr())
		{
			const uint8_t before = hit.Offset(2).GetValue<uint8_t>();
			hit.Offset(2).Patch({ 0x38 });
			const uint8_t after = hit.Offset(2).GetValue<uint8_t>();
			SDK_LogDevFile("[BRIDGE-FIX] vtable[40]->vtable[39] redirect at %p (C_Player::PostDataUpdate call 2) "
				"before=0x%02X after=0x%02X expected=0x38\n",
				(void*)(hit.GetPtr() + 2), before, after);
			++redirectsApplied;
		}
		else
		{
			SDK_LogDevFile("[BRIDGE-FIX] vtable[40] pattern NOT FOUND for C_Player::PostDataUpdate call 2 -- "
				"local-player UI gates may stay parked.\n");
		}
	}

	if (redirectsApplied == 3)
	{
		SDK_LogDevFile("[BRIDGE-FIX] All 3 vtable redirects applied -- "
			"OnEntityCreation pipeline should now fire on bridge.\n");
	}
	else
	{
		SDK_LogDevFile("[BRIDGE-FIX] WARNING: only %d/3 vtable redirects applied. "
			"The bridge will partially park UI gates.\n", redirectsApplied);
	}

	// FilterTime: fps_max<=0 becomes fps_absolute_max (300), then min-clamps
	// to it. jb->jmp keeps 0 as unlimited; jbe->jmp skips the min.
	{
		CMemory hit = Module_FindPattern(g_GameDll,
			"44 0F 2F CE 48 8B 05 ?? ?? ?? ?? 72 05 F3 0F 10 70 60 "
			"41 0F 2F F1 F3 44 0F 10 15 ?? ?? ?? ?? 0F 86 ?? ?? ?? ?? "
			"F3 0F 10 40 60 0F 2F F0 76 03 0F 28 F0");
		if (!hit.GetPtr())
		{
			SDK_LogDevFile("[FPS-CAP] FilterTime fps_absolute_max clamp NOT FOUND\n");
		}
		else
		{
			const uint8_t jb = hit.Offset(11).GetValue<uint8_t>();
			const uint8_t jbe = hit.Offset(45).GetValue<uint8_t>();
			if (jb == 0xEB && jbe == 0xEB)
			{
				SDK_LogDevFile("[FPS-CAP] FilterTime fps_absolute_max clamp already patched at %p\n",
					(void*)hit.GetPtr());
			}
			else if (jb == 0x72 && jbe == 0x76)
			{
				hit.Offset(11).Patch({ 0xEB });
				hit.Offset(45).Patch({ 0xEB });
				SDK_LogDevFile("[FPS-CAP] FilterTime fps_absolute_max clamp nuked at %p\n",
					(void*)hit.GetPtr());
			}
			else
			{
				SDK_LogDevFile("[FPS-CAP] FilterTime clamp bytes unexpected at %p (jb=%02X jbe=%02X)\n",
					(void*)hit.GetPtr(), jb, jbe);
			}
		}
	}

	// Letterbox: the letterbox rect writer zeroes the rect, then bars the frame
	// whenever the window aspect is below mat_letterbox_aspect_threshold (1.59)
	// -- so every 4:3 / 5:4 / 16:10 window gets bars. Flipping one jcc to jmp
	// skips the narrow branch and leaves the rect zeroed, which is the full
	// window. DX11 has the routine twice (standalone + inlined into resize) and
	// the whole body sits behind 'building_cubemaps == 0'; DX12 has one out-of-line
	// routine where the narrow branch is a jbe, and its ultrawide branch is left as is.
	{
		int nLetterboxHits = 0;
		auto nukeLetterboxGate = [&nLetterboxHits](const char* pszName, CMemory hit, const int nJccOffset)
		{
			if (!hit.GetPtr())
				return;

			nLetterboxHits++;
			const uint8_t jcc = hit.Offset(nJccOffset).GetValue<uint8_t>();
			if (jcc == 0xEB)
				SDK_LogDevFile("[LETTERBOX] %s gate already patched at %p\n", pszName, (void*)hit.GetPtr());
			else if (jcc == 0x75 || jcc == 0x76)
			{
				hit.Offset(nJccOffset).Patch({ 0xEB });
				SDK_LogDevFile("[LETTERBOX] %s gate nuked at %p\n", pszName, (void*)hit.GetPtr());
			}
			else
				SDK_LogDevFile("[LETTERBOX] %s gate bytes unexpected at %p (jcc=%02X)\n", pszName, (void*)hit.GetPtr(), jcc);
		};

		nukeLetterboxGate("dx11 standalone", Module_FindPattern(g_GameDll,
			"33 C0 48 89 05 ?? ?? ?? ?? 89 05 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 83 78 ?? ??"), 26);

		nukeLetterboxGate("dx11 resize", Module_FindPattern(g_GameDll,
			"48 8B 05 ?? ?? ?? ?? 33 F6 8B 7D ?? 8B CE 48 8B 5D ?? 44 8B CE 89 35 ?? ?? ?? ?? "
			"8B D6 89 0D ?? ?? ?? ?? 44 8B C6 89 35 ?? ?? ?? ?? 39 70 ??"), 47);

		nukeLetterboxGate("dx12", Module_FindPattern(g_GameDll,
			"F3 0F 10 41 60 F3 48 0F 2A D0 48 8B 05 ?? ?? ?? ?? F3 0F 10 60 60 0F 28 D9 "
			"48 8B 05 ?? ?? ?? ?? F3 0F 5E DA 0F 2F C3 76"), 39);

		if (nLetterboxHits == 0)
			SDK_LogDevFile("[LETTERBOX] no gate found -- bars stay on narrow windows\n");
	}

	// The KeyValues symbol WIDESCREEN_16_9 is computed as (w/h > 1.77) at device
	// init and on every resize, and .res/.menu layouts branch on it. Below 16:9
	// that selects the legacy narrow layouts, which Apex never ships art for.
	// -sdk_ui_force_widescreen replaces the setnbe with mov r8b,1 at both sites
	// so 4:3 and 16:10 keep the authored widescreen layouts.
	// DX12 already hardcodes the symbol to 1 at both sites, so neither pattern hits there.
	if (CommandLine() && CommandLine()->CheckParm("-sdk_ui_force_widescreen"))
	{
		auto forceWidescreenSymbol = [](const char* pszName, CMemory hit, const int nSetOffset)
		{
			if (!hit.GetPtr())
			{
				SDK_LogDevFile("[LETTERBOX] %s WIDESCREEN_16_9 site NOT FOUND\n", pszName);
				return;
			}

			const uint8_t op = hit.Offset(nSetOffset).GetValue<uint8_t>();
			if (op == 0x41)
			{
				// setnbe r8b (41 0F 97 C0) -> mov r8b, 1 (41 B0 01) + nop
				hit.Offset(nSetOffset).Patch({ 0x41, 0xB0, 0x01, 0x90 });
				SDK_LogDevFile("[LETTERBOX] %s WIDESCREEN_16_9 forced at %p\n", pszName, (void*)hit.GetPtr());
			}
			else
				SDK_LogDevFile("[LETTERBOX] %s WIDESCREEN_16_9 bytes unexpected at %p (op=%02X)\n", pszName, (void*)hit.GetPtr(), op);
		};

		forceWidescreenSymbol("init", Module_FindPattern(g_GameDll,
			"F3 48 0F 2A C8 8B C1 48 8D 0D ?? ?? ?? ?? F3 48 0F 2A C0 48 8B 05 ?? ?? ?? ?? "
			"F3 0F 5E C8 0F 2F 0D ?? ?? ?? ?? 41 0F 97 C0"), 37);

		forceWidescreenSymbol("resize", Module_FindPattern(g_GameDll,
			"F3 48 0F 2A C8 8B C7 F3 48 0F 2A C0 48 8B 05 ?? ?? ?? ?? "
			"F3 0F 5E C8 0F 2F 0D ?? ?? ?? ?? 41 0F 97 C0"), 30);
	}

	// Exclusive fullscreen enumerates DXGI modes and, on a miss, writes
	// gDefault (1280x720 on 16:9). 4:3 is never in that list. Keep the
	// requested size and set windowed+noborder (bit0|bit1 at config+0x34).
	// DX11 only: DX12 keeps separate windowed/fullscreen sizes and a different
	// flag layout, so it is handled by the block below; the launcher already
	// remaps unlisted fullscreen to borderless at the requested size.
	CMemory miss = Module_FindPattern(g_GameDll,
		"FF C7 3B FE 7C ?? 8B 05 ?? ?? ?? ?? 8B 0D ?? ?? ?? ??");
	if (!miss.GetPtr())
	{
		// DX12 exclusive-miss: load/load/store/store with the sizes at the
		// same +0x1C/+0x20 slots, but the flag byte lives at +0x3C: bit0
		// exclusive, bit1 (0x02) windowed, bit2 (0x04) noborder. The mode
		// selector consumes bit0/bit2 the same way DX11 consumes its +0x34
		// bits, and entry routes on 0x02 to the windowed path. Keep the
		// requested WxH, force windowed|noborder, and rejoin the windowed
		// floor check so the desktop clamp still runs: that clamp is what
		// saves a 1920x1200 request on a 1920x1080 desktop. Only attempted
		// when the DX11 shape missed, so each exe patches exactly one site.
		CMemory miss12 = Module_FindPattern(g_GameDll,
			"8B 0D ?? ?? ?? ?? 8B C1 8B 15 ?? ?? ?? ?? 89 53 1C 89 4B 20 E9 ?? ?? ?? ??");
		CMemory floor12 = Module_FindPattern(g_GameDll,
			"8B 43 20 3D ?? ?? 00 00 73 ?? 8B 0D ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 89 4B 1C 89 43 20 89 4B 34 89 43 38");
		if (!miss12.GetPtr() || !floor12.GetPtr())
			SDK_LogDevFile("[VIDMODE] dx12 exclusive-miss store NOT FOUND -- unlisted -fullscreen stays 720p\n");
		else
		{
			CMemory store12 = miss12.Offset(14);
			const int64_t nRel = (int64_t)floor12.GetPtr() - ((int64_t)store12.GetPtr() + 9);
			if (store12.GetValue<uint8_t>() != 0x89
				|| store12.Offset(3).GetValue<uint8_t>() != 0x89
				|| store12.Offset(6).GetValue<uint8_t>() != 0xE9
				|| nRel < INT32_MIN || nRel > INT32_MAX)
				SDK_LogDevFile("[VIDMODE] dx12 exclusive-miss bytes unexpected at %p\n", (void*)store12.GetPtr());
			else
			{
				// or [rbx+3Ch],6 (4B) + jmp rel32 to the floor check (5B) + nop nop (2B).
				const int32_t nJmp = (int32_t)nRel;
				const std::vector<uint8_t> vPatch =
				{
					0x80, 0x4B, 0x3C, 0x06,
					0xE9,
					(uint8_t)(nJmp & 0xFF), (uint8_t)((nJmp >> 8) & 0xFF),
					(uint8_t)((nJmp >> 16) & 0xFF), (uint8_t)((nJmp >> 24) & 0xFF),
					0x66, 0x90
				};
				store12.Patch(vPatch);
				SDK_LogDevFile("[VIDMODE] dx12 exclusive-miss keeps requested size at %p (rejoins floor)\n",
					(void*)store12.GetPtr());
			}
		}
	}
	else
	{
		CMemory store = miss.Offset(18);
		const uint8_t op = store.GetValue<uint8_t>();
		if (op == 0x80)
			SDK_LogDevFile("[VIDMODE] exclusive-miss already patched at %p\n", (void*)store.GetPtr());
		else if (op == 0x89
			&& store.Offset(8).GetValue<uint8_t>() == 0xE9
			&& store.Offset(9).GetValue<uint32_t>() == 0x97)
		{
			// jmp moves 1 byte earlier, rel32 0x97 -> 0x98; same target.
			store.Patch({ 0x80, 0x4B, 0x34, 0x03, 0x8B, 0x43, 0x20,
				0xE9, 0x98, 0x00, 0x00, 0x00, 0x90 });
			SDK_LogDevFile("[VIDMODE] exclusive-miss keeps requested size at %p\n",
				(void*)store.GetPtr());
		}
		else
			SDK_LogDevFile("[VIDMODE] exclusive-miss bytes unexpected at %p (op=%02X)\n",
				(void*)store.GetPtr(), op);
	}

	// Windowed path rejects height < 720. 800x600 is a 4:3 preset; 320 is
	// the launcher clamp.
	{
		CMemory floor = Module_FindPattern(g_GameDll,
			"8B 43 20 3D ?? ?? 00 00 73 17");
		if (!floor.GetPtr())
			floor = Module_FindPattern(g_GameDll,
				"8B 43 20 3D ?? ?? 00 00 73 ?? 8B 0D ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 89 4B 1C 89 43 20 89 4B 34 89 43 38");
		if (!floor.GetPtr())
			SDK_LogDevFile("[VIDMODE] windowed height floor NOT FOUND\n");
		else
		{
			const uint32_t imm = floor.Offset(4).GetValue<uint32_t>();
			if (imm == 320)
				SDK_LogDevFile("[VIDMODE] windowed height floor already 320 at %p\n",
					(void*)floor.GetPtr());
			else if (imm == 720)
			{
				floor.Offset(4).Patch({ 0x40, 0x01, 0x00, 0x00 });
				SDK_LogDevFile("[VIDMODE] windowed height floor 720->320 at %p\n",
					(void*)floor.GetPtr());
			}
			else
				SDK_LogDevFile("[VIDMODE] windowed height floor unexpected at %p (imm=%u)\n",
					(void*)floor.GetPtr(), imm);
		}
	}
}

#else // !CLIENT_DLL
/*-----------------------------------------------------------------------------
 * _opcodes.cpp
 *-----------------------------------------------------------------------------*/

#include "core/stdafx.h"
#include "common/opcodes.h"
//#include "common/netmessages.h"
//#include "engine/cmodel_bsp.h"
//#include "engine/host.h"
//#include "engine/host_cmd.h"
//#include "engine/gl_screen.h"
//#include "engine/gl_matsysiface.h"
//#include "engine/matsys_interface.h"
//#include "engine/modelloader.h"
#include "engine/server/sv_main.h"
//#include "engine/client/cl_main.h"
//#include "engine/client/client.h"
//#include "engine/client/clientstate.h"
//#include "engine/client/cdll_engine_int.h"
//#include "engine/sys_getmodes.h"
//#include "engine/sys_dll.h"
#include "game/server/ai_networkmanager.h"
#include "game/server/detour_impl.h"
//#include "rtech/rui/rui.h"
//#include "materialsystem/cmaterialsystem.h"
//#include "studiorender/studiorendercontext.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
//#include "ebisusdk/EbisuSDK.h"

 //-------------------------------------------------------------------------
 // Purpose: change runtime behavior
 //-------------------------------------------------------------------------
void Dedicated_Init()
{
	if (s_bIsDedicated)
		*s_bIsDedicated = true;

	Msg(eDLL_T::SERVER, "[Dedicated_Init] running, g_GameDll base=%p\n", (void*)g_GameDll.GetModuleBase());

	// Raise entitlements.rson entry cap from 64 to unlimited.
	CMemory entitlementsCap = Module_FindPattern(g_GameDll, "8B 40 04 83 F8 40 72");
	if (entitlementsCap)
	{
		entitlementsCap.Offset(5).Patch({ 0xEB });
		Msg(eDLL_T::SERVER, "[Dedicated_Init] entitlements cap bypassed at %p\n", entitlementsCap.GetPtr());
	}
	else
		Msg(eDLL_T::SERVER, "[Dedicated_Init] entitlements cap: PATTERN NOT FOUND\n");

	// Skip entitlements CRC64 check in ProcessConnectPacket.
	CMemory entCrcCheck = Module_FindPattern(g_GameDll, "E8 ?? ?? ?? ?? 48 3B 05 ?? ?? ?? ?? 74 0C 4C 8D 0D");
	if (entCrcCheck)
	{
		entCrcCheck.Offset(12).Patch({ 0xEB });
		Msg(eDLL_T::SERVER, "[Dedicated_Init] entitlements CRC bypass at %p\n", entCrcCheck.GetPtr());
	}
	else
		Msg(eDLL_T::SERVER, "[Dedicated_Init] entitlements CRC: PATTERN NOT FOUND\n");

	// Fix entitlement balance count mismatch: the read loop uses r13d
	// (dedi's parsed count) but the client sends fewer. Trampoline through
	// the dead rejection path to load the client's count into r13d first.
	// Layout: [cmp+jz(7+2)] [dead_code(32)] [test r13d,r13d(3)]
	CMemory entBalCheck = Module_FindPattern(g_GameDll, "44 39 AD 08 08 00 00 74 20 4C 8B 85");
	if (entBalCheck)
	{
		// byte 7-8: change jz +20 to jmp +0 (land on trampoline at byte 9)
		entBalCheck.Offset(7).Patch({ 0xEB, 0x00 });
		// byte 9-17: mov r13d,[rbp+808h]; jmp +17h (to test r13d)
		entBalCheck.Offset(9).Patch({ 0x44, 0x8B, 0xAD, 0x08, 0x08, 0x00, 0x00, 0xEB, 0x17 });
		Msg(eDLL_T::SERVER, "[Dedicated_Init] entitlements balance trampoline at %p\n", entBalCheck.GetPtr());
	}
	else
		Msg(eDLL_T::SERVER, "[Dedicated_Init] entitlements balance: PATTERN NOT FOUND\n");
//	//-------------------------------------------------------------------------
//	// CGAME
//	//-------------------------------------------------------------------------
// p_CVideoMode_Common__CreateGameWindow.Offset(0x2C).Patch({ 0xE9, 0x9A, 0x00, 0x00, 0x00 }); // PUS --> XOR | Prevent ShowWindow and CreateGameWindow from being initialized (STGS RPak data type is registered here).
// p_CVideoMode_Common__CreateWindowClass.Offset(0x0).Patch({ 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 }); // FUN --> RET | Prevent CreateWindowClass from being initialized (returned true to satisfy condition that checks window handle).
//
//	//-------------------------------------------------------------------------
//	// CHLCLIENT
//	//-------------------------------------------------------------------------
// p_CHLClient_LevelShutdown.Patch({ 0xB8, 0x00, 0x00, 0x00, 0x00, 0xC3 }); // FUN --> RET | Return early in 'CHLClient::LevelShutdown' during DLL shutdown.
// p_CHLClient_HudProcessInput.Patch({ 0xC3 }); // FUN --> RET | Return early in 'CHLClient::HudProcessInput' to prevent infinite loop.
//
// Module_FindPattern(g_GameDll, "41 85 C8 0F 84").Offset(0x40).Patch({ 0xEB, 0x23 }); // MOV --> JMP | Skip virtual call during settings layout parsing (S0/S1/S2/S3).
//
//	//-------------------------------------------------------------------------
//	// CCLIENTSTATE
//	//-------------------------------------------------------------------------
// /*MOV EAX, 0*/
// p_CClientState__RunFrame.Patch({ 0xB8, 0x00, 0x00, 0x00, 0x00, 0xC3 }); // FUN --> RET | Always return false for pending client snapshots (inline CClientState call in '_Host_RunFrame')
// p_CClientState__Disconnect.Patch({ 0xB8, 0x00, 0x00, 0x00, 0x00, 0xC3 }); // FUN --> RET | Always return false for keeping client persistent data after disconnect (CLIENT ONLY).
//
//	//-------------------------------------------------------------------------
//	// CSOURCEAPPSYSTEMGROUP
//	//-------------------------------------------------------------------------
// p_CSourceAppSystemGroup__Create.Offset(0x248).Patch({ 0x90, 0x90, 0x90 }); // CAL --> NOP | inputSystem->Connect.
// p_CSourceAppSystemGroup__Create.Offset(0x267).Patch({ 0x90, 0x90, 0x90 }); // CAL --> NOP | materials->Connect.
// //p_CSourceAppSystemGroup__Create.Offset(0x286).Patch({ 0x90, 0x90, 0x90 }); // CAL --> NOP | mdlCache->Connect.
// p_CSourceAppSystemGroup__Create.Offset(0x2A5).Patch({ 0x90, 0x90, 0x90 }); // CAL --> NOP | studioRender->Connect.
// p_CSourceAppSystemGroup__Create.Offset(0x2C4).Patch({ 0x90, 0x90, 0x90 }); // CAL --> NOP | avi->Connect.
// p_CSourceAppSystemGroup__Create.Offset(0x2E3).Patch({ 0x90, 0x90, 0x90 }); // CAL --> NOP | engineAPI->Connect.
// //p_CSourceAppSystemGroup__Create.Offset(0x302).Patch({ 0x90, 0x90, 0x90 }); // CAL --> NOP | dataCache->Connect.
// p_CSourceAppSystemGroup__Create.Offset(0x321).Patch({ 0x90, 0x90, 0x90 }); // CAL --> NOP | matSystemSurface->Connect.
// p_CSourceAppSystemGroup__Create.Offset(0x340).Patch({ 0x90, 0x90, 0x90 }); // CAL --> NOP | vgui->Connect.
// p_CSourceAppSystemGroup__Create.Offset(0x35D).Patch({ 0x90, 0x90, 0x90 }); // CAL --> NOP | inputSystem->Init.
// p_CSourceAppSystemGroup__Create.Offset(0x384).Patch({ 0x90, 0x90, 0x90 }); // CAL --> NOP | studioRender->Init.
// p_CSourceAppSystemGroup__Create.Offset(0x391).Patch({ 0x90, 0x90, 0x90 }); // CAL --> NOP | avi->Init.
// p_CSourceAppSystemGroup__Create.Offset(0x39E).Patch({ 0x90, 0x90, 0x90 }); // CAL --> NOP | bik->Init.
// p_CSourceAppSystemGroup__Create.Offset(0x3AB).Patch({ 0x90, 0x90, 0x90 }); // CAL --> NOP | engineAPI->Init.
// p_CSourceAppSystemGroup__Create.Offset(0x3F6).Patch({ 0x90, 0x90, 0x90 }); // CAL --> NOP | vgui->Init.
// p_CSourceAppSystemGroup__Create.Offset(0x3E9).Patch({ 0x90, 0x90, 0x90 }); // CAL --> NOP | matEmbeddedPanel->Init.
// p_CSourceAppSystemGroup__Create.Offset(0x3F9).Patch({ 0x90, 0x90, 0x90, 0x90, 0x90 }); // CAL --> NOP | EAC_ClientInterface_Init.
//
//	//-------------------------------------------------------------------------
//	// CMATERIALSYSTEM
//	//-------------------------------------------------------------------------
// //gCMaterialSystem__MatsysMode_Init.Offset(0x22).Patch({ 0xEB, 0x66 }); // JE --> JMP | Matsys mode init (CMaterialSystem). // TODO: Needed?
// p_CMaterialSystem__Init.Offset(0x406).Patch({ 0xE9, 0x55, 0x05, 0x00, 0x00 }); // MOV --> JMP | Jump over material KeyValue definitions and 'CMatRenderContextBase::sm_RenderData([x])'.
// p_InitMaterialSystem.Patch({ 0xC3 }); // FUN --> RET | Return early to prevent 'InitDebugMaterials' from being executed. // RESEARCH NEEDED.
//
//	//-------------------------------------------------------------------------
//	// CSHADERSYSTEM
//	//-------------------------------------------------------------------------
// CShaderSystem__Init.Patch({ 0xC3 }); // FUN --> RET | Return early in 'CShaderSystem::Init' to prevent initialization.
//
//	//-------------------------------------------------------------------------
//	// CSTUDIORENDERCONTEXT
//	//-------------------------------------------------------------------------
// // Note: The registers here seems to contains pointers to material data and 'CMaterial' class methods when the shader system is initialized.
// CStudioRenderContext__LoadModel.Offset(0x17D).Patch({ 0x90, 0x90, 0x90, 0x90 }); // MOV --> NOP | RAX + RCX are both nullptr.
// CStudioRenderContext__LoadModel.Offset(0x181).Patch({ 0x90, 0x90, 0x90 }); // MOV --> NOP | RCX is nullptr when trying to dereference.
// CStudioRenderContext__LoadModel.Offset(0x184).Patch({ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }); // CAL --> NOP | RAX is nullptr during virtual call resulting in exception 'C0000005'.
// CStudioRenderContext__LoadMaterials.Offset(0x28).Patch({ 0xE9, 0x80, 0x04, 0x00, 0x00 }); // FUN --> RET | 'CStudioRenderContext::LoadMaterials' is called virtually by the 'RMDL' streaming job.
//
//	//-------------------------------------------------------------------------
//	// CMODELLOADER
//	//-------------------------------------------------------------------------
// p_CModelLoader__LoadModel.Offset(0x462).Patch({ 0x90, 0x90, 0x90, 0x90, 0x90 }); // CAL --> NOP | Prevent call to 'CStudioRenderContext::LoadMaterials'.
// p_CModelLoader__UnloadModel.Offset(0x129).Patch({ 0x90, 0x90, 0x90 }); // MOV --> NOP | Virtual call to 'CShaderSystem' class method fails as RCX is nullptr.
// p_CModelLoader__UnloadModel.Offset(0x12C).Patch({ 0x90, 0x90, 0x90 }); // CAL --> NOP | Virtual call to 'CTexture' class member in RAX + 0x78 fails. Previous instruction could not dereference.
// p_CModelLoader__Studio_LoadModel.Offset(0x325).Patch({ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }); // CAL --> NOP | Virtual call to 'CMaterialSystem::FindMaterialEx' fails as RAX is nullptr.
// p_CModelLoader__Studio_LoadModel.Offset(0x33D).Patch({ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }); // CAL --> NOP | Virtual call to 'CMaterialGlue' class method fails as RAX is nullptr.
// p_CModelLoader__Studio_LoadModel.Offset(0x359).Patch({ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }); // CAL --> NOP | Virtual call to 'CMaterialGlue' class method fails as RAX is nullptr.
// p_CModelLoader__Studio_LoadModel.Offset(0x374).Patch({ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }); // CAL --> NOP | Virtual call to 'CMaterialGlue' class method fails as RAX is nullptr.
// p_CModelLoader__Studio_LoadModel.Offset(0x38D).Patch({ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }); // CAL --> NOP | Virtual call to 'ReturnZero' fails as RAX is nullptr.
// p_CModelLoader__Studio_LoadModel.Offset(0x3A4).Patch({ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }); // CAL --> NOP | Virtual call to 'CMaterialGlue' class method fails as RAX is nullptr.
//
// p_CModelLoader__Map_LoadModelGuts.Offset(0x41).Patch({ 0xE9, 0x4F, 0x04, 0x00, 0x00 }); // JNE --> NOP | SKYLIGHTS.
// p_CModelLoader__Map_LoadModelGuts.Offset(0x974).Patch({ 0x90, 0x90 }); // JE --> NOP | VERTNORMALS.
// p_CModelLoader__Map_LoadModelGuts.Offset(0xA55).Patch({ 0x90, 0x90, 0x90, 0x90, 0x90 }); // CAL --> NOP | MATERIALSORTS.
// p_CModelLoader__Map_LoadModelGuts.Offset(0xA62).Patch({ 0x90, 0x90, 0x90, 0x90, 0x90 }); // CAL --> NOP | MESHBOUNDS.
// p_CModelLoader__Map_LoadModelGuts.Offset(0xA83).Patch({ 0x90, 0x90, 0x90, 0x90, 0x90 }); // CAL --> NOP | MESHVERTS.
// p_CModelLoader__Map_LoadModelGuts.Offset(0xAC0).Patch({ 0x90, 0x90 }); // JE --> NOP | INDICES.
// p_CModelLoader__Map_LoadModelGuts.Offset(0xBF2).Patch({ 0x90, 0x90 }); // JE --> NOP | WORLDLIGHTS.
// p_CModelLoader__Map_LoadModelGuts.Offset(0xDA9).Patch({ 0x90, 0x90 }); // JE --> NOP | TWEAKLIGHTS.
// p_CModelLoader__Map_LoadModelGuts.Offset(0xEEB).Patch({ 0xE9, 0x3D, 0x01, 0x00, 0x00 }); // JLE --> JMP | Exception 0x57 in while trying to dereference [R15 + R14 *8 + 0x10].
// p_CModelLoader__Map_LoadModelGuts.Offset(0x61B).Patch({ 0xE9, 0xE2, 0x02, 0x00, 0x00 }); // JZ --> JMP | Prevent call to 'CMod_LoadTextures'.
// p_CModelLoader__Map_LoadModelGuts.Offset(0x1045).Patch({ 0x90, 0x90, 0x90, 0x90, 0x90 }); // CAL --> NOP | Prevent call to 'Mod_LoadCubemapSamples'.
//
// p_BuildSpriteLoadName.Patch({ 0xC3 }); // FUN --> RET | Return early in 'BuildSpriteLoadName'.
// p_GetSpriteInfo.Patch({ 0xC3 }); // FUN --> RET | Return early in 'GetSpriteInfo'.
//
//	//-------------------------------------------------------------------------
//	// CGAMESERVER
//	//-------------------------------------------------------------------------
// p_CGameServer__SpawnServer.Offset(0x43).Patch({ 0x90, 0x90, 0x90, 0x90, 0x90 }); // CAL --> NOP | Prevent call to unknown material/shader code.
// p_CGameServer__SpawnServer.Offset(0x48).Patch({ 0x90, 0x90, 0x90, 0x90, 0x90 }); // CAL --> NOP | TODO: Research 'CIVDebugOverlay'.
//
//	//-------------------------------------------------------------------------
//	// CVGUI
//	//-------------------------------------------------------------------------
// /*MOV EAX, 0*/
// CVGui__RunFrame.Patch({ 0xB8, 0x00, 0x00, 0x00, 0x00, 0xC3 }); // FUN --> RET | 'CVGui::RunFrame' gets called on DLL shutdown.
//	//-------------------------------------------------------------------------
//	// CRUI
//	//-------------------------------------------------------------------------
// /*MOV EAX, 0*/
// p_Rui_LoadAsset.Patch({ 0xB8, 0x00, 0x00, 0x00, 0x00, 0xC3 }); // FUN --> RET | Return early in RuiLoadAsset to prevent error while attempting to load RUI assets after applying player settings.
//
//	//-------------------------------------------------------------------------
//	// CENGINEVGUI
//	//-------------------------------------------------------------------------
// CEngineVGui__Shutdown.Patch({ 0xB8, 0x00, 0x00, 0x00, 0x00, 0xC3 }); // FUN --> RET | Cannot shutdown CEngineVGui if its never initialized.
// CEngineVGui__ActivateGameUI.FindPatternSelf("74 08", CMemory::Direction::DOWN).Patch({ 0x90, 0x90 }); // JZ --> NOP | Remove condition to return early when engine attempts to activate UI on the server.
//
//	//-------------------------------------------------------------------------
//	// CENGINEVGUI
//	//-------------------------------------------------------------------------
// CInputSystem__RunFrameIME.Patch({ 0xB8, 0x00, 0x00, 0x00, 0x00, 0xC3 }); // FUN --> RET | Return early in 'CInputSystem::RunFrameIME'.
//
//	//-------------------------------------------------------------------------
//	// MM_HEARTBEAT
//	//-------------------------------------------------------------------------
// MM_Heartbeat__ToString.Patch({ 0xC3 }); // SUB --> RET | Return early in ListenServer HeartBeat.
//
//	//-------------------------------------------------------------------------
//	// RUNTIME: SYS_INITGAME
//	//-------------------------------------------------------------------------
// Sys_InitGame.Offset(0x70).Patch({ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }); // STZNZ --> NOP | Prevent 'bDedicated' from being set to false.
//
//	//-------------------------------------------------------------------------
//	// RUNTIME: HOST_INIT
//	//-------------------------------------------------------------------------
// p_Host_Init.Offset(0xC2).Patch({ 0xEB, 0x34 }); // CAL --> NOP | Disable 'vpk/client_common.bsp' loading.
// p_Host_Init.Offset(0x182).Patch({ 0x90, 0x90, 0x90, 0x90, 0x90 }); // CAL --> JMP | Disable UI material asset initialization.
// p_Host_Init.Offset(0x859).Patch({ 0xE9, 0x19, 0x04, 0x00, 0x00 }); // LEA --> RET | Disable 'client.dll' library initialization.
// p_Host_Init.Offset(0xC77).Patch({ 0xE8, 0x44, 0xCF, 0xFF, 0xFF }); // CAL --> CAL | Disable user config loading and call entitlements.rson initialization instead.
//
// gHost_Init_1.Offset(0x564).Patch({ 0xEB }); // JNZ --> JMP | Skip chat room and discord presence thread creation [!TODO: set global boolean instead].
// gHost_Init_1.Offset(0x609).Patch({ 0xEB, 0x2B }); // JE --> JMP | Skip client.dll 'Init_PostVideo' validation code.
// gHost_Init_1.Offset(0x621).Patch({ 0xEB, 0x0C }); // JNE --> JMP | Skip client.dll 'Init_PostVideo' validation code.
// gHost_Init_1.Offset(0x658).Patch({ 0xE9, 0x8C, 0x00, 0x00, 0x00 }); // JE --> JMP | Skip NULL call as client is never initialized.
// gHost_Init_1.Offset(0x6E9).Patch({ 0xE9, 0xB0, 0x00, 0x00, 0x00 }); // JNE --> JMP | Skip shader preloading as cvar can't be checked due to client being NULL.
//
// gHost_Init_2.Offset(0x26F).Patch({ 0xE9, 0x4D, 0x05, 0x00, 0x00 }); // JNE --> JMP | client.dll systems initialization.
//
//	//-------------------------------------------------------------------------
//	// RUNTIME: HOST_SHUTDOWN
//	//-------------------------------------------------------------------------
//#if defined (GAMEDLL_S0) || defined (GAMEDLL_S1)
// Host_Shutdown.Offset(0x1F0).FindPatternSelf("7E", CMemory::Direction::DOWN).Patch({ 0xE9, 0x01, 0x08, 0x00, 0x00 }); // JNE --> JMP | Jump over inline 'Host_ShutdownClient' ('Host_ShutdownServer' in now inline with 'Host_Shutdown')
//#elif defined (GAMEDLL_S2) || defined (GAMEDLL_S3)
// Host_Shutdown.Offset(0x1F0).FindPatternSelf("7E", CMemory::Direction::DOWN).Patch({ 0xE9, 0xF9, 0x04, 0x00, 0x00 }); // JNE --> JMP | Jump over inline 'Host_ShutdownClient' ('Host_ShutdownServer' in now inline with 'Host_Shutdown')
//#endif // 0x700
//
//	//-------------------------------------------------------------------------
//	// RUNTIME: HOST_NEWGAME
//	//-------------------------------------------------------------------------
// p_Host_NewGame.Offset(0x50).Patch({ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }); // CAL --> NOP | Invalid CHLClient virtual call 'g_pHLClient->nullsub'.
// p_Host_NewGame.Offset(0x4E0).Patch({ 0x90, 0x90, 0x90, 0x90, 0x90 }); // CAL --> NOP | Matsys 'JT_HelpWithAnything'.
//
//	//-------------------------------------------------------------------------
//	// RUNTIME: HOST_CHANGELEVEL
//	//-------------------------------------------------------------------------
// p_Host_ChangeLevel.Offset(0x5D).Patch({ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }); // CAL --> NOP | Invalid CHLClient virtual call 'g_pHLClient->nullsub'.
//
//	//-------------------------------------------------------------------------
//	// RUNTIME: _HOST_RUNFRAME
//	//-------------------------------------------------------------------------
// p_Host_RunFrame.Offset(0xB85).Patch({ 0xEB, 0x6F }); // CMP --> JMP | Jump over inline '_Host_RunFrame_Client'
// p_Host_RunFrame_Render.Patch({ 0xC3 }); // FUN --> RET | Extraneous function for Dedicated.
// p_VCR_EnterPausedState.Patch({ 0xC3 }); // FUN --> RET | Extraneous function for Dedicated.
//
//	//-------------------------------------------------------------------------
//	// RUNTIME: HOST_DISCONNECT
//	//-------------------------------------------------------------------------
//#if defined (GAMEDLL_S2) || defined (GAMEDLL_S3)
// Host_Disconnect.Offset(0x4A).FindPatternSelf("FF 90 80", CMemory::Direction::DOWN, 300).Patch({ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, }); // CAL --> RET | This seems to call 'CEngineVGui::GetGameUIInputContext'.
//#endif
//
//	//-------------------------------------------------------------------------
//	// RUNTIME: RTECH_GAME
//	//-------------------------------------------------------------------------
//#if defined (GAMEDLL_S2) || defined (GAMEDLL_S3)
// p_CPakFile_LoadPak.Offset(0x890).FindPatternSelf("75", CMemory::Direction::DOWN, 200).Patch({ 0xEB }); // JNZ --> JMP | Disable error handling for missing streaming files on the server. The server does not need streamed data from the starpak files.
//#endif
//
//	//-------------------------------------------------------------------------
//	// RUNTIME: EBISUSDK
//	//-------------------------------------------------------------------------
// p_EbisuSDK_SetState.Offset(0x0).FindPatternSelf("0F 84", CMemory::Direction::DOWN).Patch({ 0x0F, 0x85 }); // JE --> JNZ | Prevent EbisuSDK from initializing on the engine and server.
//
//	//-------------------------------------------------------------------------
//	// RUNTIME: FAIRFIGHT
//	//-------------------------------------------------------------------------
// FairFight_Init.Offset(0x0).FindPatternSelf("0F 87", CMemory::Direction::DOWN, 200).Patch({ 0x0F, 0x85 }); // JA --> JNZ | Prevent 'FairFight' anti-cheat from initializing on the server by comparing RAX against 0x0 instead. Init will crash since the plugins aren't shipped.
//
//	//-------------------------------------------------------------------------
//	// RUNTIME: PROP_STATIC
//	//-------------------------------------------------------------------------
// // Note: At [14028F3B0 + 0x5C7] RSP seems to contain a block of pointers to data for the static prop rmdl in question. [RSP + 0x70] is a pointer to (what seems to be) shader/material data. The pointer will be NULL without a shader system.
// p_BuildPropStaticFrustumCullMap.Offset(0x5E0).Patch({ 0x90, 0x90, 0x90, 0x90, 0x90 }); // MOV --> NOP | RSP + 0x70 is a nullptr which gets moved to R13, R13 gets used here resulting in exception 'C0000005'.
// p_BuildPropStaticFrustumCullMap.Offset(0x5EB).Patch({ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }); // CAL --> NOP | RAX is nullptr during virtual call resulting in exception 'C0000005'.
//
//	//-------------------------------------------------------------------------
//	// RUNTIME: GL_SCREEN
//	//-------------------------------------------------------------------------
// SCR_BeginLoadingPlaque.Patch({ 0xC3 }); // FUN --> RET | Return early to prevent execution of 'SCR_BeginLoadingPlaque'.
//
//	//-------------------------------------------------------------------------
//	// RUNTIME: CL_CLEARSTATE
//	//-------------------------------------------------------------------------
//#if defined (GAMEDLL_S2) || defined (GAMEDLL_S3)
// p_CL_ClearState.Offset(0x0).Patch({ 0xC3 }); // FUN --> RET | Invalid 'CL_ClearState' call from Host_Shutdown causing segfault.
//#endif
//	//-------------------------------------------------------------------------
//	// RUNTIME: GAME_CFG
//	//-------------------------------------------------------------------------
//	p_UpdateMaterialSystemConfig.Offset(0x0).Patch({ 0xB8, 0x00, 0x00, 0x00, 0x00, 0xC3 });// FUN --> RET | Return early to prevent the server from updating material system configurations.
//	p_UpdateCurrentVideoConfig.Offset(0x0).Patch({ 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 }); // FUN --> RET | Return early to prevent the server from writing a videoconfig.txt file to the disk (overwriting the existing one).
//	p_HandleConfigFile.Offset(0x0).Patch({ 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 }); // FUN --> RET | Return early to prevent the server from writing various input and ConVar config files to the disk (overwriting the existing one).
//	p_ResetPreviousGameState.Offset(0x0).Patch({ 0xC3 }); // FUN --> RET | Return early to prevent the server from writing a previousgamestate.txt file to the disk (overwriting the existing one).
//	p_LoadPlayerConfig.Offset(0x0).Patch({ 0xC3 }); // FUN --> RET | Return early to prevent the server from executing 'config_default_pc.cfg' (execPlayerConfig) and (only for >S3) running 'chat_wheel' code.
//
//	//-------------------------------------------------------------------------
//	// RUNTIME: COMMUNITIES
//	//-------------------------------------------------------------------------
// //GetEngineClientThread.Offset(0x0).Patch({ 0xB8, 0x00, 0x00, 0x00, 0x00, 0xC3 }); // FUN --> RET | Return nullptr for mp_gamemode thread assignment during registration callback.
//
//	//-------------------------------------------------------------------------
//	// RUNTIME: MATCHMAKING
//	//-------------------------------------------------------------------------
// MatchMaking_Frame.Patch({ 0xB8, 0x00, 0x00, 0x00, 0x00, 0xC3 }); // FUN --> RET | Return early for 'MatchMaking_Frame'.
//
// CWin32Surface_initStaticData.Patch({ 0xC3 }); // FUN --> RET | Prevent 'CWin32Surface::initStaticData' from being ran in CInit.
//#if !defined (GAMEDLL_S0) || !defined (GAMEDLL_S1)
// KeyboardLayout_Init.Patch({ 0xC3 }); // FUN --> RET | Prevent keyboard layout initialization for IME in CInit.
//#endif
}

void RuntimePtc_Init() /* .TEXT */
{
	if (v_SQVM_CompileError)
	{
		CMemory site = CMemory(v_SQVM_CompileError).FindPatternSelf("41 B0 01", CMemory::Direction::DOWN, 400);
		if (site.IsValid())
			site.Patch({ 0x41, 0xB0, 0x00 });
	}
}
#endif // CLIENT_DLL
