//=============================================================================//
//
// Purpose: Inject script-supplied extended-range use entities into the
// native use-candidate list on the dedicated server.
//
//=============================================================================//
#include "core/stdafx.h"
#include "extended_range_use.h"


#include "tier1/cvar.h"
#include "mathlib/mathlib.h"
#include "baseentity.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "vscript/languages/squirrel_re/include/sqobject.h"
#include "vscript/languages/squirrel_re/include/sqarray.h"
#include <cmath>

//-----------------------------------------------------------------------------
// FindUseCandidates_PartitionEnumerator layout (server)
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t ENUM_OFF_PLAYER          = 0x08;  // CBaseEntity* player
static constexpr ptrdiff_t ENUM_OFF_CONE_VERT       = 0x44;  // float coneVertCos2
static constexpr ptrdiff_t ENUM_OFF_CONE_HORZ       = 0x48;  // float coneHorzCos2
static constexpr ptrdiff_t ENUM_OFF_CANDIDATES      = 0x50;  // 64 x 24-byte entries
static constexpr ptrdiff_t ENUM_OFF_CANDIDATE_COUNT = 0x650; // int candidateCount
static constexpr int       ENUM_CANDIDATE_STRIDE    = 24;
static constexpr int       ENUM_CANDIDATE_CAP       = 64;
static constexpr int       EXT_USE_SCRIPT_ARRAY_CAP = 256;

// CBaseEntity (server half)
static constexpr ptrdiff_t ENT_OFF_USABLE_TYPE    = 0x624; // m_usableType
static constexpr ptrdiff_t ENT_OFF_USABLE_DIST_OV = 0x62C; // m_usableDistanceOverride

// Usable flags (engine-registered values)
static constexpr uint32_t USABLE_FROM_EXTENDED_RANGE   = 0x20;
static constexpr uint32_t USABLE_NO_LOS_REQUIREMENT    = 0x40;
static constexpr uint32_t USABLE_USE_DISTANCE_OVERRIDE = 0x2000;

// Instance/entity type bits on SQObject._type
static constexpr unsigned SQ_INSTANCE_ENTITY_MASK = 0x408000;

// SQ entity userdata: bound CBaseEntity* at instance+0x50
static constexpr ptrdiff_t SQ_ENTITY_USERPTR_OFF = 0x50;

static constexpr int EXT_USE_DIAG_EVERY = 32;

//-----------------------------------------------------------------------------
// Engine functions
//-----------------------------------------------------------------------------
typedef void*(__fastcall* FindPlayerUseCandidate_t)(CBaseEntity* player,
	const Vector3D* eyePos, const QAngle* eyeAng, float maxDistance,
	unsigned char sortReverse);
typedef __int64(__fastcall* EnumEntity_t)(void* pEnumerator, void* pHandleEntity);
typedef void(__fastcall* UseCandidateSort_t)(void* pStart, void* pEnd, __int64 count,
	unsigned char reverse);
typedef void*(__fastcall* FindUseEntity_t)(CBaseEntity* player);
// Post-LOS "is the hit still on this usable entity?" -- only called when the
// eye->usePos trace was blocked. S3 has no USABLE_NO_LOS_REQUIREMENT path.
typedef bool(__fastcall* UseHitInEntityBounds_t)(const float* hitPos, CBaseEntity* pEnt);

static FindPlayerUseCandidate_t v_FindPlayerUseCandidate = nullptr;
static EnumEntity_t             v_FindUseCandidates_EnumEntity = nullptr;
static UseCandidateSort_t       v_UseCandidateSort = nullptr;
static FindUseEntity_t          v_CPlayer_FindUseEntity = nullptr;
static UseHitInEntityBounds_t   v_UseHitInEntityBounds = nullptr;

// FindPlayerUseCandidate only calls the candidate sort when spatial count > 0.
// Pure extended-range targets (remote deathbox, Void Nexus at range) leave the
// list empty, so the inject seam never runs without this gate NOP.
// Site is unique on the server half (client twin has a different jle displacement).
static CMemory s_pAlwaysSortGate;

// Set for the duration of FindPlayerUseCandidate so the sort hook knows the
// sort belongs to a use search.
static CBaseEntity* s_pUseSearchPlayer = nullptr;

// Resolved once per VM lifetime; cleared when the VM goes away so a rebuilt
// script VM re-resolves instead of calling through a dead handle.
static HSCRIPT s_hCallback = nullptr;
static bool s_bCallbackResolvedLatched = false;
static bool s_bCallbackMissingLatched = false;
static bool s_bAlwaysSortGatePatched = false;
static bool s_bUsableBoundPatched = false;

// S3 Set/Add/RemoveUsableValue reject with `if (arg > 0x200000)`. That is a
// scalar ceiling, not a bit-mask check -- so stock
//   USABLE_EXTENDED_USE | USABLE_FROM_EXTENDED_RANGE | ...
// (0x200868) errors even though every individual flag is valid.
// Raise the ceiling to 0x3FFFFF (bits 0..21 cover the whole recovered enum).
static constexpr uint32_t USABLE_VALUE_BOUND_OLD = 0x200000;
static constexpr uint32_t USABLE_VALUE_BOUND_NEW = 0x3FFFFF;

//-----------------------------------------------------------------------------
// ConVars
//-----------------------------------------------------------------------------
static ConVar bridge_ext_use("bridge_ext_use", "1", FCVAR_RELEASE,
	"Inject script extended-range use entities into the native candidate list.");
static ConVar bridge_ext_use_cone_vert("bridge_ext_use_cone_vert", "3", FCVAR_RELEASE,
	"Extended-range use vertical cone half-angle in degrees.");
static ConVar bridge_ext_use_cone_horz("bridge_ext_use_cone_horz", "5", FCVAR_RELEASE,
	"Extended-range use horizontal cone half-angle in degrees.");
static ConVar bridge_ext_use_diag("bridge_ext_use_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[EXT-USE] rate-limited per-search injection tracing.");

//-----------------------------------------------------------------------------
// coneCos2(deg) = max(0, cos(deg))^2 -- enumerator stores squared cosines
//-----------------------------------------------------------------------------
static float ExtendedUse_ConeCos2(const float deg)
{
	const float f = fmaxf(0.0f, cosf(deg * (M_PI_F / 180.0f)));
	return f * f;
}

static CBaseEntity* ExtendedUse_EntityFromSQObject(const SQObject& el)
{
	if ((el._type & SQ_INSTANCE_ENTITY_MASK) == 0 || !el._unVal.pInstance)
		return nullptr;

	return *reinterpret_cast<CBaseEntity**>(
		reinterpret_cast<uintptr_t>(el._unVal.pInstance) + SQ_ENTITY_USERPTR_OFF);
}

static bool ExtendedUse_AlreadyInList(const uint8_t* const pEnum, CBaseEntity* const pEnt,
	const int count)
{
	const uint8_t* const pCand = pEnum + ENUM_OFF_CANDIDATES;
	const int n = (count < ENUM_CANDIDATE_CAP) ? count : ENUM_CANDIDATE_CAP;
	for (int i = 0; i < n; ++i)
	{
		CBaseEntity* const pListed = *reinterpret_cast<CBaseEntity* const*>(
			pCand + ENUM_CANDIDATE_STRIDE * i);
		if (pListed == pEnt)
			return true;
	}
	return false;
}

//-----------------------------------------------------------------------------
// Pull CodeCallback_GetExtendedRangeUseEntitiesForPlayer results into the
// enumerator via EnumEntity under the extended cones.
//-----------------------------------------------------------------------------
static void ExtendedUse_InjectScriptEntities(void* const pEnumRaw)
{
	uint8_t* const pEnum = static_cast<uint8_t*>(pEnumRaw);
	CBaseEntity* const player = *reinterpret_cast<CBaseEntity**>(pEnum + ENUM_OFF_PLAYER);
	if (!g_pServerScript || !player)
		return;

	// FindFunction hands back an engine-allocated handle that no call site in
	// either tree frees, so resolving by name here would leak once per use
	// search per player per tick. Resolve once per VM and reuse.
	if (!s_hCallback)
	{
		s_hCallback = g_pServerScript->FindFunction(
			"CodeCallback_GetExtendedRangeUseEntitiesForPlayer", nullptr, nullptr);
		if (!s_hCallback)
		{
			if (!s_bCallbackMissingLatched)
			{
				s_bCallbackMissingLatched = true;
				Warning(eDLL_T::SERVER,
					"[EXT-USE] CodeCallback_GetExtendedRangeUseEntitiesForPlayer not found -- "
					"extended-range injection idle until scripts register it\n");
			}
			return;
		}

		s_bCallbackMissingLatched = false;
		Msg(eDLL_T::SERVER,
			"[EXT-USE] CodeCallback_GetExtendedRangeUseEntitiesForPlayer resolved\n");
	}
	s_bCallbackResolvedLatched = true;

	const HSCRIPT hPlayer = player->GetScriptInstance();
	if (!hPlayer)
		return;

	ScriptVariant_t args[1];
	args[0] = hPlayer;
	ScriptVariant_t ret;
	// Player-reachable and runs every use search: an uncaught VM error here
	// schedules HS_GAME_SHUTDOWN, which the player reads as a random kick.
	const ScriptStatus_t status = g_pServerScript->ExecuteFunction(
		s_hCallback, args, 1, &ret, nullptr);
	if (status != SCRIPT_DONE)
	{
		static uint32_t s_nErr = 0;
		if (++s_nErr <= 8 || (s_nErr % 200) == 0)
			Warning(eDLL_T::SERVER,
				"[EXT-USE] CodeCallback_GetExtendedRangeUseEntitiesForPlayer SCRIPT_ERROR "
				"(#%u) -- injection skipped for this search\n", s_nErr);
		return;
	}
	if (ret.m_type != FIELD_HSCRIPT || !ret.m_hScript)
		return;

	const SQObject* const pRetObj = reinterpret_cast<const SQObject*>(ret.m_hScript);
	if (pRetObj->_type != OT_ARRAY || !_array(*pRetObj))
		return;

	const SQArray* const pArr = _array(*pRetObj);
	const SQInteger nSize = pArr->Size();
	if (nSize <= 0)
		return;

	const int beforeCount = *reinterpret_cast<int*>(pEnum + ENUM_OFF_CANDIDATE_COUNT);

	// Save normal cones; swap in extended cones for the injection pass only.
	const float savedConeVert = *reinterpret_cast<float*>(pEnum + ENUM_OFF_CONE_VERT);
	const float savedConeHorz = *reinterpret_cast<float*>(pEnum + ENUM_OFF_CONE_HORZ);
	*reinterpret_cast<float*>(pEnum + ENUM_OFF_CONE_VERT) =
		ExtendedUse_ConeCos2(bridge_ext_use_cone_vert.GetFloat());
	*reinterpret_cast<float*>(pEnum + ENUM_OFF_CONE_HORZ) =
		ExtendedUse_ConeCos2(bridge_ext_use_cone_horz.GetFloat());

	int nScripted = 0;
	int nInjected = 0;
	int nDup = 0;
	const bool bDiag = bridge_ext_use_diag.GetBool();
	static unsigned s_nDiagCounter = 0;
	const bool bDiagLine = bDiag && ((++s_nDiagCounter % EXT_USE_DIAG_EVERY) == 0);

	// Diagnostic detail buffer (fixed, no heap)
	char diagDetail[512];
	diagDetail[0] = '\0';
	int diagDetailLen = 0;

	const SQInteger nWalk = (nSize < EXT_USE_SCRIPT_ARRAY_CAP)
		? nSize : static_cast<SQInteger>(EXT_USE_SCRIPT_ARRAY_CAP);

	for (SQInteger i = 0; i < nWalk; ++i)
	{
		const SQObjectPtr& el = pArr->_values[i];
		if ((el._type & SQ_INSTANCE_ENTITY_MASK) == 0)
			continue;

		CBaseEntity* const pEnt = ExtendedUse_EntityFromSQObject(el);
		if (!pEnt)
			continue;

		++nScripted;

		const int curCount = *reinterpret_cast<int*>(pEnum + ENUM_OFF_CANDIDATE_COUNT);
		if (ExtendedUse_AlreadyInList(pEnum, pEnt, curCount))
		{
			++nDup;
			continue;
		}

		if (curCount >= ENUM_CANDIDATE_CAP)
			continue;

		uint32_t* const pUsable = reinterpret_cast<uint32_t*>(
			reinterpret_cast<uintptr_t>(pEnt) + ENT_OFF_USABLE_TYPE);
		const uint32_t savedUsable = *pUsable;

		// Extended-list entities with USABLE_FROM_EXTENDED_RANGE use the
		// distance override without needing USABLE_USE_DISTANCE_OVERRIDE.
		if ((savedUsable & USABLE_FROM_EXTENDED_RANGE) &&
			!(savedUsable & USABLE_USE_DISTANCE_OVERRIDE))
		{
			*pUsable = savedUsable | USABLE_USE_DISTANCE_OVERRIDE;
		}

		const __int64 enumRet = v_FindUseCandidates_EnumEntity
			? v_FindUseCandidates_EnumEntity(pEnum, pEnt)
			: 0;

		*pUsable = savedUsable;

		const int afterCount = *reinterpret_cast<int*>(pEnum + ENUM_OFF_CANDIDATE_COUNT);
		if (afterCount > curCount)
		{
			++nInjected;
			if (bDiagLine && diagDetailLen < static_cast<int>(sizeof(diagDetail)) - 64)
			{
				const float distOv = *reinterpret_cast<const float*>(
					reinterpret_cast<uintptr_t>(pEnt) + ENT_OFF_USABLE_DIST_OV);
				const int n = V_snprintf(diagDetail + diagDetailLen,
					sizeof(diagDetail) - static_cast<size_t>(diagDetailLen),
					" | %p usable=0x%x distOv=%.1f",
					pEnt, savedUsable, distOv);
				if (n > 0)
					diagDetailLen += n;
			}
		}

		if (enumRet != 0)
			break;
	}

	*reinterpret_cast<float*>(pEnum + ENUM_OFF_CONE_VERT) = savedConeVert;
	*reinterpret_cast<float*>(pEnum + ENUM_OFF_CONE_HORZ) = savedConeHorz;

	if (bDiagLine)
	{
		Msg(eDLL_T::SERVER,
			"[EXT-USE] before=%d script=%d injected=%d dup=%d%s\n",
			beforeCount, nScripted, nInjected, nDup, diagDetail);
	}
}

//-----------------------------------------------------------------------------
// The script VM is torn down and rebuilt across a level change, so the cached
// callback handle must not survive it.
//-----------------------------------------------------------------------------
void ExtendedUse_LevelShutdown(void)
{
	s_hCallback = nullptr;
	s_bCallbackResolvedLatched = false;
	s_bCallbackMissingLatched = false;
}

//-----------------------------------------------------------------------------
// Hook 1 -- scope gate for the sort hook
//-----------------------------------------------------------------------------
static void* __fastcall Hook_FindPlayerUseCandidate(CBaseEntity* player,
	const Vector3D* eyePos, const QAngle* eyeAng, float maxDistance,
	unsigned char sortReverse)
{
	s_pUseSearchPlayer = player;
	void* const result = v_FindPlayerUseCandidate(player, eyePos, eyeAng,
		maxDistance, sortReverse);
	s_pUseSearchPlayer = nullptr;
	return result;
}

//-----------------------------------------------------------------------------
// Hook 3 -- honor USABLE_NO_LOS_REQUIREMENT on the post-trace accept path
// FindPlayerUseCandidate: if fraction!=1, call this; accept on true.
// S3 never reads bit 0x40; later builds skip the LOS gate when it is set.
//-----------------------------------------------------------------------------
static bool __fastcall Hook_UseHitInEntityBounds(const float* hitPos, CBaseEntity* pEnt)
{
	if (pEnt)
	{
		const uint32_t usable = *reinterpret_cast<const uint32_t*>(
			reinterpret_cast<uintptr_t>(pEnt) + ENT_OFF_USABLE_TYPE);
		if (usable & USABLE_NO_LOS_REQUIREMENT)
			return true;
	}
	return v_UseHitInEntityBounds(hitPos, pEnt);
}

//-----------------------------------------------------------------------------
// Hook 2 -- inject between spatial fill and ranking
//-----------------------------------------------------------------------------
static void __fastcall Hook_UseCandidateSort(void* pStart, void* pEnd, __int64 count,
	unsigned char reverse)
{
	// Sort recurses into itself; consume the gate so nested partitions pass through.
	CBaseEntity* const pSearchPlayer = s_pUseSearchPlayer;
	s_pUseSearchPlayer = nullptr;

	if (!pSearchPlayer || !bridge_ext_use.GetBool() ||
		!v_FindUseCandidates_EnumEntity)
	{
		v_UseCandidateSort(pStart, pEnd, count, reverse);
		return;
	}

	uint8_t* const pEnum = static_cast<uint8_t*>(pStart) - ENUM_OFF_CANDIDATES;
	const int enumCount = *reinterpret_cast<int*>(pEnum + ENUM_OFF_CANDIDATE_COUNT);
	CBaseEntity* const enumPlayer = *reinterpret_cast<CBaseEntity**>(pEnum + ENUM_OFF_PLAYER);

	if (enumCount != static_cast<int>(count) ||
		enumPlayer != pSearchPlayer ||
		count < 0 || count > ENUM_CANDIDATE_CAP)
	{
		v_UseCandidateSort(pStart, pEnd, count, reverse);
		return;
	}

	ExtendedUse_InjectScriptEntities(pEnum);

	const int newCount = *reinterpret_cast<int*>(pEnum + ENUM_OFF_CANDIDATE_COUNT);
	void* const pNewEnd = static_cast<uint8_t*>(pStart) + ENUM_CANDIDATE_STRIDE * newCount;
	v_UseCandidateSort(pStart, pNewEnd, newCount, reverse);
}

//-----------------------------------------------------------------------------
// VExtendedRangeUse
//-----------------------------------------------------------------------------
void VExtendedRangeUse::GetAdr(void) const
{
	LogFunAdr("CPlayer::FindPlayerUseCandidate", v_FindPlayerUseCandidate);
	LogFunAdr("FindUseCandidates_PartitionEnumerator::EnumEntity", v_FindUseCandidates_EnumEntity);
	LogFunAdr("UseCandidateSort", v_UseCandidateSort);
	LogFunAdr("CPlayer::FindUseEntity", v_CPlayer_FindUseEntity);
	LogFunAdr("UseHitInEntityBounds (post-LOS accept)", v_UseHitInEntityBounds);
	LogVarAdr("FindPlayerUseCandidate always-sort gate",
		reinterpret_cast<const void*>(s_pAlwaysSortGate.GetPtr()));
}

void VExtendedRangeUse::GetFun(void) const
{
	// CPlayer::FindPlayerUseCandidate -- human 210 / titan 450 search entry
	Module_FindPattern(g_GameDll, "48 8B C4 F3 0F 11 58 ? 55 57")
		.GetPtr(v_FindPlayerUseCandidate);

	// FindUseCandidates_PartitionEnumerator::EnumEntity
	Module_FindPattern(g_GameDll, "40 55 56 57 41 54 48 8D AC 24")
		.GetPtr(v_FindUseCandidates_EnumEntity);

	// use-candidate sort (std::_Sort_unchecked)
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 48 89 7C 24 ? "
		"41 54 41 56 41 57 48 83 EC ? 48 8B F2 41 0F B6 D9")
		.GetPtr(v_UseCandidateSort);

	// CPlayer::FindUseEntity -- resolve only; supplies the hardcoded radii
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC ? 48 8B D9 48 8B 0D ? ? ? ? 48 8B 01 FF 90 ? ? ? ? 84 C0 74 ? 33 C0")
		.GetPtr(v_CPlayer_FindUseEntity);

	// Post-LOS hit-in-bounds check (called only when eye->usePos fraction != 1)
	Module_FindPattern(g_GameDll,
		"48 8B C4 48 89 58 ? 57 48 81 EC ? ? ? ? 0F 29 70 ? "
		"48 8B DA 0F 29 78 ? 48 8B F9 44 0F 29 40")
		.GetPtr(v_UseHitInEntityBounds);

	// test eax,eax; jle skip -- only the server half matches the exact displacement.
	// NOP the jle so the sort (and inject) always runs after the spatial fill.
	s_pAlwaysSortGate = Module_FindPattern(g_GameDll,
		"85 C0 0F 8E C9 01 00 00 44 0F B6 8D F0 08 00 00");

	if (!v_FindPlayerUseCandidate || !v_FindUseCandidates_EnumEntity || !v_UseCandidateSort)
	{
		Warning(eDLL_T::SERVER,
			"[EXT-USE] pattern unresolved -- extended-range use disabled\n");
	}
	if (!v_UseHitInEntityBounds)
	{
		Warning(eDLL_T::SERVER,
			"[EXT-USE] UseHitInEntityBounds unresolved -- USABLE_NO_LOS_REQUIREMENT "
			"will not be honored on the dedi\n");
	}
	if (!s_pAlwaysSortGate)
	{
		Warning(eDLL_T::SERVER,
			"[EXT-USE] always-sort gate unresolved -- pure extended-range use "
			"(empty near list) will not inject\n");
	}
}

void VExtendedRangeUse::Detour(const bool bAttach) const
{
	if (v_FindPlayerUseCandidate)
		DetourSetup(&v_FindPlayerUseCandidate, &Hook_FindPlayerUseCandidate, bAttach);

	if (v_UseCandidateSort)
		DetourSetup(&v_UseCandidateSort, &Hook_UseCandidateSort, bAttach);

	if (v_UseHitInEntityBounds)
		DetourSetup(&v_UseHitInEntityBounds, &Hook_UseHitInEntityBounds, bAttach);

	if (bAttach && s_pAlwaysSortGate && !s_bAlwaysSortGatePatched)
	{
		// NOP the 6-byte jle that skips sort when spatial count <= 0.
		// After sort returns, the engine reloads count from the enumerator and
		// will walk any entities the inject path added.
		s_pAlwaysSortGate.Offset(0x2).Patch(
			{ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 });
		s_bAlwaysSortGatePatched = true;
		Msg(eDLL_T::SERVER,
			"[EXT-USE] always-sort gate patched -- inject runs with empty near list\n");
	}

	if (bAttach && !s_bUsableBoundPatched)
	{
		// Five unique `cmp edx, 0x200000` sites (native Set, thin Add/Remove
		// wrappers, script Add/Remove). Imm is at +2; raise to 0x3FFFFF.
		// Module_FindPattern needs a string literal (compile-time sig).
		const CMemory sites[] = {
			Module_FindPattern(g_GameDll, "81 FA 00 00 20 00 0F 87 F2 01 00 00"),             // SetUsableValue
			Module_FindPattern(g_GameDll, "81 FA 00 00 20 00 0F 86 A4 F9 FF FF"),             // AddUsableValue wrapper
			Module_FindPattern(g_GameDll, "81 FA 00 00 20 00 0F 86 A4 FB FF FF"),             // RemoveUsableValue wrapper
			Module_FindPattern(g_GameDll, "81 FA 00 00 20 00 77 0C 48 8B 4C 24 38 E8 C0 FB"), // ScriptRemoveUsableValue
			Module_FindPattern(g_GameDll, "81 FA 00 00 20 00 77 0C 48 8B 4C 24 38 E8 20 F9"), // ScriptAddUsableValue
		};

		int nPatched = 0;
		for (int i = 0; i < 5; ++i)
		{
			if (!sites[i])
			{
				Warning(eDLL_T::SERVER,
					"[EXT-USE] usable-value bound site unresolved (index %d)\n", i);
				continue;
			}

			// LE: 0x3FFFFF
			sites[i].Offset(0x2).Patch({ 0xFF, 0xFF, 0x3F, 0x00 });
			++nPatched;
		}

		s_bUsableBoundPatched = true;
		Msg(eDLL_T::SERVER,
			"[EXT-USE] usable-value bound raised 0x%x -> 0x%x at %d/5 sites "
			"(stock EXTENDED_USE | FROM_EXTENDED_RANGE ORs now legal)\n",
			USABLE_VALUE_BOUND_OLD, USABLE_VALUE_BOUND_NEW, nPatched);
	}
}

