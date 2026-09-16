//=============================================================================//
//
// Purpose: client twin of the latest zipline hop-regrab count.
// Use is predicted -- this side refuses in step with the dedi.
// JumpOff is predicted -- this side calls native so the return matches.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier1/cvar.h"
#include "engine/client/net_bridge_internal.h"
#include "game/client/zipline_disconnect.h"

static ConVar bridge_zip_disc("bridge_zip_disc", "1", FCVAR_RELEASE,
	"Latest zipline hop-regrab count. 3 successful Use grants then remount "
	"refused. Land or mantle resets. 3s off the rope gives one back.");

static ConVar bridge_zip_disc_diag("bridge_zip_disc_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[ZIP-DISC] Log refuse / consume / reset / +1. [ZIP-JUMPOFF] result lines. "
	"[ZIPRAIL-USE] mount result.");

static constexpr int ZIP_DISC_MAX = 3;
static constexpr float ZIP_DISC_REFRESH = 3.0f;
static constexpr ptrdiff_t ZIP_DISC_GLOBALS_CURTIME = 0x10;
static constexpr ptrdiff_t ZIP_DISC_OFF_CURRENTCOMMAND = 0x34B8;
static constexpr ptrdiff_t ZIP_DISC_OFF_PLAYERFLAGS = 10756;
static constexpr ptrdiff_t ZIP_DISC_OFF_ACTIVEZIP = 12008;
static constexpr ptrdiff_t ZIP_DISC_OFF_LASTDETACH = 12016;
static constexpr ptrdiff_t ZIP_DISC_OFF_ZIPLINESTATE = 12024;
static constexpr ptrdiff_t ZIP_DISC_OFF_COOLDOWN = 12192;
static constexpr ptrdiff_t ZIP_DISC_OFF_WEAPDISABLED = 6530;
static constexpr ptrdiff_t ZIP_DISC_OFF_CONTEXTACTION = 6576;
static constexpr ptrdiff_t ZIP_DISC_OFF_FORCESTANCE = 7672;
static constexpr ptrdiff_t ZIP_DISC_OFF_GATE_B = 18488;
static constexpr ptrdiff_t ZIP_DISC_OFF_ZIP_TYPE = 2956;

struct ZipDisc_t
{
	int nLeft;
	int nLastConsumeCmd;
	float flOffAt;
	bool bWasOn;
	bool bWasOnGround;
	bool bInited;
};

static ZipDisc_t s_local = {};
static uint32_t s_localHandle = 0;

static uint64_t s_nRefusals = 0;
static uint64_t s_nConsumes = 0;
static uint64_t s_nResets = 0;
static uint64_t s_nRefresh = 0;

static bool (*v_Zipline_Use)(uintptr_t player, bool forGrappleZipline) = nullptr;
static char (*v_Zipline_JumpOff)(void* zip, void* player, float* velocity,
	const void* eyeAngles, float flForwardMove, float flSideMove) = nullptr;
static uint64_t s_nJumpOffCalls = 0;
static uint64_t s_nUseLogs = 0;

static float ZipDisc_CurTime(void)
{
	const uintptr_t pGlobalsAddr = NetObs_Sym(NetObsSym_t::GlobalVarsPtr);
	if (!pGlobalsAddr)
		return 0.0f;
	const uintptr_t pGlobals = *reinterpret_cast<const uintptr_t*>(pGlobalsAddr);
	return pGlobals
		? *reinterpret_cast<const float*>(pGlobals + ZIP_DISC_GLOBALS_CURTIME)
		: 0.0f;
}

static ZipDisc_t& ZipDisc_State(void* player)
{
	const uint32_t nHandle = player
		? *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(player) + 8)
		: 0;
	if (nHandle != s_localHandle || !s_local.bInited)
	{
		s_localHandle = nHandle;
		s_local = {};
		s_local.nLeft = ZIP_DISC_MAX;
		s_local.nLastConsumeCmd = -1;
		s_local.bInited = true;
	}
	return s_local;
}

static void ZipDisc_Reset(ZipDisc_t& st, const char* pszWhy, void* player)
{
	if (st.nLeft == ZIP_DISC_MAX && st.flOffAt == 0.0f)
		return;

	st.nLeft = ZIP_DISC_MAX;
	st.nLastConsumeCmd = -1;
	st.flOffAt = 0.0f;
	++s_nResets;

	if (bridge_zip_disc_diag.GetBool() || s_nResets <= 8)
		Msg(eDLL_T::CLIENT, "[ZIP-DISC] reset %s player=%p left=%d resets=%llu\n",
			pszWhy, player, st.nLeft, s_nResets);
}

static int ZipDisc_CmdNumber(void* player)
{
	if (!player)
		return -1;
	void* const pCmd = *reinterpret_cast<void**>(
		reinterpret_cast<uint8_t*>(player) + ZIP_DISC_OFF_CURRENTCOMMAND);
	if (!pCmd)
		return -1;
	return *reinterpret_cast<const int*>(pCmd);
}

bool ZipDisc_ShouldRefuseMount(void* player)
{
	if (!bridge_zip_disc.GetBool() || !player)
		return false;

	const ZipDisc_t& st = ZipDisc_State(player);
	int nLeft = st.nLeft;
	const int nCmd = ZipDisc_CmdNumber(player);
	if (nCmd >= 0 && nCmd == st.nLastConsumeCmd)
		++nLeft;

	if (nLeft >= 1)
		return false;

	++s_nRefusals;
	if (bridge_zip_disc_diag.GetBool() || s_nRefusals <= 16)
		Msg(eDLL_T::CLIENT, "[ZIP-DISC] refuse player=%p left=0 refusals=%llu\n",
			player, s_nRefusals);
	return true;
}

void ZipDisc_OnMountGranted(void* player)
{
	if (!bridge_zip_disc.GetBool() || !player)
		return;

	ZipDisc_t& st = ZipDisc_State(player);
	const int nCmd = ZipDisc_CmdNumber(player);
	if (nCmd >= 0 && nCmd == st.nLastConsumeCmd)
		return;

	if (st.nLeft > 0)
		--st.nLeft;
	st.nLastConsumeCmd = nCmd;
	++s_nConsumes;
	if (bridge_zip_disc_diag.GetBool() || s_nConsumes <= 16)
		Msg(eDLL_T::CLIENT, "[ZIP-DISC] consume player=%p left=%d cmd=%d consumes=%llu\n",
			player, st.nLeft, nCmd, s_nConsumes);
}

void ZipDisc_OnCommand(void* player, const bool bOnGround, const bool bZiplining)
{
	if (!bridge_zip_disc.GetBool() || !player || !v_Zipline_Use)
		return;

	ZipDisc_t& st = ZipDisc_State(player);
	const float flNow = ZipDisc_CurTime();

	if (st.bWasOn && !bZiplining)
		st.flOffAt = flNow;

	if (!bZiplining && st.nLeft < ZIP_DISC_MAX && st.flOffAt > 0.0f
		&& (flNow - st.flOffAt) >= ZIP_DISC_REFRESH)
	{
		++st.nLeft;
		st.flOffAt = flNow;
		++s_nRefresh;
		if (bridge_zip_disc_diag.GetBool() || s_nRefresh <= 16)
			Msg(eDLL_T::CLIENT, "[ZIP-DISC] +1 player=%p left=%d refresh=%llu\n",
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

static void ZipDisc_LogUse(uintptr_t player, bool forGrappleZipline, bool result,
	const char* pszWhy, int nStateBefore, unsigned nHandleBefore)
{
	++s_nUseLogs;
	if (!result && !bridge_zip_disc_diag.GetBool() && s_nUseLogs > 16)
		return;

	int nState = nStateBefore;
	unsigned nHandle = nHandleBefore;
	int nWdf = 0;
	int nCtx = 0;
	int nForce = 0;
	int nGateB = 0;
	float flCd = 0.0f;
	float flDetach = 0.0f;
	if (player)
	{
		const uint8_t* const p = reinterpret_cast<const uint8_t*>(player);
		nState = *reinterpret_cast<const int*>(p + ZIP_DISC_OFF_ZIPLINESTATE);
		nHandle = *reinterpret_cast<const unsigned*>(p + ZIP_DISC_OFF_ACTIVEZIP);
		nWdf = static_cast<int>(p[ZIP_DISC_OFF_WEAPDISABLED]);
		nCtx = *reinterpret_cast<const int*>(p + ZIP_DISC_OFF_CONTEXTACTION);
		nForce = *reinterpret_cast<const int*>(p + ZIP_DISC_OFF_FORCESTANCE);
		nGateB = *reinterpret_cast<const int*>(p + ZIP_DISC_OFF_GATE_B);
		flCd = *reinterpret_cast<const float*>(p + ZIP_DISC_OFF_COOLDOWN);
		flDetach = *reinterpret_cast<const float*>(p + ZIP_DISC_OFF_LASTDETACH);
	}

	Msg(eDLL_T::CLIENT,
		"[ZIPRAIL-USE] result=%d why=%s cmd=%d grapple=%d state=%d->%d "
		"handle=%08X->%08X wdf=%d ctx=%d force=%d gateB=%d cd=%.3f det=%.3f "
		"player=%p\n",
		result ? 1 : 0, pszWhy,
		ZipDisc_CmdNumber(reinterpret_cast<void*>(player)),
		forGrappleZipline ? 1 : 0,
		nStateBefore, nState, nHandleBefore, nHandle,
		nWdf, nCtx, nForce, nGateB, flCd, flDetach,
		reinterpret_cast<void*>(player));
}

static bool Hook_Zipline_Use(uintptr_t player, bool forGrappleZipline)
{
	static bool s_bFirstCall = false;
	if (!s_bFirstCall)
	{
		s_bFirstCall = true;
		Warning(eDLL_T::CLIENT, "[ZIP-DISC] Use hook live player=%p grapple=%d\n",
			reinterpret_cast<void*>(player), forGrappleZipline ? 1 : 0);
	}

	int nStateBefore = 0;
	unsigned nHandleBefore = 0xFFFFFFFFu;
	if (player)
	{
		const uint8_t* const p = reinterpret_cast<const uint8_t*>(player);
		nStateBefore = *reinterpret_cast<const int*>(p + ZIP_DISC_OFF_ZIPLINESTATE);
		nHandleBefore = *reinterpret_cast<const unsigned*>(p + ZIP_DISC_OFF_ACTIVEZIP);
	}

	if (ZipDisc_ShouldRefuseMount(reinterpret_cast<void*>(player)))
	{
		ZipDisc_LogUse(player, forGrappleZipline, false, "disc",
			nStateBefore, nHandleBefore);
		return false;
	}

	const bool result = v_Zipline_Use(player, forGrappleZipline);
	if (result)
		ZipDisc_OnMountGranted(reinterpret_cast<void*>(player));

	int nGateB = 0;
	if (player)
		nGateB = *reinterpret_cast<const int*>(
			reinterpret_cast<const uint8_t*>(player) + ZIP_DISC_OFF_GATE_B);
	const char* pszWhy = result ? "ok" : (nGateB ? "gateB" : "native");
	ZipDisc_LogUse(player, forGrappleZipline, result, pszWhy,
		nStateBefore, nHandleBefore);
	return result;
}

static char __fastcall Hook_Zipline_JumpOff(void* zip, void* player, float* velocity,
	const void* eyeAngles, float flForwardMove, float flSideMove)
{
	static bool s_bFirstCall = false;
	if (!s_bFirstCall)
	{
		s_bFirstCall = true;
		Warning(eDLL_T::CLIENT, "[ZIP-JUMPOFF] hook live player=%p zip=%p\n",
			player, zip);
	}

	if (!v_Zipline_JumpOff)
		return 0;

	const char nResult = v_Zipline_JumpOff(zip, player, velocity, eyeAngles,
		flForwardMove, flSideMove);

	++s_nJumpOffCalls;
	if (nResult != 0 || s_nJumpOffCalls <= 8
		|| (bridge_zip_disc_diag.GetBool() && (s_nJumpOffCalls % 32) == 0))
	{
		int nType = -1;
		int nState = -1;
		unsigned nFlags = 0;
		if (player)
		{
			nState = *reinterpret_cast<const int*>(
				reinterpret_cast<const uint8_t*>(player) + ZIP_DISC_OFF_ZIPLINESTATE);
			nFlags = *reinterpret_cast<const unsigned*>(
				reinterpret_cast<const uint8_t*>(player) + ZIP_DISC_OFF_PLAYERFLAGS);
		}
		if (zip)
			nType = *reinterpret_cast<const int*>(
				reinterpret_cast<const uint8_t*>(zip) + ZIP_DISC_OFF_ZIP_TYPE);

		const float flVx = velocity ? velocity[0] : 0.0f;
		const float flVy = velocity ? velocity[1] : 0.0f;
		const float flVz = velocity ? velocity[2] : 0.0f;
		Msg(eDLL_T::CLIENT,
			"[ZIP-JUMPOFF] result=%d type=%d flags=0x%x state=%d cmd=%d "
			"vel=(%.1f %.1f %.1f) player=%p\n",
			static_cast<int>(nResult), nType, nFlags, nState,
			ZipDisc_CmdNumber(player), flVx, flVy, flVz, player);
	}

	return nResult;
}

void VZipDiscClient::GetAdr(void) const
{
	LogFunAdr("C_Player::Zipline_Use", v_Zipline_Use);
	LogFunAdr("C_Player::Zipline_JumpOff", v_Zipline_JumpOff);
}

void VZipDiscClient::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"48 8B C4 48 89 58 ?? 48 89 70 ?? 55 57 41 54 41 56 41 57 "
		"48 8D 68 ?? 48 81 EC ?? ?? ?? ?? 0F 29 70 ?? 0F B6 F2")
		.GetPtr(v_Zipline_Use);

	if (!v_Zipline_Use)
		Warning(eDLL_T::CLIENT,
			"[ZIP-DISC] C_Player::Zipline_Use pattern unresolved -- hop-regrab count disabled\n");

	// [rdx+2A04h] & 0x4000006 is the twin discriminator.
	Module_FindPattern(g_GameDll,
		"48 8B C4 48 89 58 ?? 48 89 70 ?? 48 89 78 ?? 55 41 54 41 55 41 56 41 57 "
		"48 8D 68 ?? 48 81 EC ?? ?? ?? ?? F7 82 04 2A 00 00 06 00 00 04")
		.GetPtr(v_Zipline_JumpOff);

	if (!v_Zipline_JumpOff)
		Warning(eDLL_T::CLIENT,
			"[ZIP-JUMPOFF] C_Player::Zipline_JumpOff pattern unresolved -- "
			"jump-off return census disabled\n");
}

void VZipDiscClient::Detour(const bool bAttach) const
{
	if (v_Zipline_Use)
		DetourSetup(&v_Zipline_Use, &Hook_Zipline_Use, bAttach);
	if (v_Zipline_JumpOff)
		DetourSetup(&v_Zipline_JumpOff, &Hook_Zipline_JumpOff, bAttach);
}
