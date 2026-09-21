//=============================================================================//
//
// Purpose: Drives fake players with user commands: replay of a recorded input
// stream, or a synthesized program (look, move, strafe, waypoint).
//
//=============================================================================//
#include "core/stdafx.h"
#include "core/logdef.h"
#include "tier0/dbg.h"
#include "tier1/cvar.h"
#include "mathlib/mathlib.h"
#include "common/protocol.h"
#include "vscript/vscript.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "public/vscript/ivscript.h"
#include "game/shared/shareddefs.h"
#include "public/game/shared/in_buttons.h"
#include "game/server/player.h"
#include "game/server/util_server.h"
#include "engine/server/server.h"
#include "engine/client/client.h"
#include "game/server/movehelper_server.h"
#include "game/server/vscript_server_natives.h"
#include "game/server/cmd_recorder.h"
#include "bot_cmd.h"

static ConVar botcmd_diag("botcmd_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Log bot command feed: batches, queue fallbacks, loop wraps.");
static ConVar botcmd_max_batch("botcmd_max_batch", "24", FCVAR_RELEASE,
	"Most commands handed to one bot per server frame.", true, 1.f, true, 48.f);
static ConVar botcmd_program_substeps("botcmd_program_substeps", "6", FCVAR_RELEASE,
	"Commands a program bot runs per server frame; a human client sends many short commands per tick.",
	true, 1.f, true, 16.f);
static ConVar botcmd_use_queue("botcmd_use_queue", "0", FCVAR_RELEASE,
	"1 = feed bot commands through the player command queue like a network client; "
	"0 = run them directly the way the engine runs bot null commands.");

enum BotCmdMode_t
{
	BOTCMD_NONE = 0,
	BOTCMD_PLAYBACK,
	BOTCMD_PROGRAM
};

enum BotMoveMode_t
{
	BOTMOVE_NONE = 0,
	BOTMOVE_CONSTANT,
	BOTMOVE_TO,
	BOTMOVE_STRAFE
};

struct BotProgram_s
{
	int      m_nMoveMode;
	bool     m_bFaceTarget;
	int      m_nFaceEdict;
	QAngle   m_angLook;
	float    m_flForward;
	float    m_flSide;
	int      m_nButtons;
	Vector3D m_vecMoveTo;
	float    m_flMoveSpeedFrac;
	bool     m_bSprint;
	Vector3D m_vecStrafeAnchor;
	float    m_flStrafeHalfWidth;
	int      m_nLastStrafeSide;
	bool     m_bStrafeHard;
	float    m_flStrafeWaitMin;
	float    m_flStrafeWaitMax;
	bool     m_bStrafeTimingSet; // script chose the leg times; BotCmd_Strafe keeps them
	float    m_flStrafeSpeedMult;
	int      m_nLegSide;        // -1 left, 0 hold, +1 right
	float    m_flLegForward;    // -1..1, depth steps
	bool     m_bLegCrouch;
	float    m_flLegEnd;        // curTime when this leg ends
	int      m_nLegsQueued;     // pending ADAD burst legs
	int      m_nFlipStreak;
	Vector3D m_vecOscOrigin;
	Vector3D m_vecLegStart;
	float    m_flLegStartTime;
	bool     m_bTrigger;        // script wants the gun firing (LOS, ammo, target alive)
	int      m_nFireMode;       // BotFireMode_t
	float    m_flShotInterval;  // seconds between trigger pulls for tap and burst modes
	float    m_flNextPull;
	bool     m_bPullHeld;
	float    m_flAimErrorDeg;
	QAngle   m_angAimErr;
	QAngle   m_angAimErrGoal;
	float    m_flAimErrNext;
	int      m_nOneShotButtons;
};

enum BotFireMode_t
{
	BOTFIRE_HOLD = 0,   // automatic: hold the trigger
	BOTFIRE_TAP,        // semi-auto: one pull per shot
	BOTFIRE_BURST       // burst weapons: one pull per burst
};

struct BotCmdState_s
{
	int      m_nMode;
	int      m_nRecordingId;
	int      m_nCursor;
	float    m_flCursorTime;   // recorded-timeline seconds already due
	float    m_flAccum;        // recorded-timeline seconds already emitted
	bool     m_bLoop;
	float    m_flRate;
	int      m_nLoops;
	int      m_nNextCmdNumber; // never reset: intake dedupes by strictly increasing number
	bool     m_bQueueFallbackLogged;
	float    m_flStatWall;
	int      m_nStatCmds;
	float    m_flStatFt;
	int      m_nStatFrames;
	Vector3D m_vecStatOrigin;
	BotProgram_s m_Prog;
};

static BotCmdState_s s_bots[MAX_PLAYERS];
static bool s_bInited = false;

static constexpr float BOTCMD_MAX_MOVE = 450.0f;
static constexpr float BOTCMD_ARRIVE_DIST = 24.0f;
static constexpr float BOTCMD_MAX_TURN_DEG_PER_SEC = 720.0f;
static constexpr float BOTCMD_FIRE_CONE_DEG = 12.0f;
static constexpr float BOTCMD_AIM_ERR_APPROACH = 6.0f;
static constexpr float BOTCMD_TAP_SLACK_MIN = 1.0f;
static constexpr float BOTCMD_TAP_SLACK_MAX = 1.35f;

static void BotCmd_EnsureInit(void)
{
	if (s_bInited)
		return;
	s_bInited = true;
	memset(s_bots, 0, sizeof(s_bots));
}

static int BotCmd_SlotOf(const CPlayer* pPlayer)
{
	if (!pPlayer)
		return -1;
	const int nSlot = static_cast<int>(pPlayer->GetEdict()) - 1;
	if (nSlot < 0 || nSlot >= MAX_PLAYERS)
		return -1;
	return nSlot;
}

static void BotCmd_StopState(BotCmdState_s& st)
{
	st.m_flStatWall = 0.0f;
	st.m_nStatCmds = 0;
	st.m_flStatFt = 0.0f;
	st.m_nStatFrames = 0;
	st.m_nMode = BOTCMD_NONE;
	st.m_nRecordingId = -1;
	st.m_nCursor = 0;
	st.m_flCursorTime = 0.0f;
	st.m_flAccum = 0.0f;
	st.m_nLoops = 0;
}

static void BotCmd_StampCommon(BotCmdState_s& st, CUserCmd* pCmd, float flCommandTime)
{
	pCmd->command_number = ++st.m_nNextCmdNumber;
	pCmd->tick_count = gpGlobals->tickCount;
	pCmd->command_time = flCommandTime;
	pCmd->randomseed = st.m_nNextCmdNumber;
	memset(pCmd->m_pingCommands, 0, sizeof(pCmd->m_pingCommands));
}

//-----------------------------------------------------------------------------
// Direct run, used only when the bot has no command queue to feed.
//-----------------------------------------------------------------------------
static void BotCmd_RunDirect(CPlayer* pPlayer, CUserCmd* pCmds, int nCount)
{
	IMoveHelper* const pMover = MoveHelperServer();
	if (!pMover)
		return;

	const float flOldFrameTime = gpGlobals->frameTime;
	const float flOldCurTime = gpGlobals->curTime;

	// Same shape as the engine's bot null command: the clock is the server
	// clock, the time base follows it, only the frametime is the command's.
	float flTotal = 0.0f;
	for (int i = 0; i < nCount; ++i)
		flTotal += pCmds[i].frametime;
	float flClock = flOldCurTime - flTotal;

	pMover->SetHost(pPlayer);
	for (int i = 0; i < nCount; ++i)
	{
		CUserCmd* const cmd = &pCmds[i];
		flClock += cmd->frametime;
		gpGlobals->frameTime = cmd->frametime;
		gpGlobals->curTime = flClock;
		cmd->command_time = flClock;
		pPlayer->SetTimeBase(flClock);
		pPlayer->PlayerRunCommand(cmd, pMover);
		pPlayer->SetLastUserCommand(cmd);
	}
	pMover->SetHost(nullptr);

	gpGlobals->frameTime = flOldFrameTime;
	gpGlobals->curTime = flOldCurTime;
}

static void BotCmd_Deliver(CPlayer* pPlayer, BotCmdState_s& st, CUserCmd* pCmds, int nCount)
{
	if (nCount <= 0)
		return;

	if (botcmd_use_queue.GetBool() && pPlayer->Diag_QueuedCommandCount() >= 0)
	{
		pPlayer->ProcessUserCmds(pCmds, nCount, nCount, 0, false);
		return;
	}

	if (!botcmd_use_queue.GetBool())
	{
		BotCmd_RunDirect(pPlayer, pCmds, nCount);
		return;
	}

	if (!st.m_bQueueFallbackLogged)
	{
		st.m_bQueueFallbackLogged = true;
		Warning(eDLL_T::SERVER, "[BOTCMD] slot %d has no command queue, running commands directly\n",
			BotCmd_SlotOf(pPlayer));
	}
	BotCmd_RunDirect(pPlayer, pCmds, nCount);
}

//-----------------------------------------------------------------------------
// Playback
//-----------------------------------------------------------------------------
static bool BotCmd_FramePlayback(CPlayer* pPlayer, BotCmdState_s& st)
{
	const CmdRecording_s* const rec = CmdRecorder_Get(st.m_nRecordingId);
	if (!rec || rec->m_nCount <= 0)
	{
		BotCmd_StopState(st);
		return false;
	}

	const int nMaxBatch = botcmd_max_batch.GetInt();
	CUserCmd batch[48];
	int nBatch = 0;

	st.m_flCursorTime += gpGlobals->frameTime * st.m_flRate;

	while (st.m_nCursor < rec->m_nCount && nBatch < nMaxBatch)
	{
		const CUserCmd& src = rec->m_pCmds[st.m_nCursor];
		const float flEnd = st.m_flAccum + src.frametime;
		if (flEnd > st.m_flCursorTime)
			break;

		CUserCmd* const dst = &batch[nBatch++];
		memcpy(dst, &src, sizeof(CUserCmd));
		dst->frametime = src.frametime * st.m_flRate;

		st.m_flAccum = flEnd;
		++st.m_nCursor;
	}

	// The drain only runs commands whose time has arrived. Stamp the batch
	// like a client packet: the newest command lands on the server clock,
	// the older ones trail it by their frametimes.
	float flTrail = 0.0f;
	for (int i = nBatch - 1; i >= 0; --i)
	{
		flTrail += batch[i].frametime;
	}
	for (int i = 0; i < nBatch; ++i)
	{
		flTrail -= batch[i].frametime;
		BotCmd_StampCommon(st, &batch[i], gpGlobals->curTime - flTrail);
	}

	BotCmd_Deliver(pPlayer, st, batch, nBatch);

	{
		float flFt = 0.0f;
		for (int i = 0; i < nBatch; ++i)
			flFt += batch[i].frametime;
		st.m_nStatCmds += nBatch;
		st.m_flStatFt += flFt;
		++st.m_nStatFrames;
		const float flNow = gpGlobals->curTime;
		if (st.m_flStatWall == 0.0f)
		{
			st.m_flStatWall = flNow;
			st.m_vecStatOrigin = pPlayer->Diag_AbsOrigin();
		}
		else if (flNow - st.m_flStatWall >= 1.0f)
		{
			const Vector3D& org = pPlayer->Diag_AbsOrigin();
			const float dx = org.x - st.m_vecStatOrigin.x, dy = org.y - st.m_vecStatOrigin.y;
			Msg(eDLL_T::SERVER,
				"[BOTCMD] slot %d 1s: frames=%d cmds=%d cmdTime=%.3f wall=%.3f moved=%.1f cursor=%d/%d rate=%.2f\n",
				BotCmd_SlotOf(pPlayer), st.m_nStatFrames, st.m_nStatCmds, st.m_flStatFt, flNow - st.m_flStatWall,
				sqrtf(dx * dx + dy * dy), st.m_nCursor, rec->m_nCount, st.m_flRate);
			st.m_flStatWall = flNow;
			st.m_nStatCmds = 0;
			st.m_flStatFt = 0.0f;
			st.m_nStatFrames = 0;
			st.m_vecStatOrigin = org;
		}
	}

	if (st.m_nCursor >= rec->m_nCount)
	{
		if (!st.m_bLoop)
		{
			if (botcmd_diag.GetBool())
				Msg(eDLL_T::SERVER, "[BOTCMD] slot %d finished recording %d\n", BotCmd_SlotOf(pPlayer), st.m_nRecordingId);
			BotCmd_StopState(st);
			return true;
		}

		++st.m_nLoops;
		st.m_nCursor = 0;
		st.m_flAccum = 0.0f;
		st.m_flCursorTime = 0.0f;
		if (botcmd_diag.GetBool())
			Msg(eDLL_T::SERVER, "[BOTCMD] slot %d loop %d\n", BotCmd_SlotOf(pPlayer), st.m_nLoops);
	}

	return true;
}

//-----------------------------------------------------------------------------
// Program
//-----------------------------------------------------------------------------
static float BotCmd_AngleDiff(float a, float b)
{
	float d = fmodf(a - b, 360.0f);
	if (d > 180.0f) d -= 360.0f;
	if (d < -180.0f) d += 360.0f;
	return d;
}

static void BotCmd_ProgramLook(CPlayer* pPlayer, BotProgram_s& prog, float dt)
{
	if (!prog.m_bFaceTarget)
		return;

	CPlayer* const pTarget = UTIL_PlayerByIndex(prog.m_nFaceEdict);
	if (!pTarget || pTarget == pPlayer)
	{
		prog.m_bFaceTarget = false;
		return;
	}

	Vector3D vecEye;
	pPlayer->EyePosition(&vecEye);
	Vector3D vecTarget;
	pTarget->EyePosition(&vecTarget);

	const Vector3D d(vecTarget.x - vecEye.x, vecTarget.y - vecEye.y, vecTarget.z - vecEye.z);
	const float flDist2D = sqrtf(d.x * d.x + d.y * d.y);
	if (flDist2D < 1.0f && fabsf(d.z) < 1.0f)
		return;

	const float flWantYaw = RAD2DEG(atan2f(d.y, d.x));
	const float flWantPitch = -RAD2DEG(atan2f(d.z, flDist2D));

	const float flMaxStep = BOTCMD_MAX_TURN_DEG_PER_SEC * Max(dt, 0.0f);
	const float dy = Clamp(BotCmd_AngleDiff(flWantYaw, prog.m_angLook.y), -flMaxStep, flMaxStep);
	const float dp = Clamp(BotCmd_AngleDiff(flWantPitch, prog.m_angLook.x), -flMaxStep, flMaxStep);

	prog.m_angLook.y += dy;
	prog.m_angLook.x = Clamp(prog.m_angLook.x + dp, -89.0f, 89.0f);
	prog.m_angLook.z = 0.0f;
}

static void BotCmd_WorldDirToMove(float flYawDeg, float dx, float dy, float& flForward, float& flSide)
{
	const float yaw = DEG2RAD(flYawDeg);
	const float fx = cosf(yaw), fy = sinf(yaw);
	const float rx = sinf(yaw), ry = -cosf(yaw);
	flForward = dx * fx + dy * fy;
	flSide = dx * rx + dy * ry;
}

static float BotCmd_Rand(float lo, float hi)
{
	return lo + (hi - lo) * (static_cast<float>(rand() % 10000) / 10000.0f);
}

// Tempo: weighted slow / normal / fast / spike, scaled by the Lab speed slider.
// Lane check along the bot's right axis; a leg that would leave the lane
// turns around instead.
static float BotCmd_StrafeOffset(const CPlayer* pPlayer, const BotProgram_s& prog)
{
	const Vector3D& org = pPlayer->Diag_AbsOrigin();
	const float yaw = DEG2RAD(prog.m_angLook.y);
	const float rx = sinf(yaw), ry = -cosf(yaw);
	return (org.x - prog.m_vecStrafeAnchor.x) * rx + (org.y - prog.m_vecStrafeAnchor.y) * ry;
}

static int BotCmd_StrafeOpenSide(const CPlayer* pPlayer, const BotProgram_s& prog, int want)
{
	if (BotCmd_StrafeOffset(pPlayer, prog) * want > prog.m_flStrafeHalfWidth)
		return -want;
	return want;
}

// A leg is far shorter than the lane, so the side only flips at the lane
// edge (or on a small random early flip); otherwise the width never binds.
static int BotCmd_StrafeNextSide(const CPlayer* pPlayer, const BotProgram_s& prog, int lastSide, bool blocked)
{
	if (blocked)
		return -lastSide;
	const float off = BotCmd_StrafeOffset(pPlayer, prog);
	const float edge = Max(prog.m_flStrafeHalfWidth - 24.0f, 8.0f);
	if (off * lastSide >= edge)
		return -lastSide;
	if ((rand() % 100) < 15)
		return -lastSide;
	return lastSide;
}

static void BotCmd_StrafeSetLeg(BotProgram_s& prog, const CPlayer* pPlayer, int side, float forward, float dur, bool crouch)
{
	prog.m_nLegSide = side;
	if (side != 0)
		prog.m_nLastStrafeSide = side;
	prog.m_flLegForward = forward;
	prog.m_bLegCrouch = crouch;
	prog.m_flLegEnd = gpGlobals->curTime + dur / Max(prog.m_flStrafeSpeedMult, 0.25f);
	prog.m_vecLegStart = pPlayer->Diag_AbsOrigin();
	prog.m_flLegStartTime = gpGlobals->curTime;
}

// Same decision table as the NPC strafer: the easy brain alternates sides on a
// random 0.22..0.5 s cadence; the hard brain mixes holds, depth steps, ADAD
// bursts, crouch and long commits, and breaks out of place-buzz oscillation.
static void BotCmd_StrafePickLeg(CPlayer* pPlayer, BotProgram_s& prog)
{
	const Vector3D& org = pPlayer->Diag_AbsOrigin();

	// The previous leg barely moved us: treat as blocked and reverse.
	bool blocked = false;
	if (prog.m_nLegSide != 0 && prog.m_flLegStartTime > 0.0f && (gpGlobals->curTime - prog.m_flLegStartTime) > 0.15f)
	{
		const float dx = org.x - prog.m_vecLegStart.x, dy = org.y - prog.m_vecLegStart.y;
		blocked = (dx * dx + dy * dy) < (12.0f * 12.0f);
	}

	const int lastSide = prog.m_nLegSide != 0 ? prog.m_nLegSide : (prog.m_nLastStrafeSide != 0 ? prog.m_nLastStrafeSide : 1);
	int side = BotCmd_StrafeNextSide(pPlayer, prog, lastSide, blocked);

	if (!prog.m_bStrafeHard)
	{
		side = BotCmd_StrafeOpenSide(pPlayer, prog, -lastSide);
		BotCmd_StrafeSetLeg(prog, pPlayer, side, 0.0f, BotCmd_Rand(prog.m_flStrafeWaitMin, prog.m_flStrafeWaitMax), false);
		return;
	}

	// Place-buzz breakout: many flips with almost no net travel.
	if (side != lastSide)
		++prog.m_nFlipStreak;
	const float ox = org.x - prog.m_vecOscOrigin.x, oy = org.y - prog.m_vecOscOrigin.y;
	if (prog.m_nFlipStreak >= 3 && (ox * ox + oy * oy) < 2500.0f)
	{
		prog.m_nFlipStreak = 0;
		prog.m_vecOscOrigin = org;
		if (rand() % 2)
			BotCmd_StrafeSetLeg(prog, pPlayer, 0, (rand() % 2) ? 1.0f : -1.0f, BotCmd_Rand(0.2f, 0.35f), false);
		else
			BotCmd_StrafeSetLeg(prog, pPlayer, BotCmd_StrafeOpenSide(pPlayer, prog, side), 0.0f, BotCmd_Rand(0.35f, 0.6f), false);
		return;
	}

	if (prog.m_nLegsQueued > 0)
	{
		--prog.m_nLegsQueued;
		side = BotCmd_StrafeOpenSide(pPlayer, prog, side);
		BotCmd_StrafeSetLeg(prog, pPlayer, side, 0.0f, BotCmd_Rand(0.22f, 0.38f), prog.m_bLegCrouch);
		return;
	}

	const int roll = rand() % 100;
	if (roll < 12)
	{
		// micro hold / peek
		BotCmd_StrafeSetLeg(prog, pPlayer, 0, 0.0f, BotCmd_Rand(0.08f, 0.26f), false);
		prog.m_nFlipStreak = 0;
		prog.m_vecOscOrigin = org;
	}
	else if (roll < 22)
	{
		// depth step
		BotCmd_StrafeSetLeg(prog, pPlayer, 0, (rand() % 2) ? 1.0f : -1.0f, BotCmd_Rand(0.18f, 0.3f), false);
		prog.m_nFlipStreak = 0;
		prog.m_vecOscOrigin = org;
	}
	else if (roll < 26)
	{
		// ADAD burst: three quick legs, sometimes crouched
		side = BotCmd_StrafeOpenSide(pPlayer, prog, side);
		const bool crouch = (rand() % 100) < 20;
		BotCmd_StrafeSetLeg(prog, pPlayer, side, 0.0f, BotCmd_Rand(0.28f, 0.45f), crouch);
		prog.m_nLegsQueued = 2;
	}
	else if (roll < 40)
	{
		// long commit
		side = BotCmd_StrafeOpenSide(pPlayer, prog, side);
		BotCmd_StrafeSetLeg(prog, pPlayer, side, 0.0f, BotCmd_Rand(0.5f, 0.75f), false);
		prog.m_nFlipStreak = 0;
		prog.m_vecOscOrigin = org;
	}
	else
	{
		side = BotCmd_StrafeOpenSide(pPlayer, prog, side);
		BotCmd_StrafeSetLeg(prog, pPlayer, side, 0.0f, BotCmd_Rand(prog.m_flStrafeWaitMin, prog.m_flStrafeWaitMax), (rand() % 100) < 8);
	}
}

static void BotCmd_ProgramMove(CPlayer* pPlayer, BotProgram_s& prog, CUserCmd* cmd, float dt)
{
	const Vector3D& org = pPlayer->Diag_AbsOrigin();

	switch (prog.m_nMoveMode)
	{
	case BOTMOVE_CONSTANT:
		cmd->forwardmove = Clamp(prog.m_flForward, -1.0f, 1.0f) * BOTCMD_MAX_MOVE;
		cmd->sidemove = Clamp(prog.m_flSide, -1.0f, 1.0f) * BOTCMD_MAX_MOVE;
		cmd->buttons |= prog.m_nButtons;
		break;

	case BOTMOVE_TO:
	{
		const float dx = prog.m_vecMoveTo.x - org.x;
		const float dy = prog.m_vecMoveTo.y - org.y;
		const float dist = sqrtf(dx * dx + dy * dy);
		if (dist <= BOTCMD_ARRIVE_DIST)
		{
			prog.m_nMoveMode = BOTMOVE_NONE;
			break;
		}
		float f, s;
		BotCmd_WorldDirToMove(prog.m_angLook.y, dx / dist, dy / dist, f, s);
		const float scale = Clamp(prog.m_flMoveSpeedFrac, 0.05f, 1.0f) * BOTCMD_MAX_MOVE;
		cmd->forwardmove = f * scale;
		cmd->sidemove = s * scale;
		if (prog.m_bSprint)
			cmd->buttons |= IN_SPEED;
		break;
	}

	case BOTMOVE_STRAFE:
	{
		if (gpGlobals->curTime >= prog.m_flLegEnd)
			BotCmd_StrafePickLeg(pPlayer, prog);

		// Full-deflection side input in the bot's own view frame, the way a
		// keyboard player strafes; a partial stick makes the run blend stutter.
		cmd->forwardmove = prog.m_flLegForward * BOTCMD_MAX_MOVE;
		cmd->sidemove = static_cast<float>(prog.m_nLegSide) * BOTCMD_MAX_MOVE;
		if (prog.m_bLegCrouch)
			cmd->buttons |= IN_DUCK;
		break;
	}

	default:
		break;
	}
}

// Trigger: script owns the decision to fire (line of sight, ammo, target
// alive); the program owns the cadence. Automatic weapons hold the trigger,
// semi-auto and burst weapons get one pull per shot at the weapon's rate
// with a little human slack. The aim wanders inside the error cone and eases
// toward a fresh goal every few hundred ms, so shots spread like a hand on a
// mouse instead of a laser. Everything waits until the look has settled on
// the target.
static void BotCmd_ProgramFire(CPlayer* pPlayer, BotProgram_s& prog, CUserCmd* cmd, float dt)
{
	if (!prog.m_bTrigger || !prog.m_bFaceTarget)
	{
		prog.m_bPullHeld = false;
		return;
	}

	CPlayer* const pTarget = UTIL_PlayerByIndex(prog.m_nFaceEdict);
	if (!pTarget || pTarget == pPlayer)
	{
		prog.m_bPullHeld = false;
		return;
	}

	const float now = gpGlobals->curTime;
	if (now >= prog.m_flAimErrNext)
	{
		const float e = Max(prog.m_flAimErrorDeg, 0.0f);
		prog.m_angAimErrGoal.y = BotCmd_Rand(-e, e);
		prog.m_angAimErrGoal.x = BotCmd_Rand(-e, e) * 0.6f;
		prog.m_flAimErrNext = now + BotCmd_Rand(0.2f, 0.5f);
	}
	const float k = Min(dt * BOTCMD_AIM_ERR_APPROACH, 1.0f);
	prog.m_angAimErr.y += (prog.m_angAimErrGoal.y - prog.m_angAimErr.y) * k;
	prog.m_angAimErr.x += (prog.m_angAimErrGoal.x - prog.m_angAimErr.x) * k;
	cmd->viewangles.y += prog.m_angAimErr.y;
	cmd->viewangles.x = Clamp(cmd->viewangles.x + prog.m_angAimErr.x, -89.0f, 89.0f);

	Vector3D vecEye;
	pPlayer->EyePosition(&vecEye);
	Vector3D vecTarget;
	pTarget->EyePosition(&vecTarget);
	const Vector3D d(vecTarget.x - vecEye.x, vecTarget.y - vecEye.y, vecTarget.z - vecEye.z);
	const float flWantYaw = RAD2DEG(atan2f(d.y, d.x));
	if (fabsf(BotCmd_AngleDiff(flWantYaw, prog.m_angLook.y)) > BOTCMD_FIRE_CONE_DEG)
	{
		prog.m_bPullHeld = false;
		return;
	}

	if (prog.m_nFireMode == BOTFIRE_HOLD)
	{
		cmd->buttons |= IN_ATTACK;
		return;
	}

	// One pull = attack held for one frame, released the next; the weapon
	// fires on the press edge.
	if (prog.m_bPullHeld)
	{
		prog.m_bPullHeld = false;
		return;
	}
	if (now < prog.m_flNextPull)
		return;

	cmd->buttons |= IN_ATTACK;
	prog.m_bPullHeld = true;
	prog.m_flNextPull = now + Max(prog.m_flShotInterval, 0.05f) * BotCmd_Rand(BOTCMD_TAP_SLACK_MIN, BOTCMD_TAP_SLACK_MAX);
}

// The movement code reads the direction keys as buttons too, not just the
// analog axes; a bot that only drives the axes never registers a key press.
static void BotCmd_MoveButtons(CUserCmd* cmd)
{
	if (cmd->forwardmove > 0.0f) cmd->buttons |= IN_FORWARD;
	if (cmd->forwardmove < 0.0f) cmd->buttons |= IN_BACK;
	if (cmd->sidemove > 0.0f)    cmd->buttons |= IN_MOVERIGHT;
	if (cmd->sidemove < 0.0f)    cmd->buttons |= IN_MOVELEFT;
}

static bool BotCmd_FrameProgram(CPlayer* pPlayer, BotCmdState_s& st)
{
	const float dt = Max(gpGlobals->frameTime, 0.001f);
	BotProgram_s& prog = st.m_Prog;

	BotCmd_ProgramLook(pPlayer, prog, dt);

	CUserCmd cmd;
	cmd.Reset();
	cmd.frametime = dt;
	cmd.viewangles = prog.m_angLook;
	cmd.buttons = 0;
	BotCmd_ProgramMove(pPlayer, prog, &cmd, dt);
	BotCmd_MoveButtons(&cmd);
	BotCmd_ProgramFire(pPlayer, prog, &cmd, dt);
	cmd.buttons |= prog.m_nOneShotButtons;
	prog.m_nOneShotButtons = 0;

	const int nSteps = Clamp(botcmd_program_substeps.GetInt(), 1, 16);
	CUserCmd batch[16];
	const float flStep = dt / static_cast<float>(nSteps);
	const float flBase = gpGlobals->curTime - dt;
	for (int i = 0; i < nSteps; ++i)
	{
		batch[i] = cmd;
		batch[i].frametime = flStep;
		BotCmd_StampCommon(st, &batch[i], flBase + flStep * static_cast<float>(i + 1));
	}
	BotCmd_Deliver(pPlayer, st, batch, nSteps);
	return true;
}

//-----------------------------------------------------------------------------
bool BotCmd_RunFrame(CPlayer* pPlayer)
{
	if (!s_bInited || !pPlayer || !pPlayer->IsBot())
		return false;

	const int nSlot = BotCmd_SlotOf(pPlayer);
	if (nSlot < 0)
		return false;

	BotCmdState_s& st = s_bots[nSlot];
	switch (st.m_nMode)
	{
	case BOTCMD_PLAYBACK: return BotCmd_FramePlayback(pPlayer, st);
	case BOTCMD_PROGRAM:  return BotCmd_FrameProgram(pPlayer, st);
	default:              return false;
	}
}

void BotCmd_OnRecordingFreed(int nRecordingId)
{
	if (!s_bInited)
		return;
	for (int i = 0; i < MAX_PLAYERS; ++i)
		if (s_bots[i].m_nMode == BOTCMD_PLAYBACK && s_bots[i].m_nRecordingId == nRecordingId)
			BotCmd_StopState(s_bots[i]);
}

void BotCmd_OnPlayerGone(int nSlot)
{
	if (!s_bInited || nSlot < 0 || nSlot >= MAX_PLAYERS)
		return;
	BotCmd_StopState(s_bots[nSlot]);
	memset(&s_bots[nSlot].m_Prog, 0, sizeof(BotProgram_s));
}

void BotCmd_LevelShutdown(void)
{
	if (!s_bInited)
		return;
	for (int i = 0; i < MAX_PLAYERS; ++i)
	{
		BotCmd_StopState(s_bots[i]);
		memset(&s_bots[i].m_Prog, 0, sizeof(BotProgram_s));
	}
}

//-----------------------------------------------------------------------------
// Squirrel
//-----------------------------------------------------------------------------
static BotCmdState_s* BotCmd_ThisBot(HSQUIRRELVM v, CPlayer** ppPlayer)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)) || !pEntity)
		return nullptr;

	CPlayer* const pPlayer = reinterpret_cast<CPlayer*>(pEntity);
	if (!pPlayer->IsBot())
		return nullptr;

	const int nSlot = BotCmd_SlotOf(pPlayer);
	if (nSlot < 0)
		return nullptr;

	if (ppPlayer)
		*ppPlayer = pPlayer;
	return &s_bots[nSlot];
}

static void BotCmd_EnterProgram(CPlayer* pPlayer, BotCmdState_s& st)
{
	if (st.m_nMode == BOTCMD_PROGRAM)
		return;
	BotCmd_StopState(st);
	memset(&st.m_Prog, 0, sizeof(BotProgram_s));
	pPlayer->EyeAngles(&st.m_Prog.m_angLook);
	st.m_Prog.m_angLook.z = 0.0f;
	st.m_nMode = BOTCMD_PROGRAM;
}

static SQRESULT Script_BotCmd_PlayRecording(HSQUIRRELVM v)
{
	CPlayer* pPlayer = nullptr;
	BotCmdState_s* const st = BotCmd_ThisBot(v, &pPlayer);
	SQInteger nId = -1;
	SQBool bLoop = false;
	SQFloat flRate = 1.0f;
	sq_getinteger(v, 2, &nId);
	sq_getbool(v, 3, &bLoop);
	sq_getfloat(v, 4, &flRate);

	const CmdRecording_s* const rec = CmdRecorder_Get(static_cast<int>(nId));
	if (!st || !rec)
	{
		sq_pushbool(v, false);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	BotCmd_StopState(*st);
	st->m_nMode = BOTCMD_PLAYBACK;
	st->m_nRecordingId = static_cast<int>(nId);
	st->m_bLoop = bLoop != 0;
	st->m_flRate = Clamp(static_cast<float>(flRate), 0.1f, 4.0f);

	if (botcmd_diag.GetBool())
		Msg(eDLL_T::SERVER, "[BOTCMD] slot %d play recording %d loop=%d rate=%.2f cmds=%d\n",
			BotCmd_SlotOf(pPlayer), st->m_nRecordingId, st->m_bLoop, st->m_flRate, rec->m_nCount);

	sq_pushbool(v, true);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_BotCmd_Stop(HSQUIRRELVM v)
{
	BotCmdState_s* const st = BotCmd_ThisBot(v, nullptr);
	if (st)
	{
		BotCmd_StopState(*st);
		memset(&st->m_Prog, 0, sizeof(BotProgram_s));
	}
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_BotCmd_IsPlaying(HSQUIRRELVM v)
{
	BotCmdState_s* const st = BotCmd_ThisBot(v, nullptr);
	sq_pushbool(v, st && st->m_nMode == BOTCMD_PLAYBACK);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_BotCmd_GetPlaybackTime(HSQUIRRELVM v)
{
	BotCmdState_s* const st = BotCmd_ThisBot(v, nullptr);
	sq_pushfloat(v, (st && st->m_nMode == BOTCMD_PLAYBACK) ? st->m_flAccum : 0.0f);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_BotCmd_GetLoopCount(HSQUIRRELVM v)
{
	BotCmdState_s* const st = BotCmd_ThisBot(v, nullptr);
	sq_pushinteger(v, st ? st->m_nLoops : 0);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_BotCmd_SetLook(HSQUIRRELVM v)
{
	CPlayer* pPlayer = nullptr;
	BotCmdState_s* const st = BotCmd_ThisBot(v, &pPlayer);
	const SQVector3D* pVec = nullptr;
	sq_getvector(v, 2, &pVec);
	if (st && pVec)
	{
		BotCmd_EnterProgram(pPlayer, *st);
		st->m_Prog.m_bFaceTarget = false;
		st->m_Prog.m_angLook = QAngle(Clamp(pVec->x, -89.0f, 89.0f), pVec->y, 0.0f);
	}
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_BotCmd_SetMove(HSQUIRRELVM v)
{
	CPlayer* pPlayer = nullptr;
	BotCmdState_s* const st = BotCmd_ThisBot(v, &pPlayer);
	SQFloat f = 0.0f, s = 0.0f;
	SQInteger buttons = 0;
	sq_getfloat(v, 2, &f);
	sq_getfloat(v, 3, &s);
	sq_getinteger(v, 4, &buttons);
	if (st)
	{
		BotCmd_EnterProgram(pPlayer, *st);
		st->m_Prog.m_nMoveMode = BOTMOVE_CONSTANT;
		st->m_Prog.m_flForward = static_cast<float>(f);
		st->m_Prog.m_flSide = static_cast<float>(s);
		st->m_Prog.m_nButtons = static_cast<int>(buttons);
	}
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_BotCmd_MoveTo(HSQUIRRELVM v)
{
	CPlayer* pPlayer = nullptr;
	BotCmdState_s* const st = BotCmd_ThisBot(v, &pPlayer);
	const SQVector3D* pVec = nullptr;
	SQFloat flSpeed = 1.0f;
	SQBool bSprint = false;
	sq_getvector(v, 2, &pVec);
	sq_getfloat(v, 3, &flSpeed);
	sq_getbool(v, 4, &bSprint);
	if (st && pVec)
	{
		BotCmd_EnterProgram(pPlayer, *st);
		st->m_Prog.m_nMoveMode = BOTMOVE_TO;
		st->m_Prog.m_vecMoveTo = Vector3D(pVec->x, pVec->y, pVec->z);
		st->m_Prog.m_flMoveSpeedFrac = static_cast<float>(flSpeed);
		st->m_Prog.m_bSprint = bSprint != 0;
	}
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_BotCmd_Strafe(HSQUIRRELVM v)
{
	CPlayer* pPlayer = nullptr;
	BotCmdState_s* const st = BotCmd_ThisBot(v, &pPlayer);
	SQFloat flHalfWidth = 256.0f, flSpeedMult = 1.0f;
	SQBool bHard = false;
	sq_getfloat(v, 2, &flHalfWidth);
	sq_getbool(v, 3, &bHard);
	sq_getfloat(v, 4, &flSpeedMult);
	if (st)
	{
		BotCmd_EnterProgram(pPlayer, *st);
		BotProgram_s& prog = st->m_Prog;
		prog.m_nMoveMode = BOTMOVE_STRAFE;
		prog.m_vecStrafeAnchor = pPlayer->Diag_AbsOrigin();
		prog.m_vecOscOrigin = prog.m_vecStrafeAnchor;
		prog.m_flStrafeHalfWidth = Clamp(static_cast<float>(flHalfWidth), 16.0f, 2048.0f);
		prog.m_bStrafeHard = bHard != 0;
		if (!prog.m_bStrafeTimingSet)
		{
			prog.m_flStrafeWaitMin = 0.22f;
			prog.m_flStrafeWaitMax = bHard ? 0.42f : 0.5f;
		}
		prog.m_flStrafeSpeedMult = Clamp(static_cast<float>(flSpeedMult), 0.5f, 2.0f);
		prog.m_nLegSide = (rand() % 2) ? 1 : -1;
		prog.m_nLastStrafeSide = prog.m_nLegSide;
		prog.m_flLegEnd = 0.0f;
		prog.m_nLegsQueued = 0;
		prog.m_nFlipStreak = 0;
	}
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_BotCmd_SetStrafeSpeed(HSQUIRRELVM v)
{
	BotCmdState_s* const st = BotCmd_ThisBot(v, nullptr);
	SQFloat flSpeedMult = 1.0f;
	sq_getfloat(v, 2, &flSpeedMult);
	if (st && st->m_nMode == BOTCMD_PROGRAM)
		st->m_Prog.m_flStrafeSpeedMult = Clamp(static_cast<float>(flSpeedMult), 0.5f, 2.0f);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Leg time bounds in seconds: how long one strafe direction is held.
static SQRESULT Script_BotCmd_SetStrafeTiming(HSQUIRRELVM v)
{
	BotCmdState_s* const st = BotCmd_ThisBot(v, nullptr);
	SQFloat flMin = 0.22f, flMax = 0.5f;
	sq_getfloat(v, 2, &flMin);
	sq_getfloat(v, 3, &flMax);
	if (st && st->m_nMode == BOTCMD_PROGRAM)
	{
		BotProgram_s& prog = st->m_Prog;
		float lo = Clamp(static_cast<float>(flMin), 0.05f, 3.0f);
		float hi = Clamp(static_cast<float>(flMax), 0.05f, 3.0f);
		if (lo > hi)
		{
			const float t = lo;
			lo = hi;
			hi = t;
		}
		prog.m_flStrafeWaitMin = lo;
		prog.m_flStrafeWaitMax = hi;
		prog.m_bStrafeTimingSet = true;
		prog.m_flLegEnd = 0.0f;
	}
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_BotCmd_SetFireProfile(HSQUIRRELVM v)
{
	CPlayer* pPlayer = nullptr;
	BotCmdState_s* const st = BotCmd_ThisBot(v, &pPlayer);
	SQInteger nMode = BOTFIRE_HOLD;
	SQFloat flShotInterval = 0.2f;
	SQFloat flAimErr = 3.0f;
	sq_getinteger(v, 2, &nMode);
	sq_getfloat(v, 3, &flShotInterval);
	sq_getfloat(v, 4, &flAimErr);
	if (st)
	{
		BotCmd_EnterProgram(pPlayer, *st);
		BotProgram_s& prog = st->m_Prog;
		prog.m_nFireMode = Clamp(static_cast<int>(nMode), static_cast<int>(BOTFIRE_HOLD), static_cast<int>(BOTFIRE_BURST));
		prog.m_flShotInterval = Clamp(static_cast<float>(flShotInterval), 0.05f, 5.0f);
		prog.m_flAimErrorDeg = Clamp(static_cast<float>(flAimErr), 0.0f, 45.0f);
	}
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_BotCmd_SetTrigger(HSQUIRRELVM v)
{
	CPlayer* pPlayer = nullptr;
	BotCmdState_s* const st = BotCmd_ThisBot(v, &pPlayer);
	SQBool bOn = false;
	sq_getbool(v, 2, &bOn);
	if (st && st->m_nMode == BOTCMD_PROGRAM)
	{
		BotProgram_s& prog = st->m_Prog;
		if ((bOn != 0) && !prog.m_bTrigger)
		{
			prog.m_bPullHeld = false;
			prog.m_flNextPull = gpGlobals->curTime;
			prog.m_angAimErr.Init();
			prog.m_angAimErrGoal.Init();
			prog.m_flAimErrNext = 0.0f;
		}
		prog.m_bTrigger = bOn != 0;
	}
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_BotCmd_PressButtons(HSQUIRRELVM v)
{
	BotCmdState_s* const st = BotCmd_ThisBot(v, nullptr);
	SQInteger buttons = 0;
	sq_getinteger(v, 2, &buttons);
	if (st && st->m_nMode == BOTCMD_PROGRAM)
		st->m_Prog.m_nOneShotButtons |= static_cast<int>(buttons);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_BotCmd_Face(HSQUIRRELVM v)
{
	CPlayer* pPlayer = nullptr;
	BotCmdState_s* const st = BotCmd_ThisBot(v, &pPlayer);
	void* const pTargetEnt = ServerScript_EntityPtrFromStackIdx(v, 2);
	if (st)
	{
		BotCmd_EnterProgram(pPlayer, *st);
		st->m_Prog.m_bFaceTarget = false;
		if (pTargetEnt)
		{
			const int nEdict = static_cast<int>(reinterpret_cast<CBaseEntity*>(pTargetEnt)->GetEdict());
			if (UTIL_PlayerByIndex(nEdict) == pTargetEnt)
			{
				st->m_Prog.m_bFaceTarget = true;
				st->m_Prog.m_nFaceEdict = nEdict;
			}
		}
	}
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_BotCmd_ClearProgram(HSQUIRRELVM v)
{
	BotCmdState_s* const st = BotCmd_ThisBot(v, nullptr);
	if (st && st->m_nMode == BOTCMD_PROGRAM)
	{
		const QAngle look = st->m_Prog.m_angLook;
		memset(&st->m_Prog, 0, sizeof(BotProgram_s));
		st->m_Prog.m_angLook = look;
	}
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_BotCmd_Kick(HSQUIRRELVM v)
{
	CPlayer* pPlayer = nullptr;
	BotCmdState_s* const st = BotCmd_ThisBot(v, &pPlayer);
	bool bKicked = false;
	if (st && pPlayer)
	{
		const int nSlot = BotCmd_SlotOf(pPlayer);
		CClient* const pClient = (nSlot >= 0) ? g_pServer->GetClient(nSlot) : nullptr;
		if (pClient && pClient->IsConnected() && pClient->IsFakeClient())
		{
			BotCmd_StopState(*st);
			memset(&st->m_Prog, 0, sizeof(BotProgram_s));
			pClient->Disconnect(Reputation_t::REP_NONE, "Recorder bot removed");
			bKicked = true;
		}
	}
	sq_pushbool(v, bKicked);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void BotCmd_RegisterPlayerFuncs(ScriptClassDescriptor_t* pPlayerStruct)
{
	if (!pPlayerStruct)
		return;

	BotCmd_EnsureInit();

	pPlayerStruct->AddFunction("BotCmd_PlayRecording", "Script_BotCmd_PlayRecording",
		"Replay a recorded input stream on this bot.", "bool", "int recordingId, bool loop, float rate", false, Script_BotCmd_PlayRecording);
	pPlayerStruct->AddFunction("BotCmd_Stop", "Script_BotCmd_Stop",
		"Stop replay or program; the bot idles.", "void", "", false, Script_BotCmd_Stop);
	pPlayerStruct->AddFunction("BotCmd_IsPlaying", "Script_BotCmd_IsPlaying",
		"True while a recording is replaying on this bot.", "bool", "", false, Script_BotCmd_IsPlaying);
	pPlayerStruct->AddFunction("BotCmd_GetPlaybackTime", "Script_BotCmd_GetPlaybackTime",
		"Seconds of the recording replayed so far in the current loop.", "float", "", false, Script_BotCmd_GetPlaybackTime);
	pPlayerStruct->AddFunction("BotCmd_GetLoopCount", "Script_BotCmd_GetLoopCount",
		"Completed loops of the current replay.", "int", "", false, Script_BotCmd_GetLoopCount);
	pPlayerStruct->AddFunction("BotCmd_SetLook", "Script_BotCmd_SetLook",
		"Program: hold these view angles.", "void", "vector angles", false, Script_BotCmd_SetLook);
	pPlayerStruct->AddFunction("BotCmd_SetMove", "Script_BotCmd_SetMove",
		"Program: constant input, forward/side in -1..1 plus IN_ buttons.", "void", "float forward, float side, int buttons", false, Script_BotCmd_SetMove);
	pPlayerStruct->AddFunction("BotCmd_MoveTo", "Script_BotCmd_MoveTo",
		"Program: walk to a world position.", "void", "vector pos, float speedFrac, bool sprint", false, Script_BotCmd_MoveTo);
	pPlayerStruct->AddFunction("BotCmd_Strafe", "Script_BotCmd_Strafe",
		"Program: strafe like the training strafer; hard adds holds, depth steps and bursts.", "void", "float halfWidth, bool hard, float speedMult", false, Script_BotCmd_Strafe);
	pPlayerStruct->AddFunction("BotCmd_SetStrafeSpeed", "Script_BotCmd_SetStrafeSpeed",
		"Program: change the strafe tempo multiplier on a live strafer.", "void", "float speedMult", false, Script_BotCmd_SetStrafeSpeed);
	pPlayerStruct->AddFunction("BotCmd_SetStrafeTiming", "Script_BotCmd_SetStrafeTiming",
		"Program: seconds one strafe direction is held, random between min and max per leg.", "void", "float minSec, float maxSec", false, Script_BotCmd_SetStrafeTiming);
	pPlayerStruct->AddFunction("BotCmd_SetFireProfile", "Script_BotCmd_SetFireProfile",
		"Program: trigger cadence for the held weapon (0 hold, 1 tap per shot, 2 pull per burst), seconds between pulls, aim error cone in degrees.", "void",
		"int mode, float shotInterval, float aimErrorDeg", false, Script_BotCmd_SetFireProfile);
	pPlayerStruct->AddFunction("BotCmd_SetTrigger", "Script_BotCmd_SetTrigger",
		"Program: fire at the faced player while true; script decides on sight, ammo and target.", "void", "bool on", false, Script_BotCmd_SetTrigger);
	pPlayerStruct->AddFunction("BotCmd_PressButtons", "Script_BotCmd_PressButtons",
		"Program: hold these IN_ buttons for the next frame only.", "void", "int buttons", false, Script_BotCmd_PressButtons);
	pPlayerStruct->AddFunction("BotCmd_Face", "Script_BotCmd_Face",
		"Program: keep looking at a player.", "void", "entity target", false, Script_BotCmd_Face);
	pPlayerStruct->AddFunction("BotCmd_Kick", "Script_BotCmd_Kick",
		"Disconnect this fake client. Returns false for a human or an unknown slot.", "bool", "", false, Script_BotCmd_Kick);
	pPlayerStruct->AddFunction("BotCmd_ClearProgram", "Script_BotCmd_ClearProgram",
		"Program: stop moving and tracking, keep the current look.", "void", "", false, Script_BotCmd_ClearProgram);
}
