//=============================================================================//
//
// Purpose: Sidecar backing for appended DT_Player / DT_BaseCombatCharacter
// props. Those tables' DTExtend_BaseOffset windows (18700 / 7200) sit inside
// CBaseCombatCharacter::m_weaponAnimEvents. Pack and writers must not touch
// entity memory for these names.
//
//=============================================================================//
#ifndef PLAYER_EXTEND_SIDECAR_H
#define PLAYER_EXTEND_SIDECAR_H

#include <cstddef>
#include <cstdint>

struct PlayerExtendWire
{
	int32_t m_armoredLeapPhase;
	int32_t m_armoredLeapType;
	int32_t m_bHasMatchAdminRole;
	int32_t m_communicationsAutoBlocked;
	int32_t m_crossPlayChat;
	int32_t m_crossPlayChatFriends;
	int32_t m_crossProgressionMigrated;
	int32_t m_dragReviveState;
	int32_t m_extraShieldHealth;
	int32_t m_extraShieldTier;
	int32_t m_laserSightColorCustomized;
	int32_t m_launcherAirControlActive;
	int32_t m_mantleBoostState;
	int32_t m_playerSettingForHoldToSprint;
	int32_t m_playerSettingForStickySprintForward;
	int32_t m_playerVehicleCount;
	int32_t m_playerVehicleDriven;
	int32_t m_reviveTarget;
	int32_t m_shadowShieldActive;
	int32_t m_skydiveFromSkywardLaunch;
	int32_t m_skydiveState;
	int32_t m_skywardLaunchFollowing;
	int32_t m_skywardLaunchInterrupted;
	int32_t m_skywardLaunchState;
	int32_t m_tempShieldHealth;
	int32_t m_turret;
	int32_t m_unspoofedHardware;

	float m_armoredLeapStartTime;
	float m_bleedoutStartTime;
	float m_dragReviveOutroStartTime;
	float m_jumpPadDebounceExpireTime;
	float m_lastSprintPressTime;
	float m_playerVehicleUseTime;
	float m_ragdollCreationYaw;
	float m_skywardLaunchEndTime;
	float m_skywardLaunchFastEndTime;
	float m_skywardLaunchFastSpeed;
	float m_skywardLaunchSlowEndTime;
	float m_skywardLaunchSlowSpeed;
	float m_skywardLaunchSlowStartTime;
	float m_stickySprintForwardDisableTime;
	float m_stickySprintForwardEnableTime;

	int64_t m_EadpUserId;
	int64_t m_progressionUserId;
	int64_t m_unSpoofedPlatformUserId;

	float m_armoredLeapAirPos[3];
	float m_armoredLeapEndPos[3];
	float m_laserSightColor[3];
	float m_ragdollCreationOrigin[3];
	float m_skywardObstacleAvoidanceEndPos[3];
	float m_skywardOffset[3];
};

struct BCCExtendWire
{
	int32_t m_weaponTypeDisabledFlags;
	int32_t m_weaponInventorySlotLockedFlags;
	int32_t m_phaseShiftType;
	int32_t m_akimboState;
	int32_t m_akimboShouldAltFire;
	int32_t m_allowHudSelectionWhileWeaponsDisabled;
	int32_t m_weaponAmmoRegenDisabled;
	int32_t m_weaponAmmoRegenDisabledRefCount;
	int32_t m_bIsPlayerOverheating;
	int32_t m_targetInfoPingValue;
	float m_playerOverheatValue;
	float m_timeLastGeneratedPlayerOverheat;
};

struct PlayerExtendBundle
{
	PlayerExtendWire player;
	BCCExtendWire bcc;
};

bool PlayerExtend_GetBundle(const void* pEntity, PlayerExtendBundle* pOut);
void PlayerExtend_GetPackReadStats(uint32_t* pReads, uint32_t* pNoSlot, uint32_t* pSeqFail);
void PlayerExtend_LevelShutdown(void);

void PlayerExtend_SetI32(void* pEntity, size_t fieldOff, int32_t value);
void PlayerExtend_SetF32(void* pEntity, size_t fieldOff, float value);
void PlayerExtend_SetI64(void* pEntity, size_t fieldOff, int64_t value);
void PlayerExtend_SetVec(void* pEntity, size_t fieldOff, const float xyz[3]);
int32_t PlayerExtend_GetI32(const void* pEntity, size_t fieldOff);
float PlayerExtend_GetF32(const void* pEntity, size_t fieldOff);

void BCCExtend_SetI32(void* pEntity, size_t fieldOff, int32_t value);
void BCCExtend_SetF32(void* pEntity, size_t fieldOff, float value);
int32_t BCCExtend_GetI32(const void* pEntity, size_t fieldOff);
float BCCExtend_GetF32(const void* pEntity, size_t fieldOff);

#endif // PLAYER_EXTEND_SIDECAR_H
