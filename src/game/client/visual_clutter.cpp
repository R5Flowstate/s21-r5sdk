//=============================================================================//
//
// Purpose: "Visual clutter" setting for the S21 client.
//
// cl_visual_clutter 0 = Full (ship behavior, every hook passes through).
// cl_visual_clutter 1 = Minimal:
//   - H1: all weapon muzzle flashes (1P + 3P, self + enemies) dropped at the
//     merged PlayWeaponParticleEffect funnel, matched by effect name.
//   - H2: engine anim-event weapon FX (event classes 18/20/115) dropped.
//   - H3: body-shield hit wrap, impact-table shield hit, shield-full flash,
//     regen loop and 3P break burst dropped by ParticleEffectNames index on
//     both the entity-attached and the world create funnels; indices are
//     seeded at runtime through the string table. The 1P cockpit hit/break
//     and all persistent walls, domes and gun shields are kept.
//   - Gib pin: cl_is_softened_locale forced to 1 so kill/death gibs use the
//     softened models and FX the engine already ships.
//
// Tracers, muzzle audio, damage numbers, hitmarkers, killfeed, the death
// screen and shield-regen status on the local cockpit are untouched, so
// threat reads and self status survive minimal mode.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "game/client/visual_clutter.h"

//-----------------------------------------------------------------------------
// ConVars
//-----------------------------------------------------------------------------
static void VisualClutter_Changed_f(IConVar* var, const char* pOldValue, float flOldValue, ChangeUserData_t pUserData)
{
	(void)var;
	(void)pOldValue;
	(void)flOldValue;
	(void)pUserData;
	VisualClutter_ApplyPin();
}

static ConVar cl_visual_clutter("cl_visual_clutter", "0", FCVAR_RELEASE | FCVAR_ARCHIVE,
	"Visual clutter: 0=Full (all weapon and shield FX), 1=Minimal (no muzzle "
	"flashes, no body-shield hit wrap or regen loop, softened kill/death FX pinned on).",
	VisualClutter_Changed_f);

// Per-bucket kill-switches. Default 1 (bucket active under minimal); set 0
// to force that bucket to passthrough for bisection.
static ConVar cl_visual_clutter_h1("cl_visual_clutter_h1", "1", FCVAR_RELEASE,
	"Minimal mode drops script/anim muzzle flashes at the weapon funnel.");
static ConVar cl_visual_clutter_h2("cl_visual_clutter_h2", "1", FCVAR_RELEASE,
	"Minimal mode drops engine anim-event weapon FX (classes 18/20/115).");
static ConVar cl_visual_clutter_h3("cl_visual_clutter_h3", "1", FCVAR_RELEASE,
	"Minimal mode drops the body-shield hit wrap, full flash and regen loop by effect index.");

static bool VisualClutter_Minimal(void)
{
	return cl_visual_clutter.GetInt() != 0;
}

//-----------------------------------------------------------------------------
// Gib pin (Bucket C): cl_is_softened_locale forced while minimal.
//-----------------------------------------------------------------------------
static ConVar* s_pSoftenedLocale = nullptr;
static int s_nSavedSoftened = 1;
static bool s_bPinning = false;
static bool s_bPinWarned = false;

static void VisualClutter_Changed_Softened_f(IConVar* var, const char* pOldValue, float flOldValue, ChangeUserData_t pUserData)
{
	(void)var;
	(void)pOldValue;
	(void)flOldValue;
	(void)pUserData;
	if (s_bPinning && s_pSoftenedLocale && s_pSoftenedLocale->GetInt() == 0)
	{
		s_pSoftenedLocale->SetValue(1);
		if (!s_bPinWarned)
		{
			s_bPinWarned = true;
			Warning(eDLL_T::CLIENT,
				"[VISCLUTTER] cl_is_softened_locale reasserted to 1 (minimal mode pins soft kill/death FX)\n");
		}
	}
}

void VisualClutter_ApplyPin(void)
{
	if (!s_pSoftenedLocale)
	{
		s_pSoftenedLocale = g_pCVar ? g_pCVar->FindVar("cl_is_softened_locale") : nullptr;
		if (!s_pSoftenedLocale)
		{
			if (VisualClutter_Minimal())
				Warning(eDLL_T::CLIENT,
					"[VISCLUTTER] cl_is_softened_locale not registered -- soft kill/death pin inactive\n");
			return;
		}
		s_pSoftenedLocale->InstallChangeCallback(VisualClutter_Changed_Softened_f, nullptr, false);
	}

	if (VisualClutter_Minimal())
	{
		if (!s_bPinning)
		{
			s_nSavedSoftened = s_pSoftenedLocale->GetInt();
			s_bPinning = true;
		}
		if (s_pSoftenedLocale->GetInt() == 0)
			s_pSoftenedLocale->SetValue(1);
	}
	else if (s_bPinning)
	{
		s_bPinning = false;
		s_pSoftenedLocale->SetValue(s_nSavedSoftened);
		Msg(eDLL_T::CLIENT, "[VISCLUTTER] cl_is_softened_locale restored to %d\n", s_nSavedSoftened);
	}
}

//-----------------------------------------------------------------------------
// Muzzle name match (Bucket A). Case-insensitive substring over a bounded
// prefix of the string; anything malformed fails open to full FX.
//-----------------------------------------------------------------------------
static bool VisClutter_CharEqCI(const char a, const char b)
{
	char ca = a, cb = b;
	if (ca >= 'A' && ca <= 'Z')
		ca = static_cast<char>(ca + ('a' - 'A'));
	if (cb >= 'A' && cb <= 'Z')
		cb = static_cast<char>(cb + ('a' - 'A'));
	return ca == cb;
}

static bool VisClutter_HasSubCI(const char* s, const char* needle)
{
	if (!s || !*s || !needle || !*needle)
		return false;
	for (int i = 0; i < 256 && s[i]; ++i)
	{
		int j = 0;
		while (needle[j] && i + j < 256 && s[i + j] && VisClutter_CharEqCI(s[i + j], needle[j]))
			++j;
		if (!needle[j])
			return true;
	}
	return false;
}

static bool VisClutter_IsMuzzleName(const char* const name)
{
	return VisClutter_HasSubCI(name, "muzzleflash")
		|| VisClutter_HasSubCI(name, "muzzle_flash")
		|| VisClutter_HasSubCI(name, "mflash");
}

//-----------------------------------------------------------------------------
// Shield denylist (Bucket B). Exact full-name match only; indices
// are seeded at runtime through the particle-manager resolver, so a wrong
// guess can never suppress the wrong effect. Entry 0 is the canary: a core
// asset used to detect table rebuilds.
//-----------------------------------------------------------------------------
// P_armor_body_CP is the per-hit body wrap (PlayShieldHitEffect), always
// precached by cl_player_common so it doubles as the canary. The 1P cockpit
// break (P_armor_FP_break) stays so self status survives.
static const char* const kVisClutterShieldDeny[] =
{
	"P_armor_body_CP",
	"P_armor_impact",
	"P_armor_3P_max_CP",
	"P_armor_3P_loop_CP",
	"P_armor_3P_break_CP",
};
static constexpr int kVisClutterShieldDenyCount = sizeof(kVisClutterShieldDeny) / sizeof(kVisClutterShieldDeny[0]);
// Client-precached names resolve to negative indices, so the sentinel must
// sit outside both ranges; 0xFFFF is the table's own miss value.
static constexpr int kVisClutterIdxUnresolved = INT_MIN;

static int s_denyIdx[kVisClutterShieldDenyCount] =
{
	kVisClutterIdxUnresolved, kVisClutterIdxUnresolved,
	kVisClutterIdxUnresolved, kVisClutterIdxUnresolved,
	kVisClutterIdxUnresolved,
};

typedef int(__fastcall* PFN_ParticleNameToIndex)(void*, const char*);

static PFN_ParticleNameToIndex s_pfnNameToIndex = nullptr;
static bool s_bResolverTrusted = false;
static bool s_bResolverDead = false;
static volatile LONG s_nSeedSpawns = 0;

static void* VisClutter_ParticleMgr(void)
{
	if (!s_ppVisClutterParticleMgr)
		return nullptr;
	return *s_ppVisClutterParticleMgr;
}

static int VisClutter_ResolveIndex(void* const mgr, const char* const name)
{
	void* const* const vt = *reinterpret_cast<void***>(mgr);
	s_pfnNameToIndex = reinterpret_cast<PFN_ParticleNameToIndex>(vt[10]);
	return s_pfnNameToIndex(mgr, name);
}

// One-shot trust probe plus canary revalidation. Runs only on the minimal
// path, so full mode never pays for it.
static bool VisClutter_DenySetReady(void)
{
	void* const mgr = VisClutter_ParticleMgr();
	if (!mgr || s_bResolverDead)
		return false;

	if (!s_bResolverTrusted)
	{
		void* const* const vt = *reinterpret_cast<void***>(mgr);
		s_pfnNameToIndex = reinterpret_cast<PFN_ParticleNameToIndex>(vt[10]);
		if (s_pfnNameToIndex(mgr, "___no_such_fx___") == 0xFFFF
			&& s_pfnNameToIndex(mgr, "") == 0xFFFF)
		{
			s_bResolverTrusted = true;
			Msg(eDLL_T::CLIENT, "[VISCLUTTER] particle name table resolver trusted (vt[10])\n");
		}
		else
		{
			s_bResolverDead = true;
			Warning(eDLL_T::CLIENT,
				"[VISCLUTTER] particle name table probe failed (miss=%d empty=%d) -- shield suppression disabled\n",
				s_pfnNameToIndex(mgr, "___no_such_fx___"), s_pfnNameToIndex(mgr, ""));
			return false;
		}
	}

	// Canary: entry 0 must resolve to the cached index. A mismatch means the
	// table was rebuilt (level change), so reseed the whole set. While the
	// canary is unresolvable the table is not ready: pass everything through.
	// Reseed attempts while unresolved are rate-limited to one per 256 spawns.
	const LONG n = InterlockedIncrement(&s_nSeedSpawns);
	const int canary = VisClutter_ResolveIndex(mgr, kVisClutterShieldDeny[0]);
	if (canary != s_denyIdx[0])
	{
		if (canary == 0xFFFF && s_denyIdx[0] == kVisClutterIdxUnresolved && (n % 256) != 1)
			return false;
		for (int i = 0; i < kVisClutterShieldDenyCount; ++i)
			s_denyIdx[i] = VisClutter_ResolveIndex(mgr, kVisClutterShieldDeny[i]);
		Msg(eDLL_T::CLIENT, "[VISCLUTTER] shield denylist seeded: %s=%d %s=%d %s=%d %s=%d %s=%d\n",
			kVisClutterShieldDeny[0], s_denyIdx[0],
			kVisClutterShieldDeny[1], s_denyIdx[1],
			kVisClutterShieldDeny[2], s_denyIdx[2],
			kVisClutterShieldDeny[3], s_denyIdx[3],
			kVisClutterShieldDeny[4], s_denyIdx[4]);
	}
	return true;
}

// Returns the denylist slot for a spawn index under minimal mode, or -1.
static int VisClutter_ShieldDenySlot(const int idx)
{
	if (!VisualClutter_Minimal() || !cl_visual_clutter_h3.GetBool() || !VisClutter_DenySetReady())
		return -1;
	if (idx == 0xFFFF || idx == kVisClutterIdxUnresolved)
		return -1;

	for (int i = 0; i < kVisClutterShieldDenyCount; ++i)
	{
		if (idx == s_denyIdx[i])
			return i;
	}
	return -1;
}

//-----------------------------------------------------------------------------
// Hooks
//-----------------------------------------------------------------------------
static char __fastcall Hook_PlayWeaponFx(int64_t weapon, const char* viewName, const char* worldName,
	char a4, char a5, int a6, char a7, char a8, int a9)
{
	if (VisualClutter_Minimal() && cl_visual_clutter_h1.GetBool()
		&& (VisClutter_IsMuzzleName(viewName) || VisClutter_IsMuzzleName(worldName)))
		return 0;
	return v_VisClutter_PlayWeaponFx(weapon, viewName, worldName, a4, a5, a6, a7, a8, a9);
}

static char __fastcall Hook_AnimWeaponFx(int64_t a1, int a2, int a3, int a4, int a5, void* a6, int64_t a7, int64_t a8)
{
	if (VisualClutter_Minimal() && cl_visual_clutter_h2.GetBool() && (a4 == 18 || a4 == 20 || a4 == 115))
		return 1; // handled: 0 re-dispatches into script
	return v_VisClutter_AnimWeaponFx(a1, a2, a3, a4, a5, a6, a7, a8);
}

static int64_t __fastcall Hook_StartParticleOnEntity(int64_t a1, int64_t a2, int64_t a3, int64_t a4,
	void* a5, int64_t a6, int a7, char a8, char a9)
{
	// -1 is the engine's own CPU-culled sentinel; scripts treat it as an
	// invalid handle.
	if (VisClutter_ShieldDenySlot(static_cast<int>(a2)) >= 0)
		return -1;
	return v_VisClutter_StartParticleOnEntity(a1, a2, a3, a4, a5, a6, a7, a8, a9);
}

static int64_t __fastcall Hook_CreateParticleByIndex(int64_t a1, int idx, int64_t a3)
{
	// Callers null-check the returned system and skip it.
	if (VisClutter_ShieldDenySlot(idx) >= 0)
		return 0;
	return v_VisClutter_CreateParticleByIndex(a1, idx, a3);
}

///////////////////////////////////////////////////////////////////////////////
void VVisualClutter::GetFun(void) const
{
	// Merged PlayWeaponParticleEffect funnel: script PlayWeaponEffect*
	// variants, viewmodel/worldmodel setup and the OnIdx resolve bottom out
	// here with plain effect names.
	Module_FindPattern(g_GameDll, "44 88 4C 24 ? 55 53")
		.GetPtr(v_VisClutter_PlayWeaponFx);

	// Anim-event weapon-FX dispatcher (weapon-FX branch at classes 18/20/115).
	Module_FindPattern(g_GameDll, "48 89 5C 24 ? 48 89 7C 24 ? 55 41 54 41 55 41 56 41 57 48 8D 6C 24")
		.GetPtr(v_VisClutter_AnimWeaponFx);

	// Entity-attached particle spawn (script, engine damage FX and TE paths
	// converge here; a2 is the ParticleEffectNames index).
	Module_FindPattern(g_GameDll, "48 8B C4 53 41 54 41 55 48 83 EC")
		.GetPtr(v_VisClutter_StartParticleOnEntity);

	// Index-based world particle create; the impact-table dispatcher spawns
	// every "shieldhit" entry through it.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 56 48 83 EC 30 48 8B 0D ? ? ? ? 49 8B F0 48 63 DA 4C 8B CB 8B C3 49 C1 F9 3F F7 D0")
		.GetPtr(v_VisClutter_CreateParticleByIndex);

	// Particle-manager object pointer: rip-relative load feeding the
	// name-to-index vtable call (+80). The pattern starts on
	// the mov opcode, so the disp32 sits at +3 and the next insn at +7.
	s_ppVisClutterParticleMgr = Module_FindPattern(g_GameDll,
		"48 8B 0D ? ? ? ? 48 8B D7 48 8B 01 FF 50 50 44 8B E0 3D FF FF 00 00")
		.ResolveRelativeAddress(0x3, 0x7)
		.RCast<void**>();

	if (!v_VisClutter_PlayWeaponFx)
		Warning(eDLL_T::CLIENT, "[VISCLUTTER] PlayWeaponFx pattern unresolved -- muzzle suppression off\n");
	if (!v_VisClutter_AnimWeaponFx)
		Warning(eDLL_T::CLIENT, "[VISCLUTTER] AnimWeaponFx pattern unresolved -- anim muzzle suppression off\n");
	if (!v_VisClutter_StartParticleOnEntity)
		Warning(eDLL_T::CLIENT, "[VISCLUTTER] StartParticleOnEntity pattern unresolved -- shield suppression off\n");
	if (!v_VisClutter_CreateParticleByIndex)
		Warning(eDLL_T::CLIENT, "[VISCLUTTER] CreateParticleByIndex pattern unresolved -- shield impact suppression off\n");
	if (!s_ppVisClutterParticleMgr)
		Warning(eDLL_T::CLIENT, "[VISCLUTTER] particle-mgr nav unresolved -- shield suppression off\n");
}

///////////////////////////////////////////////////////////////////////////////
void VVisualClutter::Detour(const bool bAttach) const
{
	if (v_VisClutter_PlayWeaponFx)
		DetourSetup(&v_VisClutter_PlayWeaponFx, &Hook_PlayWeaponFx, bAttach);
	else if (bAttach)
		Warning(eDLL_T::CLIENT, "[VISCLUTTER] disabled: PlayWeaponFx unresolved\n");

	if (v_VisClutter_AnimWeaponFx)
		DetourSetup(&v_VisClutter_AnimWeaponFx, &Hook_AnimWeaponFx, bAttach);
	else if (bAttach)
		Warning(eDLL_T::CLIENT, "[VISCLUTTER] disabled: AnimWeaponFx unresolved\n");

	if (v_VisClutter_StartParticleOnEntity && s_ppVisClutterParticleMgr)
		DetourSetup(&v_VisClutter_StartParticleOnEntity, &Hook_StartParticleOnEntity, bAttach);
	else if (bAttach)
		Warning(eDLL_T::CLIENT, "[VISCLUTTER] disabled: StartParticleOnEntity unresolved\n");

	if (v_VisClutter_CreateParticleByIndex && s_ppVisClutterParticleMgr)
		DetourSetup(&v_VisClutter_CreateParticleByIndex, &Hook_CreateParticleByIndex, bAttach);
	else if (bAttach)
		Warning(eDLL_T::CLIENT, "[VISCLUTTER] disabled: CreateParticleByIndex unresolved\n");
}
