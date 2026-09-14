//=============================================================================//
//
// Purpose: client [WALLCLIMB] FullWalkMove tap twin. First-time cmds only.
//
//=============================================================================//
#include "core/stdafx.h"
#include "game/client/wallclimb.h"

#include "tier0/dbg.h"
#include "tier1/cvar.h"

static constexpr ptrdiff_t WC_CTX_OFF_PLAYER = 0x08;
static constexpr ptrdiff_t WC_CTX_OFF_MV = 0x10;
static constexpr ptrdiff_t WC_MV_OFF_ORIGIN = 0x118;
static constexpr ptrdiff_t WC_MV_OFF_VELOCITY = 0x124;

static constexpr ptrdiff_t WC_PLAYER_OFF_SETTINGS = 0x2598;
static constexpr ptrdiff_t WC_PLAYER_OFF_UPDIR = 0x2AB0;
static constexpr ptrdiff_t WC_PLAYER_OFF_UPDIRPRED = 0x2ABC;
static constexpr ptrdiff_t WC_PLAYER_OFF_GROUNDENT = 0x324;
static constexpr ptrdiff_t WC_PLAYER_OFF_CURRENTCOMMAND = 0x34B8;
static constexpr ptrdiff_t WC_CMD_OFF_COMMANDNUMBER = 0;

static constexpr uint32_t WC_RVA_CLIMBENABLED = 0x1832508;
static constexpr uint32_t WC_RVA_WALLRUN = 0x1831738;
static constexpr uint32_t WC_RVA_ACCV = 0x182BA18;
static constexpr uint32_t WC_RVA_MAXV = 0x1726618;
static constexpr uint32_t WC_RVA_DIST = 0x1721D18;
static constexpr uint32_t WC_SETTINGS_OFF_CAP = 0x100000u;

static ConVar bridge_wallclimb_tap("bridge_wallclimb_tap", "0", FCVAR_DEVELOPMENTONLY,
	"[WALLCLIMB] attach/detach + every 8th on-wall FullWalkMove dump. Join on cmd=.");

static int s_nTapHighWater = 0;
static int s_nPrevOn = 0;
static int s_nOnCount = 0;
static bool s_bHasPrev = false;

static uint32_t WallClimb_FinderOff(const uint32_t nRva)
{
	const uintptr_t mod = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
	const uintptr_t nSize = static_cast<uintptr_t>(g_GameDll.GetModuleSize());
	if (!mod || !nSize || nRva + sizeof(uint32_t) > nSize)
		return 0xFFFFFFFFu;
	return *reinterpret_cast<const uint32_t*>(mod + nRva);
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

void WallClimbTap_AfterFullWalkMove(void* ctx)
{
	if (bridge_wallclimb_tap.GetInt() <= 0 || !ctx)
		return;

	uint8_t* const player = *reinterpret_cast<uint8_t**>(
		reinterpret_cast<uint8_t*>(ctx) + WC_CTX_OFF_PLAYER);
	uint8_t* const mv = *reinterpret_cast<uint8_t**>(
		reinterpret_cast<uint8_t*>(ctx) + WC_CTX_OFF_MV);
	if (!player || !mv)
		return;

	const __int64 pCmd = *reinterpret_cast<const __int64*>(player + WC_PLAYER_OFF_CURRENTCOMMAND);
	const int nCmd = pCmd
		? static_cast<int>(*reinterpret_cast<const uint32_t*>(pCmd + WC_CMD_OFF_COMMANDNUMBER))
		: -1;

	if (s_nTapHighWater - nCmd > 100000)
		s_nTapHighWater = 0;
	if (nCmd >= 0 && nCmd <= s_nTapHighWater)
		return;

	const float* const pUp = reinterpret_cast<const float*>(player + WC_PLAYER_OFF_UPDIR);
	const int nOn = (pUp[2] < 0.999f) ? 1 : 0;

	const char* pszEv = nullptr;
	if (!s_bHasPrev)
	{
		if (nOn)
			pszEv = "attach";
	}
	else if (!s_nPrevOn && nOn)
		pszEv = "attach";
	else if (s_nPrevOn && !nOn)
		pszEv = "detach";
	else if (nOn)
	{
		++s_nOnCount;
		if ((s_nOnCount & 7) == 0)
			pszEv = "on";
	}

	s_nPrevOn = nOn;
	s_bHasPrev = true;
	if (nCmd > s_nTapHighWater)
		s_nTapHighWater = nCmd;
	if (!pszEv)
		return;

	const uint8_t* const pSettings = *reinterpret_cast<const uint8_t* const*>(
		player + WC_PLAYER_OFF_SETTINGS);
	const uint32_t nClimbF = WallClimb_FinderOff(WC_RVA_CLIMBENABLED);
	const uint32_t nWrF = WallClimb_FinderOff(WC_RVA_WALLRUN);
	const uint32_t nAccF = WallClimb_FinderOff(WC_RVA_ACCV);
	const uint32_t nMaxF = WallClimb_FinderOff(WC_RVA_MAXV);
	const uint32_t nDistF = WallClimb_FinderOff(WC_RVA_DIST);

	const float* const pUpP = reinterpret_cast<const float*>(player + WC_PLAYER_OFF_UPDIRPRED);
	const float* const pOrg = reinterpret_cast<const float*>(mv + WC_MV_OFF_ORIGIN);
	const float* const pVel = reinterpret_cast<const float*>(mv + WC_MV_OFF_VELOCITY);
	const int nGround = *reinterpret_cast<const int*>(player + WC_PLAYER_OFF_GROUNDENT);

	Warning(eDLL_T::CLIENT,
		"[WALLCLIMB] ev=%s cmd=%d climb=%d/%d wr=%d/%d accV=%.2f/%.2f maxV=%.2f/%.2f "
		"dist=%.2f/%.2f fOff=%u/%u/%u lOff=%u/%u/%u up=%.3f %.3f %.3f upP=%.3f %.3f %.3f "
		"isW=%d o=%.2f %.2f %.2f v=%.2f %.2f %.2f gN=0.00 0.00 0.00 gH=%d dwrIdx=-1\n",
		pszEv, nCmd,
		WallClimb_ReadByte(pSettings, nClimbF), WallClimb_ReadByte(pSettings, nClimbF),
		WallClimb_ReadByte(pSettings, nWrF), WallClimb_ReadByte(pSettings, nWrF),
		WallClimb_ReadFloat(pSettings, nAccF), WallClimb_ReadFloat(pSettings, nAccF),
		WallClimb_ReadFloat(pSettings, nMaxF), WallClimb_ReadFloat(pSettings, nMaxF),
		WallClimb_ReadFloat(pSettings, nDistF), WallClimb_ReadFloat(pSettings, nDistF),
		nAccF, nMaxF, nDistF, nAccF, nMaxF, nDistF,
		pUp[0], pUp[1], pUp[2], pUpP[0], pUpP[1], pUpP[2],
		nOn, pOrg[0], pOrg[1], pOrg[2], pVel[0], pVel[1], pVel[2],
		(nGround != -1) ? 1 : 0);
}
