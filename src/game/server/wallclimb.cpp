//=============================================================================//
//
// Purpose: Remap wallrun/climb SettingsFieldFinder offsets from the live
// S21 player layout, bind disable_wall_run onto the native type slot, and
// emit [WALLCLIMB] attach/detach/on-wall taps.
//
//=============================================================================//
#include "core/stdafx.h"
#include "wallclimb.h"

#include "tier0/dbg.h"
#include "tier1/cvar.h"
#include "tier1/strtools.h"
#include "engine/server/snapshot_diag.h"
#include "game/shared/dt_extend.h"

//-----------------------------------------------------------------------------
// CGameMovement ctx / CMoveData / CPlayer -- server half.
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t WC_CTX_OFF_PLAYER = 0x08;
static constexpr ptrdiff_t WC_CTX_OFF_MV = 0x10;
static constexpr ptrdiff_t WC_MV_OFF_ORIGIN = 0x124;
static constexpr ptrdiff_t WC_MV_OFF_VELOCITY = 0x130;

static constexpr ptrdiff_t WC_PLAYER_OFF_SETTINGS = 0x5F08;
static constexpr ptrdiff_t WC_PLAYER_OFF_UPDIR = 0x662C;
static constexpr ptrdiff_t WC_PLAYER_OFF_UPDIRPRED = 0x6638;
static constexpr ptrdiff_t WC_PLAYER_OFF_GROUNDENT = 0x3C4;
static constexpr ptrdiff_t WC_PLAYER_OFF_CURRENTCOMMAND = 0x6578;
static constexpr ptrdiff_t WC_PLAYER_OFF_EMBEDDEDCMD = 0x639C;
static constexpr ptrdiff_t WC_CMD_OFF_COMMANDNUMBER = 0;

static constexpr uint32_t WC_FINDER_LIST_HEAD_RVA = 0x2385C18;
static constexpr uint32_t WC_DISABLE_WALLRUN_IDX_RVA = 0x2951AF34;
static constexpr uint32_t WC_SETTINGS_OFF_CAP = 0x100000u;

struct WallClimbFinder_t
{
	const char* pszName;
	uint32_t nRva;
};

static const WallClimbFinder_t s_wallClimbFinders[] =
{
	{ "climbEnabled", 0x2387A80 },
	{ "wallrun", 0x2387020 },
	{ "wallrunDuckCausesFallOff", 0x2387170 },
	{ "verticalGainCutoff_wallrun", 0x2387AE0 },
	{ "climbHeight", 0x2386BA8 },
	{ "wallrunAdsType", 0x23872F0 },
	{ "wallrunAllowedWallDistanceWallHang", 0x2386D28 },
	{ "wallrun_hangTimeLimit", 0x2386BC0 },
	{ "wallrun_timeLimit", 0x2387230 },
	{ "wallrunAllowedWallDistance", 0x23867B8 },
	{ "climbFinalJumpUpHeight", 0x23877C8 },
	{ "climbSpeedStart", 0x23869C8 },
	{ "wallrunCeilingLimit", 0x2387630 },
	{ "wallrunSameWallHeight", 0x2386980 },
	{ "wallrunSameWallAllowed", 0x2386D88 },
	{ "wallstickEnabled", 0x2387200 },
	{ "wallrunAccelerateVertical", 0x2386998 },
	{ "wallrunMaxSpeedVertical", 0x2387008 },
	{ "wallrunUpWallBoost", 0x2386D40 },
};

static ConVar bridge_wallclimb_settings("bridge_wallclimb_settings", "1", FCVAR_RELEASE,
	"Write live player-layout offsets into the wallrun/climb SettingsFieldFinder "
	"leaves. 0 = leave the S3-registered dwords.");

static ConVar bridge_wallclimb_disable_effect("bridge_wallclimb_disable_effect", "1", FCVAR_RELEASE,
	"Bind the native disable-wall-run type slot to disable_wall_run (S21 script name). "
	"0 = leave the missing disable_wall_run_and_double_jump lookup.");

static ConVar bridge_wallclimb_tap("bridge_wallclimb_tap", "0", FCVAR_DEVELOPMENTONLY,
	"[WALLCLIMB] attach/detach + every 8th on-wall FullWalkMove dump. Join on cmd=.");

static int (*v_StatusEffect_LookupType)(const char* pszName) = nullptr;
static __int64 (*v_StatusEffectTypes_Load)(void) = nullptr;

static bool WallClimb_IsFamilyName(const char* pszName)
{
	if (!pszName || !pszName[0])
		return false;
	if (!_strnicmp(pszName, "climb", 5))
		return true;
	if (!_strnicmp(pszName, "wallrun", 7))
		return true;
	if (!_strnicmp(pszName, "wallstick", 9))
		return true;
	if (V_stristr(pszName, "wallHang") || V_stristr(pszName, "wall_Hang"))
		return true;
	if (!_stricmp(pszName, "verticalGainCutoff_wallrun"))
		return true;
	return false;
}

static uint32_t* WallClimb_FinderDword(const uintptr_t mod, const uint32_t nRva)
{
	if (!mod || !nRva)
		return nullptr;
	const uintptr_t nSize = static_cast<uintptr_t>(g_GameDll.GetModuleSize());
	if (!nSize || nRva + sizeof(uint32_t) > nSize)
		return nullptr;
	return reinterpret_cast<uint32_t*>(mod + nRva);
}

static bool WallClimb_WriteFinder(const uintptr_t mod, const char* pszName, const uint32_t nRva,
	int* pnWrote)
{
	const uint32_t nLive = Bridge_LookupPlayerSettingsFieldOffset(pszName);
	if (nLive == 0xFFFFFFFFu || nLive >= WC_SETTINGS_OFF_CAP)
		return false;

	uint32_t* const pOff = WallClimb_FinderDword(mod, nRva);
	if (!pOff)
		return false;

	const uint32_t nPrev = *pOff;
	if (nPrev == nLive)
		return true;

	*pOff = nLive;
	if (pnWrote)
		++(*pnWrote);
	Warning(eDLL_T::SERVER, "[WALLCLIMB] finder '%s' %d -> %d\n",
		pszName, static_cast<int>(nPrev), static_cast<int>(nLive));
	return true;
}

static void WallClimb_ApplySettingsFinders(void)
{
	if (!bridge_wallclimb_settings.GetBool())
		return;
	if (!Bridge_HasPlayerSettingsLayout())
		return;

	const uintptr_t mod = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
	const uintptr_t nSize = static_cast<uintptr_t>(g_GameDll.GetModuleSize());
	if (!mod || !nSize)
		return;

	int nWrote = 0;
	for (const WallClimbFinder_t& row : s_wallClimbFinders)
		WallClimb_WriteFinder(mod, row.pszName, row.nRva, &nWrote);

	static bool s_extrasWalked = false;
	if (WC_FINDER_LIST_HEAD_RVA + sizeof(uint64_t) <= nSize)
	{
		uint64_t node = *reinterpret_cast<uint64_t*>(mod + WC_FINDER_LIST_HEAD_RVA);
		if (!s_extrasWalked && node)
		{
			int nWalk = 0;
			while (node && nWalk++ < 2000)
			{
				if (node < mod || node + 0x18 > mod + nSize)
					break;
				const char* const pszName = *reinterpret_cast<const char**>(node + 8);
				if (pszName && WallClimb_IsFamilyName(pszName))
				{
					const uint32_t nRva = static_cast<uint32_t>(node - mod);
					bool bKnown = false;
					for (const WallClimbFinder_t& row : s_wallClimbFinders)
					{
						if (row.nRva == nRva)
						{
							bKnown = true;
							break;
						}
					}
					if (!bKnown)
						WallClimb_WriteFinder(mod, pszName, nRva, &nWrote);
				}
				node = *reinterpret_cast<uint64_t*>(node + 0x10);
			}
			s_extrasWalked = true;
		}
	}

	if (nWrote > 0)
		Warning(eDLL_T::SERVER, "[WALLCLIMB] remapped %d wallrun/climb finders from the live player layout\n", nWrote);
}

static void WallClimb_BindDisableWallRun(void)
{
	if (!bridge_wallclimb_disable_effect.GetBool())
		return;
	if (!v_StatusEffect_LookupType)
		return;

	const uintptr_t mod = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
	if (!mod)
		return;

	int* const pIdx = reinterpret_cast<int*>(WallClimb_FinderDword(mod, WC_DISABLE_WALLRUN_IDX_RVA));
	if (!pIdx)
		return;
	const int nLive = v_StatusEffect_LookupType("disable_wall_run");
	if (nLive < 0)
	{
		static bool s_warned = false;
		if (!s_warned)
		{
			s_warned = true;
			Warning(eDLL_T::SERVER,
				"[WALLCLIMB] disable_wall_run missing from status_effect_types.txt\n");
		}
		return;
	}

	if (*pIdx == nLive)
		return;

	const int nPrev = *pIdx;
	*pIdx = nLive;
	Warning(eDLL_T::SERVER, "[WALLCLIMB] disable_wall_run idx %d -> %d\n", nPrev, nLive);
}

static __int64 Hook_StatusEffectTypes_Load(void)
{
	const __int64 ret = v_StatusEffectTypes_Load ? v_StatusEffectTypes_Load() : 0;
	WallClimb_BindDisableWallRun();
	return ret;
}

//-----------------------------------------------------------------------------
// Tap
//-----------------------------------------------------------------------------
static constexpr int WC_MAX_TRACKED = 64;

struct WallClimbTapSlot_t
{
	const void* pPlayer;
	int nPrevOn;
	int nOnCount;
	bool bHasPrev;
};

static WallClimbTapSlot_t s_tapSlots[WC_MAX_TRACKED] = { };

static WallClimbTapSlot_t& WallClimb_TapSlot(const void* const pPlayer)
{
	int nFree = -1;
	for (int i = 0; i < WC_MAX_TRACKED; ++i)
	{
		if (s_tapSlots[i].pPlayer == pPlayer)
			return s_tapSlots[i];
		if (nFree < 0 && !s_tapSlots[i].pPlayer)
			nFree = i;
	}
	const int nSlot = nFree < 0 ? 0 : nFree;
	s_tapSlots[nSlot].pPlayer = pPlayer;
	s_tapSlots[nSlot].nPrevOn = 0;
	s_tapSlots[nSlot].nOnCount = 0;
	s_tapSlots[nSlot].bHasPrev = false;
	return s_tapSlots[nSlot];
}

static uint8_t* WallClimb_Player(void* const ctx)
{
	return ctx ? *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(ctx) + WC_CTX_OFF_PLAYER) : nullptr;
}

static uint8_t* WallClimb_MoveData(void* const ctx)
{
	return ctx ? *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(ctx) + WC_CTX_OFF_MV) : nullptr;
}

static int WallClimb_ReadCmdNumber(const uint8_t* const player)
{
	if (!player)
		return -1;
	const __int64 pCmd = *reinterpret_cast<const __int64*>(player + WC_PLAYER_OFF_CURRENTCOMMAND);
	if (pCmd)
		return static_cast<int>(*reinterpret_cast<const uint32_t*>(pCmd + WC_CMD_OFF_COMMANDNUMBER));
	return static_cast<int>(*reinterpret_cast<const uint32_t*>(player + WC_PLAYER_OFF_EMBEDDEDCMD + WC_CMD_OFF_COMMANDNUMBER));
}

static const uint8_t* WallClimb_Settings(const uint8_t* const player)
{
	if (!player)
		return nullptr;
	return *reinterpret_cast<const uint8_t* const*>(player + WC_PLAYER_OFF_SETTINGS);
}

static int WallClimb_ReadByte(const uint8_t* const pSettings, const uint32_t nOff)
{
	if (!pSettings || nOff >= WC_SETTINGS_OFF_CAP)
		return -1;
	return pSettings[nOff];
}

static float WallClimb_ReadFloat(const uint8_t* const pSettings, const uint32_t nOff)
{
	if (!pSettings || nOff + 4 > WC_SETTINGS_OFF_CAP)
		return -1.0f;
	return *reinterpret_cast<const float*>(pSettings + nOff);
}

static uint32_t WallClimb_FinderOff(const uintptr_t mod, const uint32_t nRva)
{
	const uint32_t* const pOff = WallClimb_FinderDword(mod, nRva);
	return pOff ? *pOff : 0xFFFFFFFFu;
}

void WallClimb_BeforeFullWalkMove(void* ctx)
{
	(void)ctx;
	WallClimb_ApplySettingsFinders();
	WallClimb_BindDisableWallRun();
}

void WallClimb_AfterFullWalkMove(void* ctx)
{
	if (bridge_wallclimb_tap.GetInt() <= 0 || !ctx)
		return;

	uint8_t* const player = WallClimb_Player(ctx);
	uint8_t* const mv = WallClimb_MoveData(ctx);
	if (!player || !mv)
		return;

	const float* const pUp = reinterpret_cast<const float*>(player + WC_PLAYER_OFF_UPDIR);
	const int nOn = (pUp[2] < 0.999f) ? 1 : 0;
	WallClimbTapSlot_t& slot = WallClimb_TapSlot(player);

	const char* pszEv = nullptr;
	if (!slot.bHasPrev)
	{
		if (nOn)
			pszEv = "attach";
	}
	else if (!slot.nPrevOn && nOn)
		pszEv = "attach";
	else if (slot.nPrevOn && !nOn)
		pszEv = "detach";
	else if (nOn)
	{
		++slot.nOnCount;
		if ((slot.nOnCount & 7) == 0)
			pszEv = "on";
	}

	slot.nPrevOn = nOn;
	slot.bHasPrev = true;
	if (!pszEv)
		return;

	const uintptr_t mod = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
	const uint8_t* const pSettings = WallClimb_Settings(player);
	const uint32_t nClimbF = WallClimb_FinderOff(mod, 0x2387A80);
	const uint32_t nWrF = WallClimb_FinderOff(mod, 0x2387020);
	const uint32_t nAccF = WallClimb_FinderOff(mod, 0x2386998);
	const uint32_t nMaxF = WallClimb_FinderOff(mod, 0x2387008);
	const uint32_t nDistF = WallClimb_FinderOff(mod, 0x23867B8);
	const uint32_t nClimbL = Bridge_LookupPlayerSettingsFieldOffset("climbEnabled");
	const uint32_t nWrL = Bridge_LookupPlayerSettingsFieldOffset("wallrun");
	const uint32_t nAccL = Bridge_LookupPlayerSettingsFieldOffset("wallrunAccelerateVertical");
	const uint32_t nMaxL = Bridge_LookupPlayerSettingsFieldOffset("wallrunMaxSpeedVertical");
	const uint32_t nDistL = Bridge_LookupPlayerSettingsFieldOffset("wallrunAllowedWallDistance");

	const float* const pUpP = reinterpret_cast<const float*>(player + WC_PLAYER_OFF_UPDIRPRED);
	const float* const pOrg = reinterpret_cast<const float*>(mv + WC_MV_OFF_ORIGIN);
	const float* const pVel = reinterpret_cast<const float*>(mv + WC_MV_OFF_VELOCITY);
	const int nGround = *reinterpret_cast<const int*>(player + WC_PLAYER_OFF_GROUNDENT);
	int nGnormOff = DTExtend_FindNativePropOffset(player, "m_groundNormal");
	float flGNx = 0.0f, flGNy = 0.0f, flGNz = 0.0f;
	if (nGnormOff > 0)
	{
		const float* const pGn = reinterpret_cast<const float*>(player + nGnormOff);
		flGNx = pGn[0];
		flGNy = pGn[1];
		flGNz = pGn[2];
	}

	int nDwrIdx = -1;
	if (const int* const pDwr = reinterpret_cast<const int*>(
		WallClimb_FinderDword(mod, WC_DISABLE_WALLRUN_IDX_RVA)))
		nDwrIdx = *pDwr;

	Warning(eDLL_T::SERVER,
		"[WALLCLIMB] ev=%s cmd=%d climb=%d/%d wr=%d/%d accV=%.2f/%.2f maxV=%.2f/%.2f "
		"dist=%.2f/%.2f fOff=%u/%u/%u lOff=%u/%u/%u up=%.3f %.3f %.3f upP=%.3f %.3f %.3f "
		"isW=%d o=%.2f %.2f %.2f v=%.2f %.2f %.2f gN=%.2f %.2f %.2f gH=%d dwrIdx=%d\n",
		pszEv,
		WallClimb_ReadCmdNumber(player),
		WallClimb_ReadByte(pSettings, nClimbF), WallClimb_ReadByte(pSettings, nClimbL),
		WallClimb_ReadByte(pSettings, nWrF), WallClimb_ReadByte(pSettings, nWrL),
		WallClimb_ReadFloat(pSettings, nAccF), WallClimb_ReadFloat(pSettings, nAccL),
		WallClimb_ReadFloat(pSettings, nMaxF), WallClimb_ReadFloat(pSettings, nMaxL),
		WallClimb_ReadFloat(pSettings, nDistF), WallClimb_ReadFloat(pSettings, nDistL),
		nAccF, nMaxF, nDistF, nAccL, nMaxL, nDistL,
		pUp[0], pUp[1], pUp[2], pUpP[0], pUpP[1], pUpP[2],
		nOn, pOrg[0], pOrg[1], pOrg[2], pVel[0], pVel[1], pVel[2],
		flGNx, flGNy, flGNz, (nGround != -1) ? 1 : 0, nDwrIdx);
}

void VWallClimb::GetAdr(void) const
{
	LogFunAdr("StatusEffectTypes_Load", v_StatusEffectTypes_Load);
	LogFunAdr("StatusEffect_LookupType", v_StatusEffect_LookupType);
}

void VWallClimb::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"4C 8B DC 48 81 EC ?? ?? ?? ?? 83 A4 24 ?? ?? ?? ?? ?? "
		"48 8D 05 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 4C 8D 4C 24 ?? 49 89 73")
		.GetPtr(v_StatusEffectTypes_Load);
	if (!v_StatusEffectTypes_Load)
		Warning(eDLL_T::SERVER, "[WALLCLIMB] StatusEffectTypes_Load pattern unresolved\n");

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 44 8B 1D ?? ?? ?? ?? 45 33 C9 48 8B D9 45 85 DB 7E ?? "
		"4C 8D 15 ?? ?? ?? ?? 66 90 49 8B 12")
		.GetPtr(v_StatusEffect_LookupType);
	if (!v_StatusEffect_LookupType)
		Warning(eDLL_T::SERVER, "[WALLCLIMB] StatusEffect_LookupType pattern unresolved\n");
}

void VWallClimb::Detour(const bool bAttach) const
{
	if (v_StatusEffectTypes_Load)
		DetourSetup(&v_StatusEffectTypes_Load, &Hook_StatusEffectTypes_Load, bAttach);
}
