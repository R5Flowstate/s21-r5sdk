//=============================================================================//
//
// Purpose: first-raise DRAWFIRST before sprint skip. See bridge_deploy_gate.h.
//
//=============================================================================//
#include "core/stdafx.h"
#include "bridge_deploy_gate.h"
#include "game/server/akimbo.h"


typedef char(__fastcall* PFN_CWeaponX_DeployWeapon)(__int64 weapon, unsigned char skipRaise);
typedef unsigned char(__fastcall* PFN_CBaseEntity_IsPlayer)(__int64 entity);

static PFN_CWeaponX_DeployWeapon v_CWeaponX_DeployWeapon = nullptr;
static __int64* s_pEntList = nullptr;

static ConVar bridge_first_deploy_parity("bridge_first_deploy_parity", "1",
	FCVAR_RELEASE,
	"S21 DeployWeapon order: try ACT_VM_DRAWFIRST before the sprint "
	"DRAW_TO_SPRINT skip. The S3 engine tests m_fIsSprinting first, so a "
	"running first pickup never plays DRAWFIRST and instant-deploys when that "
	"sprint sequence is missing. 0 = legacy S3 order (A/B).");

static constexpr ptrdiff_t WEAPON_OFF_OWNER          = 0x11F0; // m_weaponOwner
static constexpr ptrdiff_t WEAPON_OFF_DIDFIRSTDEPLOY = 0x129B; // m_didFirstDeploy
static constexpr ptrdiff_t PLAYER_OFF_FISSPRINTING   = 0x6D24; // m_fIsSprinting
static constexpr unsigned int ENTINFO_QWORDS         = 6;
static constexpr unsigned int VTBL_ISPLAYER          = 0x2E8 / 8;

//-----------------------------------------------------------------------------
// Purpose: CHandle lookup through the server entity list DeployWeapon itself
// uses (6-qword CEntInfo stride). g_pEntityList is not hooked up on dedi.
//-----------------------------------------------------------------------------
static __int64 DeployGate_GetOwner(__int64 weapon)
{
	if (!weapon || !s_pEntList)
		return 0;

	const unsigned int nHandle = *reinterpret_cast<const unsigned int*>(
		weapon + WEAPON_OFF_OWNER);
	if (nHandle == 0xFFFFFFFFu)
		return 0;

	const unsigned int nIdx = nHandle & 0xFFFFu;
	const unsigned int nSerial = nHandle >> 16;
	if (static_cast<unsigned int>(s_pEntList[ENTINFO_QWORDS * nIdx + 1]) != nSerial)
		return 0;

	return s_pEntList[ENTINFO_QWORDS * nIdx];
}

static bool DeployGate_IsPlayer(__int64 entity)
{
	if (!entity)
		return false;

	const uintptr_t* const pVtbl = *reinterpret_cast<uintptr_t**>(entity);
	if (!pVtbl)
		return false;

	const PFN_CBaseEntity_IsPlayer fn =
		reinterpret_cast<PFN_CBaseEntity_IsPlayer>(pVtbl[VTBL_ISPLAYER]);
	if (!fn)
		return false;

	return fn(entity) != 0;
}

//-----------------------------------------------------------------------------
// Purpose: S21 tries DRAWFIRST before the sprint skip. Neutralise
// m_fIsSprinting for a first raise so S3 takes that same branch.
//-----------------------------------------------------------------------------
static char DeployGate_Deploy(__int64 weapon, unsigned char skipRaise)
{
	void* const pWeapon = reinterpret_cast<void*>(weapon);
	void* const pRedeploy = AkimboBridge_PreDeploy(pWeapon);
	const char result = v_CWeaponX_DeployWeapon(weapon, skipRaise);
	AkimboBridge_PostDeploy(pWeapon, pRedeploy);
	return result;
}

static char __fastcall Hook_CWeaponX_DeployWeapon(__int64 weapon, unsigned char skipRaise)
{
	if (!bridge_first_deploy_parity.GetBool()
		|| !weapon
		|| skipRaise
		|| *reinterpret_cast<unsigned char*>(weapon + WEAPON_OFF_DIDFIRSTDEPLOY))
	{
		return DeployGate_Deploy(weapon, skipRaise);
	}

	const __int64 player = DeployGate_GetOwner(weapon);
	if (!player || !DeployGate_IsPlayer(player))
		return DeployGate_Deploy(weapon, skipRaise);

	unsigned char* const pSprint = reinterpret_cast<unsigned char*>(
		player + PLAYER_OFF_FISSPRINTING);
	if (*pSprint == 0)
		return DeployGate_Deploy(weapon, skipRaise);

	const unsigned char nSaved = *pSprint;
	*pSprint = 0;
	const char result = DeployGate_Deploy(weapon, skipRaise);
	*pSprint = nSaved;
	return result;
}

///////////////////////////////////////////////////////////////////////////////
void VBridgeDeployGate::GetAdr(void) const
{
	LogFunAdr("CWeaponX::DeployWeapon", v_CWeaponX_DeployWeapon);
	LogVarAdr("g_pEntityList (DeployWeapon)", s_pEntList);
}

void VBridgeDeployGate::GetFun(void) const
{
	// CWeaponX::DeployWeapon (server half). Owner load at +0x11F0 and the
	// r12 push separate it from the client twin (owner +0x1300).
	Module_FindPattern(g_GameDll,
		"40 53 56 41 54 48 83 EC 30 48 8B D9 0F B6 F2 "
		"8B 89 F0 11 00 00 8B C1 83 F9 FF")
		.GetPtr(v_CWeaponX_DeployWeapon);

	if (v_CWeaponX_DeployWeapon)
	{
		const CMemory fn(reinterpret_cast<uintptr_t>(v_CWeaponX_DeployWeapon));
		s_pEntList = fn.Offset(0x1F)
			.ResolveRelativeAddress(3, 7)
			.RCast<__int64*>();

		if (!s_pEntList)
		{
			Warning(eDLL_T::SERVER,
				"[FIRST-DEPLOY] entity list unresolved -- first-raise "
				"sprint parity inactive\n");
		}
	}
	else
	{
		Warning(eDLL_T::SERVER,
			"[FIRST-DEPLOY] CWeaponX::DeployWeapon pattern unresolved -- "
			"first-raise sprint parity inactive\n");
	}
}

void VBridgeDeployGate::GetVar(void) const { }
void VBridgeDeployGate::GetCon(void) const { }

void VBridgeDeployGate::Detour(const bool bAttach) const
{
	if (v_CWeaponX_DeployWeapon && s_pEntList)
		DetourSetup(&v_CWeaponX_DeployWeapon, &Hook_CWeaponX_DeployWeapon, bAttach);
}
///////////////////////////////////////////////////////////////////////////////

