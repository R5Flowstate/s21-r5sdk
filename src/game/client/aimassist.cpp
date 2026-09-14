//=============================================================================//
//
// Purpose: Client aim-assist magnet miss, L2 deadzone, yaw keep, ADS
// distance scale, highspeed scale, and sniper-scope playlist miss.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/module.h"
#include "tier1/cvar.h"
#include "game/client/aimassist.h"

//-----------------------------------------------------------------------------
static ConVar bridge_aimassist("bridge_aimassist", "1", FCVAR_RELEASE,
	"Master enable for client aim-assist hooks.");
static ConVar bridge_aimassist_magnet("bridge_aimassist_magnet", "1", FCVAR_RELEASE,
	"Playlist-miss magnet 0.40 -> 0.30.");
static ConVar bridge_aimassist_l2("bridge_aimassist_l2", "0", FCVAR_RELEASE,
	"Inflate small look/move so AND treats L2 as alive. Off=native AND.");
static ConVar bridge_aimassist_look_idle("bridge_aimassist_look_idle", "1", FCVAR_RELEASE,
	"Grounded + look below deadzone zeros magnet even if move is alive.");
static ConVar bridge_aimassist_look_deadzone("bridge_aimassist_look_deadzone", "0.06", FCVAR_RELEASE,
	"Look L2 deadzone for look_idle. Native AND is 0.03.");
static ConVar bridge_aimassist_yaw("bridge_aimassist_yaw", "1", FCVAR_RELEASE,
	"Do not pull yaw away from the current target at 1000-1500 u.");
static ConVar bridge_aimassist_adsdist("bridge_aimassist_adsdist", "1", FCVAR_RELEASE,
	"ADS magnet scale by distance (200-2500 u, 0.5-1.5).");
static ConVar bridge_aimassist_highspeed("bridge_aimassist_highspeed", "1", FCVAR_RELEASE,
	"Highspeed magnet scale 0.9 ADS / 0.65 hip.");
static ConVar bridge_aimassist_sniper("bridge_aimassist_sniper", "1", FCVAR_RELEASE,
	"Playlist miss keeps sniper scopes off (zoomClass>=3). Non-sniper stay on.");

static ConVar sdk_aimassist_diag("sdk_aimassist_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[AA] log miss/l2/yaw/ads/hs/sniper. 0=off.");

//-----------------------------------------------------------------------------
static constexpr ptrdiff_t kAAMoveX = 0x184; // look-compose move axes
static constexpr ptrdiff_t kAAMoveY = 0x188;
static constexpr ptrdiff_t kPlayerGroundHandle = 0x324; // invalid = air, keep magnet
static constexpr ptrdiff_t kPlayerWeaponHandle = 0x1930; // -1 = unarmed
static constexpr ptrdiff_t kPlayerAds = 0x1BE1;
static constexpr ptrdiff_t kPlayerAimFwd = 0x1FD0; // 3x3, stride 0x10
static constexpr ptrdiff_t kPlayerHighspeedByte = 0x2D75;
static constexpr ptrdiff_t kWeaponZoomClass = 0x2524; // compose enable; miss is class<3
static constexpr uint32_t kBits09 = 0x3F666666u;
static constexpr size_t kApplyScan = 0x118D;

static constexpr float kLookThresh = 0.03f;
static constexpr float kMoveThresh = 0.10f;
static constexpr float kMissWas = 0.40f;
static constexpr float kMissWant = 0.30f;
static constexpr float kAdsMinDist = 200.0f;
static constexpr float kAdsMaxDist = 2500.0f;
static constexpr float kAdsMinScale = 0.5f;
static constexpr float kAdsMaxScale = 1.5f;
static constexpr float kHsAds = 0.9f;
static constexpr float kHsHip = 0.65f;
static constexpr float kYawKeepMin = 1000.0f;
static constexpr float kYawKeepMax = 1500.0f;
static constexpr float kDegToRad = 0.017453292f;

//-----------------------------------------------------------------------------
typedef float (*AimAssist_GetRawMagnetScaleFn)(void* player, float k);
typedef float* (*AimAssist_ApplyFn)(void* aa, void* player, float* target,
	float* a3, float lookX, float lookY, float a6, float a7, float* a8);
typedef void (*AimAssist_PullFn)(void* aa, void* player, float* outYawPitch);
typedef char (*AimAssist_SniperFn)(void* player, uint8_t a2, uint8_t* a3, float a4);
typedef float (*AimAssist_PitchFadeFn)(void* player);
typedef bool (*AimAssist_WallrunFn)(void* player);
typedef char (*PlaylistVarOnFn)(const char* name, char miss);
typedef void* (*GetWeaponFn)(void* player);
typedef void (*AimAssist_PullGatherFn)(void* weapon, void* aa, void* player, void* out);
typedef float* (*GetEyeFn)(void* player, float* out);

static AimAssist_GetRawMagnetScaleFn v_AimAssist_GetRawMagnetScale = nullptr;
static AimAssist_ApplyFn v_AimAssist_Apply = nullptr;
static AimAssist_PullFn v_AimAssist_Pull = nullptr;
static AimAssist_SniperFn v_AimAssist_Sniper = nullptr;
static AimAssist_PitchFadeFn v_AimAssist_PitchFade = nullptr;
static AimAssist_WallrunFn v_AimAssist_IsWallrun = nullptr;
static PlaylistVarOnFn v_PlaylistVarOn = nullptr;
static GetWeaponFn v_GetWeapon = nullptr;
static AimAssist_PullGatherFn v_AimAssist_PullGather = nullptr;
static GetEyeFn v_GetEye = nullptr;

static bool s_bHighspeedScale = true;
static bool s_bInApply = false;
static const float* s_pApplyTarget = nullptr;
static int s_nDiag = 0;

//-----------------------------------------------------------------------------
static bool AimAssistOn(void)
{
	return bridge_aimassist.GetBool();
}

static float SignCopy(const float mag, const float sgn)
{
	return sgn < 0.0f ? -mag : mag;
}

static float Dist3(const float* const a, const float* const b)
{
	const float dx = a[0] - b[0];
	const float dy = a[1] - b[1];
	const float dz = a[2] - b[2];
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

static float AdsDistScale(const float flDist)
{
	const float flSpan = kAdsMaxDist - kAdsMinDist;
	float t = (flDist - kAdsMinDist) / flSpan;
	if (t < 0.0f)
		t = 0.0f;
	if (t > 1.0f)
		t = 1.0f;
	return kAdsMinScale + (kAdsMaxScale - kAdsMinScale) * t;
}

static bool ReadEye(void* const pPlayer, float* const pOut)
{
	if (!pPlayer || !pOut || !v_GetEye)
		return false;
	v_GetEye(pPlayer, pOut);
	return true;
}

static bool PlayerGrounded(void* const pPlayer)
{
	if (!pPlayer)
		return false;
	const uint32_t h = *reinterpret_cast<const uint32_t*>(
		static_cast<uint8_t*>(pPlayer) + kPlayerGroundHandle);
	return h != 0xFFFFFFFFu;
}

static bool PlayerHasWeapon(void* const pPlayer)
{
	if (!pPlayer)
		return false;
	const uint32_t h = *reinterpret_cast<const uint32_t*>(
		static_cast<uint8_t*>(pPlayer) + kPlayerWeaponHandle);
	return h != 0xFFFFFFFFu;
}

static int WeaponZoomClass(void* const pPlayer)
{
	if (!pPlayer || !v_GetWeapon)
		return -1;
	void* const pWeapon = v_GetWeapon(pPlayer);
	if (!pWeapon)
		return -1;
	return *reinterpret_cast<const int*>(
		static_cast<uint8_t*>(pWeapon) + kWeaponZoomClass);
}

static bool PlayerAds(void* const pPlayer)
{
	if (!pPlayer)
		return false;
	return static_cast<uint8_t*>(pPlayer)[kPlayerAds] != 0;
}

static bool PlayerHighspeed(void* const pPlayer)
{
	if (!pPlayer)
		return false;
	if (v_AimAssist_IsWallrun && v_AimAssist_IsWallrun(pPlayer))
		return true;
	return static_cast<uint8_t*>(pPlayer)[kPlayerHighspeedByte] != 0;
}

static bool FunctionHasU32(const void* const pFn, const size_t nLen,
	const uint32_t nBits)
{
	if (!pFn || nLen < 4)
		return false;
	const uint8_t* const p = static_cast<const uint8_t*>(pFn);
	for (size_t i = 0; i + 4 <= nLen; ++i)
	{
		uint32_t v = 0;
		memcpy(&v, p + i, 4);
		if (v == nBits)
			return true;
	}
	return false;
}

static float RemapMiss040To030(void* const pPlayer, const float k, const float v)
{
	if (!v_AimAssist_PitchFade)
		return v;
	const float fade = v_AimAssist_PitchFade(pPlayer);
	if (fade == 0.0f || fabsf(1.0f - k) < 1.0e-6f)
		return v;
	const float inner = v / fade;
	const float base = (inner - k) / (1.0f - k);
	if (fabsf(base - kMissWas) > 0.02f)
		return v;
	return fade * ((1.0f - kMissWant) * k + kMissWant);
}

static void InflateL2(void* const pAA, float* const pLookX, float* const pLookY,
	float* const pSaveMX, float* const pSaveMY)
{
	*pSaveMX = *reinterpret_cast<float*>(static_cast<uint8_t*>(pAA) + kAAMoveX);
	*pSaveMY = *reinterpret_cast<float*>(static_cast<uint8_t*>(pAA) + kAAMoveY);

	const float lx = *pLookX;
	const float ly = *pLookY;
	const float mx = *pSaveMX;
	const float my = *pSaveMY;
	const float lookSq = lx * lx + ly * ly;
	const float moveSq = mx * mx + my * my;
	const float lookT2 = kLookThresh * kLookThresh;
	const float moveT2 = kMoveThresh * kMoveThresh;

	if (lookSq > lookT2 && fabsf(lx) <= kLookThresh && fabsf(ly) <= kLookThresh)
	{
		if (fabsf(lx) >= fabsf(ly))
			*pLookX = SignCopy(kLookThresh + 0.001f, lx == 0.0f ? 1.0f : lx);
		else
			*pLookY = SignCopy(kLookThresh + 0.001f, ly == 0.0f ? 1.0f : ly);
	}
	if (moveSq > moveT2 && fabsf(mx) <= kMoveThresh && fabsf(my) <= kMoveThresh)
	{
		float* const pmx = reinterpret_cast<float*>(
			static_cast<uint8_t*>(pAA) + kAAMoveX);
		float* const pmy = reinterpret_cast<float*>(
			static_cast<uint8_t*>(pAA) + kAAMoveY);
		if (fabsf(mx) >= fabsf(my))
			*pmx = SignCopy(kMoveThresh + 0.001f, mx == 0.0f ? 1.0f : mx);
		else
			*pmy = SignCopy(kMoveThresh + 0.001f, my == 0.0f ? 1.0f : my);
	}
}

static bool LookIdleDead(const float lookX, const float lookY)
{
	float flZone = bridge_aimassist_look_deadzone.GetFloat();
	if (flZone < 0.0f)
		flZone = 0.0f;
	if (flZone > 1.0f)
		flZone = 1.0f;
	const float lookSq = lookX * lookX + lookY * lookY;
	return lookSq <= flZone * flZone;
}

static void ZeroMoveForIdle(void* const pAA, float* const pSaveMX, float* const pSaveMY)
{
	*pSaveMX = *reinterpret_cast<float*>(static_cast<uint8_t*>(pAA) + kAAMoveX);
	*pSaveMY = *reinterpret_cast<float*>(static_cast<uint8_t*>(pAA) + kAAMoveY);
	*reinterpret_cast<float*>(static_cast<uint8_t*>(pAA) + kAAMoveX) = 0.0f;
	*reinterpret_cast<float*>(static_cast<uint8_t*>(pAA) + kAAMoveY) = 0.0f;
}

static bool PullTargetPos(void* const pAA, void* const pPlayer, float* const pOut)
{
	if (!pAA || !pPlayer || !pOut || !v_GetWeapon || !v_AimAssist_PullGather)
		return false;
	void* const pWeapon = v_GetWeapon(pPlayer);
	if (!pWeapon)
		return false;

	uint8_t info[64];
	memset(info, 0, sizeof(info));
	v_AimAssist_PullGather(pWeapon, pAA, pPlayer, info);
	if (!*reinterpret_cast<void**>(info))
		return false;

	memcpy(pOut, info + 8, sizeof(float) * 3);
	return true;
}

static void DiagTick(const char* const pszWhere, const float flExtra)
{
	if (!sdk_aimassist_diag.GetBool())
		return;
	++s_nDiag;
	if (s_nDiag > 8 && (s_nDiag & 63) != 0)
		return;
	Warning(eDLL_T::CLIENT,
		"[AA] %s miss=%d l2=%d yaw=%d ads=%d hs=%d sniper=%d extra=%.3f\n",
		pszWhere,
		bridge_aimassist_magnet.GetInt(),
		bridge_aimassist_l2.GetInt(),
		bridge_aimassist_yaw.GetInt(),
		bridge_aimassist_adsdist.GetInt(),
		bridge_aimassist_highspeed.GetInt(),
		bridge_aimassist_sniper.GetInt(),
		flExtra);
}

//-----------------------------------------------------------------------------
static float Hook_GetRawMagnetScale(void* pPlayer, float k)
{
	const float v = v_AimAssist_GetRawMagnetScale(pPlayer, k);
	if (!AimAssistOn())
		return v;

	float out = v;
	if (bridge_aimassist_magnet.GetBool())
		out = RemapMiss040To030(pPlayer, k, out);

	if (s_bInApply && pPlayer)
	{
		const bool bAds = PlayerAds(pPlayer);
		if (bridge_aimassist_adsdist.GetBool() && bAds && s_pApplyTarget)
		{
			float eye[3];
			if (ReadEye(pPlayer, eye))
				out *= AdsDistScale(Dist3(eye, s_pApplyTarget));
		}
		if (s_bHighspeedScale && bridge_aimassist_highspeed.GetBool()
			&& PlayerHighspeed(pPlayer))
		{
			out *= bAds ? kHsAds : kHsHip;
		}
	}

	DiagTick("scale", out);
	return out;
}

static float* Hook_Apply(void* pAA, void* pPlayer, float* pTarget, float* a3,
	float lookX, float lookY, float a6, float a7, float* a8)
{
	s_pApplyTarget = pTarget;
	s_bInApply = true;

	float saveMX = 0.0f;
	float saveMY = 0.0f;
	bool bMoved = false;
	if (AimAssistOn() && pAA)
	{
		if (bridge_aimassist_look_idle.GetBool() && LookIdleDead(lookX, lookY)
			&& PlayerGrounded(pPlayer))
		{
			lookX = 0.0f;
			lookY = 0.0f;
			ZeroMoveForIdle(pAA, &saveMX, &saveMY);
			bMoved = true;
			DiagTick("lookIdle", bridge_aimassist_look_deadzone.GetFloat());
		}
		else if (bridge_aimassist_l2.GetBool())
		{
			InflateL2(pAA, &lookX, &lookY, &saveMX, &saveMY);
			bMoved = true;
		}
	}

	float* const pRet = v_AimAssist_Apply(pAA, pPlayer, pTarget, a3,
		lookX, lookY, a6, a7, a8);

	if (bMoved && pAA)
	{
		*reinterpret_cast<float*>(static_cast<uint8_t*>(pAA) + kAAMoveX) = saveMX;
		*reinterpret_cast<float*>(static_cast<uint8_t*>(pAA) + kAAMoveY) = saveMY;
	}

	s_bInApply = false;
	s_pApplyTarget = nullptr;
	return pRet;
}

static void Hook_Pull(void* pAA, void* pPlayer, float* pOut)
{
	v_AimAssist_Pull(pAA, pPlayer, pOut);
	if (!AimAssistOn() || !bridge_aimassist_yaw.GetBool() || !pOut || !pPlayer)
		return;
	if (pOut[0] == 0.0f)
		return;

	float tgt[3];
	float eye[3];
	if (!PullTargetPos(pAA, pPlayer, tgt) || !ReadEye(pPlayer, eye))
		return;

	const float flDist = Dist3(eye, tgt);
	if (flDist < kYawKeepMin || flDist > kYawKeepMax)
		return;

	const float pull = pOut[0] * kDegToRad;
	const float* const pFwd = reinterpret_cast<const float*>(
		static_cast<uint8_t*>(pPlayer) + kPlayerAimFwd);
	float aimx = pFwd[0];
	float aimy = pFwd[1];
	const float aimLen = sqrtf(aimx * aimx + aimy * aimy);
	if (aimLen < 1.0e-6f)
		return;
	aimx /= aimLen;
	aimy /= aimLen;

	float tx = tgt[0] - eye[0];
	float ty = tgt[1] - eye[1];
	const float tLen = sqrtf(tx * tx + ty * ty);
	if (tLen < 1.0e-6f)
		return;
	tx /= tLen;
	ty /= tLen;

	const float err0 = fabsf(atan2f(aimx * ty - aimy * tx, aimx * tx + aimy * ty));
	const float c = cosf(pull);
	const float s = sinf(pull);
	const float nx = aimx * c - aimy * s;
	const float ny = aimx * s + aimy * c;
	const float err1 = fabsf(atan2f(nx * ty - ny * tx, nx * tx + ny * ty));
	if (err1 > err0)
	{
		pOut[0] = 0.0f;
		DiagTick("yawKeep", flDist);
	}
}

static char Hook_Sniper(void* pPlayer, uint8_t a2, uint8_t* a3, float a4)
{
	const char r = v_AimAssist_Sniper(pPlayer, a2, a3, a4);
	if (!AimAssistOn() || !bridge_aimassist_sniper.GetBool())
		return r;
	if (v_PlaylistVarOn && v_PlaylistVarOn("aimassist_enabled_sniper_scopes", 0))
		return r;
	if (!PlayerHasWeapon(pPlayer))
		return r;
	if (WeaponZoomClass(pPlayer) < 3)
		return r;
	if (r)
		DiagTick("sniperMiss", 3.0f);
	return 0;
}

//-----------------------------------------------------------------------------
void VAimAssist::GetAdr(void) const
{
	LogFunAdr("AimAssist_GetRawMagnetScale", v_AimAssist_GetRawMagnetScale);
	LogFunAdr("AimAssist_Apply", v_AimAssist_Apply);
	LogFunAdr("AimAssist_Pull", v_AimAssist_Pull);
	LogFunAdr("AimAssist_SniperScopes", v_AimAssist_Sniper);
	LogFunAdr("AimAssist_PitchFade", v_AimAssist_PitchFade);
	LogFunAdr("AimAssist_IsWallrun", v_AimAssist_IsWallrun);
	LogFunAdr("AimAssist_PullGather", v_AimAssist_PullGather);
	LogFunAdr("PlaylistVarOn", v_PlaylistVarOn);
	LogFunAdr("GetWeapon", v_GetWeapon);
	LogFunAdr("GetEye", v_GetEye);
}

void VAimAssist::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"40 57 48 83 EC ?? 0F 29 74 24 ?? 48 8B F9 0F 29 7C 24 ?? "
		"44 0F 29 44 24")
		.GetPtr(v_AimAssist_GetRawMagnetScale);

	Module_FindPattern(g_GameDll,
		"40 55 53 56 57 48 8D AC 24 ?? ?? ?? ?? B8")
		.GetPtr(v_AimAssist_Apply);

	Module_FindPattern(g_GameDll,
		"48 8B C4 55 53 57 41 54 41 56 41 57")
		.GetPtr(v_AimAssist_Pull);

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? "
		"0F 29 74 24 ?? 49 8B F8")
		.GetPtr(v_AimAssist_Sniper);

	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC ?? 48 8B D9 E8 ?? ?? ?? ?? 84 C0 74 ?? "
		"F3 0F 10 0D")
		.GetPtr(v_AimAssist_PitchFade);

	Module_FindPattern(g_GameDll,
		"48 83 EC ?? 83 B9 ?? ?? ?? ?? ?? 74 ?? 32 C0 48 83 C4 ?? "
		"C3 F2 0F 10 91")
		.GetPtr(v_AimAssist_IsWallrun);

	Module_FindPattern(g_GameDll,
		"4C 8B DC 49 89 5B ?? 55 57")
		.GetPtr(v_AimAssist_PullGather);

	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC ?? 4C 8B C9 0F B6 DA")
		.GetPtr(v_PlaylistVarOn);

	Module_FindPattern(g_GameDll,
		"48 83 EC ?? 48 8B 01 FF 90 ?? ?? ?? ?? 48 83 C0")
		.GetPtr(v_GetWeapon);

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 7C 24 ?? 55 48 8B EC 48 83 EC ?? "
		"48 8B DA 41 B0")
		.GetPtr(v_GetEye);

	if (v_AimAssist_Apply)
		s_bHighspeedScale = !FunctionHasU32(reinterpret_cast<const void*>(
			v_AimAssist_Apply), kApplyScan, kBits09);

	if (!v_AimAssist_GetRawMagnetScale)
		Warning(eDLL_T::CLIENT, "[AA] magnet scale pattern unresolved\n");
	if (!v_AimAssist_Apply)
		Warning(eDLL_T::CLIENT, "[AA] magnet apply pattern unresolved\n");
	if (!v_AimAssist_Pull)
		Warning(eDLL_T::CLIENT, "[AA] pull pattern unresolved\n");
	if (!v_AimAssist_Sniper)
		Warning(eDLL_T::CLIENT, "[AA] sniper-scope pattern unresolved\n");
	if (!v_AimAssist_PullGather || !v_GetWeapon)
		Warning(eDLL_T::CLIENT, "[AA] yaw-keep gather unresolved\n");
	if (!v_PlaylistVarOn)
		Warning(eDLL_T::CLIENT, "[AA] playlist var helper unresolved\n");
}

void VAimAssist::Detour(const bool bAttach) const
{
	if (v_AimAssist_GetRawMagnetScale)
		DetourSetup(&v_AimAssist_GetRawMagnetScale, &Hook_GetRawMagnetScale, bAttach);
	if (v_AimAssist_Apply)
		DetourSetup(&v_AimAssist_Apply, &Hook_Apply, bAttach);
	if (v_AimAssist_Pull)
		DetourSetup(&v_AimAssist_Pull, &Hook_Pull, bAttach);
	if (v_AimAssist_Sniper)
		DetourSetup(&v_AimAssist_Sniper, &Hook_Sniper, bAttach);

	if (bAttach && v_AimAssist_GetRawMagnetScale)
		Msg(eDLL_T::CLIENT, "[AA] aim assist attached (apply=%d pull=%d sniper=%d hs=%d)\n",
			v_AimAssist_Apply ? 1 : 0,
			v_AimAssist_Pull ? 1 : 0,
			v_AimAssist_Sniper ? 1 : 0,
			s_bHighspeedScale ? 1 : 0);
	else if (bAttach)
		Warning(eDLL_T::CLIENT, "[AA] aim assist disabled -- scale pattern unresolved\n");
}
