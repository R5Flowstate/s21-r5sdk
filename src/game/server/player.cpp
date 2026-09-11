//======== Copyright (c) Valve Corporation, All rights reserved. ============//
//
// Purpose
//
//===========================================================================//
#include "core/stdafx.h"
#include <cfloat>
#include <cstdint>
#include "common/protocol.h"
#include "game/shared/shareddefs.h"
#include "game/shared/usercmd.h"
#include "game/shared/in_buttons.h"
#include "game/shared/weapon_types.h"
#include "game/shared/weapon_script_vars.h"
#include "game/server/movehelper_server.h"
#include "gameinterface.h"
#include "player.h"
#include "player_command.h"
#include "bridge_cmd_chain.h"
#include "game/shared/dt_extend.h"
#include "translocation.h"

#include "engine/server/server.h"

// NOTE[ AMOS ]: default tick interval (0.05) * default cvar value (10) = total time buffer of 0.5, which is the default of cvar 'sv_maxunlag'.
static ConVar sv_maxUserCmdProcessTicks("sv_maxUserCmdProcessTicks", "10", FCVAR_NONE, "Maximum number of client-issued UserCmd ticks that can be replayed in packet loss conditions, 0 to allow no restrictions.");

//------------------------------------------------------------------------------
// Purpose: executes a null command for this player
//------------------------------------------------------------------------------
void CPlayer::RunNullCommand(void)
{
	IMoveHelper* const pMover = MoveHelperServer();
	if (!pMover)
		return;

	const edict_t nEdict = GetEdict();
	if (nEdict < 1 || nEdict == FL_EDICT_INVALID)
		return;

	CUserCmd cmd;

	float flOldFrameTime = gpGlobals->frameTime;
	float flOldCurTime = gpGlobals->curTime;

	cmd.frametime = flOldFrameTime;
	cmd.command_time = flOldCurTime;

	pl.fixangle = FIXANGLE_NONE;
	EyeAngles(&cmd.viewangles);

	SetTimeBase(gpGlobals->curTime);
	pMover->SetHost(this);

	PlayerRunCommand(&cmd, pMover);
	SetLastUserCommand(&cmd);

	gpGlobals->frameTime = flOldFrameTime;
	gpGlobals->curTime = flOldCurTime;

	pMover->SetHost(NULL);
}

//------------------------------------------------------------------------------
// Purpose: gets the eye angles of this player
// Input: *pAngles - 
// Output: QAngle*
//------------------------------------------------------------------------------
QAngle* CPlayer::EyeAngles(QAngle* pAngles)
{
	return CPlayer__EyeAngles(this, pAngles);
}

Vector3D* CPlayer::EyePosition(Vector3D* pPosition)
{
	return CPlayer__EyePosition(this, pPosition);
}

//------------------------------------------------------------------------------
// Purpose: sets the time base for this player
// Input: flTimeBase -
//------------------------------------------------------------------------------
// Seconds split into ticks + remainder. GetTimeBase is TICKS_TO_TIME(ticks)+remainder.
inline void CPlayer::SetTimeBase(float flTimeBase)
{
	const float flClamped = Max(flTimeBase, 0.0f);
	const int nTicks = static_cast<int>(flClamped / TICK_INTERVAL);

	SetLastUCmdSimulationTicks(nTicks);
	SetLastUCmdSimulationRemainderTime(Max(flClamped - TICKS_TO_TIME(nTicks), 0.0f));
	SetTotalExtraClientCmdTimeAttempted(0.0f);
}

//------------------------------------------------------------------------------
// Purpose: gets the time base for this player
//------------------------------------------------------------------------------
float CPlayer::GetTimeBase() const
{
	return TICKS_TO_TIME(m_lastUCmdSimulationTicks) + m_lastUCmdSimulationRemainderTime;
}

//------------------------------------------------------------------------------
// Purpose: sets the last user cmd simulation ticks
// Input: nTicks -
//------------------------------------------------------------------------------
void CPlayer::SetLastUCmdSimulationTicks(int nTicks)
{
	if (m_lastUCmdSimulationTicks != nTicks)
	{
		const edict_t nEdict = NetworkProp()->GetEdict();

		if (nEdict != FL_EDICT_INVALID)
		{
			_InterlockedOr16((SHORT*)gpGlobals->m_pEdicts + nEdict + 32, 0x200u);
		}

		m_lastUCmdSimulationTicks = nTicks;
	}
}

//------------------------------------------------------------------------------
// Purpose: sets the last user cmd simulation remainder time
// Input: nRemainderTime -
//------------------------------------------------------------------------------
void CPlayer::SetLastUCmdSimulationRemainderTime(float fRemainderTime)
{
	if (m_lastUCmdSimulationRemainderTime != fRemainderTime)
	{
		const edict_t nEdict = NetworkProp()->GetEdict();

		if (nEdict != FL_EDICT_INVALID)
		{
			_InterlockedOr16((SHORT*)gpGlobals->m_pEdicts + nEdict + 32, 0x200u);
		}

		m_lastUCmdSimulationRemainderTime = fRemainderTime;
	}
}

//------------------------------------------------------------------------------
// Purpose: sets the total extra client cmd time attempted
// Input: flAttemptedTime - 
//------------------------------------------------------------------------------
void CPlayer::SetTotalExtraClientCmdTimeAttempted(float flAttemptedTime)
{
	if (m_totalExtraClientCmdTimeAttempted != flAttemptedTime)
	{
		const edict_t nEdict = NetworkProp()->GetEdict();

		if (nEdict != FL_EDICT_INVALID)
		{
			_InterlockedOr16((SHORT*)gpGlobals->m_pEdicts + nEdict + 32, 0x200u);
		}

		m_totalExtraClientCmdTimeAttempted = flAttemptedTime;
	}
}

//------------------------------------------------------------------------------
// Purpose: processes user cmd's for this player
// Input: *cmds - 
// numCmds - 
// totalCmds - 
// droppedPackets - 
// paused - 
//------------------------------------------------------------------------------
// TODO: this code is experimental and has reported problems from players with
// high latency, needs to be debugged or a different approach needs to be taken!
// Defaulted to OFF for now
static ConVar sv_unlag_clamp("sv_unlag_clamp", "1", FCVAR_RELEASE, "Clamp the difference between player's time base and received command time to sv_maxunlag.");

// If command_time is still <=0 after the wire parse, derive it from tick_count.
// Do not add command_number.
static ConVar bridge_cmdtime_from_tick("bridge_cmdtime_from_tick", "1", FCVAR_RELEASE,
	"S21 bridge: if command_time is still <=0 after the wire parse, derive it from "
	"tick_count (TICKS_TO_TIME). Client emit (bridge_c2s_cmdtime_emit) wins when "
	"present. 1 = on (default fallback). 0 = never overwrite (wire-only).");

//------------------------------------------------------------------------------
// Clamp each command frametime into [0, bridge_usercmd_frametime_max] at intake.
//------------------------------------------------------------------------------
static ConVar bridge_usercmd_frametime_max("bridge_usercmd_frametime_max", "0.100", FCVAR_RELEASE,
	"[SUPPLY] largest per-command usercmd frametime this server accepts, seconds. Mirrors "
	"the client's own generation clamp; raise only if the client runs a timescale above 1.",
	true, 0.01f, true, 1.f);

static ConVar bridge_supply_enforce("bridge_supply_enforce", "1", FCVAR_RELEASE,
	"[SUPPLY] clamp each accepted usercmd frametime into [0, bridge_usercmd_frametime_max] "
	"at intake. 0 = accept the wire value unchanged (hostile-client hazard).");

static ConVar bridge_supply_wall("bridge_supply_wall", "0", FCVAR_DEVELOPMENTONLY,
	"[SUPPLY-WALL] log a slot-0 windowed summary (~4s) of accepted usercmd frametime vs "
	"wall clock at intake. 0 = off.");

struct SupplySlot_s
{
	double   m_dWinStartWall;
	int      m_nWinCmds;
	double   m_dWinFtSum;
	float    m_flWinFtMin;
	float    m_flWinFtMax;
	int      m_nWinClamped;
};
static SupplySlot_s s_supplySlots[MAX_PLAYERS];
static double s_dSupplyClampWarnWall = 0.0;

void CPlayer::ProcessUserCmds(CUserCmd* cmds, int numCmds, int totalCmds,
	int droppedPackets, bool paused)
{
	CmdChain_Bump(CMDCHAIN_INTAKE_CALLS);

	if (totalCmds <= 0)
		return;

	CmdChain_Bump(CMDCHAIN_INTAKE_CMDS, static_cast<uint64_t>(totalCmds));

	CUserCmd* lastCmd = &m_Commands[MAX_QUEUED_COMMANDS_PROCESS];
	const float maxUnlag = sv_maxunlag->GetFloat();
	const float timeBase = GetTimeBase();

	const bool bSupplyEnforce = bridge_supply_enforce.GetBool();
	const bool bSupplyWall = bridge_supply_wall.GetBool();
	const int nSupplySlot = GetEdict() - 1;
	const bool bSupplyActive = (bSupplyEnforce || bSupplyWall)
		&& nSupplySlot >= 0 && nSupplySlot < MAX_PLAYERS;
	const double dNowWall = bSupplyActive ? Plat_FloatTime() : 0.0;
	const float flFrameTimeMax = bridge_usercmd_frametime_max.GetFloat();

	// Re-assert laserSightColor / laserSightColorCustomized into DT_Player.
	LaserSightColorBridge_Think(this);
	CmdChain_Bump(CMDCHAIN_TICK_LASER);

	for (int i = totalCmds - 1; i >= 0; i--)
	{
		CUserCmd* cmd = &cmds[i];
		const int commandNumber = cmd->command_number;

		if (commandNumber <= m_latestCommandQueued)
			continue;

		// Claim the number only after the queue takes it; claiming first made dedupe discard the backup copy.
		const int lastCommandNumber = lastCmd->command_number;

		if (lastCommandNumber == MAX_QUEUED_COMMANDS_PROCESS)
			return;

		m_latestCommandQueued = commandNumber;

		// TODO: why are grenades not clamped to sv_maxunlag ???
		// TODO: the command_time is set from the client itself in CInput::CreateMove
		// to gpGlobals->curtime in the ucmd packet, perhaps just calculate it from
		// the server based on ucmd ticks ???
		// 
		// Possible solutions that need to be explored and worked out further
		// 
		// cmd->command_time = TICKS_TO_TIME(cmd->command_number + cmd->tick_count) // seems to be the closest, but also still manipulatable from the client.
		// cmd->command_time = TICKS_TO_TIME(client->GetDeltaTick + cmd->command_number) // delta tick is not necessarily the same as actual ucmd tick, and will be -1 on baseline request.
		// cmd->command_time = TICKS_TO_TIME(m_lastUCmdSimulationRemainderTime) + m_totalExtraClientCmdTimeAttempted; // player timebase; also up to 100ms difference between orig sent value.
		// 
		//... reverse more ticks and floats in CClient since there seem to be a
		// bunch still in the padded bytes, possibly one of them is what we could
		// and should actually use to get the remote client time since ucmd was sent.
		// Prefer client-emitted command_time (sub-tick via snapshot_interp_acc).
		// Only synthesize from tick when the wire left it empty (legacy gate-0).
		if (cmd->command_time <= 0.0f
			&& bridge_cmdtime_from_tick.GetBool()
			&& cmd->tick_count > 0)
		{
			cmd->command_time = TICKS_TO_TIME(cmd->tick_count);
		}

		if (sv_unlag_clamp.GetBool())
			cmd->command_time = Min(Max(cmd->command_time, Max(timeBase - maxUnlag, 0.0f)), timeBase + maxUnlag);

		if (bSupplyActive)
		{
			SupplySlot_s& s = s_supplySlots[nSupplySlot];
			const float flRawFrameTime = cmd->frametime; // the window stats report the wire value

			if (bSupplyEnforce)
			{
				const float flAllowed = Clamp(cmd->frametime, 0.0f, flFrameTimeMax);

				if (flAllowed != cmd->frametime)
				{
					cmd->frametime = flAllowed;
					++s.m_nWinClamped;
					CmdChain_Bump(CMDCHAIN_INTAKE_CLAMPED);

					if ((dNowWall - s_dSupplyClampWarnWall) > 2.0)
					{
						s_dSupplyClampWarnWall = dNowWall;
						Warning(eDLL_T::SERVER,
							"[SUPPLY] slot %d cmd=%u frametime %.5f out of range, clamped to %.5f\n",
							nSupplySlot, static_cast<uint32_t>(cmd->command_number),
							flRawFrameTime, flAllowed);
					}
				}
			}

			if (s.m_dWinStartWall == 0.0)
				s.m_dWinStartWall = dNowWall;

			++s.m_nWinCmds;
			s.m_dWinFtSum += flRawFrameTime;
			s.m_flWinFtMin = Min(s.m_flWinFtMin, flRawFrameTime);
			s.m_flWinFtMax = Max(s.m_flWinFtMax, flRawFrameTime);

			const double dWinDur = dNowWall - s.m_dWinStartWall;

			if (dWinDur >= 4.0 && s.m_nWinCmds > 0)
			{
				if (bSupplyWall && nSupplySlot == 0)
				{
					Msg(eDLL_T::SERVER,
						"[SUPPLY-WALL] slot0 win=%.2fs cmds=%d ftSum=%.4f lead=%+.4f ftMin=%.5f "
						"ftMax=%.5f clamped=%d\n",
						dWinDur, s.m_nWinCmds, s.m_dWinFtSum, s.m_dWinFtSum - dWinDur,
						s.m_flWinFtMin, s.m_flWinFtMax, s.m_nWinClamped);
				}

				s.m_dWinStartWall = dNowWall;
				s.m_nWinCmds = 0;
				s.m_dWinFtSum = 0.0;
				s.m_flWinFtMin = FLT_MAX;
				s.m_flWinFtMax = -FLT_MAX;
				s.m_nWinClamped = 0;
			}
		}

		CUserCmd* queuedCmd = &m_Commands[lastCommandNumber];
		queuedCmd->Copy(cmd);

		if (++lastCmd->command_number > player_userCmdsQueueWarning->GetInt())
		{
			const float curTime = float(Plat_FloatTime());

			if ((curTime - m_lastCommandCountWarnTime) > 0.5f)
				m_lastCommandCountWarnTime = curTime;
		}
	}

	lastCmd->tick_count += droppedPackets;
	m_bGamePaused = paused;
}

//------------------------------------------------------------------------------
// Purpose: runs user command for this player
// Input: *pUserCmd - 
// *pMover - 
//------------------------------------------------------------------------------
void CPlayer::PlayerRunCommand(CUserCmd* pUserCmd, IMoveHelper* pMover)
{
	CPlayer__PlayerRunCommand(this, pUserCmd, pMover);
	Translocation_OnPlayerRunCommand(this);
	SetLastUserCommand(pUserCmd);
}

//------------------------------------------------------------------------------
// Purpose: stores off a user command
// Input: *pUserCmd - 
//------------------------------------------------------------------------------
void CPlayer::SetLastUserCommand(CUserCmd* pUserCmd)
{
	m_LastCmd.Copy(pUserCmd);
}

const CUserCmd* CPlayer::GetPlacementUserCommand() const
{
	if (m_pCurrentCommand)
		return m_pCurrentCommand;

	if (m_LastCmd.command_number > 0)
		return &m_LastCmd;

	return nullptr;
}

static ConVar player_applyViewPunch("player_applyViewPunch", "0", FCVAR_RELEASE, "Whether to apply view punch from damage.");
static ConVar player_applyViewPunchDuringAim("player_applyViewPunchDuringAim", "0", FCVAR_RELEASE, "Whether to apply view punch from damage while aiming.");

//------------------------------------------------------------------------------
// Purpose: applies view punch to player view angles when taking damage
// Input: *player (this) - 
// inputInfo - 
//------------------------------------------------------------------------------
void Player_ApplyViewPunch(CPlayer* thisptr, const CTakeDamageInfo* inputInfo)
{
	if (!player_applyViewPunch.GetBool())
		return;

	if (thisptr->IsZooming() && !player_applyViewPunchDuringAim.GetBool())
		return;

	CPlayer__ApplyViewPunch(thisptr, inputInfo);
}

// Mode 1 disables timeBase rewind so the clock is a per-command accumulator.
// Mode 2 also raises sv_clockcorrection_msecs so the push-up never fires.
static ConVar bridge_timebase_native("bridge_timebase_native", "1", FCVAR_RELEASE,
	"[TB-NATIVE] player sim clock model: 1 = disable the engine timeBase rewind so the "
	"clock is a pure per-command accumulator the client can reproduce; 2 = additionally "
	"neutralize the push-up correction; 0 = stock engine clock (rewind on).",
	true, 0.f, true, 2.f);

//------------------------------------------------------------------------------
// Recompute desired clock mode every call so a live convar flip stays consistent.
//------------------------------------------------------------------------------
static void Bridge_ApplyTimebaseMode(void)
{
	const int nMode = bridge_timebase_native.GetInt();

	const int nWantShift = (nMode >= 1) ? 0 : 1;
	const int nWantPush = (nMode >= 2) ? 2000000000 : 100; // 100 = stock default

	static int s_nAppliedShift = -1;
	static int s_nAppliedPush = -1;

	if (s_nAppliedShift == nWantShift && s_nAppliedPush == nWantPush)
		return;

	ConVar* const pShift = g_pCVar->FindVar("sv_shiftPlayerSimTimeBackwards");
	ConVar* const pPush = g_pCVar->FindVar("sv_clockcorrection_msecs");

	if (!pShift || !pPush)
	{
		static bool s_bWarnedUnregistered = false;

		if (!s_bWarnedUnregistered)
		{
			s_bWarnedUnregistered = true;
			Warning(eDLL_T::SERVER, "[TB-NATIVE] engine clock convars not registered yet -- retrying\n");
		}

		return; // do not mark applied; retry next call
	}

	pShift->SetValue(nWantShift);
	pPush->SetValue(nWantPush);
	s_nAppliedShift = nWantShift;
	s_nAppliedPush = nWantPush;

	Msg(eDLL_T::SERVER, "[TB-NATIVE] clock mode applied: rewind=%s pushup=%s\n",
		(nWantShift == 0) ? "OFF" : "stock", (nWantPush != 100) ? "OFF" : "stock");

	// host_limitlocal must stay 1: one usercmd per client frame. Never override it.
	static bool s_bCheckedCmdPerFrame = false;

	if (!s_bCheckedCmdPerFrame)
	{
		const ConVar* const pOneCmd = g_pCVar->FindVar("move_one_cmd_per_client_frame");

		if (pOneCmd)
		{
			s_bCheckedCmdPerFrame = true;

			if (!pOneCmd->GetBool())
				Warning(eDLL_T::SERVER,
					"[TB-NATIVE] move_one_cmd_per_client_frame is 0 -- the player sim clock will "
					"outrun server time and starve the execution gate\n");
		}
	}
}

static ConVar bridge_gate_trace("bridge_gate_trace", "0", FCVAR_DEVELOPMENTONLY,
	"[SIMCLOCK] log a slot-0 queue/clock line every N player simulations (0 = off).",
	true, 0.f, true, 8192.f);

//------------------------------------------------------------------------------
// Standing queue drain: bounded curTime slack while unexecuted time exceeds the target.
//------------------------------------------------------------------------------
static ConVar bridge_cmdqueue_drain("bridge_cmdqueue_drain", "1", FCVAR_RELEASE,
	"[CMDQ] burn off a standing usercmd queue by granting the execution gate a "
	"bounded curTime slack while the queue holds more unexecuted player time than "
	"the standing target. 0 = stock, queue depth unregulated.");

static ConVar bridge_cmdqueue_target_ms("bridge_cmdqueue_target_ms", "25", FCVAR_RELEASE,
	"[CMDQ] most unexecuted player time the queue may stand at, milliseconds. This "
	"is the client's jitter buffer and equally its input-latency floor. With "
	"bridge_cmdqueue_adaptive it is the ceiling of the RTT-scaled target; without "
	"it, the constant target.",
	true, 0.f, true, 500.f);

static ConVar bridge_cmdqueue_adaptive("bridge_cmdqueue_adaptive", "1", FCVAR_RELEASE,
	"[CMDQ] rest the standing queue at bridge_cmdqueue_rtt_frac of the client's "
	"measured RTT instead of a constant bridge_cmdqueue_target_ms, capped by that "
	"convar and floored at one command on loopback, two on a measured path. "
	"Unknown RTT (bots, stats not yet primed) keeps the cap. 0 = constant target.");

static ConVar bridge_cmdqueue_rtt_frac("bridge_cmdqueue_rtt_frac", "0.5", FCVAR_RELEASE,
	"[CMDQ] fraction of the measured RTT held as the adaptive standing-queue target.",
	true, 0.f, true, 2.f);

//------------------------------------------------------------------------------
// Bound standing charge by shaving queued command frametimes, oldest first.
//------------------------------------------------------------------------------
static ConVar bridge_cmdqueue_govern("bridge_cmdqueue_govern", "1", FCVAR_RELEASE,
	"[CMDQ-GOV] bound the standing charge (queued frametime + sim clock beyond "
	"curTime) by shaving queued command frametimes, oldest first, down to the "
	"engine's own per-command floor. 0 = unbounded, oversupply parks as standing "
	"input latency.");

// The dedi raises a smaller executed frametime back to ~2.857ms (1/350), so a
// shave below that re-inflates at execute time.
static constexpr float CMDQ_MIN_FRAMETIME = 0.003f;

static ConVar bridge_cmdqueue_slack_ms("bridge_cmdqueue_slack_ms", "15", FCVAR_RELEASE,
	"[CMDQ] largest curTime slack granted per simulate call, milliseconds. Raised to "
	"the head command's own frametime when that is larger, and never above one tick.",
	true, 0.f, true, 50.f);

//------------------------------------------------------------------------------
// Purpose: unexecuted player time held in the queue.
// Output: head command's frametime -- a slack below it admits nothing, so it is
//         the floor on any grant worth making.
//------------------------------------------------------------------------------
static float Player_QueuedCommandTime(const CPlayer* const player, const int nQueued,
	float& flHeadFrameTime)
{
	flHeadFrameTime = 0.0f;

	const int nCount = Min(nQueued, MAX_QUEUED_COMMANDS_PROCESS);
	if (nCount <= 0)
		return 0.0f;

	float flPending = 0.0f;
	for (int i = 0; i < nCount; ++i)
		flPending += player->Diag_QueuedCommandFrameTime(i);

	flHeadFrameTime = player->Diag_QueuedCommandFrameTime(0);
	return flPending;
}

//------------------------------------------------------------------------------
// Purpose: unexecuted player time the drain rests at.
//------------------------------------------------------------------------------
static float Player_CmdQueueTargetTime(const CPlayer* const player, const float flHeadFrameTime)
{
	const float flCap = bridge_cmdqueue_target_ms.GetFloat() * 0.001f;

	if (!bridge_cmdqueue_adaptive.GetBool())
		return flCap;

	const int nSlot = player->GetEdict() - 1;

	if (nSlot < 0 || nSlot >= MAX_PLAYERS)
		return flCap;

	const CClient* const pClient = g_pServer->GetClient(nSlot);
	const CNetChan* const pChan = (pClient && pClient->IsHumanPlayer())
		? pClient->GetNetChan() : nullptr;

	if (!pChan)
		return flCap;

	const float flRtt = pChan->GetAvgLatency(FLOW_OUTGOING);

	if (flRtt < 0.0f)
		return flCap;

	if (flRtt == 0.0f && pChan->GetTotalPackets(FLOW_INCOMING) < NET_FRAMES_BACKUP)
		return flCap;

	// A path that measures above zero has real arrival jitter; one standing
	// command is no buffer against a single late packet, so rest at two.
	// Loopback (primed zero) keeps the single-command floor.
	float flFloor = flHeadFrameTime;

	if (flHeadFrameTime > 0.0f && flRtt > 0.0f)
		flFloor = flHeadFrameTime * 2.0f;

	return Max(flFloor, Min(flCap, flRtt * bridge_cmdqueue_rtt_frac.GetFloat()));
}

//------------------------------------------------------------------------------
// Purpose: true while the queued window carries time-critical input: attack or
// zoom held, or a weapon-switch edge. Shaving frametimes there rewrites hold
// durations and switch timing the client already predicted (first-pull bow lag:
// a weapon-Give stall parked q=87, the shave cost 0.34s of hold and the client
// mispredicted timeBase/origin/punch/zoom on first release).
//------------------------------------------------------------------------------
static bool Player_QueuedCommandsHoldFireOrSwitch(const CPlayer* const player, const int nCount)
{
	if (!player || nCount <= 0)
		return false;

	const int nBaseSlot = player->Diag_QueuedCommandCycleslot(0);
	const int nBaseIndex = player->Diag_QueuedCommandWeaponIndex(0);
	const int nBaseSelect = player->Diag_QueuedCommandWeaponSelect(0);
	const int nBaseImpulse = player->Diag_QueuedCommandImpulse(0);

	for (int i = 0; i < nCount; ++i)
	{
		if (player->Diag_QueuedCommandButtons(i) & (IN_ATTACK | IN_ATTACK2 | IN_ZOOM))
			return true;

		if (player->Diag_QueuedCommandCycleslot(i) != nBaseSlot
			|| player->Diag_QueuedCommandWeaponIndex(i) != nBaseIndex
			|| player->Diag_QueuedCommandWeaponSelect(i) != nBaseSelect
			|| player->Diag_QueuedCommandImpulse(i) != nBaseImpulse)
			return true;
	}

	// Switch issued on the head command itself: no edge across the window yet,
	// but the weapon frame is still being re-timed.
	return nBaseSlot != WEAPON_INVENTORY_SLOT_INVALID;
}

//------------------------------------------------------------------------------
// Purpose: run physics simulation for player
// Input: *player (this) -
// numPerIteration -
// adjustTimeBase -
//------------------------------------------------------------------------------
// Positive lead: sim clock is ahead of server time, so queued cmds are held.
bool Player_PhysicsSimulate(CPlayer* player, int numPerIteration, bool adjustTimeBase)
{
	CClientExtended* const cle = g_pServer->GetClientExtended(player->GetEdict() - 1);
	const int numUserCmdProcessTicksMax = sv_maxUserCmdProcessTicks.GetInt();

	Bridge_ApplyTimebaseMode();

	if (numUserCmdProcessTicksMax && gpGlobals->gameMode != GameMode_t::SP_MODE) // don't apply this filter in SP games
		cle->InitializeMovementTimeForUserCmdProcessing(numUserCmdProcessTicksMax, TICK_INTERVAL);
	else // Otherwise we don't care to track time
		cle->SetRemainingMovementTimeForUserCmdProcessing(FLT_MAX);

	CmdChain_Bump(CMDCHAIN_SIM_CALLS);
	CmdChain_Bump(player->IsBot() ? CMDCHAIN_SIM_BOT : CMDCHAIN_SIM_STOCK);

	const int nQueuedBefore = player->Diag_QueuedCommandCount();

	// An empty queue here means the native body runs a null command: a missed
	// server tick for this player. Bots idle empty all the time; humans should
	// almost never starve, so count humans only to keep the signal readable.
	if (nQueuedBefore == 0 && !player->IsBot())
		CmdChain_Bump(CMDCHAIN_SIM_STARVED);

	const float flSavedCurTime = gpGlobals->curTime;
	float flSlack = 0.0f;
	float flPending = 0.0f;
	float flTarget = -1.0f; // negative = drain did not evaluate this call
	const bool bWorldFreeze = GameTimescale_WorldScale() < 1.0f;

	if (!bWorldFreeze && nQueuedBefore > 1 &&
		(bridge_cmdqueue_drain.GetBool() || bridge_cmdqueue_govern.GetBool()))
	{
		float flHeadFrameTime = 0.0f;
		flPending = Player_QueuedCommandTime(player, nQueuedBefore, flHeadFrameTime);

		flTarget = Player_CmdQueueTargetTime(player, flHeadFrameTime);

		if (bridge_cmdqueue_govern.GetBool() && !player->IsBot())
		{
			const float flCharge = player->GetTimeBase() + flPending - flSavedCurTime;
			float flExcess = flCharge
				- (flTarget + bridge_cmdqueue_slack_ms.GetFloat() * 0.001f);

			// A compressed hold is a corrupted hold: while the window carries
			// attack/zoom input or a weapon-switch edge the backlog drains with
			// intact frametimes instead of being shaved.
			const bool bHoldWindow = Player_QueuedCommandsHoldFireOrSwitch(player,
				Min(nQueuedBefore, MAX_QUEUED_COMMANDS_PROCESS));

			if (bHoldWindow)
			{
				static uint32_t s_nGovSkips = 0;
				if (++s_nGovSkips <= 4 || (s_nGovSkips % 512) == 0)
					DevMsg(eDLL_T::SERVER, "[CMDQ-GOV] skip shave: fire/switch window (q=%d charge=%.4f)\n",
						nQueuedBefore, flCharge);
			}

			if (!bHoldWindow && flExcess > 0.001f)
			{
				const int nCount = Min(nQueuedBefore, MAX_QUEUED_COMMANDS_PROCESS);
				float flShaved = 0.0f;

				for (int i = 0; i < nCount && flExcess > 0.0f; ++i)
				{
					const float flFt = player->Diag_QueuedCommandFrameTime(i);
					const float flCut = Min(flExcess, flFt - CMDQ_MIN_FRAMETIME);

					if (flCut <= 0.0f)
						continue;

					player->Cmdq_SetQueuedCommandFrameTime(i, flFt - flCut);
					flShaved += flCut;
					flExcess -= flCut;
				}

				if (flShaved > 0.0f)
				{
					flPending -= flShaved;
					flHeadFrameTime = player->Diag_QueuedCommandFrameTime(0);

					static uint32_t s_nGovLogs = 0;
					if (++s_nGovLogs <= 8 || (s_nGovLogs % 512) == 0)
						Warning(eDLL_T::SERVER,
							"[CMDQ-GOV] #%u slot %d shaved %.4fs of oversupply "
							"(q=%d charge=%.4f target=%.4f)\n",
							s_nGovLogs, player->GetEdict() - 1, flShaved,
							nQueuedBefore, flCharge, flTarget);
				}
			}
		}

		if (bridge_cmdqueue_drain.GetBool() && flPending > flTarget && flHeadFrameTime > 0.0f)
		{
			const float flCap = Min(Max(bridge_cmdqueue_slack_ms.GetFloat() * 0.001f,
				flHeadFrameTime), TICK_INTERVAL);

			flSlack = Min(flPending - flTarget, flCap);
			gpGlobals->curTime = flSavedCurTime + flSlack;

			static bool s_bDrainAnnounced = false;
			if (!s_bDrainAnnounced)
			{
				s_bDrainAnnounced = true;
				Warning(eDLL_T::SERVER,
					"[CMDQ] FIRST FIRE -- draining standing usercmd queue (slot %d q=%d "
					"pending=%.4fs target=%.4fs slack=%.4fs)\n",
					player->GetEdict() - 1, nQueuedBefore, flPending, flTarget, flSlack);
			}
		}
	}

	const bool bResult = CPlayer__PhysicsSimulate(player, numPerIteration, adjustTimeBase);

	if (flSlack > 0.0f)
	{
		gpGlobals->curTime = flSavedCurTime;
		player->SetLastSimulateTime(flSavedCurTime);
	}

	const int nQueuedAfter = player->Diag_QueuedCommandCount();
	const int nRun = Max(0, nQueuedBefore - nQueuedAfter);

	CmdChain_Bump(CMDCHAIN_SIM_EXECUTED, static_cast<uint64_t>(nRun));
	CmdChain_Bump(CMDCHAIN_SIM_QUEUE_LEFT, static_cast<uint64_t>(Max(0, nQueuedAfter)));

	if ((player->GetEdict() - 1) == 0)
	{
		const double dLead = static_cast<double>(player->GetTimeBase())
			- static_cast<double>(gpGlobals->curTime);

		CmdChain_NoteGate(nQueuedBefore, nRun, dLead);

		static uint32_t s_nSimCalls = 0;
		const int nTrace = bridge_gate_trace.GetInt();

		++s_nSimCalls;

		if (nTrace > 0)
		{
			// charge = lead + pending, both post-run. A step here is a supply/wall mismatch at ingest.
			float flHeadPost = 0.0f;
			const float flPendPost =
				Player_QueuedCommandTime(player, nQueuedAfter, flHeadPost);
			const float flCharge = static_cast<float>(dLead) + flPendPost;

			static float s_flLastCharge = 0.0f;
			if (fabsf(flCharge - s_flLastCharge) > 0.008f)
			{
				Warning(eDLL_T::SERVER,
					"[CMDQ-CHARGE] slot0 chg %.4f -> %.4f (q=%d run=%d headFt=%.4f)\n",
					s_flLastCharge, flCharge, nQueuedBefore, nRun, flHeadPost);
				s_flLastCharge = flCharge;
			}

			if ((s_nSimCalls % static_cast<uint32_t>(nTrace)) == 0)
			{
				if (flTarget < 0.0f)
					flTarget = Player_CmdQueueTargetTime(player, 0.0f);

				float flRttOut = -1.0f, flRttIn = -1.0f;
				int64_t nTpIn = -1;
				const CClient* const pClient = g_pServer->GetClient(0);
				const CNetChan* const pChan = (pClient && pClient->IsHumanPlayer())
					? pClient->GetNetChan() : nullptr;

				if (pChan)
				{
					flRttOut = pChan->GetAvgLatency(FLOW_OUTGOING);
					flRttIn = pChan->GetAvgLatency(FLOW_INCOMING);
					nTpIn = pChan->GetTotalPackets(FLOW_INCOMING);
				}

				Msg(eDLL_T::SERVER,
					"[SIMCLOCK] slot0 calls=%u q=%d->%d run=%d nPerIter=%d lead=%+.4f "
					"pend=%.4f tgt=%.4f slack=%.4f chg=%.4f rttO=%.4f rttI=%.4f tpI=%lld\n",
					s_nSimCalls, nQueuedBefore, nQueuedAfter, nRun, numPerIteration, dLead,
					static_cast<double>(flPending), static_cast<double>(flTarget),
					static_cast<double>(flSlack), static_cast<double>(flCharge),
					static_cast<double>(flRttOut), static_cast<double>(flRttIn), nTpIn);
			}
		}
	}

	CmdChain_Report();

	return bResult;
}

/*
=====================
CC_CreateFakePlayer_f

  Creates a fake player
  on the server
=====================
*/
static void CC_CreateFakePlayer_f(const CCommand& args)
{
	if (!g_pServer->IsActive())
		return;

	if (args.ArgC() < 3)
	{
		Msg(eDLL_T::SERVER, "usage 'sv_addbot': name(string) teamid(int)\n");
		return;
	}

	const int numPlayers = g_pServer->GetNumClients();

	// Already at max, don't create.
	if (numPlayers >= g_ServerGlobalVariables->maxClients)
		return;

	const char* const playerName = args.Arg(1);
	const int teamNum = atoi(args.Arg(2));

	// The following code must either run inside the server frame thread, or
	// after it has finished. Lock here and help with other jobs until the
	// server has finished running the frame.
	ThreadJoinServerJob();

	// note(amos): if you call CServer::CreateFakeClient directly, you also
	// need to lock the string tables with LockNetworkStringTables. The 
	// CVEngineServer method automatically locks and unlocks the string tables.
	const edict_t nHandle = g_pEngineServer->CreateFakeClient(playerName, teamNum);
	g_pServerGameClients->ClientFullyConnect(nHandle, false);
}

static ConCommand sv_addbot("sv_addbot", CC_CreateFakePlayer_f, "Creates a bot on the server", FCVAR_RELEASE);

void VPlayer::Detour(const bool bAttach) const
{
	DetourSetup(&CPlayer__PhysicsSimulate, &Player_PhysicsSimulate, bAttach);
	DetourSetup(&CPlayer__ApplyViewPunch, &Player_ApplyViewPunch, bAttach);
}
