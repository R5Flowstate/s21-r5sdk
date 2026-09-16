//=============================================================================//
//
// Purpose: Script pose-param side table for indices 12..23. See
// poseparam_ext.h for the mechanism and the out-of-scope note.
//
//=============================================================================//
#include "core/stdafx.h"


#include "const.h"
#include "poseparam_ext.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/sdk_entity_state.h"
#include "mathlib/mathlib.h"

//-----------------------------------------------------------------------------
// Raw layout -- CBaseAnimating on the dedicated server.
//-----------------------------------------------------------------------------
// int16_t edict index; -1 = not networked.
static constexpr ptrdiff_t ENT_OFF_EDICTIDX   = 0x58;
// int16_t m_nModelIndex.
static constexpr ptrdiff_t ENT_OFF_MODELINDEX = 0xDE;
// model wrapper pointer; null until the model is locked.
static constexpr ptrdiff_t ENT_OFF_MODELWRAP  = 0xFD8;
// native m_flPoseParameter[12]. Element SendProps carry offsets relative to
// THIS base, so the encoder hands the wire proxy entity+0xD08, not the entity.
static constexpr ptrdiff_t ENT_OFF_POSEPARAM  = 0xD08;
// CPlayer::m_vecAbsVelocity (server half).
static constexpr ptrdiff_t ENT_OFF_ABSVELOCITY = 0x3DC;
// CMultiPlayerAnimState::m_player (server half).
static constexpr ptrdiff_t ANIMSTATE_OFF_PLAYER = 0x1C0;
// Class-core rrig slots -- identical on S21 and converted S3 light/medium/heavy.
static constexpr int kPoseIdxMoveYaw         = 2;
static constexpr int kPoseIdxMoveYawBackward = 3;

// Model wrapper: studioHdr at +0x08, virtualModel at +0x10.
static constexpr ptrdiff_t WRAP_OFF_STUDIOHDR    = 0x08;
static constexpr ptrdiff_t WRAP_OFF_VIRTUALMODEL = 0x10;
// virtualModel->numposeparameters.
static constexpr ptrdiff_t VMODEL_OFF_NUMPOSE    = 0x60;
// studioHdr->numlocalposeparameters.
static constexpr ptrdiff_t STUDIO_OFF_NUMPOSE    = 0x134;

static constexpr int kNativePoseSlots = 12;   // what CBaseAnimating stores
static constexpr int kWirePoseSlots   = 24;   // what the S21 client's array holds
static constexpr int kExtPoseSlots    = kWirePoseSlots - kNativePoseSlots;

// Edict-indexed side table. Owner pointer zeros a recycled edict row.
static float    s_poseExt[MAX_EDICTS][kExtPoseSlots];
static void*    s_poseExtOwner[MAX_EDICTS];
static uint32_t s_poseExtOwnerHandle[MAX_EDICTS];

void PoseParamExt_LevelShutdown(void)
{
	memset(s_poseExt, 0, sizeof(s_poseExt));
	memset(s_poseExtOwner, 0, sizeof(s_poseExtOwner));
	memset(s_poseExtOwnerHandle, 0, sizeof(s_poseExtOwnerHandle));
}

//-----------------------------------------------------------------------------
// Engine function pointers -- script Set/GetPoseParameter and OverTime.
//-----------------------------------------------------------------------------
static void(*v_ScriptSetPoseParameter)(void* self, int idx, float value) = nullptr;
static void(*v_ScriptSetPoseParameterOverTime)(void* self, int idx, float value, float time) = nullptr;
static float(*v_ScriptGetPoseParameter)(void* self, int idx) = nullptr;

// Script LookupPoseParameterIndex. Stock raises "Parameter name %s not found"
// on a miss; the silent name walk returns -1 without touching the VM error flag.
static int(*v_ScriptLookupPoseParameterIndex)(void* self, const char* name) = nullptr;
static int(*v_StudioHdr_FindPoseParameter)(void* self, void* wrapper, const char* name) = nullptr;

//-----------------------------------------------------------------------------
// m_flPoseParameter holds a normalized 0..1 control value, not the script value.
//-----------------------------------------------------------------------------
static float(*v_Studio_SetPoseParameter)(void* wrapper, int idx, float value, float* pCtlOut) = nullptr;
static float(*v_Studio_GetPoseParameter)(void* wrapper, int idx, float ctl) = nullptr;
static float(__fastcall* v_NativeSetPoseParameter)(void* self, void* studio, int idx, float value) = nullptr;
static __int64(__fastcall* v_ServerAnimStateUpdate)(void* animstate, float flDt, float a3, float a4) = nullptr;
static QAngle*(__fastcall* v_CPlayer_EyeAngles)(void* player, QAngle* out) = nullptr;

//-----------------------------------------------------------------------------
// ConVars.
//-----------------------------------------------------------------------------
static ConVar bridge_pose_param_ext(
	"bridge_pose_param_ext", "1", FCVAR_RELEASE | FCVAR_GAMEDLL | FCVAR_CHEAT,
	"Store script pose-param indices 12..23 in a server-side side table instead "
	"of raising Parameter index invalid. Required for S21 models whose pose "
	"count exceeds the S3 native 12. LookupPoseParameterIndex returns -1 on a "
	"missing name instead of raising. 0 = stock S3 behaviour.");

static ConVar bridge_pose_param_ext_diag(
	"bridge_pose_param_ext_diag", "0", FCVAR_DEVELOPMENTONLY | FCVAR_GAMEDLL,
	"[POSE-EXT] extended pose-param store diagnostics. 1 = periodic tally, "
	"2 = also one line per store (hard-capped). Default 0.");

static ConVar bridge_pose_moveyaw(
	"bridge_pose_moveyaw", "1", FCVAR_RELEASE | FCVAR_GAMEDLL | FCVAR_CHEAT,
	"After server animstate Update, write move_yaw / move_yaw_backward from "
	"abs-velocity vs EyeAngles so remotes receive the strafe lean. 0 = stock.");

static constexpr LONG kTallyInterval = 64;
static constexpr LONG kVerboseMax    = 64;

static volatile LONG s_nStored   = 0;
static volatile LONG s_nVerbose  = 0;
static volatile LONG s_bAnnounced = 0;
static volatile LONG s_bRawSpaceWarned = 0;
static volatile LONG s_bNonNetWarned = 0;
static volatile LONG s_bOverTimeWarned = 0;
static volatile LONG s_nWireNonZero = 0;
static volatile LONG s_nWireCalls    = 0;
static volatile LONG s_bWireHitLogged = 0;
static volatile LONG s_bLookupMissLogged = 0;
static volatile LONG s_bMoveYawAnnounced = 0;
static volatile LONG s_bMoveYawEyeWarned = 0;
static volatile LONG s_bNativeSetPoseAnnounced = 0;

// SP_OFFSET on grown elements 12..23: stashed element index, not a struct offset.
static constexpr ptrdiff_t kWirePropOffsetSlot = 0x78;

int PoseParamExt_GetWireSlotCount(void)
{
	return kExtPoseSlots;
}

//-----------------------------------------------------------------------------
// Purpose: the entity's model wrapper, or null before the model is locked.
//-----------------------------------------------------------------------------
static void* PoseExt_ModelWrapper(const void* self)
{
	if (!self)
		return nullptr;
	return *reinterpret_cast<void* const*>(
		reinterpret_cast<const uint8_t*>(self) + ENT_OFF_MODELWRAP);
}

//-----------------------------------------------------------------------------
// Purpose: script value -> normalized 0..1 control. Loud once if converter missing.
//-----------------------------------------------------------------------------
static float PoseExt_ToControlValue(const void* self, const int idx, const float value)
{
	void* const wrapper = PoseExt_ModelWrapper(self);
	if (!v_Studio_SetPoseParameter || !wrapper)
	{
		if (InterlockedCompareExchange(&s_bRawSpaceWarned, 1, 0) == 0)
			Warning(eDLL_T::SERVER,
				"[POSE-EXT] pose-param converters unavailable -- extended slots stored "
				"in RAW script space; the client will read them as normalized\n");
		return value;
	}

	float ctl = 0.0f;
	v_Studio_SetPoseParameter(wrapper, idx, value, &ctl);
	return ctl;
}

//-----------------------------------------------------------------------------
// Purpose: normalized control value -> script space (the getter's own mapping).
//-----------------------------------------------------------------------------
static float PoseExt_FromControlValue(const void* self, const int idx, const float ctl)
{
	void* const wrapper = PoseExt_ModelWrapper(self);
	if (!v_Studio_GetPoseParameter || !wrapper)
		return ctl;

	return v_Studio_GetPoseParameter(wrapper, idx, ctl);
}

//-----------------------------------------------------------------------------
// Purpose: mirrors the engine's own pose-param count resolution.
// Returns 0 when the model is not resolvable.
//-----------------------------------------------------------------------------
static int PoseExt_ResolveNumPoseParams(const void* self)
{
	if (!self)
		return 0;

	const uint8_t* const pEnt = reinterpret_cast<const uint8_t*>(self);
	const void* const wrapper = *reinterpret_cast<const void* const*>(pEnt + ENT_OFF_MODELWRAP);
	if (!wrapper)
		return 0;

	const uint8_t* const pWrap = reinterpret_cast<const uint8_t*>(wrapper);
	const void* const studioHdr = *reinterpret_cast<const void* const*>(pWrap + WRAP_OFF_STUDIOHDR);
	const void* const virtualModel = *reinterpret_cast<const void* const*>(pWrap + WRAP_OFF_VIRTUALMODEL);

	if (virtualModel)
		return *reinterpret_cast<const int*>(
			reinterpret_cast<const uint8_t*>(virtualModel) + VMODEL_OFF_NUMPOSE);
	if (studioHdr)
		return *reinterpret_cast<const int*>(
			reinterpret_cast<const uint8_t*>(studioHdr) + STUDIO_OFF_NUMPOSE);
	return 0;
}

//-----------------------------------------------------------------------------
// Purpose: owner-guard a side-table row, then return the extended slot.
// Caller has already bounded edictIdx and the slot index.
//-----------------------------------------------------------------------------
static float* PoseExt_Slot(void* self, const int edictIdx, const int idx)
{
	const int slot = idx - kNativePoseSlots;

	if (s_poseExtOwner[edictIdx] != self)
	{
		for (int i = 0; i < kExtPoseSlots; i++)
			s_poseExt[edictIdx][i] = 0.0f;
		s_poseExtOwner[edictIdx] = self;
		s_poseExtOwnerHandle[edictIdx] = SDKEntityState_GetHandle(self).Raw();
	}

	return &s_poseExt[edictIdx][slot];
}

//-----------------------------------------------------------------------------
// Purpose: read edict index; returns -1 when not networked or out of range.
//-----------------------------------------------------------------------------
static int PoseExt_ReadEdictIdx(const void* self)
{
	const int16_t raw = *reinterpret_cast<const int16_t*>(
		reinterpret_cast<const uint8_t*>(self) + ENT_OFF_EDICTIDX);
	if (raw < 0 || raw >= MAX_EDICTS)
		return -1;
	return static_cast<int>(raw);
}

static int16_t PoseExt_ReadModelIndex(const void* self)
{
	return *reinterpret_cast<const int16_t*>(
		reinterpret_cast<const uint8_t*>(self) + ENT_OFF_MODELINDEX);
}

//-----------------------------------------------------------------------------
// Purpose: diagnostics after a successful extended store.
//-----------------------------------------------------------------------------
static void PoseExt_OnStored(const int idx, const float value, const float ctl,
	const int edictIdx, const int modelIdx)
{
	const LONG n = InterlockedIncrement(&s_nStored);

	// value vs ctl is the whole mapping: equal means the parameter is authored
	// start=0 end=1, anything else names the real range on the first store.
	if (InterlockedCompareExchange(&s_bAnnounced, 1, 0) == 0)
		DevMsg(eDLL_T::SERVER,
			"[POSE-EXT] first extended store idx=%d value=%.4f ctl=%.4f edict=%d model=%d\n",
			idx, value, ctl, edictIdx, modelIdx);

	const int nDiag = bridge_pose_param_ext_diag.GetInt();
	if (nDiag >= 2 && InterlockedIncrement(&s_nVerbose) <= kVerboseMax)
		DevMsg(eDLL_T::SERVER,
			"[POSE-EXT] store #%ld idx=%d value=%.4f ctl=%.4f edict=%d model=%d\n",
			n, idx, value, ctl, edictIdx, modelIdx);
	else if (nDiag >= 1 && (n % kTallyInterval) == 0)
		DevMsg(eDLL_T::SERVER, "[POSE-EXT] %ld extended pose-param stores\n", n);
}

//-----------------------------------------------------------------------------
// Purpose: SetPoseParameter -- side-table write for indices 12..23.
// Delegates to the original on any doubt so error wording stays engine-native.
//-----------------------------------------------------------------------------
static void Hook_ScriptSetPoseParameter(void* self, int idx, float value)
{
	if (!bridge_pose_param_ext.GetBool() || !self)
	{
		v_ScriptSetPoseParameter(self, idx, value);
		return;
	}

	if (idx < 0)
		return;

	if (idx < kNativePoseSlots)
	{
		v_ScriptSetPoseParameter(self, idx, value);
		return;
	}

	// Beyond the wire array -- engine raises; correct, the script is out of range.
	if (idx >= kWirePoseSlots)
	{
		v_ScriptSetPoseParameter(self, idx, value);
		return;
	}

	const int numPose = PoseExt_ResolveNumPoseParams(self);
	if (numPose <= 0 || idx >= numPose)
	{
		v_ScriptSetPoseParameter(self, idx, value);
		return;
	}

	const int edictIdx = PoseExt_ReadEdictIdx(self);
	if (edictIdx < 0)
	{
		if (InterlockedCompareExchange(&s_bNonNetWarned, 1, 0) == 0)
			Warning(eDLL_T::SERVER,
				"[POSE-EXT] SetPoseParameter idx=%d on non-networked entity "
				"(model=%d) -- extended slot dropped, script continues\n",
				idx, static_cast<int>(PoseExt_ReadModelIndex(self)));
		return;
	}

	// 0 <= edictIdx < MAX_EDICTS (PoseExt_ReadEdictIdx)
	// 0 <= idx - kNativePoseSlots < kExtPoseSlots (idx in [12, 24))
	const float ctl = PoseExt_ToControlValue(self, idx, value);
	*PoseExt_Slot(self, edictIdx, idx) = ctl;
	MarkEntityEdictDirty(self);
	PoseExt_OnStored(idx, value, ctl, edictIdx, static_cast<int>(PoseExt_ReadModelIndex(self)));
}

//-----------------------------------------------------------------------------
// Purpose: SetPoseParameterOverTime -- instant set for extended slots.
// Engine OverTime state is 12-wide; no room for slots past the native array.
//-----------------------------------------------------------------------------
static void Hook_ScriptSetPoseParameterOverTime(void* self, int idx, float value, float time)
{
	if (!bridge_pose_param_ext.GetBool() || !self)
	{
		v_ScriptSetPoseParameterOverTime(self, idx, value, time);
		return;
	}

	if (idx < 0)
		return;

	if (idx < kNativePoseSlots)
	{
		v_ScriptSetPoseParameterOverTime(self, idx, value, time);
		return;
	}

	if (idx >= kWirePoseSlots)
	{
		v_ScriptSetPoseParameterOverTime(self, idx, value, time);
		return;
	}

	const int numPose = PoseExt_ResolveNumPoseParams(self);
	if (numPose <= 0 || idx >= numPose)
	{
		v_ScriptSetPoseParameterOverTime(self, idx, value, time);
		return;
	}

	const int edictIdx = PoseExt_ReadEdictIdx(self);
	if (edictIdx < 0)
	{
		if (InterlockedCompareExchange(&s_bNonNetWarned, 1, 0) == 0)
			Warning(eDLL_T::SERVER,
				"[POSE-EXT] SetPoseParameterOverTime idx=%d on non-networked entity "
				"(model=%d) -- extended slot dropped, script continues\n",
				idx, static_cast<int>(PoseExt_ReadModelIndex(self)));
		return;
	}

	if (InterlockedCompareExchange(&s_bOverTimeWarned, 1, 0) == 0)
		Warning(eDLL_T::SERVER,
			"[POSE-EXT] SetPoseParameterOverTime on extended slots applies as an "
			"instant set (no over-time state past the native 12)\n");

	// time ignored -- no m_poseParameterGoalValue/EndTime room past 12. The
	// engine's own over-time path normalizes the GOAL the same way, so the
	// instant set lands on exactly the value it would have ramped to.
	const float ctl = PoseExt_ToControlValue(self, idx, value);
	*PoseExt_Slot(self, edictIdx, idx) = ctl;
	MarkEntityEdictDirty(self);
	PoseExt_OnStored(idx, value, ctl, edictIdx, static_cast<int>(PoseExt_ReadModelIndex(self)));
}

//-----------------------------------------------------------------------------
// Purpose: SendProp value proxy for m_flPoseParameter elements 12..23.
// Element index is stashed in SP_OFFSET by the grow pass (not a struct offset).
//-----------------------------------------------------------------------------
void __fastcall PoseParamExt_WireProxy(void* pProp, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int objectID)
{
	if (!pOut)
		return;

	// Zero full DVariant first -- correct for every failure path below.
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;

	const int nDiag = bridge_pose_param_ext_diag.GetInt();
	if (nDiag >= 1)
	{
		// calls=0 grow never installed; stored>0 with nonzero=0 pStruct is not the entity.
		const LONG c = InterlockedIncrement(&s_nWireCalls);
		if ((c & 4095) == 0)
			DevMsg(eDLL_T::SERVER,
				"[POSE-EXT] wire proxy calls=%ld nonzero=%ld stored=%ld\n",
				c, s_nWireNonZero, s_nStored);
	}

	if (!pProp || !pStruct)
		return;

	const int elemIdx = *reinterpret_cast<const int*>(
		reinterpret_cast<const uint8_t*>(pProp) + kWirePropOffsetSlot);
	if (elemIdx < kNativePoseSlots || elemIdx >= kWirePoseSlots)
		return;

	// pStruct is the pose-array base, not the entity. Compare it; never dereference.
	if (objectID < 0 || objectID >= MAX_EDICTS)
		return;

	void* const owner = s_poseExtOwner[objectID];
	if (!owner)
		return;
	// The row belongs to the entity that stored it; a recycled edict must not
	// inherit its poses.
	const SDKEntityHandle ownerHandle(s_poseExtOwnerHandle[objectID]);
	if (!ownerHandle.IsValid() || SDKEntityState_Resolve(ownerHandle, ESide::Server) != owner)
	{
		for (int i = 0; i < kExtPoseSlots; i++)
			s_poseExt[objectID][i] = 0.0f;
		s_poseExtOwner[objectID] = nullptr;
		s_poseExtOwnerHandle[objectID] = 0;
		return;
	}

	const float v = s_poseExt[objectID][elemIdx - kNativePoseSlots];
	*reinterpret_cast<float*>(pOut) = v;

	if (v != 0.0f)
	{
		InterlockedIncrement(&s_nWireNonZero);
		if (InterlockedCompareExchange(&s_bWireHitLogged, 1, 0) == 0)
			Msg(eDLL_T::SERVER,
				"[POSE-EXT] first non-zero wire emit: elem=%d value=%.4f objectID=%d "
				"poseBaseDelta=%lld\n",
				elemIdx, v, objectID,
				static_cast<long long>(
					reinterpret_cast<uint8_t*>(pStruct) - reinterpret_cast<uint8_t*>(owner)));
	}
}

//-----------------------------------------------------------------------------
// Purpose: GetPoseParameter -- side-table read for indices 12..23.
// Also closes the native over-read of m_poseParameterOverTimeActive as float.
//-----------------------------------------------------------------------------
static float Hook_ScriptGetPoseParameter(void* self, int idx)
{
	if (!bridge_pose_param_ext.GetBool() || !self)
		return v_ScriptGetPoseParameter(self, idx);

	if (idx < 0)
		return 0.0f;

	if (idx < kNativePoseSlots || idx >= kWirePoseSlots)
		return v_ScriptGetPoseParameter(self, idx);

	const int numPose = PoseExt_ResolveNumPoseParams(self);
	if (numPose <= 0 || idx >= numPose)
		return v_ScriptGetPoseParameter(self, idx);

	const int edictIdx = PoseExt_ReadEdictIdx(self);
	if (edictIdx < 0)
		return 0.0f;

	// Storage is normalized; the native getter denormalizes before returning,
	// so scripts must see script space here too.
	return PoseExt_FromControlValue(self, idx, *PoseExt_Slot(self, edictIdx, idx));
}

//-----------------------------------------------------------------------------
// Purpose: LookupPoseParameterIndex -- return -1 on a miss instead of raising.
//-----------------------------------------------------------------------------
static int Hook_ScriptLookupPoseParameterIndex(void* self, const char* name)
{
	if (!bridge_pose_param_ext.GetBool() || !self || !name ||
		!v_StudioHdr_FindPoseParameter)
	{
		if (v_ScriptLookupPoseParameterIndex)
			return v_ScriptLookupPoseParameterIndex(self, name);
		return -1;
	}

	void* const wrapper = PoseExt_ModelWrapper(self);
	if (!wrapper)
		return -1;

	const int idx = v_StudioHdr_FindPoseParameter(self, wrapper, name);
	if (idx < 0 || (idx == 0 && PoseExt_ResolveNumPoseParams(self) <= 0))
	{
		if (InterlockedCompareExchange(&s_bLookupMissLogged, 1, 0) == 0)
			Warning(eDLL_T::SERVER,
				"[POSE-EXT] LookupPoseParameterIndex miss '%s' model=%d -> -1\n",
				name, static_cast<int>(PoseExt_ReadModelIndex(self)));
		else if (bridge_pose_param_ext_diag.GetInt() >= 2 &&
			InterlockedIncrement(&s_nVerbose) <= kVerboseMax)
			DevMsg(eDLL_T::SERVER,
				"[POSE-EXT] LookupPoseParameterIndex miss '%s' model=%d -> -1\n",
				name, static_cast<int>(PoseExt_ReadModelIndex(self)));
		return -1;
	}

	return idx;
}

//-----------------------------------------------------------------------------
// Purpose: write a normalized 0..1 control into native [0..11] or the sidecar.
//-----------------------------------------------------------------------------
static void PoseExt_WriteControl(void* self, const int idx, const float ctl)
{
	if (!self || idx < 0 || idx >= kWirePoseSlots)
		return;

	float clamped = ctl;
	if (clamped < 0.0f)
		clamped = 0.0f;
	else if (clamped > 1.0f)
		clamped = 1.0f;

	if (idx < kNativePoseSlots)
	{
		float* const slot = reinterpret_cast<float*>(
			reinterpret_cast<uint8_t*>(self) + ENT_OFF_POSEPARAM + idx * 4);
		if (fabsf(*slot - clamped) <= 0.001f)
			return;
		*slot = clamped;
		MarkEntityEdictDirty(self);
		return;
	}

	const int edictIdx = PoseExt_ReadEdictIdx(self);
	if (edictIdx < 0)
		return;
	float* const ext = PoseExt_Slot(self, edictIdx, idx);
	if (fabsf(*ext - clamped) <= 0.001f)
		return;
	*ext = clamped;
	MarkEntityEdictDirty(self);
}

static float PoseExt_PlusMinus90ToControl(const float degrees)
{
	return (degrees + 90.0f) / 180.0f;
}

//-----------------------------------------------------------------------------
// Purpose: native CBaseAnimating::SetPoseParameter -- sidecar for idx 12..23
// so animstate writes past the 12-wide array do not land in overtime state.
//-----------------------------------------------------------------------------
static float __fastcall Hook_NativeSetPoseParameter(void* self, void* studio, int idx, float value)
{
	if (idx >= kWirePoseSlots)
	{
		static volatile LONG s_nIdxRefuse;
		const LONG n = InterlockedIncrement(&s_nIdxRefuse);
		if (n <= 4 || (n % 128) == 0)
		{
			Warning(eDLL_T::SERVER,
				"[POSE-EXT] native SetPoseParameter idx=%d >= wire slots -- refused, stock not called\n",
				idx);
		}
		return value;
	}

	if (!self || idx < kNativePoseSlots || !studio)
	{
		if (v_NativeSetPoseParameter)
			return v_NativeSetPoseParameter(self, studio, idx, value);
		return value;
	}

	float ctl = 0.0f;
	if (v_Studio_SetPoseParameter && studio)
		v_Studio_SetPoseParameter(studio, idx, value, &ctl);
	else
		ctl = value;

	PoseExt_WriteControl(self, idx, ctl);
	if (InterlockedCompareExchange(&s_bNativeSetPoseAnnounced, 1, 0) == 0)
		DevMsg(eDLL_T::SERVER,
			"[POSE-EXT] native SetPoseParameter idx=%d value=%.4f ctl=%.4f -- sidecar\n",
			idx, value, ctl);
	return value;
}

//-----------------------------------------------------------------------------
// Purpose: eye-relative move_yaw onto class-core slots 2/3 after Update.
// Indices, not names -- Update's name walk bails the whole pose block when
// aim_pitch misses.
//-----------------------------------------------------------------------------
static void PoseExt_AuthorMoveYaw(void* animstate)
{
	if (!bridge_pose_moveyaw.GetBool() || !animstate || !v_CPlayer_EyeAngles)
		return;

	void* const player = *reinterpret_cast<void* const*>(
		reinterpret_cast<const uint8_t*>(animstate) + ANIMSTATE_OFF_PLAYER);
	if (!player)
		return;
	if (PoseExt_ReadEdictIdx(player) < 0)
		return;

	QAngle eye;
	eye.Init();
	if (!v_CPlayer_EyeAngles(player, &eye))
	{
		if (InterlockedCompareExchange(&s_bMoveYawEyeWarned, 1, 0) == 0)
			Warning(eDLL_T::SERVER,
				"[POSE-MOVEYAW] EyeAngles returned null -- locomotion pose not authored\n");
		return;
	}

	const float* const vel = reinterpret_cast<const float*>(
		reinterpret_cast<const uint8_t*>(player) + ENT_OFF_ABSVELOCITY);
	const float speed2d = sqrtf((vel[0] * vel[0]) + (vel[1] * vel[1]));

	float moveYaw = 0.0f;
	float moveYawBack = 0.0f;
	if (speed2d > 0.5f)
	{
		const float velYaw = RAD2DEG(atan2f(vel[1], vel[0]));
		float delta = AngleNormalize(velYaw - eye.y);
		if (delta < -90.0f || delta > 90.0f)
		{
			moveYawBack = AngleNormalize(delta + 180.0f);
			moveYaw = -moveYawBack;
		}
		else
		{
			moveYaw = delta;
			moveYawBack = -delta;
		}
	}

	const float ctlYaw = PoseExt_PlusMinus90ToControl(moveYaw);
	const float ctlBack = PoseExt_PlusMinus90ToControl(moveYawBack);
	PoseExt_WriteControl(player, kPoseIdxMoveYaw, ctlYaw);
	PoseExt_WriteControl(player, kPoseIdxMoveYawBackward, ctlBack);

	if (InterlockedCompareExchange(&s_bMoveYawAnnounced, 1, 0) == 0)
		Msg(eDLL_T::SERVER,
			"[POSE-MOVEYAW] first author deg=%.1f ctl=%.4f speed=%.1f eye=%.1f edict=%d\n",
			moveYaw, ctlYaw, speed2d, eye.y, PoseExt_ReadEdictIdx(player));
}

static __int64 __fastcall Hook_ServerAnimStateUpdate(void* animstate, float flDt, float a3, float a4)
{
	const __int64 result = v_ServerAnimStateUpdate
		? v_ServerAnimStateUpdate(animstate, flDt, a3, a4)
		: 0;
	PoseExt_AuthorMoveYaw(animstate);
	return result;
}

void VPoseParamExt::GetAdr(void) const
{
	LogFunAdr("ScriptLookupPoseParameterIndex", v_ScriptLookupPoseParameterIndex);
	LogFunAdr("StudioHdr_FindPoseParameter", v_StudioHdr_FindPoseParameter);
	LogFunAdr("ScriptSetPoseParameter", v_ScriptSetPoseParameter);
	LogFunAdr("ScriptSetPoseParameterOverTime", v_ScriptSetPoseParameterOverTime);
	LogFunAdr("ScriptGetPoseParameter", v_ScriptGetPoseParameter);
	LogFunAdr("Studio_SetPoseParameter", v_Studio_SetPoseParameter);
	LogFunAdr("Studio_GetPoseParameter", v_Studio_GetPoseParameter);
	LogFunAdr("NativeSetPoseParameter", v_NativeSetPoseParameter);
	LogFunAdr("ServerAnimStateUpdate", v_ServerAnimStateUpdate);
	LogFunAdr("CPlayer_EyeAngles", v_CPlayer_EyeAngles);
}

void VPoseParamExt::GetFun(void) const
{
	// Script LookupPoseParameterIndex. The 0xFD8 displacement is the server
	// CBaseAnimating model-wrapper offset -- keeps the client twin
	// (LookupSequence shares the prologue) out.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 57 48 83 EC ?? 48 83 B9 D8 0F 00 00 00 48 8B FA 48 8B D9 75 ?? "
		"0F BF 91 DE 00 00 00 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 ?? 48 85 C0 74 ?? "
		"48 8B CB E8 ?? ?? ?? ?? 48 8B 9B D8 0F 00 00 48 85 DB 74 ?? 48 83 7B 08 00 75 ?? "
		"33 DB 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 53 08 83 BA 58 01 00 00 00")
		.GetPtr(v_ScriptLookupPoseParameterIndex);

	if (!v_ScriptLookupPoseParameterIndex)
		Warning(eDLL_T::SERVER,
			"[POSE-EXT] ScriptLookupPoseParameterIndex pattern unresolved -- "
			"missing pose-param names still raise and unwind the script thread\n");

	// Silent name walk. Returns -1 on miss, 0 when the studiohdr is unusable.
	Module_FindPattern(g_GameDll,
		"48 89 74 24 ?? 57 48 83 EC ?? 49 8B F0 48 8B FA 48 85 D2")
		.GetPtr(v_StudioHdr_FindPoseParameter);

	if (!v_StudioHdr_FindPoseParameter)
		Warning(eDLL_T::SERVER,
			"[POSE-EXT] StudioHdr_FindPoseParameter pattern unresolved -- "
			"LookupPoseParameterIndex cannot swallow a miss\n");

	// Script SetPoseParameter.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 48 83 B9 ?? ?? ?? ?? ?? 8B F2")
		.GetPtr(v_ScriptSetPoseParameter);

	if (!v_ScriptSetPoseParameter)
		Warning(eDLL_T::SERVER,
			"[POSE-EXT] ScriptSetPoseParameter pattern unresolved -- Ballistic "
			"ultimate stays broken (Parameter index invalid on idx>=12)\n");

	// Script SetPoseParameterOverTime.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 48 83 B9 ?? ?? ?? ?? ?? 48 8B D9")
		.GetPtr(v_ScriptSetPoseParameterOverTime);

	if (!v_ScriptSetPoseParameterOverTime)
		Warning(eDLL_T::SERVER,
			"[POSE-EXT] ScriptSetPoseParameterOverTime pattern unresolved -- "
			"extended OverTime sets still raise Parameter index invalid\n");

	// Script GetPoseParameter.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 48 83 B9 ?? ?? ?? ?? ?? 8B EA")
		.GetPtr(v_ScriptGetPoseParameter);

	if (!v_ScriptGetPoseParameter)
		Warning(eDLL_T::SERVER,
			"[POSE-EXT] ScriptGetPoseParameter pattern unresolved -- extended "
			"reads still over-read m_poseParameterOverTimeActive as float\n");

	// Studio_SetPoseParameter.
	Module_FindPattern(g_GameDll,
		"40 57 48 83 EC ?? 48 8B 41 ?? 49 8B F9")
		.GetPtr(v_Studio_SetPoseParameter);

	// Studio_GetPoseParameter.
	Module_FindPattern(g_GameDll,
		"48 83 EC ?? 0F 29 74 24 ?? 0F 28 F2 85 D2")
		.GetPtr(v_Studio_GetPoseParameter);

	if (!v_Studio_SetPoseParameter || !v_Studio_GetPoseParameter)
		Warning(eDLL_T::SERVER,
			"[POSE-EXT] pose-param space converters unresolved -- extended slots "
			"fall back to RAW script space and the client will misread any "
			"parameter not authored start=0 end=1\n");

	// Native CBaseAnimating::SetPoseParameter. No 12-cap; idx 12+ lands in overtime.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 57 48 83 EC ?? 0F 29 74 24 ?? 0F 28 F3 49 63 F8 48 8B C2 48 8B D9 48 85 D2 0F 84")
		.GetPtr(v_NativeSetPoseParameter);

	if (!v_NativeSetPoseParameter)
		Warning(eDLL_T::SERVER,
			"[POSE-EXT] native SetPoseParameter pattern unresolved -- "
			"idx 12..23 still write through the 12-wide array into overtime\n");

	// Server CMultiPlayerAnimState::Update. [rcx+1C0]=player keeps the client twin out.
	Module_FindPattern(g_GameDll,
		"48 8B C4 55 53 57 41 55 48 8D 68 ?? 48 81 EC ?? ?? ?? ?? 48 8B 99 C0 01 00 00 48 8B F9")
		.GetPtr(v_ServerAnimStateUpdate);

	if (!v_ServerAnimStateUpdate)
		Warning(eDLL_T::SERVER,
			"[POSE-MOVEYAW] server animstate Update pattern unresolved -- "
			"move_yaw stays whatever the name walk wrote\n");

	Module_FindPattern(g_GameDll, "40 53 48 83 EC 30 F2 0F 10 05 ?? ?? ?? ??")
		.GetPtr(v_CPlayer_EyeAngles);

	if (!v_CPlayer_EyeAngles)
		Warning(eDLL_T::SERVER,
			"[POSE-MOVEYAW] CPlayer::EyeAngles pattern unresolved -- "
			"locomotion pose author disabled\n");
}

void VPoseParamExt::Detour(const bool bAttach) const
{
	if (v_ScriptLookupPoseParameterIndex)
		DetourSetup(&v_ScriptLookupPoseParameterIndex, &Hook_ScriptLookupPoseParameterIndex, bAttach);
	if (v_ScriptSetPoseParameter)
		DetourSetup(&v_ScriptSetPoseParameter, &Hook_ScriptSetPoseParameter, bAttach);
	if (v_ScriptSetPoseParameterOverTime)
		DetourSetup(&v_ScriptSetPoseParameterOverTime, &Hook_ScriptSetPoseParameterOverTime, bAttach);
	if (v_ScriptGetPoseParameter)
		DetourSetup(&v_ScriptGetPoseParameter, &Hook_ScriptGetPoseParameter, bAttach);
	if (v_NativeSetPoseParameter)
		DetourSetup(&v_NativeSetPoseParameter, &Hook_NativeSetPoseParameter, bAttach);
	if (v_ServerAnimStateUpdate)
		DetourSetup(&v_ServerAnimStateUpdate, &Hook_ServerAnimStateUpdate, bAttach);
}

