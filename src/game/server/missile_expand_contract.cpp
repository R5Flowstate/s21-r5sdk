//=============================================================================//
//
// Purpose: S21 expand-contract missile path on the dedicated server.
//
// The S3 CMissile carries a two-phase expand-contract path. S21 adds a third
// phase, a blend into the target, a velocity wiggle and a no-collide grace
// period, and moves target selection into a weapon native. The S3 think keeps
// its schedule; the velocity it writes is replaced with the S21 one.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier1/cvar.h"
#include "missile_expand_contract.h"
#include "baseentity.h"
#include "trigger_gravity.h"
#include "vscript_server.h"
#include "vscript_server_natives.h"
#include "game/shared/sdk_entity_state.h"
#include "public/edict.h"
#include "public/gametrace.h"
#include "mathlib/mathlib.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include <cmath>
#include <cfloat>
#include <cstring>
#include <random>

extern CGlobalVars* gpGlobals;

static ConVar bridge_missile_expand_contract("bridge_missile_expand_contract", "1", FCVAR_RELEASE,
	"Fly missiles set up with the S21 InitMissileExpandContract on the S21 path (third phase, "
	"target blend, wiggle, grace period). 0 = leave the S3 velocity in place.");

static ConVar bridge_missile_expand_contract_diag("bridge_missile_expand_contract_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Print a [MISSILE-EC] line every N velocity thinks per missile (0 = off).");

// CMissile (server half).
static constexpr ptrdiff_t MISSILE_OFF_COLLISION  = 0x328;  // CCollisionProperty; solid flags at +0x28
static constexpr ptrdiff_t MISSILE_OFF_ABS_VELOCITY = 0x3DC; // valid after CalcAbsoluteVelocity
static constexpr ptrdiff_t MISSILE_OFF_SPAWN_TIME = 0x23C8; // time base of the S3 velocity think
static constexpr ptrdiff_t MISSILE_OFF_LAUNCHED   = 0x2400; // byte; the S3 think does nothing until set
static constexpr ptrdiff_t MISSILE_OFF_SPEED      = 0x2470; // speed the S3 path and homing scale by
static constexpr ptrdiff_t MISSILE_OFF_GRACE_END  = 0x24EC; // main think clears FSOLID_NOT_SOLID past it
static constexpr ptrdiff_t MISSILE_OFF_LAST_THINK = 0x2550; // main think stamps curtime

// CGlobalVars frame time; the S3 velocity think reads the same slot.
static constexpr ptrdiff_t GLOBALS_OFF_FRAMETIME = 0x30;

static constexpr int FSOLID_NOT_SOLID_BIT = 0x4;

// S21 collision groups PLAYER (7) and BLOCK_WEAPONS (21) in the S3 enum.
static constexpr int COLLISION_GROUP_PLAYER_S3        = 6;
static constexpr int COLLISION_GROUP_BLOCK_WEAPONS_S3 = 19;

// TRACE_MASK_SOLID. World terrain only carries clip-class contents bits
// (PHYSICSCLIP among them), so dropping 0x200 makes every trace miss the ground.
static constexpr unsigned int MISSILE_TRACE_MASK = 0x0200420B;

static constexpr float MISSILE_EPS = 1e-6f;

struct MissileExpandContractState_t
{
	bool m_bActive = false;

	float m_flSpeed = 0.0f;
	Vector3D m_vPhase1;
	Vector3D m_vPhase2;
	Vector3D m_vPhase3;
	Vector3D m_vTarget;
	float m_flPhase1Time = 0.0f;
	float m_flPhase1To2Time = 0.0f;
	float m_flPhase2Time = 0.0f;
	float m_flPhase2To3Time = 0.0f;
	float m_flPhase3ToTarTime = 0.0f;

	// Elapsed time since spawn at the first think; 0 until captured.
	float m_flPathStartElapsed = 0.0f;
	Vector3D m_vPathStart;

	bool m_bWiggle = false;
	bool m_bWiggleStarted = false;
	float m_flEaseIn = 0.0f;
	float m_flEaseOut = 0.0f;
	float m_flWiggleStartDelay = 0.0f;
	float m_flWiggleDuration = 0.0f;
	Vector3D m_vWiggleVert;
	Vector3D m_vWiggleHorz;
	float m_flAmpVert = 0.0f;
	float m_flAmpHorz = 0.0f;
	float m_flScaleVert = 0.0f;
	float m_flScaleHorz = 0.0f;
	float m_flRateVert = 0.0f;
	float m_flRateHorz = 0.0f;
	// S21 layout: four vertical then four horizontal half-periods, then the
	// two reversal counters. Index 4 of either run reads the next field.
	struct
	{
		float m_flTimes[8];
		int m_nReversalsVert;
		int m_nReversalsHorz;
	} m_Wiggle = {};

	int m_nDiagThinks = 0;
};

static SDKEntityMap<MissileExpandContractState_t> s_missileStates(ESide::Server, "missile_ec.srv");

static void (*v_Missile_VelocityThink)(void* pMissile) = nullptr;
static void (*v_Missile_MainThink)(void* pMissile) = nullptr;
static void (*v_CCollisionProperty_SetSolidFlags)(void* pCollision, int flags) = nullptr;
static void (*v_CBaseEntity_SetNextThink)(void* pEntity, float flNextThink, const char* pszContext) = nullptr;
static void (*v_CBaseEntity_CalcAbsoluteVelocity)(void* pEntity) = nullptr;
static void (*v_Missile_InitExpandContract)(void* pMissile, const Vector3D* pPhase1, const Vector3D* pPhase2,
	float flPhase1Time, float flPhase1To2Time, float flPhase2Time, float flPhase2To3Time,
	const Vector3D* pTarget, bool bHoming) = nullptr;
static void (*v_Missile_Explode)(void* pMissile, void* pTrace) = nullptr;
static void (*v_Missile_RegisterScriptClass)(void) = nullptr;
static void (*v_CBaseEntity_SetAbsVelocity)(void* pEntity, const Vector3D* pVelocity) = nullptr;
static void (*v_CBaseEntity_SetAbsAngles)(void* pEntity, const QAngle* pAngles) = nullptr;
static ScriptClassDescriptor_t* s_pMissileScriptStruct = nullptr;

static std::mt19937 s_wiggleRng{ std::random_device{}() };

static float Missile_RandomFloat(const float flMin, const float flMax)
{
	return std::uniform_real_distribution<float>(0.0f, 1.0f)(s_wiggleRng) * (flMax - flMin) + flMin;
}

static float Missile_RandomSign(void)
{
	return std::uniform_int_distribution<int>(0, 1)(s_wiggleRng) ? 1.0f : -1.0f;
}

static inline float Missile_Time(void)
{
	return gpGlobals ? gpGlobals->curTime : 0.0f;
}

static inline float Missile_FrameTime(void)
{
	return gpGlobals ? *reinterpret_cast<const float*>(
		reinterpret_cast<const uint8_t*>(gpGlobals) + GLOBALS_OFF_FRAMETIME) : 0.0f;
}

template <typename T>
static inline T& Missile_Field(void* pMissile, const ptrdiff_t off)
{
	return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(pMissile) + off);
}

static inline Vector3D Missile_DirTo(const Vector3D& from, const Vector3D& to)
{
	const Vector3D d = to - from;
	const float len = fmaxf(sqrtf(d.x * d.x + d.y * d.y + d.z * d.z), MISSILE_EPS);
	return d * (1.0f / len);
}

static inline float Missile_TotalPathTime(const MissileExpandContractState_t& st)
{
	return st.m_flPhase1Time + st.m_flPhase1To2Time + st.m_flPhase2Time
		+ st.m_flPhase2To3Time + st.m_flPhase3ToTarTime;
}

//-----------------------------------------------------------------------------
// Position along the path: phase 1, blend to phase 2, phase 2, blend to
// phase 3, blend into the target direction, then straight at the target.
// Each blend integrates a linear velocity ramp.
//-----------------------------------------------------------------------------
static Vector3D Missile_PathPosition(const float speed, const Vector3D& start,
	const Vector3D& p1, const Vector3D& p2, const Vector3D& p3,
	const float t1, const float t12, const float t2, const float t23, const float t3tar,
	const Vector3D& target, const float time)
{
	Vector3D pos = start;
	const Vector3D v1 = p1 * speed;
	const Vector3D v2 = p2 * speed;
	const Vector3D v3 = p3 * speed;

	float step = fminf(time, t1);
	float rem = time - step;
	pos += v1 * step;
	if (rem <= 0.0f)
		return pos;

	step = fminf(rem, t12);
	rem -= step;
	if (t12 != 0.0f)
		pos += (v2 - v1) * (0.5f / t12 * step * step) + v1 * step;
	if (rem <= 0.0f)
		return pos;

	step = fminf(rem, t2);
	rem -= step;
	pos += v2 * step;
	if (rem <= 0.0f)
		return pos;

	step = fminf(rem, t23);
	rem -= step;
	if (t23 != 0.0f)
		pos += (v3 - v2) * (0.5f / t23 * step * step) + v2 * step;
	if (rem <= 0.0f)
		return pos;

	step = fminf(rem, t3tar);
	rem -= step;
	if (t3tar != 0.0f)
	{
		const Vector3D vTar = Missile_DirTo(pos, target) * speed;
		pos += (vTar - v3) * (0.5f / t3tar * step * step) + v3 * step;
	}
	if (rem <= 0.0f)
		return pos;

	pos += Missile_DirTo(pos, target) * (rem * speed);
	return pos;
}

static Vector3D Missile_PathPosition(const MissileExpandContractState_t& st, const float time)
{
	return Missile_PathPosition(st.m_flSpeed, st.m_vPathStart, st.m_vPhase1, st.m_vPhase2, st.m_vPhase3,
		st.m_flPhase1Time, st.m_flPhase1To2Time, st.m_flPhase2Time, st.m_flPhase2To3Time,
		st.m_flPhase3ToTarTime, st.m_vTarget, time);
}

// Velocity by phase, used for the target blend where the path turns on the live origin.
static Vector3D Missile_PhaseVelocity(const MissileExpandContractState_t& st, const Vector3D& origin, const float time)
{
	const float end1 = st.m_flPhase1Time;
	if (end1 > time)
		return st.m_vPhase1 * st.m_flSpeed;

	const float end12 = end1 + st.m_flPhase1To2Time;
	if (end12 > time)
	{
		const float frac = st.m_flPhase1To2Time > 0.0f ? (time - end1) / st.m_flPhase1To2Time : 1.0f;
		return (st.m_vPhase1 + (st.m_vPhase2 - st.m_vPhase1) * frac) * st.m_flSpeed;
	}

	const float end2 = end12 + st.m_flPhase2Time;
	if (end2 > time)
		return st.m_vPhase2 * st.m_flSpeed;

	const float end23 = end2 + st.m_flPhase2To3Time;
	if (end23 > time)
	{
		const float frac = st.m_flPhase2To3Time > 0.0f ? (time - end2) / st.m_flPhase2To3Time : 1.0f;
		return (st.m_vPhase2 + (st.m_vPhase3 - st.m_vPhase2) * frac) * st.m_flSpeed;
	}

	const Vector3D toTarget = Missile_DirTo(origin, st.m_vTarget);
	const float endTar = end23 + st.m_flPhase3ToTarTime;
	if (endTar <= time)
		return toTarget * st.m_flSpeed;

	Vector3D dir;
	if (end23 == endTar)
	{
		dir = time < endTar ? st.m_vPhase3 : toTarget;
	}
	else
	{
		const float frac = fminf(fmaxf((time - end23) / (endTar - end23), 0.0f), 1.0f);
		dir = st.m_vPhase3 + (toTarget - st.m_vPhase3) * frac;
	}
	return dir * st.m_flSpeed;
}

//-----------------------------------------------------------------------------
// Wiggle: a vertical and a sideways offset that bounce between +-amplitude.
// Each zero crossing re-rolls the rate from the next half-period and eases
// the amplitude in until the wiggle duration ends, then out.
//-----------------------------------------------------------------------------
static float Missile_WiggleEase(const MissileExpandContractState_t& st, const float scale, const float time)
{
	if (time - st.m_flWiggleStartDelay < st.m_flWiggleDuration)
		return fminf(scale + st.m_flEaseIn, 1.0f);
	return fmaxf(scale - st.m_flEaseOut, 0.0f);
}

static float Missile_WiggleRate(const float amp, const float scale, const float halfPeriod, const float rate)
{
	return fabsf(amp * scale / halfPeriod) * (rate / fabsf(rate));
}

static float Missile_WiggleHalfPeriod(const MissileExpandContractState_t& st, const int first, const int reversals)
{
	float half;
	memcpy(&half, &st.m_Wiggle.m_flTimes[first] + (reversals < 4 ? reversals : 4), sizeof(half));
	return half;
}

static Vector3D Missile_Wiggle(MissileExpandContractState_t& st, const float time, const float dt, const Vector3D& vel)
{
	const Vector3D dir = vel * (1.0f / fmaxf(sqrtf(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z), MISSILE_EPS));
	Vector3D side(dir.y, -dir.x, 0.0f);
	side *= 1.0f / fmaxf(sqrtf(side.x * side.x + side.y * side.y), MISSILE_EPS);

	const Vector3D oldVert = st.m_vWiggleVert;
	const Vector3D oldHorz = st.m_vWiggleHorz;
	st.m_vWiggleVert.z += st.m_flRateVert * dt;
	st.m_vWiggleHorz += side * (st.m_flRateHorz * dt);

	const float vertLimit = st.m_flAmpVert * st.m_flScaleVert;
	if (st.m_vWiggleVert != Vector3D(0, 0, 0) && st.m_vWiggleVert.LengthSqr() >= vertLimit * vertLimit)
		st.m_flRateVert = -st.m_flRateVert;

	const float horzLimit = st.m_flAmpHorz * st.m_flScaleHorz;
	if (st.m_vWiggleHorz != Vector3D(0, 0, 0) && st.m_vWiggleHorz.LengthSqr() >= horzLimit * horzLimit)
		st.m_flRateHorz = -st.m_flRateHorz;

	if (st.m_bWiggleStarted && DotProduct(oldVert, st.m_vWiggleVert) <= 0.0f)
	{
		st.m_flScaleVert = Missile_WiggleEase(st, st.m_flScaleVert, time);
		if (st.m_flScaleVert == 0.0f)
		{
			st.m_vWiggleVert.Init();
			st.m_flRateVert = 0.0f;
		}
		else
		{
			st.m_flRateVert = Missile_WiggleRate(st.m_flAmpVert, st.m_flScaleVert,
				Missile_WiggleHalfPeriod(st, 0, st.m_Wiggle.m_nReversalsVert), st.m_flRateVert);
			st.m_Wiggle.m_nReversalsVert++;
		}
	}

	if (st.m_bWiggleStarted && DotProduct(oldHorz, st.m_vWiggleHorz) <= 0.0f)
	{
		st.m_flScaleHorz = Missile_WiggleEase(st, st.m_flScaleHorz, time);
		if (st.m_flScaleHorz == 0.0f)
		{
			st.m_vWiggleHorz.Init();
			st.m_flRateHorz = 0.0f;
		}
		else
		{
			st.m_flRateHorz = Missile_WiggleRate(st.m_flAmpHorz, st.m_flScaleHorz,
				Missile_WiggleHalfPeriod(st, 4, st.m_Wiggle.m_nReversalsHorz), st.m_flRateHorz);
			st.m_Wiggle.m_nReversalsHorz++;
		}
	}

	if (!st.m_bWiggleStarted && time >= st.m_flWiggleStartDelay)
		st.m_bWiggleStarted = true;

	// Component-wise, as S21 combines them.
	return Vector3D(
		side.x * st.m_vWiggleHorz.x,
		side.y * st.m_vWiggleHorz.y,
		side.z * st.m_vWiggleHorz.z + st.m_vWiggleVert.z) * st.m_flSpeed;
}

static Vector3D Missile_Velocity(MissileExpandContractState_t& st, const Vector3D& origin, const float time, const float dt)
{
	const float blendStart = st.m_flPhase1Time + st.m_flPhase1To2Time + st.m_flPhase2Time + st.m_flPhase2To3Time;

	if (time > blendStart + st.m_flPhase3ToTarTime)
		return Missile_DirTo(origin, st.m_vTarget) * st.m_flSpeed;

	if (time > blendStart)
	{
		Vector3D vel = Missile_PhaseVelocity(st, origin, time + dt);
		if (st.m_bWiggle)
			vel += Missile_Wiggle(st, time, dt, vel);
		return vel;
	}

	if (dt == 0.0f)
		return Missile_PhaseVelocity(st, origin, time);

	Vector3D next = Missile_PathPosition(st, time + dt);
	const Vector3D cur = Missile_PathPosition(st, time);
	const float invDt = 1.0f / dt;
	if (st.m_bWiggle)
		next += Missile_Wiggle(st, time, dt, (next - cur) * invDt) * dt;
	return (next - cur) * invDt;
}

static void Missile_SetNotSolid(void* pMissile, const bool bNotSolid)
{
	void* const pColl = reinterpret_cast<uint8_t*>(pMissile) + MISSILE_OFF_COLLISION;
	const int flags = *reinterpret_cast<const int*>(reinterpret_cast<const uint8_t*>(pColl) + 0x28);
	v_CCollisionProperty_SetSolidFlags(pColl, bNotSolid ? (flags | FSOLID_NOT_SOLID_BIT) : (flags & ~FSOLID_NOT_SOLID_BIT));
}

static inline bool Missile_InGrace(void* pMissile)
{
	return Missile_Field<float>(pMissile, MISSILE_OFF_GRACE_END) != 0.0f;
}

static MissileExpandContractState_t* Missile_ActiveState(void* pMissile)
{
	if (!bridge_missile_expand_contract.GetBool())
		return nullptr;
	MissileExpandContractState_t* const pSt = s_missileStates.Find(pMissile);
	return (pSt && pSt->m_bActive) ? pSt : nullptr;
}

//-----------------------------------------------------------------------------
// Main think of a missile on the S21 path: grace expiry, then detonation once
// it is past the end of its path and moving away from the target. Velocity
// belongs to the velocity think.
//-----------------------------------------------------------------------------
static void Missile_MainThink(void* pMissile)
{
	MissileExpandContractState_t* const pSt = Missile_ActiveState(pMissile);
	if (!pSt || !v_CBaseEntity_SetNextThink || !v_Missile_Explode || !v_CBaseEntity_CalcAbsoluteVelocity)
	{
		v_Missile_MainThink(pMissile);
		return;
	}

	const float now = Missile_Time();
	Missile_Field<float>(pMissile, MISSILE_OFF_LAST_THINK) = now;

	float& graceEnd = Missile_Field<float>(pMissile, MISSILE_OFF_GRACE_END);
	if (graceEnd != 0.0f && now > graceEnd)
	{
		Missile_SetNotSolid(pMissile, false);
		graceEnd = 0.0f;
	}

	const float elapsed = now - Missile_Field<float>(pMissile, MISSILE_OFF_SPAWN_TIME);
	if (graceEnd == 0.0f && elapsed - pSt->m_flPathStartElapsed > Missile_TotalPathTime(*pSt))
	{
		v_CBaseEntity_CalcAbsoluteVelocity(pMissile);
		const Vector3D& absVelocity = Missile_Field<Vector3D>(pMissile, MISSILE_OFF_ABS_VELOCITY);
		const Vector3D origin = reinterpret_cast<CBaseEntity*>(pMissile)->Diag_AbsOrigin();
		if (DotProduct(origin - pSt->m_vTarget, absVelocity) >= 0.0f)
		{
			if (bridge_missile_expand_contract_diag.GetInt() > 0)
				Msg(eDLL_T::SERVER, "[MISSILE-EC] ent=%p past target: pos=(%.0f %.0f %.0f) miss=%.0f\n",
					pMissile, origin.x, origin.y, origin.z, (origin - pSt->m_vTarget).Length());
			s_missileStates.Erase(pMissile);
			v_Missile_Explode(pMissile, nullptr);
			return;
		}
	}

	v_CBaseEntity_SetNextThink(pMissile, now, nullptr);
}

//-----------------------------------------------------------------------------
// Runs after the S3 velocity think has rescheduled itself.
//-----------------------------------------------------------------------------
static void Missile_VelocityThink(void* pMissile)
{
	v_Missile_VelocityThink(pMissile);

	if (!v_CBaseEntity_SetAbsVelocity)
		return;

	MissileExpandContractState_t* const pSt = Missile_ActiveState(pMissile);
	if (!pSt || !Missile_Field<uint8_t>(pMissile, MISSILE_OFF_LAUNCHED))
		return;

	MissileExpandContractState_t& st = *pSt;
	CBaseEntity* const pEnt = reinterpret_cast<CBaseEntity*>(pMissile);
	const float now = Missile_Time();
	const float dt = Missile_FrameTime();
	const float elapsed = now - Missile_Field<float>(pMissile, MISSILE_OFF_SPAWN_TIME);

	if (st.m_flPathStartElapsed == 0.0f)
	{
		Missile_Field<float>(pMissile, MISSILE_OFF_SPEED) = st.m_flSpeed;
		st.m_flPathStartElapsed = elapsed;
		st.m_vPathStart = pEnt->Diag_AbsOrigin();

		if (bridge_missile_expand_contract_diag.GetInt() > 0)
			Msg(eDLL_T::SERVER, "[MISSILE-EC] ent=%p path start=(%.0f %.0f %.0f) target=(%.0f %.0f %.0f) dist=%.0f speed=%.0f total=%.3f\n",
				pMissile, st.m_vPathStart.x, st.m_vPathStart.y, st.m_vPathStart.z,
				st.m_vTarget.x, st.m_vTarget.y, st.m_vTarget.z, (st.m_vTarget - st.m_vPathStart).Length(),
				st.m_flSpeed, Missile_TotalPathTime(st));
	}

	const Vector3D origin = pEnt->Diag_AbsOrigin();
	Vector3D vel = Missile_Velocity(st, origin, elapsed - st.m_flPathStartElapsed, dt);

	// Grace: slide along whatever the next step would hit instead of into it.
	if (Missile_InGrace(pMissile))
	{
		trace_t tr;
		memset(&tr, 0, sizeof(tr));
		tr.fraction = 1.0f;
		if (TriggerGravity_TraceLine(origin, origin + vel * dt, MISSILE_TRACE_MASK, pMissile, 0, &tr)
			&& tr.fraction < 1.0f)
		{
			vel -= tr.plane.normal * DotProduct(vel, tr.plane.normal);
		}
	}

	v_CBaseEntity_SetAbsVelocity(pMissile, &vel);
	if (v_CBaseEntity_SetAbsAngles)
	{
		QAngle angles;
		VectorAngles(vel, angles);
		v_CBaseEntity_SetAbsAngles(pMissile, &angles);
	}

	const int every = bridge_missile_expand_contract_diag.GetInt();
	if (every > 0 && (st.m_nDiagThinks++ % every) == 0)
	{
		Msg(eDLL_T::SERVER, "[MISSILE-EC] ent=%p t=%.3f/%.3f pos=(%.0f %.0f %.0f) vel=(%.0f %.0f %.0f) grace=%d wig=%d\n",
			pMissile, elapsed - st.m_flPathStartElapsed, Missile_TotalPathTime(st),
			origin.x, origin.y, origin.z, vel.x, vel.y, vel.z,
			Missile_InGrace(pMissile) ? 1 : 0, st.m_bWiggle ? 1 : 0);
	}
}

//-----------------------------------------------------------------------------
// Script argument helpers.
//-----------------------------------------------------------------------------
static void* Missile_This(HSQUIRRELVM v)
{
	void* pEnt = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
		return nullptr;
	return pEnt;
}

static bool Missile_GetFloats(HSQUIRRELVM v, const SQInteger first, const int count, float* pOut)
{
	for (int i = 0; i < count; ++i)
	{
		SQFloat f = 0.0f;
		if (SQ_FAILED(sq_getfloat(v, first + i, &f)) || !std::isfinite(f))
			return false;
		pOut[i] = f;
	}
	return true;
}

static bool Missile_GetVector(HSQUIRRELVM v, const SQInteger idx, Vector3D& out)
{
	const SQVector3D* p = nullptr;
	if (SQ_FAILED(sq_getvector(v, idx, &p)) || !p
		|| !std::isfinite(p->x) || !std::isfinite(p->y) || !std::isfinite(p->z))
		return false;
	out.Init(p->x, p->y, p->z);
	return true;
}

static constexpr float MISSILE_MAX_SPEED = 100000.0f;
static constexpr float MISSILE_MAX_PHASE_TIME = 60.0f;

static bool Missile_ValidPhaseTimes(const float* pTimes, const int count)
{
	for (int i = 0; i < count; ++i)
	{
		if (pTimes[i] < 0.0f || pTimes[i] > MISSILE_MAX_PHASE_TIME)
			return false;
	}
	return true;
}

//-----------------------------------------------------------------------------
// missile.InitMissileExpandContract( speed, phase1Vec, phase2Vec, phase3Vec,
//     phase1Time, phase1To2Time, phase2Time, phase2To3Time, phase3ToTarTime, target, bool )
//-----------------------------------------------------------------------------
static SQRESULT Script_InitMissileExpandContract(HSQUIRRELVM v)
{
	void* const pMissile = Missile_This(v);
	if (!pMissile)
		return SQ_ERROR;

	float speed = 0.0f;
	Vector3D p1, p2, p3, target;
	float times[5];
	SQBool bUnused = false;
	if (!Missile_GetFloats(v, 2, 1, &speed) || !Missile_GetVector(v, 3, p1) || !Missile_GetVector(v, 4, p2)
		|| !Missile_GetVector(v, 5, p3) || !Missile_GetFloats(v, 6, 5, times) || !Missile_GetVector(v, 11, target))
	{
		v_SQVM_ScriptError("InitMissileExpandContract: non-numeric or non-finite argument\n");
		return SQ_ERROR;
	}
	sq_getbool(v, 12, &bUnused);

	if (speed < 0.0f || speed > MISSILE_MAX_SPEED || !Missile_ValidPhaseTimes(times, 5))
	{
		v_SQVM_ScriptError("InitMissileExpandContract: speed or phase time out of range\n");
		return SQ_ERROR;
	}

	static bool s_bFirst = false;
	if (!s_bFirst)
	{
		s_bFirst = true;
		Msg(eDLL_T::SERVER, "[MISSILE-EC] InitMissileExpandContract first call ent=%p speed=%.0f total=%.2f\n",
			pMissile, speed, times[0] + times[1] + times[2] + times[3] + times[4]);
	}

	if (!v_Missile_InitExpandContract)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER, "[MISSILE-EC] missile path init unresolved -- InitMissileExpandContract no-op\n");
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	// Installs the S3 velocity think; the S21 velocity replaces its output.
	v_Missile_InitExpandContract(pMissile, &p1, &p2, times[0], times[1], times[2], times[3], &target, false);

	MissileExpandContractState_t& st = s_missileStates[pMissile];
	st.m_bActive = true;
	st.m_flSpeed = speed;
	st.m_vPhase1 = p1;
	st.m_vPhase2 = p2;
	st.m_vPhase3 = p3;
	st.m_vTarget = target;
	st.m_flPhase1Time = times[0];
	st.m_flPhase1To2Time = times[1];
	st.m_flPhase2Time = times[2];
	st.m_flPhase2To3Time = times[3];
	st.m_flPhase3ToTarTime = times[4];
	st.m_flPathStartElapsed = 0.0f;

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// missile.MissilePath_GetExpandContractPositionAtTime( speed, startPos, phase1Vec,
//     phase2Vec, phase3Vec, phase1Time, phase1To2Time, phase2Time, phase2To3Time,
//     phase3ToTarTime, target, time )
//-----------------------------------------------------------------------------
static SQRESULT Script_MissilePath_GetExpandContractPositionAtTime(HSQUIRRELVM v)
{
	float speed = 0.0f;
	Vector3D start, p1, p2, p3, target;
	float times[5];
	float time = 0.0f;
	if (!Missile_GetFloats(v, 2, 1, &speed) || !Missile_GetVector(v, 3, start) || !Missile_GetVector(v, 4, p1)
		|| !Missile_GetVector(v, 5, p2) || !Missile_GetVector(v, 6, p3) || !Missile_GetFloats(v, 7, 5, times)
		|| !Missile_GetVector(v, 12, target) || !Missile_GetFloats(v, 13, 1, &time))
	{
		v_SQVM_ScriptError("MissilePath_GetExpandContractPositionAtTime: non-numeric or non-finite argument\n");
		return SQ_ERROR;
	}

	if (speed < 0.0f || speed > MISSILE_MAX_SPEED || !Missile_ValidPhaseTimes(times, 5))
	{
		v_SQVM_ScriptError("MissilePath_GetExpandContractPositionAtTime: speed or phase time out of range\n");
		return SQ_ERROR;
	}

	const Vector3D pos = Missile_PathPosition(speed, start, p1, p2, p3,
		times[0], times[1], times[2], times[3], times[4], target, time);
	const SQVector3D out(pos.x, pos.y, pos.z);
	sq_pushvector(v, &out);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// missile.InitMissileWiggleSettings( horizontalVelocityMax, horizontalVelocityMin,
//     horizontalTimeMax, horizontalTimeMin, verticalTimeMax, verticalTimeMin,
//     verticalVelocityMax, verticalVelocityMin, easeIn, easeOut, startDelay, duration )
//-----------------------------------------------------------------------------
static SQRESULT Script_InitMissileWiggleSettings(HSQUIRRELVM v)
{
	void* const pMissile = Missile_This(v);
	if (!pMissile)
		return SQ_ERROR;

	float w[12];
	if (!Missile_GetFloats(v, 2, 12, w))
	{
		v_SQVM_ScriptError("InitMissileWiggleSettings: non-numeric or non-finite argument\n");
		return SQ_ERROR;
	}

	const float hVelMax = w[0], hVelMin = w[1], hTimeMax = w[2], hTimeMin = w[3];
	const float vTimeMax = w[4], vTimeMin = w[5], vVelMax = w[6], vVelMin = w[7];

	// Half-periods divide the rate; a non-positive one would blow it up.
	if (hTimeMin <= 0.0f || vTimeMin <= 0.0f || hTimeMax <= 0.0f || vTimeMax <= 0.0f)
	{
		v_SQVM_ScriptError("InitMissileWiggleSettings: wiggle times must be positive\n");
		return SQ_ERROR;
	}

	MissileExpandContractState_t& st = s_missileStates[pMissile];
	st.m_bWiggle = true;
	st.m_bWiggleStarted = false;
	st.m_flEaseIn = w[8];
	st.m_flEaseOut = w[9];
	st.m_flWiggleStartDelay = w[10];
	st.m_flWiggleDuration = w[11];
	st.m_vWiggleVert.Init();
	st.m_vWiggleHorz.Init();

	st.m_flAmpVert = Missile_RandomSign() * Missile_RandomFloat(vVelMin, vVelMax);
	st.m_flAmpHorz = Missile_RandomSign() * Missile_RandomFloat(hVelMin, hVelMax);
	st.m_flScaleVert = st.m_flEaseIn;
	st.m_flScaleHorz = st.m_flEaseIn;

	const float vHalf = Missile_RandomFloat(vTimeMin, vTimeMax) * 2.0f;
	const float hHalf = Missile_RandomFloat(hTimeMin, hTimeMax) * 2.0f;
	st.m_flRateVert = st.m_flScaleVert * st.m_flAmpVert / vHalf;
	st.m_flRateHorz = st.m_flScaleHorz * st.m_flAmpHorz / hHalf;

	for (int i = 0; i < 4; ++i)
	{
		st.m_Wiggle.m_flTimes[i] = Missile_RandomFloat(vTimeMin, vTimeMax) * 2.0f;
		st.m_Wiggle.m_flTimes[4 + i] = Missile_RandomFloat(hTimeMin, hTimeMax) * 2.0f;
	}
	st.m_Wiggle.m_nReversalsVert = 0;
	st.m_Wiggle.m_nReversalsHorz = 0;

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// missile.SetGracePeriod( seconds ): not solid to characters until it expires.
//-----------------------------------------------------------------------------
static SQRESULT Script_SetGracePeriod(HSQUIRRELVM v)
{
	void* const pMissile = Missile_This(v);
	if (!pMissile)
		return SQ_ERROR;

	float grace = 0.0f;
	if (!Missile_GetFloats(v, 2, 1, &grace) || grace < 0.0f || grace > MISSILE_MAX_PHASE_TIME)
	{
		v_SQVM_ScriptError("SetGracePeriod: expected a finite duration in [0, 60]\n");
		return SQ_ERROR;
	}

	if (!v_CCollisionProperty_SetSolidFlags)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER, "[MISSILE-EC] SetSolidFlags unresolved -- SetGracePeriod no-op\n");
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	// The CMissile main think expires this field for every missile.
	Missile_Field<float>(pMissile, MISSILE_OFF_GRACE_END) = Missile_Time() + grace;
	Missile_SetNotSolid(pMissile, true);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Multi-target grid finder.
//-----------------------------------------------------------------------------
struct MissileTarget_t
{
	Vector3D m_vPos;
	Vector3D m_vNormal;
	float m_flDelay;
};

static void Missile_Trace(const Vector3D& start, const Vector3D& end, void* pOwner, const int group, trace_t& tr)
{
	memset(&tr, 0, sizeof(tr));
	tr.endpos = end;
	tr.fraction = 1.0f;
	TriggerGravity_TraceLine(start, end, MISSILE_TRACE_MASK, pOwner, group, &tr, 0, 0);
}

// Lift, move, then drop to the ground. A move blocked within 20u is retried
// from above the blocked point when bNudge is set.
static void Missile_TargetStep(const Vector3D& start, const Vector3D& lift, const Vector3D& move, void* pOwner,
	const bool bNudge, Vector3D& outPos, Vector3D& outNormal)
{
	trace_t up, across, down;
	Missile_Trace(start, start + lift, pOwner, COLLISION_GROUP_BLOCK_WEAPONS_S3, up);
	Missile_Trace(up.endpos, up.endpos + move, pOwner, COLLISION_GROUP_BLOCK_WEAPONS_S3, across);

	Vector3D dropStart = across.endpos;
	const Vector3D moved = up.endpos - across.endpos;
	if (bNudge && sqrtf(moved.x * moved.x + moved.y * moved.y + moved.z * moved.z) < 20.0f)
		dropStart = up.endpos + move - Vector3D(0.0f, 0.0f, 10.0f);

	const Vector3D dropEnd(across.endpos.x, across.endpos.y, across.endpos.z - 3000.0f);
	Missile_Trace(dropStart, dropEnd, pOwner, COLLISION_GROUP_BLOCK_WEAPONS_S3, down);

	outPos = down.fraction == 1.0f ? start + move : down.endpos;
	outNormal = down.plane.normal;
}

static void Missile_FindTargets(const Vector3D& attackPos, const Vector3D& attackDir, void* pOwner,
	const int rows, const int columns, const float stepForward, const float stepSide, const float stepHeight,
	float delay, const float inRowDelay, const float rowToRowDelay, const float maxRange, const float minRange,
	CUtlVector<MissileTarget_t>& out)
{
	trace_t tr;
	Missile_Trace(attackPos, attackPos + attackDir * maxRange, pOwner, COLLISION_GROUP_PLAYER_S3, tr);

	Vector3D pos = tr.endpos;
	Vector3D normal = tr.plane.normal;
	if (tr.fraction >= 1.0f)
	{
		const Vector3D aim = tr.endpos;
		Missile_Trace(aim, Vector3D(aim.x, aim.y, aim.z - maxRange - 3000.0f), pOwner, COLLISION_GROUP_PLAYER_S3, tr);
		pos = tr.endpos;
		normal = tr.plane.normal;
	}

	Vector3D flat(attackDir.x, attackDir.y, 0.0f);
	flat *= 1.0f / fmaxf(sqrtf(flat.x * flat.x + flat.y * flat.y), MISSILE_EPS);

	// Too close: push the grid out to the minimum range.
	if ((pos - attackPos).Length() < minRange)
	{
		const Vector3D push = flat * minRange;
		Vector3D stepPos = pos, stepNormal = normal;
		Missile_TargetStep(attackPos, Vector3D(0.0f, 0.0f, stepHeight), push, pOwner, false, stepPos, stepNormal);
		pos = stepPos;
		normal = stepNormal;

		if ((pos - attackPos).Length() < minRange)
		{
			const Vector3D p = attackPos + push;
			Missile_Trace(p, Vector3D(p.x, p.y, p.z - maxRange - 200.0f), pOwner, COLLISION_GROUP_PLAYER_S3, tr);
			if (tr.fraction != 1.0f)
			{
				pos = tr.endpos;
				normal = tr.plane.normal;
			}
			else
			{
				trace_t ceiling;
				Missile_Trace(p, Vector3D(p.x, p.y, p.z + maxRange + 200.0f), pOwner, COLLISION_GROUP_PLAYER_S3, ceiling);
				Missile_Trace(ceiling.endpos + Vector3D(0.0f, 0.0f, stepHeight), ceiling.endpos, pOwner, COLLISION_GROUP_PLAYER_S3, tr);
				pos = tr.endpos;
				normal = tr.plane.normal;
			}
		}
	}

	const Vector3D forward = flat * stepForward;
	const Vector3D right(flat.y, -flat.x, 0.0f);
	const Vector3D leftStep = right * -stepSide;
	const Vector3D rightStep = right * stepSide;
	const Vector3D lift(0.0f, 0.0f, stepHeight);

	MissileTarget_t rowBase{ pos, normal, delay };
	for (int row = 0; row < rows; ++row)
	{
		MissileTarget_t leftCur = rowBase;
		MissileTarget_t rightCur = rowBase;
		for (int col = 0; col < columns; ++col)
		{
			if (col == 0)
			{
				rowBase.m_flDelay = delay;
				leftCur = rowBase;
				rightCur = rowBase;
				out.AddToTail(rowBase);
				continue;
			}

			Missile_TargetStep(leftCur.m_vPos, lift, leftStep, pOwner, true, leftCur.m_vPos, leftCur.m_vNormal);
			Missile_TargetStep(rightCur.m_vPos, lift, rightStep, pOwner, true, rightCur.m_vPos, rightCur.m_vNormal);
			leftCur.m_flDelay = delay + inRowDelay;
			delay = delay + inRowDelay + inRowDelay;
			rightCur.m_flDelay = delay;
			out.AddToTail(leftCur);
			out.AddToTail(rightCur);
		}

		Missile_TargetStep(rowBase.m_vPos, lift, forward, pOwner, true, rowBase.m_vPos, rowBase.m_vNormal);
		delay += rowToRowDelay;
	}
}

// Field order must match `global struct WeaponMissileMultipleTargetData` in init.nut.
enum
{
	kMissileTargetField_Pos = 0,
	kMissileTargetField_Normal = 1,
	kMissileTargetField_Delay = 2,
	kMissileTargetField_Count = 3,
};

static constexpr int MISSILE_MAX_GRID_STEPS = 16;
static constexpr float MISSILE_MAX_RANGE = 65536.0f;

//-----------------------------------------------------------------------------
// weapon.GetWeaponMissileMultipleTargets( attackPos, attackDir, owner, forwardSteps,
//     sideSteps, stepForward, stepSide, stepHeight, initialDelay, inRowDelay,
//     rowToRowDelay, maxAttackRange, minAttackRange )
//-----------------------------------------------------------------------------
static SQRESULT Script_GetWeaponMissileMultipleTargets(HSQUIRRELVM v)
{
	Vector3D attackPos, attackDir;
	SQInteger rows = 0, columns = 0;
	float f[8];
	if (!Missile_GetVector(v, 2, attackPos) || !Missile_GetVector(v, 3, attackDir)
		|| SQ_FAILED(sq_getinteger(v, 5, &rows)) || SQ_FAILED(sq_getinteger(v, 6, &columns))
		|| !Missile_GetFloats(v, 7, 8, f))
	{
		v_SQVM_ScriptError("GetWeaponMissileMultipleTargets: non-numeric or non-finite argument\n");
		return SQ_ERROR;
	}

	if (rows < 0 || rows > MISSILE_MAX_GRID_STEPS || columns < 0 || columns > MISSILE_MAX_GRID_STEPS
		|| fabsf(f[6]) > MISSILE_MAX_RANGE || fabsf(f[7]) > MISSILE_MAX_RANGE)
	{
		v_SQVM_ScriptError("GetWeaponMissileMultipleTargets: grid or range out of bounds\n");
		return SQ_ERROR;
	}

	void* const pOwner = ServerScript_EntityPtrFromStackIdx(v, 4);

	static bool s_bFirst = false;
	if (!s_bFirst)
	{
		s_bFirst = true;
		Msg(eDLL_T::SERVER, "[MISSILE-EC] GetWeaponMissileMultipleTargets first call owner=%p grid=%dx%d\n",
			pOwner, static_cast<int>(rows), static_cast<int>(columns));
	}

	CUtlVector<MissileTarget_t> targets;
	Missile_FindTargets(attackPos, attackDir, pOwner, static_cast<int>(rows), static_cast<int>(columns),
		f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], targets);

	if (bridge_missile_expand_contract_diag.GetInt() > 0 && targets.Count() > 0)
	{
		const Vector3D& t0 = targets[0].m_vPos;
		Msg(eDLL_T::SERVER, "[MISSILE-EC] targets n=%d attackPos=(%.0f %.0f %.0f) attackDir=(%.3f %.3f %.3f) first=(%.0f %.0f %.0f) dist=%.0f\n",
			targets.Count(), attackPos.x, attackPos.y, attackPos.z, attackDir.x, attackDir.y, attackDir.z,
			t0.x, t0.y, t0.z, (t0 - attackPos).Length());
	}

	sq_newarray(v, 0);
	if (!v_sq_newstruct || !v_sq_setstructfield)
	{
		Warning(eDLL_T::SERVER, "[MISSILE-EC] struct push unresolved -- GetWeaponMissileMultipleTargets returns empty\n");
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	for (int i = 0; i < targets.Count(); ++i)
	{
		const MissileTarget_t& t = targets[i];
		v_sq_newstruct(v, kMissileTargetField_Count);

		const SQVector3D pos(t.m_vPos.x, t.m_vPos.y, t.m_vPos.z);
		sq_pushvector(v, &pos);
		v_sq_setstructfield(v, kMissileTargetField_Pos);

		const SQVector3D nrm(t.m_vNormal.x, t.m_vNormal.y, t.m_vNormal.z);
		sq_pushvector(v, &nrm);
		v_sq_setstructfield(v, kMissileTargetField_Normal);

		sq_pushfloat(v, t.m_flDelay);
		v_sq_setstructfield(v, kMissileTargetField_Delay);

		sq_arrayappend(v, -2);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void MissileExpandContract_RegisterWeaponFuncs(ScriptClassDescriptor_t* weaponStruct)
{
	if (!weaponStruct)
		return;

	weaponStruct->AddFunction("GetWeaponMissileMultipleTargets",
		"Script_GetWeaponMissileMultipleTargets",
		"Ground targets for a forward grid of missiles, with per-missile launch delays",
		"array< WeaponMissileMultipleTargetData >",
		"vector attackPos, vector attackDir, entity owner, int forwardSteps, int sideSteps, "
		"float stepForward, float stepSide, float stepHeight, float initialDelay, float inRowDelay, "
		"float rowToRowDelay, float maxAttackRange, float minAttackRange",
		false,
		Script_GetWeaponMissileMultipleTargets);
}

//-----------------------------------------------------------------------------
// CMissile script class: the S3 two-phase InitMissileExpandContract moves
// aside so the S21 signature owns the name.
//-----------------------------------------------------------------------------
static void Missile_RenameBinding(CUtlVector<ScriptFunctionBinding_t>& bindings,
	const char* pszName, const char* pszNewName)
{
	for (int i = 0; i < bindings.Count(); ++i)
	{
		ScriptFunctionBinding_t& binding = bindings[i];
		if (binding.m_Descriptor.m_ScriptName && strcmp(binding.m_Descriptor.m_ScriptName, pszName) == 0)
			binding.m_Descriptor.m_ScriptName = pszNewName;
	}
}

static void Missile_RegisterScriptClass(void)
{
	v_Missile_RegisterScriptClass();

	static bool s_bInitialized = false;
	if (s_bInitialized || !s_pMissileScriptStruct)
		return;
	s_bInitialized = true;

	ScriptClassDescriptor_t* const s = s_pMissileScriptStruct;
	Missile_RenameBinding(s->m_NumTypedFunctions, "InitMissileExpandContract", "InitMissileExpandContract_TwoPhase");
	Missile_RenameBinding(s->m_StrTypedFunctions, "InitMissileExpandContract", "InitMissileExpandContract_TwoPhase");

	s->AddFunction("InitMissileExpandContract",
		"Script_InitMissileExpandContract",
		"Three-phase expand-contract path that blends into the target",
		"void",
		"float speed, vector phase1Vector, vector phase2Vector, vector phase3Vector, float phase1Time, "
		"float phase1To2Time, float phase2Time, float phase2To3Time, float phase3ToTarTime, vector target, "
		"bool unused",
		false,
		Script_InitMissileExpandContract);
	s->AddFunction("MissilePath_GetExpandContractPositionAtTime",
		"Script_MissilePath_GetExpandContractPositionAtTime",
		"Position along a three-phase expand-contract path at a time",
		"vector",
		"float speed, vector startPos, vector phase1Vector, vector phase2Vector, vector phase3Vector, "
		"float phase1Time, float phase1To2Time, float phase2Time, float phase2To3Time, float phase3ToTarTime, "
		"vector target, float time",
		false,
		Script_MissilePath_GetExpandContractPositionAtTime);
	s->AddFunction("InitMissileWiggleSettings",
		"Script_InitMissileWiggleSettings",
		"Randomised sideways and vertical wiggle along the missile path",
		"void",
		"float horizontalVelocityMax, float horizontalVelocityMin, float horizontalTimeMax, "
		"float horizontalTimeMin, float verticalTimeMax, float verticalTimeMin, float verticalVelocityMax, "
		"float verticalVelocityMin, float easeIn, float easeOut, float startDelay, float duration",
		false,
		Script_InitMissileWiggleSettings);
	s->AddFunction("SetGracePeriod",
		"Script_SetGracePeriod",
		"Missile passes through characters until the grace period ends",
		"void",
		"float gracePeriod",
		false,
		Script_SetGracePeriod);
}

//-----------------------------------------------------------------------------
void VMissileExpandContract::GetAdr(void) const
{
	LogFunAdr("Missile_VelocityThink", v_Missile_VelocityThink);
	LogFunAdr("Missile_MainThink", v_Missile_MainThink);
	LogFunAdr("CCollisionProperty::SetSolidFlags", v_CCollisionProperty_SetSolidFlags);
	LogFunAdr("CBaseEntity::SetNextThink", v_CBaseEntity_SetNextThink);
	LogFunAdr("CBaseEntity::CalcAbsoluteVelocity", v_CBaseEntity_CalcAbsoluteVelocity);
	LogFunAdr("Missile_InitExpandContract", v_Missile_InitExpandContract);
	LogFunAdr("Missile_Explode", v_Missile_Explode);
	LogFunAdr("Missile_RegisterScriptClass", v_Missile_RegisterScriptClass);
	LogFunAdr("CBaseEntity::SetAbsVelocity", v_CBaseEntity_SetAbsVelocity);
	LogFunAdr("CBaseEntity::SetAbsAngles", v_CBaseEntity_SetAbsAngles);
	LogVarAdr("Missile_ScriptStruct", s_pMissileScriptStruct);
}

void VMissileExpandContract::GetFun(void) const
{
	// Server-half bodies: the literal CMissile field displacements (0x24F1,
	// 0x2518) separate them from the client twins.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 40 48 8B 05 ?? ?? ?? ?? 48 8B D9 F3 0F 10 40 10 0F 2E 05 ?? ?? ?? ?? "
		"7A 09 75 07 BF FF FF FF FF EB 11 F3 0F 5E 40 44")
		.GetPtr(v_Missile_VelocityThink);
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 60 48 8B 05 ?? ?? ?? ?? 48 8B D9 F3 0F 10 89 EC 24 00 00")
		.GetPtr(v_Missile_MainThink);
	Module_FindPattern(g_GameDll,
		"40 53 55 56 57 48 83 EC 28 8B 69 28 8B DA 48 8B F9 3B D5 0F 84 ?? ?? ?? ?? 48 8B 01 48 8D 51 28 FF 90 10 01 00 00")
		.GetPtr(v_CCollisionProperty_SetSolidFlags);
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 0F 2E 0D ?? ?? ?? ?? 49 8B F0 48 8B F9 7A 09 75 07 BB FF FF FF FF EB 18")
		.GetPtr(v_CBaseEntity_SetNextThink);
	Module_FindPattern(g_GameDll,
		"4C 8B DC 56 48 81 EC 80 00 00 00 8B 81 30 02 00 00 48 8B F1 C1 E8 0C A8 01 0F 84")
		.GetPtr(v_CBaseEntity_CalcAbsoluteVelocity);
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 20 F3 0F 10 44 24 50 48 8B D9 F3 0F 10 4C 24 58 C6 81 F1 24 00 00 01 "
		"8B 02 89 81 18 25 00 00")
		.GetPtr(v_Missile_InitExpandContract);
	Module_FindPattern(g_GameDll,
		"48 8B C4 55 53 41 55 48 8D A8 88 FC FF FF 48 81 EC 60 04 00 00 48 89 70 08 45 33 C9 48 89 78 10 "
		"45 33 C0 48 8B 01 48 8B FA 48 8D 55 98 48 8B D9 FF 90 E0 04 00 00")
		.GetPtr(v_Missile_Explode);
	Module_FindPattern(g_GameDll,
		"48 8B C4 48 89 58 ?? 48 89 68 ?? 48 89 70 ?? 57 48 81 EC B0 00 00 00 "
		"48 8B 1D ?? ?? ?? ?? 48 8B E9 44 0F 29 40 C8 48 8B F2 0F BF 41 58")
		.GetPtr(v_CBaseEntity_SetAbsVelocity);
	Module_FindPattern(g_GameDll,
		"40 55 53 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? "
		"F3 0F 10 07 0F 2E 83 5C 04 00 00 7A ?? 75 ?? F3 0F 10 47 04 0F 2E 83 60 04 00 00")
		.GetPtr(v_CBaseEntity_SetAbsAngles);

	// Every script-class registrar shares one body shape, so anchor on the
	// class name: the CMissile registrar loads it 0x22 bytes in and stores
	// the descriptor at +0x68.
	static const uint8_t s_registrarHead[] = { 0x48, 0x83, 0xEC, 0x28, 0x80, 0x3D };
	for (ptrdiff_t n = 1; n <= 8 && !v_Missile_RegisterScriptClass; ++n)
	{
		const CMemory lea = g_GameDll.FindString("CMissile", n, true);
		if (!lea)
			break;

		const uint8_t* const pLea = reinterpret_cast<const uint8_t*>(lea.GetPtr()) - 1;
		if (pLea[0] != 0x48 || pLea[1] != 0x8D || pLea[2] != 0x15)
			continue;

		const uint8_t* const pFunc = pLea - 0x22;
		if (memcmp(pFunc, s_registrarHead, sizeof(s_registrarHead)) != 0)
			continue;

		const CMemory store(reinterpret_cast<uintptr_t>(pFunc) + 0x68);
		if (store.GetValue<uint8_t>() != 0x48 || store.Offset(1).GetValue<uint8_t>() != 0x89)
			continue;

		v_Missile_RegisterScriptClass = reinterpret_cast<void (*)(void)>(const_cast<uint8_t*>(pFunc));
		s_pMissileScriptStruct = store.ResolveRelativeAddress(0x3, 0x7).RCast<ScriptClassDescriptor_t*>();
	}

	if (!v_Missile_VelocityThink || !v_Missile_MainThink || !v_Missile_InitExpandContract || !v_Missile_Explode
		|| !v_CCollisionProperty_SetSolidFlags || !v_CBaseEntity_SetNextThink || !v_CBaseEntity_CalcAbsoluteVelocity)
		Warning(eDLL_T::SERVER, "[MISSILE-EC] CMissile pattern unresolved -- S21 missile path disabled\n");
	if (!v_Missile_RegisterScriptClass || !s_pMissileScriptStruct)
		Warning(eDLL_T::SERVER, "[MISSILE-EC] CMissile script class unresolved -- S21 missile natives missing\n");
	if (!v_CBaseEntity_SetAbsVelocity || !v_CBaseEntity_SetAbsAngles)
		Warning(eDLL_T::SERVER, "[MISSILE-EC] SetAbsVelocity/SetAbsAngles unresolved\n");
}

void VMissileExpandContract::Detour(const bool bAttach) const
{
	if (v_Missile_VelocityThink)
		DetourSetup(&v_Missile_VelocityThink, &Missile_VelocityThink, bAttach);
	if (v_Missile_MainThink)
		DetourSetup(&v_Missile_MainThink, &Missile_MainThink, bAttach);
	if (v_Missile_RegisterScriptClass && s_pMissileScriptStruct)
		DetourSetup(&v_Missile_RegisterScriptClass, &Missile_RegisterScriptClass, bAttach);
}
