//=============================================================================//
//
// Purpose: Live diagnostic for jumppad view-punch loss on the S21 bridge
// client. See jumppad_viewpunch_diag.h.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "game/client/jumppad_viewpunch_diag.h"
#include <cstdio>

//-----------------------------------------------------------------------------
// S21 C_TriggerCylinderHeavy field offsets ( StartTouch + recv-table
// layout shifted by -32 for m_triggerType 2592 -> 2560).
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t TRG_OFF_TRIGGER_TYPE   = 0xA00; // m_triggerType
static constexpr ptrdiff_t TRG_OFF_VERT_OVERRIDE  = 0xA04; // m_vertOverride
static constexpr ptrdiff_t TRG_OFF_LAUNCH_POWER   = 0xA08; // m_launchPower
static constexpr ptrdiff_t TRG_OFF_PUNCH_SOFT     = 0xA0C; // m_punchSoftAmount
static constexpr ptrdiff_t TRG_OFF_PUNCH_HARD     = 0xA10; // m_punchHardAmount
static constexpr ptrdiff_t TRG_OFF_PUNCH_RANDOM   = 0xA14; // m_punchRandomBoost
static constexpr ptrdiff_t TRG_OFF_REALM_MASK     = 0x938; // realm bitfield used in StartTouch gate
static constexpr ptrdiff_t TRG_OFF_CLIENT_SIDE_PREDICTED = 0x9A0; // m_bClientSidePredicted

// Player-side StartTouch early-out gates (from S21 of StartTouch).
static constexpr ptrdiff_t PLR_OFF_IGNORE_TRIG_TYPES = 0x34C; // dword a2+211*4
static constexpr ptrdiff_t PLR_OFF_REALM_MASK        = 0x938; // qword a2[295]

// Trigger types are bitflags, not enum indices -- the wire field carries 6 bits.
static constexpr int TT_JUMP_PAD            = 1;
static constexpr int TT_TESLA_TRAP          = 2;
static constexpr int TT_GRAVITY_LIFT        = 4;
static constexpr int TT_BLACKHOLE           = 8;
static constexpr int TT_MORTAR_RING_SEGMENT = 16;
static constexpr int TT_GRAVITY_CANNON      = 32;

static ConVar bridge_jumppad_viewpunch_diag("bridge_jumppad_viewpunch_diag", "0",
	FCVAR_DEVELOPMENTONLY,
	"[JP-PUNCH] Log C_TriggerCylinderHeavy::StartTouch for jump pads: type, "
	"punch floats, launch power, ignore/realm gates. 0=off.");

static volatile LONG s_jpPunchDiagCount = 0;

static inline int ReadI32(const void* base, ptrdiff_t off)
{
	return *reinterpret_cast<const int*>(static_cast<const uint8_t*>(base) + off);
}

static inline float ReadF32(const void* base, ptrdiff_t off)
{
	return *reinterpret_cast<const float*>(static_cast<const uint8_t*>(base) + off);
}

static inline uint64_t ReadU64(const void* base, ptrdiff_t off)
{
	return *reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(base) + off);
}

static void JpPunch_FormatTriggerType(int nType, char* pszOut, size_t nOutLen)
{
	static const struct { int flag; const char* name; } s_flags[] = {
		{ TT_JUMP_PAD, "JUMP_PAD" }, { TT_TESLA_TRAP, "TESLA_TRAP" },
		{ TT_GRAVITY_LIFT, "GRAVITY_LIFT" }, { TT_BLACKHOLE, "BLACKHOLE" },
		{ TT_MORTAR_RING_SEGMENT, "MORTAR_RING_SEGMENT" },
		{ TT_GRAVITY_CANNON, "GRAVITY_CANNON" },
	};

	pszOut[0] = '\0';
	size_t used = 0;
	for (size_t i = 0; i < sizeof(s_flags) / sizeof(s_flags[0]); ++i)
	{
		if (!(nType & s_flags[i].flag))
			continue;
		const int n = snprintf(pszOut + used, nOutLen - used, "%s%s",
			used ? "|" : "", s_flags[i].name);
		if (n <= 0 || static_cast<size_t>(n) >= nOutLen - used)
			break;
		used += static_cast<size_t>(n);
	}

	// Bits above the known set mean the 6-bit wire field decoded wrong.
	const int unknown = nType & ~(TT_JUMP_PAD | TT_TESLA_TRAP | TT_GRAVITY_LIFT |
		TT_BLACKHOLE | TT_MORTAR_RING_SEGMENT | TT_GRAVITY_CANNON);
	if (unknown && used < nOutLen)
		snprintf(pszOut + used, nOutLen - used, "%sUNKNOWN(0x%X)", used ? "|" : "", unknown);
	if (!pszOut[0])
		snprintf(pszOut, nOutLen, "NONE");
}

static void Hook_C_TriggerCylinderHeavy_StartTouch(void* trigger, void* other)
{
	// Live StartTouch args: null-checked once, then plain field reads (D2).
	if (bridge_jumppad_viewpunch_diag.GetBool() && trigger && other)
	{
		const int triggerType = ReadI32(trigger, TRG_OFF_TRIGGER_TYPE);
		// Log every typed trigger always; untyped stay rate-limited to first 8.
		const bool isJumpPad = (triggerType & TT_JUMP_PAD) != 0;
		const LONG n = InterlockedIncrement(&s_jpPunchDiagCount);

		if (triggerType != 0 || n <= 8)
		{
			const float soft   = ReadF32(trigger, TRG_OFF_PUNCH_SOFT);
			const float hard   = ReadF32(trigger, TRG_OFF_PUNCH_HARD);
			const float random = ReadF32(trigger, TRG_OFF_PUNCH_RANDOM);
			const float launch = ReadF32(trigger, TRG_OFF_LAUNCH_POWER);
			const float vert   = ReadF32(trigger, TRG_OFF_VERT_OVERRIDE);

			const int      plrIgnore = ReadI32(other, PLR_OFF_IGNORE_TRIG_TYPES);
			const uint64_t plrRealm  = ReadU64(other, PLR_OFF_REALM_MASK);
			const uint64_t trgRealm  = ReadU64(trigger, TRG_OFF_REALM_MASK);

			// Mirror StartTouch's two early-out predicates.
			const bool gateIgnore = (plrIgnore & triggerType) != 0;
			const bool gateRealm  = (plrRealm & trgRealm) == 0;

			char szType[128];
			JpPunch_FormatTriggerType(triggerType, szType, sizeof(szType));

			Msg(eDLL_T::CLIENT,
				"[JP-PUNCH] StartTouch n=%ld type=%d(%s) jumpPad=%d "
				"soft=%.2f hard=%.2f rand=%.2f launch=%.1f vert=%.2f "
				"plrIgnore=0x%X trgRealm=0x%llX plrRealm=0x%llX "
				"gateIgnore=%d gateRealmFail=%d trigger=%p other=%p\n",
				n, triggerType, szType, isJumpPad ? 1 : 0,
				soft, hard, random, launch, vert,
				plrIgnore,
				static_cast<unsigned long long>(trgRealm),
				static_cast<unsigned long long>(plrRealm),
				gateIgnore ? 1 : 0,
				gateRealm ? 1 : 0,
				trigger, other);
		}
	}

	C_TriggerCylinderHeavy__StartTouch(trigger, other);
}

void VJumpPadViewPunchDiag::GetFun(void) const
{
	// C_TriggerCylinderHeavy::StartTouch @ S21.
	// Unique: prologue + player ignore-mask load (0x34C) + m_triggerType test (0xA00).
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 20 57 48 83 EC 40 "
		"8B 82 4C 03 00 00 "
		"48 8B DA 48 8B F9 "
		"85 81 00 0A 00 00")
		.GetPtr(C_TriggerCylinderHeavy__StartTouch);

	if (!C_TriggerCylinderHeavy__StartTouch)
		Warning(eDLL_T::CLIENT,
			"[JP-PUNCH] C_TriggerCylinderHeavy::StartTouch pattern unresolved\n");
}

void VJumpPadViewPunchDiag::Detour(const bool bAttach) const
{
	if (bAttach && !bridge_jumppad_viewpunch_diag.GetBool())
		return;

	if (C_TriggerCylinderHeavy__StartTouch)
		DetourSetup(&C_TriggerCylinderHeavy__StartTouch,
			&Hook_C_TriggerCylinderHeavy_StartTouch, bAttach);
}

//-----------------------------------------------------------------------------
// C_TriggerCylinderNetworked::UpdatePartitionListEntry -- force the
// predicted-trigger partition arm when the wire prop arrives as 0.
//-----------------------------------------------------------------------------
static ConVar bridge_trigger_clientpredict_force(
	"bridge_trigger_clientpredict_force", "1", FCVAR_RELEASE,
	"Force m_bClientSidePredicted on networked cylinder triggers so the client-side "
	"predicted-trigger partition arms. 0 = trust the wire value.");

static volatile LONG s_bTrigPredWireLogged = 0;

static void Hook_C_TriggerCylinderNetworked_UpdatePartitionListEntry(void* pThis)
{
	if (!pThis)
	{
		C_TriggerCylinderNetworked__UpdatePartitionListEntry(pThis);
		return;
	}

	uint8_t* const pByte = static_cast<uint8_t*>(pThis) + TRG_OFF_CLIENT_SIDE_PREDICTED;
	const int nWire = *pByte;

	// Once per session: value as received, before any force. 1 = dedi authored
	// and the wire carried it; 0 = client force is doing the work alone.
	if (InterlockedCompareExchange(&s_bTrigPredWireLogged, 1, 0) == 0)
		Msg(eDLL_T::CLIENT,
			"[TRIG-PRED] wire m_bClientSidePredicted=%d\n", nWire);

	if (bridge_trigger_clientpredict_force.GetBool() && nWire == 0)
		*pByte = 1;

	C_TriggerCylinderNetworked__UpdatePartitionListEntry(pThis);
}

void VTriggerClientPredictForce::GetFun(void) const
{
	// C_TriggerCylinderNetworked::UpdatePartitionListEntry. Wildcard is the
	// jz rel32 only. BA 0A 00 00 00 (mov edx, 0Ah) excludes the sibling that
	// tests the same offset with mov edx, 2.
	Module_FindPattern(g_GameDll,
		"80 B9 A0 09 00 00 00 0F 84 ?? ?? ?? ?? BA 0A 00 00 00 48 81 C1 B8 03 00 00 E9")
		.GetPtr(C_TriggerCylinderNetworked__UpdatePartitionListEntry);

	if (!C_TriggerCylinderNetworked__UpdatePartitionListEntry)
		Warning(eDLL_T::CLIENT,
			"[TRIG-PRED] C_TriggerCylinderNetworked::UpdatePartitionListEntry pattern "
			"unresolved -- predicted-trigger partition force not installed\n");
}

void VTriggerClientPredictForce::Detour(const bool bAttach) const
{
	if (C_TriggerCylinderNetworked__UpdatePartitionListEntry)
		DetourSetup(&C_TriggerCylinderNetworked__UpdatePartitionListEntry,
			&Hook_C_TriggerCylinderNetworked_UpdatePartitionListEntry, bAttach);
}
