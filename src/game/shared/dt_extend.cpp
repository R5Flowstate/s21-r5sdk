//=============================================================================//
//
// Purpose: Native SendTable expansion -- see dt_extend.h. Server-only.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "core/bridge_ready.h"
#include "core/bridge_stats.h"
#include "tier0/module.h"
#include "tier0/commandline.h"
#include "tier1/cvar.h"
#include "tier0/memvalidate.h"
#include "tier0/memstd.h"
#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/detours.h"
#include "game/shared/dt_extend.h"
#include "game/shared/dt_extend_diag.h"
#include "game/shared/dt_extend_system14.h"
#include "game/shared/s21_dt_schema.h"
#include "game/shared/scriptnetdata_ext.h"
#include "common/global.h" // host_timescale
#include "game/server/consumable_inv.h"
#include "game/server/jetdrive.h"
#include "game/server/player_launch.h"
#include "game/server/trigger_gravity.h"
#include "game/server/trigger_updraft.h"
#include "game/server/track_entity.h"
#include "game/server/zipline_extend_state.h"
#include "game/server/poseparam_ext.h"
#include "game/shared/player_extend_sidecar.h"
#include "engine/server/snapshot_send.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/heap_canary.h"
#include "game/shared/scriptnetdata_limits.h"
#include "game/shared/deathfield_system.h"
#include "game/shared/offhand_slots_ext.h"
#include "game/shared/activity_s3_to_s21.h"
#include "game/shared/weapstate_s3_to_s21.h"
#include "engine/server/zipline_validation.h"
#include "game/shared/weapon_script_vars.h"
#include "game/shared/weapon_heat.h"
#include "game/shared/sdk_entity_state.h"
#include "game/server/energize.h"
#ifndef CLIENT_DLL
#include "game/server/akimbo.h"
#endif
#include "game/shared/globalnonrewind_vars.h"
#include "engine/server/server.h"
#include "engine/client/client.h"

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <ctime>

// ===========================================================================
// SYSTEM 01: FOUNDATION
// ===========================================================================

// ST_*/SP_* layout consts: dt_extend.h

// LEGACY parentTable pass: same rewrite the canonical path does under
// bridge_canonical_fix_parenttable. Default ON -- A/B with 0 if needed.
static ConVar bridge_legacy_fix_parenttable("bridge_legacy_fix_parenttable", "1",
	FCVAR_RELEASE,
	"After deep-clone and DTExtend_Apply, repoint every SendProp+0x08 (parentTable) "
	"to its owning table so pack guts resolve the correct entity sub-object. "
	"Mirrors bridge_canonical_fix_parenttable for the LEGACY path. 1=on (default).");

//-----------------------------------------------------------------------------
// Prop registry -- add entries here, bump kNumExtendProps.
// Send side must state each width; S21 RecvProps carry nBits=0.
//-----------------------------------------------------------------------------
// Templates: Vector prefers flags==4, Int64 flags==1. Vector high=-121121 is NOSCALE.
#define EI(tbl, name)    { tbl, name, SendPropType::DPT_Int,    32, 0, 0.f,       0.f,  0,   4, nullptr }
#define EF(tbl, name)    { tbl, name, SendPropType::DPT_Float,   0, 4, 0.f,       0.f,  0,   4, nullptr }
#define ET(tbl, name)    { tbl, name, SendPropType::DPT_Time,   32, 0, 0.f,       0.f,  0,   4, nullptr }
#define EV(tbl, name)    { tbl, name, SendPropType::DPT_Vector,  0, 4, 0.f, -121121.f,  0,  12, nullptr }
#define E64(tbl, name)   { tbl, name, SendPropType::DPT_Int64,  64, 1, 0.f,       0.f,  0,   8, nullptr }
#define ES(tbl, name)    { tbl, name, SendPropType::DPT_String,  0, 0, 0.f,       0.f,  0, 256, nullptr } // reserve a 256-byte char for the field

// memmove the new prop to immediately after `anchor` so the flat index stream matches.
#define EI_AFTER(tbl, name, anchor)  { tbl, name, SendPropType::DPT_Int,    32, 0, 0.f,       0.f,  0,   4, anchor }
#define EF_AFTER(tbl, name, anchor)  { tbl, name, SendPropType::DPT_Float,   0, 4, 0.f,       0.f,  0,   4, anchor }
#define ET_AFTER(tbl, name, anchor)  { tbl, name, SendPropType::DPT_Time,   32, 0, 0.f,       0.f,  0,   4, anchor }
#define EV_AFTER(tbl, name, anchor)  { tbl, name, SendPropType::DPT_Vector,  0, 4, 0.f, -121121.f,  0,  12, anchor }
#define E64_AFTER(tbl, name, anchor) { tbl, name, SendPropType::DPT_Int64,  64, 1, 0.f,       0.f,  0,   8, anchor }

const DTExtendProp s_extendProps[] = {

	// --- DT_BaseAnimating (2) ---
	EI("DT_BaseAnimating", "m_animRelativeToGroundEnabled"),
	// Int, not Int64: the S21 client's RecvProp is DPT_Int, 4 bytes, at entity +0xD5C -- an Int64 SendProp does not pair and the wire bits are discarded.
	// Value-proxied (see ItemFlavorGUID_ValueProxy) -- no entity memory is written, so the unverified DT_BaseAnimating slack is untouched.
	EI("DT_BaseAnimating", "m_itemFlavorGUID"),

	// --- DT_BaseCombatCharacter (12) ---
	EI("DT_BaseCombatCharacter", "m_weaponTypeDisabledFlags"),
	EI("DT_BaseCombatCharacter", "m_weaponInventorySlotLockedFlags"),
	EI("DT_BaseCombatCharacter", "m_phaseShiftType"),
	EI("DT_BaseCombatCharacter", "m_akimboState"),
	EI("DT_BaseCombatCharacter", "m_akimboShouldAltFire"),
	EI("DT_BaseCombatCharacter", "m_allowHudSelectionWhileWeaponsDisabled"),
	EI("DT_BaseCombatCharacter", "m_weaponAmmoRegenDisabled"),
	EI("DT_BaseCombatCharacter", "m_weaponAmmoRegenDisabledRefCount"),
	EI("DT_BaseCombatCharacter", "m_bIsPlayerOverheating"),
	EF("DT_BaseCombatCharacter", "m_playerOverheatValue"),
	ET("DT_BaseCombatCharacter", "m_timeLastGeneratedPlayerOverheat"),
	EI("DT_BaseCombatCharacter", "m_targetInfoPingValue"),

	// --- DT_BaseEntity (2) ---
	EI("DT_BaseEntity", "m_wantsScopeHighlight"),
	EI("DT_BaseEntity", "m_ignoreParentRotation"),

	// --- DT_BaseGrenade (1) ---
	EI("DT_BaseGrenade", "m_grenadeStatusFlags"),

	// --- DT_BaseViewModel (1) ---
	EF("DT_BaseViewModel", "m_nResetEventsStartTime"),

	// --- DT_CarePackageInsightProp (2) --- Nest + m_lootIndex (int, client+5600) + m_contentsTaken (client+5604, 1 byte).
	// GetLootIndex/GetAreContentsTaken bind those offsets; without them every revealed care package reports loot index 0 / not looted.
	EI("DT_CarePackageInsightProp", "m_lootIndex"),
	EI("DT_CarePackageInsightProp", "m_contentsTaken"),

	// --- DT_CPropDoor (2) ---
	EI("DT_CPropDoor", "m_iTeamNum"),
	EI("DT_CPropDoor", "m_isReinforced"),

	// --- DT_DynamicPropLightweight (1) ---
	EI("DT_DynamicPropLightweight", "m_phaseShiftFlags"),

	// --- DT_GlobalNonRewinding (1) ---
	// Value-proxied from CServer+0x3CC (SetTimescale out). Base 2500 is not slack.
	EF("DT_GlobalNonRewinding", "m_gameTimescale"),

	// --- DT_InfoTarget (2) ---
	EI("DT_InfoTarget", "m_bIsSoundCodeControllerValueSet"),
	EF("DT_InfoTarget", "m_flSoundCodeControllerValue"),

	// --- DT_LocalPlayerExclusive (17) --- [JETDRIVE] Vantage tactical recall-launch.
	// S21 RecvProp names/types harvested verbatim from s21_dt_schema.h:3299-3312 (s_sch_DT_LocalPlayerExclusive) -- names must match exactly, see dt_extend.h's hard requirement.
	EI("DT_LocalPlayerExclusive", "m_deathFieldIndex"),
	EI("DT_LocalPlayerExclusive", "m_jetDriveWasActive"),
	EI("DT_LocalPlayerExclusive", "m_jetDriveActive"),
	EI("DT_LocalPlayerExclusive", "m_jetDriveTargetEnt"),
	EI("DT_LocalPlayerExclusive", "m_jetDriveInDecelWindow"),
	EF("DT_LocalPlayerExclusive", "m_jetDriveSpeed"),
	EF("DT_LocalPlayerExclusive", "m_jetDriveAccel"),
	EF("DT_LocalPlayerExclusive", "m_jetDriveTimeout"),
	EF("DT_LocalPlayerExclusive", "m_jetDriveDoubleJumpVelBackFrac"),
	ET("DT_LocalPlayerExclusive", "m_jetDriveStartTime"),
	ET("DT_LocalPlayerExclusive", "m_jetDriveDecelWindowTimeOutTime"),
	EV("DT_LocalPlayerExclusive", "m_jetDriveTargetPos"),
	EV("DT_LocalPlayerExclusive", "m_jetDriveTargetEntOffset"),
	EV("DT_LocalPlayerExclusive", "m_jetDriveStartPos"),
	EV("DT_LocalPlayerExclusive", "m_jetDriveDoubleJumpVelocity"),
	EI("DT_LocalPlayerExclusive", "m_gravityLiftActive"),
	EI("DT_LocalPlayerExclusive", "m_blackholeActive"),
	EI("DT_LocalPlayerExclusive", "m_updraftCount"),
	EI("DT_LocalPlayerExclusive", "m_updraftStage"),
	ET("DT_LocalPlayerExclusive", "m_updraftEnterTime"),
	ET("DT_LocalPlayerExclusive", "m_updraftLeaveTime"),
	EF("DT_LocalPlayerExclusive", "m_updraftMinShakeActivationHeight"),
	EF("DT_LocalPlayerExclusive", "m_updraftMaxShakeActivationHeight"),
	EF("DT_LocalPlayerExclusive", "m_updraftLiftActivationHeight"),
	EF("DT_LocalPlayerExclusive", "m_updraftLiftSpeed"),
	EF("DT_LocalPlayerExclusive", "m_updraftLiftAcceleration"),
	EF("DT_LocalPlayerExclusive", "m_updraftLiftExitDuration"),
	ET("DT_LocalPlayerExclusive", "m_updraftSlowTime"),
	EI("DT_LocalPlayerExclusive", "m_skydiveFromUpdraft"),
	// PlayerLaunch cluster. S21 RecvProps on DT_LocalPlayerExclusive; S3 has
	// no members. Sidecar + value proxy, same as jetdrive -- base 20000 is
	// mid-class. Idle is zeros.
	EI("DT_LocalPlayerExclusive", "m_playerLaunchActivate"),
	EI("DT_LocalPlayerExclusive", "m_playerLaunchAvoidedMantle"),
	EI("DT_LocalPlayerExclusive", "m_playerLaunchLock3pRotation"),
	EV("DT_LocalPlayerExclusive", "m_playerLaunchVelocity"),
	ET("DT_LocalPlayerExclusive", "m_playerLaunchStartTime"),

	// --- DT_LootGrabber (4) --- Client offsets/widths: m_impactEffectColorID 5600 (1B), m_lootGrabberType 5602 (1B), m_lootBeingGrabbed 5608 (4B), m_lootGrabDist 5612 (float). m_lootGrabberType: IsVendingMachine == 1, IsLinkedBox == 2. m_minimapData deliberately absent -- CBaseEntity already sends it via nested DT_BaseEntity; re-declaring would double it.
	EI("DT_LootGrabber", "m_impactEffectColorID"),
	EI("DT_LootGrabber", "m_lootGrabberType"),
	EI("DT_LootGrabber", "m_lootBeingGrabbed"),
	EF("DT_LootGrabber", "m_lootGrabDist"),

	// --- DT_ThirdPersonView (20) ---
	// S21 camera extras S3 ThirdPersonViewData does not carry. Value-proxied
	// from the track-entity sidecar; do not grow the nested S3 struct.
	EF("DT_ThirdPersonView", "m_thirdPersonEntBlendOutDuration"),
	EF("DT_ThirdPersonView", "m_thirdPersonEntFixedRight"),
	EF("DT_ThirdPersonView", "m_thirdPersonEntVariableDistStart"),
	EF("DT_ThirdPersonView", "m_thirdPersonEntVariableDistEnd"),
	EF("DT_ThirdPersonView", "m_thirdPersonEntVariableDistStartTime"),
	EF("DT_ThirdPersonView", "m_thirdPersonEntVariableDistEndTime"),
	EI("DT_ThirdPersonView", "m_thirdPersonEntVariableDistLerpType"),
	EI("DT_ThirdPersonView", "m_thirdPersonEntVariableDistLerpLogGrowthFactor"),
	EF("DT_ThirdPersonView", "m_thirdPersonEntVariableHeightStart"),
	EF("DT_ThirdPersonView", "m_thirdPersonEntVariableHeightEnd"),
	EF("DT_ThirdPersonView", "m_thirdPersonEntVariableHeightStartTime"),
	EF("DT_ThirdPersonView", "m_thirdPersonEntVariableHeightEndTime"),
	EI("DT_ThirdPersonView", "m_thirdPersonEntVariableHeightLerpType"),
	EI("DT_ThirdPersonView", "m_thirdPersonEntVariableHeightLerpLogGrowthFactor"),
	EF("DT_ThirdPersonView", "m_thirdPersonEntVariableRightStart"),
	EF("DT_ThirdPersonView", "m_thirdPersonEntVariableRightEnd"),
	EF("DT_ThirdPersonView", "m_thirdPersonEntVariableRightStartTime"),
	EF("DT_ThirdPersonView", "m_thirdPersonEntVariableRightEndTime"),
	EI("DT_ThirdPersonView", "m_thirdPersonEntVariableRightLerpType"),
	EI("DT_ThirdPersonView", "m_thirdPersonEntVariableRightLerpLogGrowthFactor"),

	// --- DT_LootRoller (2) --- DT_LootRoller is nest + these two (m_tier int @ client+5568, m_hasVaultKey byte @ client+5572).
	// GetTier reads +5568; without them every roller reports tier 0 and the eye FX colour as TIER0.
	EI("DT_LootRoller", "m_tier"),
	EI("DT_LootRoller", "m_hasVaultKey"),

	// --- DT_ParticleSystem (5) ---
	EI("DT_ParticleSystem", "m_bLOSBlockScan"),
	EI("DT_ParticleSystem", "m_bPlaySounds"),
	EI("DT_ParticleSystem", "m_enemyControlPoint"),
	EI("DT_ParticleSystem", "m_enemyControlPointOverride"),
	ES("DT_ParticleSystem", "m_soundSuffix"),

	// --- DT_Player (44 Int/Float/Time; 7 Vector deferred to step 3, 3 Int64 to step 4,
	// __skip_m_passives Array + m_passives[ 0 ] element to step 7) ---
	EI("DT_Player", "m_armoredLeapPhase"),
	ET("DT_Player", "m_armoredLeapStartTime"),
	EI("DT_Player", "m_armoredLeapType"),
	EI("DT_Player", "m_bHasMatchAdminRole"),
	EF("DT_Player", "m_bleedoutStartTime"),
	EI("DT_Player", "m_communicationsAutoBlocked"),
	EI("DT_Player", "m_crossPlayChat"),
	EI("DT_Player", "m_crossPlayChatFriends"),
	EI("DT_Player", "m_crossProgressionMigrated"),
	EF("DT_Player", "m_dragReviveOutroStartTime"),
	EI("DT_Player", "m_dragReviveState"),
	EI("DT_Player", "m_extraShieldHealth"),
	EI("DT_Player", "m_extraShieldTier"),
	ET("DT_Player", "m_jumpPadDebounceExpireTime"),
	EI("DT_Player", "m_laserSightColorCustomized"),
	ET("DT_Player", "m_lastSprintPressTime"),
	EI("DT_Player", "m_launcherAirControlActive"),
	// The mantle-boost FSM state (0 idle, 1 invalid/hang, 3 failed, 4 boost).
	// Server-authoritative: the client predicts the same FSM for feel, but the two evaluate an animation-derived sweet spot on independently advanced proxies, so borderline attempts can disagree.
	EI("DT_Player", "m_mantleBoostState"),
	EI("DT_Player", "m_playerSettingForHoldToSprint"),
	EI("DT_Player", "m_playerSettingForStickySprintForward"),
	EI("DT_Player", "m_playerVehicleCount"),
	EI("DT_Player", "m_playerVehicleDriven"),
	ET("DT_Player", "m_playerVehicleUseTime"),
	EF("DT_Player", "m_ragdollCreationYaw"),
	EI("DT_Player", "m_reviveTarget"),
	EI("DT_Player", "m_shadowShieldActive"),
	// m_skydiveContraintRadius / m_skydiveContraintPostion / m_skydiveScriptInputOverride are deliberately NOT appended.
	// All three are "negative means disabled" sentinels: the client's ctor and Player_EndSkydive both store -1.0f into the radius and the input override.
	EI("DT_Player", "m_skydiveFromSkywardLaunch"),
	EI("DT_Player", "m_skydiveState"),
	EF("DT_Player", "m_skywardLaunchEndTime"),
	EF("DT_Player", "m_skywardLaunchFastEndTime"),
	EF("DT_Player", "m_skywardLaunchFastSpeed"),
	EI("DT_Player", "m_skywardLaunchFollowing"),
	EI("DT_Player", "m_skywardLaunchInterrupted"),
	EF("DT_Player", "m_skywardLaunchSlowEndTime"),
	EF("DT_Player", "m_skywardLaunchSlowSpeed"),
	EF("DT_Player", "m_skywardLaunchSlowStartTime"),
	EI("DT_Player", "m_skywardLaunchState"),
	ET("DT_Player", "m_stickySprintForwardDisableTime"),
	ET("DT_Player", "m_stickySprintForwardEnableTime"),
	EI("DT_Player", "m_tempShieldHealth"),
	EI("DT_Player", "m_turret"),
	EI("DT_Player", "m_unspoofedHardware"),
	// DT_Player Int64 + Vector
	E64("DT_Player", "m_EadpUserId"),
	E64("DT_Player", "m_progressionUserId"),
	E64("DT_Player", "m_unSpoofedPlatformUserId"),
	EV("DT_Player", "m_armoredLeapAirPos"),
	EV("DT_Player", "m_armoredLeapEndPos"),
	EV("DT_Player", "m_laserSightColor"),
	EV("DT_Player", "m_ragdollCreationOrigin"),
	EV("DT_Player", "m_skywardObstacleAvoidanceEndPos"),
	EV("DT_Player", "m_skywardOffset"),
	// (m_passives[0..1] Int64-array + the SNDC arrays still deferred to step 6/7)

	// --- DT_PlayerDecoy (4) --- m_decoyVelocity belongs at slot anchor m_classModsActive+1 in S21's flat layout.
	// EV_AFTER tells DTExtend_Apply to memmove it there post-append so the changed-prop index stream matches the S21 client without a per-class fix function. m_vecViewOffset.x/y/z are tail-appended as before; the zero-proxy gate in DTExtend_ShouldZeroProxyAppendedProp keeps them safe.
	EF("DT_PlayerDecoy", "m_vecViewOffset.x"),
	EF("DT_PlayerDecoy", "m_vecViewOffset.y"),
	EF("DT_PlayerDecoy", "m_vecViewOffset.z"),
	EV_AFTER("DT_PlayerDecoy", "m_decoyVelocity", "m_classModsActive"),

	// --- DT_PlayerVehicle (20; m_vehiclePlayers Array deferred to step 6) ---
	EF("DT_PlayerVehicle", "m_cameraVehicleMaxDist"),
	EF("DT_PlayerVehicle", "m_cameraVehicleMaxVertDist"),
	ET("DT_PlayerVehicle", "m_driverActivationTime"),
	ET("DT_PlayerVehicle", "m_driverDeactivationTime"),
	EF("DT_PlayerVehicle", "m_hoverVehicleBanking"),
	EF("DT_PlayerVehicle", "m_hoverVehicleFrictionLastTime"),
	EI("DT_PlayerVehicle", "m_hoverVehicleFrictionSurfPropOther"),
	EI("DT_PlayerVehicle", "m_hoverVehicleIsMarkedAsDrivingForward"),
	EI("DT_PlayerVehicle", "m_hoverVehicleIsOnGround"),
	EI("DT_PlayerVehicle", "m_hoverVehicleIsParked"),
	ET("DT_PlayerVehicle", "m_hoverVehicleLastBoostTime"),
	EF("DT_PlayerVehicle", "m_hoverVehicleStunTimeEnd"),
	EF("DT_PlayerVehicle", "m_hoverVehicleThrottle"),
	EI("DT_PlayerVehicle", "m_iHealth"),
	EI("DT_PlayerVehicle", "m_iMaxHealth"),
	EI("DT_PlayerVehicle", "m_materialDualColorMask"),
	EI("DT_PlayerVehicle", "m_vehicleGroundEntity"),
	EV("DT_PlayerVehicle", "m_hoverVehicleFrictionNormal"),
	EV("DT_PlayerVehicle", "m_hoverVehicleFrictionPos"),
	EV("DT_PlayerVehicle", "m_vehicleGroundNormal"),

	// --- DT_Projectile (2) ---
	EI("DT_Projectile", "m_passThroughModCount"),
	EV("DT_Projectile", "m_launchOrigin"),

	// --- DT_PropSurvival (1) ---
	EI("DT_PropSurvival", "m_itemFlavorGUID"),

	// --- DT_RopeKeyframe (4) ---
	EI("DT_RopeKeyframe", "m_clrRender"),
	EI("DT_RopeKeyframe", "m_nRenderMode"),
	EV("DT_RopeKeyframe", "m_endOffset"),
	EV("DT_RopeKeyframe", "m_startOffset"),

	// --- DT_ScriptMover (3) ---
	EI("DT_ScriptMover", "m_parentAttachmentIndex"),
	EV("DT_ScriptMover", "m_initialAngles"),
	EV("DT_ScriptMover", "m_initialOrigin"),

	// --- DT_ScriptMoverLightweight (4) ---
	EF("DT_ScriptMoverLightweight", "m_trainPitchMax"),
	EI("DT_ScriptMoverLightweight", "m_trainSimulateBeforeMeEntity"),
	EV("DT_ScriptMoverLightweight", "m_initialAngles"),
	EV("DT_ScriptMoverLightweight", "m_initialOrigin"),

	// --- DT_ScriptProp (6) ---
	EF("DT_ScriptProp", "m_cloakEndTime"),
	EF("DT_ScriptProp", "m_cloakFadeInDuration"),
	ET("DT_ScriptProp", "m_cloakFadeInEndTime"),
	EF("DT_ScriptProp", "m_cloakFadeOutStartTime"),
	EF("DT_ScriptProp", "m_cloakFlickerAmount"),
	ET("DT_ScriptProp", "m_cloakFlickerEndTime"),

	// --- DT_TriggerCylinderHeavy (25) ---
	EF("DT_TriggerCylinderHeavy", "m_airControlMoveAccel"),
	EF("DT_TriggerCylinderHeavy", "m_airControlMoveSpeed"),
	EF("DT_TriggerCylinderHeavy", "m_blackholeInnerMoveSpeed"),
	EF("DT_TriggerCylinderHeavy", "m_blackholeInnerPullSpeed"),
	EF("DT_TriggerCylinderHeavy", "m_blackholeInnerRadius"),
	EI("DT_TriggerCylinderHeavy", "m_blackholeIsStrongPulling"),
	EF("DT_TriggerCylinderHeavy", "m_blackholeOuterMoveSpeed"),
	EF("DT_TriggerCylinderHeavy", "m_blackholeOuterPullSpeed"),
	EF("DT_TriggerCylinderHeavy", "m_blackholeStrongPullAddlSpeed"),
	EI("DT_TriggerCylinderHeavy", "m_enableDoubleJump"),
	EI("DT_TriggerCylinderHeavy", "m_gravityCannonLaunched"),
	EF("DT_TriggerCylinderHeavy", "m_gravityLiftEjectForwardSpeed"),
	EF("DT_TriggerCylinderHeavy", "m_gravityLiftEjectUpSpeed"),
	EF("DT_TriggerCylinderHeavy", "m_gravityLiftMaxEjectTime"),
	EF("DT_TriggerCylinderHeavy", "m_gravityLiftMaxHoverTime"),
	EF("DT_TriggerCylinderHeavy", "m_gravityLiftToCenterAccel"),
	EF("DT_TriggerCylinderHeavy", "m_gravityLiftToCenterSpeed"),
	EF("DT_TriggerCylinderHeavy", "m_gravityLiftUpAccel"),
	EF("DT_TriggerCylinderHeavy", "m_gravityLiftUpSpeed"),
	EF("DT_TriggerCylinderHeavy", "m_launchDelayAmount"),
	EF("DT_TriggerCylinderHeavy", "m_launchFlightTime"),
	EI("DT_TriggerCylinderHeavy", "m_limitedAirControl"),
	EI("DT_TriggerCylinderHeavy", "m_mortarRingSegementEnd"),
	EI("DT_TriggerCylinderHeavy", "m_mortarRingSegementStart"),
	EF("DT_TriggerCylinderHeavy", "m_nextLaunchTime"),

	// --- DT_TriggerPointGravity (1) ---
	EI("DT_TriggerPointGravity", "m_constantPullStregnth"),

	// --- DT_Turret (4) ---
	EI("DT_Turret", "m_driver"),
	EF("DT_Turret", "m_driverDetachTime"),
	EI("DT_Turret", "m_driverState"),
	EI("DT_Turret", "m_turretWeapon"),

	// --- DT_VortexSphere (1) ---
	EI("DT_VortexSphere", "m_iMaxHealth"),

	// --- DT_WeaponX (21) ---
	EI("DT_WeaponX", "m_curReactiveSkinKillCount"),
	EI("DT_WeaponX", "m_curReactiveSkinKnockdownCount"),
	EI("DT_WeaponX", "m_energizeState"),
	// m_lastEnergizeState: no such RecvProp exists (only
	// m_energizeState/m_startEnergizingTime/m_energizedEndTime), so appending it
	// would deliver nothing while writing modVars+0. See [ENERGIZE-WIRE].
	ET("DT_WeaponX", "m_energizedEndTime"),
	EI("DT_WeaponX", "m_fullyHeated"),
	EF("DT_WeaponX", "m_heatValue"),
	EF("DT_WeaponX", "m_heatValueOnLastFire"),
	EI("DT_WeaponX", "m_lastTossedGrenade"),
	EI("DT_WeaponX", "m_lockedSet"),
	EI("DT_WeaponX", "m_modBitfieldDisabled"),
	EI("DT_WeaponX", "m_offhandSwitchSlot"),
	EI("DT_WeaponX", "m_parentTurret"),
	EF("DT_WeaponX", "m_scriptFloat0"),
	EF("DT_WeaponX", "m_scriptVectorTransitionDuration"),
	EF("DT_WeaponX", "m_scriptVectorTransitionStartTime"),
	ET("DT_WeaponX", "m_startEnergizingTime"),
	EI("DT_WeaponX", "m_targetingLaserEnabledScript"),
	EV("DT_WeaponX", "m_scriptVector"),
	EV("DT_WeaponX", "m_scriptVectorTransitionEnd"),
	EV("DT_WeaponX", "m_scriptVectorTransitionStart"),

	// --- DT_WeaponX_PredictingClientOnly (1) --- S21 split S3's single shot counter into m_shotIndexForSpread (viewkick pattern row + weapon spread) and m_shotCount (loop-sound parity + fire-rate lerp).
	// S3 has one counter and uses it for both, published as m_shotCount; this second prop fans that same counter out to the client's spread index.
	EI_AFTER("DT_WeaponX_PredictingClientOnly", "m_shotIndexForSpread", "m_burstFireIndex"),

	// --- DT_WeaponX_LocalWeaponData (1) --- S21 client RecvTable expects m_infiniteAmmoState between m_ammoInStockpile and m_lifetimeShots (s21_dt_schema.h).
	// Value-proxied from the SDK sidecar (weapon_script_vars.cpp) -- NO entity-memory slot; see the value proxy below.
	EI("DT_WeaponX_LocalWeaponData", "m_infiniteAmmoState"),

	// --- DT_WeaponPlayerData (1) ---
	// Flat on DT_WeaponX, not nested under DT_WeaponPlayerData: the nested
	// subtree's flattened indices diverge between the dedi's delta/baseline
	// paths and the client's decode, so the bit never lands. The client
	// binds by leaf name either way.
	EI("DT_WeaponX", "m_akimboDisabled"),

	// --- DT_Zipline (3) ---
	EF("DT_Zipline", "m_ziplineMountReverseDistance"),
	EI("DT_Zipline", "m_ziplinePreventManualDetach"),
	EV("DT_Zipline", "m_ropeColorModulation"),
};
const int kNumExtendProps = sizeof(s_extendProps) / sizeof(s_extendProps[0]);

#undef EI
#undef EF
#undef ET
#undef EV
#undef E64
#undef ES
#undef EI_AFTER
#undef EF_AFTER
#undef ET_AFTER
#undef EV_AFTER
#undef E64_AFTER

// SPROP_* flag overrides on EXISTING S3 SendProps.
// The dedi's native flags don't always match what the S21 client decoder expects (e.g.
const DTFlagOverride s_flagOverrides[] = {
	{ "DT_PlayerDecoy", "m_currentClass", 0x1,   false },
	{ "DT_PlayerDecoy", "m_localAngles",  0x208, true  },
	// Pairs with the predictableFlags widening below -- see kPredictableFlagsBits
	// for why the sign is load-bearing on bit 7.
	{ "DT_Local",       "predictableFlags", 0x1, true  },
	// Keyed on the S3 name (flag overrides run before the rename pass).
	// Low 16 match the client's own 0x1.
	{ "DT_Local",       "m_nDuckTransitionTimeMsecs", 0x1, true  },
};
constexpr int kNumFlagOverrides = sizeof(s_flagOverrides) / sizeof(s_flagOverrides[0]);

// SendProp m_nBits widenings on EXISTING S3 SendProps whose native width is too narrow for the ported S21 content.
// Concretely: m_effectIndex ships 11 bits (max 2048) while S21's ParticleEffectNames grows past that (~3774 entries, maxEnt 4096), so wide indices truncate on send.
struct DTBitWidthOverride
{
	const char* tableName;
	const char* propName;
	int         nBits;
	bool        recursive;
};
// These props ship with SP_FLAGS == 0, i.e.
// SIGNED, and the SIGN IS LOAD- BEARING: the S21 client keeps TWO particle index arrays and picks between them on the sign of the index -- positive is the networked ParticleEffectNames namespace, negative is client-local, indexed by ~idx.
static constexpr int kEffectIndexBits = 13;

// DT_SoundData.m_networkTableID -- the sentinel width, not a capacity widening, and the reason every server sound was inaudible on the S21 client.
// With miles_server_useSoundIDTable 0 the dedi does not put the Miles hash in the SoundIDs string table; it sends the full 64-bit hash in m_soundID and writes -1 into m_networkTableID to mean "no table row, use the hash".
static constexpr int kSoundTableIdBits = 11;

// Trigger types are BIT FLAGS on the S21 client -- jump pad 1, tesla trap 2, gravity lift 4, blackhole 8, mortar ring segment 16, gravity cannon 32.
// S3 only ever had the first two, so both props that carry them ship 2 bits wide (max 3) and anything above a tesla trap is truncated away: a mortar-ring trigger arrives as 0, and an ignore mask of jumppad|gravitylift|blackhole (13) arrives as 1, so the client keeps predicting movement through lifts and blackholes. 6 bits carries the whole flag set and any mask over it.
static constexpr int kTriggerTypeBits = 6;

// DT_Local.predictableFlags is a BIT FIELD, and S3 only ever had the low six flags, so its prop ships 6 bits wide.
// S21 uses eight, so bits 6-7 are dropped on the dedi's send and the client's predicted byte can never match the acked one -- a rebase on every command that sets either bit, ~10% of all of them.
static constexpr int kPredictableFlagsBits = 8;

static ConVar bridge_pflags_slots8("bridge_pflags_slots8", "1", FCVAR_RELEASE,
	"Widen the two predictableFlags slot-mask immediates from 6 slots (0x3F) to the "
	"client's 8 (0xFF / full clear) so bits 6-7 track the client lifecycle. Pairs with "
	"the DT_Local.predictableFlags bit-width override. 0 = leave engine immediates.");

static const DTBitWidthOverride s_bitWidthOverrides[] = {
	{ "DT_Local",                          "predictableFlags", kPredictableFlagsBits, true },

	{ "DT_TEScriptParticleSystem",         "m_effectIndex", kEffectIndexBits, false },
	{ "DT_TEScriptParticleSystemOnEntity", "m_effectIndex", kEffectIndexBits, false },

	{ "DT_SoundData",                      "m_networkTableID", kSoundTableIdBits, false },

	{ "DT_TriggerCylinderHeavy", "m_triggerType",                 kTriggerTypeBits, false },
	{ "DT_BaseEntity",           "m_ignorePredictedTriggerFlags", kTriggerTypeBits, false },

	// model-index widen 13->14.
	// S3's native m_nModelIndex SendProps are 13-bit (max idx 8191).
	{ "DT_BaseEntity",            "m_nModelIndex",                    14, false },
	{ "DT_BaseAnimating",         "m_animModelIndex",                 14, false },
	{ "DT_BaseViewModel",         "m_nModelIndex",                    14, false },
	{ "DT_BaseViewModel",         "m_animModelIndex",                 14, false },
	{ "DT_Beam",                  "m_nModelIndex",                    14, false },
	{ "DT_BaseBeam",              "m_nModelIndex",                    14, false },
	{ "DT_BoneFollower",          "m_nModelIndex",                    14, false },
	{ "DT_BoneFollower",          "m_modelIndex",                     14, false },
	{ "DT_DeathBoxProp",          "m_nModelIndex",                    14, false },
	{ "DT_DynamicPropLightweight","m_nModelIndex",                    14, false },
	{ "DT_FuncBrushLightweight",  "m_nModelIndex",                    14, false },
	{ "DT_GrappleHook",           "m_nModelIndex",                    14, false },
	{ "DT_Projectile",            "m_nModelIndex",                    14, false },
	{ "DT_CPropDoor",             "m_nModelIndex",                    14, false },
	{ "DT_PropSurvival",          "m_nModelIndex",                    14, false },
	{ "DT_TEBreakModel",          "m_nModelIndex",                    14, false },
	{ "DT_TEPhysicsProp",         "m_nModelIndex",                    14, false },
	{ "DT_DoorMover",             "m_nModelIndex",                    14, false },
	{ "DT_ScriptMoverLightweight","m_nModelIndex",                    14, false },
	{ "DT_WeaponX",               "m_iWorldModelIndex",               14, false },
	{ "DT_WeaponX",               "m_worldModelIndexOverride",        14, false },
	{ "DT_WeaponX",               "m_holsterModelIndex",              14, false },
	{ "DT_WeaponX",               "m_droppedModelIndex",              14, false },
	{ "DT_WeaponPlayerData",      "m_customActivityAttachedModelIndex",14, false },
};
constexpr int kNumBitWidthOverrides = sizeof(s_bitWidthOverrides) / sizeof(s_bitWidthOverrides[0]);

//-----------------------------------------------------------------------------
// SNDC Array Resize -- modify existing Array SendProps in-place to match S21's element counts and repacked offsets. 5 tables x 5 arrays = 25 modifications.
// Array names in each table: m_bools, m_ranges, m_int32s, m_times, m_entities.

const SNDCTableSpec s_sndcSpecs[] = {
	// GLOBAL sized from the S21 client's actual SendTable layout; a short size makes the client decoder AV on every snapshot tick (infinite CL_CopyNewEntity retries, then NonFatalError recursion).
	// Mirrored offsets (not packed from 2832) keep bridge log byte offsets comparable with the client's.
	{ "DT_ScriptNetData_SNDC_GLOBAL", {
		{ "m_bools",    18, 1, 2944 },
		{ "m_ranges",   34, 2, 2962 },
		{ "m_int32s",   18, 4, 3032 },
		{ "m_times",    26, 4, 3104 },
		{ "m_entities", 10, 4, 3208 },
		// 3384, not 3392: this is what GetEntitySize reports and what the factory node declares, and the Create alloc immediate is patched to the family table's 3384.
		// Reporting 8 bytes more than Create allocates hands every replication path a tail the entity does not own.
	}, 3384 }, // S21 end=3248
	{ "DT_ScriptNetData_SNDC_PLAYER_GLOBAL", {
		{ "m_bools",    18, 1, 2832 },
		{ "m_ranges",   30, 2, 2850 },
		{ "m_int32s",   34, 4, 2912 },
		{ "m_times",    14, 4, 3048 },
		{ "m_entities",  6, 4, 3104 },
	}, 3384 }, // S21 end=3128, +256 margin
	{ "DT_ScriptNetData_SNDC_PLAYER_EXCLUSIVE", {
		{ "m_bools",    32, 1, 2832 },
		{ "m_ranges",   30, 2, 2864 },
		{ "m_int32s",    6, 4, 2924 },
		{ "m_times",    10, 4, 2948 },
		{ "m_entities", 22, 4, 2988 },
	}, 3332 }, // S21 end=3076, +256 margin
	{ "DT_ScriptNetData_SNDC_TITAN_SOUL", {
		{ "m_bools",    10, 1, 2832 },
		{ "m_ranges",   18, 2, 2842 },
		{ "m_int32s",    4, 4, 2880 },
		{ "m_times",    10, 4, 2896 },
		{ "m_entities",  6, 4, 2936 },
	}, 3216 }, // S21 end=2960, +256 margin
	{ "DT_ScriptNetData_SNDC_DEATH_BOX", {
		{ "m_bools",     5, 1, 2832 },
		{ "m_ranges",    4, 2, 2838 },
		{ "m_int32s",   14, 4, 2848 },
		{ "m_times",     3, 4, 2904 },
		{ "m_entities",  3, 4, 2916 },
	}, 3184 }, // S21 end=2928, +256 margin
};
const int kNumSNDCSpecs = sizeof(s_sndcSpecs) / sizeof(s_sndcSpecs[0]);

// AssignedOffset: dt_extend.h
AssignedOffset s_assigned[512] = {};
int s_assignedCount = 0;
bool s_applied = false;

char (*v_SendTable_Init)(void** tables, int count) = nullptr;

// CSendTablePrecalc builder.
// SendTable_Init calls this per table after allocating the precalc object and back-linking it (precalc+0x48 = table).
void (*v_SendTable_BuildPrecalc)(void* precalc, unsigned char bServerSide) = nullptr;

//-----------------------------------------------------------------------------
// Factory list head -- resolved at GetVar time from the factory registration function's body.
// Each factory node: +0x00 className (char*), +0x08 SendTable*, +0x10 next (Factory*), +0x1C allocSize (int), +0x20 classID (int).

uintptr_t* g_pFactoryListHead = nullptr;

//-----------------------------------------------------------------------------
// Step 6: NonRewind entity creation Factory registration function Signature: __int64 __fastcall RegisterServerClass(void* factoryStruct, const char* className, void* sendTable, int allocSize)
typedef __int64 (__fastcall* PFN_RegisterServerClass)(void*, const char*, void*, int);
PFN_RegisterServerClass v_RegisterServerClass = nullptr;

// GLOBAL creation callback: -- called at level-init
typedef __int64 (__fastcall* PFN_GlobalCreationCB)(__int64 a1);
PFN_GlobalCreationCB v_GlobalCreationCB = nullptr;

// CreateEntityByName factory interface
PFN_GetEntityFactory v_GetEntityFactory = nullptr;

// ActivateEntity
typedef void (__fastcall* PFN_ActivateEntity)(__int64 classID, __int64 entity);
PFN_ActivateEntity v_ActivateEntity = nullptr;

// DispatchSpawn: marks entity spawned + links into snapshot tracking
typedef __int64 (__fastcall* PFN_DispatchSpawn)(__int64 entity);
PFN_DispatchSpawn v_DispatchSpawn = nullptr;

// SNDC entity init
typedef void (__fastcall* PFN_SNDCEntityInit)(void* entity, __int64 a2, int catIndex);
PFN_SNDCEntityInit v_SNDCEntityInit = nullptr;

// NonRewind entity global pointer (the GLOBAL singleton's entity slot for GLOBAL)
static uintptr_t s_nonRewindEntity = 0;

// Static factory structs for our NonRewind ServerClasses (persists for process lifetime) Both the base (CScriptNetData_SNDC_GLOBAL_NON_REWIND) and wrapper (CScriptNetDataGlobalNonRewind) need factory nodes so the engine sends svc_SendTable for both and the S21 client can build decoders that expand the wrapper's DataTable prop into the base's SNDC RecvProps.
static uint8_t s_nonRewindBaseFactory[48] = {};
static uint8_t s_nonRewindFactory[48] = {};

// Static SendTable + props for NonRewind (persists for process lifetime)
// NR_SENDTABLE_SIZE: dt_extend.h
static uint8_t s_nrSendTable[NR_SENDTABLE_SIZE] = {};
static uint8_t s_nrWrapperTable[NR_SENDTABLE_SIZE] = {};
static uint8_t s_nrProps[11 * SP_SIZE] = {}; // 5 templates + 5 arrays + 1 DataTable
static uint8_t s_nrWrapperProp[SP_SIZE] = {}; // 1 DataTable prop for wrapper

// NonRewind nElements
static const int s_nrNElements[5] = { 18, 18, 10, 18, 6 };

static bool s_nonRewindRegistered = false;

// Per-entity vtable copy for the NonRewind entity (so GetServerClass can be patched without affecting the shared GLOBAL vtable).
// MSVC vtable layout [-1] = const RTTI Complete Object Locator* (read by __RTDynamicCast) [ 0..N ] = virtual functions We store the RTTI locator at s_nrVtableBuf[0] and the vfuncs at [1..], then hand entity->vtable = &s_nrVtableBuf[1] so *(vtable-8) == the original RTTI.
static uintptr_t s_nrVtableBuf[513] = {};

// GetServerClass stub: returns our NonRewind ServerClass node.
static void* __fastcall NonRewind_GetServerClass(void* /*thisptr*/)
{
	return (void*)s_nonRewindFactory;
}

//-----------------------------------------------------------------------------
// Step 6b: CEntityFactory registration for the NonRewind class.
// SV_CreateBaseline calls CreateBaselineEntity(className) for each ServerClass.
//-----------------------------------------------------------------------------
static uintptr_t s_nrEntFactoryVtable[8] = {};
static uintptr_t s_nrEntFactoryObj = 0;

// s_globalFactoryRawCreate: resolved to the GLOBAL factory's raw Create (vtable[0]).
// This allocates the C++ entity object WITHOUT calling ActivateEntity -- the engine's caller does ActivateEntity + Spawn separately.
static uintptr_t s_globalFactoryRawPtr = 0;

static uintptr_t __fastcall NonRewind_FactoryCreate(uintptr_t /*unused*/, __int64 a2)
{
	if (!s_globalFactoryRawPtr) return 0;

	typedef uintptr_t (__fastcall* PFN_RawCreate)(uintptr_t, __int64);
	PFN_RawCreate pfnRawCreate = *(PFN_RawCreate*)(*(uintptr_t*)s_globalFactoryRawPtr);
	uintptr_t networkable = pfnRawCreate(s_globalFactoryRawPtr, a2);
	if (!networkable)
	{
		Warning(eDLL_T::ENGINE,
			"[dt_extend] NonRewind factory: GLOBAL raw Create failed\n");
		return 0;
	}

	*(uintptr_t*)(networkable + 0x10) = (uintptr_t)s_nonRewindFactory;

	// Only capture the entity pointer if Hook_ActivateEntity hasn't already caught the engine's native "global_non_rewinding" entity.
	// SV_CreateBaseline calls this factory to build a TEMP entity for the instance baseline -- that temp is freed after encoding.
	uintptr_t entity = *(uintptr_t*)(networkable + 8);
	if (entity && !s_nonRewindEntity)
	{
		s_nonRewindEntity = entity;
		g_pScriptNetDataNonRewindEnt = (void*)entity;
		// Canary the tail padding [3080, 3104). m_entities (GLOBAL layout, 10 slots * 4 bytes) ends at 3080; entity allocsize is 3104.
		// The 24-byte gap is unused by any SNDC array; any write here is OOB from one of our SetGlobalNonRewindNet* paths (or engine code).
		if (HeapCanary::Armed())
		{
			uint8_t* pad = reinterpret_cast<uint8_t*>(entity) + 3080;
			bool padZero = true;
			for (int i = 0; i < 24; ++i)
			{
				if (pad[i] != 0) { padZero = false; break; }
			}
			if (padZero)
			{
				HeapCanary::RegisterRegion("nr-entity-pad", pad, 24);
			}
			else
			{
				Warning(eDLL_T::ENGINE,
					"[CANARY] nr-entity-pad: bytes [+3080..+3104) are NON-ZERO "
					"at entity creation -- skipping canary install. First "
					"qword: 0x%016llX (offset assumption may be wrong)\n",
					(unsigned long long)*reinterpret_cast<uint64_t*>(pad));
			}
		}
	}
	if (entity)
	{
		int16_t edict = -1;
		uint32_t handle = 0xFFFFFFFF;
		if (DTExtend_IsSafeToRead(reinterpret_cast<const void*>(entity + 0x58), sizeof(int16_t)))
			edict = *reinterpret_cast<const int16_t*>(entity + 0x58);
		if (DTExtend_IsSafeToRead(reinterpret_cast<const void*>(entity + 0x578), sizeof(uint32_t)))
			handle = *reinterpret_cast<const uint32_t*>(entity + 0x578);
		Warning(eDLL_T::ENGINE,
			"[dt_extend] NonRewind factory: entity=0x%p edict=%d handle=0x%08X (idx=%d) %s\n",
			(void*)entity, (int)edict, handle, (int)(handle & 0xFFFF),
			s_nonRewindEntity == entity ? "(CAPTURED)" : "(baseline temp, NOT captured)");
	}

	Warning(eDLL_T::ENGINE,
		"[dt_extend] NonRewind factory: entity created, ServerClass overridden\n");
	return networkable;
}

static __int64 NonRewind_FactoryGetSize()
{
	return 3384;
}

//-----------------------------------------------------------------------------
// SNDC native factory GetSize override + per-entity tail canary The 5 native CScriptNetData_SNDC_* factories are singleton vtable pointers in engine.data.
// CEntityFactory::GetEntitySize (vtable slot[2]) is what the allocator reads -- NOT the ServerClass node's m_iAllocSize field Step 6's growth loop edits -- so an entity left at S3-native 3008 overflows the next mspace chunk footer once scripts write S21-count vars (heap corruption at mspace_free / tmalloc_large).
//-----------------------------------------------------------------------------
__int64 SNDC_GLOBAL_GetSize()           { return s_sndcSpecs[0].requiredAllocSize; }
__int64 SNDC_PLAYER_GLOBAL_GetSize()    { return s_sndcSpecs[1].requiredAllocSize; }
__int64 SNDC_PLAYER_EXCLUSIVE_GetSize() { return s_sndcSpecs[2].requiredAllocSize; }
__int64 SNDC_TITAN_SOUL_GetSize()       { return s_sndcSpecs[3].requiredAllocSize; }
__int64 SNDC_DEATH_BOX_GetSize()        { return s_sndcSpecs[4].requiredAllocSize; }

// kNumSNDCFamily: dt_extend.h

struct SNDCFamilyEntry
{
	const char* dictName;     // entity factory dictionary key
	const char* canaryTag;    // short name for canary log
	int         entitySize;   // for canary placement (entity[size-8..size))
	uintptr_t   getSizeStub;  // 0 = don't patch slot[2] (wrapper case: leave native)
};

// Forward decls so the family table can reference the per-idx Create wrappers.
static uintptr_t __fastcall SNDC_Family_Create_0(uintptr_t self, __int64 a2);
static uintptr_t __fastcall SNDC_Family_Create_1(uintptr_t self, __int64 a2);
static uintptr_t __fastcall SNDC_Family_Create_2(uintptr_t self, __int64 a2);
static uintptr_t __fastcall SNDC_Family_Create_3(uintptr_t self, __int64 a2);
static uintptr_t __fastcall SNDC_Family_Create_4(uintptr_t self, __int64 a2);
static uintptr_t __fastcall SNDC_Family_Create_5(uintptr_t self, __int64 a2);

static const SNDCFamilyEntry s_sndcFamily[kNumSNDCFamily] = {
	// 5 native SNDC factories: patch slot[2] AND the engine-Create alloc-imm to enforce the grown size.
	// Target = requiredAllocSize per s_sndcSpecs.
	{ "script_net_data_SNDC_GLOBAL",           "sndc-GLOBAL",  3384, (uintptr_t)&SNDC_GLOBAL_GetSize           },
	{ "script_net_data_SNDC_PLAYER_GLOBAL",    "sndc-PG",      3384, (uintptr_t)&SNDC_PLAYER_GLOBAL_GetSize    },
	{ "script_net_data_SNDC_PLAYER_EXCLUSIVE", "sndc-PE",      3332, (uintptr_t)&SNDC_PLAYER_EXCLUSIVE_GetSize },
	{ "script_net_data_SNDC_TITAN_SOUL",       "sndc-TS",      3216, (uintptr_t)&SNDC_TITAN_SOUL_GetSize       },
	{ "script_net_data_SNDC_DEATH_BOX",        "sndc-DB",      3184, (uintptr_t)&SNDC_DEATH_BOX_GetSize        },
	// CScriptNetDataGlobal wrapper: this factory creates the GLOBAL SNDC singleton entity AND every NonRewind entity (via NonRewind_FactoryCreate's pfnRawCreate).
	// Same S21-grown-allocation requirement as the GLOBAL base factory above -- bump to 3384. getSizeStub stays 0 (vtable[2] reads return engine-native size to replication paths, which is fine: replication reads nElements * stride, not allocation size).
	{ "script_net_data_global",                "sndc-WRAPPER", 3384, 0                                       },
};

// Slot layout (per CEntityFactory<X>::vftable)
// [0] = Create, [1] = Destroy, [2] = GetEntitySize, [3..7] = misc.
static uintptr_t s_sndcFamilyVtables[kNumSNDCFamily][8] = {};
static uintptr_t s_sndcFamilyOriginalCreate[kNumSNDCFamily] = {};
bool s_sndcFamilyCreateOk[kNumSNDCFamily] = {};

// Per-class Create wrapper. Chains to the captured original Create; overflow
// detection is owned by the mspace_free cookie hook, so nothing extra here.
static uintptr_t SNDC_Family_CreateCommon(int idx, uintptr_t self, __int64 a2)
{
	if (idx < 0 || idx >= kNumSNDCFamily || !s_sndcFamilyOriginalCreate[idx])
		return 0;
	typedef uintptr_t(__fastcall* PFN_Create)(uintptr_t, __int64);
	PFN_Create pfn = (PFN_Create)s_sndcFamilyOriginalCreate[idx];
	return pfn(self, a2);
}

static uintptr_t __fastcall SNDC_Family_Create_0(uintptr_t self, __int64 a2) { return SNDC_Family_CreateCommon(0, self, a2); }
static uintptr_t __fastcall SNDC_Family_Create_1(uintptr_t self, __int64 a2) { return SNDC_Family_CreateCommon(1, self, a2); }
static uintptr_t __fastcall SNDC_Family_Create_2(uintptr_t self, __int64 a2) { return SNDC_Family_CreateCommon(2, self, a2); }
static uintptr_t __fastcall SNDC_Family_Create_3(uintptr_t self, __int64 a2) { return SNDC_Family_CreateCommon(3, self, a2); }
static uintptr_t __fastcall SNDC_Family_Create_4(uintptr_t self, __int64 a2) { return SNDC_Family_CreateCommon(4, self, a2); }
static uintptr_t __fastcall SNDC_Family_Create_5(uintptr_t self, __int64 a2) { return SNDC_Family_CreateCommon(5, self, a2); }

static const uintptr_t s_sndcFamilyCreateWrappers[kNumSNDCFamily] = {
	(uintptr_t)&SNDC_Family_Create_0,
	(uintptr_t)&SNDC_Family_Create_1,
	(uintptr_t)&SNDC_Family_Create_2,
	(uintptr_t)&SNDC_Family_Create_3,
	(uintptr_t)&SNDC_Family_Create_4,
	(uintptr_t)&SNDC_Family_Create_5,
};

//-----------------------------------------------------------------------------
// Per-class Create immediate patcher.
// Each CEntityFactory<X>::Create is a per-class template instantiation with the entity's sizeof expanded as compile-time `mov edx, IMM` and `mov r8d, IMM` immediates (the mspace_malloc arg + the memset arg).
static int ReadEntityCreateAllocImm(uintptr_t createFn)
{
	if (!createFn) return 0;
	const uint8_t* code = reinterpret_cast<const uint8_t*>(createFn);
	constexpr size_t kScanWindow = 0x80;

	for (size_t i = 0; i + 5 <= kScanWindow; ++i)
	{
		if (code[i] != 0xBA) continue;
		const uint32_t imm = *reinterpret_cast<const uint32_t*>(code + i + 1);
		if (imm < 0x100 || imm > 0x100000) continue;
		for (size_t j = i + 5; j + 6 <= kScanWindow; ++j)
			if (code[j] == 0x41 && code[j + 1] == 0xB8 &&
				*reinterpret_cast<const uint32_t*>(code + j + 2) == imm)
				return static_cast<int>(imm);
	}
	return 0;
}

int PatchEntityCreateAllocImm(uintptr_t createFn, uint32_t targetSize, const char* tag,
	uint32_t expectedNativeSize)
{
	if (!createFn) return 0;
	uint8_t* code = reinterpret_cast<uint8_t*>(createFn);
	constexpr size_t kScanWindow = 0x80;

	// Locate the first `mov edx, imm32` whose immediate is a plausible entity
	// size (0x800..0x2000 byte range).
	int   allocOff   = -1;
	uint32_t nativeSize = 0;
	for (size_t i = 0; i + 5 <= kScanWindow; ++i)
	{
		if (code[i] != 0xBA) continue;
		uint32_t imm = *reinterpret_cast<uint32_t*>(code + i + 1);
		const bool match = expectedNativeSize
			? (imm == expectedNativeSize)
			: (imm >= 0x800 && imm <= 0x2000);
		if (match)
		{
			allocOff   = static_cast<int>(i);
			nativeSize = imm;
			break;
		}
	}
	if (allocOff < 0)
	{
		if (expectedNativeSize)
			Warning(eDLL_T::ENGINE,
				"[ENT-SIZE-PATCH] %s: no `mov edx, %u` in first 0x%zX bytes -- the class layout "
				"moved, patch REFUSED\n",
				tag, expectedNativeSize, kScanWindow);
		else
			Warning(eDLL_T::ENGINE,
				"[ENT-SIZE-PATCH] %s: no plausible `mov edx, imm32` found in first 0x%zX bytes -- SKIPPED\n",
				tag, kScanWindow);
		return 0;
	}
	// -1, not 0: the requirement IS met, there is just nothing left to write.
	// Two S21 classes sharing a parent both ask for the grow, and the second one arrives after the first already applied it -- reporting that as failure fed the caller's fail-safe and disarmed a class whose backing was fine.
	if (nativeSize >= targetSize)
	{
		Warning(eDLL_T::ENGINE,
			"[ENT-SIZE-PATCH] %s: native %u already >= target %u -- already grown\n",
			tag, nativeSize, targetSize);
		return -1;
	}

	DWORD oldProt = 0;
	if (!VirtualProtect(code, kScanWindow, PAGE_EXECUTE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::ENGINE,
			"[ENT-SIZE-PATCH] %s: VirtualProtect failed (GetLastError=%lu)\n",
			tag, GetLastError());
		return 0;
	}

	int patches = 0;
	DWORD scratch = 0;

	// Locate BOTH immediates before writing either.
	// Growing only the alloc leaves Create zero-initializing just the native prefix while replication serializes the appended tail -- uninitialized heap on the wire.
	int memsetOff = -1;
	for (size_t i = static_cast<size_t>(allocOff) + 5; i + 6 <= kScanWindow; ++i)
	{
		if (code[i] == 0x41 && code[i + 1] == 0xB8 &&
			*reinterpret_cast<uint32_t*>(code + i + 2) == nativeSize)
		{
			memsetOff = static_cast<int>(i);
			break;
		}
	}
	if (memsetOff < 0)
	{
		VirtualProtect(code, kScanWindow, oldProt, &scratch);
		Warning(eDLL_T::ENGINE,
			"[ENT-SIZE-PATCH] %s: matching memset imm NOT FOUND -- grow REFUSED "
			"(a half-grow would ship an uninitialized alloc tail)\n", tag);
		return 0;
	}

	// Patch the alloc immediate.
	*reinterpret_cast<uint32_t*>(code + allocOff + 1) = targetSize;
	patches++;

	// Patch the matching memset immediate: `mov r8d, imm32` = 41 B8 XX XX XX XX.
	*reinterpret_cast<uint32_t*>(code + memsetOff + 2) = targetSize;
	patches++;

	VirtualProtect(code, kScanWindow, oldProt, &scratch);
	FlushInstructionCache(GetCurrentProcess(), code, kScanWindow);
	return patches;
}

int DTExtend_OverrideSNDCFactoryGetSize()
{
	if (!v_GetEntityFactory)
	{
		Warning(eDLL_T::ENGINE,
			"[dt_extend] SNDC GetSize override SKIPPED: v_GetEntityFactory unresolved\n");
		return 0;
	}
	void** dict = (void**)v_GetEntityFactory();
	if (!dict || !dict[0])
	{
		Warning(eDLL_T::ENGINE,
			"[dt_extend] SNDC GetSize override SKIPPED: dict unresolved\n");
		return 0;
	}
	typedef uintptr_t(__fastcall* PFN_DictFindByName)(void**, const char*);
	PFN_DictFindByName pfnFind = *(PFN_DictFindByName*)((uintptr_t)dict[0] + 0x18);

	int patched = 0;
	for (int i = 0; i < kNumSNDCFamily; ++i)
		s_sndcFamilyCreateOk[i] = false;
	for (int i = 0; i < kNumSNDCFamily; ++i)
	{
		const SNDCFamilyEntry& fe = s_sndcFamily[i];
		uintptr_t factoryPtr = pfnFind(dict, fe.dictName);
		if (!factoryPtr)
		{
			Warning(eDLL_T::ENGINE,
				"[dt_extend] SNDC family '%s' NOT FOUND in dict -- skipped\n",
				fe.dictName);
			continue;
		}
		uintptr_t engineVtable = *(uintptr_t*)factoryPtr;
		memcpy(s_sndcFamilyVtables[i], (void*)engineVtable, sizeof(s_sndcFamilyVtables[i]));

		// Capture original Create before patching for the tail-canary wrapper.
		s_sndcFamilyOriginalCreate[i] = s_sndcFamilyVtables[i][0];
		s_sndcFamilyVtables[i][0] = s_sndcFamilyCreateWrappers[i];

		const int nCreate = PatchEntityCreateAllocImm(s_sndcFamilyOriginalCreate[i],
			static_cast<uint32_t>(fe.entitySize), fe.canaryTag);
		s_sndcFamilyCreateOk[i] = (nCreate != 0);
		if (s_sndcFamilyCreateOk[i] && fe.getSizeStub)
			s_sndcFamilyVtables[i][2] = fe.getSizeStub;
		else if (!s_sndcFamilyCreateOk[i])
		{
			Error(eDLL_T::ENGINE, EXIT_FAILURE,
				"[dt_extend] SNDC family '%s': Create-imm grow failed -- "
				"refusing mixed native nElem against S21 Recv\n",
				fe.dictName);
		}

		*(uintptr_t*)factoryPtr = (uintptr_t)s_sndcFamilyVtables[i];

		patched++;
	}
	return patched;
}

//-----------------------------------------------------------------------------
// DTExtend_GrowWorldEntity -- give CWorld real backing memory for the deathfield arrays.
// The native S3 CWorld is only ~0xC90 (3216) bytes (verified at runtime chunk head 0xca3); its data ends right after m_World* fields.
//-----------------------------------------------------------------------------
int DTExtend_GrowWorldEntity()
{
	// Unconditional + idempotent: growing the singleton CWorld to 8192 is safe
	// even without deathfield (zeroed trailing memory nothing reads), and it
	// matches the dt_extend DT_World base-offset assumption of 8192.
	if (!v_GetEntityFactory)
	{
		Warning(eDLL_T::ENGINE,
			"[dt_extend] world grow SKIPPED: v_GetEntityFactory unresolved\n");
		return 0;
	}
	void** dict = (void**)v_GetEntityFactory();
	if (!dict || !dict[0])
	{
		Warning(eDLL_T::ENGINE,
			"[dt_extend] world grow SKIPPED: dict unresolved\n");
		return 0;
	}
	typedef uintptr_t(__fastcall* PFN_DictFindByName)(void**, const char*);
	PFN_DictFindByName pfnFind =
		*(PFN_DictFindByName*)((uintptr_t)dict[0] + 0x18);
	uintptr_t factoryPtr = pfnFind(dict, "worldspawn");
	if (!factoryPtr)
	{
		Warning(eDLL_T::ENGINE,
			"[dt_extend] 'worldspawn' factory NOT FOUND -- CWorld NOT grown; "
			"deathfield arrays will overrun the entity. ABORT-ish\n");
		return 0;
	}
	const uintptr_t vtable = *(uintptr_t*)factoryPtr;
	const uintptr_t createFn = *(uintptr_t*)vtable; // slot[0] = Create
	// 0x2000 (8192) comfortably covers the deathfield arrays (end ~0x1800) plus
	// margin for any future DT_World extended props (dt_extend default base 8192).
	const int n = PatchEntityCreateAllocImm(createFn, 0x2000, "world");
	Warning(eDLL_T::ENGINE,
		"[dt_extend] world entity grow: factory=0x%p createFn=0x%p -> %d "
		"immediate(s) patched (target alloc 0x2000)\n",
		(void*)factoryPtr, (void*)createFn, n);

	Bridge_ApplyParallelSendPolicy();
	return n;
}


int DTExtend_GetOffset(const char* tableName, const char* propName)
{
	if (!tableName || !propName) return -1;
	for (int i = 0; i < s_assignedCount; ++i)
	{
		if (s_assigned[i].tableName && s_assigned[i].propName &&
			!strcmp(s_assigned[i].tableName, tableName) && !strcmp(s_assigned[i].propName, propName))
		{
			// Any match counts: even offset -1 (proxied) means a caller asked.
			s_assigned[i].resolved = true;
			return s_assigned[i].offset;
		}
	}
	return -1;
}

bool DTExtend_Applied() { return s_applied; }
// ===========================================================================
// SYSTEM 02: RESHAPE HANDLERS
// ===========================================================================

// [TRIG-HEAVY-GROW] CTriggerCylinderHeavy ships a 3440-byte allocation whose native fields end at 3428, so the 25 appended props have nowhere to live.
// Grow the factory and they get real, zeroed, entity-owned storage; if the grow fails they stay zero-proxied at the old mid-class base.
bool s_triggerHeavyGrown = false;

// Grow CTriggerCylinderHeavy Create/memset (+ GetEntitySize when the stub matches)
// so the 25 appends land in entity-owned tail. Fail closed: leave s_triggerHeavyGrown
// false and the props stay zero-proxied at the old mid-class base.
void DTExtend_EnsureTriggerCylinderHeavyGrown(void)
{
	if (s_triggerHeavyGrown)
		return;

	if (!v_GetEntityFactory)
	{
		Warning(eDLL_T::ENGINE,
			"[TRIG-HEAVY-GROW] SKIPPED: v_GetEntityFactory unresolved -- "
			"DT_TriggerCylinderHeavy appends stay zero-proxied at base 2800\n");
		return;
	}
	void** dict = (void**)v_GetEntityFactory();
	if (!dict || !dict[0])
	{
		Warning(eDLL_T::ENGINE,
			"[TRIG-HEAVY-GROW] SKIPPED: factory dict unresolved -- "
			"DT_TriggerCylinderHeavy appends stay zero-proxied at base 2800\n");
		return;
	}

	typedef uintptr_t(__fastcall* PFN_DictFindByName)(void**, const char*);
	PFN_DictFindByName pfnFind =
		*(PFN_DictFindByName*)((uintptr_t)dict[0] + 0x18);
	uintptr_t factoryPtr = pfnFind(dict, "trigger_cylinder_heavy");
	if (!factoryPtr)
	{
		Warning(eDLL_T::ENGINE,
			"[TRIG-HEAVY-GROW] 'trigger_cylinder_heavy' NOT in the entity factory dict -- "
			"DT_TriggerCylinderHeavy appends stay zero-proxied at base 2800\n");
		return;
	}

	const uintptr_t createFn = *(uintptr_t*)(*(uintptr_t*)factoryPtr); // vtable slot[0]
	// != 0 is success: PatchEntityCreateAllocImm returns -1 when already grown.
	if (PatchEntityCreateAllocImm(createFn, (uint32_t)kTriggerHeavyGrownAlloc,
			"CTriggerCylinderHeavy", (uint32_t)kTriggerHeavyNativeAlloc) == 0)
	{
		Warning(eDLL_T::ENGINE,
			"[TRIG-HEAVY-GROW] could NOT grow CTriggerCylinderHeavy Create alloc %d -> %d -- "
			"falling back to zero-proxy at base 2800. Any append past the native tail has no "
			"backing and would write out of bounds.\n",
			kTriggerHeavyNativeAlloc, kTriggerHeavyGrownAlloc);
		return;
	}

	// GetEntitySize is a separate 6-byte stub (mov eax, imm32; retn) at vtable[2].
	// No shared helper patches that shape in this file -- only alloc/memset is covered by PatchEntityCreateAllocImm, and SNDC/S21Class replace the slot.
	{
		const uintptr_t getSizeFn = *((uintptr_t*)(*(uintptr_t*)factoryPtr) + 2);
		uint8_t* stub = reinterpret_cast<uint8_t*>(getSizeFn);
		const bool shapeOk = stub
			&& stub[0] == 0xB8
			&& *reinterpret_cast<uint32_t*>(stub + 1) == (uint32_t)kTriggerHeavyNativeAlloc
			&& stub[5] == 0xC3;
		if (!shapeOk)
		{
			Warning(eDLL_T::ENGINE,
				"[TRIG-HEAVY-GROW] GetEntitySize stub at 0x%p is not `mov eax, %d; ret` -- "
				"reported size left stale (alloc grow still stands)\n",
				(void*)getSizeFn, kTriggerHeavyNativeAlloc);
		}
		else
		{
			DWORD oldProt = 0;
			if (!VirtualProtect(stub, 6, PAGE_EXECUTE_READWRITE, &oldProt))
			{
				Warning(eDLL_T::ENGINE,
					"[TRIG-HEAVY-GROW] GetEntitySize VirtualProtect failed (GetLastError=%lu) -- "
					"reported size left stale (alloc grow still stands)\n",
					GetLastError());
			}
			else
			{
				*reinterpret_cast<uint32_t*>(stub + 1) = (uint32_t)kTriggerHeavyGrownAlloc;
				DWORD scratch = 0;
				VirtualProtect(stub, 6, oldProt, &scratch);
				FlushInstructionCache(GetCurrentProcess(), stub, 6);
			}
		}
	}

	// Raise FACT_ALLOCSIZE so the post-append backing audit sees the grown size
	// and does not force-disarm the window we just backed.
	if (g_pFactoryListHead && *g_pFactoryListHead)
	{
		uintptr_t node = *g_pFactoryListHead;
		for (int guard = 0; node && guard < 4096; ++guard)
		{
			const char* cn = *(const char**)(node + FACT_CLASSNAME);
			if (cn && !strcmp(cn, "CTriggerCylinderHeavy"))
			{
				if (*(int*)(node + FACT_ALLOCSIZE) < kTriggerHeavyGrownAlloc)
					*(int*)(node + FACT_ALLOCSIZE) = kTriggerHeavyGrownAlloc;
				break;
			}
			node = *(uintptr_t*)(node + FACT_NEXT);
		}
	}

	s_triggerHeavyGrown = true;
	Msg(eDLL_T::ENGINE,
		"[TRIG-HEAVY-GROW] CTriggerCylinderHeavy alloc %d -> %d; append window [%d, %d) now backed\n",
		kTriggerHeavyNativeAlloc, kTriggerHeavyGrownAlloc,
		kTriggerHeavyAppendBase, kTriggerHeavyGrownAlloc);
}

//-----------------------------------------------------------------------------
// Per-table base offsets: where to place extended props in entity memory.
// The raw (pre-flat) SendTable props have struct-relative offsets, NOT absolute entity offsets -- those get resolved during SetupFlatPropertyArray which runs AFTER us.
//-----------------------------------------------------------------------------
int DTExtend_BaseOffset(const char* tableName)
{
	// Base offsets: derived from S21 RecvTable highest matched prop + margin.
	// These land in allocation slack past the S3 entity's known fields.
	if (strcmp(tableName, "DT_BaseAnimating") == 0)         return 3800;
	// Value-proxied only. 7200 is inside CBaseCombatCharacter::m_weaponAnimEvents
	// (0x1800..0x5908), not slack. A NON-proxied append here aliases live anim events.
	if (strcmp(tableName, "DT_BaseCombatCharacter") == 0)   return 7200;
	if (strcmp(tableName, "DT_BaseEntity") == 0)            return 2500;
	if (strcmp(tableName, "DT_BaseGrenade") == 0)           return 11800;
	if (strcmp(tableName, "DT_BaseViewModel") == 0)         return 3500;
	if (strcmp(tableName, "DT_CPropDoor") == 0)             return 5700;
	if (strcmp(tableName, "DT_DynamicPropLightweight") == 0)return 2500;
	if (strcmp(tableName, "DT_GlobalNonRewinding") == 0)    return 2500;
	if (strcmp(tableName, "DT_InfoTarget") == 0)            return 2500;
	// [JETDRIVE] Value-proxied only -- never consumed today, same as DT_WeaponX_LocalWeaponData below. m_Local is embedded directly in CPlayer, and sizeof(CPlayer) is 32496 on this dedi (factory allocs and zeroes exactly that), so 20000 is NOT tail slack: it is mid-class, inside the base-class field span.
	// Never point a NON-proxied prop here.
	if (strcmp(tableName, "DT_LocalPlayerExclusive") == 0)  return 20000;
	// Real, measured slack -- not a margin guess: the S21Class registration allocates CLootRoller at 6064 while its S3 parent CPhysicsProp ends at 5200, so [5200, 6064) is 864 bytes no native code touches. m_tier lands at 5200 and m_hasVaultKey at 5204.
	if (strcmp(tableName, "DT_LootRoller") == 0)            return 5200;
	// Same measurement for the two prop_dynamic-backed S21 classes: the parent CDynamicProp allocates 4912 (Create immediate 0x1330) and the S21Class registration grows it to 6112, so [4912, 6112) is untouched by native code.
	// The 4 / 2 appended props fit in the first 16 bytes of that window.
	if (strcmp(tableName, "DT_LootGrabber") == 0)           return 4912;
	if (strcmp(tableName, "DT_CarePackageInsightProp") == 0) return 4912;
	if (strcmp(tableName, "DT_ParticleSystem") == 0)        return 2700;
	// Value-proxied only. 18700 is inside the same m_weaponAnimEvents blob
	// (CPlayer embeds BCC). AuditExtendBacking alloc-size check cannot see this.
	if (strcmp(tableName, "DT_Player") == 0)                return 18700;
	if (strcmp(tableName, "DT_PlayerDecoy") == 0)           return 6400;
	if (strcmp(tableName, "DT_PlayerVehicle") == 0)         return 7200;
	if (strcmp(tableName, "DT_Projectile") == 0)            return 5600;
	if (strcmp(tableName, "DT_PropSurvival") == 0)          return 3500;
	if (strcmp(tableName, "DT_RopeKeyframe") == 0)          return 2800;
	if (strcmp(tableName, "DT_ScriptMover") == 0)           return 6200;
	if (strcmp(tableName, "DT_ScriptMoverLightweight") == 0)return 6200;
	if (strcmp(tableName, "DT_ScriptProp") == 0)            return 2500;
	// Heavy: append base moves to the grown tail only when the factory grow
	// succeeded; otherwise mid-class 2800 + whole-table zero-proxy. PointGravity
	// is not grown -- stays zero-proxied at 2600.
	if (strcmp(tableName, "DT_TriggerCylinderHeavy") == 0)
		return s_triggerHeavyGrown ? kTriggerHeavyAppendBase : 2800;
	if (strcmp(tableName, "DT_TriggerPointGravity") == 0)   return 2600;
	if (strcmp(tableName, "DT_Turret") == 0)                return 6500;
	if (strcmp(tableName, "DT_VortexSphere") == 0)          return 2500;
	if (strcmp(tableName, "DT_WeaponX") == 0)               return 6100;
	// Value-proxied only -- the base is never consumed today.
	// Explicit entry so a future NON-proxied prop cannot fall through to the 8192 default, which lands INSIDE real S3 CWeaponX fields (modvars ~+7000, live fields observed out to +11400). verified any real slack before ever using this base.
	if (strcmp(tableName, "DT_WeaponX_LocalWeaponData") == 0) return 6240;
	// Value-proxied only (m_shotIndexForSpread) -- the base is never consumed.
	// Live CWeaponX fields run past +11400; there is no proven slack here, so a
	// future NON-proxied prop on this table must find its own verified base.
	if (strcmp(tableName, "DT_WeaponX_PredictingClientOnly") == 0) return 8192;
	// Value-proxied only (zipline_extend_state.cpp). CZipline's own fields start
	// at 2832 and CBaseEntity reaches 2800, so no base below that is slack.
	if (strcmp(tableName, "DT_Zipline") == 0)               return 2500;
	return 8192;
}

static void __fastcall Canon_ZeroProxy(void* pProp, void* pStruct,
	void* pData, void* pOut, int iElement, int objectID);
static void __fastcall Canon_InvalidEhandleProxy(void* pProp, void* pStruct,
	void* pData, void* pOut, int iElement, int objectID);
static void __fastcall Canon_NeverFiredTimeProxy(void* pProp, void* pStruct,
	void* pData, void* pOut, int iElement, int objectID);
static void __fastcall Canon_AllowAutoMoveProxy(void* pProp, void* pStruct,
	void* pData, void* pOut, int iElement, int objectID);

// DT_Player.m_passives tail-zero proxy.
// S3 backs 128 passives (2 Int64 words @ CPlayer+0x5FF0); S21's RecvProp is array[3] (192).
void __fastcall Passives_TailZeroProxy(void* pProp, void* pStruct,
	void* pData, void* pOut, int iElement, int objectID);

// DT_HighlightSettings.m_highlightTeamBits context-translate proxy.
// S3 carries the highlight as a (m_highlightServerContextID, m_highlightTeamBits) scalar pair in the DT_HighlightSettings sub-object (@ +0x218 / +0x21C); S21's RecvProp is a per-context Int32[8].
static void __fastcall Highlight_TeamBitsTranslateProxy(void* pProp, void* pStruct,
	void* pData, void* pOut, int iElement, int objectID);
static void __fastcall Highlight_GenericContextProxy(void* pProp, void* pStruct,
	void* pData, void* pOut, int iElement, int objectID);
static void __fastcall Highlight_FocusedProxy(void* pProp, void* pStruct,
	void* pData, void* pOut, int iElement, int objectID);

// DTExtendProxyFn: dt_extend.h

// Install-time bind: name known at rebuild/append only. Encode path never reads names.
DTExtendProxyFn DTExtend_ZeroProxyForProp(const char* propName);

// Gate for the comprehensive DT_HighlightSettings rebuild below.
// The S21 highlight subsystem diverged from S3 (only m_highlightTeamBits is name-shared): S3's raw 7-member sub-table leaves 6 nodes unmatched on the S21 client, which desyncs the ENTER-PVS instance-baseline decode.
static ConVar bridge_highlight_teambits("bridge_highlight_teambits", "1", FCVAR_RELEASE,
	"Rebuild the shared DT_HighlightSettings sub-table to the S21 client's exact "
	"6-member shape (translate m_highlightTeamBits, zero-fill the rest, drop S3-only "
	"members). Default 1 (ON). 0 = ship the raw S3 layout (reproduces the enter-PVS "
	"desync crash on highlightable entities).");

// m_modInventory 32-bit -> 64-bit widen.
// S3 networks 32 player-mod entries (double-jump and other gameplay mods) as 32-bit UNSIGNED Int; S21 widened each element to Int64.
static ConVar bridge_canon_modinv_widen("bridge_canon_modinv_widen", "1", FCVAR_RELEASE,
	"Canonical rebuild: 1 = retype m_modInventory's 32 elements Int32->Int64 and "
	"zero-extend the S3 32-bit value (preserve S3 player-mod data); 0 = leave synth-zero. "
	"Only meaningful when bridge_canonical_dt is 1.");

// m_consumableInventory 16-bit -> 32-bit widen.
// S3 networks the 32 carried-consumable slots (shields/batteries/heals/ordnance, keyed by a small type id -- the array behind SURVIVAL_AddToPlayerInventory/ConsumableInventory_Add, the mechanism that stores the bulk of general ground loot) as a packed 16-bit {type:u8, count:u8} value per slot

// 2-byte-stride array at CPlayer+0x5FAC, and the native DT registration in
// passes byte-width 2 / nBits 16 to the shared SendProp builder). The S21 client's own

// element as a full 32-bit Int (byte-width 4), matching the auto-generated verified schema (s21_dt_schema.h: s_sch_m_consumableInventory elements type 0 = DPT_Int).
// Same disease as m_modInventory above -- a native array that exists and works on both bridge endpoints, just never widened to match S21's wire width -- so the generic rebuild's name+type match (both sides type 0) blindly copies the narrower S3 SP_NBITS, truncating what reaches the client.
static ConVar bridge_canon_consumableinv_widen("bridge_canon_consumableinv_widen", "1", FCVAR_RELEASE,
	"Canonical rebuild: 1 = widen m_consumableInventory's 32 elements from the S3 native "
	"16-bit {type,count} pack to the S21-expected 32-bit Int (zero-extend, preserve S3 "
	"consumable/ordnance pickup data); 0 = leave the narrower S3 width (picked-up "
	"consumables never reach the client inventory). Only meaningful when bridge_canonical_dt is 1.");

// DPT_Time/Ticks props keep their native types; the S21 wire format is produced in
// snapshot_diag.cpp `bridge_time_encode` inside the DT_EncodePropValue hook
// (retyping the props corrupts the SNDC m_times native layout).

// S21 highlight array-child recv names. Both the dedi send and S21 recv use the
// bracketed "[000N]" convention (after bracketization), so the matcher binds by name.
static const char* const kHlElemNames[8] = {
	"[0000]", "[0001]", "[0002]", "[0003]", "[0004]", "[0005]", "[0006]", "[0007]" };

// Build one type-10 array-as-datatable member into destSlot (SP_SIZE bytes) by cloning a same-shaped S3 donor parent (m_highlightFunctionBits for Int32 arrays, m_highlightServerFadeBases for Float arrays): clone the donor's child SendTable, resize it to `count` elements, rename children to S21's "[000N]" recv convention, and install `childProxy` on each element.
// When stashCtxIndex, each element's m_Offset (+0x78) is set to its index so Highlight_TeamBitsTranslateProxy can place TeamBits at element == ServerContextID.
static bool Highlight_BuildArrayMember(uint8_t* destSlot, const uint8_t* donorParent,
	int count, const char* name, DTExtendProxyFn childProxy, bool stashCtxIndex)
{
	if (!destSlot || !donorParent || count <= 0 || count > 8)
		return false;
	uintptr_t donorChild      = *(const uintptr_t*)(donorParent + 0x70 /* m_pDataTable */);
	uint8_t*  donorChildProps = donorChild ? *(uint8_t**)(donorChild + ST_PROPS) : nullptr;
	const int donorN          = donorChild ? *(int*)(donorChild + ST_NPROPS) : 0;
	if (!donorChild || !donorChildProps || donorN < count)
		return false;

	uint8_t* newChild      = (uint8_t*)malloc(NR_SENDTABLE_SIZE);
	uint8_t* newChildProps = (uint8_t*)malloc((size_t)count * SP_SIZE);
	if (!newChild || !newChildProps)
	{
		free(newChild); free(newChildProps);
		return false;
	}
	memcpy(newChild, (void*)donorChild, NR_SENDTABLE_SIZE);
	memcpy(newChildProps, donorChildProps, (size_t)count * SP_SIZE);
	*(uint8_t**)(newChild + ST_PROPS)           = newChildProps;
	*(int*)(newChild + ST_NPROPS)               = count;
	*(const char**)(newChild + ST_NETTABLENAME) = name;
	*(uintptr_t*)(newChild + 0x4C0)             = 0;   // precalc -> engine rebuilds
	*(uintptr_t*)(newChild + 0x508)             = 0;
	for (int e = 0; e < count; ++e)
	{
		uint8_t* ep = newChildProps + (uint64_t)e * SP_SIZE;
		*(const char**)(ep + SP_VARNAME) = kHlElemNames[e];
		if (stashCtxIndex) *(int*)(ep + SP_OFFSET) = e;   // ctx index for the translate proxy
		*(uintptr_t*)(ep + 0x60) = (uintptr_t)childProxy;
	}
	memcpy(destSlot, donorParent, SP_SIZE);
	*(const char**)(destSlot + SP_VARNAME) = name;
	*(int*)(destSlot + SP_OFFSET)          = 0;   // DataTable proxy yields the sub-object base
	*(uintptr_t*)(destSlot + 0x70)         = (uintptr_t)newChild;
	return true;
}

// Build one scalar member into destSlot by cloning an S3 Int scalar donor
// (m_highlightServerContextID): inherit its type/nBits, rename, and install
// childProxy (or the zero proxy when null).
static void Highlight_BuildScalarMember(uint8_t* destSlot, const uint8_t* scalarDonor,
	const char* name, DTExtendProxyFn childProxy)
{
	memcpy(destSlot, scalarDonor, SP_SIZE);
	*(const char**)(destSlot + SP_VARNAME) = name;
	*(int*)(destSlot + SP_OFFSET)          = 0;
	*(uintptr_t*)(destSlot + 0x60)         = (uintptr_t)(childProxy
		? childProxy
		: DTExtend_ZeroProxyForProp(name));
}

// Recursively walk the SendTable DataTable hierarchy from `table`, find the shared nested DT_HighlightSettings, and rebuild it to the S21 6-member shape (members listed at bridge_highlight_teambits above).
// DT_HighlightSettings is NESTED, so it never appears in tables[] and must be reached through DataTable props. `seen` dedups the shared sub-table (referenced by many entity classes); the rebuild is name-bound, so one in-place pass fixes every highlightable entity.
static void Highlight_RebuildSettingsInTree(uintptr_t table, uintptr_t* seen,
	int& seenCount, int seenCap, int& found, int& converted, int depth)
{
	if (!table || depth > 24) return;
	for (int s = 0; s < seenCount; ++s)
		if (seen[s] == table) return;            // shared/cycle guard
	if (seenCount < seenCap) seen[seenCount++] = table;

	uint8_t* props = *(uint8_t**)(table + ST_PROPS);
	const int nProps = *(int*)(table + ST_NPROPS);
	if (!props || nProps <= 0) return;

	const char* tn = *(const char**)(table + ST_NETTABLENAME);
	if (tn && strcmp(tn, "DT_HighlightSettings") == 0)
	{
		++found;
		static bool s_hlDumped = false;
		if (!s_hlDumped)
			s_hlDumped = true;
		// Idempotence: if this shared sub-table was already rebuilt, the S21-only
		// member m_highlightFadeParity is present -- bail.
		for (int j = 0; j < nProps; ++j)
		{
			const char* nm = *(const char**)(props + (uint64_t)j * SP_SIZE + SP_VARNAME);
			if (nm && strcmp(nm, "m_highlightFadeParity") == 0)
				return;
		}

		// Locate the three S3 donors (all native to S3 DT_HighlightSettings) intDonor = m_highlightFunctionBits (type-10 -> Int32[8] child) floatDonor = m_highlightServerFadeBases (type-10 -> Float[2] child) scalarDonor = m_highlightServerContextID (type-0 Int scalar)
		const uint8_t* intDonor    = nullptr;
		const uint8_t* floatDonor  = nullptr;
		const uint8_t* scalarDonor = nullptr;
		for (int j = 0; j < nProps; ++j)
		{
			const uint8_t* p = props + (uint64_t)j * SP_SIZE;
			const char* nm = *(const char**)(p + SP_VARNAME);
			if (!nm) continue;
			const int ty = *(const int*)(p + SP_TYPE);
			if (ty == 10 && strcmp(nm, "m_highlightFunctionBits") == 0)        intDonor    = p;
			else if (ty == 10 && strcmp(nm, "m_highlightServerFadeBases") == 0) floatDonor  = p;
			else if (ty == 0  && strcmp(nm, "m_highlightServerContextID") == 0) scalarDonor = p;
		}
		if (!intDonor || !floatDonor || !scalarDonor)
		{
			Warning(eDLL_T::ENGINE, "[dt_extend] DT_HighlightSettings rebuild: missing donor "
				"(functionBits=%p fadeBases=%p contextID=%p) -- skipped\n",
				(void*)intDonor, (void*)floatDonor, (void*)scalarDonor);
			return;
		}

		// Build a fresh props array matching S21's DT_HighlightSettings -- but OMIT m_highlightTeamIndex.
		// It is a SYNTH (zero, no S3 backing) Int32[8] whose "[0000]".."[0007]" children are STRUCTURALLY IDENTICAL to m_highlightTeamBits'.
		const int kNew = 5;
		uint8_t* np = (uint8_t*)malloc((size_t)kNew * SP_SIZE);
		if (!np)
		{
			Warning(eDLL_T::ENGINE, "[dt_extend] DT_HighlightSettings rebuild: props alloc FAILED -- skipped\n");
			return;
		}
		bool ok = true;
		ok = ok && Highlight_BuildArrayMember(np + 0 * SP_SIZE, intDonor,   8, "m_highlightTeamBits",        &Highlight_TeamBitsTranslateProxy, false);
		ok = ok && Highlight_BuildArrayMember(np + 1 * SP_SIZE, intDonor,   4, "m_highlightGenericContexts", &Highlight_GenericContextProxy,    false);
		Highlight_BuildScalarMember(np + 2 * SP_SIZE, scalarDonor, "m_highlightFocused", &Highlight_FocusedProxy);
		ok = ok && Highlight_BuildArrayMember(np + 3 * SP_SIZE, floatDonor, 2, "m_highlightFadeDuration",    &Canon_ZeroProxy,                  false);
		Highlight_BuildScalarMember(np + 4 * SP_SIZE, scalarDonor, "m_highlightFadeParity", nullptr);
		if (!ok)
		{
			Warning(eDLL_T::ENGINE, "[dt_extend] DT_HighlightSettings rebuild: array-member clone FAILED -- skipped\n");
			free(np);
			return;
		}

		// Swap the S3 props array (7 members) for the S21 one (6). The S3-only members
		// (m_highlightParams/FunctionBits/ServerFade*/ServerContextID) are GONE, so the
		// dedi no longer ships them -> nothing for the S21 client to misread.
		*(uint8_t**)(table + ST_PROPS) = np;
		*(int*)(table + ST_NPROPS)     = kNew;
		*(uintptr_t*)(table + 0x4C0)   = 0;   // precalc -> SetupFlatPropertyArray rebuilds
		*(uintptr_t*)(table + 0x508)   = 0;
		++converted;
		Warning(eDLL_T::ENGINE, "[dt_extend] DT_HighlightSettings rebuilt to S21 6-member shape "
			"(TeamIndex[8] TeamBits[8] GenericContexts[4] Focused FadeDuration[2] FadeParity)\n");
		return;
	}

	for (int j = 0; j < nProps; ++j)                             // recurse into DataTable children
	{
		uint8_t* p = props + (uint64_t)j * SP_SIZE;
		if (*(int*)(p + SP_TYPE) != 10) continue;               // DPT_DataTable
		uintptr_t child = *(uintptr_t*)(p + 0x70 /* m_pDataTable */);
		if (child)
			Highlight_RebuildSettingsInTree(child, seen, seenCount, seenCap, found, converted, depth + 1);
	}
}

// Rebuild every DT_HighlightSettings in the table tree to the S21 6-member shape (the legacy reshape).
// Runs on the legacy path (from DTExtend_Apply) AND the canonical path (from Hook_SendTable_Init -- the reshape table was deferred raw, see CanonIsReshapeTable, so its S3 donors are still present for the translate proxy).
void DTExtend_RebuildHighlightSettings(void** tables, int count)
{
	if (!bridge_highlight_teambits.GetBool())
	{
		Warning(eDLL_T::ENGINE,
			"[dt_extend] DT_HighlightSettings rebuild SKIPPED (bridge_highlight_teambits=0) "
			"-- ships raw S3 layout (reproduces enter-PVS desync)\n");
		return;
	}
	// DT_HighlightSettings is a NESTED sub-table (reached via a DataTable prop in the
	// entity classes), so it is NOT in tables -- descend the DataTable hierarchy.
	static uintptr_t s_hlSeen[4096];
	int seenCount = 0, hlFound = 0, hlConverted = 0;
	for (int i = 0; i < count; ++i)
		Highlight_RebuildSettingsInTree((uintptr_t)tables[i], s_hlSeen, seenCount,
			(int)(sizeof(s_hlSeen) / sizeof(s_hlSeen[0])), hlFound, hlConverted, 0);
	Warning(eDLL_T::ENGINE,
		"[dt_extend] DT_HighlightSettings rebuilt to S21 6-member shape; "
		"tablesScanned=%d hlTablesFound=%d rebuilt=%d\n",
		count, hlFound, hlConverted);
}

// m_modInventory[i] widen value proxy.
// S3 packs each 4-byte slot { u16 modName @0x0, u8 weaponIdx @0x2 (255 = "no specific weapon" / universal mod), u8 count @0x3 }; S21's ModInventoryItem_s is 8 bytes { u16 modName @0x0, u16 weaponNameIndex @0x2, u8 _pad[3], u8 count @0x7 } and Script_ModInventory_Get's occupied-slot test reads byte 7 (511 = the no-specific-weapon sentinel).
static void __fastcall ModInventory_WidenProxy(void* pProp, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	if (!pStruct || !pProp) return;
	const int off = *(int*)((const char*)pProp + 0x78 /* SP_OFFSET */);
	const uint8_t* src = (const uint8_t*)pStruct + off;
	const uint16_t modName   = *(const uint16_t*)(src + 0);
	const uint8_t  weaponIdx = src[2];
	const uint8_t  count     = src[3];
	uint8_t* dst = (uint8_t*)pOut;
	*(uint16_t*)(dst + 0) = modName;
	*(uint16_t*)(dst + 2) = (weaponIdx == 255) ? (uint16_t)511 : (uint16_t)weaponIdx;
	dst[7] = count;
}

// m_consumableInventory[i] widen value proxy.
// S3 packs each slot {u8 type, u8 count}; S21's ConsumableInventoryItem_s is 32-bit { u16 type @0x0, u16 count @0x2 } and its occupied-slot test reads HIWORD != 0, so count must land in the HIGH 16 bits, not packed alongside type in the low 16.
static int s_ciElemBaseOff = -1;   // SP_OFFSET of element 0, read off the built table
static int s_ciElemStride  = 0;    // element 1 offset - element 0 offset

static void __fastcall ConsumableInventory_WidenProxy(void* pProp, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int objectID)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	if (!pStruct || !pProp) return;
	const int off = *(int*)((const char*)pProp + 0x78 /* SP_OFFSET */);
	const uint16_t raw = *(const uint16_t*)((const char*)pStruct + off);
	uint32_t type  = raw & 0xFF;
	uint32_t count = (raw >> 8) & 0xFF;

	// Base and stride come from the built table rather than the S3 array offset, so
	// the slot stays correct whichever base the engine stamped into the elements.
	if (count && s_ciElemStride > 0)
	{
		const int rel = off - s_ciElemBaseOff;
		const int slot = rel / s_ciElemStride;

		if (rel >= 0 && (rel % s_ciElemStride) == 0 && slot < 32)
		{
			const uint16_t full = ConsumableInv_ResolveTypeByEnt(objectID, slot,
				(uint16_t)type);

			if (full != (uint16_t)type)
			{
				type = full;

				static bool s_ciFullTypeLogged = false;
				if (!s_ciFullTypeLogged)
				{
					s_ciFullTypeLogged = true;
					Warning(eDLL_T::ENGINE,
						"[dt_extend][CONSUMABLEINV] first widened type on the wire: ent=%d "
						"slot=%d native=%u full=%u count=%u\n",
						objectID, slot, raw & 0xFF, full, count);
				}
			}
		}
	}

	*(uint32_t*)pOut = type | (count << 16);
}

// 32 bracketed element names ("[0000]".."[0031]") matching the S21 client's recv
// convention. Built once into a static table (the SendProp stores a const char*).
static const char* ModInv_ElemName(int i)
{
	static char names[32][8];
	static bool init = false;
	if (!init)
	{
		for (int k = 0; k < 32; ++k)
			snprintf(names[k], sizeof(names[k]), "[%04d]", k);
		init = true;
	}
	return (i >= 0 && i < 32) ? names[i] : "[0000]";
}

// Walk the DataTable hierarchy, find the nested "m_modInventory" array sub-table (deferred raw by CanonIsReshapeTable so its 32 S3 Int32 donor elements survive), and retype each child Int32->Int64 + install the widen proxy + rename to "[000N]".
// The S3 element's real m_Offset is KEPT (field addressing unchanged); only the wire type + value width change.
static void ModInv_RetypeInTree(uintptr_t table, uintptr_t* seen, int& seenCount,
	int seenCap, int& found, int depth)
{
	if (!table || depth > 24) return;
	for (int s = 0; s < seenCount; ++s)
		if (seen[s] == table) return;
	if (seenCount < seenCap) seen[seenCount++] = table;

	uint8_t* props = *(uint8_t**)(table + ST_PROPS);
	const int nProps = *(int*)(table + ST_NPROPS);
	if (!props || nProps <= 0) return;

	const char* tn = *(const char**)(table + ST_NETTABLENAME);
	if (tn && strcmp(tn, "m_modInventory") == 0)
	{
		if (*(int*)(props + SP_TYPE) == 7) return;   // idempotent: already retyped
		int converted = 0;
		for (int j = 0; j < nProps && j < 32; ++j)
		{
			uint8_t* p = props + (uint64_t)j * SP_SIZE;
			*(int*)(p + SP_TYPE)            = 7;                  // DPT_Int64
			*(const char**)(p + SP_VARNAME) = ModInv_ElemName(j);
			*(int*)(p + SP_FLAGS)          |= 1;                  // SPROP_UNSIGNED
			*(uintptr_t*)(p + 0x60)         = (uintptr_t)&ModInventory_WidenProxy;
			++converted;
		}
		++found;
		Warning(eDLL_T::ENGINE,
			"[dt_extend][MODINV] m_modInventory retyped %d elems Int32->Int64 "
			"(zero-extend, S3 player-mod data preserved)\n", converted);
		return;
	}

	for (int j = 0; j < nProps; ++j)
	{
		const uint8_t* p = props + (uint64_t)j * SP_SIZE;
		if (*(const int*)(p + SP_TYPE) == 10)
		{
			uintptr_t child = *(uintptr_t*)(p + 0x70 /* m_pDataTable */);
			if (child) ModInv_RetypeInTree(child, seen, seenCount, seenCap, found, depth + 1);
		}
	}
}

// Entry: convert the deferred raw m_modInventory sub-table to the S21 Int64 shape.
// Runs on the canonical path from Hook_SendTable_Init AFTER DTExtend_RebuildSchemaDriven (the sub-table was deferred raw via CanonIsReshapeTable).
static void DTExtend_RebuildModInventory(void** tables, int count)
{
	if (!bridge_canon_modinv_widen.GetBool())
	{
		Warning(eDLL_T::ENGINE,
			"[dt_extend][MODINV] m_modInventory widen SKIPPED (bridge_canon_modinv_widen=0) "
			"-- elements stay synth-zero\n");
		return;
	}
	static uintptr_t s_miSeen[4096];
	int seenCount = 0, found = 0;
	for (int i = 0; i < count; ++i)
		ModInv_RetypeInTree((uintptr_t)tables[i], s_miSeen, seenCount,
			(int)(sizeof(s_miSeen) / sizeof(s_miSeen[0])), found, 0);
	Warning(eDLL_T::ENGINE,
		"[dt_extend][MODINV] m_modInventory reshape: tablesScanned=%d subTablesFound=%d\n",
		count, found);
}

// Walk the DataTable hierarchy, find the nested "m_consumableInventory" array sub-table (deferred raw by CanonIsReshapeTable so its 32 S3 16-bit donor elements survive), and widen each child's SP_NBITS 16->32 + install the zero-extend proxy + rename to "[000N]" (bracket text is cosmetic housekeeping, matching the proven ModInventory scheme -- subtree correlation binds on the PARENT table name, not the child text).
// SP_TYPE stays 0 (DPT_Int) on both sides -- unlike m_modInventory this is a bit-width widen only, no type change.
static void ConsumableInv_RetypeInTree(uintptr_t table, uintptr_t* seen, int& seenCount,
	int seenCap, int& found, int depth)
{
	if (!table || depth > 24) return;
	for (int s = 0; s < seenCount; ++s)
		if (seen[s] == table) return;
	if (seenCount < seenCap) seen[seenCount++] = table;

	uint8_t* props = *(uint8_t**)(table + ST_PROPS);
	const int nProps = *(int*)(table + ST_NPROPS);
	if (!props || nProps <= 0) return;

	const char* tn = *(const char**)(table + ST_NETTABLENAME);
	if (tn && strcmp(tn, "m_consumableInventory") == 0)
	{
		if (*(int*)(props + SP_NBITS) == 32) return;   // idempotent: already widened
		int converted = 0;
		for (int j = 0; j < nProps && j < 32; ++j)
		{
			uint8_t* p = props + (uint64_t)j * SP_SIZE;
			*(int*)(p + SP_NBITS)           = 32;
			*(const char**)(p + SP_VARNAME) = ModInv_ElemName(j);
			*(int*)(p + SP_FLAGS)          |= 1;                  // SPROP_UNSIGNED
			*(uintptr_t*)(p + 0x60)         = (uintptr_t)&ConsumableInventory_WidenProxy;
			++converted;
		}
		// The proxy has no other way to name its slot: it is handed the array, not the entity, and the element offsets the engine stamped are the only ordering it can see.
		// Two elements are needed for a stride; one leaves the shadow off and the wire falls back to the truncated byte.
		if (converted >= 2)
		{
			s_ciElemBaseOff = *(int*)(props + SP_OFFSET);
			s_ciElemStride  = *(int*)(props + SP_SIZE + SP_OFFSET) - s_ciElemBaseOff;
		}

		if (s_ciElemStride <= 0)
		{
			s_ciElemBaseOff = -1;
			s_ciElemStride  = 0;
			Warning(eDLL_T::ENGINE,
				"[dt_extend][CONSUMABLEINV] element stride unreadable (%d elems) -- loot "
				"indices >= 256 stay truncated on the wire\n", converted);
		}

		++found;
		Warning(eDLL_T::ENGINE,
			"[dt_extend][CONSUMABLEINV] m_consumableInventory widened %d elems 16bit->32bit "
			"(elemBase=0x%X stride=%d, S3 consumable/ordnance pickup data preserved)\n",
			converted, s_ciElemBaseOff, s_ciElemStride);
		return;
	}

	for (int j = 0; j < nProps; ++j)
	{
		const uint8_t* p = props + (uint64_t)j * SP_SIZE;
		if (*(const int*)(p + SP_TYPE) == 10)
		{
			uintptr_t child = *(uintptr_t*)(p + 0x70 /* m_pDataTable */);
			if (child) ConsumableInv_RetypeInTree(child, seen, seenCount, seenCap, found, depth + 1);
		}
	}
}

// Entry: widen the deferred raw m_consumableInventory sub-table to the S21 32-bit Int shape.
// Runs on the canonical path from Hook_SendTable_Init AFTER DTExtend_RebuildSchemaDriven (the sub-table was deferred raw via CanonIsReshapeTable).
static void DTExtend_RebuildConsumableInventory(void** tables, int count)
{
	if (!bridge_canon_consumableinv_widen.GetBool())
	{
		Warning(eDLL_T::ENGINE,
			"[dt_extend][CONSUMABLEINV] m_consumableInventory widen SKIPPED "
			"(bridge_canon_consumableinv_widen=0) -- elements stay 16-bit narrow\n");
		return;
	}
	static uintptr_t s_ciSeen[4096];
	int seenCount = 0, found = 0;
	for (int i = 0; i < count; ++i)
		ConsumableInv_RetypeInTree((uintptr_t)tables[i], s_ciSeen, seenCount,
			(int)(sizeof(s_ciSeen) / sizeof(s_ciSeen[0])), found, 0);
	Warning(eDLL_T::ENGINE,
		"[dt_extend][CONSUMABLEINV] m_consumableInventory reshape: tablesScanned=%d subTablesFound=%d\n",
		count, found);
}

bool DTExtend_ShouldZeroProxyAppendedProp(const char* tableName, const char* propName)
{
	if (!tableName || !propName)
		return false;

	// These are S21-only CPlayerDecoy tail props. The S3 dedicated entity does
	// not own backing storage for them, so reading the appended slack offsets
	// can serialize garbage and desync the S21 decoder after m_decoyVelocity.
	if (strcmp(tableName, "DT_PlayerDecoy") == 0)
	{
		return strcmp(propName, "m_vecViewOffset.x") == 0 ||
		       strcmp(propName, "m_vecViewOffset.y") == 0 ||
		       strcmp(propName, "m_vecViewOffset.z") == 0 ||
		       strcmp(propName, "m_decoyVelocity") == 0;
	}

	// [PASSMOD-ZERO] Same bug class as DT_PlayerDecoy: S3 has no backing storage for m_passThroughModCount, so "allocation slack" offset 5600 (DTExtend_BaseOffset) reads live entity memory.
	// That offset is not free padding -- it aliases a real S3 field that defaults to 0x3F800000 (float 1.0f).
	if (strcmp(tableName, "DT_Projectile") == 0)
		return strcmp(propName, "m_passThroughModCount") == 0;

	// [DOOR-REINFORCED-ZERO] Same bug class.
	// C_PropDoor::m_isReinforced is a real native bool at 0x1815 (next to m_isLocked at 0x1814).
	if (strcmp(tableName, "DT_CPropDoor") == 0)
		return strcmp(propName, "m_isReinforced") == 0 ||
		       strcmp(propName, "m_iTeamNum") == 0;

	// [TRIGGER-EF-ZERO] PointGravity is unbacked (not grown). Heavy is zero-proxied
	// only while the factory grow has not succeeded -- fallback for a failed grow,
	// not the steady state once [TRIG-HEAVY-GROW] backs the tail.
	if (strcmp(tableName, "DT_TriggerPointGravity") == 0)
		return true;
	if (strcmp(tableName, "DT_TriggerCylinderHeavy") == 0)
		return !s_triggerHeavyGrown;

	// [SCRIPTVEC-ZERO] S21-only script-driven weapon vector family.
	// DT_WeaponX base is 6100 while CWeaponX live fields run past +11400 (modvars ~+7000), so 6156..6196 is live weapon state, not slack.
	if (strcmp(tableName, "DT_WeaponX") == 0)
	{
		return strcmp(propName, "m_scriptVector") == 0 ||
		       strcmp(propName, "m_scriptVectorTransitionStart") == 0 ||
		       strcmp(propName, "m_scriptVectorTransitionEnd") == 0 ||
		       strcmp(propName, "m_scriptVectorTransitionDuration") == 0 ||
		       strcmp(propName, "m_scriptVectorTransitionStartTime") == 0 ||
		       strcmp(propName, "m_curReactiveSkinKillCount") == 0 ||
		       strcmp(propName, "m_curReactiveSkinKnockdownCount") == 0 ||
		       strcmp(propName, "m_lastTossedGrenade") == 0 ||
		       strcmp(propName, "m_modBitfieldDisabled") == 0 ||
		       strcmp(propName, "m_offhandSwitchSlot") == 0 ||
		       strcmp(propName, "m_parentTurret") == 0;
	}

	return false;
}

static const S21SchemaTable* CanonSchemaFind(const char* name); // fwd decl (defined later in this file)

// Walk the S21 schema hierarchy from `leafTable` upward via each table's self-named baseclass
// entry (props[0], type==10 DataTable), collecting ancestor table names (leaf first). Bounded
// depth (this project's loop-safety-counter convention).
static int DTExtend_CollectAncestorTables(const char* leafTable, const char** outTables, int maxOut)
{
	int n = 0;
	const char* cur = leafTable;
	for (int depth = 0; cur && depth < 12 && n < maxOut; ++depth)
	{
		outTables[n++] = cur;
		const S21SchemaTable* t = CanonSchemaFind(cur);
		if (!t || t->nProps <= 0 || !t->props) break;
		const S21SchemaProp& p0 = t->props[0];
		if (p0.type != 10 || !p0.name || strcmp(p0.name, t->table) != 0) break; // not a baseclass entry
		if (!p0.dtName || !p0.dtName[0]) break;                                 // no parent
		cur = p0.dtName;
	}
	return n;
}

// [EXTEND-AUDIT] safety net: "allocation slack" offsets are UNVERIFIED guesses (DTExtend_BaseOffset) -- reading one past the real allocation is an OOB read.
// That fault crashed inside value memcpy during State_NewGame; __try/__except did not stop it (VEH-before-SEH on the dedi too).
bool DTExtend_IsSafeToRead(const void* addr, size_t size, bool forWrite)
{
	MEMORY_BASIC_INFORMATION mbi;
	if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi))
		return false;
	if (mbi.State != MEM_COMMIT)
		return false;
	if (mbi.Protect == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD))
		return false;
	if (forWrite)
	{
		const DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		if (!(mbi.Protect & kWritable))
			return false;
	}
	const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
	const uintptr_t readEnd   = reinterpret_cast<uintptr_t>(addr) + size;
	return readEnd <= regionEnd; // the whole read must stay inside this one committed region
}

// ===========================================================================
// [LAUNCH-ORIGIN] -- DT_Projectile.m_launchOrigin value sidecar S3 has no native launch-snapshot position; S21 trails FROM m_launchOrigin, so encoding 0 draws the trail to world origin.
// Capture m_vecAbsOrigin at activation into a sidecar keyed by entity pointer; LaunchOrigin_ValueProxy serves it.
// ===========================================================================

// CBaseEntity::m_localOrigin server layout 0x554 (packed SP_OFFSET 0x500554 & 0xFFFFF; after m_cellX/Y/Z at 0x548/0x54C/0x550).
// This is the networked field -- world-space for unparented projectiles. m_vecAbsOrigin (+0x450) is lazily recomputed and reads (0,0,0) for every projectile at capture time.
static constexpr uintptr_t ENT_OFF_LOCALORIGIN = 0x554;

// CAP/LOCK spew is diagnosis-only (defaults off). PENDING at activation is
// expected -- the engine positions the entity after the capture hook runs.
static ConVar bridge_launch_origin_log("bridge_launch_origin_log", "0", FCVAR_DEVELOPMENTONLY,
	"Log the [LAUNCH-CAP]/[LAUNCH-LOCK] projectile launch-origin sidecar activity "
	"(first 64 each). PENDING at capture is normal: activation precedes engine "
	"positioning and the encode proxy re-samples until a real origin locks. "
	"0 = quiet (default), 1 = log.");

static inline bool LaunchOrigin_IsZero(const float* p)
{
	return p[0] == 0.f && p[1] == 0.f && p[2] == 0.f;
}

struct LaunchOriginEntry
{
	uintptr_t ent;     // entity base pointer (0 = free slot)
	uint32_t  handle;  // m_RefEHandle at store time
	float     pos[3];  // captured launch position
};
static LaunchOriginEntry s_launchOrigins[256];

static LaunchOriginEntry* DTExtend_FindLaunchOrigin(uintptr_t entity)
{
	for (int i = 0; i < 256; ++i)
		if (s_launchOrigins[i].ent == entity)
			return &s_launchOrigins[i];
	return nullptr;
}

// Store (or refresh) the sidecar entry for `entity` with its CURRENT local origin.
// Encode-path safe: no VirtualQuery -- pStruct/entity is the pack loop's live entity (same trust as native value proxies).
static LaunchOriginEntry* DTExtend_StoreLaunchOrigin(uintptr_t entity)
{
	if (!entity)
		return nullptr;

	const void* const src = reinterpret_cast<const void*>(entity + ENT_OFF_LOCALORIGIN);
	LaunchOriginEntry* e = DTExtend_FindLaunchOrigin(entity);
	if (!e)
		e = DTExtend_FindLaunchOrigin(0);
	if (!e)
	{
		for (int i = 0; i < 256; ++i)
		{
			LaunchOriginEntry& cand = s_launchOrigins[i];
			if (cand.ent == 0)
			{
				e = &cand;
				break;
			}
			const SDKEntityHandle h(cand.handle);
			if (!h.IsValid() || !SDKEntityState_Resolve(h, ESide::Server))
			{
				e = &cand;
				break;
			}
		}
	}
	if (!e)
	{
		static volatile LONG s_fullN = 0;
		if (InterlockedIncrement(&s_fullN) <= 8)
			Warning(eDLL_T::SERVER,
				"[LAUNCH-ORIGIN] sidecar full (256 live) -- store refused\n");
		return nullptr;
	}
	memcpy(e->pos, src, 12);
	e->ent = entity;
	e->handle = SDKEntityState_GetHandle(reinterpret_cast<const void*>(entity)).Raw();
	return e;
}

// Called from Hook_ActivateEntity for EVERY newly-activated entity (one-shot per spawn, not a hot path).
// Invalidates any stale sidecar entry for this pointer (allocator reuse across entity lifetimes), then captures the launch position if the entity's SendTable hierarchy includes DT_Projectile.
static void DTExtend_CaptureLaunchOrigin(uintptr_t entity)
{
	if (!entity) return;

	// Spawn/activate: preflight only (no outer SEH -- VEH-before-SEH on dedi).
	if (LaunchOriginEntry* stale = DTExtend_FindLaunchOrigin(entity))
		stale->ent = 0;

	if (!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(entity + 0x50), 8)) return;
	const uintptr_t serverClass = *reinterpret_cast<uintptr_t*>(entity + 0x50);
	if (!serverClass || !DTExtend_IsSafeToRead(reinterpret_cast<const void*>(serverClass + 0x08), 8)) return;
	const uintptr_t sendTable = *reinterpret_cast<uintptr_t*>(serverClass + 0x08);
	if (!sendTable || !DTExtend_IsSafeToRead(reinterpret_cast<const void*>(sendTable + ST_NETTABLENAME), 8)) return;
	const char* leafTable = *reinterpret_cast<const char**>(sendTable + ST_NETTABLENAME);
	if (!leafTable) return;

	const char* ancestors[12];
	const int nAnc = DTExtend_CollectAncestorTables(leafTable, ancestors, 12);
	bool isProjectile = false;
	for (int a = 0; a < nAnc; ++a)
		if (!strcmp(ancestors[a], "DT_Projectile")) { isProjectile = true; break; }
	if (!isProjectile) return;

	const LaunchOriginEntry* e = DTExtend_StoreLaunchOrigin(entity);
	static volatile LONG s_capN = 0;
	const LONG capN = (e && bridge_launch_origin_log.GetBool()) ? InterlockedIncrement(&s_capN) : 0;
	if (capN >= 1 && capN <= 64)
		Warning(eDLL_T::SERVER, "[LAUNCH-CAP] %s ent=%p origin=(%.1f %.1f %.1f)%s%s\n",
			leafTable, reinterpret_cast<void*>(entity), e->pos[0], e->pos[1], e->pos[2],
			LaunchOrigin_IsZero(e->pos) ? " PENDING (not positioned yet, proxy will retry)" : "",
			capN == 64 ? " (further captures silenced)" : "");
}

// DT_Projectile.m_launchOrigin value proxy. pStruct = entity base (main hierarchy).
// Serves the sidecar's captured launch position; if the entity was never captured (activated before our detour attached), lazily captures its CURRENT origin on first encode -- the first snapshot after spawn is still the launch position for all practical purposes.
static void __fastcall LaunchOrigin_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;

	const uintptr_t entity = reinterpret_cast<uintptr_t>(pStruct);
	const LaunchOriginEntry* e = DTExtend_FindLaunchOrigin(entity);
	if (!e)
	{
		e = DTExtend_StoreLaunchOrigin(entity);
		static volatile LONG s_lazyN = 0;
		if (InterlockedIncrement(&s_lazyN) <= 16)
			Warning(eDLL_T::SERVER, "[LAUNCH-ORIGIN] lazy capture ent=%p (activation hook missed it)%s\n",
				reinterpret_cast<void*>(entity), e ? "" : " -- origin UNREADABLE, encoding 0");
		if (!e) return;
	}
	else if (LaunchOrigin_IsZero(e->pos))
	{
		// Activation caught the entity before the engine positioned it (live [LAUNCH-CAP] showed (0,0,0) captures).
		// Re-sample the CURRENT m_localOrigin each encode until a real position appears, then that value stays locked in the sidecar as the launch point.
		LaunchOriginEntry* fresh = DTExtend_StoreLaunchOrigin(entity);
		if (fresh)
		{
			e = fresh;
			// Print cap matches [LAUNCH-CAP]'s 64: a lower cap made healthy bolts
			// read as "PENDING forever" once LOCK prints ran out while CAP continued.
			static volatile LONG s_lockN = 0;
			const LONG lockN = (!LaunchOrigin_IsZero(fresh->pos) && bridge_launch_origin_log.GetBool())
				? InterlockedIncrement(&s_lockN) : 0;
			if (lockN >= 1 && lockN <= 64)
				Warning(eDLL_T::SERVER, "[LAUNCH-LOCK] ent=%p origin=(%.1f %.1f %.1f)%s\n",
					reinterpret_cast<void*>(entity), fresh->pos[0], fresh->pos[1], fresh->pos[2],
					lockN == 64 ? " (further locks silenced)" : "");
		}
	}
	memcpy(pOut, e->pos, 12);
}

// DT_WeaponX_LocalWeaponData.m_infiniteAmmoState value proxy. pStruct = the weapon entity base (DT_WeaponX_LocalWeaponData's leaf prop offsets are weapon-relative, e.g. m_ammoInClip +4916).
// Serves the SDK sidecar (weapon_script_vars.cpp WeaponScriptVars_GetInfiniteAmmoState) instead of any entity-memory slot -- this prop has none.
static void __fastcall InfiniteAmmoState_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;

	const int state = WeaponScriptVars_GetInfiniteAmmoState(pStruct);
	*(int*)pOut = state;
}

// [SCRIPTFLOAT0-WIRE] DT_WeaponX.m_scriptFloat0 value proxy. pStruct = the weapon entity base.
// SetScriptFloat0 is server-only (replicate to client); the SDK stores it in a sidecar (weapon_script_vars.cpp) that never reached the wire, while the appended SendProp read raw offset 6152.
static void __fastcall ScriptFloat0_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;

	*(float*)pOut = WeaponScriptVars_GetScriptFloat0(pStruct);
}

#ifndef CLIENT_DLL
// Flat DT_WeaponX leaf, so pStruct is the weapon entity base. Reads the
// akimbo sidecar directly; nothing is derived from entity memory.
static void __fastcall AkimboDisabled_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut = 0; *((uint64_t*)pOut + 1) = 0; *((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;
	*(int*)pOut = AkimboBridge_IsDisabled(pStruct) ? 1 : 0;
}
#endif // !CLIENT_DLL

// CServer+0x3CC is the SetTimescale store. host_timescale is a separate ConVar
// (applied on both ends), so the wire is this float, not the GetTimescale product.
static float (*v_Engine_GetTimescale)(void) = nullptr;
static float* s_pSvTimescale = nullptr;
static uintptr_t s_gnrTimescaleEnt = 0;
static float s_gnrTimescaleLastSent = 0.0f;
static bool s_gnrTimescaleHaveLast = false;

static ConVar sdk_gametimescale_diag("sdk_gametimescale_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Log DT_GlobalNonRewinding.m_gameTimescale wire from the engine SetTimescale store.");

static ConVar bridge_world_timescale("bridge_world_timescale", "1.0", FCVAR_CHEAT,
	"Dedi world-freeze scale. 1.0 = off. 0.0001 = frozen world, live net. "
	"Clamped to [0.0001, 1.0]. host_timescale stays 1.0 so ticks keep flowing; "
	"curTime is held and cmd dt is scaled.");

static float s_flFreezeLogged = 1.0f;

static float GameTimescale_ClampedConVar(void)
{
	float want = bridge_world_timescale.GetFloat();
	if (want != want)
		want = 1.0f;
	if (want < 0.0001f)
		want = 0.0001f;
	if (want > 1.0f)
		want = 1.0f;
	return want;
}

float GameTimescale_WorldScale(void)
{
	return GameTimescale_ClampedConVar();
}

static float GameTimescale_WireValue(void)
{
	const float want = GameTimescale_ClampedConVar();
	if (want < 1.0f)
		return want;
	if (!s_pSvTimescale)
		return 1.0f;
	const float v = *s_pSvTimescale;
	if (v != v)
		return 1.0f;
	return v;
}

static void GameTimescale_TryCaptureGnr(uintptr_t entity)
{
	if (!entity || s_gnrTimescaleEnt)
		return;
	if (!DTExtend_EntityHasSendTable(reinterpret_cast<void*>(entity), "DT_GlobalNonRewinding"))
		return;
	s_gnrTimescaleEnt = entity;
	// The S21 client reads the game timescale off cl_entitylist slot 257 and
	// nowhere else, so the edict index this entity got is what decides whether
	// the decoded value is ever the one the client reads back.
	const int edictIdx = *reinterpret_cast<const int16_t*>(entity + 0x58);
	Warning(eDLL_T::ENGINE,
		"[GTS-WIRE] captured GNR entity %p edict=%d (S21 client reads slot 257) timescale=%.4f\n",
		reinterpret_cast<void*>(entity), edictIdx,
		static_cast<double>(GameTimescale_WireValue()));
}

static void __fastcall GameTimescale_ValueProxy(void* /*pProp*/, void* /*pStruct*/,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	*(float*)pOut = GameTimescale_WireValue();
}

void GameTimescale_TickServer(void)
{
	const float want = GameTimescale_ClampedConVar();
	if (want != s_flFreezeLogged)
	{
		Msg(eDLL_T::ENGINE, "[FREEZE] world sim scale %.4f -> %.4f\n",
			static_cast<double>(s_flFreezeLogged), static_cast<double>(want));
		s_flFreezeLogged = want;
	}
	// Host_AccumulateTime remainder is host_timescale * this store, so a
	// 0.0001 write here stops tick production. Freeze keeps the store at 1.0.
	if (s_pSvTimescale && want < 1.0f && *s_pSvTimescale != 1.0f)
		*s_pSvTimescale = 1.0f;
	if (!s_gnrTimescaleEnt)
		return;
	const float v = GameTimescale_WireValue();
	if (s_gnrTimescaleHaveLast && v == s_gnrTimescaleLastSent)
		return;
	s_gnrTimescaleLastSent = v;
	s_gnrTimescaleHaveLast = true;
	MarkEntityEdictDirty(reinterpret_cast<void*>(s_gnrTimescaleEnt));
	if (sdk_gametimescale_diag.GetBool())
	{
		static volatile LONG s_n = 0;
		const LONG n = InterlockedIncrement(&s_n);
		if (n <= 16 || (n % 200) == 0)
			Msg(eDLL_T::ENGINE, "[GTS-WIRE] #%ld dirty GNR %p timescale=%.4f\n",
				n, reinterpret_cast<void*>(s_gnrTimescaleEnt), static_cast<double>(v));
	}
}

// [HEAT-WIRE] DT_WeaponX heat trio value proxies. pStruct = the weapon entity.
// Heat storage lives in an SDK sidecar (weapon_heat.cpp); the append offsets sit inside CWeaponX m_modVars (base 6100 vs m_modVars 0x17E0 = 6112), so these proxies are the wire path -- no entity-memory slot.
static void __fastcall HeatValue_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut = 0; *((uint64_t*)pOut + 1) = 0; *((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;
	*(float*)pOut = WeaponHeat_WireGetHeatValue(pStruct);
}

static void __fastcall HeatValueOnLastFire_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut = 0; *((uint64_t*)pOut + 1) = 0; *((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;
	// THE load-bearing one: the client's UpdateHeatDecay recomputes heatValue, scriptFloat0, the pose param and fullyHeated every predicted frame from this plus m_lastPrimaryAttack, so this single value drives the whole client-side heat model.
	*(float*)pOut = WeaponHeat_WireGetHeatValueOnLastFire(pStruct);
}

static void __fastcall FullyHeated_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut = 0; *((uint64_t*)pOut + 1) = 0; *((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;
	*(int*)pOut = WeaponHeat_WireGetFullyHeated(pStruct);
}

// [ENERGIZE-WIRE] DT_WeaponX energize trio value proxies.
// Same as heat: the appends sit inside CWeaponX m_modVars (base 6100 vs m_modVars 0x17E0).
static void __fastcall EnergizeState_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut = 0; *((uint64_t*)pOut + 1) = 0; *((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;
	int state = 0;
	if (EnergizeBridge_WireGet(pStruct, &state, nullptr, nullptr))
		*(int*)pOut = state;
}

static void __fastcall StartEnergizingTime_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut = 0; *((uint64_t*)pOut + 1) = 0; *((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;
	float t = 0.0f;
	if (EnergizeBridge_WireGet(pStruct, nullptr, &t, nullptr))
		*(float*)pOut = t;
}

static void __fastcall EnergizedEndTime_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut = 0; *((uint64_t*)pOut + 1) = 0; *((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;
	float t = 0.0f;
	if (EnergizeBridge_WireGet(pStruct, nullptr, nullptr, &t))
		*(float*)pOut = t;
}

// [LOCKEDSET-SIDECAR] / [LASER-SIDECAR] Two more aliased DT_WeaponX rows: the append offsets for m_lockedSet (6136) and m_targetingLaserEnabledScript (6168) land inside the live CWeaponX's m_modVars, so both serve SDK sidecars.
// Their setters keep MarkEntityEdictDirty, which is load-bearing here: with no entity write left, the dirty edict is the only signal that makes the snapshot re-pack and run these proxies, and neither value changes often enough to rely on a sibling prop dirtying the weapon in the same frame.
static void __fastcall LockedSet_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut = 0; *((uint64_t*)pOut + 1) = 0; *((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;
	*(int*)pOut = WeaponScriptVars_WireGetLockedSet(pStruct);
}

static void __fastcall TargetingLaserEnabled_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut = 0; *((uint64_t*)pOut + 1) = 0; *((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;
	*(int*)pOut = WeaponScriptVars_WireGetTargetingLaserEnabled(pStruct);
}

// DT_WeaponX_PredictingClientOnly.m_shotIndexForSpread value proxy. pStruct = the weapon entity base.
// S21 split S3's single shot counter in two; this serves S3's one counter (the same field m_shotCount publishes) to the client's spread index, so both sides index the same viewkick pattern row.
static constexpr ptrdiff_t WEAPON_SHOTCOUNT_OFFSET_LIVE = 5476;

static ConVar bridge_weapon_spread_index("bridge_weapon_spread_index", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Publish the server's shot counter as DT_WeaponX_PredictingClientOnly."
	"m_shotIndexForSpread so the client's viewkick pattern row and weapon spread "
	"index match the server's. 0 = off (client free-runs the index).");

static void __fastcall ShotIndexForSpread_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;

	// ConVar gates only the emitted value -- the prop stays registered either way.
	if (!bridge_weapon_spread_index.GetBool())
		return;

	int off = DTExtend_FindNativePropOffset(pStruct, "m_shotCount");
	if (off < 0)
		off = static_cast<int>(WEAPON_SHOTCOUNT_OFFSET_LIVE);

	*(int*)pOut = *reinterpret_cast<const int*>(
		reinterpret_cast<uintptr_t>(pStruct) + off);

	static bool s_announced = false;
	if (!s_announced && *(int*)pOut != 0)
	{
		s_announced = true;
		Msg(eDLL_T::SERVER, "[SPREAD-IDX] first publish: shotIndexForSpread=%d off=%d\n",
			*(int*)pOut, off);
	}
}

// ---------------------------------------------------------------------------
// [JETDRIVE-WIRE] DT_LocalPlayerExclusive jetdrive value proxies (14). pStruct = the PLAYER entity base: m_Local is embedded directly in CPlayer, so this sub-table's leaf props address the same object DT_Player's do.
// These carry no entity-memory slot on purpose, so DTExtend_BaseOffset's 20000 entry is never consumed.
// ---------------------------------------------------------------------------
static void JetDrive_EmitWireField(void* pStruct, void* pOut, size_t fieldOff, size_t fieldBytes)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;

	JetDriveWire wire;
	if (!JetDrive_GetWire(pStruct, &wire))
		return;

	memcpy(pOut, reinterpret_cast<const uint8_t*>(&wire) + fieldOff, fieldBytes);
}

#define JD_WIRE_PROXY(fn, member)                                              \
	static void __fastcall fn(void* /*pProp*/, void* pStruct, void* /*pData*/, \
		void* pOut, int /*iElement*/, int /*objectID*/) \
	{                                                                          \
		JetDrive_EmitWireField(pStruct, pOut,                                  \
			offsetof(JetDriveWire, member), sizeof(JetDriveWire::member));     \
	}

JD_WIRE_PROXY(JetDriveWasActive_ValueProxy,             m_wasActive)
JD_WIRE_PROXY(JetDriveActive_ValueProxy,                m_active)
JD_WIRE_PROXY(JetDriveTargetEnt_ValueProxy,             m_targetEnt)
JD_WIRE_PROXY(JetDriveInDecelWindow_ValueProxy,         m_inDecelWindow)
JD_WIRE_PROXY(JetDriveSpeed_ValueProxy,                 m_speed)
JD_WIRE_PROXY(JetDriveAccel_ValueProxy,                 m_accel)
JD_WIRE_PROXY(JetDriveTimeout_ValueProxy,               m_timeout)
JD_WIRE_PROXY(JetDriveDoubleJumpVelBackFrac_ValueProxy, m_doubleJumpVelBackFrac)
JD_WIRE_PROXY(JetDriveStartTime_ValueProxy,             m_startTime)
JD_WIRE_PROXY(JetDriveDecelWindowTimeOut_ValueProxy,    m_decelWindowTimeOutTime)
JD_WIRE_PROXY(JetDriveTargetPos_ValueProxy,             m_targetPos)
JD_WIRE_PROXY(JetDriveTargetEntOffset_ValueProxy,       m_targetEntOffset)
JD_WIRE_PROXY(JetDriveStartPos_ValueProxy,              m_startPos)
JD_WIRE_PROXY(JetDriveDoubleJumpVelocity_ValueProxy,    m_doubleJumpVelocity)

#undef JD_WIRE_PROXY

// Names must match the s_extendProps "DT_LocalPlayerExclusive" block exactly;
// a typo here silently falls through to a RAW SLACK READ at base 20000.
static const struct { const char* propName; DTExtendProxyFn proxy; } s_jetDriveWireProxies[] = {
	{ "m_jetDriveWasActive",              &JetDriveWasActive_ValueProxy },
	{ "m_jetDriveActive",                 &JetDriveActive_ValueProxy },
	{ "m_jetDriveTargetEnt",              &JetDriveTargetEnt_ValueProxy },
	{ "m_jetDriveInDecelWindow",          &JetDriveInDecelWindow_ValueProxy },
	{ "m_jetDriveSpeed",                  &JetDriveSpeed_ValueProxy },
	{ "m_jetDriveAccel",                  &JetDriveAccel_ValueProxy },
	{ "m_jetDriveTimeout",                &JetDriveTimeout_ValueProxy },
	{ "m_jetDriveDoubleJumpVelBackFrac",  &JetDriveDoubleJumpVelBackFrac_ValueProxy },
	{ "m_jetDriveStartTime",              &JetDriveStartTime_ValueProxy },
	{ "m_jetDriveDecelWindowTimeOutTime", &JetDriveDecelWindowTimeOut_ValueProxy },
	{ "m_jetDriveTargetPos",              &JetDriveTargetPos_ValueProxy },
	{ "m_jetDriveTargetEntOffset",        &JetDriveTargetEntOffset_ValueProxy },
	{ "m_jetDriveStartPos",               &JetDriveStartPos_ValueProxy },
	{ "m_jetDriveDoubleJumpVelocity",     &JetDriveDoubleJumpVelocity_ValueProxy },
};

// Sibling of s_jetDriveWireProxies: same table, same dispatcher scan path.
static void __fastcall DeathFieldIndex_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;

	const int32_t index = DeathField_GetIndexForPlayer(pStruct);
	*reinterpret_cast<int32_t*>(pOut) = index;
}

static const struct { const char* propName; DTExtendProxyFn proxy; } s_deathFieldWireProxies[] = {
	{ "m_deathFieldIndex", &DeathFieldIndex_ValueProxy },
};

// [TRIGGRAV-WIRE] DT_LocalPlayerExclusive gravity-lift / blackhole bits.
// Same shape as jetdrive: zero the 24-byte out buffer, sidecar read, no entity
// memory. A player with no slot emits zeros (idle for both bits).
static void __fastcall GravityLiftActive_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;

	TriggerGravityWire wire;
	if (!TriggerGravity_GetWire(pStruct, &wire))
		return;

	*reinterpret_cast<int*>(pOut) = wire.m_gravityLiftActive;

	static volatile LONG s_hitN = 0;
	const LONG n = InterlockedIncrement(&s_hitN);
	if (n <= 8)
		Warning(eDLL_T::SERVER, "[TRIGGRAV-WIRE] proxy hit #%d ent=%p lift=%d\n",
			static_cast<int>(n), pStruct, wire.m_gravityLiftActive);
}

static void __fastcall BlackholeActive_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;

	TriggerGravityWire wire;
	if (!TriggerGravity_GetWire(pStruct, &wire))
		return;

	*reinterpret_cast<int*>(pOut) = wire.m_blackholeActive;

	static volatile LONG s_hitN = 0;
	const LONG n = InterlockedIncrement(&s_hitN);
	if (n <= 8)
		Warning(eDLL_T::SERVER, "[TRIGGRAV-WIRE] proxy hit #%d ent=%p blackhole=%d\n",
			static_cast<int>(n), pStruct, wire.m_blackholeActive);
}

// Names must match the s_extendProps DT_LocalPlayerExclusive block exactly;
// a typo here silently falls through to a RAW SLACK READ at base 20000.
static const struct { const char* propName; DTExtendProxyFn proxy; } s_triggerGravityWireProxies[] = {
	{ "m_gravityLiftActive", &GravityLiftActive_ValueProxy },
	{ "m_blackholeActive",   &BlackholeActive_ValueProxy },
};

// [UPDRAFT-WIRE] DT_LocalPlayerExclusive updraft value proxies (12).
// Same shape as jetdrive: zero the 24-byte out buffer, sidecar read, no entity
// memory. A player with no slot emits zeros (correct idle for all twelve).
static void Updraft_EmitWireField(void* pStruct, void* pOut, size_t fieldOff, size_t fieldBytes)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;

	UpdraftState wire;
	if (!UpdraftBridge_GetWire(pStruct, &wire))
		return;

	memcpy(pOut, reinterpret_cast<const uint8_t*>(&wire) + fieldOff, fieldBytes);

	if (UpdraftBridge_DiagEnabled())
	{
		static volatile LONG s_hitN = 0;
		const LONG n = InterlockedIncrement(&s_hitN);
		if (n <= 8)
			Warning(eDLL_T::SERVER, "[UPDRAFT-WIRE] proxy hit #%d ent=%p count=%d stage=%d\n",
				static_cast<int>(n), pStruct, wire.m_updraftCount, wire.m_updraftStage);
	}
}

#define UD_WIRE_PROXY(fn, member)                                              \
	static void __fastcall fn(void* /*pProp*/, void* pStruct, void* /*pData*/, \
		void* pOut, int /*iElement*/, int /*objectID*/) \
	{                                                                          \
		Updraft_EmitWireField(pStruct, pOut,                                   \
			offsetof(UpdraftState, member), sizeof(UpdraftState::member));     \
	}

UD_WIRE_PROXY(UpdraftCount_ValueProxy,                      m_updraftCount)
UD_WIRE_PROXY(UpdraftStage_ValueProxy,                      m_updraftStage)
UD_WIRE_PROXY(UpdraftEnterTime_ValueProxy,                  m_updraftEnterTime)
UD_WIRE_PROXY(UpdraftLeaveTime_ValueProxy,                  m_updraftLeaveTime)
UD_WIRE_PROXY(UpdraftMinShakeActivationHeight_ValueProxy,   m_updraftMinShakeActivationHeight)
UD_WIRE_PROXY(UpdraftMaxShakeActivationHeight_ValueProxy,   m_updraftMaxShakeActivationHeight)
UD_WIRE_PROXY(UpdraftLiftActivationHeight_ValueProxy,       m_updraftLiftActivationHeight)
UD_WIRE_PROXY(UpdraftLiftSpeed_ValueProxy,                  m_updraftLiftSpeed)
UD_WIRE_PROXY(UpdraftLiftAcceleration_ValueProxy,           m_updraftLiftAcceleration)
UD_WIRE_PROXY(UpdraftLiftExitDuration_ValueProxy,           m_updraftLiftExitDuration)
UD_WIRE_PROXY(UpdraftSlowTime_ValueProxy,                   m_updraftSlowTime)
UD_WIRE_PROXY(SkydiveFromUpdraft_ValueProxy,                m_skydiveFromUpdraft)

#undef UD_WIRE_PROXY

static const struct { const char* propName; DTExtendProxyFn proxy; } s_updraftWireProxies[] = {
	{ "m_updraftCount",                      &UpdraftCount_ValueProxy },
	{ "m_updraftStage",                      &UpdraftStage_ValueProxy },
	{ "m_updraftEnterTime",                  &UpdraftEnterTime_ValueProxy },
	{ "m_updraftLeaveTime",                  &UpdraftLeaveTime_ValueProxy },
	{ "m_updraftMinShakeActivationHeight",   &UpdraftMinShakeActivationHeight_ValueProxy },
	{ "m_updraftMaxShakeActivationHeight",   &UpdraftMaxShakeActivationHeight_ValueProxy },
	{ "m_updraftLiftActivationHeight",       &UpdraftLiftActivationHeight_ValueProxy },
	{ "m_updraftLiftSpeed",                  &UpdraftLiftSpeed_ValueProxy },
	{ "m_updraftLiftAcceleration",           &UpdraftLiftAcceleration_ValueProxy },
	{ "m_updraftLiftExitDuration",           &UpdraftLiftExitDuration_ValueProxy },
	{ "m_updraftSlowTime",                   &UpdraftSlowTime_ValueProxy },
	{ "m_skydiveFromUpdraft",                &SkydiveFromUpdraft_ValueProxy },
};

static void PlayerLaunch_EmitWireField(void* pStruct, void* pOut, size_t fieldOff, size_t fieldBytes)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;

	PlayerLaunchWire wire;
	if (!PlayerLaunch_GetWire(pStruct, &wire))
		return;

	memcpy(pOut, reinterpret_cast<const uint8_t*>(&wire) + fieldOff, fieldBytes);
}

#define PL_WIRE_PROXY(fn, member)                                              \
	static void __fastcall fn(void* /*pProp*/, void* pStruct, void* /*pData*/, \
		void* pOut, int /*iElement*/, int /*objectID*/) \
	{                                                                          \
		PlayerLaunch_EmitWireField(pStruct, pOut,                              \
			offsetof(PlayerLaunchWire, member), sizeof(PlayerLaunchWire::member)); \
	}

PL_WIRE_PROXY(PlayerLaunchAvoidedMantle_ValueProxy,  m_avoidedMantle)
PL_WIRE_PROXY(PlayerLaunchLock3pRotation_ValueProxy, m_lock3pRotation)
PL_WIRE_PROXY(PlayerLaunchVelocity_ValueProxy,       m_velocity)
PL_WIRE_PROXY(PlayerLaunchStartTime_ValueProxy,      m_startTime)

#undef PL_WIRE_PROXY

static void __fastcall PlayerLaunchActivate_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	PlayerLaunch_EmitWireField(pStruct, pOut,
		offsetof(PlayerLaunchWire, m_activate), sizeof(PlayerLaunchWire::m_activate));
	PlayerLaunch_NoteActivateSampled(pStruct);
}

static const struct { const char* propName; DTExtendProxyFn proxy; } s_playerLaunchWireProxies[] = {
	{ "m_playerLaunchActivate",       &PlayerLaunchActivate_ValueProxy },
	{ "m_playerLaunchAvoidedMantle",  &PlayerLaunchAvoidedMantle_ValueProxy },
	{ "m_playerLaunchLock3pRotation", &PlayerLaunchLock3pRotation_ValueProxy },
	{ "m_playerLaunchVelocity",       &PlayerLaunchVelocity_ValueProxy },
	{ "m_playerLaunchStartTime",      &PlayerLaunchStartTime_ValueProxy },
};


// ===========================================================================
// [ITEMFLAVOR] -- DT_BaseAnimating.m_itemFlavorGUID value sidecar The cosmetic identity (weapon/melee skin, artifact power source) the S21 client resolves through GetItemFlavorByGUID.
// S3 has neither the prop nor the native, so both halves are ours.
// ===========================================================================
struct ItemFlavorGUIDEntry
{
	uintptr_t entity;
	uint32_t  serial; // m_RefEHandle -- reject slot reuse mid-map
	int64_t   guid;
};

static constexpr int kItemFlavorGUIDCapacity = 2048;
static ItemFlavorGUIDEntry s_itemFlavorGUIDs[kItemFlavorGUIDCapacity] = {};
static int s_itemFlavorGUIDCount = 0;

static uint32_t ItemFlavor_ReadSerial(const void* pEntity)
{
	if (!pEntity)
		return 0;
	return SDKEntityState_GetHandle(pEntity).Raw();
}

static void ItemFlavor_RemoveAt(int idx)
{
	if (idx < 0 || idx >= s_itemFlavorGUIDCount)
		return;
	s_itemFlavorGUIDs[idx] = s_itemFlavorGUIDs[s_itemFlavorGUIDCount - 1];
	--s_itemFlavorGUIDCount;
}

bool DTExtend_ClearItemFlavorGUID(const void* pEntity)
{
	if (!pEntity)
		return false;
	const uintptr_t e = reinterpret_cast<uintptr_t>(pEntity);
	for (int i = 0; i < s_itemFlavorGUIDCount; ++i)
	{
		if (s_itemFlavorGUIDs[i].entity == e)
		{
			ItemFlavor_RemoveAt(i);
			return true;
		}
	}
	return false;
}

bool DTExtend_SetItemFlavorGUID(const void* pEntity, int64_t guid)
{
	if (!pEntity)
		return false;
	if (guid == 0)
		return DTExtend_ClearItemFlavorGUID(pEntity);

	const uintptr_t e = reinterpret_cast<uintptr_t>(pEntity);
	const uint32_t serial = ItemFlavor_ReadSerial(pEntity);
	for (int i = 0; i < s_itemFlavorGUIDCount; ++i)
	{
		if (s_itemFlavorGUIDs[i].entity == e)
		{
			s_itemFlavorGUIDs[i].guid = guid;
			s_itemFlavorGUIDs[i].serial = serial;
			return true;
		}
	}
	if (s_itemFlavorGUIDCount >= kItemFlavorGUIDCapacity)
	{
		static volatile LONG s_fullN = 0;
		if (InterlockedIncrement(&s_fullN) <= 4)
			Warning(eDLL_T::SERVER,
				"[ITEMFLAVOR] sidecar full at %d entries -- GUID dropped\n",
				kItemFlavorGUIDCapacity);
		return false;
	}
	s_itemFlavorGUIDs[s_itemFlavorGUIDCount].entity = e;
	s_itemFlavorGUIDs[s_itemFlavorGUIDCount].serial = serial;
	s_itemFlavorGUIDs[s_itemFlavorGUIDCount].guid   = guid;
	++s_itemFlavorGUIDCount;
	return true;
}

bool DTExtend_GetItemFlavorGUID(const void* pEntity, int64_t* outGuid)
{
	if (!pEntity)
		return false;
	const uintptr_t e = reinterpret_cast<uintptr_t>(pEntity);
	const uint32_t serial = ItemFlavor_ReadSerial(pEntity);
	for (int i = 0; i < s_itemFlavorGUIDCount; ++i)
	{
		if (s_itemFlavorGUIDs[i].entity != e)
			continue;
		// Pointer reused for a new entity -- drop the stale cosmetic identity.
		if (s_itemFlavorGUIDs[i].serial != serial)
		{
			ItemFlavor_RemoveAt(i);
			return false;
		}
		if (outGuid)
			*outGuid = s_itemFlavorGUIDs[i].guid;
		return true;
	}
	return false;
}

// [NATIVE-ROUTE] S21 declares these on a leaf table that S3 builds FLAT -- the
// dedi owns the field at its CBaseEntity offset but never lists it on that leaf,
// so the append has a real source and does not need entity slack at all.
// Offsets from the server DT_BaseEntity builder.
static constexpr uintptr_t ENT_OFF_RENDERMODE     = 221;  // byte
static constexpr uintptr_t ENT_OFF_CLRRENDER      = 224;  // color32
static constexpr uintptr_t ENT_OFF_PHASESHIFTFLAGS = 1184;
// 1160/1164 is the m_iMaxHealth/m_iHealth pair; DT_ScriptProp, DT_Turret and
// DT_Player all publish it at the same offsets, so it is a CBaseEntity field.
static constexpr uintptr_t ENT_OFF_MAXHEALTH      = 1160;

static void __fastcall NativeRenderMode_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;
	*(int32_t*)pOut = *reinterpret_cast<const uint8_t*>(
		reinterpret_cast<uintptr_t>(pStruct) + ENT_OFF_RENDERMODE);
}

static void __fastcall NativeClrRender_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;
	*(int32_t*)pOut = *reinterpret_cast<const int32_t*>(
		reinterpret_cast<uintptr_t>(pStruct) + ENT_OFF_CLRRENDER);
}

static void __fastcall NativePhaseShiftFlags_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;
	*(int32_t*)pOut = *reinterpret_cast<const int32_t*>(
		reinterpret_cast<uintptr_t>(pStruct) + ENT_OFF_PHASESHIFTFLAGS);
}

static void __fastcall NativeMaxHealth_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;
	*(int32_t*)pOut = *reinterpret_cast<const int32_t*>(
		reinterpret_cast<uintptr_t>(pStruct) + ENT_OFF_MAXHEALTH);
}

static void __fastcall ItemFlavorGUID_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;

	int64_t guid = 0;
	if (!DTExtend_GetItemFlavorGUID(pStruct, &guid) || guid == 0)
		return;
	*reinterpret_cast<int32_t*>(pOut) = static_cast<int32_t>(guid);
}

// ===========================================================================
// DT_BaseEntity.m_ignoreParentRotation S21 RecvProp + C_BaseEntity::GetParentToWorldTransform already honor this bool (SetNoRotationMatrix when set).
// S3 has no native field -- the EI append at DT_BaseEntity base 2500 is not safe slack on every carrier, so the value lives in a sidecar and a value proxy serves the wire.
// ===========================================================================
struct IgnoreParentRotEntry
{
	uintptr_t entity;
	uint32_t  serial;
	bool      ignore;
};

static constexpr int kIgnoreParentRotCapacity = 256;
static IgnoreParentRotEntry s_ignoreParentRot[kIgnoreParentRotCapacity] = {};
static int s_ignoreParentRotCount = 0;

static uint32_t IgnoreParentRot_ReadSerial(const void* pEntity)
{
	if (!pEntity)
		return 0;
	return SDKEntityState_GetHandle(pEntity).Raw();
}

static void IgnoreParentRot_RemoveAt(int idx)
{
	if (idx < 0 || idx >= s_ignoreParentRotCount)
		return;
	s_ignoreParentRot[idx] = s_ignoreParentRot[s_ignoreParentRotCount - 1];
	--s_ignoreParentRotCount;
}

void DTExtend_SetIgnoreParentRotation(const void* pEntity, bool bIgnore)
{
	if (!pEntity)
		return;

	const uintptr_t e = reinterpret_cast<uintptr_t>(pEntity);
	const uint32_t serial = IgnoreParentRot_ReadSerial(pEntity);
	for (int i = 0; i < s_ignoreParentRotCount; ++i)
	{
		if (s_ignoreParentRot[i].entity == e)
		{
			if (!bIgnore)
			{
				IgnoreParentRot_RemoveAt(i);
				return;
			}
			s_ignoreParentRot[i].serial = serial;
			s_ignoreParentRot[i].ignore = true;
			return;
		}
	}

	if (!bIgnore)
		return;

	if (s_ignoreParentRotCount >= kIgnoreParentRotCapacity)
	{
		static volatile LONG s_fullN = 0;
		if (InterlockedIncrement(&s_fullN) <= 4)
			Warning(eDLL_T::SERVER,
				"[IGN-PARENT-ROT] sidecar full at %d entries -- write dropped\n",
				kIgnoreParentRotCapacity);
		return;
	}

	s_ignoreParentRot[s_ignoreParentRotCount].entity = e;
	s_ignoreParentRot[s_ignoreParentRotCount].serial = serial;
	s_ignoreParentRot[s_ignoreParentRotCount].ignore = true;
	++s_ignoreParentRotCount;
}

bool DTExtend_GetIgnoreParentRotation(const void* pEntity)
{
	if (!pEntity)
		return false;

	const uintptr_t e = reinterpret_cast<uintptr_t>(pEntity);
	const uint32_t serial = IgnoreParentRot_ReadSerial(pEntity);
	for (int i = 0; i < s_ignoreParentRotCount; ++i)
	{
		if (s_ignoreParentRot[i].entity != e)
			continue;
		if (s_ignoreParentRot[i].serial != serial)
		{
			IgnoreParentRot_RemoveAt(i);
			return false;
		}
		return s_ignoreParentRot[i].ignore;
	}
	return false;
}

static void __fastcall IgnoreParentRotation_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut)
		return;
	*reinterpret_cast<int32_t*>(pOut) = 0;
	if (!pStruct)
		return;
	*reinterpret_cast<int32_t*>(pOut) = DTExtend_GetIgnoreParentRotation(pStruct) ? 1 : 0;
}

// ===========================================================================
// DT_BaseAnimating.m_animRelativeToGroundEnabled
// S21 RecvProp + consumer already honor this; S3 has no field and no native.
// Sidecar + value proxy; publish 1 only while the native collision flag is set.
// ===========================================================================
struct AnimRelativeToGroundEntry
{
	uintptr_t entity;
	uint32_t  serial;
	bool      enabled;
};

static constexpr int kAnimRelativeToGroundCapacity = 256;
static AnimRelativeToGroundEntry s_animRelativeToGround[kAnimRelativeToGroundCapacity] = {};
static int s_animRelativeToGroundCount = 0;

static constexpr ptrdiff_t ENT_OFF_ANIM_COLLISION_ENABLED = 0xDC1; // CBaseAnimating::m_animCollisionEnabled

static uint32_t AnimRelativeToGround_ReadSerial(const void* pEntity)
{
	if (!pEntity)
		return 0;
	return SDKEntityState_GetHandle(pEntity).Raw();
}

static void AnimRelativeToGround_RemoveAt(int idx)
{
	if (idx < 0 || idx >= s_animRelativeToGroundCount)
		return;
	s_animRelativeToGround[idx] = s_animRelativeToGround[s_animRelativeToGroundCount - 1];
	--s_animRelativeToGroundCount;
}

void DTExtend_SetAnimRelativeToGround(const void* pEntity, bool bEnable)
{
	if (!pEntity)
		return;

	const uintptr_t e = reinterpret_cast<uintptr_t>(pEntity);
	const uint32_t serial = AnimRelativeToGround_ReadSerial(pEntity);
	for (int i = 0; i < s_animRelativeToGroundCount; ++i)
	{
		if (s_animRelativeToGround[i].entity == e)
		{
			if (!bEnable)
			{
				AnimRelativeToGround_RemoveAt(i);
				return;
			}
			s_animRelativeToGround[i].serial = serial;
			s_animRelativeToGround[i].enabled = true;
			return;
		}
	}

	if (!bEnable)
		return;

	if (s_animRelativeToGroundCount >= kAnimRelativeToGroundCapacity)
	{
		static volatile LONG s_fullN = 0;
		if (InterlockedIncrement(&s_fullN) <= 4)
			Warning(eDLL_T::SERVER,
				"[ANIM-RTG] sidecar full at %d entries -- write dropped\n",
				kAnimRelativeToGroundCapacity);
		return;
	}

	s_animRelativeToGround[s_animRelativeToGroundCount].entity = e;
	s_animRelativeToGround[s_animRelativeToGroundCount].serial = serial;
	s_animRelativeToGround[s_animRelativeToGroundCount].enabled = true;
	++s_animRelativeToGroundCount;
}

bool DTExtend_GetAnimRelativeToGround(const void* pEntity)
{
	if (!pEntity)
		return false;

	const uintptr_t e = reinterpret_cast<uintptr_t>(pEntity);
	const uint32_t serial = AnimRelativeToGround_ReadSerial(pEntity);
	for (int i = 0; i < s_animRelativeToGroundCount; ++i)
	{
		if (s_animRelativeToGround[i].entity != e)
			continue;
		if (s_animRelativeToGround[i].serial != serial)
		{
			AnimRelativeToGround_RemoveAt(i);
			return false;
		}
		return s_animRelativeToGround[i].enabled;
	}
	return false;
}

static void __fastcall AnimRelativeToGround_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut)
		return;
	*reinterpret_cast<int32_t*>(pOut) = 0;
	if (!pStruct)
		return;
	if (!DTExtend_GetAnimRelativeToGround(pStruct))
		return;

	// Scripted-anim start zeroes collision; a stale sidecar must not keep publishing 1.
	const uint8_t* pCollision = reinterpret_cast<const uint8_t*>(pStruct) + ENT_OFF_ANIM_COLLISION_ENABLED;
	if (!DTExtend_IsSafeToRead(pCollision, sizeof(uint8_t)) || *pCollision == 0)
		return;

	*reinterpret_cast<int32_t*>(pOut) = 1;
}

static int (*v_CBaseAnimating_AnimEnableCollision)(uintptr_t thisptr) = nullptr;

static int Hook_CBaseAnimating_AnimEnableCollision(uintptr_t thisptr)
{
	const int result = v_CBaseAnimating_AnimEnableCollision(thisptr);

	if (thisptr)
		DTExtend_SetAnimRelativeToGround(reinterpret_cast<const void*>(thisptr), false);

	return result;
}

// ===========================================================================
// [SCOPEHL] -- DT_BaseEntity.m_wantsScopeHighlight S21 declares this on DT_BaseEntity; S3 registers it natively only on DT_DynamicProp (its ServerClass registration + decoder build).
// CreateDecoders pairs by name WITHIN a table, so the native prop can never reach the client no matter where it sits in flat order -- the DT_BaseEntity append is the only copy that CAN bind, and un-proxied it served the synthetic append region.
// ===========================================================================
uint8_t* DTExtend_FindTableByName(const char* name);
int DTExtend_FindPropIdx(uint8_t* table, const char* propName);

// Latches only on success: the table cache fills as tables are seen, so a miss
// means "not yet", never "absent".
static int ScopeHighlight_NativeOffset(void)
{
	static int s_off = -1;
	if (s_off >= 0)
		return s_off;

	uint8_t* const table = DTExtend_FindTableByName("DT_DynamicProp");
	const int idx = table ? DTExtend_FindPropIdx(table, "m_wantsScopeHighlight") : -1;
	if (idx < 0)
	{
		static volatile LONG s_warnN = 0;
		if (InterlockedIncrement(&s_warnN) == 1)
			Warning(eDLL_T::SERVER, "[SCOPEHL] DT_DynamicProp.m_wantsScopeHighlight "
				"unresolved -- the appended prop sends 0 until it caches\n");
		return -1;
	}

	uint8_t* const props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
	const uint8_t* const prop = props + static_cast<uint64_t>(idx) * SP_SIZE;
	s_off = *reinterpret_cast<const int*>(prop + SP_OFFSET) & 0xFFFFF;
	Msg(eDLL_T::SERVER, "[SCOPEHL] native field resolved at entity+0x%X\n", s_off);
	return s_off;
}

static void __fastcall WantsScopeHighlight_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*reinterpret_cast<int32_t*>(pOut) = 0;
	if (!pStruct) return;

	const int off = ScopeHighlight_NativeOffset();
	if (off < 0 || !DTExtend_EntityHasSendTable(pStruct, "DT_DynamicProp"))
		return;

	// Byte-sized bool: the native prop and its neighbour sit at consecutive
	// entity offsets (0x12F3 / 0x12F4 on the server half of this build).
	const uintptr_t field = reinterpret_cast<uintptr_t>(pStruct) + off;
	if (!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(field), sizeof(uint8_t)))
		return;

	*reinterpret_cast<int32_t*>(pOut) = *reinterpret_cast<const uint8_t*>(field) ? 1 : 0;
}

static void PlayerExtend_EmitPlayer(void* pStruct, void* pOut, size_t fieldOff, size_t fieldBytes)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;
	PlayerExtendBundle bundle;
	if (!PlayerExtend_GetBundle(pStruct, &bundle))
		return;
	memcpy(pOut, reinterpret_cast<const uint8_t*>(&bundle.player) + fieldOff, fieldBytes);
}

static void PlayerExtend_EmitBCC(void* pStruct, void* pOut, size_t fieldOff, size_t fieldBytes)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;
	PlayerExtendBundle bundle;
	if (!PlayerExtend_GetBundle(pStruct, &bundle))
		return;
	memcpy(pOut, reinterpret_cast<const uint8_t*>(&bundle.bcc) + fieldOff, fieldBytes);
}

#define PX_WIRE_PROXY(fn, member)                                              \
	static void __fastcall fn(void* /*pProp*/, void* pStruct, void* /*pData*/, \
		void* pOut, int /*iElement*/, int /*objectID*/) \
	{                                                                          \
		PlayerExtend_EmitPlayer(pStruct, pOut,                                 \
			offsetof(PlayerExtendWire, member), sizeof(PlayerExtendWire::member)); \
	}

#define BX_WIRE_PROXY(fn, member)                                              \
	static void __fastcall fn(void* /*pProp*/, void* pStruct, void* /*pData*/, \
		void* pOut, int /*iElement*/, int /*objectID*/) \
	{                                                                          \
		PlayerExtend_EmitBCC(pStruct, pOut,                                    \
			offsetof(BCCExtendWire, member), sizeof(BCCExtendWire::member));   \
	}

PX_WIRE_PROXY(PxArmoredLeapPhase_ValueProxy, m_armoredLeapPhase)
PX_WIRE_PROXY(PxArmoredLeapStartTime_ValueProxy, m_armoredLeapStartTime)
PX_WIRE_PROXY(PxArmoredLeapType_ValueProxy, m_armoredLeapType)
PX_WIRE_PROXY(PxHasMatchAdminRole_ValueProxy, m_bHasMatchAdminRole)
PX_WIRE_PROXY(PxBleedoutStartTime_ValueProxy, m_bleedoutStartTime)
PX_WIRE_PROXY(PxCommunicationsAutoBlocked_ValueProxy, m_communicationsAutoBlocked)
PX_WIRE_PROXY(PxCrossPlayChat_ValueProxy, m_crossPlayChat)
PX_WIRE_PROXY(PxCrossPlayChatFriends_ValueProxy, m_crossPlayChatFriends)
PX_WIRE_PROXY(PxCrossProgressionMigrated_ValueProxy, m_crossProgressionMigrated)
PX_WIRE_PROXY(PxDragReviveOutroStartTime_ValueProxy, m_dragReviveOutroStartTime)
PX_WIRE_PROXY(PxDragReviveState_ValueProxy, m_dragReviveState)
PX_WIRE_PROXY(PxExtraShieldHealth_ValueProxy, m_extraShieldHealth)
PX_WIRE_PROXY(PxExtraShieldTier_ValueProxy, m_extraShieldTier)
PX_WIRE_PROXY(PxJumpPadDebounceExpireTime_ValueProxy, m_jumpPadDebounceExpireTime)
PX_WIRE_PROXY(PxLaserSightColorCustomized_ValueProxy, m_laserSightColorCustomized)
PX_WIRE_PROXY(PxLastSprintPressTime_ValueProxy, m_lastSprintPressTime)
PX_WIRE_PROXY(PxLauncherAirControlActive_ValueProxy, m_launcherAirControlActive)
PX_WIRE_PROXY(PxMantleBoostState_ValueProxy, m_mantleBoostState)
PX_WIRE_PROXY(PxHoldToSprint_ValueProxy, m_playerSettingForHoldToSprint)
PX_WIRE_PROXY(PxStickySprintForward_ValueProxy, m_playerSettingForStickySprintForward)
PX_WIRE_PROXY(PxPlayerVehicleUseTime_ValueProxy, m_playerVehicleUseTime)

// S3 CPlayer::m_playerVehicle @ +0x6120 is a memory EHANDLE
// (serial<<16)|index. S21 RecvPropEHandle unpacks (serial<<14)|(index&0x3FFF)
// with invalid=0xFFFFFF. Emitting the memory dword makes Get() fail the serial.
static constexpr ptrdiff_t kPlayerVehicleHandleOff = 0x6120;

static int32_t Player_NativeVehicleHandle(const void* pStruct)
{
	if (!pStruct)
		return -1;
	const void* field = static_cast<const char*>(pStruct) + kPlayerVehicleHandleOff;
	if (!DTExtend_IsSafeToRead(field, sizeof(int32_t)))
		return -1;
	return *static_cast<const int32_t*>(field);
}

static void __fastcall PxPlayerVehicleDriven_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut)
		return;
	const int32_t s3Handle = Player_NativeVehicleHandle(pStruct);
	const int32_t wire = SDKEntityState_PackS21RecvEHandle(s3Handle);
	*reinterpret_cast<int32_t*>(pOut) = wire;

	static volatile LONG s_vehDrivenLog = 0;
	if (s3Handle != -1)
	{
		const LONG n = InterlockedIncrement(&s_vehDrivenLog);
		if (n <= 8)
			Msg(eDLL_T::ENGINE,
				"[VEH-DRIVEN] s3=0x%X wire=0x%X idx=%d ser=%d (#%d)\n",
				s3Handle, wire, s3Handle & 0xFFFF, (s3Handle >> 16) & 0xFFFF,
				static_cast<int>(n));
	}
}

static void __fastcall PxPlayerVehicleCount_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut)
		return;
	const int32_t handle = Player_NativeVehicleHandle(pStruct);
	*reinterpret_cast<int32_t*>(pOut) = (handle != -1) ? 1 : 0;
}
PX_WIRE_PROXY(PxRagdollCreationYaw_ValueProxy, m_ragdollCreationYaw)
PX_WIRE_PROXY(PxReviveTarget_ValueProxy, m_reviveTarget)
PX_WIRE_PROXY(PxShadowShieldActive_ValueProxy, m_shadowShieldActive)
PX_WIRE_PROXY(PxSkydiveFromSkywardLaunch_ValueProxy, m_skydiveFromSkywardLaunch)
PX_WIRE_PROXY(PxSkydiveState_ValueProxy, m_skydiveState)
PX_WIRE_PROXY(PxSkywardLaunchEndTime_ValueProxy, m_skywardLaunchEndTime)
PX_WIRE_PROXY(PxSkywardLaunchFastEndTime_ValueProxy, m_skywardLaunchFastEndTime)
PX_WIRE_PROXY(PxSkywardLaunchFastSpeed_ValueProxy, m_skywardLaunchFastSpeed)
PX_WIRE_PROXY(PxSkywardLaunchFollowing_ValueProxy, m_skywardLaunchFollowing)
PX_WIRE_PROXY(PxSkywardLaunchInterrupted_ValueProxy, m_skywardLaunchInterrupted)
PX_WIRE_PROXY(PxSkywardLaunchSlowEndTime_ValueProxy, m_skywardLaunchSlowEndTime)
PX_WIRE_PROXY(PxSkywardLaunchSlowSpeed_ValueProxy, m_skywardLaunchSlowSpeed)
PX_WIRE_PROXY(PxSkywardLaunchSlowStartTime_ValueProxy, m_skywardLaunchSlowStartTime)
PX_WIRE_PROXY(PxSkywardLaunchState_ValueProxy, m_skywardLaunchState)
PX_WIRE_PROXY(PxStickySprintForwardDisableTime_ValueProxy, m_stickySprintForwardDisableTime)
PX_WIRE_PROXY(PxStickySprintForwardEnableTime_ValueProxy, m_stickySprintForwardEnableTime)
PX_WIRE_PROXY(PxTempShieldHealth_ValueProxy, m_tempShieldHealth)
PX_WIRE_PROXY(PxTurret_ValueProxy, m_turret)
PX_WIRE_PROXY(PxUnspoofedHardware_ValueProxy, m_unspoofedHardware)
PX_WIRE_PROXY(PxEadpUserId_ValueProxy, m_EadpUserId)
PX_WIRE_PROXY(PxProgressionUserId_ValueProxy, m_progressionUserId)
PX_WIRE_PROXY(PxUnSpoofedPlatformUserId_ValueProxy, m_unSpoofedPlatformUserId)
PX_WIRE_PROXY(PxArmoredLeapAirPos_ValueProxy, m_armoredLeapAirPos)
PX_WIRE_PROXY(PxArmoredLeapEndPos_ValueProxy, m_armoredLeapEndPos)
PX_WIRE_PROXY(PxLaserSightColor_ValueProxy, m_laserSightColor)
PX_WIRE_PROXY(PxRagdollCreationOrigin_ValueProxy, m_ragdollCreationOrigin)
PX_WIRE_PROXY(PxSkywardObstacleAvoidanceEndPos_ValueProxy, m_skywardObstacleAvoidanceEndPos)
PX_WIRE_PROXY(PxSkywardOffset_ValueProxy, m_skywardOffset)

BX_WIRE_PROXY(BxWeaponTypeDisabledFlags_ValueProxy, m_weaponTypeDisabledFlags)
BX_WIRE_PROXY(BxWeaponInventorySlotLockedFlags_ValueProxy, m_weaponInventorySlotLockedFlags)
BX_WIRE_PROXY(BxPhaseShiftType_ValueProxy, m_phaseShiftType)
BX_WIRE_PROXY(BxAkimboState_ValueProxy, m_akimboState)
BX_WIRE_PROXY(BxAkimboShouldAltFire_ValueProxy, m_akimboShouldAltFire)
BX_WIRE_PROXY(BxAllowHudSelectionWhileWeaponsDisabled_ValueProxy, m_allowHudSelectionWhileWeaponsDisabled)
BX_WIRE_PROXY(BxWeaponAmmoRegenDisabled_ValueProxy, m_weaponAmmoRegenDisabled)
BX_WIRE_PROXY(BxWeaponAmmoRegenDisabledRefCount_ValueProxy, m_weaponAmmoRegenDisabledRefCount)
BX_WIRE_PROXY(BxIsPlayerOverheating_ValueProxy, m_bIsPlayerOverheating)
BX_WIRE_PROXY(BxPlayerOverheatValue_ValueProxy, m_playerOverheatValue)
BX_WIRE_PROXY(BxTimeLastGeneratedPlayerOverheat_ValueProxy, m_timeLastGeneratedPlayerOverheat)
BX_WIRE_PROXY(BxTargetInfoPingValue_ValueProxy, m_targetInfoPingValue)

#undef PX_WIRE_PROXY
#undef BX_WIRE_PROXY

static const struct { const char* propName; DTExtendProxyFn proxy; } s_playerExtendWireProxies[] = {
	{ "m_armoredLeapPhase", &PxArmoredLeapPhase_ValueProxy },
	{ "m_armoredLeapStartTime", &PxArmoredLeapStartTime_ValueProxy },
	{ "m_armoredLeapType", &PxArmoredLeapType_ValueProxy },
	{ "m_bHasMatchAdminRole", &PxHasMatchAdminRole_ValueProxy },
	{ "m_bleedoutStartTime", &PxBleedoutStartTime_ValueProxy },
	{ "m_communicationsAutoBlocked", &PxCommunicationsAutoBlocked_ValueProxy },
	{ "m_crossPlayChat", &PxCrossPlayChat_ValueProxy },
	{ "m_crossPlayChatFriends", &PxCrossPlayChatFriends_ValueProxy },
	{ "m_crossProgressionMigrated", &PxCrossProgressionMigrated_ValueProxy },
	{ "m_dragReviveOutroStartTime", &PxDragReviveOutroStartTime_ValueProxy },
	{ "m_dragReviveState", &PxDragReviveState_ValueProxy },
	{ "m_extraShieldHealth", &PxExtraShieldHealth_ValueProxy },
	{ "m_extraShieldTier", &PxExtraShieldTier_ValueProxy },
	{ "m_jumpPadDebounceExpireTime", &PxJumpPadDebounceExpireTime_ValueProxy },
	{ "m_laserSightColorCustomized", &PxLaserSightColorCustomized_ValueProxy },
	{ "m_lastSprintPressTime", &PxLastSprintPressTime_ValueProxy },
	{ "m_launcherAirControlActive", &PxLauncherAirControlActive_ValueProxy },
	{ "m_mantleBoostState", &PxMantleBoostState_ValueProxy },
	{ "m_playerSettingForHoldToSprint", &PxHoldToSprint_ValueProxy },
	{ "m_playerSettingForStickySprintForward", &PxStickySprintForward_ValueProxy },
	{ "m_playerVehicleCount", &PxPlayerVehicleCount_ValueProxy },
	{ "m_playerVehicleDriven", &PxPlayerVehicleDriven_ValueProxy },
	{ "m_playerVehicleUseTime", &PxPlayerVehicleUseTime_ValueProxy },
	{ "m_ragdollCreationYaw", &PxRagdollCreationYaw_ValueProxy },
	{ "m_reviveTarget", &PxReviveTarget_ValueProxy },
	{ "m_shadowShieldActive", &PxShadowShieldActive_ValueProxy },
	{ "m_skydiveFromSkywardLaunch", &PxSkydiveFromSkywardLaunch_ValueProxy },
	{ "m_skydiveState", &PxSkydiveState_ValueProxy },
	{ "m_skywardLaunchEndTime", &PxSkywardLaunchEndTime_ValueProxy },
	{ "m_skywardLaunchFastEndTime", &PxSkywardLaunchFastEndTime_ValueProxy },
	{ "m_skywardLaunchFastSpeed", &PxSkywardLaunchFastSpeed_ValueProxy },
	{ "m_skywardLaunchFollowing", &PxSkywardLaunchFollowing_ValueProxy },
	{ "m_skywardLaunchInterrupted", &PxSkywardLaunchInterrupted_ValueProxy },
	{ "m_skywardLaunchSlowEndTime", &PxSkywardLaunchSlowEndTime_ValueProxy },
	{ "m_skywardLaunchSlowSpeed", &PxSkywardLaunchSlowSpeed_ValueProxy },
	{ "m_skywardLaunchSlowStartTime", &PxSkywardLaunchSlowStartTime_ValueProxy },
	{ "m_skywardLaunchState", &PxSkywardLaunchState_ValueProxy },
	{ "m_stickySprintForwardDisableTime", &PxStickySprintForwardDisableTime_ValueProxy },
	{ "m_stickySprintForwardEnableTime", &PxStickySprintForwardEnableTime_ValueProxy },
	{ "m_tempShieldHealth", &PxTempShieldHealth_ValueProxy },
	{ "m_turret", &PxTurret_ValueProxy },
	{ "m_unspoofedHardware", &PxUnspoofedHardware_ValueProxy },
	{ "m_EadpUserId", &PxEadpUserId_ValueProxy },
	{ "m_progressionUserId", &PxProgressionUserId_ValueProxy },
	{ "m_unSpoofedPlatformUserId", &PxUnSpoofedPlatformUserId_ValueProxy },
	{ "m_armoredLeapAirPos", &PxArmoredLeapAirPos_ValueProxy },
	{ "m_armoredLeapEndPos", &PxArmoredLeapEndPos_ValueProxy },
	{ "m_laserSightColor", &PxLaserSightColor_ValueProxy },
	{ "m_ragdollCreationOrigin", &PxRagdollCreationOrigin_ValueProxy },
	{ "m_skywardObstacleAvoidanceEndPos", &PxSkywardObstacleAvoidanceEndPos_ValueProxy },
	{ "m_skywardOffset", &PxSkywardOffset_ValueProxy },
};

static const struct { const char* propName; DTExtendProxyFn proxy; } s_bccExtendWireProxies[] = {
	{ "m_weaponTypeDisabledFlags", &BxWeaponTypeDisabledFlags_ValueProxy },
	{ "m_weaponInventorySlotLockedFlags", &BxWeaponInventorySlotLockedFlags_ValueProxy },
	{ "m_phaseShiftType", &BxPhaseShiftType_ValueProxy },
	{ "m_akimboState", &BxAkimboState_ValueProxy },
	{ "m_akimboShouldAltFire", &BxAkimboShouldAltFire_ValueProxy },
	{ "m_allowHudSelectionWhileWeaponsDisabled", &BxAllowHudSelectionWhileWeaponsDisabled_ValueProxy },
	{ "m_weaponAmmoRegenDisabled", &BxWeaponAmmoRegenDisabled_ValueProxy },
	{ "m_weaponAmmoRegenDisabledRefCount", &BxWeaponAmmoRegenDisabledRefCount_ValueProxy },
	{ "m_bIsPlayerOverheating", &BxIsPlayerOverheating_ValueProxy },
	{ "m_playerOverheatValue", &BxPlayerOverheatValue_ValueProxy },
	{ "m_timeLastGeneratedPlayerOverheat", &BxTimeLastGeneratedPlayerOverheat_ValueProxy },
	{ "m_targetInfoPingValue", &BxTargetInfoPingValue_ValueProxy },
};

// Registry hook for DTExtend_Apply: returns the value proxy to install on an
// appended prop instead of the raw slack read / Canon_ZeroProxy, or nullptr.
DTExtendProxyFn DTExtend_ValueProxyForAppendedProp(const char* tableName, const char* propName)
{
	if (tableName && propName &&
		strcmp(tableName, "DT_GlobalNonRewinding") == 0 && strcmp(propName, "m_gameTimescale") == 0)
		return &GameTimescale_ValueProxy;
	if (tableName && propName &&
		strcmp(tableName, "DT_Projectile") == 0 && strcmp(propName, "m_launchOrigin") == 0)
		return &LaunchOrigin_ValueProxy;
	if (tableName && propName && strcmp(propName, "m_itemFlavorGUID") == 0 &&
		(strcmp(tableName, "DT_BaseAnimating") == 0 ||
		 strcmp(tableName, "DT_PropSurvival") == 0))
		return &ItemFlavorGUID_ValueProxy;
	// [NATIVE-ROUTE] see the proxies: S3 owns these at their CBaseEntity offset
	// but leaves them off the flat leaf table S21 declares them on.
	if (tableName && propName && strcmp(tableName, "DT_RopeKeyframe") == 0)
	{
		if (strcmp(propName, "m_nRenderMode") == 0) return &NativeRenderMode_ValueProxy;
		if (strcmp(propName, "m_clrRender") == 0)   return &NativeClrRender_ValueProxy;
	}
	if (tableName && propName &&
		strcmp(tableName, "DT_DynamicPropLightweight") == 0 &&
		strcmp(propName, "m_phaseShiftFlags") == 0)
		return &NativePhaseShiftFlags_ValueProxy;
	if (tableName && propName &&
		strcmp(tableName, "DT_VortexSphere") == 0 && strcmp(propName, "m_iMaxHealth") == 0)
		return &NativeMaxHealth_ValueProxy;
	if (tableName && propName &&
		strcmp(tableName, "DT_BaseEntity") == 0 && strcmp(propName, "m_wantsScopeHighlight") == 0)
		return &WantsScopeHighlight_ValueProxy;
	if (tableName && propName &&
		strcmp(tableName, "DT_BaseEntity") == 0 && strcmp(propName, "m_ignoreParentRotation") == 0)
		return &IgnoreParentRotation_ValueProxy;
	if (tableName && propName &&
		strcmp(tableName, "DT_BaseAnimating") == 0 && strcmp(propName, "m_animRelativeToGroundEnabled") == 0)
		return &AnimRelativeToGround_ValueProxy;
	if (tableName && propName &&
		strcmp(tableName, "DT_WeaponX_LocalWeaponData") == 0 && strcmp(propName, "m_infiniteAmmoState") == 0)
		return &InfiniteAmmoState_ValueProxy;
	if (tableName && propName &&
		strcmp(tableName, "DT_WeaponX_PredictingClientOnly") == 0 &&
		strcmp(propName, "m_shotIndexForSpread") == 0)
		return &ShotIndexForSpread_ValueProxy;
	if (tableName && propName &&
		strcmp(tableName, "DT_WeaponX") == 0 && strcmp(propName, "m_scriptFloat0") == 0)
		return &ScriptFloat0_ValueProxy;
#ifndef CLIENT_DLL
	if (tableName && propName &&
		strcmp(tableName, "DT_WeaponX") == 0 && strcmp(propName, "m_akimboDisabled") == 0)
		return &AkimboDisabled_ValueProxy;
#endif // !CLIENT_DLL
	if (tableName && propName && strcmp(tableName, "DT_WeaponX") == 0)
	{
		// [HEAT-WIRE] see the proxies above -- these three were writing into
		// m_modVars, not reading slack.
		if (strcmp(propName, "m_heatValue") == 0)           return &HeatValue_ValueProxy;
		if (strcmp(propName, "m_heatValueOnLastFire") == 0) return &HeatValueOnLastFire_ValueProxy;
		if (strcmp(propName, "m_fullyHeated") == 0)         return &FullyHeated_ValueProxy;
		// [ENERGIZE-WIRE] same aliasing, same sidecar treatment.
		if (strcmp(propName, "m_energizeState") == 0)       return &EnergizeState_ValueProxy;
		if (strcmp(propName, "m_startEnergizingTime") == 0) return &StartEnergizingTime_ValueProxy;
		if (strcmp(propName, "m_energizedEndTime") == 0)    return &EnergizedEndTime_ValueProxy;
		// [LOCKEDSET-SIDECAR] / [LASER-SIDECAR] -- the last two aliased rows.
		if (strcmp(propName, "m_lockedSet") == 0)           return &LockedSet_ValueProxy;
		if (strcmp(propName, "m_targetingLaserEnabledScript") == 0)
			return &TargetingLaserEnabled_ValueProxy;
	}
	if (tableName && propName && strcmp(tableName, "DT_LocalPlayerExclusive") == 0)
	{
		for (const auto& entry : s_jetDriveWireProxies)
			if (strcmp(propName, entry.propName) == 0)
				return entry.proxy;
		for (const auto& entry : s_deathFieldWireProxies)
			if (strcmp(propName, entry.propName) == 0)
				return entry.proxy;
		for (const auto& entry : s_triggerGravityWireProxies)
			if (strcmp(propName, entry.propName) == 0)
				return entry.proxy;
		for (const auto& entry : s_updraftWireProxies)
			if (strcmp(propName, entry.propName) == 0)
				return entry.proxy;
		for (const auto& entry : s_playerLaunchWireProxies)
			if (strcmp(propName, entry.propName) == 0)
				return entry.proxy;
	}
	if (tableName && propName && strcmp(tableName, "DT_ThirdPersonView") == 0)
	{
		DTExtendProxyFn fn = TrackEntity_ValueProxyForProp(propName);
		if (fn)
			return fn;
	}
	if (tableName && propName && strcmp(tableName, "DT_Zipline") == 0)
	{
		DTExtendProxyFn fn = Zipline_ValueProxyForProp(propName);
		if (fn)
			return fn;
	}
	if (tableName && propName && strcmp(tableName, "DT_Player") == 0)
	{
		for (const auto& entry : s_playerExtendWireProxies)
			if (strcmp(propName, entry.propName) == 0)
				return entry.proxy;
	}
	if (tableName && propName && strcmp(tableName, "DT_BaseCombatCharacter") == 0)
	{
		for (const auto& entry : s_bccExtendWireProxies)
			if (strcmp(propName, entry.propName) == 0)
				return entry.proxy;
	}
	return nullptr;
}

// ===========================================================================
// SYSTEM 03: SPAWN INFRA -- RegisterNonRewindClass
// ===========================================================================

// --- Step 6: Register NonRewind ServerClass BEFORE AssignClassIds walks the list --- Called from Hook_AssignClassIds, BEFORE the original.
// At this point the factory list is unlocked, static SendTables exist, and we can insert our factory so the engine naturally assigns a classID and collects our SendTable.
static void DTExtend_RegisterNonRewindClass()
{
	if (s_nonRewindRegistered || !g_pFactoryListHead || !*g_pFactoryListHead)
		return;

	// Find the GLOBAL SNDC sub-table from the factory list
	uintptr_t globalSndcTable = 0;
	uintptr_t globalWrapper = 0;
	{
		uintptr_t node = *g_pFactoryListHead;
		for (int guard = 0; node && guard < kFactoryWalkCap; ++guard)
		{
			const char* cn = *(const char**)(node + FACT_CLASSNAME);
			void* st = *(void**)(node + 0x08);
			if (cn && st)
			{
				// The wrapper's SendTable has a DataTable prop pointing to the SNDC sub-table
				if (strcmp(cn, "CScriptNetDataGlobal") == 0)
				{
					globalWrapper = (uintptr_t)st;
					// The wrapper's first prop is a DataTable -> its m_pDataTable is the SNDC sub-table
					uint8_t* wProps = *(uint8_t**)((uintptr_t)st + ST_PROPS);
					if (wProps && *(int*)(wProps + SP_TYPE) == 10) // DataTable
						globalSndcTable = *(uintptr_t*)(wProps + 0x70);
				}
			}
			node = *(uintptr_t*)(node + FACT_NEXT);
		}
	}

	if (!globalSndcTable || !globalWrapper)
	{
		Warning(eDLL_T::ENGINE, "[dt_extend] NonRewind: GLOBAL tables not found in factory list\n");
		return;
	}

	// Clone the GLOBAL SNDC sub-table's props
	uint8_t* globalProps = *(uint8_t**)(globalSndcTable + ST_PROPS);
	int globalNProps = *(int*)(globalSndcTable + ST_NPROPS);
	if (globalNProps <= 0 || globalNProps > 11) return;

	memcpy(s_nrProps, globalProps, (uint64_t)globalNProps * SP_SIZE);

	// Patch nElements on cloned Array props
	int arrayIdx = 0;
	for (int j = 0; j < globalNProps && arrayIdx < 5; ++j)
	{
		uint8_t* prop = s_nrProps + (uint64_t)j * SP_SIZE;
		if (*(int*)(prop + SP_TYPE) == 5)
			*(int*)(prop + SP_NELEMENTS) = s_nrNElements[arrayIdx++];
	}

	// Build NonRewind SNDC sub-table
	memcpy(s_nrSendTable, (void*)globalSndcTable, NR_SENDTABLE_SIZE);
	*(uint8_t**)(s_nrSendTable + ST_PROPS) = s_nrProps;
	*(int*)(s_nrSendTable + ST_NPROPS) = globalNProps;
	*(const char**)(s_nrSendTable + ST_NETTABLENAME) = "DT_ScriptNetData_SNDC_GLOBAL_NON_REWIND";
	*(uintptr_t*)(s_nrSendTable + 0x4C0) = 0;

	// Rename the cloned self-DataTable prop[0] varName so it matches the new sub-table name -- not "DT_ScriptNetData_SNDC_GLOBAL" carried over from the clone.
	// Leave m_pSubTable (+0x70) alone: the dedi's GLOBAL prop[0] points at an external sub-table (not self), so the SendTable deep-clone walker terminates; making prop[0] self-referential creates a cycle -> EXCEPTION_STACK_OVERFLOW at first signon.
	if (globalNProps >= 1 && *(int*)(s_nrProps + SP_TYPE) == 10)
	{
		*(const char**)(s_nrProps + SP_VARNAME) = "DT_ScriptNetData_SNDC_GLOBAL_NON_REWIND";
	}

	// Clone wrapper table's DataTable prop, point to our sub-table
	uint8_t* wrapperProps = *(uint8_t**)(globalWrapper + ST_PROPS);
	memcpy(s_nrWrapperProp, wrapperProps, SP_SIZE);
	*(uint8_t**)(s_nrWrapperProp + 0x70) = s_nrSendTable;
	// Rename the cloned wrapper prop so its varName matches the new wrapper table -- not "DT_ScriptNetDataGlobal" inherited from GLOBAL.
	// Source's decoder matcher pairs DataTable props by varName before descending; if the wrapper's child prop name is wrong, the sub-table is never reached and all child flat props end up ?_unmatched on the client.
	*(const char**)(s_nrWrapperProp + SP_VARNAME) = "DT_ScriptNetDataGlobalNonRewind";

	// Build NonRewind wrapper table
	memcpy(s_nrWrapperTable, (void*)globalWrapper, NR_SENDTABLE_SIZE);
	*(uint8_t**)(s_nrWrapperTable + ST_PROPS) = s_nrWrapperProp;
	*(int*)(s_nrWrapperTable + ST_NPROPS) = 1;
	*(const char**)(s_nrWrapperTable + ST_NETTABLENAME) = "DT_ScriptNetDataGlobalNonRewind";
	*(uintptr_t*)(s_nrWrapperTable + 0x4C0) = 0;

	// Insert ONLY the WRAPPER factory at the TAIL of the list.
	// The dedi sends svc_SendTable for the sub-table (DT_ScriptNetData_SNDC_GLOBAL_NON_REWIND) automatically because it's reachable via the wrapper's DataTable prop; it does NOT need its own ServerClass.
	(void)s_nonRewindBaseFactory; // reserved; not registered

	memset(s_nonRewindFactory, 0, sizeof(s_nonRewindFactory));
	*(const char**)(s_nonRewindFactory + FACT_CLASSNAME) = "CScriptNetDataGlobalNonRewind";
	*(void**)(s_nonRewindFactory + FACT_SENDTABLE) = s_nrWrapperTable;
	*(uintptr_t*)(s_nonRewindFactory + FACT_NEXT) = 0; // tail
	*(int*)(s_nonRewindFactory + FACT_CLASSID) = 0xFFFF; // assigned by AssignClassIds
	*(int*)(s_nonRewindFactory + FACT_ALLOCSIZE) = 3384;
	*(int*)(s_nonRewindFactory + FACT_UNK20) = 0xFFFF;
	{
		uintptr_t tail = *g_pFactoryListHead;
		int guard = 0;
		while (*(uintptr_t*)(tail + FACT_NEXT) && guard++ < kFactoryWalkCap)
			tail = *(uintptr_t*)(tail + FACT_NEXT);
		*(uintptr_t*)(tail + FACT_NEXT) = (uintptr_t)s_nonRewindFactory;
	}
	s_nonRewindRegistered = true;

	Warning(eDLL_T::ENGINE,
		"[dt_extend] NonRewind: WRAPPER ServerClass registered at factory list tail\n");

	// --- Register an entity factory in the entity factory dictionary --- Without this, CreateBaselineEntity("CScriptNetDataGlobalNonRewind") returns EDICT_NULL -> no instance baseline -> client crash.
	// The factory's Create delegates to the GLOBAL factory, then overrides m_pServerClass.
	if (v_GetEntityFactory)
	{
		void** dict = (void**)v_GetEntityFactory();
		if (dict && dict[0])
		{
			// Resolve the GLOBAL entity factory from the dictionary to copy its vtable
			typedef uintptr_t (__fastcall* PFN_DictFindByName)(void**, const char*);
			PFN_DictFindByName pfnFind = *(PFN_DictFindByName*)((uintptr_t)dict[0] + 0x18);
			uintptr_t globalFactoryPtr = pfnFind(dict, "script_net_data_global");

			if (globalFactoryPtr)
			{
				s_globalFactoryRawPtr = globalFactoryPtr;
				uintptr_t globalVtable = *(uintptr_t*)globalFactoryPtr;
				memcpy(s_nrEntFactoryVtable, (void*)globalVtable, sizeof(s_nrEntFactoryVtable));
				s_nrEntFactoryVtable[0] = (uintptr_t)&NonRewind_FactoryCreate;
				// [1] = Destroy -- keep the engine's, copied above (see slot map).
				s_nrEntFactoryVtable[2] = (uintptr_t)&NonRewind_FactoryGetSize;

				s_nrEntFactoryObj = (uintptr_t)s_nrEntFactoryVtable;

				typedef __int64 (__fastcall* PFN_DictRegister)(void**, void*, const char*, const char*);
				PFN_DictRegister pfnRegister = *(PFN_DictRegister*)((uintptr_t)dict[0]);
				pfnRegister(dict, &s_nrEntFactoryObj,
					"script_net_data_global_non_rewind",
					"CScriptNetDataGlobalNonRewind");

				Warning(eDLL_T::ENGINE,
					"[dt_extend] NonRewind: entity factory registered in dictionary "
					"(vtable cloned from GLOBAL, Create+GetSize patched)\n");
			}
			else
			{
				Warning(eDLL_T::ENGINE,
					"[dt_extend] NonRewind: GLOBAL factory not found in dictionary -- "
					"entity factory NOT registered (instance baseline will fail)\n");
			}
		}
	}
}

// RegisterNonRewindClass clones GLOBAL SendProps before SNDC offset
// relocation. Copy the live GLOBAL template offsets onto the clone so
// encode reads the bytes SetGlobalNonRewindNet* writes.
static int NR_FindArrayTemplateOffset(uint8_t* props, int nProps, const char* name)
{
	for (int j = 1; j < nProps; ++j)
	{
		uint8_t* prop = props + (uint64_t)j * SP_SIZE;
		if (*(int*)(prop + SP_TYPE) != 5)
			continue;
		const char* pn = *(const char**)(prop + SP_VARNAME);
		if (!pn || strcmp(pn, name) != 0)
			continue;
		uint8_t* tmpl = props + (uint64_t)(j - 1) * SP_SIZE;
		const char* tn = *(const char**)(tmpl + SP_VARNAME);
		if (tn && strcmp(tn, name) == 0)
			return *(int*)(tmpl + SP_OFFSET);
		return -1;
	}
	return -1;
}

void DTExtend_RetargetNonRewindSendProps()
{
	if (!s_nonRewindRegistered)
		return;

	uintptr_t globalSndcTable = 0;
	if (g_pFactoryListHead && *g_pFactoryListHead)
	{
		uintptr_t node = *g_pFactoryListHead;
		for (int guard = 0; node && guard < kFactoryWalkCap; ++guard)
		{
			const char* cn = *(const char**)(node + FACT_CLASSNAME);
			void* st = *(void**)(node + 0x08);
			if (cn && st && strcmp(cn, "CScriptNetDataGlobal") == 0)
			{
				uint8_t* wProps = *(uint8_t**)((uintptr_t)st + ST_PROPS);
				if (wProps && *(int*)(wProps + SP_TYPE) == 10)
					globalSndcTable = *(uintptr_t*)(wProps + 0x70);
				break;
			}
			node = *(uintptr_t*)(node + FACT_NEXT);
		}
	}
	if (!globalSndcTable)
	{
		Warning(eDLL_T::ENGINE, "[dt_extend] NonRewind: cannot retarget, GLOBAL SNDC table missing\n");
		return;
	}

	uint8_t* gProps = *(uint8_t**)(globalSndcTable + ST_PROPS);
	const int gN = *(int*)(globalSndcTable + ST_NPROPS);
	const int nrN = *(int*)(s_nrSendTable + ST_NPROPS);
	static const char* const kNames[5] = { "m_bools", "m_ranges", "m_int32s", "m_times", "m_entities" };

	for (int i = 0; i < 5; ++i)
	{
		const int src = NR_FindArrayTemplateOffset(gProps, gN, kNames[i]);
		if (src < 0)
		{
			Warning(eDLL_T::ENGINE, "[dt_extend] NonRewind: GLOBAL %s template offset missing\n", kNames[i]);
			continue;
		}

		int old = -1;
		for (int j = 1; j < nrN; ++j)
		{
			uint8_t* prop = s_nrProps + (uint64_t)j * SP_SIZE;
			if (*(int*)(prop + SP_TYPE) != 5)
				continue;
			const char* pn = *(const char**)(prop + SP_VARNAME);
			if (!pn || strcmp(pn, kNames[i]) != 0)
				continue;
			uint8_t* tmpl = s_nrProps + (uint64_t)(j - 1) * SP_SIZE;
			const char* tn = *(const char**)(tmpl + SP_VARNAME);
			if (!tn || strcmp(tn, kNames[i]) != 0)
				break;
			old = *(int*)(tmpl + SP_OFFSET);
			*(int*)(tmpl + SP_OFFSET) = src;
			break;
		}

		Warning(eDLL_T::ENGINE,
			"[dt_extend] NonRewind.%s template offset %d -> %d (from GLOBAL)\n",
			kNames[i], old, src);
	}
}

// ===========================================================================
// SYSTEM 04: S21 CLASS / ZIPRAIL CONFIG
// ===========================================================================

//-----------------------------------------------------------------------------
// Step 7: Generic S21-only entity class registration.
//-----------------------------------------------------------------------------

// ziprail, trigger_cylinder_networked, env_decoy, trigger_slip_sphere -- real BSP/script entity names; prop_lootroller, material_harvester, prop_loot_grabber, prop_care_package_insight, vfog_volume -- script-spawned.

// S21ClassDef: dt_extend.h


// expands 30->50; everything else same as base PLAYER_EXCLUSIVE.
static const int s_peExpandedNElements[5] = { 32, 50, 6, 10, 22 };


// Entity size 3232, offsets are post-shift to fit 50 m_ranges without colliding with the int32s/times/entities arrays.
// These differ from PE base offsets (which place m_bools at 2832); the entire array block is shifted 112 bytes forward so 50 ranges fit.
const PEExpandedArrayInfo s_peExpandedLayout[5] = {
	{ "m_bools",    32, 2944 },
	{ "m_ranges",   50, 2976 },
	{ "m_int32s",    6, 3076 },
	{ "m_times",    10, 3100 },
	{ "m_entities", 22, 3140 },
};

// PE_EXPANDED is ON by default.
// Alloc is PE 3332 (S21 end 3228).
ConVar bridge_pe_expanded("bridge_pe_expanded", "1", FCVAR_RELEASE,
	"Register CScriptNetData_SNDC_PLAYER_EXCLUSIVE_EXPANDED with S21 "
	"layout (m_bools@2944, m_ranges 50@2976, m_int32s@3076, m_times@3100, "
	"m_entities@3140). Requires PE allocation patch (alloc imm 0xBC0->0xD04) "
	"which is unconditional in DTExtend_OverrideSNDCFactoryGetSize.");

// Gate the entire s_s21Classes pipeline.
// The dispatch swaps `entity+0x10` (m_pServerClass) AFTER the parent factory creates a parent-sized entity; if that violates a snapshot-manager registration invariant, the per-entity-index dirty-prop walker at +0x85 AVs on a NULL chain-head when iterating an entity that is in the PVS but missing from the dirty-prop hash.
static ConVar bridge_s21_classes("bridge_s21_classes", "1", FCVAR_RELEASE,
	"Register the S21-only entity ServerClasses (CLootRoller, CMaterialHarvester, "
	"CLootGrabber, CEnvDecoy, CTriggerSlipSphere, CTriggerCylinderNetworked, CZiprail, "
	"CVFogVolume, CCarePackageInsightProp) -- and PE_EXPANDED if bridge_pe_expanded=1.");

// The remaining S21 class gates below default ON and read their launch arg FIRST, so '+<gate> 0' turns one back off for an A/B.
// CLootRoller and CVFogVolume have no gate: they always register alongside bridge_s21_classes, and the wrapper deep-clone runs as the last structural pass ([S21-CHAIN] verifies it each boot).

// On by default. Registration runs before +convar exec, so S21ClassGateEnabled
// reads this compiled default unless +sdk_ziprail_enable 0 is on the command line.
static ConVar sdk_ziprail_enable("sdk_ziprail_enable", "1", FCVAR_RELEASE,
	"Enable S21 curved ziprails on the dedicated server (generic CZiprail class "
	"+ native rest-point injection + client-local path data -- see "
	"internal notes). 0 = parked, ziprail entities stay plain "
	"native ziplines.");

static constexpr int kMaterialHarvesterCollectedStateOffset = 0x15E0;
static constexpr int kMaterialHarvesterCollectedStateWords = 2;
// The m_collectedState block only has backing when the parent Create alloc was
// actually grown; on grow failure actualAllocSize is 0 and 0x15E0 is past the
// end of a parent-sized CDynamicProp.
bool DTExtend_MaterialHarvesterStateHasBacking(void);
// ===========================================================================
// SYSTEM 05: ZIPRAIL / MATERIAL HARVESTER [LIVE] CZipline->CZiprail promotion: side-table, proxies, DeepClone, wrapper builders.
// Ziprail remains gated by sdk_ziprail_enable; material harvester is always registered.
// ===========================================================================

// nestRoot / wireParentDT are S21 client RecvTable parent chains: DT_LootRoller(3) -> DT_PhysicsProp DT_LootGrabber(6) -> DT_DynamicProp DT_CarePackageInsightProp(3) -> DT_DynamicProp DT_EnvDecoy(1) -> DT_BaseAnimating DT_VFogVolume(21) -> DT_BaseEntity DT_TriggerCylinderNetworked(5) -> DT_BaseTrigger DT_TriggerSlipSphere(5) -> DT_BaseTrigger DT_MaterialHarvester(2) -> DT_DynamicProp DT_Ziprail(2) -> DT_Zipline env_decoy and vfog_volume are the two whose S21 chain does NOT follow the S3 entity chain: the entity stays a prop_dynamic / info_target, only the tree we transmit changes.
const S21ClassDef s_s21Classes[] = {
	// Tier 1: Simple (clone parent, few unique props -- props deferred)
	// entityName is what CreateEntity is called with from script (mp/_loot_rollers.nut).
	{ "CLootRoller",                "DT_LootRoller",                "prop_lootroller",            "prop_physics",     "CPhysicsProp",         6064, nullptr, 0, true },
	{ "CMaterialHarvester",         "DT_MaterialHarvester",         "prop_material_harvester",    "prop_dynamic",     "CDynamicProp",         6112, nullptr, 0 },
	// entityName is the string S21 scripts pass to CreateEntity and compare
	// GetNetworkedClassName against (loba_ultimate_black_market.nut,
	// sh_firing_range_vending_machine.nut, sh_survival_loot.gnut, sh_ping.gnut).
	{ "CLootGrabber",               "DT_LootGrabber",               "prop_loot_grabber",          "prop_dynamic",     "CDynamicProp",         6112, nullptr, 0, true },
	{ "CEnvDecoy",                  "DT_EnvDecoy",                  "env_decoy",                  "prop_dynamic",     "CDynamicProp",         5952, nullptr, 0, true, "CBaseAnimating" },
	// CTriggerSlipSphere: parentFactory = native "trigger_slip" (Create = real CTriggerSlip). parentDT = "CTriggerSlip" so the deep-clone SendTable is the native S3 DT_TriggerSlip tree (BaseTrigger nest + m_defaultSlipDirection @3296 + m_slipSpeed @3308 + m_slipAcceleration @3312, size 3328).
	// Root is renamed to DT_TriggerSlipSphere for the S21 client (cid 124, not stub 110).
	{ "CTriggerSlipSphere",         "DT_TriggerSlipSphere",         "trigger_slip_sphere",        "trigger_slip",      "CTriggerSlip",        3328, nullptr, 0 },
	{ "CTriggerCylinderNetworked",  "DT_TriggerCylinderNetworked",  "trigger_cylinder_networked", "trigger_cylinder",  "CBaseTrigger",        2592, nullptr, 0, true },
	// CZiprail: S21 curved ziprail.
	// Parent = native "zipline" entity / "CZipline" ServerClass.
	{ "CZiprail",                   "DT_Ziprail",                   "ziprail",                    "zipline",          "CZipline",             4848, nullptr, 0 },
	{ "CVFogVolume",                "DT_VFogVolume",                "vfog_volume",                "info_target",      "CInfoTarget",          2784, nullptr, 0, true, "CBaseEntity" },
	// entityName per perk_care_package_insight.nut's CreateEntity("prop_care_package_insight").
	{ "CCarePackageInsightProp",    "DT_CarePackageInsightProp",    "prop_care_package_insight",  "prop_dynamic",     "CDynamicProp",         6112, nullptr, 0, true },
	// NOTE: prop_ferro_prop is deliberately NOT here -- see s_nativeFactoryAliases.
	// Its client RecvTable is FLAT, which no wrapper shape this table can express will pair with.
	{ "CScriptNetData_SNDC_PLAYER_EXCLUSIVE_EXPANDED",
	  "DT_ScriptNetData_SNDC_PLAYER_EXCLUSIVE_EXPANDED",
	  "script_net_data_SNDC_PLAYER_EXCLUSIVE_EXPANDED",
	  "script_net_data_SNDC_PLAYER_EXCLUSIVE",
	  "CScriptNetData_SNDC_PLAYER_EXCLUSIVE",
	  3232, s_peExpandedNElements, 5 },
};
const int kNumS21Classes = sizeof(s_s21Classes) / sizeof(s_s21Classes[0]);

static const char* const s_triggerCylinderNetworkedAliases[] = {
	"trigger_networked_out_of_bounds",
	"trigger_networked_no_op",
	"trigger_networked_no_ops",
	"trigger_networked_block_all_op",
};

static const char* const s_materialHarvesterAliases[] = {
	"material_harvester",
};

// S21 entity names that only need to RESOLVE -- script calls CreateEntity with them, but the class carries no S21-only networked data, so the entity is registered straight onto a native S3 factory instead of getting a synthesized ServerClass.
// It then networks as that parent. prop_ferro_prop (Catalyst's spike shards) is here rather than in s_s21Classes because its client RecvTable is the one FLAT table in the S21 set: 40 props whose prop[0] is m_cellX, not a DataTable named after the table.
struct NativeFactoryAlias { const char* entityName; const char* nativeFactory; const char* className; };
static const NativeFactoryAlias s_nativeFactoryAliases[] = {
	{ "prop_ferro_prop", "prop_dynamic", "CDynamicProp" },
};

// Bump allocator for deep-cloning SendTable trees.
// Each S21 class needs a fully independent copy of the parent's entire SendTable tree (tables + props at every level) because SendTable_Init's precalc builder writes per-table metadata into SendProp fields and SendTable fields.

// Repoint SendProp.m_data.parentTable (+0x08) for every prop in `table` and recurse into DPT_DataTable children.
// Required after deep-clone (props keep the SOURCE table's parentTable) and after template-based appends.
void DTExtend_FixParentTablesInTree(uint8_t* table, int depth)
{
	if (!table || depth > 64)
		return;
	if (!bridge_legacy_fix_parenttable.GetBool())
		return;

	uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
	const int nProps = *reinterpret_cast<int*>(table + ST_NPROPS);
	if (!props || nProps <= 0 || nProps > 4096)
		return;

	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* prop = props + static_cast<uint64_t>(i) * SP_SIZE;
		*reinterpret_cast<uintptr_t*>(prop + SP_PARENTTABLE) =
			reinterpret_cast<uintptr_t>(table);

		if (*reinterpret_cast<int*>(prop + SP_TYPE) == 10) // DPT_DataTable
		{
			uint8_t* child = *reinterpret_cast<uint8_t**>(prop + 0x70);
			if (child)
				DTExtend_FixParentTablesInTree(child, depth + 1);
		}
	}
}

// Deep-clone a SendTable and all its children (recursive). Returns cloned
// table pointer, or nullptr on pool exhaustion. Clones: table struct, props
// array, and for each DPT_DataTable prop, the child table (recurse).
uint8_t* DeepCloneSendTable(const void* srcTable, ClonePool& pool)
{
	if (!srcTable) return nullptr;

	uint8_t* dst = (uint8_t*)pool.alloc(NR_SENDTABLE_SIZE);
	if (!dst) return nullptr;
	memcpy(dst, srcTable, NR_SENDTABLE_SIZE);
	*(uintptr_t*)(dst + 0x4C0) = 0; // clear precalc -- rebuilt by SendTable_Init

	const uint8_t* srcProps = *(const uint8_t**)((uintptr_t)srcTable + ST_PROPS);
	const int nProps = *(const int*)((uintptr_t)srcTable + ST_NPROPS);
	if (srcProps && nProps > 0)
	{
		uint8_t* dstProps = (uint8_t*)pool.alloc((size_t)nProps * SP_SIZE);
		if (!dstProps) return nullptr;
		memcpy(dstProps, srcProps, (size_t)nProps * SP_SIZE);
		*(uint8_t**)(dst + ST_PROPS) = dstProps;

		for (int j = 0; j < nProps; ++j)
		{
			uint8_t* prop = dstProps + (size_t)j * SP_SIZE;
			if (*(int*)(prop + SP_TYPE) == 10) // DPT_DataTable
			{
				uintptr_t childSrc = *(uintptr_t*)(prop + 0x70);
				if (childSrc)
				{
					uint8_t* childDst = DeepCloneSendTable((void*)childSrc, pool);
					if (!childDst)
					{
						// Pool exhausted mid-tree: abort the whole clone so we never
						// share a sub-table with the source (SendTable_Init would
						// write precalc into foreign nodes). Callers skip on nullptr.
						const char* srcName =
							*(const char* const*)((uintptr_t)srcTable + ST_NETTABLENAME);
						Warning(eDLL_T::ENGINE,
							"[dt_extend] DeepCloneSendTable: child clone FAILED under "
							"'%s' (pool %zu/%zu used) -- ABORTING clone, slot skipped\n",
							srcName ? srcName : "<null>", pool.used, CLONE_POOL_SIZE);
						return nullptr;
					}
					else
						*(uintptr_t*)(prop + 0x70) = (uintptr_t)childDst;
				}
			}
		}
	}

	// Own parentTable on the clone tree (children already fixed in recursion).
	// Force-enable for the clone itself even if the convar is read later --
	// call the rewrite inline when gated off by checking the convar once here.
	DTExtend_FixParentTablesInTree(dst);
	return dst;
}

const uint8_t* DTExtend_FindPropTemplateInTree(const uint8_t* table,
	int type, int depth)
{
	if (!table || depth > 32)
		return nullptr;

	const uint8_t* props = *(const uint8_t* const*)(table + ST_PROPS);
	const int nProps = *(const int*)(table + ST_NPROPS);
	if (!props || nProps <= 0 || nProps > 4096)
		return nullptr;

	for (int i = 0; i < nProps; ++i)
	{
		const uint8_t* prop = props + (uint64_t)i * SP_SIZE;
		if (*(const int*)(prop + SP_TYPE) == type &&
			!(*(const int*)(prop + SP_FLAGS) & 0x40))
			return prop;
	}

	for (int i = 0; i < nProps; ++i)
	{
		const uint8_t* prop = props + (uint64_t)i * SP_SIZE;
		if (*(const int*)(prop + SP_TYPE) != (int)SendPropType::DPT_DataTable)
			continue;
		const uint8_t* child = *(const uint8_t* const*)(prop + 0x70);
		if (const uint8_t* found = DTExtend_FindPropTemplateInTree(child, type, depth + 1))
			return found;
	}

	return nullptr;
}

static bool DTExtend_SetPropProxyInTree(uint8_t* table,
	const char* propName, DTExtendProxyFn proxyFn, int depth = 0)
{
	if (!table || !propName || !proxyFn || depth > 32)
		return false;

	uint8_t* props = *(uint8_t**)(table + ST_PROPS);
	const int nProps = *(const int*)(table + ST_NPROPS);
	if (!props || nProps <= 0 || nProps > 4096)
		return false;

	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* prop = props + (uint64_t)i * SP_SIZE;
		const char* name = *(const char**)(prop + SP_VARNAME);
		if (name && strcmp(name, propName) == 0)
		{
			*(uintptr_t*)(prop + 0x60) = (uintptr_t)proxyFn;
			return true;
		}
	}

	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* prop = props + (uint64_t)i * SP_SIZE;
		if (*(const int*)(prop + SP_TYPE) != (int)SendPropType::DPT_DataTable)
			continue;
		uint8_t* child = *(uint8_t**)(prop + 0x70);
		if (DTExtend_SetPropProxyInTree(child, propName, proxyFn, depth + 1))
			return true;
	}

	return false;
}

// Rename the first matching prop (DFS) so it stops colliding by name: retires the nested base m_nextZipline, whose subtree (props[0]) only rides the instancebaseline and would win the client's name-match with a stale 0xFFFFFFFF handle.
// After the rename, the client's single m_nextZipline name-matches our TOP-LEVEL copy (which carries the live handle in the per-entity delta).
static bool DTExtend_RenamePropInTree(uint8_t* table, const char* oldName,
	const char* newName, int depth = 0)
{
	if (!table || !oldName || !newName || depth > 32)
		return false;

	uint8_t* props = *(uint8_t**)(table + ST_PROPS);
	const int nProps = *(const int*)(table + ST_NPROPS);
	if (!props || nProps <= 0 || nProps > 4096)
		return false;

	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* prop = props + (uint64_t)i * SP_SIZE;
		const char* name = *(const char**)(prop + SP_VARNAME);
		if (name && strcmp(name, oldName) == 0)
		{
			*(const char**)(prop + SP_VARNAME) = newName;
			return true;
		}
	}

	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* prop = props + (uint64_t)i * SP_SIZE;
		if (*(const int*)(prop + SP_TYPE) != (int)SendPropType::DPT_DataTable)
			continue;
		uint8_t* child = *(uint8_t**)(prop + 0x70);
		if (DTExtend_RenamePropInTree(child, oldName, newName, depth + 1))
			return true;
	}

	return false;
}

const uint8_t* DTExtend_FindCleanTemplateProp(int type);

static void DTExtend_InitMaterialHarvesterStateElem(uint8_t* dst,
	const uint8_t* scalarTemplate, int elem)
{
	if (!dst || elem < 0 || elem >= kMaterialHarvesterCollectedStateWords)
		return;

	static const char* const kElemNames[kMaterialHarvesterCollectedStateWords] = {
		"[0000]",
		"[0001]",
	};

	if (scalarTemplate)
		memcpy(dst, scalarTemplate, SP_SIZE);
	else
		memset(dst, 0, SP_SIZE);

	*reinterpret_cast<int*>(dst + SP_TYPE) = static_cast<int>(SendPropType::DPT_Int64);
	*reinterpret_cast<int*>(dst + SP_NBITS) = 64;
	*reinterpret_cast<int*>(dst + SP_FLAGS) = 1;
	*reinterpret_cast<int*>(dst + SP_NELEMENTS) = 1;
	*reinterpret_cast<int*>(dst + SP_OFFSET) = elem * static_cast<int>(sizeof(uint64_t));
	*reinterpret_cast<int*>(dst + SP_SIZEOFVAR) = static_cast<int>(sizeof(uint64_t));
	*reinterpret_cast<const char**>(dst + SP_VARNAME) = kElemNames[elem];
	*reinterpret_cast<uint8_t*>(dst + SP_PRIORITY) = 128;
}

static uint8_t* DTExtend_BuildMaterialHarvesterStateTable(ClonePool& pool,
	const uint8_t* tableTemplate, const uint8_t* scalarTemplate)
{
	uint8_t* table = static_cast<uint8_t*>(pool.alloc(NR_SENDTABLE_SIZE));
	if (!table)
		return nullptr;
	if (tableTemplate)
		memcpy(table, tableTemplate, NR_SENDTABLE_SIZE);
	else
		memset(table, 0, NR_SENDTABLE_SIZE);

	uint8_t* props = static_cast<uint8_t*>(
		pool.alloc(kMaterialHarvesterCollectedStateWords * SP_SIZE));
	if (!props)
		return nullptr;
	memset(props, 0, kMaterialHarvesterCollectedStateWords * SP_SIZE);

	for (int i = 0; i < kMaterialHarvesterCollectedStateWords; ++i)
	{
		DTExtend_InitMaterialHarvesterStateElem(
			props + static_cast<uint64_t>(i) * SP_SIZE, scalarTemplate, i);
	}

	*reinterpret_cast<uint8_t**>(table + ST_PROPS) = props;
	*reinterpret_cast<int*>(table + ST_NPROPS) = kMaterialHarvesterCollectedStateWords;
	*reinterpret_cast<const char**>(table + ST_NETTABLENAME) = "m_collectedState";
	*reinterpret_cast<uintptr_t*>(table + 0x4C0) = 0;
	*reinterpret_cast<uintptr_t*>(table + 0x508) = 0;
	return table;
}

bool DTExtend_BuildMaterialHarvesterWrapper(uint8_t* wrapperRoot,
	uint8_t* parentWrapper, ClonePool& pool)
{
	if (!wrapperRoot || !parentWrapper)
		return false;

	memcpy(wrapperRoot, parentWrapper, NR_SENDTABLE_SIZE);

	constexpr int kTopPropCount = 2;
	uint8_t* props = static_cast<uint8_t*>(
		pool.alloc(static_cast<size_t>(kTopPropCount) * SP_SIZE));
	if (!props)
		return false;
	memset(props, 0, static_cast<size_t>(kTopPropCount) * SP_SIZE);

	const uint8_t* dataTableTemplate = DTExtend_FindPropTemplateInTree(
		parentWrapper, static_cast<int>(SendPropType::DPT_DataTable));
	const uint8_t* scalarTemplate = DTExtend_FindPropTemplateInTree(
		parentWrapper, static_cast<int>(SendPropType::DPT_Int64));
	if (!scalarTemplate)
		scalarTemplate = DTExtend_FindCleanTemplateProp(
			static_cast<int>(SendPropType::DPT_Int64));
	if (!scalarTemplate)
		return false;

	if (dataTableTemplate)
		memcpy(props, dataTableTemplate, SP_SIZE);
	else
		memset(props, 0, SP_SIZE);

	*reinterpret_cast<int*>(props + SP_TYPE) =
		static_cast<int>(SendPropType::DPT_DataTable);
	*reinterpret_cast<const char**>(props + SP_VARNAME) =
		"DT_MaterialHarvester";
	*reinterpret_cast<int*>(props + SP_NELEMENTS) = 1;
	*reinterpret_cast<int*>(props + SP_OFFSET) = 0;
	*reinterpret_cast<uintptr_t*>(props + 0x70) =
		reinterpret_cast<uintptr_t>(parentWrapper);

	uint8_t* stateProp = props + SP_SIZE;
	if (dataTableTemplate)
		memcpy(stateProp, dataTableTemplate, SP_SIZE);
	else
		memset(stateProp, 0, SP_SIZE);

	uint8_t* stateTable = DTExtend_BuildMaterialHarvesterStateTable(
		pool, parentWrapper, scalarTemplate);
	if (!stateTable)
		return false;

	*reinterpret_cast<int*>(stateProp + SP_TYPE) =
		static_cast<int>(SendPropType::DPT_DataTable);
	*reinterpret_cast<const char**>(stateProp + SP_VARNAME) = "m_collectedState";
	*reinterpret_cast<int*>(stateProp + SP_NELEMENTS) = 1;
	*reinterpret_cast<int*>(stateProp + SP_OFFSET) =
		kMaterialHarvesterCollectedStateOffset;
	*reinterpret_cast<uint8_t*>(stateProp + SP_PRIORITY) = 128;
	*reinterpret_cast<uintptr_t*>(stateProp + 0x70) =
		reinterpret_cast<uintptr_t>(stateTable);

	*reinterpret_cast<uint8_t**>(wrapperRoot + ST_PROPS) = props;
	*reinterpret_cast<int*>(wrapperRoot + ST_NPROPS) = kTopPropCount;
	*reinterpret_cast<const char**>(wrapperRoot + ST_NETTABLENAME) =
		"DT_MaterialHarvester";
	*reinterpret_cast<uintptr_t*>(wrapperRoot + 0x4C0) = 0;
	*reinterpret_cast<uintptr_t*>(wrapperRoot + 0x508) = 0;
	return true;
}

// ===========================================================================
// [ZIPRAIL-WIRE] Native-shape DT_Ziprail wire block.
// S21 C_Ziprail consumes 9 networked DT_Ziprail props (path nodes/tangents/ smooth counts/arc lengths/extents) plus S21-only m_ropeColorModulation.
// ===========================================================================

static const ZiprailWireBlock s_ziprailZeroWireBlock = {};

struct ZiprailWireLatch
{
	uint32_t         handle;
	ZiprailWireBlock block;
	bool             valid;
};
static ZiprailWireLatch s_ziprailWireLatch[64];
static int s_ziprailWireLatchCount;
static const void* s_ziprailWireWarnBase[64];
static int s_ziprailWireWarnCount;
static bool s_ziprailWireLatchFullWarned;

// SendProp* -> wire-block field mapping, filled once at wrapper build.
struct ZiprailWirePropMap
{
	const uint8_t* prop;     // SendProp inside the shim (identity key)
	int            blockOff; // byte offset into ZiprailWireBlock
	int            kind;     // 0=int32 1=float 2=vector(3 floats inline in DVariant) 3=bool(byte->dword)
};
static ZiprailWirePropMap s_ziprailWireMap[16];
int s_ziprailWireMapCount = 0;

bool ZiprailWire_AddMap(const uint8_t* prop, int blockOff, int kind)
{
	if (s_ziprailWireMapCount >= 16) // capacity of s_ziprailWireMap
		return false;
	s_ziprailWireMap[s_ziprailWireMapCount].prop = prop;
	s_ziprailWireMap[s_ziprailWireMapCount].blockOff = blockOff;
	s_ziprailWireMap[s_ziprailWireMapCount].kind = kind;
	++s_ziprailWireMapCount;
	return true;
}

static void ZiprailWire_ResetLatch(void)
{
	memset(s_ziprailWireLatch, 0, sizeof(s_ziprailWireLatch));
	s_ziprailWireLatchCount = 0;
	memset(s_ziprailWireWarnBase, 0, sizeof(s_ziprailWireWarnBase));
	s_ziprailWireWarnCount = 0;
	s_ziprailWireLatchFullWarned = false;
}

static uint32_t ZiprailWire_HandleOf(const void* structBase)
{
	if (!structBase)
		return 0;
	const SDKEntityHandle h = SDKEntityState_GetHandle(structBase);
	return h.IsValid() ? h.Raw() : 0;
}

static int ZiprailWire_FindLatch(uint32_t handle)
{
	if (!handle)
		return -1;
	for (int i = 0; i < s_ziprailWireLatchCount && i < 64; ++i)
	{
		if (s_ziprailWireLatch[i].handle == handle)
			return i;
	}
	return -1;
}

static bool ZiprailWire_ShouldWarn(const void* structBase)
{
	for (int i = 0; i < s_ziprailWireWarnCount && i < 64; ++i)
	{
		if (s_ziprailWireWarnBase[i] == structBase)
			return false;
	}
	if (s_ziprailWireWarnCount < 64)
		s_ziprailWireWarnBase[s_ziprailWireWarnCount++] = structBase;
	return true;
}

static const ZiprailWireBlock* ZiprailWire_BlockFor(const void* structBase)
{
	const ZiprailWireBlock* live =
		ZiprailDedi_GetWireBlock(reinterpret_cast<uintptr_t>(structBase));

	const uint32_t handle = ZiprailWire_HandleOf(structBase);

	if (live && live->numNodes >= 2 && live->pathLen > 0.0f)
	{
		int idx = ZiprailWire_FindLatch(handle);
		if (idx < 0 && handle)
		{
			if (s_ziprailWireLatchCount < 64)
			{
				idx = s_ziprailWireLatchCount++;
				s_ziprailWireLatch[idx].handle = handle;
				s_ziprailWireLatch[idx].valid = false;
			}
			else
			{
				if (!s_ziprailWireLatchFullWarned)
				{
					s_ziprailWireLatchFullWarned = true;
					Warning(eDLL_T::SERVER,
						"[ZIPRAIL-WIRE] latch full (capacity 64) -- cannot cache "
						"further rails; fall through without latching\n");
				}
				return live;
			}
		}
		if (idx >= 0)
		{
			s_ziprailWireLatch[idx].block = *live;
			s_ziprailWireLatch[idx].valid = true;
		}
		return live;
	}

	const int idx = ZiprailWire_FindLatch(handle);
	if (idx >= 0 && s_ziprailWireLatch[idx].valid)
	{
		if (ZiprailWire_ShouldWarn(structBase))
		{
			Warning(eDLL_T::SERVER,
				"[ZIPRAIL-WIRE] ent=0x%p live lookup miss served from latch\n",
				structBase);
		}
		return &s_ziprailWireLatch[idx].block;
	}

	if (ZiprailDedi_IsKnownChainStart(reinterpret_cast<uintptr_t>(structBase))
		&& ZiprailWire_ShouldWarn(structBase))
	{
		Warning(eDLL_T::SERVER,
			"[ZIPRAIL-WIRE] ent=0x%p publishing zero path -- rail will be "
			"invisible on the client for the rest of the map\n",
			structBase);
	}
	return &s_ziprailZeroWireBlock;
}

// Ziprail wire proxies -- install-time kind bind (no SP_TYPE / map discover on encode).
// SP_OFFSET = ABSOLUTE ZiprailWireBlock offset so flatten clones still work.
static void ZiprailWire_ZeroDv(void* pOut)
{
	*reinterpret_cast<uint64_t*>(pOut) = 0;
	*(reinterpret_cast<uint64_t*>(pOut) + 1) = 0;
	*(reinterpret_cast<uint64_t*>(pOut) + 2) = 0;
}

static const uint8_t* ZiprailWire_Src(void* pProp, void* pStruct)
{
	const uint8_t* prop = reinterpret_cast<const uint8_t*>(pProp);
	const int off = *reinterpret_cast<const int*>(prop + SP_OFFSET);
	return reinterpret_cast<const uint8_t*>(ZiprailWire_BlockFor(pStruct)) + off;
}

void __fastcall ZiprailWire_ScalarIntProxy(void* pProp, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	ZiprailWire_ZeroDv(pOut);
	*reinterpret_cast<int*>(pOut) = *reinterpret_cast<const int*>(ZiprailWire_Src(pProp, pStruct));
}

void __fastcall ZiprailWire_ScalarFloatProxy(void* pProp, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	ZiprailWire_ZeroDv(pOut);
	*reinterpret_cast<float*>(pOut) = *reinterpret_cast<const float*>(ZiprailWire_Src(pProp, pStruct));
}

void __fastcall ZiprailWire_ScalarVectorProxy(void* pProp, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	ZiprailWire_ZeroDv(pOut);
	memcpy(pOut, ZiprailWire_Src(pProp, pStruct), 12);
}

static void __fastcall ZiprailWire_ScalarBoolProxy(void* pProp, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	ZiprailWire_ZeroDv(pOut);
	*reinterpret_cast<int*>(pOut) = (*ZiprailWire_Src(pProp, pStruct) != 0) ? 1 : 0;
}

// Array elements: type fixed at install from elem donor (Int / Float / Vector).
static void __fastcall ZiprailWire_ElemIntProxy(void* pProp, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	ZiprailWire_ZeroDv(pOut);
	*reinterpret_cast<int*>(pOut) = *reinterpret_cast<const int*>(ZiprailWire_Src(pProp, pStruct));
}

static void __fastcall ZiprailWire_ElemFloatProxy(void* pProp, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	ZiprailWire_ZeroDv(pOut);
	*reinterpret_cast<float*>(pOut) = *reinterpret_cast<const float*>(ZiprailWire_Src(pProp, pStruct));
}

static void __fastcall ZiprailWire_ElemVectorProxy(void* pProp, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	ZiprailWire_ZeroDv(pOut);
	memcpy(pOut, ZiprailWire_Src(pProp, pStruct), 12);
}

static DTExtendProxyFn ZiprailWire_ScalarProxyForKind(int kind)
{
	switch (kind)
	{
	case 1:  return &ZiprailWire_ScalarFloatProxy;
	case 2:  return &ZiprailWire_ScalarVectorProxy;
	case 3:  return &ZiprailWire_ScalarBoolProxy;
	default: return &ZiprailWire_ScalarIntProxy;
	}
}

static DTExtendProxyFn ZiprailWire_ElemProxyForType(int ty)
{
	if (ty == 1) return &ZiprailWire_ElemFloatProxy;
	if (ty == 2) return &ZiprailWire_ElemVectorProxy;
	return &ZiprailWire_ElemIntProxy;
}

// 32 bracketized element names (S21 recv convention, same as the native
// rest-position child and the DT_HighlightSettings rebuild).
static const char* const kZiprailWireElemNames[kZiprailWireMaxNodes] = {
	"[0000]", "[0001]", "[0002]", "[0003]", "[0004]", "[0005]", "[0006]", "[0007]",
	"[0008]", "[0009]", "[0010]", "[0011]", "[0012]", "[0013]", "[0014]", "[0015]",
	"[0016]", "[0017]", "[0018]", "[0019]", "[0020]", "[0021]", "[0022]", "[0023]",
	"[0024]", "[0025]", "[0026]", "[0027]", "[0028]", "[0029]", "[0030]", "[0031]" };

// Find a direct prop of `table` by name (no recursion).
uint8_t* ZiprailWire_FindRootProp(uint8_t* table, const char* name, int wantType)
{
	if (!table)
		return nullptr;
	uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
	const int nProps = *reinterpret_cast<int*>(table + ST_NPROPS);
	if (!props || nProps <= 0 || nProps > 4096)
		return nullptr;
	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* p = props + static_cast<uint64_t>(i) * SP_SIZE;
		const char* nm = *reinterpret_cast<const char**>(p + SP_VARNAME);
		if (!nm || strcmp(nm, name) != 0)
			continue;
		if (wantType >= 0 && *reinterpret_cast<int*>(p + SP_TYPE) != wantType)
			continue;
		return p;
	}
	return nullptr;
}

// Build one shim leaf scalar: donor-clone + rename + kind-specific side-block proxy.
// SP_OFFSET = ABSOLUTE ZiprailWireBlock offset so the proxy still works after
// flatten clones the prop (pointer map alone is not enough for nested props).
bool ZiprailWire_BuildScalar(uint8_t* dst, const uint8_t* donor,
	const char* name, int blockOff, int kind)
{
	if (!dst || !donor)
		return false;
	memcpy(dst, donor, SP_SIZE);
	*reinterpret_cast<const char**>(dst + SP_VARNAME) = name;
	*reinterpret_cast<int*>(dst + SP_OFFSET) = blockOff;
	*reinterpret_cast<uint8_t*>(dst + SP_PRIORITY) = 128;
	if (name && strcmp(name, "m_numZiprailPathNodes") == 0)
		*reinterpret_cast<int*>(dst + SP_NBITS) = 8;
	*reinterpret_cast<uintptr_t*>(dst + 0x60) =
		reinterpret_cast<uintptr_t>(ZiprailWire_ScalarProxyForKind(kind));
	return ZiprailWire_AddMap(dst, blockOff, kind);
}

// Build one shim array-as-datatable member: donor rest-position parent shape,
// fresh 32-element child (elements donor-cloned, ZiprailWire_ElemProxy on each
// leaf with abs side-block offsets). Parent keeps 0x100 SET (inline flatten).
bool ZiprailWire_BuildArray(uint8_t* dst, const uint8_t* arrDonor,
	const uint8_t* elemDonor, const char* name, int blockOff, int elemStride,
	ClonePool& pool)
{
	if (!dst || !arrDonor || !elemDonor)
		return false;
	const uint8_t* donorChild = *reinterpret_cast<const uint8_t* const*>(arrDonor + 0x70);
	if (!donorChild)
		return false;

	uint8_t* child = static_cast<uint8_t*>(pool.alloc(NR_SENDTABLE_SIZE));
	uint8_t* childProps = static_cast<uint8_t*>(
		pool.alloc(static_cast<size_t>(kZiprailWireMaxNodes) * SP_SIZE));
	if (!child || !childProps)
		return false;

	memcpy(child, donorChild, NR_SENDTABLE_SIZE);
	const int elemTy = *reinterpret_cast<const int*>(elemDonor + SP_TYPE);
	const DTExtendProxyFn elemProxy = ZiprailWire_ElemProxyForType(elemTy);
	for (int e = 0; e < kZiprailWireMaxNodes; ++e)
	{
		uint8_t* ep = childProps + static_cast<uint64_t>(e) * SP_SIZE;
		memcpy(ep, elemDonor, SP_SIZE);
		*reinterpret_cast<const char**>(ep + SP_VARNAME) = kZiprailWireElemNames[e];
		// ABSOLUTE ZiprailWireBlock offset; elem proxy emits from side block.
		*reinterpret_cast<int*>(ep + SP_OFFSET) = blockOff + e * elemStride;
		*reinterpret_cast<int*>(ep + SP_NELEMENTS) = 1;
		*reinterpret_cast<uint8_t*>(ep + SP_PRIORITY) = 128;
		*reinterpret_cast<uintptr_t*>(ep + 0x60) = reinterpret_cast<uintptr_t>(elemProxy);
	}
	*reinterpret_cast<uint8_t**>(child + ST_PROPS) = childProps;
	*reinterpret_cast<int*>(child + ST_NPROPS) = kZiprailWireMaxNodes;
	*reinterpret_cast<const char**>(child + ST_NETTABLENAME) = name;
	*reinterpret_cast<uintptr_t*>(child + 0x4C0) = 0; // precalc -> engine rebuilds
	*reinterpret_cast<uintptr_t*>(child + 0x508) = 0;

	memcpy(dst, arrDonor, SP_SIZE);
	*reinterpret_cast<const char**>(dst + SP_VARNAME) = name;
	*reinterpret_cast<int*>(dst + SP_OFFSET) = 0;
	*reinterpret_cast<int*>(dst + SP_NELEMENTS) = 1;
	*reinterpret_cast<uint8_t*>(dst + SP_PRIORITY) = 128;
	*reinterpret_cast<uintptr_t*>(dst + 0x70) = reinterpret_cast<uintptr_t>(child);
	// KEEP 0x100 SET ("inline array"): the array flattens INLINE on both the pack side and the client's recv side (RecvPropArray3 is always inline), so the 32 element leaves take the SAME flat indices on both -> no index-space divergence (the ROUND-2 NULL-deref crash).
	// The parent keeps its native identity datatable proxy (from arrDonor); pack loop 1 gives the child base = entityBase+accumOffset, which our element proxies ignore.
	*reinterpret_cast<int*>(dst + SP_FLAGS) |= 0x100;
	return true;
}

// ===========================================================================
// SYSTEM 06: SPAWN / PROMOTION HOOKS [LIVE] serverclass-swap lifecycle (DispatchSpawn/DestroyBaseline/GlobalCreationCB/AssignClassIds), RegisterS21Classes.
// Gated bridge_s21_classes (default 1).
// ===========================================================================

// Per-class static storage (VirtualAlloc'd pool)
// S21ClassSlot: dt_extend.h
S21ClassSlot* s_s21Slots = nullptr;
static int s_s21Registered = 0;
// District alone can place 300+ trigger_slip entities; headroom for other promoted s_s21Classes on the same map (decoy/loot grabber/etc.).
// Bounded by the edict table: every promoted entity occupies an edict, so capacity below MAX_EDICTS can silently drop records mid-map and leave a destroy-time restore mismatch.
static constexpr int kS21SwapCapacity = 16384;
int s_ziprailSlotIndex = -1;
static int s_materialHarvesterSlotIndex = -1;
static int s_peExpandedSlotIndex = -1;

bool DTExtend_MaterialHarvesterStateHasBacking(void)
{
	if (!s_s21Slots || s_materialHarvesterSlotIndex < 0)
		return false;

	const int need = kMaterialHarvesterCollectedStateOffset +
		static_cast<int>(kMaterialHarvesterCollectedStateWords * sizeof(uint64_t));
	return s_s21Slots[s_materialHarvesterSlotIndex].actualAllocSize >= need;
}

// CTriggerSlipSphere: parentFactory = native "trigger_slip".
// MapEntity looks up that dict entry; we steal the native factory object's vtable so Create/GetSize land in S21Class_Factory*Dispatch with factoryPtr == parentFactoryRawPtr (InstallFactory will NOT replace an existing name, so alias-register is useless). s_triggerSlipNativeCreate is the pre-steal vtable[0] -- after steal, reading parentFactoryRawPtr->vtable[0] would recurse into our dispatch.
static int s_triggerSlipSphereSlotIndex = -1;
static uintptr_t s_triggerSlipNativeCreate = 0;
static uintptr_t s_triggerSlipNativeGetSize = 0;
static uintptr_t s_ziprailParentFactoryRawPtr = 0;
static uintptr_t s_ziprailParentCreate = 0;
static uintptr_t s_ziprailParentGetSize = 0;
static std::atomic<uint32_t> s_ziprailFactoryCreateLogs{0};
static std::atomic<uint32_t> s_ziprailNativePromoteLogs{0};
static std::atomic<uint32_t> s_ziprailPreSpawnActivateLogs{0};
static std::atomic<uint32_t> s_ziprailSpawnSwapLogs{0};

static void S21Class_ReadEntityNetDebug(uintptr_t entity, int16_t& edictIdx,
	uint32_t& handle, uint16_t& edictFlags, uintptr_t& edictEnt,
	uintptr_t& serverClass, const char*& serverClassName, int& serverClassId,
	uint16_t& modelIndex)
{
	edictIdx = -1;
	handle = 0xFFFFFFFFu;
	edictFlags = 0;
	edictEnt = 0;
	serverClass = 0;
	serverClassName = "<none>";
	serverClassId = -1;
	modelIndex = 0xFFFFu;

	if (!entity)
		return;

	// Cold NetDebug probe: ODP chain, fail-closed defaults (no SEH).
	if (DTExtend_IsSafeToRead(reinterpret_cast<const void*>(entity + 0x58), sizeof(int16_t)))
		edictIdx = *reinterpret_cast<int16_t*>(entity + 0x58);
	else
		serverClassName = "<read-fault>";
	if (DTExtend_IsSafeToRead(reinterpret_cast<const void*>(entity + 0x08), sizeof(uint32_t)))
		handle = *reinterpret_cast<uint32_t*>(entity + 0x08);
	if (DTExtend_IsSafeToRead(reinterpret_cast<const void*>(entity + 0x50), sizeof(uintptr_t)))
		serverClass = *reinterpret_cast<uintptr_t*>(entity + 0x50);
	if (DTExtend_IsSafeToRead(reinterpret_cast<const void*>(entity + 0xDE), sizeof(uint16_t)))
		modelIndex = *reinterpret_cast<uint16_t*>(entity + 0xDE);
	if (serverClass)
	{
		if (DTExtend_IsSafeToRead(reinterpret_cast<const void*>(serverClass + 0x00), sizeof(const char*)))
		{
			const char* name = *reinterpret_cast<const char**>(serverClass + 0x00);
			if (name)
				serverClassName = name;
		}
		if (DTExtend_IsSafeToRead(reinterpret_cast<const void*>(serverClass + 0x18), sizeof(int)))
			serverClassId = *reinterpret_cast<int*>(serverClass + 0x18);
	}

	if (edictIdx >= 0 && gpGlobals && gpGlobals->m_pEdicts)
	{
		uint16_t* const flagsSlot =
			reinterpret_cast<uint16_t*>(gpGlobals->m_pEdicts) + edictIdx + 32;
		if (DTExtend_IsSafeToRead(flagsSlot, sizeof(uint16_t)))
			edictFlags = *flagsSlot;
		if (DTExtend_IsSafeToRead(
				reinterpret_cast<const void*>(&gpGlobals->m_pEdicts[edictIdx + 0x7808]),
				sizeof(gpGlobals->m_pEdicts[0])))
			edictEnt = static_cast<uintptr_t>(gpGlobals->m_pEdicts[edictIdx + 0x7808]);
	}
}

// DIAG (sv_dump_seqtable / bridge_extend_audit): dt_extend_diag.cpp

static int S21Class_RetailAllocationSize(const S21ClassDef& def)
{
	if (def.dtName && strcmp(def.dtName, "DT_MaterialHarvester") == 0)
		return def.classSize;

	// Same reason as the harvester: the S21 client's DT_LootRoller addresses m_tier at 5568 and m_hasVaultKey at 5572, and its DT_PhysicsProp base reaches 5548 -- all past CPhysicsProp's 5200-byte S3 allocation.
	// Mirroring down to the parent leaves the object hundreds of bytes short of the offsets the class is declared over. actualAllocSize routes Create through our cloned factory, so this is a real 6064-byte object, not just a larger number in FACT_ALLOCSIZE.
	if (def.className && strcmp(def.className, "CLootRoller") == 0)
		return def.classSize;

	// Both address S21 fields past their S3 parent: CLootGrabber's m_impactEffectColorID..m_lootGrabDist sit at client 5600..5615 and CCarePackageInsightProp's m_lootIndex/m_contentsTaken at 5600/5604, while CDynamicProp allocates 4912.
	// One patched prop_dynamic Create covers both.
	if (def.className &&
		(strcmp(def.className, "CLootGrabber") == 0 ||
		 strcmp(def.className, "CCarePackageInsightProp") == 0))
		return def.classSize;

	// CTriggerSlip native size. parentDT=CTriggerSlip
	// already mirrors FACT_ALLOCSIZE 3328; keep explicit so a missing parent
	// never falls back to the wrong declared 2560/BaseTrigger 3296.
	if (def.className && strcmp(def.className, "CTriggerSlipSphere") == 0)
		return 3328;

	// PE_EXPANDED is also S21-only, but it is backed by the PLAYER_EXCLUSIVE factory/layout.
	// Keep its ServerClass alloc size in lockstep with the patched PE factory size so snapshot dirty-prop bookkeeping sees the same enlarged object that the bridge writes into.
	if (def.className &&
		strcmp(def.className, "CScriptNetData_SNDC_PLAYER_EXCLUSIVE_EXPANDED") == 0)
		return s_sndcSpecs[2].requiredAllocSize;

	return 0;
}

// DT_TriggerSlip SendProp offsets -- S3 CTriggerSlip entity.
static constexpr int kS3TriggerSlip_DefaultSlipDirection = 3296; // Vector 12B
static constexpr int kS3TriggerSlip_SlipSpeed            = 3308; // float
static constexpr int kS3TriggerSlip_SlipAcceleration     = 3312; // float

// After deep-clone of native DT_TriggerSlip: rename root + inheritance prop to DT_TriggerSlipSphere so the S21 client name-matches (recv root is "DT_TriggerSlipSphere" -> DT_BaseTrigger + the three slip floats).
// Offsets and codecs stay S3-native so encode still reads CTriggerSlip memory.
bool DTExtend_RetargetTriggerSlipSphereWrapper(uint8_t* wrapperRoot)
{
	if (!wrapperRoot)
		return false;

	*(const char**)(wrapperRoot + ST_NETTABLENAME) = "DT_TriggerSlipSphere";
	*(uintptr_t*)(wrapperRoot + 0x4C0) = 0;
	*(uintptr_t*)(wrapperRoot + 0x508) = 0;

	uint8_t* props = *reinterpret_cast<uint8_t**>(wrapperRoot + ST_PROPS);
	const int nProps = *reinterpret_cast<int*>(wrapperRoot + ST_NPROPS);
	if (!props || nProps < 4)
	{
		Warning(eDLL_T::ENGINE,
			"[dt_extend] CTriggerSlipSphere: expected native DT_TriggerSlip nProps>=4, got %d\n",
			nProps);
		return false;
	}

	// prop[0] = inheritance DataTable -- must be named DT_TriggerSlipSphere for
	// the client RecvTable matcher (ziprail/MH nest pattern).
	if (*reinterpret_cast<int*>(props + SP_TYPE) == 10)
		*reinterpret_cast<const char**>(props + SP_VARNAME) = "DT_TriggerSlipSphere";

	// Verify the three force fields still point at S3 CTriggerSlip memory.
	int foundDir = 0, foundSpd = 0, foundAcc = 0;
	for (int i = 0; i < nProps && i < 16; ++i)
	{
		uint8_t* p = props + static_cast<uint64_t>(i) * SP_SIZE;
		const char* nm = *reinterpret_cast<const char**>(p + SP_VARNAME);
		const int off = *reinterpret_cast<int*>(p + SP_OFFSET) & 0xFFFFF;
		const int ty  = *reinterpret_cast<int*>(p + SP_TYPE);
		if (!nm)
			continue;
		if (strcmp(nm, "m_defaultSlipDirection") == 0)
		{
			foundDir = 1;
			if (off != kS3TriggerSlip_DefaultSlipDirection || ty != 2)
				Warning(eDLL_T::ENGINE,
					"[dt_extend] CTriggerSlipSphere: m_defaultSlipDirection unexpected "
					"off=0x%X ty=%d (want 0x%X ty=2)\n",
					off, ty, kS3TriggerSlip_DefaultSlipDirection);
		}
		else if (strcmp(nm, "m_slipSpeed") == 0)
		{
			foundSpd = 1;
			if (off != kS3TriggerSlip_SlipSpeed || ty != 1)
				Warning(eDLL_T::ENGINE,
					"[dt_extend] CTriggerSlipSphere: m_slipSpeed unexpected "
					"off=0x%X ty=%d (want 0x%X ty=1)\n",
					off, ty, kS3TriggerSlip_SlipSpeed);
		}
		else if (strcmp(nm, "m_slipAcceleration") == 0)
		{
			foundAcc = 1;
			if (off != kS3TriggerSlip_SlipAcceleration || ty != 1)
				Warning(eDLL_T::ENGINE,
					"[dt_extend] CTriggerSlipSphere: m_slipAcceleration unexpected "
					"off=0x%X ty=%d (want 0x%X ty=1)\n",
					off, ty, kS3TriggerSlip_SlipAcceleration);
		}
	}

	DTExtend_FixParentTablesInTree(wrapperRoot);

	Warning(eDLL_T::ENGINE,
		"[dt_extend] CTriggerSlipSphere: wire retargeted from DT_TriggerSlip "
		"(nProps=%d dir=%d spd=%d acc=%d offs=0x%X/0x%X/0x%X) -- S3 force fields on wire\n",
		nProps, foundDir, foundSpd, foundAcc,
		kS3TriggerSlip_DefaultSlipDirection, kS3TriggerSlip_SlipSpeed,
		kS3TriggerSlip_SlipAcceleration);

	return foundDir && foundSpd && foundAcc;
}

// S21 entity lifecycle management.
// The m_pServerClass swap must be ordered carefully because the engine has THREE phases that read it 1.

struct PendingSwap {
	uintptr_t entityPtr;
	uintptr_t networkablePtr;
	uintptr_t targetClass;     // our S21 ServerClass to swap to
	uintptr_t originalClass;   // parent's ServerClass for later restoration
};
static PendingSwap s_pendingSwap[kS21SwapCapacity] = {};
static int s_pendingSwapCount = 0;

struct SwappedEntity {
	uintptr_t entityPtr;
	uintptr_t originalServerClass;
};
static SwappedEntity s_swappedEntities[kS21SwapCapacity] = {};
static int s_swappedCount = 0;

static void S21Class_RecordSwappedEntity(uintptr_t entity, uintptr_t originalClass)
{
	if (!entity || !originalClass)
		return;

	for (int j = 0; j < kS21SwapCapacity; ++j)
	{
		if (s_swappedEntities[j].entityPtr == entity)
			return;
	}

	for (int j = 0; j < kS21SwapCapacity; ++j)
	{
		if (s_swappedEntities[j].entityPtr)
			continue;
		s_swappedEntities[j].entityPtr = entity;
		s_swappedEntities[j].originalServerClass = originalClass;
		s_swappedCount++;
		return;
	}

	// Unrestorable swap: destroy would unregister under the synthetic class
	// while registration happened under the parent.
	static long long s_nRecDropWarns = 0;
	if (++s_nRecDropWarns <= 10 || (s_nRecDropWarns % 500) == 0)
		Warning(eDLL_T::ENGINE,
			"[dt_extend] swapped-entity table FULL (%d) -- entity 0x%llX will not restore "
			"its original ServerClass on destroy\n",
			kS21SwapCapacity, (unsigned long long)entity);
}

static void S21Class_QueueServerClassSwap(uintptr_t networkable,
	uintptr_t targetClass, uintptr_t originalClass)
{
	if (!networkable || !targetClass)
		return;

	uintptr_t entity = *(uintptr_t*)(networkable + 8);
	if (!entity)
		return;

	for (int j = 0; j < kS21SwapCapacity; ++j)
	{
		if (s_pendingSwap[j].entityPtr == entity &&
			s_pendingSwap[j].targetClass == targetClass)
		{
			return;
		}
	}

	for (int j = 0; j < kS21SwapCapacity; ++j)
	{
		if (s_pendingSwap[j].entityPtr)
			continue;
		s_pendingSwap[j].entityPtr = entity;
		s_pendingSwap[j].networkablePtr = networkable;
		s_pendingSwap[j].targetClass = targetClass;
		s_pendingSwap[j].originalClass = originalClass;
		s_pendingSwapCount++;
		return;
	}

	static long long s_nQueueDropWarns = 0;
	if (++s_nQueueDropWarns <= 10 || (s_nQueueDropWarns % 500) == 0)
		Warning(eDLL_T::ENGINE,
			"[dt_extend] pending-swap table FULL (%d) -- entity 0x%llX class swap dropped\n",
			kS21SwapCapacity, (unsigned long long)entity);
}

bool DTExtend_EntityIsS21Class(const void* pEntity, const char* className)
{
	if (!pEntity || !className || !s_s21Slots)
		return false;

	int slotIndex = -1;
	for (int i = 0; i < kNumS21Classes; ++i)
	{
		if (strcmp(s_s21Classes[i].className, className) == 0)
		{
			slotIndex = i;
			break;
		}
	}
	if (slotIndex < 0)
		return false;

	const uintptr_t targetClass = (uintptr_t)s_s21Slots[slotIndex].factory;
	if (!targetClass)
		return false;

	const uintptr_t entity = (uintptr_t)pEntity;
	if (DTExtend_IsSafeToRead(reinterpret_cast<const void*>(entity + 0x50), sizeof(uintptr_t)) &&
		*reinterpret_cast<const uintptr_t*>(entity + 0x50) == targetClass)
		return true;

	// Not promoted yet -- Hook_DispatchSpawn applies the swap so engine
	// registration happens under the parent class, and the entry is cleared
	// there, so a pending match only exists inside the create->spawn window.
	for (int j = 0; j < kS21SwapCapacity; ++j)
	{
		if (s_pendingSwap[j].entityPtr == entity &&
			s_pendingSwap[j].targetClass == targetClass)
			return true;
	}
	return false;
}

// Baseclass-chain membership only (DT_Player -> DT_BaseCombatCharacter
// ->...). NESTED DataTable children (DT_Local, DT_LocalPlayerExclusive,
// DT_CurrentData_*) are NOT baseclasses and never match here.
bool DTExtend_EntityHasSendTable(const void* pEntity, const char* tableName)
{
	if (!pEntity || !tableName || !*tableName)
		return false;

	const uintptr_t entity = reinterpret_cast<uintptr_t>(pEntity);
	if (!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(entity + 0x50), sizeof(uintptr_t)))
		return false;

	const uintptr_t serverClass = *reinterpret_cast<const uintptr_t*>(entity + 0x50);
	if (!serverClass || !DTExtend_IsSafeToRead(reinterpret_cast<const void*>(serverClass + 0x08), sizeof(uintptr_t)))
		return false;

	const uintptr_t sendTable = *reinterpret_cast<const uintptr_t*>(serverClass + 0x08);
	if (!sendTable || !DTExtend_IsSafeToRead(reinterpret_cast<const void*>(sendTable + ST_NETTABLENAME), sizeof(const char*)))
		return false;

	const char* const leafTable = *reinterpret_cast<const char* const*>(sendTable + ST_NETTABLENAME);
	if (!leafTable)
		return false;

	const char* ancestors[12];
	const int nAnc = DTExtend_CollectAncestorTables(leafTable, ancestors, 12);
	for (int i = 0; i < nAnc; ++i)
	{
		if (strcmp(ancestors[i], tableName) == 0)
			return true;
	}

	return false;
}

bool DTExtend_IsZiprailPromoteEnabled()
{
	return sdk_ziprail_enable.GetBool();
}

// Promote a native CZipline (a map "zipline" entity that the GUID-chain walk has reconstructed as a ziprail path) to the CZiprail ServerClass on the wire.
// The entity keeps its CZipline C++ behavior; only its network identity changes so the S21 client decodes DT_Ziprail and builds the curve.
bool DTExtend_QueueNativeZiplineAsZiprail(uintptr_t entity, const char* reason)
{
	if (!entity || !sdk_ziprail_enable.GetBool() ||
		s_ziprailSlotIndex < 0 || !s_s21Slots)
		return false;

	const uintptr_t targetClass = (uintptr_t)s_s21Slots[s_ziprailSlotIndex].factory;
	if (!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(entity + 0x50), sizeof(uintptr_t)) ||
		!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(entity + 0x58), sizeof(int16_t)))
		return false;

	const uintptr_t origClass = *reinterpret_cast<uintptr_t*>(entity + 0x50);
	const int16_t edictIdx = *reinterpret_cast<int16_t*>(entity + 0x58);

	if (!origClass)
		return false;
	if (origClass == targetClass)
		return true; // already promoted

	// Already networked: swap immediately so SV picks up CZiprail next snapshot.
	// Not yet networked: leave for Hook_DispatchSpawn (post-original) to promote.
	if (edictIdx >= 0)
	{
		if (!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(entity + 0x50), sizeof(uintptr_t), true))
			return false;
		*reinterpret_cast<uintptr_t*>(entity + 0x50) = targetClass;
		S21Class_RecordSwappedEntity(entity, origClass);
		MarkEntityEdictDirty(reinterpret_cast<void*>(entity));

		const uint32_t n = s_ziprailNativePromoteLogs.fetch_add(1, std::memory_order_relaxed);
		if (n < 32 || (n % 128) == 0)
			Warning(eDLL_T::ENGINE,
				"[dt_extend] CZiprail promote(live) #%u entity=0x%p edict=%d "
				"origClass=0x%p -> CZiprail reason='%s'\n",
				n + 1, (void*)entity, (int)edictIdx, (void*)origClass,
				reason ? reason : "<none>");
	}
	return true;
}

// DispatchSpawn hook: applies the swap AFTER engine registration so that
// per-class tracking is keyed by the PARENT class.
static __int64 __fastcall Hook_DispatchSpawn(__int64 entity)
{
	bool ziprailPendingForEntity = false;
	if (entity && s_ziprailSlotIndex >= 0 && s_s21Slots && s_pendingSwapCount > 0)
	{
		for (int i = 0; i < kS21SwapCapacity; ++i)
		{
			if (s_pendingSwap[i].entityPtr != (uintptr_t)entity)
				continue;
			if (s_pendingSwap[i].targetClass != (uintptr_t)s_s21Slots[s_ziprailSlotIndex].factory)
				continue;
			ziprailPendingForEntity = true;
			break;
		}
	}

	if (ziprailPendingForEntity && v_ActivateEntity)
	{
		int16_t edictIdxBefore = -1;
		uint32_t handleBefore = 0xFFFFFFFFu;
		uint16_t edictFlagsBefore = 0;
		uintptr_t edictEntBefore = 0;
		uintptr_t serverClassBefore = 0;
		const char* serverClassNameBefore = "<none>";
		int serverClassIdBefore = -1;
		uint16_t modelIndexBefore = 0xFFFFu;
		S21Class_ReadEntityNetDebug(static_cast<uintptr_t>(entity),
			edictIdxBefore, handleBefore, edictFlagsBefore, edictEntBefore,
			serverClassBefore, serverClassNameBefore, serverClassIdBefore,
			modelIndexBefore);

		if (handleBefore == 0xFFFFFFFFu)
		{
			const uint32_t n = s_ziprailPreSpawnActivateLogs.fetch_add(1, std::memory_order_relaxed);
			if (n < 32)
			{
				Warning(eDLL_T::ENGINE,
					"[dt_extend] CZiprail pre-DispatchSpawn ActivateEntity #%u before "
					"entity=0x%p edict=%d handle=0x%08X flags=0x%04X dirty=%d "
					"edictEnt=0x%p class='%s' classID=%d model=%u\n",
					n + 1, (void*)entity, (int)edictIdxBefore, handleBefore,
					(unsigned)edictFlagsBefore,
					(edictFlagsBefore & 0x200) ? 1 : 0,
					(void*)edictEntBefore, serverClassNameBefore,
					serverClassIdBefore, (unsigned)modelIndexBefore);
			}

			v_ActivateEntity(0xFFFFFFFF, entity);

			if (n < 32)
			{
				int16_t edictIdxAfter = -1;
				uint32_t handleAfter = 0xFFFFFFFFu;
				uint16_t edictFlagsAfter = 0;
				uintptr_t edictEntAfter = 0;
				uintptr_t serverClassAfter = 0;
				const char* serverClassNameAfter = "<none>";
				int serverClassIdAfter = -1;
				uint16_t modelIndexAfter = 0xFFFFu;
				S21Class_ReadEntityNetDebug(static_cast<uintptr_t>(entity),
					edictIdxAfter, handleAfter, edictFlagsAfter, edictEntAfter,
					serverClassAfter, serverClassNameAfter, serverClassIdAfter,
					modelIndexAfter);
				const uint16_t handleIdx = static_cast<uint16_t>(handleAfter & 0xFFFFu);
				const uint16_t handleSerial = static_cast<uint16_t>(handleAfter >> 16);
				Warning(eDLL_T::ENGINE,
					"[dt_extend] CZiprail pre-DispatchSpawn ActivateEntity #%u after "
					"entity=0x%p edict=%d handle=0x%08X(idx=%u serial=%u) "
					"flags=0x%04X dirty=%d edictEnt=0x%p class='%s' classID=%d model=%u\n",
					n + 1, (void*)entity, (int)edictIdxAfter, handleAfter,
					(unsigned)handleIdx, (unsigned)handleSerial,
					(unsigned)edictFlagsAfter,
					(edictFlagsAfter & 0x200) ? 1 : 0,
					(void*)edictEntAfter, serverClassNameAfter,
					serverClassIdAfter, (unsigned)modelIndexAfter);
			}
		}
	}

	// Run original first so engine registration happens under parent class
	__int64 result = v_DispatchSpawn(entity);
	GameTimescale_TryCaptureGnr(static_cast<uintptr_t>(entity));

	// Now apply the swap for SV_CreateBaseline's encoding phase
	if (s_pendingSwapCount > 0)
	{
		for (int i = 0; i < kS21SwapCapacity; i++)
		{
			if (s_pendingSwap[i].entityPtr != (uintptr_t)entity)
				continue;

			if (!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(entity + 0x50), sizeof(uintptr_t), true))
			{
				Warning(eDLL_T::ENGINE,
					"[dt_extend] DispatchSpawn ServerClass write skipped entity=0x%p "
					"targetClass=0x%p -- not writable (identity not promoted)\n",
					(void*)entity, (void*)s_pendingSwap[i].targetClass);
				continue;
			}
			*reinterpret_cast<uintptr_t*>(entity + 0x50) = s_pendingSwap[i].targetClass;

			// PE_EXPANDED: ServerClass alone still leaves PE-base C++ vtable
			// (writes@2832/2864/...) while SendProp encodes S21 offsets.
			// Install the forked get/set vtable so SetPlayerNet* hits S21.
			if (s_peExpandedSlotIndex >= 0 && s_s21Slots &&
				s_pendingSwap[i].targetClass == (uintptr_t)s_s21Slots[s_peExpandedSlotIndex].factory
				&& s_s21Slots[s_peExpandedSlotIndex].actualAllocSize > 0)
			{
				SNDC_ApplyPEExpandedVtable(reinterpret_cast<void*>(entity));
			}

			// Move to swapped list so DestroyBaselineEntity hook can restore.
			S21Class_RecordSwappedEntity((uintptr_t)entity, s_pendingSwap[i].originalClass);
			if (s_ziprailSlotIndex >= 0 && s_s21Slots &&
				s_pendingSwap[i].targetClass == (uintptr_t)s_s21Slots[s_ziprailSlotIndex].factory)
			{
				const uint32_t n = s_ziprailSpawnSwapLogs.fetch_add(1, std::memory_order_relaxed);
				if (n < 32)
				{
					int16_t edictIdx = -1;
					uint32_t handle = 0xFFFFFFFFu;
					uint16_t edictFlags = 0;
					uintptr_t edictEnt = 0;
					uintptr_t serverClass = 0;
					const char* serverClassName = "<none>";
					int serverClassId = -1;
					uint16_t modelIndex = 0xFFFFu;
					S21Class_ReadEntityNetDebug(static_cast<uintptr_t>(entity),
						edictIdx, handle, edictFlags, edictEnt, serverClass,
						serverClassName, serverClassId, modelIndex);
					const uint16_t handleIdx = static_cast<uint16_t>(handle & 0xFFFFu);
					const uint16_t handleSerial = static_cast<uint16_t>(handle >> 16);
					Warning(eDLL_T::ENGINE,
						"[dt_extend] CZiprail DispatchSpawn swap #%u entity=0x%p "
						"networkable=0x%p origClass=0x%p targetClass=0x%p result=%lld\n",
						n + 1, (void*)entity,
						(void*)s_pendingSwap[i].networkablePtr,
						(void*)s_pendingSwap[i].originalClass,
						(void*)s_pendingSwap[i].targetClass,
						(long long)result);
					Warning(eDLL_T::ENGINE,
						"[dt_extend] CZiprail DispatchSpawn netstate #%u edict=%d "
						"handle=0x%08X(idx=%u serial=%u) flags=0x%04X dirty=%d "
						"edictEnt=0x%p serverClass=0x%p class='%s' classID=%d model=%u\n",
						n + 1, (int)edictIdx, handle, (unsigned)handleIdx,
						(unsigned)handleSerial, (unsigned)edictFlags,
						(edictFlags & 0x200) ? 1 : 0,
						(void*)edictEnt, (void*)serverClass, serverClassName,
						serverClassId, (unsigned)modelIndex);
				}
			}
			s_pendingSwap[i].entityPtr = 0;
			s_pendingSwap[i].networkablePtr = 0;
			s_pendingSwap[i].targetClass = 0;
			s_pendingSwap[i].originalClass = 0;
			if (s_pendingSwapCount > 0)
				s_pendingSwapCount--;
			break;
		}
	}

	// S21 map ziprail promotion: maps spawn "zipline" (native factory), so a ziprail start never flows through our CZiprail factory / pending-swap.
	// Promote here by class identity once the GUID-chain walk has registered its path.
	if (entity && s_ziprailSlotIndex >= 0 && s_s21Slots &&
		sdk_ziprail_enable.GetBool() &&
		ZiprailDedi_HasPath((uintptr_t)entity))
	{
		const uintptr_t targetClass = (uintptr_t)s_s21Slots[s_ziprailSlotIndex].factory;
		uintptr_t origClass = 0;
		if (DTExtend_IsSafeToRead(reinterpret_cast<const void*>(entity + 0x50), sizeof(uintptr_t)))
			origClass = *reinterpret_cast<uintptr_t*>(entity + 0x50);
		if (origClass && origClass != targetClass)
		{
			if (!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(entity + 0x50), sizeof(uintptr_t), true))
			{
				Warning(eDLL_T::ENGINE,
					"[dt_extend] CZiprail promote(spawn) ServerClass write skipped "
					"entity=0x%p -- not writable\n",
					(void*)entity);
			}
			else
			{
				*reinterpret_cast<uintptr_t*>(entity + 0x50) = targetClass;
				S21Class_RecordSwappedEntity((uintptr_t)entity, origClass);
				MarkEntityEdictDirty(reinterpret_cast<void*>(entity));
				const uint32_t n = s_ziprailNativePromoteLogs.fetch_add(1, std::memory_order_relaxed);
				if (n < 32 || (n % 128) == 0)
					Warning(eDLL_T::ENGINE,
						"[dt_extend] CZiprail promote(spawn) #%u entity=0x%p "
						"origClass=0x%p -> CZiprail\n",
						n + 1, (void*)entity, (void*)origClass);
			}
		}
	}

	DeathField_OnEntitySpawned(reinterpret_cast<void*>(entity));

	return result;
}

// DestroyBaselineEntity: -- restores m_pServerClass before
// the engine's cleanup path runs so destructor unregisters under parent class.
typedef __int64 (__fastcall* PFN_DestroyBaselineEntity)(__int64 a1, __int16 a2);
static PFN_DestroyBaselineEntity v_DestroyBaselineEntity = nullptr;

static __int64 __fastcall Hook_DestroyBaselineEntity(__int64 a1, __int16 a2)
{
	if (s_swappedCount > 0)
	{
		for (int i = 0; i < kS21SwapCapacity; i++)
		{
			uintptr_t ent = s_swappedEntities[i].entityPtr;
			if (!ent) continue;
			if (!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(ent + 0x58), sizeof(int16_t)))
				continue;
			const int16_t entEdict = *reinterpret_cast<int16_t*>(ent + 0x58);
			if (entEdict != a2)
				continue;
			if (!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(ent + 0x50), sizeof(uintptr_t), true))
				continue;
			*reinterpret_cast<uintptr_t*>(ent + 0x50) = s_swappedEntities[i].originalServerClass;
			// Undo PE_EXPANDED C++ vtable fork if present (restore PE-base).
			if (SNDC_GetPEExpandedVtable() &&
				*reinterpret_cast<uintptr_t*>(ent) == reinterpret_cast<uintptr_t>(SNDC_GetPEExpandedVtable()))
			{
				const uintptr_t peBaseVt = g_GameDll.GetModuleBase() + 0x14F3DC0;
				*reinterpret_cast<uintptr_t*>(ent) = peBaseVt;
			}
			s_swappedEntities[i].entityPtr = 0;
			s_swappedEntities[i].originalServerClass = 0;
			if (s_swappedCount > 0)
				s_swappedCount--;
			break;
		}
	}
	return v_DestroyBaselineEntity(a1, a2);
}

// The engine calls the factory via vtable[0](factoryPtr, a2). factoryPtr is our cloned parent factory object.
// We match it against s_s21Slots to find which S21 class it represents.
static uintptr_t __fastcall S21Class_FactoryCreateDispatch(uintptr_t factoryPtr, __int64 a2)
{
	if (!s_s21Slots) return 0;
	for (int i = 0; i < kNumS21Classes; i++)
	{
		S21ClassSlot& slot = s_s21Slots[i];
		if (!slot.parentFactoryRawPtr)
			continue;
		// Normal path: Create was invoked on our cloned factory object (new
		// entityName / aliases). SlipSphere promote: MapEntity still holds the
		// NATIVE "trigger_slip" factory object; we stole its vtable so Create
		if (slot.entFactoryObjPtr != factoryPtr)
		{
			if (!(i == s_triggerSlipSphereSlotIndex &&
				factoryPtr == slot.parentFactoryRawPtr))
				continue;
		}

		typedef uintptr_t (__fastcall* PFN_RawCreate)(uintptr_t, __int64);
		uintptr_t rawCreate = 0;
		if (i == s_ziprailSlotIndex && s_ziprailParentCreate)
			rawCreate = s_ziprailParentCreate;
		else if (i == s_triggerSlipSphereSlotIndex && s_triggerSlipNativeCreate)
			rawCreate = s_triggerSlipNativeCreate; // pre-vtable-steal native Create
		else
			rawCreate = *(uintptr_t*)(*(uintptr_t*)slot.parentFactoryRawPtr);
		PFN_RawCreate pfn = (PFN_RawCreate)rawCreate;
		// Invoke native Create with the parent factory object as `this`.
		// For slip promote the parent's vtable has been stolen (our dispatch sits there) but we call the SAVED native function pointer directly, so no re-entry.
		uintptr_t createFactory = slot.actualAllocSize > 0
			? slot.entFactoryObjPtr
			: slot.parentFactoryRawPtr;
		uintptr_t networkable = pfn(createFactory, a2);
		if (!networkable) return 0;

		uintptr_t origClass = 0;
		if (DTExtend_IsSafeToRead(reinterpret_cast<const void*>(networkable + 0x10), sizeof(uintptr_t)))
			origClass = *reinterpret_cast<uintptr_t*>(networkable + 0x10);
		const bool isZiprailFactory = (i == s_ziprailSlotIndex);
		const bool isSlipSphereFactory = (i == s_triggerSlipSphereSlotIndex);
		uintptr_t entity = 0;
		if (DTExtend_IsSafeToRead(reinterpret_cast<const void*>(networkable + 8), sizeof(uintptr_t)))
			entity = *reinterpret_cast<uintptr_t*>(networkable + 8);

		bool queueSwap = true;
		bool baselineOverride = false;
		if (i == s_materialHarvesterSlotIndex && entity)
		{
			uint8_t* const state =
				reinterpret_cast<uint8_t*>(entity) +
				kMaterialHarvesterCollectedStateOffset;
			const size_t stateBytes =
				kMaterialHarvesterCollectedStateWords * sizeof(uint64_t);
			if (!DTExtend_MaterialHarvesterStateHasBacking())
			{
				static bool s_warned = false;
				if (!s_warned)
				{
					s_warned = true;
					Warning(eDLL_T::ENGINE,
						"[dt_extend] material harvester: Create alloc was not grown -- "
						"m_collectedState at %d has no backing; state init skipped\n",
						kMaterialHarvesterCollectedStateOffset);
				}
			}
			else if (DTExtend_IsSafeToRead(state, stateBytes, true))
			{
				memset(state, 0, stateBytes);
			}
		}
		// PE_EXPANDED: install S21-offset C++ vtable as soon as the PE body
		// exists so SetPlayerNet* before DispatchSpawn still writes correctly.
		if (i == s_peExpandedSlotIndex && entity && slot.actualAllocSize > 0)
			SNDC_ApplyPEExpandedVtable(reinterpret_cast<void*>(entity));
		if (isZiprailFactory)
		{
			// Path presence now lives in the dedi chain registry (ZiprailDedi_HasPath,
			// zipline_validation.cpp) -- s_ziprailPaths/DTExtendZiprailPathData
			// were retired with the SendTable-graft wrapper.
			queueSwap = entity && ZiprailDedi_HasPath(entity);
		}

		if (queueSwap)
			S21Class_QueueServerClassSwap(networkable, (uintptr_t)slot.factory, origClass);
		else if (isZiprailFactory && entity && origClass && slot.factory)
		{
			// SV_CreateBaseline still needs a temporary CZiprail identity so it emits the instancebaseline entry for classID 127.
			// Do this directly without queueing a DispatchSpawn swap; otherwise a pathless temp ziprail becomes a dirty live edict and poisons the client.
			if (DTExtend_IsSafeToRead(reinterpret_cast<const void*>(networkable + 0x10), sizeof(uintptr_t), true))
			{
				*reinterpret_cast<uintptr_t*>(networkable + 0x10) = (uintptr_t)slot.factory;
				S21Class_RecordSwappedEntity(entity, origClass);
				baselineOverride = true;
			}
		}
		if (isZiprailFactory)
		{
			const uint32_t n = s_ziprailFactoryCreateLogs.fetch_add(1, std::memory_order_relaxed);
			if (n < 32)
			{
				int16_t edictIdx = -1;
				uint32_t handle = 0xFFFFFFFFu;
				uint16_t edictFlags = 0;
				uintptr_t edictEnt = 0;
				uintptr_t serverClass = 0;
				const char* serverClassName = "<none>";
				int serverClassId = -1;
				uint16_t modelIndex = 0xFFFFu;
				S21Class_ReadEntityNetDebug(entity, edictIdx, handle, edictFlags,
					edictEnt, serverClass, serverClassName, serverClassId,
					modelIndex);
				const uint16_t handleIdx = static_cast<uint16_t>(handle & 0xFFFFu);
				const uint16_t handleSerial = static_cast<uint16_t>(handle >> 16);
				Warning(eDLL_T::ENGINE,
					"[dt_extend] CZiprail factory create #%u factory=0x%p "
					"createFactory=0x%p networkable=0x%p entity=0x%p "
					"origClass=0x%p rawCreate=0x%p promote=%d baselineOverride=%d edict=%d "
					"handle=0x%08X(idx=%u serial=%u) flags=0x%04X dirty=%d "
					"edictEnt=0x%p entityClass=0x%p class='%s' classID=%d model=%u\n",
					n + 1, (void*)factoryPtr, (void*)createFactory,
					(void*)networkable, (void*)entity, (void*)origClass,
					(void*)rawCreate, queueSwap ? 1 : 0, baselineOverride ? 1 : 0, (int)edictIdx,
					handle, (unsigned)handleIdx, (unsigned)handleSerial, (unsigned)edictFlags,
					(edictFlags & 0x200) ? 1 : 0, (void*)edictEnt,
					(void*)serverClass, serverClassName, serverClassId,
					(unsigned)modelIndex);
			}
		}
		return networkable;
	}
	return 0;
}

// IEntityFactory::GetSize must return the ACTUAL entity-allocation size, not a padded ceiling.
// The engine uses this for the destroy-time memset/memcpy that scrubs the entity before free; returning 8192 made it write up to ~6 KB past the parent-factory-sized entity (CPhysicsProp ~5800, CBaseTrigger ~2500), stomping neighbour heap chunks until later snapshot-manager hash lookups dereffed a corrupted chain-head pointer and AVed at +0x85.
static __int64 __fastcall S21Class_FactoryGetSize(uintptr_t factoryPtr)
{
	if (!s_s21Slots) return 0;
	for (int i = 0; i < kNumS21Classes; i++)
	{
		S21ClassSlot& slot = s_s21Slots[i];
		if (!slot.parentFactoryRawPtr)
			continue;
		if (slot.entFactoryObjPtr != factoryPtr)
		{
			if (!(i == s_triggerSlipSphereSlotIndex &&
				factoryPtr == slot.parentFactoryRawPtr))
				continue;
		}
		if (slot.actualAllocSize > 0)
			return slot.actualAllocSize;
		// Parent factory's vtable[2] = GetSize (saved for slip/ziprail after steal)
		typedef __int64 (__fastcall* PFN_GetSize)(uintptr_t);
		uintptr_t rawGetSize = 0;
		if (i == s_ziprailSlotIndex && s_ziprailParentGetSize)
			rawGetSize = s_ziprailParentGetSize;
		else if (i == s_triggerSlipSphereSlotIndex && s_triggerSlipNativeGetSize)
			rawGetSize = s_triggerSlipNativeGetSize;
		else
			rawGetSize = *(uintptr_t*)(*(uintptr_t*)slot.parentFactoryRawPtr + 16);
		PFN_GetSize pfn = (PFN_GetSize)rawGetSize;
		return pfn(slot.parentFactoryRawPtr);
	}
	return 0;
}

// ConVar values from +args are not applied yet when class registration runs -- AssignClassIds is very early in init, only the compiled default is readable, and registration is one-shot -- so read launch intent straight from the command line.
// Check the arg FIRST so it wins in BOTH directions; promotion itself stays gated on the live ConVar value, which is correct by the time entities spawn.
static bool S21ClassGateEnabled(const ConVar& cv, const char* launchArg)
{
	const char* val = nullptr;
	if (CommandLine()->CheckParm(launchArg, &val))
		return !val || val[0] != '0';
	return const_cast<ConVar&>(cv).GetBool();
}

static bool Ziprail_PromoteRequestedAtLaunch()
{
	return S21ClassGateEnabled(sdk_ziprail_enable, "+sdk_ziprail_enable");
}

static void DTExtend_RegisterS21Classes()
{
	if (s_s21Registered > 0 || !g_pFactoryListHead || !*g_pFactoryListHead || !v_GetEntityFactory)
		return;

	// Gate the whole pipeline.
	if (!bridge_s21_classes.GetBool())
	{
		Warning(eDLL_T::ENGINE,
			"[dt_extend] S21Classes: skipped (bridge_s21_classes=0). "
			"Set bridge_s21_classes 1 to enable.\n");
		return;
	}

	// Allocate slot pool
	s_s21Slots = (S21ClassSlot*)VirtualAlloc(
		nullptr, sizeof(S21ClassSlot) * kNumS21Classes,
		MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!s_s21Slots)
	{
		Warning(eDLL_T::ENGINE, "[dt_extend] S21Classes: VirtualAlloc failed\n");
		return;
	}

	void** dict = (void**)v_GetEntityFactory();
	if (!dict || !dict[0]) return;

	typedef uintptr_t (__fastcall* PFN_DictFindByName)(void**, const char*);
	PFN_DictFindByName pfnFind = *(PFN_DictFindByName*)((uintptr_t)dict[0] + 0x18);
	typedef __int64 (__fastcall* PFN_DictRegister)(void**, void*, const char*, const char*);
	PFN_DictRegister pfnRegister = *(PFN_DictRegister*)((uintptr_t)dict[0]);

	for (int ci = 0; ci < kNumS21Classes; ci++)
	{
		const S21ClassDef& def = s_s21Classes[ci];
		S21ClassSlot& slot = s_s21Slots[ci];

		// CLootRoller and CVFogVolume have no gate: aliasing CLootRoller to its native S3 parent is never a usable fallback -- the entity builds as a C_PhysicsProp while the client's GetTier/GetHasVaultKey are bound to C_LootRoller, so every call throws and kills ShLootRoller_Spawned before it creates the eye FX.
		if (def.className && strcmp(def.className, "CZiprail") == 0 &&
			!Ziprail_PromoteRequestedAtLaunch())
		{
			Warning(eDLL_T::ENGINE,
				"[dt_extend] S21Class 'CZiprail': SKIPPED "
				"(+sdk_ziprail_enable 0 at launch)\n");
			continue;
		}


		// Pool allocated later in Hook_SendTable_Init (post-DTExtend_Apply deep clone)
		slot.treePool.base = nullptr;
		slot.treePool.used = 0;

		// PE_EXPANDED is the one S21 class whose nElements override is larger
		// than the parent entity's backing -- gated until backing is enlarged.
		if (def.propNElementsOverride &&
			!S21ClassGateEnabled(bridge_pe_expanded, "+bridge_pe_expanded"))
		{
			Warning(eDLL_T::ENGINE,
				"[dt_extend] S21Class '%s': skipped (bridge_pe_expanded=0)\n",
				def.className);
			continue;
		}

		// Find parent ServerClass in factory list (by class name)
		uintptr_t parentSC = 0;
		{
			uintptr_t node = *g_pFactoryListHead;
			for (int guard = 0; node && guard < kFactoryWalkCap; ++guard)
			{
				const char* cn = *(const char**)(node + FACT_CLASSNAME);
				if (cn && strcmp(cn, def.parentDT) == 0) { parentSC = node; break; }
				node = *(uintptr_t*)(node + FACT_NEXT);
			}
		}
		if (!parentSC)
		{
			Warning(eDLL_T::ENGINE, "[dt_extend] S21Class '%s': parent '%s' not found, skipping\n",
				def.className, def.parentDT);
			continue;
		}

		// Placeholder: shallow-copy parent's wrapper table.
		// The REAL deep-clone happens in Hook_SendTable_Init AFTER DTExtend_Apply extends child tables, so the clone captures post-extension state.
		void* parentST = *(void**)(parentSC + 0x08);
		if (!parentST) continue;

		memcpy(slot.wrapperTable, parentST, NR_SENDTABLE_SIZE);
		*(const char**)(slot.wrapperTable + ST_NETTABLENAME) = def.dtName;
		*(uintptr_t*)(slot.wrapperTable + 0x4C0) = 0;
		*(uintptr_t*)(slot.wrapperTable + 0x508) = 0;

		// Create ServerClass node.
		// Most shimmed S21 classes are parent-sized aliases: the parent factory allocates the object, and the S21 class identity is only used for baseline/wire encoding.
		memset(slot.factory, 0, sizeof(slot.factory));
		*(const char**)(slot.factory + FACT_CLASSNAME) = def.className;
		*(void**)(slot.factory + 0x08) = slot.wrapperTable;
		*(uintptr_t*)(slot.factory + FACT_NEXT) = 0;
		*(int*)(slot.factory + FACT_CLASSID) = 0xFFFF;
		// Raised to the parent FACTORY's real Create immediate once that factory
		// is resolved below -- the DT parent and the building factory can differ.
		int parentAllocSize = *(const int*)(parentSC + FACT_ALLOCSIZE);
		int retailAllocSize = S21Class_RetailAllocationSize(def);

		// NOTE: an enlarged FACT_ALLOCSIZE grows NOTHING by itself -- the real
		// allocation is a compile-time immediate inside the parent's Create (see
		// the enlargement + fail-safe below, once the parent factory is resolved).
		const int reportedAllocSize = retailAllocSize > 0 ? retailAllocSize : parentAllocSize;
		slot.actualAllocSize = retailAllocSize;
		*(int*)(slot.factory + FACT_ALLOCSIZE) = reportedAllocSize;
		if (retailAllocSize > 0 && parentAllocSize != reportedAllocSize)
			Warning(eDLL_T::ENGINE,
				"[dt_extend] S21Class '%s': declares allocsize %d (parent '%s' is %d) -- "
				"the Create immediate is grown below\n",
				def.className, reportedAllocSize, def.parentDT, parentAllocSize);
		else if (reportedAllocSize != def.classSize)
			Warning(eDLL_T::ENGINE,
				"[dt_extend] S21Class '%s': allocsize %d -> %d (mirrored from parent '%s' to avoid destroy OOB)\n",
				def.className, def.classSize, reportedAllocSize, def.parentDT);
		*(int*)(slot.factory + FACT_UNK20) = 0xFFFF;

		// Append to factory list tail
		{
			uintptr_t tail = *g_pFactoryListHead;
			int guard = 0;
			while (*(uintptr_t*)(tail + FACT_NEXT) && guard++ < kFactoryWalkCap)
				tail = *(uintptr_t*)(tail + FACT_NEXT);
			*(uintptr_t*)(tail + FACT_NEXT) = (uintptr_t)slot.factory;
		}

		// Register entity factory in dictionary
		uintptr_t parentFactoryPtr = pfnFind(dict, def.parentFactory);
		if (parentFactoryPtr)
		{
			slot.parentFactoryRawPtr = parentFactoryPtr;

			// parentAllocSize above came from the DT parent, but the entity is built by parentFactory, and the two are not always the same class: CTriggerCylinderNetworked nests DT_BaseTrigger (3296) while 'trigger_cylinder' builds a CTriggerCylinder (3360).
			// Take the size from the factory that actually allocates -- understating it leaves replication and baseline code sizing buffers below props the class really networks, and overstating it hands out a phantom tail.
			{
				const uintptr_t pc = *(uintptr_t*)(*(uintptr_t*)parentFactoryPtr);
				const int factoryAlloc = ReadEntityCreateAllocImm(pc);
				if (factoryAlloc > parentAllocSize)
				{
					Warning(eDLL_T::ENGINE,
						"[dt_extend] S21Class '%s': '%s' allocates %d, DT parent '%s' declares %d "
						"-- reporting the factory size\n",
						def.className, def.parentFactory, factoryAlloc, def.parentDT,
						parentAllocSize);
					parentAllocSize = factoryAlloc;
					if (*(int*)(slot.factory + FACT_ALLOCSIZE) < factoryAlloc)
						*(int*)(slot.factory + FACT_ALLOCSIZE) = factoryAlloc;
				}
			}

			// Make an enlarged class actually enlarged.
			// CEntityFactory<T>::Create carries the entity's sizeof as a compile-time `mov edx, IMM` (the alloc) plus a matching `mov r8d, IMM` (the memset); it never consults the factory's virtual GetSize, which only replication/baseline code reads.
			if (retailAllocSize > parentAllocSize)
			{
				const uintptr_t parentCreate =
					*(uintptr_t*)(*(uintptr_t*)parentFactoryPtr);
				if (PatchEntityCreateAllocImm(parentCreate,
						(uint32_t)retailAllocSize, def.className) != 0)
				{
					Warning(eDLL_T::ENGINE,
						"[dt_extend] S21Class '%s': Create alloc grown %d -> %d for '%s'\n",
						def.className, parentAllocSize, retailAllocSize, def.parentFactory);
				}
				else
				{
					Warning(eDLL_T::ENGINE,
						"[dt_extend] S21Class '%s': could NOT grow '%s' Create alloc %d -> %d -- "
						"falling back to the parent size. Any prop declared past %d has no "
						"backing and would write out of bounds.\n",
						def.className, def.parentFactory, parentAllocSize, retailAllocSize,
						parentAllocSize);
					slot.actualAllocSize = 0;
					*(int*)(slot.factory + FACT_ALLOCSIZE) = parentAllocSize;
				}
			}

			uintptr_t parentVtable = *(uintptr_t*)parentFactoryPtr;
			memcpy(slot.entFactoryVtable, (void*)parentVtable, sizeof(slot.entFactoryVtable));
			slot.entFactoryVtable[0] = (uintptr_t)&S21Class_FactoryCreateDispatch;
			// [1] is Destroy(factory, entity) -- leave the parent's, copied above.
			// Pointing it at the Create dispatch made every destroy ALLOCATE a new
			// entity (a2 is the entity to free, not a create context) and never free.
			slot.entFactoryVtable[2] = (uintptr_t)&S21Class_FactoryGetSize;
			memcpy(slot.entFactoryObj, (void*)parentFactoryPtr, sizeof(slot.entFactoryObj));
			*(uintptr_t*)slot.entFactoryObj = (uintptr_t)slot.entFactoryVtable;
			slot.entFactoryObjPtr = (uintptr_t)slot.entFactoryObj;

			pfnRegister(dict, slot.entFactoryObj, def.entityName, def.className);

			if (strcmp(def.dtName, "DT_MaterialHarvester") == 0)
			{
				s_materialHarvesterSlotIndex = ci;
				for (const char* aliasName : s_materialHarvesterAliases)
				{
					pfnRegister(dict, slot.entFactoryObj, aliasName, def.className);
					Warning(eDLL_T::ENGINE,
						"[dt_extend] material harvester: entity factory alias '%s' registered\n",
						aliasName);
				}
			}

			if (strcmp(def.className, "CZiprail") == 0)
			{
				s_ziprailSlotIndex = ci;
				s_ziprailParentFactoryRawPtr = parentFactoryPtr;
				s_ziprailParentCreate = *(uintptr_t*)parentVtable;
				s_ziprailParentGetSize = *(uintptr_t*)(parentVtable + 16);
				Warning(eDLL_T::ENGINE,
					"[dt_extend] CZiprail: registered explicit '%s' factory; native '%s' factory left as CZipline "
					"(parent factory=0x%p)\n",
					def.entityName, def.parentFactory, (void*)parentFactoryPtr);
			}

			if (strcmp(def.className, "CScriptNetData_SNDC_PLAYER_EXCLUSIVE_EXPANDED") == 0)
			{
				s_peExpandedSlotIndex = ci;
				Warning(eDLL_T::ENGINE,
					"[dt_extend] PE_EXPANDED: slot=%d (C++ vtable fork applied at DispatchSpawn)\n",
					ci);
			}

			if (strcmp(def.entityName, "trigger_cylinder_networked") == 0)
			{
				for (const char* aliasName : s_triggerCylinderNetworkedAliases)
				{
					pfnRegister(dict, slot.entFactoryObj, aliasName, def.className);
					Warning(eDLL_T::ENGINE,
						"[dt_extend] S21Class '%s': entity factory alias '%s' registered\n",
						def.className, aliasName);
				}
			}

			// Promote BSP/script "trigger_slip" -> CTriggerSlipSphere.
			// InstallFactory refuses to replace an existing name, so alias-register is a no-op.
			if (strcmp(def.className, "CTriggerSlipSphere") == 0)
			{
				s_triggerSlipSphereSlotIndex = ci;
				s_triggerSlipNativeCreate = *(uintptr_t*)(parentVtable + 0);
				s_triggerSlipNativeGetSize = *(uintptr_t*)(parentVtable + 16);
				*(uintptr_t*)parentFactoryPtr = (uintptr_t)slot.entFactoryVtable;
				uintptr_t verify = pfnFind(dict, "trigger_slip");
				Warning(eDLL_T::ENGINE,
					"[dt_extend] CTriggerSlipSphere: PROMOTE trigger_slip via vtable steal "
					"nativeFactory=0x%p ourVtbl=0x%p dictFind=0x%p nativeCreate=0x%p "
					"nativeGetSize=0x%p (bound=%d)\n",
					(void*)parentFactoryPtr, (void*)slot.entFactoryVtable,
					(void*)verify, (void*)s_triggerSlipNativeCreate,
					(void*)s_triggerSlipNativeGetSize,
					(verify == parentFactoryPtr) ? 1 : 0);
			}
		}
		else
		{
			Warning(eDLL_T::ENGINE, "[dt_extend] S21Class '%s': parent factory '%s' not found in dict\n",
				def.className, def.parentFactory);
		}

		s_s21Registered++;
	}

	// Entity names that only need to resolve: point them straight at a native factory object so CreateEntity works and the entity keeps that parent's wire identity.
	// No ServerClass, no SendTable, nothing for the client to decode -- and nothing that can go wrong at decode time.
	for (const NativeFactoryAlias& alias : s_nativeFactoryAliases)
	{
		const uintptr_t nativeFactory = pfnFind(dict, alias.nativeFactory);
		if (!nativeFactory)
		{
			Warning(eDLL_T::ENGINE,
				"[dt_extend] alias '%s': native factory '%s' not found -- "
				"CreateEntity('%s') will keep failing\n",
				alias.entityName, alias.nativeFactory, alias.entityName);
			continue;
		}

		pfnRegister(dict, (void*)nativeFactory, alias.entityName, alias.className);
	}

	Warning(eDLL_T::ENGINE, "[dt_extend] S21Classes: %d/%d registered\n",
		s_s21Registered, kNumS21Classes);
}

// AssignClassIds hook -- insert SDK ServerClasses before the
// engine walks the factory list. The engine then naturally assigns classIDs,
// collects SendTables, and passes them to SendTable_Init.
static void (*v_AssignClassIds)() = nullptr;

static void Hook_AssignClassIds()
{
	DTExtend_RegisterNonRewindClass();
	DTExtend_RegisterS21Classes();
	v_AssignClassIds();
}

// Hook on the GLOBAL SNDC entity creation callback.
// Creates the permanent NonRewind entity using the GLOBAL factory + ServerClass override.
static const bool kCreateNonRewindEntity = true;

static uintptr_t s_globalEntity = 0;

// Detour on ActivateEntity to catch the engine's native NonRewind entity.
// When ActivateEntity is called with an entity whose classname is
// "global_non_rewinding", override its ServerClass and capture the pointer.
static PFN_ActivateEntity v_OrigActivateEntity = nullptr;

static void __fastcall Hook_ActivateEntity(__int64 a1, __int64 entity)
{
	v_OrigActivateEntity(a1, entity);

	// [LAUNCH-ORIGIN] capture the projectile launch position at activation (and
	// invalidate stale sidecar entries on entity-pointer reuse) -- feeds
	// LaunchOrigin_ValueProxy for DT_Projectile.m_launchOrigin.
	DTExtend_CaptureLaunchOrigin(static_cast<uintptr_t>(entity));
	GameTimescale_TryCaptureGnr(static_cast<uintptr_t>(entity));

	if (!entity || s_nonRewindEntity || !s_nonRewindFactory) return;

	// Do NOT treat the engine's native "global_non_rewinding" entity as the SNDC carrier: it is a CGlobalNonRewinding instance with CObserverMode[128] embedded at +0xB10..+0x1310, so writing SNDC arrays there stomps observer-mode vtables (AV at +0xD0 during DecideRespawnPlayer on big-map spawn).
	// Hook_GlobalCreationCB creates the carrier instead via our SDK-registered classname "script_net_data_global_non_rewind"; the native entity stays engine-owned.
	(void)entity; // suppress unused warning
}

static __int64 __fastcall Hook_GlobalCreationCB(__int64 a1)
{
	s_globalEntity = (uintptr_t)a1;
	__int64 result = v_GlobalCreationCB(a1);

	if (!kCreateNonRewindEntity || s_nonRewindEntity || !v_GetEntityFactory || !s_nonRewindRegistered)
		return result;

	// Find the GLOBAL wrapper ServerClass node (entity->GetServerClass returns this)
	uintptr_t globalServerClass = 0;
	if (g_pFactoryListHead && *g_pFactoryListHead)
	{
		uintptr_t node = *g_pFactoryListHead;
		for (int guard = 0; node && guard < kFactoryWalkCap; ++guard)
		{
			const char* cn = *(const char**)(node + FACT_CLASSNAME);
			if (cn && strcmp(cn, "CScriptNetDataGlobal") == 0) { globalServerClass = node; break; }
			node = *(uintptr_t*)(node + FACT_NEXT);
		}
	}

	void** factory = v_GetEntityFactory();
	if (!factory || !factory[0])
		return result;

	typedef __int64 (__fastcall* PFN_Create)(void**, const char*);
	PFN_Create pfnCreate = *(PFN_Create*)((uintptr_t)*factory + 8);

	// Route through OUR registered factory under the unique classname "script_net_data_global_non_rewind" (plain "script_net_data_global" is taken by an existing engine factory): NonRewind_FactoryCreate uses s_globalFactoryRawPtr (the GLOBAL SNDC RawCreate) to allocate a CScriptNetDataGlobal-sized entity (3104 B / 0xC20), then patches m_pServerClass to our wrapper.
	// Never create via the engine's native "global_non_rewinding": CGlobalNonRewinding embeds CObserverMode[128] where the SNDC arrays sit (+0xB10..+0xC20), so writing them stomps observer-mode vtables (AV at +0xD0 during DecideRespawnPlayer on big-map spawn).
	__int64 entResult = pfnCreate(factory, "script_net_data_global_non_rewind");
	if (!entResult)
	{
		Warning(eDLL_T::ENGINE, "[dt_extend] NonRewind: CreateEntityByName failed\n");
		return result;
	}

	uintptr_t entity = *(__int64*)(entResult + 8);
	if (!entity)
	{
		Warning(eDLL_T::ENGINE, "[dt_extend] NonRewind: entity ptr null\n");
		return result;
	}

	// Log pre-existing handle from factory Create (before any manual ActivateEntity)
	{
		uint32_t preHandle = *(uint32_t*)(entity + 0x08);
		int16_t preEdict = *(int16_t*)(entity + 0x58);
		Warning(eDLL_T::ENGINE,
			"[dt_extend] NonRewind: post-Create handle=0x%08X (idx=%d serial=%d) edict=%d\n",
			preHandle, (int)(preHandle & 0xFFFF), (int)(preHandle >> 16), (int)preEdict);
	}

	// Make the entity report OUR ServerClass instead of GLOBAL's, two ways (1) overwrite any entity-memory field holding &globalServerClass with &s_nonRewindFactory (covers CServerNetworkProperty::m_pServerClass); (2) if the vtable has a GetServerClass slot returning &globalServerClass (lea/mov constant), copy the vtable, patch that slot to our stub, swap.
	if (globalServerClass)
	{
		// (1) memory scan -- first 0xE00 bytes of the entity
		// Cold spawn path: one-region preflight then bare loop (no SEH-break).
		int memHits = 0;
		if (DTExtend_IsSafeToRead(reinterpret_cast<const void*>(entity), 0xE00, true))
		{
			for (uintptr_t off = 0; off + 8 <= 0xE00; off += 8)
			{
				if (*(uintptr_t*)(entity + off) == globalServerClass)
				{
					*(uintptr_t*)(entity + off) = (uintptr_t)s_nonRewindFactory;
					memHits++;
				}
			}
		}

		// (2) vtable scan -- find the slot returning &globalServerClass
		uintptr_t origVtable = *(uintptr_t*)entity;
		int slot = -1;
		for (int i = 0; i < 512; ++i)
		{
			if (!DTExtend_IsSafeToRead(
					reinterpret_cast<const void*>(origVtable + static_cast<uintptr_t>(i) * sizeof(uintptr_t)),
					sizeof(uintptr_t)))
				break;
			uintptr_t fn = ((uintptr_t*)origVtable)[i];
			if (!fn) break;
			// mov-imm path needs 11 bytes; lea path needs 8 -- cover both.
			if (!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(fn), 11))
				continue;
			const uint8_t* p = (const uint8_t*)fn;
			// lea rax, [rip+disp32]; ret = 48 8D 05 ?? ?? ?? ?? C3
			if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x05 && p[7] == 0xC3)
			{
				int32_t disp = *(const int32_t*)(p + 3);
				if ((uintptr_t)(p + 7) + disp == globalServerClass) { slot = i; break; }
			}
			// mov rax, imm64; ret = 48 B8 ?? x8 C3
			if (p[0] == 0x48 && p[1] == 0xB8 && p[10] == 0xC3)
			{
				if (*(const uintptr_t*)(p + 2) == globalServerClass) { slot = i; break; }
			}
		}
		if (slot >= 0)
		{
			// [0] = original RTTI locator (so __RTDynamicCast still works);
			// [1..512] = the 512 vfunc slots; entity->vtable = &s_nrVtableBuf[1].
			s_nrVtableBuf[0] = *(uintptr_t*)(origVtable - 8);
			memcpy(&s_nrVtableBuf[1], (void*)origVtable, 512 * sizeof(uintptr_t));
			s_nrVtableBuf[1 + slot] = (uintptr_t)&NonRewind_GetServerClass;
			*(uintptr_t*)entity = (uintptr_t)&s_nrVtableBuf[1];
		}
		Warning(eDLL_T::ENGINE,
			"[dt_extend] NonRewind: ServerClass override -- memHits=%d vtableSlot=%d\n",
			memHits, slot);
	}

	if (v_ActivateEntity)
		v_ActivateEntity(0xFFFFFFFF, entity);

	if (v_DispatchSpawn)
	{
		__int64 spawnResult = v_DispatchSpawn(entity);
		Warning(eDLL_T::ENGINE, "[dt_extend] NonRewind: DispatchSpawn result=%lld\n", (long long)spawnResult);
	}

	s_nonRewindEntity = entity;
	g_pScriptNetDataNonRewindEnt = (void*)entity;

	// Run the entity-init pass.
	// The 3rd arg indexes unk_1695091F0 (the per-category DEFAULT-VALUE table -- only 5 entries 0..4, NOT the 7-entry scriptNetCategories).
	if (v_SNDCEntityInit)
		v_SNDCEntityInit((void*)entity, 0, 0);

	int nrClassID = *(int*)(s_nonRewindFactory + FACT_CLASSID);
	int16_t edictIdx = *(int16_t*)(entity + 0x58);
	uint32_t handle = *(uint32_t*)(entity + 0x08);
	uint16_t handleIdx = (uint16_t)(handle & 0xFFFF);
	uint16_t handleSerial = (uint16_t)(handle >> 16);
	Warning(eDLL_T::ENGINE,
		"[dt_extend] NonRewind: entity=0x%p classID=%d edict=%d handle=0x%08X (idx=%d serial=%d)\n",
		(void*)entity, nrClassID, (int)edictIdx, handle, (int)handleIdx, (int)handleSerial);
	return result;
}

// Deep-clone an S21Class's SendTable tree from its parent and apply per-class SendProp overrides (nElements + per-element template offset).
// Used for PE_EXPANDED to get a wire-format that differs from PE base: 50 m_ranges at S21 offsets, independent from PE's 30-element layout.
void DTExtend_RebuildSendTableCache(void** tables, int count,
	const char* reason);
// ===========================================================================
// SYSTEM 07: CANONICAL REBUILD (CORE)
// ===========================================================================

// Complete S3 SendTable index for the active SendTable_Init pass.
// SendTable_Init is passed ONLY the top-level ServerClass tables; the shared sub-tables (DT_BaseEntity, DT_CollisionProperty,...) are reachable solely through DataTable-prop recursion.
CanonS3Tab s_canonS3Index[2048];
int        s_canonS3IndexCount = 0;

// One clean template SendProp per leaf type, captured from the live S3 tables.
// Shared with the deathfield native-DT builder (see DTExtend_BuildDeathFieldArrays).
const uint8_t* s_canonTmpl[16];

// Typed-zero SendProp value proxy for synthesized props.
// The pack loop calls SendProp+0x60 as proxy(pProp,entity,pData,pOut,iElem,objID).
static void __fastcall Canon_ZeroProxy(void* pProp, void* /*pStruct*/,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	// String encoder strlens DVariant[0] as char* -- empty string, not NULL.
	if (pProp && *reinterpret_cast<const int*>(pProp) == 4 /* DPT_String */)
		*reinterpret_cast<const char**>(pOut) = "";
}

// Sentinel leaf after typed zero. Client constructors use these non-zero defaults;
// shipping 0 on synth props overrides them and breaks game logic (handle 0, time 0).
// Confirmed via legacy-vs-canonical memory diff only -- no heuristic name match.
static void Canon_WriteU32AfterZero(void* pProp, void* pOut, uint32_t leaf)
{
	Canon_ZeroProxy(pProp, nullptr, nullptr, pOut, 0, 0);
	if (pOut)
		*reinterpret_cast<uint32_t*>(pOut) = leaf;
}

static void __fastcall Canon_InvalidEhandleProxy(void* pProp, void* /*pStruct*/,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	// INVALID_EHANDLE = -1
	Canon_WriteU32AfterZero(pProp, pOut, 0xFFFFFFFFu);
}

static void __fastcall Canon_NeverFiredTimeProxy(void* pProp, void* /*pStruct*/,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	// NEVER_FIRED = -1.0f
	Canon_WriteU32AfterZero(pProp, pOut, 0xBF800000u);
}

static void __fastcall Canon_AllowAutoMoveProxy(void* pProp, void* /*pStruct*/,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	// default-allowed game state
	Canon_WriteU32AfterZero(pProp, pOut, 1u);
}

static void __fastcall Canon_OffhandSwitchSlotInvalidProxy(void* pProp, void* /*pStruct*/,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	// eActiveInventorySlot::_invalid. GetOffhandActiveSlot then uses the
	// weapon-txt slot. A wire 0 is mainHand and blocks one-hand-on-mainhand.
	Canon_WriteU32AfterZero(pProp, pOut, 4u);
}

// Cold path only. Call when building/appending a SendProp whose name is known.
DTExtendProxyFn DTExtend_ZeroProxyForProp(const char* propName)
{
	if (propName)
	{
		if (strcmp(propName, "m_bossPlayer") == 0)
			return &Canon_InvalidEhandleProxy;
		// S21-only turret crew handles with no S3 source. A zeroed EHANDLE is
		// entity 0 (worldspawn), which reads as a real driver holding a real
		// weapon; -1 is the "nobody" the client tests for.
		if (strcmp(propName, "m_driver") == 0 ||
			strcmp(propName, "m_turretWeapon") == 0)
			return &Canon_InvalidEhandleProxy;
		if (strcmp(propName, "m_playerFloatLookStartTime") == 0)
			return &Canon_NeverFiredTimeProxy;
		if (strcmp(propName, "m_bAllowAutoMovement") == 0)
			return &Canon_AllowAutoMoveProxy;
		if (strcmp(propName, "m_offhandSwitchSlot") == 0)
			return &Canon_OffhandSwitchSlotInvalidProxy;
	}
	return &Canon_ZeroProxy;
}

// DT_Player.m_passives tail-zero value proxy.
// The S3 dedi backs only 2 Int64 passive words (@ CPlayer+0x5FF0; GivePassive caps the index at 127).
void __fastcall Passives_TailZeroProxy(void* /*pProp*/, void* /*pStruct*/,
	void* pData, void* pOut, int iElement, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	// Only words 0..1 are real S3 passives; synthetic word 2+ stays zero.
	if (iElement < 2 && pData)
		*(uint64_t*)pOut = *(uint64_t*)pData;
}

// DT_HighlightSettings.m_highlightTeamBits context-translate value proxy.
// The S3 dedi networks the highlight as a (m_highlightServerContextID @ +0x218, m_highlightTeamBits @ +0x21C) scalar pair in the DT_HighlightSettings sub-object; S21 expects a per-context Int32[8].
static void __fastcall Highlight_TeamBitsTranslateProxy(void* pProp, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	if (!pStruct || !pProp) return;
	// Array-as-datatable elements don't carry iElement.
	// The element keeps the donor's REAL Int32 m_Offset (i*4) -- NOT a 1-byte ctx index -- because 1-byte-stride offsets make adjacent Int32 children overlap, which corrupts the flattened layout and leaves the whole array ?_unmatched on the S21 client.
	const int i     = *(int*)((const char*)pProp + 0x78) >> 2; // ctx index = m_Offset/4 (Int32 stride)
	const int ctxId = *(int*)((const char*)pStruct + 0x218); // m_highlightServerContextID
	if (i == ctxId && (unsigned)ctxId < 8u)
		*(int*)pOut = *(int*)((const char*)pStruct + 0x21C);  // m_highlightTeamBits
}

static void __fastcall Highlight_GenericContextProxy(void* pProp, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	if (!pStruct || !pProp) return;
	const int i = *(int*)((const char*)pProp + 0x78) >> 2;
	if ((unsigned)i < 4u)
		*(int*)pOut = WeaponScriptVars_WireGetGenericHighlightContext(pStruct, i);
	else
		*(int*)pOut = 0xFF;
}

static void __fastcall Highlight_FocusedProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	if (!pStruct) return;
	*(int*)pOut = WeaponScriptVars_WireGetHighlightFocused(pStruct);
}

// [FILTER-DELETED] Do not reintroduce an address-equality drop filter on Canon_ZeroProxy.
// One-sided compaction of the changed-prop index array desyncs the engine's parallel data-slot array -> transmit AV.

static const S21SchemaTable* CanonSchemaFind(const char* name)
{
	if (!name) return nullptr;
	for (int i = 0; i < g_s21SchemaCount; ++i)
		if (g_s21Schema[i].table && _stricmp(g_s21Schema[i].table, name) == 0)
			return &g_s21Schema[i];
	return nullptr;
}

// Recursively index every S3 SendTable reachable from a root, following each
// type-10 (DataTable) prop's m_pDataTable child. Dedups by pointer.
void CanonDiscoverTable(uintptr_t table, int depth)
{
	if (!table || depth > 64) return;
	for (int i = 0; i < s_canonS3IndexCount; ++i)
		if (s_canonS3Index[i].table == table) return;   // already visited
	if (s_canonS3IndexCount >= 2048) return;
	s_canonS3Index[s_canonS3IndexCount].name  = *(const char**)(table + ST_NETTABLENAME);
	s_canonS3Index[s_canonS3IndexCount].table = table;
	++s_canonS3IndexCount;

	uint8_t*  props  = *(uint8_t**)(table + ST_PROPS);
	const int nProps = *(int*)(table + ST_NPROPS);
	for (int j = 0; j < nProps && props; ++j)
	{
		const uint8_t* p = props + (uint64_t)j * SP_SIZE;
		if (*(const int*)(p + SP_TYPE) == 10)
		{
			uintptr_t child = *(uintptr_t*)(p + 0x70 /* m_pDataTable */);
			if (child) CanonDiscoverTable(child, depth + 1);
		}
	}
}

uintptr_t CanonS3TableFind(const char* name)
{
	if (!name) return 0;
	for (int i = 0; i < s_canonS3IndexCount; ++i)
		if (s_canonS3Index[i].name && _stricmp(s_canonS3Index[i].name, name) == 0)
			return s_canonS3Index[i].table;
	return 0;
}
// ===========================================================================
// SYSTEM 08: DIAG -- SendTable dump helpers [DIAG]
// DumpSendTree/DumpCPlayerFlat/DumpOffhandSubTable + ODP_* readers.
// ===========================================================================

// Recursive SEND-TREE dump: walks a SendTable's ST_PROPS depth-first, printing each prop indented by depth (type/nElem/flags/offset/name); for DPT_DataTable (ty=10) props it prints the child table name + child count and recurses.
// Unlike the flat dump -- which strips the hierarchy and resets each array-as-datatable child offset to sub-table-relative -- this names every array PARENT, so an unmatched bracketized child like '[0007]' can be traced back to its parent SendProp for reconciliation.
static void DTExtend_DumpSendTree(uint8_t* table, int depth)
{
	if (!table || depth > 24)
		return;
	// Cold diag: ODP table/props/child before deref (no outer SEH).
	if (!DTExtend_IsSafeToRead(table + ST_PROPS, sizeof(void*)) ||
		!DTExtend_IsSafeToRead(table + ST_NPROPS, sizeof(int)))
	{
		Warning(eDLL_T::ENGINE, "[DT-TREE] walk unreadable at depth %d\n", depth);
		return;
	}
	uint8_t*  props  = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
	const int nProps = *reinterpret_cast<int*>(table + ST_NPROPS);
	if (!props || nProps <= 0 || nProps > 4096)
		return;
	if (!DTExtend_IsSafeToRead(props, static_cast<size_t>(nProps) * SP_SIZE))
	{
		Warning(eDLL_T::ENGINE, "[DT-TREE] props unreadable at depth %d\n", depth);
		return;
	}

	char indent[64];
	int  ind = depth * 2;
	if (ind > 60) ind = 60;
	for (int k = 0; k < ind; ++k) indent[k] = ' ';
	indent[ind] = '\0';

	for (int i = 0; i < nProps; ++i)
	{
		uint8_t*    p  = props + static_cast<uint64_t>(i) * SP_SIZE;
		const int   ty = *reinterpret_cast<int*>(p + SP_TYPE);
		const int   ne = *reinterpret_cast<int*>(p + SP_NELEMENTS);
		const int   fl = *reinterpret_cast<int*>(p + SP_FLAGS);
		const int   of = *reinterpret_cast<int*>(p + SP_OFFSET);
		const char* nm = *reinterpret_cast<const char**>(p + SP_VARNAME);

		if (ty == 10) // DPT_DataTable -> recurse
		{
			uint8_t* child = nullptr;
			if (DTExtend_IsSafeToRead(p + 0x70, sizeof(void*)))
				child = *reinterpret_cast<uint8_t**>(p + 0x70);
			const char* cn = nullptr;
			int ccount = -1;
			if (child &&
				DTExtend_IsSafeToRead(child + ST_NETTABLENAME, sizeof(void*)) &&
				DTExtend_IsSafeToRead(child + ST_NPROPS, sizeof(int)))
			{
				cn = *reinterpret_cast<const char**>(child + ST_NETTABLENAME);
				ccount = *reinterpret_cast<int*>(child + ST_NPROPS);
			}
			Warning(eDLL_T::ENGINE,
				"[DT-TREE]%s[%2d] DT fl=0x%X off=0x%X name='%s' -> '%s' (%d kids)\n",
				indent, i, fl, of, nm ? nm : "?", cn ? cn : "<NULL>", ccount);
			if (child)
				DTExtend_DumpSendTree(child, depth + 1);
		}
		else
		{
			Warning(eDLL_T::ENGINE,
				"[DT-TREE]%s[%2d] ty=%d nElem=%d fl=0x%X off=0x%X name='%s'\n",
				indent, i, ty, ne, fl, of, nm ? nm : "?");
		}
	}
}

// Flat SendTable dump ([DT-DUMP]) for CPlayer and key peer classes.
// CSendTablePrecalc lives at SendTable+0x4C0; the flattened SendProp* array the snapshot encoder walks is precalc+0x08, count precalc+0x10.
static void DTExtend_DumpCPlayerFlat(void** tables, int count)
{
	if (!tables || count <= 0)
		return;

	Warning(eDLL_T::ENGINE, "[DT-DUMP] === dedi flat SendTable dump: %d tables ===\n", count);

	// Per-table summary (name + flat prop count) for every table.
	for (int t = 0; t < count; ++t)
	{
		uint8_t* st = reinterpret_cast<uint8_t*>(tables[t]);
		if (!st)
			continue;
		if (!DTExtend_IsSafeToRead(st + ST_NETTABLENAME, sizeof(void*)) ||
			!DTExtend_IsSafeToRead(st + 0x4C0, sizeof(void*)))
		{
			Warning(eDLL_T::ENGINE, "[DT-DUMP] table summary unreadable at idx=%d\n", t);
			continue;
		}
		const char* name = *reinterpret_cast<const char**>(st + ST_NETTABLENAME);
		uint8_t*    pc   = *reinterpret_cast<uint8_t**>(st + 0x4C0);  // CSendTablePrecalc
		int nFlat = -1;
		if (pc && DTExtend_IsSafeToRead(pc + 0x10, sizeof(int)))
			nFlat = *reinterpret_cast<int*>(pc + 0x10);
		Warning(eDLL_T::ENGINE, "[DT-DUMP] table='%s' flatProps=%d\n",
			name ? name : "?", nFlat);
	}

	// Full flat-prop dump for the entities decoded before the snapshot desync
	// (CWorld, CPlayer, CScriptNetDataGlobal, CBaseViewModel,
	// CScriptNetDataGlobalNonRewind) -- diff each vs the client [DEC-FLAT].
	static const char* const kTargets[] = {
		"DT_Player", "DT_WORLD", "DT_ScriptNetDataGlobal",
		"DT_ScriptNetDataGlobalNonRewind", "DT_BaseViewModel",
		"DT_PlayerDecoy", "DT_PlayerVehicle", "DT_Zipline", "DT_Ziprail",
		// canyonlands_hu enter-PVS instance-baseline desync: diff the dedi's
		// flattened CDynamicProp send layout against the S21 client [DEC-FLAT]
		// class 21 to find the prop where the baseline blob misaligns.
		"DT_DynamicProp", "DT_DynamicPropLightweight",
		// Baseline-decode OOB (entCrumbs=0): the client reports precalc.nProps == recvProps.size (99 / 200) yet the wire carries indices past both, so the count the ENCODER walks here is the missing side of the diff.
		// Pull the native PARENTS too -- these classes are promoted from them by a m_pServerClass swap, so a baseline written before the swap lands would be parent-shaped while the client decodes it with the cloned table.
		"DT_TriggerSlipSphere", "DT_TriggerSlip",
		"DT_LootRoller", "DT_PhysicsProp",
	};
	for (const char* target : kTargets)
	{
		uint8_t* pc    = nullptr;
		int      nFlat = 0;
		for (int t = 0; t < count; ++t)
		{
			uint8_t* st = reinterpret_cast<uint8_t*>(tables[t]);
			if (!st)
				continue;
			if (!DTExtend_IsSafeToRead(st + ST_NETTABLENAME, sizeof(void*)))
				continue;
			const char* name = *reinterpret_cast<const char**>(st + ST_NETTABLENAME);
			if (!name || !DTExtend_IsSafeToRead(name, 1) || strcmp(name, target) != 0)
				continue;
			if (!DTExtend_IsSafeToRead(st + 0x4C0, sizeof(void*)))
				continue;
			pc = *reinterpret_cast<uint8_t**>(st + 0x4C0);
			nFlat = -1;
			if (pc && DTExtend_IsSafeToRead(pc + 0x10, sizeof(int)))
				nFlat = *reinterpret_cast<int*>(pc + 0x10);
			break;
		}
		if (!pc || nFlat <= 0)
		{
			Warning(eDLL_T::ENGINE, "[DT-DUMP] target '%s' unresolved\n", target);
			continue;
		}
		Warning(eDLL_T::ENGINE, "[DT-DUMP] === full flat list: '%s' (%d props) ===\n",
			target, nFlat);
		if (!DTExtend_IsSafeToRead(pc + 0x08, sizeof(void*)))
		{
			Warning(eDLL_T::ENGINE, "[DT-DUMP] '%s' flat ptr unreadable\n", target);
			continue;
		}
		uint8_t** flat = *reinterpret_cast<uint8_t***>(pc + 0x08);
		if (!flat ||
			!DTExtend_IsSafeToRead(flat, static_cast<size_t>(nFlat) * sizeof(uint8_t*)))
		{
			Warning(eDLL_T::ENGINE, "[DT-DUMP] '%s' flat array unreadable\n", target);
			continue;
		}
		for (int i = 0; i < nFlat && i < 4096; ++i)
		{
			uint8_t* sp = flat[i];
			if (!sp)
			{
				Warning(eDLL_T::ENGINE, "[DT-DUMP]   %s [%4d] <NULL>\n", target, i);
				continue;
			}
			if (!DTExtend_IsSafeToRead(sp, SP_SIZE))
			{
				Warning(eDLL_T::ENGINE, "[DT-DUMP]   %s [%4d] <unreadable>\n", target, i);
				continue;
			}
			const int   ty = *reinterpret_cast<int*>(sp + SP_TYPE);
			const int   nb = *reinterpret_cast<int*>(sp + SP_NBITS);
			const int   fl = *reinterpret_cast<int*>(sp + SP_FLAGS);
			const int   ne = *reinterpret_cast<int*>(sp + SP_NELEMENTS);
			const int   of = *reinterpret_cast<int*>(sp + SP_OFFSET);
			const char* nm = *reinterpret_cast<const char**>(sp + SP_VARNAME);
			Warning(eDLL_T::ENGINE,
				"[DT-DUMP]   %s [%4d] off=0x%04X ty=%d nBits=%d fl=0x%X nElem=%d name='%s'\n",
				target, i, of, ty, nb, fl, ne, nm ? nm : "?");

			// ty=5 (DPT_Array): follow SP_ARRAYPROP (+0x18) and dump the element template.
			// Its ty/nBits/flags drive the per-element decode in the S3 delta decoder (element loop).
			if (ty == 5)
			{
				uint8_t*    ep  = *reinterpret_cast<uint8_t**>(sp + SP_ARRAYPROP);
				int         ety = -1, enb = -1, efl = 0, ene = -1;
				const char* enm = nullptr;
				if (ep && DTExtend_IsSafeToRead(ep, SP_SIZE))
				{
					ety = *reinterpret_cast<int*>(ep + SP_TYPE);
					enb = *reinterpret_cast<int*>(ep + SP_NBITS);
					efl = *reinterpret_cast<int*>(ep + SP_FLAGS);
					ene = *reinterpret_cast<int*>(ep + SP_NELEMENTS);
					enm = *reinterpret_cast<const char**>(ep + SP_VARNAME);
				}
				Warning(eDLL_T::ENGINE,
					"[DT-DUMP]   %s [%4d] elem ty=%d nBits=%d fl=0x%X nElem=%d name='%s'\n",
					target, i, ety, enb, efl, ene, ep ? (enm ? enm : "?") : "<NULL>");
			}
		}
	}

	// SEND-TREE dump for DT_DynamicProp: names every array-as-datatable parent (the flat dump above cannot), so the canyonlands_hu CDynamicProp '?_unmatched' arrays can be mapped to the SendProp to reconcile.
	// DumpSendTree is ODP-safe; no outer SEH needed.
	for (int t = 0; t < count; ++t)
	{
		uint8_t* st = reinterpret_cast<uint8_t*>(tables[t]);
		if (!st)
			continue;
		if (!DTExtend_IsSafeToRead(st + ST_NETTABLENAME, sizeof(void*)))
			continue;
		const char* name = *reinterpret_cast<const char**>(st + ST_NETTABLENAME);
		if (!name || !DTExtend_IsSafeToRead(name, 1) ||
			(strcmp(name, "DT_DynamicProp") != 0 && strcmp(name, "DT_PlayerDecoy") != 0
			&& strcmp(name, "DT_Player") != 0))
			continue;
		Warning(eDLL_T::ENGINE, "[DT-TREE] === send tree: '%s' ===\n", name);
		DTExtend_DumpSendTree(st, 0);
		Warning(eDLL_T::ENGINE, "[DT-TREE] === send tree done ===\n");
		// no break: dump all matching tables (DT_DynamicProp + DT_PlayerDecoy + DT_Player).
		// DT_Player keeps the REAL DT_BaseAnimatingOverlay sub-table (the decoy's is a separate synthetic alloc), so its m_animOverlay* props show the verified S3 send offsets to lift into the decoy/vehicle synthetic wrapper.
	}

	Warning(eDLL_T::ENGINE, "[DT-DUMP] === done ===\n");
}

// [OFFHAND-SUB-DUMP] -- one-shot diag that finds the SendTable containing `offhandWeapons` as a DPT_DataTable (=10) prop, walks to its sub-table, and dumps each sub-prop's metadata plus raw bytes, to establish whether the sub-table stores contiguous SP_SIZE (0x88) elements or qword pointers.
// Searches by name (expected parent DT_BaseCombatCharacter) to survive a schema rename.
bool ODP_IsReadable(const void* p, size_t n)
{
	if (n == 0) return false;
	return Mem_IsReadable(p, n);
}

// Bounded string-readability check: one region probe for [p, p+maxLen), then scan for NUL.
// Never VirtualQuery per character (encode-path landmine if ever called from a hot walk).
int ODP_StrLenSafe(const char* p, int maxLen)
{
	if (!p || maxLen <= 0) return -1;
	if (!ODP_IsReadable(p, static_cast<size_t>(maxLen)))
	{
		// Tail of a string may sit near a region end: probe progressively smaller.
		int n = maxLen;
		while (n > 0 && !ODP_IsReadable(p, static_cast<size_t>(n)))
			n >>= 1;
		if (n <= 0) return -1;
		maxLen = n;
	}
	for (int i = 0; i < maxLen; ++i)
	{
		if (p[i] == 0) return i;
	}
	return -1;
}

// Recursive find -- walks a SendTable tree looking for a DPT_DataTable prop named `targetName`.
// Dedups visited tables via a fixed-size pointer set.

void DTExtend_RecursiveOffhandFind(uint8_t* table, OffhandFindCtx& ctx, int depth)
{
	if (!table || depth > 32 || ctx.matchedProp) return;
	if (!ODP_IsReadable(table, 0x4C0 + 8)) return; // need ST_PROPS+ST_NPROPS+ST_NETTABLENAME

	const uintptr_t pt = reinterpret_cast<uintptr_t>(table);
	for (int v = 0; v < ctx.visitedN; ++v)
		if (ctx.visited[v] == pt) return;
	if (ctx.visitedN < 256) ctx.visited[ctx.visitedN++] = pt;

	uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
	const int nProps = *reinterpret_cast<int*>(table + ST_NPROPS);
	if (!props || nProps <= 0 || nProps > 2048) return;
	const char* tableName = *reinterpret_cast<const char**>(
		table + ST_NETTABLENAME);
	const int   tableNameLen = ODP_StrLenSafe(tableName, 128);

	if (!ODP_IsReadable(props, static_cast<size_t>(nProps) * SP_SIZE)) return;

	for (int j = 0; j < nProps; ++j)
	{
		uint8_t* p = props + static_cast<uint64_t>(j) * SP_SIZE;
		const int   type = *reinterpret_cast<int*>(p + SP_TYPE);
		const char* nm   = *reinterpret_cast<const char**>(p + SP_VARNAME);

		// Validate the name pointer before strcmp so a garbage qword doesn't
		// AV inside libc. Names should be short ASCII identifiers.
		const int nmLen = ODP_StrLenSafe(nm, 96);
		if (nmLen > 0 && strcmp(nm, ctx.targetName) == 0)
		{
			ctx.parentTable     = table;
			ctx.parentTableName = (tableNameLen > 0) ? tableName : nullptr;
			ctx.matchedProp     = p;
			ctx.depth           = depth;
			return;
		}
		if (type == 10)
		{
			uint8_t* child = *reinterpret_cast<uint8_t**>(p + 0x70);
			if (child) DTExtend_RecursiveOffhandFind(child, ctx, depth + 1);
			if (ctx.matchedProp) return;
		}
	}
}

static void DTExtend_DumpOffhandSubTable(void** tables, int count)
{
	if (!tables || count <= 0) return;
	Warning(eDLL_T::ENGINE, "[OFFHAND-SUB-DUMP] === recursive sweep over %d top-level tables ===\n", count);

	OffhandFindCtx ctx = {};
	ctx.targetName = "offhandWeapons";

	for (int t = 0; t < count && !ctx.matchedProp; ++t)
	{
		uint8_t* topTable = reinterpret_cast<uint8_t*>(tables[t]);
		if (!topTable) continue;
		DTExtend_RecursiveOffhandFind(topTable, ctx, 0);
	}

	if (!ctx.matchedProp)
	{
		Warning(eDLL_T::ENGINE,
			"[OFFHAND-SUB-DUMP] 'offhandWeapons' prop NOT FOUND at any depth "
			"(walked %d distinct tables). offhandWeapons may live in a "
			"non-registered SendTable or the prop name has changed.\n",
			ctx.visitedN);
		Warning(eDLL_T::ENGINE, "[OFFHAND-SUB-DUMP] === done ===\n");
		return;
	}

	Warning(eDLL_T::ENGINE,
		"[OFFHAND-SUB-DUMP] FOUND 'offhandWeapons' at depth=%d, parent='%s' "
		"(table=%p), prop=%p\n",
		ctx.depth, ctx.parentTableName ? ctx.parentTableName : "?",
		ctx.parentTable, ctx.matchedProp);

	// Body already ODP_IsReadable throughout; outer SEH was dead false safety.
	{
		uint8_t* parentTable = ctx.parentTable;
		const char* parentName = ctx.parentTableName;
		uint8_t* p = ctx.matchedProp;
		if (!p || !ODP_IsReadable(p, SP_SIZE))
		{
			Warning(eDLL_T::ENGINE,
				"[OFFHAND-SUB-DUMP] matched prop unreadable\n");
		}
		else
		{
			const int   type = *reinterpret_cast<int*>(p + SP_TYPE);
			const char* nm   = "offhandWeapons";

			Warning(eDLL_T::ENGINE,
				"[OFFHAND-SUB-DUMP] parent='%s' (table=%p) "
				"depth=%d type=%d nm='%s' off=0x%X\n",
				parentName ? parentName : "?", parentTable,
				ctx.depth, type, nm,
				*reinterpret_cast<int*>(p + SP_OFFSET));

			// type==10 should be DPT_DataTable. m_pDataTable at +0x70.
			if (type != 10)
			{
				Warning(eDLL_T::ENGINE,
					"[OFFHAND-SUB-DUMP] unexpected type %d (want 10 "
					"DPT_DataTable); aborting sub-walk.\n", type);
			}
			else
			{
				uint8_t* subTable = *reinterpret_cast<uint8_t**>(p + 0x70);
				if (!subTable || !ODP_IsReadable(subTable, 0x4C0 + 8))
				{
					Warning(eDLL_T::ENGINE,
						"[OFFHAND-SUB-DUMP] m_pDataTable=%p NOT readable\n",
						subTable);
				}
				else
				{

				const char* subName = *reinterpret_cast<const char**>(
					subTable + ST_NETTABLENAME);
				const int   subNameLen = ODP_StrLenSafe(subName, 128);
				uint8_t* subPropsArr = *reinterpret_cast<uint8_t**>(
					subTable + ST_PROPS);
				const int subNProps = *reinterpret_cast<int*>(
					subTable + ST_NPROPS);
				Warning(eDLL_T::ENGINE,
					"[OFFHAND-SUB-DUMP] sub-table='%s' table=%p props=%p "
					"nProps=%d  (ST_PROPS=+0x%X ST_NPROPS=+0x%X)\n",
					(subNameLen > 0) ? subName : "<unreadable>",
					subTable, subPropsArr,
					subNProps, ST_PROPS, ST_NPROPS);
				if (subPropsArr && subNProps > 0 && subNProps <= 256)
				{
					// Walk both ways: (a) contiguous SendProp[i*136], (b)
					// qword-pointer array. Dump both so the log shows which layout
					// yields plausible types/names at index 0.

					// (a) Contiguous walk at SP_SIZE=136. Pre-flight every read
					// since sub-table props may use a different stride and
					// reading past would AV.
					Warning(eDLL_T::ENGINE,
						"[OFFHAND-SUB-DUMP] -- (a) contiguous SendProp[136] walk --\n");
					const int subWalkN = (subNProps < 16) ? subNProps : 16;
					if (ODP_IsReadable(subPropsArr,
						static_cast<size_t>(subWalkN) * SP_SIZE))
					{
						for (int k = 0; k < subWalkN; ++k)
						{
							uint8_t* sp = subPropsArr + static_cast<uint64_t>(k) * SP_SIZE;
							const int   sty = *reinterpret_cast<int*>(sp + SP_TYPE);
							const int   snb = *reinterpret_cast<int*>(sp + SP_NBITS);
							const int   sfl = *reinterpret_cast<int*>(sp + SP_FLAGS);
							const int   sne = *reinterpret_cast<int*>(sp + SP_NELEMENTS);
							const int   sof = *reinterpret_cast<int*>(sp + SP_OFFSET);
							const char* snm = *reinterpret_cast<const char**>(sp + SP_VARNAME);
							const int   snmLen = ODP_StrLenSafe(snm, 96);
							Warning(eDLL_T::ENGINE,
								"[OFFHAND-SUB-DUMP]   (a)[%d] sp=%p ty=%d nb=%d "
								"fl=0x%X ne=%d off=0x%X nmPtr=%p nmLen=%d nm='%s'\n",
								k, sp, sty, snb, sfl, sne, sof, snm, snmLen,
								(snmLen > 0) ? snm : "<unreadable>");
						}
					}
					else
					{
						Warning(eDLL_T::ENGINE,
							"[OFFHAND-SUB-DUMP]   (a) subPropsArr+%zu bytes not "
							"fully readable; skipping contiguous walk\n",
							static_cast<size_t>(subWalkN) * SP_SIZE);
					}

					// (b) qword-pointer-array walk (a la build).
					Warning(eDLL_T::ENGINE,
						"[OFFHAND-SUB-DUMP] -- (b) qword-pointer-array walk --\n");
					if (ODP_IsReadable(subPropsArr,
						static_cast<size_t>(subWalkN) * 8))
					{
						for (int k = 0; k < subWalkN; ++k)
						{
							uint8_t** slot = reinterpret_cast<uint8_t**>(
								subPropsArr + static_cast<uint64_t>(k) * 8);
							uint8_t*  spB  = *slot;
							Warning(eDLL_T::ENGINE,
								"[OFFHAND-SUB-DUMP]   (b)[%d] slot=%p -> sp=%p\n",
								k, slot, spB);
							if (!spB || !ODP_IsReadable(spB, 0x80)) continue;
							const int   sty = *reinterpret_cast<int*>(spB + SP_TYPE);
							const int   snb = *reinterpret_cast<int*>(spB + SP_NBITS);
							const int   sfl = *reinterpret_cast<int*>(spB + SP_FLAGS);
							const int   sne = *reinterpret_cast<int*>(spB + SP_NELEMENTS);
							const int   sof = *reinterpret_cast<int*>(spB + SP_OFFSET);
							const char* snm = *reinterpret_cast<const char**>(spB + SP_VARNAME);
							const int   snmLen = ODP_StrLenSafe(snm, 96);
							Warning(eDLL_T::ENGINE,
								"[OFFHAND-SUB-DUMP]   (b)[%d]   ty=%d nb=%d fl=0x%X "
								"ne=%d off=0x%X nmPtr=%p nm='%s'\n",
								k, sty, snb, sfl, sne, sof, snm,
								(snmLen > 0) ? snm : "<unreadable>");
						}
					}

					// Hex-dump first 136 bytes of subPropsArr if readable.
					Warning(eDLL_T::ENGINE,
						"[OFFHAND-SUB-DUMP] -- raw bytes at subPropsArr[0..136] --\n");
					if (ODP_IsReadable(subPropsArr, 136))
					{
						for (int row = 0; row < 9; ++row)
						{
							char hex[16 * 3 + 1] = {};
							int  pos = 0;
							for (int c = 0; c < 16 && (row * 16 + c) < 136; ++c)
							{
								const uint8_t b = subPropsArr[row * 16 + c];
								pos += snprintf(hex + pos, sizeof(hex) - pos,
									"%02X ", b);
							}
							Warning(eDLL_T::ENGINE,
								"[OFFHAND-SUB-DUMP]   raw+%02X: %s\n",
								row * 16, hex);
						}
					}

					// Dereference subPropsArr[0] and hex-dump.
					if (ODP_IsReadable(subPropsArr, 8))
					{
						uint8_t* derefed0 = *reinterpret_cast<uint8_t**>(subPropsArr);
						if (derefed0 && ODP_IsReadable(derefed0, 136))
						{
							Warning(eDLL_T::ENGINE,
								"[OFFHAND-SUB-DUMP] -- raw bytes at *(subPropsArr[0]) "
								"= %p, 0..136 --\n", derefed0);
							for (int row = 0; row < 9; ++row)
							{
								char hex[16 * 3 + 1] = {};
								int  pos = 0;
								for (int c = 0; c < 16 && (row * 16 + c) < 136; ++c)
								{
									const uint8_t b = derefed0[row * 16 + c];
									pos += snprintf(hex + pos, sizeof(hex) - pos,
										"%02X ", b);
								}
								Warning(eDLL_T::ENGINE,
									"[OFFHAND-SUB-DUMP]   d0+%02X: %s\n",
									row * 16, hex);
							}
						}
						else
						{
							Warning(eDLL_T::ENGINE,
								"[OFFHAND-SUB-DUMP] *(subPropsArr[0]) = %p NOT "
								"readable -> walk (b) inappropriate for this table\n",
								derefed0);
						}
					}
				}
				}
			}
		}
	}
	Warning(eDLL_T::ENGINE, "[OFFHAND-SUB-DUMP] === done ===\n");
}
// ===========================================================================
// SYSTEM 09: DEATHFIELD CONFIG
// ===========================================================================

//-----------------------------------------------------------------------------
// Deathfield realms: rebuild DT_WORLD's 6 scalar m_deathField* props into the S21 struct-of-arrays layout -- DataTable(type-10) sub-tables of 64 scalar children each (isActive=Int, origin=Vector, radiusStart/End=Float, timeStart/End=Float).
// S3 sends them as scalars, so the client renames the type-mismatched native props to __skip_* and the realm rings never replicate.
//-----------------------------------------------------------------------------
ConVar sdk_deathfield_native_dt("sdk_deathfield_native_dt", "1", FCVAR_RELEASE,
	"Build DT_WORLD m_deathField*[64] DataTable arrays sourced from s_deathFields. "
	"Each field's 64 children get DISJOINT absolute offsets (DF_LAYOUT, base 4096), "
	"so the flattened SendTable has 384 unique offsets -- no changeframe descriptor "
	"overlap. Set 0 to skip the arrays (client rings stay default/inactive).");

// Ring count actually networked.
// The S21 client RecvTable declares 64 children per field.
static ConVar sdk_deathfield_ring_count("sdk_deathfield_ring_count", "64", FCVAR_RELEASE,
	"Deathfield realm rings to network (1..64). 64 = full S21 RecvTable parity. "
	"Read once at SendTable_Init.");

// Command-line +convars aren't applied until ~map load (t~7.4s), but this build
// runs at SendTable_Init (t~1.3s). Cmdline wins so +sdk_deathfield_native_dt 0
// still disables when the compiled default is 1.
bool DeathField_NativeDTRequestedAtLaunch()
{
	const char* val = nullptr;
	if (CommandLine()->CheckParm("+sdk_deathfield_native_dt", &val))
		return !val || val[0] != '0';
	return sdk_deathfield_native_dt.GetBool();
}

// Same timing trap for the ring count: +sdk_deathfield_ring_count N is not live
// at SendTable_Init. Prefer the command-line token, else the convar default (64).
int DeathField_RingCountAtLaunch()
{
	const char* val = nullptr;
	if (CommandLine()->CheckParm("+sdk_deathfield_ring_count", &val) && val && val[0])
		return atoi(val);
	return sdk_deathfield_ring_count.GetInt();
}

// ---------------------------------------------------------------------------
// DT_GlobalNonRewinding.m_playerMiscData -- S21 1:1 array of DT_NonRewindMiscData (m_nextRespawnTime float + m_musicPackAssigned int).
// The count is 128 rather than "however many players fit" on purpose.
// ---------------------------------------------------------------------------
ConVar sdk_nonrewind_misc_dt("sdk_nonrewind_misc_dt", "1", FCVAR_RELEASE,
	"Build DT_GlobalNonRewinding.m_playerMiscData as N x DT_NonRewindMiscData "
	"(m_nextRespawnTime + m_musicPackAssigned) sourced from the slot-indexed "
	"NonRewind store. Default 1. Disable with +sdk_nonrewind_misc_dt 0. Element "
	"count via +sdk_nonrewind_misc_count (default 128, clamp 1..128).");

static ConVar sdk_nonrewind_misc_count("sdk_nonrewind_misc_count", "128", FCVAR_RELEASE,
	"m_playerMiscData element count (1..128). 128 matches the client RecvTable; "
	"anything less leaves the tail unmatched there. Lower it only to test the "
	"snapshot budget. Read once at SendTable_Init via +sdk_nonrewind_misc_count.");

bool NonRewindMisc_DTRequestedAtLaunch()
{
	// Launch arg wins. SendTable_Init runs before +convar commands are executed,
	// so the ConVar still holds its compiled default here -- checking it first
	// would make +sdk_nonrewind_misc_dt 0 impossible to honour.
	const char* val = nullptr;
	if (CommandLine()->CheckParm("+sdk_nonrewind_misc_dt", &val))
		return !val || val[0] != '0';
	return sdk_nonrewind_misc_dt.GetBool();
}

int NonRewindMisc_CountAtLaunch()
{
	const char* val = nullptr;
	if (CommandLine()->CheckParm("+sdk_nonrewind_misc_count", &val) && val && val[0])
		return atoi(val);
	return sdk_nonrewind_misc_count.GetInt();
}

// ===========================================================================
// SYSTEM 10: SHARED DT UTILS -- cache + tree-finders
// ===========================================================================

// Cache the SendTable POINTERS so the sdk_dt_dump_tree ConCommand and the Find* helpers can resolve arbitrary SendTables by name post-init; the flat dump loses sub-table boundaries (DataTable parents are expanded by the precalc).
// Snapshot individual SendTable* pointers, NOT the tables[] array -- that array is a temporary bootstrap buffer whose lifetime ends after init, while the table objects are referenced globally for the process lifetime.
uintptr_t s_cachedSendTablePtrs[kMaxCachedSendTables] = {};
int       s_cachedSendTableCount = 0;

// Recursively walk a SendTable's tree, adding each encountered SendTable* (including DPT_DataTable sub-table children at SP_DATATABLE = 0x70) to the cache.
// Dedup via linear scan of already-cached pointers; depth-bounded + count-bounded for safety.
static int s_cacheRecurseFaults = 0;

static void DTExtend_CacheRecurse(uint8_t* table, int depth)
{
	if (!table || depth > 32) return;
	if (s_cachedSendTableCount >= kMaxCachedSendTables) return;
	if (!ODP_IsReadable(table, 0x4C0 + 8)) return;
	const uintptr_t pt = reinterpret_cast<uintptr_t>(table);
	for (int v = 0; v < s_cachedSendTableCount; ++v)
		if (s_cachedSendTablePtrs[v] == pt) return;
	s_cachedSendTablePtrs[s_cachedSendTableCount++] = pt;

	uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
	const int nProps = *reinterpret_cast<int*>(table + ST_NPROPS);
	if (!props || nProps <= 0 || nProps > 4096) return;
	if (!ODP_IsReadable(props, static_cast<size_t>(nProps) * SP_SIZE)) return;
	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* p = props + static_cast<uint64_t>(i) * SP_SIZE;
		const int ty = *reinterpret_cast<int*>(p + SP_TYPE);
		if (ty != 10) continue; // DPT_DataTable
		uint8_t* child = *reinterpret_cast<uint8_t**>(p + 0x70);
		if (!child) continue;
		// Child re-entry ODP-returns; count preflight skips for incomplete-cache signal.
		if (!ODP_IsReadable(child, 0x4C0 + 8))
		{
			++s_cacheRecurseFaults;
			continue;
		}
		DTExtend_CacheRecurse(child, depth + 1);
	}
}

void DTExtend_RebuildSendTableCache(void** tables, int count,
	const char* reason)
{
	s_cachedSendTableCount = 0;
	s_cacheRecurseFaults = 0;
	if (tables && count > 0)
	{
		for (int t = 0; t < count; ++t)
		{
			uint8_t* topTable = reinterpret_cast<uint8_t*>(tables[t]);
			if (!topTable) continue;
			DTExtend_CacheRecurse(topTable, 0);
		}
	}
	Msg(eDLL_T::ENGINE,
		"[DT-CACHE] rebuilt %d SendTables (%s)\n",
		s_cachedSendTableCount, reason ? reason : "no reason");
	if (s_cacheRecurseFaults > 0)
	{
		Warning(eDLL_T::ENGINE,
			"[DT-CACHE] CacheRecurse skipped %d child prop(s) (preflight unreadable) -- "
			"cache may be incomplete (sub-table Find* can miss)\n",
			s_cacheRecurseFaults);
	}
}

// Find a top-level SendTable in the cache by ST_NETTABLENAME.
uint8_t* DTExtend_FindTableByName(const char* name)
{
	if (!name) return nullptr;
	for (int t = 0; t < s_cachedSendTableCount; ++t)
	{
		uint8_t* st = reinterpret_cast<uint8_t*>(s_cachedSendTablePtrs[t]);
		if (!st) continue;
		if (!ODP_IsReadable(st, 0x4C0 + 8)) continue;
		const char* nm = *reinterpret_cast<const char**>(st + ST_NETTABLENAME);
		if (ODP_StrLenSafe(nm, 128) < 0) continue;
		if (strcmp(nm, name) == 0) return st;
	}
	return nullptr;
}

// Find the index of a SendProp named propName inside table's props array.
// Returns -1 if not found.
int DTExtend_FindPropIdx(uint8_t* table, const char* propName)
{
	if (!table || !propName) return -1;
	if (!ODP_IsReadable(table, 0x4C0 + 8)) return -1;
	uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
	const int nProps = *reinterpret_cast<int*>(table + ST_NPROPS);
	if (!props || nProps <= 0 || nProps > 4096) return -1;
	if (!ODP_IsReadable(props, static_cast<size_t>(nProps) * SP_SIZE)) return -1;
	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* p = props + static_cast<uint64_t>(i) * SP_SIZE;
		const char* nm = *reinterpret_cast<const char**>(p + SP_VARNAME);
		if (ODP_StrLenSafe(nm, 96) < 0) continue;
		if (strcmp(nm, propName) == 0) return i;
	}
	return -1;
}

static uint8_t* DTExtend_FindPropRecursive(uint8_t* table, const char* propName,
	int depth = 0)
{
	if (!table || !propName || depth > 16) return nullptr;
	if (!ODP_IsReadable(table, 0x4C0 + 8)) return nullptr;
	uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
	const int nProps = *reinterpret_cast<int*>(table + ST_NPROPS);
	if (!props || nProps <= 0 || nProps > 4096) return nullptr;
	if (!ODP_IsReadable(props, static_cast<size_t>(nProps) * SP_SIZE)) return nullptr;

	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* p = props + static_cast<uint64_t>(i) * SP_SIZE;
		const char* nm = *reinterpret_cast<const char**>(p + SP_VARNAME);
		if (ODP_StrLenSafe(nm, 96) < 0) continue;
		if (strcmp(nm, propName) == 0) return p;
	}

	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* p = props + static_cast<uint64_t>(i) * SP_SIZE;
		if (*reinterpret_cast<int*>(p + SP_TYPE) != 10) continue;
		uint8_t* child = *reinterpret_cast<uint8_t**>(p + 0x70);
		if (!child) continue;
		if (uint8_t* found = DTExtend_FindPropRecursive(child, propName, depth + 1))
			return found;
	}

	return nullptr;
}

// ===========================================================================
// SYSTEM 12: LIVE POST-PASSES -- overrides + flat-order
// ===========================================================================

// Apply data-driven SPROP_* flag overrides from s_flagOverrides.
// The m_decoyVelocity slot reorder lives in DTExtend_Apply via the EV_AFTER macro's insertAfter field, so this applier only handles existing-prop flag changes.
static ConVar bridge_prop_renames("bridge_prop_renames", "1", FCVAR_RELEASE,
	"[PROP-RENAME] Comprehensive dedi-side S3->S21 SendProp name reconciliation (the dedi port of the "
	"client s_propRenames): renames the wire prop names to the S21 client names so the matcher pairs "
	"them -- fixes the ~50-class unmatched-prop replication gap (attachments, dissolve fade, ideal "
	"activity/seq, thirdPerson blend, world decoder) + the TE-path NULL-source crash. Pairs with client "
	"bridge_recv_renames=0 (opposite direction; must not both run). 1=on (default), 0=off.");
// [WEAP-IDEAL-BIND] Binds m_IdealActivity->m_idealActivity / m_nIdealSequence-> m_idealSequence (S3 -> S21) on DT_WeaponX so the dedicated ideal activity/sequence replicates instead of pure client prediction. The dedicated side is load-bearing for sustaining a custom-activity sequence.
static ConVar bridge_weap_ideal_bind("bridge_weap_ideal_bind", "1", FCVAR_RELEASE,
	"Bind m_IdealActivity/m_nIdealSequence (S3) -> m_idealActivity/m_idealSequence (S21) on "
	"DT_WeaponX instead of leaving them client-predicted. Default ON. Inspect can play "
	"the wrong animation (S3<->S21 model-sequence-index drift on m_nIdealSequence); "
	"flip 0 to fall back to client-predicted if needed.");
// [SKYDIVE-STATE-BIND] The state half of the freefall/skydive family, gated apart from the other 8 because it is the only ORDINAL in the set.
// The two names are one field: the S3 dedi has m_freefallState and NO m_skydiveState, the S21 client has m_skydiveState and NO m_freefallState, both are int in DT_Player, and S3's own CPlayer interleaves m_freefallState inside the m_skydive* member run.
static ConVar bridge_skydive_state_bind("bridge_skydive_state_bind", "1", FCVAR_RELEASE,
	"Bind m_freefallState (S3) -> m_skydiveState (S21) so the client is told which skydive "
	"phase it is in. Name pair and ordinals (0 NONE / 1 DIVING / 2 ANTICIPATING) are "
	"shared across both builds. 0 = leave unmatched (client predicts the phase itself).");

ConVar bridge_dt_dup_append_suppress("bridge_dt_dup_append_suppress", "1", FCVAR_RELEASE,
	"Suppress a dt_extend append whose name the dedi already registers natively in the "
	"same table. 1 = suppress and keep the native prop (default). 0 = append anyway "
	"(legacy; two identically-named props, client binding ambiguous).");


//-----------------------------------------------------------------------------
// [RENAME-COLLIDE] An APPEND and a rename TARGET can land on the same name.
// The append adds the S21 name with no S3 member behind it; the rename pass then overwrites a real S3 prop's varName with that same name -- leaving TWO props of that name in one SendTable.
//-----------------------------------------------------------------------------
bool DTExtend_AppendSuppressedByRename(const char* tableName, const char* propName)
{
	if (!tableName || !propName)
		return false;

	// m_freefallState -> m_skydiveState, applied by DTExtend_RenameProps below.
	if (bridge_skydive_state_bind.GetBool()
		&& !strcmp(tableName, "DT_Player") && !strcmp(propName, "m_skydiveState"))
		return true;

	return false;
}

// A wire lever that is off must mean "not appended", not "appended as zero": an appended prop still ships a value every snapshot, and a value the S21 client never authored for itself is what produced the prediction-correction storm.
// Scoped to DT_LocalPlayerExclusive only -- every prop on it is value-proxied, so skipping without advancing the offset cursor cannot re-base anything. m_deathFieldIndex is NOT levered.
bool DTExtend_AppendSuppressedByWireLever(const char* tableName, const char* propName)
{
	if (!tableName || !propName)
		return false;
	if (strcmp(tableName, "DT_LocalPlayerExclusive") != 0)
		return false;

	if (!JetDrive_WireEnabled())
	{
		static const char* const s_jetDriveNames[] = {
			"m_jetDriveWasActive",
			"m_jetDriveActive",
			"m_jetDriveTargetEnt",
			"m_jetDriveInDecelWindow",
			"m_jetDriveSpeed",
			"m_jetDriveAccel",
			"m_jetDriveTimeout",
			"m_jetDriveDoubleJumpVelBackFrac",
			"m_jetDriveStartTime",
			"m_jetDriveDecelWindowTimeOutTime",
			"m_jetDriveTargetPos",
			"m_jetDriveTargetEntOffset",
			"m_jetDriveStartPos",
			"m_jetDriveDoubleJumpVelocity",
		};
		for (const char* psz : s_jetDriveNames)
		{
			if (strcmp(propName, psz) == 0)
				return true;
		}
	}

	if (!TriggerGravity_WireEnabled())
	{
		if (strcmp(propName, "m_gravityLiftActive") == 0
			|| strcmp(propName, "m_blackholeActive") == 0)
			return true;
	}

	if (!UpdraftBridge_WireEnabled())
	{
		static const char* const s_updraftNames[] = {
			"m_updraftCount",
			"m_updraftStage",
			"m_updraftEnterTime",
			"m_updraftLeaveTime",
			"m_updraftMinShakeActivationHeight",
			"m_updraftMaxShakeActivationHeight",
			"m_updraftLiftActivationHeight",
			"m_updraftLiftSpeed",
			"m_updraftLiftAcceleration",
			"m_updraftLiftExitDuration",
			"m_updraftSlowTime",
			"m_skydiveFromUpdraft",
		};
		for (const char* psz : s_updraftNames)
		{
			if (strcmp(propName, psz) == 0)
				return true;
		}
	}

	return false;
}

// True when this table already carries a prop of that name.
// An append that collides with one the dedi registers natively puts two identically-named props on the wire; the client pairs by name and can bind the appended one, whose offset addresses the synthetic append region instead of the real member.
int DTExtend_FindExistingPropIdx(const uint8_t* props, const int nProps,
	const char* propName)
{
	if (!props || !propName || nProps <= 0 || nProps > 4096)
		return -1;

	for (int i = 0; i < nProps; ++i)
	{
		const char* const nm = *reinterpret_cast<const char* const*>(
			props + static_cast<uint64_t>(i) * SP_SIZE + SP_VARNAME);
		if (ODP_StrLenSafe(nm, 96) < 0)
			continue;
		if (!strcmp(nm, propName))
			return i;
	}
	return -1;
}

//-----------------------------------------------------------------------------
// The DETECTION point for the collision above, kept generic on purpose: any future rename whose target name already exists in the table announces itself at boot instead of surfacing as a silent prediction mismatch.
// Suppressing the append is the fix; this is the tripwire that says a new one is needed.
//-----------------------------------------------------------------------------
static void DTExtend_WarnDuplicateRenameTarget(const uint8_t* props, const int nProps,
	const int renamedIdx, const char* tableName, const char* toName)
{
	for (int j = 0; j < nProps; ++j)
	{
		if (j == renamedIdx)
			continue;

		const char* const other = *reinterpret_cast<const char* const*>(
			props + static_cast<uint64_t>(j) * SP_SIZE + SP_VARNAME);
		if (ODP_StrLenSafe(other, 96) < 0)
			continue;

		if (!strcmp(other, toName))
		{
			Warning(eDLL_T::ENGINE,
				"[PROP-RENAME] DUPLICATE NAME '%s.%s' now at slots %d and %d -- the client "
				"pairs by name and may bind the empty one. Add it to "
				"DTExtend_AppendSuppressedByRename.\n",
				tableName ? tableName : "?", toName, j, renamedIdx);
			return;
		}
	}
}

// Tripwire: a rename that lands the S21 name in the wrong table never binds
// (CreateDecoders pairs by name WITHIN a table). Quiet if the table is not in
// the canonical schema at all (many S3 tables have no S21 twin).
static void DTExtend_WarnRenameTargetNotInSchema(const char* tableName, const char* toName)
{
	if (!tableName || !toName)
		return;

	// Deliberate unbind: the "__skip_" prefix exists to break name pairing, so
	// "will never bind" is the intended outcome, not a cross-table defect.
	if (strncmp(toName, "__skip_", 7) == 0)
		return;

	const S21SchemaTable* const sch = CanonSchemaFind(tableName);
	if (!sch)
		return;

	for (int i = 0; i < sch->nProps; ++i)
	{
		if (sch->props[i].name && !strcmp(sch->props[i].name, toName))
			return;
	}

	const char* foundIn[3] = {};
	int nFound = 0;
	for (int t = 0; t < g_s21SchemaCount && nFound < 3; ++t)
	{
		const S21SchemaTable& other = g_s21Schema[t];
		if (!other.table || !other.props)
			continue;
		if (other.table == sch->table)
			continue;
		for (int i = 0; i < other.nProps; ++i)
		{
			if (other.props[i].name && !strcmp(other.props[i].name, toName))
			{
				foundIn[nFound++] = other.table;
				break;
			}
		}
	}

	char where[192];
	if (nFound <= 0)
	{
		snprintf(where, sizeof(where), "(no canonical table)");
	}
	else
	{
		int used = 0;
		for (int i = 0; i < nFound; ++i)
		{
			used += snprintf(where + used, sizeof(where) - static_cast<size_t>(used),
				"%s%s", (i > 0) ? ", " : "", foundIn[i] ? foundIn[i] : "?");
			if (used < 0 || used >= (int)sizeof(where))
				break;
		}
	}

	Warning(eDLL_T::ENGINE,
		"[PROP-RENAME] CROSS-TABLE '%s.%s' -- the S21 client keeps that name in %s; "
		"it pairs RecvProps by name WITHIN a table, so this prop will never bind and "
		"the client reads 0.\n",
		tableName, toName, where);
}

static void DTExtend_RenameProps()
{
	if (!bridge_prop_renames.GetBool()) return;
	// scope==nullptr => all tables. type==-1 (default) => match any SendProp type; set it only when the S3 name is SHARED by two distinct props (the classic [template, array] idiom: the type=2 Vector template and the type=5 DPT_Array both named identically) and only ONE of the two needs the rename -- an unscoped rename would clobber the other's already-correct match.
	// See m_airMoveBlockPlanes below for the verified case.
	struct R { const char* from; const char* to; const char* scope; int type = -1; };
	static const R kRenames[] = {
		// Attachment family: S3 "...Index" -> S21 "...Id" (and the lone m_parentAttachmentIndex ->
		// m_parentAttachment, suffix dropped). Global: these map 1:1 on every carrier.
		{ "m_parentAttachmentIndex",                      "m_parentAttachment",                        nullptr },
		{ "m_controlPoint1AttachmentIndex",               "m_controlPoint1AttachmentId",               nullptr },
		{ "m_ziplineGrenadeBeginStationAttachmentIndex",  "m_ziplineGrenadeBeginStationAttachmentId",  nullptr },
		{ "m_nAttachmentIndex",                           "m_nAttachmentId",                           nullptr },
		{ "m_attachmentIndexForViewmodel",                "m_attachmentIdForViewmodel",                nullptr },
		{ "m_attachmentIndex",                            "m_attachmentId",                            nullptr },
		{ "m_attachmentIndex2",                           "m_attachmentId2",                           nullptr },
		{ "m_customActivityAttachedModelAttachmentIndex", "m_customActivityAttachedModelAttachmentId", nullptr },
		// Field renames -- distinctive S3 names, global-safe.
		{ "m_flFadeInLength",                             "m_flFadeLength",                            nullptr },
		{ "m_flFadeInStart",                              "m_flFadeStart",                             nullptr },
		{ "m_ignoresCollisionWithPlayers",               "m_ignoresCollisionWithCombatCharacters",    nullptr },
		// DT_WeaponX_LocalWeaponData: S3 m_lastPrimaryAttack -> S21 m_lastPrimaryAttackTime.
		// Absolute server time, no value drift -> safe. Scope is the table the prop is
		// found in (DT_WeaponX itself does not register it).
		{ "m_lastPrimaryAttack",                         "m_lastPrimaryAttackTime",                   "DT_WeaponX_LocalWeaponData" },
		// NOT renamed HERE by default: m_IdealActivity/m_nIdealSequence carry a weapon ACTIVITY ENUM and a model-bound SEQUENCE INDEX that drift S3<->S21, so a raw rename breaks melee.
		// Leave ?_unmatched so the client predicts locally.
		{ "m_thirdPersonEntBlendEaseInDuration",         "m_thirdPersonEntBlendInEaseInDuration",     nullptr },
		{ "m_thirdPersonEntBlendTotalDuration",          "m_thirdPersonEntBlendInTotalDuration",      nullptr },
		{ "m_thirdPersonEntBlendEaseOutDuration",        "m_thirdPersonEntBlendInEaseOutDuration",    nullptr },
		// [SKYDIVE-BIND] The drop-sequence family: S3 calls it "freefall", S21 calls it "skydive", and S21 genericized the Leviathan special-case into "DisableSkydiveEndEntity".
		// All 8 pairs exist on both sides with identical types.
		{ "m_freefallStartTime",                         "m_skydiveStartTime",                        nullptr },
		{ "m_freefallEndTime",                           "m_skydiveEndTime",                          nullptr },
		{ "m_freefallAnticipateStartTime",               "m_skydiveAnticipateStartTime",              nullptr },
		{ "m_freefallAnticipateEndTime",                 "m_skydiveAnticipateEndTime",                nullptr },
		{ "m_freefallDistanceToLand",                    "m_skydiveDistanceToLand",                   nullptr },
		{ "m_skydiveIsNearLeviathan",                    "m_skydiveIsNearDisableSkydiveEndEntity",    nullptr },
		{ "m_skydiveLeviathanHitPosition",               "m_skydiveDisableSkydiveEndEntityHitPosition", nullptr },
		{ "m_skydiveLeviathanHitNormal",                 "m_skydiveDisableSkydiveEndEntityHitNormal", nullptr },
		// [DUCK-TIMER] Do NOT rename m_nDuckTransitionTimeMsecs -> m_duckTransitionRemainderMsec: that puts the S21 name in DT_Local while the client keeps it only in DT_CurrentData_LocalPlayer, and CreateDecoders pairs by name WITHIN a table, so the prop would never bind.
		// [AIRMOVE-BLOCK-RENAME] m_airMoveBlockPlanes is the classic [template, array] idiom -- S3 has TWO SendProps of this name: type=2 Vector TEMPLATE and type=5 DPT_Array (nElements=2).
		{ "m_airMoveBlockPlanes",                        "m_airMoveBlockPlanes[0]",                   "DT_Local", 2 },
		// Generic S3 name -- SCOPED (m_fadeDist stays m_fadeDist on DT_PhysBox/DT_Zipline/etc.).
		{ "m_fadeDist",                                  "m_survivalPropFadeDist",                    "DT_PropSurvival" },
		// World DataTable prop (type 10): S3 "DT_WORLD" (caps) vs S21 "DT_World" -- strict-case match
		// fails -> world entity (classID 116) gets no decoder -> snapshot decode crash.
		{ "DT_WORLD",                                    "DT_World",                                  nullptr },
		// Applied only when native deathfield DT is OFF (see loop below).
		{ "m_deathFieldIsActive",                        "__skip_m_deathFieldIsActive",               nullptr },
		{ "m_deathFieldRadiusEnd",                       "__skip_m_deathFieldRadiusEnd",              nullptr },
		{ "m_deathFieldRadiusStart",                     "__skip_m_deathFieldRadiusStart",            nullptr },
		{ "m_deathFieldTimeEnd",                         "__skip_m_deathFieldTimeEnd",                nullptr },
		{ "m_deathFieldTimeStart",                       "__skip_m_deathFieldTimeStart",              nullptr },
		{ "m_deathFieldOrigin",                          "__skip_m_deathFieldOrigin",                 nullptr },
	};
	// [WEAP-IDEAL-BIND] gated separately from kRenames via bridge_weap_ideal_bind;
	// kept as its own tiny array/pass so toggling it never disturbs kRenames.
	static const R kWeapIdealRenames[] = {
		{ "m_IdealActivity",  "m_idealActivity",  "DT_WeaponX" },
		{ "m_nIdealSequence", "m_idealSequence",  "DT_WeaponX" },
	};
	// [SKYDIVE-STATE-BIND] its own tiny pass so the one ordinal in the family stays
	// toggleable without disturbing the 8 always-on data renames -- see the banner above.
	static const R kSkydiveStateRename[] = {
		{ "m_freefallState", "m_skydiveState", nullptr },
	};
	const bool bindWeapIdeal    = bridge_weap_ideal_bind.GetBool();
	const bool bindSkydiveState = bridge_skydive_state_bind.GetBool();
	// NOTE: NO type==10 skip here -- the world prop "DT_WORLD" is a DataTable (type 10) link that must
	// be renamed. Exact-name matching makes this safe (no other type-10 prop matches a 'from').
	int renamed = 0;
	int skippedTables = 0;
	int highFlagProps = 0;
	int renameHits[SDK_ARRAYSIZE(kRenames)] = {};
	int skydiveHits[SDK_ARRAYSIZE(kSkydiveStateRename)] = {};
	int weapIdealHits[SDK_ARRAYSIZE(kWeapIdealRenames)] = {};
	for (int t = 0; t < s_cachedSendTableCount; ++t)
	{
		uint8_t* table = reinterpret_cast<uint8_t*>(s_cachedSendTablePtrs[t]);
		if (!table) continue;
		if (!ODP_IsReadable(table, 0x4C0 + 8))
		{
			++skippedTables;
			continue;
		}
		const char* tn = *reinterpret_cast<const char**>(table + ST_NETTABLENAME);
		if (tn && ODP_StrLenSafe(tn, 128) < 0) tn = nullptr;
		uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
		const int nProps = *reinterpret_cast<int*>(table + ST_NPROPS);
		if (!props || nProps <= 0 || nProps > 4096) continue;
		if (!ODP_IsReadable(props, static_cast<size_t>(nProps) * SP_SIZE))
		{
			++skippedTables;
			continue;
		}
		for (int i = 0; i < nProps; ++i)
		{
			uint8_t* p = props + static_cast<uint64_t>(i) * SP_SIZE;
			const char* vn = *reinterpret_cast<const char**>(p + SP_VARNAME);
			if (ODP_StrLenSafe(vn, 96) < 0) continue;

			// Census only. ~217 of DT_Player's 1180 flat props carry bits above the low-16 networked subset while the client's descriptor carries the same value without them, and core props (m_localOrigin, m_angEyeAngles) are in that set and decode correctly -- so a high bit alone is NOT evidence of a broken prop.
			// Diff against the client before adding any flag override.
			const unsigned spFlags = *reinterpret_cast<unsigned*>(p + SP_FLAGS);
			if (spFlags & 0xFFFF0000u)
				++highFlagProps;

			bool didRename = false;
			for (int ri = 0; ri < (int)SDK_ARRAYSIZE(kRenames); ++ri)
			{
				const R& r = kRenames[ri];
				if (r.scope && (!tn || strcmp(tn, r.scope) != 0)) continue;
				if (r.type >= 0 && *reinterpret_cast<int*>(p + SP_TYPE) != r.type) continue;
				if (strcmp(vn, r.from) == 0)
				{
					if (DeathField_NativeDTRequestedAtLaunch() &&
						r.to && strncmp(r.to, "__skip_m_deathField", 19) == 0)
						continue;
					if (!DTExtend_IsSafeToRead(p + SP_VARNAME, sizeof(const char*), true))
						break;
					*reinterpret_cast<const char**>(p + SP_VARNAME) = r.to; // literal, static lifetime
					++renamed;
					++renameHits[ri];
					DTExtend_WarnDuplicateRenameTarget(props, nProps, i, tn, r.to);
					DTExtend_WarnRenameTargetNotInSchema(tn, r.to);
					didRename = true;
					break;
				}
			}
			if (didRename) continue;
			if (bindSkydiveState)
			{
				for (int ri = 0; ri < (int)SDK_ARRAYSIZE(kSkydiveStateRename); ++ri)
				{
					const R& r = kSkydiveStateRename[ri];
					if (r.scope && (!tn || strcmp(tn, r.scope) != 0)) continue;
					if (strcmp(vn, r.from) == 0)
					{
						if (!DTExtend_IsSafeToRead(p + SP_VARNAME, sizeof(const char*), true))
							break;
						*reinterpret_cast<const char**>(p + SP_VARNAME) = r.to;
						++renamed;
						++skydiveHits[ri];
						didRename = true;
						DTExtend_WarnDuplicateRenameTarget(props, nProps, i, tn, r.to);
						DTExtend_WarnRenameTargetNotInSchema(tn, r.to);
						break;
					}
				}
			}
			if (didRename || !bindWeapIdeal) continue;
			for (int ri = 0; ri < (int)SDK_ARRAYSIZE(kWeapIdealRenames); ++ri)
			{
				const R& r = kWeapIdealRenames[ri];
				if (r.scope && (!tn || strcmp(tn, r.scope) != 0)) continue;
				if (strcmp(vn, r.from) == 0)
				{
					if (!DTExtend_IsSafeToRead(p + SP_VARNAME, sizeof(const char*), true))
						break;
					*reinterpret_cast<const char**>(p + SP_VARNAME) = r.to;
					++renamed;
					++weapIdealHits[ri];
					break;
				}
			}
		}
	}
	Msg(eDLL_T::ENGINE, "[PROP-RENAME] comprehensive pass: renamed %d SendProp(s) across %d cached tables\n",
		renamed, s_cachedSendTableCount);
	if (highFlagProps > 0)
		Msg(eDLL_T::ENGINE,
			"[PROP-HIGHFLAG] %d SendProp(s) carry bits above the networked subset "
			"(census only -- widespread and mostly benign; diff against the client "
			"with tools/wire_schema_diff.py before acting)\n",
			highFlagProps);
	if (skippedTables > 0)
	{
		Warning(eDLL_T::ENGINE,
			"[PROP-RENAME] skipped %d table(s) (preflight unreadable) -- rename pass incomplete (green counts may lie)\n",
			skippedTables);
	}

	// Tripwire: a rename whose from-name never matched any SendProp is silent
	// without this -- wrong scope or a drifted name leaves the S21 recv slot at 0.
	for (int ri = 0; ri < (int)SDK_ARRAYSIZE(kRenames); ++ri)
	{
		if (renameHits[ri] != 0)
			continue;
		const R& r = kRenames[ri];
		if (r.to && strncmp(r.to, "__skip_", 7) == 0)
			continue;
		Warning(eDLL_T::ENGINE,
			"[PROP-RENAME] ENTRY NEVER FIRED '%s' -> '%s' (scope=%s) -- no SendProp "
			"of that name was found in scope, so the client never binds '%s' and "
			"reads 0.\n",
			r.from, r.to, r.scope ? r.scope : "*", r.to);
		if (!r.scope || !r.to)
			continue;
		const char* foundTable = nullptr;
		for (int t = 0; t < g_s21SchemaCount && !foundTable; ++t)
		{
			const S21SchemaTable& other = g_s21Schema[t];
			if (!other.table || !other.props)
				continue;
			for (int pi = 0; pi < other.nProps; ++pi)
			{
				if (other.props[pi].name && !strcmp(other.props[pi].name, r.to))
				{
					foundTable = other.table;
					break;
				}
			}
		}
		if (foundTable && strcmp(foundTable, r.scope) != 0)
		{
			Warning(eDLL_T::ENGINE,
				"[PROP-RENAME]   -> the S21 client keeps '%s' in %s; scope '%s' is the "
				"wrong table.\n", r.to, foundTable, r.scope);
		}
	}
	if (bindSkydiveState)
	{
		for (int ri = 0; ri < (int)SDK_ARRAYSIZE(kSkydiveStateRename); ++ri)
		{
			if (skydiveHits[ri] != 0)
				continue;
			const R& r = kSkydiveStateRename[ri];
			if (r.to && strncmp(r.to, "__skip_", 7) == 0)
				continue;
			Warning(eDLL_T::ENGINE,
				"[PROP-RENAME] ENTRY NEVER FIRED '%s' -> '%s' (scope=%s) -- no SendProp "
				"of that name was found in scope, so the client never binds '%s' and "
				"reads 0.\n",
				r.from, r.to, r.scope ? r.scope : "*", r.to);
		}
	}
	if (bindWeapIdeal)
	{
		for (int ri = 0; ri < (int)SDK_ARRAYSIZE(kWeapIdealRenames); ++ri)
		{
			if (weapIdealHits[ri] != 0)
				continue;
			const R& r = kWeapIdealRenames[ri];
			if (r.to && strncmp(r.to, "__skip_", 7) == 0)
				continue;
			Warning(eDLL_T::ENGINE,
				"[PROP-RENAME] ENTRY NEVER FIRED '%s' -> '%s' (scope=%s) -- no SendProp "
				"of that name was found in scope, so the client never binds '%s' and "
				"reads 0.\n",
				r.from, r.to, r.scope ? r.scope : "*", r.to);
			if (!r.scope || !r.to)
				continue;
			const char* foundTable = nullptr;
			for (int t = 0; t < g_s21SchemaCount && !foundTable; ++t)
			{
				const S21SchemaTable& other = g_s21Schema[t];
				if (!other.table || !other.props)
					continue;
				for (int pi = 0; pi < other.nProps; ++pi)
				{
					if (other.props[pi].name && !strcmp(other.props[pi].name, r.to))
					{
						foundTable = other.table;
						break;
					}
				}
			}
			if (foundTable && strcmp(foundTable, r.scope) != 0)
			{
				Warning(eDLL_T::ENGINE,
					"[PROP-RENAME]   -> the S21 client keeps '%s' in %s; scope '%s' is the "
					"wrong table.\n", r.to, foundTable, r.scope);
			}
		}
	}
}

// [PROP-DROP] Splice out S3-only props S21 removed entirely (a rename cannot express "gone").
// Mid-table extras shift every later flatten index -- last index OOB and CL_CopyNewEntity FAIL.
static ConVar bridge_prop_drops("bridge_prop_drops", "1", FCVAR_RELEASE,
	"[PROP-DROP] Splice S3-only SendProps that S21 removed (e.g. DT_Team reserved/"
	"connecting/loading counts) out of the dedi SendTables before precalc so "
	"flatten indices match the S21 RecvTable. 1 = on (default), 0 = native S3 shape.");

static void DTExtend_DropProps()
{
	if (!bridge_prop_drops.GetBool())
	{
		Warning(eDLL_T::ENGINE, "[PROP-DROP] disabled via bridge_prop_drops 0 -- S3-only SendProps stay on the wire\n");
		return;
	}
	struct D { const char* tableName; const char* propName; };
	static const D kDrops[] = {
		{ "DT_Team", "m_reservedPlayerCount" },
		{ "DT_Team", "m_connectingPlayerCount" },
		{ "DT_Team", "m_loadingPlayerCount" },
	};
	for (const auto& d : kDrops)
	{
		if (!d.tableName || !d.propName) continue;
		uint8_t* table = DTExtend_FindTableByName(d.tableName);
		if (!table)
		{
			Warning(eDLL_T::ENGINE, "[PROP-DROP] table '%s' not found; cannot drop '%s'\n",
				d.tableName, d.propName);
			continue;
		}
		const int idx = DTExtend_FindPropIdx(table, d.propName);
		if (idx < 0)
		{
			Warning(eDLL_T::ENGINE, "[PROP-DROP] %s.%s not found (already dropped or drifted?)\n",
				d.tableName, d.propName);
			continue;
		}
		if (!ODP_IsReadable(table, 0x4C0 + 8) ||
			!DTExtend_IsSafeToRead(table + ST_NPROPS, sizeof(int), true))
		{
			Warning(eDLL_T::ENGINE, "[PROP-DROP] %s.%s table unreadable -- skipped\n",
				d.tableName, d.propName);
			continue;
		}
		uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
		const int n = *reinterpret_cast<int*>(table + ST_NPROPS);
		if (!props || n <= 0 || idx >= n)
		{
			Warning(eDLL_T::ENGINE, "[PROP-DROP] %s.%s bad state (idx=%d n=%d) -- skipped\n",
				d.tableName, d.propName, idx, n);
			continue;
		}
		if (!DTExtend_IsSafeToRead(props, static_cast<size_t>(n) * SP_SIZE, true))
		{
			Warning(eDLL_T::ENGINE, "[PROP-DROP] %s.%s props unwritable -- skipped\n",
				d.tableName, d.propName);
			continue;
		}
		if (idx < n - 1)
			memmove(props + static_cast<uint64_t>(idx) * SP_SIZE,
				props + static_cast<uint64_t>(idx + 1) * SP_SIZE,
				static_cast<uint64_t>(n - 1 - idx) * SP_SIZE);
		*reinterpret_cast<int*>(table + ST_NPROPS) = n - 1;
		Msg(eDLL_T::ENGINE, "[PROP-DROP] %s.%s spliced at idx %d (%d -> %d props)\n",
			d.tableName, d.propName, idx, n, n - 1);
	}
}

static bool s_flagOverridesApplied = false;
static void DTExtend_ApplyFlagOverrides()
{
	if (s_flagOverridesApplied) return;
	s_flagOverridesApplied = true;

	int applied = 0;
	for (int i = 0; i < kNumFlagOverrides; ++i)
	{
		const DTFlagOverride& ov = s_flagOverrides[i];
		if (!ov.tableName || !ov.propName) continue;

		uint8_t* table = DTExtend_FindTableByName(ov.tableName);
		if (!table)
		{
			Warning(eDLL_T::ENGINE,
				"[PROP-FLAGS] '%s' not found; cannot override '%s' flags\n",
				ov.tableName, ov.propName);
			continue;
		}

		uint8_t* prop = ov.recursive
			? DTExtend_FindPropRecursive(table, ov.propName)
			: nullptr;
		if (!prop && !ov.recursive)
		{
			const int idx = DTExtend_FindPropIdx(table, ov.propName);
			if (idx >= 0)
			{
				uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
				if (props)
					prop = props + static_cast<uint64_t>(idx) * SP_SIZE;
			}
		}

		if (!prop)
		{
			Warning(eDLL_T::ENGINE,
				"[PROP-FLAGS] %s.%s not found (%s scope); flags 0x%X not applied\n",
				ov.tableName, ov.propName,
				ov.recursive ? "recursive" : "top-level", ov.flags);
			continue;
		}

		const int flagsBefore = *reinterpret_cast<int*>(prop + SP_FLAGS);
		if (flagsBefore == ov.flags)
			continue;

		*reinterpret_cast<int*>(prop + SP_FLAGS) = ov.flags;
		++applied;
	}

	Msg(eDLL_T::ENGINE,
		"[PROP-FLAGS] applied %d/%d flag overrides\n", applied, kNumFlagOverrides);
}

// Apply data-driven m_nBits widenings from s_bitWidthOverrides -- identical shape to DTExtend_ApplyFlagOverrides but writes SP_NBITS instead of SP_FLAGS.
// Runs once after DTExtend_Apply and before precalc so both the wire send-table info field and the snapshot int-encode pick up the widened field.
static bool s_bitWidthOverridesApplied = false;
static void DTExtend_ApplyBitWidthOverrides()
{
	if (s_bitWidthOverridesApplied) return;
	s_bitWidthOverridesApplied = true;

	int applied = 0;
	for (int i = 0; i < kNumBitWidthOverrides; ++i)
	{
		const DTBitWidthOverride& ov = s_bitWidthOverrides[i];
		if (!ov.tableName || !ov.propName) continue;

		uint8_t* table = DTExtend_FindTableByName(ov.tableName);
		if (!table)
		{
			Warning(eDLL_T::ENGINE,
				"[PROP-BITS] '%s' not found; cannot widen '%s'\n",
				ov.tableName, ov.propName);
			continue;
		}

		uint8_t* prop = ov.recursive
			? DTExtend_FindPropRecursive(table, ov.propName)
			: nullptr;
		if (!prop && !ov.recursive)
		{
			const int idx = DTExtend_FindPropIdx(table, ov.propName);
			if (idx >= 0)
			{
				uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
				if (props)
					prop = props + static_cast<uint64_t>(idx) * SP_SIZE;
			}
		}

		if (!prop)
		{
			Warning(eDLL_T::ENGINE,
				"[PROP-BITS] %s.%s not found (%s scope); nBits=%d not applied\n",
				ov.tableName, ov.propName,
				ov.recursive ? "recursive" : "top-level", ov.nBits);
			continue;
		}

		const int bitsBefore = *reinterpret_cast<int*>(prop + SP_NBITS);
		if (bitsBefore == ov.nBits)
			continue;

		*reinterpret_cast<int*>(prop + SP_NBITS) = ov.nBits;
		++applied;
	}

	Msg(eDLL_T::ENGINE,
		"[PROP-BITS] applied %d/%d bit-width overrides\n", applied, kNumBitWidthOverrides);
}

// Widen the two predictableFlags 6-slot immediates (0x3F set-all / 0xC0 mass-clear)
// to the client's 8-slot lifecycle (0xFF / full clear). Boot-time permanent for
// the process -- no detach/restore path.
static bool s_pflagsSlots8Applied = false;
static void DTExtend_ApplyPFlagsSlots8()
{
	if (s_pflagsSlots8Applied)
		return;
	s_pflagsSlots8Applied = true;

	if (!bridge_pflags_slots8.GetBool())
	{
		Msg(eDLL_T::ENGINE, "[PFLAGS8] disabled by convar\n");
		return;
	}

	// Site 1: or dword ptr [rbx+5B0Ch], 3Fh (set-all on weapon select)
	{
		const CMemory site = Module_FindPattern(g_GameDll,
			"83 8B 0C 5B 00 00 3F");
		if (!site)
		{
			Warning(eDLL_T::ENGINE,
				"[PFLAGS8] site 1 pattern unresolved -- immediate NOT patched\n");
		}
		else
		{
			site.Offset(6).Patch({ 0xFF });
			Msg(eDLL_T::ENGINE,
				"[PFLAGS8] site 1 patched at %p (imm 0x3F->0xFF)\n",
				reinterpret_cast<void*>(site.GetPtr()));
		}
	}

	// Site 2: and dword ptr [rdi+5B0Ch], 0FFFFFFC0h (mass clear)
	{
		const CMemory site = Module_FindPattern(g_GameDll,
			"83 A7 0C 5B 00 00 C0");
		if (!site)
		{
			Warning(eDLL_T::ENGINE,
				"[PFLAGS8] site 2 pattern unresolved -- immediate NOT patched\n");
		}
		else
		{
			site.Offset(6).Patch({ 0x00 });
			Msg(eDLL_T::ENGINE,
				"[PFLAGS8] site 2 patched at %p (imm 0xC0->0x00)\n",
				reinterpret_cast<void*>(site.GetPtr()));
		}
	}
}

// Comprehensive model-index re-widen. s_bitWidthOverrides only enumerates the NATIVE tables that carry an independent 13b m_nModelIndex; but bridge-synthesized S21 classes (s_s21Classes: DT_TriggerCylinderHeavy/Networked, DT_TriggerSlipSphere, DT_LootGrabber, DT_EnvDecoy, DT_CarePackageInsightProp) carry their OWN 13b model- index SendProps the per-class list never named.
// The shared DT_BaseEntity widen does not reach them (the synth/reparent chain points at independent copies), so the dedi packs 13 while the client decodes 14 -> bitstream drift -> ReadPropIdx over-run -> OOB -> dropped snapshot ([FLATN-SCAN] found 9 such props across 6 classes).
static bool s_modelIndexRewidenApplied = false;
static void DTExtend_RewidenAllModelIndexProps()
{
	if (s_modelIndexRewidenApplied) return;
	s_modelIndexRewidenApplied = true;

	int widened = 0;
	int skippedTables = 0;
	for (int t = 0; t < s_cachedSendTableCount; ++t)
	{
		uint8_t* table = reinterpret_cast<uint8_t*>(s_cachedSendTablePtrs[t]);
		if (!table) continue;
		// Boot/install only -- ODP preflight OK (not encode path).
		if (!ODP_IsReadable(table, 0x4C0 + 8))
		{
			++skippedTables;
			continue;
		}
		const char* tn = *reinterpret_cast<const char**>(table + ST_NETTABLENAME);
		uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
		const int nProps = *reinterpret_cast<int*>(table + ST_NPROPS);
		if (!props || nProps <= 0 || nProps > 4096) continue;
		if (!ODP_IsReadable(props, static_cast<size_t>(nProps) * SP_SIZE))
		{
			++skippedTables;
			continue;
		}
		for (int i = 0; i < nProps; ++i)
		{
			uint8_t* p = props + static_cast<uint64_t>(i) * SP_SIZE;
			if (*reinterpret_cast<int*>(p + SP_TYPE) == 10) continue; // DataTable link
			const char* nm = *reinterpret_cast<const char**>(p + SP_VARNAME);
			if (ODP_StrLenSafe(nm, 96) < 0 || !strstr(nm, "ModelIndex")) continue;
			int* pnb = reinterpret_cast<int*>(p + SP_NBITS);
			if (*pnb == 13)
			{
				*pnb = 14;
				++widened;
				Msg(eDLL_T::ENGINE,
					"[MDLIDX-REWIDEN] %s.%s nBits 13 -> 14\n", tn ? tn : "?", nm);
			}
		}
	}
	Msg(eDLL_T::ENGINE,
		"[MDLIDX-REWIDEN] comprehensive pass: widened %d model-index SendProp(s) 13->14 "
		"across %d cached tables\n", widened, s_cachedSendTableCount);
	if (skippedTables > 0)
		Warning(eDLL_T::ENGINE,
			"[MDLIDX-REWIDEN] skipped %d table(s) (preflight unreadable)\n", skippedTables);
}

// Comprehensive effect-index re-widen -- the effect-index twin of the model-index pass above, added for the SAME reason it was needed there. s_bitWidthOverrides enumerates only DT_TEScriptParticleSystem +...OnEntity and ASSUMES...OnEntityWithPos (and any other carrier) inherits the m_effectIndex widen.
// But the synth/reparent chain makes INDEPENDENT prop copies (exactly how the per-class model-index list missed 9 props), so an assumed-inherited m_effectIndex can still be 11b: the dedi then packs 11 while the 12b client decodes 12 -> bitstream drift, AND the index wraps (& 0x7FF) for the ~1600 ParticleEffectNames entries at idx >= 2048 (table holds 3682) -> wrong/garbage particle definition -> uninitialized collection control points -> the bangalore-tactical crash.
static bool s_effectIndexRewidenApplied = false;
static void DTExtend_RewidenAllEffectIndexProps()
{
	if (s_effectIndexRewidenApplied) return;
	s_effectIndexRewidenApplied = true;

	int widened = 0;
	int skippedTables = 0;
	for (int t = 0; t < s_cachedSendTableCount; ++t)
	{
		uint8_t* table = reinterpret_cast<uint8_t*>(s_cachedSendTablePtrs[t]);
		if (!table) continue;
		if (!ODP_IsReadable(table, 0x4C0 + 8))
		{
			++skippedTables;
			continue;
		}
		const char* tn = *reinterpret_cast<const char**>(table + ST_NETTABLENAME);
		uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
		const int nProps = *reinterpret_cast<int*>(table + ST_NPROPS);
		if (!props || nProps <= 0 || nProps > 4096) continue;
		if (!ODP_IsReadable(props, static_cast<size_t>(nProps) * SP_SIZE))
		{
			++skippedTables;
			continue;
		}
		for (int i = 0; i < nProps; ++i)
		{
			uint8_t* p = props + static_cast<uint64_t>(i) * SP_SIZE;
			if (*reinterpret_cast<int*>(p + SP_TYPE) == 10) continue; // DataTable link
			const char* nm = *reinterpret_cast<const char**>(p + SP_VARNAME);
			if (ODP_StrLenSafe(nm, 96) < 0 || !strstr(nm, "ffectIndex")) continue;
			int* pnb = reinterpret_cast<int*>(p + SP_NBITS);
			const int before = *pnb;
			if (before < kEffectIndexBits)
			{
				*pnb = kEffectIndexBits;
				++widened;
				Msg(eDLL_T::ENGINE,
					"[FXIDX-REWIDEN] %s.%s nBits %d -> %d\n",
					tn ? tn : "?", nm, before, kEffectIndexBits);
			}
		}
	}
	Msg(eDLL_T::ENGINE,
		"[FXIDX-REWIDEN] comprehensive pass: widened %d effect-index SendProp(s) to %d bits "
		"across %d cached tables\n", widened, kEffectIndexBits, s_cachedSendTableCount);
	if (skippedTables > 0)
		Warning(eDLL_T::ENGINE,
			"[FXIDX-REWIDEN] skipped %d table(s) (preflight unreadable)\n", skippedTables);
}

// m_camoIndex S3->S21 sentinel translation.
// S3 networks m_camoIndex as a SIGNED 10-bit int whose no-camo sentinel is -1 (valid range [-1, numSkins)); S21 made the no-camo sentinel 0 and indexes camo_skins as table[8*camo] for any NONZERO value, so an authentic S3 -1 arrives as 0xFFFFFFFF -> ~34GB OOB -> AV.
static void __fastcall Camo_ClampSentinelProxy(void* pProp, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	int v = 0;
	if (pStruct && pProp)
	{
		const int off = *(int*)((const char*)pProp + SP_OFFSET) & 0xFFFFF;
		v = *(const int*)((const char*)pStruct + off);
	}
	*(int*)pOut = (v < 0) ? 0 : v;   // S3 -1 (no camo) -> S21 0 (no camo)
}

// Default ON. 0 = ship the raw S3 m_camoIndex (-1 reaches the S21 client; only the
// client-side sdk_bridge_camo_fix masks then stand between -1 and the camo OOB AV).
static ConVar bridge_camo_encode("bridge_camo_encode", "1", FCVAR_RELEASE,
	"Translate m_camoIndex's S3 '-1 = no camo' sentinel to S21's '0 = no camo' on the "
	"wire (clamp negative -> 0; real camos pass through). 1 = ON (retires the client-side "
	"camo OOB at the encode boundary). 0 = raw S3 value (reproduces the -1 crash unless "
	"the client sdk_bridge_camo_fix masks are on).");

// Comprehensive install: walk the WHOLE cached SendTable set and install Camo_ClampSentinelProxy on every m_camoIndex leaf (DPT_Int). m_camoIndex is a flat prop on multiple unrelated tables -- DT_BaseAnimating (inherited by players/dynamic props), DT_PropSurvival (ground loot -- the high-volume -1 source on a loot map), and any lightweight/future carrier -- so a single-table install would miss loot.
// Walking the cache covers every carrier and any future one automatically (same doctrine as the model-index re-widen above).
static bool s_camoSentinelProxyApplied = false;
static void DTExtend_ApplyCamoSentinelProxy()
{
	if (s_camoSentinelProxyApplied) return;
	s_camoSentinelProxyApplied = true;

	if (!bridge_camo_encode.GetBool())
	{
		Warning(eDLL_T::ENGINE,
			"[CAMO-ENCODE] DISABLED (bridge_camo_encode=0): raw S3 m_camoIndex=-1 ships to "
			"the S21 client -- relying on client-side sdk_bridge_camo_fix masks\n");
		return;
	}

	int installed = 0;
	int skippedTables = 0;
	for (int t = 0; t < s_cachedSendTableCount; ++t)
	{
		uint8_t* table = reinterpret_cast<uint8_t*>(s_cachedSendTablePtrs[t]);
		if (!table) continue;
		if (!ODP_IsReadable(table, 0x4C0 + 8))
		{
			++skippedTables;
			continue;
		}
		const char* tn = *reinterpret_cast<const char**>(table + ST_NETTABLENAME);
		uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
		const int nProps = *reinterpret_cast<int*>(table + ST_NPROPS);
		if (!props || nProps <= 0 || nProps > 4096) continue;
		if (!ODP_IsReadable(props, static_cast<size_t>(nProps) * SP_SIZE))
		{
			++skippedTables;
			continue;
		}
		for (int i = 0; i < nProps; ++i)
		{
			uint8_t* p = props + static_cast<uint64_t>(i) * SP_SIZE;
			if (*reinterpret_cast<int*>(p + SP_TYPE) != 0) continue; // DPT_Int only
			const char* nm = *reinterpret_cast<const char**>(p + SP_VARNAME);
			if (ODP_StrLenSafe(nm, 64) < 0 || strcmp(nm, "m_camoIndex") != 0) continue;
			*reinterpret_cast<uintptr_t*>(p + 0x60) = (uintptr_t)&Camo_ClampSentinelProxy;
			++installed;
			Msg(eDLL_T::ENGINE,
				"[CAMO-ENCODE] %s.m_camoIndex -> clamp-neg proxy installed\n",
				tn ? tn : "?");
		}
	}
	Msg(eDLL_T::ENGINE,
		"[CAMO-ENCODE] comprehensive pass: installed %d m_camoIndex clamp proxy(ies) "
		"across %d cached tables\n", installed, s_cachedSendTableCount);
	if (skippedTables > 0)
		Warning(eDLL_T::ENGINE,
			"[CAMO-ENCODE] skipped %d table(s) (preflight unreadable)\n", skippedTables);
}

// Grow DT_BaseAnimating.m_flPoseParameter child SendTable 12 -> 24.
// Element count lives in the child ST_NPROPS (type-10 parent SP_NELEMENTS stays 1).
static ConVar bridge_pose_param_wire("bridge_pose_param_wire", "1", FCVAR_RELEASE,
	"Grow DT_BaseAnimating's m_flPoseParameter child SendTable from S3's 12 "
	"elements to the S21 client's 24 and source elements 12..23 from the "
	"server-side extended pose-param table. 1 = ON. 0 = ship S3's 12 (the S21 "
	"client leaves [0012]..[0023] unmatched and the extended pose never renders). "
	"Read once at SendTable init -- set it on the command line, not mid-game.");

static const char* const kPoseExtElemNames[12] = {
	"[0012]", "[0013]", "[0014]", "[0015]", "[0016]", "[0017]",
	"[0018]", "[0019]", "[0020]", "[0021]", "[0022]", "[0023]"
};

static bool s_poseParamWireApplied = false;
static void DTExtend_GrowPoseParamArray(void)
{
	if (s_poseParamWireApplied)
		return;
	s_poseParamWireApplied = true;

	if (!bridge_pose_param_wire.GetBool())
	{
		Warning(eDLL_T::ENGINE,
			"[POSE-WIRE] DISABLED (bridge_pose_param_wire=0): S3 ships 12 "
			"m_flPoseParameter elements; S21 client [0012]..[0023] stay unmatched "
			"and extended pose never renders\n");
		return;
	}

	constexpr int kNativePoseSlots = 12;
	const int kWirePoseSlots = kNativePoseSlots + PoseParamExt_GetWireSlotCount();
	if (kWirePoseSlots != 24 || PoseParamExt_GetWireSlotCount() != 12)
	{
		Warning(eDLL_T::ENGINE,
			"[POSE-WIRE] unexpected wire slot count (native=%d ext=%d wire=%d) -- "
			"grow skipped\n",
			kNativePoseSlots, PoseParamExt_GetWireSlotCount(), kWirePoseSlots);
		return;
	}

	void* visited[64] = {};
	int nVisited = 0;
	int grown = 0;
	int skippedTables = 0;
	int skippedVisitedCap = 0;

	for (int t = 0; t < s_cachedSendTableCount; ++t)
	{
		uint8_t* table = reinterpret_cast<uint8_t*>(s_cachedSendTablePtrs[t]);
		if (!table)
			continue;
		if (!ODP_IsReadable(table, 0x4C0 + 8))
		{
			++skippedTables;
			continue;
		}
		uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
		const int nProps = *reinterpret_cast<int*>(table + ST_NPROPS);
		if (!props || nProps <= 0 || nProps > 4096)
			continue;
		if (!ODP_IsReadable(props, static_cast<size_t>(nProps) * SP_SIZE))
		{
			++skippedTables;
			continue;
		}

		for (int i = 0; i < nProps; ++i)
		{
			uint8_t* p = props + static_cast<uint64_t>(i) * SP_SIZE;
			if (*reinterpret_cast<int*>(p + SP_TYPE) != 10)
				continue; // DPT_DataTable
			const char* nm = *reinterpret_cast<const char**>(p + SP_VARNAME);
			if (ODP_StrLenSafe(nm, 64) < 0 || strcmp(nm, "m_flPoseParameter") != 0)
				continue;

			uint8_t* child = *reinterpret_cast<uint8_t**>(p + SP_CHILDTABLE);
			if (!child)
			{
				Warning(eDLL_T::ENGINE,
					"[POSE-WIRE] m_flPoseParameter type-10 prop has null child -- skipped\n");
				continue;
			}

			bool seen = false;
			for (int v = 0; v < nVisited; ++v)
			{
				if (visited[v] == child)
				{
					seen = true;
					break;
				}
			}
			if (seen)
				continue;
			if (nVisited >= 64)
			{
				++skippedVisitedCap;
				continue;
			}
			visited[nVisited++] = child;

			if (!ODP_IsReadable(child, 0x4C0 + 8))
			{
				Warning(eDLL_T::ENGINE,
					"[POSE-WIRE] m_flPoseParameter child unreadable -- skipped\n");
				continue;
			}

			uint8_t* childProps = *reinterpret_cast<uint8_t**>(child + ST_PROPS);
			const int childN = *reinterpret_cast<int*>(child + ST_NPROPS);
			if (!childProps || childN <= 0 || childN > 4096 ||
				!ODP_IsReadable(childProps, static_cast<size_t>(childN) * SP_SIZE))
			{
				Warning(eDLL_T::ENGINE,
					"[POSE-WIRE] m_flPoseParameter child props unreadable "
					"(n=%d) -- skipped\n", childN);
				continue;
			}

			if (childN == kWirePoseSlots)
			{
				DevMsg(eDLL_T::ENGINE,
					"[POSE-WIRE] child already has %d props -- already grown\n",
					childN);
				continue;
			}
			if (childN != kNativePoseSlots)
			{
				Warning(eDLL_T::ENGINE,
					"[POSE-WIRE] unexpected child nProps=%d (want %d) -- skipped\n",
					childN, kNativePoseSlots);
				continue;
			}

			uint8_t* newProps = static_cast<uint8_t*>(
				malloc(static_cast<size_t>(kWirePoseSlots) * SP_SIZE));
			if (!newProps)
			{
				Warning(eDLL_T::ENGINE,
					"[POSE-WIRE] malloc failed for %d props -- left at 12\n",
					kWirePoseSlots);
				continue;
			}

			memcpy(newProps, childProps,
				static_cast<size_t>(kNativePoseSlots) * SP_SIZE);

			// Element 11 is the float-encoding template for slots 12..23.
			const uint8_t* tmpl = newProps +
				static_cast<uint64_t>(kNativePoseSlots - 1) * SP_SIZE;
			for (int e = kNativePoseSlots; e < kWirePoseSlots; ++e)
			{
				uint8_t* ep = newProps + static_cast<uint64_t>(e) * SP_SIZE;
				memcpy(ep, tmpl, SP_SIZE);
				*reinterpret_cast<const char**>(ep + SP_VARNAME) =
					kPoseExtElemNames[e - kNativePoseSlots];
				*reinterpret_cast<int*>(ep + SP_OFFSET) = e; // stashed elem index
				*reinterpret_cast<uintptr_t*>(ep + 0x60) =
					reinterpret_cast<uintptr_t>(&PoseParamExt_WireProxy);
				*reinterpret_cast<uintptr_t*>(ep + 0x70) = 0;
				*reinterpret_cast<int*>(ep + SP_NELEMENTS) = 1;
			}

			// Old props array deliberately leaked (one-shot at SendTable init;
			// other passes may still hold pointers into it). Do not free.
			// Leave child +0x4C0 / +0x508 alone -- live table list links.
			*reinterpret_cast<uint8_t**>(child + ST_PROPS) = newProps;
			*reinterpret_cast<int*>(child + ST_NPROPS) = kWirePoseSlots;
			++grown;
		}
	}

	Msg(eDLL_T::ENGINE,
		"[POSE-WIRE] grew %d m_flPoseParameter child table(s) to 24 elements "
		"across %d cached tables\n", grown, s_cachedSendTableCount);
	if (skippedTables > 0)
		Warning(eDLL_T::ENGINE,
			"[POSE-WIRE] skipped %d table(s) (preflight unreadable)\n",
			skippedTables);
	if (skippedVisitedCap > 0)
		Warning(eDLL_T::ENGINE,
			"[POSE-WIRE] visited cap (64) full -- skipped %d additional "
			"child table(s)\n", skippedVisitedCap);
}

// [WEAP-ACT-XLAT] S3->S21 activity ID translation for the wire-bound weapon ideal-activity field.
// ActivityList_RegisterSharedActivities registers in strict sequential order, so ACT_* IDs are list positions -- S21 inserted/ reshuffled ACT_VM_* relative to S3: ACT_VM_IDLE S3=468/S21=472, ACT_VM_RELOAD 499/505, ACT_VM_WEAPON_INSPECT 557/564.
static void __fastcall WeapIdealActivity_XlatProxy(void* pProp, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	int v = 0;
	if (pStruct && pProp)
	{
		const int off = *(int*)((const char*)pProp + SP_OFFSET) & 0xFFFFF;
		v = *(const int*)((const char*)pStruct + off);
	}
	const int xlat = Bridge_TranslateS3ActivityToS21(v);
	*(int*)pOut = xlat;
}

// Comprehensive install: walk the cached SendTable set and install WeapIdealActivity_XlatProxy on the activity-carrying DPT_Int leaves of DT_WeaponX - "m_idealActivity" (post-rename name; only exists when bridge_weap_ideal_bind has renamed the S3 "m_IdealActivity" prop, so the name match self-gates on that convar) - "m_customActivity" (identically named, always wire-matched).
// [WEAP-ACT-C2S] stores S3-space ids (inspect=557); raw on the S21 client is a wrong activity (S21 557 is a melee-tier act).
static bool s_weapActXlatProxyApplied = false;
static void DTExtend_ApplyWeapIdealActivityXlatProxy()
{
	if (s_weapActXlatProxyApplied) return;
	s_weapActXlatProxyApplied = true;

	int installed = 0;
	for (int t = 0; t < s_cachedSendTableCount; ++t)
	{
		uint8_t* table = reinterpret_cast<uint8_t*>(s_cachedSendTablePtrs[t]);
		if (!table || !ODP_IsReadable(table, 0x4C0 + 8)) continue;
		const char* tn = *reinterpret_cast<const char**>(table + ST_NETTABLENAME);
		if (ODP_StrLenSafe(tn, 64) < 0 || strcmp(tn, "DT_WeaponX") != 0) continue;
		uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
		const int nProps = *reinterpret_cast<int*>(table + ST_NPROPS);
		if (!props || nProps <= 0 || nProps > 4096) continue;
		if (!ODP_IsReadable(props, static_cast<size_t>(nProps) * SP_SIZE)) continue;
		for (int i = 0; i < nProps; ++i)
		{
			uint8_t* p = props + static_cast<uint64_t>(i) * SP_SIZE;
			if (*reinterpret_cast<int*>(p + SP_TYPE) != 0) continue; // DPT_Int only
			const char* nm = *reinterpret_cast<const char**>(p + SP_VARNAME);
			if (ODP_StrLenSafe(nm, 64) < 0) continue;
			// m_weaponActivity: client's HUD-hide watch (sh_weapon_inspect.gnut)
			// polls GetWeaponActivity per frame and bails when the raw S3-space
			// wire value (557) mismatches S21 ACT_VM_WEAPON_INSPECT (564).
			if (strcmp(nm, "m_idealActivity") != 0 && strcmp(nm, "m_customActivity") != 0
				&& strcmp(nm, "m_weaponActivity") != 0) continue;
			*reinterpret_cast<uintptr_t*>(p + 0x60) = (uintptr_t)&WeapIdealActivity_XlatProxy;
			++installed;
			Msg(eDLL_T::ENGINE, "[WEAP-ACT-XLAT] %s.%s -> S3->S21 translate proxy installed\n",
				tn ? tn : "?", nm);
		}
	}
	Msg(eDLL_T::ENGINE, "[WEAP-ACT-XLAT] comprehensive pass: installed %d weapon-activity translate proxy(ies)\n",
		installed);
}

//=============================================================================
// [WEAPSTATE-XLAT] S3->S21 m_weapState wire remap.
// S21 inserted ENERGIZE(8) and COOLDOWN_OVERHEAT(22) into WeaponState_e, so every S3 ordinal from 8 up is one-to-two low on the S21 client: S3 ATTACK(9) reads as SPRINT, RELOAD(10) as ATTACK, SPRINT(8) as ENERGIZE.
//=============================================================================
static ConVar bridge_weapstate_s21_ordinals("bridge_weapstate_s21_ordinals", "1", FCVAR_RELEASE,
	"S21 bridge: emit S21 m_weapState ordinals on the wire (S3 ATTACK=9 -> S21 10). "
	"The S3 server's own value is untouched; only the wire copy changes. 0 = raw S3 "
	"ordinals, which the S21 client reads as SPRINT for ATTACK, ATTACK for RELOAD and "
	"ENERGIZE for SPRINT.");

// [WEAPSTATE-ENERGIZE] The one S21 state the translation can never produce.
// WeapState_S3ToS21 is `s3 + (s3>=8) + (s3>=21)`, so s3=7 maps to 7 and s3=8 maps to 9: ordinal 8 is a HOLE in the output range, and it is exactly WEAP_STATE_ENERGIZE -- the state S21 inserted and S3 never had.
static constexpr int kWeapStateS21_Energize = 8;
static constexpr int kEnergizeState_Energizing = 1; // EnergizeState_e::ENERGIZING

static ConVar bridge_weapstate_energize("bridge_weapstate_energize", "1", FCVAR_RELEASE,
	"S21 bridge: emit WEAP_STATE_ENERGIZE(8) on the wire while the energize FSM has "
	"the weapon winding up. S3 has no such state and the ordinal translation cannot "
	"produce it. 0 = ship the translated S3 state, which stomps the client's own.");

static constexpr int kWeapStateS21_Cooldown = 21;
static constexpr int kWeapStateS21_CooldownOverheat = 22;

static ConVar bridge_weapstate_cooldown_overheat("bridge_weapstate_cooldown_overheat", "1",
	FCVAR_RELEASE,
	"S21 bridge: emit WEAP_STATE_COOLDOWN_OVERHEAT(22) on the wire while a charge "
	"weapon is overheated in COOLDOWN. S3 has no such state and the ordinal "
	"translation cannot produce it. 0 = ship translated COOLDOWN (21).");

static void __fastcall WeapState_XlatProxy(void* pProp, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;

	int v = 0;
	if (pStruct && pProp)
	{
		const int off = *(int*)((const char*)pProp + SP_OFFSET) & 0xFFFFF;
		v = *(const int*)((const char*)pStruct + off);
	}

	const bool bXlat = bridge_weapstate_s21_ordinals.GetBool();
	int out = bXlat ? WeapState_S3ToS21(v) : v;

	if (bXlat && bridge_weapstate_energize.GetBool() && pStruct)
	{
		int energizeState = 0;
		if (EnergizeBridge_WireGet(pStruct, &energizeState, nullptr, nullptr)
			&& energizeState == kEnergizeState_Energizing)
		{
			static int s_loggedOnce = 0;
			if (s_loggedOnce < 4)
			{
				++s_loggedOnce;
				Msg(eDLL_T::ENGINE,
					"[WEAPSTATE-ENERGIZE] wind-up: emitting WEAP_STATE_ENERGIZE(8) "
					"in place of S3 %d (translated %d)\n", v, out);
			}
			out = kWeapStateS21_Energize;
		}
	}

	if (bXlat && bridge_weapstate_cooldown_overheat.GetBool() && pStruct
		&& out == kWeapStateS21_Cooldown && WeaponHeat_IsChargeOverheated(pStruct))
	{
		static int s_loggedOhOnce = 0;
		if (s_loggedOhOnce < 4)
		{
			++s_loggedOhOnce;
			Msg(eDLL_T::ENGINE,
				"[WEAPSTATE-OH] overheat: emitting WEAP_STATE_COOLDOWN_OVERHEAT(22) "
				"in place of S3 %d (translated %d)\n", v, out);
		}
		out = kWeapStateS21_CooldownOverheat;
	}

	*(int*)pOut = out;
	BridgeStat_Bump(bXlat
		? BridgeStat_e::WEAPSTATE_XLAT_TRANSLATED
		: BridgeStat_e::WEAPSTATE_XLAT_PASSTHROUGH);
}

// Install on the DT_WeaponX m_weapState DPT_Int leaf.
// Same walk/timing as the activity proxy above (cache populated, after DTExtend_RenameProps, before precalc).
static bool s_weapStateXlatProxyApplied = false;
static void DTExtend_ApplyWeapStateXlatProxy()
{
	if (s_weapStateXlatProxyApplied) return;
	s_weapStateXlatProxyApplied = true;

	int installed = 0;
	for (int t = 0; t < s_cachedSendTableCount; ++t)
	{
		uint8_t* table = reinterpret_cast<uint8_t*>(s_cachedSendTablePtrs[t]);
		if (!table || !ODP_IsReadable(table, 0x4C0 + 8)) continue;
		const char* tn = *reinterpret_cast<const char**>(table + ST_NETTABLENAME);
		if (ODP_StrLenSafe(tn, 64) < 0 || strcmp(tn, "DT_WeaponX") != 0) continue;
		uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
		const int nProps = *reinterpret_cast<int*>(table + ST_NPROPS);
		if (!props || nProps <= 0 || nProps > 4096) continue;
		if (!ODP_IsReadable(props, static_cast<size_t>(nProps) * SP_SIZE)) continue;

		for (int i = 0; i < nProps; ++i)
		{
			uint8_t* p = props + static_cast<uint64_t>(i) * SP_SIZE;
			if (*reinterpret_cast<int*>(p + SP_TYPE) != 0) continue; // DPT_Int only
			const char* nm = *reinterpret_cast<const char**>(p + SP_VARNAME);
			if (ODP_StrLenSafe(nm, 64) < 0 || strcmp(nm, "m_weapState") != 0) continue;
			*reinterpret_cast<uintptr_t*>(p + 0x60) = (uintptr_t)&WeapState_XlatProxy;
			++installed;
			Msg(eDLL_T::ENGINE, "[WEAPSTATE-XLAT] %s.m_weapState -> S3->S21 translate proxy installed\n",
				tn ? tn : "?");
		}
	}

	Msg(eDLL_T::ENGINE, "[WEAPSTATE-XLAT] comprehensive pass: installed %d weapon-state translate proxy(ies)\n",
		installed);

	// A zero install is indistinguishable from the feature working, so say so:
	// the client would keep reading raw S3 ordinals with the convar reading 1.
	if (installed == 0)
		Warning(eDLL_T::ENGINE,
			"[WEAPSTATE-XLAT] no DT_WeaponX.m_weapState prop found -- weapon-state "
			"ordinals stay RAW S3 on the wire regardless of bridge_weapstate_s21_ordinals\n");

	BridgeReady_Report("WeapState_XlatProxy", installed, 1);
}

//=============================================================================
// [COLGROUP-XLAT] S3->S21 m_CollisionGroup wire remap.
// Each script VM binds TRACE_COLLISION_GROUP_* from its own engine table, and
// S21 inserted groups that shift every index above DEBRIS_TRIGGER. m_CollisionGroup
// is a networked 6-bit DPT_Int, so it reaches the client as a raw S3 index.
//
//   group                        S3   S21
//   DEBRIS                        1     1
//   DEBRIS_TRIGGER                2     2
//   INTERACTIVE                   3     4
//   PLAYER                        6     7
//   BREAKABLE_GLASS               7     8
//   PLAYER_MOVEMENT               8     9
//   NPC                           9    10
//   NPC_MOVEMENT                 10    11
//   WEAPON                       13    14
//   PROJECTILE                   15    16
//   BLOCK_WEAPONS                19    21
//   BLOCK_WEAPONS_AND_PHYSICS    20    22
//=============================================================================
static ConVar bridge_colgroup_xlat("bridge_colgroup_xlat", "1", FCVAR_RELEASE,
	"Translate m_CollisionGroup from the S3 collision-group enum to S21's on the "
	"wire (S21 inserted groups, shifting PLAYER/NPC/WEAPON/PROJECTILE by 1 and "
	"BLOCK_WEAPONS by 2). 1 = ON. 0 = raw S3 index (client mis-classifies every "
	"script-placed collider -- e.g. Gibraltar's dome turns solid client-side).");

struct ColGroupPair_t
{
	int nS3;
	int nS21;
	const char* pszName;
};

// Name-matched anchors plus the two unnamed groups resolved by BEHAVIOUR.
// Indices with no entry pass through untouched and are reported once each -- guessing a uniform shift across the gaps would be inventing wire values. 17 (carrier CPhysicsProp) -> 18; 21 (carrier CTriggerNoZipline) -> 23: S3's matrix never sets bit 21 and 23 is S21's only no-collide group, so the picks are behaviour-unique and monotonic with their neighbours (16->17, 18->19 / 19->21, 20->22).
static const ColGroupPair_t s_colGroupMap[] = {
	{  1,  1, "DEBRIS"                    },
	{  2,  2, "DEBRIS_TRIGGER"            },
	{  3,  4, "INTERACTIVE"               },
	{  6,  7, "PLAYER"                    },
	{  7,  8, "BREAKABLE_GLASS"           },
	{  8,  9, "PLAYER_MOVEMENT"           },
	{  9, 10, "NPC"                       },
	{ 10, 11, "NPC_MOVEMENT"              },
	{ 13, 14, "WEAPON"                    },
	{ 15, 16, "PROJECTILE"                },
	{ 17, 18, "(unnamed, CPhysicsProp)"   },
	{ 19, 21, "BLOCK_WEAPONS"             },
	{ 20, 22, "BLOCK_WEAPONS_AND_PHYSICS" },
	{ 21, 23, "(unnamed, no-collide)"     },
};

// 6b prop, so the whole domain fits a 64-entry seen-mask for the one-shot report.
static uint64_t s_colGroupUnmappedSeen = 0;

// CBaseEntity::m_pServerClass -- same offset trigger_slip_diag reads.
// The ServerClass' network name is its first member (verified at the registration sites, e.g. "CBaseTrigger" / "CTriggerCylinderHeavy").
static constexpr ptrdiff_t ENT_OFF_SERVERCLASS = 0x50;

static const char* ColGroup_CarrierName(const void* const pEntity)
{
	if (!pEntity)
		return "?";

	__try
	{
		const void* const pServerClass =
			*reinterpret_cast<void* const*>(
				reinterpret_cast<const uint8_t*>(pEntity) + ENT_OFF_SERVERCLASS);
		if (!pServerClass)
			return "?";

		const char* const pszName = *reinterpret_cast<const char* const*>(pServerClass);
		return (pszName && ODP_StrLenSafe(pszName, 64) > 0) ? pszName : "?";
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return "?";
	}
}

static int CollisionGroup_S3ToS21(const int nGroup, const void* const pEntity)
{
	if (nGroup <= 0)
		return nGroup;

	for (const ColGroupPair_t& pair : s_colGroupMap)
	{
		if (pair.nS3 == nGroup)
			return pair.nS21;
	}

	if (nGroup < 64)
	{
		const uint64_t bit = 1ull << nGroup;
		if ((s_colGroupUnmappedSeen & bit) == 0)
		{
			s_colGroupUnmappedSeen |= bit;
			Warning(eDLL_T::ENGINE,
				"[COLGROUP-XLAT] no anchor for S3 collision group %d -- shipped raw "
				"(first carrier: %s). Map it if that entity mis-collides on the client.\n",
				nGroup, ColGroup_CarrierName(pEntity));
		}
	}

	return nGroup;
}

static void __fastcall CollisionGroup_XlatProxy(void* pProp, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	int v = 0;
	if (pStruct && pProp)
	{
		const int off = *(int*)((const char*)pProp + SP_OFFSET) & 0xFFFFF;
		v = *(const int*)((const char*)pStruct + off);
	}

	*(int*)pOut = bridge_colgroup_xlat.GetBool() ? CollisionGroup_S3ToS21(v, pStruct) : v;
}

//-----------------------------------------------------------------------------
// [COLGROUP-DUMP] Print the dedi's live collision-rules matrix.
// Unmapped indices (17 = CPhysicsProp, 21 = CTriggerNoZipline) have no group-name table in the image; the rules matrix is built at runtime by game-rules init.
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t COLRULES_TABLE_DISP = 0x804C; // byte disp inside the rules block
static constexpr int       COLRULES_MAX_GROUP  = 32;     // mask is 32b, so 32 groups max

static void CC_ColGroupDump(const CCommand& args)
{
	NOTE_UNUSED(args);

	// mov rax,[rip+rulesBasePtr] / mov edx,[rbx+3A4h] / mov rbx,[rsp+..] /
	// mov ecx,[rax+rdi*4+804Ch] / bt ecx,edx -- the collision test itself, so the
	// signature cannot drift onto an unrelated table read. 1 hit, verified.
	const CMemory hit = Module_FindPattern(g_GameDll,
		"48 8B 05 ?? ?? ?? ?? 8B 93 A4 03 00 00 48 8B 5C 24 ?? "
		"8B 8C B8 4C 80 00 00 0F A3 D1");

	if (!hit)
	{
		Warning(eDLL_T::SERVER, "[COLGROUP-DUMP] collision-rules lookup pattern unresolved\n");
		return;
	}

	uint8_t** const ppRulesBase =
		hit.ResolveRelativeAddress(3, 7).RCast<uint8_t**>();
	uint8_t* const pRulesBase = ppRulesBase ? *ppRulesBase : nullptr;

	if (!pRulesBase)
	{
		Warning(eDLL_T::SERVER,
			"[COLGROUP-DUMP] rules base is null -- run this after a map is loaded "
			"(the matrix is populated by game-rules init, not by the image)\n");
		return;
	}

	Msg(eDLL_T::SERVER, "[COLGROUP-DUMP] S3 collision matrix @ %p (bit N = trace group collides with entity group N)\n",
		reinterpret_cast<void*>(pRulesBase));

	for (int g = 0; g < COLRULES_MAX_GROUP; g++)
	{
		const uint32_t mask =
			*reinterpret_cast<const uint32_t*>(pRulesBase + g * 4 + COLRULES_TABLE_DISP);

		char bits[COLRULES_MAX_GROUP + 1];
		for (int b = 0; b < COLRULES_MAX_GROUP; b++)
			bits[b] = ((mask >> b) & 1) ? '1' : '0';
		bits[COLRULES_MAX_GROUP] = '\0';

		int n = 0;
		for (int b = 0; b < COLRULES_MAX_GROUP; b++)
			n += (mask >> b) & 1;

		// Name the anchors so the row is readable without cross-referencing.
		const char* pszName = "";
		for (const ColGroupPair_t& pair : s_colGroupMap)
		{
			if (pair.nS3 == g) { pszName = pair.pszName; break; }
		}

		Msg(eDLL_T::SERVER, "[COLGROUP-DUMP]  %2d %-28s %s  0x%08X  (%d)\n",
			g, pszName, bits, mask, n);
	}
}

static ConCommand sdk_colgroup_dump("sdk_colgroup_dump", CC_ColGroupDump,
	"Dump the dedi's live collision-rules matrix. Run with a map loaded; diff the "
	"rows against the S21 matrix to identify collision groups the anchor table "
	"does not name.", FCVAR_DEVELOPMENTONLY);

// m_CollisionGroup is a flat prop on many unrelated tables (DT_BaseEntity and
// every divergent class that redeclares it), so walk the whole cache like the
// camo pass rather than naming one table.
static bool s_colGroupXlatProxyApplied = false;
static void DTExtend_ApplyCollisionGroupXlatProxy()
{
	if (s_colGroupXlatProxyApplied) return;
	s_colGroupXlatProxyApplied = true;

	int installed = 0;
	for (int t = 0; t < s_cachedSendTableCount; ++t)
	{
		uint8_t* table = reinterpret_cast<uint8_t*>(s_cachedSendTablePtrs[t]);
		if (!table || !ODP_IsReadable(table, 0x4C0 + 8)) continue;
		const char* tn = *reinterpret_cast<const char**>(table + ST_NETTABLENAME);
		uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
		const int nProps = *reinterpret_cast<int*>(table + ST_NPROPS);
		if (!props || nProps <= 0 || nProps > 4096) continue;
		if (!ODP_IsReadable(props, static_cast<size_t>(nProps) * SP_SIZE)) continue;

		for (int i = 0; i < nProps; ++i)
		{
			uint8_t* p = props + static_cast<uint64_t>(i) * SP_SIZE;
			if (*reinterpret_cast<int*>(p + SP_TYPE) != 0) continue; // DPT_Int only
			const char* nm = *reinterpret_cast<const char**>(p + SP_VARNAME);
			if (ODP_StrLenSafe(nm, 64) < 0 || strcmp(nm, "m_CollisionGroup") != 0) continue;
			*reinterpret_cast<uintptr_t*>(p + 0x60) = (uintptr_t)&CollisionGroup_XlatProxy;
			++installed;
			Msg(eDLL_T::ENGINE,
				"[COLGROUP-XLAT] %s.m_CollisionGroup -> S3->S21 translate proxy installed\n",
				tn ? tn : "?");
		}
	}

	Msg(eDLL_T::ENGINE,
		"[COLGROUP-XLAT] comprehensive pass: installed %d collision-group translate proxy(ies)\n",
		installed);

	if (installed == 0)
		Warning(eDLL_T::ENGINE,
			"[COLGROUP-XLAT] no m_CollisionGroup prop found -- collision groups stay RAW "
			"S3 on the wire regardless of bridge_colgroup_xlat\n");

	BridgeReady_Report("CollisionGroup_XlatProxy", installed, 1);
}

//=============================================================================
// [ANIM-XLATE] S3->S21 m_playAnimationType wire remap.
// S3 networks CPlayer::m_playAnimationType (server +0x6914, int, 0..5) where 5 is grapple.
//=============================================================================
static ConVar bridge_anim_type_xlate("bridge_anim_type_xlate", "1", FCVAR_RELEASE,
	"S21 bridge: remap m_playAnimationType on the wire (S3 grapple=5 -> S21 grapple=6). "
	"The S21 client defines grapple as 6 and cannot clear a wire 5; without this every "
	"zipline/grapple ride storms prediction rebases. 0 = raw S3 value.");

static DTExtendProxyFn s_playAnimTypeOrigProxy = nullptr;

static void __fastcall PlayAnimType_XlateProxy(void* pProp, void* pStruct,
	void* pData, void* pOut, int iElement, int objectID)
{
	if (s_playAnimTypeOrigProxy)
		s_playAnimTypeOrigProxy(pProp, pStruct, pData, pOut, iElement, objectID);
	else if (pOut)
		*(int*)pOut = pData ? *(const int*)pData : 0;

	if (!pOut) return;
	if (bridge_anim_type_xlate.GetBool() && *(int*)pOut == 5)
		*(int*)pOut = 6;
}

// Distinguishes "original proxy not yet captured" from "original proxy was
// genuinely nullptr" so re-entry and the mismatch check below stay correct.
static bool s_playAnimTypeOrigCaptured = false;

// Recursive per-occurrence installer mirroring DTExtend_SetPropProxyInTree's tree-walk shape, but visiting EVERY "m_playAnimationType" match instead of stopping at the first: other classes (e.g.
// DT_PlayerDecoy -- Pathfinder decoys mimic grapple poses) may carry a same-named prop, and shared sub-table pointers can expose one prop object through several top-level trees (the proxy==ours check dedupes those naturally).
static int DTExtend_InstallPlayAnimTypeXlateInTree(uint8_t* table, int& nFound, int depth = 0)
{
	if (!table || depth > 32)
		return 0;

	uint8_t* props = *(uint8_t**)(table + ST_PROPS);
	const int nProps = *(const int*)(table + ST_NPROPS);
	if (!props || nProps <= 0 || nProps > 4096)
		return 0;

	int patched = 0;
	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* prop = props + (uint64_t)i * SP_SIZE;
		const char* name = *(const char**)(prop + SP_VARNAME);
		if (!name || strcmp(name, "m_playAnimationType") != 0)
			continue;

		++nFound;
		const char* tableName = *(const char**)(table + ST_NETTABLENAME);

		const int spType = *(const int*)(prop + SP_TYPE);
		if (spType != (int)SendPropType::DPT_Int)
		{
			Warning(eDLL_T::ENGINE,
				"[ANIM-XLATE] %s.m_playAnimationType has type=%d (expected DPT_Int) -- "
				"occurrence NOT armed\n", tableName ? tableName : "?", spType);
			continue;
		}

		const DTExtendProxyFn curProxy = *(DTExtendProxyFn*)(prop + 0x60);
		if (curProxy == &PlayAnimType_XlateProxy)
			continue; // already armed (re-entry on changelevel / shared sub-table)

		if (!s_playAnimTypeOrigCaptured)
		{
			s_playAnimTypeOrigProxy = curProxy;
			s_playAnimTypeOrigCaptured = true;
		}
		else if (curProxy != s_playAnimTypeOrigProxy)
		{
			// A second DISTINCT original proxy: one saved slot can't chain two
			// different originals -- skip this occurrence, loudly, once.
			static bool s_mismatchWarned = false;
			if (!s_mismatchWarned)
			{
				s_mismatchWarned = true;
				Warning(eDLL_T::ENGINE,
					"[ANIM-XLATE] %s.m_playAnimationType carries a DIFFERENT original proxy "
					"(%p vs saved %p) -- occurrence NOT armed (cannot chain two originals)\n",
					tableName ? tableName : "?", reinterpret_cast<void*>(curProxy),
					reinterpret_cast<void*>(s_playAnimTypeOrigProxy));
			}
			continue;
		}

		*(uintptr_t*)(prop + 0x60) = (uintptr_t)&PlayAnimType_XlateProxy;
		++patched;
	}

	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* prop = props + (uint64_t)i * SP_SIZE;
		if (*(const int*)(prop + SP_TYPE) != (int)SendPropType::DPT_DataTable)
			continue;
		uint8_t* child = *(uint8_t**)(prop + 0x70);
		patched += DTExtend_InstallPlayAnimTypeXlateInTree(child, nFound, depth + 1);
	}

	return patched;
}

// Install PlayAnimType_XlateProxy on EVERY m_playAnimationType SendProp across all top-level trees (the primary lives in DT_LocalPlayerExclusive under DT_Player).
// Re-entry safe (Hook_SendTable_Init can run again on changelevel) occurrences whose proxy is already ours are skipped, so a re-entry patches 0 and logs nothing.
static void DTExtend_InstallPlayAnimTypeXlate(void** tables, int count)
{
	if (!tables || count <= 0) return;

	int nFound = 0;
	int nPatched = 0;
	for (int t = 0; t < count; ++t)
	{
		uint8_t* topTable = reinterpret_cast<uint8_t*>(tables[t]);
		if (!topTable) continue;
		nPatched += DTExtend_InstallPlayAnimTypeXlateInTree(topTable, nFound);
	}

	if (nFound == 0)
	{
		Warning(eDLL_T::ENGINE,
			"[ANIM-XLATE] m_playAnimationType prop not found -- remap NOT armed\n");
		return;
	}

	if (nPatched > 0)
		Msg(eDLL_T::ENGINE,
			"[ANIM-XLATE] m_playAnimationType wire remap armed on %d prop(s) "
			"(S3 grapple=5 -> S21 grapple=6), origProxy=%p\n",
			nPatched, reinterpret_cast<void*>(s_playAnimTypeOrigProxy));
	// nFound > 0 && nPatched == 0: re-entry, everything already armed -- silent.
}

// [REGEN-WIRE] m_lastRegenTime is DPT_Time, and DPT_Time crosses this bridge as an ABSOLUTE stamp with no rebase at either end.
// The receiver's proxy adds gpGlobals+0x2C, which is CGlobalVarsBase::replayDelay -- zero outside replay playback.
static ConVar bridge_regen_stamp_wire("bridge_regen_stamp_wire", "1", FCVAR_RELEASE,
	"S21 bridge: send m_lastRegenTime as the inactive sentinel (-1) instead of the "
	"dedi's stale absolute stamp. The dedi never runs the ammo-regen FSM, so its raw "
	"value is an old stamp in the server's time epoch that the client cannot match. "
	"0 = raw S3 value.");

static void __fastcall RegenStamp_WireProxy(void* pProp, void* pStruct,
	void* pData, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut)
		return;

	float flValue = 0.f;
	if (pData)
	{
		flValue = *reinterpret_cast<const float*>(pData);
	}
	else if (pProp && pStruct)
	{
		const uint32_t off =
			*reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(pProp) + SP_OFFSET) & 0xFFFFF;
		flValue = *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(pStruct) + off);
	}

	if (bridge_regen_stamp_wire.GetBool())
		flValue = -1.0f;

	*reinterpret_cast<float*>(pOut) = flValue;
}

// Arms every DPT_Time m_lastRegenTime occurrence. Refuses (loudly) to touch one
// that already carries a proxy -- the S3 slot is verified NULL, so a non-null
// proxy means the layout moved and chaining blind would corrupt the value.
static int DTExtend_InstallRegenStampWireInTree(uint8_t* table, int& nFound, int depth = 0)
{
	if (!table || depth > 32)
		return 0;

	uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
	const int nProps = *reinterpret_cast<const int*>(table + ST_NPROPS);
	if (!props || nProps <= 0 || nProps > 4096)
		return 0;

	int patched = 0;
	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* prop = props + static_cast<uint64_t>(i) * SP_SIZE;
		const char* name = *reinterpret_cast<const char**>(prop + SP_VARNAME);
		if (!name || strcmp(name, "m_lastRegenTime") != 0)
			continue;

		++nFound;
		const char* tableName = *reinterpret_cast<const char**>(table + ST_NETTABLENAME);

		const int spType = *reinterpret_cast<const int*>(prop + SP_TYPE);
		if (spType != static_cast<int>(SendPropType::DPT_Time))
		{
			Warning(eDLL_T::ENGINE,
				"[REGEN-WIRE] %s.m_lastRegenTime has type=%d (expected DPT_Time=%d) -- NOT armed\n",
				tableName ? tableName : "?", spType, static_cast<int>(SendPropType::DPT_Time));
			continue;
		}

		const DTExtendProxyFn curProxy = *reinterpret_cast<DTExtendProxyFn*>(prop + 0x60);
		if (curProxy == &RegenStamp_WireProxy)
			continue; // re-entry on changelevel, or a shared sub-table pointer

		if (curProxy)
		{
			Warning(eDLL_T::ENGINE,
				"[REGEN-WIRE] %s.m_lastRegenTime already carries proxy %p -- NOT armed\n",
				tableName ? tableName : "?", reinterpret_cast<void*>(curProxy));
			continue;
		}

		*reinterpret_cast<uintptr_t*>(prop + 0x60) = reinterpret_cast<uintptr_t>(&RegenStamp_WireProxy);
		++patched;
		Msg(eDLL_T::ENGINE, "[REGEN-WIRE] armed on %s (wireOff=0x%X)\n",
			tableName ? tableName : "?",
			*reinterpret_cast<const uint32_t*>(prop + SP_OFFSET) & 0xFFFFF);
	}

	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* prop = props + static_cast<uint64_t>(i) * SP_SIZE;
		if (*reinterpret_cast<const int*>(prop + SP_TYPE) != static_cast<int>(SendPropType::DPT_DataTable))
			continue;
		uint8_t* child = *reinterpret_cast<uint8_t**>(prop + 0x70);
		patched += DTExtend_InstallRegenStampWireInTree(child, nFound, depth + 1);
	}

	return patched;
}

static void DTExtend_InstallRegenStampWire(void** tables, int count)
{
	if (!tables || count <= 0)
		return;

	int nFound = 0;
	int nPatched = 0;
	for (int t = 0; t < count; ++t)
	{
		uint8_t* topTable = reinterpret_cast<uint8_t*>(tables[t]);
		if (topTable)
			nPatched += DTExtend_InstallRegenStampWireInTree(topTable, nFound);
	}

	if (nFound == 0)
	{
		Warning(eDLL_T::ENGINE,
			"[REGEN-WIRE] m_lastRegenTime prop not found -- sentinel NOT armed\n");
		return;
	}

	if (nPatched > 0)
		Msg(eDLL_T::ENGINE, "[REGEN-WIRE] sentinel armed on %d of %d m_lastRegenTime prop(s)\n",
			nPatched, nFound);
	// nFound > 0 && nPatched == 0: re-entry, already armed -- silent.
}

// seComboVars: one packed int per status-effect slot. S3 stores X<<7 (endless
// |1); S21 expects X<<6. Wire-only remap so native S3 readers stay coherent.
static ConVar bridge_status_combo_wire("bridge_status_combo_wire", "1", FCVAR_RELEASE,
	"S21 bridge: repack seComboVars from the S3 layout (final <<7) to the S21 layout "
	"(final <<6) on the wire only. 0 = send the raw S3 value.");

static void __fastcall StatusCombo_WireProxy(void* /*pProp*/, void* /*pStruct*/,
	void* pData, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut)
		return;

	// SP_OFFSET is element-relative (0x8); without pData encode empty slot.
	if (!pData)
	{
		*reinterpret_cast<uint32_t*>(pOut) = 0;
		return;
	}

	const uint32_t in = *reinterpret_cast<const uint32_t*>(pData);
	uint32_t out = in;
	if (bridge_status_combo_wire.GetBool())
		out = ((in >> 7) << 6) | (in & 1u);
	*reinterpret_cast<uint32_t*>(pOut) = out;
}

// Arms every DPT_Int/32-bit seComboVars. Foreign non-null proxy = layout moved.
static int DTExtend_InstallStatusComboWireInTree(uint8_t* table, int& nFound, int depth = 0)
{
	if (!table || depth > 32)
		return 0;

	uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
	const int nProps = *reinterpret_cast<const int*>(table + ST_NPROPS);
	if (!props || nProps <= 0 || nProps > 4096)
		return 0;

	int patched = 0;
	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* prop = props + static_cast<uint64_t>(i) * SP_SIZE;
		const char* name = *reinterpret_cast<const char**>(prop + SP_VARNAME);
		if (!name || strcmp(name, "seComboVars") != 0)
			continue;

		++nFound;
		const char* tableName = *reinterpret_cast<const char**>(table + ST_NETTABLENAME);

		const int spType = *reinterpret_cast<const int*>(prop + SP_TYPE);
		const int nBits = *reinterpret_cast<const int*>(prop + SP_NBITS);
		if (spType != static_cast<int>(SendPropType::DPT_Int) || nBits != 32)
		{
			Warning(eDLL_T::ENGINE,
				"[SE-COMBO-WIRE] %s.seComboVars has type=%d nBits=%d (expected DPT_Int=%d nBits=32) -- NOT armed\n",
				tableName ? tableName : "?", spType, nBits, static_cast<int>(SendPropType::DPT_Int));
			continue;
		}

		const DTExtendProxyFn curProxy = *reinterpret_cast<DTExtendProxyFn*>(prop + 0x60);
		if (curProxy == &StatusCombo_WireProxy)
			continue; // re-entry on changelevel, or a shared sub-table pointer

		if (curProxy)
		{
			Warning(eDLL_T::ENGINE,
				"[SE-COMBO-WIRE] %s.seComboVars already carries proxy %p -- NOT armed\n",
				tableName ? tableName : "?", reinterpret_cast<void*>(curProxy));
			continue;
		}

		*reinterpret_cast<uintptr_t*>(prop + 0x60) = reinterpret_cast<uintptr_t>(&StatusCombo_WireProxy);
		++patched;
	}

	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* prop = props + static_cast<uint64_t>(i) * SP_SIZE;
		if (*reinterpret_cast<const int*>(prop + SP_TYPE) != static_cast<int>(SendPropType::DPT_DataTable))
			continue;
		uint8_t* child = *reinterpret_cast<uint8_t**>(prop + 0x70);
		patched += DTExtend_InstallStatusComboWireInTree(child, nFound, depth + 1);
	}

	return patched;
}

static void DTExtend_InstallStatusComboWire(void** tables, int count)
{
	if (!tables || count <= 0)
		return;

	int nFound = 0;
	int nPatched = 0;
	for (int t = 0; t < count; ++t)
	{
		uint8_t* topTable = reinterpret_cast<uint8_t*>(tables[t]);
		if (topTable)
			nPatched += DTExtend_InstallStatusComboWireInTree(topTable, nFound);
	}

	if (nFound == 0)
	{
		Warning(eDLL_T::ENGINE,
			"[SE-COMBO-WIRE] seComboVars prop not found -- remap NOT armed\n");
		return;
	}

	if (nPatched > 0)
		Msg(eDLL_T::ENGINE, "[SE-COMBO-WIRE] remap armed on %d of %d seComboVars prop(s)\n",
			nPatched, nFound);
	// nFound > 0 && nPatched == 0: re-entry, already armed -- silent.
}


// ===========================================================================
// WIRE-LINK AUDIT [LIVE, read-only] The wire carries no table pointers.
// A DPT_DataTable prop ships its child's NAME, and the client's SetupClientSendTableHierarchy re-links it by walking the tables it received and taking the FIRST case-insensitive match.
// ===========================================================================

// 0 = off (report only), 1 = rename structurally-distinct duplicates that are
// NOT SPROP_EXCLUDE targets, 2 = also rename exclude targets (changes which
// props survive the flatten -- symmetric on both sides, but it moves the wire).
static ConVar bridge_wire_name_uniquify("bridge_wire_name_uniquify", "2", FCVAR_RELEASE,
	"[WIRE-NAME] give every structurally-distinct transmitted SendTable its own "
	"net table name, so the client's name-based re-link rebuilds the graph we "
	"encode against. 0=off 1=safe 2=include exclude targets. "
	"Boot-read: use +bridge_wire_name_uniquify.");

static int DTExtend_WireUniquifyMode(void)
{
	const char* val = nullptr;
	if (CommandLine()->CheckParm("+bridge_wire_name_uniquify", &val) && val && val[0])
		return atoi(val);
	return bridge_wire_name_uniquify.GetInt();
}

namespace {
constexpr int kWireMaxTables = 4096;
constexpr int kWireMaxDupLog = 40;
constexpr int kWireMaxMisLog = 60;

struct WireTableEnt
{
	uint8_t*    table;
	const char* name;
	int         nProps;
	uint64_t    hash;     // deep structural hash (props + children)
	int         winner;   // index of the entry the client will bind this name to
	bool        isRoot;   // class root: transmitted first, needsDecoder=1
};
}

// Read a table's prop array under one SEH block. POD out-params only.
static bool WireReadTable(const uint8_t* table, const uint8_t** outProps, int* outN,
	const char** outName)
{
	if (!table)
		return false;
	__try
	{
		*outProps = *reinterpret_cast<const uint8_t* const*>(table + ST_PROPS);
		*outN     = *reinterpret_cast<const int*>(table + ST_NPROPS);
		*outName  = *reinterpret_cast<const char* const*>(table + ST_NETTABLENAME);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }

	return (*outN >= 0 && *outN <= 4096 && (*outN == 0 || *outProps != nullptr));
}

// FNV-1a 64. 32 bits would give ~1 collision in 2500 boots across the ~1850
// duplicate tables, and this hash decides whether a table gets RENAMED -- a
// collision there would silently leave a real divergence in place.
constexpr uint64_t kWireFnvPrime = 1099511628211ull;
constexpr uint64_t kWireFnvBasis = 14695981039346656037ull;

static uint64_t WireFnvStr(uint64_t h, const char* s)
{
	if (!s)
		return h * kWireFnvPrime;
	for (; *s; ++s)
		h = (h ^ static_cast<uint8_t>(*s)) * kWireFnvPrime;
	return h;
}

static uint64_t WireFnvInt(uint64_t h, int v)
{
	for (int b = 0; b < 4; ++b)
		h = (h ^ static_cast<uint8_t>((v >> (b * 8)) & 0xFF)) * kWireFnvPrime;
	return h;
}

// Structural hash of a table: its own props plus, recursively, its children.
// Two duplicate-named tables with the same hash are interchangeable on the wire, so a mislink between them is benign -- that is the whole point of computing it.
static uint64_t WireHashTable(const uint8_t* table, int depth)
{
	if (!table)
		return 0ull;
	if (depth > 32)
		return 0x9E3779B97F4A7C15ull;

	const uint8_t* props = nullptr;
	int nProps = 0;
	const char* name = nullptr;
	if (!WireReadTable(table, &props, &nProps, &name))
		return 0xDEADBEEFDEADBEEFull;

	// The table's OWN name is deliberately NOT hashed: the uniquify pass renames
	// tables, and a renamed copy must still compare equal to the copy it is
	// structurally identical to (otherwise a second boot would rename again).
	uint64_t h = WireFnvInt(kWireFnvBasis, nProps);

	for (int i = 0; i < nProps; ++i)
	{
		const uint8_t* p = props + static_cast<size_t>(i) * SP_SIZE;
		const char* pn = nullptr;
		int ty = -1, nb = -1, fl = 0, nel = 0;
		const uint8_t* child = nullptr;
		__try
		{
			pn    = *reinterpret_cast<const char* const*>(p + SP_VARNAME);
			ty    = *reinterpret_cast<const int*>(p + SP_TYPE);
			nb    = *reinterpret_cast<const int*>(p + SP_NBITS);
			fl    = *reinterpret_cast<const int*>(p + SP_FLAGS);
			nel   = *reinterpret_cast<const int*>(p + SP_NELEMENTS);
			child = (ty == 10)
				? *reinterpret_cast<const uint8_t* const*>(p + SP_CHILDTABLE)
				: nullptr;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return h * 16777619u; }

		h = WireFnvStr(h, pn);
		h = WireFnvInt(h, ty);
		h = WireFnvInt(h, nb);
		h = WireFnvInt(h, fl);
		h = WireFnvInt(h, nel);
		if (child)
			h = WireFnvInt(h, static_cast<int>(WireHashTable(child, depth + 1)));
	}
	return h;
}

static int WireFindTable(const WireTableEnt* ents, int n, const uint8_t* table)
{
	for (int i = 0; i < n; ++i)
	{
		if (ents[i].table == table)
			return i;
	}
	return -1;
}

// Append every DPT_DataTable child not already collected, pre-order, mirroring
// DataTable_MaybeWriteSendTableBuffer_R (mark, then recurse).
static void WireCollectChildren_R(WireTableEnt* ents, int* n, int cap,
	const uint8_t* table, int depth)
{
	if (depth > 32 || *n >= cap)
		return;

	const uint8_t* props = nullptr;
	int nProps = 0;
	const char* name = nullptr;
	if (!WireReadTable(table, &props, &nProps, &name))
		return;

	for (int i = 0; i < nProps && *n < cap; ++i)
	{
		const uint8_t* p = props + static_cast<size_t>(i) * SP_SIZE;
		uint8_t* child = nullptr;
		__try
		{
			if (*reinterpret_cast<const int*>(p + SP_TYPE) == 10)
				child = *reinterpret_cast<uint8_t* const*>(p + SP_CHILDTABLE);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }

		if (!child || WireFindTable(ents, *n, child) >= 0)
			continue;

		const uint8_t* cProps = nullptr;
		int cN = 0;
		const char* cName = nullptr;
		if (!WireReadTable(child, &cProps, &cN, &cName))
			continue;

		WireTableEnt& e = ents[(*n)++];
		e.table  = child;
		e.name   = cName;
		e.nProps = cN;
		e.hash   = 0;
		e.winner = -1;
		e.isRoot = false;

		WireCollectChildren_R(ents, n, cap, child, depth + 1);
	}
}

// Collect the transmitted table set in the engine's own write order: every class ROOT first (the writer's pass 1, needsDecoder=1), then sub-tables depth-first per root (pass 2) -- mirroring DataTable_WriteSendTablesBuffer. tables/count are the class root tables SendTable_Init was handed.
// Fills the structural hashes.
static int WireCollectTransmitted(WireTableEnt* ents, int cap, void** tables,
	int count, int* outRoots)
{
	int n = 0;

	for (int t = 0; t < count && n < cap; ++t)
	{
		uint8_t* root = reinterpret_cast<uint8_t*>(tables ? tables[t] : nullptr);
		if (!root || WireFindTable(ents, n, root) >= 0)
			continue;
		const uint8_t* props = nullptr;
		int nProps = 0;
		const char* name = nullptr;
		if (!WireReadTable(root, &props, &nProps, &name))
			continue;
		WireTableEnt& e = ents[n++];
		e.table = root; e.name = name; e.nProps = nProps;
		e.winner = -1; e.isRoot = true;
	}

	// The synthetic S21 wrappers are reachable only through the entity factory,
	// never through `tables`, but they ARE registered ServerClasses, so the
	// engine writes them as roots too.
	if (s_s21Slots)
	{
		for (int ci = 0; ci < kNumS21Classes && n < cap; ++ci)
		{
			uint8_t* w = s_s21Slots[ci].wrapperTable;
			const char* cn = nullptr;
			__try { cn = *reinterpret_cast<const char* const*>(
				s_s21Slots[ci].factory + FACT_CLASSNAME); }
			__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
			if (!cn || WireFindTable(ents, n, w) >= 0)
				continue;   // class skipped this boot
			const uint8_t* props = nullptr;
			int nProps = 0;
			const char* name = nullptr;
			if (!WireReadTable(w, &props, &nProps, &name))
				continue;
			WireTableEnt& e = ents[n++];
			e.table = w; e.name = name; e.nProps = nProps;
			e.winner = -1; e.isRoot = true;
		}
	}

	if (outRoots)
		*outRoots = n;

	const int nRoots = n;
	for (int i = 0; i < nRoots; ++i)
		WireCollectChildren_R(ents, &n, cap, ents[i].table, 0);

	for (int i = 0; i < n; ++i)
		ents[i].hash = WireHashTable(ents[i].table, 0);

	return n;
}

// Names the SDK itself resolves by name AFTER the uniquify pass runs.
// Class roots are protected unconditionally (the name is the class identity the client matches its RecvTable against); these are the sub-tables our own later passes look up.
static const char* const s_wireProtectedNames[] = {
	"DT_WeaponX_LocalWeaponData",   // DTExtend_FindTableByName, post-init
};

static bool WireIsProtectedName(const char* name)
{
	if (!name)
		return true;
	for (const char* p : s_wireProtectedNames)
	{
		if (_stricmp(p, name) == 0)
			return true;
	}
	return false;
}

// Every table name targeted by an SPROP_EXCLUDE prop anywhere in the graph.
// Exclusions are (tableName, propName) STRING pairs, so renaming a table that is an exclude target changes WHICH props survive the flatten -- symmetric on both sides, but it moves the wire.
static int WireCollectExcludeTargets(const WireTableEnt* ents, int n,
	const char** out, int cap)
{
	int nOut = 0;
	for (int i = 0; i < n && nOut < cap; ++i)
	{
		const uint8_t* props = nullptr;
		int nProps = 0;
		const char* tn = nullptr;
		if (!WireReadTable(ents[i].table, &props, &nProps, &tn))
			continue;
		for (int pi = 0; pi < nProps && nOut < cap; ++pi)
		{
			const uint8_t* p = props + static_cast<size_t>(pi) * SP_SIZE;
			const char* ex = nullptr;
			__try
			{
				if ((*reinterpret_cast<const int*>(p + SP_FLAGS) & 0x40) != 0 &&
					*reinterpret_cast<const int*>(p + SP_TYPE) != 10)
					ex = *reinterpret_cast<const char* const*>(p + SP_EXCLUDEDTNAME);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
			if (!ex)
				continue;
			bool seen = false;
			for (int k = 0; k < nOut && !seen; ++k)
			{
				if (_stricmp(out[k], ex) == 0)
					seen = true;
			}
			if (!seen)
				out[nOut++] = ex;
		}
	}
	return nOut;
}

static bool WireNameInList(const char* const* list, int n, const char* name)
{
	if (!name)
		return false;
	for (int i = 0; i < n; ++i)
	{
		if (list[i] && _stricmp(list[i], name) == 0)
			return true;
	}
	return false;
}

// Renamed table names must outlive every table, so this pool is never freed.
static char* s_wireNamePool = nullptr;
static size_t s_wireNamePoolUsed = 0;
static constexpr size_t kWireNamePoolSize = 32 * 1024;

static const char* WireAllocName(const char* base, int idx)
{
	if (!s_wireNamePool)
	{
		s_wireNamePool = static_cast<char*>(malloc(kWireNamePoolSize));
		s_wireNamePoolUsed = 0;
	}
	if (!s_wireNamePool || !base)
		return nullptr;

	char tmp[192];
	const int len = _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s_x%d", base, idx);
	if (len <= 0)
		return nullptr;
	const size_t need = static_cast<size_t>(len) + 1;
	if (s_wireNamePoolUsed + need > kWireNamePoolSize)
		return nullptr;

	char* out = s_wireNamePool + s_wireNamePoolUsed;
	memcpy(out, tmp, need);
	s_wireNamePoolUsed += need;
	return out;
}

// ---------------------------------------------------------------------------
// THE FIX: give every structurally-distinct table its own net table name.
// A duplicate name is only a defect when the copies DIFFER -- the client's flatten of two identical copies is identical, so those stay shared.
// ---------------------------------------------------------------------------
static bool s_wireNamesUniquified = false;

static void DTExtend_UniquifyTransmittedTableNames(void** tables, int count)
{
	if (s_wireNamesUniquified)
		return;
	s_wireNamesUniquified = true;

	const int mode = DTExtend_WireUniquifyMode();
	if (mode <= 0)
	{
		// Say so: "no [WIRE-NAME] lines" must never be ambiguous between OFF and
		// NOT BUILT, and the compensation interlock keys off this same mode.
		Msg(eDLL_T::ENGINE,
			"[WIRE-NAME] OFF (bridge_wire_name_uniquify 0) -- duplicate table "
			"names left as-is, flat-order compensations stay ACTIVE. "
			"+bridge_wire_name_uniquify 1 to fix the graph instead\n");
		return;
	}

	WireTableEnt* ents = static_cast<WireTableEnt*>(
		calloc(kWireMaxTables, sizeof(WireTableEnt)));
	if (!ents)
	{
		Warning(eDLL_T::ENGINE, "[WIRE-NAME] alloc failed -- uniquify skipped\n");
		return;
	}

	int nRoots = 0;
	const int n = WireCollectTransmitted(ents, kWireMaxTables, tables, count, &nRoots);

	constexpr int kMaxExcludeTargets = 256;
	const char* exclTargets[kMaxExcludeTargets] = {};
	const int nExclTargets =
		WireCollectExcludeTargets(ents, n, exclTargets, kMaxExcludeTargets);

	int renamed = 0, blockedExclude = 0, blockedRoot = 0, blockedProtected = 0;
	int poolFail = 0, groups = 0;

	for (int i = 0; i < n; ++i)
	{
		if (ents[i].winner >= 0)
			continue;

		// Retain the name on the class root if the group has one -- its name is
		// the class identity the client matches its RecvTable against.
		int keep = i;
		for (int j = i; j < n; ++j)
		{
			if (j != i && (!ents[i].name || !ents[j].name ||
				_stricmp(ents[i].name, ents[j].name) != 0))
				continue;
			ents[j].winner = i;
			if (ents[j].isRoot && !ents[keep].isRoot)
				keep = j;
		}

		int members = 0;
		for (int j = i; j < n; ++j)
		{
			if (ents[j].winner == i)
				++members;
		}
		if (members <= 1)
			continue;
		++groups;

		const char* groupName = ents[keep].name;
		const bool isExcludeTarget =
			WireNameInList(exclTargets, nExclTargets, groupName);
		const bool isProtected = WireIsProtectedName(groupName);

		for (int j = i; j < n; ++j)
		{
			if (ents[j].winner != i || j == keep)
				continue;
			if (ents[j].hash == ents[keep].hash)
				continue;   // interchangeable copy -- sharing the name is harmless

			if (ents[j].isRoot)
			{
				++blockedRoot;
				Warning(eDLL_T::ENGINE,
					"[WIRE-NAME] BLOCKED '%s': two CLASS ROOTS share the name and "
					"differ (%p vs %p) -- neither can be renamed\n",
					groupName ? groupName : "?",
					reinterpret_cast<void*>(ents[keep].table),
					reinterpret_cast<void*>(ents[j].table));
				continue;
			}
			if (isProtected)
			{
				++blockedProtected;
				Warning(eDLL_T::ENGINE,
					"[WIRE-NAME] BLOCKED '%s': the SDK resolves this name after "
					"the pass (s_wireProtectedNames)\n", groupName ? groupName : "?");
				continue;
			}
			if (isExcludeTarget && mode < 2)
			{
				++blockedExclude;
				Warning(eDLL_T::ENGINE,
					"[WIRE-NAME] BLOCKED '%s': SPROP_EXCLUDE target -- renaming "
					"changes which props survive the flatten. +bridge_wire_name_"
					"uniquify 2 to rename anyway\n", groupName ? groupName : "?");
				continue;
			}

			const char* newName = WireAllocName(groupName, ++renamed);
			if (!newName)
			{
				++poolFail;
				--renamed;
				continue;
			}

			bool ok = false;
			__try
			{
				// The name and nothing else. Everything the engine keys off --
				// the props, the child pointers, the list link at +0x508 -- stays
				// exactly as it was; only what we TRANSMIT for this node changes.
				*reinterpret_cast<const char**>(ents[j].table + ST_NETTABLENAME) = newName;
				ok = true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }

			if (!ok)
			{
				--renamed;
				Warning(eDLL_T::ENGINE,
					"[WIRE-NAME] FAULT renaming '%s' at %p -- left as-is\n",
					groupName ? groupName : "?",
					reinterpret_cast<void*>(ents[j].table));
				continue;
			}

			ents[j].name = newName;
		}
	}

	Warning(eDLL_T::ENGINE,
		"[WIRE-NAME] uniquify mode=%d: tables=%d dupGroups=%d exclTargets=%d "
		"renamed=%d blocked(root=%d protected=%d exclude=%d) poolFail=%d "
		"pool=%zu/%zu\n",
		mode, n, groups, nExclTargets, renamed, blockedRoot, blockedProtected,
		blockedExclude, poolFail, s_wireNamePoolUsed, kWireNamePoolSize);

	free(ents);
}

// ---------------------------------------------------------------------------
// [TREE] one-shot nested dump of a transmitted table's tree.
// The flat dumps ([FLATN-DUMP] / [RECV-FLAT]) show the RESULT of flattening; this shows the INPUT.
// ---------------------------------------------------------------------------
static ConVar bridge_dump_tree("bridge_dump_tree", "0", FCVAR_DEVELOPMENTONLY,
	"[TREE] one-shot nested dump of these SendTable trees at SendTable_Init "
	"(comma-separated names, 0 = off). Boot-read: use +bridge_dump_tree.");

static int s_treeLinesLeft = 0;

static void DTExtend_DumpTableTree_R(const uint8_t* table, int depth)
{
	if (!table || depth > 16 || s_treeLinesLeft <= 0)
		return;

	const uint8_t* props = nullptr;
	int nProps = 0;
	const char* name = nullptr;
	if (!WireReadTable(table, &props, &nProps, &name))
		return;

	static const char* const kPad = "                                ";
	const int pad = (depth * 2 < 32) ? depth * 2 : 32;

	Warning(eDLL_T::ENGINE, "[TREE] %.*s%s (nProps=%d) %p\n",
		pad, kPad, name ? name : "<null>", nProps,
		reinterpret_cast<const void*>(table));
	--s_treeLinesLeft;

	for (int i = 0; i < nProps && s_treeLinesLeft > 0; ++i)
	{
		const uint8_t* p = props + static_cast<size_t>(i) * SP_SIZE;
		const char* pn = nullptr;
		int ty = -1, nb = -1, fl = 0, nel = 0, pri = -1;
		const uint8_t* child = nullptr;
		__try
		{
			pn  = *reinterpret_cast<const char* const*>(p + SP_VARNAME);
			ty  = *reinterpret_cast<const int*>(p + SP_TYPE);
			nb  = *reinterpret_cast<const int*>(p + SP_NBITS);
			fl  = *reinterpret_cast<const int*>(p + SP_FLAGS);
			nel = *reinterpret_cast<const int*>(p + SP_NELEMENTS);
			// The flatten sorts by priority, so a tree without it cannot predict
			// the flat. Synthesized props inherit the donor template's byte here,
			// which is how our array elements end up scattered.
			pri = *reinterpret_cast<const uint8_t*>(p + SP_PRIORITY);
			child = (ty == 10)
				? *reinterpret_cast<const uint8_t* const*>(p + SP_CHILDTABLE)
				: nullptr;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }

		const char* cn = nullptr;
		if (child)
		{
			__try { cn = *reinterpret_cast<const char* const*>(child + ST_NETTABLENAME); }
			__except (EXCEPTION_EXECUTE_HANDLER) { cn = "<fault>"; }
		}

		Warning(eDLL_T::ENGINE,
			"[TREE] %.*s  [%3d] ty=%-2d nBits=%-4d fl=0x%-4X nEl=%-3d pri=%-3d %s%s%s\n",
			pad, kPad, i, ty, nb, fl, nel, pri, pn ? pn : "<null>",
			cn ? " -> " : "", cn ? cn : "");
		--s_treeLinesLeft;

		if (child)
			DTExtend_DumpTableTree_R(child, depth + 1);
	}
}

static void DTExtend_DumpRequestedTrees(void** tables, int count)
{
	const char* cfg = nullptr;
	if (!(CommandLine()->CheckParm("+bridge_dump_tree", &cfg) && cfg && cfg[0]))
		cfg = bridge_dump_tree.GetString();
	if (!cfg || !cfg[0] || strcmp(cfg, "0") == 0)
		return;

	WireTableEnt* ents = static_cast<WireTableEnt*>(
		calloc(kWireMaxTables, sizeof(WireTableEnt)));
	if (!ents)
		return;

	int nRoots = 0;
	const int n = WireCollectTransmitted(ents, kWireMaxTables, tables, count, &nRoots);

	const char* p = cfg;
	while (*p)
	{
		while (*p == ' ' || *p == ',')
			++p;
		if (!*p)
			break;
		const char* start = p;
		while (*p && *p != ',' && *p != ' ')
			++p;
		const size_t len = static_cast<size_t>(p - start);
		if (!len || len >= 128)
			continue;

		char want[128];
		memcpy(want, start, len);
		want[len] = '\0';

		bool found = false;
		for (int i = 0; i < n; ++i)
		{
			if (!ents[i].name || _stricmp(ents[i].name, want) != 0)
				continue;
			found = true;
			s_treeLinesLeft = 4000;
			Warning(eDLL_T::ENGINE, "[TREE] === %s ===\n", want);
			DTExtend_DumpTableTree_R(ents[i].table, 0);
			Warning(eDLL_T::ENGINE, "[TREE] === end %s ===\n", want);
			break;
		}
		if (!found)
			Warning(eDLL_T::ENGINE, "[TREE] '%s' not in the transmitted set\n", want);
	}

	free(ents);
}

// ===========================================================================
// SHARED UTIL: FindCleanTemplateProp
// ===========================================================================
const uint8_t* DTExtend_FindCleanTemplateProp(int type);

const uint8_t* DTExtend_FindCleanTemplateProp(int type)
{
	for (int t = 0; t < s_cachedSendTableCount; ++t)
	{
		uint8_t* table = reinterpret_cast<uint8_t*>(s_cachedSendTablePtrs[t]);
		if (!table)
			continue;
		uint8_t* props = nullptr;
		int nProps = 0;
		__try
		{
			props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
			nProps = *reinterpret_cast<int*>(table + ST_NPROPS);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
		if (!props || nProps <= 0 || nProps > 4096)
			continue;
		for (int i = 0; i < nProps; ++i)
		{
			const uint8_t* prop = props + static_cast<uint64_t>(i) * SP_SIZE;
			if (*reinterpret_cast<const int*>(prop + SP_TYPE) == type &&
				!(*reinterpret_cast<const int*>(prop + SP_FLAGS) & 0x40))
				return prop;
		}
	}
	return nullptr;
}
// ===========================================================================
// SYSTEM 14 (legacy graft): dt_extend_system14.cpp

// ===========================================================================
// SYSTEM 15: ORCHESTRATOR + VDTExtend DETOUR
// ===========================================================================

// Bisection dump of DT_WORLD m_deathField* parents + child0 after structural
// passes. pszWhen names the stage (post-build / pre-init / post-init).
static ConVar sdk_deathfield_tree_dump("sdk_deathfield_tree_dump", "0",
	FCVAR_DEVELOPMENTONLY,
	"Dump the DT_WORLD m_deathField* parents and their first child at each "
	"structural stage (post-build / pre-init / post-init).");

static void DTExtend_DeathFieldTreeDump(const char* pszWhen)
{
	if (!sdk_deathfield_tree_dump.GetBool())
		return;

	const char* const when = pszWhen ? pszWhen : "?";
	static const char* const s_dfTreeNames[6] = {
		"m_deathFieldIsActive",
		"m_deathFieldOrigin",
		"m_deathFieldRadiusStart",
		"m_deathFieldRadiusEnd",
		"m_deathFieldTimeStart",
		"m_deathFieldTimeEnd",
	};

	const uintptr_t tableU = CanonS3TableFind("DT_WORLD");
	uint8_t* const table = tableU ? reinterpret_cast<uint8_t*>(tableU) : nullptr;
	uint8_t* const treeProps = table ? *reinterpret_cast<uint8_t**>(table + ST_PROPS) : nullptr;
	int treeNProps = table ? *reinterpret_cast<int*>(table + ST_NPROPS) : 0;
	if (treeNProps < 0)
		treeNProps = 0;
	if (treeNProps > 4096)
		treeNProps = 4096;

	for (int ni = 0; ni < 6; ++ni)
	{
		const char* const want = s_dfTreeNames[ni];
		int found = 0, type = 0, off = 0, subProps = -1;
		uint8_t* sub = nullptr;
		const char* subName = "";
		const char* child0Name = "";
		int child0Off = 0;
		void* child0Proxy = nullptr;

		if (treeProps)
		{
			for (int i = 0; i < treeNProps; ++i)
			{
				uint8_t* const p = treeProps + static_cast<size_t>(i) * SP_SIZE;
				const char* const nm = *reinterpret_cast<const char* const*>(p + SP_VARNAME);
				if (!nm || strcmp(nm, want) != 0)
					continue;
				found = 1;
				type = *reinterpret_cast<int*>(p + SP_TYPE);
				off = *reinterpret_cast<int*>(p + SP_OFFSET) & 0xFFFFF;
				sub = *reinterpret_cast<uint8_t**>(p + SP_CHILDTABLE);
				if (sub)
				{
					const char* const sn = *reinterpret_cast<const char* const*>(sub + ST_NETTABLENAME);
					subName = sn ? sn : "";
					subProps = *reinterpret_cast<int*>(sub + ST_NPROPS);
					uint8_t* const subPropArr = *reinterpret_cast<uint8_t**>(sub + ST_PROPS);
					if (subPropArr && subProps > 0)
					{
						uint8_t* const c0 = subPropArr;
						const char* const c0n = *reinterpret_cast<const char* const*>(c0 + SP_VARNAME);
						child0Name = c0n ? c0n : "";
						child0Off = *reinterpret_cast<int*>(c0 + SP_OFFSET) & 0xFFFFF;
						child0Proxy = *reinterpret_cast<void**>(c0 + 0x60);
					}
				}
				break;
			}
		}

		Warning(eDLL_T::ENGINE,
			"[DEATHFIELD-TREE] %s '%s' found=%d type=%d off=%d sub=%p subName='%s' "
			"subProps=%d child0='%s' child0Off=%d child0Proxy=%p\n",
			when, want, found, type, off, sub, subName, subProps,
			child0Name, child0Off, child0Proxy);
	}
}

// Post-flatten census: deathfield children in the encoder flat array.
static void DTExtend_DeathFieldFlatAndPackProbe(void)
{
	const uintptr_t tableU = CanonS3TableFind("DT_WORLD");
	if (!tableU)
	{
		Warning(eDLL_T::ENGINE,
			"[DEATHFIELD-FLAT] table=(null) precalc=(null) flatCount=0 dfChildren=0\n");
		return;
	}

	uint8_t* const table = reinterpret_cast<uint8_t*>(tableU);

	DTExtend_DeathFieldTreeDump("post-init");

	uint8_t* const precalc = *reinterpret_cast<uint8_t**>(table + 0x4C0);
	if (!precalc)
	{
		Warning(eDLL_T::ENGINE,
			"[DEATHFIELD-FLAT] table=%p precalc=(null) flatCount=0 dfChildren=0\n",
			table);
		return;
	}

	void** const flatArr = *reinterpret_cast<void***>(precalc + 0x08);
	int flatCount = *reinterpret_cast<int*>(precalc + 0x10);
	if (flatCount < 0)
		flatCount = 0;
	if (flatCount > 8192)
		flatCount = 8192;

	if (!flatArr)
	{
		Warning(eDLL_T::ENGINE,
			"[DEATHFIELD-FLAT] table=%p precalc=%p flatCount=%d dfChildren=0\n",
			table, precalc, flatCount);
		return;
	}

	int dfChildren = 0;
	for (int i = 0; i < flatCount; ++i)
	{
		uint8_t* const p = reinterpret_cast<uint8_t*>(flatArr[i]);
		if (!p)
			continue;
		const int off = *reinterpret_cast<int*>(p + SP_OFFSET) & 0xFFFFF;
		if (off >= DF_CHILD_BASE && off < DF_CHILD_BASE + 2048)
			++dfChildren;
	}

	Warning(eDLL_T::ENGINE,
		"[DEATHFIELD-FLAT] table=%p precalc=%p flatCount=%d dfChildren=%d\n",
		table, precalc, flatCount, dfChildren);

	// Census by per-ring proxy identity -- survives offset rewrites.
	int byIsActive = 0, byOrigin = 0, byRadStart = 0, byRadEnd = 0, byTimeStart = 0, byTimeEnd = 0;
	for (int i = 0; i < flatCount; ++i)
	{
		uint8_t* const p = reinterpret_cast<uint8_t*>(flatArr[i]);
		if (!p)
			continue;
		void* const proxy = *reinterpret_cast<void**>(p + 0x60);
		if (!proxy)
			continue;
		bool matched = false;
		for (int k = 0; k < 64 && !matched; ++k)
		{
			if (proxy == DeathField_GetRingProxy(DF_ISACTIVE, k))
			{ ++byIsActive; matched = true; }
			else if (proxy == DeathField_GetRingProxy(DF_ORIGIN, k))
			{ ++byOrigin; matched = true; }
			else if (proxy == DeathField_GetRingProxy(DF_RADSTART, k))
			{ ++byRadStart; matched = true; }
			else if (proxy == DeathField_GetRingProxy(DF_RADEND, k))
			{ ++byRadEnd; matched = true; }
			else if (proxy == DeathField_GetRingProxy(DF_TIMESTART, k))
			{ ++byTimeStart; matched = true; }
			else if (proxy == DeathField_GetRingProxy(DF_TIMEEND, k))
			{ ++byTimeEnd; matched = true; }
		}
	}
	Warning(eDLL_T::ENGINE,
		"[DEATHFIELD-FLAT] byProxy isActive=%d origin=%d radStart=%d radEnd=%d timeStart=%d timeEnd=%d\n",
		byIsActive, byOrigin, byRadStart, byRadEnd, byTimeStart, byTimeEnd);
}

static char Hook_SendTable_Init(void** tables, int count)
{
	if (tables && count > 0)
	{
		// Recursive walk: visit every top-level entry AND every DPT_DataTable sub-table child reachable from it.
		// SendTable_Init only receives top-level class tables (~292 entries); the full tree (including sub-tables like DT_ServerAnimationData, DT_LocalPlayerExclusive, DT_CollisionProperty, DT_SequenceTransitionerLayer) requires this walk to be addressable by name via DTExtend_FindTableByName.
		s_cachedSendTableCount = 0;
		for (int t = 0; t < count; ++t)
		{
			uint8_t* topTable = reinterpret_cast<uint8_t*>(tables[t]);
			if (!topTable) continue;
			DTExtend_CacheRecurse(topTable, 0);
		}
		Msg(eDLL_T::ENGINE,
			"[DT-CACHE] populated %d SendTables (recursive; %d top-level seeds)\n",
			s_cachedSendTableCount, count);
	}
	if (!s_applied)
	{
		s_applied = true;
		Msg(eDLL_T::ENGINE, "[BRIDGE-DT] SendTable rebuild path = LEGACY\n");
		// Legacy: append S21-new props to the S3 SendTables.
		DTExtend_Apply(tables, count);
		// Relocate top-level SendProps into named sub-tables so the matcher pairs them with S21 RecvProps that live one level deeper.
		// Source entity offset + bit count preserved; only the wire-side serialization scope changes.
		DTExtend_RelocateProps();
		// Bulk additive cross-scope copies (143 entries, auto-discover source).
		// Adds wire-side prop emissions inside named sub-tables so the matcher pairs them on classes that DERIVE the destination -- without removing source-side emissions (which still match for classes that don't derive dest).
		DTExtend_CopyProps();
		// Apply data-driven SPROP_* flag overrides on existing S3 props (e.g.
		// DT_PlayerDecoy.m_currentClass=0x1, m_localAngles=0x208).
		DTExtend_ApplyFlagOverrides();
		// [INF-AMMO-WIRE] PARKED: clip/stockpile value proxies do not fix infinite ammo (state=1 reaches the client; clip still mispredicts), and offset mistakes regress normal reload.
		// Leave stock SendProps alone; m_infiniteAmmoState proxy (appended) stays; enforcement is gated by bridge_infinite_ammo (default 0).
		DTExtend_RenameProps();
		// [PROP-DROP] Structural sibling of the rename pass: splice S3-only props
		// S21 removed (DT_Team reserved/connecting/loading counts). Same timing
		// contract: before flag/bit-width overrides and the precalc.
		DTExtend_DropProps();
		// Widen narrow index fields that the ported S21 content overflows script-particle m_effectIndex 11->12 bits so ParticleEffectNames indices >= 2048 stop truncating.
		// Same timing as the flag overrides (before precalc) so the wire info field + snapshot encode honor it.
		DTExtend_ApplyBitWidthOverrides();
		// predictableFlags 6-slot set/clear immediates -> 8-slot (pairs with the
		// DT_Local.predictableFlags nBits widen above).
		DTExtend_ApplyPFlagsSlots8();
		// m_modInventory / m_consumableInventory: retype+widen the native S3 array sub-tables in place (Int32->Int64 for mods, 16bit->32bit Int for consumables) + install zero-extend proxies.
		// These native sub-tables are already in their raw, walkable shape on the legacy path, so the tree-walk runs unchanged here.
		DTExtend_RebuildModInventory(tables, count);
		DTExtend_RebuildConsumableInventory(tables, count);
		// Comprehensive model-index re-widen across ALL cached SendTables: covers the bridge-synth s_s21Classes (trigger cohort, loot grabber, env decoy, care-package prop) whose independent 13b model-index props the per-class override never named ([FLATN-SCAN] found 9 across 6 classes).
		// The cache holds the synths here; the reparent in DTExtend_InsertWrapperTables below then links DT_TriggerCylinderHeavy to the already-widened synth chain.
		DTExtend_RewidenAllModelIndexProps();
		// Comprehensive effect-index re-widen (twin of the model-index pass) -- catches any *EffectIndex* SendProp the 2-entry s_bitWidthOverrides + assumed-inheritance missed (e.g.
		// DT_TEScriptParticleSystemOnEntityWithPos as an independent synth copy), so no ParticleEffectNames index >= 2048 truncates.
		DTExtend_RewidenAllEffectIndexProps();
		// Translate m_camoIndex's S3 '-1 = no camo' sentinel to S21's '0 = no camo' on the wire across every carrier (DT_BaseAnimating, DT_PropSurvival, lightweight/ future).
		// Fixes the camo OOB AV at the authoritative encode boundary so the S21 client never receives -1.
		DTExtend_ApplyCamoSentinelProxy();
		// [WEAP-ACT-XLAT] Translate m_idealActivity's S3 activity ID to its S21 equivalent at the wire-encode boundary (only meaningful once bridge_weap_ideal_bind has renamed the S3 prop into the S21 name).
		// Reuses the activity translation map.
		DTExtend_ApplyWeapIdealActivityXlatProxy();
		// [WEAPSTATE-XLAT] Translate DT_WeaponX.m_weapState from S3's 22-state enum to S21's 25-state one at the same wire-encode boundary (S21 inserted ENERGIZE at 8 and COOLDOWN_OVERHEAT at 22).
		// Sender-side by design -- every client-side attempt degenerated into a compare mask on a predicted field.
		DTExtend_ApplyWeapStateXlatProxy();
		// [COLGROUP-XLAT] Translate m_CollisionGroup from the S3 collision-group enum to S21's at the same wire-encode boundary (S21 inserted groups, shifting PLAYER/BREAKABLE_GLASS/NPC/WEAPON/PROJECTILE by 1 and BLOCK_WEAPONS by 2).
		// The dedi keeps its own index for local collision; only the wire value moves.
		DTExtend_ApplyCollisionGroupXlatProxy();
		// Insert missing inheritance levels (synthesized wrapper SendTables).
		// For chain-mismatch divergent classes where S21 added a level the dedi's S3 chain lacks (e.g.
		DTExtend_InsertWrapperTables();
		// Grow m_flPoseParameter 12->24 and bind 12..23 before precalc. New
		// elements are pre-bracketed so Bracketize leaves them alone.
		DTExtend_GrowPoseParamArray();
		// Bracketize numerically-named sub-table children (`000N` -> `[000N]`) so the S21 client's name-based matcher pairs them with native RecvProps.
		// Without this, inventory arrays (weapons / offhand / active) and any other numerically-named children stay `?_unmatched` on the client and their values never reach S21 entity bytes via DT.
		DTExtend_BracketizeSubTableChildren(tables, count);
		// Extend DT_WeaponInventory.offhandWeapons 6 -> 8 BEFORE the engine builds precalcs so the flat list comes out with 8 entries.
		// The extender's clone-source (prop[5]) is now already bracketed by the rename pass above, so new entries 6,7 inherit the right name shape without per-element string fixup in the extender body.
		DTExtend_ExtendOffhandWeaponsTo8(tables, count);
		// Deathfield realms: rebuild DT_WORLD's m_deathField* scalars into the
		// S21 DataTable[64] arrays sourced from s_deathFields (ConVar-gated,
		// sdk_deathfield_native_dt, default on).
		DTExtend_BuildDeathFieldArrays(tables, count);
		DTExtend_DeathFieldTreeDump("post-build");
		// DT_GlobalNonRewinding.m_playerMiscData: N x DT_NonRewindMiscData
		// (ConVar-gated sdk_nonrewind_misc_dt, default off). After DeathField so
		// the parentTable fix-up pass below covers the new props.
		DTExtend_BuildNonRewindMiscArray(tables, count);
		// DT_Player.connectionQualityIndex -> DT_ConnectionQualityIndex
		// (ConVar-gated sdk_conn_quality_dt, default on).
		DTExtend_BuildConnectionQualityIndex(tables, count);

		// S21 wrapper deep-clone.
		// MUST be the last structural pass: every wrapper is a snapshot of a native tree, and the S21 client resolves a DataTable prop's child BY TABLE NAME over the tables it received, so it binds the NATIVE table of that name -- not our copy.
		DTExtend_PostApplyS21Overrides(tables, count);

// [PARENT-FIX-FINAL] the ONE authoritative global parentTable pass.
// Every structural pass above (PostApplyS21Overrides / Relocate / Copy / InsertWrapper / Bracketize / Offhand / DeathField) memcpy's SendProps or clones foreign templates, leaving +0x08 pointing at a foreign table; the encoder then resolves structBase from a foreign class index (the changelevel pack memmove-AV class).
		if (bridge_legacy_fix_parenttable.GetBool() && tables && count > 0)
		{
			int seedsWalked = 0;
			for (int t = 0; t < count; ++t)
			{
				if (!tables[t]) continue;
				DTExtend_FixParentTablesInTree(reinterpret_cast<uint8_t*>(tables[t]));
				++seedsWalked;
			}
			const int cachedWalked = s_cachedSendTableCount;
			for (int t = 0; t < s_cachedSendTableCount; ++t)
				DTExtend_FixParentTablesInTree(
					reinterpret_cast<uint8_t*>(s_cachedSendTablePtrs[t]));
			Warning(eDLL_T::ENGINE,
				"[dt_extend] [PARENT-FIX-FINAL] walked %d seed trees + %d cached "
				"tables (post Relocate/Copy/Wrapper/Bracketize/Offhand/DeathField, "
				"pre-precalc)\n", seedsWalked, cachedWalked);
		}
	}

	// [WIRE-NAME] LAST structural act before the engine flattens.
	// The client re-links our transmitted graph BY NAME, so a name owned by two differing tables makes it flatten a tree we never encode against.
	DTExtend_UniquifyTransmittedTableNames(tables, count);

	// [SE-COMBO-WIRE] Must be armed BEFORE the precalc build: the flatten takes its own copy of each SendProp, so a proxy installed in the common tail below lands only on the tree prop the encoder no longer reads (armed 87/87, zero encodes).
	// The install is idempotent, so the tail call still catches anything built later.
	DTExtend_InstallStatusComboWire(tables, count);
	DTExtend_InstallPlayAnimTypeXlate(tables, count);
	DTExtend_InstallRegenStampWire(tables, count);
	AnimAnchorProxy_Install(tables, count);

	// Last word before the flatten: every entity-relative prop, whoever authored it, has to fit inside the allocation its class actually gets.
	// The pack path memmoves from entity + offset with no bounds check, so anything named here ships adjacent heap to clients and access-violates at a segment edge.
	DTExtend_AuditAllClassBacking();

	DTExtend_DeathFieldTreeDump("pre-init");
	const char stInitRet = v_SendTable_Init(tables, count);

	// [ZIPRAIL-PACK] The synthetic CZiprail wrapper is never in tables (it's only reachable via the entity factory), so the engine's precalc build above never touches it -- its precalc stays NULL forever, which packs every promoted entity AND the class instancebaseline as header-only (zero fields), so the S21 client never creates the entity.
	// Build the wrapper its OWN precalc from its OWN complete deep-clone tree (the same per-table flatten SendTable_Init runs), so ALL props pack -- base origin/ cell + m_ziplineRestPositions.
	DTExtend_ZiprailBuildOwnPrecalc();
	DTExtend_DeathFieldReinstallPostInit();
	DTExtend_NonRewindMiscReinstallPostInit();

	// No flat-order compensation passes here: with table names unique, the engine's own flatten already produces the order the client derives (the overlay arrays sit in the right sub-table and their elements carry the anchor's priority), so none are needed.

	// [ANIM-XLATE] Install the m_playAnimationType wire-remap proxy. Common tail --
	// reached after all rebuild/apply/reflatten passes above have finished mutating
	// the tree, so the prop lives at its final location.
	DTExtend_InstallPlayAnimTypeXlate(tables, count);

	// [REGEN-WIRE] Same common tail: the DPT_Time sentinel for m_lastRegenTime.
	DTExtend_InstallRegenStampWire(tables, count);

	// [SE-COMBO-WIRE] Same common tail: repack seComboVars S3 (<<7) -> S21 (<<6).
	DTExtend_InstallStatusComboWire(tables, count);

	// [ANIM-ANCHOR-PROXY] Same common tail: idempotent re-arm (pre-flatten
	// install above already covered the encoder-visible copies).
	AnimAnchorProxy_Install(tables, count);

	DTExtend_DumpRequestedTrees(tables, count);

	// One-shot [DT-DUMP] -- dumps the flat SendTable for CPlayer and key
	// peer classes so [CP-PROP] idx values can be cross-referenced to prop names.
	static bool s_dtFlatDumped = false;
	if (!s_dtFlatDumped && CommandLine()->CheckParm("-bridgediaglogs"))
	{
		s_dtFlatDumped = true;
		DTExtend_DumpCPlayerFlat(tables, count);
		// One-shot offhandWeapons sub-table layout probe.
		DTExtend_DumpOffhandSubTable(tables, count);
	}

	// [S21-PRECALC] The synthetic wrappers are reachable only through the entity factory, never through `tables`, so SendTable_Init does not build their precalc -- that is why the ziprail wrapper needs its own builder above.
	// The snapshot encoder walks the precalc; the client decodes against the SendTable we transmitted.
	if (s_s21Slots)
	{
		for (int ci = 0; ci < kNumS21Classes; ++ci)
		{
			S21ClassSlot& sl = s_s21Slots[ci];
			const char* cn = *(const char**)(sl.factory + FACT_CLASSNAME);
			if (!cn)
				continue;   // class skipped this boot
			const char* tn = *(const char**)(sl.wrapperTable + ST_NETTABLENAME);
			uint8_t* pc = *(uint8_t**)(sl.wrapperTable + 0x4C0);
			const int nFlat = pc ? *(int*)(pc + 0x10) : -1;
			(void)tn;
			(void)nFlat;
		}
		DTExtend_S21ChainAudit();
	}

	// After the engine flatten: deathfield children present in the flat array?
	// Does CWorld pack at all? One-shot per Init, ungated.
	DTExtend_DeathFieldFlatAndPackProbe();

	return stInitRet;
}

void VDTExtend::GetAdr(void) const
{
	LogFunAdr("SendTable_Init", v_SendTable_Init);
	LogFunAdr("CBaseAnimating::Anim_EnableCollision", v_CBaseAnimating_AnimEnableCollision);
	LogFunAdr("CVEngineServer::GetTimescale", v_Engine_GetTimescale);
	LogVarAdr("sv.m_flTimescale", s_pSvTimescale);
}

void VDTExtend::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"89 54 24 10 56 57 48 83 EC 48 83 3D ?? ?? ?? ?? 00 4C 89 64 24 78 4C 8B E1")
		.GetPtr(v_SendTable_Init);

	// CSendTablePrecalc builder -- big-stack-frame prologue
	// (mov eax,<stacksize>; call __chkstk) + `mov r14,[rcx+0x48]`. Called
	// directly by DTExtend_ZiprailBuildOwnPrecalc.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 55 56 57 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 2B E0 4C 8B 71")
		.GetPtr(v_SendTable_BuildPrecalc);

	// GLOBAL creation callback: push rdi; sub rsp,20h; mov rax,[gameState];...
	Module_FindPattern(g_GameDll,
		"57 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 8B F9 83 78 70 01")
		.Offset(-1) // pattern starts at +1 (inside the push rdi)
		.GetPtr(v_GlobalCreationCB);

	// AssignClassIds: sub rsp, 848h; call...; cmp byte_..., 0
	Module_FindPattern(g_GameDll,
		"48 81 EC 48 08 00 00 E8 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00")
		.GetPtr(v_AssignClassIds);

	// CBaseAnimating::Anim_EnableCollision body. The
	// mov byte ptr [rbx+0DC1h],1 tail pins the server half against its client twin.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 48 8B 01 48 8B D9 FF 90 E8 02 00 00 84 C0 74 ?? 0F B7 43 58 "
		"41 B8 00 02 00 00 66 83 F8 FF 74 ?? 48 0F BF D0 48 8B 05 ?? ?? ?? ?? 48 8B 48 78 "
		"66 F0 44 09 44 51 40 C6 83 C1 0D 00 00 01")
		.GetPtr(v_CBaseAnimating_AnimEnableCollision);

	// CVEngineServer::GetTimescale. First rip-rel is host_timescale.m_pParent;
	// second is CServer+0x3CC (SetTimescale out).
	Module_FindPattern(g_GameDll,
		"48 8B 05 ?? ?? ?? ?? F3 0F 10 05 ?? ?? ?? ?? F3 0F 59 40 68 C3")
		.GetPtr(v_Engine_GetTimescale);
	if (!v_Engine_GetTimescale)
		Warning(eDLL_T::ENGINE,
			"[GTS-WIRE] CVEngineServer::GetTimescale pattern unresolved -- "
			"m_gameTimescale stays 1.0\n");
}

void VDTExtend::GetVar(void) const
{
	// Resolve the factory linked list head from the factory registration fn.
	// Pattern: mov [rcx],rdx; mov [rcx+8],r8; mov [rcx+1Ch],r9d; mov [rcx+20h],0FFFFh
	// The `mov rdi, cs:[factoryListHead]` is at +0x20 from the pattern match.
	CMemory factoryRegFn = Module_FindPattern(g_GameDll,
		"48 89 11 4C 89 41 08 44 89 49 1C C7 41 20 FF FF 00 00");

	if (factoryRegFn)
	{
		g_pFactoryListHead = factoryRegFn.Offset(0x20).ResolveRelativeAddress(0x3, 0x7)
			.RCast<uintptr_t*>();
		// The factory registration function entry is at pattern - 0x1C
		v_RegisterServerClass = factoryRegFn.Offset(-0x1C).RCast<PFN_RegisterServerClass>();
		Warning(eDLL_T::ENGINE, "[dt_extend] factory list head @ 0x%p, RegisterServerClass @ 0x%p\n",
			(void*)g_pFactoryListHead, (void*)v_RegisterServerClass);
	}
	else
	{
		Warning(eDLL_T::ENGINE,
			"[dt_extend] factory registration pattern unresolved -- factory growth DISABLED\n");
	}

	// Resolve GetEntityFactory, ActivateEntity,
	// and SNDCEntityInit from the GLOBAL creation callback.
	if (v_GlobalCreationCB)
	{
		// In: call is at +0x1B from entry
		CMemory cbBase((uintptr_t)v_GlobalCreationCB);
		CMemory callGetFactory = cbBase.Offset(0x1B);
		v_GetEntityFactory = callGetFactory.FollowNearCallSelf().RCast<PFN_GetEntityFactory>();

		// call (ActivateEntity) is at +0x4D from entry
		CMemory callActivate = cbBase.Offset(0x4D);
		v_ActivateEntity = callActivate.FollowNearCallSelf().RCast<PFN_ActivateEntity>();

		// call (SNDCEntityInit) is at +0x61 from entry
		CMemory callSNDCInit = cbBase.Offset(0x61);
		v_SNDCEntityInit = callSNDCInit.FollowNearCallSelf().RCast<PFN_SNDCEntityInit>();

		Warning(eDLL_T::ENGINE,
			"[dt_extend] GetEntityFactory=0x%p ActivateEntity=0x%p SNDCEntityInit=0x%p\n",
			(void*)v_GetEntityFactory, (void*)v_ActivateEntity, (void*)v_SNDCEntityInit);
	}

	// DestroyBaselineEntity: restores m_pServerClass before
	// engine cleanup so the destructor unregisters under the correct classID.
	v_DestroyBaselineEntity = Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 4C 0F BF C2 48 8B 48 78 4A 8B 9C C1 40 C0 03 00")
		.RCast<PFN_DestroyBaselineEntity>();
	Warning(eDLL_T::ENGINE, "[dt_extend] DestroyBaselineEntity=0x%p\n",
		(void*)v_DestroyBaselineEntity);

	// Resolve DispatchSpawn from its unique prologue
	v_DispatchSpawn = Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 48 89 6C 24 18 48 89 7C 24 20 41 56 48 83 EC 40 48 8B F9 48 85 C9")
		.RCast<PFN_DispatchSpawn>();
	Warning(eDLL_T::ENGINE, "[dt_extend] DispatchSpawn=0x%p\n", (void*)v_DispatchSpawn);

	if (v_Engine_GetTimescale)
	{
		const CMemory getTs(reinterpret_cast<uintptr_t>(v_Engine_GetTimescale));
		const CMemory parent = getTs.ResolveRelativeAddress(3, 7).Deref();
		if (parent)
			host_timescale = parent.RCast<ConVar*>();
		s_pSvTimescale = getTs.Offset(7).ResolveRelativeAddress(4, 8).RCast<float*>();
		if (!s_pSvTimescale)
			Warning(eDLL_T::ENGINE, "[GTS-WIRE] CServer+0x3CC unresolved\n");
		else
			Msg(eDLL_T::ENGINE,
				"[GTS-WIRE] host_timescale=%p sv.m_flTimescale=%p val=%.4f\n",
				host_timescale, s_pSvTimescale,
				static_cast<double>(s_pSvTimescale ? *s_pSvTimescale : 1.0f));
	}
}

void VDTExtend::Detour(const bool bAttach) const
{
	// Bisect kill-switch: -disable_dt_extend on the dedi cmdline skips the
	// SendTable_Init / AssignClassIds hooks entirely (no prop append, no S21
	// class registration). For empirically isolating entity-collision lag.
	if (bAttach && CommandLine()->CheckParm("-disable_dt_extend"))
	{
		Warning(eDLL_T::ENGINE,
			"[dt_extend] DISABLED via -disable_dt_extend cmdline -- "
			"no SendTable expansion, no S21 class registration\n");
		return;
	}
	if (!v_SendTable_Init)
	{
		if (bAttach)
			Warning(eDLL_T::ENGINE,
				"[dt_extend] SendTable_Init pattern unresolved -- DISABLED\n");
		return;
	}
	DetourSetup(&v_SendTable_Init, &Hook_SendTable_Init, bAttach);
	if (bAttach)
		Warning(eDLL_T::ENGINE, "[dt_extend] hooked SendTable_Init @ 0x%p (%d props)\n",
			(void*)v_SendTable_Init, kNumExtendProps);

	// Hook AssignClassIds to register SDK ServerClasses before the engine walks the list
	if (v_AssignClassIds)
	{
		DetourSetup(&v_AssignClassIds, &Hook_AssignClassIds, bAttach);
		if (bAttach)
			Warning(eDLL_T::ENGINE, "[dt_extend] hooked AssignClassIds @ 0x%p\n",
				(void*)v_AssignClassIds);
	}

	// Hook DestroyBaselineEntity to restore m_pServerClass before cleanup
	if (v_DestroyBaselineEntity)
	{
		DetourSetup(&v_DestroyBaselineEntity, &Hook_DestroyBaselineEntity, bAttach);
		if (bAttach)
			Warning(eDLL_T::ENGINE, "[dt_extend] hooked DestroyBaselineEntity @ 0x%p\n",
				(void*)v_DestroyBaselineEntity);
	}

	// Hook DispatchSpawn so we can apply the m_pServerClass swap AFTER engine
	// registration. Construction + registration under parent class; swap
	// only kicks in for SV_CreateBaseline's encoding phase.
	if (v_DispatchSpawn)
	{
		DetourSetup(&v_DispatchSpawn, &Hook_DispatchSpawn, bAttach);
		if (bAttach)
			Warning(eDLL_T::ENGINE, "[dt_extend] hooked DispatchSpawn @ 0x%p\n",
				(void*)v_DispatchSpawn);
	}

	// Hook GLOBAL creation callback
	if (v_GlobalCreationCB)
	{
		DetourSetup(&v_GlobalCreationCB, &Hook_GlobalCreationCB, bAttach);
		if (bAttach)
			Warning(eDLL_T::ENGINE, "[dt_extend] hooked GlobalCreationCB @ 0x%p\n",
				(void*)v_GlobalCreationCB);
	}

	// Hook ActivateEntity to catch the engine's native NonRewind entity and
	// override its ServerClass post-activation (proper edict + handle).
	if (v_ActivateEntity)
	{
		v_OrigActivateEntity = v_ActivateEntity;
		DetourSetup(&v_OrigActivateEntity, &Hook_ActivateEntity, bAttach);
		if (bAttach)
			Warning(eDLL_T::ENGINE, "[dt_extend] hooked ActivateEntity @ 0x%p\n",
				(void*)v_OrigActivateEntity);
	}

	if (v_CBaseAnimating_AnimEnableCollision)
		DetourSetup(&v_CBaseAnimating_AnimEnableCollision,
			&Hook_CBaseAnimating_AnimEnableCollision, bAttach);
	else if (bAttach)
		Warning(eDLL_T::SERVER,
			"[ANIM-RTG] Anim_EnableCollision pattern unresolved -- "
			"relative-to-ground will not auto-clear\n");
}
// ===========================================================================
// SYSTEM 17: LevelShutdown [LIVE]
// per-map state reset (clears live-entity caches / pending swaps).
// ===========================================================================

//-----------------------------------------------------------------------------
// DTExtend_LevelShutdown: drop every static cache that holds live entity pointers + every "captured?" guard flag so the next map's Hook_GlobalCreationCB re-captures NonRewind / GLOBAL fresh and the pending/applied-swap arrays don't keep stale entries that can edict- collide with new entities on map 2+.
// Symptom we fix: 3rd-changelevel infinite crash loop -- Map 1 entities freed, Map 2 reuses the memory at a different class, Map 3's Hook_DestroyBaselineEntity reads *(int16_t*)(stale_ent + 0x58) on s_swappedEntities entries that now point at unrelated chunks; the edict-match path writes the wrong original-ServerClass over a living entity's m_pServerClass and the next snapshot encode AVs.
//-----------------------------------------------------------------------------
void DTExtend_LevelShutdown()
{
	// Captured entity pointers used as "already captured this map?" guards.
	// Hook_GlobalCreationCB checks s_nonRewindEntity to gate re-capture; if
	// we don't clear it, Map 2 sees the stale pointer and skips creation.
	s_nonRewindEntity = 0;
	s_globalEntity = 0;
	s_gnrTimescaleEnt = 0;
	s_gnrTimescaleHaveLast = false;

	// Slot-indexed connection quality: slots are reused across maps, so a stale
	// override would follow the next occupant of that slot.
	ConnQuality_LevelShutdown();

	// Item-flavor sidecar is keyed by raw entity pointer, so every entry is stale the moment the map tears down -- and the allocator hands those addresses back out, which would attach the previous owner's cosmetic to whatever lands there next.
	s_itemFlavorGUIDCount = 0;
	s_ignoreParentRotCount = 0;
	s_animRelativeToGroundCount = 0;
	PlayerExtend_LevelShutdown();
	TrackEntity_LevelShutdown();

	// Pending / applied m_pServerClass swap queues. Hook_DestroyBaselineEntity
	// iterates s_swappedEntities and matches by edict; stale entries from a
	// previous map can edict-collide with new entities and corrupt them.
	for (int i = 0; i < kS21SwapCapacity; ++i)
	{
		s_pendingSwap[i].entityPtr = 0;
		s_pendingSwap[i].networkablePtr = 0;
		s_pendingSwap[i].targetClass = 0;
		s_pendingSwap[i].originalClass = 0;
		s_swappedEntities[i].entityPtr = 0;
		s_swappedEntities[i].originalServerClass = 0;
	}
	s_pendingSwapCount = 0;
	s_swappedCount = 0;

	// Ziprail chain registry + KeyValue-observer meta table now live in
	// zipline_validation.cpp; clear them here too
	// so the next map's chain walk doesn't see stale entity pointers.
	ZiprailDedi_LevelShutdown();
	ZiprailWire_ResetLatch();
	ConsumableInv_LevelShutdown();
	DTExtend_NonRewindMiscLevelShutdown();
	PoseParamExt_LevelShutdown();
	AnimAnchorProxy_LevelShutdown();

	Warning(eDLL_T::ENGINE,
		"[dt_extend] LevelShutdown: cleared NonRewind/GLOBAL captures + "
		"pendingSwap/swappedEntities queues + ziprail chain registry\n");
}