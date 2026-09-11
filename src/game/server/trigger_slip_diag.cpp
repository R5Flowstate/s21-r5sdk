//=============================================================================//
//
// Purpose: Enter/leave/force diagnostics for CTriggerSlip (promoted wire
// CTriggerSlipSphere). See trigger_slip_diag.h for the mechanism.
//
//=============================================================================//
#include "core/stdafx.h"


#include "trigger_slip_diag.h"
#include "public/edict.h"
#include "game/shared/edict_dirty.h"
#include <cmath>
#include <cstring>

// gpGlobals is defined in the game module (same pattern as mantle_boost / jetdrive).
extern CGlobalVars* gpGlobals;

//-----------------------------------------------------------------------------
// Raw layout -- -verified (r5apex_ds, base).
//-----------------------------------------------------------------------------
// CTriggerSlip force fields (DT_TriggerSlip SendProp offsets)
static constexpr ptrdiff_t SLIP_OFF_DEFAULT_DIR = 3296; // float[3] m_defaultSlipDirection
static constexpr ptrdiff_t SLIP_OFF_SPEED       = 3308; // float m_slipSpeed
static constexpr ptrdiff_t SLIP_OFF_ACCEL       = 3312; // float m_slipAcceleration

// CBaseEntity
static constexpr ptrdiff_t ENT_OFF_EHANDLE      = 0x08;  // EHANDLE (stored into player list)
static constexpr ptrdiff_t ENT_OFF_SERVERCLASS  = 0x50;  // CServerClass*
static constexpr ptrdiff_t ENT_OFF_EDICT        = 0x58;  // int16 edict index
// m_vecAbsOrigin -- CBaseEntity xyz at +0x450/0x454/0x458 (zipline_validation_dedi
// verified). Used for map dump so you can find where each slip sits.
static constexpr ptrdiff_t ENT_OFF_ABS_ORIGIN_X = 0x450;
static constexpr ptrdiff_t ENT_OFF_ABS_ORIGIN_Y = 0x454;
static constexpr ptrdiff_t ENT_OFF_ABS_ORIGIN_Z = 0x458;

// CPlayer (StartTouch / FullWalkMove consumers of m_touchingSlipTriggers)
static constexpr ptrdiff_t PLR_OFF_ORIGIN_X     = 1104;  // float (error msg in StartTouch)
static constexpr ptrdiff_t PLR_OFF_ORIGIN_Y     = 1108;
static constexpr ptrdiff_t PLR_OFF_ORIGIN_Z     = 1112;
static constexpr ptrdiff_t PLR_OFF_SLIP_HANDLES = 26992; // EHANDLE[16] m_touchingSlipTriggers
static constexpr ptrdiff_t PLR_OFF_SLIP_COUNT   = 27056; // int64 count of handles

// Edict walk -- same as dt_extend SvSeqTable (MAX_EDICTS 1<<14).
static constexpr int SLIP_MAX_EDICTS = 16384;
// gpGlobals->m_pEdicts[idx + 0x7808] -> entity ptr (seqtable / util_server pattern).
static constexpr int SLIP_EDICT_ENT_BASE = 0x7808;

// CGameMovement ctx
static constexpr ptrdiff_t GM_CTX_OFF_PLAYER    = 8;
static constexpr ptrdiff_t GM_CTX_OFF_MOVEDATA  = 16;

// CMoveData velocity (mantle_boost S21 layout / FullWalkMove)
static constexpr ptrdiff_t MV_OFF_VELOCITY      = 304; // float[3]

// CPlayer settings block (SettingsBlockData data ptr) -- FullWalkMove slip base.
static constexpr ptrdiff_t PLR_OFF_SETTINGS     = 24328;

// SettingsFieldFinder offset globals (module RVA). Patched by
// snapshot_diag.cpp PatchPlayerLayoutGlobals ([SLIP-SETTINGS]).
static constexpr ptrdiff_t RVA_FINDER_SLIP_SPEED = 0x23875A0;
static constexpr ptrdiff_t RVA_FINDER_SLIP_ACCEL = 0x2386728;

// IsPlayer is vtable slot 93 (offset 744) on CBaseEntity -- StartTouch uses it.
static constexpr int VTBL_SLOT_ISPLAYER        = 93;

//-----------------------------------------------------------------------------
// Engine function pointers.
//-----------------------------------------------------------------------------
// CTriggerSlip::StartTouch(CBaseEntity* other) --
static __int64 (*v_CTriggerSlip__StartTouch)(void* self, void* other) = nullptr;
// CTriggerSlip::EndTouch(CBaseEntity* other) --
static __int64 (*v_CTriggerSlip__EndTouch)(void* self, void* other) = nullptr;
// CTriggerSlip::Activate -- (sets m_defaultSlipDirection from link)
static __int64 (*v_CTriggerSlip__Activate)(void* self) = nullptr;
// CGameMovement helper: resolve slip force direction from player handle list
// -- (returns Vector* in a1 from player a2)
static void* (*v_SlipGetForceDir)(float* outDir, void* player) = nullptr;
// CGameMovement::FullWalkMove -- (applies slip speed/accel)
static __int64 (*v_CGameMovement__FullWalkMove)(void* ctx) = nullptr;

//-----------------------------------------------------------------------------
// ConVars -- default OFF. 0 off, 1 touch, 2 + FullWalkMove force samples.
//-----------------------------------------------------------------------------
static ConVar sdk_slip_diag("sdk_slip_diag", "0", FCVAR_DEVELOPMENTONLY | FCVAR_GAMEDLL,
	"CTriggerSlip touch+force diag. 0=off 1=StartTouch/EndTouch 2=+FullWalkMove force samples. "
	"[SLIP-TOUCH]/[SLIP-END]/[SLIP-FORCE] -> warning.log. Default 0.");

// Sample interval for level-2 force logs (ms of wall time is unavailable cheaply;
// use call counter modulo instead -- FullWalkMove is ~1/tick per player).
static ConVar sdk_slip_diag_force_n("sdk_slip_diag_force_n", "16", FCVAR_DEVELOPMENTONLY | FCVAR_GAMEDLL,
	"When sdk_slip_diag>=2, log [SLIP-FORCE] every N FullWalkMove ticks while "
	"m_touchingSlipTriggers.count > 0 (min 1).");

// Auto map dump: when sdk_slip_diag>=1, emit [SLIP-MAP] once per map on the
// first FullWalkMove after the map name changes (entities are fully spawned).
static ConVar sdk_slip_dump_auto("sdk_slip_dump_auto", "0", FCVAR_DEVELOPMENTONLY | FCVAR_GAMEDLL,
	"When 1 and sdk_slip_diag>=1, auto [SLIP-MAP] dump once per map so you can "
	"see every slip origin without hunting. Manual: sdk_slip_dump. Default 0.");

// Materialize map-baked -1 sentinels into real force numbers. Default OFF.
static ConVar sdk_slip_materialize("sdk_slip_materialize", "0", FCVAR_RELEASE | FCVAR_GAMEDLL,
	"When 1, replace entity m_slipSpeed/m_slipAcceleration < 0 with the player "
	"class settings defaults (from resolved SettingsFieldFinders). Map KVs of "
	"-1 are intentional sentinels, not real force values. Default 0.");

//-----------------------------------------------------------------------------
// Helpers
//-----------------------------------------------------------------------------
static const char* SlipDiag_ServerClassName(const void* ent)
{
	if (!ent)
		return "?";
	const uintptr_t sc = *reinterpret_cast<const uintptr_t*>(
		reinterpret_cast<const uint8_t*>(ent) + ENT_OFF_SERVERCLASS);
	if (!sc)
		return "?";
	const char* const cn = *reinterpret_cast<const char* const*>(sc);
	return cn ? cn : "?";
}

static bool SlipDiag_IsSlipClassName(const char* cn)
{
	// Native S3 class is CTriggerSlip; after promote the ServerClass name is
	// CTriggerSlipSphere (dt_extend vtable steal + class swap). Match both.
	if (!cn || !cn[0])
		return false;
	return strstr(cn, "TriggerSlip") != nullptr;
}

static void SlipDiag_ReadAbsOrigin(const void* ent, float o[3])
{
	const uint8_t* const p = reinterpret_cast<const uint8_t*>(ent);
	o[0] = *reinterpret_cast<const float*>(p + ENT_OFF_ABS_ORIGIN_X);
	o[1] = *reinterpret_cast<const float*>(p + ENT_OFF_ABS_ORIGIN_Y);
	o[2] = *reinterpret_cast<const float*>(p + ENT_OFF_ABS_ORIGIN_Z);
}

// Map-name stamp for once-per-map auto dump.
static char s_slipDumpMapDone[128] = {};

static int16_t SlipDiag_Edict(const void* ent)
{
	if (!ent)
		return -1;
	return *reinterpret_cast<const int16_t*>(
		reinterpret_cast<const uint8_t*>(ent) + ENT_OFF_EDICT);
}

static uint32_t SlipDiag_Handle(const void* ent)
{
	if (!ent)
		return 0xFFFFFFFFu;
	return *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<const uint8_t*>(ent) + ENT_OFF_EHANDLE);
}

static int64_t SlipDiag_PlayerSlipCount(const void* player)
{
	if (!player)
		return -1;
	return *reinterpret_cast<const int64_t*>(
		reinterpret_cast<const uint8_t*>(player) + PLR_OFF_SLIP_COUNT);
}

static bool SlipDiag_IsPlayer(void* ent)
{
	if (!ent)
		return false;
	const uintptr_t* const vtbl = *reinterpret_cast<uintptr_t**>(ent);
	if (!vtbl)
		return false;
	using IsPlayerFn = char(__fastcall*)(void*);
	const IsPlayerFn fn = reinterpret_cast<IsPlayerFn>(vtbl[VTBL_SLOT_ISPLAYER]);
	if (!fn)
		return false;
	return fn(ent) != 0;
}

static void SlipDiag_ReadForce(const void* trigger, float dir[3], float* speed, float* accel)
{
	const uint8_t* const p = reinterpret_cast<const uint8_t*>(trigger);
	dir[0] = *reinterpret_cast<const float*>(p + SLIP_OFF_DEFAULT_DIR + 0);
	dir[1] = *reinterpret_cast<const float*>(p + SLIP_OFF_DEFAULT_DIR + 4);
	dir[2] = *reinterpret_cast<const float*>(p + SLIP_OFF_DEFAULT_DIR + 8);
	*speed = *reinterpret_cast<const float*>(p + SLIP_OFF_SPEED);
	*accel = *reinterpret_cast<const float*>(p + SLIP_OFF_ACCEL);
}

static void SlipDiag_ReadOrigin(const void* player, float o[3])
{
	const uint8_t* const p = reinterpret_cast<const uint8_t*>(player);
	o[0] = *reinterpret_cast<const float*>(p + PLR_OFF_ORIGIN_X);
	o[1] = *reinterpret_cast<const float*>(p + PLR_OFF_ORIGIN_Y);
	o[2] = *reinterpret_cast<const float*>(p + PLR_OFF_ORIGIN_Z);
}

static float SlipDiag_DirLen(const float d[3])
{
	return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

//-----------------------------------------------------------------------------
// Materialize map-baked -1 sentinels (class default) into real force numbers.
//-----------------------------------------------------------------------------
static bool SlipDiag_ReadPlayerSlipDefaults(const void* player, float* outSpeed, float* outAccel)
{
	if (!player || !outSpeed || !outAccel)
		return false;

	const uintptr_t mod = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
	if (!mod)
		return false;

	const uint32_t speedOff = *reinterpret_cast<const uint32_t*>(mod + RVA_FINDER_SLIP_SPEED);
	const uint32_t accelOff = *reinterpret_cast<const uint32_t*>(mod + RVA_FINDER_SLIP_ACCEL);
	if (speedOff == 0xFFFFFFFFu || accelOff == 0xFFFFFFFFu)
		return false;

	const uint64_t settings = *reinterpret_cast<const uint64_t*>(
		reinterpret_cast<const uint8_t*>(player) + PLR_OFF_SETTINGS);
	if (!settings)
		return false;

	const float speed = *reinterpret_cast<const float*>(settings + speedOff);
	const float accel = *reinterpret_cast<const float*>(settings + accelOff);
	// Reject non-finite / still-negative / absurd.
	if (!(speed > 0.f) || !(accel > 0.f) || speed > 100000.f || accel > 100000.f)
		return false;

	*outSpeed = speed;
	*outAccel = accel;
	return true;
}

static int SlipDiag_MaterializeEntityForce(void* ent, float defSpeed, float defAccel, const char* reason)
{
	if (!ent || !sdk_slip_materialize.GetBool())
		return 0;

	float dir[3] = {};
	float speed = 0.f, accel = 0.f;
	SlipDiag_ReadForce(ent, dir, &speed, &accel);

	int wrote = 0;
	uint8_t* const p = reinterpret_cast<uint8_t*>(ent);
	if (speed < 0.f && defSpeed > 0.f)
	{
		*reinterpret_cast<float*>(p + SLIP_OFF_SPEED) = defSpeed;
		wrote |= 1;
	}
	if (accel < 0.f && defAccel > 0.f)
	{
		*reinterpret_cast<float*>(p + SLIP_OFF_ACCEL) = defAccel;
		wrote |= 2;
	}

	if (wrote)
	{
		// Late joiners get the baked force only if the edict re-encodes.
		MarkEntityEdictDirty(ent);

		Warning(eDLL_T::SERVER,
			"[SLIP-MAT] %s class='%s' edict=%d h=0x%08X "
			"speed %.1f->%.1f accel %.1f->%.1f reason=%s\n",
			wrote == 3 ? "speed+accel" : (wrote & 1) ? "speed" : "accel",
			SlipDiag_ServerClassName(ent),
			static_cast<int>(SlipDiag_Edict(ent)),
			SlipDiag_Handle(ent),
			speed, *reinterpret_cast<float*>(p + SLIP_OFF_SPEED),
			accel, *reinterpret_cast<float*>(p + SLIP_OFF_ACCEL),
			reason ? reason : "?");
	}
	return wrote;
}

static int SlipDiag_MaterializeAllSlips(const void* player, const char* reason)
{
	if (!sdk_slip_materialize.GetBool() || !gpGlobals || !gpGlobals->m_pEdicts)
		return 0;

	float defSpeed = 0.f, defAccel = 0.f;
	if (!SlipDiag_ReadPlayerSlipDefaults(player, &defSpeed, &defAccel))
		return 0;

	int n = 0;
	for (int idx = 0; idx < SLIP_MAX_EDICTS; ++idx)
	{
		const uintptr_t ent = static_cast<uintptr_t>(
			gpGlobals->m_pEdicts[idx + SLIP_EDICT_ENT_BASE]);
		if (!ent)
			continue;
		const char* cn = SlipDiag_ServerClassName(reinterpret_cast<const void*>(ent));
		if (!SlipDiag_IsSlipClassName(cn))
			continue;
		if (SlipDiag_MaterializeEntityForce(
				reinterpret_cast<void*>(ent), defSpeed, defAccel, reason))
			++n;
	}
	if (n > 0)
	{
		Msg(eDLL_T::SERVER,
			"[SLIP-MAT] materialized %d slip(s) with class defaults "
			"speed=%.1f accel=%.1f (%s)\n",
			n, defSpeed, defAccel, reason ? reason : "?");
	}
	return n;
}

// Once-per-process cache of whether we already swept after settings resolved.
static bool s_slipMaterializeDone = false;

static void SlipDiag_MaybeMaterializeFromPlayer(const void* player)
{
	if (s_slipMaterializeDone || !player || !sdk_slip_materialize.GetBool())
		return;
	if (SlipDiag_MaterializeAllSlips(player, "first-player-settings") > 0)
		s_slipMaterializeDone = true;
	else
	{
		// Settings finders may not be patched yet; retry next tick until they are.
	}
}

//-----------------------------------------------------------------------------
// Map dump: walk every edict, print every CTriggerSlip / CTriggerSlipSphere
// with abs origin + force fields. Use this to find where slips are on the map.
// ConCommand: sdk_slip_dump. Also auto once per map when diag+auto are on.
//-----------------------------------------------------------------------------
static int SlipDiag_DumpMapSlips(const char* reason)
{
	if (!gpGlobals || !gpGlobals->m_pEdicts)
	{
		Warning(eDLL_T::SERVER, "[SLIP-MAP] no gpGlobals/m_pEdicts (%s)\n",
			reason ? reason : "?");
		return 0;
	}

	const char* mapName = "?";
	if (gpGlobals->mapName.ToCStr() && gpGlobals->mapName.ToCStr()[0])
		mapName = gpGlobals->mapName.ToCStr();

	int found = 0;
	int deadForce = 0;

	Warning(eDLL_T::SERVER,
		"[SLIP-MAP] === BEGIN map='%s' reason=%s ===\n",
		mapName, reason ? reason : "manual");

	for (int idx = 0; idx < SLIP_MAX_EDICTS; ++idx)
	{
		const uintptr_t ent = static_cast<uintptr_t>(
			gpGlobals->m_pEdicts[idx + SLIP_EDICT_ENT_BASE]);
		if (!ent)
			continue;

		const char* cn = "?";
		float dir[3] = {};
		float speed = 0.f, accel = 0.f;
		float origin[3] = {};
		uint32_t handle = 0xFFFFFFFFu;
		int16_t edictField = -1;

		cn = SlipDiag_ServerClassName(reinterpret_cast<const void*>(ent));
		if (!SlipDiag_IsSlipClassName(cn))
			continue;

		SlipDiag_ReadForce(reinterpret_cast<const void*>(ent), dir, &speed, &accel);
		SlipDiag_ReadAbsOrigin(reinterpret_cast<const void*>(ent), origin);
		handle = SlipDiag_Handle(reinterpret_cast<const void*>(ent));
		edictField = SlipDiag_Edict(reinterpret_cast<const void*>(ent));

		const float dirLen = SlipDiag_DirLen(dir);
		const int dead = (speed <= 0.f || accel <= 0.f || dirLen < 0.001f) ? 1 : 0;
		if (dead)
			++deadForce;

		++found;
		Warning(eDLL_T::SERVER,
			"[SLIP-MAP] #%d edict=%d class='%s' h=0x%08X "
			"origin=(%.1f,%.1f,%.1f) dir=(%.3f,%.3f,%.3f) |dir|=%.3f "
			"speed=%.1f accel=%.1f dead=%d\n",
			found, idx, cn, handle,
			origin[0], origin[1], origin[2],
			dir[0], dir[1], dir[2], dirLen,
			speed, accel, dead);
		(void)edictField;
	}

	Warning(eDLL_T::SERVER,
		"[SLIP-MAP] === END map='%s' count=%d deadForce=%d ===\n",
		mapName, found, deadForce);

	// Remember this map so auto-dump does not spam every tick.
	strncpy(s_slipDumpMapDone, mapName, sizeof(s_slipDumpMapDone) - 1);
	s_slipDumpMapDone[sizeof(s_slipDumpMapDone) - 1] = '\0';
	// New map: allow another materialize pass if finders resolve later.
	s_slipMaterializeDone = false;

	return found;
}

static void CC_SlipDump(const CCommand& args)
{
	(void)args;
	SlipDiag_DumpMapSlips("concommand");
}

static ConCommand sdk_slip_dump("sdk_slip_dump", CC_SlipDump,
	"Dump every CTriggerSlip / CTriggerSlipSphere on the current map: edict, "
	"class, abs origin, force dir/speed/accel, dead flag. [SLIP-MAP] -> warning.log",
	FCVAR_DEVELOPMENTONLY);

static void SlipDiag_MaybeAutoDumpMap(void)
{
	if (sdk_slip_diag.GetInt() < 1 || !sdk_slip_dump_auto.GetBool())
		return;
	if (!gpGlobals || !gpGlobals->m_pEdicts)
		return;

	const char* mapName = gpGlobals->mapName.ToCStr();
	if (!mapName || !mapName[0])
		return;
	if (strcmp(s_slipDumpMapDone, mapName) == 0)
		return;

	SlipDiag_DumpMapSlips("auto");
}

//-----------------------------------------------------------------------------
// StartTouch -- enter-slip diagnostic (the primary ask).
// Native path: BaseTrigger StartTouch, then if IsPlayer and
// not already listed, append this trigger's EHANDLE and bump count (cap 16).
//-----------------------------------------------------------------------------
static __int64 __fastcall Hook_CTriggerSlip_StartTouch(void* self, void* other)
{
	const int level = sdk_slip_diag.GetInt();
	if (level < 1 || !self)
		return v_CTriggerSlip__StartTouch(self, other);

	const int64_t countPre = other ? SlipDiag_PlayerSlipCount(other) : -1;
	const bool isPlayer = other ? SlipDiag_IsPlayer(other) : false;

	float dir[3] = {};
	float speed = 0.f, accel = 0.f;
	SlipDiag_ReadForce(self, dir, &speed, &accel);
	const float dirLen = SlipDiag_DirLen(dir);

	float origin[3] = {};
	if (other)
		SlipDiag_ReadOrigin(other, origin);

	// Pre-orig: capture why native may no-op (not player / already listed / full).
	Warning(eDLL_T::SERVER,
		"[SLIP-TOUCH] PRE  trig=%p class='%s' edict=%d h=0x%08X "
		"dir=(%.3f,%.3f,%.3f) |dir|=%.3f speed=%.1f accel=%.1f "
		"other=%p isPlayer=%d slipCount=%lld plrOrigin=(%.1f,%.1f,%.1f)\n",
		self,
		SlipDiag_ServerClassName(self),
		static_cast<int>(SlipDiag_Edict(self)),
		SlipDiag_Handle(self),
		dir[0], dir[1], dir[2], dirLen, speed, accel,
		other, isPlayer ? 1 : 0, static_cast<long long>(countPre),
		origin[0], origin[1], origin[2]);

	// Flag zeroed force fields -- common map-KV miss that yields "no move".
	if (speed <= 0.f || accel <= 0.f || dirLen < 0.001f)
	{
		Warning(eDLL_T::SERVER,
			"[SLIP-TOUCH] WARN force fields look dead: speed=%.1f accel=%.1f |dir|=%.3f "
			"(KeyValue slipSpeed/slipAcceleration and Activate dir-from-link may be unset)\n",
			speed, accel, dirLen);
	}

	const __int64 ret = v_CTriggerSlip__StartTouch(self, other);

	const int64_t countPost = other ? SlipDiag_PlayerSlipCount(other) : -1;
	const char* outcome = "noop";
	if (!other)
		outcome = "other_null";
	else if (!isPlayer)
		outcome = "not_player";
	else if (countPost > countPre)
		outcome = "ADDED";
	else if (countPost == countPre && countPre >= 16)
		outcome = "full_cap16";
	else if (countPost == countPre)
		outcome = "already_listed_or_skip";

	Warning(eDLL_T::SERVER,
		"[SLIP-TOUCH] POST trig=%p other=%p slipCount %lld -> %lld outcome=%s\n",
		self, other,
		static_cast<long long>(countPre),
		static_cast<long long>(countPost),
		outcome);

	return ret;
}

//-----------------------------------------------------------------------------
// EndTouch -- leave-slip diagnostic.
//-----------------------------------------------------------------------------
static __int64 __fastcall Hook_CTriggerSlip_EndTouch(void* self, void* other)
{
	const int level = sdk_slip_diag.GetInt();
	if (level < 1 || !self)
		return v_CTriggerSlip__EndTouch(self, other);

	const int64_t countPre = other ? SlipDiag_PlayerSlipCount(other) : -1;
	const bool isPlayer = other ? SlipDiag_IsPlayer(other) : false;

	float dir[3] = {};
	float speed = 0.f, accel = 0.f;
	SlipDiag_ReadForce(self, dir, &speed, &accel);

	Warning(eDLL_T::SERVER,
		"[SLIP-END] PRE  trig=%p class='%s' edict=%d h=0x%08X "
		"dir=(%.3f,%.3f,%.3f) speed=%.1f accel=%.1f "
		"other=%p isPlayer=%d slipCount=%lld\n",
		self,
		SlipDiag_ServerClassName(self),
		static_cast<int>(SlipDiag_Edict(self)),
		SlipDiag_Handle(self),
		dir[0], dir[1], dir[2], speed, accel,
		other, isPlayer ? 1 : 0, static_cast<long long>(countPre));

	const __int64 ret = v_CTriggerSlip__EndTouch(self, other);

	const int64_t countPost = other ? SlipDiag_PlayerSlipCount(other) : -1;
	Warning(eDLL_T::SERVER,
		"[SLIP-END] POST trig=%p other=%p slipCount %lld -> %lld\n",
		self, other,
		static_cast<long long>(countPre),
		static_cast<long long>(countPost));

	return ret;
}

//-----------------------------------------------------------------------------
// Activate -- sets m_defaultSlipDirection from linked entity.
// After native Activate, try materialize if we already know class defaults
// (player settings may not exist yet at map parse -- FullWalkMove sweeps later).
//-----------------------------------------------------------------------------
static __int64 __fastcall Hook_CTriggerSlip_Activate(void* self)
{
	const __int64 ret = v_CTriggerSlip__Activate(self);
	// No player here during BSP parse; materialize is deferred to FullWalkMove
	// once a player has settings. Activate hook kept for future late-spawned
	// script slips when defaults are already cached.
	return ret;
}

//-----------------------------------------------------------------------------
// FullWalkMove post-sample (level 2). Rate-limited by sdk_slip_diag_force_n.
//-----------------------------------------------------------------------------
void SlipDiag_BeforeFullWalkMove(void* ctx)
{
	if (ctx)
	{
		void* const playerPre = *reinterpret_cast<void**>(
			reinterpret_cast<uintptr_t>(ctx) + GM_CTX_OFF_PLAYER);
		SlipDiag_MaybeMaterializeFromPlayer(playerPre);
	}
	SlipDiag_MaybeAutoDumpMap();
}

void SlipDiag_AfterFullWalkMove(void* ctx)
{
	if (sdk_slip_diag.GetInt() < 2 || !ctx)
		return;

	void* const player = *reinterpret_cast<void**>(
		reinterpret_cast<uintptr_t>(ctx) + GM_CTX_OFF_PLAYER);
	if (!player)
		return;

	const int64_t count = SlipDiag_PlayerSlipCount(player);
	if (count <= 0)
		return;

	static uint32_t s_forceTick = 0;
	const int n = sdk_slip_diag_force_n.GetInt() > 0 ? sdk_slip_diag_force_n.GetInt() : 16;
	if ((++s_forceTick % static_cast<uint32_t>(n)) != 0)
		return;

	int liveHandles = 0;
	const uint32_t* const handles = reinterpret_cast<const uint32_t*>(
		reinterpret_cast<const uint8_t*>(player) + PLR_OFF_SLIP_HANDLES);
	const int64_t walk = count < 16 ? count : 16;
	for (int64_t i = 0; i < walk; ++i)
	{
		if (handles[i] != 0xFFFFFFFFu)
			++liveHandles;
	}

	float forceDir[3] = {};
	if (v_SlipGetForceDir)
		v_SlipGetForceDir(forceDir, player);

	float vx = 0.f, vy = 0.f, vz = 0.f;
	uint8_t* const mv = *reinterpret_cast<uint8_t**>(
		reinterpret_cast<uintptr_t>(ctx) + GM_CTX_OFF_MOVEDATA);
	if (mv)
	{
		vx = *reinterpret_cast<float*>(mv + MV_OFF_VELOCITY + 0);
		vy = *reinterpret_cast<float*>(mv + MV_OFF_VELOCITY + 4);
		vz = *reinterpret_cast<float*>(mv + MV_OFF_VELOCITY + 8);
	}

	float origin[3] = {};
	SlipDiag_ReadOrigin(player, origin);

	Warning(eDLL_T::SERVER,
		"[SLIP-FORCE] plr=%p slipCount=%lld liveH=%d "
		"forceDir=(%.3f,%.3f,%.3f) |dir|=%.3f "
		"vel=(%.1f,%.1f,%.1f) |v|=%.1f origin=(%.1f,%.1f,%.1f)\n",
		player,
		static_cast<long long>(count),
		liveHandles,
		forceDir[0], forceDir[1], forceDir[2],
		SlipDiag_DirLen(forceDir),
		vx, vy, vz,
		sqrtf(vx * vx + vy * vy + vz * vz),
		origin[0], origin[1], origin[2]);
}

//-----------------------------------------------------------------------------
// IDetour
//-----------------------------------------------------------------------------
void VTriggerSlipDiag::GetAdr(void) const
{
	LogFunAdr("CTriggerSlip::StartTouch", v_CTriggerSlip__StartTouch);
	LogFunAdr("CTriggerSlip::EndTouch", v_CTriggerSlip__EndTouch);
	LogFunAdr("CTriggerSlip::Activate", v_CTriggerSlip__Activate);
	LogFunAdr("CGameMovement::SlipGetForceDir", v_SlipGetForceDir);
	LogFunAdr("CGameMovement::FullWalkMove", v_CGameMovement__FullWalkMove);
}

void VTriggerSlipDiag::GetFun(void) const
{

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 57 48 83 EC ?? 48 8B DA 48 8B F9 E8 ?? ?? ?? ?? 48 85 DB 0F 84")
		.GetPtr(v_CTriggerSlip__StartTouch);


	Module_FindPattern(g_GameDll,
		"40 55 41 56 48 83 EC ?? 4C 8B F2 48 8B E9 E8")
		.GetPtr(v_CTriggerSlip__EndTouch);


	// Entry: mov [rsp+10h],rbx; push rsi; sub rsp,20h; mov rbx,rcx; call CFA810;
	// mov rcx,rbx; call E271F0; cmp byte ptr [rbx+0CA8h],1
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 56 48 83 EC ?? 48 8B D9 E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 80 BB")
		.GetPtr(v_CTriggerSlip__Activate);

	// Slip force-dir resolver --. Entry: sub rsp,0D8h; mov

	Module_FindPattern(g_GameDll,
		"48 81 EC ?? ?? ?? ?? 48 8B 82 ?? ?? ?? ?? 4C 8B D1")
		.GetPtr(v_SlipGetForceDir);

	// FullWalkMove -- (parent of AirMove / slip force apply).

	Module_FindPattern(g_GameDll,
		"48 8B C4 55 53 41 57 48 8D 68 ?? 48 81 EC ?? ?? ?? ?? 0F 29 70")
		.GetPtr(v_CGameMovement__FullWalkMove);

	if (!v_CTriggerSlip__StartTouch)
		Warning(eDLL_T::SERVER, "[SLIP-DIAG] CTriggerSlip::StartTouch pattern unresolved -- enter diag dead\n");
	if (!v_CTriggerSlip__EndTouch)
		Warning(eDLL_T::SERVER, "[SLIP-DIAG] CTriggerSlip::EndTouch pattern unresolved -- leave diag dead\n");
	if (!v_CTriggerSlip__Activate)
		Warning(eDLL_T::SERVER, "[SLIP-DIAG] CTriggerSlip::Activate pattern unresolved -- post-activate materialize deferred only\n");
	if (!v_CGameMovement__FullWalkMove)
		Warning(eDLL_T::SERVER, "[SLIP-DIAG] FullWalkMove pattern unresolved -- force sample/materialize dead\n");
	if (!v_SlipGetForceDir)
		Warning(eDLL_T::SERVER, "[SLIP-DIAG] SlipGetForceDir pattern unresolved -- forceDir in [SLIP-FORCE] will be zero\n");
}

void VTriggerSlipDiag::Detour(const bool bAttach) const
{
	if (bAttach && sdk_slip_diag.GetInt() == 0 && !sdk_slip_dump_auto.GetBool()
		&& !sdk_slip_materialize.GetBool())
		return;

	if (v_CTriggerSlip__StartTouch)
		DetourSetup(&v_CTriggerSlip__StartTouch, &Hook_CTriggerSlip_StartTouch, bAttach);
	if (v_CTriggerSlip__EndTouch)
		DetourSetup(&v_CTriggerSlip__EndTouch, &Hook_CTriggerSlip_EndTouch, bAttach);
	if (v_CTriggerSlip__Activate)
		DetourSetup(&v_CTriggerSlip__Activate, &Hook_CTriggerSlip_Activate, bAttach);
}

