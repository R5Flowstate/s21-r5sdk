//=============================================================================
//
// Purpose: S21 dual-wield partner is N+7, not S3's N+5.
//
//=============================================================================
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier1/convar.h"
#include "public/tier0/memaddr.h"
#include "weapon_dualwield_slot_patch.h"

#include <Windows.h>

// Post-deploy pairing: S3 `lea eax, [rcx+5]` -> +7. rdi = WeaponInventory.
static const char* const DUALWIELD_PARTNER_PATTERN =
	"80 F9 04 73 ?? 8D 41 05 83 F8 09 73 ?? 48 63 C1 8B 54 87 1C";

// Offsets into the matched pattern of the two bytes that encode the pairing.
static constexpr ptrdiff_t DUALWIELD_OFF_LEA_IMM  = 7;  // the 05 in lea eax,[rcx+5]
static constexpr ptrdiff_t DUALWIELD_OFF_LOAD_DSP = 19; // the 1C in mov edx,[rdi+rax*4+1Ch]

// S21 pairs main slot N with N+7. The load displacement is the byte offset of
// weapons[N+7] within the inventory: 8 + 4*7 = 0x24.
static constexpr uint8_t DUALWIELD_S21_PARTNER_DELTA = 0x07;
static constexpr uint8_t DUALWIELD_S21_LOAD_DISP     = 0x24;

static uintptr_t g_dualWieldPartnerAnchor = 0;
static uint8_t   g_dualWieldLeaImmSaved   = 0;
static uint8_t   g_dualWieldLoadDspSaved  = 0;
static bool      g_dualWieldPatched       = false;

// Weapon_Give auto-activate: S3 `sub bpl, 5` / slot-5 immediates -> +7 / slot-7.
static const char* const DUALWIELD_GIVE_ACTIVATE_PATTERN =
	"40 80 ED 05 4C 8D 25 ?? ?? ?? ?? 40 80 FD 03 0F 87 ?? ?? ?? ?? 41 8D 45 FB 33 D2 83 F8 09 73 ?? 43 8B 4C AE F4";

// Offsets into the matched pattern of the three bytes that encode slot-5.
static constexpr ptrdiff_t DUALWIELD_GIVE_OFF_SUB_IMM  = 3;  // the 05 in sub bpl, 5
static constexpr ptrdiff_t DUALWIELD_GIVE_OFF_LEA_IMM  = 24; // the FB in lea eax,[r13-5]
static constexpr ptrdiff_t DUALWIELD_GIVE_OFF_LOAD_DSP = 36; // the F4 in mov ecx,[r14+r13*4-0Ch]

// S21: slot-7 range test, partner index, and weapons[slot-7] load.
// weapons[slot-7] = inv+8 + 4*(slot-7) = [r14+r13*4-14h]; -14h as a signed
// byte is 0xEC. lea [r13-7] encodes imm8 0xF9.
static constexpr uint8_t DUALWIELD_GIVE_S21_SUB_IMM  = 0x07;
static constexpr uint8_t DUALWIELD_GIVE_S21_LEA_IMM  = 0xF9;
static constexpr uint8_t DUALWIELD_GIVE_S21_LOAD_DSP = 0xEC;

static uintptr_t g_dualWieldGiveAnchor     = 0;
static uint8_t   g_dualWieldGiveSubSaved   = 0;
static uint8_t   g_dualWieldGiveLeaSaved   = 0;
static uint8_t   g_dualWieldGiveLoadSaved  = 0;
static bool      g_dualWieldGivePatched    = false;

static ConVar bridge_weap_dualwield_partner_s21(
	"bridge_weap_dualwield_partner_s21", "1", FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Pair the dual-wield partner slot the way S21 does (main+7) instead of the "
	"way S3 does (main+5). Read once at detour attach -- pass it as a launch "
	"arg to A/B. 0 = leave the engine's S3 pairing, which activates the "
	"ordnance at slot 5 alongside the first primary.");

void VWeaponDualWieldSlotPatch::GetAdr(void) const
{
	LogVarAdr("WeaponDualWieldPartnerSite",
		reinterpret_cast<const void*>(g_dualWieldPartnerAnchor));
	LogVarAdr("WeaponDualWieldGiveActivateSite",
		reinterpret_cast<const void*>(g_dualWieldGiveAnchor));
}

void VWeaponDualWieldSlotPatch::GetFun(void) const
{
	g_dualWieldPartnerAnchor = Module_FindPattern(
		g_GameDll, DUALWIELD_PARTNER_PATTERN).GetPtr();

	if (!g_dualWieldPartnerAnchor)
		Warning(eDLL_T::SERVER,
			"[WEAP-DUALWIELD] partner-slot pattern unresolved -- the engine "
			"keeps its S3 main+5 pairing, so the ordnance at slot 5 will be "
			"drawn alongside the first primary.\n");

	g_dualWieldGiveAnchor = Module_FindPattern(
		g_GameDll, DUALWIELD_GIVE_ACTIVATE_PATTERN).GetPtr();

	if (!g_dualWieldGiveAnchor)
		Warning(eDLL_T::SERVER,
			"[WEAP-DUALWIELD] give auto-activate pattern unresolved -- the "
			"engine keeps its S3 pairing, so the survival item at slot 6 is "
			"auto-drawn into the alt hand next to the primary.\n");
}

void VWeaponDualWieldSlotPatch::Detour(const bool bAttach) const
{
	if (!bAttach)
	{
		if (g_dualWieldPatched)
		{
			DWORD oldProtect = 0;
			VirtualProtect(reinterpret_cast<void*>(g_dualWieldPartnerAnchor),
				DUALWIELD_OFF_LOAD_DSP + 1, PAGE_EXECUTE_READWRITE, &oldProtect);
			*reinterpret_cast<uint8_t*>(g_dualWieldPartnerAnchor + DUALWIELD_OFF_LEA_IMM)
				= g_dualWieldLeaImmSaved;
			*reinterpret_cast<uint8_t*>(g_dualWieldPartnerAnchor + DUALWIELD_OFF_LOAD_DSP)
				= g_dualWieldLoadDspSaved;
			VirtualProtect(reinterpret_cast<void*>(g_dualWieldPartnerAnchor),
				DUALWIELD_OFF_LOAD_DSP + 1, oldProtect, &oldProtect);

			g_dualWieldPatched = false;
		}

		if (g_dualWieldGivePatched)
		{
			DWORD oldProtect = 0;
			VirtualProtect(reinterpret_cast<void*>(g_dualWieldGiveAnchor),
				DUALWIELD_GIVE_OFF_LOAD_DSP + 1, PAGE_EXECUTE_READWRITE, &oldProtect);
			*reinterpret_cast<uint8_t*>(g_dualWieldGiveAnchor + DUALWIELD_GIVE_OFF_SUB_IMM)
				= g_dualWieldGiveSubSaved;
			*reinterpret_cast<uint8_t*>(g_dualWieldGiveAnchor + DUALWIELD_GIVE_OFF_LEA_IMM)
				= g_dualWieldGiveLeaSaved;
			*reinterpret_cast<uint8_t*>(g_dualWieldGiveAnchor + DUALWIELD_GIVE_OFF_LOAD_DSP)
				= g_dualWieldGiveLoadSaved;
			VirtualProtect(reinterpret_cast<void*>(g_dualWieldGiveAnchor),
				DUALWIELD_GIVE_OFF_LOAD_DSP + 1, oldProtect, &oldProtect);

			g_dualWieldGivePatched = false;
		}
		return;
	}

	if (!bridge_weap_dualwield_partner_s21.GetBool())
	{
		Warning(eDLL_T::SERVER,
			"[WEAP-DUALWIELD] disabled by convar -- engine keeps the S3 main+5 "
			"pairing (ordnance at slot 5 activates with the first primary).\n");
		return;
	}

	if (g_dualWieldPartnerAnchor)
	{
		DWORD oldProtect = 0;
		VirtualProtect(reinterpret_cast<void*>(g_dualWieldPartnerAnchor),
			DUALWIELD_OFF_LOAD_DSP + 1, PAGE_EXECUTE_READWRITE, &oldProtect);

		uint8_t* const pLeaImm = reinterpret_cast<uint8_t*>(
			g_dualWieldPartnerAnchor + DUALWIELD_OFF_LEA_IMM);
		uint8_t* const pLoadDsp = reinterpret_cast<uint8_t*>(
			g_dualWieldPartnerAnchor + DUALWIELD_OFF_LOAD_DSP);

		g_dualWieldLeaImmSaved  = *pLeaImm;
		g_dualWieldLoadDspSaved = *pLoadDsp;
		*pLeaImm  = DUALWIELD_S21_PARTNER_DELTA;
		*pLoadDsp = DUALWIELD_S21_LOAD_DISP;

		VirtualProtect(reinterpret_cast<void*>(g_dualWieldPartnerAnchor),
			DUALWIELD_OFF_LOAD_DSP + 1, oldProtect, &oldProtect);

		g_dualWieldPatched = true;

		Msg(eDLL_T::SERVER,
			"[WEAP-DUALWIELD] partner slot re-pointed main+%u -> main+%u at %p "
			"(load disp 0x%02X -> 0x%02X). Slots 0/1 now pair with weapons[7]/[8]; "
			"the engine's own bound check drops 2..4. Ordnance (5) and gadget (6) "
			"are no longer auto-activated by a primary select.\n",
			g_dualWieldLeaImmSaved, DUALWIELD_S21_PARTNER_DELTA,
			reinterpret_cast<void*>(g_dualWieldPartnerAnchor),
			g_dualWieldLoadDspSaved, DUALWIELD_S21_LOAD_DISP);
	}

	if (g_dualWieldGiveAnchor)
	{
		DWORD oldProtect = 0;
		VirtualProtect(reinterpret_cast<void*>(g_dualWieldGiveAnchor),
			DUALWIELD_GIVE_OFF_LOAD_DSP + 1, PAGE_EXECUTE_READWRITE, &oldProtect);

		uint8_t* const pSubImm = reinterpret_cast<uint8_t*>(
			g_dualWieldGiveAnchor + DUALWIELD_GIVE_OFF_SUB_IMM);
		uint8_t* const pLeaImm = reinterpret_cast<uint8_t*>(
			g_dualWieldGiveAnchor + DUALWIELD_GIVE_OFF_LEA_IMM);
		uint8_t* const pLoadDsp = reinterpret_cast<uint8_t*>(
			g_dualWieldGiveAnchor + DUALWIELD_GIVE_OFF_LOAD_DSP);

		g_dualWieldGiveSubSaved  = *pSubImm;
		g_dualWieldGiveLeaSaved  = *pLeaImm;
		g_dualWieldGiveLoadSaved = *pLoadDsp;
		*pSubImm  = DUALWIELD_GIVE_S21_SUB_IMM;
		*pLeaImm  = DUALWIELD_GIVE_S21_LEA_IMM;
		*pLoadDsp = DUALWIELD_GIVE_S21_LOAD_DSP;

		VirtualProtect(reinterpret_cast<void*>(g_dualWieldGiveAnchor),
			DUALWIELD_GIVE_OFF_LOAD_DSP + 1, oldProtect, &oldProtect);

		g_dualWieldGivePatched = true;

		Msg(eDLL_T::SERVER,
			"[WEAP-DUALWIELD] give auto-activate re-pointed sub 0x%02X->0x%02X, "
			"lea 0x%02X->0x%02X, load 0x%02X->0x%02X at %p. GiveWeapon into "
			"slot 6 (gadget) no longer takes the dual-primary branch into the "
			"alt hand.\n",
			g_dualWieldGiveSubSaved, DUALWIELD_GIVE_S21_SUB_IMM,
			g_dualWieldGiveLeaSaved, DUALWIELD_GIVE_S21_LEA_IMM,
			g_dualWieldGiveLoadSaved, DUALWIELD_GIVE_S21_LOAD_DSP,
			reinterpret_cast<void*>(g_dualWieldGiveAnchor));
	}
}
