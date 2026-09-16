//=============================================================================//
//
// Purpose: latest zipline hop-regrab count. Consume on Use grant, refuse
// the next Use at 0. Land or mantle resets. +1 every 3s off the rope.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier1/cvar.h"
#include "zipline_disconnect.h"
#include "game/shared/sdk_entity_state.h"
#include "public/edict.h"

extern CGlobalVars* gpGlobals;

static ConVar bridge_zip_disc("bridge_zip_disc", "1", FCVAR_RELEASE,
	"Latest zipline hop-regrab count. 3 successful Use grants then remount "
	"refused. Land or mantle resets. 3s off the rope gives one back.");

static ConVar bridge_zip_disc_diag("bridge_zip_disc_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[ZIP-DISC] Log refuse / consume / reset / +1.");

static constexpr int ZIP_DISC_MAX = 3;
static constexpr float ZIP_DISC_REFRESH = 3.0f;
static constexpr ptrdiff_t ZIP_DISC_OFF_CURRENTCOMMAND = 25976;

struct ZipDisc_t
{
	int nLeft;
	float flOffAt;
	bool bWasOn;
	bool bWasOnGround;
	bool bInited;
};

static SDKEntityMap<ZipDisc_t> s_zipDisc(ESide::Server, "zipDisc.srv");

static uint64_t s_nRefusals = 0;
static uint64_t s_nConsumes = 0;
static uint64_t s_nResets = 0;
static uint64_t s_nRefresh = 0;

static ZipDisc_t& ZipDisc_State(void* player)
{
	ZipDisc_t& st = s_zipDisc[player];
	if (!st.bInited)
	{
		st.nLeft = ZIP_DISC_MAX;
		st.flOffAt = 0.0f;
		st.bWasOn = false;
		st.bWasOnGround = false;
		st.bInited = true;
	}
	return st;
}

static void ZipDisc_Reset(ZipDisc_t& st, const char* pszWhy, void* player)
{
	if (st.nLeft == ZIP_DISC_MAX && st.flOffAt == 0.0f)
		return;

	st.nLeft = ZIP_DISC_MAX;
	st.flOffAt = 0.0f;
	++s_nResets;

	if (bridge_zip_disc_diag.GetBool() || s_nResets <= 8)
		Msg(eDLL_T::SERVER, "[ZIP-DISC] reset %s player=%p left=%d resets=%llu\n",
			pszWhy, player, st.nLeft, s_nResets);
}

bool ZipDisc_ShouldRefuseMount(void* player)
{
	if (!bridge_zip_disc.GetBool() || !player)
		return false;

	const ZipDisc_t& st = ZipDisc_State(player);
	if (st.nLeft >= 1)
		return false;

	++s_nRefusals;
	if (bridge_zip_disc_diag.GetBool() || s_nRefusals <= 16)
		Msg(eDLL_T::SERVER, "[ZIP-DISC] refuse player=%p left=0 refusals=%llu\n",
			player, s_nRefusals);
	return true;
}

void ZipDisc_OnMountGranted(void* player)
{
	if (!bridge_zip_disc.GetBool() || !player)
		return;

	ZipDisc_t& st = ZipDisc_State(player);
	if (st.nLeft > 0)
		--st.nLeft;
	++s_nConsumes;

	int nCmd = -1;
	void* const pCmd = *reinterpret_cast<void**>(
		reinterpret_cast<uint8_t*>(player) + ZIP_DISC_OFF_CURRENTCOMMAND);
	if (pCmd)
		nCmd = *reinterpret_cast<const int*>(pCmd);

	if (bridge_zip_disc_diag.GetBool() || s_nConsumes <= 16)
		Msg(eDLL_T::SERVER, "[ZIP-DISC] consume player=%p left=%d cmd=%d consumes=%llu\n",
			player, st.nLeft, nCmd, s_nConsumes);
}

void ZipDisc_OnCommand(void* player, const bool bOnGround, const bool bZiplining)
{
	if (!bridge_zip_disc.GetBool() || !player || !gpGlobals)
		return;

	ZipDisc_t& st = ZipDisc_State(player);
	const float flNow = gpGlobals->curTime;

	if (st.bWasOn && !bZiplining)
		st.flOffAt = flNow;

	if (!bZiplining && st.nLeft < ZIP_DISC_MAX && st.flOffAt > 0.0f
		&& (flNow - st.flOffAt) >= ZIP_DISC_REFRESH)
	{
		++st.nLeft;
		st.flOffAt = flNow;
		++s_nRefresh;
		if (bridge_zip_disc_diag.GetBool() || s_nRefresh <= 16)
			Msg(eDLL_T::SERVER, "[ZIP-DISC] +1 player=%p left=%d refresh=%llu\n",
				player, st.nLeft, s_nRefresh);
	}

	const bool bLanded = bOnGround && !st.bWasOnGround;
	st.bWasOnGround = bOnGround;
	st.bWasOn = bZiplining;

	if (bLanded && !bZiplining)
		ZipDisc_Reset(st, "ground", player);
}

void ZipDisc_OnMantle(void* player)
{
	if (!bridge_zip_disc.GetBool() || !player)
		return;

	ZipDisc_Reset(ZipDisc_State(player), "mantle", player);
}
