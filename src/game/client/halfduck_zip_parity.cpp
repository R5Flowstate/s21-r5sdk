//=============================================================================//
//
// Purpose: client half of zipline-exit m_doingHalfDuck parity.
// Grace window is keyed on curtime (replay-safe); dedi counts commands.
// Twin of halfduck_zip_parity.cpp -- same ConVar names and defaults.
//
//=============================================================================//
#include "core/stdafx.h"


#include "tier1/cvar.h"
#include "engine/client/net_bridge_internal.h"
#include "game/client/halfduck_zip_parity.h"

//-----------------------------------------------------------------------------
// Raw layout constants -- read off this engine's own Duck.
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t HD_CTX_OFF_PLAYER = 8;   // C_Player*
static constexpr ptrdiff_t HD_CTX_OFF_MV     = 16;  // CMoveData*

// C_Player. m_doingHalfDuck sits two ints past m_duckState (m_leanState
// between them). These are the S21 offsets and share nothing with the dedi's.
static constexpr ptrdiff_t HD_PLAYER_OFF_DUCKSTATE     = 10864;  // int
static constexpr ptrdiff_t HD_PLAYER_OFF_DOINGHALFDUCK = 10872;  // bool
static constexpr ptrdiff_t HD_PLAYER_OFF_REMAINDER     = 9260;   // int msec
static constexpr ptrdiff_t HD_PLAYER_OFF_FORCESTANCE   = 7672;   // int
static constexpr ptrdiff_t HD_PLAYER_OFF_DUCKTOGGLE    = 7677;   // bool
static constexpr ptrdiff_t HD_PLAYER_OFF_SLIDING       = 11637;  // bool
// Duck completion OR 2 / test & 2 at this slot (FL_DUCKING) -- v51[50].
static constexpr ptrdiff_t HD_PLAYER_OFF_FFLAGS        = 200;    // int m_fFlags

// Client-only wantsDuck gates (absent on dedi -- twin logs present=0).
static constexpr ptrdiff_t HD_PLAYER_OFF_TURRET       = 11684;  // EHANDLE
static constexpr ptrdiff_t HD_PLAYER_OFF_GLIDE        = 12251;  // bool
static constexpr ptrdiff_t HD_PLAYER_OFF_GATE_A       = 18372;  // int
static constexpr ptrdiff_t HD_PLAYER_OFF_GATE_B       = 18488;  // int
static constexpr ptrdiff_t HD_PLAYER_OFF_TOGGLE_INNER = 10852;  // int

// Ground-entity EHANDLE -- the very input the latch samples. Logged at the edge
// so the two engines' answers can be compared directly instead of inferred.
static constexpr ptrdiff_t HD_PLAYER_OFF_GROUNDENT = 804;   // EHANDLE, -1 = airborne

// C_Player::m_pCurrentCommand -- set for the command currently being predicted.
static constexpr ptrdiff_t HD_PLAYER_OFF_CURRENTCOMMAND = 0x34B8;
static constexpr ptrdiff_t HD_CMD_OFF_COMMANDNUMBER     = 0x00;

// CMoveData -- buttons @0x24 (IN_DUCK=4); abs origin @+280.
static constexpr ptrdiff_t HD_MV_OFF_ORIGIN  = 280;
static constexpr ptrdiff_t HD_MV_OFF_BUTTONS = 0x24;

// DuckState. Only the standing -> starting edge latches.
static constexpr int HD_DS_STANDING      = 0;
static constexpr int HD_DS_DUCK_STARTING = 1;
static constexpr int HD_DS_DUCKED        = 2;
static constexpr int HD_DS_DUCK_ENDING   = 3;

static constexpr int HD_IN_DUCK     = 4;
static constexpr int HD_FL_DUCKING  = 0x2;
static constexpr int HD_FORCE_STAND = 1;
static constexpr int HD_EHANDLE_INVALID = -1;

// CGlobalVarsBase::curtime. Same global mantle_boost.cpp reads.
static constexpr ptrdiff_t HD_GLOBALS_OFF_CURTIME = 0x10;

//-----------------------------------------------------------------------------
// C_GameMovement::Duck. xmm0 return must stay double (call-only twin can ignore it).
//-----------------------------------------------------------------------------
static double (*v_C_GameMovement__Duck)(void* ctx) = nullptr;

// C_Player::Zipline_IsZiplining. The engine's own suppression predicate; a
// weaker stand-in (a bare m_activeZipline validity test) would arm the window
// over a different span than the suppression it is meant to track.
static bool (*v_C_Player__Zipline_IsZiplining)(void* player) = nullptr;

// C_Player::Lunge_IsActive -- wantsDuck suppressor; optional for the FSM dump.
static bool (*v_C_Player__Lunge_IsActive)(void* player) = nullptr;

static ConVar bridge_halfduck_zip_parity("bridge_halfduck_zip_parity", "0", FCVAR_RELEASE,
	"Force m_doingHalfDuck on a duck that begins just after a zipline release, so both "
	"engines apply the half-hull origin step together. 0 = off, 1 = force set (airborne, "
	"the physical answer), 2 = force clear. Must match the dedi's value.", true, 0.f, true, 2.f);

static ConVar bridge_halfduck_zip_grace_time("bridge_halfduck_zip_grace_time", "0.15", FCVAR_RELEASE,
	"Seconds after this engine's own zipline release during which a duck-start is treated "
	"as airborne. The dedi counts commands (bridge_halfduck_zip_grace); keep the two spans equal.",
	true, 0.f, true, 1.f);

static ConVar bridge_halfduck_zip_debug("bridge_halfduck_zip_debug", "0", FCVAR_DEVELOPMENTONLY,
	"[HALFDUCK] Log every forced latch, not just the first few.");

static ConVar bridge_halfduck_fsm_trace("bridge_halfduck_fsm_trace", "0", FCVAR_DEVELOPMENTONLY,
	"[HALFDUCK-FSM] 0=off 1=every Duck call 2=interesting+heartbeat. "
	"Must match on client and dedi for joinable logs.", true, 0.f, true, 2.f);

static ConVar bridge_halfduck_fsm_heartbeat("bridge_halfduck_fsm_heartbeat", "32", FCVAR_RELEASE,
	"[HALFDUCK-FSM] mode-2: emit a line every N Duck calls even if boring.",
	true, 1.f, true, 512.f);

static ConVar bridge_zip_forcestance_predict("bridge_zip_forcestance_predict", "1", FCVAR_RELEASE,
	"Predict the zipline forced-stand push/pop locally so Duck sees the same m_forceStance "
	"the server used on this command. 0 restores receive-only behaviour for A/B.");

static ConVar bridge_zip_forcestance_window("bridge_zip_forcestance_window", "0.15", FCVAR_RELEASE,
	"Seconds after this engine's own zipline release during which forced-stance prediction may "
	"still correct the field. Must cover one ack of latency.", true, 0.f, true, 1.f);

//-----------------------------------------------------------------------------
// Last zipline command stamp, keyed by player pointer.
//-----------------------------------------------------------------------------
static constexpr int HD_MAX_TRACKED = 4;

struct HalfDuckSlot_t
{
	const void* pPlayer;
	float flLastZipTime;
};

static HalfDuckSlot_t s_halfDuckZipTime[HD_MAX_TRACKED] = { };
static int s_nNextSlot = 0;

// Census: nDuckCalls climbing while nZipCommands stays 0 means Duck does not run while ziplining.
static uint64_t s_nDuckCalls    = 0;   // hook entered
static uint64_t s_nZipCommands  = 0;   // ... and Zipline_IsZiplining() was true
static uint64_t s_nStartEdges   = 0;   // ... standing -> duck-starting seen
static uint64_t s_nEdgesInGrace = 0;   // ... and inside the grace window
static uint64_t s_nForcedCount  = 0;   // ... and the latch actually changed
static uint64_t s_nForceRaises  = 0;   // predicted m_forceStance 0->1
static uint64_t s_nForceClears  = 0;   // predicted m_forceStance 1->0

// FSM arm / join state.
static int      s_nFsmLastArmMode     = 0;
static bool     s_bFsmFirstDuckArmed  = false;
static bool     s_bFsmDuckPatWarned   = false;
static int      s_nFsmPrevWantsDuckR  = -1;
static uint64_t s_nFsmTraceCalls      = 0;

struct HalfDuckFsmSnap_t
{
	int   nDs;
	int   nRem;
	bool  bHalf;
	int   nFlags;
	float flOrgZ;
	int   nForce;
	bool  bToggle;
	bool  bSlide;
	int   nButtons;
	int   nGroundEnt;
	int   nTurretHandle;
	bool  bGlide;
	int   nGateA;
	int   nGateB;
	int   nToggleInner;
};

static float& HalfDuck_LastZipTime(const void* const pPlayer)
{
	for (int i = 0; i < HD_MAX_TRACKED; ++i)
	{
		if (s_halfDuckZipTime[i].pPlayer == pPlayer)
			return s_halfDuckZipTime[i].flLastZipTime;
	}

	const int nSlot = s_nNextSlot;
	s_nNextSlot = (s_nNextSlot + 1) % HD_MAX_TRACKED;

	s_halfDuckZipTime[nSlot].pPlayer = pPlayer;
	s_halfDuckZipTime[nSlot].flLastZipTime = -1000.0f;

	return s_halfDuckZipTime[nSlot].flLastZipTime;
}

static float HalfDuck_CurTime(void)
{
	const uintptr_t pGlobals = *reinterpret_cast<const uintptr_t*>(
		NetObs_Sym(NetObsSym_t::GlobalVarsPtr));

	return pGlobals
		? *reinterpret_cast<const float*>(pGlobals + HD_GLOBALS_OFF_CURTIME)
		: 0.0f;
}

static int HalfDuck_ReadCmdNumber(void* const player)
{
	if (!player)
		return -1;

	const __int64 pCmd = *reinterpret_cast<__int64*>(
		reinterpret_cast<uintptr_t>(player) + HD_PLAYER_OFF_CURRENTCOMMAND);
	if (!pCmd)
		return -1;

	return static_cast<int>(*reinterpret_cast<uint32_t*>(pCmd + HD_CMD_OFF_COMMANDNUMBER));
}

static void HalfDuck_FsmSnap(uint8_t* const player, uint8_t* const mv, HalfDuckFsmSnap_t& out)
{
	out.nDs       = *reinterpret_cast<const int*>(player + HD_PLAYER_OFF_DUCKSTATE);
	out.nRem      = *reinterpret_cast<const int*>(player + HD_PLAYER_OFF_REMAINDER);
	out.bHalf     = *reinterpret_cast<const bool*>(player + HD_PLAYER_OFF_DOINGHALFDUCK);
	out.nFlags    = *reinterpret_cast<const int*>(player + HD_PLAYER_OFF_FFLAGS);
	out.nForce    = *reinterpret_cast<const int*>(player + HD_PLAYER_OFF_FORCESTANCE);
	out.bToggle   = *reinterpret_cast<const bool*>(player + HD_PLAYER_OFF_DUCKTOGGLE);
	out.bSlide    = *reinterpret_cast<const bool*>(player + HD_PLAYER_OFF_SLIDING);
	out.nGroundEnt = *reinterpret_cast<const int*>(player + HD_PLAYER_OFF_GROUNDENT);
	out.nTurretHandle = *reinterpret_cast<const int*>(player + HD_PLAYER_OFF_TURRET);
	out.bGlide    = *reinterpret_cast<const bool*>(player + HD_PLAYER_OFF_GLIDE);
	out.nGateA    = *reinterpret_cast<const int*>(player + HD_PLAYER_OFF_GATE_A);
	out.nGateB    = *reinterpret_cast<const int*>(player + HD_PLAYER_OFF_GATE_B);
	out.nToggleInner = *reinterpret_cast<const int*>(player + HD_PLAYER_OFF_TOGGLE_INNER);
	out.nButtons  = mv ? *reinterpret_cast<const int*>(mv + HD_MV_OFF_BUTTONS) : 0;
	out.flOrgZ    = mv ? *reinterpret_cast<const float*>(mv + HD_MV_OFF_ORIGIN + 8) : 0.f;
}

static void HalfDuck_FsmArmIfNeeded(const int nMode)
{
	if (nMode <= 0)
	{
		s_nFsmLastArmMode = 0;
		s_bFsmFirstDuckArmed = false;
		return;
	}

	if (s_nFsmLastArmMode != nMode)
	{
		s_nFsmLastArmMode = nMode;
		s_bFsmFirstDuckArmed = false;
		Msg(eDLL_T::CLIENT,
			"[HALFDUCK-FSM] ARM side=CLIENT mode=%d heartbeat=%d duckPat=%p zipPat=%p\n",
			nMode, bridge_halfduck_fsm_heartbeat.GetInt(),
			reinterpret_cast<void*>(v_C_GameMovement__Duck),
			reinterpret_cast<void*>(v_C_Player__Zipline_IsZiplining));
	}
}

static void HalfDuck_FsmTraceEmit(
	void* const player,
	const HalfDuckFsmSnap_t& pre,
	const HalfDuckFsmSnap_t& post,
	const bool bZiplining,
	const int nZipGrace)
{
	const int nMode = bridge_halfduck_fsm_trace.GetInt();
	HalfDuck_FsmArmIfNeeded(nMode);
	if (nMode <= 0)
		return;

	if (!v_C_GameMovement__Duck)
	{
		if (!s_bFsmDuckPatWarned)
		{
			s_bFsmDuckPatWarned = true;
			Warning(eDLL_T::CLIENT,
				"[HALFDUCK-FSM] WARN Duck pattern unresolved -- fsm trace inert\n");
		}
		return;
	}

	if (!s_bFsmFirstDuckArmed)
	{
		s_bFsmFirstDuckArmed = true;
		Msg(eDLL_T::CLIENT,
			"[HALFDUCK-FSM] ARM side=CLIENT mode=%d heartbeat=%d duckPat=%p zipPat=%p\n",
			nMode, bridge_halfduck_fsm_heartbeat.GetInt(),
			reinterpret_cast<void*>(v_C_GameMovement__Duck),
			reinterpret_cast<void*>(v_C_Player__Zipline_IsZiplining));
	}

	++s_nFsmTraceCalls;

	const int nCmd = HalfDuck_ReadCmdNumber(player);
	const int nRemDec = pre.nRem - post.nRem;
	const int nBtnDuck = (pre.nButtons & HD_IN_DUCK) != 0 ? 1 : 0;
	const int nSupZip = bZiplining ? 1 : 0;
	const int nSupLunge = v_C_Player__Lunge_IsActive
		? (v_C_Player__Lunge_IsActive(player) ? 1 : 0)
		: -1;
	const int nSupForce = (pre.nForce == HD_FORCE_STAND) ? 1 : 0;
	// Turret handle: invalid sentinel is -1; a live handle is the suppressor bit.
	const int nSupTurret = (pre.nTurretHandle != HD_EHANDLE_INVALID) ? 1 : 0;
	const int nTurretPresent = 1;
	const int nSupGlide = pre.bGlide ? 1 : 0;
	const int nGlidePresent = 1;
	const int nSupGateA = (pre.nGateA != 0) ? 1 : 0;
	const int nGateAPresent = 1;
	const int nSupGateB = (pre.nGateB != 0) ? 1 : 0;
	const int nGateBPresent = 1;
	const int nSupInner = (pre.nToggleInner <= 0) ? 1 : 0;
	const int nInnerPresent = 1;

	const int nLungeForWant = (nSupLunge > 0) ? 1 : 0;
	const int nWantsDuckR =
		((nBtnDuck || pre.bToggle)
			&& !nSupZip && !nLungeForWant && !nSupForce
			&& !nSupTurret && !nSupGlide
			&& !nSupGateA && !nSupGateB) ? 1 : 0;

	const float flDZ = post.flOrgZ - pre.flOrgZ;
	const int nStep =
		(!(pre.nFlags & HD_FL_DUCKING) && (post.nFlags & HD_FL_DUCKING)
			&& pre.bHalf
			&& fabsf(flDZ) > 1.0f) ? 1 : 0;
	const int nEndingCollapse =
		(pre.nDs == HD_DS_DUCK_ENDING
			&& post.nDs == HD_DS_DUCKED
			&& post.nRem <= 0) ? 1 : 0;

	const bool bWantsEdge = (s_nFsmPrevWantsDuckR >= 0 && s_nFsmPrevWantsDuckR != nWantsDuckR);
	const bool bAnySup =
		(nSupZip == 1) || (nSupLunge == 1) || (nSupForce == 1)
		|| (nSupTurret == 1) || (nSupGlide == 1)
		|| (nSupGateA == 1) || (nSupGateB == 1) || (nSupInner == 1);
	const bool bInteresting =
		(pre.nDs != post.nDs)
		|| (nRemDec != 0)
		|| (nStep != 0)
		|| (nEndingCollapse != 0)
		|| bAnySup
		|| bWantsEdge
		|| (fabsf(flDZ) > 0.01f);

	const int nHb = bridge_halfduck_fsm_heartbeat.GetInt();
	const bool bHeartbeat = (nHb > 0) && ((s_nFsmTraceCalls % static_cast<uint64_t>(nHb)) == 0);

	const bool bEmit = (nMode == 1) || bInteresting || bHeartbeat;
	s_nFsmPrevWantsDuckR = nWantsDuckR;

	if (!bEmit)
		return;

	Msg(eDLL_T::CLIENT,
		"[HALFDUCK-FSM] cmd=%d ds=%d->%d rem=%d->%d remDec=%d half=%d->%d "
		"wantsDuck_r=%d btnDuck=%d toggle=%d force=%d slide=%d "
		"supZip=%d supLunge=%d supForce=%d supTurret=%d/%d supGlide=%d/%d "
		"supGateA=%d/%d supGateB=%d/%d supInner=%d/%d "
		"step=%d endingCollapse=%d dZ=%.3f orgZ=%.3f->%.3f flags=%X->%X "
		"ground=0x%08X zipGrace=%d calls=%llu\n",
		nCmd, pre.nDs, post.nDs, pre.nRem, post.nRem, nRemDec,
		pre.bHalf ? 1 : 0, post.bHalf ? 1 : 0,
		nWantsDuckR, nBtnDuck, pre.bToggle ? 1 : 0, pre.nForce, pre.bSlide ? 1 : 0,
		nSupZip, nSupLunge, nSupForce,
		nSupTurret, nTurretPresent, nSupGlide, nGlidePresent,
		nSupGateA, nGateAPresent, nSupGateB, nGateBPresent, nSupInner, nInnerPresent,
		nStep, nEndingCollapse, flDZ, pre.flOrgZ, post.flOrgZ,
		static_cast<unsigned>(pre.nFlags), static_cast<unsigned>(post.nFlags),
		static_cast<unsigned>(post.nGroundEnt), nZipGrace, s_nDuckCalls);
}

//-----------------------------------------------------------------------------
// Post-orig: engine writes the latch on the standing->duck edge; consumer reads later.
//-----------------------------------------------------------------------------
static double __fastcall Hook_C_GameMovement_Duck(void* ctx)
{
	uint8_t* const player = ctx
		? *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(ctx) + HD_CTX_OFF_PLAYER)
		: nullptr;

	// The census runs even with the force disabled -- it is the instrument that
	// refuted the latch theory, and it stays useful once the force is off.
	if (!player || !v_C_Player__Zipline_IsZiplining)
		return v_C_GameMovement__Duck(ctx);

	uint8_t* const mv = *reinterpret_cast<uint8_t**>(
		reinterpret_cast<uint8_t*>(ctx) + HD_CTX_OFF_MV);

	HalfDuckFsmSnap_t pre = {};
	HalfDuck_FsmSnap(player, mv, pre);

	const bool bZiplining = v_C_Player__Zipline_IsZiplining(player);
	const int nPrevDuckState = pre.nDs;
	const float flCurTime = HalfDuck_CurTime();

	// Pre-orig: Duck reads m_forceStance inside the original. Windowed clear so
	// a forced stand from some other server system is never clobbered here.
	if (bridge_zip_forcestance_predict.GetBool())
	{
		const float flPrevZipStamp = HalfDuck_LastZipTime(player);
		const bool bInWindow =
			(flCurTime - flPrevZipStamp) <= bridge_zip_forcestance_window.GetFloat();
		int* const pForce = reinterpret_cast<int*>(player + HD_PLAYER_OFF_FORCESTANCE);
		const int nPrevForce = *pForce;
		int nNewForce = nPrevForce;

		if (bZiplining && bInWindow && nPrevForce == 0)
			nNewForce = HD_FORCE_STAND;
		else if (!bZiplining && bInWindow && nPrevForce == HD_FORCE_STAND)
			nNewForce = 0;

		if (nNewForce != nPrevForce)
		{
			*pForce = nNewForce;
			// Re-snap force so the FSM line shows the value Duck consumed.
			pre.nForce = nNewForce;

			const bool bRaise = (nNewForce == HD_FORCE_STAND);
			if (bRaise)
				++s_nForceRaises;
			else
				++s_nForceClears;

			const uint64_t nWrites = s_nForceRaises + s_nForceClears;
			if (bridge_halfduck_zip_debug.GetBool()
				|| nWrites <= 64
				|| (nWrites % 16) == 0)
			{
				Msg(eDLL_T::CLIENT,
					"[ZIP-FORCE] cmd=%d %s force=%d->%d dt=%.3f raises=%llu clears=%llu\n",
					HalfDuck_ReadCmdNumber(player),
					bRaise ? "raise" : "clear",
					nPrevForce, nNewForce,
					flCurTime - flPrevZipStamp,
					s_nForceRaises, s_nForceClears);
			}
		}
	}

	const double flResult = v_C_GameMovement__Duck(ctx);

	HalfDuckFsmSnap_t post = {};
	HalfDuck_FsmSnap(player, mv, post);

	float& flLastZipTime = HalfDuck_LastZipTime(player);

	++s_nDuckCalls;

	if (bZiplining)
	{
		// The premise of the whole fix: Duck runs while ziplining, so the
		// suppression edge is observable from in here. Announce the first one.
		if (s_nZipCommands == 0 && bridge_halfduck_zip_debug.GetBool())
			Msg(eDLL_T::CLIENT, "[HALFDUCK] first ziplining Duck command (after %llu calls) -- window can arm\n", s_nDuckCalls);

		++s_nZipCommands;
		flLastZipTime = flCurTime;
	}

	const int nDuckState = post.nDs;
	const float flSinceZip = flCurTime - flLastZipTime;
	const bool bInGraceWindow = flSinceZip <= bridge_halfduck_zip_grace_time.GetFloat();
	const int nZipGrace = bInGraceWindow ? 1 : 0;

	if (nPrevDuckState == HD_DS_STANDING && nDuckState == HD_DS_DUCK_STARTING)
	{
		bool* const pDoingHalfDuck = reinterpret_cast<bool*>(player + HD_PLAYER_OFF_DOINGHALFDUCK);
		const int nGroundEnt = *reinterpret_cast<const int*>(player + HD_PLAYER_OFF_GROUNDENT);
		const bool bNatural = *pDoingHalfDuck;
		const bool bForced = bridge_halfduck_zip_parity.GetInt() != 2;
		const bool bInGrace = bInGraceWindow;

		++s_nStartEdges;

		if (bInGrace && bridge_halfduck_zip_parity.GetInt())
		{
			++s_nEdgesInGrace;

			if (bNatural != bForced)
				++s_nForcedCount;

			*pDoingHalfDuck = bForced;
		}

		if (bridge_halfduck_zip_debug.GetBool())
		{
			Msg(eDLL_T::CLIENT,
				"[HALFDUCK] duck-start edge #%llu: inGrace=%d dt=%.3f groundEnt=0x%08X natural=%d -> %d "
				"| calls=%llu zipCmds=%llu edges=%llu inGrace=%llu forced=%llu\n",
				s_nStartEdges, bInGrace ? 1 : 0, flSinceZip, static_cast<unsigned>(nGroundEnt),
				bNatural ? 1 : 0, *pDoingHalfDuck ? 1 : 0,
				s_nDuckCalls, s_nZipCommands, s_nStartEdges, s_nEdgesInGrace, s_nForcedCount);
		}
	}

	// Re-snap half after a possible force write so the FSM line sees the latch
	// the rest of the transition will actually consume.
	post.bHalf = *reinterpret_cast<const bool*>(player + HD_PLAYER_OFF_DOINGHALFDUCK);

	HalfDuck_FsmTraceEmit(player, pre, post, bZiplining, nZipGrace);

	return flResult;
}

void VHalfDuckZipParityClient::GetAdr(void) const
{
	LogFunAdr("C_GameMovement::Duck", v_C_GameMovement__Duck);
	LogFunAdr("C_Player::Zipline_IsZiplining", v_C_Player__Zipline_IsZiplining);
	LogFunAdr("C_Player::Lunge_IsActive", v_C_Player__Lunge_IsActive);
}

void VHalfDuckZipParityClient::GetFun(void) const
{
	// C_GameMovement::Duck. xmm0 return must stay double.
	Module_FindPattern(g_GameDll,
		"48 8B C4 53 57 48 83 EC ?? 4C 8B 41")
		.GetPtr(v_C_GameMovement__Duck);

	// C_Player::Zipline_IsZiplining. Pinned by m_activeZipline handle load.
	Module_FindPattern(g_GameDll,
		"48 83 EC 28 8B 81 E8 2E 00 00 83 F8 FF 74 ?? 0F B7 C8 48 8D 15 ?? ?? ?? ?? "
		"48 C1 E1 05 C1 E8 10 39 44 11 08")
		.GetPtr(v_C_Player__Zipline_IsZiplining);

	// C_Player::Lunge_IsActive. Pinned by lunge-active byte @+16608 and target @+16604.
	Module_FindPattern(g_GameDll,
		"80 B9 E0 40 00 00 00 75 ?? 8B 81 DC 40 00 00 83 F8 FF 74 ?? 0F B7 D0")
		.GetPtr(v_C_Player__Lunge_IsActive);

	if (!v_C_GameMovement__Duck)
		Warning(eDLL_T::CLIENT, "[HALFDUCK] C_GameMovement::Duck pattern unresolved -- zipline-exit half-duck parity disabled\n");

	if (!v_C_Player__Zipline_IsZiplining)
		Warning(eDLL_T::CLIENT, "[HALFDUCK] C_Player::Zipline_IsZiplining pattern unresolved -- zipline-exit half-duck parity disabled\n");

	if (!v_C_Player__Lunge_IsActive)
		Warning(eDLL_T::CLIENT, "[HALFDUCK-FSM] C_Player::Lunge_IsActive pattern unresolved -- supLunge=-1\n");
}

void VHalfDuckZipParityClient::Detour(const bool bAttach) const
{
	if (v_C_GameMovement__Duck)
		DetourSetup(&v_C_GameMovement__Duck, &Hook_C_GameMovement_Duck, bAttach);
}

