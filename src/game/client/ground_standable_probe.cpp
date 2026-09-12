//=============================================================================//
//
// Purpose: client [GROUND-REJECT] probe. CategorizePosition nulls the ground
// entity and applies the push-away-from-top acceleration whenever the traced
// surface fails its standable test; this reports the offending collider and
// the clause that rejected it. Measurement only.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier1/cvar.h"
#include "engine/client/net_bridge_internal.h"   // NetObs_Sym, IsFirstTimePredicted
#include "game/client/ground_standable_probe.h"
#include "game/client/pred_authority.h"   // ENT_ENTINDEX, EntField

//-----------------------------------------------------------------------------
// Raw layout constants -- r5apex client.
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t GR_CTX_OFF_PLAYER      = 8;      // C_Player*
static constexpr ptrdiff_t GR_CTX_OFF_MV          = 16;     // CMoveData*
static constexpr ptrdiff_t GR_MV_OFF_ORIGIN       = 280;    // Vector m_vecAbsOrigin
static constexpr ptrdiff_t GR_ENT_OFF_SOLIDTYPE   = 0x3E4;  // SolidType_t, byte
static constexpr ptrdiff_t GR_PLAYER_OFF_PUSHAWAY = 0x46B0; // Vector m_pushAwayFromTopAcceleration
static constexpr ptrdiff_t GR_VT_GET_ABS_ORIGIN   = 0x40;   // const float* GetAbsOrigin()
static constexpr ptrdiff_t GR_VT_GET_SOLID_FLAGS  = 0x2B0;  // int GetSolidFlags()
static constexpr ptrdiff_t GR_PLAYER_OFF_CURCMD   = 0x34B8; // CUserCmd* m_pCurrentCommand
static constexpr ptrdiff_t GR_CMD_OFF_NUMBER      = 0x00;   // int command_number

// Bit index is the SolidType_t value: {1, 2, 6, 11} stand, everything else
// falls through to the can_stand_on_obb allowance.
static constexpr unsigned int  GR_STANDABLE_MASK = 0x846;
static constexpr unsigned char GR_SOLID_OBB      = 3;

//-----------------------------------------------------------------------------
// Engine function pointers.
//-----------------------------------------------------------------------------
static bool (*v_EntityIsNpcGround)(void* pEnt) = nullptr;
static __int64 (*v_C_GameMovement__CategorizePosition)(void* ctx, char bGrappling) = nullptr;

static ConVar bridge_ground_reject_probe("bridge_ground_reject_probe", "0",
	FCVAR_DEVELOPMENTONLY, "[GROUND-REJECT] report the collider CategorizePosition "
	"refuses to stand on, plus the rejecting clause. One line per distinct "
	"collider unless bridge_ground_reject_probe_repeat is set.");
static ConVar bridge_ground_reject_probe_repeat("bridge_ground_reject_probe_repeat", "0",
	FCVAR_DEVELOPMENTONLY, "[GROUND-REJECT] 1 = emit every occurrence instead of "
	"one line per distinct collider. Per-movement-tick volume.");

//-----------------------------------------------------------------------------
// Distinct-collider memo so a stationary refusal does not flood the log.
//-----------------------------------------------------------------------------
struct GroundRejectSeen_t
{
	uintptr_t     vtable;
	unsigned char solidType;
	const char*   pszReason;
	int           nFirstTime;
};

static GroundRejectSeen_t s_seen[16] = { };
static uint32_t s_nSeen = 0;
static uint32_t s_nNamed = 0;
static uint32_t s_nUnnamed = 0;

//-----------------------------------------------------------------------------
// Command context, stashed by the CategorizePosition hook so the entity hook
// can tag the collider with the command that tripped over it. The standable
// test has a second caller, so the stash is only trusted inside the orig call.
//-----------------------------------------------------------------------------
static bool s_bInCategorizePosition = false;
static int  s_nCurCmd = -1;
static int  s_nCurFirstTime = -1;

static bool GroundReject_FirstSight(const uintptr_t vtable,
	const unsigned char nSolidType, const char* const pszReason,
	const int nFirstTime)
{
	for (uint32_t i = 0; i < s_nSeen && i < ARRAYSIZE(s_seen); ++i)
	{
		if (s_seen[i].vtable == vtable
			&& s_seen[i].solidType == nSolidType
			&& s_seen[i].pszReason == pszReason
			&& s_seen[i].nFirstTime == nFirstTime)
			return false;
	}

	if (s_nSeen < ARRAYSIZE(s_seen))
	{
		s_seen[s_nSeen].vtable = vtable;
		s_seen[s_nSeen].solidType = nSolidType;
		s_seen[s_nSeen].pszReason = pszReason;
		s_seen[s_nSeen].nFirstTime = nFirstTime;
		++s_nSeen;
	}

	return true;
}

// -1 when the engine's flag is unresolved. 0 means a replayed command.
static int GroundReject_ReadFirstTimePredicted(void)
{
	const uintptr_t pFirstPred = NetObs_Sym(NetObsSym_t::IsFirstTimePredicted);
	if (!pFirstPred)
		return -1;

	return *reinterpret_cast<const unsigned char*>(pFirstPred) != 0 ? 1 : 0;
}

static int GroundReject_ReadCmdNumber(void* const pPlayer)
{
	if (!pPlayer)
		return -1;

	const uintptr_t pCmd = *reinterpret_cast<const uintptr_t*>(
		reinterpret_cast<uintptr_t>(pPlayer) + GR_PLAYER_OFF_CURCMD);
	if (!pCmd)
		return -1;

	return *reinterpret_cast<const int*>(pCmd + GR_CMD_OFF_NUMBER);
}

static bool GroundReject_SolidTypeStandable(const unsigned char nSolidType)
{
	if (nSolidType <= 0xB && ((GR_STANDABLE_MASK >> nSolidType) & 1u) != 0)
		return true;

	if (nSolidType != GR_SOLID_OBB)
		return false;

	// Native ConVar, ships at 1. Null means it has not registered yet.
	static ConVar* s_pCanStandOnObb = nullptr;
	if (!s_pCanStandOnObb && g_pCVar)
		s_pCanStandOnObb = g_pCVar->FindVar("can_stand_on_obb");

	return s_pCanStandOnObb && s_pCanStandOnObb->GetBool();
}

static void GroundReject_Inspect(void* const pEnt, const bool bNpcGround)
{
	const uintptr_t ent = reinterpret_cast<uintptr_t>(pEnt);
	const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(ent);
	if (!vtable)
		return;

	const unsigned char nSolidType =
		*reinterpret_cast<const unsigned char*>(ent + GR_ENT_OFF_SOLIDTYPE);

	const char* pszReason = nullptr;
	if (bNpcGround)
		pszReason = "npc-ground";
	else if (!GroundReject_SolidTypeStandable(nSolidType))
		pszReason = "solid-type";

	if (!pszReason)
		return;

	++s_nNamed;

	const int nCmd = s_bInCategorizePosition ? s_nCurCmd : -1;
	const int nFirstTime = s_bInCategorizePosition ? s_nCurFirstTime : -1;

	if (!GroundReject_FirstSight(vtable, nSolidType, pszReason, nFirstTime)
		&& !bridge_ground_reject_probe_repeat.GetBool())
		return;

	using FnGetSolidFlags = int(__fastcall*)(void*);
	using FnGetAbsOrigin = const float*(__fastcall*)(void*);

	const int nSolidFlags =
		(*reinterpret_cast<const FnGetSolidFlags*>(vtable + GR_VT_GET_SOLID_FLAGS))(pEnt);
	const float* const pOrigin =
		(*reinterpret_cast<const FnGetAbsOrigin*>(vtable + GR_VT_GET_ABS_ORIGIN))(pEnt);

	Warning(eDLL_T::CLIENT,
		"[GROUND-REJECT] %s ent=%d solidType=%u solidFlags=0x%X "
		"vt=r5apex+0x%llX entOrigin=(%.2f %.2f %.2f) cmd=%d ftp=%d #%u\n",
		pszReason,
		EntField<int>(pEnt, ENT_ENTINDEX),
		static_cast<unsigned int>(nSolidType),
		nSolidFlags,
		static_cast<unsigned long long>(vtable - g_GameDll.GetModuleBase()),
		pOrigin ? pOrigin[0] : 0.0f,
		pOrigin ? pOrigin[1] : 0.0f,
		pOrigin ? pOrigin[2] : 0.0f,
		nCmd, nFirstTime, s_nNamed);
}

static bool __fastcall Hook_EntityIsNpcGround(void* pEnt)
{
	const bool bNpcGround = v_EntityIsNpcGround ? v_EntityIsNpcGround(pEnt) : false;

	if (pEnt && bridge_ground_reject_probe.GetBool())
		GroundReject_Inspect(pEnt, bNpcGround);

	return bNpcGround;
}

static __int64 __fastcall Hook_C_GameMovement_CategorizePosition(void* ctx, char bGrappling)
{
	const uint32_t nNamedBefore = s_nNamed;
	const bool bProbe = ctx && bridge_ground_reject_probe.GetBool();

	void* const pPlayer = ctx
		? *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(ctx) + GR_CTX_OFF_PLAYER)
		: nullptr;

	if (bProbe)
	{
		s_nCurCmd = GroundReject_ReadCmdNumber(pPlayer);
		s_nCurFirstTime = GroundReject_ReadFirstTimePredicted();
		s_bInCategorizePosition = true;
	}

	const __int64 nResult = v_C_GameMovement__CategorizePosition
		? v_C_GameMovement__CategorizePosition(ctx, bGrappling)
		: 0;

	s_bInCategorizePosition = false;

	if (!bProbe || !pPlayer)
		return nResult;

	// CategorizePosition zeroes the push-away at entry and writes it only in
	// the refuse-to-stand branch, so non-zero means that branch fired here.
	const float* const pPush = reinterpret_cast<const float*>(
		reinterpret_cast<uintptr_t>(pPlayer) + GR_PLAYER_OFF_PUSHAWAY);
	if (pPush[0] == 0.0f && pPush[1] == 0.0f && pPush[2] == 0.0f)
		return nResult;

	if (s_nNamed != nNamedBefore)
		return nResult;

	// The branch fired but the entity hook saw nothing: FSOLID_NOT_STANDABLE
	// (or the titan clause) short-circuits ahead of it, so the collider cannot
	// be read here. Elimination is the finding.
	if (++s_nUnnamed > 12 && (s_nUnnamed & 0xFFu) != 0)
		return nResult;

	void* const pMv =
		*reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(ctx) + GR_CTX_OFF_MV);
	const float* const pMvOrigin = pMv
		? reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(pMv) + GR_MV_OFF_ORIGIN)
		: nullptr;

	Warning(eDLL_T::CLIENT,
		"[GROUND-REJECT] not-standable-flag (collider unreadable, clause "
		"short-circuits) push=(%.2f %.2f %.2f) at=(%.2f %.2f %.2f) "
		"cmd=%d ftp=%d #%u\n",
		pPush[0], pPush[1], pPush[2],
		pMvOrigin ? pMvOrigin[0] : 0.0f,
		pMvOrigin ? pMvOrigin[1] : 0.0f,
		pMvOrigin ? pMvOrigin[2] : 0.0f,
		s_nCurCmd, s_nCurFirstTime, s_nUnnamed);

	return nResult;
}

void VGroundStandableProbe::GetAdr(void) const
{
	LogFunAdr("EntityIsNpcGround", v_EntityIsNpcGround);
	LogFunAdr("C_GameMovement::CategorizePosition", v_C_GameMovement__CategorizePosition);
}

void VGroundStandableProbe::GetFun(void) const
{
	// EntityIsNpcGround(ent). Called only from the two standable tests, with
	// the ground-trace hit entity. Interior anchors are the GetSolidFlags-style
	// vtable call at +0x5C8 and the aiClass index load at ent+0x1DA4. Unique
	// on this client (1 hit).
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 48 8B 01 48 8B D9 FF 90 C8 05 00 00 84 C0 75 06 "
		"48 83 C4 20 5B C3 48 63 83 A4 1D 00 00")
		.GetPtr(v_EntityIsNpcGround);

	if (!v_EntityIsNpcGround)
		Warning(eDLL_T::CLIENT,
			"[GROUND-REJECT] EntityIsNpcGround pattern unresolved -- "
			"colliders stay unnamed\n");

	// C_GameMovement::CategorizePosition(ctx, bGrappling). The prologue stamp
	// of 1.0f at player+0x4060 and the vec3_origin store to the push-away at
	// player+0x46B0 are the anchors; the rip displacement is wildcarded.
	// Unique on this client (1 hit).
	Module_FindPattern(g_GameDll,
		"40 55 53 57 48 8D AC 24 00 FB FF FF 48 81 EC 00 06 00 00 48 8B 41 08 "
		"48 8B F9 0F B6 DA C7 80 60 40 00 00 00 00 80 3F F3 0F 10 05 ?? ?? ?? ?? "
		"48 8B 49 08 F3 0F 11 81 B0 46 00 00")
		.GetPtr(v_C_GameMovement__CategorizePosition);

	if (!v_C_GameMovement__CategorizePosition)
		Warning(eDLL_T::CLIENT,
			"[GROUND-REJECT] CategorizePosition pattern unresolved -- "
			"the not-standable-flag case stays invisible\n");
}

void VGroundStandableProbe::Detour(const bool bAttach) const
{
	if (v_EntityIsNpcGround)
		DetourSetup(&v_EntityIsNpcGround, &Hook_EntityIsNpcGround, bAttach);

	if (v_C_GameMovement__CategorizePosition)
		DetourSetup(&v_C_GameMovement__CategorizePosition,
			&Hook_C_GameMovement_CategorizePosition, bAttach);
}
