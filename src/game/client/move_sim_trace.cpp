//=============================================================================//
//
// Purpose: client [MOVE-TRACE] twin -- FullWalkMove per-command state dump
// joinable against the dedi on cmd=. Measurement only.
//
//=============================================================================//
#include "core/stdafx.h"


#include "tier1/cvar.h"
#include "game/client/move_sim_trace.h"

//-----------------------------------------------------------------------------
// Raw layout constants -- r5apex client.
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t MT_CTX_OFF_PLAYER = 8;   // C_Player*
static constexpr ptrdiff_t MT_CTX_OFF_MV     = 16;  // CMoveData*

static constexpr ptrdiff_t MT_MV_OFF_MAXSPEED    = 212;  // float
static constexpr ptrdiff_t MT_MV_OFF_FORWARDMOVE = 48;   // float
static constexpr ptrdiff_t MT_MV_OFF_SIDEMOVE    = 52;   // float
static constexpr ptrdiff_t MT_MV_OFF_BUTTONS     = 0x24; // int
static constexpr ptrdiff_t MT_MV_OFF_ORIGIN      = 280;  // Vector
static constexpr ptrdiff_t MT_MV_OFF_VELOCITY    = 292;  // Vector

static constexpr ptrdiff_t MT_PLAYER_OFF_SPRINTING      = 10796; // bool m_fIsSprinting
static constexpr ptrdiff_t MT_PLAYER_OFF_DUCKSTATE      = 10864; // int
static constexpr ptrdiff_t MT_PLAYER_OFF_SLIDING        = 11637; // bool
static constexpr ptrdiff_t MT_PLAYER_OFF_GROUNDENT      = 804;   // EHANDLE, -1 = airborne
static constexpr ptrdiff_t MT_PLAYER_OFF_MOVESCALE      = 12920; // float m_cachedMoveScale
static constexpr ptrdiff_t MT_PLAYER_OFF_CURRENTCOMMAND = 0x34B8; // cmd*
static constexpr ptrdiff_t MT_CMD_OFF_COMMANDNUMBER     = 0x00;

//-----------------------------------------------------------------------------
// Engine function pointers.
//-----------------------------------------------------------------------------
static __int64 (*v_CGameMovement__FullWalkMove)(void* ctx) = nullptr;

static ConVar bridge_move_trace("bridge_move_trace", "0", FCVAR_DEVELOPMENTONLY,
	"[MOVE-TRACE] per-command move-state trace at FullWalkMove. 0=off, "
	"1=every command, 2=state-edge changes plus heartbeat. Must match on "
	"client and dedi for joinable logs.", true, 0.f, true, 2.f);
static ConVar bridge_move_trace_heartbeat("bridge_move_trace_heartbeat", "64",
	FCVAR_DEVELOPMENTONLY, "[MOVE-TRACE] mode-2: emit a line every N commands "
	"even if nothing changed.", true, 1.f, true, 1024.f);
static ConVar bridge_move_trace_replays("bridge_move_trace_replays", "0",
	FCVAR_DEVELOPMENTONLY, "[MOVE-TRACE] 1 = also log replayed (re-predicted) "
	"commands, tagged rp=1. 0 = only first-time-simulated commands.");

//-----------------------------------------------------------------------------
// Local-player previous logged state + replay high-water.
//-----------------------------------------------------------------------------
struct MoveTracePrev_t
{
	int nPrevSprint;
	int nPrevDuck;
	int nPrevSlide;
	int nPrevGe;
	int nPrevButtons;
	int nCmdCount;
	bool bHasPrev;
};

static MoveTracePrev_t s_moveTracePrev = { };
static int s_nMoveTraceHighWater = 0;

static int MoveTrace_ReadCmdNumber(void* const player)
{
	if (!player)
		return -1;

	const __int64 pCmd = *reinterpret_cast<__int64*>(
		reinterpret_cast<uintptr_t>(player) + MT_PLAYER_OFF_CURRENTCOMMAND);
	if (!pCmd)
		return -1;

	return static_cast<int>(*reinterpret_cast<uint32_t*>(pCmd + MT_CMD_OFF_COMMANDNUMBER));
}

static __int64 __fastcall Hook_CGameMovement_FullWalkMove(void* ctx)
{
	const int nMode = bridge_move_trace.GetInt();
	if (nMode <= 0 || !ctx)
		return v_CGameMovement__FullWalkMove(ctx);

	uint8_t* const player = *reinterpret_cast<uint8_t**>(
		reinterpret_cast<uint8_t*>(ctx) + MT_CTX_OFF_PLAYER);
	uint8_t* const mv = *reinterpret_cast<uint8_t**>(
		reinterpret_cast<uint8_t*>(ctx) + MT_CTX_OFF_MV);

	if (!player || !mv)
		return v_CGameMovement__FullWalkMove(ctx);

	const int nCmd = MoveTrace_ReadCmdNumber(player);
	const float flMaxSpeed = *reinterpret_cast<const float*>(mv + MT_MV_OFF_MAXSPEED);
	const int nSprint = *reinterpret_cast<const bool*>(player + MT_PLAYER_OFF_SPRINTING) ? 1 : 0;
	const int nDuck = *reinterpret_cast<const int*>(player + MT_PLAYER_OFF_DUCKSTATE);
	const int nSlide = *reinterpret_cast<const bool*>(player + MT_PLAYER_OFF_SLIDING) ? 1 : 0;
	const int nGroundEnt = *reinterpret_cast<const int*>(player + MT_PLAYER_OFF_GROUNDENT);
	const int nGe = (nGroundEnt != -1) ? 1 : 0;
	const float flFwd = *reinterpret_cast<const float*>(mv + MT_MV_OFF_FORWARDMOVE);
	const float flSide = *reinterpret_cast<const float*>(mv + MT_MV_OFF_SIDEMOVE);
	const int nButtons = *reinterpret_cast<const int*>(mv + MT_MV_OFF_BUTTONS);
	const float flMoveScale = *reinterpret_cast<const float*>(player + MT_PLAYER_OFF_MOVESCALE);
	const float* const pOrigin = reinterpret_cast<const float*>(mv + MT_MV_OFF_ORIGIN);
	const float* const pVel = reinterpret_cast<const float*>(mv + MT_MV_OFF_VELOCITY);
	const float flOX = pOrigin[0], flOY = pOrigin[1], flOZ = pOrigin[2];
	const float flVX = pVel[0], flVY = pVel[1], flVZ = pVel[2];

	const __int64 nResult = v_CGameMovement__FullWalkMove(ctx);

	const float* const pPostOrigin = reinterpret_cast<const float*>(mv + MT_MV_OFF_ORIGIN);
	const float* const pPostVel = reinterpret_cast<const float*>(mv + MT_MV_OFF_VELOCITY);

	if (s_nMoveTraceHighWater - nCmd > 100000)
		s_nMoveTraceHighWater = 0;

	const bool bReplay = (nCmd >= 0) && (nCmd <= s_nMoveTraceHighWater);
	if (bReplay && !bridge_move_trace_replays.GetBool())
		return nResult;

	++s_moveTracePrev.nCmdCount;

	bool bEmit = (nMode == 1);
	if (nMode == 2)
	{
		const bool bEdge = !s_moveTracePrev.bHasPrev
			|| s_moveTracePrev.nPrevSprint != nSprint
			|| s_moveTracePrev.nPrevDuck != nDuck
			|| s_moveTracePrev.nPrevSlide != nSlide
			|| s_moveTracePrev.nPrevGe != nGe
			|| s_moveTracePrev.nPrevButtons != nButtons;
		const int nHb = bridge_move_trace_heartbeat.GetInt();
		const bool bHeartbeat = (nHb > 0) && ((s_moveTracePrev.nCmdCount % nHb) == 0);
		bEmit = bEdge || bHeartbeat;
	}

	if (!bEmit)
		return nResult;

	Msg(eDLL_T::CLIENT,
		"[MOVE-TRACE] cmd=%d ent=%d ms=%.3f spr=%d duck=%d slide=%d ge=%d "
		"fwd=%.3f sd=%.3f b=%08X msc=%.4f o=%.3f %.3f %.3f v=%.3f %.3f %.3f "
		"po=%.3f %.3f %.3f pv=%.3f %.3f %.3f%s\n",
		nCmd, 1, flMaxSpeed, nSprint, nDuck, nSlide, nGe,
		flFwd, flSide, static_cast<unsigned>(nButtons), flMoveScale,
		flOX, flOY, flOZ, flVX, flVY, flVZ,
		pPostOrigin[0], pPostOrigin[1], pPostOrigin[2],
		pPostVel[0], pPostVel[1], pPostVel[2],
		bReplay ? " rp=1" : "");

	if (nCmd > s_nMoveTraceHighWater)
		s_nMoveTraceHighWater = nCmd;

	s_moveTracePrev.nPrevSprint = nSprint;
	s_moveTracePrev.nPrevDuck = nDuck;
	s_moveTracePrev.nPrevSlide = nSlide;
	s_moveTracePrev.nPrevGe = nGe;
	s_moveTracePrev.nPrevButtons = nButtons;
	s_moveTracePrev.bHasPrev = true;

	return nResult;
}

void VMoveSimTraceClient::GetAdr(void) const
{
	LogFunAdr("CGameMovement::FullWalkMove", v_CGameMovement__FullWalkMove);
}

void VMoveSimTraceClient::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"48 8B C4 55 53 56 48 8D 68 A1 48 81 EC E0 00 00 00 4C 8B 49 08 48 8B D9 0F 29 70 D8 49 8B C9 48 89 78 08 4C 89 78 18")
		.GetPtr(v_CGameMovement__FullWalkMove);

	if (!v_CGameMovement__FullWalkMove)
		Warning(eDLL_T::CLIENT, "[MOVE-TRACE] FullWalkMove pattern unresolved\n");
}

void VMoveSimTraceClient::Detour(const bool bAttach) const
{
	if (v_CGameMovement__FullWalkMove)
		DetourSetup(&v_CGameMovement__FullWalkMove, &Hook_CGameMovement_FullWalkMove, bAttach);
}

