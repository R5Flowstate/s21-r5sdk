//=============================================================================//
//
// Purpose: keep m_doingHalfDuck from diverging after a zipline release.
// Latch is not networked; grace window is keyed on each engine's own release.
// Twin of game/client/halfduck_zip_parity.cpp -- same ConVar names and defaults.
//
//=============================================================================//
#include "core/stdafx.h"


#include "tier1/cvar.h"
#include "halfduck_zip_parity.h"
#include "game/server/zipline_cooldown.h"
#include "game/server/zipline_disconnect.h"
#include "game/shared/dt_extend.h"
#include "game/shared/edict_dirty.h"

//-----------------------------------------------------------------------------
// Layout from this engine's Duck. m_doingHalfDuck is not replicated.
//-----------------------------------------------------------------------------
// CGameMovement ctx (same offsets tapstrafe.cpp / mantle_boost.cpp use)
static constexpr ptrdiff_t HD_CTX_OFF_PLAYER = 8;   // CPlayer*
static constexpr ptrdiff_t HD_CTX_OFF_MV     = 16;  // CMoveData*

// CPlayer. m_doingHalfDuck is two ints past m_duckState; written raw, no dirty-mark.
static constexpr ptrdiff_t HD_PLAYER_OFF_DUCKSTATE     = 26096;  // int
static constexpr ptrdiff_t HD_PLAYER_OFF_DOINGHALFDUCK = 26104;  // bool
static constexpr ptrdiff_t HD_PLAYER_OFF_REMAINDER     = 23216;  // int msec
static constexpr ptrdiff_t HD_PLAYER_OFF_FORCESTANCE   = 23212;  // int
static constexpr ptrdiff_t HD_PLAYER_OFF_DUCKTOGGLE    = 23208;  // bool
static constexpr ptrdiff_t HD_PLAYER_OFF_SLIDING       = 26565;  // bool
// Duck completion OR 2 / test & 2 at this slot (FL_DUCKING).
static constexpr ptrdiff_t HD_PLAYER_OFF_FFLAGS        = 564;    // int m_fFlags

// Ground-entity EHANDLE -- the very input the latch samples. Logged at the edge
// so the two engines' answers can be compared directly instead of inferred.
static constexpr ptrdiff_t HD_PLAYER_OFF_GROUNDENT = 964;   // EHANDLE, -1 = airborne

// Live cmd pointer (PlayerRunCommand installs) + embedded fallback struct.
static constexpr ptrdiff_t HD_PLAYER_OFF_CURRENTCOMMAND = 25976; // cmd*
static constexpr ptrdiff_t HD_PLAYER_OFF_EMBEDDEDCMD    = 25500; // CUserCmd body
static constexpr ptrdiff_t HD_CMD_OFF_COMMANDNUMBER     = 0x00;

// CMoveData -- Duck button-edge xor of +0x28 against +0x24 pins buttons @0x24;
// origin Z is the third float of the abs-origin vec.
static constexpr ptrdiff_t HD_MV_OFF_ORIGIN  = 292;
static constexpr ptrdiff_t HD_MV_OFF_BUTTONS = 0x24;

// DuckState. Only the standing -> starting edge latches.
static constexpr int HD_DS_STANDING      = 0;
static constexpr int HD_DS_DUCK_STARTING = 1;
static constexpr int HD_DS_DUCKED        = 2;
static constexpr int HD_DS_DUCK_ENDING   = 3;

static constexpr int HD_IN_DUCK     = 4;
static constexpr int HD_FL_DUCKING  = 0x2;
static constexpr int HD_FORCE_STAND = 1;

//-----------------------------------------------------------------------------
// CGameMovement::Duck. xmm0 return must stay double.
//-----------------------------------------------------------------------------
static double (*v_CGameMovement__Duck)(void* ctx) = nullptr;

// CPlayer::Zipline_IsZiplining. The engine's own suppression predicate; using
// anything weaker (a bare m_activeZipline validity test) would arm the grace
// over a different span than the suppression it is meant to track.
static bool (*v_CPlayer__Zipline_IsZiplining)(void* player) = nullptr;

// CPlayer::Lunge_IsActive -- wantsDuck suppressor; optional for the FSM dump.
static bool (*v_CPlayer__Lunge_IsActive)(void* player) = nullptr;

static ConVar bridge_halfduck_zip_parity("bridge_halfduck_zip_parity", "0", FCVAR_RELEASE,
	"Force m_doingHalfDuck on a duck that begins just after a zipline release, so both "
	"engines apply the half-hull origin step together. 0 = off, 1 = force set (airborne, "
	"the physical answer), 2 = force clear.", true, 0.f, true, 2.f);

static ConVar bridge_halfduck_zip_grace("bridge_halfduck_zip_grace", "3", FCVAR_RELEASE,
	"Movement commands after this engine's own zipline release during which a duck-start "
	"is treated as airborne. Must match the client's value.", true, 0.f, true, 32.f);

static ConVar bridge_halfduck_zip_debug("bridge_halfduck_zip_debug", "0", FCVAR_DEVELOPMENTONLY,
	"[HALFDUCK] Log every forced latch, not just the first few.");

static ConVar bridge_halfduck_fsm_trace("bridge_halfduck_fsm_trace", "0", FCVAR_DEVELOPMENTONLY,
	"[HALFDUCK-FSM] 0=off 1=every Duck call 2=interesting+heartbeat. "
	"Must match on client and dedi for joinable logs.", true, 0.f, true, 2.f);

static ConVar bridge_halfduck_fsm_heartbeat("bridge_halfduck_fsm_heartbeat", "32", FCVAR_RELEASE,
	"[HALFDUCK-FSM] mode-2: emit a line every N Duck calls even if boring.",
	true, 1.f, true, 512.f);

//-----------------------------------------------------------------------------
// Per-player grace, keyed by pointer. Idle players do not hold a slot.
//-----------------------------------------------------------------------------
static constexpr int HD_MAX_TRACKED = 64;

struct HalfDuckSlot_t
{
	const void* pPlayer;
	int nGrace;
};

static HalfDuckSlot_t s_halfDuckGrace[HD_MAX_TRACKED] = { };

// Census: nDuckCalls climbing while nZipCommands stays 0 means Duck does not run while ziplining.
static uint64_t s_nDuckCalls    = 0;   // hook entered
static uint64_t s_nZipCommands  = 0;   //... and Zipline_IsZiplining was true
static uint64_t s_nStartEdges   = 0;   //... standing -> duck-starting seen
static uint64_t s_nEdgesInGrace = 0;   //... and inside the grace window
static uint64_t s_nForcedCount  = 0;   //... and the latch actually changed

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
};

static int& HalfDuck_Grace(const void* const pPlayer)
{
	int nFree = -1;

	for (int i = 0; i < HD_MAX_TRACKED; ++i)
	{
		if (s_halfDuckGrace[i].pPlayer == pPlayer)
			return s_halfDuckGrace[i].nGrace;

		if (nFree < 0 && s_halfDuckGrace[i].nGrace <= 0)
			nFree = i;
	}

	// Every slot is holding live grace. Recycling slot 0 costs at most one
	// unforced latch; it can never produce a wrongly forced one.
	const int nSlot = nFree < 0 ? 0 : nFree;

	s_halfDuckGrace[nSlot].pPlayer = pPlayer;
	s_halfDuckGrace[nSlot].nGrace = 0;

	return s_halfDuckGrace[nSlot].nGrace;
}

static int HalfDuck_ReadCmdNumber(void* const player)
{
	if (!player)
		return -1;

	const uintptr_t p = reinterpret_cast<uintptr_t>(player);
	const __int64 pCmd = *reinterpret_cast<__int64*>(p + HD_PLAYER_OFF_CURRENTCOMMAND);
	if (pCmd)
		return static_cast<int>(*reinterpret_cast<uint32_t*>(pCmd + HD_CMD_OFF_COMMANDNUMBER));

	return static_cast<int>(*reinterpret_cast<uint32_t*>(p + HD_PLAYER_OFF_EMBEDDEDCMD + HD_CMD_OFF_COMMANDNUMBER));
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
		Msg(eDLL_T::SERVER,
			"[HALFDUCK-FSM] ARM side=DEDI mode=%d heartbeat=%d duckPat=%p zipPat=%p\n",
			nMode, bridge_halfduck_fsm_heartbeat.GetInt(),
			reinterpret_cast<void*>(v_CGameMovement__Duck),
			reinterpret_cast<void*>(v_CPlayer__Zipline_IsZiplining));
	}
}

static void HalfDuck_FsmTraceEmit(
	void* const player,
	const HalfDuckFsmSnap_t& pre,
	const HalfDuckFsmSnap_t& post,
	const bool bZiplining,
	const int nGrace)
{
	const int nMode = bridge_halfduck_fsm_trace.GetInt();
	HalfDuck_FsmArmIfNeeded(nMode);
	if (nMode <= 0)
		return;

	if (!v_CGameMovement__Duck)
	{
		if (!s_bFsmDuckPatWarned)
		{
			s_bFsmDuckPatWarned = true;
			Warning(eDLL_T::SERVER,
				"[HALFDUCK-FSM] WARN Duck pattern unresolved -- fsm trace inert\n");
		}
		return;
	}

	if (!s_bFsmFirstDuckArmed)
	{
		s_bFsmFirstDuckArmed = true;
		Msg(eDLL_T::SERVER,
			"[HALFDUCK-FSM] ARM side=DEDI mode=%d heartbeat=%d duckPat=%p zipPat=%p\n",
			nMode, bridge_halfduck_fsm_heartbeat.GetInt(),
			reinterpret_cast<void*>(v_CGameMovement__Duck),
			reinterpret_cast<void*>(v_CPlayer__Zipline_IsZiplining));
	}

	++s_nFsmTraceCalls;

	const int nCmd = HalfDuck_ReadCmdNumber(player);
	const int nRemDec = pre.nRem - post.nRem;
	const int nBtnDuck = (pre.nButtons & HD_IN_DUCK) != 0 ? 1 : 0;
	const int nSupZip = bZiplining ? 1 : 0;
	const int nSupLunge = v_CPlayer__Lunge_IsActive
		? (v_CPlayer__Lunge_IsActive(player) ? 1 : 0)
		: -1;
	const int nSupForce = (pre.nForce == HD_FORCE_STAND) ? 1 : 0;
	// Client-only gates: val/present=0/0 (absent, not false).
	const int nSupTurret = 0, nTurretPresent = 0;
	const int nSupGlide  = 0, nGlidePresent  = 0;
	const int nSupGateA  = 0, nGateAPresent  = 0;
	const int nSupGateB  = 0, nGateBPresent  = 0;
	const int nSupInner  = 0, nInnerPresent  = 0;

	const int nLungeForWant = (nSupLunge > 0) ? 1 : 0;
	const int nWantsDuckR =
		((nBtnDuck || pre.bToggle) && !nSupZip && !nLungeForWant && !nSupForce) ? 1 : 0;

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
		(nSupZip == 1) || (nSupLunge == 1) || (nSupForce == 1);
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

	Msg(eDLL_T::SERVER,
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
		static_cast<unsigned>(post.nGroundEnt), nGrace, s_nDuckCalls);
}

//-----------------------------------------------------------------------------
// Post-orig: engine writes the latch on the standing->duck edge; consumer reads later.
//-----------------------------------------------------------------------------
static double __fastcall Hook_CGameMovement_Duck(void* ctx)
{
	uint8_t* const player = ctx
		? *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(ctx) + HD_CTX_OFF_PLAYER)
		: nullptr;

	// The census runs even with the force disabled -- it is the instrument that
	// refuted the latch theory, and it stays useful once the force is off.
	if (!player || !v_CPlayer__Zipline_IsZiplining)
		return v_CGameMovement__Duck(ctx);

	uint8_t* const mv = *reinterpret_cast<uint8_t**>(
		reinterpret_cast<uint8_t*>(ctx) + HD_CTX_OFF_MV);

	HalfDuckFsmSnap_t pre = {};
	HalfDuck_FsmSnap(player, mv, pre);

	const bool bZiplining = v_CPlayer__Zipline_IsZiplining(player);
	const int nPrevDuckState = pre.nDs;

	const double flResult = v_CGameMovement__Duck(ctx);

	HalfDuckFsmSnap_t post = {};
	HalfDuck_FsmSnap(player, mv, post);

	int& nGrace = HalfDuck_Grace(player);

	++s_nDuckCalls;

	if (bZiplining)
	{
		// The premise of the whole fix: Duck runs while ziplining, so the
		// suppression edge is observable from in here. Announce the first one.
		if (s_nZipCommands == 0 && bridge_halfduck_zip_debug.GetBool())
			Msg(eDLL_T::SERVER, "[HALFDUCK] first ziplining Duck command (after %llu calls) -- grace can arm\n", s_nDuckCalls);

		++s_nZipCommands;
		nGrace = bridge_halfduck_zip_grace.GetInt();
		ZiplineCooldown_OnRideCommand(player);
	}
	else if (nGrace > 0)
	{
		--nGrace;
	}

	// Ground state every command: remount ladder clears on landing.
	const bool bZipNow = v_CPlayer__Zipline_IsZiplining(player);
	ZiplineCooldown_OnGroundState(player, post.nGroundEnt != -1, bZipNow);
	ZipDisc_OnCommand(player, post.nGroundEnt != -1, bZipNow);

	const int nDuckState = post.nDs;

	if (nPrevDuckState == HD_DS_STANDING && nDuckState == HD_DS_DUCK_STARTING)
	{
		bool* const pDoingHalfDuck = reinterpret_cast<bool*>(player + HD_PLAYER_OFF_DOINGHALFDUCK);
		const int nGroundEnt = *reinterpret_cast<const int*>(player + HD_PLAYER_OFF_GROUNDENT);
		const bool bNatural = *pDoingHalfDuck;
		const bool bForced = bridge_halfduck_zip_parity.GetInt() != 2;
		const bool bInGrace = nGrace > 0;

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
			Msg(eDLL_T::SERVER,
				"[HALFDUCK] duck-start edge #%llu: inGrace=%d grace=%d groundEnt=0x%08X natural=%d -> %d "
				"| calls=%llu zipCmds=%llu edges=%llu inGrace=%llu forced=%llu\n",
				s_nStartEdges, bInGrace ? 1 : 0, nGrace, static_cast<unsigned>(nGroundEnt),
				bNatural ? 1 : 0, *pDoingHalfDuck ? 1 : 0,
				s_nDuckCalls, s_nZipCommands, s_nStartEdges, s_nEdgesInGrace, s_nForcedCount);
		}
	}

	// Re-snap half after a possible force write so the FSM line sees the latch
	// the rest of the transition will actually consume.
	post.bHalf = *reinterpret_cast<const bool*>(player + HD_PLAYER_OFF_DOINGHALFDUCK);

	HalfDuck_FsmTraceEmit(player, pre, post, bZiplining, nGrace);

	return flResult;
}

bool HalfDuck_PlayerIsZiplining(void* player)
{
	if (!player || !v_CPlayer__Zipline_IsZiplining)
		return false;
	return v_CPlayer__Zipline_IsZiplining(player);
}

void VHalfDuckZipParity::GetAdr(void) const
{
	LogFunAdr("CGameMovement::Duck", v_CGameMovement__Duck);
	LogFunAdr("CPlayer::Zipline_IsZiplining", v_CPlayer__Zipline_IsZiplining);
	LogFunAdr("CPlayer::Lunge_IsActive", v_CPlayer__Lunge_IsActive);
}

void VHalfDuckZipParity::GetFun(void) const
{
	// CGameMovement::Duck. Ctx unpack + button-edge xor separates the live movement twin.
	Module_FindPattern(g_GameDll,
		"48 8B C4 57 48 81 EC ?? ?? ?? ?? 4C 8B 41 10 48 8B F9 48 8B 49 08 41 8B 50 28 41 33 50 24")
		.GetPtr(v_CGameMovement__Duck);

	// CPlayer::Zipline_IsZiplining. Pinned by m_activeZipline handle load.
	Module_FindPattern(g_GameDll,
		"48 83 EC 28 8B 91 D4 67 00 00 8B C2 83 FA FF 74 ?? 0F B7 C2 C1 EA 10 48 8D 0C 40 "
		"48 03 C9 48 8D 05 ?? ?? ?? ?? 39 54 C8 08")
		.GetPtr(v_CPlayer__Zipline_IsZiplining);

	// CPlayer::Lunge_IsActive. Pinned by lunge-active byte @+28412 and target @+28408.
	Module_FindPattern(g_GameDll,
		"80 B9 FC 6E 00 00 00 75 ?? 8B 91 F8 6E 00 00 8B C2 83 FA FF")
		.GetPtr(v_CPlayer__Lunge_IsActive);

	if (!v_CGameMovement__Duck)
		Warning(eDLL_T::SERVER, "[HALFDUCK] CGameMovement::Duck pattern unresolved -- zipline-exit half-duck parity disabled\n");

	if (!v_CPlayer__Zipline_IsZiplining)
		Warning(eDLL_T::SERVER, "[HALFDUCK] CPlayer::Zipline_IsZiplining pattern unresolved -- zipline-exit half-duck parity disabled\n");

	if (!v_CPlayer__Lunge_IsActive)
		Warning(eDLL_T::SERVER, "[HALFDUCK-FSM] CPlayer::Lunge_IsActive pattern unresolved -- supLunge=-1\n");
}

void VHalfDuckZipParity::Detour(const bool bAttach) const
{
	if (v_CGameMovement__Duck)
		DetourSetup(&v_CGameMovement__Duck, &Hook_CGameMovement_Duck, bAttach);
}

