//=============================================================================
//
// Purpose: S21->S3 weaponCustomActivity at Weapon_ExecuteCustomActivityCmd.
//
//=============================================================================
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "public/tier0/memaddr.h"
#include "tier1/convar.h"
#include "thirdparty/detours/include/detours.h"
#include "weapon_customact_c2s_xlat.h"
#include "activity_s3_to_s21.h"
#include "activity.h"

// Two LTCG clones. Hook both. Live clone is the per-usercmd consumer (ucmd+0x3A).
typedef void (__fastcall* Weapon_ExecuteCustomActivityCmd_t)(__int64 playerCtx, unsigned int activity);
static Weapon_ExecuteCustomActivityCmd_t v_Weapon_ExecuteCustomActivityCmd_Live = nullptr;
static Weapon_ExecuteCustomActivityCmd_t v_Weapon_ExecuteCustomActivityCmd_Shadow = nullptr;

static ConVar bridge_weap_act_c2s_xlat("bridge_weap_act_c2s_xlat", "1", FCVAR_RELEASE,
	"[WEAP-ACT-C2S] Translate the incoming usercmd's weaponCustomActivity id from "
	"S21-native (what the bridge client sends) to S3-native (what the dedi's own "
	"weapon simulation is compiled against) before CWeaponX::Weapon_ExecuteCustomActivityCmd "
	"processes it. Lets the dedi independently compute the SAME activity/sequence the "
	"client already predicted, instead of silently misinterpreting the raw S21 id through "
	"S3's own (differently-ordered) activity table. 1 = on (default), 0 = legacy raw passthrough.");
static unsigned int WeapActC2S_Translate(unsigned int activity)
{
	unsigned int xlatActivity = activity;

	if (bridge_weap_act_c2s_xlat.GetBool())
	{
		const int xlat = Bridge_TranslateS21ActivityToS3(static_cast<int>(activity));
		if (xlat >= 0)
			xlatActivity = static_cast<unsigned int>(xlat);
	}

	return xlatActivity;
}

static void __fastcall Hook_Weapon_ExecuteCustomActivityCmd_Live(__int64 playerCtx, unsigned int activity)
{
	v_Weapon_ExecuteCustomActivityCmd_Live(playerCtx, WeapActC2S_Translate(activity));
}

static void __fastcall Hook_Weapon_ExecuteCustomActivityCmd_Shadow(__int64 playerCtx, unsigned int activity)
{
	v_Weapon_ExecuteCustomActivityCmd_Shadow(playerCtx, WeapActC2S_Translate(activity));
}

bool WeaponCustomAct_ServerExecuteByName(void* pPlayer, const char* activityName)
{
	if (!pPlayer || !activityName || !*activityName)
		return false;

	if (!v_Weapon_ExecuteCustomActivityCmd_Live)
	{
		static bool s_once = false;
		if (!s_once)
		{
			s_once = true;
			Warning(eDLL_T::SERVER,
				"[WEAP-ACT-C2S] ServerExecuteByName('%s') -- LIVE ExecuteCustomActivityCmd unresolved\n",
				activityName);
		}
		return false;
	}

	const int s3Id = FindActivityByName(activityName);
	if (s3Id < 0)
	{
		Warning(eDLL_T::SERVER,
			"[WEAP-ACT-C2S] ServerExecuteByName: activity '%s' not registered "
			"(add it to scripts/activity_types.txt and restart)\n",
			activityName);
		return false;
	}

	// Trampoline -> original, with S3-native id. Do NOT call through the
	// detoured entry (that applies S21->S3 C2S translation).
	v_Weapon_ExecuteCustomActivityCmd_Live(
		reinterpret_cast<__int64>(pPlayer),
		static_cast<unsigned int>(s3Id));

	return true;
}

void VWeaponCustomActC2SXlat::GetFun(void) const
{
	// LIVE ends the 557-compare with near jnz (0F 85), SHADOW with short jnz (75).
	Module_FindPattern(g_GameDll,
		"83 FA ? 0F 84 ? ? ? ? 48 8B C4 55 41 56 48 83 EC ? 48 89 58 ? 8B EA "
		"48 8B 1D ? ? ? ? 4C 8B F1 4C 89 78 ? 45 33 FF 48 85 DB 74 ? "
		"C7 40 ? ? ? ? ? C6 40 ? ? E8 ? ? ? ? 48 8B 0D ? ? ? ? 45 8D 4F ? "
		"48 89 44 24 ? 4C 8D 44 24 ? 48 8D 44 24 ? 4C 89 7C 24 ? 4C 89 7C 24 ? "
		"48 8B D3 48 89 44 24 ? C7 44 24 ? ? ? ? ? C7 44 24 ? ? ? ? ? 89 6C 24 ? "
		"E8 ? ? ? ? 44 38 7C 24 ? 0F 84 ? ? ? ? 4C 89 6C 24 ? 4C 8D 2D ? ? ? ? "
		"81 FD ? ? ? ? 0F 85")
		.GetPtr(v_Weapon_ExecuteCustomActivityCmd_Live);

	Module_FindPattern(g_GameDll,
		"83 FA ? 0F 84 ? ? ? ? 48 8B C4 55 41 56 48 83 EC ? 48 89 58 ? 8B EA "
		"48 8B 1D ? ? ? ? 4C 8B F1 4C 89 78 ? 45 33 FF 48 85 DB 74 ? "
		"C7 40 ? ? ? ? ? C6 40 ? ? E8 ? ? ? ? 48 8B 0D ? ? ? ? 45 8D 4F ? "
		"48 89 44 24 ? 4C 8D 44 24 ? 48 8D 44 24 ? 4C 89 7C 24 ? 4C 89 7C 24 ? "
		"48 8B D3 48 89 44 24 ? C7 44 24 ? ? ? ? ? C7 44 24 ? ? ? ? ? 89 6C 24 ? "
		"E8 ? ? ? ? 44 38 7C 24 ? 0F 84 ? ? ? ? 4C 89 6C 24 ? 4C 8D 2D ? ? ? ? "
		"81 FD ? ? ? ? 75")
		.GetPtr(v_Weapon_ExecuteCustomActivityCmd_Shadow);

	if (!v_Weapon_ExecuteCustomActivityCmd_Live)
		Warning(eDLL_T::SERVER, "[WEAP-ACT-C2S] LIVE Weapon_ExecuteCustomActivityCmd pattern unresolved\n");
	if (!v_Weapon_ExecuteCustomActivityCmd_Shadow)
		Warning(eDLL_T::SERVER, "[WEAP-ACT-C2S] SHADOW Weapon_ExecuteCustomActivityCmd pattern unresolved\n");
}

void VWeaponCustomActC2SXlat::Detour(const bool bAttach) const
{
	if (v_Weapon_ExecuteCustomActivityCmd_Live)
	{
		void* const preAttachTarget = reinterpret_cast<void*>(v_Weapon_ExecuteCustomActivityCmd_Live);
		const LONG result = DetourSetup(&v_Weapon_ExecuteCustomActivityCmd_Live, &Hook_Weapon_ExecuteCustomActivityCmd_Live, bAttach);
		if (bAttach)
			Msg(eDLL_T::SERVER, "[WEAP-ACT-C2S] hooked LIVE Weapon_ExecuteCustomActivityCmd @ %p result=0x%lX (0=NO_ERROR)\n",
				preAttachTarget, result);
	}
	else if (bAttach)
	{
		Warning(eDLL_T::SERVER, "[WEAP-ACT-C2S] DISABLED -- LIVE Weapon_ExecuteCustomActivityCmd not resolved\n");
	}

	if (v_Weapon_ExecuteCustomActivityCmd_Shadow)
	{
		void* const preAttachTarget = reinterpret_cast<void*>(v_Weapon_ExecuteCustomActivityCmd_Shadow);
		const LONG result = DetourSetup(&v_Weapon_ExecuteCustomActivityCmd_Shadow, &Hook_Weapon_ExecuteCustomActivityCmd_Shadow, bAttach);
		if (bAttach)
			Msg(eDLL_T::SERVER, "[WEAP-ACT-C2S] hooked SHADOW Weapon_ExecuteCustomActivityCmd @ %p result=0x%lX (0=NO_ERROR)\n",
				preAttachTarget, result);
	}
	else if (bAttach)
	{
		Warning(eDLL_T::SERVER, "[WEAP-ACT-C2S] SHADOW Weapon_ExecuteCustomActivityCmd not resolved (non-fatal, dead-twin coverage only)\n");
	}
}
