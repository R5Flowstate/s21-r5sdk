//=============================================================================//
//
// Purpose: client-predicted twin of the automantle-exit boost. Same FSM and
// ConVar defaults as the dedi; a divergent default desyncs prediction.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "engine/client/net_bridge_internal.h"
#include "game/client/mantle_boost.h"
#include "game/client/mantle_boost_anim.h"
#include "game/client/zipline_disconnect.h"
#include "game/client/mantle_boost_rui.h"   // MantleBoostCurveDump_Think
#include "game/client/trigger_cannon.h"
#include "game/shared/mantle_boost_curves.h"
#include "mathlib/mathlib.h"

#include <cmath>

//-----------------------------------------------------------------------------
// C_MoveData: view/buttons match dedi; velocity is +292 here, +304 on dedi.
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t MB_MV_OFF_VIEWANGLES = 0x0C;   // QAngle m_vecAbsViewAngles -- the eye-pitch source
static constexpr ptrdiff_t MB_MV_OFF_BUTTONS    = 0x24;   // int m_nButtons (held)
static constexpr ptrdiff_t MB_MV_OFF_OLDBUTTONS = 0x28;   // int m_nOldButtons
static constexpr ptrdiff_t MB_MV_OFF_PRESSED    = 0x2C;   // int m_nButtonsPressed -- activation press-edge input

// ConVars: names/defaults must match the dedi registration.
static ConVar mantle_boost_enabled("mantle_boost_enabled", "1", FCVAR_RELEASE,
	"[MB] master enable for the client-predicted mantle-exit boost. "
	"Must match the dedi-side default.");
static ConVar bridge_mantle_boost_sweet_spot_angle("bridge_mantle_boost_sweet_spot_angle", "2", FCVAR_RELEASE,
	"[MB] sweet-spot angle threshold (deg): |animPitch - eyePitch| must stay BELOW this. "
	"Must match the dedi-side default.");
static ConVar bridge_mantle_boost_sweet_spot_auto("bridge_mantle_boost_sweet_spot_auto", "1", FCVAR_RELEASE,
	"[MB] derive the sweet-spot threshold per traversal state from the baked curve so the "
	"window is the last min_valid_traversal_frac of the climb, as S21's is. 0 uses the "
	"authored bridge_mantle_boost_sweet_spot_angle. Must match the dedi-side default.");
// FCVAR_USERINFO: mode-3 (Movement Ability/custom) uses this mask as the
// activation button -- network it per-client like mantle_boost_input_setting so the dedi
// gates the authoritative boost on the same bits the client predicts.
static ConVar bridge_mantle_boost_button_mask("bridge_mantle_boost_button_mask", "268435456",
	FCVAR_RELEASE | FCVAR_ARCHIVE | FCVAR_USERINFO,
	"[MB] Held-button bitmask when mantle_boost_input_setting=3 (Movement Ability). "
	"Default 268435456 (bit 28 / +dodge). 4 is remapped to bit 28. "
	"FCVAR_USERINFO -> networked to the dedi per-client.");
static ConVar mantle_boost_input_setting("mantle_boost_input_setting", "1",
	FCVAR_RELEASE | FCVAR_ARCHIVE | FCVAR_USERINFO,
	"[MB] Activation input: 0=Off, 1=Jump (S21 default), 2=Crouch, 3=Movement Ability/custom "
	"(bridge_mantle_boost_button_mask). FCVAR_USERINFO -> networked to the dedi per-client; "
	"FCVAR_ARCHIVE so the settings-menu selection persists across restarts.");
static ConVar mantle_boost_require_increasing_view_angle("mantle_boost_require_increasing_view_angle", "0", FCVAR_RELEASE,
	"[MB] Require |delta| still opening (moving away from zero) for the sweet spot (default 0). Must match dedi.");
static ConVar mantle_boost_require_decreasing_view_angle("mantle_boost_require_decreasing_view_angle", "1", FCVAR_RELEASE,
	"[MB] Require |delta| past apex and settling toward zero, either sign (default 1). Must match dedi.");
static ConVar mantle_boost_min_valid_traversal_frac("mantle_boost_min_valid_traversal_frac", "0.5", FCVAR_RELEASE,
	"[MB] Min traversal frac at press for BOOST vs FAILED (default 0.5); at/below -> FAILED (3), above -> BOOST (4). Must match dedi.");
// Fallback when the native standing-pose sprint getter is unresolved; the dedi
// uses the same fallback. State 4 multiplies by sprint_mult.
static ConVar bridge_mantle_boost_exit_speed("bridge_mantle_boost_exit_speed", "200", FCVAR_RELEASE,
	"[MB] base exit speed (u/s) when GetPoseSpeed_Sprint is unresolved. State 4 multiplies by sprint_mult. Must match dedi.");
static ConVar bridge_mantle_boost_sprint_mult("bridge_mantle_boost_sprint_mult", "1.5", FCVAR_RELEASE,
	"[MB] exit speed multiplier on BOOST (state 4); default 1.5 (most legends; Sparrow 1.0). Must match dedi.");
static ConVar bridge_mantle_boost_jump_height("bridge_mantle_boost_jump_height", "90", FCVAR_RELEASE,
	"[MB] jump height (u) for BOOST (state 4) via sqrt(2*gravity*height). "
	"State 3 keeps player_jumpHeight (56). Must match dedi.");
static ConVar bridge_mantle_boost_gravity("bridge_mantle_boost_gravity", "750", FCVAR_RELEASE,
	"[MB] fallback gravity for the BOOST (state 4) vz solve, used only if sv_gravity fails to resolve. "
	"Real Jump reads sv_gravity directly ( JumpHeightToVelocity) -- this is a safety net, not the"
	"primary source. Must match the dedi-side default.");
static ConVar bridge_mantle_boost_apply_log("bridge_mantle_boost_apply_log", "0", FCVAR_DEVELOPMENTONLY,
	"[MB] log every boost finish apply: state, dir, pose, scale, speed, final velocity. Same name on the dedi.");
static ConVar bridge_mantle_boost_trig_log("bridge_mantle_boost_trig_log", "0", FCVAR_DEVELOPMENTONLY,
	"[MB] log each activation press-edge: travState, raw/anim frac, delta, latch. Same name on the dedi.");

// Reading the dedi's m_mantleBoostState off the wire to correct the predicted FSM
// state. The FX is NOT gated on this -- it fires from the predicted finish and the
// wire only adds the case where the dedi granted a boost this side called failed.
static ConVar bridge_mantle_boost_authoritative("bridge_mantle_boost_authoritative", "1", FCVAR_RELEASE,
	"[MB] 1 = take the dedi's m_mantleBoostState off the wire and let it correct the predicted "
	"state (default). 0 = ignore the wire, prediction alone. Either way the FX fires from the "
	"predicted finish.");

bool MantleBoostClient_AuthoritativeEnabled()
{
	return bridge_mantle_boost_authoritative.GetBool();
}

// Post-boost lurch/tap-strafe gate (default on). S21 AirMove_TapStrafe has no native state==4 self-suppress; Hook_AirMove_TapStrafe adds it. Must match dedi.
static ConVar mantle_boost_disables_tap_strafes("mantle_boost_disables_tap_strafes", "1", FCVAR_RELEASE,
	"[MB] Master enable for post-boost lurch/tap-strafe restriction while state==4. Must match dedi.");

//-----------------------------------------------------------------------------
// Activation mask lockstep with dedi: 1=IN_JUMP, 2=duck, 3=custom.
//-----------------------------------------------------------------------------
static int MantleBoost_ActivationButtonMask()
{
	switch (mantle_boost_input_setting.GetInt())
	{
	case 1:  return 2;            // Jump (IN_JUMP)
	case 2:  return 0x04000004;   // Crouch (IN_DUCK | duck-toggle bit)
	case 3:
	{
		const int mask = bridge_mantle_boost_button_mask.GetInt();
		return mask == 4 ? 0x10000000 : mask;
	}
	default: return 0;            // Off -> feature inert
	}
}

//-----------------------------------------------------------------------------
// Datamap walk: flatOffset[0] (TD +0x70) is the live C_Player member offset.
//-----------------------------------------------------------------------------
#define MB_VIDX_GetPredDescMap   14     // C_BaseEntity vtable slot (mirrors pred_diag.cpp)
#define MB_DMAP_OPTIMIZED        0x38   // datamap_t::m_pOptimizedDataMap
#define MB_OPT_INFO_STRIDE       72     // optimized_datamap_t per-type datamapinfo_t stride
#define MB_INFO_FLAT_FIELDS      0      // flattenedoffsets_t::m_Flattened (CUtlVector ptr)
#define MB_INFO_FLAT_COUNT       24     // flattenedoffsets_t::m_Flattened.m_Size

#define MB_TD_STRIDE             0x80   // typedescription_Client_t
#define MB_TD_FIELDNAME          0x08
#define MB_TD_FLATOFFSET0        0x70   // flatOffset[0] (live-member offset)

#define MB_OFFSET_BOUND_LO       0
#define MB_OFFSET_BOUND_HI       0x20000

static bool s_bOffsetsResolved      = false;   // lazy-resolve latch (attempted once)
static int  s_offTraversalFwdDir    = -1;      // m_traversalForwardDir (float[3])
static int  s_offTraversalRefPos    = -1;      // m_traversalRefPos (float[3]) -- ledge basis anchor for the unified delta
static int  s_offSliding            = -1;      // m_sliding (bool)
static int  s_offSlideLongJump      = -1;      // m_slideLongJumpAllowed (bool)
static int  s_offLastSlideWasBoost  = -1;      // m_lastSlideWasBoost (bool)
static int  s_offLastSlideTime      = -1;      // m_lastSlideTime (float)
static int  s_offLastSlideBoost     = -1;      // m_lastSlideBoost (float, native slide speed gain)
static int  s_offVelocity           = -1;      // m_vecVelocity (float[3])

//-----------------------------------------------------------------------------
// Cache traversal-dir offsets once; trigger/finish still use fixed S21 layout.
//-----------------------------------------------------------------------------
static void MantleBoost_ResolveOffsets(void* pPlayerEnt)
{
	if (s_bOffsetsResolved)
		return;

	s_bOffsetsResolved = true;   // attempt exactly once, pass or fail

	if (!pPlayerEnt)
	{
		Warning(eDLL_T::CLIENT, "[MB] offset resolve skipped -- null player entity; "
			"traversal-forward-dir fallback disabled\n");
		return;
	}

	void* (*GetPredDescMap)(void*) = reinterpret_cast<void* (*)(void*)>(
		(*reinterpret_cast<void***>(pPlayerEnt))[MB_VIDX_GetPredDescMap]);
	const uintptr_t dmap = reinterpret_cast<uintptr_t>(GetPredDescMap(pPlayerEnt));
	if (!dmap)
	{
		Warning(eDLL_T::CLIENT, "[MB] offset resolve failed -- GetPredDescMap returned "
			"null; traversal-forward-dir fallback disabled\n");
		return;
	}

	const uintptr_t opt = *reinterpret_cast<uintptr_t*>(dmap + MB_DMAP_OPTIMIZED);
	if (!opt)
	{
		Warning(eDLL_T::CLIENT, "[MB] offset resolve failed -- no optimized datamap; "
			"traversal-forward-dir fallback disabled\n");
		return;
	}

	struct MBFieldWant_t { const char* m_pszName; int* m_pnOut; };
	const MBFieldWant_t wants[] = {
		{ "m_traversalForwardDir",  &s_offTraversalFwdDir   },
		{ "m_traversalRefPos",      &s_offTraversalRefPos   },
		{ "m_sliding",              &s_offSliding           },
		{ "m_slideLongJumpAllowed", &s_offSlideLongJump     },
		{ "m_lastSlideWasBoost",    &s_offLastSlideWasBoost },
		{ "m_lastSlideTime",        &s_offLastSlideTime     },
		{ "m_lastSlideBoost",       &s_offLastSlideBoost    },
		{ "m_vecVelocity",          &s_offVelocity          },
	};

	// The flattened lists split the predicted fields by category
	// (non-networked, networked-only, everything); a field lives in one of them.
	int nWalked = 0;
	for (int cat = 0; cat < 3; ++cat)
	{
		const uintptr_t info = opt + static_cast<uintptr_t>(MB_OPT_INFO_STRIDE) * cat;
		const uintptr_t fields = *reinterpret_cast<uintptr_t*>(info + MB_INFO_FLAT_FIELDS);
		const int count = *reinterpret_cast<int*>(info + MB_INFO_FLAT_COUNT);
		if (!fields || count <= 0 || count > 4096)
			continue;
		nWalked += count;

		for (int i = 0; i < count; ++i)
		{
			const uintptr_t td = fields + static_cast<uintptr_t>(i) * MB_TD_STRIDE;
			const char* name = *reinterpret_cast<const char**>(td + MB_TD_FIELDNAME);
			if (!name)
				continue;

			for (const MBFieldWant_t& w : wants)
			{
				if (strcmp(name, w.m_pszName))
					continue;
				const int flatOff0 = *reinterpret_cast<int*>(td + MB_TD_FLATOFFSET0);
				if (flatOff0 > MB_OFFSET_BOUND_LO && flatOff0 < MB_OFFSET_BOUND_HI && *w.m_pnOut < 0)
					*w.m_pnOut = flatOff0;
				break;
			}
		}
	}

	if (nWalked == 0)
	{
		Warning(eDLL_T::CLIENT, "[MB] offset resolve failed -- flattened field lists "
			"empty/corrupt; traversal-forward-dir fallback disabled\n");
		return;
	}

	Msg(eDLL_T::CLIENT, "[MB] offsets: fwdDir=%d refPos=%d sliding=%d longJump=%d wasBoost=%d "
		"lastSlideTime=%d slideBoost=%d velocity=%d (walked %d fields)\n",
		s_offTraversalFwdDir, s_offTraversalRefPos, s_offSliding, s_offSlideLongJump,
		s_offLastSlideWasBoost, s_offLastSlideTime, s_offLastSlideBoost, s_offVelocity, nWalked);
}

//-----------------------------------------------------------------------------
// FSM state. Ring is the predicted-field save/restore stand-in.
//-----------------------------------------------------------------------------
static int   s_nState                    = 0;      // 0 idle, 1 INVALID (hang), 3 FAILED, 4 BOOST
static float s_flBoostAppliedTime        = 0.0f;   // curtime of the last state-4 finish apply (landing-reset anchor)
static float s_flPrevDelta               = 0.0f;

// Authoritative state from the dedi (DT_Player.m_mantleBoostState). The predicted
// s_nState above is for feel; this one corrects it. -1 = nothing has arrived yet
// earlier, in which case the prediction stands on its own.
static int   s_nAuthState                = -1;
static bool  s_bAuthFxPending            = false;   // drained by MantleBoostClient_FrameUpdate

// Wire sequence binds to s_nLocalClimb so a late verdict cannot land on the next climb.
static int   s_nLocalClimb               = 0;
static int   s_nAuthSeq                  = -1;
static int   s_nAuthClimb                = -1;

// Decision inputs, held from the latching tick to the finish so one [MB-CLIMB]
// line can carry the whole verdict. The dedi emits the identical shape, which is
// what makes a client/dedi divergence a single diff rather than a log correlation.
static const char* s_pszDecisionGate     = "none";
static float s_flDecisionDelta           = 0.0f;
static float s_flDecisionAnim            = 0.0f;
static bool  s_bDecisionNative           = false;

// One screen FX per traversal, whichever source gets there first -- the predicted
// finish or a later authoritative 4. Cleared by MantleBoost_ResetState, i.e. on the
// next traversal, so a boost can never spend two.
static bool  s_bFxFiredThisTraversal     = false;

//-----------------------------------------------------------------------------
// Time-keyed snapshot ring -- prediction-replay determinism core.
//-----------------------------------------------------------------------------
struct MantleRingEnt_t
{
	float m_flTime;
	int   m_nState;
	float m_flPrevDelta;
};

static MantleRingEnt_t s_ring[64];
static int             s_nRingCount = 0;   // kept sorted ascending by m_flTime

static void MantleRing_Reset()
{
	s_nRingCount = 0;
}

// Push the CURRENT statics as the snapshot for prediction time t (overwrite on
// same-time replay, sorted insert otherwise, drop-oldest when full).
static void MantleRing_Push(float flTime)
{
	for (int i = 0; i < s_nRingCount; ++i)
	{
		if (fabsf(s_ring[i].m_flTime - flTime) < 1e-4f)
		{
			s_ring[i].m_nState          = s_nState;
			s_ring[i].m_flPrevDelta = s_flPrevDelta;
			return;
		}
	}

	if (s_nRingCount >= 64)
	{
		for (int i = 1; i < s_nRingCount; ++i)
			s_ring[i - 1] = s_ring[i];
		--s_nRingCount;
	}

	int nInsertAt = s_nRingCount;
	for (int i = 0; i < s_nRingCount; ++i)
	{
		if (s_ring[i].m_flTime > flTime)
		{
			nInsertAt = i;
			break;
		}
	}
	for (int i = s_nRingCount; i > nInsertAt; --i)
		s_ring[i] = s_ring[i - 1];

	s_ring[nInsertAt].m_flTime      = flTime;
	s_ring[nInsertAt].m_nState      = s_nState;
	s_ring[nInsertAt].m_flPrevDelta = s_flPrevDelta;
	++s_nRingCount;
}

// Replayed cmd: restore the snapshot at/preceding time t into the statics
// (instead of evaluating). No snapshot below t -> statics left untouched.
static void MantleRing_Restore(float flTime)
{
	int nBest = -1;
	for (int i = 0; i < s_nRingCount; ++i)
	{
		if (s_ring[i].m_flTime < flTime + 1e-4f)
			nBest = i;
		else
			break;   // ascending order -- nothing further qualifies
	}
	if (nBest < 0)
		return;

	s_nState      = s_ring[nBest].m_nState;
	s_flPrevDelta = s_ring[nBest].m_flPrevDelta;
}

// Last TraversalMove entity (local player) plus its m_RefEHandle serial (+0x8).
// The slot is recycled across a disconnect, so a serial mismatch is the same as
// null. Dereferenced by PostBoostTrace and the FX fire; every use re-checks identity.
static uintptr_t s_pPredictedPlayer = 0;
static uint32_t s_predictedPlayerHandle = 0;
static volatile LONG s_nStaleHandleWarn = 0;

//-----------------------------------------------------------------------------
// Wire: bits 0..2 state, 3..6 sequence, 7..13 edict. Decode index is not identity.
// Never writes s_nState (trigger only runs while idle).
//-----------------------------------------------------------------------------
void MantleBoostClient_OnAuthoritativeState(int nEntIndex, int nWireValue)
{
	// Every early-out here is a different defect, and silence makes them all look
	// like "the wire never arrived". Name the one that fired, for the first few.
	static int s_nRejects = 0;
	const char* pszReject = nullptr;

	const int nState = nWireValue & 7;
	const int nSeq   = (nWireValue >> 3) & 15;
	const int nEdict = (nWireValue >> 7) & 0x7F;

	if (nWireValue >= 0 && nState <= 4)
		MantleBoostAnim_OnWireState(nEdict, nState);

	uintptr_t pEnt  = 0;
	uintptr_t pList = 0;

	if (nWireValue < 0 || nState > 4)
		pszReject = "value out of FSM range";   // peek is reading the wrong bits
	else if (nEdict == 0)
		pszReject = "unencoded value";
	else if (!s_pPredictedPlayer)
		pszReject = "no predicted player yet";   // TraversalMove has not run earlier
	else if ((pList = NetObs_EntityHandleTableAddr()) == 0)
		pszReject = "entity handle table unresolved";
	else
	{
		__try
		{
			pEnt = *reinterpret_cast<const uintptr_t*>(
				pList + 32ull * static_cast<uint32_t>(nEdict));   // 4-qword stride, entity ptr at [idx*4]
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			pEnt = 0;
		}

		if (pEnt != s_pPredictedPlayer)
			pszReject = "another player's verdict";
		else
		{
			uint32_t hNow = 0;
			bool bHandleOk = false;
			__try
			{
				hNow = *reinterpret_cast<const uint32_t*>(s_pPredictedPlayer + 0x8);   // m_RefEHandle serial
				bHandleOk = true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				bHandleOk = false;
			}
			if (!bHandleOk || hNow != s_predictedPlayerHandle)
			{
				s_pPredictedPlayer = 0;
				s_predictedPlayerHandle = 0;
				pszReject = "stale predicted player handle";
			}
		}
	}

	if (pszReject)
	{
		if (++s_nRejects <= 20)
			Warning(eDLL_T::CLIENT, "[MB] authoritative state dropped: wire=0x%04X state=%d ent=%d decIdx=%d: %s (#%d)\n",
				nWireValue, nState, nEdict, nEntIndex, pszReject, s_nRejects);
		return;
	}

	// Announce the first one unconditionally. Everything else here only logs on a
	// disagreement, which makes "the wire never arrived" look exactly like "the
	// wire agreed every time" -- and those need very different fixes.
	static bool s_bAnnouncedWire = false;
	if (!s_bAnnouncedWire)
	{
		s_bAnnouncedWire = true;
		Warning(eDLL_T::CLIENT, "[MB] first authoritative state received: %d (ent %d seq %d)\n",
			nState, nEdict, nSeq);
	}

	// A sequence this side has not seen is the dedi opening a new climb. Bind it to
	// whichever local climb is current and start its verdict from nothing.
	if (nSeq != s_nAuthSeq)
	{
		s_nAuthSeq   = nSeq;
		s_nAuthClimb = s_nLocalClimb;
		s_nAuthState = -1;
	}

	// The local player has started another traversal since this sequence was bound,
	// so the verdict is about a climb that is already over. Applying it is what made
	// a boost land on one mantle and the "too early" readout on the next.
	if (s_nAuthClimb != s_nLocalClimb)
	{
		// The dedi's landing clear (state 0 on the finished sequence) is expected here.
		if (s_nAuthClimb == -1 && nState == 0)
			return;
		if (++s_nRejects <= 20)
			Warning(eDLL_T::CLIENT, "[MB] authoritative state dropped: state=%d seq=%d is for climb %d, on climb %d (#%d)\n",
				nState, nSeq, s_nAuthClimb, s_nLocalClimb, s_nRejects);
		return;
	}

	const int nPrev = s_nAuthState;
	s_nAuthState = nState;

	if (nState != nPrev && bridge_mantle_boost_trig_log.GetBool())
		Msg(eDLL_T::CLIENT, "[MB-AUTH] climb=%d seq=%d state=%d (was %d) pred=%d applied=%d\n",
			s_nLocalClimb, nSeq, nState, nPrev, s_nState, s_flBoostAppliedTime > 0.0f ? 1 : 0);

	// Grant 4 queues FX; skip if prediction already fired. Queue only -- decode must not enter VM.
	if (nState == 4 && nPrev != 4 && !s_bFxFiredThisTraversal)
	{
		s_bFxFiredThisTraversal = true;
		s_bAuthFxPending        = true;
	}
}

//-----------------------------------------------------------------------------
// Drain queued authoritative FX outside snapshot decode.
//-----------------------------------------------------------------------------
static float s_flLastTraceTime = -1.0f;

static void MantleBoost_PostBoostTrace(void)
{
	if (!bridge_mantle_boost_apply_log.GetBool() || !s_pPredictedPlayer ||
		s_flBoostAppliedTime <= 0.0f || s_offVelocity <= 0)
		return;

	uint32_t hNow = 0;
	bool bHandleOk = false;
	__try
	{
		hNow = *reinterpret_cast<const uint32_t*>(s_pPredictedPlayer + 0x8);   // m_RefEHandle serial
		bHandleOk = true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		bHandleOk = false;
	}
	if (!bHandleOk || hNow != s_predictedPlayerHandle)
	{
		s_pPredictedPlayer = 0;
		s_predictedPlayerHandle = 0;
		const LONG n = InterlockedIncrement(&s_nStaleHandleWarn);
		if (n <= 8)
			Warning(eDLL_T::CLIENT, "[MB] stale predicted player handle -- post-boost trace dropped (#%ld)\n", n);
		return;
	}

	const uintptr_t pGlobalsAddr = NetObs_Sym(NetObsSym_t::GlobalVarsPtr);
	if (!pGlobalsAddr)
		return;
	const uintptr_t pGlobals = *reinterpret_cast<const uintptr_t*>(pGlobalsAddr);
	const float flCurTime = pGlobals ? *reinterpret_cast<const float*>(pGlobals + 0x10) : 0.0f;
	if (flCurTime - s_flBoostAppliedTime > 0.25f)
		return;

	if (flCurTime == s_flLastTraceTime)
		return;
	s_flLastTraceTime = flCurTime;

	const uintptr_t p = s_pPredictedPlayer;
	const float* const v = reinterpret_cast<const float*>(p + s_offVelocity);
	Msg(eDLL_T::CLIENT, "[MB-POST] t=%.3f +%.3f vel=(%.1f %.1f %.1f) xy=%.1f sliding=%d longJump=%d wasBoost=%d lastSlideTime=%.3f slideBoost=%.2f\n",
		flCurTime, flCurTime - s_flBoostAppliedTime, v[0], v[1], v[2], sqrtf(v[0] * v[0] + v[1] * v[1]),
		s_offSliding > 0 ? *reinterpret_cast<const uint8_t*>(p + s_offSliding) : -1,
		s_offSlideLongJump > 0 ? *reinterpret_cast<const uint8_t*>(p + s_offSlideLongJump) : -1,
		s_offLastSlideWasBoost > 0 ? *reinterpret_cast<const uint8_t*>(p + s_offLastSlideWasBoost) : -1,
		s_offLastSlideTime > 0 ? *reinterpret_cast<const float*>(p + s_offLastSlideTime) : -1.0f,
		s_offLastSlideBoost > 0 ? *reinterpret_cast<const float*>(p + s_offLastSlideBoost) : -1.0f);
}

static void MantleBoost_ClearIfLanded(uintptr_t pPlayer);

void MantleBoostClient_FrameUpdate(void)
{
	MantleBoost_PostBoostTrace();

	// The grapple mover bypasses AirMove, so the flight-state clear is sampled here too.
	if (s_nState == 4 && s_pPredictedPlayer)
		MantleBoost_ClearIfLanded(s_pPredictedPlayer);

	if (!s_bAuthFxPending)
		return;

	s_bAuthFxPending = false;
	if (!s_pPredictedPlayer)
		return;

	uint32_t hNowFx = 0;
	bool bHandleOkFx = false;
	__try
	{
		hNowFx = *reinterpret_cast<const uint32_t*>(s_pPredictedPlayer + 0x8);   // m_RefEHandle serial
		bHandleOkFx = true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		bHandleOkFx = false;
	}
	if (!bHandleOkFx || hNowFx != s_predictedPlayerHandle)
	{
		s_pPredictedPlayer = 0;
		s_predictedPlayerHandle = 0;
		const LONG n = InterlockedIncrement(&s_nStaleHandleWarn);
		if (n <= 8)
			Warning(eDLL_T::CLIENT, "[MB] stale predicted player handle -- authoritative FX dropped (#%ld)\n", n);
		return;
	}

	if (bridge_mantle_boost_apply_log.GetBool())
		Msg(eDLL_T::CLIENT, "[MB] FX fire authoritative state=4\n");

	NetBridge_FireClientPlayerCallback("CodeCallback_OnPlayerMantleBoosted",
		reinterpret_cast<void*>(s_pPredictedPlayer));
}

static void MantleBoost_ResetState()
{
	MantleRing_Reset();
	s_nState                = 0;
	s_nAuthState            = -1;
	s_flBoostAppliedTime    = 0.0f;
	s_flPrevDelta           = 0.0f;
	s_bFxFiredThisTraversal = false;
	s_bAuthFxPending        = false;
	s_pszDecisionGate       = "none";
	s_flDecisionDelta       = 0.0f;
	s_flDecisionAnim        = 0.0f;
	s_bDecisionNative       = false;
	++s_nLocalClimb;
}

void MantleBoostClient_OnSessionReset(void)
{
	s_pPredictedPlayer = 0;
	s_predictedPlayerHandle = 0;
	MantleBoost_ResetState();
	MantleBoostAnim_OnSessionReset();
	s_flLastTraceTime = -1.0f;
}

uintptr_t MantleBoostClient_GetPredictedPlayer(void)
{
	return s_pPredictedPlayer;
}

//-----------------------------------------------------------------------------
// Traversal ledge forward dir (datamap, then VA getter).
//-----------------------------------------------------------------------------
bool MantleBoostClient_GetTraversalFwd(uintptr_t pPlayer, float out[3])
{
	if (!pPlayer)
		return false;
	MantleBoost_ResolveOffsets(reinterpret_cast<void*>(pPlayer));

	typedef void* (__fastcall* PFN_TraversalVecGetter)(void*, float*);
	if (s_offTraversalFwdDir > 0)
	{
		const float* const p = reinterpret_cast<const float*>(pPlayer + s_offTraversalFwdDir);
		out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
	}
	else
	{
		out[0] = out[1] = out[2] = 0.0f;
		const uintptr_t pGetter = NetObs_Sym(NetObsSym_t::TraversalVecGetter);
		if (!pGetter)
			return false;
		reinterpret_cast<PFN_TraversalVecGetter>(pGetter)(
			reinterpret_cast<void*>(pPlayer), out);
	}
	return (out[0] * out[0] + out[1] * out[1] + out[2] * out[2]) > 1e-6f;
}

//-----------------------------------------------------------------------------
// Eye angles from C_MoveData. Both engines keep m_vecAbsViewAngles at mv+0x0C.
//-----------------------------------------------------------------------------
static bool MantleBoost_ClientEyeAngles(uintptr_t pMove, QAngle* pOut)
{
	const float* const pAng = reinterpret_cast<const float*>(pMove + MB_MV_OFF_VIEWANGLES);
	if (!isfinite(pAng[0]) || !isfinite(pAng[1]))
		return false;
	if (pAng[0] < -90.1f || pAng[0] > 90.1f)
		return false;   // not a plausible eye pitch -- treat as unavailable this tick

	pOut->Init(pAng[0], pAng[1], pAng[2]);
	return true;
}

static constexpr ptrdiff_t MB_PROXY_OFF_VIEWCORR = 5568; // 8 floats: view origin/angles correction + timestamps

//-----------------------------------------------------------------------------
// GetTraversalViewPosition pitch minus eye pitch. Snapshot/restore proxy view-correction.
//-----------------------------------------------------------------------------
static bool MantleBoost_EvalNativeDelta(uintptr_t pPlayer, const QAngle& eyeAngles, float* pflDelta)
{
	if (!C_PredictedFirstPersonProxy__GetTraversalViewPosition || !pPlayer || !pflDelta)
		return false;

	const uint32_t hProxy = *reinterpret_cast<const uint32_t*>(pPlayer + 0x3704);
	if (hProxy == 0xFFFFFFFFu)
		return false;
	const uintptr_t pList = NetObs_EntityHandleTableAddr();
	if (!pList)
		return false;
	const uint32_t nIdx = static_cast<uint16_t>(hProxy);
	const uintptr_t pEntry = pList + 32ull * nIdx;
	const uintptr_t pProxy = *reinterpret_cast<const uintptr_t*>(pEntry);
	const uint32_t nSerial = *reinterpret_cast<const uint32_t*>(pEntry + 8);
	if (!pProxy || nSerial != (hProxy >> 16))
		return false;

	const int nRefId  = *reinterpret_cast<const uint8_t*>(pProxy + 5536);
	const int nCamId  = *reinterpret_cast<const uint8_t*>(pProxy + 5537);
	const int nBaseId = *reinterpret_cast<const uint8_t*>(pProxy + 5538);
	if (nRefId <= 0 || nCamId <= 0 || nBaseId <= 0)
		return false;

	float* const pCorr = reinterpret_cast<float*>(pProxy + MB_PROXY_OFF_VIEWCORR);
	float flSaved[8];
	for (int i = 0; i < 8; ++i)
		flSaved[i] = pCorr[i];

	float flOrg[3] = { 0.0f, 0.0f, 0.0f };
	float flAng[3] = { eyeAngles.x, eyeAngles.y, eyeAngles.z };
	C_PredictedFirstPersonProxy__GetTraversalViewPosition(pProxy, flOrg, flAng);

	for (int i = 0; i < 8; ++i)
		pCorr[i] = flSaved[i];

	if (flOrg[0] == 0.0f && flOrg[1] == 0.0f && flOrg[2] == 0.0f)
		return false;

	const float flDelta = flAng[0] - eyeAngles.x;
	if (!isfinite(flDelta))
		return false;
	*pflDelta = flDelta;
	return true;
}

//-----------------------------------------------------------------------------
// Region-1 trigger (pre-orig, !justStarted && state==0, first-time cmds).
//-----------------------------------------------------------------------------
static void MantleBoost_EvaluateTrigger(uintptr_t pPlayer, uintptr_t pMove, float flCurTime)
{
	// Eye angles out of mv (m_vecAbsViewAngles) -- the dedi twin reads the same
	// field at the same offset, which is what makes the two deltas comparable.
	QAngle eyeAngles;
	if (!MantleBoost_ClientEyeAngles(pMove, &eyeAngles))
		return;   // no usable eye pitch this tick -- skip (bookkeeping untouched)

	// Table delta from shared ledge-frame inputs; GTVP is A/B only.
	const float flRaw = *reinterpret_cast<const float*>(pPlayer + 0x2B7C);       // m_traversalProgress (cycle)
	const int nTravState = *reinterpret_cast<const int*>(pPlayer + 0x2B34);
	float vecFwd[3] = { 0.0f, 0.0f, 0.0f };
	float flLiveDelta = 0.0f;
	bool bHaveDelta = false;
	if (MantleBoostClient_GetTraversalFwd(pPlayer, vecFwd))
	{
		bHaveDelta = MBCurves_Eval(nTravState, flRaw,
			Vector3D(vecFwd[0], vecFwd[1], vecFwd[2]), eyeAngles, &flLiveDelta);
	}

	// Press-edge latch. No curve -> weak tier 3 (do not leave pending).
	const int nPressed = *reinterpret_cast<const int*>(pMove + MB_MV_OFF_PRESSED);
	const float flAnim = *reinterpret_cast<const float*>(pPlayer + 0x2234);   // m_traversalAnimProgress -- min_frac input
	const bool bMantleType = (*reinterpret_cast<const int*>(pPlayer + 0x2B38) == 0);   // m_traversalType
	const bool bDangleClear =
		(*reinterpret_cast<const uint8_t*>(pPlayer + 0x2BAD) == 0) &&
		(*reinterpret_cast<const float*>(pPlayer + 0x2BA8) == 0.0f);
	if ((nPressed & MantleBoost_ActivationButtonMask()) && bMantleType && bDangleClear && s_nState == 0)
	{
		// Decreasing: negative lobe toward zero. Increasing: positive lobe toward zero. No epsilon.
		const bool bDecreasingOk = flLiveDelta <= 0.0f && flLiveDelta >= s_flPrevDelta;
		const bool bIncreasingOk = flLiveDelta >= 0.0f && s_flPrevDelta >= flLiveDelta;
		const char* pszGate = "ok";

		if (!bHaveDelta)
		{
			s_nState = 3;
			pszGate = "nocurve";
		}
		else if (!bDecreasingOk && mantle_boost_require_increasing_view_angle.GetBool())
		{
			s_nState = 3;
			pszGate = "open";
		}
		else if (!bIncreasingOk && mantle_boost_require_decreasing_view_angle.GetBool())
		{
			s_nState = 3;
			pszGate = "dec";
		}
		else if (fabsf(flLiveDelta) >= MantleBoostClient_GetSweetSpotAngle(
			nTravState, Vector3D(vecFwd[0], vecFwd[1], vecFwd[2]), eyeAngles))
		{
			s_nState = 3;
			pszGate = "angle";
		}
		else if (flAnim <= mantle_boost_min_valid_traversal_frac.GetFloat())
		{
			s_nState = 3;
			pszGate = "frac";
		}
		else
			s_nState = 4;

		s_pszDecisionGate = pszGate;
		s_flDecisionDelta = flLiveDelta;
		s_flDecisionAnim  = flAnim;
		s_bDecisionNative = false;

		if (bridge_mantle_boost_trig_log.GetBool())
		{
			// Native rides along as the A/B: table-vs-native is the residual of
			// the ledge-frame model.
			float flNative = 0.0f;
			const bool bNative = MantleBoost_EvalNativeDelta(pPlayer, eyeAngles, &flNative);
			Msg(eDLL_T::CLIENT, "[MB-TRIG] t=%.3f travState=%d raw=%.3f anim=%.3f table=%s%.2f native=%s%.2f prev=%.2f thr=%.2f gate=%s -> %d\n",
				flCurTime, nTravState, flRaw, flAnim,
				bHaveDelta ? "" : "!", flLiveDelta,
				bNative ? "" : "!", bNative ? flNative : 0.0f,
				s_flPrevDelta,
				MantleBoostClient_GetSweetSpotAngle(nTravState,
					Vector3D(vecFwd[0], vecFwd[1], vecFwd[2]), eyeAngles),
				pszGate, s_nState);
		}
	}

	if (bHaveDelta)
		s_flPrevDelta = flLiveDelta;
}

//-----------------------------------------------------------------------------
// Grant always wins. Revoke only before boost velocity is applied.
//-----------------------------------------------------------------------------
static int MantleBoost_ApplyState(void)
{
	if (s_nAuthState != 3 && s_nAuthState != 4)
		return s_nState;

	if (s_nAuthState == 4)
		return 4;

	if (s_nState == 4 && s_flBoostAppliedTime > 0.0f)
	{
		static int s_nLateRevokes = 0;
		if (++s_nLateRevokes <= 20)
			Warning(eDLL_T::CLIENT, "[MB] dedi revoked climb %d AFTER the boost applied -- "
				"trigger parity divergence, prediction stands (#%d)\n", s_nLocalClimb, s_nLateRevokes);
		return 4;
	}

	return 3;
}

int MantleBoostClient_GetState(void)
{
	return MantleBoost_ApplyState();
}

// The gate and the ring MUST read this one function: an indicator derived by a
// different rule than the gate it draws makes a working mechanic look broken.
static float s_flSweetSpotCached = 1.5f;

float MantleBoostClient_GetSweetSpotAngle(int nTravState, const Vector3D& vecFwd,
	const QAngle& eyeAngles)
{
	float flThreshold = bridge_mantle_boost_sweet_spot_angle.GetFloat();

	float flDerived = 0.0f;
	if (bridge_mantle_boost_sweet_spot_auto.GetBool()
		&& MBCurves_AutoThreshold(nTravState, mantle_boost_min_valid_traversal_frac.GetFloat(),
			vecFwd, eyeAngles, &flDerived)
		&& flDerived > flThreshold)
	{
		flThreshold = flDerived;
	}

	s_flSweetSpotCached = flThreshold;
	return flThreshold;
}

// Callers with no traversal inputs to hand (the ring's geometric radius) reuse
// the value the gate or the scrub last derived this climb.
float MantleBoostClient_GetSweetSpotAngle(void)
{
	return s_flSweetSpotCached;
}

static constexpr ptrdiff_t MB_PLAYER_OFF_TIME_LAST_LANDED = 13952; // m_flTimeLastLanded
static constexpr ptrdiff_t MB_PLAYER_OFF_GRAPPLE_ACTIVE   = 11616; // bool m_grappleActive
static constexpr ptrdiff_t MB_PLAYER_OFF_GRAPPLE_POINTS   = 11540; // int m_grapple.m_grapplePointCount
static constexpr ptrdiff_t MB_PLAYER_OFF_GRAPPLE_ATTACHED = 11544; // bool m_grapple.m_grappleAttached
static bool s_bGrappledSinceBoost = false;
static constexpr ptrdiff_t MB_PLAYER_OFF_MOVE_SCALE       = 12920; // m_cachedMoveScale (GetPoseSpeed_Sprint multiplier)
// m_cachedMoveScale = recentLanding * (weapon * m_playerMoveSpeedScale) * (1-moveSlow) * (1+2*statusSev).
// Splitting the script-settable factor out separates a mode/class buff from a weapon or status effect.
static constexpr ptrdiff_t MB_PLAYER_OFF_MOVE_SPEED_SCALE = 12916; // m_playerMoveSpeedScale

static bool MantleBoost_LandedAfterBoost(uintptr_t pPlayer)
{
	if (!pPlayer || s_flBoostAppliedTime <= 0.0f)
		return false;
	return *reinterpret_cast<const float*>(pPlayer + MB_PLAYER_OFF_TIME_LAST_LANDED) > s_flBoostAppliedTime;
}

// Landing and grapple detach both end the boost's flight state.
static void MantleBoost_ClearIfLanded(uintptr_t pPlayer)
{
	if (s_nState != 4 || !pPlayer)
		return;
	const bool bGrappled = *reinterpret_cast<const bool*>(pPlayer + MB_PLAYER_OFF_GRAPPLE_ACTIVE)
		&& *reinterpret_cast<const int*>(pPlayer + MB_PLAYER_OFF_GRAPPLE_POINTS) != 0
		&& *reinterpret_cast<const bool*>(pPlayer + MB_PLAYER_OFF_GRAPPLE_ATTACHED);
	if (bGrappled)
		s_bGrappledSinceBoost = true;
	if (!MantleBoost_LandedAfterBoost(pPlayer) && !(s_bGrappledSinceBoost && !bGrappled))
		return;
	s_bGrappledSinceBoost = false;
	s_nState = 0;
	s_nAuthState = -1;
	s_nAuthClimb = -1;   // climb is over and grounded; a verdict still in flight for it changes nothing
}

// Gravity for the closed-form jump-add solve: live sv_gravity, ConVar fallback.
static float MantleBoost_Gravity(void)
{
	static ConVar* s_pGravity = nullptr;
	if (!s_pGravity && g_pCVar)
		s_pGravity = g_pCVar->FindVar("sv_gravity");
	const float flGravity = s_pGravity ? s_pGravity->GetFloat() : bridge_mantle_boost_gravity.GetFloat();
	return (isfinite(flGravity) && flGravity > 0.0f) ? flGravity : bridge_mantle_boost_gravity.GetFloat();
}

// Traversal ledge forward dir: datamap-resolved live field, engine getter fallback.
static void MantleBoost_GetTraversalFwdDir(uintptr_t pPlayer, float out[3])
{
	out[0] = out[1] = out[2] = 0.0f;
	if (s_offTraversalFwdDir > 0)
	{
		const float* const pFwd = reinterpret_cast<const float*>(pPlayer + s_offTraversalFwdDir);
		out[0] = pFwd[0]; out[1] = pFwd[1]; out[2] = pFwd[2];
		return;
	}
	typedef void* (__fastcall* PFN_GetTraversalForwardDir)(void*, float*);
	const PFN_GetTraversalForwardDir pfnFwd =
		reinterpret_cast<PFN_GetTraversalForwardDir>(
			NetObs_Sym(NetObsSym_t::TraversalVecGetter));
	if (pfnFwd)
		pfnFwd(reinterpret_cast<void*>(pPlayer), out);
}

//-----------------------------------------------------------------------------
// Region-3 boost apply on finish. Overwrites; replays restore state from the ring.
//-----------------------------------------------------------------------------
static void MantleBoost_ApplyBoost(uintptr_t ctx, uintptr_t pPlayer, uintptr_t pMove, bool bFirstTime)
{
	MantleBoost_ResolveOffsets(reinterpret_cast<void*>(pPlayer));   // lazy; fields optional

	// gpGlobals_Client static ptr -> +0x10 curtime.
	const uintptr_t pGlobalsAddr = NetObs_Sym(NetObsSym_t::GlobalVarsPtr);
	if (!pGlobalsAddr)
		return;
	const uintptr_t pGlobals = *reinterpret_cast<const uintptr_t*>(pGlobalsAddr);
	const float flCurTime = pGlobals ? *reinterpret_cast<const float*>(pGlobals + 0x10) : 0.0f;

	const int nApply = MantleBoost_ApplyState();

	// mv gravity-space move dir; fallback traversal fwd; reflect if into the ledge. Do not normalize or z-flip.
	float dir[3] = {
		*reinterpret_cast<const float*>(pMove + 180),   // mv moveDirGravity.x
		*reinterpret_cast<const float*>(pMove + 184),   // mv moveDirGravity.y
		*reinterpret_cast<const float*>(pMove + 188),   // mv moveDirGravity.z
	};
	float fwd[3];
	MantleBoost_GetTraversalFwdDir(pPlayer, fwd);
	if (fmaxf(fmaxf(fabsf(dir[0]), fabsf(dir[1])), fabsf(dir[2])) <= 0.0099999998f)
	{
		dir[0] = fwd[0]; dir[1] = fwd[1]; dir[2] = fwd[2];
	}
	const float flDot = dir[0] * fwd[0] + dir[1] * fwd[1] + dir[2] * fwd[2];
	if (flDot < 0.0f && isfinite(flDot))
	{
		dir[0] -= 2.0f * flDot * fwd[0];
		dir[1] -= 2.0f * flDot * fwd[1];
		dir[2] -= 2.0f * flDot * fwd[2];
	}

	const float flDirAbs = fmaxf(fmaxf(fabsf(dir[0]), fabsf(dir[1])), fabsf(dir[2]));
	const bool bDirOk = (flDirAbs > 0.0099999998f) && isfinite(flDirAbs);

	// Same selection as the dedi: the native standing-pose sprint speed when it
	// resolves and is sane, otherwise the shared ConVar.
	float flSpeed = bridge_mantle_boost_exit_speed.GetFloat();
	float flPose = 0.0f;
	float flScale = 1.0f;
	if (C_Player__GetPoseSpeed_Sprint)
	{
		flPose = C_Player__GetPoseSpeed_Sprint(pPlayer, 0); // PLAYERPOSE_STANDING
		if (isfinite(flPose) && flPose > 0.0f && flPose < 2000.0f)
		{
			flScale = *reinterpret_cast<const float*>(pPlayer + MB_PLAYER_OFF_MOVE_SCALE);
			flSpeed = flPose;
		}
	}
	if (nApply == 4)
		flSpeed *= bridge_mantle_boost_sprint_mult.GetFloat();

	if (bDirOk)
	{
		float* vel = reinterpret_cast<float*>(pMove + 292);   // mv velocity +292..300
		vel[0] = dir[0] * flSpeed;
		vel[1] = dir[1] * flSpeed;
		vel[2] = dir[2] * flSpeed;
	}
	// degenerate dir: keep the native exit velocity, still run the forced jump.

	// Slide-boost cooldown stamp on state-4 velocity arm, before Jump; flags after.
	if (nApply == 4 && s_offLastSlideTime > 0)
		*reinterpret_cast<float*>(pPlayer + s_offLastSlideTime) = flCurTime;

	const bool bCrouchMode = (mantle_boost_input_setting.GetInt() == 2);

	bool bDoJump = false;
	if ((nApply == 4 || !bCrouchMode) && C_GameMovement__Jump)
	{
		bDoJump = true;
		uint32_t* pHeld    = reinterpret_cast<uint32_t*>(pMove + MB_MV_OFF_PRESSED);
		uint32_t* pPressed = reinterpret_cast<uint32_t*>(pMove + MB_MV_OFF_OLDBUTTONS);
		const uint32_t nHeldSaved    = *pHeld;
		const uint32_t nPressedSaved = *pPressed;
		*pHeld    |= 2;
		*pPressed |= 2;

		float* pTimeLastLanded = reinterpret_cast<float*>(pPlayer + 13952);   // m_flTimeLastLanded
		const float flLandedSaved = *pTimeLastLanded;
		*pTimeLastLanded = -100000.0f;

		float* const vel = reinterpret_cast<float*>(pMove + 292);   // mv velocity
		const float velBefore[3] = { vel[0], vel[1], vel[2] };

		C_GameMovement__Jump(reinterpret_cast<void*>(ctx));

		// Jump for side effects; recompose vz as sqrt(2*g*h).
		const float flHeight = (nApply == 4)
			? bridge_mantle_boost_jump_height.GetFloat()
			: 56.0f;   // player_jumpHeight authored default (state 3)
		vel[0] = velBefore[0];
		vel[1] = velBefore[1];
		vel[2] = velBefore[2] + sqrtf(2.0f * MantleBoost_Gravity() * flHeight);

		*pTimeLastLanded = flLandedSaved;
		*pPressed = nPressedSaved;
		*pHeld    = nHeldSaved;
	}
	else if (nApply == 3 && bCrouchMode)
	{
		if (C_GameMovement__Duck)
		{
			uint32_t* const pHeldTrue = reinterpret_cast<uint32_t*>(pMove + MB_MV_OFF_BUTTONS);
			const uint32_t nHeldTrueSave = *pHeldTrue;
			__try
			{
				*pHeldTrue |= 4;   // IN_DUCK
				C_GameMovement__Duck(reinterpret_cast<void*>(ctx));
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				Warning(eDLL_T::CLIENT, "[MB] C_GameMovement::Duck faulted -- crouch-mode failed-attempt duck skipped\n");
			}
			*pHeldTrue = nHeldTrueSave;
		}
		else
		{
			static bool s_bWarnedNoDuck = false;
			if (!s_bWarnedNoDuck)
			{
				s_bWarnedNoDuck = true;
				Warning(eDLL_T::CLIENT, "[MB] C_GameMovement::Duck unavailable -- "
					"crouch-mode failed attempts get no native reaction (neither jump nor duck)\n");
			}
		}
	}
	if (nApply == 4)
	{
		if (s_offSliding > 0)
			*reinterpret_cast<uint8_t*>(pPlayer + s_offSliding) = 1;
		if (s_offSlideLongJump > 0)
			*reinterpret_cast<uint8_t*>(pPlayer + s_offSlideLongJump) = 1;
		if (s_offLastSlideWasBoost > 0)
			*reinterpret_cast<uint8_t*>(pPlayer + s_offLastSlideWasBoost) = 1;
	}

	if (bridge_mantle_boost_apply_log.GetBool())
	{
		const float* const v = reinterpret_cast<const float*>(pMove + 292);
		const float flMoveSpeedScale = *reinterpret_cast<const float*>(pPlayer + MB_PLAYER_OFF_MOVE_SPEED_SCALE);
		Msg(eDLL_T::CLIENT, "[MB-APPLY] t=%.3f state=%d dir=(%.3f %.3f %.3f) pose=%.1f scale=%.3f mss=%.3f speed=%.1f vel=(%.1f %.1f %.1f)\n",
			flCurTime, nApply, dir[0], dir[1], dir[2], flPose, flScale, flMoveSpeedScale, flSpeed, v[0], v[1], v[2]);
	}

	// Predicted finish FX on first-time only; one fire per traversal.
	if (nApply == 4 && bFirstTime && !s_bFxFiredThisTraversal)
	{
		s_bFxFiredThisTraversal = true;
		if (bridge_mantle_boost_apply_log.GetBool())
			Msg(eDLL_T::CLIENT, "[MB] FX fire predicted state=4 t=%.3f\n", flCurTime);
		NetBridge_FireClientPlayerCallback("CodeCallback_OnPlayerMantleBoosted",
			reinterpret_cast<void*>(pPlayer));
	}

	if (nApply == 4)
		s_flBoostAppliedTime = flCurTime;
		s_bGrappledSinceBoost = false;

	if (bFirstTime && bridge_mantle_boost_trig_log.GetBool())
		Msg(eDLL_T::CLIENT, "[MB-CLIMB] side=cl climb=%d pred=%d auth=%d apply=%d "
			"gate=%s delta=%.2f src=%s anim=%.3f speed=%.1f jump=%d t=%.3f\n",
			s_nLocalClimb, s_nState, s_nAuthState, nApply, s_pszDecisionGate,
			s_flDecisionDelta, s_bDecisionNative ? "native" : "recon", s_flDecisionAnim,
			flSpeed, bDoJump ? 1 : 0, flCurTime);

	static bool s_bAnnounced = false;
	if (!s_bAnnounced)
	{
		s_bAnnounced = true;
		Warning(eDLL_T::CLIENT, "[MB] FIRST FIRE -- predicted mantle-exit boost ACTIVE. "
			"state=%d pose=%.1f scale=%.3f mss=%.3f speed=%.1f jump=%d t=%.3f\n",
			nApply, flPose, flScale,
			*reinterpret_cast<const float*>(pPlayer + MB_PLAYER_OFF_MOVE_SPEED_SCALE),
			flSpeed, bDoJump ? 1 : 0, flCurTime);
	}
}

//-----------------------------------------------------------------------------
// AirMove_TapStrafe: while state==4 and cvar on, skip native (void return unused).
// S21 has no baked-in gate.
//-----------------------------------------------------------------------------
static char __fastcall Hook_AirMove_TapStrafe(void* ctxRaw)
{
	if (!C_GameMovement__AirMove_TapStrafe)
		return 0;

	// Landing reset: m_flTimeLastLanded > boost-apply (`>` so finish-tick equal stays restricted).
	if (ctxRaw)
	{
		const void* const pFlightPlayer = *reinterpret_cast<void* const*>(
			reinterpret_cast<uintptr_t>(ctxRaw) + 8);   // ctx+8 C_Player*
		if (TriggerCannonClient_IsFlightLocked(pFlightPlayer))
			return 0;
	}

	if (s_nState == 4 && ctxRaw)
	{
		const uintptr_t pPlayer = *reinterpret_cast<const uintptr_t*>(
			reinterpret_cast<uintptr_t>(ctxRaw) + 8);   // ctx+8 C_Player*
		if (pPlayer)
			MantleBoost_ClearIfLanded(pPlayer);
	}

	if (mantle_boost_enabled.GetBool() && mantle_boost_disables_tap_strafes.GetBool() && s_nState == 4)
	{
		return 0;   // suppress tap-strafe while boosted
	}

	return C_GameMovement__AirMove_TapStrafe(ctxRaw);
}

//-----------------------------------------------------------------------------
// C_GameMovement::TraversalMove: reset, trigger, orig, finish apply.
//-----------------------------------------------------------------------------
static char __fastcall Hook_TraversalMove(void* ctxRaw, char justStarted)
{
	const uintptr_t ctx = reinterpret_cast<uintptr_t>(ctxRaw);

	if (justStarted && ctx)
	{
		const uintptr_t pDiscPlayer = *reinterpret_cast<const uintptr_t*>(ctx + 8);
		const uintptr_t pFirstPred = NetObs_Sym(NetObsSym_t::IsFirstTimePredicted);
		if (pDiscPlayer && pFirstPred
			&& *reinterpret_cast<const unsigned char*>(pFirstPred) != 0)
			ZipDisc_OnMantle(reinterpret_cast<void*>(pDiscPlayer));
	}

	if (!mantle_boost_enabled.GetBool())
	{
		MantleBoost_ResetState();
		return C_GameMovement__TraversalMove(ctxRaw, justStarted);
	}

	const uintptr_t pPlayer = ctx ? *reinterpret_cast<const uintptr_t*>(ctx + 8)  : 0;   // ctx+8 C_Player*
	const uintptr_t pMove   = ctx ? *reinterpret_cast<const uintptr_t*>(ctx + 16) : 0;   // ctx+16 C_MoveData*
	if (!pPlayer || !pMove)
		return C_GameMovement__TraversalMove(ctxRaw, justStarted);

	// Client movement runs only for the locally predicted player, so this is how
	// the authoritative handler recognises which entity's state belongs to us.
	// m_RefEHandle (+0x8) is the serial identity: a disconnect recycles the slot.
	s_pPredictedPlayer = pPlayer;
	__try
	{
		s_predictedPlayerHandle = *reinterpret_cast<const uint32_t*>(pPlayer + 0x8);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		s_pPredictedPlayer = 0;
		s_predictedPlayerHandle = 0;
	}

	float flProgressBefore = 1.0f;
	float flJumpOffBefore  = 0.0f;   // m_wallDangleJumpOffTime pre-orig (backup auto-complete, see post-orig undo)
	bool  bFirstTime = false;   // hoisted: the post-orig FX fire reuses it (IsFirstTimePredicted is stable across the orig call)
	__try
	{
		flProgressBefore = *reinterpret_cast<const float*>(pPlayer + 0x2B7C);   // m_traversalProgress
		flJumpOffBefore  = *reinterpret_cast<const float*>(pPlayer + 0x2BA8);

		// gpGlobals_Client static ptr -> +0x10 curtime.
		const uintptr_t pGlobalsAddr = NetObs_Sym(NetObsSym_t::GlobalVarsPtr);
		const uintptr_t pGlobals = pGlobalsAddr
			? *reinterpret_cast<const uintptr_t*>(pGlobalsAddr) : 0;
		const float flCurTime = pGlobals ? *reinterpret_cast<const float*>(pGlobals + 0x10) : 0.0f;

		const uintptr_t pFirstPred = NetObs_Sym(NetObsSym_t::IsFirstTimePredicted);
		if (!pFirstPred)
		{
			static uint32_t s_nFirstPredMiss = 0;
			if (++s_nFirstPredMiss <= 8)
				Warning(eDLL_T::CLIENT, "[MB] IsFirstTimePredicted unresolved -- skipped trigger this command\n");
		}
		else
			bFirstTime = *reinterpret_cast<const unsigned char*>(pFirstPred) != 0;

		// The engine's entry call is the climb boundary, on first-time cmds only
		// (replays must not wipe the ring). m_traversalStartTime is not a boundary:
		// a correction adopts the dedi's clock-shifted stamp mid-climb.
		if (bFirstTime && justStarted)
			MantleBoost_ResetState();

		if (flCurTime > 0.0f && isfinite(flCurTime))
		{
			if (bFirstTime)
			{
				if (!justStarted && s_nState == 0)
					MantleBoost_EvaluateTrigger(pPlayer, pMove, flCurTime);
				MantleRing_Push(flCurTime);   // snapshot AFTER the (possible) eval
				if (!justStarted)
					MantleBoostCurveDump_Think(pPlayer);
			}
			else
			{
				MantleRing_Restore(flCurTime);   // replayed cmd: restore, never evaluate
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		static uint32_t s_nTrigFaults = 0;
		if (++s_nTrigFaults <= 8)
			Warning(eDLL_T::CLIENT, "[MB] exception #%u in trigger eval -- skipped this command\n",
				s_nTrigFaults);
	}

	const char ret = C_GameMovement__TraversalMove(ctxRaw, justStarted);

	__try
	{
		// Armed states (>=3): undo wallDangleJumpOffTime stamp post-orig so auto-dismount
		// does not abort the pull-through; state 1 keeps native dismount.
		if (MantleBoost_ApplyState() >= 3 && flJumpOffBefore == 0.0f)
		{
			float* const pJumpOff = reinterpret_cast<float*>(pPlayer + 0x2BA8);
			if (*pJumpOff != 0.0f)
				*pJumpOff = 0.0f;
		}

		const float flProgress = *reinterpret_cast<const float*>(pPlayer + 0x2B7C);   // m_traversalProgress
		if (MantleBoost_ApplyState() >= 3 && flProgressBefore < 1.0f && flProgress == 1.0f)
			MantleBoost_ApplyBoost(ctx, pPlayer, pMove, bFirstTime);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		static uint32_t s_nApplyFaults = 0;
		if (++s_nApplyFaults <= 8)
			Warning(eDLL_T::CLIENT, "[MB] exception #%u in boost apply -- skipped this command\n",
				s_nApplyFaults);
	}

	return ret;
}

//-----------------------------------------------------------------------------
// IDetour implementation
//-----------------------------------------------------------------------------
void VMantleBoostClient::GetFun(void) const
{


	Module_FindPattern(g_GameDll,
		"88 54 24 ?? 55 53 57 41 55")
		.GetPtr(C_GameMovement__TraversalMove);

	// C_GameMovement::Jump (S21) -- verified UNIQUE (S21 layout).
	Module_FindPattern(g_GameDll,
		"40 55 53 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 4C 8B 41")
		.GetPtr(C_GameMovement__Jump);


	// C_GameMovement::Duck (S21) -- call-only for crouch-mode FAILED finish.
	Module_FindPattern(g_GameDll,
		"48 8B C4 53 57 48 83 EC ? 4C 8B 41")
		.GetPtr(C_GameMovement__Duck);

	// C_GameMovement::AirMove_TapStrafe (S21). HOOKED, not call-only.
	Module_FindPattern(g_GameDll,
		"40 53 48 81 EC ? ? ? ? 4C 8B 49")
		.GetPtr(C_GameMovement__AirMove_TapStrafe);

	// C_Player::GetPoseSpeed_Sprint. The Sprint/Normal twins differ only in one rip
	// global, so resolve via the GetPoseSpeed dispatcher: its jmp at +0x18 is Sprint.
	Module_FindPattern(g_GameDll,
		"80 B9 2C 2A 00 00 00 8B 81 28 03 00 00 74 ?? 85 C0 7F ?? BA 02 00 00 00 E9")
		.Offset(0x18)
		.FollowNearCallSelf()
		.GetPtr(C_Player__GetPoseSpeed_Sprint);
	if (!C_Player__GetPoseSpeed_Sprint)
		Warning(eDLL_T::CLIENT, "[MB] GetPoseSpeed_Sprint pattern unresolved -- "
			"boost exit speed falls back to bridge_mantle_boost_exit_speed\n");

	// C_PredictedFirstPersonProxy::GetTraversalViewPosition (S21 3-arg).
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 55 56 57 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? "
		"48 81 EC ?? ?? ?? ?? 48 8B F9")
		.GetPtr(C_PredictedFirstPersonProxy__GetTraversalViewPosition);
	if (!C_PredictedFirstPersonProxy__GetTraversalViewPosition)
		Warning(eDLL_T::CLIENT, "[MB] GetTraversalViewPosition pattern unresolved -- "
			"sweet-spot delta falls back to reconstruction\n");
}

///////////////////////////////////////////////////////////////////////////////
void VMantleBoostClient::Detour(const bool bAttach) const
{
	if (C_GameMovement__TraversalMove)
	{
		DetourSetup(&C_GameMovement__TraversalMove, &Hook_TraversalMove, bAttach);
	}
	else if (bAttach)
	{
		Warning(eDLL_T::CLIENT, "[MB] disabled: TraversalMove pattern unresolved\n");
	}

	if (C_GameMovement__AirMove_TapStrafe)
	{
		DetourSetup(&C_GameMovement__AirMove_TapStrafe, &Hook_AirMove_TapStrafe, bAttach);
	}
	else if (bAttach)
	{
		Warning(eDLL_T::CLIENT, "[MB] AirMove_TapStrafe pattern unresolved -- lurch-restrict disabled\n");
	}

	// Jump / Duck / GetTraversalViewPosition are call-only; unresolved is degraded, not
	// fatal (no forced jump / entry-pitch fallback) -- announce loudly anyway.
	if (bAttach && !C_GameMovement__Jump)
		Warning(eDLL_T::CLIENT, "[MB] Jump pattern unresolved -- forced boost jump disabled\n");
	if (bAttach && !C_GameMovement__Duck)
		Warning(eDLL_T::CLIENT, "[MB] Duck pattern unresolved -- crouch-mode failed-attempt duck disabled\n");
	if (bAttach && !C_PredictedFirstPersonProxy__GetTraversalViewPosition)
		Warning(eDLL_T::CLIENT, "[MB] GetTraversalViewPosition unresolved -- trigger uses reconstruction\n");
}
///////////////////////////////////////////////////////////////////////////////
