//=============================================================================//
//
// Purpose: C_Ziprail client-side observability of dedi-published rail state.
//
//=============================================================================//
#include "core/stdafx.h"
#include "engine/client/net_observer.h"
#include "engine/client/net_bridge_internal.h"

#include "engine/cmd.h"
#include "engine/net.h"
#include "engine/net_chan.h"
#include "engine/client/clientstate.h"
#include "engine/client/client.h"
#include "public/tier1/cmd.h"
#include "public/bspflags.h"
#include "tier1/cvar.h"
#include "tier0/commandline.h"
#include "tier0/memvalidate.h"
#include "tier0/module.h"
#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/idetour.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#pragma warning(disable: 4456 4459)

// Mirror dedi sdk_ziprail_enable (dt_extend is server-only). Both sides must agree.
static ConVar sdk_ziprail_enable_cl("sdk_ziprail_enable", "1", FCVAR_RELEASE,
	"Client: extra C_Ziprail path-end / tune hooks.");

static ConVar sdk_ziprail_spatial_exclude("sdk_ziprail_spatial_exclude", "0", FCVAR_RELEASE,
	"Exclude client ziprails from spatial queries after PostDataUpdate. "
	"0 = ship (dedi publishes bounds + 0x4000).");

// C_Ziprail m_numZiprailPathNodes +0x0FA0 (BYTE) -- S21.

typedef void(__fastcall* PFN_ZiprailPostDataUpdate)(void* self, int updateType, float oldTime, float newTime);
static PFN_ZiprailPostDataUpdate s_origZiprailPostDataUpdate = nullptr;
typedef void(__fastcall* PFN_ExcludeFromSpatial)(__int64);
static PFN_ExcludeFromSpatial v_ExcludeFromSpatial = nullptr;
typedef void(__fastcall* PFN_PartitionListClear)(__int64, unsigned __int16);
static PFN_PartitionListClear v_PartitionListClear = nullptr;
typedef void(__fastcall* PFN_SpatialAccelDestroy)(__int64);
static PFN_SpatialAccelDestroy v_SpatialAccelDestroy = nullptr;

// C_Zipline::IsZiprail vtbl slot (S21: call [rax+7B0h] inside Find).
static constexpr int kZiprailVtbl_IsZiprail = 0x7B0;
static constexpr ptrdiff_t kClEntOffCollisionProp = 0x3B8;
static constexpr ptrdiff_t kClPropOffSolidFlags = 0x28;
static constexpr unsigned int kSolidFlagNotInSpatial = 0x4000;
static constexpr ptrdiff_t kClZiprailOffNodeCount = 0x0FA0;
static constexpr unsigned kZiprailPathNodeMax = 32;

// Base zipline virtual: false for plain C_Zipline, true only for C_Ziprail.
// Safe to call on any zipline entity (slot exists on the base class).
static bool ClZiprail_IsZiprailEntity(void* self)
{
	if (!self)
		return false;
	const auto isZiprail = reinterpret_cast<unsigned char(__fastcall*)(void*)>(
		*reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(self) + kZiprailVtbl_IsZiprail));
	if (!isZiprail)
		return false;
	return isZiprail(self) != 0;
}

static void __fastcall Hook_ZiprailPostDataUpdate(void* self, int updateType, float oldTime, float newTime)
{
	// self = IClientNetworkable subobject; C_Ziprail base is self-24.
	const uintptr_t ent = reinterpret_cast<uintptr_t>(self) - 24;

	// RecvPropArray3 is 32. Native create loops nodeCount with no min;
	// clamp before that walk so positions[i] stay inside the 0x12F0 object.
	if (ClZiprail_IsZiprailEntity(reinterpret_cast<void*>(ent)))
	{
		__try
		{
			unsigned char* const pCount = reinterpret_cast<unsigned char*>(ent + kClZiprailOffNodeCount);
			if (*pCount > kZiprailPathNodeMax)
			{
				static volatile LONG s_nodeClamp = 0;
				if (InterlockedIncrement(&s_nodeClamp) <= 8)
					Warning(eDLL_T::ENGINE,
						"[CL-ZIPRAIL] nodeCount=%u over %u -- clamp ent=0x%p\n",
						static_cast<unsigned>(*pCount), kZiprailPathNodeMax,
						reinterpret_cast<void*>(ent));
				*pCount = static_cast<unsigned char>(kZiprailPathNodeMax);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			Warning(eDLL_T::ENGINE,
				"[CL-ZIPRAIL] nodeCount clamp raised ent=0x%p\n",
				reinterpret_cast<void*>(ent));
		}
	}

	if (s_origZiprailPostDataUpdate)
		s_origZiprailPostDataUpdate(self, updateType, oldTime, newTime);

	// Pattern is shared by C_Zipline and C_Ziprail vtables; the path-node
	// count only exists on the subclass. IsZiprail lives in the PRIMARY
	// vtable, so it takes the entity base, not the interface subobject.
	if (!ClZiprail_IsZiprailEntity(reinterpret_cast<void*>(ent)))
		return;

	if (sdk_ziprail_spatial_exclude.GetBool())
	{
		const uintptr_t prop = ent + kClEntOffCollisionProp;
		int flags = 0;
		__try
		{
			flags = *reinterpret_cast<const int*>(prop + kClPropOffSolidFlags);
			if ((flags & kSolidFlagNotInSpatial) == 0 && v_ExcludeFromSpatial)
				v_ExcludeFromSpatial(static_cast<__int64>(ent));
			if (v_PartitionListClear)
				v_PartitionListClear(static_cast<__int64>(prop), 0);
			if (v_SpatialAccelDestroy)
				v_SpatialAccelDestroy(static_cast<__int64>(prop));
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			Warning(eDLL_T::ENGINE,
				"[CL-ZIPRAIL] spatial-exclude raised ent=0x%p\n",
				reinterpret_cast<void*>(ent));
		}

		static volatile LONG s_excludeLogs = 0;
		if (InterlockedIncrement(&s_excludeLogs) <= 8)
			Msg(eDLL_T::ENGINE,
				"[CL-ZIPRAIL] spatial-exclude ent=0x%p flags=0x%X\n",
				reinterpret_cast<void*>(ent), flags);
	}

	// The client builds rail geometry once, in the create update. A rail
	// whose wire carried no path at that moment stays empty for the map.
	if (updateType == 1 /* DATA_UPDATE_CREATED */)
	{
		const unsigned int nodeCount = *reinterpret_cast<unsigned __int8*>(ent + kClZiprailOffNodeCount);
		if (nodeCount < 2)
		{
			static volatile LONG s_emptyPathWarns = 0;
			if (InterlockedIncrement(&s_emptyPathWarns) <= 8)
				Warning(eDLL_T::ENGINE,
					"[CL-ZIPRAIL] ent=0x%p created with an empty path (nodes=%u) -- "
					"the dedi published nothing for this rail\n",
					reinterpret_cast<void*>(ent), nodeCount);
		}
	}
}

static void InstallFrameHooks_S21()
{
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC ?? 48 8D 99 ?? ?? ?? ?? 8B 43")
		.GetPtr(v_ExcludeFromSpatial);
	Module_FindPattern(g_GameDll,
		"80 79 ?? ?? 44 0F B7 CA")
		.GetPtr(v_PartitionListClear);
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 57 48 83 EC ?? 0F B7 59 ?? 48 8B F9 66 85 DB")
		.GetPtr(v_SpatialAccelDestroy);

	if (!v_ExcludeFromSpatial)
		Warning(eDLL_T::ENGINE,
			"[CL-ZIPRAIL] ExcludeFromSpatialQueries pattern UNRESOLVED\n");
	if (!v_PartitionListClear || !v_SpatialAccelDestroy)
		Warning(eDLL_T::ENGINE,
			"[CL-ZIPRAIL] partition-remove helpers UNRESOLVED (clear=%p accel=%p)\n",
			reinterpret_cast<void*>(v_PartitionListClear),
			reinterpret_cast<void*>(v_SpatialAccelDestroy));

	CMemory pduAddr = Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 57 48 83 EC 20 8B DA 48 8B F9 E8 ?? ?? ?? ?? 83 FB 01 0F 85 4E 01 00 00");

	if (pduAddr)
	{
		s_origZiprailPostDataUpdate = (PFN_ZiprailPostDataUpdate)pduAddr.GetPtr();
		if (AttachInTxn(reinterpret_cast<PVOID*>(&s_origZiprailPostDataUpdate),
		              reinterpret_cast<PVOID>(&Hook_ZiprailPostDataUpdate),
		              "ZiprailPostDataUpdate"))
		{
			SDK_Log("[CL-ZIPRAIL] LIVE hooked PostDataUpdate @ 0x%p\n",
				(void*)s_origZiprailPostDataUpdate);
		}
	}
	else
	{
		Warning(eDLL_T::ENGINE,
			"[CL-ZIPRAIL] path hook NOT installed (PostDataUpdate=0x%p)\n",
			(void*)pduAddr.GetPtr());
	}
}

void VNetFrameDiagS21::Detour(const bool bAttach) const
{
	if (bAttach)
		InstallFrameHooks_S21();
}

//-----------------------------------------------------------------------------
// One-shot dump of the player's ziprail tune values for sdk_ziprail_tune_*.
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t kClPlayerOffSettings = 9624; // m_classSettings.data

struct ZiprailTuneField_t
{
	const char* pszName;
	uintptr_t   nNodeRva;
};

static const ZiprailTuneField_t s_ziprailTuneFields[] =
{
	{ "ziprailAcceleration",          0x18626C8 },
	{ "pathGravityAcceleration",      0x1862A38 },
	{ "maxSpeedMultiplier",           0x18624B8 },
	{ "minSpeedFraction",             0x1862AE8 },
	{ "ziprailDetachSpeed",           0x1862358 },
	{ "maxJumpOffSpeed",              0x18628D8 },
	{ "minJumpOffSpeed",              0x1862148 },
	{ "viewInfluenceOnJumpDirection", 0x1861FE8 },
	{ "detachDirMaxDownAngle",        0x1861D28 },
	{ "detachDirMaxUpAngle",          0x1861F38 },
	{ "bankCurveDist",                0x1861C78 },
	{ "maxBankAngle",                 0x1862988 },
	{ "maxCameraRoll",                0x1860668 },
	{ "ziprailMaxCurvatureRollAngle", 0x18609D8 },
};

static ConVar sdk_ziprail_tune_dump("sdk_ziprail_tune_dump", "0", FCVAR_DEVELOPMENTONLY,
	"One-shot dump of ziprail player-settings values on first rail ride tick.");

// 18-arg S21 client ride integrator (returns pOutAlpha in rax).
typedef float*(__fastcall* PFN_ClZiprailMoveAlongRope)(
	void* player, void* zipline, char bReverse, int bEndDetached,
	float flDetachDistance, float flAcceleration, float flZiplineSpeed,
	const float* pPoints, float* pDistances, int nPoints,
	float flAlphaIn, float flEyeHullHeightDiff, const float* pPlayerVelocity,
	float* pOutPosition, float* pOutVelocity, float* pOutAlpha,
	float* pOutTargetAlpha, float* pOutPathDirection);

static PFN_ClZiprailMoveAlongRope s_origClZiprailMoveAlongRope = nullptr;
static volatile LONG s_ziprailTuneDumped = 0;

static bool ClZiprailTune_ValueTrustworthy(float fl)
{
	return std::isfinite(fl) && fl > 0.0f && fl < 100000.0f;
}

static void ClZiprailTune_DumpOnce(void* player)
{
	if (!sdk_ziprail_tune_dump.GetBool())
		return;
	if (InterlockedCompareExchange(&s_ziprailTuneDumped, 1, 0) != 0)
		return;
	if (!player)
	{
		Warning(eDLL_T::ENGINE,
			"[CL-ZIPRAIL-TUNE] dump skipped -- null player\n");
		return;
	}

	const uintptr_t settings = *reinterpret_cast<const uintptr_t*>(
		reinterpret_cast<const uint8_t*>(player) + kClPlayerOffSettings);
	if (!settings || !Mem_IsReadable(reinterpret_cast<const void*>(settings), 0x1000))
	{
		Warning(eDLL_T::ENGINE,
			"[CL-ZIPRAIL-TUNE] dump skipped -- settings block unreadable "
			"(player+0x%X=%p)\n",
			static_cast<unsigned>(kClPlayerOffSettings),
			reinterpret_cast<void*>(settings));
		return;
	}

	const uintptr_t base = g_GameDll.GetModuleBase();
	float flTune[10] = { -1.f, -1.f, -1.f, -1.f, -1.f, -1.f, -1.f, -1.f, -1.f, -1.f };
	const int nFieldCount = static_cast<int>(SDK_ARRAYSIZE(s_ziprailTuneFields));
	int nResolved = 0;

	for (int i = 0; i < nFieldCount; ++i)
	{
		const ZiprailTuneField_t& field = s_ziprailTuneFields[i];
		const uint32_t* const pNode = reinterpret_cast<const uint32_t*>(
			base + field.nNodeRva);
		if (!Mem_IsReadable(pNode, sizeof(uint32_t)))
		{
			Warning(eDLL_T::ENGINE,
				"[CL-ZIPRAIL-TUNE] %s node unreadable (rva=0x%llX)\n",
				field.pszName, static_cast<unsigned long long>(field.nNodeRva));
			continue;
		}

		const uint32_t nOff = pNode[0];
		if (nOff == 0xFFFFFFFFu)
		{
			// Layout not loaded yet -- allow a later tick to retry.
			continue;
		}
		++nResolved;

		if (!Mem_IsReadable(reinterpret_cast<const void*>(settings + nOff),
			sizeof(float)))
		{
			Warning(eDLL_T::ENGINE,
				"[CL-ZIPRAIL-TUNE] %s off=0x%X value unreadable\n",
				field.pszName, nOff);
			continue;
		}

		const float flValue = *reinterpret_cast<const float*>(settings + nOff);
		if (!ClZiprailTune_ValueTrustworthy(flValue))
		{
			Warning(eDLL_T::ENGINE,
				"[CL-ZIPRAIL-TUNE] %s off=0x%X value=%.9g untrusted "
				"(need finite in (0,100000))\n",
				field.pszName, nOff, flValue);
			continue;
		}

		Msg(eDLL_T::ENGINE,
			"[CL-ZIPRAIL-TUNE] %s off=0x%X value=%.9g\n",
			field.pszName, nOff, flValue);

		// The leading ten fields map 1:1, in order, to the dedi
		// sdk_ziprail_tune_* levers; the remaining four are camera-only.
		if (i < 10)
			flTune[i] = flValue;
	}

	if (nResolved == 0)
	{
		// No node offsets yet -- unlock so the next rail tick can try again.
		InterlockedExchange(&s_ziprailTuneDumped, 0);
		return;
	}

	Msg(eDLL_T::ENGINE,
		"[CL-ZIPRAIL-TUNE] dedi: sdk_ziprail_tune_accel %.9g;"
		"sdk_ziprail_tune_grav_accel %.9g;"
		"sdk_ziprail_tune_max_speed_mul %.9g;"
		"sdk_ziprail_tune_min_speed_frac %.9g;"
		"sdk_ziprail_tune_detach_speed %.9g;"
		"sdk_ziprail_tune_max_jumpoff %.9g;"
		"sdk_ziprail_tune_min_jumpoff %.9g;"
		"sdk_ziprail_tune_view_influence %.9g;"
		"sdk_ziprail_tune_detach_dir_max_down %.9g;"
		"sdk_ziprail_tune_detach_dir_max_up %.9g\n",
		flTune[0], flTune[1], flTune[2], flTune[3], flTune[4],
		flTune[5], flTune[6], flTune[7], flTune[8], flTune[9]);
}

static float* __fastcall Hook_ClZiprailMoveAlongRope(
	void* player, void* zipline, char bReverse, int bEndDetached,
	float flDetachDistance, float flAcceleration, float flZiplineSpeed,
	const float* pPoints, float* pDistances, int nPoints,
	float flAlphaIn, float flEyeHullHeightDiff, const float* pPlayerVelocity,
	float* pOutPosition, float* pOutVelocity, float* pOutAlpha,
	float* pOutTargetAlpha, float* pOutPathDirection)
{
	if (!s_origClZiprailMoveAlongRope)
		return nullptr;

	float* const pResult = s_origClZiprailMoveAlongRope(player, zipline, bReverse,
		bEndDetached, flDetachDistance, flAcceleration, flZiplineSpeed,
		pPoints, pDistances, nPoints,
		flAlphaIn, flEyeHullHeightDiff, pPlayerVelocity,
		pOutPosition, pOutVelocity, pOutAlpha,
		pOutTargetAlpha, pOutPathDirection);

	// First call with a live player is enough; gate is one-shot via latch.
	if (player && zipline && ClZiprail_IsZiprailEntity(zipline))
		ClZiprailTune_DumpOnce(player);

	return pResult;
}

class VZiprailTuneDump : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("Zipline_MoveAlongRope (client)", s_origClZiprailMoveAlongRope);
	}
	virtual void GetFun(void) const
	{
		// S21 client ride integrator; pattern verified 1 hit.
		Module_FindPattern(g_GameDll,
			"48 8B C4 48 89 58 ?? 55 56 57 41 54 41 55 41 56 41 57 "
			"48 8D 68 ?? 48 81 EC ?? ?? ?? ?? 48 8B 9D")
			.GetPtr(s_origClZiprailMoveAlongRope);

		if (!s_origClZiprailMoveAlongRope)
			Warning(eDLL_T::ENGINE,
				"[CL-ZIPRAIL-TUNE] MoveAlongRope pattern unresolved -- dump disabled\n");
	}
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const
	{
		if (s_origClZiprailMoveAlongRope)
			DetourSetup(&s_origClZiprailMoveAlongRope, &Hook_ClZiprailMoveAlongRope, bAttach);
	}
};
///////////////////////////////////////////////////////////////////////////////
REGISTER(VZiprailTuneDump);
