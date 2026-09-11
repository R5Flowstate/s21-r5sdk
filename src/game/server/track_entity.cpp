//=============================================================================//
//
// Purpose: Track-entity camera sidecar and CPlayer natives. See track_entity.h.
//
//=============================================================================//
#include "core/stdafx.h"


#include "track_entity.h"
#include "player.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript_server.h"
#include "tier0/dbg.h"
#include "public/tier0/memaddr.h"
#include <cstddef>
#include <cstring>

extern CGlobalVars* gpGlobals;

static constexpr ptrdiff_t kTrackEntNv          = 0x6590;
static constexpr ptrdiff_t kTrackEntBlendTotal  = 0x65AC;
static constexpr ptrdiff_t kTrackEntBlendEaseIn = 0x65B0;
static constexpr ptrdiff_t kTrackEntBlendEaseOut= 0x65B4;
static constexpr ptrdiff_t kTrackEntFixedDist   = 0x65C0;
static constexpr ptrdiff_t kTrackEntFixedHeight = 0x65C4;

static __int64 (__fastcall* v_CPlayer_ClearTrackEntitySettings)(void* pPlayer) = nullptr;

struct TrackEntitySlot
{
	volatile uint64_t handleKey;
	volatile LONG seq;
	TrackEntityWire wire;
};

static TrackEntitySlot s_slots[128];
static LONG s_cursor = 0;
static volatile LONG s_used = 0;

static inline uint64_t PackHandle(const SDKEntityHandle& h)
{
	return h.IsValid() ? static_cast<uint64_t>(h.Raw()) : 0;
}

static inline float TrackEnt_CurTime(void)
{
	return gpGlobals ? gpGlobals->curTime : 0.0f;
}

static TrackEntitySlot* FindSlot(uint64_t key)
{
	if (!key || !s_used)
		return nullptr;
	for (TrackEntitySlot& slot : s_slots)
	{
		if (slot.handleKey == key)
			return &slot;
	}
	return nullptr;
}

static TrackEntitySlot* EnsureSlot(uint64_t key)
{
	if (!key)
		return nullptr;

	TrackEntitySlot* slot = FindSlot(key);
	if (slot)
		return slot;

	for (TrackEntitySlot& cand : s_slots)
	{
		if (cand.handleKey == 0)
		{
			slot = &cand;
			break;
		}
	}
	if (slot)
		InterlockedIncrement(&s_used);
	else
	{
		const LONG idx = (InterlockedIncrement(&s_cursor) - 1) & 127;
		slot = &s_slots[idx];
	}
	slot->handleKey = 0;
	InterlockedIncrement(&slot->seq);
	memset(&slot->wire, 0, sizeof(slot->wire));
	InterlockedIncrement(&slot->seq);
	slot->handleKey = key;
	return slot;
}

static bool CopyWire(const void* pEntity, TrackEntityWire* pOut)
{
	if (!pEntity || !pOut)
		return false;

	const uint64_t key = PackHandle(SDKEntityState_GetHandle(pEntity));
	TrackEntitySlot* const slot = FindSlot(key);
	if (!slot)
		return false;

	for (int attempt = 0; attempt < 8; ++attempt)
	{
		const LONG before = slot->seq;
		if (before & 1)
			continue;
		const TrackEntityWire copy = slot->wire;
		MemoryBarrier();
		if (slot->seq == before && slot->handleKey == key)
		{
			*pOut = copy;
			return true;
		}
	}
	return false;
}

bool TrackEntity_GetWire(const void* pPlayer, TrackEntityWire* pOut)
{
	if (!pOut)
		return false;
	if (CopyWire(pPlayer, pOut))
		return true;
	memset(pOut, 0, sizeof(*pOut));
	return false;
}

void TrackEntity_LevelShutdown(void)
{
	InterlockedExchange(&s_used, 0);
	memset(s_slots, 0, sizeof(s_slots));
	InterlockedExchange(&s_cursor, 0);
}

static void CommitWrite(TrackEntitySlot* slot, uint64_t key, const TrackEntityWire& wire)
{
	InterlockedIncrement(&slot->seq);
	slot->wire = wire;
	InterlockedIncrement(&slot->seq);
	slot->handleKey = key;
}

static void TrackEnt_NetworkStateChanged(void* pPlayer, ptrdiff_t fieldOff)
{
	if (!pPlayer)
		return;
	void* const nv = reinterpret_cast<char*>(pPlayer) + kTrackEntNv;
	void** const vt = *reinterpret_cast<void***>(nv);
	if (!vt || !vt[0])
		return;
	reinterpret_cast<void(__fastcall*)(void*, void*)>(vt[0])(
		nv, reinterpret_cast<char*>(pPlayer) + fieldOff);
}

static void TrackEnt_SetNativeFloat(void* pPlayer, ptrdiff_t fieldOff, float value)
{
	if (!pPlayer)
		return;
	float* const pField = reinterpret_cast<float*>(
		reinterpret_cast<char*>(pPlayer) + fieldOff);
	if (*pField == value)
		return;
	TrackEnt_NetworkStateChanged(pPlayer, fieldOff);
	*pField = value;
}

static void TrackEnt_Commit(void* pPlayer, const TrackEntityWire& wire)
{
	const uint64_t key = PackHandle(SDKEntityState_GetHandle(pPlayer));
	TrackEntitySlot* const slot = EnsureSlot(key);
	if (!slot)
		return;
	CommitWrite(slot, key, wire);
	MarkEntityEdictDirty(pPlayer);
}

static void TrackEnt_ClearExtra(void* pPlayer)
{
	if (!pPlayer)
		return;
	const uint64_t key = PackHandle(SDKEntityState_GetHandle(pPlayer));
	TrackEntitySlot* const slot = FindSlot(key);
	if (!slot)
		return;
	TrackEntityWire zero;
	memset(&zero, 0, sizeof(zero));
	CommitWrite(slot, key, zero);
	MarkEntityEdictDirty(pPlayer);
}

static void __fastcall TrackEnt_Emit(void* pStruct, void* pOut, size_t fieldOff, size_t fieldBytes)
{
	if (!pOut)
		return;
	memset(pOut, 0, fieldBytes);
	if (!pStruct)
		return;
	TrackEntityWire wire;
	if (!TrackEntity_GetWire(pStruct, &wire))
		return;
	memcpy(pOut, reinterpret_cast<const uint8_t*>(&wire) + fieldOff, fieldBytes);
}

#define TE_WIRE_PROXY(fn, member)                                              \
	static void __fastcall fn(void* /*pProp*/, void* pStruct, void* /*pData*/, \
		void* pOut, int /*iElement*/, int /*objectID*/)                        \
	{                                                                          \
		TrackEnt_Emit(pStruct, pOut,                                           \
			offsetof(TrackEntityWire, member), sizeof(TrackEntityWire::member)); \
	}

TE_WIRE_PROXY(TrackEnt_BlendOut_ValueProxy,          m_blendOutDuration)
TE_WIRE_PROXY(TrackEnt_FixedRight_ValueProxy,        m_fixedRight)
TE_WIRE_PROXY(TrackEnt_VarDistStart_ValueProxy,      m_varDistStart)
TE_WIRE_PROXY(TrackEnt_VarDistEnd_ValueProxy,        m_varDistEnd)
TE_WIRE_PROXY(TrackEnt_VarDistStartTime_ValueProxy,  m_varDistStartTime)
TE_WIRE_PROXY(TrackEnt_VarDistEndTime_ValueProxy,    m_varDistEndTime)
TE_WIRE_PROXY(TrackEnt_VarDistLerpType_ValueProxy,   m_varDistLerpType)
TE_WIRE_PROXY(TrackEnt_VarDistLogGrowth_ValueProxy,  m_varDistLogGrowth)
TE_WIRE_PROXY(TrackEnt_VarHeightStart_ValueProxy,    m_varHeightStart)
TE_WIRE_PROXY(TrackEnt_VarHeightEnd_ValueProxy,      m_varHeightEnd)
TE_WIRE_PROXY(TrackEnt_VarHeightStartTime_ValueProxy, m_varHeightStartTime)
TE_WIRE_PROXY(TrackEnt_VarHeightEndTime_ValueProxy,  m_varHeightEndTime)
TE_WIRE_PROXY(TrackEnt_VarHeightLerpType_ValueProxy, m_varHeightLerpType)
TE_WIRE_PROXY(TrackEnt_VarHeightLogGrowth_ValueProxy, m_varHeightLogGrowth)
TE_WIRE_PROXY(TrackEnt_VarRightStart_ValueProxy,     m_varRightStart)
TE_WIRE_PROXY(TrackEnt_VarRightEnd_ValueProxy,       m_varRightEnd)
TE_WIRE_PROXY(TrackEnt_VarRightStartTime_ValueProxy, m_varRightStartTime)
TE_WIRE_PROXY(TrackEnt_VarRightEndTime_ValueProxy,   m_varRightEndTime)
TE_WIRE_PROXY(TrackEnt_VarRightLerpType_ValueProxy,  m_varRightLerpType)
TE_WIRE_PROXY(TrackEnt_VarRightLogGrowth_ValueProxy, m_varRightLogGrowth)

#undef TE_WIRE_PROXY

static const struct { const char* propName; DTExtendProxyFn proxy; } s_trackEntProxies[] =
{
	{ "m_thirdPersonEntBlendOutDuration",                    &TrackEnt_BlendOut_ValueProxy },
	{ "m_thirdPersonEntFixedRight",                          &TrackEnt_FixedRight_ValueProxy },
	{ "m_thirdPersonEntVariableDistStart",                   &TrackEnt_VarDistStart_ValueProxy },
	{ "m_thirdPersonEntVariableDistEnd",                     &TrackEnt_VarDistEnd_ValueProxy },
	{ "m_thirdPersonEntVariableDistStartTime",               &TrackEnt_VarDistStartTime_ValueProxy },
	{ "m_thirdPersonEntVariableDistEndTime",                 &TrackEnt_VarDistEndTime_ValueProxy },
	{ "m_thirdPersonEntVariableDistLerpType",                &TrackEnt_VarDistLerpType_ValueProxy },
	{ "m_thirdPersonEntVariableDistLerpLogGrowthFactor",     &TrackEnt_VarDistLogGrowth_ValueProxy },
	{ "m_thirdPersonEntVariableHeightStart",                 &TrackEnt_VarHeightStart_ValueProxy },
	{ "m_thirdPersonEntVariableHeightEnd",                   &TrackEnt_VarHeightEnd_ValueProxy },
	{ "m_thirdPersonEntVariableHeightStartTime",             &TrackEnt_VarHeightStartTime_ValueProxy },
	{ "m_thirdPersonEntVariableHeightEndTime",               &TrackEnt_VarHeightEndTime_ValueProxy },
	{ "m_thirdPersonEntVariableHeightLerpType",              &TrackEnt_VarHeightLerpType_ValueProxy },
	{ "m_thirdPersonEntVariableHeightLerpLogGrowthFactor",   &TrackEnt_VarHeightLogGrowth_ValueProxy },
	{ "m_thirdPersonEntVariableRightStart",                  &TrackEnt_VarRightStart_ValueProxy },
	{ "m_thirdPersonEntVariableRightEnd",                    &TrackEnt_VarRightEnd_ValueProxy },
	{ "m_thirdPersonEntVariableRightStartTime",              &TrackEnt_VarRightStartTime_ValueProxy },
	{ "m_thirdPersonEntVariableRightEndTime",                &TrackEnt_VarRightEndTime_ValueProxy },
	{ "m_thirdPersonEntVariableRightLerpType",               &TrackEnt_VarRightLerpType_ValueProxy },
	{ "m_thirdPersonEntVariableRightLerpLogGrowthFactor",    &TrackEnt_VarRightLogGrowth_ValueProxy },
};

DTExtendProxyFn TrackEntity_ValueProxyForProp(const char* propName)
{
	if (!propName)
		return nullptr;
	for (const auto& entry : s_trackEntProxies)
	{
		if (strcmp(propName, entry.propName) == 0)
			return entry.proxy;
	}
	return nullptr;
}

static void TrackEnt_LogOnce(const char* tag, void* pPlayer)
{
	static LONG s_logged = 0;
	if (InterlockedIncrement(&s_logged) != 1)
		return;
	Msg(eDLL_T::SERVER, "[TRACK-ENT] %s first call player=%p\n", tag, pPlayer);
}

static SQRESULT Script_SetTrackEntityBlendInTimes(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	SQFloat total = 0.f, easeIn = 0.f, easeOut = 0.f;
	sq_getfloat(v, 2, &total);
	sq_getfloat(v, 3, &easeIn);
	sq_getfloat(v, 4, &easeOut);

	if (total < 0.f || easeIn < 0.f || easeOut < 0.f || total < (easeIn + easeOut))
	{
		Warning(eDLL_T::SERVER,
			"[TRACK-ENT] BlendInTimes rejected total=%f easeIn=%f easeOut=%f\n",
			total, easeIn, easeOut);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	TrackEnt_SetNativeFloat(pPlayer, kTrackEntBlendTotal, static_cast<float>(total));
	TrackEnt_SetNativeFloat(pPlayer, kTrackEntBlendEaseIn, static_cast<float>(easeIn));
	TrackEnt_SetNativeFloat(pPlayer, kTrackEntBlendEaseOut, static_cast<float>(easeOut));
	MarkEntityEdictDirty(pPlayer);
	TrackEnt_LogOnce("SetTrackEntityBlendInTimes", pPlayer);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetTrackEntityBlendOutTime(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	SQFloat duration = 0.f;
	sq_getfloat(v, 2, &duration);
	if (duration < 0.f)
		duration = 0.f;

	TrackEntityWire wire;
	if (!CopyWire(pPlayer, &wire))
		memset(&wire, 0, sizeof(wire));
	wire.m_blendOutDuration = static_cast<float>(duration);
	TrackEnt_Commit(pPlayer, wire);
	TrackEnt_LogOnce("SetTrackEntityBlendOutTime", pPlayer);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetTrackEntityOffsetRight(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	SQFloat right = 0.f;
	sq_getfloat(v, 2, &right);

	TrackEntityWire wire;
	if (!CopyWire(pPlayer, &wire))
		memset(&wire, 0, sizeof(wire));
	wire.m_fixedRight = static_cast<float>(right);
	wire.m_varRightStartTime = 0.f;
	wire.m_varRightEndTime = 0.f;
	TrackEnt_Commit(pPlayer, wire);
	TrackEnt_LogOnce("SetTrackEntityOffsetRight", pPlayer);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetTrackEntityOffsetRight(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	TrackEntityWire wire;
	TrackEntity_GetWire(pPlayer, &wire);
	sq_pushfloat(v, wire.m_fixedRight);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

enum TrackEntAxis
{
	kTrackEntAxisDist = 0,
	kTrackEntAxisHeight = 1,
	kTrackEntAxisRight = 2,
};

static void TrackEnt_SetOverTime(void* pPlayer, TrackEntAxis axis,
	float startVal, float endVal, float duration, int lerpType)
{
	if (lerpType < 0 || lerpType > 3)
	{
		Warning(eDLL_T::SERVER, "[TRACK-ENT] lerpType %d clamped to 0..3\n", lerpType);
		if (lerpType < 0)
			lerpType = 0;
		else
			lerpType = 3;
	}
	if (duration < 0.f)
		duration = 0.f;

	const float now = TrackEnt_CurTime();
	TrackEntityWire wire;
	if (!CopyWire(pPlayer, &wire))
		memset(&wire, 0, sizeof(wire));

	switch (axis)
	{
	case kTrackEntAxisDist:
		wire.m_varDistStart = startVal;
		wire.m_varDistEnd = endVal;
		wire.m_varDistStartTime = now;
		wire.m_varDistEndTime = now + duration;
		wire.m_varDistLerpType = lerpType;
		TrackEnt_SetNativeFloat(pPlayer, kTrackEntFixedDist, endVal);
		break;
	case kTrackEntAxisHeight:
		wire.m_varHeightStart = startVal;
		wire.m_varHeightEnd = endVal;
		wire.m_varHeightStartTime = now;
		wire.m_varHeightEndTime = now + duration;
		wire.m_varHeightLerpType = lerpType;
		TrackEnt_SetNativeFloat(pPlayer, kTrackEntFixedHeight, endVal);
		break;
	case kTrackEntAxisRight:
		wire.m_varRightStart = startVal;
		wire.m_varRightEnd = endVal;
		wire.m_varRightStartTime = now;
		wire.m_varRightEndTime = now + duration;
		wire.m_varRightLerpType = lerpType;
		wire.m_fixedRight = endVal;
		break;
	}

	TrackEnt_Commit(pPlayer, wire);
}

static SQRESULT Script_SetOverTime(HSQUIRRELVM v, TrackEntAxis axis, const char* tag)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	SQFloat startVal = 0.f, endVal = 0.f, duration = 0.f;
	SQInteger lerpType = 0;
	sq_getfloat(v, 2, &startVal);
	sq_getfloat(v, 3, &endVal);
	sq_getfloat(v, 4, &duration);
	sq_getinteger(v, 5, &lerpType);

	TrackEnt_SetOverTime(pPlayer, axis,
		static_cast<float>(startVal), static_cast<float>(endVal),
		static_cast<float>(duration), static_cast<int>(lerpType));
	TrackEnt_LogOnce(tag, pPlayer);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetTrackEntityOffsetDistanceOverTime(HSQUIRRELVM v)
{
	return Script_SetOverTime(v, kTrackEntAxisDist, "SetTrackEntityOffsetDistanceOverTime");
}

static SQRESULT Script_SetTrackEntityOffsetHeightOverTime(HSQUIRRELVM v)
{
	return Script_SetOverTime(v, kTrackEntAxisHeight, "SetTrackEntityOffsetHeightOverTime");
}

static SQRESULT Script_SetTrackEntityOffsetRightOverTime(HSQUIRRELVM v)
{
	return Script_SetOverTime(v, kTrackEntAxisRight, "SetTrackEntityOffsetRightOverTime");
}

static SQRESULT Script_SetLogGrowth(HSQUIRRELVM v, TrackEntAxis axis)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	SQInteger growth = 0;
	sq_getinteger(v, 2, &growth);

	TrackEntityWire wire;
	if (!CopyWire(pPlayer, &wire))
		memset(&wire, 0, sizeof(wire));
	switch (axis)
	{
	case kTrackEntAxisDist:
		wire.m_varDistLogGrowth = static_cast<int32_t>(growth);
		break;
	case kTrackEntAxisHeight:
		wire.m_varHeightLogGrowth = static_cast<int32_t>(growth);
		break;
	case kTrackEntAxisRight:
		wire.m_varRightLogGrowth = static_cast<int32_t>(growth);
		break;
	}
	TrackEnt_Commit(pPlayer, wire);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetTrackEntityOffsetDistanceOverTimeLogLerpGrowthFactor(HSQUIRRELVM v)
{
	return Script_SetLogGrowth(v, kTrackEntAxisDist);
}

static SQRESULT Script_SetTrackEntityOffsetHeightOverTimeLogLerpGrowthFactor(HSQUIRRELVM v)
{
	return Script_SetLogGrowth(v, kTrackEntAxisHeight);
}

static SQRESULT Script_SetTrackEntityOffsetRightOverTimeLogLerpGrowthFactor(HSQUIRRELVM v)
{
	return Script_SetLogGrowth(v, kTrackEntAxisRight);
}

void TrackEntity_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct)
{
	if (!playerStruct)
		return;

	playerStruct->AddFunction(
		"SetTrackEntityBlendInTimes",
		"Script_SetTrackEntityBlendInTimes",
		"Sets third-person track-entity blend-in times (total, ease-in, ease-out).",
		"void",
		"float totalDuration, float easeIn, float easeOut",
		false,
		Script_SetTrackEntityBlendInTimes);
	playerStruct->AddFunction(
		"SetTrackEntityBlendOutTime",
		"Script_SetTrackEntityBlendOutTime",
		"Sets third-person track-entity blend-out duration.",
		"void",
		"float duration",
		false,
		Script_SetTrackEntityBlendOutTime);
	playerStruct->AddFunction(
		"SetTrackEntityOffsetRight",
		"Script_SetTrackEntityOffsetRight",
		"Sets third-person track-entity camera right offset.",
		"void",
		"float right",
		false,
		Script_SetTrackEntityOffsetRight);
	playerStruct->AddFunction(
		"GetTrackEntityOffsetRight",
		"Script_GetTrackEntityOffsetRight",
		"Gets third-person track-entity camera right offset.",
		"float",
		"",
		false,
		Script_GetTrackEntityOffsetRight);
	playerStruct->AddFunction(
		"SetTrackEntityOffsetDistanceOverTime",
		"Script_SetTrackEntityOffsetDistanceOverTime",
		"Lerps third-person track-entity camera distance.",
		"void",
		"float startDistance, float endDistance, float changeTime, int lerpType",
		false,
		Script_SetTrackEntityOffsetDistanceOverTime);
	playerStruct->AddFunction(
		"SetTrackEntityOffsetHeightOverTime",
		"Script_SetTrackEntityOffsetHeightOverTime",
		"Lerps third-person track-entity camera height.",
		"void",
		"float startHeight, float endHeight, float changeTime, int lerpType",
		false,
		Script_SetTrackEntityOffsetHeightOverTime);
	playerStruct->AddFunction(
		"SetTrackEntityOffsetRightOverTime",
		"Script_SetTrackEntityOffsetRightOverTime",
		"Lerps third-person track-entity camera right offset.",
		"void",
		"float startRight, float endRight, float changeTime, int lerpType",
		false,
		Script_SetTrackEntityOffsetRightOverTime);
	playerStruct->AddFunction(
		"SetTrackEntityOffsetDistanceOverTimeLogLerpGrowthFactor",
		"Script_SetTrackEntityOffsetDistanceOverTimeLogLerpGrowthFactor",
		"Sets log-lerp growth factor for track-entity camera distance.",
		"void",
		"int growthFactor",
		false,
		Script_SetTrackEntityOffsetDistanceOverTimeLogLerpGrowthFactor);
	playerStruct->AddFunction(
		"SetTrackEntityOffsetHeightOverTimeLogLerpGrowthFactor",
		"Script_SetTrackEntityOffsetHeightOverTimeLogLerpGrowthFactor",
		"Sets log-lerp growth factor for track-entity camera height.",
		"void",
		"int growthFactor",
		false,
		Script_SetTrackEntityOffsetHeightOverTimeLogLerpGrowthFactor);
	playerStruct->AddFunction(
		"SetTrackEntityOffsetRightOverTimeLogLerpGrowthFactor",
		"Script_SetTrackEntityOffsetRightOverTimeLogLerpGrowthFactor",
		"Sets log-lerp growth factor for track-entity camera right offset.",
		"void",
		"int growthFactor",
		false,
		Script_SetTrackEntityOffsetRightOverTimeLogLerpGrowthFactor);
}

void TrackEntity_RegisterScriptConstants(CSquirrelVM* s)
{
	if (!s)
		return;
	s->RegisterConstant("THIRD_PERSON_CAMERA_LERP_MODE_LINEAR", 0);
	s->RegisterConstant("THIRD_PERSON_CAMERA_LERP_MODE_EXPONENTIAL", 1);
	s->RegisterConstant("THIRD_PERSON_CAMERA_LERP_MODE_LOGARITHMIC", 2);
}

static __int64 __fastcall Hook_CPlayer_ClearTrackEntitySettings(void* pPlayer)
{
	const __int64 result = v_CPlayer_ClearTrackEntitySettings
		? v_CPlayer_ClearTrackEntitySettings(pPlayer)
		: 0;
	TrackEnt_ClearExtra(pPlayer);
	TrackEnt_LogOnce("ClearTrackEntitySettings", pPlayer);
	return result;
}

void VTrackEntity::GetAdr(void) const
{
	LogFunAdr("CPlayer::ClearTrackEntitySettings", v_CPlayer_ClearTrackEntitySettings);
}

void VTrackEntity::GetFun(void) const
{
	// CPlayer::ClearTrackEntitySettings. lea rdi,[rcx+6598h] pins the server
	// ThirdPersonViewData (client twin uses a different displacement).
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? "
		"48 8D B9 98 65 00 00 0F 29 74 24")
		.GetPtr(v_CPlayer_ClearTrackEntitySettings);

	if (!v_CPlayer_ClearTrackEntitySettings)
		Warning(eDLL_T::SERVER,
			"[TRACK-ENT] CPlayer::ClearTrackEntitySettings pattern unresolved -- sidecar will not reset with Clear\n");
}

void VTrackEntity::Detour(const bool bAttach) const
{
	if (v_CPlayer_ClearTrackEntitySettings)
		DetourSetup(&v_CPlayer_ClearTrackEntitySettings, &Hook_CPlayer_ClearTrackEntitySettings, bAttach);
}

