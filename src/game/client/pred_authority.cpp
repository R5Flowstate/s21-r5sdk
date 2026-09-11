//=============================================================================//
//
// Purpose: Prediction-authority engine (S21 client). Implements the
// declarative per-field table declared in pred_authority.h -- the boot-time
// table validator, the per-dmap resolver (kind detection + tolerance writes +
// slot binding), the repair self-check, the unfed/clock-rebase/grace/byteMask
// apply passes, and the FORCED0-NEUT dispatch tally. See
// for the full design.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/module.h"
#include "tier1/cvar.h"
#include "datamap.h"            // fieldtype_t (FIELD_FLOAT, FIELD_EHANDLE,...)
#include "game/client/pred_authority.h"
#include "game/client/pred_authority_probes.h"
#include "game/client/pred_diag.h"   // C_BaseEntity__GetPredictedEntityState
#include "game/client/cliententitylist.h" // g_pClientEntityList (weapon PNR before player)
#include "engine/client/net_observer.h"

//-----------------------------------------------------------------------------
// [PRED-UNFED] THE crouch/jump correction fix. S21 added networked+predicted
// player fields that DO NOT EXIST in S3 (string-absent from the S3 binary): the dedi's dt_extend
// carries the S21-shaped props with no S3 source (s21_dt_schema.h maps them empty), so the wire
// honestly delivers a constant 0 while the client predicts real values (duck timer 158-393ms,
// jump press times,...). Every ack during a crouch/jump the PNR compare then sees pred!=0 vs
// srv==0 -> false "had errors" -> the rebase copies originalData (0) over the client's LIVE state
// -> the duck transition timer dies mid-transition (viewOffset snaps 60->32 instantly while the
// real server lerps over ~250ms), jump/land state resets -> the felt crouch/jump corrections.
// Measured (pred_diff.log 40MB tail): srv==0 on 100% of samples for exactly these five fields --
// m_currentFrameLocalPlayer.m_duckTransitionRemainderMsec (497/497), m_prevJumpPressTime
// (1353/1353), m_lastSprintPressTime (460/460), m_touchedGroundSinceLastGlide (178/178),
// m_ziplineReattachCooldownTime (6/6). FIX: pre-PNR, copy the PREDICTED frame's value over the
// originalData (server-baseline) slot for exactly these fields -> the compare sees equal (no
// false error) AND the restore writes the predicted value back (no stomp). The client owns these
// input-derived timers (in native S21 the server computes them from the same usercmds it replays
// verbatim, so genuine divergence is not expected); every field the S3 wire ACTUALLY feeds stays
// fully server-authoritative.
ConVar sdk_pred_unfed_mask(
	"sdk_pred_unfed_mask", "1", FCVAR_RELEASE,
	"[PRED-UNFED] neutralize the S21-only predicted fields the S3 bridge wire never feeds "
	"(srv always 0): copy pred over the originalData slot pre-compare so they neither flag "
	"false prediction errors nor get stomped to 0 on rebase. 1=on. Default 1.");

// [PRED-TOL] tolerances for the cross-binary float-drift fields (0 = leave native).
// Values chosen an order of magnitude above the measured drift (sub-0.1u origin,
// sub-1u/s velocity) and an order below real misprediction magnitudes.
// 0.60 absorbs sub-unit land snaps; multi-unit land / zip path
// divergences (2-20u) still flag for Shape C.
ConVar sdk_pred_tol_origin("sdk_pred_tol_origin", "0.60", FCVAR_RELEASE,
	"[PRED-TOL] fieldTolerance for m_localOrigin in the pred errorcheck (absorbs "
	"S3-vs-S21 cross-binary float drift + sub-unit land snaps; real multi-unit "
	"mispredictions still flag). 0 = native.");
ConVar sdk_pred_tol_velocity("sdk_pred_tol_velocity", "2.0", FCVAR_RELEASE,
	"[PRED-TOL] fieldTolerance for m_vecVelocity/m_vecAbsVelocity. 0 = native.");

// Live census: m_flFallVelocity routinely differs by 1 unit on zip/land
// acks (47/48, 100/101) with origin already in-tol -- pure one-tick gravity
// residue, not a gameplay correction. Shape B.
ConVar sdk_pred_tol_fallvelocity("sdk_pred_tol_fallvelocity", "3.0", FCVAR_RELEASE,
	"[PRED-TOL] fieldTolerance for m_flFallVelocity (one-tick gravity residue at "
	"land/zip edges; real multi-tick fall divergences still flag). 0 = native.");

// [PRED-SAT] The dedicated server ships m_flFallVelocity capped at 4096 while
// its own physics is uncapped. sv_maxvelocity is 34000, so this is the prop's
// own ceiling, not the velocity clamp. A prediction past the ceiling can never
// match; the native errorcheck flags every tick of a long fall and one flagged
// predictable forces a full restore+replay of every predicted field.
ConVar sdk_pred_saturate_mask("sdk_pred_saturate_mask", "1", FCVAR_RELEASE,
	"[PRED-SAT] 1 = treat a SATURATING field as equal when the server value sits on "
	"its wire ceiling and the client's prediction is at or above it (the wire cannot "
	"express the larger value). 0 = native compare (every tick past the cap errors).");

ConVar sdk_pred_fallvel_cap("sdk_pred_fallvel_cap", "4096", FCVAR_RELEASE,
	"[PRED-SAT] wire ceiling for m_flFallVelocity as shipped by the S3 dedi. Values "
	"below it still compare under sdk_pred_tol_fallvelocity, so genuine fall "
	"mispredictions keep flagging.");

// [PRED-TOL-TIME] remainder bookkeeping residual under TB-NATIVE is smaller than
// the old clamp sawtooth but still non-zero at connect (live tBd ~0.01..0.19).
// Default 0.06 (~1.2 ticks) absorbs remainder/stamp float noise; genuine multi-
// tick clock resets still flag. Zoom stamps are Shape-A unfed so tol is free.
ConVar sdk_pred_tol_time("sdk_pred_tol_time", "0.06", FCVAR_RELEASE,
	"[PRED-TOL] fieldTolerance for the pure clock-bookkeeping TIME fields "
	"(m_lastUCmdSimulationRemainderTime, m_airMoveBlockPlaneTime) and the once-written "
	"ride/zoom stamp fields (m_traversalStartTime, m_traversalBlendOutStartTime, "
	"m_traversalReleaseTime, m_traversalHandAppearTime, m_wallDangleJumpOffTime, m_wallRunStartTime, "
	"m_wallRunClearTime, m_lastZiplineDetachTime, m_zoomBaseTime, m_zoomFullStartTime). "
	"Default 0.06 under TB-NATIVE (~1 tick residual). 0 = native.");

// The server clears the once-written ride/wallrun stamps in one write on respawn
// and character setup; the client's predicted copies still hold the previous
// life's values, so the first ack after spawn rebases on a bundle of stamps that
// carry no movement content. A cleared or bulk-assigned server bundle is adopted
// before the compare instead.
ConVar sdk_pred_stamp_reset_adopt("sdk_pred_stamp_reset_adopt", "1", FCVAR_RELEASE,
	"[PRED-STAMP-RESET] 1 = when the server clears a once-written stamp (value <= 0) "
	"or rewrites 3+ of them to one identical value in the same ack, copy the server "
	"values over the predicted record and live member before the native compare. "
	"0 = native compare (every respawn flags the stamp bundle).");

ConVar sdk_pred_teleport_adopt("sdk_pred_teleport_adopt", "1", FCVAR_RELEASE,
	"[PRED-TELEPORT-ADOPT] 1 = on a teleport-sized origin delta, skip the unfed "
	"mask so waiting-room jump/duck timers restore from the server instead of "
	"surviving into the pad repredict. 0 = keep unfed on those acks.");
ConVar sdk_pred_teleport_dist("sdk_pred_teleport_dist", "64", FCVAR_RELEASE,
	"[PRED-TELEPORT-ADOPT] origin delta (units) that counts as a teleport. "
	"Below this, unfed stays on. Heading drift is a few units; pad snaps are hundreds.");

// [SCRIPTVEC] The script-driven weapon vector family is S21-only -- this dedi owns
// no backing member, so its appended wire slots were aliasing live CWeaponX state
// (DT_WeaponX base 6100 vs live fields past +11400). Client owns the value; the
// dedi half zero-proxies the same five props.
static ConVar bridge_scriptvec_client("bridge_scriptvec_client", "1", FCVAR_RELEASE,
	"[SCRIPTVEC] Client owns the m_scriptVector* family (mask + repair). These five "
	"S21-only props have no S3 member behind them, so the wire slot aliased live "
	"weapon state -- together the top two weapon offenders, 48% of all weapon blames. "
	"1 = client owns (default), 0 = native compare against the wire value.");

// The dedi folds the view punch into the direction it actually fires along, so
// the two springs have to reconcile. 1 restores the legacy mask (the A/B control).
static ConVar bridge_punch_legacy_mask("bridge_punch_legacy_mask", "0", FCVAR_RELEASE,
	"1 = keep the legacy CLIENT_TIMING mask on the m_vecPunch* rows (they never "
	"reconcile). 0 = let the ack correct them, so the client's punch tracks the "
	"punch the server aims with.");

// [PRED-CLOCK-REBASE] Fix for the clock-derived false-verdict class
// (supersedes putting these fields under a blunt time tolerance, which (a) also hid genuine
// sub-0.12s corrections and (b) could not stop a GENUINE error ack's rebase from restoring
// server-DOMAIN gate times onto the client -- the fire gate re-opened anyway).
// MECHANISM: the player clock m_currentFramePlayer.timeBase differs across the bridge by an
// irreducible +-2-tick sawtooth (the S3 dedi batches ~138Hz cmds at 20Hz through its own
// budget/remainder machinery; the S21 client's prediction cannot replicate it bit-exactly).
// Every clock-DERIVED field (m_flNextAttack, m_meleePressTime, m_raiseFromMeleeEndTime,
// attackStartTime) = timeBase + offset, so its absolute value inherits the sawtooth -- but its
// SEMANTIC content ("how far from the clock is the gate") is domain-free. So compare in
// RELATIVE time: with d = timeBase_pred - timeBase_srv, a derived field matches iff
// (F_pred - timeBase_pred) == (F_srv - timeBase_srv) within a small epsilon (float noise +
// sub-cmd slop; a genuine denial/reset of ANY size >= ~20ms still flags). timeBase itself has
// no residual content once it is the reference -> compare-masked outright. Applied by copying
// the server value over the record slot pre-compare ONLY when the relative times agree (exactly
// the [PRED-UNFED] copy mechanism); sanity gate |d| < 0.25 so a genuine clock reset
// (respawn/teleport) disables the whole rebase for that ack and everything flags natively.
// Relative compare is the domain transform under residual timeBase epoch
// skew; |d|>=0.25 still disables (respawn/teleport).
ConVar sdk_pred_clock_rebase("sdk_pred_clock_rebase", "1", FCVAR_RELEASE,
	"[PRED-CLOCK-REBASE] compare the clock-derived gate times (m_flNextAttack, "
	"m_meleePressTime, m_raiseFromMeleeEndTime, attackStartTime) in RELATIVE time "
	"(value - timeBase) so residual timeBase epoch skew cancels while genuine "
	"corrections of any size still flag; timeBase itself is compare-masked. "
	"1 = on (default), 0 = native absolute-time compare.");

// [PRED-REPAIR]: repairs LIVE members for Shape-A unfed fields so a
// foreign-family rebase (landing origin, etc.) cannot reinstall server edge state
// and re-fire ANIMEVT/ZOOM/punch/step on replay. SHIP DEFAULTS
// sdk_pred_baseline_repair 1 -- live-member repair (unfed mask alone is not enough)
// sdk_pred_unfed_mask 1 -- Shape-A compare neutralize
// sdk_pred_clock_rebase 1 -- relative-time gate compare (residual epoch)
ConVar sdk_pred_baseline_repair("sdk_pred_baseline_repair", "1", FCVAR_RELEASE,
	"[PRED-REPAIR] On Shape-A unfed fields (sdk_pred_unfed_mask), repair the LIVE entity "
	"members (the rebase/replay baseline) with the client's own predicted values instead of "
	"only equalizing the compare buffers. Prevents foreign-family rebases from re-firing "
	"client-owned fire/zoom/punch state. 1 = on (default), 0 = compare-buffer only.");

// [PRED-TOL-GNORM] m_groundNormal is a PER-TICK
// live-trace-derived FIELD_VECTOR with zero tolerance, recomputed unconditionally every
// tick (Player_SetGroundNormal, outside the ground-entity-change gate) --
// so it inherits ROOT-A's ~1-tick pairing residual on EVERY ground contact (gNz-mismatch
// acks show k1/kS ~2x the agreeing acks). A few degrees of slack absorbs the residual;
// a genuine surface-class change (flat vs slope vs stand/slide boundary 0.7) is far
// larger. Implements #40's proposed fix (candidate range 0.05-0.1).
ConVar sdk_pred_tol_groundnormal("sdk_pred_tol_groundnormal", "0.06", FCVAR_RELEASE,
	"[PRED-TOL] fieldTolerance for m_groundNormal (absorbs the ~1-tick pairing "
	"residual on the per-tick ground trace = PREDICTION_LAG_MASTER #40's remaining "
	"jump/land icon driver; real surface-class changes still flag). 0 = native.");

// [PRED-TOL-TRAV] The traversal geometry sextet (m_traversalBegin/Mid/End,
// m_traversalRefPos, m_traversalBlendOutStartOffset, m_traversalForwardDir,
// m_traversalMidFrac) ships at fieldTolerance 0 = EXACT compare, but every one of
// them is a snapshot of the player's position and eye direction taken once at
// traversal start -- i.e. a derivative of m_localOrigin, which is itself tolerated
// at 0.60 because the two binaries cannot agree on it to the float. So an origin
// difference the engine already forgives is amplified into a hard rebase through
// the snapshot, and it lands exactly on the ack that ends a zipline ride.
// Measured over a zip/melee run: p90 residue 0.001-0.008u on 250+ rows (at or
// below the log's 3-decimal print resolution), with a real 3.4-5.5u tail where the
// two engines genuinely anchored the traversal at different points. 0.60 keeps
// that tail flagging while retiring the float-drift bulk.
static ConVar sdk_pred_tol_traversal("sdk_pred_tol_traversal", "0.60", FCVAR_RELEASE,
	"[PRED-TOL] fieldTolerance for the traversal geometry positions (m_traversalBegin/"
	"Mid/End, m_traversalRefPos, m_traversalBlendOutStartOffset) -- once-written "
	"snapshots of m_localOrigin, matched to its tolerance; multi-unit anchor "
	"divergences still flag. 0 = native (exact).");
static ConVar sdk_pred_tol_traversal_dir("sdk_pred_tol_traversal_dir", "0.05", FCVAR_RELEASE,
	"[PRED-TOL] fieldTolerance for m_traversalForwardDir (unit vector; measured drift "
	"peaks at 0.029 = under 2 degrees). 0 = native (exact).");
static ConVar sdk_pred_tol_traversal_frac("sdk_pred_tol_traversal_frac", "0.01", FCVAR_RELEASE,
	"[PRED-TOL] fieldTolerance for m_traversalMidFrac (the engine already gives "
	"m_traversalProgress this value; measured drift peaks at 0.0027, median 1 ULP). "
	"0 = native (exact).");

// [ZIP-ALPHA-AUTH] The zipline mover integrates a normalized rail parameter and
// DERIVES world position from it. The S21 pred map ships that parameter with
// FTYPEDESC_SKIP, so the native compare never consults it, while the derived
// m_localOrigin IS compared at sdk_pred_tol_origin -- a tolerance sized for
// cross-binary float drift. Speed and acceleration never cross the wire at all
// (each engine reads them from its own m_classSettings and integrates locally),
// so any input difference accrues silently for the whole ride and is paid as one
// origin rebase on the ack that ends it. That error grows with rail length, so no
// origin tolerance can ever be the right size for it.
//
// Reconciling the parameter instead bounds it: a per-ack alpha correction the
// client integrates forward from, rather than an unbounded position drift. The
// alpha->position map itself is INLINED on the dedi -- nothing is hooked
// here; each engine's own map re-derives origin from the corrected
// parameter during the ordinary replay.
static ConVar bridge_zip_alpha_authority("bridge_zip_alpha_authority", "0", FCVAR_RELEASE,
	"[ZIP-ALPHA-AUTH] Clear FTYPEDESC_SKIP on the zipline rail parameter "
	"(m_slidingZiplineAlpha / m_mountingZiplineAlpha) so the native error-check "
	"reconciles the rail STATE instead of only its derived position. 0=off "
	"(default, native), 1=on. Measure with bridge_zip_alpha_probe first.");
// Measured agreement on the rail parameter is 2e-06 (p50) / 5e-06 (p90), so this
// sits ~100x above the noise floor while still catching a divergence long before
// it becomes a multi-unit position split. alpha is normalized 0..1, so 5e-4 is
// half a per-mille of rail length.
static ConVar sdk_pred_tol_zipalpha("sdk_pred_tol_zipalpha", "0.0005", FCVAR_RELEASE,
	"[PRED-TOL] fieldTolerance for the zipline rail parameter once "
	"bridge_zip_alpha_authority has un-skipped it. 0 = exact.");

// [FORCED0-NEUT] Native rule (S21 / PART 15a): if (delta==0 && predictedFieldsChanged)
// force m_bPreviousAckHadErrors=1 BEFORE any field compare -- free full restore+replay.
// Bridge induces chronic forced0 (~3/s) via double prediction-snapshot transitions that
// consume the cmdRun delta (PART 15e). When NO entity field compare actually failed, the
// forced bit is pure noise that re-fires weapons on replay. Neutralize: after orig, rewrite
// hadErrors from the entity-error tally only. Real field errors still set the bit (entity
// loop ORs 212=1). SHIP DEFAULT 1.
ConVar sdk_pred_forced0_neut("sdk_pred_forced0_neut", "1", FCVAR_RELEASE,
	"[FORCED0-NEUT] When commands_acknowledged delta==0 forces had-errors with no entity "
	"field mispredict, clear m_bPreviousAckHadErrors so the free rebase is skipped. "
	"1=on (ship), 0=native (forced0 free rebases stand).");

// m_nResetEventsParity is a counter CBaseAnimating bumps inside ResetSequence.
// The client only predicts it if its animation code calls ResetSequence at the
// same moments the dedicated server does. Its one consumer ("changed since last
// frame -> reset the event indices") reads the live member, which the wire
// still feeds. Server-authored anim-event counters; skip error-check
// (FTYPEDESC_SKIP / 0x400) so client anim timing does not force rebases.
ConVar sdk_pred_animevt_noerrcheck("sdk_pred_animevt_noerrcheck", "1", FCVAR_RELEASE,
	"[PRED-AUTH] stop error-checking the server-authored animation-event parity "
	"counters (sets the engine's own FTYPEDESC_SKIP on their datamap entries). "
	"Replication is unaffected. 1=on (default), 0=off to A/B the rebase rate.");

// FTYPEDESC_SKIP does NOT suppress restore: S21 RestoreData drives the same
// flat transfer engine as SaveData with the fast-path selector set, a packed
// memmove loop that never reads typedescription flags. NO_ERRORCHECK only
// suppresses the compare; the field is still installed by every rebase --
// safe on a field that must still be replicated. Until the wire fix, srv was
// 0 on every sample so each rebase installed m_duckState=1 beside rem=0 and
// the replay completed the duck instantly (+16.500 hull step, one per ack).
ConVar sdk_pred_skip_duckrem("sdk_pred_skip_duckrem", "1", FCVAR_RELEASE,
	"[PRED-AUTH] stop error-checking m_duckTransitionRemainderMsec (compare only; "
	"restore still installs the field). 1=on (default), 0=off to A/B the rebase rate.");

// [DUCK-REM-WIRE] The wire fix the row above was waiting on. The dedi does send
// the countdown -- as m_nDuckTransitionTimeMsecs in DT_Local, while S21 keeps the
// same quantity in DT_CurrentData_LocalPlayer. The decoder pairs by name within a
// table, so it never bound; and the two tables sit at different bases inside
// CPlayer (23184 vs 27464), so the prop cannot be re-homed either. The decode
// layer peeks the unpaired value off the wire and hands it here instead.
static ConVar bridge_duck_remainder_wire("bridge_duck_remainder_wire", "1", FCVAR_RELEASE,
	"[DUCK-REM-WIRE] Install the dedi's duck-transition countdown, which arrives "
	"under the S3 name in a table the S21 client does not pair. 1=on (default), "
	"0=leave the remainder client-predicted.");
static ConVar bridge_duck_remainder_diag("bridge_duck_remainder_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[DUCK-REM-WIRE] Log the live/wire pair on install. The two builds name this "
	"field differently, so this is what confirms they mean the same thing. "
	"0=off (default).");

// m_updraftStage has two authors by design: the S21 client flips FALLING->LIFTING
// inside its own predicted air move, off its predicted origin, while the dedi flips
// off the origin it has simulated. Both run identical math on inputs that are one
// prediction depth apart, so the flip lands a tick either side and the field is
// compared exactly. Replication has to stay -- the client has no path that resets
// the stage on its own -- so mask the compare, not the value.
ConVar sdk_pred_updraft_stage_noerrcheck("sdk_pred_updraft_stage_noerrcheck", "1", FCVAR_RELEASE,
	"[PRED-AUTH] stop error-checking m_updraftStage (compare only; the received "
	"value is still installed by every rebase). 1=on (default), 0=off to A/B.");

ConVar sdk_pred_jetdrive_errcheck("sdk_pred_jetdrive_errcheck", "1", FCVAR_RELEASE,
	"[PRED-AUTH] keep the 14 m_jetDrive* fields in the native compare. "
	"Clears FTYPEDESC_SKIP if this build shipped them exempt. 1=on.");

// [PRED-FIELDS] one-shot per-dmap dump of the full flattened networked predicted field
// set -- the DENOMINATOR the parity program never had written down. Emitted once per
// dmap at first resolve, so it costs ~4 bursts a session and answers every "what type /
// what flags / is it even error-checked" question offline instead of per live run.
ConVar sdk_pred_field_dump("sdk_pred_field_dump", "0", FCVAR_DEVELOPMENTONLY,
	"[PRED-FIELDS] dump every flattened networked predicted field (name, type, flags, "
	"tolerance, offsets) once per datamap at first resolve. 1=on (default), 0=off.");

// The viewkick pattern row index is floor(m_kickPatternScaleBase /
// viewkick_scale_valuePerShot), so a divergence in these accumulators selects a
// DIFFERENT pattern row. Rows carry only yaw/pitch/randomScale*, never roll, which
// is why a row split shows up as pitch+yaw magnitude error with roll bit-identical.
ConVar bridge_kick_row_tap("bridge_kick_row_tap", "0", FCVAR_DEVELOPMENTONLY,
	"[KICK-ROW] print the srv/pred pair whenever a viewkick accumulator or one of its "
	"decay stamps diverges, so the selected pattern row can be derived per shot. "
	"0=off (default), 1=on.");

// [TIME-DOMAIN] D1 -- convert server-domain absolute TIME props into the client's
// timeBase domain on receive (live members), before originalData is packed and
// before the native compare/restore. Weapon DPT_Time props ship raw under
// bridge_time_encode FULL form; without this convert a restore installs a gate
// stamp in the client's past and FirstTimePredicted re-fires (double-fire).
// Selector: FIELD_TIME + the known absolute gate names (weapon times may land as
// FIELD_FLOAT in the pred map). Sanity |delta| < 0.25 (same as clock_rebase).
ConVar bridge_time_domain_convert("bridge_time_domain_convert", "1", FCVAR_RELEASE,
	"[TIME-DOMAIN] Convert networked absolute time stamps from the server's timeBase "
	"domain into the client's on receive (player + weapons + viewmodel). Fixes the "
	"fire-gate early-open / phantom bolt class. 1=on (default), 0=ship raw (legacy).");

// [INF-AMMO-CLIENT] GetActiveAmmoSource: m_infiniteAmmoState != 0 => AMMOSOURCE_INFINITE.
// Clip still cycles client-side; reserve is infinite. On the bridge the S3 dedi often
// holds stale wire clip/stockpile that PNR would stomp over the client's predicted
// values. When state is live on the weapon, client owns those two fields for the
// compare/rebase (same mechanism as Shape-A unfed, scoped to infinite weapons only).
// Does NOT fight the SERVER_CONTENT tombstone: that forbids a permanent table entry;
// this is a per-entity runtime predicate after the table pass.
// Offsets verified S21 DT_WeaponX_LocalWeaponData ClientClassInit
// m_ammoInClip 5520=0x1590, m_ammoInStockpile 5524=0x1594, m_infiniteAmmoState 5528=0x1598.
ConVar bridge_inf_ammo_client("bridge_inf_ammo_client", "0", FCVAR_RELEASE,
	"[INF-AMMO-CLIENT] PARKED. When m_infiniteAmmoState!=0, mask m_ammoInClip/"
	"m_ammoInStockpile from PNR stomp. 0=off (default until reload baseline proven).");
ConVar bridge_inf_ammo_client_diag("bridge_inf_ammo_client_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[INF-AMMO-CLIENT] First-N probe + clip-mask logs. 1=on, 0=quiet.");

// [MODBITS] Attachment HUD probe. Live-member and compare-buffer offsets come from
// this client's own [PRED-FIELDS] weapon dump: m_modBitfieldFromPlayer 5908/892,
// m_modBitfieldInternal 5912/896, m_modBitfieldCurrent 5916/900 (off0 = live member,
// off1 = index into the predicted/server serialized records).
ConVar bridge_weapon_modbits_probe("bridge_weapon_modbits_probe", "0", FCVAR_DEVELOPMENTONLY,
	"[MODBITS] Log the weapon mod bitfields (live / predicted / server) whenever they "
	"change, to separate 'the attachment never replicated' from 'prediction reverted it'. "
	"0 = off (default).");

// Same trick for the anim-event dedup array (see its table rows below).
static const char* const kAnimEvtTimesName = "m_predictedAnimEventTimes";

static ConVar sdk_pred_animevt_diag("sdk_pred_animevt_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[PRED-ANIMEVT] Report every suppressed resurrection of a consumed predicted-anim-event "
	"slot (live 0 vs a pre-fire value in the acked command's record). Each one is a melee "
	"swing / attack effect that would have fired a second time. 1=on (default).");

// [REGEN-STAMP] Every entry into the dedi's ammo-regen FSM sits behind
// CWeaponX::IsPredicted, which reads cl_predict and is permanently false on a
// dedicated server -- so the dedi can never author this stamp and has nothing
// true to say about it. The dedi-side sentinel (bridge_regen_stamp_wire) keeps
// the wire value harmless but does NOT make it absent: a rebase still installs
// the wire value into the live member, so an ability that IS regenerating gets
// its live stamp overwritten with the inactive sentinel on every ack and its
// regen FSM restarts. Ownership is what stops that; the sentinel alone is not.
static ConVar bridge_regen_stamp_client("bridge_regen_stamp_client", "1", FCVAR_RELEASE,
	"[REGEN-STAMP] Client owns m_lastRegenTime. The dedi cannot author it (its "
	"regen FSM is gated on cl_predict), so an unowned field lets the wire "
	"sentinel stomp a live cooldown stamp every ack. 1 = mask + repair "
	"(default), 0 = native compare.");

// The six sprint rows are masked on a measurement, not a mechanism: the dedi
// networks all six and its server half writes the members. Dial 0 to measure
// the raw wire value once bridge_bind_census has reported the pairing.
static ConVar sdk_pred_sprint_mask("sdk_pred_sprint_mask", "1", FCVAR_RELEASE,
	"[PRED-AUTH] keep the CLIENT_TIMING mask on the six sprint fields "
	"(m_fIsSprinting, m_bIsStickySprinting, m_fStickySprintMinTime and the "
	"three sprint stamps). 1=masked (default), 0=native compare to measure "
	"the wire value.");

// [WEAP-CLOCK] Weapon dmaps have no m_currentFramePlayer.timeBase, so the player-side
// DOMAIN_TRANSPLANT path never armed on them. Relative compare borrows the local
// player's timeBase pair (see PredAuth_Apply).
static ConVar sdk_pred_weapon_clock("sdk_pred_weapon_clock", "1", FCVAR_RELEASE,
	"[WEAP-CLOCK] Relative-time compare for weapon absolute fire/kick/idle stamps "
	"(borrows player timeBase). 1=on (default), 0=native absolute compare on weapons.");

// Residual after convert+relative often lands near multi-tick under 90ms RTT.
static ConVar sdk_pred_weapon_time_eps("sdk_pred_weapon_time_eps", "0.35", FCVAR_RELEASE,
	"[WEAP-CLOCK] Relative-time match epsilon (seconds) for weapon DOMAIN_TRANSPLANT "
	"stamps. Default 0.35 (~7 ticks at 20Hz / RTT-class lag).");

// Shape-B floor when relative match fails.
static ConVar sdk_pred_tol_weapon_time("sdk_pred_tol_weapon_time", "0.50", FCVAR_RELEASE,
	"[PRED-TOL] fieldTolerance for weapon fire-gate TIME stamps. Default 0.50. "
	"0 = native half-tick only.");

// Either pole of m_attackTimeThisFrame can be unset under RTT (pred=0 srv=live OR
// srv=0 pred=live). Compare-only both ways; never live-repair a hard 0 onto a live clock.
static ConVar sdk_pred_attacktime_cmp_only("sdk_pred_attacktime_cmp_only", "1", FCVAR_RELEASE,
	"[WEAP-CLOCK] Bipolar unset equalize for m_attackTimeThisFrame (either side <=1 "
	"while the other is live). Compare-only, no live repair. 1=on (default).");

// idealSequence residual is authorship/timing on a 1:1 seqtable (parity proven), not
// a namespace translator case. CLIENT_TIMING+repair was a regression (latched stale
// deploy). NO_ERRORCHECK only drops the field from had-errors; wire still applies.
static ConVar sdk_pred_ideal_seq_noerr("sdk_pred_ideal_seq_noerr", "1", FCVAR_RELEASE,
	"[PRED-AUTH] FTYPEDESC_SKIP compare on m_idealSequence (and idealActivity companion) "
	"so one-tick authorship lag cannot force full weapon restore+replay. Wire still "
	"applies. 1=on (default), 0=native exact compare.");

// One-round / one-counter lag under RTT is not a real inventory correction --
// equalize the compare buffers only when |pred-srv| <= N.
static ConVar sdk_pred_weapon_fire_lag("sdk_pred_weapon_fire_lag", "1", FCVAR_RELEASE,
	"[WEAP-FIRE-LAG] Neutralize compare on weapon fire counters when |pred-srv| <= "
	"sdk_pred_weapon_fire_lag_n (clip/shotCount/semiAuto/spread index). Does not "
	"repair live members. 1=on (default).");
static ConVar sdk_pred_weapon_fire_lag_n("sdk_pred_weapon_fire_lag_n", "2", FCVAR_RELEASE,
	"[WEAP-FIRE-LAG] Max integer |pred-srv| still treated as lag (not a real correction). "
	"Default 2.");

// H7: punch mask intentionally OFF (aim direction). Residual still forces player
// restore+replay under fire -- Shape-B fieldTolerance, not CLIENT_TIMING re-mask.
static ConVar sdk_pred_tol_punch("sdk_pred_tol_punch", "1.50", FCVAR_RELEASE,
	"[PRED-TOL] fieldTolerance for m_vecPunchWeapon_* / Base_* when the legacy punch "
	"mask is retired. Default 1.50 (deg-scale residual). 0 = native exact.");
static ConVar sdk_pred_punch_tol_enable("sdk_pred_punch_tol_enable", "1", FCVAR_RELEASE,
	"[PRED-TOL] Apply sdk_pred_tol_punch to punch angle fields while "
	"bridge_punch_legacy_mask is 0. 1=on (default).");

// Kick-row float bases advance one valuePerShot per fire; under RTT the predicted
// slot trails by 1-2 steps and exact-compare forces weapon rebases that inject the
// wrong pattern row into punch. Compare-only lag equalize, mirror WEAP-FIRE-LAG --
// no live repair, no free CLIENT_TIMING of aim pattern.
// Live 42ab N=2 A/B: pure lag2 staircase collapsed (eq d1/d2 dominate); hard |d|>=3
// and reverse still residual. Default N=2 matches FIRE-LAG depth. Hipfire unit is
// ~2.4 (not 1.0) -- equalize uses N*unit so one hipfire step is lag, not free mask.
static ConVar sdk_pred_kick_row_lag("sdk_pred_kick_row_lag", "1", FCVAR_RELEASE,
	"[KICK-ROW-LAG] Compare-only equalize kick pattern/scale bases + hipfire when "
	"|pred-srv| <= N*unit. 1=on, 0=off.");
static ConVar sdk_pred_kick_row_lag_n("sdk_pred_kick_row_lag_n", "2", FCVAR_RELEASE,
	"[KICK-ROW-LAG] Max fire-step lag (bases unit=1, hipfire unit~2.5). Default 2; "
	"|d| > N*unit stays exact (hard rebuild / multi-shot miss).");

// Derived heat is recomputed every pred frame from client-owned lastPrimary while
// the server Derive uses the real fire stamp -- exact-compare is dual-authority thrash
// on has_heat_decay weapons. OnLastFire stays FED seed.
static ConVar bridge_heat_client_owned("bridge_heat_client_owned", "1", FCVAR_RELEASE,
	"[HEAT] Client owns m_heatValue + m_fullyHeated (+ optional scriptFloat0). "
	"1=mask+repair (default), 0=native compare.");

// Integrator anglevel diverges multi-deg when kick-row lags; wire still applies.
static ConVar sdk_pred_punch_vel_noerr("sdk_pred_punch_vel_noerr", "1", FCVAR_RELEASE,
	"[PRED-AUTH] FTYPEDESC_SKIP compare on m_vecPunch*AngleVel* while legacy punch "
	"mask is off. 1=on (default).");

static ConVar sdk_pred_soundcode_noerr("sdk_pred_soundcode_noerr", "1", FCVAR_RELEASE,
	"[SOUNDCODE] Stop error-checking m_flSoundCodeControllerValue / "
	"m_bIsSoundCodeControllerValueSet. The client has no simulation that advances "
	"them, so every server-side change forces a rebase. 1=on, 0=exact compare.");

static ConVar bridge_wallclimb_setup_client("bridge_wallclimb_setup_client", "1", FCVAR_RELEASE,
	"[WALLCLIMB] Client owns m_wallClimbSetUp (mask + repair). The dedi's copy is a "
	"lagged echo of a gate evaluated differently across builds. 1=on (default), "
	"0=native compare.");

static ConVar bridge_viewoffset_ent_noerr("bridge_viewoffset_ent_noerr", "1", FCVAR_RELEASE,
	"[VIEWOFFSET-ENT] Stop error-checking viewOffsetEntityHandle (FTYPEDESC_SKIP "
	"compare only; wire still applies). Dedi and client index entities differently, "
	"so resolved handles never match. 1=on (default), 0=native compare.");

//-----------------------------------------------------------------------------
// PredNative_TickInterval -- see pred_authority.h. Pattern lands on the site that
// feeds FIELD_TIME tolerance in the engine error check:
// mov rax, gpGlobals_Client / movss xmm0, [rax+44h] / mulss xmm0, 0.5
// so the interval we read is the interval the compare uses.
// gpGlobals_Client is a POINTER, hence the extra deref; +0x44 is
// CGlobalVarsBase::interval_per_tick (tickcount sits at +0x40).
//-----------------------------------------------------------------------------
static uintptr_t PredNative_Globals(void)
{
	static void** s_ppGlobals = nullptr;
	static bool s_tried = false;
	if (!s_tried)
	{
		s_tried = true;
		s_ppGlobals = Module_FindPattern(g_GameDll,
			"48 8B 05 ?? ?? ?? ?? F3 0F 10 40 44 F3 0F 59 05 ?? ?? ?? ?? 0F 28 C8")
			.ResolveRelativeAddress(3, 7)
			.RCast<void**>();
		if (!s_ppGlobals)
			Warning(eDLL_T::CLIENT,
				"[PRED-AUTH] gpGlobals_Client pattern unresolved -- FIELD_TIME fields "
				"compare on fieldTolerance only, so time-family counts will read HIGH\n");
	}
	// The pattern resolves the address OF the global, which HOLDS the pointer --
	// deref it here, late, because the engine leaves it null until it publishes
	// globals (first resolve happens before that).
	if (!s_ppGlobals || !*s_ppGlobals)
		return 0;
	return reinterpret_cast<uintptr_t>(*s_ppGlobals);
}

float PredNative_TickInterval(void)
{
	const uintptr_t globals = PredNative_Globals();
	if (!globals)
		return 0.f;
	const float f = *reinterpret_cast<const float*>(globals + 0x44);
	return (f > 0.f && f <= 1.f) ? f : 0.f;
}

float PredNative_CurTime(void)
{
	const uintptr_t globals = PredNative_Globals();
	if (!globals)
		return 0.f;
	return *reinterpret_cast<const float*>(globals + 0x10);   // CGlobalVarsBase::curtime
}

float PredNative_LatestPredictedTime(void)
{
	const uintptr_t globals = PredNative_Globals();
	if (!globals)
		return 0.f;
	return *reinterpret_cast<const float*>(globals + 0x28);   // the clock weapon timers compare against
}

float PredNative_Tolerance(int nType, float flFieldTol)
{
	if (nType != FIELD_TIME)
		return flFieldTol;
	const float flHalfTick = PredNative_TickInterval() * 0.5f;
	return (flHalfTick > flFieldTol) ? flHalfTick : flFieldTol;
}

//-----------------------------------------------------------------------------
// PredNative_EntInfoArray -- &g_clientEntityList->m_EntPtrArray[0]. Resolved off
// the `lea r13` inside the engine error check, which hoists the array base for
// local-player grapple and every EHANDLE field -- same array the native compare
// uses. NOT from g_pClientEntityList (factory interface with truncated vtable);
// this is a raw read that must not depend on a virtual.
//-----------------------------------------------------------------------------
static const uint8_t* PredNative_EntInfoArray(void)
{
	static const uint8_t* s_pArray = nullptr;
	static bool s_tried = false;
	if (!s_tried)
	{
		s_tried = true;
		s_pArray = Module_FindPattern(g_GameDll,
			"4C 8B 79 10 4C 8D 2D ?? ?? ?? ?? 4C 8B 61 18 48 8B D9 49 63 C0 4C 8D 04 C0")
			.ResolveRelativeAddress(7, 11)
			.RCast<const uint8_t*>();
		if (!s_pArray)
			Warning(eDLL_T::CLIENT,
				"[PRED-AUTH] entity-info array pattern unresolved -- FIELD_EHANDLE "
				"compares RAW, so handle families will read HIGH\n");
	}
	return s_pArray;
}

bool PredNative_HasEntInfoArray(void)
{
	return PredNative_EntInfoArray() != nullptr;
}

const void* PredNative_ResolveEHandle(unsigned int nHandle)
{
	const uint8_t* const pArray = PredNative_EntInfoArray();
	if (!pArray || nHandle == INVALID_EHANDLE_INDEX)
		return nullptr;

	// The native masks to 16 bits and indexes unchecked -- the array is
	// NUM_ENT_ENTRIES slots, so a masked index can never leave it.
	const uint8_t* const pInfo = pArray
		+ (size_t)(nHandle & ENT_ENTRY_MASK) * ENTINFO_STRIDE;
	if (*reinterpret_cast<const int*>(pInfo + ENTINFO_SERIAL)
		!= static_cast<int>(nHandle >> NUM_SERIAL_NUM_SHIFT_BITS))
		return nullptr;
	return *reinterpret_cast<void* const*>(pInfo + ENTINFO_ENTITY);
}

void* PredNative_EntityFromSlot(int slot)
{
	const uint8_t* const pArray = PredNative_EntInfoArray();
	if (!pArray || slot < 0)
		return nullptr;
	const unsigned idx = static_cast<unsigned>(slot) & ENT_ENTRY_MASK;
	return *reinterpret_cast<void* const*>(
		pArray + static_cast<size_t>(idx) * ENTINFO_STRIDE + ENTINFO_ENTITY);
}

PredNativeCmp_t PredNative_Compare(int nType, const uint8_t* pPred, const uint8_t* pSrv,
	int nCount, float flFieldTol)
{
	const float tol = PredNative_Tolerance(nType, flFieldTol);
	int nFloats = 0, nBytes = 0;

	switch (nType)
	{
	case FIELD_FLOAT: case FIELD_TIME:   nFloats = nCount;     break;
	case FIELD_VECTOR:                   nFloats = nCount * 3; break;
	case FIELD_QUATERNION:               nFloats = nCount * 4; break;
	case FIELD_STRING:
		// The engine strcmp's the two inline buffers; bound it so a malformed
		// descriptor cannot walk a diagnostic off the end of the snapshot.
		return strncmp(reinterpret_cast<const char*>(pPred),
			reinterpret_cast<const char*>(pSrv), 256) ? PRED_CMP_ERROR : PRED_CMP_SAME;
	case FIELD_EHANDLE:
	{
		// The engine RESOLVES both handles through the entity list and compares the
		// resulting entity pointers, so handles that differ raw but name the same
		// entity -- including two distinct stale serials that both resolve to null --
		// are EQUAL to it and force no rebase. Fail back to the raw compare when the
		// array is unresolved: that under-counts, where resolving everything to null
		// would silently forgive every real handle divergence.
		if (!PredNative_HasEntInfoArray())
		{
			nBytes = 4 * nCount;
			break;
		}

		bool bForgiven = false;
		for (int e = 0; e < nCount; ++e)
		{
			const uint32_t hPred = reinterpret_cast<const uint32_t*>(pPred)[e];
			const uint32_t hSrv  = reinterpret_cast<const uint32_t*>(pSrv)[e];
			if (hPred == hSrv)
				continue;
			if (PredNative_ResolveEHandle(hPred) != PredNative_ResolveEHandle(hSrv))
				return PRED_CMP_ERROR;
			bForgiven = true;
		}
		return bForgiven ? PRED_CMP_BELOW_TOL : PRED_CMP_SAME;
	}
	case FIELD_INTEGER: case FIELD_COLOR32:                     nBytes = 4 * nCount; break;
	case FIELD_SHORT:                                           nBytes = 2 * nCount; break;
	case FIELD_BOOLEAN: case FIELD_CHARACTER:                   nBytes = 1 * nCount; break;
	default:
		return PRED_CMP_SAME;   // 11/12/14/15 and unknown: the engine never compares them
	}

	if (nBytes)
		return memcmp(pPred, pSrv, static_cast<size_t>(nBytes)) ? PRED_CMP_ERROR : PRED_CMP_SAME;

	// tol <= 0 is an EXACT compare in the engine, not a zero-width band.
	bool bAny = false;
	for (int e = 0; e < nFloats; ++e)
	{
		const float d = reinterpret_cast<const float*>(pPred)[e]
			- reinterpret_cast<const float*>(pSrv)[e];
		if (d == 0.f)
			continue;
		bAny = true;
		if (!(tol > 0.f) || fabsf(d) > tol)
			return PRED_CMP_ERROR;
	}
	return bAny ? PRED_CMP_BELOW_TOL : PRED_CMP_SAME;
}

//-----------------------------------------------------------------------------
// [PRED-UNFED] S21 SaveData ((this, name, cmd, slot); slot -1 =
// originalData). Shared by the mask (below) and the verdict probe: refreshing
// originalData from the LIVE members pre-orig is idempotent (the orig's first call
// repeats it) and is the only way to see/patch what the native compare actually reads.
//-----------------------------------------------------------------------------
PFN_PredSaveData PredAuth_SaveData(void)
{
	static PFN_PredSaveData s_p = nullptr;
	static bool s_tried = false;
	if (!s_tried)
	{
		s_tried = true;
		// S21 CPrediction::PostNetworkDataReceived / SaveData path =.

		// Fail-closed: on miss leave s_p null so all PredAuth_SaveData callers skip.
		s_p = Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? "
			"41 54 41 56 41 57 48 81 EC ?? ?? ?? ?? 0F BF 91")
			.RCast<PFN_PredSaveData>();
		if (!s_p)
		{
			Warning(eDLL_T::CLIENT,
				"[PRED-AUTH] SaveData pattern unresolved --"
				"PredAuth mutations that need SaveData are skipped\n");
		}
		else
		{
			Msg(eDLL_T::CLIENT, "[PRED-AUTH] SaveData resolved %p\n", (void*)s_p);
		}
	}
	return s_p;
}

// [PRED-REPAIR] read flatOffset[0] (live-member offset) with sanity bounds; -1 = do not
// member-repair this slot (fall back to legacy compare-only masking for it).
static inline int PredAuth_MemberOff(uintptr_t td)
{
	const int v = *reinterpret_cast<const int*>(td + TD_FLATOFFSET0);
	return (v > 0 && v < 0x40000) ? v : -1; // sane entity-relative range
}

//=============================================================================
// THE TABLE (PRED_AUTHORITY_DESIGN.md PART 3 + PART 4)
//=============================================================================
static const PredAuthEntry_t s_authTable[] =
{
	//-------------------------------------------------------------------
	// CLIENT_TIMING, scope ANY (migrated verbatim from s_unfedNames)
	//-------------------------------------------------------------------
	{ "m_prevJumpPressTime", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"jump press timestamp: S21-only, unfed (srv==0 1353/1353 measured); client-derived from its own usercmds." },
	{ "m_jumpPressTime", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"jump press timestamp pair with m_prevJumpPressTime; same unfed contract." },
	{ "m_lastSprintPressTime", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"sprint press timestamp: S21-only, unfed (srv==0 460/460 measured)." },
	{ "m_sprintStartedTime", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, &sdk_pred_sprint_mask, 0,
		"dedi networks it (DT_LocalPlayerExclusive, server-half sprint FSM authors the member); masked while the wire value reads 0 -- run bridge_bind_census to get the pairing verdict." },
	{ "m_sprintEndedTime", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, &sdk_pred_sprint_mask, 0,
		"dedi networks it (DT_LocalPlayerExclusive, server-half sprint FSM authors the member); masked while the wire value reads 0 -- run bridge_bind_census to get the pairing verdict." },
	{ "m_stickySprintStartTime", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, &sdk_pred_sprint_mask, 0,
		"dedi networks it (DT_LocalPlayerExclusive, server-half sticky-sprint FSM authors the member); masked while the wire value reads 0 -- run bridge_bind_census to get the pairing verdict." },
	{ "m_touchedGroundSinceLastGlide", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"S21-only glide bookkeeping; server value is unfed." },
	{ "m_wallrunLatestFloorHeight", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"wallrun floor-height bookkeeping: S21-only, unfed." },
	{ "m_wallClimbSetUp", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, &bridge_wallclimb_setup_client, 0,
		"client-predicted wall-climb latch; dedi's copy is a lagged echo of a gate evaluated differently, so the client owns it." },
	{ "m_ziplineReattachCooldownTime", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"zipline reattach cooldown: S21-only, unfed (srv==0 6/6 measured)." },
	{ "m_canStand", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"duck-mirror bookkeeping: cross-table recv move, structurally unfeedable; working timer kept correct by DUCK-RESTORE." },
	// [PUNCH #21] land-punch RNG jitter cannot pair cross-build. These are networked
	// DT_CurrentData_LocalPlayer leaves; the local-exclusive table's only server-side
	// consumer is this errorcheck, so client-authoritative is correct-by-design.
	{ "m_currentFrameLocalPlayer.m_vecPunchBase_Angle", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"punch RNG jitter cannot pair cross-build; networked DT_CurrentData_LocalPlayer leaf whose only server-side consumer is this errorcheck." },
	{ "m_currentFrameLocalPlayer.m_vecPunchBase_AngleVel", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"punch RNG jitter, same contract as m_vecPunchBase_Angle." },
	{ "m_currentFrameLocalPlayer.m_vecPunchWeapon_Angle", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"punch RNG jitter, same contract as m_vecPunchBase_Angle." },
	{ "m_currentFrameLocalPlayer.m_vecPunchWeapon_AngleVel.x", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"punch RNG jitter, same contract as m_vecPunchBase_Angle." },
	{ "m_currentFrameLocalPlayer.m_vecPunchWeapon_AngleVel.y", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"punch RNG jitter, same contract as m_vecPunchBase_Angle." },
	{ "m_currentFrameLocalPlayer.m_vecPunchWeapon_AngleVel.z", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"punch RNG jitter, same contract as m_vecPunchBase_Angle." },
	// [STEP #35] pure client-cosmetic view easing vector, decays on REAL render frametime
	// (not per-command tick delta) -- a replay cannot reproduce the server's curve even in
	// principle. Networked DT_CurrentData_LocalPlayer leaf, same ownership contract.
	{ "m_currentFrameLocalPlayer.m_stepSmoothingOffset", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"decays on real render frametime, not tick delta; a replay cannot reproduce the curve. Networked local-exclusive leaf; client owns the ease." },
	// [ANIMEVT-CONSUME] COMPARE-ONLY, never repaired. The dedup is
	// consume-by-zeroing: ProcessPredictedAnimEvents fires a slot whose time is
	// non-zero and <= the current weapon time, then writes that slot to 0. It runs
	// on the FRAME path (ItemPreFrame), not the command path, so the acked command's
	// record is always older than the fire. Repairing the live member from it puts
	// the pre-fire time back and the next frame fires the same event again -- one
	// frame later, which is the +14 ms double melee registration. Masking the
	// compare is still right: S3 seq/model ids are namespace-divergent
	// (1091-vs-1095 class), so the wire copy can never equal the prediction.
	{ kAnimEvtTimesName, PredAuthClass_t::CLIENT_TIMING_COMPARE_ONLY, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"client's own fire-effect replay-dedup state; consumed by zeroing on the frame path, so a command-slot snapshot can only resurrect it." },
	{ "m_predictedAnimEventIndices", PredAuthClass_t::CLIENT_TIMING_COMPARE_ONLY, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"predicted-anim-event dedup state, same contract as m_predictedAnimEventTimes." },
	{ "m_predictedAnimEventCount", PredAuthClass_t::CLIENT_TIMING_COMPARE_ONLY, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"predicted-anim-event dedup state, same contract as m_predictedAnimEventTimes." },
	{ "m_predictedAnimEventTarget", PredAuthClass_t::CLIENT_TIMING_COMPARE_ONLY, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"predicted-anim-event dedup state, same contract as m_predictedAnimEventTimes." },
	{ "m_predictedAnimEventSequence", PredAuthClass_t::CLIENT_TIMING_COMPARE_ONLY, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"predicted-anim-event dedup state, same contract as m_predictedAnimEventTimes." },
	{ "m_predictedAnimEventModel", PredAuthClass_t::CLIENT_TIMING_COMPARE_ONLY, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"predicted-anim-event dedup state, same contract as m_predictedAnimEventTimes." },
	// [ZOOM RECLASS ] The 07-15 "client-authoritative zoom" masks were the wrong
	// class (S3-parity bar: S3 clients reconciled zoom natively; only the clock EPOCH is
	// bridge-structural). m_bZooming is now FED (deleted -- native compare/adopt; the S3
	// dedi delivers it, and the epoch skew lives in the TIME stamps, not the bool);
	// m_zoomBaseTime/m_zoomFullStartTime moved to DOMAIN_TRANSPLANT below (the
	// m_flNextAttack treatment: relative-time compare cancels the epoch, genuine
	// denials still flag). Only the in-flight press frac stays masked -- it snapshots a
	// sub-tick interpolation state whose exact value cannot pair across the cmd-batch
	// boundary; its divergence is derivative of the (now transplanted) stamps.
	{ "m_zoomBaseFrac", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"ADS in-flight frac snapshotted at press: sub-tick interpolation state, cannot pair across the cmd-batch boundary; the zoom stamps themselves are transplanted, not masked." },
	// [PUNCH/VIEW PART 2] remaining networked DT_CurrentData_LocalPlayer leaves.
	// Ownership claim: the local-exclusive table's only server-side consumer is this
	// errorcheck -- not an absence claim (the props are networked).
	{ "m_currentFrameLocalPlayer.m_localGravityRotation", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"networked DT_CurrentData_LocalPlayer leaf; server copy's only consumer is this errorcheck." },
	{ "m_currentFrameLocalPlayer.m_viewConeAngleMin", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"networked DT_CurrentData_LocalPlayer leaf; server copy's only consumer is this errorcheck." },
	{ "m_currentFrameLocalPlayer.m_viewConeAngleMax", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"networked DT_CurrentData_LocalPlayer leaf; server copy's only consumer is this errorcheck." },
	// [ZIP-VIEW] pure client camera-ease vectors along the rail; real rail path
	// is m_localOrigin (kept fed). Dedi networks and authors the position member.
	{ "m_ziplineViewOffsetPosition", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"pure client camera-ease vector along the zip rail (dedi networks and authors the member); real rail path (m_localOrigin) stays fed." },
	{ "m_ziplineViewOffsetVelocity", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"pure client camera-ease vector along the zip rail, same contract as m_ziplineViewOffsetPosition." },
	// [SPRINT-STICKY] discrete sticky latch; dedi networks and authors all three.
	// Masked while the wire value still reads 0 -- pairing verdict from bridge_bind_census.
	{ "m_fIsSprinting", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, &sdk_pred_sprint_mask, 0,
		"dedi networks it and authors the member; masked while the wire value reads 0 -- run bridge_bind_census to get the pairing verdict." },
	{ "m_bIsStickySprinting", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, &sdk_pred_sprint_mask, 0,
		"dedi networks it (DT_LocalPlayerExclusive, server-half sticky-sprint FSM authors the member); masked while the wire value reads 0 -- run bridge_bind_census to get the pairing verdict." },
	{ "m_fStickySprintMinTime", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, &sdk_pred_sprint_mask, 0,
		"dedi networks it (DT_LocalPlayerExclusive, server-half sticky-sprint FSM authors the member); masked while the wire value reads 0 -- run bridge_bind_census to get the pairing verdict." },
	// [ANIM-NEXT] anim-queue next-slot bookkeeping. Dedi networks it in
	// DT_LocalPlayerExclusive; client still reads 0 pending the bind-census verdict.
	{ "m_playAnimationNext", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"dedi networks it (DT_LocalPlayerExclusive); client still reads 0 -- run bridge_bind_census to get the pairing verdict. Nested array also flattens as [0]/[1] on some dmaps." },
	{ "m_playAnimationNext[0]", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"m_playAnimationNext flattened array element, same contract." },
	{ "m_playAnimationNext[1]", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"m_playAnimationNext flattened array element, same contract." },
	// [REGEN-STAMP] weapon scope. See the bridge_regen_stamp_client banner: the
	// dedi's regen FSM is unreachable on a dedicated server, so the wire carries
	// an inactive sentinel that a rebase would otherwise install over a live
	// cooldown stamp -- restarting the regen of any ability that is regenerating.
	{ "m_lastRegenTime", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_WEAPON, nullptr, &bridge_regen_stamp_client, 0,
		"the dedi cannot author this stamp (regen FSM gated on cl_predict, false on a dedicated server), so the client owns what it alone simulates; without ownership the wire sentinel stomps a live cooldown every ack." },
	// [SCRIPTVEC] S21-only props appended with no S3 member; dedi value-proxies
	// each to a constant 0 by design, so the wire has nothing to say and the client owns it.
	{ "m_scriptVector", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_WEAPON, nullptr, &bridge_scriptvec_client, 0,
		"S21-only prop appended with no S3 member; the dedi value-proxies it to a constant 0 by design, so the wire has nothing to say and the client owns it." },
	{ "m_scriptVectorTransitionStart", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_WEAPON, nullptr, &bridge_scriptvec_client, 0,
		"S21-only prop appended with no S3 member; the dedi value-proxies it to a constant 0 by design, so the wire has nothing to say and the client owns it." },
	{ "m_scriptVectorTransitionEnd", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_WEAPON, nullptr, &bridge_scriptvec_client, 0,
		"S21-only prop appended with no S3 member; the dedi value-proxies it to a constant 0 by design, so the wire has nothing to say and the client owns it." },
	{ "m_scriptVectorTransitionDuration", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_WEAPON, nullptr, &bridge_scriptvec_client, 0,
		"S21-only prop appended with no S3 member; the dedi value-proxies it to a constant 0 by design, so the wire has nothing to say and the client owns it." },
	{ "m_scriptVectorTransitionStartTime", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_WEAPON, nullptr, &bridge_scriptvec_client, 0,
		"S21-only prop appended with no S3 member; the dedi value-proxies it to a constant 0 by design, so the wire has nothing to say and the client owns it." },
	//-------------------------------------------------------------------
	// CLIENT_TIMING_COMPARE_ONLY
	//-------------------------------------------------------------------
	{ "seComboVars", PredAuthClass_t::CLIENT_TIMING_COMPARE_ONLY, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"status-effect packed dword: server copy is authoritative-ish bookkeeping (server-only apply/pack writer); repairing the live member would change semantics." },
	{ "m_lastUCmdSimulationTicks", PredAuthClass_t::CLIENT_TIMING_COMPARE_ONLY, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"FIELD_INTEGER twin of the timeBase sawtooth: pure sim-bookkeeping the client recomputes per cmd; repairing the live member would change semantics." },

	//-------------------------------------------------------------------
	// TOLERANCE (Shape B)
	//-------------------------------------------------------------------
	{ "m_localOrigin", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_ANY, &sdk_pred_tol_origin, nullptr, 0,
		"absorbs S3-vs-S21 cross-binary float drift + sub-unit land snaps; real multi-unit mispredictions still flag." },
	{ "m_vecVelocity", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_ANY, &sdk_pred_tol_velocity, nullptr, 0,
		"absorbs cross-binary float drift on velocity." },
	{ "m_vecAbsVelocity", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_ANY, &sdk_pred_tol_velocity, nullptr, 0,
		"absorbs cross-binary float drift on velocity." },
	{ "m_groundNormal", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_ANY, &sdk_pred_tol_groundnormal, nullptr, 0,
		"PREDICTION_LAG_MASTER #40: per-tick live-trace field inherits the ~1-tick pairing residual on every ground contact; genuine surface-class changes are far larger." },
	// m_flFallVelocity: SIM_DIVERGENT interim per PART 3 (Shape C backlog item).
	{ "m_flFallVelocity", PredAuthClass_t::SIM_DIVERGENT, AUTH_SCOPE_ANY, &sdk_pred_tol_fallvelocity, nullptr, 0,
		"one-tick gravity residue at land/zip edges (open Shape-C root item); real multi-tick fall divergences still flag." },
	// Second entry, same name: the classes bind independently (see the "NO break"
	// note in the resolve loop). Tolerance covers the sub-cap noise, saturation
	// covers the region the wire cannot represent at all.
	{ "m_flFallVelocity", PredAuthClass_t::SATURATING, AUTH_SCOPE_ANY, &sdk_pred_fallvel_cap, &sdk_pred_saturate_mask, 0,
		"dedi ships this capped at 4096 with uncapped physics -- past the cap the server value carries no information to reconcile against." },
	// 11 time-tol names, each its own TOLERANCE entry pointing at sdk_pred_tol_time
	// (preserves the multi-name behavior of the single sdk_pred_tol_time ConVar).
	{ "m_lastUCmdSimulationRemainderTime", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_ANY, &sdk_pred_tol_time, nullptr, 0,
		"pure clock-bookkeeping TIME field; ~1 tick residual under TB-NATIVE." },
	{ "m_airMoveBlockPlaneTime", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_ANY, &sdk_pred_tol_time, nullptr, 0,
		"pure clock-bookkeeping TIME field; ~1 tick residual under TB-NATIVE." },
	{ "m_traversalStartTime", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_ANY, &sdk_pred_tol_time, nullptr, 0,
		"once-written ride stamp; carries the server clock-servo offset baked in at write time, never re-derived -- takes the sawtooth-amplitude tolerance." },
	{ "m_traversalBlendOutStartTime", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_ANY, &sdk_pred_tol_time, nullptr, 0,
		"once-written ride stamp, same contract as m_traversalStartTime." },
	{ "m_traversalReleaseTime", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_ANY, &sdk_pred_tol_time, nullptr, 0,
		"once-written ride stamp, same contract as m_traversalStartTime." },
	{ "m_traversalHandAppearTime", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_ANY, &sdk_pred_tol_time, nullptr, 0,
		"once-written ride stamp; the one traversal stamp the family had missed, so it sat at the "
		"bare FIELD_TIME half-tick (0.025) and was the most frequent traversal offender in the census." },
	{ "m_wallDangleJumpOffTime", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_ANY, &sdk_pred_tol_time, nullptr, 0,
		"once-written ride stamp, same contract as m_traversalStartTime." },
	{ "m_wallRunStartTime", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_ANY, &sdk_pred_tol_time, nullptr, 0,
		"once-written ride stamp, same contract as m_traversalStartTime." },
	{ "m_wallRunClearTime", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_ANY, &sdk_pred_tol_time, nullptr, 0,
		"once-written ride stamp, same contract as m_traversalStartTime." },
	// CKnockBack slot windows (m_playerKnockBacks[4]); the pred map lists them bare.
	{ "beginTime", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_PLAYER, &sdk_pred_tol_time, nullptr, 0,
		"knockback window start, written once by the server; moves with every server clock correction." },
	{ "endTime", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_PLAYER, &sdk_pred_tol_time, nullptr, 0,
		"knockback window end, written once by the server; moves with every server clock correction." },
	{ "m_lastZiplineDetachTime", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_ANY, &sdk_pred_tol_time, nullptr, 0,
		"once-written ride stamp, same contract as m_traversalStartTime." },
	{ "m_zoomBaseTime", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_ANY, &sdk_pred_tol_time, nullptr, 0,
		"once-written zoom stamp; also unfed (CLIENT_TIMING entry above) so tolerance is a free extra layer." },
	{ "m_zoomFullStartTime", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_ANY, &sdk_pred_tol_time, nullptr, 0,
		"once-written zoom stamp, same contract as m_zoomBaseTime." },
	// [PRED-TOL-TRAV] the traversal geometry snapshot -- see the sdk_pred_tol_traversal banner.
	{ "m_traversalBegin", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_ANY, &sdk_pred_tol_traversal, nullptr, 0,
		"traversal geometry snapshotted from m_localOrigin at ride start; exact-compared while its own source is tolerated at 0.60, so sub-tolerance origin drift rebased the landing ack." },
	{ "m_traversalMid", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_ANY, &sdk_pred_tol_traversal, nullptr, 0,
		"traversal geometry snapshot, same contract as m_traversalBegin." },
	{ "m_traversalEnd", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_ANY, &sdk_pred_tol_traversal, nullptr, 0,
		"traversal geometry snapshot, same contract as m_traversalBegin." },
	{ "m_traversalRefPos", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_ANY, &sdk_pred_tol_traversal, nullptr, 0,
		"traversal geometry snapshot, same contract as m_traversalBegin." },
	{ "m_traversalBlendOutStartOffset", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_ANY, &sdk_pred_tol_traversal, nullptr, 0,
		"traversal blend-out offset, same snapshot contract; the most frequent traversal offender in the census." },
	{ "m_traversalForwardDir", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_ANY, &sdk_pred_tol_traversal_dir, nullptr, 0,
		"traversal facing snapshotted at ride start; unit vector, so it takes an angular tolerance rather than the positional one." },
	{ "m_traversalMidFrac", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_ANY, &sdk_pred_tol_traversal_frac, nullptr, 0,
		"traversal arc parameter derived from the same snapshot; median divergence is one float32 ULP." },
	// 1P viewmodel traversal playback rate is forced to 0; both 1P and 3P cycles
	// are written from this field alone, so the client owns the phase.
	{ "m_currentFramePlayer.m_traversalAnimProgress", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"sole cycle driver for both traversal anim consumers; client-predicted phase, never the dedi's." },
	{ "m_traversalAnimProgress", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"undotted spelling of the same field, same contract." },

	//-------------------------------------------------------------------
	// FORCE_ERRORCHECK -- reconcile the cause, not the consequence
	//-------------------------------------------------------------------
	{ "m_slidingZiplineAlpha", PredAuthClass_t::FORCE_ERRORCHECK, AUTH_SCOPE_ANY,
		&sdk_pred_tol_zipalpha, &bridge_zip_alpha_authority, 0,
		"the rail STATE: ships FTYPEDESC_SKIP while its derived m_localOrigin is the field that flags, so the engine checks the consequence and ignores the cause. Speed/acceleration never cross the wire, so an unreconciled parameter drifts without bound in rail length." },
	{ "m_mountingZiplineAlpha", PredAuthClass_t::FORCE_ERRORCHECK, AUTH_SCOPE_ANY,
		&sdk_pred_tol_zipalpha, &bridge_zip_alpha_authority, 0,
		"mount-phase rail parameter, same contract as m_slidingZiplineAlpha -- the mount point is where the ride's position split is seeded." },

	//-------------------------------------------------------------------
	// DOMAIN_TRANSPLANT_ANCHOR / DOMAIN_TRANSPLANT (the clock family)
	//-------------------------------------------------------------------
	{ "m_currentFramePlayer.timeBase", PredAuthClass_t::DOMAIN_TRANSPLANT_ANCHOR, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"clock reference: irreducible +-2-tick sawtooth across the bridge; no residual content once it is the reference -- compare-masked outright." },
	{ "m_flNextAttack", PredAuthClass_t::DOMAIN_TRANSPLANT, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"clock-derived gate time: compared/repaired in RELATIVE time so residual timeBase epoch skew cancels; genuine denial/reset still flags." },
	{ "m_meleePressTime", PredAuthClass_t::DOMAIN_TRANSPLANT, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"clock-derived gate time, same contract as m_flNextAttack." },
	{ "m_raiseFromMeleeEndTime", PredAuthClass_t::DOMAIN_TRANSPLANT, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"clock-derived gate time, same contract as m_flNextAttack." },
	{ "attackStartTime", PredAuthClass_t::DOMAIN_TRANSPLANT, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"clock-derived gate time, same contract as m_flNextAttack." },
	// PlayerMelee_SetAttackHitEntity writes both of these from gpGlobals->curtime,
	// four bytes from attackStartTime -- the client's curtime there IS the command
	// time base, the dedi's is server tick time. Same absolute-stamp contract, so
	// they take the same relative compare; a real hit/no-hit disagreement (0 vs a
	// live stamp) is far outside the sawtooth and still flags.
	{ "attackHitEntityTime", PredAuthClass_t::DOMAIN_TRANSPLANT, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"melee hit stamp, same contract as attackStartTime; hit-vs-no-hit disagreements still flag." },
	{ "attackLastHitNonWorldEntity", PredAuthClass_t::DOMAIN_TRANSPLANT, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"melee non-world hit stamp, same contract as attackHitEntityTime." },
	// [ZOOM RECLASS ] moved from CLIENT_TIMING: S3 delivers both stamps; only
	// the epoch is bridge-structural, which is exactly what transplant cancels.
	{ "m_zoomBaseTime", PredAuthClass_t::DOMAIN_TRANSPLANT, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"ADS toggle stamp: clock-derived, compared/repaired in RELATIVE time; a genuine server zoom denial/reset still flags and adopts." },
	{ "m_zoomFullStartTime", PredAuthClass_t::DOMAIN_TRANSPLANT, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"ADS full-zoom stamp, same contract as m_zoomBaseTime." },

	// [WEAP-CLOCK] weapon-scoped absolute stamps. Player dmap has timeBase; weapon
	// dmap does not -- Apply borrows the local player's timeBase pair when these
	// bind. NEVER CLIENT_TIMING/repair on fire gates (load-bearing IsReadyToFire).
	{ "m_nextReadyTime", PredAuthClass_t::DOMAIN_TRANSPLANT, AUTH_SCOPE_WEAPON, nullptr, &sdk_pred_weapon_clock, 0,
		"weapon ready gate: relative to player timeBase; residual false errors reopen fire on replay under RTT." },
	{ "m_nextPrimaryAttackTime", PredAuthClass_t::DOMAIN_TRANSPLANT, AUTH_SCOPE_WEAPON, nullptr, &sdk_pred_weapon_clock, 0,
		"primary fire gate, same WEAP-CLOCK contract as m_nextReadyTime." },
	{ "m_attackTimeThisFrame", PredAuthClass_t::DOMAIN_TRANSPLANT, AUTH_SCOPE_WEAPON, nullptr, &sdk_pred_weapon_clock, 0,
		"frame fire stamp + kick-row clock; often pred=0 (see sdk_pred_attacktime_cmp_only)." },
	{ "m_flTimeWeaponIdle", PredAuthClass_t::DOMAIN_TRANSPLANT, AUTH_SCOPE_WEAPON, nullptr, &sdk_pred_weapon_clock, 0,
		"weapon idle stamp; absolute TIME, same WEAP-CLOCK path (not the parked FSM CLIENT_TIMING mask)." },
	{ "m_kickTime", PredAuthClass_t::DOMAIN_TRANSPLANT, AUTH_SCOPE_WEAPON, nullptr, &sdk_pred_weapon_clock, 0,
		"viewkick clock; KICK-ROW residual under lag." },
	{ "m_kickSpringHeatBaseTime", PredAuthClass_t::DOMAIN_TRANSPLANT, AUTH_SCOPE_WEAPON, nullptr, &sdk_pred_weapon_clock, 0,
		"viewkick spring heat stamp, same WEAP-CLOCK contract as m_kickTime." },
	{ "m_requestedAttackEndTime", PredAuthClass_t::DOMAIN_TRANSPLANT, AUTH_SCOPE_WEAPON, nullptr, &sdk_pred_weapon_clock, 0,
		"requested attack end stamp, WEAP-CLOCK." },
	{ "m_fullReloadStartTime", PredAuthClass_t::DOMAIN_TRANSPLANT, AUTH_SCOPE_WEAPON, nullptr, &sdk_pred_weapon_clock, 0,
		"reload start stamp, WEAP-CLOCK." },
	{ "m_predictedAnimEventsReadyToFireTime", PredAuthClass_t::DOMAIN_TRANSPLANT, AUTH_SCOPE_WEAPON, nullptr, &sdk_pred_weapon_clock, 0,
		"predicted anim-event ready stamp, WEAP-CLOCK." },
	{ "m_semiAutoTriggerHoldTime", PredAuthClass_t::DOMAIN_TRANSPLANT, AUTH_SCOPE_WEAPON, nullptr, &sdk_pred_weapon_clock, 0,
		"semi-auto hold stamp, WEAP-CLOCK." },
	{ "m_chargeEndTime", PredAuthClass_t::DOMAIN_TRANSPLANT, AUTH_SCOPE_WEAPON, nullptr, &sdk_pred_weapon_clock, 0,
		"charge end stamp (census residual), WEAP-CLOCK." },
	{ "m_customActivityEndTime", PredAuthClass_t::DOMAIN_TRANSPLANT, AUTH_SCOPE_WEAPON, nullptr, &sdk_pred_weapon_clock, 0,
		"custom activity end stamp, WEAP-CLOCK." },
	// Dual TOLERANCE rows: native compare still consults fieldTolerance when relative
	// equalize does not fire (same pattern as zoom stamps).
	{ "m_nextReadyTime", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_WEAPON, &sdk_pred_tol_weapon_time, &sdk_pred_weapon_clock, 0,
		"Shape-B floor for ready gate residual after WEAP-CLOCK relative pass." },
	{ "m_nextPrimaryAttackTime", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_WEAPON, &sdk_pred_tol_weapon_time, &sdk_pred_weapon_clock, 0,
		"Shape-B floor for primary gate residual after WEAP-CLOCK relative pass." },
	{ "m_attackTimeThisFrame", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_WEAPON, &sdk_pred_tol_weapon_time, &sdk_pred_weapon_clock, 0,
		"Shape-B floor for frame fire stamp residual after WEAP-CLOCK relative pass." },
	{ "m_flTimeWeaponIdle", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_WEAPON, &sdk_pred_tol_weapon_time, &sdk_pred_weapon_clock, 0,
		"Shape-B floor for idle stamp residual." },
	{ "m_kickTime", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_WEAPON, &sdk_pred_tol_weapon_time, &sdk_pred_weapon_clock, 0,
		"Shape-B floor for kick clock residual." },
	{ "m_kickSpringHeatBaseTime", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_WEAPON, &sdk_pred_tol_weapon_time, &sdk_pred_weapon_clock, 0,
		"Shape-B floor for kick heat stamp residual." },
	{ "m_chargeEndTime", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_WEAPON, &sdk_pred_tol_weapon_time, &sdk_pred_weapon_clock, 0,
		"Shape-B floor for charge end residual." },
	{ "m_customActivityEndTime", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_WEAPON, &sdk_pred_tol_weapon_time, &sdk_pred_weapon_clock, 0,
		"Shape-B floor for custom activity end residual." },
	// [PUNCH Shape-B] aim angle residual. AngleVel is NO_ERRORCHECK below (integrator).
	// ValidateTable only kills CLIENT_TIMING punch rows when legacy mask is off.
	{ "m_currentFrameLocalPlayer.m_vecPunchWeapon_Angle", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_PLAYER, &sdk_pred_tol_punch, &sdk_pred_punch_tol_enable, 0,
		"viewkick aim angle residual under RTT; Shape-B while legacy punch mask is off." },
	{ "m_currentFrameLocalPlayer.m_vecPunchBase_Angle", PredAuthClass_t::TOLERANCE, AUTH_SCOPE_PLAYER, &sdk_pred_tol_punch, &sdk_pred_punch_tol_enable, 0,
		"base punch aim angle residual, Shape-B." },
	// Integrator vel: multi-deg residual when kick-row lags; wire still applies.
	{ "m_currentFrameLocalPlayer.m_vecPunchWeapon_AngleVel.x", PredAuthClass_t::NO_ERRORCHECK, AUTH_SCOPE_PLAYER, nullptr, &sdk_pred_punch_vel_noerr, 0,
		"punch anglevel integrator; skip had-errors only (not aim direction)." },
	{ "m_currentFrameLocalPlayer.m_vecPunchWeapon_AngleVel.y", PredAuthClass_t::NO_ERRORCHECK, AUTH_SCOPE_PLAYER, nullptr, &sdk_pred_punch_vel_noerr, 0,
		"punch anglevel integrator, same as .x." },
	{ "m_currentFrameLocalPlayer.m_vecPunchWeapon_AngleVel.z", PredAuthClass_t::NO_ERRORCHECK, AUTH_SCOPE_PLAYER, nullptr, &sdk_pred_punch_vel_noerr, 0,
		"punch anglevel integrator, same as .x." },
	{ "m_currentFrameLocalPlayer.m_vecPunchBase_AngleVel", PredAuthClass_t::NO_ERRORCHECK, AUTH_SCOPE_PLAYER, nullptr, &sdk_pred_punch_vel_noerr, 0,
		"base punch anglevel integrator; skip had-errors only." },
	{ "m_flSoundCodeControllerValue", PredAuthClass_t::NO_ERRORCHECK, AUTH_SCOPE_ANY, nullptr, &sdk_pred_soundcode_noerr, 0,
		"server-authored sound controller; client has no simulation for it, so the "
		"predicted record is always the previous replicated value." },
	{ "m_bIsSoundCodeControllerValueSet", PredAuthClass_t::NO_ERRORCHECK, AUTH_SCOPE_ANY, nullptr, &sdk_pred_soundcode_noerr, 0,
		"set-flag twin of m_flSoundCodeControllerValue; same class." },

	// [HEAT] derived presentation -- client rewrites every pred frame from client-owned
	// lastPrimary; server Derive uses real fire stamp. OnLastFire stays FED seed.
	{ "m_heatValue", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_WEAPON, nullptr, &bridge_heat_client_owned, 0,
		"derived heat output; dual-authority under RTT without client ownership." },
	{ "m_fullyHeated", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_WEAPON, nullptr, &bridge_heat_client_owned, 0,
		"derived fully-heated latch; same heat ownership contract." },
	{ "m_scriptFloat0", PredAuthClass_t::CLIENT_TIMING, AUTH_SCOPE_WEAPON, nullptr, &bridge_heat_client_owned, 0,
		"client mirrors heat into scriptFloat0 every decay frame; client owns presentation." },

	// [IDEAL-SEQ] NO_ERRORCHECK -- not CLIENT_TIMING. Parked FSM CLIENT_TIMING+repair
	// made shooting worse (latched stale deploy). Skip compare only; wire still feeds.
	{ "m_idealSequence", PredAuthClass_t::NO_ERRORCHECK, AUTH_SCOPE_WEAPON, nullptr, &sdk_pred_ideal_seq_noerr, 0,
		"seqtable parity proven 1:1; residual is authorship lag, not namespace drift -- drop from had-errors only." },
	{ "m_idealActivity", PredAuthClass_t::NO_ERRORCHECK, AUTH_SCOPE_WEAPON, nullptr, &sdk_pred_ideal_seq_noerr, 0,
		"companion to m_idealSequence; activity is already xlat'd on wire -- authorship lag only." },
	// Fire counters: WEAP-FIRE-LAG PIPE equalizes |d|<=n only (kLagNames). Free
	// NO_ERRORCHECK here was policy debt -- multi-step miss must still flag.

	// m_weapState remains FED. idealSequence/Activity are NO_ERRORCHECK
	// above (compare-skip only). Do not rewrite live m_weapState during fire;
	// that reintroduces m_bZooming thrash.

	//-------------------------------------------------------------------
	// BYTEMASK
	//-------------------------------------------------------------------
	{ "m_weaponDisabledFlags", PredAuthClass_t::CLIENT_TIMING_COMPARE_ONLY, AUTH_SCOPE_ANY, nullptr, nullptr, 0x04,
		"#34e: server never sets utility bit; bits 0/1 stay exact-compared" },

	//-------------------------------------------------------------------
	// SERVER_CONTENT tombstones -- structural guard against masking a
	// server-content field. Boot validator refuses any interventionist
	// duplicate of these names.
	//-------------------------------------------------------------------
	{ "weapons", PredAuthClass_t::SERVER_CONTENT, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"inventory handle array: CONTENT, must rebase to enter predicted-adoption state. Masking this blocked picking up ground weapons." },
	{ "activeWeapons", PredAuthClass_t::SERVER_CONTENT, AUTH_SCOPE_PLAYER, nullptr, nullptr, 0,
		"active weapon handles: inventory CONTENT; genuine server switch/pickup must reach predicted state. Tombstone only -- installs no mechanism; boot validator refuses any interventionist duplicate." },
	{ "m_selectedOffhands", PredAuthClass_t::SERVER_CONTENT, AUTH_SCOPE_PLAYER, nullptr, nullptr, 0,
		"offhand selection CONTENT; co-fires with activeWeapons on switch windows, same adoption contract. Tombstone only -- installs no mechanism; boot validator refuses any interventionist duplicate." },
	{ "m_latestPrimaryWeaponsIndexZeroOrOne", PredAuthClass_t::SERVER_CONTENT, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"inventory selection CONTENT (not pure timing); masking this co-blocks pickup adoption alongside 'weapons'." },
	{ "m_ammoInClip", PredAuthClass_t::SERVER_CONTENT, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"ammo CONTENT: genuine server changes (consumption, resupply) must reach the predicted state." },
	{ "m_ammoInStockpile", PredAuthClass_t::SERVER_CONTENT, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"ammo CONTENT, same contract as m_ammoInClip." },
	{ "m_weaponTypeDisabledFlags", PredAuthClass_t::SERVER_CONTENT, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"server-authoritative weapon-usability CONTENT (distinct from the m_weaponDisabledFlags utility-bit BYTEMASK above)." },
	{ "m_modBitfieldCurrent", PredAuthClass_t::SERVER_CONTENT, AUTH_SCOPE_ANY, nullptr, nullptr, 0,
		"weapon mod CONTENT: genuine server changes (attachment swap) must reach the predicted state." },

	//-------------------------------------------------------------------
	// NO_ERRORCHECK -- fields the client has no business predicting, excluded
	// from the compare through the engine's own FTYPEDESC_SKIP bit. Replication
	// is untouched; only the rebase verdict stops consulting them.
	//-------------------------------------------------------------------
	{ "m_nResetEventsParity", PredAuthClass_t::NO_ERRORCHECK, AUTH_SCOPE_ANY,
		nullptr, &sdk_pred_animevt_noerrcheck, 0,
		"animation-event parity: bumped inside ResetSequence, so predicting it correctly would "
		"require the client's animation code to call ResetSequence at exactly the moments the "
		"S3 dedi does. Its only consumer compares against the previous frame off the LIVE "
		"member, which the wire still feeds. Largest single measured rebase driver (3483 of "
		"3497 viewmodel offences). Server-authored; NO_ERRORCHECK via FTYPEDESC_SKIP (0x400)." },
	{ "m_nNewSequenceParity", PredAuthClass_t::NO_ERRORCHECK, AUTH_SCOPE_ANY,
		nullptr, &sdk_pred_animevt_noerrcheck, 0,
		"sequence parity: same contract as m_nResetEventsParity; stock Source ships it "
		"NOERRORCHECK for the same reason. Inert if this build does not carry the field." },
	// NO_ERRORCHECK = compare only. Restore still installs (FTYPEDESC_SKIP is
	// not honoured by the fast-path memmove). Was srv=0 forever until the wire
	// append landed the remainder in DT_CurrentData_LocalPlayer; rem=0 next to
	// m_duckState=1 completed the duck on every replay (+16.500 hull step).
	{ "m_currentFrameLocalPlayer.m_duckTransitionRemainderMsec", PredAuthClass_t::NO_ERRORCHECK, AUTH_SCOPE_ANY,
		nullptr, &sdk_pred_skip_duckrem, 0,
		"integer remainder; exact compare is noise across one-cmd sampling skew. "
		"NO_ERRORCHECK suppresses only the compare -- restore still installs. "
		"Wire must carry the real value (DT_CurrentData_LocalPlayer append)." },
	{ "m_updraftStage", PredAuthClass_t::NO_ERRORCHECK, AUTH_SCOPE_PLAYER,
		nullptr, &sdk_pred_updraft_stage_noerrcheck, 0,
		"the only updraft prop with two authors: the client writes it in its own "
		"predicted air move, the server also sends it. Error-checked exactly and "
		"the flip tick cannot be made to agree. Restore still installs it." },
	{ "m_jetDriveWasActive", PredAuthClass_t::FORCE_ERRORCHECK, AUTH_SCOPE_PLAYER,
		nullptr, &sdk_pred_jetdrive_errcheck, 0,
		"recv seed plus Accel-mutated predicted state. Keep in the compare." },
	{ "m_jetDriveActive", PredAuthClass_t::FORCE_ERRORCHECK, AUTH_SCOPE_PLAYER,
		nullptr, &sdk_pred_jetdrive_errcheck, 0,
		"recv seed plus Accel-mutated predicted state. Keep in the compare." },
	{ "m_jetDriveTargetEnt", PredAuthClass_t::FORCE_ERRORCHECK, AUTH_SCOPE_PLAYER,
		nullptr, &sdk_pred_jetdrive_errcheck, 0,
		"recv seed plus Accel-mutated predicted state. Keep in the compare." },
	{ "m_jetDriveInDecelWindow", PredAuthClass_t::FORCE_ERRORCHECK, AUTH_SCOPE_PLAYER,
		nullptr, &sdk_pred_jetdrive_errcheck, 0,
		"recv seed plus Accel-mutated predicted state. Keep in the compare." },
	{ "m_jetDriveSpeed", PredAuthClass_t::FORCE_ERRORCHECK, AUTH_SCOPE_PLAYER,
		nullptr, &sdk_pred_jetdrive_errcheck, 0,
		"recv seed plus Accel-mutated predicted state. Keep in the compare." },
	{ "m_jetDriveAccel", PredAuthClass_t::FORCE_ERRORCHECK, AUTH_SCOPE_PLAYER,
		nullptr, &sdk_pred_jetdrive_errcheck, 0,
		"recv seed plus Accel-mutated predicted state. Keep in the compare." },
	{ "m_jetDriveTimeout", PredAuthClass_t::FORCE_ERRORCHECK, AUTH_SCOPE_PLAYER,
		nullptr, &sdk_pred_jetdrive_errcheck, 0,
		"recv seed plus Accel-mutated predicted state. Keep in the compare." },
	{ "m_jetDriveDoubleJumpVelBackFrac", PredAuthClass_t::FORCE_ERRORCHECK, AUTH_SCOPE_PLAYER,
		nullptr, &sdk_pred_jetdrive_errcheck, 0,
		"recv seed plus Accel-mutated predicted state. Keep in the compare." },
	{ "m_jetDriveStartTime", PredAuthClass_t::FORCE_ERRORCHECK, AUTH_SCOPE_PLAYER,
		nullptr, &sdk_pred_jetdrive_errcheck, 0,
		"recv seed plus Accel-mutated predicted state. Keep in the compare." },
	{ "m_jetDriveDecelWindowTimeOutTime", PredAuthClass_t::FORCE_ERRORCHECK, AUTH_SCOPE_PLAYER,
		nullptr, &sdk_pred_jetdrive_errcheck, 0,
		"recv seed plus Accel-mutated predicted state. Keep in the compare." },
	{ "m_jetDriveTargetPos", PredAuthClass_t::FORCE_ERRORCHECK, AUTH_SCOPE_PLAYER,
		nullptr, &sdk_pred_jetdrive_errcheck, 0,
		"recv seed plus Accel-mutated predicted state. Keep in the compare." },
	{ "m_jetDriveTargetEntOffset", PredAuthClass_t::FORCE_ERRORCHECK, AUTH_SCOPE_PLAYER,
		nullptr, &sdk_pred_jetdrive_errcheck, 0,
		"recv seed plus Accel-mutated predicted state. Keep in the compare." },
	{ "m_jetDriveStartPos", PredAuthClass_t::FORCE_ERRORCHECK, AUTH_SCOPE_PLAYER,
		nullptr, &sdk_pred_jetdrive_errcheck, 0,
		"recv seed plus Accel-mutated predicted state. Keep in the compare." },
	{ "m_jetDriveDoubleJumpVelocity", PredAuthClass_t::FORCE_ERRORCHECK, AUTH_SCOPE_PLAYER,
		nullptr, &sdk_pred_jetdrive_errcheck, 0,
		"recv seed plus Accel-mutated predicted state. Keep in the compare." },
	{ "viewOffsetEntityHandle", PredAuthClass_t::NO_ERRORCHECK, AUTH_SCOPE_ANY,
		nullptr, &bridge_viewoffset_ent_noerr, 0,
		"entity handle; native compare resolves both sides through the entity list "
		"and dedi/client index entities differently, so it can never agree. Skip "
		"compare only -- repairing would write a client-side index over a server one." },
};
static constexpr int kAuthTableCount = sizeof(s_authTable) / sizeof(s_authTable[0]);

// Runtime dead-flag array, one slot per s_authTable entry (validator disables
// interventionist duplicates of a SERVER_CONTENT name).
static bool s_authDead[kAuthTableCount] = {};

//-----------------------------------------------------------------------------
// PredAuth_ValidateTable: runs once (static latch) at first PredAuth_Apply.
// For every pair of entries with identical pszName where one is SERVER_CONTENT
// and the other is any interventionist class, disable the interventionist entry
// and warn loudly. The SERVER_CONTENT guard is structurally enforced.
//-----------------------------------------------------------------------------
static void PredAuth_ValidateTable(void)
{
	int nConflicts = 0;
	int nTiming = 0, nCmpOnly = 0, nTol = 0, nTransplant = 0, nSimDiv = 0, nContent = 0, nSat = 0;
	int nNoErrChk = 0, nForceErrChk = 0;

	for (int i = 0; i < kAuthTableCount; ++i)
	{
		switch (s_authTable[i].eClass)
		{
		case PredAuthClass_t::CLIENT_TIMING:              ++nTiming;      break;
		case PredAuthClass_t::CLIENT_TIMING_COMPARE_ONLY: ++nCmpOnly;     break;
		case PredAuthClass_t::TOLERANCE:                  ++nTol;         break;
		case PredAuthClass_t::DOMAIN_TRANSPLANT_ANCHOR:
		case PredAuthClass_t::DOMAIN_TRANSPLANT:          ++nTransplant;  break;
		case PredAuthClass_t::SIM_DIVERGENT:              ++nSimDiv;      break;
		case PredAuthClass_t::SATURATING:                 ++nSat;         break;
		case PredAuthClass_t::NO_ERRORCHECK:              ++nNoErrChk;    break;
		case PredAuthClass_t::FORCE_ERRORCHECK:           ++nForceErrChk; break;
		case PredAuthClass_t::SERVER_CONTENT:             ++nContent;     break;
		}

		if (s_authTable[i].eClass != PredAuthClass_t::SERVER_CONTENT)
			continue;

		for (int j = 0; j < kAuthTableCount; ++j)
		{
			if (i == j || s_authTable[j].eClass == PredAuthClass_t::SERVER_CONTENT)
				continue;
			if (strcmp(s_authTable[i].pszName, s_authTable[j].pszName) != 0)
				continue;

			s_authDead[j] = true;
			++nConflicts;
			Warning(eDLL_T::CLIENT,
				"[PRED-AUTH] TABLE CONFLICT '%s' -- SERVER_CONTENT wins, interventionist entry DISABLED\n",
				s_authTable[i].pszName);
		}
	}

	// The punch mask's two stated reasons have both expired. "RNG jitter cannot
	// pair" was retired by bridge_cmd_seed_parity -- the kick draws SharedRandom
	// off cmd+0x184, which parity makes byte-identical. "The server copy has no
	// real consumer" is wrong outright: the dedi's authoritative aim assembly
	// folds the punch, scaled by lerp(hipfire, ADS, zoomFrac), straight into the
	// direction the bullet is fired along. Measured by joining the two
	// fire taps per command number: the pre-spread aim matches EXACTLY on the
	// first shot of every burst, then ramps with the punch to ~3.6 deg by the end
	// of it -- while masked, nothing can ever pull the two springs back together.
	// Retire ONLY CLIENT_TIMING punch ownership rows. TOLERANCE / NO_ERRORCHECK
	// punch rows must stay live so Shape-B and pWV skip-errorcheck can arm
	// (strstr-all was killing all 12 and left exact compare on punch).
	if (!bridge_punch_legacy_mask.GetBool())
	{
		int nFreed = 0;

		for (int i = 0; i < kAuthTableCount; ++i)
		{
			if (s_authDead[i] || !strstr(s_authTable[i].pszName, "m_vecPunch"))
				continue;
			if (s_authTable[i].eClass != PredAuthClass_t::CLIENT_TIMING)
				continue;

			s_authDead[i] = true;
			++nFreed;
		}

		Warning(eDLL_T::CLIENT,
			"[PRED-AUTH] punch mask retired: %d CLIENT_TIMING m_vecPunch* rows "
			"(TOLERANCE/NO_ERRORCHECK punch rows stay live; bridge_punch_legacy_mask 1 restores ownership mask)\n",
			nFreed);
	}

	Warning(eDLL_T::CLIENT,
		"[PRED-AUTH] table validated: %d entries (timing=%d cmpOnly=%d tol=%d transplant=%d simDiv=%d sat=%d noErrChk=%d forceErrChk=%d content=%d) conflicts=%d\n",
		kAuthTableCount, nTiming, nCmpOnly, nTol, nTransplant, nSimDiv, nSat, nNoErrChk, nForceErrChk, nContent, nConflicts);
}

//-----------------------------------------------------------------------------
// Per-dmap resolved cache. Mirrors the shape of the old UnfedCache, generalized
// to be table-driven. wdf-equivalent is now a small {off,mask} array (cap 4) so
// future byteMask entries do not require a new dedicated field.
//-----------------------------------------------------------------------------
enum { kUnfedSlotMax = 128 };
enum { kClockDrvSlotMax = 32 }; // player gates + weapon fire/kick/idle stamps
enum { kByteMaskSlotMax = 4 };
enum { kSatSlotMax = 4 };
enum { kStampSlotMax = 24 };

enum AuthDmapKind_t { AUTHKIND_ANY = 0, AUTHKIND_PLAYER, AUTHKIND_WEAPON, AUTHKIND_VIEWMODEL };

struct AuthCache_t
{
	uintptr_t dmap;
	AuthDmapKind_t kind;

	// CLIENT_TIMING / CLIENT_TIMING_COMPARE_ONLY slots (unfed apply loop)
	int n;
	int off[kUnfedSlotMax];
	int size[kUnfedSlotMax];
	int mOff[kUnfedSlotMax];      // [PRED-REPAIR] flatOffset[0] twin (-1 = unresolved/insane)
	bool mEligible[kUnfedSlotMax];
	const PredAuthEntry_t* entry[kUnfedSlotMax];

	// DOMAIN_TRANSPLANT_ANCHOR / DOMAIN_TRANSPLANT
	int tbOff; int tbMOff;
	int cdN; int cdOff[kClockDrvSlotMax]; int cdMOff[kClockDrvSlotMax];
	const PredAuthEntry_t* cdEntry[kClockDrvSlotMax];

	// byteMask (generalized wdf); entry ptr so pGateCvar works like unfed slots
	int bmN; int bmOff[kByteMaskSlotMax]; uint8_t bmMask[kByteMaskSlotMax];
	const PredAuthEntry_t* bmEntry[kByteMaskSlotMax];

	// SATURATING -- record offset + the ConVar supplying the wire ceiling
	int satN; int satOff[kSatSlotMax]; const PredAuthEntry_t* satEntry[kSatSlotMax];

	// once-written stamp family (sdk_pred_tol_time rows): record + member offsets
	int stampN; int stampOff[kStampSlotMax]; int stampMOff[kStampSlotMax];
	const PredAuthEntry_t* stampEntry[kStampSlotMax];

	int orgOff; // m_localOrigin packed offset; -1 if unbound
};
static AuthCache_t s_cache[16] = {};

//-----------------------------------------------------------------------------
// [PRED-REPAIR] 0 = self-check pending, 1 = member offsets verified sane (repair live),
// -1 = self-check FAILED (flatOffset[0] is not the member offset on this build -> legacy only).
//-----------------------------------------------------------------------------
static int s_predRepairState = 0;

// [FORCED0-NEUT] per-dispatch tally of entity PostNetworkDataReceived had-errors.
static int s_pnrDispatchEntityErrors = 0;
static int s_pnrDispatchActive = 0;

static int AuthWidthForType(int type, int cnt)
{
	int w = 0;
	switch (type)
	{
	case FIELD_FLOAT: case FIELD_TIME: case FIELD_INTEGER:
	case FIELD_EHANDLE: case FIELD_COLOR32: w = 4;  break;
	case FIELD_VECTOR:                      w = 12; break;
	case FIELD_QUATERNION:                  w = 16; break;
	case FIELD_SHORT:                       w = 2;  break;
	case FIELD_BOOLEAN: case FIELD_CHARACTER: w = 1; break;
	default: break; // unhandled type -- leave unmasked
	}
	return w * cnt;
}

static const char* AuthKindName(AuthDmapKind_t k)
{
	switch (k)
	{
	case AUTHKIND_PLAYER:     return "player";
	case AUTHKIND_WEAPON:     return "weapon";
	case AUTHKIND_VIEWMODEL:  return "viewmodel";
	default:                  return "any";
	}
}

static bool AuthScopeMatches(int nScope, AuthDmapKind_t kind)
{
	if (nScope == AUTH_SCOPE_ANY)
		return true;
	switch (nScope)
	{
	case AUTH_SCOPE_PLAYER:    return kind == AUTHKIND_PLAYER;
	case AUTH_SCOPE_WEAPON:    return kind == AUTHKIND_WEAPON;
	case AUTH_SCOPE_VIEWMODEL: return kind == AUTHKIND_VIEWMODEL;
	default:                   return false;
	}
}

// Text after the last '.', or the whole string when undotted.
static const char* AuthLeafName(const char* pszName)
{
	if (!pszName)
		return "";
	const char* pDot = strrchr(pszName, '.');
	return pDot ? (pDot + 1) : pszName;
}

// Bind one table entry to one flat typedescription. Shared by exact-name and
// leaf-suffix resolution so both paths run the same class switch.
static void PredAuth_BindOneEntry(AuthCache_t* pc, int e, uintptr_t td, const char* name,
	int type, int cnt, int& nTol, bool* matched, uintptr_t dmap)
{
	const PredAuthEntry_t& ent = s_authTable[e];
	if (!AuthScopeMatches(ent.nScope, pc->kind))
		return;

	switch (ent.eClass)
	{
	case PredAuthClass_t::TOLERANCE:
	case PredAuthClass_t::SIM_DIVERGENT:
	{
		if (ent.pGateCvar && !ent.pGateCvar->GetBool())
		{
			matched[e] = true;
			break;
		}
		// fieldTolerance is only read on the float path of
		// PredNative_Compare; other types memcmp and ignore it.
		const bool bTolHonoured =
			type == FIELD_FLOAT || type == FIELD_TIME
			|| type == FIELD_VECTOR || type == FIELD_QUATERNION;
		if (!bTolHonoured)
		{
			static bool s_tolInertSeen[kAuthTableCount * 4] = {};
			const int nKind = static_cast<int>(pc->kind);
			const int nSeenKey = e * 4
				+ ((nKind >= 0 && nKind < 4) ? nKind : 0);
			if (nSeenKey >= 0 && nSeenKey < kAuthTableCount * 4
				&& !s_tolInertSeen[nSeenKey])
			{
				s_tolInertSeen[nSeenKey] = true;
				Warning(eDLL_T::CLIENT,
					"[PRED-AUTH] inert entry '%s' type=%d kind=%s -- "
					"type is compared exactly so tolerance is ignored; "
					"class NO_ERRORCHECK or CLIENT_TIMING instead\n",
					name, type, AuthKindName(pc->kind));
			}
			matched[e] = true;
			break;
		}
		if (ent.pTolCvar)
		{
			*reinterpret_cast<float*>(td + TD_TOLERANCE) = ent.pTolCvar->GetFloat();
			++nTol;
			if (ent.pTolCvar == &sdk_pred_tol_time && pc->stampN < kStampSlotMax
				&& (type == FIELD_FLOAT || type == FIELD_TIME))
			{
				pc->stampOff[pc->stampN] = *reinterpret_cast<int*>(td + TD_FLATOFFSET1);
				pc->stampMOff[pc->stampN] = PredAuth_MemberOff(td);
				pc->stampEntry[pc->stampN] = &ent;
				++pc->stampN;
			}
		}
		matched[e] = true;
		break;
	}
	case PredAuthClass_t::CLIENT_TIMING:
	case PredAuthClass_t::CLIENT_TIMING_COMPARE_ONLY:
	{
		if (ent.nByteMask != 0)
		{
			if (pc->bmN < kByteMaskSlotMax)
			{
				pc->bmOff[pc->bmN] = *reinterpret_cast<int*>(td + TD_FLATOFFSET1);
				pc->bmMask[pc->bmN] = ent.nByteMask;
				pc->bmEntry[pc->bmN] = &ent;
				++pc->bmN;
			}
			matched[e] = true;
			break;
		}
		const int w = AuthWidthForType(type, cnt);
		if (w > 0)
		{
			if (pc->n < kUnfedSlotMax)
			{
				pc->off[pc->n]  = *reinterpret_cast<int*>(td + TD_FLATOFFSET1);
				pc->size[pc->n] = w;
				pc->mOff[pc->n] = PredAuth_MemberOff(td);
				pc->mEligible[pc->n] = (ent.eClass == PredAuthClass_t::CLIENT_TIMING);
				pc->entry[pc->n] = &ent;
				++pc->n;
				matched[e] = true;
			}
			else if (!matched[e])
			{
				Warning(eDLL_T::CLIENT,
					"[PRED-UNFED-MISS] slot storage FULL (%d) -- '%s' occurrence "
					"dropped on dmap=0x%p; bump kUnfedSlotMax\n",
					kUnfedSlotMax, name, reinterpret_cast<void*>(dmap));
			}
		}
		else
		{
			Warning(eDLL_T::CLIENT,
				"[PRED-UNFED-MISS] '%s' matched by name but fieldType=%d has no "
				"width handler -- left UNMASKED on dmap=0x%p\n",
				name, type, reinterpret_cast<void*>(dmap));
		}
		break;
	}
	case PredAuthClass_t::NO_ERRORCHECK:
	{
		// The engine's own exclusion bit, set once per dmap. Do
		// not clear it when the gate is off -- a build that
		// already ships 0x400 on this field must keep it.
		if (!ent.pGateCvar || ent.pGateCvar->GetBool())
		{
			uint32_t* const pFlags =
				reinterpret_cast<uint32_t*>(td + TD_FLAGS);
			if (!(*pFlags & FTYPEDESC_SKIP))
			{
				*pFlags |= FTYPEDESC_SKIP;
			}
		}
		matched[e] = true;
		break;
	}
	case PredAuthClass_t::FORCE_ERRORCHECK:
	{
		// Inverse of NO_ERRORCHECK. Only ever clears when the gate
		// is on; with the gate off the build's own exemption stands
		// untouched, so flipping the ConVar back mid-session cannot
		// leave the field half-enabled.
		if (ent.pGateCvar && ent.pGateCvar->GetBool())
		{
			uint32_t* const pFlags =
				reinterpret_cast<uint32_t*>(td + TD_FLAGS);
			if (*pFlags & FTYPEDESC_SKIP)
			{
				*pFlags &= ~FTYPEDESC_SKIP;
				if (ent.pTolCvar)
					*reinterpret_cast<float*>(td + TD_TOLERANCE) =
						ent.pTolCvar->GetFloat();
				Warning(eDLL_T::CLIENT,
					"[PRED-AUTH] FORCE_ERRORCHECK '%s' on kind=%s -- "
					"FTYPEDESC_SKIP cleared, tol=%.6g\n",
					name, AuthKindName(pc->kind),
					ent.pTolCvar ? ent.pTolCvar->GetFloat() : 0.0f);
			}
		}
		matched[e] = true;
		break;
	}
	case PredAuthClass_t::SATURATING:
	{
		if (type == FIELD_FLOAT && pc->satN < kSatSlotMax)
		{
			pc->satOff[pc->satN]     = *reinterpret_cast<int*>(td + TD_FLATOFFSET1);
			pc->satEntry[pc->satN]   = &ent;
			++pc->satN;
		}
		matched[e] = true;
		break;
	}
	case PredAuthClass_t::DOMAIN_TRANSPLANT_ANCHOR:
	{
		if (pc->tbOff < 0)
		{
			pc->tbOff = *reinterpret_cast<int*>(td + TD_FLATOFFSET1);
			pc->tbMOff = PredAuth_MemberOff(td);
		}
		matched[e] = true;
		break;
	}
	case PredAuthClass_t::DOMAIN_TRANSPLANT:
	{
		// Gate-off entries stay unbound (native absolute compare).
		if (ent.pGateCvar && !ent.pGateCvar->GetBool())
		{
			matched[e] = true;
			break;
		}
		if ((type == FIELD_FLOAT || type == FIELD_TIME) && pc->cdN < kClockDrvSlotMax)
		{
			pc->cdMOff[pc->cdN] = PredAuth_MemberOff(td);
			pc->cdEntry[pc->cdN] = &ent;
			pc->cdOff[pc->cdN++] = *reinterpret_cast<int*>(td + TD_FLATOFFSET1);
		}
		matched[e] = true;
		break;
	}
	default:
		break;
	}
}

//-----------------------------------------------------------------------------
// [TIME-DOMAIN] absolute-time names that may appear as FIELD_FLOAT in the pred
// map (weapon LocalWeaponData often does). Remainder/bookkeeping fields are
// intentionally absent -- they are not absolute stamps.
//-----------------------------------------------------------------------------
static bool TimeDomain_IsAbsoluteName(const char* name)
{
	if (!name)
		return false;
	static const char* const kNames[] = {
		"m_nextReadyTime",
		"m_nextPrimaryAttackTime",
		"m_lastPrimaryAttackTime",
		"m_lastPrimaryAttack",
		"m_attackTimeThisFrame",
		"m_flTimeWeaponIdle",
		"m_flNextAttack",
		"m_meleePressTime",
		"m_raiseFromMeleeEndTime",
		"attackStartTime",
		"attackHitEntityTime",
		"attackLastHitNonWorldEntity",
		"m_zoomBaseTime",
		"m_zoomFullStartTime",
		"m_traversalStartTime",
		"m_traversalBlendOutStartTime",
		"m_traversalReleaseTime",
		"m_wallDangleJumpOffTime",
		"m_wallRunStartTime",
		"m_wallRunClearTime",
		"m_lastZiplineDetachTime",
		"m_spreadStartTime",
		"m_kickTime",
		"m_fullReloadStartTime",
		"m_cooldownEndTime",
		"m_lastRegenTime",
		"m_chargeStartTime",
		"m_predictedAnimEventsReadyToFireTime",
	};
	for (const char* n : kNames)
	{
		if (!strcmp(name, n))
			return true;
	}
	return false;
}

// PNR ack-count join for residual kick classification (set once per
// PostNetworkDataReceived). Real prediction depth needs the acknowledged-command
// anchor; this probe only has the per-snapshot count.
static int s_pnrLatest = 0;
static int s_pnrAckCount = 0;

void PredAuth_NoteAckCount(int nLatest, int nAckCount)
{
	s_pnrLatest = nLatest;
	s_pnrAckCount = nAckCount;
}

int PredAuth_PnrAckCount(void)
{
	return s_pnrAckCount;
}

int PredAuth_PnrLatest(void)
{
	return s_pnrLatest;
}

// Per-ack domain delta cache (player computed once; weapons reuse).
static unsigned s_tdCmd = 0;
static float    s_tdDelta = 0.f;
static bool     s_tdOk = false;
static void*    s_tdPlayer = nullptr;

// [TIME-DOMAIN] why ComputeDelta declined, so the caller can log it and decide
// whether the last good delta still applies.
enum TdReason_t
{
	TD_OK = 0,
	TD_NO_ENTRY,      // hook prerequisites missing (fn ptr, dmap, player)
	TD_NO_RECORD,     // no predicted slot for this exact command
	TD_NO_FIELDS,     // neither timeBase nor the ticks+remainder fallback resolved
	TD_NOT_STARTED,   // a base still reads <= 0 -- pre-spawn, nothing to align yet
	TD_OUT_OF_RANGE,  // both bases present and sane, but they disagree wildly
};

// Held delta: the last value that passed, carried across commands that decline
// for a transient reason. Without it one stale ack leaves every stamp in the
// server's domain for that command, which is the same silent skip the magnitude
// check used to cause.
static float    s_tdHeldDelta = 0.f;
static bool     s_tdHeldOk = false;
static unsigned s_tdHeldCmd = 0;
static unsigned s_tdHeldUses = 0;
static float    s_tdLastRejected = 0.f;
static TdReason_t s_tdLastReason = TD_OK;

// Commands the held delta may cover before the convert gives up. The bases
// re-align within a couple of acks of any real event; a longer silence means
// something is wrong and raw stamps are the honest output.
static ConVar bridge_time_domain_hold("bridge_time_domain_hold", "8", FCVAR_RELEASE,
	"[TIME-DOMAIN] commands the last good clientTb-serverTb delta stays valid for "
	"when a command cannot compute its own. 0 = never carry it.", true, 0.f, true, 64.f);

// Sanity bound on clientTb-serverTb. Both are read from the SAME acked command,
// so a healthy value is sub-tick; the bound only rejects a genuinely broken pair.
// A lever, not a magic number: an A/B is one edit away.
static ConVar bridge_time_domain_max_delta("bridge_time_domain_max_delta", "0.25", FCVAR_RELEASE,
	"[TIME-DOMAIN] reject a computed clientTb-serverTb delta wider than this (seconds). "
	"Rejections are logged, not silent.", true, 0.f, true, 5.f);

//-----------------------------------------------------------------------------
// [TIME-DOMAIN] LATCH -- convert a stamp once per value the wire delivers.
//
// The convert walks EVERY absolute TIME prop on the entity, but a snapshot only
// re-delivers the props that changed. Without a latch an untouched stamp is
// shifted by d again on every ack and ratchets away from the server's copy
// without bound: measured at a constant d=+0.0185 for a whole session (~20
// acks/s = +0.37s per second), which walked the once-written m_skydiveStartTime
// +1.18s and pushed every ride stamp -- m_traversalStartTime,
// m_traversalHandAppearTime, m_traversalBlendOutStartTime, m_wallRunStartTime,
// m_lastZiplineDetachTime -- past its tolerance, so the ack that lands after a
// zip rebased on stamps nothing had touched. Signature in pred_diff.log: every
// diverging TIME field in an ack shares one delta, and pred at ack N+1 equals
// srv at ack N.
//
// Shadow the value written; a live value still equal to it was not re-delivered.
//-----------------------------------------------------------------------------
enum { kTdLatchEntMax = 24, kTdLatchSlotMax = 96 };

struct TimeDomainLatch_t
{
	void*  pEntity;
	int    n;
	int    off[kTdLatchSlotMax];
	float  flWritten[kTdLatchSlotMax];
};
static TimeDomainLatch_t s_tdLatch[kTdLatchEntMax] = {};
static unsigned s_tdLatchCmd = 0;

static void TimeDomain_ResetLatches(void)
{
	for (int i = 0; i < kTdLatchEntMax; ++i)
	{
		s_tdLatch[i].pEntity = nullptr;
		s_tdLatch[i].n = 0;
	}
}

static TimeDomainLatch_t* TimeDomain_LatchFor(void* pEntity)
{
	for (int i = 0; i < kTdLatchEntMax; ++i)
	{
		if (s_tdLatch[i].pEntity == pEntity)
			return &s_tdLatch[i];
	}
	for (int i = 0; i < kTdLatchEntMax; ++i)
	{
		if (!s_tdLatch[i].pEntity)
		{
			s_tdLatch[i].pEntity = pEntity;
			s_tdLatch[i].n = 0;
			return &s_tdLatch[i];
		}
	}
	// Table full: the caller falls back to the legacy unconditional convert.
	static uint32_t s_nFull = 0;
	if (++s_nFull <= 2)
		Warning(eDLL_T::CLIENT, "[TIME-DOMAIN] latch table full (%d entities) -- "
			"stamps on further predictables convert unlatched and will drift\n", kTdLatchEntMax);
	return nullptr;
}

// Shadow cell for one flat offset, or null when the row has no room left.
static float* TimeDomain_LatchSlot(TimeDomainLatch_t* pLatch, int nOff)
{
	for (int i = 0; i < pLatch->n; ++i)
	{
		if (pLatch->off[i] == nOff)
			return &pLatch->flWritten[i];
	}
	if (pLatch->n >= kTdLatchSlotMax)
	{
		static uint32_t s_nFull = 0;
		if (++s_nFull <= 2)
			Warning(eDLL_T::CLIENT, "[TIME-DOMAIN] latch slots exhausted (%d/entity) -- "
				"remaining stamps convert unlatched and will drift\n", kTdLatchSlotMax);
		return nullptr;
	}
	const int i = pLatch->n++;
	pLatch->off[i] = nOff;
	pLatch->flWritten[i] = 0.f;   // never equals a live stamp (>1.0 gate below)
	return &pLatch->flWritten[i];
}

static bool TimeDomain_ResolveFlatOff(uintptr_t dmap, const char* want, int* pOff0, int* pOff1)
{
	if (pOff0) *pOff0 = -1;
	if (pOff1) *pOff1 = -1;
	if (!dmap || !want)
		return false;
	const uintptr_t opt = *reinterpret_cast<uintptr_t*>(dmap + DMAP_OPTIMIZED);
	if (!opt)
		return false;
	const uintptr_t info   = opt + (uintptr_t)OPT_INFO_STRIDE * PC_NETWORKED_ONLY;
	const uintptr_t fields = *reinterpret_cast<uintptr_t*>(info + INFO_FLAT_FIELDS);
	const int count        = *reinterpret_cast<int*>(info + INFO_FLAT_COUNT);
	if (!fields || count <= 0 || count > 4096)
		return false;
	for (int i = 0; i < count; ++i)
	{
		const uintptr_t td = fields + (uintptr_t)i * TD_STRIDE;
		const char* name = *reinterpret_cast<const char**>(td + TD_FIELDNAME);
		if (!name || strcmp(name, want) != 0)
			continue;
		if (pOff0) *pOff0 = *reinterpret_cast<int*>(td + TD_FLATOFFSET0);
		if (pOff1) *pOff1 = *reinterpret_cast<int*>(td + TD_FLATOFFSET1);
		return true;
	}
	return false;
}

// serverTimeBase from live members (just-applied network), clientTimeBase from
// the predicted slot for nCmd. Prefers m_currentFramePlayer.timeBase; falls
// back to ticks*interval + remainder (interval 0.05 = 20Hz dedi).
static bool TimeDomain_ComputeDelta(void* pPlayer, unsigned int nCmd, float* pDelta,
	TdReason_t* pReason)
{
	if (pReason) *pReason = TD_NO_ENTRY;
	if (!pPlayer || !pDelta || !C_BaseEntity__GetPredictedEntityState)
		return false;

	void* (*GetPredDescMap)(void*) = reinterpret_cast<void* (*)(void*)>(
		(*reinterpret_cast<void***>(pPlayer))[VIDX_GetPredDescMap]);
	const uintptr_t dmap = reinterpret_cast<uintptr_t>(GetPredDescMap(pPlayer));
	if (!dmap)
		return false;

	// Exact-command lookup: the native walks its ring and returns null unless a
	// slot's command number equals nCmd, so a record here is never stale.
	if (pReason) *pReason = TD_NO_RECORD;
	void* pPredState = C_BaseEntity__GetPredictedEntityState(pPlayer, nCmd);
	if (!pPredState)
		return false;
	const uint8_t* pPredicted = EntField<uint8_t*>(pPredState, STATE_SERIALIZED_DATA);
	if (!pPredicted)
		return false;

	uint8_t* pLive = reinterpret_cast<uint8_t*>(pPlayer);
	float tbSrv = 0.f;
	float tbCli = 0.f;
	bool bResolved = false;   // the fields exist and were read
	bool bStarted = false;    // ...and both hold a running base

	int off0 = -1, off1 = -1;
	if (TimeDomain_ResolveFlatOff(dmap, "m_currentFramePlayer.timeBase", &off0, &off1)
		&& off0 >= 0 && off1 >= 0)
	{
		tbSrv = *reinterpret_cast<const float*>(pLive + off0);
		tbCli = *reinterpret_cast<const float*>(pPredicted + off1);
		bResolved = true;
		bStarted = (tbSrv > 0.f && tbCli > 0.f);
	}

	if (!bStarted)
	{
		int t0 = -1, t1 = -1, r0 = -1, r1 = -1;
		if (TimeDomain_ResolveFlatOff(dmap, "m_lastUCmdSimulationTicks", &t0, &t1)
			&& TimeDomain_ResolveFlatOff(dmap, "m_lastUCmdSimulationRemainderTime", &r0, &r1)
			&& t0 >= 0 && t1 >= 0 && r0 >= 0 && r1 >= 0)
		{
			const int nTicksSrv = *reinterpret_cast<const int*>(pLive + t0);
			const int nTicksCli = *reinterpret_cast<const int*>(pPredicted + t1);
			const float flRemSrv = *reinterpret_cast<const float*>(pLive + r0);
			const float flRemCli = *reinterpret_cast<const float*>(pPredicted + r1);
			// S3 dedi / S21 client both run 0.05s tick interval for this bridge.
			constexpr float kInterval = 0.05f;
			tbSrv = static_cast<float>(nTicksSrv) * kInterval + flRemSrv;
			tbCli = static_cast<float>(nTicksCli) * kInterval + flRemCli;
			bResolved = true;
			bStarted = (tbSrv > 0.f && tbCli > 0.f);
		}
	}

	if (!bResolved)
	{
		if (pReason) *pReason = TD_NO_FIELDS;
		return false;
	}
	if (!bStarted)
	{
		if (pReason) *pReason = TD_NOT_STARTED;
		return false;
	}

	// Both bases come from the SAME acked command, so a healthy delta is sub-tick.
	// A wide one is a real fault, not an absence of data -- name it and let the
	// caller decide whether the held delta still covers this command.
	const float d = tbCli - tbSrv;
	const float flMax = bridge_time_domain_max_delta.GetFloat();
	if (flMax > 0.f && fabsf(d) >= flMax)
	{
		s_tdLastRejected = d;
		if (pReason) *pReason = TD_OUT_OF_RANGE;
		return false;
	}

	if (pReason) *pReason = TD_OK;
	*pDelta = d;
	return true;
}

static const char* TimeDomain_ReasonName(TdReason_t r)
{
	switch (r)
	{
	case TD_OK:           return "ok";
	case TD_NO_RECORD:    return "no predicted record for cmd";
	case TD_NO_FIELDS:    return "timeBase/ticks fields unresolved";
	case TD_NOT_STARTED:  return "time base not started";
	case TD_OUT_OF_RANGE: return "delta out of range";
	default:              return "no entry";
	}
}

static void TimeDomain_ConvertEntity(void* pEntity, unsigned int nCmd, float flDelta)
{
	if (!pEntity || flDelta == 0.f)
		return;

	void* (*GetPredDescMap)(void*) = reinterpret_cast<void* (*)(void*)>(
		(*reinterpret_cast<void***>(pEntity))[VIDX_GetPredDescMap]);
	const uintptr_t dmap = reinterpret_cast<uintptr_t>(GetPredDescMap(pEntity));
	if (!dmap)
		return;
	const uintptr_t opt = *reinterpret_cast<uintptr_t*>(dmap + DMAP_OPTIMIZED);
	if (!opt)
		return;
	const uintptr_t info   = opt + (uintptr_t)OPT_INFO_STRIDE * PC_NETWORKED_ONLY;
	const uintptr_t fields = *reinterpret_cast<uintptr_t*>(info + INFO_FLAT_FIELDS);
	const int count        = *reinterpret_cast<int*>(info + INFO_FLAT_COUNT);
	if (!fields || count <= 0 || count > 4096)
		return;

	uint8_t* pLive = reinterpret_cast<uint8_t*>(pEntity);
	TimeDomainLatch_t* const pLatch = TimeDomain_LatchFor(pEntity);
	int nConverted = 0;
	int nLatched = 0;

	for (int i = 0; i < count; ++i)
	{
		const uintptr_t td = fields + (uintptr_t)i * TD_STRIDE;
		const uint32_t flags = *reinterpret_cast<uint32_t*>(td + TD_FLAGS);
		if (flags & (FTYPEDESC_SKIP | FTYPEDESC_GRAPPLE))
			continue;

		const int type = *reinterpret_cast<int*>(td + TD_FIELDTYPE);
		const char* name = *reinterpret_cast<const char**>(td + TD_FIELDNAME);
		const bool bTimeType = (type == FIELD_TIME);
		const bool bNamed = TimeDomain_IsAbsoluteName(name);
		if (!bTimeType && !bNamed)
			continue;
		// Do not shift the domain reference itself -- delta is defined against it.
		if (name && !strcmp(name, "m_currentFramePlayer.timeBase"))
			continue;
		if (name && !strcmp(name, "m_lastUCmdSimulationRemainderTime"))
			continue;

		const int off0 = *reinterpret_cast<int*>(td + TD_FLATOFFSET0);
		if (off0 < 0 || off0 > 0x40000)
			continue;

		float* pVal = reinterpret_cast<float*>(pLive + off0);
		const float v = *pVal;
		// Only real absolute stamps (sentinels 0 / -1 / tiny remainders stay put).
		if (v <= 1.0f)
			continue;

		// A stamp still holding the number this hook wrote was not re-delivered,
		// so it is already in the client's domain -- shifting it again ratchets.
		float* const pShadow = pLatch ? TimeDomain_LatchSlot(pLatch, off0) : nullptr;
		if (pShadow && *pShadow == v)
		{
			++nLatched;
			continue;
		}

		*pVal = v + flDelta;
		if (pShadow)
			*pShadow = *pVal;
		++nConverted;
	}

	if (nConverted > 0 || nLatched > 0)
	{
		static uint32_t s_nWin = 0, s_nProps = 0, s_nEnts = 0, s_nHeld = 0;
		static float s_flAbsSum = 0.f;
		++s_nEnts;
		s_nProps += static_cast<uint32_t>(nConverted);
		s_nHeld  += static_cast<uint32_t>(nLatched);
		s_flAbsSum += fabsf(flDelta);
		if (++s_nWin >= 256)
		{
			// held >> props is the healthy shape: only the stamps the wire actually
			// re-sent convert. held==0 with a steady props count is the ratchet back.
			Warning(eDLL_T::CLIENT,
				"[TIME-DOMAIN] window ents=%u props=%u held=%u mean|d|=%.4f last_d=%+.4f\n",
				s_nEnts, s_nProps, s_nHeld, s_flAbsSum / static_cast<float>(s_nWin), flDelta);
			s_nWin = 0; s_nProps = 0; s_nEnts = 0; s_nHeld = 0; s_flAbsSum = 0.f;
		}
	}
}

float TimeDomain_LastDelta(void)
{
	return s_tdOk ? s_tdDelta : (s_tdHeldOk ? s_tdHeldDelta : 0.f);
}

static void TimeDomain_OnEntity(void* pEntity, unsigned int nCmd)
{
	// One reset per clock-domain break; cleared when a fresh delta computes.
	static bool s_tdOorLatchCleared = false;

	if (!bridge_time_domain_convert.GetBool() || !pEntity || nCmd == 0)
		return;

	// The command counter restarts on reconnect/changelevel, and every entity
	// pointer behind the latch shadows is gone with it. The held delta belongs to
	// the old session's clocks and must go with them.
	if (nCmd < s_tdLatchCmd)
	{
		TimeDomain_ResetLatches();
		s_tdHeldOk = false;
		s_tdHeldUses = 0;
		s_tdOorLatchCleared = false;
	}
	s_tdLatchCmd = nCmd;

	// Refresh per-cmd delta from the local player (ent index 1 on this bridge,
	// or the entity itself when it is the player dmap).
	if (s_tdCmd != nCmd)
	{
		s_tdCmd = nCmd;
		s_tdOk = false;
		s_tdDelta = 0.f;
		s_tdPlayer = nullptr;
	}

	const int nEnt = EntField<int>(pEntity, ENT_ENTINDEX);
	if (nEnt == 1)
		s_tdPlayer = pEntity;

	if (!s_tdOk)
	{
		void* pPlayer = s_tdPlayer ? s_tdPlayer : ((nEnt == 1) ? pEntity : nullptr);
		// Weapon/viewmodel may PNR before the player -- pull ent 1 from the list.
		if (!pPlayer && g_pClientEntityList)
			pPlayer = g_pClientEntityList->GetClientEntity(1);

		float d = 0.f;
		TdReason_t reason = TD_NO_ENTRY;
		if (pPlayer && TimeDomain_ComputeDelta(pPlayer, nCmd, &d, &reason))
		{
			s_tdDelta = d;
			s_tdOk = true;
			s_tdPlayer = pPlayer;
			s_tdOorLatchCleared = false;

			const bool bWasHeld = (s_tdHeldUses != 0);
			s_tdHeldDelta = d;
			s_tdHeldOk = true;
			s_tdHeldCmd = nCmd;
			s_tdHeldUses = 0;

			static volatile LONG s_announced = 0;
			if (InterlockedCompareExchange(&s_announced, 1, 0) == 0)
				Warning(eDLL_T::CLIENT,
					"[TIME-DOMAIN] armed: convert absolute TIME props by d=clientTb-serverTb "
					"(first d=%+.4f cmd=%u)\n", d, nCmd);
			else if (bWasHeld)
				Warning(eDLL_T::CLIENT,
					"[TIME-DOMAIN] recovered at cmd=%u d=%+.4f (rode the held delta for %u cmd)\n",
					nCmd, d, s_tdHeldUses);
		}
		else
		{
			s_tdLastReason = reason;

			// Stale latch shadows from the pre-break domain would suppress re-delivery
			// conversion after recovery; drop them once per OUT_OF_RANGE event.
			if (reason == TD_OUT_OF_RANGE && !s_tdOorLatchCleared)
			{
				s_tdOorLatchCleared = true;
				TimeDomain_ResetLatches();
				Warning(eDLL_T::CLIENT,
					"[TIME-DOMAIN] latches reset -- clock domain broke (d=%+.4f) at cmd=%u sinceFlush=%.0fms sinceFrame=%.0fms\n",
					s_tdLastRejected, nCmd,
					S21BridgeDiag_MsSinceC2SFlush(),
					S21BridgeDiag_MsSinceEngineFrame());
			}

			// A command that cannot compute its own delta is not a command with no
			// skew. Carry the last good one for a bounded window; only when that
			// runs out do stamps ship raw -- and then say so.
			const unsigned nHold = static_cast<unsigned>(bridge_time_domain_hold.GetInt());
			if (s_tdHeldOk && nHold > 0 && (nCmd - s_tdHeldCmd) <= nHold)
			{
				s_tdDelta = s_tdHeldDelta;
				s_tdOk = true;
				if (pPlayer)
					s_tdPlayer = pPlayer;
				++s_tdHeldUses;

				static uint32_t s_nHoldLog = 0;
				if (++s_nHoldLog <= 4 || (s_nHoldLog % 256) == 0)
					Warning(eDLL_T::CLIENT,
						"[TIME-DOMAIN] cmd=%u %s (last_d=%+.4f) -- holding d=%+.4f from cmd=%u (#%u)\n",
						nCmd, TimeDomain_ReasonName(reason), s_tdLastRejected,
						s_tdHeldDelta, s_tdHeldCmd, s_nHoldLog);
			}
			else if (s_tdHeldOk)
			{
				// The window closed. This is the state the old magnitude check
				// entered silently on its very first bad command.
				s_tdHeldOk = false;
				Warning(eDLL_T::CLIENT,
					"[TIME-DOMAIN] DISARMED at cmd=%u after %u held cmd -- %s "
					"(last_d=%+.4f, max=%.4f). Absolute TIME stamps now ship RAW.\n",
					nCmd, s_tdHeldUses, TimeDomain_ReasonName(reason),
					s_tdLastRejected, bridge_time_domain_max_delta.GetFloat());
				s_tdHeldUses = 0;
			}
		}
	}

	if (!s_tdOk)
	{
		// Never armed at all: the convert is inert for the whole run and every
		// [TIME-DOMAIN] row in pred_diff.log is unexplained. Say it once.
		static volatile LONG s_neverArmed = 0;
		static uint32_t s_nSilent = 0;
		if (++s_nSilent >= 512 && InterlockedCompareExchange(&s_neverArmed, 1, 0) == 0)
			Warning(eDLL_T::CLIENT,
				"[TIME-DOMAIN] NEVER ARMED after %u commands -- %s (last_d=%+.4f). "
				"Absolute TIME stamps are shipping RAW for this session.\n",
				s_nSilent, TimeDomain_ReasonName(s_tdLastReason), s_tdLastRejected);
		return;
	}

	// Only local predictables: player + its weapons/viewmodels (all predicted
	// entities that run this path under the local client's PNR). Remote players
	// are not predicted (ENT_PREDICTED_FLAG gate in caller).
	TimeDomain_ConvertEntity(pEntity, nCmd, s_tdDelta);
}

//-----------------------------------------------------------------------------
// [PRED-ANIMEVT] Value proof for the ANIMEVT-CONSUME rows. A slot the client has
// already consumed reads 0 live while the acked command's record still holds the
// pre-fire time; that pair is exactly the double-fire this class now refuses to
// write back. Keyed off the entry POINTER, so it costs one compare per masked slot.
//-----------------------------------------------------------------------------
static void PredAuth_AnimEvtResurrectProbe(const PredAuthEntry_t* pEntry, void* pEntity,
	const uint8_t* pMember, const uint8_t* pRecord, int nSize)
{
	if (!pEntry || pEntry->pszName != kAnimEvtTimesName || !pMember
		|| !sdk_pred_animevt_diag.GetBool())
		return;

	const int nSlots = nSize / 4;
	for (int i = 0; i < nSlots; ++i)
	{
		const float flLive = reinterpret_cast<const float*>(pMember)[i];
		const float flRec  = reinterpret_cast<const float*>(pRecord)[i];
		if (flLive != 0.0f || flRec == 0.0f)
			continue;

		static uint32_t s_nRes = 0;
		if (++s_nRes <= 12 || (s_nRes & 0xFFu) == 0)
			Warning(eDLL_T::CLIENT,
				"[PRED-ANIMEVT] #%u ent=%d slot=%d consumed live=0 record=%.4f -- "
				"resurrection suppressed (would have re-fired next frame)\n",
				s_nRes, EntField<int>(pEntity, ENT_ENTINDEX), i, flRec);
	}
}

//-----------------------------------------------------------------------------
// [DUCK-REM-WIRE] Latch from the decode layer, consumed once. The prop is delta
// encoded, so it only arrives on acks where it changed -- re-installing a stale
// latch every ack would pin the remainder and the duck would never finish.
//-----------------------------------------------------------------------------
static int s_duckRemWireEnt  = -1;
static int s_duckRemWireMsec = 0;

void PredAuth_OnDuckRemainderWire(int nEntIndex, int nMsec)
{
	if (nEntIndex < 0 || nMsec < 0 || nMsec > 60000)
		return;
	s_duckRemWireEnt  = nEntIndex;
	s_duckRemWireMsec = nMsec;
}

void PredAuth_ResetSession(void)
{
	s_duckRemWireEnt  = -1;
	s_duckRemWireMsec = 0;
}

static void DuckRemainder_Apply(void* pEntity)
{
	if (s_duckRemWireEnt < 0 || !bridge_duck_remainder_wire.GetBool())
		return;
	if (EntField<int>(pEntity, ENT_ENTINDEX) != s_duckRemWireEnt)
		return;

	void* (*GetPredDescMap)(void*) = reinterpret_cast<void* (*)(void*)>(
		(*reinterpret_cast<void***>(pEntity))[VIDX_GetPredDescMap]);
	const uintptr_t dmap = reinterpret_cast<uintptr_t>(GetPredDescMap(pEntity));
	if (!dmap)
		return;

	int off0 = -1, off1 = -1;
	if (!TimeDomain_ResolveFlatOff(dmap,
			"m_currentFrameLocalPlayer.m_duckTransitionRemainderMsec", &off0, &off1)
		|| off0 <= 0 || off0 > 0x40000)
		return;

	const int nWire = s_duckRemWireMsec;
	s_duckRemWireEnt = -1;

	int* const pLive = reinterpret_cast<int*>(
		reinterpret_cast<uintptr_t>(pEntity) + off0);
	const int nLive = *pLive;
	*pLive = nWire;

	if (bridge_duck_remainder_diag.GetBool())
	{
		static uint32_t s_nDuck = 0;
		if (++s_nDuck <= 20 || (s_nDuck & 0xFFu) == 0)
			Warning(eDLL_T::CLIENT,
				"[DUCK-REM-WIRE] #%u ent=%d live=%d <- wire=%d\n",
				s_nDuck, EntField<int>(pEntity, ENT_ENTINDEX), nLive, nWire);
	}
}

//-----------------------------------------------------------------------------
// PredAuth_Apply -- see pred_authority.h. Behavior-identical restructuring of
// the former PredDiag_MaskUnfedFields, driven off s_authTable.
//-----------------------------------------------------------------------------
void PredAuth_Apply(void* pEntity, unsigned int nCmd)
{
	static bool s_bValidated = false;
	if (!s_bValidated)
	{
		s_bValidated = true;
		PredAuth_ValidateTable();
	}

	if (!EntField<unsigned char>(pEntity, ENT_PREDICTED_FLAG))
		return;
	void* pPredState = C_BaseEntity__GetPredictedEntityState(pEntity, nCmd);
	if (!pPredState)
		return;
	uint8_t* pPredicted = EntField<uint8_t*>(pPredState, STATE_SERIALIZED_DATA);
	const uintptr_t states = EntField<uintptr_t>(pEntity, ENT_SAVED_STATES);
	if (!pPredicted || !states)
		return;

	// Both of these write live members and must land BEFORE originalData is
	// packed below, or the native compare reads the pre-install value.
	__try {
		DuckRemainder_Apply(pEntity);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		static uint32_t s_nDuckFaults = 0;
		if (++s_nDuckFaults <= 8)
			Warning(eDLL_T::CLIENT, "[DUCK-REM-WIRE] exception #%u -- install skipped this ack\n", s_nDuckFaults);
	}

	// [TIME-DOMAIN] convert live absolute times BEFORE packing originalData so
	// the native compare/restore both see client-domain stamps.
	__try {
		TimeDomain_OnEntity(pEntity, nCmd);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		static uint32_t s_nTdFaults = 0;
		if (++s_nTdFaults <= 8)
			Warning(eDLL_T::CLIENT, "[TIME-DOMAIN] exception #%u -- convert skipped this ack\n", s_nTdFaults);
	}

	// Refresh originalData from the live members FIRST (what the native compare
	// will read); idempotent with the orig's own SaveData(-1) at entry.
	if (PFN_PredSaveData pSave = PredAuth_SaveData())
		pSave(pEntity, "sdkunfedmask", static_cast<int>(nCmd), -1);

	const uintptr_t originalData = states + STATES_ORIGINALDATA;
	if (!*reinterpret_cast<unsigned char*>(originalData)) // originalData.active
		return;
	uint8_t* pServer = *reinterpret_cast<uint8_t**>(originalData + STATE_SERIALIZED_DATA);
	if (!pServer)
		return;

	void* (*GetPredDescMap)(void*) = reinterpret_cast<void* (*)(void*)>(
		(*reinterpret_cast<void***>(pEntity))[VIDX_GetPredDescMap]);
	const uintptr_t dmap = reinterpret_cast<uintptr_t>(GetPredDescMap(pEntity));
	if (!dmap)
		return;

	AuthCache_t* pc = nullptr;
	for (auto& c : s_cache) { if (c.dmap == dmap) { pc = &c; break; } }
	if (!pc)
	{
		for (auto& c : s_cache) { if (!c.dmap) { pc = &c; break; } }
		if (!pc)
		{
			Warning(eDLL_T::CLIENT,
				"[PRED-UNFED-MISS] dmap cache FULL (%zu slots) -- dmap=0x%p will never be "
				"masked; bump s_cache size\n", sizeof(s_cache) / sizeof(s_cache[0]),
				reinterpret_cast<void*>(dmap));
			return; // cache full -- leave this dmap unmasked
		}
		pc->dmap = dmap; pc->kind = AUTHKIND_ANY; pc->n = 0;
		pc->tbOff = -1; pc->tbMOff = -1; pc->cdN = 0; pc->bmN = 0; pc->satN = 0; pc->stampN = 0;
		pc->orgOff = -1;
		int nTol = 0;
		bool matched[kAuthTableCount] = {};

		const uintptr_t opt = *reinterpret_cast<uintptr_t*>(dmap + DMAP_OPTIMIZED);
		if (opt)
		{
			const uintptr_t info   = opt + (uintptr_t)OPT_INFO_STRIDE * PC_NETWORKED_ONLY;
			const uintptr_t fields = *reinterpret_cast<uintptr_t*>(info + INFO_FLAT_FIELDS);
			const int count        = *reinterpret_cast<int*>(info + INFO_FLAT_COUNT);
			if (fields && count > 0 && count <= 4096)
			{
				// (a) KIND DETECTION pre-pass -- first match wins in priority order
				// PLAYER > WEAPON > VIEWMODEL, default ANY-only.
				for (int i = 0; i < count && pc->kind == AUTHKIND_ANY; ++i)
				{
					const uintptr_t td = fields + (uintptr_t)i * TD_STRIDE;
					const char* name = *reinterpret_cast<const char**>(td + TD_FIELDNAME);
					if (!name) continue;
					if (!strcmp(name, "m_currentFramePlayer.timeBase"))      pc->kind = AUTHKIND_PLAYER;
					else if (!strcmp(name, "m_ammoInClip"))                  pc->kind = AUTHKIND_WEAPON;
					else if (!strcmp(name, "m_viewModelOwner"))              pc->kind = AUTHKIND_VIEWMODEL;
				}

				// (a2) [PRED-FIELDS] the DENOMINATOR. One line per flattened networked
				// predicted field, in the flat order the native check walks -- so the
				// offline tooling can answer "is this field error-checked at all",
				// "what type and tolerance does it carry" and "which one comes first"
				// without another live run.
				if (sdk_pred_field_dump.GetBool())
				{
					Warning(eDLL_T::CLIENT,
						"[PRED-FIELDS] BEGIN kind=%s count=%d tick=%.4f dmap=0x%p\n",
						AuthKindName(pc->kind), count, PredNative_TickInterval(),
						reinterpret_cast<void*>(dmap));
					for (int i = 0; i < count; ++i)
					{
						const uintptr_t td = fields + (uintptr_t)i * TD_STRIDE;
						const char* name = *reinterpret_cast<const char**>(td + TD_FIELDNAME);
						const int type    = *reinterpret_cast<int*>(td + TD_FIELDTYPE);
						const int n16     = *reinterpret_cast<uint16_t*>(td + TD_FIELDSIZE);
						const uint32_t fl = *reinterpret_cast<uint32_t*>(td + TD_FLAGS);
						const float tol   = *reinterpret_cast<float*>(td + TD_TOLERANCE);
						Warning(eDLL_T::CLIENT,
							"[PRED-FIELDS] %s %3d %-52s type=%2d flags=0x%04X tol=%g effTol=%g "
							"cnt=%d off0=%d off1=%d%s\n",
							AuthKindName(pc->kind), i, name ? name : "<noname>", type, fl,
							tol, PredNative_Tolerance(type, tol), n16 ? n16 : 1,
							*reinterpret_cast<int*>(td + TD_FLATOFFSET0),
							*reinterpret_cast<int*>(td + TD_FLATOFFSET1),
							(fl & (FTYPEDESC_SKIP | FTYPEDESC_GRAPPLE)) ? " SKIPPED" : "");
					}
					Warning(eDLL_T::CLIENT, "[PRED-FIELDS] END kind=%s\n", AuthKindName(pc->kind));
				}

				// (b) resolution pass -- bind entries whose scope is ANY or matches kind.
				for (int i = 0; i < count; ++i)
				{
					const uintptr_t td = fields + (uintptr_t)i * TD_STRIDE;
					const char* name = *reinterpret_cast<const char**>(td + TD_FIELDNAME);
					if (!name)
						continue;

					const int type = *reinterpret_cast<int*>(td + TD_FIELDTYPE);
					const int n16  = *reinterpret_cast<uint16_t*>(td + TD_FIELDSIZE);
					const int cnt  = n16 ? n16 : 1;

					if (pc->orgOff < 0 && !strcmp(name, "m_localOrigin"))
						pc->orgOff = *reinterpret_cast<int*>(td + TD_FLATOFFSET1);

					for (int e = 0; e < kAuthTableCount; ++e)
					{
						if (s_authDead[e] || strcmp(name, s_authTable[e].pszName) != 0)
							continue;
						// NO break: a name can carry entries in TWO classes that BOTH bind
						// (m_zoomBaseTime / m_zoomFullStartTime are DOMAIN_TRANSPLANT *and*
						// TOLERANCE via sdk_pred_tol_time -- the tolerance write and the
						// relative-time transplant apply independently for the same td).
						// Each class binds a distinct mechanism, so multiple matches are safe.
						PredAuth_BindOneEntry(pc, e, td, name, type, cnt, nTol, matched, dmap);
					}
				}

				// Leaf-suffix fallback: table entry "a.b.c" binds to the sole flat
				// field whose text after the last '.' equals "c". Ambiguous leaves
				// stay unmatched and keep reporting [PRED-UNFED-MISS].
				for (int e = 0; e < kAuthTableCount; ++e)
				{
					if (matched[e] || s_authDead[e])
						continue;
					if (s_authTable[e].eClass == PredAuthClass_t::SERVER_CONTENT)
						continue;
					if (!AuthScopeMatches(s_authTable[e].nScope, pc->kind))
						continue;

					const char* const pszEntLeaf = AuthLeafName(s_authTable[e].pszName);
					int nHits = 0;
					int nHitIdx = -1;
					for (int i = 0; i < count; ++i)
					{
						const uintptr_t tdFlat = fields + (uintptr_t)i * TD_STRIDE;
						const char* pszFlat = *reinterpret_cast<const char**>(tdFlat + TD_FIELDNAME);
						if (!pszFlat)
							continue;
						if (strcmp(pszEntLeaf, AuthLeafName(pszFlat)) != 0)
							continue;
						++nHits;
						nHitIdx = i;
					}
					if (nHits != 1)
						continue;

					const uintptr_t td = fields + (uintptr_t)nHitIdx * TD_STRIDE;
					const char* name = *reinterpret_cast<const char**>(td + TD_FIELDNAME);
					const int type = *reinterpret_cast<int*>(td + TD_FIELDTYPE);
					const int n16  = *reinterpret_cast<uint16_t*>(td + TD_FIELDSIZE);
					const int cnt  = n16 ? n16 : 1;
					PredAuth_BindOneEntry(pc, e, td, name, type, cnt, nTol, matched, dmap);
					if (matched[e])
					{
						Warning(eDLL_T::CLIENT,
							"[PRED-AUTH] '%s' bound via leaf-name fallback to flat field '%s' on kind=%s\n",
							s_authTable[e].pszName, name ? name : "<noname>", AuthKindName(pc->kind));
					}
				}
			}
		}

		if (pc->n > 0 || nTol > 0)
			Warning(eDLL_T::CLIENT,
				"[PRED-AUTH] FIRST RESOLVE kind=%s -- masking %d never-fed predicted "
				"fields + tolerance on %d drift fields (origin %.2f, velocity %.2f, fallV %.2f) on dmap=0x%p\n",
				AuthKindName(pc->kind), pc->n, nTol, sdk_pred_tol_origin.GetFloat(),
				sdk_pred_tol_velocity.GetFloat(), sdk_pred_tol_fallvelocity.GetFloat(),
				reinterpret_cast<void*>(dmap));

		// Only report MISS on dmaps that actually bound >=1 entry. Per entry, report
		// only when (a) the entry's scope is a SPECIFIC kind that equals this dmap's
		// kind, or (b) the entry is ANY-scoped AND this is the PLAYER dmap -- the
		// historical home of every ANY-scope entry. Without (b)'s player restriction,
		// the ~40 ANY-scoped player-only names would print a MISS line per WEAPON /
		// VIEWMODEL dmap (which now bind deploy entries, so pc->n > 0 no longer
		// implies "player") = the exact log-spam class the old `pc->n > 0` guard
		// existed to prevent.
		if (pc->n > 0)
		{
			for (int e = 0; e < kAuthTableCount; ++e)
			{
				if (matched[e] || s_authDead[e])
					continue;
				if (s_authTable[e].eClass == PredAuthClass_t::SERVER_CONTENT)
					continue;
				const int nScope = s_authTable[e].nScope;
				const bool bScopedHit = (nScope != AUTH_SCOPE_ANY) && AuthScopeMatches(nScope, pc->kind);
				const bool bAnyOnPlayer = (nScope == AUTH_SCOPE_ANY) && pc->kind == AUTHKIND_PLAYER;
				if (!bScopedHit && !bAnyOnPlayer)
					continue;
			}
		}
	}

	// [PRED-REPAIR] self-check, runs ONCE on the player dmap only.
	// Compare live member @ flatOffset[0] vs originalData packed @ flatOffset[1]
	// after SaveData(-1). Live timeBase vs record[ack] are different states
	// (current predicted members vs the acked cmd snapshot) and must not be
	// the pair. SaveData just packed members into originalData, so equal
	// values prove the offset twin.
	if (sdk_pred_baseline_repair.GetBool() && s_predRepairState == 0 && pc->tbOff >= 0 && pc->tbMOff >= 0)
	{
		const float tbMember = *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(pEntity) + pc->tbMOff);
		const float tbOrig   = *reinterpret_cast<const float*>(pServer + pc->tbOff);
		if (tbMember > 0.0f && tbOrig > 0.0f && fabsf(tbMember - tbOrig) < 0.5f)
		{
			s_predRepairState = 1;
			Warning(eDLL_T::CLIENT, "[PRED-REPAIR] armed: member-offset self-check PASSED "
				"(tb member@+%d=%.4f vs originalData@+%d=%.4f) -- baseline repair live\n",
				pc->tbMOff, tbMember, pc->tbOff, tbOrig);
		}
		else
		{
			// Fallback: some S21 fields share flatOffset[0]==flatOffset[1] (member
			// lives at the packed index). If entity+tbOff matches originalData+tbOff
			// after SaveData, use flatOffset[1] as the member path for timeBase and
			// still arm repair (clock-derived cdMOff already resolved per-field).
			const float tbMemberAlt = *reinterpret_cast<const float*>(
				reinterpret_cast<const uint8_t*>(pEntity) + pc->tbOff);
			if (tbMemberAlt > 0.0f && tbOrig > 0.0f && fabsf(tbMemberAlt - tbOrig) < 0.5f)
			{
				pc->tbMOff = pc->tbOff;
				s_predRepairState = 1;
				Warning(eDLL_T::CLIENT, "[PRED-REPAIR] armed via flatOffset[1] alias "
					"(tb entity@+%d=%.4f vs originalData@+%d=%.4f) -- baseline repair live\n",
					pc->tbOff, tbMemberAlt, pc->tbOff, tbOrig);
			}
			else
			{
				s_predRepairState = -1;
				Warning(eDLL_T::CLIENT, "[PRED-REPAIR] SELF-CHECK FAILED (tb member@+%d=%.4f "
					"orig@+%d=%.4f altMember@+%d=%.4f) -- flatOffset twins not coherent; "
					"falling back to legacy compare-only masking\n",
					pc->tbMOff, tbMember, pc->tbOff, tbOrig, pc->tbOff, tbMemberAlt);
			}
		}
	}
	const bool bRepair = sdk_pred_baseline_repair.GetBool() && s_predRepairState == 1;

	// Script SetOrigin is not predicted. The ack that lands the dest still carries
	// the client's jump/duck timers (unfed, client-owned). Skip that mask so those
	// fields fail natively and restore to the server zeros at the dest. Do NOT copy
	// origin here -- the native errorcheck must still see the teleport so it replays
	// the predicted tail from the new place.
	bool bTeleport = false;
	if (sdk_pred_teleport_adopt.GetBool() && pc->kind == AUTHKIND_PLAYER && pc->orgOff >= 0)
	{
		const float flThresh = sdk_pred_teleport_dist.GetFloat();
		if (flThresh > 0.0f)
		{
			const float* const pPredOrg = reinterpret_cast<const float*>(pPredicted + pc->orgOff);
			const float* const pSrvOrg  = reinterpret_cast<const float*>(pServer + pc->orgOff);
			const float dx = pPredOrg[0] - pSrvOrg[0];
			const float dy = pPredOrg[1] - pSrvOrg[1];
			const float dz = pPredOrg[2] - pSrvOrg[2];
			const float flDist2 = dx * dx + dy * dy + dz * dz;
			if (flDist2 >= flThresh * flThresh)
			{
				bTeleport = true;
				static uint32_t s_nTeleportAdopt = 0;
				if (++s_nTeleportAdopt <= 16 || (s_nTeleportAdopt & 0x3Fu) == 0)
				{
					Warning(eDLL_T::CLIENT,
						"[PRED-TELEPORT-ADOPT] #%u cmd=%u dOrg=%.1f "
						"(%.1f %.1f %.1f)->(%.1f %.1f %.1f) -- skipped unfed\n",
						s_nTeleportAdopt, nCmd, sqrtf(flDist2),
						pPredOrg[0], pPredOrg[1], pPredOrg[2],
						pSrvOrg[0], pSrvOrg[1], pSrvOrg[2]);
				}
			}
		}
	}

	// record[ack].field:= fresh member value -> the native compare sees equality.
	if (sdk_pred_unfed_mask.GetBool() && !bTeleport)
		for (int u = 0; u < pc->n; ++u)
		{
			// Entries with a gate ConVar (the deploy family) are inert unless it's true.
			if (pc->entry[u] && pc->entry[u]->pGateCvar && !pc->entry[u]->pGateCvar->GetBool())
				continue;

			// [PRED-REPAIR] repair the LIVE member (replay baseline) with the client's own
			// predicted value; the native's entry SaveData(-1) then propagates it into
			// originalData so the compare passes too. Legacy path (repair off/failed or
			// slot ineligible/unresolved): equalize the compare buffers only.
			if (bRepair && pc->mEligible[u] && pc->mOff[u] >= 0)
				memcpy(reinterpret_cast<uint8_t*>(pEntity) + pc->mOff[u], pPredicted + pc->off[u], pc->size[u]);
			else
			{
				if (pc->mOff[u] >= 0)
					PredAuth_AnimEvtResurrectProbe(pc->entry[u], pEntity,
						reinterpret_cast<const uint8_t*>(pEntity) + pc->mOff[u],
						pPredicted + pc->off[u], pc->size[u]);
				memcpy(pPredicted + pc->off[u], pServer + pc->off[u], pc->size[u]);
			}
		}

	// [PRED-SAT] saturating-wire compare. The server value is pinned at the prop's
	// ceiling while the client predicts the true (larger) number, so the pair carries
	// no disagreement to reconcile -- equalise the compare buffers and let the rest of
	// the ack stand. Only the record is touched; the live predicted member keeps the
	// client's own value, which is the accurate one.
	if (sdk_pred_saturate_mask.GetBool())
	{
		for (int s = 0; s < pc->satN; ++s)
		{
			const PredAuthEntry_t* ent = pc->satEntry[s];
			if (!ent || !ent->pTolCvar)
				continue;
			if (ent->pGateCvar && !ent->pGateCvar->GetBool())
				continue;

			const float flCap = ent->pTolCvar->GetFloat();
			if (flCap <= 0.0f)
				continue;

			const float flSrv  = *reinterpret_cast<const float*>(pServer + pc->satOff[s]);
			const float flPred = *reinterpret_cast<const float*>(pPredicted + pc->satOff[s]);
			if (flSrv < flCap || flPred < flSrv)
				continue;   // below the ceiling, or client under the server: a real compare

			memcpy(pPredicted + pc->satOff[s], pServer + pc->satOff[s], 4);

			static uint32_t s_nSat = 0;
			if (++s_nSat <= 4 || (s_nSat & 0xFFu) == 0)
				Warning(eDLL_T::CLIENT,
					"[PRED-SAT] '%s' saturated #%u: srv=%.1f (cap %.1f) pred=%.1f -- compare equalised\n",
					ent->pszName, s_nSat, flSrv, flCap, flPred);
		}
	}

	// [PRED-STAMP-RESET] adopt a server-side clear, bulk rewrite or clock-correction
	// shift of the time-stamp family before the native compare. A cleared stamp
	// (<= 0) never carries movement content; a bulk rewrite is 3+ diverging stamps
	// that read one server value; a shift is 3+ diverging stamps (once-written
	// stamps plus the clock-derived gates) that differ by one delta -- the server
	// moves every live TIME field by the correction in one write and the client
	// copies stay put.
	if (sdk_pred_stamp_reset_adopt.GetBool() && pc->kind == AUTHKIND_PLAYER
		&& (pc->stampN > 0 || pc->cdN > 0))
	{
		const float flTol = sdk_pred_tol_time.GetFloat();
		// A gate both engines already hold well in the past carries no content
		// whichever exact stamp it reads; equalise it instead of rebasing on it.
		float flExpSrv = 0.0f, flExpPred = 0.0f;
		if (pc->tbOff >= 0)
		{
			flExpSrv  = *reinterpret_cast<const float*>(pServer + pc->tbOff) - 1.0f;
			flExpPred = *reinterpret_cast<const float*>(pPredicted + pc->tbOff) - 1.0f;
		}
		enum { kShiftMax = kStampSlotMax + kClockDrvSlotMax };
		int nCand = 0;
		int candOff[kShiftMax], candMOff[kShiftMax];
		const PredAuthEntry_t* candEnt[kShiftMax];
		bool candStamp[kShiftMax];
		for (int s = 0; s < pc->stampN && nCand < kShiftMax; ++s)
		{
			candOff[nCand] = pc->stampOff[s]; candMOff[nCand] = pc->stampMOff[s];
			candEnt[nCand] = pc->stampEntry[s]; candStamp[nCand] = true; ++nCand;
		}
		for (int c = 0; c < pc->cdN && nCand < kShiftMax; ++c)
		{
			if (pc->cdEntry[c] && pc->cdEntry[c]->pGateCvar && !pc->cdEntry[c]->pGateCvar->GetBool())
				continue;
			bool bDup = false;
			for (int k = 0; k < nCand; ++k)
				if (candOff[k] == pc->cdOff[c]) { bDup = true; break; }
			if (bDup)
				continue;
			candOff[nCand] = pc->cdOff[c]; candMOff[nCand] = pc->cdMOff[c];
			candEnt[nCand] = pc->cdEntry[c]; candStamp[nCand] = false; ++nCand;
		}

		int nDiverge = 0, nCleared = 0, nSameValue = 0, nSameDelta = 0;
		float flFirstSrv = 0.0f, flFirstDelta = 0.0f;
		bool bHaveFirst = false, bHaveDelta = false;
		for (int k = 0; k < nCand; ++k)
		{
			const float fs = *reinterpret_cast<const float*>(pServer + candOff[k]);
			const float fp = *reinterpret_cast<const float*>(pPredicted + candOff[k]);
			if (fabsf(fs - fp) <= flTol)
				continue;
			if (candStamp[k])
			{
				++nDiverge;
				if (fs <= 0.0f && fp > 0.0f)
					++nCleared;
				if (!bHaveFirst) { flFirstSrv = fs; bHaveFirst = true; ++nSameValue; }
				else if (fs == flFirstSrv) ++nSameValue;
			}
			if (fp <= 1.0f || fs <= 1.0f)
				continue;
			const float fd = fs - fp;
			if (!bHaveDelta) { flFirstDelta = fd; bHaveDelta = true; ++nSameDelta; }
			else if (fabsf(fd - flFirstDelta) <= 0.005f) ++nSameDelta;
		}
		const bool bShift = (nSameDelta >= 3);
		const bool bBulk = (nDiverge >= 3 && nSameValue == nDiverge);
		if (nCleared > 0 || bBulk || bShift || (flExpSrv > 1.0f && flExpPred > 1.0f))
		{
			static uint32_t s_nStampAdopt = 0;
			const bool bLog = (++s_nStampAdopt <= 16 || (s_nStampAdopt & 0xFFu) == 0);
			for (int k = 0; k < nCand; ++k)
			{
				const float fs = *reinterpret_cast<const float*>(pServer + candOff[k]);
				const float fp = *reinterpret_cast<const float*>(pPredicted + candOff[k]);
				if (fabsf(fs - fp) <= flTol)
					continue;
				const bool bThisShift = bShift && fp > 1.0f && fs > 1.0f
					&& fabsf((fs - fp) - flFirstDelta) <= 0.005f;
				const bool bThisClear = candStamp[k] && fs <= 0.0f && fp > 0.0f;
				const bool bThisBulk  = candStamp[k] && bBulk && fs == flFirstSrv;
				const bool bThisExpired = !candStamp[k] && flExpSrv > 1.0f && flExpPred > 1.0f
					&& fs > 1.0f && fp > 1.0f && fs < flExpSrv && fp < flExpPred;
				if (!(bThisShift || bThisClear || bThisBulk || bThisExpired))
					continue;
				memcpy(pPredicted + candOff[k], pServer + candOff[k], 4);
				if (bRepair && candMOff[k] >= 0)
					memcpy(reinterpret_cast<uint8_t*>(pEntity) + candMOff[k], pServer + candOff[k], 4);
				if (bLog)
					Warning(eDLL_T::CLIENT,
						"[PRED-STAMP-RESET] #%u '%s' srv=%.3f pred=%.3f adopted (%s, n=%d, d=%+.4f)\n",
						s_nStampAdopt, candEnt[k] ? candEnt[k]->pszName : "?",
						fs, fp, bThisShift ? "shift" : (bThisBulk ? "bulk" : (bThisClear ? "cleared" : "expired")),
						bThisShift ? nSameDelta : nDiverge, bThisShift ? flFirstDelta : 0.0f);
			}
		}
	}

	// [PRED-CLOCK-REBASE] + [WEAP-CLOCK] relative-time compare for clock-derived
	// gate times. Player dmaps carry timeBase (tbOff). Weapon dmaps do not -- borrow
	// the local player's timeBase pair so fire/kick/idle stamps can still compare
	// in relative time after D1 domain convert. Copy direction: server -> record
	// when relative matches (native compare sees equality). Optional live transplant
	// keeps client relative offset across foreign-family rebases.
	if (sdk_pred_clock_rebase.GetBool() && (pc->tbOff >= 0 || pc->cdN > 0))
	{
		float tbSrv = 0.0f;
		float tbPred = 0.0f;
		bool bHaveTb = false;
		bool bBorrowedPlayerTb = false;

		if (pc->tbOff >= 0)
		{
			tbSrv  = *reinterpret_cast<const float*>(pServer + pc->tbOff);
			tbPred = *reinterpret_cast<const float*>(pPredicted + pc->tbOff);
			bHaveTb = (tbSrv > 0.0f && tbPred > 0.0f);
		}
		else if (pc->kind == AUTHKIND_WEAPON && sdk_pred_weapon_clock.GetBool()
			&& C_BaseEntity__GetPredictedEntityState)
		{
			// Weapon PNR may run before or after player; pull ent 1's packed states.
			void* pPlayer = nullptr;
			if (g_pClientEntityList)
				pPlayer = g_pClientEntityList->GetClientEntity(1);
			if (!pPlayer && s_tdPlayer)
				pPlayer = s_tdPlayer;

			if (pPlayer && EntField<unsigned char>(pPlayer, ENT_PREDICTED_FLAG))
			{
				void* pPlPredState = C_BaseEntity__GetPredictedEntityState(pPlayer, nCmd);
				const uintptr_t plStates = EntField<uintptr_t>(pPlayer, ENT_SAVED_STATES);
				if (pPlPredState && plStates)
				{
					const uint8_t* pPlPred = EntField<uint8_t*>(pPlPredState, STATE_SERIALIZED_DATA);
					const uintptr_t plOrig = plStates + STATES_ORIGINALDATA;
					const uint8_t* pPlSrv = nullptr;
					if (*reinterpret_cast<const unsigned char*>(plOrig))
						pPlSrv = *reinterpret_cast<uint8_t* const*>(plOrig + STATE_SERIALIZED_DATA);

					void* (*GetPlPredDescMap)(void*) = reinterpret_cast<void* (*)(void*)>(
						(*reinterpret_cast<void***>(pPlayer))[VIDX_GetPredDescMap]);
					const uintptr_t plDmap = reinterpret_cast<uintptr_t>(GetPlPredDescMap(pPlayer));
					int off0 = -1, off1 = -1;
					if (pPlPred && plDmap
						&& TimeDomain_ResolveFlatOff(plDmap, "m_currentFramePlayer.timeBase", &off0, &off1)
						&& off1 >= 0)
					{
						// Prefer packed originalData; fall back to live member (off0) after
						// TimeDomain convert so weapon PNR still has a reference clock.
						if (pPlSrv)
							tbSrv = *reinterpret_cast<const float*>(pPlSrv + off1);
						if (tbSrv <= 0.0f && off0 >= 0)
							tbSrv = *reinterpret_cast<const float*>(
								reinterpret_cast<const uint8_t*>(pPlayer) + off0);
						tbPred = *reinterpret_cast<const float*>(pPlPred + off1);
						if (tbPred <= 0.0f && off0 >= 0)
							tbPred = *reinterpret_cast<const float*>(
								reinterpret_cast<const uint8_t*>(pPlayer) + off0);
						bHaveTb = (tbSrv > 0.0f && tbPred > 0.0f);
						bBorrowedPlayerTb = bHaveTb;
					}
				}
			}
			// Last resort: per-cmd TimeDomain delta already computed against player.
			if (!bHaveTb && s_tdOk && s_tdPlayer)
			{
				float dTd = 0.f;
				if (TimeDomain_ComputeDelta(s_tdPlayer, nCmd, &dTd, nullptr))
				{
					// Reconstruct a reference pair around client tb from live player.
					int off0 = -1, off1 = -1;
					void* (*GetPlPredDescMap)(void*) = reinterpret_cast<void* (*)(void*)>(
						(*reinterpret_cast<void***>(s_tdPlayer))[VIDX_GetPredDescMap]);
					const uintptr_t plDmap = reinterpret_cast<uintptr_t>(GetPlPredDescMap(s_tdPlayer));
					if (plDmap && TimeDomain_ResolveFlatOff(plDmap, "m_currentFramePlayer.timeBase", &off0, &off1)
						&& off0 >= 0)
					{
						tbPred = *reinterpret_cast<const float*>(
							reinterpret_cast<const uint8_t*>(s_tdPlayer) + off0);
						tbSrv = tbPred - dTd;
						bHaveTb = (tbSrv > 0.0f && tbPred > 0.0f);
						bBorrowedPlayerTb = bHaveTb;
					}
				}
			}
			static volatile LONG s_weapTbFail = 0;
			if (!bHaveTb && pc->cdN > 0 && InterlockedCompareExchange(&s_weapTbFail, 1, 0) == 0)
				Warning(eDLL_T::CLIENT,
					"[WEAP-CLOCK] weapon dmap has %d transplant slots but player timeBase "
					"borrow FAILED (entlist=%p s_tdPlayer=%p) -- absolute compare remains\n",
					pc->cdN, (void*)g_pClientEntityList, s_tdPlayer);
		}

		const float d = tbPred - tbSrv;
		if (bHaveTb && fabsf(d) < 0.25f)   // sanity: sawtooth only, not a clock reset
		{
			static uint32_t s_nCrWin = 0, s_nCrMasked = 0, s_nCrKept = 0, s_nCrUnset = 0;
			// timeBase itself: the reference has no residual content across domains.
			if (pc->tbOff >= 0)
				memcpy(pPredicted + pc->tbOff, pServer + pc->tbOff, 4);

			const float flEps = (pc->kind == AUTHKIND_WEAPON)
				? sdk_pred_weapon_time_eps.GetFloat()
				: 0.02f;

			for (int cd = 0; cd < pc->cdN; ++cd)
			{
				const PredAuthEntry_t* pCdEnt = pc->cdEntry[cd];
				if (pCdEnt && pCdEnt->pGateCvar && !pCdEnt->pGateCvar->GetBool())
					continue;

				const float fs = *reinterpret_cast<const float*>(pServer + pc->cdOff[cd]);
				const float fp = *reinterpret_cast<const float*>(pPredicted + pc->cdOff[cd]);

				// One-sided unset stamps: pred unset vs srv live, OR inverted
				// (srv=0 pred=live -- common for m_attackTimeThisFrame on heat fire).
				// Equalize compare only -- never live-repair a hard 0 onto a live clock.
				const bool bUnsetPred = (fp <= 1.0f) && (fs > 1.0f);
				const bool bUnsetSrv  = (fs <= 1.0f) && (fp > 1.0f);
				const bool bIsAttackTime = pCdEnt && pCdEnt->pszName
					&& !strcmp(pCdEnt->pszName, "m_attackTimeThisFrame");
				const bool bAttackBipolar = sdk_pred_attacktime_cmp_only.GetBool()
					&& bIsAttackTime && (bUnsetPred || bUnsetSrv);
				const bool bAnyUnsetNeutralize = bUnsetPred && (pc->kind == AUTHKIND_WEAPON)
					&& sdk_pred_weapon_clock.GetBool() && !bIsAttackTime;

				if (bAttackBipolar || bAnyUnsetNeutralize)
				{
					// Prefer the live pole for the compare buffer so restore (if any
					// other field errs) does not force a hard-zero stamp from the unset side.
					if (bUnsetSrv && bIsAttackTime)
						memcpy(pServer + pc->cdOff[cd], pPredicted + pc->cdOff[cd], 4);
					else
						memcpy(pPredicted + pc->cdOff[cd], pServer + pc->cdOff[cd], 4);
					++s_nCrUnset;
					++s_nCrMasked;
					continue;
				}

				// Only real timestamps. Relative match (preferred) OR absolute residual
				// within weapon tol (RTT-class lag after D1 convert) equalizes compare.
				const float flAbs = fabsf(fp - fs);
				const float flRel = fabsf((fp - tbPred) - (fs - tbSrv));
				const float flAbsTol = (pc->kind == AUTHKIND_WEAPON)
					? sdk_pred_tol_weapon_time.GetFloat()
					: 0.0f;
				const bool bRelOk = (fs > 1.0f && fp > 1.0f && flRel < flEps);
				const bool bAbsOk = (pc->kind == AUTHKIND_WEAPON && fs > 1.0f && fp > 1.0f
					&& flAbsTol > 0.0f && flAbs <= flAbsTol);
				if (bRelOk || bAbsOk)
				{
					memcpy(pPredicted + pc->cdOff[cd], pServer + pc->cdOff[cd], 4);
					// Live transplant only when this dmap owns timeBase (player). On
					// borrowed weapon clocks, equalizing the compare buffers is enough --
					// transplanting onto a foreign tb pair can fight D1 convert.
					if (bRelOk && bRepair && pc->cdMOff[cd] >= 0 && pc->tbOff >= 0 && !bBorrowedPlayerTb)
					{
						const float flTransplant = tbSrv + (fp - tbPred);
						memcpy(reinterpret_cast<uint8_t*>(pEntity) + pc->cdMOff[cd], &flTransplant, 4);
						memcpy(pPredicted + pc->cdOff[cd], &flTransplant, 4);
					}
					++s_nCrMasked;
				}
				else if (fs != fp)
					++s_nCrKept;
			}

			static volatile LONG s_crAnnounced = 0;
			static volatile LONG s_weapCrAnnounced = 0;
			if (pc->kind == AUTHKIND_WEAPON)
			{
				if (InterlockedCompareExchange(&s_weapCrAnnounced, 1, 0) == 0)
					Warning(eDLL_T::CLIENT,
						"[WEAP-CLOCK] armed: borrowed player tb d=%.4f + %d weapon stamps "
						"on dmap=0x%p (eps=%.3f absTol=%.3f)\n",
						d, pc->cdN, reinterpret_cast<void*>(dmap),
						sdk_pred_weapon_time_eps.GetFloat(),
						sdk_pred_tol_weapon_time.GetFloat());
			}
			else
				InterlockedCompareExchange(&s_crAnnounced, 1, 0);
			if (++s_nCrWin >= 2048)
			{
				s_nCrWin = 0; s_nCrMasked = 0; s_nCrKept = 0; s_nCrUnset = 0;
			}
		}
	}

	// [WEAP-FIRE-LAG] + [KICK-ROW-LAG] -- integer/bool fire-step lag and float
	// kick-row bases under RTT. Compare-only; live member keeps wire value.
	// Kick bases: |d|<=N (unit steps of valuePerShot, typically 1.0 per fire).
	if (pc->kind == AUTHKIND_WEAPON
		&& (sdk_pred_weapon_fire_lag.GetBool() || sdk_pred_kick_row_lag.GetBool()))
	{
		const int nLag = sdk_pred_weapon_fire_lag_n.GetInt();
		const float flKickLag = sdk_pred_kick_row_lag_n.GetFloat();
		const uintptr_t opt = *reinterpret_cast<uintptr_t*>(dmap + DMAP_OPTIMIZED);
		if (opt)
		{
			const uintptr_t info   = opt + (uintptr_t)OPT_INFO_STRIDE * PC_NETWORKED_ONLY;
			const uintptr_t fields = *reinterpret_cast<uintptr_t*>(info + INFO_FLAT_FIELDS);
			const int count        = *reinterpret_cast<int*>(info + INFO_FLAT_COUNT);
			static const char* const kLagNames[] = {
				"m_ammoInClip",
				"m_ammoInStockpile",
				"m_shotCount",
				"m_shotIndexForSpread",
				"m_semiAutoTriggerDown",
				"m_pendingTriggerPull",
				"m_burstFireIndex",
			};
			static const char* const kKickNames[] = {
				"m_kickPatternScaleBase",
				"m_kickScaleBasePitch",
				"m_kickScaleBaseYaw",
				"m_kickSpreadHipfire",
			};
			if (fields && count > 0 && count <= 4096)
			{
				static uint32_t s_nFireLag = 0;
				static uint32_t s_nKickLag = 0;
				static float s_armedKickLag = -1.0f;
				// Re-arm when N changes so N=1 vs N=2 A/B is loud in the same session.
				if (sdk_pred_kick_row_lag.GetBool() && flKickLag > 0.0f
					&& s_armedKickLag != flKickLag)
				{
					s_armedKickLag = flKickLag;
					Warning(eDLL_T::CLIENT,
						"[KICK-ROW-LAG] armed: bases |d|<=%.2f hipfire |d|<=%.2f "
						"(compare-only; hard/multi-step miss still exact)\n",
						flKickLag, flKickLag * 2.5f);
				}
				for (int i = 0; i < count; ++i)
				{
					const uintptr_t td = fields + (uintptr_t)i * TD_STRIDE;
					const char* name = *reinterpret_cast<const char**>(td + TD_FIELDNAME);
					if (!name)
						continue;
					const int type = *reinterpret_cast<int*>(td + TD_FIELDTYPE);
					const int off  = *reinterpret_cast<int*>(td + TD_FLATOFFSET1);
					if (off < 0)
						continue;

					if (sdk_pred_kick_row_lag.GetBool() && flKickLag > 0.0f
						&& (type == FIELD_FLOAT || type == FIELD_TIME))
					{
						bool bKick = false;
						bool bHipfire = false;
						for (const char* k : kKickNames)
						{
							if (!strcmp(name, k))
							{
								bKick = true;
								bHipfire = !strcmp(name, "m_kickSpreadHipfire");
								break;
							}
						}
						if (bKick)
						{
							const float fs = *reinterpret_cast<const float*>(pServer + off);
							const float fp = *reinterpret_cast<const float*>(pPredicted + off);
							const float d  = fp - fs;
							const float ad = fabsf(d);
							// Bases: unit=1 (valuePerShot). Hipfire: live unit ~2.4
							// (42ab rev srv=-2.4 pred=0). Bound = N*unit; over stays exact.
							const float unit = bHipfire ? 2.5f : 1.0f;
							const float thresh = flKickLag * unit;
							const bool bEq = (ad > 0.0001f && ad <= thresh + 0.05f);
							if (ad > 0.0001f)
								PredAuth_KickRowHistNote(d, ad, flKickLag, bEq);
							if (bEq)
							{
								memcpy(pPredicted + off, pServer + off, 4);
								if (++s_nKickLag <= 12 || (s_nKickLag & 0xFFu) == 0)
									Warning(eDLL_T::CLIENT,
										"[KICK-ROW-LAG] '%s' |d|=%.3f unit=%.1f thresh=%.2f -- "
										"compare equalised #%u ackCount=%d\n",
										name, ad, unit, thresh, s_nKickLag, s_pnrAckCount);
							}
							else if (ad > thresh + 0.05f
								&& (bridge_kick_row_tap.GetBool() || sdk_pred_ent_census.GetBool()))
							{
								// Residual multi-step / hard / reverse -- sample with class+ackCount.
								static uint32_t s_nKickRes = 0;
								if (++s_nKickRes <= 200 || (s_nKickRes & 0x3Fu) == 0)
									Warning(eDLL_T::CLIENT,
										"[KICK-ROW-RES] cmd=%u %s srv=%.4f pred=%.4f d=%+.4f "
										"class=%s ackCount=%d latest=%d N=%.0f thresh=%.2f #%u\n",
										nCmd, name, fs, fp, d,
										PredAuth_KickRowClassName(d, ad),
										s_pnrAckCount, s_pnrLatest, flKickLag, thresh,
										s_nKickRes);
							}
							continue;
						}
					}

					if (!sdk_pred_weapon_fire_lag.GetBool() || nLag <= 0)
						continue;
					bool bWant = false;
					for (const char* k : kLagNames)
						if (!strcmp(name, k)) { bWant = true; break; }
					if (!bWant)
						continue;
					if (type == FIELD_BOOLEAN)
					{
						if (pPredicted[off] != pServer[off])
						{
							pPredicted[off] = pServer[off];
							++s_nFireLag;
						}
					}
					else if (type == FIELD_INTEGER || type == FIELD_SHORT)
					{
						const int is = *reinterpret_cast<const int*>(pServer + off);
						const int ip = *reinterpret_cast<const int*>(pPredicted + off);
						int d = ip - is;
						if (d < 0) d = -d;
						if (d > 0 && d <= nLag)
						{
							memcpy(pPredicted + off, pServer + off, 4);
							++s_nFireLag;
						}
					}
				}
			}
		}
	}

	// Partial-byte compare mask (shipped #34e utility bit + any later byteMask rows).
	// Direction: pred = (pred & ~mask) | (srv & mask) on the predicted record only --
	// CLIENT_TIMING_COMPARE_ONLY, no live-member repair. Per-entry pGateCvar (A/B).
	if (sdk_pred_unfed_mask.GetBool())
	{
		for (int b = 0; b < pc->bmN; ++b)
		{
			const PredAuthEntry_t* ent = pc->bmEntry[b];
			if (ent && ent->pGateCvar && !ent->pGateCvar->GetBool())
				continue;

			pPredicted[pc->bmOff[b]] = static_cast<uint8_t>(
				(pPredicted[pc->bmOff[b]] & ~pc->bmMask[b])
				| (pServer[pc->bmOff[b]] & pc->bmMask[b]));
		}
	}

	// -----------------------------------------------------------------------
	// [INF-AMMO-CLIENT] m_infiniteAmmoState decoded into the
	// member (0x1598) that GetActiveAmmoSource reads. Independent of the parked
	// clip mask below.
	// -----------------------------------------------------------------------
	if (bridge_inf_ammo_client_diag.GetBool() && pc->kind == AUTHKIND_WEAPON && pEntity)
	{
		int state = 0, clip = 0, stock = 0;
		__try
		{
			const uint8_t* const p = reinterpret_cast<const uint8_t*>(pEntity);
			state = *reinterpret_cast<const int*>(p + 0x1598); // m_infiniteAmmoState
			clip  = *reinterpret_cast<const int*>(p + 0x1590); // m_ammoInClip
			stock = *reinterpret_cast<const int*>(p + 0x1594); // m_ammoInStockpile
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { state = 0; }

		if (state != 0)
		{
			// First-N + on entity/state transition only.
			static const void* s_lastEnt = nullptr;
			static int s_lastState = 0;
			if (pEntity != s_lastEnt || state != s_lastState)
			{
				s_lastEnt = pEntity;
				s_lastState = state;

				static volatile LONG s_landedN = 0;
				const LONG n = InterlockedIncrement(&s_landedN);
				if (n <= 24)
					Warning(eDLL_T::CLIENT,
						"[INF-AMMO-CLIENT] LANDED m_infiniteAmmoState=%d ent=%p clip=%d stockpile=%d%s\n",
						state, pEntity, clip, stock,
						n == 24 ? " (further silenced)" : "");
			}
		}
	}

	// -----------------------------------------------------------------------
	// [MODBITS] Attachment-change observability. Runs BEFORE the native compare,
	// so pServer is the value this ack delivered and pPredicted is what the
	// client carried into it -- the pair says whether an attachment reached the
	// client at all, and whether prediction is about to overwrite it. Also
	// reads the two laser FX gate bytes at the offsets the FX update itself uses.
	// -----------------------------------------------------------------------
	if (bridge_weapon_modbits_probe.GetBool() && pc->kind == AUTHKIND_WEAPON && pEntity)
	{
		static constexpr int kLiveFromPlayer = 5908, kLiveInternal = 5912, kLiveCurrent = 5916;
		static constexpr int kFlatFromPlayer = 892,  kFlatInternal = 896,  kFlatCurrent = 900;
		static constexpr int kLaserModEnabled = 8292;     // m_modVars.targeting_laser_enabled
		static constexpr int kLaserScriptEnabled = 5836;  // m_targetingLaserEnabledScript
		static constexpr int kModBitsSeenMax = 32;

		int liveFrom = 0, liveInt = 0, liveCur = 0;
		int predCur = 0, srvCur = 0, srvFrom = 0;
		int laserMod = 0, laserScript = 0;
		bool bRead = false;

		__try
		{
			const uint8_t* const p = reinterpret_cast<const uint8_t*>(pEntity);
			liveFrom = *reinterpret_cast<const int*>(p + kLiveFromPlayer);
			liveInt  = *reinterpret_cast<const int*>(p + kLiveInternal);
			liveCur  = *reinterpret_cast<const int*>(p + kLiveCurrent);
			predCur  = *reinterpret_cast<const int*>(pPredicted + kFlatCurrent);
			srvCur   = *reinterpret_cast<const int*>(pServer + kFlatCurrent);
			srvFrom  = *reinterpret_cast<const int*>(pServer + kFlatFromPlayer);
			laserMod    = p[kLaserModEnabled];
			laserScript = p[kLaserScriptEnabled];
			bRead = true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { bRead = false; }

		if (bRead)
		{
			// Log on transition only. The dedup is PER ENTITY: several weapon
			// predictables alternate every ack, so a single last-seen slot
			// mismatches on every call and prints the whole steady state.
			struct ModBitsSeen_t
			{
				const void* pEnt;
				int liveCur, srvCur, liveFrom;
				int laserMod, laserScript;
			};
			static ModBitsSeen_t s_seen[kModBitsSeenMax] = {};

			int slot = -1;
			for (int i = 0; i < kModBitsSeenMax; ++i)
			{
				if (s_seen[i].pEnt == pEntity || !s_seen[i].pEnt)
				{
					slot = i;
					break;
				}
			}
			if (slot < 0)
			{
				// table full: reuse slot 0 rather than going silent
				slot = 0;
				static bool s_bModBitsSeenFullWarned = false;
				if (!s_bModBitsSeenFullWarned)
				{
					s_bModBitsSeenFullWarned = true;
					Warning(eDLL_T::CLIENT,
						"[MODBITS] dedup table full (%d); reusing slot 0\n",
						kModBitsSeenMax);
				}
			}

			ModBitsSeen_t& seen = s_seen[slot];
			const bool bChanged = (seen.pEnt != pEntity) || (seen.liveCur != liveCur) ||
				(seen.srvCur != srvCur) || (seen.liveFrom != liveFrom) ||
				(seen.laserMod != laserMod) || (seen.laserScript != laserScript);

			if (bChanged)
			{
				seen.pEnt = pEntity;
				seen.liveCur = liveCur;
				seen.srvCur = srvCur;
				seen.liveFrom = liveFrom;
				seen.laserMod = laserMod;
				seen.laserScript = laserScript;

				Msg(eDLL_T::CLIENT,
					"[MODBITS] ent=%p live(from=0x%X int=0x%X cur=0x%X) pred(cur=0x%X) srv(from=0x%X cur=0x%X) laser(mod=%d script=%d)\n",
					pEntity, liveFrom, liveInt, liveCur, predCur, srvFrom, srvCur,
					laserMod, laserScript);
			}
		}
	}

	// -----------------------------------------------------------------------
	// [INF-AMMO-CLIENT] Weapon clip/stockpile client authority while infinite.
	// C_WeaponX::GetActiveAmmoSource: state != 0 => source INFINITE (clip
	// still cycles; reserve is free). Bridge dedi often wires stale clip; PNR
	// would rubber-band reloads. Equalize the compare buffers so pred==srv for
	// those fields only, and repair the live member from the predicted record
	// so a foreign-field rebase cannot reinstall empty clip.
	// -----------------------------------------------------------------------
	if (bridge_inf_ammo_client.GetBool() && pc->kind == AUTHKIND_WEAPON && pEntity)
	{
		// Live member offset (S21 recv/datamap): m_infiniteAmmoState @ 0x1598.
		static constexpr int kInfAmmoStateOff = 0x1598;
		int state = 0;
		__try { state = *reinterpret_cast<const int*>(
			reinterpret_cast<const uint8_t*>(pEntity) + kInfAmmoStateOff); }
		__except (EXCEPTION_EXECUTE_HANDLER) { state = 0; }

		if (state != 0)
		{
			const uintptr_t opt = *reinterpret_cast<uintptr_t*>(dmap + DMAP_OPTIMIZED);
			if (opt)
			{
				const uintptr_t info   = opt + (uintptr_t)OPT_INFO_STRIDE * PC_NETWORKED_ONLY;
				const uintptr_t fields = *reinterpret_cast<uintptr_t*>(info + INFO_FLAT_FIELDS);
				const int count        = *reinterpret_cast<int*>(info + INFO_FLAT_COUNT);
				int nMasked = 0;
				if (fields && count > 0 && count <= 4096)
				{
					for (int i = 0; i < count; ++i)
					{
						const uintptr_t td = fields + (uintptr_t)i * TD_STRIDE;
						const char* name = *reinterpret_cast<const char**>(td + TD_FIELDNAME);
						if (!name)
							continue;
						if (strcmp(name, "m_ammoInClip") != 0 &&
							strcmp(name, "m_ammoInStockpile") != 0)
							continue;

						const int type = *reinterpret_cast<int*>(td + TD_FIELDTYPE);
						const int n16  = *reinterpret_cast<uint16_t*>(td + TD_FIELDSIZE);
						const int cnt  = n16 ? n16 : 1;
						const int w    = AuthWidthForType(type, cnt);
						if (w <= 0)
							continue;
						const int off  = *reinterpret_cast<int*>(td + TD_FLATOFFSET1);
						const int mOff = PredAuth_MemberOff(td);
						if (off < 0)
							continue;

						// Equalize compare: predicted record keeps its value,
						// server baseline adopts it (unfed direction that
						// preserves client-owned content).
						memcpy(pServer + off, pPredicted + off, static_cast<size_t>(w));
						if (bRepair && mOff >= 0)
							memcpy(reinterpret_cast<uint8_t*>(pEntity) + mOff,
								pPredicted + off, static_cast<size_t>(w));
						++nMasked;
					}
				}
				if (nMasked > 0 && bridge_inf_ammo_client_diag.GetBool())
				{
					static volatile LONG s_infMaskN = 0;
					const LONG n = InterlockedIncrement(&s_infMaskN);
					if (n <= 24)
						Warning(eDLL_T::CLIENT,
							"[INF-AMMO-CLIENT] mask clip/stockpile ent=%p state=%d fields=%d%s\n",
							pEntity, state, nMasked,
							n == 24 ? " (further silenced)" : "");
				}
			}
		}
	}
}

//-----------------------------------------------------------------------------
// [FORCED0-NEUT] dispatch tally API. Moved from h_Prediction_Dispatch.
//-----------------------------------------------------------------------------
void PredAuth_DispatchBegin(void)
{
	s_pnrDispatchEntityErrors = 0;
	s_pnrDispatchActive = 1;
}

void PredAuth_NoteEntityError(void)
{
	++s_pnrDispatchEntityErrors;
}

bool PredAuth_DispatchActive(void)
{
	return s_pnrDispatchActive != 0;
}

PredAuthDispatchResult_s PredAuth_DispatchEnd(__int64 a1, unsigned int a2, int a3, char a4)
{
	s_pnrDispatchActive = 0;

	PredAuthDispatchResult_s res = { 0, s_pnrDispatchEntityErrors, 0 };
	if (!a1)
		return res;

	__try
	{
		res.errAfter = *reinterpret_cast<int*>(a1 + 212); // m_Split[0].m_bPreviousAckHadErrors

		// Native: if (a3==0 && a4) forces 212=1 before entity compares; entity
		// failures also set 212=1. When forced0 alone (no entity field errors),
		// clear the free-rebase latch (PART 15e chronic re-fire driver).
		if (sdk_pred_forced0_neut.GetBool() && a3 == 0 && a4 && res.errAfter != 0
			&& res.entErrs == 0)
		{
			*reinterpret_cast<int*>(a1 + 212) = 0;
			res.errAfter = 0;
			res.neutered = 1;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}

	return res;
}

// When the punch dial is 0 the six m_vecPunch* rows are intentionally s_authDead.
// Board nPunch is then the owned dial cost, not a mask miss. 0 = always count
// punch (prove the alarm still fires). Default 1.
static ConVar sdk_pred_alarm_ignore_retired_punch(
	"sdk_pred_alarm_ignore_retired_punch", "1", FCVAR_DEVELOPMENTONLY,
	"1 = omit punch from REGRESSION-ALARM while bridge_punch_legacy_mask is 0 "
	"(owned dial). 0 = always count punch (prove-alive / legacy). Ship default 1.");

// m_bZooming is FED (ZOOM RECLASS); board nZoom is almost always that bool riding
// weapon-switch, not a m_zoomBaseFrac miss. 0 = count zoom (prove-alive). Default 1.
static ConVar sdk_pred_alarm_ignore_fed_zoom(
	"sdk_pred_alarm_ignore_fed_zoom", "1", FCVAR_DEVELOPMENTONLY,
	"1 = omit zoom from REGRESSION-ALARM (m_bZooming is FED by design). "
	"0 = count zoom (prove-alive / hunt m_zoomBaseFrac misses via board). "
	"Ship default 1.");

//-----------------------------------------------------------------------------
// [PRED-BOARD] regression alarm -- animevt is fully table-covered; punch is
// covered only while bridge_punch_legacy_mask is 1; zoom's only masked leaf is
// m_zoomBaseFrac (m_bZooming is FED). Nonzero after those gates => mask miss /
// renamed field / dmap cache full. Rate: once per 512-ack window from caller.
// Prove-alive: sdk_pred_alarm_ignore_retired_punch 0 (with dial still 0) must
// still print REGRESSION-ALARM punch=N on a fire window; animevt is never gated.
//-----------------------------------------------------------------------------
int PredAuth_BoardAlarm(unsigned int nZoom, unsigned int nPunch, unsigned int nAnimevt)
{
	// Dial 0 retires all six m_vecPunch* rows (s_authDead). Counting them as a
	// regression is a false positive every fire window.
	if (!bridge_punch_legacy_mask.GetBool() &&
		sdk_pred_alarm_ignore_retired_punch.GetBool())
	{
		nPunch = 0;
	}

	// m_bZooming is intentionally FED; board zoom is not proof of a mask miss.
	if (sdk_pred_alarm_ignore_fed_zoom.GetBool())
		nZoom = 0;

	if (nZoom == 0 && nPunch == 0 && nAnimevt == 0)
		return 0;

	Warning(eDLL_T::CLIENT,
		"[PRED-AUTH] REGRESSION-ALARM zoom=%u punch=%u animevt=%u -- covered family "
		"fired (mask miss / renamed field / dmap cache full; punch counted only if "
		"bridge_punch_legacy_mask 1 or sdk_pred_alarm_ignore_retired_punch 0; zoom "
		"counted only if sdk_pred_alarm_ignore_fed_zoom 0)\n",
		nZoom, nPunch, nAnimevt);
	return 1;
}

//-----------------------------------------------------------------------------
// PredAuth_GetDmapKind -- cache lookup only (PredAuth_Apply populated the kind
// earlier in the same PNR hook). Returns AUTH_SCOPE_* (numerically identical to
// AuthDmapKind_t), or AUTH_SCOPE_ANY if the dmap is unknown/uncached.
//-----------------------------------------------------------------------------
int PredAuth_GetDmapKind(void* pEntity)
{
	if (!pEntity)
		return AUTH_SCOPE_ANY;
	void* (*GetPredDescMap)(void*) = reinterpret_cast<void* (*)(void*)>(
		(*reinterpret_cast<void***>(pEntity))[VIDX_GetPredDescMap]);
	const uintptr_t dmap = reinterpret_cast<uintptr_t>(GetPredDescMap(pEntity));
	if (!dmap)
		return AUTH_SCOPE_ANY;
	for (const auto& c : s_cache)
		if (c.dmap == dmap)
			return static_cast<int>(c.kind);
	return AUTH_SCOPE_ANY;
}

//-----------------------------------------------------------------------------
// [SHOT-IDX-FANOUT] S21 split S3's single burst-shot counter into
// m_shotIndexForSpread + m_shotCount; S3 only networks m_shotCount, so the
// spread index arrives as permanent 0 and the S21 start-of-burst viewkick
// clamp in AddViewKickForAttack re-fires on EVERY shot -- the frozen recoil
// pattern row. At PNR time the live members hold the wire-applied server
// values, so copying live m_shotCount over live m_shotIndexForSpread feeds
// the clamp the same counter the dedi's own clamp reads. Must run before the
// mask/census/native compare so the compare, the originalData refresh and any
// rebase all see the fed value. Offsets resolved BY NAME from the predicted
// datamap -- no hardcoded client layout.
//-----------------------------------------------------------------------------
static ConVar bridge_shot_index_fanout("bridge_shot_index_fanout", "1", FCVAR_RELEASE,
	"Per-ack copy of the wire-fed m_shotCount into m_shotIndexForSpread on "
	"predictable weapons (S3 networks only the pre-split counter). Fixes the "
	"viewkick pattern row freezing at the start-of-burst clamp. 0 = off.");

void PredAuth_ShotIndexFanout(void* pEntity)
{
	if (!bridge_shot_index_fanout.GetBool() || !pEntity)
		return;

	struct FanoutDmap_t { uintptr_t dmap; int offIdx; int offCnt; };
	static FanoutDmap_t s_fanCache[8] = {};
	static int s_fanCacheCount = 0;
	static bool s_fanAnnounced = false;

	__try
	{
		void* (*GetPredDescMap)(void*) = reinterpret_cast<void* (*)(void*)>(
			(*reinterpret_cast<void***>(pEntity))[VIDX_GetPredDescMap]);
		const uintptr_t dmap = reinterpret_cast<uintptr_t>(GetPredDescMap(pEntity));
		if (!dmap)
			return;

		FanoutDmap_t* entry = nullptr;
		for (int i = 0; i < s_fanCacheCount; ++i)
		{
			if (s_fanCache[i].dmap == dmap) { entry = &s_fanCache[i]; break; }
		}
		if (!entry)
		{
			if (s_fanCacheCount >= 8)
				return;
			entry = &s_fanCache[s_fanCacheCount++];
			entry->dmap = dmap;
			int dummy = -1;
			if (!TimeDomain_ResolveFlatOff(dmap, "m_shotIndexForSpread", &entry->offIdx, &dummy))
				entry->offIdx = -1;
			if (!TimeDomain_ResolveFlatOff(dmap, "m_shotCount", &entry->offCnt, &dummy))
				entry->offCnt = -1;
		}
		if (entry->offIdx < 0 || entry->offCnt < 0)
			return; // dmap without the pair (player etc.) -- not a weapon

		const int cnt = *reinterpret_cast<const int*>(
			reinterpret_cast<uint8_t*>(pEntity) + entry->offCnt);
		*reinterpret_cast<int*>(
			reinterpret_cast<uint8_t*>(pEntity) + entry->offIdx) = cnt;

		if (!s_fanAnnounced && cnt != 0)
		{
			s_fanAnnounced = true;
			Warning(eDLL_T::CLIENT,
				"[SHOT-IDX-FANOUT] live: m_shotCount=%d -> m_shotIndexForSpread (offs %d -> %d)\n",
				cnt, entry->offCnt, entry->offIdx);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool PredAuth_ResolveFlatOff(uintptr_t dmap, const char* want, int* pOff0, int* pOff1)
{
	return TimeDomain_ResolveFlatOff(dmap, want, pOff0, pOff1);
}
