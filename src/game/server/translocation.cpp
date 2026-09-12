//=============================================================================//
//
// Purpose: S3 native gap-fills for translocation. See header.
//
//=============================================================================//

#include "core/stdafx.h"
#include "translocation.h"
#include "baseentity.h"
#include "player.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "game/shared/weapon_script_vars.h"
#include "game/server/jetdrive.h"
#include "game/server/player_launch.h"
#include "game/server/trigger_cannon.h"
#include "game/server/melee_activity_trace.h"
#include "game/shared/collisionproperty.h"
#include "game/shared/activity.h"
#include <cstdint>
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "vscript_server.h"
#include "vscript_server_placement.h"
#include "game/shared/dt_extend.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/deathfield_system.h"

#include <unordered_map>
#include <vector>
#include <cmath>
#include <cstring>
#include <intrin.h>

extern CGlobalVars* gpGlobals;

// S21 RegisterScriptConsts: GSF_PLANTED=1, GSF_REDIRECTED=2. Do not also
// declare these in sh_consts.gnut -- that redefines them and kills compile.
static constexpr int GSF_PLANTED_S21 = 1;
static constexpr int GSF_REDIRECTED_S21 = 2;

// EF_NOINTERP. S21 TeleportPlayerNoInterp_Script or's this before SetAbsOrigin
// so the dest does not interpolate from the pre-warp origin.
static constexpr int EF_NOINTERP = 8;

static constexpr ptrdiff_t kEntOffAbsOrigin = 0x450;
static constexpr ptrdiff_t PLAYER_OFF_BUTTONS = 0x60DC;
static constexpr ptrdiff_t PLAYER_OFF_BUTTONPRESSED = 0x60E0;
static constexpr int kInAttack = 1; // IN_ATTACK
static constexpr int kInOffhand1 = 0x200000; // IN_OFFHAND1
static constexpr int kDropClickBits = kInAttack | kInOffhand1;
static constexpr ptrdiff_t WEAPON_CLASSNAME_OFFSET = 0x15B0;
static constexpr ptrdiff_t WEAPON_OFF_MODVARS = 0x17E0;
static constexpr ptrdiff_t WEAPON_OFF_CUSTOM_BOOL_5 = 0xE49; // toss_has_post_loop
static constexpr ptrdiff_t WEAPON_OFF_NEXTREADYTIME = 0x11F8; // m_nextReadyTime
static constexpr ptrdiff_t WEAPON_OFF_NEXTPRIMARYATTACK = 0x11FC; // m_nextPrimaryAttackTime
static constexpr ptrdiff_t WEAPON_OFF_TIMEWEAPONIDLE = 0x1230; // m_flTimeWeaponIdle
static constexpr ptrdiff_t WEAPON_OFF_WEAPSTATE = 0x1234;
static constexpr ptrdiff_t WEAPON_OFF_OWNER = 0x11F0;
static constexpr ptrdiff_t PLAYER_OFF_NOINTERP_PARITY = 0x6D40; // m_ubEFNoInterpParity
static constexpr ptrdiff_t PLAYER_OFF_FLAGS = 0x234;
static constexpr ptrdiff_t PLAYER_OFF_DUCKSTATE = 0x65F0;
static constexpr ptrdiff_t PLAYER_OFF_DOINGHALFDUCK = 0x65F8;
static constexpr ptrdiff_t PLAYER_OFF_DUCKTOGGLE = 0x5AA8;
static constexpr ptrdiff_t PLAYER_OFF_FORCESTANCE = 0x5AAC; // 0 none, 1 stand, 2 crouch
static constexpr ptrdiff_t PLAYER_OFF_DUCK_REMAINDER = 0x5AB0;
static constexpr ptrdiff_t PLAYER_OFF_DUCK_HULL_MIN = 0x6614;
static constexpr ptrdiff_t PLAYER_OFF_DUCK_HULL_MAX = 0x6620;
static constexpr ptrdiff_t PLAYER_OFF_STAND_HULL_MIN = 0x65FC;
static constexpr ptrdiff_t PLAYER_OFF_STAND_HULL_MAX = 0x6608;
static constexpr ptrdiff_t PLAYER_OFF_ACTIVE_MAINHAND = 0x16CC; // inventory + activeWeapons[0]
static constexpr ptrdiff_t WEAPON_OFF_CUSTOM_ACT_FLAGS = 0x1254; // m_customActivityFlags
static constexpr ptrdiff_t WEAPON_OFF_ACTIVE_SLOT = 0x2930; // m_latestActiveInventorySlot
static constexpr unsigned int WEAP_STATE_IDLE = 0;
static constexpr unsigned int WEAP_STATE_HOLSTERED = 2;
static constexpr unsigned int WEAP_STATE_CUSTOM_ACTIVITY = 12;
static constexpr unsigned int WEAP_STATE_TOSS = 14; // S3 TOSS; S21 goes POST_TOSS_LOOP here
static constexpr unsigned int WEAP_STATE_POST_TOSS_LOOP = 19;
static constexpr unsigned char WCAF_PLAYRAISE_S3 = 2;
static constexpr unsigned char WCAF_PLAYRAISE_S21 = 0x80;
static constexpr unsigned int ACT_VM_IDLE_S3 = 468;
static constexpr unsigned int ACT_VM_HOLSTER_S3 = 453;
static constexpr int kMoveTypeWalk = 2; // MOVETYPE_WALK
static constexpr int kFlDucking = 2;
static constexpr int kFlOnGround = 1;
static constexpr int kDuckStateStanding = 0; // DS_STANDING
// CPlayerLocalData: duckToggle 0x5AA8 = localdata+0x18, fallVel is localdata+0x48.
static constexpr ptrdiff_t PLAYER_OFF_FALLVEL = 0x5AD8;
static constexpr ptrdiff_t PLAYER_OFF_HAS_JUMPED = 0x6230; // m_bHasJumpedSinceTouchedGround
static constexpr ptrdiff_t PLAYER_OFF_PUSHAWAY = 30700; // m_pushAwayFromTopAcceleration

// TeleportPlayerNoInterp step selector, so the placement path can be bisected
// against a plain SetOrigin without a rebuild.
static constexpr int kTeleportStepNoInterp     = 1 << 0;
static constexpr int kTeleportStepSettleStance = 1 << 1;
static constexpr int kTeleportStepDuck         = 1 << 2;
static constexpr int kTeleportStepBounds       = 1 << 3;
static constexpr int kTeleportStepMoveType     = 1 << 4;


static SDKEntityMap<int> s_cmdButtonSticky(ESide::Server, "transloc.cmdSticky");
static SDKEntityMap<uint8_t> s_flightHoldWeapons(ESide::Server, "transloc.flightHold");
static SDKEntityMap<uint8_t> s_tossOneHandPlayers(ESide::Server, "transloc.oneHand");
static SDKEntityMap<uint8_t> s_tossLoopWeapons(ESide::Server, "transloc.tossLoop");
static SDKEntityMap<uint8_t> s_tossLoopNoProj(ESide::Server, "transloc.tossLoopNoProj");
static SDKEntityMap<unsigned int> s_tossLoopPrevState(ESide::Server, "transloc.tossPrevState");
static SDKEntityMap<SDKEntityHandle> s_tossWeaponByOwner(ESide::Server, "transloc.tossWeap");
static SDKEntityMap<SDKEntityHandle> s_tossProjHandle(ESide::Server, "transloc.tossProj");
static SDKEntityMap<float> s_tossLoopBeginTime(ESide::Server, "transloc.tossBeginT");
static const void* s_pTossCompleteHolsterRet = nullptr;
static int s_postTossActId = -1;
static int s_pickupActId = -1;
static int s_missActId = -1;

static __int64 (*v_WeaponX_SetWeaponState)(void* pWeapon, unsigned int state) = nullptr;
static char (*v_SetIdealWeaponActivity)(void* pWeapon, unsigned int activity) = nullptr;
static char (*v_WeaponX_StartCustomActivity)(void* pWeapon, unsigned int activity, unsigned char flags) = nullptr;
static void (*v_CPlayer_DuckImmediate)(void* pPlayer) = nullptr;
static char (*v_CBaseEntity_SetMoveType)(void* pEnt, int moveType, char moveCollide) = nullptr;
static void (*v_CPlayer_SetOneHandedOn)(void* pPlayer) = nullptr;
static void (*v_CPlayer_SetOneHandedOff)(void* pPlayer) = nullptr;

static SQRESULT (*v_IsInputCommandPressed)(HSQUIRRELVM v) = nullptr;
static ConVar bridge_translocation_diag("bridge_translocation_diag", "0",
	FCVAR_DEVELOPMENTONLY,
	"Log translocation native first-calls and TeleportPlayerNoInterp results.");
static ConVar bridge_teleport_steps("bridge_teleport_steps", "31",
	FCVAR_RELEASE,
	"TeleportPlayerNoInterp steps to run, as a bitmask. 1 = EF_NOINTERP, "
	"2 = settle stance, 4 = DuckImmediate, 8 = collision bounds, "
	"16 = SetMoveType. 0 leaves only SetAbsOrigin.", true, 0.f, true, 31.f);
static ConVar bridge_plant_on_ground("bridge_plant_on_ground", "1",
	FCVAR_RELEASE,
	"PlantOnGround after a floor teleport: zero fall velocity, clear jump-since-land, "
	"and SetGroundEntity(world) so the snapshot is standing. 0 = leave airborne.");
static ConVar bridge_tossloop_clock_hold("bridge_tossloop_clock_hold", "0.75",
	FCVAR_RELEASE,
	"Seconds ahead of server time the post-toss-loop weapon fire/idle clocks "
	"are held while the loop is open. 0 = leave the native stamps alone.");

static void Translocation_SetFlightHold(void* pWeapon, bool bHold)
{
	if (!pWeapon)
		return;
	if (bHold)
		s_flightHoldWeapons[pWeapon] = 1;
	else
		s_flightHoldWeapons.Erase(pWeapon);

	static bool s_bLoggedOnce = false;
	if (!s_bLoggedOnce || bridge_translocation_diag.GetBool())
	{
		s_bLoggedOnce = true;
		Msg(eDLL_T::SERVER, "[TRANSLOC] flight hold weapon=%p hold=%d\n",
			pWeapon, bHold ? 1 : 0);
	}
}

static bool Translocation_ShouldHoldOffhand(void* pWeapon)
{
	return pWeapon && s_flightHoldWeapons.Find(pWeapon) != nullptr;
}

static bool Translocation_WeaponHasTossPostLoop(void* pWeapon)
{
	if (!pWeapon)
		return false;
	const uint8_t flag = *reinterpret_cast<const uint8_t*>(
		reinterpret_cast<uintptr_t>(pWeapon) + WEAPON_OFF_MODVARS
		+ WEAPON_OFF_CUSTOM_BOOL_5);
	return flag != 0;
}

static void* Translocation_WeaponOwner(void* pWeapon)
{
	if (!pWeapon)
		return nullptr;
	const uint32_t raw = *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<uintptr_t>(pWeapon) + WEAPON_OFF_OWNER);
	return SDKEntityState_Resolve(SDKEntityHandle(raw), ESide::Server);
}

static int Translocation_PostTossActivity(void)
{
	if (s_postTossActId > 0)
		return s_postTossActId;
	s_postTossActId = FindActivityByName("ACT_VM_POST_TOSS_LOOP");
	if (s_postTossActId <= 0)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER,
				"[TRANSLOC] ACT_VM_POST_TOSS_LOOP not in the dedi activity table -- "
				"flight pose cannot replicate\n");
		}
	}
	else
	{
		Msg(eDLL_T::SERVER, "[TRANSLOC] ACT_VM_POST_TOSS_LOOP id=%d\n", s_postTossActId);
	}
	return s_postTossActId;
}

static int Translocation_NamedActivity(int* pCache, const char* pszName)
{
	if (*pCache > 0)
		return *pCache;
	*pCache = FindActivityByName(pszName);
	return *pCache;
}

static bool Translocation_IsEndTossActivity(unsigned int activity)
{
	const int pickup = Translocation_NamedActivity(&s_pickupActId, "ACT_VM_PICKUP");
	const int miss = Translocation_NamedActivity(&s_missActId, "ACT_VM_MISSCENTER");
	return (pickup > 0 && activity == static_cast<unsigned int>(pickup))
		|| (miss > 0 && activity == static_cast<unsigned int>(miss));
}

static void Translocation_PlayPostTossAnim(void* pWeapon)
{
	const int actId = Translocation_PostTossActivity();
	if (!pWeapon || actId <= 0 || !v_SetIdealWeaponActivity)
		return;
	v_SetIdealWeaponActivity(pWeapon, static_cast<unsigned int>(actId));
}

static unsigned int Translocation_WeaponState(void* pWeapon);
static bool Translocation_OwnerAlive(void* pWeapon);
static void Translocation_ClearOneHand(void* pWeapon);
static void Translocation_ApplyOneHand(void* pWeapon);
static void Translocation_ArmTossLoopClocks(void* pWeapon);
static void Translocation_ReleaseTossLoopClocks(void* pWeapon);

static void Translocation_WriteWeaponState(void* pWeapon, unsigned int state)
{
	if (v_WeaponX_SetWeaponState)
		v_WeaponX_SetWeaponState(pWeapon, state);
	else
	{
		*reinterpret_cast<unsigned int*>(
			reinterpret_cast<uintptr_t>(pWeapon) + WEAPON_OFF_WEAPSTATE) = state;
		MarkEntityEdictDirty(pWeapon);
	}
}

static void Translocation_LogTossLoop(const char* pszEdge, void* pWeapon,
	unsigned int stateIn, unsigned int stateOut, float held, int holstered)
{
	static int s_nLog = 24;
	if (s_nLog <= 0 && !bridge_translocation_diag.GetBool())
		return;
	if (s_nLog > 0)
		--s_nLog;

	void* const pOwner = Translocation_WeaponOwner(pWeapon);
	const int oneHand = (pOwner && s_tossOneHandPlayers.Find(pOwner)) ? 1 : 0;
	if (pszEdge && strcmp(pszEdge, "end") == 0)
	{
		Msg(eDLL_T::SERVER,
			"[TRANSLOC] tossloop end weapon=%p state %u -> %u oneHand=%d held=%.3f holster=%d\n",
			pWeapon, stateIn, stateOut, oneHand, held, holstered);
	}
	else
	{
		Msg(eDLL_T::SERVER,
			"[TRANSLOC] tossloop begin weapon=%p state %u -> %u oneHand=%d\n",
			pWeapon, stateIn, stateOut, oneHand);
	}
}

// divertedState is the state the engine asked for and we overrode; EndTossLoop
// replays it so the weapon never outlives the loop parked in POST_TOSS_LOOP.
static void Translocation_BeginTossLoop(void* pWeapon, unsigned int divertedState)
{
	if (!pWeapon)
		return;
	const unsigned int stateIn = Translocation_WeaponState(pWeapon);
	s_tossLoopWeapons[pWeapon] = 1;
	s_tossLoopPrevState[pWeapon] = divertedState;
	s_tossLoopBeginTime[pWeapon] = gpGlobals ? gpGlobals->curTime : 0.0f;
	Translocation_WriteWeaponState(pWeapon, WEAP_STATE_POST_TOSS_LOOP);
	Translocation_PlayPostTossAnim(pWeapon);
	Translocation_LogTossLoop("begin", pWeapon, stateIn, WEAP_STATE_POST_TOSS_LOOP, 0.0f, 0);
}

// bRestoreState is false only where the engine is itself driving the weapon out
// of the loop (an end-toss activity, or a holster that is about to run) -- a
// forced write there would fight it.
static void Translocation_EndTossLoop(void* pWeapon, bool bRestoreState)
{
	if (!pWeapon)
		return;

	const bool bWasInLoop = s_tossLoopWeapons.Find(pWeapon) != nullptr;
	const unsigned int* const pPrev = s_tossLoopPrevState.Find(pWeapon);
	const unsigned int prevState = pPrev ? *pPrev : WEAP_STATE_IDLE;
	const unsigned int stateIn = Translocation_WeaponState(pWeapon);

	const float now = gpGlobals ? gpGlobals->curTime : 0.0f;
	float held = 0.0f;
	if (const float* const pBegin = s_tossLoopBeginTime.Find(pWeapon))
		held = now - *pBegin;

	s_tossLoopWeapons.Erase(pWeapon);
	s_tossLoopNoProj.Erase(pWeapon);
	s_tossProjHandle.Erase(pWeapon);
	s_tossLoopPrevState.Erase(pWeapon);
	s_tossLoopBeginTime.Erase(pWeapon);
	if (void* const pOwner = Translocation_WeaponOwner(pWeapon))
		s_tossWeaponByOwner.Erase(pOwner);

	unsigned int stateOut = stateIn;
	if (bRestoreState && bWasInLoop && stateIn == WEAP_STATE_POST_TOSS_LOOP
		&& Translocation_OwnerAlive(pWeapon))
	{
		Translocation_WriteWeaponState(pWeapon, prevState);
		stateOut = prevState;
	}

	const bool bDoHolster = bRestoreState && bWasInLoop
		&& v_WeaponX_HolsterInternal && Translocation_OwnerAlive(pWeapon);

	// Log before the clear so the line reports the flag as it was on entry.
	if (bWasInLoop)
		Translocation_LogTossLoop("end", pWeapon, stateIn, stateOut, held, bDoHolster ? 1 : 0);

	if (bRestoreState)
		Translocation_ClearOneHand(pWeapon);

	if (bRestoreState && bWasInLoop)
	{
		Translocation_ReleaseTossLoopClocks(pWeapon);
		// Original HolsterInternal: the hook refuses toss-family holsters.
		if (bDoHolster)
			v_WeaponX_HolsterInternal(pWeapon, true);
	}
}

void Translocation_EndNoProjTossForPlayer(void* pPlayer)
{
	if (!pPlayer)
		return;

	std::vector<void*> end;
	for (auto it = s_tossLoopNoProj.begin(); it != s_tossLoopNoProj.end(); ++it)
	{
		void* const pWeapon = SDKEntityState_Resolve(it->first, ESide::Server);
		if (pWeapon && Translocation_WeaponOwner(pWeapon) == pPlayer)
			end.push_back(pWeapon);
	}
	for (void* const pWeapon : end)
		Translocation_EndTossLoop(pWeapon, true);
}

void Translocation_BeginNoProjTossForPlayer(void* pPlayer)
{
	if (!pPlayer)
		return;

	void* pWeapon = nullptr;
	if (SDKEntityHandle* const pH = s_tossWeaponByOwner.Find(pPlayer))
		pWeapon = SDKEntityState_Resolve(*pH, ESide::Server);

	if (!pWeapon)
	{
		static const ptrdiff_t s_activeOffs[3] = {
			PLAYER_OFF_ACTIVE_MAINHAND, 0x16D0, 0x16D4
		};
		for (int i = 0; i < 3; ++i)
		{
			const uint32_t raw = *reinterpret_cast<const uint32_t*>(
				reinterpret_cast<uintptr_t>(pPlayer) + s_activeOffs[i]);
			void* const pCand = SDKEntityState_Resolve(
				SDKEntityHandle(raw), ESide::Server);
			if (pCand && Translocation_WeaponHasTossPostLoop(pCand))
			{
				pWeapon = pCand;
				break;
			}
		}
	}

	if (!pWeapon || !Translocation_OwnerAlive(pWeapon))
		return;

	if (s_tossLoopWeapons.Find(pWeapon))
	{
		s_tossLoopNoProj[pWeapon] = 1;
		return;
	}

	s_tossWeaponByOwner[pPlayer] = SDKEntityState_GetHandle(pWeapon);
	// One-hand first: the post-toss activity is picked inside the begin, and the
	// client translates it through OneHandedWeaponUsageIsEnabled.
	Translocation_ApplyOneHand(pWeapon);
	Translocation_BeginTossLoop(pWeapon, WEAP_STATE_IDLE);
	s_tossLoopNoProj[pWeapon] = 1;
	Translocation_ArmTossLoopClocks(pWeapon);
}

static unsigned int Translocation_WeaponState(void* pWeapon)
{
	if (!pWeapon)
		return 0;
	return *reinterpret_cast<const unsigned int*>(
		reinterpret_cast<uintptr_t>(pWeapon) + WEAPON_OFF_WEAPSTATE);
}

static bool Translocation_OwnerAlive(void* pWeapon)
{
	void* const pOwner = Translocation_WeaponOwner(pWeapon);
	return pOwner && static_cast<CBaseEntity*>(pOwner)->Diag_LifeState() == 0;
}

static void Translocation_LatchCmdBits(void* pPlayer, int bits)
{
	if (pPlayer && bits)
		s_cmdButtonSticky[pPlayer] |= bits;
}

static bool Translocation_PlayerHasTossLoop(void* pPlayer)
{
	if (!pPlayer)
		return false;
	for (auto it = s_tossLoopWeapons.begin(); it != s_tossLoopWeapons.end(); ++it)
	{
		void* const pWeapon = SDKEntityState_Resolve(it->first, ESide::Server);
		if (pWeapon && Translocation_WeaponOwner(pWeapon) == pPlayer)
			return true;
	}
	return false;
}

static bool Translocation_ShouldEatAttack(void* pPlayer)
{
	if (!pPlayer)
		return false;
	return Translocation_PlayerHasTossLoop(pPlayer)
		|| JetDrive_IsActive(reinterpret_cast<CPlayer*>(pPlayer));
}

static bool Translocation_IsTossFamily(unsigned int state)
{
	return state >= WEAP_STATE_TOSS && state <= WEAP_STATE_POST_TOSS_LOOP;
}

static bool Translocation_HasLiveProjectile(void* pWeapon)
{
	if (!pWeapon)
		return false;
	SDKEntityHandle* const pH = s_tossProjHandle.Find(pWeapon);
	if (!pH || !pH->IsValid())
		return false;
	return SDKEntityState_Resolve(*pH, ESide::Server) != nullptr;
}

static void Translocation_BindLiveProjectile(void* pWeapon, void* pProj)
{
	if (!pWeapon || !pProj)
		return;
	s_tossProjHandle[pWeapon] = SDKEntityState_GetHandle(pProj);
	if (void* const pOwner = Translocation_WeaponOwner(pWeapon))
		s_tossWeaponByOwner[pOwner] = SDKEntityState_GetHandle(pWeapon);
	Translocation_BeginTossLoop(pWeapon, WEAP_STATE_IDLE);
	Translocation_ApplyOneHand(pWeapon);
}

static void Translocation_OnProjectileSpawned(void* pProj)
{
	if (!pProj)
		return;

	void* pWeapon = nullptr;
	const int ownerOff = DTExtend_GetOffset("DT_BaseEntity", "m_hOwnerEntity");
	if (ownerOff > 0)
	{
		const uint32_t raw = *reinterpret_cast<const uint32_t*>(
			reinterpret_cast<uintptr_t>(pProj) + static_cast<uintptr_t>(ownerOff));
		void* const pOwner = SDKEntityState_Resolve(SDKEntityHandle(raw), ESide::Server);
		if (pOwner)
		{
			if (SDKEntityHandle* const pWeapH = s_tossWeaponByOwner.Find(pOwner))
				pWeapon = SDKEntityState_Resolve(*pWeapH, ESide::Server);
		}
	}
	if (!pWeapon)
	{
		for (auto it = s_tossWeaponByOwner.begin(); it != s_tossWeaponByOwner.end(); ++it)
		{
			void* const pCand = SDKEntityState_Resolve(it->second, ESide::Server);
			if (pCand && Translocation_WeaponHasTossPostLoop(pCand)
				&& Translocation_WeaponState(pCand) == WEAP_STATE_TOSS)
			{
				pWeapon = pCand;
				break;
			}
		}
	}
	if (pWeapon)
		Translocation_BindLiveProjectile(pWeapon, pProj);
}

static void Translocation_FlushDeadToss(void)
{
	std::vector<void*> dead;
	for (auto it = s_tossLoopWeapons.begin(); it != s_tossLoopWeapons.end(); ++it)
	{
		void* const pWeapon = SDKEntityState_Resolve(it->first, ESide::Server);
		if (pWeapon && !Translocation_HasLiveProjectile(pWeapon)
			&& !s_tossLoopNoProj.Find(pWeapon))
			dead.push_back(pWeapon);
	}
	for (void* const pWeapon : dead)
		Translocation_EndTossLoop(pWeapon, true);
}

static void Translocation_ApplyOneHand(void* pWeapon)
{
	void* const pOwner = Translocation_WeaponOwner(pWeapon);
	if (!pOwner || !v_CPlayer_SetOneHandedOn)
		return;
	v_CPlayer_SetOneHandedOn(pOwner);
	s_tossOneHandPlayers[pOwner] = 1;
}

static void Translocation_ClearOneHand(void* pWeapon)
{
	void* const pOwner = Translocation_WeaponOwner(pWeapon);
	if (!pOwner)
		return;
	if (!s_tossOneHandPlayers.Find(pOwner))
		return;
	s_tossOneHandPlayers.Erase(pOwner);
	if (JetDrive_IsActive(reinterpret_cast<CPlayer*>(pOwner)))
		return;
	if (v_CPlayer_SetOneHandedOff)
		v_CPlayer_SetOneHandedOff(pOwner);
}

static void Translocation_ArmTossLoopClocks(void* pWeapon)
{
	if (!pWeapon)
		return;
	const float hold = bridge_tossloop_clock_hold.GetFloat();
	if (hold <= 0.0f)
		return;
	const float now = gpGlobals ? gpGlobals->curTime : 0.0f;
	if (now <= 0.0f)
		return;

	float* const pReady = reinterpret_cast<float*>(
		reinterpret_cast<uintptr_t>(pWeapon) + WEAPON_OFF_NEXTREADYTIME);
	if (*pReady - now > hold * 0.5f)
		return;

	const float stamp = now + hold;
	*pReady = stamp;
	*reinterpret_cast<float*>(
		reinterpret_cast<uintptr_t>(pWeapon) + WEAPON_OFF_NEXTPRIMARYATTACK) = stamp;
	*reinterpret_cast<float*>(
		reinterpret_cast<uintptr_t>(pWeapon) + WEAPON_OFF_TIMEWEAPONIDLE) = stamp;
	MarkEntityEdictDirty(pWeapon);

	if (bridge_translocation_diag.GetBool())
	{
		Msg(eDLL_T::SERVER,
			"[TRANSLOC] tossloop clocks weapon=%p ready=%.3f idle=%.3f now=%.3f\n",
			pWeapon, stamp, stamp, now);
	}
}

static void Translocation_ReleaseTossLoopClocks(void* pWeapon)
{
	if (!pWeapon)
		return;
	const float now = gpGlobals ? gpGlobals->curTime : 0.0f;
	if (now <= 0.0f)
		return;

	*reinterpret_cast<float*>(
		reinterpret_cast<uintptr_t>(pWeapon) + WEAPON_OFF_NEXTREADYTIME) = now;
	*reinterpret_cast<float*>(
		reinterpret_cast<uintptr_t>(pWeapon) + WEAPON_OFF_NEXTPRIMARYATTACK) = now;
	*reinterpret_cast<float*>(
		reinterpret_cast<uintptr_t>(pWeapon) + WEAPON_OFF_TIMEWEAPONIDLE) = now;
	MarkEntityEdictDirty(pWeapon);
}

void Translocation_OnPlayerRunCommand(void* pPlayer)
{
	if (!pPlayer)
		return;
	const int pressed = *reinterpret_cast<const int*>(
		reinterpret_cast<uintptr_t>(pPlayer) + PLAYER_OFF_BUTTONPRESSED);
	if (pressed & kDropClickBits)
		s_cmdButtonSticky[pPlayer] |= (pressed & kDropClickBits);

	for (auto it = s_tossLoopWeapons.begin(); it != s_tossLoopWeapons.end(); ++it)
	{
		void* const pWeapon = SDKEntityState_Resolve(it->first, ESide::Server);
		if (pWeapon && Translocation_WeaponOwner(pWeapon) == pPlayer)
		{
			Translocation_ArmTossLoopClocks(pWeapon);
			break;
		}
	}
}

void Translocation_SetOneHandedForPlayer(void* pPlayer, bool on)
{
	if (!pPlayer)
		return;
	if (on)
	{
		if (v_CPlayer_SetOneHandedOn)
			v_CPlayer_SetOneHandedOn(pPlayer);
		s_tossOneHandPlayers[pPlayer] = 1;
	}
	else
	{
		if (!s_tossOneHandPlayers.Find(pPlayer))
			return;
		s_tossOneHandPlayers.Erase(pPlayer);
		if (JetDrive_IsActive(reinterpret_cast<CPlayer*>(pPlayer)))
			return;
		if (v_CPlayer_SetOneHandedOff)
			v_CPlayer_SetOneHandedOff(pPlayer);
	}
}

static bool Translocation_ConsumeSticky(void* pPlayer, int cmd)
{
	int* const pBits = s_cmdButtonSticky.Find(pPlayer);
	if (!pBits)
		return false;
	const bool hit = (*pBits & cmd) != 0;
	*pBits &= ~cmd;
	return hit;
}

static SDKEntityMap<int> s_grenadeStatusFlags(ESide::Server, "transloc.gsf");
static SDKEntityMap<bool> s_touchesOwnerTriggers(ESide::Server, "transloc.touchOwner");

static int GrenadeStatusFlags_Read(void* pEnt)
{
	const int off = DTExtend_GetOffset("DT_BaseGrenade", "m_grenadeStatusFlags");
	if (off > 0)
		return *reinterpret_cast<const int*>(
			reinterpret_cast<uintptr_t>(pEnt) + static_cast<uintptr_t>(off));

	const int* const pBits = s_grenadeStatusFlags.Find(pEnt);
	return pBits ? *pBits : 0;
}

static int GrenadeStatusFlags_Write(void* pEnt, int bits)
{
	const int off = DTExtend_GetOffset("DT_BaseGrenade", "m_grenadeStatusFlags");
	if (off > 0)
	{
		*reinterpret_cast<int*>(
			reinterpret_cast<uintptr_t>(pEnt) + static_cast<uintptr_t>(off)) = bits;
		MarkEntityEdictDirty(pEnt);
		return bits;
	}

	static bool s_bNoSlotWarned = false;
	if (!s_bNoSlotWarned)
	{
		s_bNoSlotWarned = true;
		Warning(eDLL_T::SERVER,
			"[TRANSLOC] m_grenadeStatusFlags has no DT slot -- "
			"GSF stays local and the client will not see plant/redirect\n");
	}
	s_grenadeStatusFlags[pEnt] = bits;
	return bits;
}

static void* Translocation_EntityFromStack(HSQUIRRELVM v, SQInteger sqIdx)
{
	const SQObjectPtr& o = stack_get(v, sqIdx);
	if (sq_isnull(o))
		return nullptr;
	if (o._type != OT_ENTITY || !o._unVal.pInstance)
		return nullptr;
	return *reinterpret_cast<void**>(
		reinterpret_cast<uintptr_t>(o._unVal.pInstance) + 0x50);
}

static void* Translocation_ThisEntity(HSQUIRRELVM v)
{
	void* pEnt = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
		return nullptr;
	return pEnt;
}

static void (*v_CBaseEntity_SetAbsOrigin)(CBaseEntity* pEntity, const float* pAbsOrigin) = nullptr;
static bool s_bSetAbsOriginResolved = false;
static void (*v_CBaseEntity_SetAbsVelocity)(void* pEntity, const Vector3D* pVelocity) = nullptr;
static bool s_bSetAbsVelocityResolved = false;

static void Translocation_ResolveSetAbsOrigin(void)
{
	if (s_bSetAbsOriginResolved)
		return;
	s_bSetAbsOriginResolved = true;

	Module_FindPattern(g_GameDll,
		"48 8B C4 55 53 57 48 8D 68 A1 48 81 EC F0 00 00 00 0F 29 70 D8 48 8B D9 0F 29 78 C8")
		.GetPtr(v_CBaseEntity_SetAbsOrigin);

	if (!v_CBaseEntity_SetAbsOrigin)
	{
		Warning(eDLL_T::SERVER,
			"[TRANSLOC] SetAbsOrigin pattern unresolved -- "
			"TeleportPlayerNoInterp cannot move the player\n");
	}
	else
	{
		Msg(eDLL_T::SERVER,
			"[TRANSLOC] SetAbsOrigin resolved at %p\n",
			reinterpret_cast<void*>(v_CBaseEntity_SetAbsOrigin));
	}
}

static void Translocation_ResolveSetAbsVelocity(void)
{
	if (s_bSetAbsVelocityResolved)
		return;
	s_bSetAbsVelocityResolved = true;

	Module_FindPattern(g_GameDll,
		"48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 57 48 81 EC B0 00 00 00 "
		"48 8B 1D ?? ?? ?? ?? 48 8B E9 44 0F 29 40 C8 48 8B F2 0F BF 41 58")
		.GetPtr(v_CBaseEntity_SetAbsVelocity);

	if (!v_CBaseEntity_SetAbsVelocity)
	{
		Warning(eDLL_T::SERVER,
			"[TRANSLOC] SetAbsVelocity pattern unresolved -- "
			"PlantOnGround cannot zero velocity\n");
	}
	else
	{
		Msg(eDLL_T::SERVER,
			"[TRANSLOC] SetAbsVelocity resolved at %p\n",
			reinterpret_cast<void*>(v_CBaseEntity_SetAbsVelocity));
	}
}

class Translocation_EntityFieldAccess : public CBaseEntity
{
public:
	using CBaseEntity::m_fEffects;
};

static void Translocation_AddNoInterp(void* pEnt)
{
	if (!pEnt)
		return;
	auto* const acc = static_cast<Translocation_EntityFieldAccess*>(
		reinterpret_cast<CBaseEntity*>(pEnt));
	acc->m_fEffects |= EF_NOINTERP;

	unsigned char* const pParity = reinterpret_cast<unsigned char*>(
		reinterpret_cast<uintptr_t>(pEnt) + PLAYER_OFF_NOINTERP_PARITY);
	*pParity = static_cast<unsigned char>(*pParity + 1);

	MarkEntityEdictDirty(pEnt);
}

// A teleport leaves the player with no ground entity for a tick. A duck that
// STARTS on such a tick latches m_doingHalfDuck, and when that transition
// completes the movement FSM adds (standHeight - duckHeight) * 0.5 = 16.5 to
// origin.z -- the player ends up a half hull above an exactly authored spawn.
// Settling the state machine before placement leaves nothing in flight to
// complete. duckState is edict-dirtied by the engine, so the client replays
// from the same standing state.
static void Translocation_SettleStance(void* pEnt)
{
	if (!pEnt)
		return;

	const uintptr_t base = reinterpret_cast<uintptr_t>(pEnt);

	*reinterpret_cast<int*>(base + PLAYER_OFF_DUCKSTATE) = kDuckStateStanding;
	*reinterpret_cast<int*>(base + PLAYER_OFF_DUCK_REMAINDER) = 0;
	*reinterpret_cast<unsigned char*>(base + PLAYER_OFF_DOINGHALFDUCK) = 0;
	*reinterpret_cast<unsigned char*>(base + PLAYER_OFF_DUCKTOGGLE) = 0;
	*reinterpret_cast<int*>(base + PLAYER_OFF_FORCESTANCE) = 0;

	int* const pFlags = reinterpret_cast<int*>(base + PLAYER_OFF_FLAGS);
	*pFlags &= ~kFlDucking;

	MarkEntityEdictDirty(pEnt);
}

static void Translocation_UpdateCollisionBounds(void* pEnt)
{
	if (!pEnt)
		return;
	auto* const pBase = reinterpret_cast<CBaseEntity*>(pEnt);
	CCollisionProperty* const pColl = pBase->CollisionProp();
	if (!pColl)
		return;

	const int flags = *reinterpret_cast<const int*>(
		reinterpret_cast<uintptr_t>(pEnt) + PLAYER_OFF_FLAGS);
	const bool bDucked = (flags & kFlDucking) != 0;
	const Vector3D* const pMins = reinterpret_cast<const Vector3D*>(
		reinterpret_cast<uintptr_t>(pEnt)
		+ (bDucked ? PLAYER_OFF_DUCK_HULL_MIN : PLAYER_OFF_STAND_HULL_MIN));
	const Vector3D* const pMaxs = reinterpret_cast<const Vector3D*>(
		reinterpret_cast<uintptr_t>(pEnt)
		+ (bDucked ? PLAYER_OFF_DUCK_HULL_MAX : PLAYER_OFF_STAND_HULL_MAX));
	pColl->SetBounds(*pMins, *pMaxs);
	MarkEntityEdictDirty(pEnt);
}

static SQRESULT Script_SettleStance(HSQUIRRELVM v)
{
	void* const pEnt = Translocation_ThisEntity(v);
	if (!pEnt)
		return SQ_ERROR;

	Translocation_SettleStance(pEnt);
	Translocation_UpdateCollisionBounds(pEnt);
	TriggerPass_SetGroundEntityNull(pEnt);

	static bool s_bLoggedOnce = false;
	if (!s_bLoggedOnce || bridge_translocation_diag.GetBool())
	{
		s_bLoggedOnce = true;
		Msg(eDLL_T::SERVER, "[TRANSLOC] SettleStance ent=%p\n", pEnt);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// SettleStance nulls ground so the old floor is not parented across SetOrigin.
// Call this AFTER SetOrigin on a floor dest so the same-frame snapshot is standing.
static void Translocation_PlantOnGround(void* pEnt)
{
	if (!pEnt)
		return;
	if (!bridge_plant_on_ground.GetBool())
		return;

	const uintptr_t base = reinterpret_cast<uintptr_t>(pEnt);
	void* pWorld = nullptr;
	if (g_ppWorldEntity)
		pWorld = *g_ppWorldEntity;
	if (pWorld)
		TriggerPass_SetGroundEntity(pEnt, pWorld);

	*reinterpret_cast<float*>(base + PLAYER_OFF_FALLVEL) = 0.0f;
	*reinterpret_cast<unsigned char*>(base + PLAYER_OFF_HAS_JUMPED) = 0;

	float* const pPush = reinterpret_cast<float*>(base + PLAYER_OFF_PUSHAWAY);
	pPush[0] = 0.0f;
	pPush[1] = 0.0f;
	pPush[2] = 0.0f;

	Translocation_ResolveSetAbsVelocity();
	if (v_CBaseEntity_SetAbsVelocity)
	{
		const Vector3D vel(0.0f, 0.0f, 0.0f);
		v_CBaseEntity_SetAbsVelocity(pEnt, &vel);
	}

	int* const pFlags = reinterpret_cast<int*>(base + PLAYER_OFF_FLAGS);
	if (pWorld)
		*pFlags |= kFlOnGround;
	else
	{
		static bool s_bNoWorld = false;
		if (!s_bNoWorld)
		{
			s_bNoWorld = true;
			Warning(eDLL_T::SERVER,
				"[TRANSLOC] PlantOnGround has no world entity -- standing flags skipped\n");
		}
	}

	MarkEntityEdictDirty(pEnt);

	if (bridge_translocation_diag.GetBool())
	{
		Msg(eDLL_T::SERVER, "[TRANSLOC] PlantOnGround ent=%p world=%p flags=0x%x\n",
			pEnt, pWorld, *pFlags);
	}
}

static SQRESULT Script_PlantOnGround(HSQUIRRELVM v)
{
	void* const pEnt = Translocation_ThisEntity(v);
	if (!pEnt)
		return SQ_ERROR;

	Translocation_PlantOnGround(pEnt);

	static bool s_bLoggedOnce = false;
	if (!s_bLoggedOnce || bridge_translocation_diag.GetBool())
	{
		s_bLoggedOnce = true;
		Msg(eDLL_T::SERVER, "[TRANSLOC] PlantOnGround ent=%p\n", pEnt);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// projectile.AddGrenadeStatusFlag( int )
//-----------------------------------------------------------------------------
static SQRESULT Script_AddGrenadeStatusFlag(HSQUIRRELVM v)
{
	void* const pEnt = Translocation_ThisEntity(v);
	if (!pEnt)
		return SQ_ERROR;

	SQInteger flag = 0;
	if (SQ_FAILED(sq_getinteger(v, 2, &flag)))
		return SQ_ERROR;

	if (flag <= 0 || flag > 8)
	{
		v_SQVM_ScriptError("Invalid flag value for adding grenade status flags %d\n",
			static_cast<int>(flag));
		return SQ_ERROR;
	}

	const int bits = GrenadeStatusFlags_Write(
		pEnt, GrenadeStatusFlags_Read(pEnt) | static_cast<int>(flag));

	static bool s_bLoggedOnce = false;
	if (!s_bLoggedOnce || bridge_translocation_diag.GetBool())
	{
		s_bLoggedOnce = true;
		Msg(eDLL_T::SERVER, "[TRANSLOC] AddGrenadeStatusFlag ent=%p flag=%d -> 0x%X\n",
			pEnt, static_cast<int>(flag), bits);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// projectile.RemoveGrenadeStatusFlag( int )
//-----------------------------------------------------------------------------
static SQRESULT Script_RemoveGrenadeStatusFlag(HSQUIRRELVM v)
{
	void* const pEnt = Translocation_ThisEntity(v);
	if (!pEnt)
		return SQ_ERROR;

	SQInteger flag = 0;
	if (SQ_FAILED(sq_getinteger(v, 2, &flag)))
		return SQ_ERROR;

	GrenadeStatusFlags_Write(
		pEnt, GrenadeStatusFlags_Read(pEnt) & ~static_cast<int>(flag));

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// projectile.IsGrenadeStatusFlagSet( int ) -> bool
//-----------------------------------------------------------------------------
static SQRESULT Script_IsGrenadeStatusFlagSet(HSQUIRRELVM v)
{
	void* const pEnt = Translocation_ThisEntity(v);
	if (!pEnt)
		return SQ_ERROR;

	SQInteger flag = 0;
	if (SQ_FAILED(sq_getinteger(v, 2, &flag)))
		return SQ_ERROR;

	const int bits = GrenadeStatusFlags_Read(pEnt);
	sq_pushbool(v, (bits & static_cast<int>(flag)) != 0 ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// projectile.SetProjectileTouchesOwnerTriggers( bool )
// S3 has no engine twin. Sidecar so the call compiles and is observable;
// SetTouchTriggers (already on the entity class) is the live trigger bit.
//-----------------------------------------------------------------------------
static SQRESULT Script_SetProjectileTouchesOwnerTriggers(HSQUIRRELVM v)
{
	void* const pEnt = Translocation_ThisEntity(v);
	if (!pEnt)
		return SQ_ERROR;

	SQBool bTouch = SQFalse;
	if (SQ_FAILED(sq_getbool(v, 2, &bTouch)))
		return SQ_ERROR;

	s_touchesOwnerTriggers[pEnt] = (bTouch != SQFalse);
	if (bTouch != SQFalse)
		Translocation_OnProjectileSpawned(pEnt);

	static bool s_bLoggedOnce = false;
	if (!s_bLoggedOnce || bridge_translocation_diag.GetBool())
	{
		s_bLoggedOnce = true;
		Msg(eDLL_T::SERVER,
			"[TRANSLOC] SetProjectileTouchesOwnerTriggers ent=%p touch=%d\n",
			pEnt, bTouch ? 1 : 0);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// projectile.TriggerAndTouchOwnerTouchedTriggers()
// Server-only; the script already called owner.SetTouchTriggers(true) on the
// previous line.
//-----------------------------------------------------------------------------
static SQRESULT Script_TriggerAndTouchOwnerTouchedTriggers(HSQUIRRELVM v)
{
	void* const pEnt = Translocation_ThisEntity(v);
	if (!pEnt)
		return SQ_ERROR;

	static bool s_bLoggedOnce = false;
	if (!s_bLoggedOnce || bridge_translocation_diag.GetBool())
	{
		s_bLoggedOnce = true;
		Msg(eDLL_T::SERVER,
			"[TRANSLOC] TriggerAndTouchOwnerTouchedTriggers ent=%p\n", pEnt);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// player.IsInputCommandPressed( int ) / IsInputCommandHeld( int )
// S3 binds these on the CLIENT player class only.
//-----------------------------------------------------------------------------
static SQRESULT Script_IsInputCommandPressed(HSQUIRRELVM v)
{
	void* const pEnt = Translocation_ThisEntity(v);
	if (!pEnt)
		return SQ_ERROR;

	SQInteger cmd = 0;
	if (SQ_FAILED(sq_getinteger(v, 2, &cmd)))
		return SQ_ERROR;

	const int held = *reinterpret_cast<const int*>(
		reinterpret_cast<uintptr_t>(pEnt) + PLAYER_OFF_BUTTONS);
	const int pressed = *reinterpret_cast<const int*>(
		reinterpret_cast<uintptr_t>(pEnt) + PLAYER_OFF_BUTTONPRESSED);
	const bool sticky = Translocation_ConsumeSticky(pEnt, static_cast<int>(cmd));
	const bool hit = ((held & static_cast<int>(cmd)) != 0)
		|| ((pressed & static_cast<int>(cmd)) != 0)
		|| sticky;
	if (sticky && bridge_translocation_diag.GetBool())
	{
		Msg(eDLL_T::SERVER, "[TRANSLOC] drop-click sticky cmd=0x%X\n",
			static_cast<int>(cmd));
	}
	sq_pushbool(v, hit ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_IsInputCommandHeld(HSQUIRRELVM v)
{
	void* const pEnt = Translocation_ThisEntity(v);
	if (!pEnt)
		return SQ_ERROR;

	SQInteger cmd = 0;
	if (SQ_FAILED(sq_getinteger(v, 2, &cmd)))
		return SQ_ERROR;

	const int held = *reinterpret_cast<const int*>(
		reinterpret_cast<uintptr_t>(pEnt) + PLAYER_OFF_BUTTONS);
	sq_pushbool(v, (held & static_cast<int>(cmd)) != 0 ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// TeleportPlayerNoInterp( entity player, vector pos, bool keepStance = 0 ) -> bool
// S21 free function. Returns false if the dest equals the current origin.
// DuckImmediate is the S21 contract; keepStance skips it.
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_TeleportPlayerNoInterp(HSQUIRRELVM v)
{
	Translocation_ResolveSetAbsOrigin();

	void* const pEnt = Translocation_EntityFromStack(v, 2);
	if (!pEnt)
	{
		static bool s_bNullWarned = false;
		if (!s_bNullWarned)
		{
			s_bNullWarned = true;
			Warning(eDLL_T::SERVER,
				"[TRANSLOC] TeleportPlayerNoInterp requires a player entity\n");
		}
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const SQVector3D* pPos = nullptr;
	if (SQ_FAILED(sq_getvector(v, 3, &pPos)) || !pPos)
	{
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	// OT_BOOL carries no SQOBJECT_NUMERIC bit, so sq_getinteger rejects a literal
	// true and the duck fires anyway. Read the bool first, number second.
	bool keepStance = false;
	if (sq_gettop(v) >= 4)
	{
		SQBool bKeep = SQFalse;
		SQInteger iKeep = 0;
		if (SQ_SUCCEEDED(sq_getbool(v, 4, &bKeep)))
			keepStance = bKeep != SQFalse;
		else if (SQ_SUCCEEDED(sq_getinteger(v, 4, &iKeep)))
			keepStance = iKeep != 0;
	}

	const float* const pCur = reinterpret_cast<const float*>(
		reinterpret_cast<uintptr_t>(pEnt) + kEntOffAbsOrigin);
	const float dx = fabsf(pPos->x - pCur[0]);
	const float dy = fabsf(pPos->y - pCur[1]);
	const float dz = fabsf(pPos->z - pCur[2]);
	if (fmaxf(fmaxf(dx, dy), dz) == 0.0f)
	{
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	if (!v_CBaseEntity_SetAbsOrigin)
	{
		static bool s_bStubWarned = false;
		if (!s_bStubWarned)
		{
			s_bStubWarned = true;
			Warning(eDLL_T::SERVER,
				"[TRANSLOC] TeleportPlayerNoInterp stub -- SetAbsOrigin unresolved\n");
		}
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const int nSteps = bridge_teleport_steps.GetInt();

	if (nSteps & kTeleportStepNoInterp)
		Translocation_AddNoInterp(pEnt);
	if (nSteps & kTeleportStepSettleStance)
		Translocation_SettleStance(pEnt);
	if ((nSteps & kTeleportStepDuck) && !keepStance && v_CPlayer_DuckImmediate)
		v_CPlayer_DuckImmediate(pEnt);
	if (nSteps & kTeleportStepBounds)
		Translocation_UpdateCollisionBounds(pEnt);
	if ((nSteps & kTeleportStepMoveType) && v_CBaseEntity_SetMoveType)
		v_CBaseEntity_SetMoveType(pEnt, kMoveTypeWalk, 0);
	const float dest[3] = { pPos->x, pPos->y, pPos->z };
	v_CBaseEntity_SetAbsOrigin(reinterpret_cast<CBaseEntity*>(pEnt), dest);
	MarkEntityEdictDirty(pEnt);

	if (bridge_translocation_diag.GetBool())
	{
		Msg(eDLL_T::SERVER,
			"[TRANSLOC] TeleportPlayerNoInterp ent=%p -> %.1f %.1f %.1f\n",
			pEnt, dest[0], dest[1], dest[2]);
	}

	sq_pushbool(v, SQTrue);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void Translocation_RegisterProjectileFuncs(ScriptClassDescriptor_t* projectileStruct)
{
	if (!projectileStruct)
		return;

	projectileStruct->AddFunction(
		"AddGrenadeStatusFlag",
		"Script_AddGrenadeStatusFlag",
		"Sets a grenade status flag bit (GSF_PLANTED=1, GSF_REDIRECTED=2)",
		"void",
		"int flag",
		false,
		Script_AddGrenadeStatusFlag);
	projectileStruct->AddFunction(
		"RemoveGrenadeStatusFlag",
		"Script_RemoveGrenadeStatusFlag",
		"Clears a grenade status flag bit",
		"void",
		"int flag",
		false,
		Script_RemoveGrenadeStatusFlag);
	projectileStruct->AddFunction(
		"IsGrenadeStatusFlagSet",
		"Script_IsGrenadeStatusFlagSet",
		"Returns whether a grenade status flag bit is set",
		"bool",
		"int flag",
		false,
		Script_IsGrenadeStatusFlagSet);
	projectileStruct->AddFunction(
		"SetProjectileTouchesOwnerTriggers",
		"Script_SetProjectileTouchesOwnerTriggers",
		"Allows this projectile to touch triggers owned by its thrower",
		"void",
		"bool touch",
		false,
		Script_SetProjectileTouchesOwnerTriggers);
	projectileStruct->AddFunction(
		"TriggerAndTouchOwnerTouchedTriggers",
		"Script_TriggerAndTouchOwnerTouchedTriggers",
		"Replays trigger touches the projectile accumulated onto the owner",
		"void",
		"",
		false,
		Script_TriggerAndTouchOwnerTouchedTriggers);

	Msg(eDLL_T::SERVER, "[TRANSLOC] entity/projectile natives registered\n");
}

void Translocation_RegisterPlayerFuncs(ScriptClassDescriptor_t* playerStruct)
{
	if (!playerStruct)
		return;

	playerStruct->AddFunction(
		"IsInputCommandPressed",
		"Script_IsInputCommandPressed",
		"Returns true if the input command was pressed this command",
		"bool",
		"int command",
		false,
		Script_IsInputCommandPressed);
	playerStruct->AddFunction(
		"IsInputCommandHeld",
		"Script_IsInputCommandHeld",
		"Returns true if the input command is currently held",
		"bool",
		"int command",
		false,
		Script_IsInputCommandHeld);
	playerStruct->AddFunction(
		"SettleStance",
		"Script_SettleStance",
		"Puts the player in a settled standing stance without moving them",
		"void",
		"",
		false,
		Script_SettleStance);
	playerStruct->AddFunction(
		"PlantOnGround",
		"Script_PlantOnGround",
		"Standing plant at the current origin after a floor teleport",
		"void",
		"",
		false,
		Script_PlantOnGround);

	Msg(eDLL_T::SERVER, "[TRANSLOC] player input natives registered\n");
}

void Translocation_RegisterFreeFuncs(CSquirrelVM* s)
{
	if (!s)
		return;

	s->RegisterConstant("GSF_PLANTED", GSF_PLANTED_S21);
	s->RegisterConstant("GSF_REDIRECTED", GSF_REDIRECTED_S21);

	Script_RegisterFuncNamed(s, "TeleportPlayerNoInterp",
		"Script_TeleportPlayerNoInterp",
		"Teleports a player to pos without interpolating the move",
		"bool",
		"entity player, vector pos, bool keepStance = 0",
		false,
		ServerScript_TeleportPlayerNoInterp);

	Msg(eDLL_T::SERVER, "[TRANSLOC] free natives + GSF consts registered\n");
}

//-----------------------------------------------------------------------------
// weapon.SetTranslocationFlightHold( bool )
// Keeps the tac deployed after toss so the S21 client can play toss-hold /
// one-hand. S3 holsters every toss weapon the same frame as release.
//-----------------------------------------------------------------------------
static SQRESULT Script_SetTranslocationFlightHold(HSQUIRRELVM v)
{
	void* const pEnt = Translocation_ThisEntity(v);
	if (!pEnt)
		return SQ_ERROR;

	SQBool bHold = SQFalse;
	if (SQ_FAILED(sq_getbool(v, 2, &bHold)))
		return SQ_ERROR;

	Translocation_SetFlightHold(pEnt, bHold != SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void Translocation_RegisterWeaponFuncs(ScriptClassDescriptor_t* weaponStruct)
{
	if (!weaponStruct)
		return;

	weaponStruct->AddFunction(
		"SetTranslocationFlightHold",
		"Script_SetTranslocationFlightHold",
		"Keeps this toss weapon deployed while its projectile is in flight",
		"void",
		"bool hold",
		false,
		Script_SetTranslocationFlightHold);

	Msg(eDLL_T::SERVER, "[TRANSLOC] weapon flight-hold native registered\n");
}

static void Hook_PlayerRunCommand(CPlayer* pPlayer, CUserCmd* pUserCmd, IMoveHelper* pMover)
{
	Translocation_FlushDeadToss();
	JetDrive_TickHolds(pPlayer);
	if (pPlayer && pUserCmd && Translocation_ShouldEatAttack(pPlayer))
	{
		Translocation_LatchCmdBits(pPlayer, pUserCmd->buttons & kDropClickBits);
		pUserCmd->buttons &= ~kDropClickBits;
	}
	CPlayer__PlayerRunCommand(pPlayer, pUserCmd, pMover);
	if (pPlayer)
	{
		if (Translocation_ShouldEatAttack(pPlayer))
		{
			int* const pHeld = reinterpret_cast<int*>(
				reinterpret_cast<uintptr_t>(pPlayer) + PLAYER_OFF_BUTTONS);
			int* const pPressed = reinterpret_cast<int*>(
				reinterpret_cast<uintptr_t>(pPlayer) + PLAYER_OFF_BUTTONPRESSED);
			*pHeld &= ~kDropClickBits;
			*pPressed &= ~kDropClickBits;
		}
		Translocation_OnPlayerRunCommand(pPlayer);
		const int cmdNumber = pUserCmd ? pUserCmd->command_number : 0;
		ServerScript_UpdateHeldObjectPlacement(pPlayer, cmdNumber);
	}
}

static SQRESULT Hook_IsInputCommandPressed(HSQUIRRELVM v)
{
	return Script_IsInputCommandPressed(v);
}

static char Hook_HolsterInternal(void* pWeapon, bool bDoFastHolster)
{
	const unsigned int weapState = Translocation_WeaponState(pWeapon);
	const char* const pszName = pWeapon
		? reinterpret_cast<const char*>(
			reinterpret_cast<uintptr_t>(pWeapon) + WEAPON_CLASSNAME_OFFSET)
		: "?";
	static int s_nHolsterLog = 16;
	static int s_nLaunchHolsterLog = 32;
	const bool bLaunchName = pszName && strstr(pszName, "companion_launch") != nullptr;
	if (s_nHolsterLog > 0 || (bLaunchName && s_nLaunchHolsterLog > 0))
	{
		if (bLaunchName && s_nLaunchHolsterLog > 0)
			--s_nLaunchHolsterLog;
		if (s_nHolsterLog > 0)
			--s_nHolsterLog;
		Msg(eDLL_T::SERVER,
			"[TRANSLOC] HolsterInternal '%s' state=%u fast=%d flag=%d\n",
			pszName && pszName[0] ? pszName : "?", weapState,
			bDoFastHolster ? 1 : 0,
			Translocation_WeaponHasTossPostLoop(pWeapon) ? 1 : 0);
	}

	const bool bJdHold = JetDrive_ShouldHoldOffhand(pWeapon)
		|| JetDrive_NoteOffhandHolster(pWeapon);
	if (Translocation_ShouldHoldOffhand(pWeapon) || bJdHold)
	{
		Msg(eDLL_T::SERVER, "[TRANSLOC] skipped holster weapon=%p (hold)\n", pWeapon);
		return 0;
	}

	const bool bDead = pWeapon && !Translocation_OwnerAlive(pWeapon);
	if (pWeapon && Translocation_WeaponHasTossPostLoop(pWeapon) && !bDead)
	{
		const bool bInLoop = s_tossLoopWeapons.Find(pWeapon) != nullptr;
		const bool bNoProj = s_tossLoopNoProj.Find(pWeapon) != nullptr;
		const bool bLive = Translocation_HasLiveProjectile(pWeapon);
		if (bInLoop && !bLive && !bNoProj)
			Translocation_EndTossLoop(pWeapon, false);
		else if (bLive || bInLoop || bNoProj || Translocation_IsTossFamily(weapState))
		{
			return 0;
		}
	}

	if (pWeapon && s_tossLoopWeapons.Find(pWeapon))
		Translocation_EndTossLoop(pWeapon, false);

	// S3 skips the holster VM on bit 2; S21 skips it on 0x80.
	if (pWeapon && weapState == WEAP_STATE_CUSTOM_ACTIVITY)
	{
		uint8_t* const pFlags = reinterpret_cast<uint8_t*>(
			reinterpret_cast<uintptr_t>(pWeapon) + WEAPON_OFF_CUSTOM_ACT_FLAGS);
		if ((*pFlags & WCAF_PLAYRAISE_S21)
			&& !(*pFlags & WCAF_PLAYRAISE_S3))
		{
			*pFlags |= WCAF_PLAYRAISE_S3;
		}
	}

	const char result = v_WeaponX_HolsterInternal(pWeapon, bDoFastHolster);
	Translocation_ClearOneHand(pWeapon);
	return result;
}

void Translocation_HolsterWeaponOriginal(void* pWeapon)
{
	if (!pWeapon || !v_WeaponX_HolsterInternal)
		return;
	v_WeaponX_HolsterInternal(pWeapon, true);
}

static void Translocation_FinishPlayRaise(void* pWeapon)
{
	if (!pWeapon)
		return;

	Msg(eDLL_T::SERVER, "[TRANSLOC] playraise complete -> holster weapon=%p\n", pWeapon);

	if (v_WeaponX_HolsterInternal)
	{
		uint8_t* const pFlags = reinterpret_cast<uint8_t*>(
			reinterpret_cast<uintptr_t>(pWeapon) + WEAPON_OFF_CUSTOM_ACT_FLAGS);
		if ((*pFlags & WCAF_PLAYRAISE_S21)
			&& !(*pFlags & WCAF_PLAYRAISE_S3))
		{
			*pFlags |= WCAF_PLAYRAISE_S3;
		}
		v_WeaponX_HolsterInternal(pWeapon, true);
	}

	void* const pOwner = Translocation_WeaponOwner(pWeapon);
	if (pOwner && v_Weapon_SetSelectedOffhandCleared)
	{
		const unsigned int slot = *reinterpret_cast<const unsigned int*>(
			reinterpret_cast<uintptr_t>(pWeapon) + WEAPON_OFF_ACTIVE_SLOT);
		if (slot < 3)
			v_Weapon_SetSelectedOffhandCleared(pOwner, slot);
		else
		{
			v_Weapon_SetSelectedOffhandCleared(pOwner, 0);
			v_Weapon_SetSelectedOffhandCleared(pOwner, 1);
		}
	}

	Translocation_ClearOneHand(pWeapon);
}

static __int64 Hook_SetWeaponState(void* pWeapon, unsigned int state)
{
	// S3 ends custom activity on bit 2 -> IDLE. S21 ends 0x80 -> holster.
	if (pWeapon && state == WEAP_STATE_IDLE
		&& Translocation_WeaponState(pWeapon) == WEAP_STATE_CUSTOM_ACTIVITY)
	{
		const uint8_t flags = *reinterpret_cast<const uint8_t*>(
			reinterpret_cast<uintptr_t>(pWeapon) + WEAPON_OFF_CUSTOM_ACT_FLAGS);
		if (flags & WCAF_PLAYRAISE_S21)
		{
			Translocation_FinishPlayRaise(pWeapon);
			return 1;
		}
	}

	if (JetDrive_FilterWeaponState(pWeapon, state))
		return 1;

	if (pWeapon && Translocation_WeaponHasTossPostLoop(pWeapon)
		&& Translocation_OwnerAlive(pWeapon))
	{
		const bool bInLoop = s_tossLoopWeapons.Find(pWeapon) != nullptr;
		if (state == WEAP_STATE_TOSS)
		{
			if (void* const pOwner = Translocation_WeaponOwner(pWeapon))
				s_tossWeaponByOwner[pOwner] = SDKEntityState_GetHandle(pWeapon);
		}
		else if (state == 0 || state == WEAP_STATE_HOLSTERED)
		{
			if (bInLoop && Translocation_HasLiveProjectile(pWeapon))
			{
				s_tossLoopPrevState[pWeapon] = state;
				state = WEAP_STATE_POST_TOSS_LOOP;
			}
			else if (void* const pOwner = Translocation_WeaponOwner(pWeapon);
				pOwner && PlayerLaunch_KeepsToss(pOwner))
			{
				if (bInLoop)
					s_tossLoopPrevState[pWeapon] = state;
				else
					Translocation_BeginTossLoop(pWeapon, state);
				s_tossLoopNoProj[pWeapon] = 1;
				state = WEAP_STATE_POST_TOSS_LOOP;
			}
		}
	}
	return v_WeaponX_SetWeaponState(pWeapon, state);
}

static char Hook_SetIdealWeaponActivity(void* pWeapon, unsigned int activity)
{
	if (JetDrive_FilterIdealActivity(pWeapon, activity))
		return 0;

	if (pWeapon && s_tossLoopWeapons.Find(pWeapon))
	{
		if (Translocation_IsEndTossActivity(activity))
			Translocation_EndTossLoop(pWeapon, false);
		else if (activity == ACT_VM_IDLE_S3)
		{
			const int actId = Translocation_PostTossActivity();
			if (actId > 0)
				activity = static_cast<unsigned int>(actId);
		}
	}
	return v_SetIdealWeaponActivity(pWeapon, activity);
}

static char Hook_StartCustomActivity(void* pWeapon, unsigned int activity, unsigned char flags)
{
	// S3 RegisterEnum: PLAYRAISEONCOMPLETE=2. S21 is 0x80. The byte is
	// networked; 2 makes the client SLOBYTE>=0 path IDLE/deploy the tac.
	if (flags & WCAF_PLAYRAISE_S3)
	{
		flags = static_cast<unsigned char>((flags & ~WCAF_PLAYRAISE_S3) | WCAF_PLAYRAISE_S21);
		Msg(eDLL_T::SERVER,
			"[TRANSLOC] playraise flags 2 -> 0x80 act=%u weapon=%p\n",
			activity, pWeapon);
	}

	if (pWeapon && s_tossLoopWeapons.Find(pWeapon)
		&& Translocation_IsEndTossActivity(activity))
	{
		Translocation_EndTossLoop(pWeapon, false);
		Msg(eDLL_T::SERVER, "[TRANSLOC] custom-act ended toss loop act=%u\n", activity);
	}
	const char result = v_WeaponX_StartCustomActivity(pWeapon, activity, flags);
	MeleeActivityTrace_OnStart(pWeapon, activity, flags, result, _ReturnAddress());
	return result;
}

void Translocation_LevelShutdown(void)
{
	s_cmdButtonSticky.Clear();
	s_flightHoldWeapons.Clear();
	s_tossOneHandPlayers.Clear();
	s_tossLoopWeapons.Clear();
	s_tossLoopNoProj.Clear();
	s_tossLoopPrevState.Clear();
	s_tossWeaponByOwner.Clear();
	s_tossProjHandle.Clear();
	s_tossLoopBeginTime.Clear();
	s_grenadeStatusFlags.Clear();
	s_touchesOwnerTriggers.Clear();
}

void VTranslocation::GetAdr(void) const
{
	LogFunAdr("CPlayer::PlayerRunCommand", CPlayer__PlayerRunCommand);
	LogFunAdr("CPlayer::IsInputCommandPressed", v_IsInputCommandPressed);
	LogFunAdr("CWeaponX::HolsterInternal", v_WeaponX_HolsterInternal);
	LogFunAdr("CWeaponX::SetWeaponState", v_WeaponX_SetWeaponState);
	LogFunAdr("CWeaponX::SetIdealWeaponActivity", v_SetIdealWeaponActivity);
	LogFunAdr("CWeaponX::StartCustomActivity", v_WeaponX_StartCustomActivity);
	LogFunAdr("CPlayer::DuckImmediate", v_CPlayer_DuckImmediate);
	LogFunAdr("CBaseEntity::SetMoveType", v_CBaseEntity_SetMoveType);
	LogFunAdr("CPlayer::SetOneHandedWeaponUsageOn", v_CPlayer_SetOneHandedOn);
	LogVarAdr("TossCompleteHolsterRet", s_pTossCompleteHolsterRet);
}

void VTranslocation::GetFun(void) const
{
	// Interior of the SERVER IsInputCommandPressed binding: test [rax+0x60DC], ecx.
	// Unique (1 hit). 0x33 bytes back to the function start.
	Module_FindPattern(g_GameDll,
		"BA 00 00 00 00 85 88 DC 60 00 00 48 8B CB 0F 95 C2")
		.Offset(-0x33)
		.GetPtr(v_IsInputCommandPressed);

	if (!v_IsInputCommandPressed)
		Warning(eDLL_T::SERVER,
			"[TRANSLOC] IsInputCommandPressed pattern unresolved -- "
			"drop-click latch cannot wrap the engine native\n");

	// End-of-toss holster in CWeaponX toss-complete (1 hit). Return address
	// is the byte after the near call (pattern start + 0x1A).
	const CMemory tossHolster = Module_FindPattern(g_GameDll,
		"48 8B 06 48 8B CE FF 90 E8 02 00 00 84 C0 74 0A B2 01 48 8B CF E8");
	if (tossHolster)
		s_pTossCompleteHolsterRet = tossHolster.Offset(0x1A).RCast<const void*>();
	else
		Warning(eDLL_T::SERVER,
			"[TRANSLOC] end-of-toss holster callsite unresolved -- "
			"toss_has_post_loop cannot keep the weapon out\n");

	// Writes weapon+0x1234. Unique (1 hit). S3 POST_TOSS_LOOP is 19.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 55 48 83 EC ?? 8B 81 ?? ?? ?? ?? 8B EA 48 8B D9")
		.GetPtr(v_WeaponX_SetWeaponState);
	if (!v_WeaponX_SetWeaponState)
		Warning(eDLL_T::SERVER,
			"[TRANSLOC] SetWeaponState unresolved -- will write weapState 19 directly\n");

	// SetIdealWeaponActivity: studiohdr at +0xFD8. Unique (1 hit).
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 7C 24 10 55 48 8B EC 48 83 EC 70 "
		"48 83 B9 D8 0F 00 00 00")
		.GetPtr(v_SetIdealWeaponActivity);
	if (!v_SetIdealWeaponActivity)
		Warning(eDLL_T::SERVER,
			"[TRANSLOC] SetIdealWeaponActivity unresolved -- POST_TOSS_LOOP cannot play\n");

	// SERVER StartCustomActivity(weapon, activity, flags). Unique (1 hit).
	Module_FindPattern(g_GameDll,
		"40 53 55 57 41 56 48 83 EC 48")
		.GetPtr(v_WeaponX_StartCustomActivity);
	if (!v_WeaponX_StartCustomActivity)
		Warning(eDLL_T::SERVER,
			"[TRANSLOC] StartCustomActivity unresolved -- PICKUP/MISS cannot end toss loop\n");

	// SERVER DuckImmediate: flags+0x234, duckState+0x65F0. Unique (1 hit).
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 8B 81 ?? ?? ?? ?? 48 8B D9 8B F8")
		.GetPtr(v_CPlayer_DuckImmediate);
	if (!v_CPlayer_DuckImmediate)
		Warning(eDLL_T::SERVER,
			"[TRANSLOC] DuckImmediate unresolved -- TeleportPlayerNoInterp skips duck\n");

	// SERVER SetMoveType. Unique (1 hit). WALK=2, collide=0 is ClearTraverse.
	Module_FindPattern(g_GameDll,
		"40 55 56 57 48 83 EC ?? 0F B6 81")
		.GetPtr(v_CBaseEntity_SetMoveType);
	if (!v_CBaseEntity_SetMoveType)
		Warning(eDLL_T::SERVER,
			"[TRANSLOC] SetMoveType unresolved -- TeleportPlayerNoInterp skips ClearTraverse\n");

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 20 80 B9 90 5C 00 00 01 48 8B D9 74 1A")
		.GetPtr(v_CPlayer_SetOneHandedOn);
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 20 80 B9 90 5C 00 00 00 48 8B D9 74 1A")
		.GetPtr(v_CPlayer_SetOneHandedOff);
	if (!v_CPlayer_SetOneHandedOn)
		Warning(eDLL_T::SERVER,
			"[TRANSLOC] SetOneHandedWeaponUsageOn unresolved -- gun stays two-hand in flight\n");
}

void VTranslocation::Detour(const bool bAttach) const
{
	if (CPlayer__PlayerRunCommand)
		DetourSetup(&CPlayer__PlayerRunCommand, &Hook_PlayerRunCommand, bAttach);
	else
		Warning(eDLL_T::SERVER,
			"[TRANSLOC] PlayerRunCommand unresolved -- drop-click latch is dead\n");

	if (v_IsInputCommandPressed)
		DetourSetup(&v_IsInputCommandPressed, &Hook_IsInputCommandPressed, bAttach);

	if (v_WeaponX_HolsterInternal)
		DetourSetup(&v_WeaponX_HolsterInternal, &Hook_HolsterInternal, bAttach);
	else
		Warning(eDLL_T::SERVER,
			"[TRANSLOC] HolsterInternal unresolved -- toss weapon will holster\n");

	if (v_WeaponX_SetWeaponState)
		DetourSetup(&v_WeaponX_SetWeaponState, &Hook_SetWeaponState, bAttach);

	if (v_SetIdealWeaponActivity)
		DetourSetup(&v_SetIdealWeaponActivity, &Hook_SetIdealWeaponActivity, bAttach);

	if (v_WeaponX_StartCustomActivity)
		DetourSetup(&v_WeaponX_StartCustomActivity, &Hook_StartCustomActivity, bAttach);
}
