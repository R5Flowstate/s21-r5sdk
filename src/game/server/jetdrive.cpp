//=============================================================================//
//
// Purpose: JetDrive server-side port. See jetdrive.h.
//
//=============================================================================//
#include "core/stdafx.h"


#include "jetdrive.h"
#include "player_launch.h"
#include "bridge_cmd_chain.h"
#include "translocation.h"
#include "player.h"
#include "baseentity.h"
#include "entitylist.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/dt_extend.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "game/shared/in_buttons.h"
#include "mathlib/mathlib.h"
#include "public/edict.h"
#include "vscript_server.h"
#include "trigger_cannon.h"
#include "halfduck_zip_parity.h"
#include "trigger_slip_diag.h"
#include "move_sim_trace.h"
#include <cstring>
#include <unordered_map>
#include <unordered_set>

// gpGlobals is defined in the game module; declare locally -- same pattern
// snapshot_diag.cpp / vscript_player.cpp use.
extern CGlobalVars* gpGlobals;
extern int64_t Server_PrecacheModel_Invoke(const char* modelName);

//-----------------------------------------------------------------------------
// Networked DPT_Time: gpGlobals->curtime, not Plat_FloatTime.
//-----------------------------------------------------------------------------
static inline float JetDrive_CurTime(void)
{
	return gpGlobals ? gpGlobals->curTime : static_cast<float>(Plat_FloatTime());
}

//-----------------------------------------------------------------------------
// Per-player state. SDKEntityMap, not a native struct field -- see jetdrive.h.
//-----------------------------------------------------------------------------
static SDKEntityMap<JetDriveState> s_jetDriveMapServer(ESide::Server, "jetDrive.srv");
static SDKEntityMap<float> s_jdAttackTime(ESide::Server, "jetDrive.atk");
// Handle-keyed like every other shadow map here: raw CPlayer* keys dangle
// across disconnect/changelevel and alias onto recycled player slots.
static SDKEntityMap<uint8_t> s_jdHoldPlayers(ESide::Server, "jetDrive.hold");
static SDKEntityMap<float>   s_jdPendingTime(ESide::Server, "jetDrive.pending");
static constexpr float kJdHoldOrphanSeconds = 0.5f;
static constexpr float kJdAttackLockSeconds = 1.5f;
static constexpr ptrdiff_t kJdWeaponWeapState = 0x1234;
static constexpr ptrdiff_t kJdWeaponNextReady = 4600; // m_nextReadyTime
static constexpr ptrdiff_t kJdWeaponNextPrimary = 4604; // m_nextPrimaryAttackTime
static constexpr ptrdiff_t kJdWeaponTimeIdle = 4656; // m_flTimeWeaponIdle
static constexpr unsigned int kJdWeapStateAttack = 9;
static constexpr ptrdiff_t kJdEntOffModelIndex = 0xDE;
static constexpr ptrdiff_t kJdWeaponOffWorldModelIndex = 0x1208;
static constexpr ptrdiff_t kJdWeaponOffCStudio = 0xFD8;
static const char* const kJdWhistlePtpov =
	"mdl/weapons/vantage_tactical_whistle/ptpov_vantage_tactical_whistle.rmdl";

//-----------------------------------------------------------------------------
// Tunables. FCVAR_REPLICATED so the client's native JetDriveAccel sees the same values.
//-----------------------------------------------------------------------------
static ConVar jetdrive_decel_dist("jetdrive_decel_dist", "180.0", FCVAR_RELEASE | FCVAR_REPLICATED,
	"JetDrive: radius from target inside which the decel/double-jump window begins.");
static ConVar jetdrive_decel_doublejump_time_window("jetdrive_decel_doublejump_time_window", "0.75", FCVAR_RELEASE | FCVAR_REPLICATED,
	"JetDrive: duration of the decel/double-jump window once entered.");
static ConVar jetdrive_decel_final_vel_frac("jetdrive_decel_final_vel_frac", "0.1", FCVAR_RELEASE | FCVAR_REPLICATED,
	"JetDrive: floor fraction the in-window velocity scale never drops below.");
static ConVar jetdrive_duck_cancel_min_time("jetdrive_duck_cancel_min_time", "0.2", FCVAR_RELEASE | FCVAR_REPLICATED,
	"JetDrive: minimum elapsed time before a duck press is honored as a cancel.");
static ConVar jetdrive_duck_cancel_vel_frac("jetdrive_duck_cancel_vel_frac", "0.8", FCVAR_RELEASE | FCVAR_REPLICATED,
	"JetDrive: uniform velocity scalar applied on every terminal exit.");
static ConVar jetdrive_initial_speed_mult("jetdrive_initial_speed_mult", "1.2", FCVAR_RELEASE | FCVAR_REPLICATED,
	"JetDrive: launch-burst speed multiplier at t=1, decaying to 1.0.");
static ConVar jetdrive_initial_vert_offset("jetdrive_initial_vert_offset", "0.4", FCVAR_RELEASE | FCVAR_REPLICATED,
	"JetDrive: multiplier on horizontal distance giving peak arc height.");
static ConVar jetdrive_stuck_speed("jetdrive_stuck_speed", "100.0", FCVAR_RELEASE | FCVAR_REPLICATED,
	"JetDrive: alignment-speed threshold below which the stuck-kick check engages.");
static ConVar jetdrive_anim_linger_time("jetdrive_anim_linger_time", "1.0", FCVAR_RELEASE | FCVAR_REPLICATED,
	"JetDrive: added to curtime to set the anim-linger deadline on drive end.");
static ConVar jetdrive_delay_time("jetdrive_delay_time", "0.2", FCVAR_RELEASE | FCVAR_REPLICATED,
	"JetDrive: wind-up delay before BeginJetDrive's drive actually starts moving the player.");
static ConVar jetdrive_weapon_clock_hold("jetdrive_weapon_clock_hold", "0.75",
	FCVAR_RELEASE,
	"Seconds ahead of server time the JetDrive weapon fire/idle clocks are held "
	"while the drive is live. 0 = leave the native stamps alone.");
static ConVar sdk_jetdrive_anim_diag("sdk_jetdrive_anim_diag", "0",
	FCVAR_DEVELOPMENTONLY,
	"Log JetDrive weapon clock arm/refresh/release.");

// Duck-cancel: IN_DUCK (0x4) plus 0x4000000. No named constant for the high bit.
static constexpr int JETDRIVE_DUCKCANCEL_BUTTON_MASK = 0x4000004;
// S21 JetDriveAccel tests CMoveData+0x2C & IN_JUMP (0x2).
static constexpr int JETDRIVE_DOUBLEJUMP_BUTTON_MASK = IN_JUMP;

static constexpr ptrdiff_t JD_CTX_OFF_PLAYER   = 8;
static constexpr ptrdiff_t JD_CTX_OFF_MOVEDATA = 16;
static constexpr ptrdiff_t JD_MV_OFF_BUTTONS_PRESSED = 44; // CMoveData+0x2C
static constexpr ptrdiff_t JD_MV_OFF_VELOCITY = 304;       // S3 m_vecVelocity
static constexpr ptrdiff_t JD_PLAYER_OFF_FLOORHEIGHT = 23968;
static constexpr unsigned int PLAYERANIMEVENT_DOUBLEJUMP = 7;
static constexpr uintptr_t JD_WEAPON_TYPE_FLAGS = 0x19B0; // CWeaponX server
static constexpr uint32_t JD_WPT_TACTICAL = 0x004u;
static constexpr uint32_t JD_WPT_VIEWHANDS = 0x100u;

static __int64 (*v_CGameMovement__FullWalkMove)(void* ctx) = nullptr;
static void (*v_CPlayer__DoAnimationEvent)(void* player, unsigned int event, int a3, int a4) = nullptr;
static int64_t (*v_CBaseEntity_SetModel)(int64_t entity, const char* modelName) = nullptr;
static void JetDrive_BindWhistleStudio(void* pWeapon);
static void JetDrive_BindWhistleOnPlayer(void* pPlayer);
static bool JetDrive_AttackLockLive(void* pWeapon);

static inline float GraphCapped(float val, float a, float b, float outAtA, float outAtB)
{
	return RemapValClamped(val, a, b, outAtA, outAtB);
}

static inline bool JetDrive_IsZiplining(CPlayer* player)
{
	return HalfDuck_PlayerIsZiplining(player);
}

// m_phaseShiftTimeEnd is private; 0x15B8 is the server offset.
static constexpr uintptr_t JETDRIVE_PHASESHIFT_TIMEEND_OFFSET_SERVER = 0x15B8;
static inline bool JetDrive_IsPhaseShifted(CPlayer* player)
{
	if (!player)
		return false;

	// POD float at the server offset.
	const float timeEnd = *reinterpret_cast<float*>(
		reinterpret_cast<uintptr_t>(player) + JETDRIVE_PHASESHIFT_TIMEEND_OFFSET_SERVER);
	return JetDrive_CurTime() < timeEnd; // m_phaseShiftTimeEnd is curtime-domain
}

// m_RefEHandle is protected. Same-layout derived class, zero-offset downcast.
class JetDrive_EntityFieldAccess : public CBaseEntity
{
public:
	using CBaseEntity::m_RefEHandle;
};

// CBaseHandle::Set has no implementation here; use m_RefEHandle + UnsafeFromIndex.
static EHANDLE JetDrive_HandleForEntity(CBaseEntity* ent)
{
	if (!ent)
		return EHANDLE();

	auto* const accessor = static_cast<JetDrive_EntityFieldAccess*>(ent);
	return EHANDLE::UnsafeFromIndex(static_cast<int>(accessor->m_RefEHandle.ToInt()));
}

// CBaseHandle::Get has no implementation here; resolve via g_serverEntityList.
static CBaseEntity* JetDrive_ResolveHandle(const EHANDLE& h)
{
	const uint32_t rawHandle = static_cast<uint32_t>(h.ToInt());
	if (rawHandle == INVALID_EHANDLE_INDEX || !g_serverEntityList)
		return nullptr;

	const CBaseHandle handle = CBaseHandle::UnsafeFromIndex(static_cast<int>(rawHandle));
	if (void* const pEntity = g_serverEntityList->LookupEntity(handle))
		return reinterpret_cast<CBaseEntity*>(pEntity);

	const int entIndex = static_cast<int>(rawHandle & ENT_ENTRY_MASK);
	if (entIndex >= 0 && entIndex < NUM_ENT_ENTRIES)
		return reinterpret_cast<CBaseEntity*>(g_serverEntityList->LookupEntityByNetworkIndex(entIndex));

	return nullptr;
}

//-----------------------------------------------------------------------------
// Fires CodeCallback_OnJetDriveXXX(player[, extraFloat]) into the server VM.
//-----------------------------------------------------------------------------
static void JetDrive_FireCallback(CPlayer* const player, const char* const funcName,
	const float* const extraFloatArg = nullptr)
{
	if (!g_pServerScript)
		return;

	const HSCRIPT hPlayerScript = player->GetScriptInstance();
	if (!hPlayerScript)
		return;

	const HSCRIPT hFunc = g_pServerScript->FindFunction(funcName, nullptr, nullptr);
	if (!hFunc)
		return;

	ScriptVariant_t args[2];
	args[0] = hPlayerScript;
	int nArgs = 1;
	if (extraFloatArg)
	{
		args[1] = *extraFloatArg;
		nArgs = 2;
	}

	g_pServerScript->ExecuteFunction(hFunc, args, nArgs, nullptr, nullptr);
}

//-----------------------------------------------------------------------------
// 14 networked fields from a seqlock sidecar. No entity memory. Boot-time SendTable_Init read.
//-----------------------------------------------------------------------------
static ConVar bridge_jetdrive_wire("bridge_jetdrive_wire", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL | FCVAR_ACCESSIBLE_FROM_THREADS,
	"Replicate the 14 JetDrive fields to the S21 client. Boot-time "
	"SendTable_Init read -- set as a launch arg, not in-console.");

struct JetDriveWireSlot
{
	volatile uint64_t handleKey;  // 0 = free; packed SDKEntityHandle otherwise
	volatile LONG     seq;        // even = stable, odd = publish in progress
	JetDriveWire      wire;
};

static JetDriveWireSlot s_jetDriveWireSlots[64];
static LONG s_jetDriveWireCursor = 0;
// Published before any key and retired first on shutdown so a reader cannot
// scan slots being torn down.
static volatile LONG s_jetDriveWireUsed = 0;

static inline uint64_t JetDriveWire_PackHandle(const SDKEntityHandle& h)
{
	return h.IsValid() ? static_cast<uint64_t>(h.Raw()) : 0;
}

static JetDriveWireSlot* JetDriveWire_FindSlot(uint64_t key)
{
	if (!key || !s_jetDriveWireUsed)
		return nullptr;
	for (JetDriveWireSlot& slot : s_jetDriveWireSlots)
	{
		if (slot.handleKey == key)
			return &slot;
	}
	return nullptr;
}

static inline void JetDriveWire_CopyVec(float* const dst, const Vector3D& v)
{
	dst[0] = v.x;
	dst[1] = v.y;
	dst[2] = v.z;
}

static void JetDriveWire_FillCtorIdle(JetDriveWire& w)
{
	memset(&w, 0, sizeof(w));
	w.m_targetEnt = 0x00FFFFFF;
	w.m_decelWindowTimeOutTime = -1.0f;
	JetDriveWire_CopyVec(w.m_doubleJumpVelocity, vec3_invalid);
}

// Bools flatten to ints; the RecvProps are Int, not 1-bit.
static void JetDriveWire_Flatten(const JetDriveState& s, JetDriveWire& w)
{
	w.m_wasActive              = s.m_jetDriveWasActive ? 1 : 0;
	w.m_active                 = s.m_jetDriveActive ? 1 : 0;
	w.m_targetEnt              = SDKEntityState_PackS21RecvEHandle(
		static_cast<int>(s.m_jetDriveTargetEnt.ToInt()));
	w.m_inDecelWindow          = s.m_jetDriveInDecelWindow ? 1 : 0;
	w.m_speed                  = s.m_jetDriveSpeed;
	w.m_accel                  = s.m_jetDriveAccel;
	w.m_timeout                = s.m_jetDriveTimeout;
	w.m_doubleJumpVelBackFrac  = s.m_jetDriveDoubleJumpVelBackFrac;
	w.m_startTime              = s.m_jetDriveStartTime;
	w.m_decelWindowTimeOutTime = s.m_jetDriveDecelWindowTimeOutTime;
	JetDriveWire_CopyVec(w.m_targetPos,          s.m_jetDriveTargetPos);
	JetDriveWire_CopyVec(w.m_targetEntOffset,    s.m_jetDriveTargetEntOffset);
	JetDriveWire_CopyVec(w.m_startPos,           s.m_jetDriveStartPos);
	JetDriveWire_CopyVec(w.m_doubleJumpVelocity, s.m_jetDriveDoubleJumpVelocity);
}

// seq goes odd for the payload write so a reader detects a tear and retries.
static void JetDriveWire_Publish(CPlayer* const player, const JetDriveState& s)
{
	const uint64_t key = JetDriveWire_PackHandle(SDKEntityState_GetHandle(player));
	if (!key)
		return;

	JetDriveWire w;
	JetDriveWire_Flatten(s, w);

	JetDriveWireSlot* slot = JetDriveWire_FindSlot(key);
	if (!slot)
	{
		for (JetDriveWireSlot& cand : s_jetDriveWireSlots)
		{
			if (cand.handleKey == 0)
			{
				slot = &cand;
				break;
			}
		}
		if (slot)
			InterlockedIncrement(&s_jetDriveWireUsed);
		else
		{
			const LONG idx = (InterlockedIncrement(&s_jetDriveWireCursor) - 1) & 63;
			slot = &s_jetDriveWireSlots[idx];
		}
		// Retire the evicted key before the payload write so an eviction cannot be read as the previous owner.
		slot->handleKey = 0;
	}

	InterlockedIncrement(&slot->seq);   // -> odd, full barrier
	slot->wire = w;
	InterlockedIncrement(&slot->seq);   // -> even, full barrier
	slot->handleKey = key;

	// Cold path (state changes only). Proves script -> sidecar on run one and
	// records the player pointer the proxies must match against.
	static volatile LONG s_pubN = 0;
	const LONG n = InterlockedIncrement(&s_pubN);
	if (n <= 8)
		Warning(eDLL_T::SERVER,
			"[JETDRIVE-WIRE] publish #%d player=%p key=0x%llX active=%d startTime=%.2f target=(%.1f %.1f %.1f)\n",
			static_cast<int>(n), player, static_cast<unsigned long long>(key),
			w.m_active, w.m_startTime, w.m_targetPos[0], w.m_targetPos[1], w.m_targetPos[2]);
}

bool JetDrive_WireEnabled(void)
{
	return bridge_jetdrive_wire.GetBool();
}

bool JetDrive_GetWire(const void* pPlayer, JetDriveWire* pOut)
{
	if (!pPlayer || !pOut || !bridge_jetdrive_wire.GetBool())
		return false;

	JetDriveWire_FillCtorIdle(*pOut);

	const uint64_t key = JetDriveWire_PackHandle(SDKEntityState_GetHandle(pPlayer));
	JetDriveWireSlot* const slot = JetDriveWire_FindSlot(key);
	if (!slot)
		return true;

	// Seqlock with a bounded retry (this project's loop-safety convention).
	// The re-checked key also closes the slot-eviction race.
	for (int attempt = 0; attempt < 8; ++attempt)
	{
		const LONG before = slot->seq;
		if (before & 1)
			continue;
		const JetDriveWire copy = slot->wire;
		MemoryBarrier();
		if (slot->seq == before && slot->handleKey == key)
		{
			*pOut = copy;
			return true;
		}
	}

	static volatile LONG s_tornN = 0;
	if (InterlockedIncrement(&s_tornN) <= 8)
		Warning(eDLL_T::SERVER,
			"[JETDRIVE-WIRE] seqlock contended out on player=%p -- emitting ctor idle this snapshot\n",
			pPlayer);
	return true;
}

void JetDrive_Wire_LevelShutdown(void)
{
	// Retire the used count FIRST so a reader mid-shutdown skips the scan
	// rather than walking slots being zeroed underneath it.
	InterlockedExchange(&s_jetDriveWireUsed, 0);
	memset(s_jetDriveWireSlots, 0, sizeof(s_jetDriveWireSlots));
	InterlockedExchange(&s_jetDriveWireCursor, 0);
}

// Called at the end of every function that mutates state.
static void JetDrive_Mirror(CPlayer* const player, const JetDriveState& s)
{
	JetDriveWire_Publish(player, s);
	MarkEntityEdictDirty(player);
}

void JetDrive_Begin(CPlayer* player, float speed, float accel,
	const Vector3D& targetPos, CBaseEntity* targetEnt, const Vector3D& targetEntOffset, float timeOut)
{
	if (!player)
		return;

	JetDriveState& s = s_jetDriveMapServer[player];

	s.m_jetDriveSpeed = speed;
	s.m_jetDriveAccel = accel;
	s.m_jetDriveActive = true;
	s.m_jetDriveTargetPos = targetPos;
	s.m_jetDriveStartPos = player->Diag_AbsOrigin();

	s.m_jetDriveDecelWindowTimeOutTime = -1.0f;
	s.m_jetDriveInDecelWindow = false;
	s.m_jetDriveAnimTime = -1.0f;
	s.m_jetDriveStartTime = JetDrive_CurTime() + jetdrive_delay_time.GetFloat();
	s.m_jetDriveTimeout = timeOut;

	if (targetEnt)
	{
		s.m_jetDriveTargetEnt = JetDrive_HandleForEntity(targetEnt);
		s.m_jetDriveTargetEntOffset = targetEntOffset;
	}
	// else: leave m_jetDriveTargetEnt/Offset untouched -- safe because
	// JetDrive_End always resets them to invalid/0 between drives.

	s_jdHoldPlayers[player] = 1;
	s_jdPendingTime.Erase(player);
	JetDrive_BindWhistleOnPlayer(player);
	JetDrive_Mirror(player, s);
}

void JetDrive_End(CPlayer* player)
{
	if (!player)
		return;

	JetDriveState* const s = s_jetDriveMapServer.Find(player);
	if (!s)
		return;

	s->m_jetDriveActive = false;
	s->m_jetDriveSpeed = 0.0f;
	s->m_jetDriveAccel = 0.0f;
	s->m_jetDriveTargetPos = vec3_invalid;
	s->m_jetDriveStartPos = vec3_invalid;
	s->m_jetDriveStartTime = FLT_MAX;
	s->m_jetDriveTimeout = 0.0f;
	s->m_jetDriveDecelWindowTimeOutTime = -1.0f;
	s->m_jetDriveInDecelWindow = false;
	s->m_jetDriveTargetEnt = EHANDLE();
	s->m_jetDriveTargetEntOffset = vec3_invalid;
	s->m_jetDriveDoubleJumpVelocity = vec3_invalid;
	s->m_jetDriveDoubleJumpVelBackFrac = 0.0f;
	s->m_jetDriveAnimTime = jetdrive_anim_linger_time.GetFloat() + JetDrive_CurTime();

	const Vector3D origin = player->Diag_AbsOrigin();
	*reinterpret_cast<float*>(
		reinterpret_cast<uintptr_t>(player) + JD_PLAYER_OFF_FLOORHEIGHT) = origin.z;

	s_jdHoldPlayers[player] = 1;
	s_jdPendingTime[player] = JetDrive_CurTime();
	JetDrive_Mirror(player, *s);
}

//=============================================================================
// Bounded copy into the sound-name buffers.
//=============================================================================
void JetDrive_EnableDoubleJump(CPlayer* player, const Vector3D& launchVelocity,
	float jumpBackVelFrac, const char* sound1p, const char* sound3p)
{
	if (!player)
		return;

	JetDriveState& s = s_jetDriveMapServer[player];
	s.m_jetDriveDoubleJumpVelocity = launchVelocity;
	s.m_jetDriveDoubleJumpVelBackFrac = jumpBackVelFrac;

	if (sound1p)
	{
		strncpy_s(s.m_jetDriveDoubleJumpSound1p, sizeof(s.m_jetDriveDoubleJumpSound1p), sound1p, _TRUNCATE);
	}
	if (sound3p)
	{
		strncpy_s(s.m_jetDriveDoubleJumpSound3p, sizeof(s.m_jetDriveDoubleJumpSound3p), sound3p, _TRUNCATE);
	}

	JetDrive_Mirror(player, s);
}

static bool JetDrive_IsLaunchWeapon(void* pWeapon)
{
	if (!pWeapon)
		return false;
	const char* const pszName = reinterpret_cast<const char*>(
		reinterpret_cast<uintptr_t>(pWeapon) + 0x15B0);
	if (!pszName || !pszName[0] || !strstr(pszName, "companion_launch"))
		return false;
	return strstr(pszName, "entry") == nullptr;
}

static void* JetDrive_PlayerWeaponAt(void* pPlayer, uintptr_t base, int index)
{
	if (!pPlayer)
		return nullptr;
	const uint32_t raw = *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<uintptr_t>(pPlayer) + base + 4u * static_cast<unsigned>(index));
	return SDKEntityState_Resolve(SDKEntityHandle(raw), ESide::Server);
}

static void JetDrive_ForEachLaunchWeapon(void* pPlayer, void (*fn)(void* pWeapon))
{
	if (!pPlayer || !fn)
		return;
	static constexpr uintptr_t kWeapons0 = 0x1690;
	static constexpr uintptr_t kOffhand0 = 0x16B4;
	static constexpr uintptr_t kActive0 = 0x16CC;
	void* seen[20];
	int nSeen = 0;

	for (int i = 0; i < 9; ++i)
	{
		void* const pWeap = JetDrive_PlayerWeaponAt(pPlayer, kWeapons0, i);
		if (!JetDrive_IsLaunchWeapon(pWeap))
			continue;
		bool bDup = false;
		for (int s = 0; s < nSeen; ++s)
		{
			if (seen[s] == pWeap)
			{
				bDup = true;
				break;
			}
		}
		if (bDup || nSeen >= 20)
			continue;
		seen[nSeen++] = pWeap;
	}
	for (int i = 0; i < 8; ++i)
	{
		void* const pWeap = JetDrive_PlayerWeaponAt(pPlayer, kOffhand0, i);
		if (!JetDrive_IsLaunchWeapon(pWeap))
			continue;
		bool bDup = false;
		for (int s = 0; s < nSeen; ++s)
		{
			if (seen[s] == pWeap)
			{
				bDup = true;
				break;
			}
		}
		if (bDup || nSeen >= 20)
			continue;
		seen[nSeen++] = pWeap;
	}
	for (int i = 0; i < 3; ++i)
	{
		void* const pWeap = JetDrive_PlayerWeaponAt(pPlayer, kActive0, i);
		if (!JetDrive_IsLaunchWeapon(pWeap))
			continue;
		bool bDup = false;
		for (int s = 0; s < nSeen; ++s)
		{
			if (seen[s] == pWeap)
			{
				bDup = true;
				break;
			}
		}
		if (bDup || nSeen >= 20)
			continue;
		seen[nSeen++] = pWeap;
	}

	for (int i = 0; i < nSeen; ++i)
		fn(seen[i]);
}

static const char* JetDrive_StudioName(void* pWeapon)
{
	if (!pWeapon)
		return "";
	const uintptr_t cstudio = *reinterpret_cast<uintptr_t*>(
		reinterpret_cast<uint8_t*>(pWeapon) + kJdWeaponOffCStudio);
	if (!cstudio)
		return "";
	const uintptr_t hdr = *reinterpret_cast<uintptr_t*>(cstudio + 0x08);
	if (!hdr)
		return "";
	return reinterpret_cast<const char*>(hdr + 0x10);
}

static void JetDrive_BindWhistleStudio(void* pWeapon)
{
	static volatile LONG s_nBind = 0;
	const LONG n = InterlockedIncrement(&s_nBind);
	if (!pWeapon)
		return;
	if (!v_CBaseEntity_SetModel)
	{
		if (n <= 8)
			Msg(eDLL_T::SERVER, "[JETDRIVE] ptpov bind skipped (SetModel unresolved)\n");
		return;
	}
	const char* const cur = JetDrive_StudioName(pWeapon);
	if (cur && strstr(cur, "ptpov"))
	{
		if (n <= 8)
			Msg(eDLL_T::SERVER, "[JETDRIVE] ptpov already '%.63s'\n", cur);
		return;
	}

	uint8_t* const w = reinterpret_cast<uint8_t*>(pWeapon);
	const int16_t modelIdx = *reinterpret_cast<int16_t*>(w + kJdEntOffModelIndex);
	const int32_t worldIdx = *reinterpret_cast<int32_t*>(w + kJdWeaponOffWorldModelIndex);

	(void)Server_PrecacheModel_Invoke(kJdWhistlePtpov);
	(void)v_CBaseEntity_SetModel(reinterpret_cast<int64_t>(pWeapon), kJdWhistlePtpov);
	const uintptr_t cstudio = *reinterpret_cast<uintptr_t*>(w + kJdWeaponOffCStudio);
	*reinterpret_cast<int16_t*>(w + kJdEntOffModelIndex) = modelIdx;
	*reinterpret_cast<int32_t*>(w + kJdWeaponOffWorldModelIndex) = worldIdx;
	if (cstudio)
		*reinterpret_cast<uintptr_t*>(w + kJdWeaponOffCStudio) = cstudio;
	MarkEntityEdictDirty(pWeapon);

	if (n <= 8)
		Msg(eDLL_T::SERVER, "[JETDRIVE] bound whistle ptpov was='%.63s' now='%.63s'\n",
			cur && cur[0] ? cur : "?", JetDrive_StudioName(pWeapon));
}

static void JetDrive_BindWhistleOnPlayer(void* pPlayer)
{
	JetDrive_ForEachLaunchWeapon(pPlayer, JetDrive_BindWhistleStudio);
}

static bool JetDrive_AttackLockLive(void* pWeapon)
{
	const float* const pAtk = s_jdAttackTime.Find(pWeapon);
	if (!pAtk)
		return false;
	return (JetDrive_CurTime() - *pAtk) < kJdAttackLockSeconds;
}

static bool JetDrive_ShouldLockAnims(void* pWeapon)
{
	return JetDrive_ShouldHoldOffhand(pWeapon) || JetDrive_AttackLockLive(pWeapon);
}

static void JetDrive_ArmIdleTimer(void* pWeapon, float duration)
{
	if (!pWeapon)
		return;
	uint8_t* const w = reinterpret_cast<uint8_t*>(pWeapon);
	const float until = JetDrive_CurTime() + duration;
	*reinterpret_cast<float*>(w + kJdWeaponNextReady) = until;
	*reinterpret_cast<float*>(w + kJdWeaponTimeIdle) = until;
	MarkEntityEdictDirty(pWeapon);
}

static void JetDrive_SetIdleAnims(void* pWeapon, bool bEnable)
{
	if (!pWeapon)
		return;
	const int off = DTExtend_FindNativePropOffset(pWeapon, "m_shouldPlayIdleAnims");
	if (off <= 0)
		return;
	*reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(pWeapon) + off) = bEnable ? 1 : 0;
	MarkEntityEdictDirty(pWeapon);
}

static void JetDrive_ArmDriveClocks(void* pWeapon)
{
	if (!pWeapon)
		return;
	const float hold = jetdrive_weapon_clock_hold.GetFloat();
	if (hold <= 0.0f)
		return;
	const float now = JetDrive_CurTime();
	if (now <= 0.0f)
		return;

	float* const pReady = reinterpret_cast<float*>(
		reinterpret_cast<uintptr_t>(pWeapon) + kJdWeaponNextReady);
	if (*pReady - now > hold * 0.5f)
		return;

	const float stamp = now + hold;
	*pReady = stamp;
	*reinterpret_cast<float*>(
		reinterpret_cast<uintptr_t>(pWeapon) + kJdWeaponNextPrimary) = stamp;
	*reinterpret_cast<float*>(
		reinterpret_cast<uintptr_t>(pWeapon) + kJdWeaponTimeIdle) = stamp;
	MarkEntityEdictDirty(pWeapon);

	if (sdk_jetdrive_anim_diag.GetBool())
	{
		const unsigned int state = *reinterpret_cast<const unsigned int*>(
			reinterpret_cast<uintptr_t>(pWeapon) + kJdWeaponWeapState);
		uint8_t idleAnims = 1;
		const int off = DTExtend_FindNativePropOffset(pWeapon, "m_shouldPlayIdleAnims");
		if (off > 0)
			idleAnims = *reinterpret_cast<const uint8_t*>(
				reinterpret_cast<const uint8_t*>(pWeapon) + off);
		Msg(eDLL_T::SERVER,
			"[JETDRIVE] clocks weapon=%p state=%u ready=%.3f idle=%.3f now=%.3f idleAnims=%u\n",
			pWeapon, state, stamp, stamp, now, static_cast<unsigned>(idleAnims));
	}
}

static void JetDrive_ReleaseDriveClocks(void* pWeapon)
{
	if (!pWeapon)
		return;
	const float now = JetDrive_CurTime();
	if (now <= 0.0f)
		return;

	*reinterpret_cast<float*>(
		reinterpret_cast<uintptr_t>(pWeapon) + kJdWeaponNextReady) = now;
	*reinterpret_cast<float*>(
		reinterpret_cast<uintptr_t>(pWeapon) + kJdWeaponNextPrimary) = now;
	*reinterpret_cast<float*>(
		reinterpret_cast<uintptr_t>(pWeapon) + kJdWeaponTimeIdle) = now;
	MarkEntityEdictDirty(pWeapon);
}

static void JetDrive_HoldTickWeapon(void* pWeapon)
{
	JetDrive_SetIdleAnims(pWeapon, false);
	JetDrive_ArmDriveClocks(pWeapon);
}

static void JetDrive_ReleaseHoldWeapon(void* pWeapon)
{
	JetDrive_ReleaseDriveClocks(pWeapon);
	JetDrive_SetIdleAnims(pWeapon, true);

	if (sdk_jetdrive_anim_diag.GetBool())
	{
		const float now = JetDrive_CurTime();
		const unsigned int state = *reinterpret_cast<const unsigned int*>(
			reinterpret_cast<uintptr_t>(pWeapon) + kJdWeaponWeapState);
		const float ready = *reinterpret_cast<const float*>(
			reinterpret_cast<uintptr_t>(pWeapon) + kJdWeaponNextReady);
		const float idle = *reinterpret_cast<const float*>(
			reinterpret_cast<uintptr_t>(pWeapon) + kJdWeaponTimeIdle);
		uint8_t idleAnims = 1;
		const int off = DTExtend_FindNativePropOffset(pWeapon, "m_shouldPlayIdleAnims");
		if (off > 0)
			idleAnims = *reinterpret_cast<const uint8_t*>(
				reinterpret_cast<const uint8_t*>(pWeapon) + off);
		Msg(eDLL_T::SERVER,
			"[JETDRIVE] release weapon=%p state=%u ready=%.3f idle=%.3f now=%.3f idleAnims=%u\n",
			pWeapon, state, ready, idle, now, static_cast<unsigned>(idleAnims));
	}

	Translocation_HolsterWeaponOriginal(pWeapon);
}

static void JetDrive_FireDoubleJumpAnim(CPlayer* player)
{
	if (!player || !v_CPlayer__DoAnimationEvent)
		return;
	v_CPlayer__DoAnimationEvent(player, PLAYERANIMEVENT_DOUBLEJUMP, 0, 0);
}

bool JetDrive_FilterWeaponState(void* pWeapon, unsigned int newState)
{
	if (!JetDrive_IsLaunchWeapon(pWeapon))
		return false;
	JetDrive_BindWhistleStudio(pWeapon);

	if (newState == kJdWeapStateAttack)
	{
		s_jdAttackTime[pWeapon] = JetDrive_CurTime();
		JetDrive_ArmIdleTimer(pWeapon, kJdAttackLockSeconds);
		return false;
	}

	if (!JetDrive_ShouldLockAnims(pWeapon))
		return false;

	unsigned int* const pState = reinterpret_cast<unsigned int*>(
		reinterpret_cast<uintptr_t>(pWeapon) + kJdWeaponWeapState);
	if (*pState != kJdWeapStateAttack)
	{
		*pState = kJdWeapStateAttack;
		MarkEntityEdictDirty(pWeapon);
	}

	static volatile LONG s_nLock = 0;
	if (InterlockedIncrement(&s_nLock) <= 16)
		Msg(eDLL_T::SERVER,
			"[JETDRIVE] keep ATTACK (refuse state %u) weap=%p\n",
			newState, pWeapon);
	return true;
}

bool JetDrive_FilterIdealActivity(void* pWeapon, unsigned int activity)
{
	if (!JetDrive_IsLaunchWeapon(pWeapon))
		return false;
	if (!JetDrive_ShouldLockAnims(pWeapon))
		return false;
	if (activity == 453 || activity == 454 || activity == 458)
		return true;
	if (activity >= 468 && activity <= 471)
		return true;
	if (activity == 478 || activity == 479)
		return true;
	return false;
}

bool JetDrive_ShouldBlockSetActiveWeapon(void* pPlayer, void* pWeapon)
{
	if (!pPlayer || !pWeapon)
		return false;
	if (JetDrive_IsLaunchWeapon(pWeapon))
		return false;
	const uint32_t flags = *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<uintptr_t>(pWeapon) + JD_WEAPON_TYPE_FLAGS);
	if ((flags & (JD_WPT_TACTICAL | JD_WPT_VIEWHANDS)) != 0)
		return false;

	CPlayer* const pOwner = reinterpret_cast<CPlayer*>(pPlayer);
	if (JetDrive_IsActive(pOwner))
		return true;
	if (s_jdPendingTime.Find(pOwner))
		return false;

	static constexpr uintptr_t kWeapons0 = 0x1690;
	static constexpr uintptr_t kOffhand0 = 0x16B4;
	static constexpr uintptr_t kActive0 = 0x16CC;
	for (int i = 0; i < 9; ++i)
	{
		void* const pWeap = JetDrive_PlayerWeaponAt(pPlayer, kWeapons0, i);
		if (JetDrive_IsLaunchWeapon(pWeap) && JetDrive_AttackLockLive(pWeap))
			return true;
	}
	for (int i = 0; i < 8; ++i)
	{
		void* const pWeap = JetDrive_PlayerWeaponAt(pPlayer, kOffhand0, i);
		if (JetDrive_IsLaunchWeapon(pWeap) && JetDrive_AttackLockLive(pWeap))
			return true;
	}
	for (int i = 0; i < 3; ++i)
	{
		void* const pWeap = JetDrive_PlayerWeaponAt(pPlayer, kActive0, i);
		if (JetDrive_IsLaunchWeapon(pWeap) && JetDrive_AttackLockLive(pWeap))
			return true;
	}
	return false;
}

bool JetDrive_ShouldHoldOffhand(void* pWeapon)
{
	if (!JetDrive_IsLaunchWeapon(pWeapon))
		return false;
	const uint32_t rawOwner = *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<const uint8_t*>(pWeapon) + 0x11F0);
	void* const pOwner = SDKEntityState_Resolve(SDKEntityHandle(rawOwner), ESide::Server);
	if (!pOwner)
		return false;
	CPlayer* const pPlayer = reinterpret_cast<CPlayer*>(pOwner);
	return JetDrive_IsActive(pPlayer)
		|| s_jdHoldPlayers.Find(pPlayer) != nullptr;
}

bool JetDrive_NoteOffhandHolster(void* pWeapon)
{
	if (!JetDrive_IsLaunchWeapon(pWeapon))
		return false;
	JetDrive_BindWhistleStudio(pWeapon);
	const uint32_t rawOwner = *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<const uint8_t*>(pWeapon) + 0x11F0);
	void* const pOwner = SDKEntityState_Resolve(SDKEntityHandle(rawOwner), ESide::Server);
	if (!pOwner)
		return false;
	CPlayer* const pPlayer = reinterpret_cast<CPlayer*>(pOwner);
	if (JetDrive_IsActive(pPlayer)
		|| s_jdHoldPlayers.Find(pPlayer) != nullptr)
		return true;
	if (JetDrive_AttackLockLive(pWeapon))
		return true;
	const unsigned int weapState = *reinterpret_cast<const unsigned int*>(
		reinterpret_cast<uintptr_t>(pWeapon) + kJdWeaponWeapState);
	if (weapState == kJdWeapStateAttack)
		return true;
	return false;
}

void JetDrive_TickHolds(void* pPlayer)
{
	if (pPlayer && s_jdHoldPlayers.Find(pPlayer))
		JetDrive_ForEachLaunchWeapon(pPlayer, JetDrive_HoldTickWeapon);

	if (!s_jdPendingTime.Size())
		return;

	// Resolve through the handle first: a stale entry must never deref freed
	// memory (the destroy observer normally evicts before this runs).
	const float now = JetDrive_CurTime();
	SDKEntityHandle expired[16];
	int nExpired = 0;
	for (const auto& kv : s_jdPendingTime)
	{
		CPlayer* const pPending = reinterpret_cast<CPlayer*>(
			SDKEntityState_Resolve(kv.first, ESide::Server));
		const bool bDrop = !pPending || JetDrive_IsActive(pPending)
			|| (now - kv.second >= kJdHoldOrphanSeconds);
		if (!bDrop)
			continue;
		if (pPending && !JetDrive_IsActive(pPending))
			JetDrive_ForEachLaunchWeapon(pPending, JetDrive_ReleaseHoldWeapon);
		if (nExpired < static_cast<int>(sizeof(expired) / sizeof(expired[0])))
			expired[nExpired++] = kv.first;
	}
	for (int i = 0; i < nExpired; ++i)
	{
		s_jdHoldPlayers.Erase(expired[i]);
		s_jdPendingTime.Erase(expired[i]);
	}
}

bool JetDrive_IsActive(CPlayer* player)
{
	if (!player)
		return false;
	const JetDriveState* const s = s_jetDriveMapServer.Find(player);
	return s && s->m_jetDriveActive;
}

bool JetDrive_IsInDecelWindow(CPlayer* player)
{
	if (!player)
		return false;
	const JetDriveState* const s = s_jetDriveMapServer.Find(player);
	return s && s->m_jetDriveInDecelWindow;
}

static int JetDrive_ButtonsPressed(void* mv)
{
	if (!mv)
		return 0;
	return *reinterpret_cast<const int*>(
		reinterpret_cast<uintptr_t>(mv) + JD_MV_OFF_BUTTONS_PRESSED);
}

static void JetDrive_WriteMoveVelocity(void* mv, const Vector3D& vel)
{
	if (!mv)
		return;
	float* const out = reinterpret_cast<float*>(
		reinterpret_cast<uintptr_t>(mv) + JD_MV_OFF_VELOCITY);
	out[0] = vel.x;
	out[1] = vel.y;
	out[2] = vel.z;
}

static Vector3D JetDrive_ReadMoveVelocity(void* mv, const Vector3D& fallback)
{
	if (!mv)
		return fallback;
	const float* const in = reinterpret_cast<const float*>(
		reinterpret_cast<uintptr_t>(mv) + JD_MV_OFF_VELOCITY);
	return Vector3D(in[0], in[1], in[2]);
}

static Vector3D JetDrive_PlayerUp(CPlayer* player)
{
	if (player && v_CBaseEntity_EntityToWorldTransform)
	{
		if (const matrix3x4_t* const mat = v_CBaseEntity_EntityToWorldTransform(player))
		{
			const Vector3D up(mat->m_flMatVal[0][2], mat->m_flMatVal[1][2], mat->m_flMatVal[2][2]);
			if (up.LengthSqr() > 0.25f && up.LengthSqr() < 4.0f)
				return up.NormalizedSafe(Vector3D(0.0f, 0.0f, 1.0f));
		}
	}
	return Vector3D(0.0f, 0.0f, 1.0f);
}

static bool JetDrive_DoubleJumpVelOk(const Vector3D& v)
{
	if (v == vec3_invalid)
		return false;
	if (!v.IsValid())
		return false;
	return v.Length() > 1.0f && v.Length() < 2000.0f;
}

static Vector3D JetDrive_ResolveTargetPos(const JetDriveState& s)
{
	CBaseEntity* const targetEnt = JetDrive_ResolveHandle(s.m_jetDriveTargetEnt);
	if (!targetEnt)
		return s.m_jetDriveTargetPos;

	// Script offset is world-space. VectorTransform only if the matrix
	// translation is the live origin -- identity-at-0 sends dest to 0.
	TriggerPass_EnsureAbsOrigin(targetEnt);
	const Vector3D origin = targetEnt->Diag_AbsOrigin();
	if (v_CBaseEntity_EntityToWorldTransform)
	{
		if (const matrix3x4_t* const mat = v_CBaseEntity_EntityToWorldTransform(targetEnt))
		{
			const Vector3D matOrg(mat->m_flMatVal[0][3], mat->m_flMatVal[1][3], mat->m_flMatVal[2][3]);
			if ((matOrg - origin).LengthSqr() < 1.0f)
			{
				Vector3D world;
				VectorTransform(s.m_jetDriveTargetEntOffset, *mat, world);
				return world;
			}
		}
	}
	return origin + s.m_jetDriveTargetEntOffset;
}

void JetDrive_AccelFromMoveCtx(void* ctx)
{
	if (!ctx)
		return;
	CPlayer* const player = *reinterpret_cast<CPlayer**>(
		reinterpret_cast<uintptr_t>(ctx) + JD_CTX_OFF_PLAYER);
	void* const mv = *reinterpret_cast<void**>(
		reinterpret_cast<uintptr_t>(ctx) + JD_CTX_OFF_MOVEDATA);
	JetDrive_Accel(player, mv, TriggerPass_FrameTime());
}

void JetDrive_Accel(CPlayer* player, void* mv, float dt)
{
	CmdChain_Bump(CMDCHAIN_TICK_JETDRIVE);

	if (!player)
		return;

	TriggerPass_EnsureAbsOrigin(player);

	JetDriveState* const pState = s_jetDriveMapServer.Find(player);
	if (!pState)
		return;
	JetDriveState& s = *pState;

	const float curtime = JetDrive_CurTime();
	const int buttonsPressed = JetDrive_ButtonsPressed(mv);

	// Step 0 -- gate.
	if (!s.m_jetDriveActive || curtime <= s.m_jetDriveStartTime)
	{
		if (s.m_jetDriveWasActive)
		{
			s.m_jetDriveWasActive = false;
			JetDrive_Mirror(player, s);
		}
		return;
	}

	// Step 1 -- start edge.
	bool justStarted = false;
	if (!s.m_jetDriveWasActive)
	{
		s.m_jetDriveWasActive = true;
		justStarted = true;
		JetDrive_FireCallback(player, "CodeCallback_OnJetDriveStart");
	}

	const Vector3D targetPos = JetDrive_ResolveTargetPos(s);
	const Vector3D currentOrigin = player->Diag_AbsOrigin();

	if (justStarted)
	{
		static volatile LONG s_startN = 0;
		if (InterlockedIncrement(&s_startN) <= 4)
			Warning(eDLL_T::SERVER,
				"[JETDRIVE] start #%d origin=(%.1f %.1f %.1f) dest=(%.1f %.1f %.1f) snap=(%.1f %.1f %.1f) speed=%.1f timeout=%.2f\n",
				static_cast<int>(s_startN),
				currentOrigin.x, currentOrigin.y, currentOrigin.z,
				targetPos.x, targetPos.y, targetPos.z,
				s.m_jetDriveTargetPos.x, s.m_jetDriveTargetPos.y, s.m_jetDriveTargetPos.z,
				s.m_jetDriveSpeed, s.m_jetDriveTimeout);
	}

	// Step 3 -- distances and arc height.
	const float distSqrFromCurrent = (targetPos - currentOrigin).LengthSqr();
	const float distSqrFromStartXY =
		Sqr(targetPos.x - s.m_jetDriveStartPos.x) + Sqr(targetPos.y - s.m_jetDriveStartPos.y);
	const float totalDist = sqrtf(distSqrFromStartXY + Sqr(targetPos.z - s.m_jetDriveStartPos.z));
	const float horizDist = sqrtf(distSqrFromStartXY);

	const float decelDist = jetdrive_decel_dist.GetFloat();
	const float travelSpan = totalDist - decelDist;
	const float vertOffsetAtT = jetdrive_initial_vert_offset.GetFloat() * horizDist;

	float t = 1.0f - fminf(distSqrFromCurrent / Sqr(travelSpan), 1.0f);
	t = fminf(fmaxf(t, 0.0f), 1.0f);
	const float archHeight = GraphCapped(t, 0.0f, 1.0f, vertOffsetAtT, 0.0f);

	// Step 4 -- aim direction including the arc.
	Vector3D aimDir = (targetPos + Vector3D(0, 0, archHeight)) - currentOrigin;

	// Step 5 -- speed for this tick.
	const float speedMult = GraphCapped(t, 0.0f, 1.0f, jetdrive_initial_speed_mult.GetFloat(), 1.0f);
	float baseSpeed = speedMult * s.m_jetDriveSpeed;
	const float elapsed = curtime - s.m_jetDriveStartTime;

	// Step 6 -- duck-cancel input test (edge-triggered, matches CMoveData::m_nButtonsPressed).
	const bool duckCancelPressed = (buttonsPressed & JETDRIVE_DUCKCANCEL_BUTTON_MASK) != 0;
	const bool duckCancel = duckCancelPressed && (elapsed > jetdrive_duck_cancel_min_time.GetFloat());

	// Step 7 -- is JetDrive still alive this tick?
	const bool stillActive = s.m_jetDriveInDecelWindow
		? (curtime <= s.m_jetDriveDecelWindowTimeOutTime)
		: (elapsed <= s.m_jetDriveTimeout);

	bool terminal = !stillActive || duckCancel;
	Vector3D newVel = JetDrive_ReadMoveVelocity(mv, player->Diag_AbsVelocity());
	bool wroteVelocity = false;
	bool doubleJumpFiredThisTick = false;

	if (!terminal)
	{
		if (!JetDrive_IsZiplining(player) && !JetDrive_IsPhaseShifted(player))
		{
			const Vector3D curVel = JetDrive_ReadMoveVelocity(mv, player->Diag_AbsVelocity());
			const float curSpeed = curVel.Length();
			Vector3D aimDirNorm = aimDir.NormalizedSafe(vec3_origin);
			const float alignSpeed = DotProduct(aimDirNorm, curVel);

			// Stuck-kick: 0.5, cos(30deg)=0.86602539, 4.0.
			if (!justStarted && !s.m_jetDriveInDecelWindow && alignSpeed < jetdrive_stuck_speed.GetFloat())
			{
				// S21: 1 - min(aimLen/travelSpan, 1). Negative span => progress 1.
				const float aimLen = aimDir.Length();
				const float spanRatio = (travelSpan != 0.0f) ? (aimLen / travelSpan) : 1.0f;
				const float progressFrac = fminf(fmaxf(1.0f - fminf(spanRatio, 1.0f), 0.0f), 1.0f);
				const float verticalDot = -aimDirNorm.z;

				if (progressFrac < 0.5f && verticalDot < 0.86602539f)
				{
					baseSpeed *= 1.5f;
					// Sign-preserving: aimDir.z += fabsf(aimDir.z) * 4.0, not a flat +4.0.
					aimDir.z += fabsf(aimDir.z) * 4.0f;
				}
			}

			const float targetSpeed = (baseSpeed <= curSpeed)
				? fmaxf(curSpeed - dt * s.m_jetDriveAccel, baseSpeed)
				: baseSpeed;
			newVel = aimDir.NormalizedSafe(vec3_origin) * targetSpeed;

			// Step 9 -- decel-window entry.
			bool applyDirect = true;
			if (!s.m_jetDriveInDecelWindow)
			{
				if (Sqr(decelDist) < distSqrFromCurrent || justStarted)
				{
					applyDirect = true;
				}
				else
				{
					s.m_jetDriveInDecelWindow = true;
					s.m_jetDriveDecelWindowTimeOutTime = jetdrive_decel_doublejump_time_window.GetFloat() + curtime;
					// Callback float is the absolute window-expiry timestamp, not a fraction.
					JetDrive_FireCallback(player, "CodeCallback_OnJetDriveWindowBegin", &s.m_jetDriveDecelWindowTimeOutTime);
					applyDirect = false;
				}
			}
			else
			{
				applyDirect = false;
			}

			if (!applyDirect)
			{
				// S21 scales the current mv velocity, not aim. Aim goes to
				// zero on the Echo point and would freeze the player there.
				const float windowRemaining = s.m_jetDriveDecelWindowTimeOutTime - curtime;
				const float windowFrac = windowRemaining / jetdrive_decel_doublejump_time_window.GetFloat();
				const float finalVelFrac = fmaxf(windowFrac, jetdrive_decel_final_vel_frac.GetFloat());
				const Vector3D slideSrc = JetDrive_ReadMoveVelocity(mv, player->Diag_AbsVelocity());
				Vector3D slideDir = slideSrc.NormalizedSafe(aimDirNorm);
				if (slideDir.LengthSqr() < 1.0e-6f)
					slideDir = aimDirNorm;
				newVel = slideDir * s.m_jetDriveSpeed * finalVelFrac;

				const bool doubleJumpRegistered = JetDrive_DoubleJumpVelOk(s.m_jetDriveDoubleJumpVelocity);
				const bool dodgePressed = (buttonsPressed & JETDRIVE_DOUBLEJUMP_BUTTON_MASK) != 0;

				if (doubleJumpRegistered && dodgePressed)
				{
					QAngle eyeAngles;
					player->EyeAngles(&eyeAngles);
					Vector3D viewDir;
					AngleVectors(eyeAngles, &viewDir);
					Vector3D viewFlat(viewDir.x, viewDir.y, 0.0f);
					const Vector3D viewFlatNorm = viewFlat.NormalizedSafe(vec3_origin);

					const float backAmount = DotProduct(newVel.NormalizedSafe(vec3_origin), viewFlatNorm);
					const float backFrac = GraphCapped(backAmount, -1.0f, 1.0f, s.m_jetDriveDoubleJumpVelBackFrac, 1.0f);
					const float launchX = backFrac * s.m_jetDriveDoubleJumpVelocity.x;
					const Vector3D launchFwdComponent = viewFlatNorm * launchX;
					const Vector3D launchUpComponent = JetDrive_PlayerUp(player) * s.m_jetDriveDoubleJumpVelocity.z;

					newVel = launchFwdComponent + launchUpComponent;
					doubleJumpFiredThisTick = true;

					static volatile LONG s_djN = 0;
					if (InterlockedIncrement(&s_djN) <= 8)
						Warning(eDLL_T::SERVER,
							"[JETDRIVE] dj #%d vel=(%.1f %.1f %.1f) src=(%.1f %.1f %.1f) back=%.2f anim=%d\n",
							static_cast<int>(s_djN),
							newVel.x, newVel.y, newVel.z,
							s.m_jetDriveDoubleJumpVelocity.x,
							s.m_jetDriveDoubleJumpVelocity.y,
							s.m_jetDriveDoubleJumpVelocity.z,
							backFrac,
							v_CPlayer__DoAnimationEvent ? 1 : 0);
				}
			}

			wroteVelocity = true;
		}
		// Ziplining or phase-shifted: skip movement; timeout/duck-cancel clocks still run.
		else
		{
			terminal = !stillActive || duckCancel;
		}
	}

	if (wroteVelocity)
	{
		JetDrive_WriteMoveVelocity(mv, newVel);
		TriggerPass_SetGroundEntityNull(player);
		JetDrive_Mirror(player, s);

		if (doubleJumpFiredThisTick)
		{
			JetDrive_FireDoubleJumpAnim(player);
			JetDrive_FireCallback(player, "CodeCallback_OnJetDriveDoubleJump");
			// No 1p/3p sound-pair helper; names are stored for a later hookup.
			JetDrive_End(player);
		}
		return;
	}

	if (terminal)
	{
		// Terminal exit.
		if (!s.m_jetDriveInDecelWindow && elapsed > s.m_jetDriveTimeout && !duckCancel)
		{
			JetDrive_FireCallback(player, "CodeCallback_OnJetDrivePlayerStuck", &t);
		}

		JetDrive_WriteMoveVelocity(mv,
			JetDrive_ReadMoveVelocity(mv, player->Diag_AbsVelocity()) * jetdrive_duck_cancel_vel_frac.GetFloat());
		JetDrive_FireCallback(player, "CodeCallback_OnJetDriveDuckCancel");
		JetDrive_End(player);
		return;
	}

	// Pause tick that did not hit the terminal condition; state already mirrored where it changed.
}

static SQRESULT Script_JetDrive_Begin(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)) || !pEntity)
		return SQ_ERROR;

	SQFloat speed = 0, accel = 0, timeOut = 0;
	sq_getfloat(v, 2, &speed);
	sq_getfloat(v, 3, &accel);

	const SQVector3D* targetPosVec = nullptr;
	sq_getvector(v, 4, &targetPosVec);

	CBaseEntity* targetEnt = nullptr;
	const SQObjectPtr& entObj = stack_get(v, 5);
	if (!sq_isnull(entObj) && entObj._type == OT_ENTITY && entObj._unVal.pInstance)
	{
		targetEnt = *reinterpret_cast<CBaseEntity**>(
			reinterpret_cast<uintptr_t>(entObj._unVal.pInstance) + 0x50);
	}

	const SQVector3D* targetEntOffsetVec = nullptr;
	sq_getvector(v, 6, &targetEntOffsetVec);

	sq_getfloat(v, 7, &timeOut);

	if (!targetPosVec || !targetEntOffsetVec)
		return SQ_ERROR;

	JetDrive_Begin(reinterpret_cast<CPlayer*>(pEntity), static_cast<float>(speed), static_cast<float>(accel),
		Vector3D(targetPosVec->x, targetPosVec->y, targetPosVec->z), targetEnt,
		Vector3D(targetEntOffsetVec->x, targetEntOffsetVec->y, targetEntOffsetVec->z), static_cast<float>(timeOut));

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_JetDrive_End(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)) || !pEntity)
		return SQ_ERROR;

	JetDrive_End(reinterpret_cast<CPlayer*>(pEntity));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_JetDrive_EnableDoubleJump(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)) || !pEntity)
		return SQ_ERROR;

	const SQVector3D* launchVelVec = nullptr;
	sq_getvector(v, 2, &launchVelVec);

	SQFloat jumpBackVelFrac = 0;
	sq_getfloat(v, 3, &jumpBackVelFrac);

	const SQChar* sound1p = nullptr;
	const SQChar* sound3p = nullptr;
	sq_getstring(v, 4, &sound1p);
	sq_getstring(v, 5, &sound3p);

	if (!launchVelVec)
		return SQ_ERROR;

	JetDrive_EnableDoubleJump(reinterpret_cast<CPlayer*>(pEntity),
		Vector3D(launchVelVec->x, launchVelVec->y, launchVelVec->z),
		static_cast<float>(jumpBackVelFrac), sound1p, sound3p);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Script name ForceEndJetDrive; same implementation as EndJetDrive.
static SQRESULT Script_JetDrive_ForceEnd(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)) || !pEntity)
		return SQ_ERROR;

	JetDrive_End(reinterpret_cast<CPlayer*>(pEntity));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_JetDrive_IsActive(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)) || !pEntity)
		return SQ_ERROR;

	sq_pushbool(v, JetDrive_IsActive(reinterpret_cast<CPlayer*>(pEntity)));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_JetDrive_IsInDecelWindow(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)) || !pEntity)
		return SQ_ERROR;

	sq_pushbool(v, JetDrive_IsInDecelWindow(reinterpret_cast<CPlayer*>(pEntity)));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void JetDrive_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct)
{
	playerStruct->AddFunction(
		"BeginJetDrive",
		"Script_JetDrive_Begin",
		"JetDrive: begins a recall-launch drive toward a target position",
		"void",
		"float speed, float accel, vector targetPos, entity targetEnt, vector targetEntOffset, float timeOut",
		false,
		Script_JetDrive_Begin);

	playerStruct->AddFunction(
		"EndJetDrive",
		"Script_JetDrive_End",
		"JetDrive: force-ends the current drive immediately",
		"void",
		"",
		false,
		Script_JetDrive_End);

	playerStruct->AddFunction(
		"EnableJetDriveDoubleJump",
		"Script_JetDrive_EnableDoubleJump",
		"JetDrive: registers a mid-flight double-jump exit for the current drive",
		"void",
		"vector launchVelocity, float jumpBackVelFrac, string sound1p, string sound3p",
		false,
		Script_JetDrive_EnableDoubleJump);

	playerStruct->AddFunction(
		"ForceEndJetDrive",
		"Script_JetDrive_ForceEnd",
		"JetDrive: force-ends the current drive immediately (script-visible alias, see reference .nut usage)",
		"void",
		"",
		false,
		Script_JetDrive_ForceEnd);

	playerStruct->AddFunction(
		"IsJetDriveActive",
		"Script_JetDrive_IsActive",
		"JetDrive: is a drive currently active",
		"bool",
		"",
		false,
		Script_JetDrive_IsActive);

	playerStruct->AddFunction(
		"IsInJetDriveDecelWindow",
		"Script_JetDrive_IsInDecelWindow",
		"JetDrive: has the current drive entered its decel/double-jump window",
		"bool",
		"",
		false,
		Script_JetDrive_IsInDecelWindow);
}

static __int64 __fastcall Hook_CGameMovement_FullWalkMove_JetDrive(void* ctx)
{
	MoveSimTrace_BeforeFullWalkMove(ctx);
	SlipDiag_BeforeFullWalkMove(ctx);
	PlayerLaunch_BeginFullWalkMove(ctx);
	const __int64 ret = v_CGameMovement__FullWalkMove
		? v_CGameMovement__FullWalkMove(ctx)
		: 0;
	PlayerLaunch_EndFullWalkMove();
	JetDrive_AccelFromMoveCtx(ctx);
	SlipDiag_AfterFullWalkMove(ctx);
	MoveSimTrace_AfterFullWalkMove(ctx);
	return ret;
}

void VJetDrive::GetAdr(void) const
{
	LogFunAdr("JetDrive_Begin", (void*)&JetDrive_Begin);
	LogFunAdr("JetDrive_Accel", (void*)&JetDrive_Accel);
	LogFunAdr("JetDrive_End", (void*)&JetDrive_End);
	LogFunAdr("CGameMovement::FullWalkMove", v_CGameMovement__FullWalkMove);
	LogFunAdr("CPlayer::DoAnimationEvent", v_CPlayer__DoAnimationEvent);
	LogFunAdr("CBaseEntity::SetModel", v_CBaseEntity_SetModel);
}

void VJetDrive::GetFun(void) const
{
	// CGameMovement::FullWalkMove. VMoveSimTrace resolves the same target for
	// verification but never attaches; this class owns the only attach.
	Module_FindPattern(g_GameDll,
		"48 8B C4 55 53 41 57 48 8D 68 ?? 48 81 EC ?? ?? ?? ?? 0F 29 70")
		.GetPtr(v_CGameMovement__FullWalkMove);

	if (!v_CGameMovement__FullWalkMove)
		Warning(eDLL_T::SERVER,
			"[JETDRIVE] CGameMovement::FullWalkMove pattern unresolved -- Accel stays parked\n");

	// CPlayer::DoAnimationEvent. Unique via animstate at +0x7238 (server CPlayer).
	// Event 7 = DOUBLEJUMP: weapon overlay ACT_VM_DOUBLEJUMP (559), 3p overlay 720.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 "
		"48 8B F9 8B F2 48 8B 89 38 72 00 00")
		.GetPtr(v_CPlayer__DoAnimationEvent);

	if (!v_CPlayer__DoAnimationEvent)
		Warning(eDLL_T::SERVER,
			"[JETDRIVE] CPlayer::DoAnimationEvent pattern unresolved -- DJ viewmodel overlay stays client-predicted\n");

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? "
		"57 48 83 EC 20 33 FF 48 8B DA 48 8B F1")
		.GetPtr(v_CBaseEntity_SetModel);
	if (!v_CBaseEntity_SetModel)
		Warning(eDLL_T::SERVER,
			"[JETDRIVE] CBaseEntity::SetModel pattern unresolved -- whistle ptpov will not bind\n");
}

void VJetDrive::GetCon(void) const
{
}

void VJetDrive::Detour(const bool bAttach) const
{
	if (v_CGameMovement__FullWalkMove)
		DetourSetup(&v_CGameMovement__FullWalkMove, &Hook_CGameMovement_FullWalkMove_JetDrive, bAttach);
}

