//=============================================================================//
//
// Purpose: [MOVE-TRACE] lockstep per-command movement-state dump at
// CGameMovement::FullWalkMove. Joinable against the client twin on cmd=.
// Measurement only -- gated by bridge_move_trace.
//
//=============================================================================//
#include "core/stdafx.h"


#include "tier1/cvar.h"
#include "move_sim_trace.h"

//-----------------------------------------------------------------------------
// Raw layout constants -- r5apex_ds server-half.
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t MT_CTX_OFF_PLAYER = 8;   // CPlayer*
static constexpr ptrdiff_t MT_CTX_OFF_MV     = 16;  // CMoveData*

static constexpr ptrdiff_t MT_MV_OFF_MAXSPEED    = 212;  // float
static constexpr ptrdiff_t MT_MV_OFF_FORWARDMOVE = 48;   // float
static constexpr ptrdiff_t MT_MV_OFF_SIDEMOVE    = 52;   // float
static constexpr ptrdiff_t MT_MV_OFF_BUTTONS     = 0x24; // int
static constexpr ptrdiff_t MT_MV_OFF_ORIGIN      = 292;  // Vector
static constexpr ptrdiff_t MT_MV_OFF_VELOCITY    = 304;  // Vector

static constexpr ptrdiff_t MT_PLAYER_OFF_SPRINTING      = 27940; // bool m_fIsSprinting
static constexpr ptrdiff_t MT_PLAYER_OFF_DUCKSTATE      = 26096; // int
static constexpr ptrdiff_t MT_PLAYER_OFF_SLIDING        = 26565; // bool
static constexpr ptrdiff_t MT_PLAYER_OFF_GROUNDENT      = 964;   // EHANDLE, -1 = airborne
static constexpr ptrdiff_t MT_PLAYER_OFF_MOVESCALE      = 24884; // float m_cachedMoveScale
static constexpr ptrdiff_t MT_PLAYER_OFF_EDICT          = 88;    // int16 edict index, -1 = none
static constexpr ptrdiff_t MT_PLAYER_OFF_CURRENTCOMMAND = 25976; // cmd*
static constexpr ptrdiff_t MT_PLAYER_OFF_EMBEDDEDCMD    = 25500; // CUserCmd body
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

//-----------------------------------------------------------------------------
// Per-player previous logged state. Slot recycled when idle (nCmdCount==0)
// or when the table is full (steal slot 0).
//-----------------------------------------------------------------------------
static constexpr int MT_MAX_TRACKED = 64;

struct MoveTraceSlot_t
{
	const void* pPlayer;
	int nPrevSprint;
	int nPrevDuck;
	int nPrevSlide;
	int nPrevGe;
	int nPrevButtons;
	int nCmdCount;
	bool bHasPrev;
};

static MoveTraceSlot_t s_moveTraceSlots[MT_MAX_TRACKED] = { };

static MoveTraceSlot_t& MoveTrace_Slot(const void* const pPlayer)
{
	int nFree = -1;

	for (int i = 0; i < MT_MAX_TRACKED; ++i)
	{
		if (s_moveTraceSlots[i].pPlayer == pPlayer)
			return s_moveTraceSlots[i];

		if (nFree < 0 && s_moveTraceSlots[i].nCmdCount <= 0)
			nFree = i;
	}

	const int nSlot = nFree < 0 ? 0 : nFree;

	s_moveTraceSlots[nSlot].pPlayer = pPlayer;
	s_moveTraceSlots[nSlot].nPrevSprint = 0;
	s_moveTraceSlots[nSlot].nPrevDuck = 0;
	s_moveTraceSlots[nSlot].nPrevSlide = 0;
	s_moveTraceSlots[nSlot].nPrevGe = 0;
	s_moveTraceSlots[nSlot].nPrevButtons = 0;
	s_moveTraceSlots[nSlot].nCmdCount = 0;
	s_moveTraceSlots[nSlot].bHasPrev = false;

	return s_moveTraceSlots[nSlot];
}

static int MoveTrace_ReadCmdNumber(void* const player)
{
	if (!player)
		return -1;

	const uintptr_t p = reinterpret_cast<uintptr_t>(player);
	const __int64 pCmd = *reinterpret_cast<__int64*>(p + MT_PLAYER_OFF_CURRENTCOMMAND);
	if (pCmd)
		return static_cast<int>(*reinterpret_cast<uint32_t*>(pCmd + MT_CMD_OFF_COMMANDNUMBER));

	return static_cast<int>(*reinterpret_cast<uint32_t*>(p + MT_PLAYER_OFF_EMBEDDEDCMD + MT_CMD_OFF_COMMANDNUMBER));
}

static uint8_t* MoveTrace_Player(void* const ctx)
{
	return ctx ? *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(ctx) + MT_CTX_OFF_PLAYER) : nullptr;
}

static uint8_t* MoveTrace_MoveData(void* const ctx)
{
	return ctx ? *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(ctx) + MT_CTX_OFF_MV) : nullptr;
}

// Pre-state stashed by Before, consumed by After. FullWalkMove is synchronous.
struct MoveTracePre_t
{
	void* pCtx;
	uint8_t* pPlayer;
	uint8_t* pMv;
	int nCmd;
	float flMaxSpeed;
	int nSprint;
	int nDuck;
	int nSlide;
	int nGe;
	float flFwd;
	float flSide;
	int nButtons;
	float flMoveScale;
	float flOX, flOY, flOZ;
	float flVX, flVY, flVZ;
	int nEnt;
	bool bValid;
};

static MoveTracePre_t s_moveTracePre = { };

void MoveSimTrace_BeforeFullWalkMove(void* ctx)
{
	s_moveTracePre.bValid = false;

	if (bridge_move_trace.GetInt() <= 0 || !ctx)
		return;

	uint8_t* const player = MoveTrace_Player(ctx);
	uint8_t* const mv = MoveTrace_MoveData(ctx);

	if (!player || !mv)
		return;

	const int nGroundEnt = *reinterpret_cast<const int*>(player + MT_PLAYER_OFF_GROUNDENT);

	s_moveTracePre.pCtx = ctx;
	s_moveTracePre.pPlayer = player;
	s_moveTracePre.pMv = mv;
	s_moveTracePre.nCmd = MoveTrace_ReadCmdNumber(player);
	s_moveTracePre.flMaxSpeed = *reinterpret_cast<const float*>(mv + MT_MV_OFF_MAXSPEED);
	s_moveTracePre.nSprint = *reinterpret_cast<const bool*>(player + MT_PLAYER_OFF_SPRINTING) ? 1 : 0;
	s_moveTracePre.nDuck = *reinterpret_cast<const int*>(player + MT_PLAYER_OFF_DUCKSTATE);
	s_moveTracePre.nSlide = *reinterpret_cast<const bool*>(player + MT_PLAYER_OFF_SLIDING) ? 1 : 0;
	s_moveTracePre.nGe = (nGroundEnt != -1) ? 1 : 0;
	s_moveTracePre.flFwd = *reinterpret_cast<const float*>(mv + MT_MV_OFF_FORWARDMOVE);
	s_moveTracePre.flSide = *reinterpret_cast<const float*>(mv + MT_MV_OFF_SIDEMOVE);
	s_moveTracePre.nButtons = *reinterpret_cast<const int*>(mv + MT_MV_OFF_BUTTONS);
	s_moveTracePre.flMoveScale = *reinterpret_cast<const float*>(player + MT_PLAYER_OFF_MOVESCALE);
	s_moveTracePre.flOX = reinterpret_cast<const float*>(mv + MT_MV_OFF_ORIGIN)[0];
	s_moveTracePre.flOY = reinterpret_cast<const float*>(mv + MT_MV_OFF_ORIGIN)[1];
	s_moveTracePre.flOZ = reinterpret_cast<const float*>(mv + MT_MV_OFF_ORIGIN)[2];
	s_moveTracePre.flVX = reinterpret_cast<const float*>(mv + MT_MV_OFF_VELOCITY)[0];
	s_moveTracePre.flVY = reinterpret_cast<const float*>(mv + MT_MV_OFF_VELOCITY)[1];
	s_moveTracePre.flVZ = reinterpret_cast<const float*>(mv + MT_MV_OFF_VELOCITY)[2];
	s_moveTracePre.nEnt = static_cast<int>(*reinterpret_cast<const int16_t*>(player + MT_PLAYER_OFF_EDICT));
	s_moveTracePre.bValid = true;
}

void MoveSimTrace_AfterFullWalkMove(void* ctx)
{
	if (!s_moveTracePre.bValid || s_moveTracePre.pCtx != ctx)
		return;

	s_moveTracePre.bValid = false;

	const int nMode = bridge_move_trace.GetInt();
	if (nMode <= 0)
		return;

	uint8_t* const mv = MoveTrace_MoveData(ctx);
	if (!mv || mv != s_moveTracePre.pMv)
		return;

	const float* const pPostOrigin = reinterpret_cast<const float*>(mv + MT_MV_OFF_ORIGIN);
	const float* const pPostVel = reinterpret_cast<const float*>(mv + MT_MV_OFF_VELOCITY);

	MoveTraceSlot_t& slot = MoveTrace_Slot(s_moveTracePre.pPlayer);
	++slot.nCmdCount;

	bool bEmit = (nMode == 1);
	if (nMode == 2)
	{
		const bool bEdge = !slot.bHasPrev
			|| slot.nPrevSprint != s_moveTracePre.nSprint
			|| slot.nPrevDuck != s_moveTracePre.nDuck
			|| slot.nPrevSlide != s_moveTracePre.nSlide
			|| slot.nPrevGe != s_moveTracePre.nGe
			|| slot.nPrevButtons != s_moveTracePre.nButtons;
		const int nHb = bridge_move_trace_heartbeat.GetInt();
		const bool bHeartbeat = (nHb > 0) && ((slot.nCmdCount % nHb) == 0);
		bEmit = bEdge || bHeartbeat;
	}

	if (!bEmit)
		return;

	Msg(eDLL_T::SERVER,
		"[MOVE-TRACE] cmd=%d ent=%d ms=%.3f spr=%d duck=%d slide=%d ge=%d "
		"fwd=%.3f sd=%.3f b=%08X msc=%.4f o=%.3f %.3f %.3f v=%.3f %.3f %.3f "
		"po=%.3f %.3f %.3f pv=%.3f %.3f %.3f\n",
		s_moveTracePre.nCmd, s_moveTracePre.nEnt,
		s_moveTracePre.flMaxSpeed, s_moveTracePre.nSprint, s_moveTracePre.nDuck,
		s_moveTracePre.nSlide, s_moveTracePre.nGe,
		s_moveTracePre.flFwd, s_moveTracePre.flSide,
		static_cast<unsigned>(s_moveTracePre.nButtons), s_moveTracePre.flMoveScale,
		s_moveTracePre.flOX, s_moveTracePre.flOY, s_moveTracePre.flOZ,
		s_moveTracePre.flVX, s_moveTracePre.flVY, s_moveTracePre.flVZ,
		pPostOrigin[0], pPostOrigin[1], pPostOrigin[2],
		pPostVel[0], pPostVel[1], pPostVel[2]);

	slot.nPrevSprint = s_moveTracePre.nSprint;
	slot.nPrevDuck = s_moveTracePre.nDuck;
	slot.nPrevSlide = s_moveTracePre.nSlide;
	slot.nPrevGe = s_moveTracePre.nGe;
	slot.nPrevButtons = s_moveTracePre.nButtons;
	slot.bHasPrev = true;
}

void VMoveSimTrace::GetAdr(void) const
{
	LogFunAdr("CGameMovement::FullWalkMove", v_CGameMovement__FullWalkMove);
}

void VMoveSimTrace::GetFun(void) const
{
	// Same target as VJetDrive's FullWalkMove pattern below; resolved here for
	// address verification only, never attached (see Detour).
	Module_FindPattern(g_GameDll,
		"48 8B C4 55 53 41 57 48 8D 68 A1 48 81 EC E0 00 00 00 0F 29 70 D8 48 8B D9 48 8B 49 08 48 89 70 08 48 89 78 10 4C 89 60 18 4C 89 70 20")
		.GetPtr(v_CGameMovement__FullWalkMove);

	if (!v_CGameMovement__FullWalkMove)
		Warning(eDLL_T::SERVER, "[MOVE-TRACE] FullWalkMove pattern unresolved\n");
}

void VMoveSimTrace::Detour(const bool bAttach) const
{
	(void)bAttach;
	// Attaches nothing: VJetDrive owns the only FullWalkMove attach and samples
	// this TU through MoveSimTrace_Before/AfterFullWalkMove. A second attach on
	// the same target orphans one of the two hooks.
}

