//=============================================================================//
//
// Purpose: 256 status-effect types on the dedicated server.
//
// The S3 dedi packs a status effect as severity<<7 | generation<<15 | type<<25,
// so the type is 7 bits and the name table holds 128. The S21 client already
// reads an 8-bit type (bits 24-31) and parses 256 names. This takes one bit
// from the 10-bit generation counter, moves the type down to bit 24, grows the
// name table to 256 and replaces the one native that sized a per-type buffer
// at 128. The wire proxy in dt_extend.cpp repacks for the client.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier0/memory_patch.h"
#include "game/shared/status_effect.h"
#include "status_effect_capacity.h"
#include "thirdparty/detours/include/detours.h"

static constexpr int STATUS_EFFECT_TYPES_MAX = 256;

enum StatusEffectAnchor_t
{
	SEA_SLOT_FIND,      // timed-slot finder
	SEA_STOP_TYPE,      // stop every effect of a type
	SEA_PAUSE,          // pause/unpause by handle mask
	SEA_SEVERITY,       // max severity + time of a type
	SEA_FILL,           // per-type severity fill for GetAll
	SEA_ADD_TIMED,
	SEA_ADD_ENDLESS,
	SEA_GENERATION,
	SEA_LOOKUP,         // name -> type
	SEA_PARSE,          // status_effect_types.txt
	SEA_ENUM,           // eStatusEffect script enum
	SEA_PAUSE_NATIVE,   // pause-by-type script native (interior anchor)
	SEA_SMART_AMMO_A,
	SEA_SMART_AMMO_B,

	SEA_COUNT
};

struct StatusEffectSite_t
{
	StatusEffectAnchor_t m_nAnchor;
	uint16_t m_nOffset;
	uint8_t m_nLen;
	uint8_t m_Old[7];
	uint8_t m_New[7];
};

// sar r32,25 -> shr r32,24 and "and r32,7Fh" -> "and r32,-1" keep every
// instruction the same length.
#define SE_DECODE(anchor, sarOff, andOff, modrm, andModrm) \
	{ anchor, sarOff, 3, { 0xC1, modrm, 0x19 }, { 0xC1, static_cast<uint8_t>(modrm - 0x10), 0x18 } }, \
	{ anchor, andOff, 3, { 0x83, andModrm, 0x7F }, { 0x83, andModrm, 0xFF } }

static const StatusEffectSite_t s_statusEffectSites[] =
{
	SE_DECODE(SEA_SLOT_FIND, 131, 134, 0xF8, 0xE0),
	SE_DECODE(SEA_SLOT_FIND, 224, 227, 0xF8, 0xE0),
	SE_DECODE(SEA_SLOT_FIND, 310, 313, 0xFA, 0xE2),
	SE_DECODE(SEA_SLOT_FIND, 391, 394, 0xFA, 0xE2),
	SE_DECODE(SEA_SLOT_FIND, 491, 494, 0xF8, 0xE0),

	SE_DECODE(SEA_STOP_TYPE, 356, 359, 0xF8, 0xE0),
	SE_DECODE(SEA_STOP_TYPE, 419, 422, 0xF8, 0xE0),

	SE_DECODE(SEA_SEVERITY, 473, 476, 0xF8, 0xE0),
	SE_DECODE(SEA_SEVERITY, 585, 588, 0xF8, 0xE0),
	SE_DECODE(SEA_SEVERITY, 637, 640, 0xF8, 0xE0),
	SE_DECODE(SEA_SEVERITY, 690, 693, 0xF8, 0xE0),
	SE_DECODE(SEA_SEVERITY, 743, 746, 0xF8, 0xE0),
	SE_DECODE(SEA_SEVERITY, 831, 834, 0xF8, 0xE0),

	SE_DECODE(SEA_FILL, 412, 415, 0xF8, 0xE0),
	SE_DECODE(SEA_FILL, 489, 495, 0xFA, 0xE2),
	SE_DECODE(SEA_FILL, 531, 537, 0xFA, 0xE2),
	SE_DECODE(SEA_FILL, 574, 580, 0xFA, 0xE2),
	SE_DECODE(SEA_FILL, 617, 623, 0xFA, 0xE2),
	SE_DECODE(SEA_FILL, 695, 701, 0xF9, 0xE1),

	// Name lookup for the endless-pause error message.
	{ SEA_PAUSE, 0x129, 4, { 0x48, 0xC1, 0xE8, 0x19 }, { 0x48, 0xC1, 0xE8, 0x18 } },

	// Pause-by-type: handle mask and type shift.
	{ SEA_PAUSE_NATIVE, 0x03, 6, { 0x41, 0xB8, 0x00, 0x00, 0x00, 0xFE }, { 0x41, 0xB8, 0x00, 0x00, 0x00, 0xFF } },
	{ SEA_PAUSE_NATIVE, 0x10, 4, { 0x41, 0xC1, 0xE2, 0x19 }, { 0x41, 0xC1, 0xE2, 0x18 } },

	// Encoders: type<<10 becomes type<<9 so it lands on bit 24, and the
	// generation wraps at 512 so it stays below it.
	{ SEA_ADD_TIMED, 0x193, 7, { 0x41, 0x81, 0xE0, 0xFF, 0x03, 0x00, 0x00 }, { 0x41, 0x81, 0xE0, 0xFF, 0x01, 0x00, 0x00 } },
	{ SEA_ADD_TIMED, 0x1D8, 4, { 0x41, 0xC1, 0xE6, 0x0A }, { 0x41, 0xC1, 0xE6, 0x09 } },
	{ SEA_ADD_ENDLESS, 0x1C7, 4, { 0x41, 0xC1, 0xE7, 0x0A }, { 0x41, 0xC1, 0xE7, 0x09 } },
	{ SEA_GENERATION, 0x12, 7, { 0x41, 0x81, 0xE1, 0xFF, 0x03, 0x00, 0x00 }, { 0x41, 0x81, 0xE1, 0xFF, 0x01, 0x00, 0x00 } },

	// Parser: clear and fill 256 names.
	{ SEA_PARSE, 0xE0, 6, { 0x41, 0xB8, 0x00, 0x04, 0x00, 0x00 }, { 0x41, 0xB8, 0x00, 0x08, 0x00, 0x00 } },
	{ SEA_PARSE, 0x107, 6, { 0x81, 0xF9, 0x80, 0x00, 0x00, 0x00 }, { 0x81, 0xF9, 0x00, 0x01, 0x00, 0x00 } },
};

#undef SE_DECODE

// Every "lea reg, [name table]" -- retargeted at the 256-entry copy.
struct StatusEffectTableRef_t
{
	StatusEffectAnchor_t m_nAnchor;
	uint16_t m_nOffset;
};

static const StatusEffectTableRef_t s_statusEffectTableRefs[] =
{
	{ SEA_LOOKUP,       0x17 },
	{ SEA_PARSE,        0xD4 },
	{ SEA_ENUM,         0x11C },
	{ SEA_PAUSE,        0x122 },
	{ SEA_SMART_AMMO_A, 0xAA },
	{ SEA_SMART_AMMO_B, 0x204 },
};

static uint8_t* s_pAnchors[SEA_COUNT];
static bool s_bWideTypes = false;

static void* (*v_StatusEffect_GetAllSeverities)(void* pOut, void* pEntity) = nullptr;
static void (*v_StatusEffect_FillSeverities)(void* pEntity, float* pSeverities) = nullptr;
static void (*v_ScriptArray_Create)(void* hVM, void* pOut, bool bUnk) = nullptr;
static void (*v_ScriptArray_Append)(void* hVM, void* hArray, const void* pValue) = nullptr;
static void** g_pServerScriptVMHandle = nullptr;
static int* g_pStatusEffectTypeCount = nullptr;

bool StatusEffects_WideTypes(void)
{
	return s_bWideTypes;
}

//-----------------------------------------------------------------------------
// Purpose: GetAll with a per-type buffer sized for 256 (stock stack buffer is 128)
//-----------------------------------------------------------------------------
static void* StatusEffect_GetAllSeverities(void* pOut, void* pEntity)
{
	if (!pEntity)
		return v_StatusEffect_GetAllSeverities(pOut, pEntity);

	// Same entity gate as the stock native: player, titan soul, NPC.
	void** const vtable = *reinterpret_cast<void***>(pEntity);
	typedef bool (*EntityPredicate_t)(void*);
	if (!reinterpret_cast<EntityPredicate_t>(vtable[744 / 8])(pEntity)
		&& !reinterpret_cast<EntityPredicate_t>(vtable[560 / 8])(pEntity)
		&& !reinterpret_cast<EntityPredicate_t>(vtable[760 / 8])(pEntity))
	{
		return v_StatusEffect_GetAllSeverities(pOut, pEntity);
	}

	float severities[STATUS_EFFECT_TYPES_MAX] = {};
	v_StatusEffect_FillSeverities(pEntity, severities);

	void* const hVM = *g_pServerScriptVMHandle;

	alignas(16) uint8_t array[16] = {};
	v_ScriptArray_Create(hVM, array, true);

	const int count = Min(*g_pStatusEffectTypeCount, STATUS_EFFECT_TYPES_MAX);
	void* const hArray = *reinterpret_cast<void**>(array);
	for (int i = 0; i < count; ++i)
	{
		// Script variant: value at +0, type tag at +0xC (0x10000 = float).
		alignas(16) uint8_t value[16] = {};
		memcpy(value, &severities[i], sizeof(float));
		*reinterpret_cast<uint32_t*>(value + 0xC) = 0x10000;
		v_ScriptArray_Append(hVM, hArray, value);
	}

	memcpy(pOut, array, sizeof(array));
	return pOut;
}

void VStatusEffectCapacity::GetAdr(void) const
{
	LogFunAdr("StatusEffect_GetAllSeverities", v_StatusEffect_GetAllSeverities);
	LogFunAdr("StatusEffect_FillSeverities", v_StatusEffect_FillSeverities);
	LogVarAdr("g_pStatusEffectTypeCount", g_pStatusEffectTypeCount);
}

void VStatusEffectCapacity::GetFun(void) const
{
	s_pAnchors[SEA_SLOT_FIND] = Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 57 48 8B 05 AB 91 8B 0C 33 C9 49 8B F9").RCast<uint8_t*>();
	s_pAnchors[SEA_STOP_TYPE] = Module_FindPattern(g_GameDll,
		"48 89 5C 24 18 55 56 57 41 54 41 57 48 83 EC 30 48 8B 01 44 8B E2 48 8B F1 FF 90 E8 02 00 00 84 C0 74 1B").RCast<uint8_t*>();
	s_pAnchors[SEA_PAUSE] = Module_FindPattern(g_GameDll,
		"44 88 4C 24 20 53 55 56 41 54 41 55 48 83 EC 60 48 8B 01 45 8B E0").RCast<uint8_t*>();
	s_pAnchors[SEA_SEVERITY] = Module_FindPattern(g_GameDll,
		"48 8B C4 48 89 68 10 48 89 70 18 57 41 54 41 55 41 56 41 57 48 83 EC 70 44 0F B6 AC 24 C0 00 00 00").RCast<uint8_t*>();
	s_pAnchors[SEA_FILL] = Module_FindPattern(g_GameDll,
		"48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 41 54 41 56 41 57 48 83 EC 50 0F 29 70 D8 41 BF FF FF FF FF").RCast<uint8_t*>();
	s_pAnchors[SEA_ADD_TIMED] = Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 50 48 8B 01 44 8B F2").RCast<uint8_t*>();
	s_pAnchors[SEA_ADD_ENDLESS] = Module_FindPattern(g_GameDll,
		"40 53 55 41 57 48 83 EC 30 0F 29 74 24 20 0F 28 F2 44 8B FA").RCast<uint8_t*>();
	s_pAnchors[SEA_GENERATION] = Module_FindPattern(g_GameDll,
		"4C 8B 15 F1 9A 8B 0C 41 8B 82 2C 0B 00 00 44 8D 48 01 41 81 E1 FF 03 00 00").RCast<uint8_t*>();
	s_pAnchors[SEA_LOOKUP] = Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 44 8B 1D 6C 8D 8B 0C 45 33 C9 48 8B D9 45 85 DB 7E 4C 4C 8D 15 F2 21 8E 28").RCast<uint8_t*>();
	s_pAnchors[SEA_PARSE] = Module_FindPattern(g_GameDll,
		"4C 8B DC 48 81 EC 28 01 00 00 83 A4 24 90 00 00 00 FC").RCast<uint8_t*>();
	s_pAnchors[SEA_ENUM] = Module_FindPattern(g_GameDll,
		"41 56 48 83 EC 30 48 89 5C 24 40 48 8B D9 48 89 74 24 50 48 89 7C 24 58 E8 D3 FA FF FF").RCast<uint8_t*>();
	s_pAnchors[SEA_PAUSE_NATIVE] = Module_FindPattern(g_GameDll,
		"45 85 DB 41 B8 00 00 00 FE 48 8B C8 41 0F 95 C1 41 C1 E2 19 41 8B D2 E8").RCast<uint8_t*>();
	s_pAnchors[SEA_SMART_AMMO_A] = Module_FindPattern(g_GameDll,
		"40 53 55 41 56 41 57 48 83 EC 48 48 8B 02 48 8B E9 48 8B CA 4C 8B F2 FF 90 80 04 00 00 4C 63 95 50 14 00 00").RCast<uint8_t*>();
	s_pAnchors[SEA_SMART_AMMO_B] = Module_FindPattern(g_GameDll,
		"48 89 4C 24 08 53 57 41 55 41 57 48 83 EC 48 48 8B 02 4C 8B F9 48 8B CA 4C 89 74 24 30").RCast<uint8_t*>();

	CMemory getAll = Module_FindPattern(g_GameDll,
		"40 53 55 56 48 81 EC 40 02 00 00 48 8B DA 48 8B E9 48 85 D2 0F 84 04 01 00 00 48 8B 02 48 8B CA FF 90 E8 02 00 00 84 C0");
	if (!getAll)
		return;

	getAll.GetPtr(v_StatusEffect_GetAllSeverities);
	getAll.Offset(0x83).FollowNearCallSelf().GetPtr(v_StatusEffect_FillSeverities);
	g_pServerScriptVMHandle = getAll.Offset(0x88).ResolveRelativeAddress(3, 7).RCast<void**>();
	getAll.Offset(0xA2).FollowNearCallSelf().GetPtr(v_ScriptArray_Create);
	g_pStatusEffectTypeCount = getAll.Offset(0xA7).ResolveRelativeAddress(2, 6).RCast<int*>();
	getAll.Offset(0xF1).FollowNearCallSelf().GetPtr(v_ScriptArray_Append);
}

static bool StatusEffectCapacity_Verify(uint8_t*& pOldTable)
{
	for (int i = 0; i < SEA_COUNT; ++i)
	{
		if (!s_pAnchors[i])
		{
			Warning(eDLL_T::SERVER, "[SE-CAPACITY] anchor %d not found\n", i);
			return false;
		}
	}

	if (!v_StatusEffect_GetAllSeverities || !v_StatusEffect_FillSeverities || !v_ScriptArray_Create
		|| !v_ScriptArray_Append || !g_pServerScriptVMHandle || !g_pStatusEffectTypeCount)
	{
		Warning(eDLL_T::SERVER, "[SE-CAPACITY] GetAll native not resolved\n");
		return false;
	}

	for (const StatusEffectSite_t& site : s_statusEffectSites)
	{
		const uint8_t* const p = s_pAnchors[site.m_nAnchor] + site.m_nOffset;
		if (memcmp(p, site.m_Old, site.m_nLen) != 0)
		{
			Warning(eDLL_T::SERVER, "[SE-CAPACITY] site %d+0x%X bytes differ\n",
				site.m_nAnchor, site.m_nOffset);
			return false;
		}
	}

	pOldTable = nullptr;
	for (const StatusEffectTableRef_t& ref : s_statusEffectTableRefs)
	{
		const uint8_t* const p = s_pAnchors[ref.m_nAnchor] + ref.m_nOffset;
		if ((p[0] != 0x48 && p[0] != 0x4C) || p[1] != 0x8D || (p[2] & 0xC7) != 0x05)
		{
			Warning(eDLL_T::SERVER, "[SE-CAPACITY] table ref %d+0x%X is not a rip lea\n",
				ref.m_nAnchor, ref.m_nOffset);
			return false;
		}

		uint8_t* const pTarget = const_cast<uint8_t*>(p) + 7 + *reinterpret_cast<const int32_t*>(p + 3);
		if (pOldTable && pTarget != pOldTable)
		{
			Warning(eDLL_T::SERVER, "[SE-CAPACITY] table ref %d+0x%X points elsewhere\n",
				ref.m_nAnchor, ref.m_nOffset);
			return false;
		}
		pOldTable = pTarget;
	}

	return true;
}

void VStatusEffectCapacity::Detour(const bool bAttach) const
{
	if (!bAttach)
	{
		if (s_bWideTypes)
			DetourSetup(&v_StatusEffect_GetAllSeverities, &StatusEffect_GetAllSeverities, false);
		return;
	}

	uint8_t* pOldTable = nullptr;
	if (!StatusEffectCapacity_Verify(pOldTable))
	{
		Warning(eDLL_T::SERVER, "[SE-CAPACITY] not applied -- status effects stay capped at 128 types\n");
		return;
	}

	uint8_t* const pNewTable = Mem_AllocNearModule(g_GameDll, STATUS_EFFECT_TYPES_MAX * sizeof(void*));
	if (!pNewTable)
	{
		Warning(eDLL_T::SERVER, "[SE-CAPACITY] no memory near the module -- not applied\n");
		return;
	}
	memcpy(pNewTable, pOldTable, 128 * sizeof(void*));

	for (const StatusEffectTableRef_t& ref : s_statusEffectTableRefs)
	{
		uint8_t* const p = s_pAnchors[ref.m_nAnchor] + ref.m_nOffset;
		const int32_t disp = static_cast<int32_t>(pNewTable - (p + 7));
		Mem_PatchCode(p + 3, &disp, sizeof(disp));
	}

	for (const StatusEffectSite_t& site : s_statusEffectSites)
		Mem_PatchCode(s_pAnchors[site.m_nAnchor] + site.m_nOffset, site.m_New, site.m_nLen);

	DetourSetup(&v_StatusEffect_GetAllSeverities, &StatusEffect_GetAllSeverities, true);
	s_bWideTypes = true;

	Msg(eDLL_T::SERVER, "[SE-CAPACITY] %d status-effect types (%zu sites, table @ %p)\n",
		STATUS_EFFECT_TYPES_MAX, ARRAYSIZE(s_statusEffectSites) + ARRAYSIZE(s_statusEffectTableRefs), pNewTable);
}
