//=============================================================================//
//
// Purpose: Server-authoritative updraft. See header.
// Twelve player fields live in SDKEntityMap and ship as DT_LocalPlayerExclusive.
//
//=============================================================================//
#include "core/stdafx.h"


#include "trigger_updraft.h"
#include "skydive.h"
#include "jetdrive.h"
#include "player.h"
#include "baseentity.h"
#include "basecombatcharacter.h"
#include "entitylist.h"
#include "trigger_cannon.h"
#include "trigger_gravity.h"
#include "game/shared/dt_extend.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/sdk_entity_state.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include <cmath>
#include <cstring>

// gpGlobals is defined in the game module; declare locally -- same pattern
// as jetdrive.cpp.
extern CGlobalVars* gpGlobals;

//-----------------------------------------------------------------------------
// Raw layout -- movement ctx, CMoveData, player.
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t UD_CTX_OFF_PLAYER   = 8;   // CPlayer*
static constexpr ptrdiff_t UD_CTX_OFF_MOVEDATA = 16;  // CMoveData*

// dedi CMoveData: origin at 292, velocity at 304 (not client 292 for vel).
static constexpr ptrdiff_t UD_MV_OFF_ORIGIN   = 292; // float[3]
static constexpr ptrdiff_t UD_MV_OFF_VELOCITY = 304; // float[3]

// m_freefallState -- S3 name for the field the wire renames to m_skydiveState.
static constexpr ptrdiff_t UD_PLAYER_OFF_FREEFALLSTATE = 0x7B60; // 0 = none

// Phase-shift window. Server m_phaseShiftTimeStart @ 0x15B4, end @ 0x15B8. Inclusive vs movement clock.
static constexpr uintptr_t UD_PHASESHIFT_TIMESTART_OFFSET_SERVER = 0x15B4;
static constexpr uintptr_t UD_PHASESHIFT_TIMEEND_OFFSET_SERVER   = 0x15B8;

// Protected CBaseEntity members re-exported with no layout change.
class UpdraftBridge_EntityFieldAccess : public CBaseEntity
{
public:
	using CBaseEntity::m_hMoveParent;
	using CBaseEntity::m_iClassname;
};

// CBaseEntity::IsPlayer is vtable slot 93 (offset 744) -- StartTouch uses it.
// Ground truth: the slot returns `xor al,al` on CBaseEntity/CBaseTrigger and
// `mov al,1` on CPlayer.
static constexpr int UD_VTBL_SLOT_ISPLAYER = 93;

// CPlayer::m_classSettings + 16 -- the resolved settings-block pointer the
// engine's own pose-speed accessor dereferences. Null before a class is
// assigned, so it has to be checked before calling in.
static constexpr ptrdiff_t UD_PLAYER_OFF_SETTINGSBLOCK = 0x5F08;

// PlayerPose. Values read off the Squirrel constant registrar, not assumed:
// STANDING 0, CROUCHING 1, DEAD 2, OBSERVING 3.
static constexpr int UD_PLAYERPOSE_STANDING = 0;

// Raw EHANDLE dword at entity+8 (IHandleEntity).
static constexpr ptrdiff_t UD_ENT_OFF_REFEHANDLE = 0x08;
static constexpr uint32_t  UD_INVALID_EHANDLE    = 0xFFFFFFFFu;

//-----------------------------------------------------------------------------
// ConVars.
//-----------------------------------------------------------------------------
// Boot-time read: dt_extend decides whether to append at SendTable_Init.
static ConVar bridge_updraft_wire(
	"bridge_updraft_wire", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL | FCVAR_ACCESSIBLE_FROM_THREADS,
	"Replicate the 12 updraft fields to the S21 client. Off must mean not on "
	"the wire at all -- an appended zero still ships every snapshot. "
	"Set it as a launch arg, not in-console.");

// Registers trigger_updraft as a CTriggerMultiple factory alias and drives
// the updraft touch callbacks. Off leaves the map entities dropped.
static ConVar bridge_updraft_entity(
	"bridge_updraft_entity", "1", FCVAR_RELEASE,
	"Install the trigger_updraft entity factory (CTriggerMultiple alias) and "
	"fire CodeCallback_PlayerEnter/LeaveUpdraftTrigger on touch. Off drops "
	"the map's updraft brush entities at spawn.");

static ConVar bridge_updraft_diag(
	"bridge_updraft_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Log the updraft movement pass every Nth tick (0 = off). Also gates "
	"[UPDRAFT-WIRE] proxy hits and enter/leave/stage lines.");

// The S21 client suppresses gravity for BOTH of these; one gate, two terms.
static ConVar bridge_movement_gravity_gate(
	"bridge_movement_gravity_gate", "1", FCVAR_RELEASE,
	"Skip the movement pass's half-gravity while an updraft is lifting the player "
	"or a gravity lift is carrying them, as the S21 client does. Off leaves a "
	"downward bias the client never predicts, which mismatches every tick.");

static ConVar bridge_updraft_skydive_handoff(
	"bridge_updraft_skydive_handoff", "1", FCVAR_RELEASE,
	"Hand the end of an updraft ride to the engine's own freefall, as S21 does. "
	"0 leaves the player in plain air movement the moment the last trigger is left, "
	"which is mid-ride -- the lift clears the brush in about a third of a second.");

static ConVar bridge_updraft_anticipate_latch(
	"bridge_updraft_anticipate_latch", "1", FCVAR_RELEASE,
	"Hold the landing-anticipation pose for the whole of an updraft dive. S21 "
	"skips its anticipation predicate entirely while the dive came from an updraft; "
	"0 lets this engine's predicate clear the pose from ground proximity instead.");

static ConVar bridge_updraft_lift_callback(
	"bridge_updraft_lift_callback", "1", FCVAR_RELEASE,
	"Fire CodeCallback_OnPlayerUpdraftLift when the ride reaches its activation "
	"height, as S21 does -- the map script then gives the updraft mod and starts "
	"the dive that carries the rest of the climb. 0 leaves the bridge's own "
	"last-trigger handoff as the only author.");

static constexpr int UD_DIAG_CAP = 64;
static int s_nDiagCount = 0;
static int s_nMoveDiagTick = 0;
// Single-player carry tracking for the move diag -- deliberately not per-player;
// it exists to characterise the move pass, not to drive behaviour.
static float s_flLastVzOut = 0.0f;
static bool  s_bHaveLastVzOut = false;

// Lift callback re-entrancy -- the map script starts a whole skydive.
static bool s_bInLiftCallback = false;
// One fissure_updraft probe report per level.
static bool s_bModProbeReported = false;

bool UpdraftBridge_DiagEnabled(void)
{
	return bridge_updraft_diag.GetInt() > 0;
}

bool UpdraftBridge_SkydiveHandoffEnabled(void)
{
	return bridge_updraft_skydive_handoff.GetBool();
}

bool UpdraftBridge_AnticipateLatchEnabled(void)
{
	return bridge_updraft_anticipate_latch.GetBool();
}

//-----------------------------------------------------------------------------
// Entity factory anchors -- resolved in GetFun.
//-----------------------------------------------------------------------------
static void* (*v_EntityFactoryDictionary)(void) = nullptr;
static void* g_pTriggerMultipleFactory = nullptr;

// GetPoseSpeed_Normal: poseSettings[pose].speed * m_cachedMoveScale. Use the engine accessor.
static float (*v_CPlayer__GetPoseSpeed_Normal)(void* pPlayer, int pose) = nullptr;

// The move driver's half-gravity step. Runs up to three times per command, all of
// which have to be gated, which is why this is hooked instead of the effect being
// cancelled out inside the updraft pass.
static void* (*v_ApplyHalfGravity)(void* pCtx, float flFrameTime) = nullptr;

//-----------------------------------------------------------------------------
// Per-player state. No entity memory for any of the twelve fields.
//-----------------------------------------------------------------------------
static SDKEntityMap<UpdraftState> s_updraftMap(ESide::Server, "updraft.srv");

bool UpdraftBridge_IsLifting(const void* pPlayer)
{
	if (!pPlayer)
		return false;

	const UpdraftState* const pState = s_updraftMap.Find(pPlayer);
	if (!pState || pState->m_updraftStage != UPDRAFT_STAGE_LIFTING)
		return false;

	return pState->m_updraftCount > 0;
}

// Gravity gated while lifting or gravity-lift active.
static void* Hook_ApplyHalfGravity(void* pCtx, float flFrameTime)
{
	if (pCtx && bridge_movement_gravity_gate.GetBool())
	{
		void* const pPlayer = *reinterpret_cast<void**>(
			static_cast<uint8_t*>(pCtx) + UD_CTX_OFF_PLAYER);

		const bool bUpdraft = UpdraftBridge_IsLifting(pPlayer);
		const bool bLift = !bUpdraft && TriggerGravity_IsLiftActive(pPlayer);

		if (bUpdraft || bLift)
		{
			static int s_nGateDiag[2] = {};
			const int nWhich = bUpdraft ? 0 : 1;
			if (bridge_updraft_diag.GetInt() > 0 && s_nGateDiag[nWhich] < 8)
			{
				++s_nGateDiag[nWhich];
				Warning(eDLL_T::SERVER,
					"[GRAV-GATE] suppressed half-gravity #%d src=%s player=%p dt=%.4f\n",
					s_nGateDiag[nWhich], bUpdraft ? "updraft" : "gravitylift",
					pPlayer, flFrameTime);
			}
			return nullptr;
		}
	}

	return v_ApplyHalfGravity(pCtx, flFrameTime);
}

//-----------------------------------------------------------------------------
// Wire sidecar -- seqlock. Snapshot pack runs on worker threads; the game
// thread mutates the SDKEntityMap. Same shape as JetDrive's wire.
//-----------------------------------------------------------------------------
struct UpdraftWireSlot
{
	volatile uint64_t handleKey; // 0 = free
	volatile LONG     seq;       // even = stable, odd = publish in progress
	UpdraftState      wire;
};

static UpdraftWireSlot s_updraftWireSlots[64];
static LONG s_updraftWireCursor = 0;
static volatile LONG s_updraftWireUsed = 0;

static inline uint64_t UpdraftWire_PackHandle(const SDKEntityHandle& h)
{
	return h.IsValid() ? static_cast<uint64_t>(h.Raw()) : 0;
}

static UpdraftWireSlot* UpdraftWire_FindSlot(uint64_t key)
{
	if (!key || !s_updraftWireUsed)
		return nullptr;

	for (UpdraftWireSlot& slot : s_updraftWireSlots)
	{
		if (slot.handleKey == key)
			return &slot;
	}
	return nullptr;
}

static void UpdraftWire_Publish(void* pPlayer, const UpdraftState& s)
{
	const uint64_t key = UpdraftWire_PackHandle(SDKEntityState_GetHandle(pPlayer));
	if (!key)
		return;

	UpdraftWireSlot* slot = UpdraftWire_FindSlot(key);
	if (!slot)
	{
		for (UpdraftWireSlot& cand : s_updraftWireSlots)
		{
			if (cand.handleKey == 0)
			{
				slot = &cand;
				break;
			}
		}
		if (slot)
			InterlockedIncrement(&s_updraftWireUsed);
		else
		{
			const LONG idx = (InterlockedIncrement(&s_updraftWireCursor) - 1) & 63;
			slot = &s_updraftWireSlots[idx];
		}
		slot->handleKey = 0;
	}

	InterlockedIncrement(&slot->seq);
	slot->wire = s;
	InterlockedIncrement(&slot->seq);
	slot->handleKey = key;

	if (!bridge_updraft_diag.GetInt())
		return;

	static volatile LONG s_pubN = 0;
	const LONG n = InterlockedIncrement(&s_pubN);
	if (n <= 8)
		Warning(eDLL_T::SERVER,
			"[UPDRAFT] wire publish #%d player=%p key=0x%llX count=%d stage=%d\n",
			static_cast<int>(n), pPlayer, static_cast<unsigned long long>(key),
			s.m_updraftCount, s.m_updraftStage);
}

static void UpdraftBridge_Mirror(void* pPlayer, const UpdraftState& s)
{
	if (!pPlayer)
		return;

	UpdraftWire_Publish(pPlayer, s);
	MarkEntityEdictDirty(pPlayer);
}

bool UpdraftBridge_WireEnabled(void)
{
	return bridge_updraft_wire.GetBool();
}

bool UpdraftBridge_GetWire(const void* pPlayer, UpdraftState* pOut)
{
	if (!pPlayer || !pOut || !bridge_updraft_wire.GetBool())
		return false;

	const uint64_t key = UpdraftWire_PackHandle(SDKEntityState_GetHandle(pPlayer));
	UpdraftWireSlot* const slot = UpdraftWire_FindSlot(key);
	if (!slot)
		return false;

	for (int attempt = 0; attempt < 8; ++attempt)
	{
		const LONG before = slot->seq;
		if (before & 1)
			continue;
		const UpdraftState copy = slot->wire;
		MemoryBarrier();
		if (slot->seq == before && slot->handleKey == key)
		{
			*pOut = copy;
			return true;
		}
	}

	static volatile LONG s_tornN = 0;
	if (InterlockedIncrement(&s_tornN) <= 8)
		Warning(eDLL_T::SERVER,
			"[UPDRAFT] wire seqlock contended out on player=%p -- zeros this snapshot\n",
			pPlayer);
	return false;
}

void UpdraftBridge_Wire_LevelShutdown(void)
{
	InterlockedExchange(&s_updraftWireUsed, 0);
	memset(s_updraftWireSlots, 0, sizeof(s_updraftWireSlots));
	InterlockedExchange(&s_updraftWireCursor, 0);
	s_nDiagCount = 0;
	s_bHaveLastVzOut = false;
	s_bModProbeReported = false;

	// The per-player state holds up to six trigger EHANDLEs. Indices and
	// serials are reissued on the next map, so a surviving handle would either
	// dedupe away a real enter or leave m_updraftCount stuck above zero.
	s_updraftMap.Clear();
}

//-----------------------------------------------------------------------------
// Entity factory: alias trigger_updraft onto CTriggerMultiple.
// Dictionary vtable: slot0 InstallFactory, slot3 FindFactory.
//-----------------------------------------------------------------------------
void UpdraftBridge_InstallEntityFactory(void)
{
	if (!bridge_updraft_entity.GetBool())
		return;

	if (!v_EntityFactoryDictionary)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER,
				"[UPDRAFT] EntityFactoryDictionary unresolved -- "
				"trigger_updraft factory not installed\n");
		}
		return;
	}

	void* const pDict = v_EntityFactoryDictionary();
	if (!pDict)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER,
				"[UPDRAFT] EntityFactoryDictionary() returned null -- "
				"trigger_updraft factory not installed\n");
		}
		return;
	}

	void** const pVtbl = *reinterpret_cast<void***>(pDict);
	if (!pVtbl)
		return;

	using PFN_FindFactory = void* (__fastcall*)(void* pDict, const char* pszName);
	using PFN_InstallFactory = void (__fastcall*)(void* pDict, void* pFactory,
		const char* pszClassName, const char* pszDebugName);

	const PFN_FindFactory pfnFind =
		reinterpret_cast<PFN_FindFactory>(pVtbl[3]);
	const PFN_InstallFactory pfnInstall =
		reinterpret_cast<PFN_InstallFactory>(pVtbl[0]);
	if (!pfnFind || !pfnInstall)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER,
				"[UPDRAFT] dictionary FindFactory/InstallFactory null -- "
				"trigger_updraft factory not installed\n");
		}
		return;
	}

	// Already installed -- process-lifetime dictionary entry.
	if (pfnFind(pDict, "trigger_updraft"))
		return;

	// Prefer a live lookup of trigger_multiple; fall back to the pattern ptr.
	void* pFactory = pfnFind(pDict, "trigger_multiple");
	if (!pFactory)
		pFactory = g_pTriggerMultipleFactory;
	if (!pFactory)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER,
				"[UPDRAFT] CTriggerMultiple factory not found -- "
				"trigger_updraft not installed\n");
		}
		return;
	}

	pfnInstall(pDict, pFactory, "trigger_updraft", "CTriggerMultiple");
	Msg(eDLL_T::SERVER,
		"[UPDRAFT] installed trigger_updraft factory alias -> CTriggerMultiple\n");
}

//-----------------------------------------------------------------------------
// Touch helpers.
//-----------------------------------------------------------------------------
static bool UpdraftBridge_IsPlayer(void* pEntity)
{
	if (!pEntity)
		return false;

	void** const pVtbl = *reinterpret_cast<void***>(pEntity);
	if (!pVtbl)
		return false;

	using PFN_IsPlayer = bool (__fastcall*)(void* pThis);
	const PFN_IsPlayer pfn =
		reinterpret_cast<PFN_IsPlayer>(pVtbl[UD_VTBL_SLOT_ISPLAYER]);
	return pfn ? pfn(pEntity) : false;
}

static bool UpdraftBridge_IsUpdraftTrigger(void* pTrigger)
{
	if (!pTrigger)
		return false;

	const auto* const pAccess =
		reinterpret_cast<const UpdraftBridge_EntityFieldAccess*>(pTrigger);
	const char* const pszClass = STRING(pAccess->m_iClassname);
	return pszClass && strcmp(pszClass, "trigger_updraft") == 0;
}

static uint32_t UpdraftBridge_EntityHandle(const void* pEntity)
{
	if (!pEntity)
		return UD_INVALID_EHANDLE;
	return *reinterpret_cast<const uint32_t*>(
		static_cast<const uint8_t*>(pEntity) + UD_ENT_OFF_REFEHANDLE);
}

// CodeCallback_PlayerEnter/LeaveUpdraftTrigger(trigger, player). Missing
// script function is a silent no-op after one Warning (same shape as gravity
// enter-cb / jetdrive callbacks).
static void UpdraftBridge_FireTouchCallback(const char* pszName,
	void* pTrigger, void* pPlayer)
{
	if (!g_pServerScript || !pTrigger || !pPlayer)
		return;

	const HSCRIPT hFunc = g_pServerScript->FindFunction(pszName, nullptr, nullptr);
	if (!hFunc)
	{
		static bool s_bWarnedEnter = false;
		static bool s_bWarnedLeave = false;
		bool* pWarned = (pszName && strstr(pszName, "Leave"))
			? &s_bWarnedLeave : &s_bWarnedEnter;
		if (!*pWarned)
		{
			*pWarned = true;
			Warning(eDLL_T::SERVER,
				"[UPDRAFT] %s not found in server VM -- touch callback skipped\n",
				pszName ? pszName : "?");
		}
		return;
	}

	CBaseEntity* const pTrigEnt = reinterpret_cast<CBaseEntity*>(pTrigger);
	CBaseEntity* const pPlayerEnt = reinterpret_cast<CBaseEntity*>(pPlayer);
	const HSCRIPT hTrigger = pTrigEnt->GetScriptInstance();
	const HSCRIPT hPlayer = pPlayerEnt->GetScriptInstance();
	if (!hTrigger || !hPlayer)
		return;

	// Order: (trigger, player).
	ScriptVariant_t args[2];
	args[0] = hTrigger;
	args[1] = hPlayer;
	g_pServerScript->ExecuteFunction(hFunc, args, 2, nullptr, nullptr);
}

// CodeCallback_OnPlayerUpdraftLift(player) -- one argument. Map scripts start
// the dive here; fire only after the pass has finished writing velocity.
static void UpdraftBridge_FireLiftCallback(void* pPlayer)
{
	if (!g_pServerScript || !pPlayer)
		return;

	if (s_bInLiftCallback)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER,
				"[UPDRAFT] CodeCallback_OnPlayerUpdraftLift re-entered -- refused\n");
		}
		return;
	}

	const HSCRIPT hFunc = g_pServerScript->FindFunction(
		"CodeCallback_OnPlayerUpdraftLift", nullptr, nullptr);
	if (!hFunc)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER,
				"[UPDRAFT] CodeCallback_OnPlayerUpdraftLift not found in server VM -- "
				"lift callback skipped; last-trigger handoff remains as fallback\n");
		}
		return;
	}

	CBaseEntity* const pPlayerEnt = reinterpret_cast<CBaseEntity*>(pPlayer);
	const HSCRIPT hPlayer = pPlayerEnt->GetScriptInstance();
	if (!hPlayer)
		return;

	s_bInLiftCallback = true;
	ScriptVariant_t args[1];
	args[0] = hPlayer;
	g_pServerScript->ExecuteFunction(hFunc, args, 1, nullptr, nullptr);
	s_bInLiftCallback = false;
}

void UpdraftBridge_OnTriggerStartTouch(void* pTrigger, void* pOther)
{
	if (!bridge_updraft_entity.GetBool())
		return;
	if (!pTrigger || !pOther)
		return;
	if (!UpdraftBridge_IsUpdraftTrigger(pTrigger))
		return;
	if (!UpdraftBridge_IsPlayer(pOther))
		return;

	const uint32_t nTrigHandle = UpdraftBridge_EntityHandle(pTrigger);
	if (nTrigHandle == UD_INVALID_EHANDLE)
		return;

	UpdraftState& s = s_updraftMap[pOther];

	// Dedupe: already listed -> no callback, no refcount.
	for (int i = 0; i < s.m_touchingUpdraftTriggersCount
		&& i < UPDRAFT_MAX_TOUCHING_TRIGGERS; ++i)
	{
		if (s.m_touchingUpdraftTriggers[i] == nTrigHandle)
			return;
	}

	if (s.m_touchingUpdraftTriggersCount >= UPDRAFT_MAX_TOUCHING_TRIGGERS)
	{
		TriggerPass_EnsureAbsOrigin(pOther);
		const Vector3D& origin =
			reinterpret_cast<CBaseEntity*>(pOther)->Diag_AbsOrigin();
		const char* pszName = reinterpret_cast<CPlayer*>(pOther)->GetNetName();
		if (!pszName || !pszName[0])
			pszName = "?";
		Warning(eDLL_T::SERVER,
			"[UPDRAFT] Player %s at (%.1f, %.1f, %.1f) is in too many updraft "
			"triggers at once (%d)\n",
			pszName, origin.x, origin.y, origin.z, UPDRAFT_MAX_TOUCHING_TRIGGERS);
		return;
	}

	s.m_touchingUpdraftTriggers[s.m_touchingUpdraftTriggersCount++] = nTrigHandle;
	UpdraftBridge_FireTouchCallback(
		"CodeCallback_PlayerEnterUpdraftTrigger", pTrigger, pOther);
}

void UpdraftBridge_OnTriggerEndTouch(void* pTrigger, void* pOther)
{
	if (!bridge_updraft_entity.GetBool())
		return;
	if (!pTrigger || !pOther)
		return;
	if (!UpdraftBridge_IsUpdraftTrigger(pTrigger))
		return;
	if (!UpdraftBridge_IsPlayer(pOther))
		return;

	const uint32_t nTrigHandle = UpdraftBridge_EntityHandle(pTrigger);

	UpdraftState* const pState = s_updraftMap.Find(pOther);
	if (pState && nTrigHandle != UD_INVALID_EHANDLE)
	{
		// Swap-remove if present. Leave callback fires either way.
		for (int i = 0; i < pState->m_touchingUpdraftTriggersCount
			&& i < UPDRAFT_MAX_TOUCHING_TRIGGERS; ++i)
		{
			if (pState->m_touchingUpdraftTriggers[i] != nTrigHandle)
				continue;

			const int nLast = pState->m_touchingUpdraftTriggersCount - 1;
			pState->m_touchingUpdraftTriggers[i] =
				pState->m_touchingUpdraftTriggers[nLast];
			pState->m_touchingUpdraftTriggers[nLast] = UD_INVALID_EHANDLE;
			--pState->m_touchingUpdraftTriggersCount;
			break;
		}
	}

	// Leave is outside the enter-side dedupe -- always fire.
	UpdraftBridge_FireTouchCallback(
		"CodeCallback_PlayerLeaveUpdraftTrigger", pTrigger, pOther);
}

//-----------------------------------------------------------------------------
// Phase / parent and pose-speed helpers.
//-----------------------------------------------------------------------------
float UpdraftBridge_GetPoseSpeedNormal(const void* pPlayer)
{
	if (!pPlayer)
		return 0.0f;

	if (!v_CPlayer__GetPoseSpeed_Normal)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER,
				"[UPDRAFT] GetPoseSpeed_Normal unresolved -- FALLING horizontal "
				"bleed stays off (prediction will diverge horizontally)\n");
		}
		return 0.0f;
	}

	// The accessor indexes straight off this pointer with no null check.
	const void* const pSettings = *reinterpret_cast<void* const*>(
		reinterpret_cast<uintptr_t>(pPlayer) + UD_PLAYER_OFF_SETTINGSBLOCK);
	if (!pSettings)
		return 0.0f;

	const float flSpeed = v_CPlayer__GetPoseSpeed_Normal(
		const_cast<void*>(pPlayer), UD_PLAYERPOSE_STANDING);

	// A settings block mid-reload can hand back garbage; the bleed divides by
	// the horizontal magnitude, so only a finite positive target is usable.
	if (!isfinite(flSpeed) || flSpeed <= 0.0f)
		return 0.0f;

	return flSpeed;
}

bool UpdraftBridge_IsPhaseShiftedAndParented(const void* pPlayer)
{
	if (!pPlayer)
		return false;

	// Movement clock, inclusive at both ends -- same window the move driver uses.
	const float flStart = *reinterpret_cast<const float*>(
		reinterpret_cast<uintptr_t>(pPlayer) + UD_PHASESHIFT_TIMESTART_OFFSET_SERVER);
	const float flEnd = *reinterpret_cast<const float*>(
		reinterpret_cast<uintptr_t>(pPlayer) + UD_PHASESHIFT_TIMEEND_OFFSET_SERVER);
	const float flNow = TriggerPass_MovementTime();

	if (!std::isfinite(flStart) || !std::isfinite(flEnd))
		return false;
	if (!(flNow >= flStart && flNow <= flEnd))
		return false;

	// m_hMoveParent is protected on CBaseEntity -- re-export via zero-size subclass.
	const auto* const pAccess = reinterpret_cast<const UpdraftBridge_EntityFieldAccess*>(pPlayer);
	if (!pAccess->m_hMoveParent.IsValid())
		return false;

	if (!g_serverEntityList)
		return false;

	const CBaseHandle handle = CBaseHandle::UnsafeFromIndex(
		static_cast<int>(pAccess->m_hMoveParent.ToInt()));
	return g_serverEntityList->LookupEntity(handle) != nullptr;
}

//-----------------------------------------------------------------------------
// Script natives.
// params[6] = { minShake, maxShake, liftActivationHeight,
//               liftSpeed, liftAcceleration, liftExitDuration }
//-----------------------------------------------------------------------------
bool UpdraftBridge_EnterUpdraft(void* pPlayer, const float params[6])
{
	if (!pPlayer || !params)
	{
		Warning(eDLL_T::SERVER, "[UPDRAFT] EnterUpdraft rejected null player/params\n");
		return false;
	}

	// Script-callable, so every parameter is attacker-controlled. The engine only
	// range-checks the last three; a NaN passes `< 0` and would reach the
	// velocity write and the wire, so it is rejected here too.
	for (int i = 0; i < 6; ++i)
	{
		if (!isfinite(params[i]))
		{
			Warning(eDLL_T::SERVER,
				"[UPDRAFT] EnterUpdraft rejected non-finite param[%d]\n", i);
			return false;
		}
	}

	// Only these three are range-checked; the heights are free-form world Z.
	if (params[3] < 0.0f)
	{
		Warning(eDLL_T::SERVER,
			"[UPDRAFT] EnterUpdraft rejected liftSpeed=%f (must be non-negative)\n",
			params[3]);
		return false;
	}
	if (params[4] < 0.0f)
	{
		Warning(eDLL_T::SERVER,
			"[UPDRAFT] EnterUpdraft rejected liftAcceleration=%f (must be non-negative)\n",
			params[4]);
		return false;
	}
	if (params[5] < 0.0f)
	{
		Warning(eDLL_T::SERVER,
			"[UPDRAFT] EnterUpdraft rejected liftExitDuration=%f (must be non-negative)\n",
			params[5]);
		return false;
	}

	UpdraftState& s = s_updraftMap[pPlayer];

	// Only the FIRST overlapping trigger writes parameters and stage.
	if (++s.m_updraftCount != 1)
	{
		UpdraftBridge_Mirror(pPlayer, s);
		return true;
	}

	s.m_updraftMinShakeActivationHeight = params[0];
	s.m_updraftMaxShakeActivationHeight = params[1];
	s.m_updraftLiftActivationHeight     = params[2];
	s.m_updraftLiftSpeed                = params[3];
	s.m_updraftLiftAcceleration         = params[4];
	s.m_updraftLiftExitDuration         = params[5];
	s.m_updraftSlowTime                 = 0.0f;

	// Re-entering while SKYDIVE does not reset to FALLING.
	if (s.m_updraftStage != UPDRAFT_STAGE_SKYDIVE)
	{
		s.m_updraftStage = UPDRAFT_STAGE_FALLING;
		// Gates Skydive_IsFromUpdraft -- a stale 1 would claim every later freefall.
		s.m_skydiveFromUpdraft = 0;
		// Enter stamps GetTimeBase, leave stamps curtime -- different clocks.
		s.m_updraftEnterTime = reinterpret_cast<CPlayer*>(pPlayer)->GetTimeBase();
	}

	if (bridge_updraft_diag.GetInt() > 0 && s_nDiagCount < UD_DIAG_CAP)
	{
		++s_nDiagCount;
		Warning(eDLL_T::SERVER,
			"[UPDRAFT] enter player=%p count=%d stage=%d actZ=%.1f speed=%.1f "
			"accel=%.1f poseSpeed=%.1f\n",
			pPlayer, s.m_updraftCount, s.m_updraftStage,
			s.m_updraftLiftActivationHeight, s.m_updraftLiftSpeed,
			s.m_updraftLiftAcceleration, UpdraftBridge_GetPoseSpeedNormal(pPlayer));
	}

	UpdraftBridge_Mirror(pPlayer, s);
	return true;
}

// Last trigger left and lift done: call Player_BeginSkydive with current velocity.
static void UpdraftBridge_BeginSkydiveHandoff(void* pPlayer, UpdraftState& s)
{
	if (!bridge_updraft_skydive_handoff.GetBool())
		return;

	// A dive already in flight is not ours to restart, and the engine asserts on it.
	if (SkydiveBridge_IsFreefalling(pPlayer))
		return;

	// The lift callback is the intended author -- the map script starts the dive at
	// the activation height and the movement step carries it from there. Reaching
	// here with no dive in flight means that never happened.
	if (bridge_updraft_lift_callback.GetBool())
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER,
				"[UPDRAFT] no dive was running at the last trigger leave -- the map's "
				"CodeCallback_OnPlayerUpdraftLift did not start one; falling back to the "
				"bridge handoff\n");
		}
	}

	const Vector3D& vecVelocity =
		reinterpret_cast<CBaseEntity*>(pPlayer)->Diag_AbsVelocity();

	if (!SkydiveBridge_BeginFreefall(pPlayer, vecVelocity))
	{
		// Left at 1 it would suppress EndFreefallAnticipate for an unrelated dive.
		s.m_skydiveFromUpdraft = 0;
		return;
	}

	// The landing pose is the phase an updraft exit is supposed to leave the player
	// in, and the anticipation predicate is disabled for the whole of an updraft
	// dive -- so it is forced here and held by the latch, never re-derived.
	SkydiveBridge_BeginFreefallAnticipate(pPlayer);

	if (bridge_updraft_diag.GetInt() > 0 && s_nDiagCount < UD_DIAG_CAP)
	{
		++s_nDiagCount;
		Warning(eDLL_T::SERVER,
			"[UPDRAFT] skydive handoff player=%p vel=(%.1f, %.1f, %.1f) state=%d\n",
			pPlayer, vecVelocity.x, vecVelocity.y, vecVelocity.z,
			SkydiveBridge_GetFreefallState(pPlayer));
	}
}

bool UpdraftBridge_LeaveUpdraft(void* pPlayer)
{
	if (!pPlayer)
	{
		Warning(eDLL_T::SERVER, "[UPDRAFT] LeaveUpdraft rejected null player\n");
		return false;
	}

	UpdraftState* const pState = s_updraftMap.Find(pPlayer);
	if (!pState || pState->m_updraftCount == 0)
		return true;

	UpdraftState& s = *pState;
	if (--s.m_updraftCount != 0)
	{
		UpdraftBridge_Mirror(pPlayer, s);
		return true;
	}

	// Hands the ride to the skydive system here, exempting a
	// phase-shifted parented player. m_skydiveFromUpdraft is set before the
	// handoff call because the anticipate latch reads it from the first command.
	if (s.m_updraftStage == UPDRAFT_STAGE_LIFTING
		&& !UpdraftBridge_IsPhaseShiftedAndParented(pPlayer))
	{
		s.m_updraftStage = UPDRAFT_STAGE_SKYDIVE;
		s.m_skydiveFromUpdraft = 1;

		UpdraftBridge_BeginSkydiveHandoff(pPlayer, s);
	}

	// Leave stamps curtime, NOT GetTimeBase.
	s.m_updraftLeaveTime = gpGlobals ? gpGlobals->curTime : 0.0f;

	if (bridge_updraft_diag.GetInt() > 0 && s_nDiagCount < UD_DIAG_CAP)
	{
		++s_nDiagCount;
		Warning(eDLL_T::SERVER,
			"[UPDRAFT] leave player=%p stage=%d leaveT=%.3f fromUpdraft=%d\n",
			pPlayer, s.m_updraftStage, s.m_updraftLeaveTime, s.m_skydiveFromUpdraft);
	}

	UpdraftBridge_Mirror(pPlayer, s);
	return true;
}

// The engine's Player_EndSkydive resets these; called from the EndFreefall hook.
void UpdraftBridge_OnFreefallEnded(void* pPlayer)
{
	UpdraftState* const pState = s_updraftMap.Find(pPlayer);
	if (!pState)
		return;

	bool bChanged = false;

	// Guarding on SKYDIVE keeps an unrelated dive ending while the player is
	// riding a different updraft from cancelling that ride.
	if (pState->m_updraftStage == UPDRAFT_STAGE_SKYDIVE)
	{
		pState->m_updraftStage = UPDRAFT_STAGE_FALLING;
		bChanged = true;
	}

	if (pState->m_updraftSlowTime != 0.0f)
	{
		pState->m_updraftSlowTime = 0.0f;
		bChanged = true;
	}

	if (pState->m_skydiveFromUpdraft != 0)
	{
		pState->m_skydiveFromUpdraft = 0;
		bChanged = true;
	}

	if (pState->m_bFissureUpdraftMod != 0)
	{
		pState->m_bFissureUpdraftMod = 0;
		bChanged = true;
	}

	if (bChanged)
		UpdraftBridge_Mirror(pPlayer, *pState);
}

bool UpdraftBridge_GetLiftParams(const void* pPlayer, float* pflSpeed, float* pflAccel)
{
	if (!pPlayer || !pflSpeed || !pflAccel)
		return false;

	const UpdraftState* const pState = s_updraftMap.Find(pPlayer);
	if (!pState)
		return false;

	const float flSpeed = pState->m_updraftLiftSpeed;
	const float flAccel = pState->m_updraftLiftAcceleration;
	if (!std::isfinite(flSpeed) || flSpeed < 0.0f
		|| !std::isfinite(flAccel) || flAccel < 0.0f)
		return false;

	*pflSpeed = flSpeed;
	*pflAccel = flAccel;
	return true;
}

void UpdraftBridge_OnSkydiveStartedFromUpdraft(void* pPlayer)
{
	if (!pPlayer)
		return;

	UpdraftState& s = s_updraftMap[pPlayer];
	bool bChanged = false;

	// Stage stays LIFTING until the last EndTouch.
	if (s.m_skydiveFromUpdraft != 1)
	{
		s.m_skydiveFromUpdraft = 1;
		bChanged = true;
	}

	// Probe once per dive -- the accessor warns on an undefined mod name.
	const int nMod = SkydiveBridge_IsClassModActive(pPlayer, "fissure_updraft") ? 1 : 0;
	if (s.m_bFissureUpdraftMod != nMod)
	{
		s.m_bFissureUpdraftMod = nMod;
		bChanged = true;
	}

	if (!s_bModProbeReported)
	{
		s_bModProbeReported = true;
		if (s.m_bFissureUpdraftMod)
			Msg(eDLL_T::SERVER,
				"[UPDRAFT-MOD] fissure_updraft resolves on this server -- the descent "
				"settings the mod overrides are live\n");
		else
			Warning(eDLL_T::SERVER,
				"[UPDRAFT-MOD] fissure_updraft did NOT resolve for this player's class "
				"settings -- the two descent speed overrides are inert and the exit will "
				"plummet instead of float; the anticipation pose is still held\n");
	}

	if (bChanged)
		UpdraftBridge_Mirror(pPlayer, s);
}

bool UpdraftBridge_HasForceAnticipationMod(const void* pPlayer)
{
	if (!pPlayer)
		return false;

	const UpdraftState* const pState = s_updraftMap.Find(pPlayer);
	return pState && pState->m_bFissureUpdraftMod != 0;
}

void UpdraftBridge_SetSlowTime(void* pPlayer, float flTime)
{
	UpdraftState* const pState = s_updraftMap.Find(pPlayer);
	if (!pState)
		return;

	if (pState->m_updraftSlowTime == flTime)
		return;

	pState->m_updraftSlowTime = flTime;
	UpdraftBridge_Mirror(pPlayer, *pState);
}

void UpdraftBridge_UpdateSlowTime(void* pPlayer, float flActualUpSpeed, float flNow)
{
	UpdraftState* const pState = s_updraftMap.Find(pPlayer);
	if (!pState)
		return;

	const float flThreshold = pState->m_updraftLiftSpeed * 0.1f;
	const float flOld = pState->m_updraftSlowTime;

	// Latch the moment the realised climb rate falls below a tenth of the lift
	// speed; clear it the moment it recovers.
	if (pState->m_updraftSlowTime == 0.0f)
	{
		if (flThreshold > flActualUpSpeed)
			pState->m_updraftSlowTime = flNow;
	}
	else if (flActualUpSpeed >= flThreshold)
	{
		pState->m_updraftSlowTime = 0.0f;
	}

	if (pState->m_updraftSlowTime != flOld)
		UpdraftBridge_Mirror(pPlayer, *pState);
}

bool UpdraftBridge_IsStalled(const void* pPlayer, float flNow)
{
	if (!pPlayer)
		return false;

	const UpdraftState* const pState = s_updraftMap.Find(pPlayer);
	if (!pState || pState->m_updraftSlowTime == 0.0f)
		return false;

	// Stalled for more than two seconds.
	return (flNow - pState->m_updraftSlowTime) > 2.0f;
}

bool UpdraftBridge_IsInsideUpdraftTrigger(const void* pPlayer)
{
	if (!pPlayer)
		return false;

	const UpdraftState* const pState = s_updraftMap.Find(pPlayer);
	return pState && pState->m_updraftCount > 0;
}

bool UpdraftBridge_IsSkydiveFromUpdraft(const void* pPlayer)
{
	if (!pPlayer)
		return false;

	const UpdraftState* const pState = s_updraftMap.Find(pPlayer);
	if (!pState || !pState->m_skydiveFromUpdraft)
		return false;

	const int nFreefall = *reinterpret_cast<const int*>(
		static_cast<const uint8_t*>(pPlayer) + UD_PLAYER_OFF_FREEFALLSTATE);
	return nFreefall != 0;
}

bool UpdraftBridge_IsSkydivingOutUpdraftTrigger(const void* pPlayer)
{
	if (!pPlayer)
		return false;

	const UpdraftState* const pState = s_updraftMap.Find(pPlayer);
	if (!pState)
		return false;

	const int nFreefall = *reinterpret_cast<const int*>(
		static_cast<const uint8_t*>(pPlayer) + UD_PLAYER_OFF_FREEFALLSTATE);
	if (nFreefall == PLAYER_FREEFALL_STATE_NONE
		|| !pState->m_skydiveFromUpdraft
		|| pState->m_updraftStage != UPDRAFT_STAGE_SKYDIVE)
		return false;

	const float flNow = gpGlobals ? gpGlobals->curTime : 0.0f;
	return fmaxf(0.0f, flNow - pState->m_updraftLeaveTime) <= pState->m_updraftLiftExitDuration;
}

//-----------------------------------------------------------------------------
// Per-tick pass -- stage machine. LIFTING integrates off the working velocity
// with half-gravity gated. SKYDIVE is owned by freefall; stage clears on end.
//-----------------------------------------------------------------------------
void UpdraftBridge_ApplyPass(void* pCtx)
{
	if (!pCtx)
		return;

	void* const pPlayer = *reinterpret_cast<void**>(
		static_cast<uint8_t*>(pCtx) + UD_CTX_OFF_PLAYER);
	void* const pMoveData = *reinterpret_cast<void**>(
		static_cast<uint8_t*>(pCtx) + UD_CTX_OFF_MOVEDATA);
	if (!pPlayer || !pMoveData)
		return;

	UpdraftState* const pState = s_updraftMap.Find(pPlayer);
	if (!pState)
		return;

	// Skydive owns the player here; the pass has nothing to apply -- the
	// engine's air move does nothing for this stage either.
	if (pState->m_updraftStage == UPDRAFT_STAGE_SKYDIVE)
		return;

	// Inside a trigger, or still in a residual LIFTING stage after a leave that
	// did not hand off (e.g. phase-shifted and parented).
	if (pState->m_updraftCount <= 0
		&& pState->m_updraftStage == UPDRAFT_STAGE_FALLING)
		return;

	// Drop ground entity while count > 0 (walk would zero the lift vz).
	if (pState->m_updraftCount > 0)
		TriggerPass_SetGroundEntityNull(pPlayer);

	UpdraftState& s = *pState;

	// Alive players that are phase-shifted AND parented skip entirely.
	if (reinterpret_cast<CBaseEntity*>(pPlayer)->Diag_LifeState() == 0
		&& UpdraftBridge_IsPhaseShiftedAndParented(pPlayer))
		return;

	if (JetDrive_IsActive(reinterpret_cast<CPlayer*>(pPlayer)))
		return;

	float* const pVel = reinterpret_cast<float*>(
		static_cast<uint8_t*>(pMoveData) + UD_MV_OFF_VELOCITY);
	const float* const pOrigin = reinterpret_cast<const float*>(
		static_cast<const uint8_t*>(pMoveData) + UD_MV_OFF_ORIGIN);

	float vx = pVel[0];
	float vy = pVel[1];
	float vz = pVel[2];
	const float vzIn = vz;
	const float dt = TriggerPass_FrameTime();
	if (dt <= 0.0f)
		return;

	bool bDirty = false;
	bool bFireLiftCallback = false;

	if (s.m_updraftStage == UPDRAFT_STAGE_FALLING && s.m_updraftCount > 0)
	{
		const float originZ = pOrigin[2];
		const float activationZ = s.m_updraftLiftActivationHeight;

		if (vz < -1.0f)
		{
			// Floor at 1.0 so the divide cannot blow up as we cross the plane.
			const float distanceToActivationHeight = fmaxf(1.0f, originZ - activationZ);
			const float timeToActivationHeight = distanceToActivationHeight / -vz;
			const float verticalDeceleration = (-1.0f - vz) / timeToActivationHeight;

			// Target exactly -1 u/s; fminf keeps the brake from overshooting up.
			vz = fminf(-1.0f, vz + verticalDeceleration * dt);

			// Bleed horizontal speed down to the standing pose speed over the
			// same window the vertical brake uses. Zero means the settings
			// block is not readable yet -- skip rather than bleed toward 0.
			const float target = UpdraftBridge_GetPoseSpeedNormal(pPlayer);
			const float h2 = vx * vx + vy * vy;
			if (target > 0.0f && h2 > target * target)
			{
				const float h = sqrtf(h2);
				const float nx = vx / h;
				const float ny = vy / h;
				const float ns = fmaxf(target,
					h + (fminf(0.0f, target - h) / timeToActivationHeight) * dt);
				vx = ns * nx;
				vy = ns * ny;
			}
		}

		if (activationZ >= originZ)
		{
			s.m_updraftStage = UPDRAFT_STAGE_LIFTING;
			bDirty = true;
			bFireLiftCallback = true;

			if (bridge_updraft_diag.GetInt() > 0 && s_nDiagCount < UD_DIAG_CAP)
			{
				++s_nDiagCount;
				Warning(eDLL_T::SERVER,
					"[UPDRAFT] stage FALLING->LIFTING player=%p z=%.1f actZ=%.1f velZ=%.1f\n",
					pPlayer, originZ, activationZ, vz);
			}
		}
	}
	else if (s.m_updraftStage == UPDRAFT_STAGE_LIFTING && s.m_updraftCount > 0)
	{
		// Integrate working vz with gravity gated; one-sided vs liftSpeed.
		const float liftSpeed = s.m_updraftLiftSpeed;
		if (liftSpeed > vz)
			vz = fminf(liftSpeed, vz + dt * s.m_updraftLiftAcceleration);
	}

	pVel[0] = vx;
	pVel[1] = vy;
	pVel[2] = vz;

	const int nEvery = bridge_updraft_diag.GetInt();
	const bool bActive = (s.m_updraftCount > 0
		|| s.m_updraftStage != UPDRAFT_STAGE_FALLING);

	// carry: this tick's vzIn minus last tick's vzOut. With the gravity gate
	// on, LIFTING carry should sit near zero instead of a steady ~-6.
	const float flCarry = (s_bHaveLastVzOut && bActive) ? (vzIn - s_flLastVzOut) : 0.0f;
	s_flLastVzOut = vz;
	s_bHaveLastVzOut = bActive;

	if (nEvery > 0 && bActive && (++s_nMoveDiagTick % nEvery) == 0)
	{
		Warning(eDLL_T::SERVER,
			"[UPDRAFT-MOVE] stage=%d dt=%.4f vzIn=%.1f vzOut=%.1f carry=%.2f "
			"poseSpeed=%.1f originZ=%.1f activationZ=%.1f count=%d\n",
			s.m_updraftStage, dt, vzIn, vz, flCarry,
			UpdraftBridge_GetPoseSpeedNormal(pPlayer),
			pOrigin[2], s.m_updraftLiftActivationHeight, s.m_updraftCount);
	}

	if (bDirty)
		UpdraftBridge_Mirror(pPlayer, s);

	// After velocity write-back and mirror -- the callback starts a whole skydive.
	if (bFireLiftCallback && bridge_updraft_lift_callback.GetBool())
		UpdraftBridge_FireLiftCallback(pPlayer);
}

//-----------------------------------------------------------------------------
// Squirrel bindings -- SERVER VM only. Client already has all four natively.
//-----------------------------------------------------------------------------
static SQRESULT Script_Player_EnterUpdraft(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)) || !pEntity)
		return SQ_ERROR;

	float params[6] = {};
	for (int i = 0; i < 6; ++i)
	{
		SQFloat fl = 0.0f;
		sq_getfloat(v, 2 + i, &fl);
		params[i] = static_cast<float>(fl);
	}

	UpdraftBridge_EnterUpdraft(pEntity, params);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Player_LeaveUpdraft(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)) || !pEntity)
		return SQ_ERROR;

	UpdraftBridge_LeaveUpdraft(pEntity);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Player_IsInsideUpdraftTrigger(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)) || !pEntity)
		return SQ_ERROR;

	sq_pushbool(v, UpdraftBridge_IsInsideUpdraftTrigger(pEntity));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Skydive_IsFromUpdraft(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)) || !pEntity)
		return SQ_ERROR;

	sq_pushbool(v, UpdraftBridge_IsSkydiveFromUpdraft(pEntity));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void UpdraftBridge_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct)
{
	if (!playerStruct)
		return;

	playerStruct->AddFunction(
		"Player_EnterUpdraft",
		"Script_Player_EnterUpdraft",
		"Enter an updraft: six lift parameters from script (first trigger only)",
		"void",
		"float minShakeActivationHeight, float maxShakeActivationHeight, "
		"float liftActivationHeight, float liftSpeed, float liftAcceleration, "
		"float liftExitDuration",
		false,
		Script_Player_EnterUpdraft);

	playerStruct->AddFunction(
		"Player_LeaveUpdraft",
		"Script_Player_LeaveUpdraft",
		"Leave an updraft trigger (refcount; last leave may hand off to skydive)",
		"void",
		"",
		false,
		Script_Player_LeaveUpdraft);

	playerStruct->AddFunction(
		"Player_IsInsideUpdraftTrigger",
		"Script_Player_IsInsideUpdraftTrigger",
		"True while the player is inside one or more updraft triggers",
		"bool",
		"",
		false,
		Script_Player_IsInsideUpdraftTrigger);

	playerStruct->AddFunction(
		"Skydive_IsFromUpdraft",
		"Script_Skydive_IsFromUpdraft",
		"True when the current skydive was handed off from an updraft",
		"bool",
		"",
		false,
		Script_Skydive_IsFromUpdraft);
}

//-----------------------------------------------------------------------------
// IDetour -- factory anchors + half-gravity gate.
//-----------------------------------------------------------------------------
void VTriggerUpdraftBridge::GetAdr(void) const
{
	LogFunAdr("UpdraftBridge_ApplyPass", reinterpret_cast<void*>(&UpdraftBridge_ApplyPass));
	LogFunAdr("UpdraftBridge_GetWire", reinterpret_cast<void*>(&UpdraftBridge_GetWire));
	LogFunAdr("UpdraftBridge_EnterUpdraft", reinterpret_cast<void*>(&UpdraftBridge_EnterUpdraft));
	LogFunAdr("UpdraftBridge_LeaveUpdraft", reinterpret_cast<void*>(&UpdraftBridge_LeaveUpdraft));
	LogFunAdr("CPlayer::GetPoseSpeed_Normal", reinterpret_cast<void*>(v_CPlayer__GetPoseSpeed_Normal));
	LogFunAdr("ApplyHalfGravity", reinterpret_cast<void*>(v_ApplyHalfGravity));
	LogFunAdr("EntityFactoryDictionary", reinterpret_cast<void*>(v_EntityFactoryDictionary));
	LogVarAdr("g_pTriggerMultipleFactory", g_pTriggerMultipleFactory);
}

void VTriggerUpdraftBridge::GetFun(void) const
{
	// trigger_multiple factory-registration call site. The literal displacement
	// to the "trigger_multiple" string is what makes it unique -- do not wildcard.
	CMemory site = Module_FindPattern(g_GameDll,
		"E8 ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 48 8B C8 4C 8D 05 E4 AC 83 00 "
		"48 8D 15 ?? ?? ?? ?? 4C 8B 10 41 FF 12");

	if (!site)
	{
		Warning(eDLL_T::SERVER,
			"[UPDRAFT] trigger_multiple factory-registration pattern unresolved -- "
			"trigger_updraft entity support disabled\n");
		// Still resolve the half-gravity pattern below -- factory failure must not
		// leave the gravity gate unhooked.
	}
	else
	{
		// Offset 0: call EntityFactoryDictionary().
		// FollowNearCallSelf mutates site to the callee; stash the call-site first.
		const CMemory callSite = site;
		site.FollowNearCallSelf().GetPtr(v_EntityFactoryDictionary);

		// Offset 22: lea rdx, [CTriggerMultiple factory object].
		g_pTriggerMultipleFactory = callSite.Offset(22)
			.ResolveRelativeAddress(3, 7)
			.RCast<void*>();
	}

	// GetPoseSpeed_Normal. Server +0x6134 / +0x5F08; client twin +0x2CB8 / +0x2240.
	Module_FindPattern(g_GameDll,
		"F3 0F 10 81 34 61 00 00 4C 8B C1 8B 0D ?? ?? ?? ?? 85 C9 79 2D "
		"4D 8B 80 08 5F 00 00 8B C1 0F BA F0 1F 42 8B 4C 00 04 "
		"8B 05 ?? ?? ?? ?? 49 03 C8 0F AF C2 48 03 C1 8B 0D DC 08 79 01")
		.GetPtr(v_CPlayer__GetPoseSpeed_Normal);

	if (!v_CPlayer__GetPoseSpeed_Normal)
		Warning(eDLL_T::SERVER,
			"[UPDRAFT] CPlayer::GetPoseSpeed_Normal pattern unresolved -- the "
			"FALLING horizontal bleed is disabled and predicted horizontal "
			"velocity will diverge from the client\n");

	// Half-gravity. Server +0x5B8 / +0x5F08; client twin +0x3C8 / +0x2240.
	Module_FindPattern(g_GameDll,
		"40 53 48 81 EC 80 00 00 00 48 8B 51 08 48 8B D9 0F 29 74 24 70 "
		"F3 0F 10 35 ?? ?? ?? ?? 0F 29 7C 24 60 0F 57 FF "
		"F3 0F 10 92 B8 05 00 00 0F 2E D7 44 0F 29 44 24 50 44 0F 29 4C 24 40 "
		"44 0F 29 54 24 30 44 0F 28 D1 7A 07 75 05 0F 28 CE EB 03 0F 28 CA "
		"8B 0D ?? ?? ?? ?? 48 8B 82 08 5F 00 00")
		.GetPtr(v_ApplyHalfGravity);

	if (!v_ApplyHalfGravity)
		Warning(eDLL_T::SERVER,
			"[UPDRAFT] ApplyHalfGravity pattern unresolved -- gravity is not "
			"suppressed during lift and the client will mispredict every tick "
			"of the ride\n");

	if (!v_EntityFactoryDictionary)
		Warning(eDLL_T::SERVER,
			"[UPDRAFT] EntityFactoryDictionary unresolved -- "
			"trigger_updraft entity support disabled\n");
	if (!g_pTriggerMultipleFactory)
		Warning(eDLL_T::SERVER,
			"[UPDRAFT] CTriggerMultiple factory pointer unresolved -- "
			"will try live FindFactory(\"trigger_multiple\") at install time\n");
}

void VTriggerUpdraftBridge::Detour(const bool bAttach) const
{
	if (v_ApplyHalfGravity)
		DetourSetup(&v_ApplyHalfGravity, &Hook_ApplyHalfGravity, bAttach);
}

