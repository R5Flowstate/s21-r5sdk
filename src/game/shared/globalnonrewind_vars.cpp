//=============================================================================
//
// Purpose: GlobalNonRewind networked variable system.
//
//=============================================================================

#include "core/stdafx.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "globalnonrewind_vars.h"
#include "game/shared/scriptnetdata_ext.h"
#include "game/shared/scriptnetdata_limits.h"
#include "game/shared/edict_dirty.h"
#include "game/client/scriptnetdata_client.h"


#include <unordered_map>
#include <string>

// NonRewind array offsets must match the patched GLOBAL SNDC layout or SDK writes miss the SendProp bytes.
static constexpr int ENT_OFFSET_BOOLS    = 2944;  // m_bools[18], 1 byte each
static constexpr int ENT_OFFSET_RANGES   = 2962;  // m_ranges[34], 2 bytes each (uint16, 10-bit signed)
static constexpr int ENT_OFFSET_INT32S   = 3032;  // m_int32s[18], 4 bytes each
static constexpr int ENT_OFFSET_TIMES    = 3104;  // m_times[26], 4 bytes each (float)
static constexpr int ENT_OFFSET_ENTITIES = 3208;  // m_entities[10], 4 bytes each

// Must match NonRewind_FactoryGetSize in dt_extend.cpp (the CEntityFactory
// vtable[2] override that controls the actual allocation size).
static constexpr int kNonRewindEntitySize = 3384;

// Refuse writes past the entity allocation. Backstop if GetEntitySize and slot counts drift.
static inline bool NonRewindBoundsOk(const char* path, int offset, int elemBytes, const char* name)
{
	const int end = offset + elemBytes;
	if (end > kNonRewindEntitySize)
	{
		Warning(eDLL_T::SERVER,
			"[SNDC-BOUNDS] %s refused: name='%s' offset=%d elemBytes=%d end=%d > entitySize=%d\n",
			path, name ? name : "<null>", offset, elemBytes, end, kNonRewindEntitySize);
		return false;
	}
	return true;
}

// Direct SNDC array access. Encode/decode 10-bit signed packing used by m_ranges.
static inline uint16_t Range_Encode(int value) { return static_cast<uint16_t>(value & 0x3FF); }
static inline int      Range_Decode(uint16_t raw) { return (raw < 0x200) ? static_cast<int>(raw) : static_cast<int>(raw) - 1024; }

static bool EntitySetBool(const char* name, bool value)
{
	if (!g_pScriptNetDataNonRewindEnt) return false;
	int type = -1;
	int slot = ScriptNetData_FindVarSlotAndType(name, SNDC_GLOBAL_NON_REWIND, &type);
	if (slot < 0 || type != SNVT_BOOL) return false;
	const int offset = ENT_OFFSET_BOOLS + slot;
	if (!NonRewindBoundsOk("EntitySetBool", offset, 1, name)) return false;
	*reinterpret_cast<uint8_t*>(
		reinterpret_cast<uintptr_t>(g_pScriptNetDataNonRewindEnt) + offset) = value ? 1 : 0;
	MarkEntityEdictDirty(g_pScriptNetDataNonRewindEnt);
	return true;
}

static bool EntityGetBool(const char* name, bool& out)
{
	if (!g_pScriptNetDataNonRewindEnt) return false;
	int type = -1;
	int slot = ScriptNetData_FindVarSlotAndType(name, SNDC_GLOBAL_NON_REWIND, &type);
	if (slot < 0 || type != SNVT_BOOL) return false;
	out = *reinterpret_cast<uint8_t*>(
		reinterpret_cast<uintptr_t>(g_pScriptNetDataNonRewindEnt) + ENT_OFFSET_BOOLS + slot) != 0;
	return true;
}

// Dispatch on hash +0x0C SNVT: INT/UNSIGNED_INT -> m_ranges 10-bit; BIG_INT -> m_int32s.
static bool EntitySetInt(const char* name, int value)
{
	if (!g_pScriptNetDataNonRewindEnt) return false;
	int type = -1;
	int slot = ScriptNetData_FindVarSlotAndType(name, SNDC_GLOBAL_NON_REWIND, &type);
	if (slot < 0) return false;
	uintptr_t entBase = reinterpret_cast<uintptr_t>(g_pScriptNetDataNonRewindEnt);
	switch (type)
	{
	case SNVT_BIG_INT:
	{
		// Full int32 lives in m_int32s at slot*4.
		const int offset = ENT_OFFSET_INT32S + slot * 4;
		if (!NonRewindBoundsOk("EntitySetInt[BIG]", offset, 4, name)) return false;
		*reinterpret_cast<int32_t*>(entBase + offset) = value;
		break;
	}
	case SNVT_INT:
	case SNVT_UNSIGNED_INT:
	{
		// m_ranges 10-bit pack: signed -512..511 or unsigned 0..1023. Mask to 10 bits.
		const int offset = ENT_OFFSET_RANGES + slot * 2;
		if (!NonRewindBoundsOk("EntitySetInt[RANGE]", offset, 2, name)) return false;
		*reinterpret_cast<uint16_t*>(entBase + offset) = Range_Encode(value);
		break;
	}
	default:
		return false; // not an "int"-shaped var (script API misuse)
	}
	MarkEntityEdictDirty(g_pScriptNetDataNonRewindEnt);
	return true;
}

static bool EntityGetInt(const char* name, int& out)
{
	if (!g_pScriptNetDataNonRewindEnt) return false;
	int type = -1;
	int slot = ScriptNetData_FindVarSlotAndType(name, SNDC_GLOBAL_NON_REWIND, &type);
	if (slot < 0) return false;
	uintptr_t entBase = reinterpret_cast<uintptr_t>(g_pScriptNetDataNonRewindEnt);
	switch (type)
	{
	case SNVT_BIG_INT:
		out = *reinterpret_cast<int32_t*>(entBase + ENT_OFFSET_INT32S + slot * 4);
		return true;
	case SNVT_INT:
	case SNVT_UNSIGNED_INT:
	{
		uint16_t raw = *reinterpret_cast<uint16_t*>(entBase + ENT_OFFSET_RANGES + slot * 2);
		out = (type == SNVT_INT) ? Range_Decode(raw) : static_cast<int>(raw); // unsigned: no sign-bit reinterpret
		return true;
	}
	default:
		return false;
	}
}

// SNVT_TIME and FLOAT_RANGE_OVER_TIME store in m_times. SNVT_FLOAT_RANGE is refused here (no min/max).
static bool EntitySetFloat(const char* name, float value)
{
	if (!g_pScriptNetDataNonRewindEnt) return false;
	int type = -1;
	int slot = ScriptNetData_FindVarSlotAndType(name, SNDC_GLOBAL_NON_REWIND, &type);
	if (slot < 0) return false;
	uintptr_t entBase = reinterpret_cast<uintptr_t>(g_pScriptNetDataNonRewindEnt);
	switch (type)
	{
	case SNVT_TIME:
	case SNVT_FLOAT_RANGE_OVER_TIME:
	{
		const int offset = ENT_OFFSET_TIMES + slot * 4;
		if (!NonRewindBoundsOk("EntitySetFloat", offset, 4, name)) return false;
		*reinterpret_cast<float*>(entBase + offset) = value;
		break;
	}
	case SNVT_FLOAT_RANGE:
		return false; // needs min/max bounds, see comment above
	default:
		return false;
	}
	MarkEntityEdictDirty(g_pScriptNetDataNonRewindEnt);
	return true;
}

static bool EntityGetFloat(const char* name, float& out)
{
	if (!g_pScriptNetDataNonRewindEnt) return false;
	int type = -1;
	int slot = ScriptNetData_FindVarSlotAndType(name, SNDC_GLOBAL_NON_REWIND, &type);
	if (slot < 0) return false;
	uintptr_t entBase = reinterpret_cast<uintptr_t>(g_pScriptNetDataNonRewindEnt);
	switch (type)
	{
	case SNVT_TIME:
	case SNVT_FLOAT_RANGE_OVER_TIME:
		out = *reinterpret_cast<float*>(entBase + ENT_OFFSET_TIMES + slot * 4);
		return true;
	case SNVT_FLOAT_RANGE:
		return false; // see EntitySetFloat
	default:
		return false;
	}
}

//-----------------------------------------------------------------------------
// Fallback key-value storage (used when entity isn't available)
//-----------------------------------------------------------------------------
struct NonRewindVar_t
{
	enum Type { BOOL, INT, FLOAT, TIME, ENT };

	Type type = BOOL;
	union
	{
		bool bVal;
		int iVal;
		float fVal;
	};

	NonRewindVar_t() : type(BOOL), iVal(0) {}
};

static std::unordered_map<std::string, NonRewindVar_t> s_nonRewindVars;
static constexpr size_t MAX_NONREWIND_VARS = 1024;

//-----------------------------------------------------------------------------
// Setters
//-----------------------------------------------------------------------------
static constexpr size_t MAX_NONREWIND_NAME_LEN = 256;

static bool EnsureVarSlot(const char* name)
{
	if (s_nonRewindVars.count(name))
		return true;
	if (strlen(name) > MAX_NONREWIND_NAME_LEN)
	{
		Warning(eDLL_T::SERVER, "GlobalNonRewind: Variable name too long (%zu > %zu), rejecting\n",
			strlen(name), MAX_NONREWIND_NAME_LEN);
		return false;
	}
	if (s_nonRewindVars.size() >= MAX_NONREWIND_VARS)
	{
		Warning(eDLL_T::SERVER, "GlobalNonRewind: Variable limit reached (%zu), rejecting '%s'\n",
			MAX_NONREWIND_VARS, name);
		return false;
	}
	return true;
}
SQRESULT Script_SetGlobalNonRewindNetBool(HSQUIRRELVM v)
{
	const SQChar* name = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name)
		return SQ_ERROR;

	SQBool value;
	sq_getbool(v, 3, &value);

	int32_t oldVal = 0;
	auto it = s_nonRewindVars.find(name);
	if (it != s_nonRewindVars.end() && it->second.type == NonRewindVar_t::BOOL)
		oldVal = it->second.bVal ? 1 : 0;
	int32_t newVal = (value != 0) ? 1 : 0;

	if (EntitySetBool(name, value != 0))
	{
		if (newVal != oldVal)
			SNDC_QueueDeferredCallback(name, oldVal, newVal);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	if (!EnsureVarSlot(name))
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	NonRewindVar_t& var = s_nonRewindVars[name];
	var.type = NonRewindVar_t::BOOL;
	var.bVal = (value != 0);

	if (newVal != oldVal)
		SNDC_QueueDeferredCallback(name, oldVal, newVal);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_SetGlobalNonRewindNetInt(HSQUIRRELVM v)
{
	const SQChar* name = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name)
		return SQ_ERROR;

	SQInteger value;
	sq_getinteger(v, 3, &value);

	// Capture old value before Set for callback notification
	int32_t oldVal = 0;
	auto it = s_nonRewindVars.find(name);
	if (it != s_nonRewindVars.end() && it->second.type == NonRewindVar_t::INT)
		oldVal = it->second.iVal;

	if (EntitySetInt(name, static_cast<int>(value)))
	{
		// Entity path succeeded. Fire callback if value changed.
		if (static_cast<int32_t>(value) != oldVal)
			SNDC_QueueDeferredCallback(name, oldVal, static_cast<int32_t>(value));
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	if (!EnsureVarSlot(name))
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	NonRewindVar_t& var = s_nonRewindVars[name];
	var.type = NonRewindVar_t::INT;
	var.iVal = static_cast<int>(value);

	// Fire callback if value changed (KV fallback path)
	if (static_cast<int32_t>(value) != oldVal)
		SNDC_QueueDeferredCallback(name, oldVal, static_cast<int32_t>(value));

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Helper: reinterpret float bits as int32 for the callback queue.
// FireSingleCallback reinterprets back to float for SNVT_FLOAT_RANGE/SNVT_TIME callbacks.
static int32_t FloatBitsToInt(float f) { int32_t i; memcpy(&i, &f, sizeof(i)); return i; }

SQRESULT Script_SetGlobalNonRewindNetFloat(HSQUIRRELVM v)
{
	const SQChar* name = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name)
		return SQ_ERROR;

	SQFloat value;
	sq_getfloat(v, 3, &value);

	float oldF = 0.0f;
	auto it = s_nonRewindVars.find(name);
	if (it != s_nonRewindVars.end() && (it->second.type == NonRewindVar_t::FLOAT || it->second.type == NonRewindVar_t::TIME))
		oldF = it->second.fVal;
	float newF = static_cast<float>(value);

	if (EntitySetFloat(name, newF))
	{
		if (newF != oldF)
			SNDC_QueueDeferredCallback(name, FloatBitsToInt(oldF), FloatBitsToInt(newF));
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	if (!EnsureVarSlot(name))
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	NonRewindVar_t& var = s_nonRewindVars[name];
	var.type = NonRewindVar_t::FLOAT;
	var.fVal = newF;

	if (newF != oldF)
		SNDC_QueueDeferredCallback(name, FloatBitsToInt(oldF), FloatBitsToInt(newF));

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_SetGlobalNonRewindNetTime(HSQUIRRELVM v)
{
	const SQChar* name = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name)
		return SQ_ERROR;

	SQFloat value;
	sq_getfloat(v, 3, &value);

	float oldF = 0.0f;
	auto it = s_nonRewindVars.find(name);
	if (it != s_nonRewindVars.end() && (it->second.type == NonRewindVar_t::FLOAT || it->second.type == NonRewindVar_t::TIME))
		oldF = it->second.fVal;
	float newF = static_cast<float>(value);

	if (EntitySetFloat(name, newF))
	{
		if (newF != oldF)
			SNDC_QueueDeferredCallback(name, FloatBitsToInt(oldF), FloatBitsToInt(newF));
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	if (!EnsureVarSlot(name))
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	NonRewindVar_t& var = s_nonRewindVars[name];
	var.type = NonRewindVar_t::TIME;
	var.fVal = newF;

	if (newF != oldF)
		SNDC_QueueDeferredCallback(name, FloatBitsToInt(oldF), FloatBitsToInt(newF));

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Getters
//-----------------------------------------------------------------------------
SQRESULT Script_GetGlobalNonRewindNetBool(HSQUIRRELVM v)
{
	const SQChar* name = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name)
		return SQ_ERROR;

	bool bVal = false;
	if (EntityGetBool(name, bVal))
	{
		sq_pushbool(v, bVal);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	auto it = s_nonRewindVars.find(name);
	if (it != s_nonRewindVars.end() && it->second.type == NonRewindVar_t::BOOL)
		sq_pushbool(v, it->second.bVal);
	else
		sq_pushbool(v, false);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_GetGlobalNonRewindNetInt(HSQUIRRELVM v)
{
	const SQChar* name = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name)
		return SQ_ERROR;

	int iVal = 0;
	if (EntityGetInt(name, iVal))
	{
		sq_pushinteger(v, iVal);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	auto it = s_nonRewindVars.find(name);
	if (it != s_nonRewindVars.end() && it->second.type == NonRewindVar_t::INT)
		sq_pushinteger(v, it->second.iVal);
	else
		sq_pushinteger(v, 0);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_GetGlobalNonRewindNetFloat(HSQUIRRELVM v)
{
	const SQChar* name = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name)
		return SQ_ERROR;

	float fVal = 0.0f;
	if (EntityGetFloat(name, fVal))
	{
		sq_pushfloat(v, fVal);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	auto it = s_nonRewindVars.find(name);
	if (it != s_nonRewindVars.end() && it->second.type == NonRewindVar_t::FLOAT)
		sq_pushfloat(v, it->second.fVal);
	else
		sq_pushfloat(v, 0.0f);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_GetGlobalNonRewindNetTime(HSQUIRRELVM v)
{
	const SQChar* name = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name)
		return SQ_ERROR;

	float fVal = 0.0f;
	if (EntityGetFloat(name, fVal))
	{
		sq_pushfloat(v, fVal);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	auto it = s_nonRewindVars.find(name);
	if (it != s_nonRewindVars.end() && it->second.type == NonRewindVar_t::TIME)
		sq_pushfloat(v, it->second.fVal);
	else
		sq_pushfloat(v, 0.0f);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Set/Get by name; stores entity EHANDLE. m_RefEHandle offset in C_BaseEntity.
static constexpr int ENTITY_REFEHANDLE_OFFSET = 0x08;
static constexpr uint32_t NONREWIND_INVALID_EHANDLE = 0xFFFFFFFF;
static constexpr uint32_t NONREWIND_ENT_ENTRY_MASK  = 0xFFFF;

SQRESULT Script_SetGlobalNonRewindNetEnt(HSQUIRRELVM v)
{
	const SQChar* name = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name)
		return SQ_ERROR;

	if (!EnsureVarSlot(name))
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	int32_t oldVal = static_cast<int32_t>(NONREWIND_INVALID_EHANDLE);
	auto it = s_nonRewindVars.find(name);
	if (it != s_nonRewindVars.end() && it->second.type == NonRewindVar_t::ENT)
		oldVal = it->second.iVal;

	void* pEnt = nullptr;
	const bool gotEntity = v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt));

	NonRewindVar_t& var = s_nonRewindVars[name];
	var.type = NonRewindVar_t::ENT;
	var.iVal = (gotEntity && pEnt)
		? *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(pEnt) + ENTITY_REFEHANDLE_OFFSET)
		: NONREWIND_INVALID_EHANDLE;

	if (var.iVal != oldVal)
		SNDC_QueueDeferredCallback(name, oldVal, var.iVal);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_GetGlobalNonRewindNetEnt(HSQUIRRELVM v)
{
	const SQChar* name = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name)
		return SQ_ERROR;

	auto it = s_nonRewindVars.find(name);
	if (it == s_nonRewindVars.end() || it->second.type != NonRewindVar_t::ENT)
	{
		sq_pushnull(v);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}


	// Entity not found, deleted, or running on server — return null
	sq_pushnull(v);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Per-player NonRewind data (respawn time, music pack)
//-----------------------------------------------------------------------------
struct PlayerNonRewindData_t
{
	float respawnTime = 0.0f;
	int musicPack = 0;
};

// Keyed by EHANDLE (m_RefEHandle) instead of raw pointer to prevent ABA reuse
static std::unordered_map<uint32_t, PlayerNonRewindData_t> s_playerNonRewindData;

// Wire array is player-slot indexed; proxies must not walk the EHandle map.
static constexpr int kNonRewindMaxSlots = 128;
struct NonRewindSlotMisc_t
{
	float respawnTime;
	int musicPack;
};
static NonRewindSlotMisc_t s_slotMiscData[kNonRewindMaxSlots] = {};

// edictIdx at entity+0x58 (same field MarkEntityEdictDirty uses); slot = idx - 1.
static void WriteSlotMisc(void* pPlayer, float respawnTime, int musicPack, bool setRespawn, bool setMusic)
{
	if (!pPlayer)
		return;
	const int16_t edictIdx = *reinterpret_cast<int16_t*>(
		reinterpret_cast<uintptr_t>(pPlayer) + 0x58);
	const int slot = edictIdx - 1;
	if (edictIdx < 1 || slot >= kNonRewindMaxSlots)
		return;
	if (setRespawn)
		s_slotMiscData[slot].respawnTime = respawnTime;
	if (setMusic)
		s_slotMiscData[slot].musicPack = musicPack;
}

// Extract EHANDLE from entity pointer, returns NONREWIND_INVALID_EHANDLE on failure.
// Optionally hands back the entity pointer so callers need not re-resolve it.
static uint32_t GetPlayerEHandle(HSQUIRRELVM v, void** outEntity = nullptr)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
	{
		if (outEntity)
			*outEntity = nullptr;
		return NONREWIND_INVALID_EHANDLE;
	}
	if (outEntity)
		*outEntity = pPlayer;
	return static_cast<uint32_t>(
		*reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(pPlayer) + ENTITY_REFEHANDLE_OFFSET));
}

bool GlobalNonRewind_GetSlotMisc(int slot, float* outRespawnTime, int* outMusicPack)
{
	if (slot < 0 || slot >= kNonRewindMaxSlots)
		return false;
	if (outRespawnTime)
		*outRespawnTime = s_slotMiscData[slot].respawnTime;
	if (outMusicPack)
		*outMusicPack = s_slotMiscData[slot].musicPack;
	return true;
}

SQRESULT Script_GetNonRewindRespawnTime(HSQUIRRELVM v)
{
	const uint32_t ehandle = GetPlayerEHandle(v);
	if (ehandle == NONREWIND_INVALID_EHANDLE)
		return SQ_ERROR;

	auto it = s_playerNonRewindData.find(ehandle);
	float val = (it != s_playerNonRewindData.end()) ? it->second.respawnTime : 0.0f;

	sq_pushfloat(v, val);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_SetNonRewindRespawnTime(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	const uint32_t ehandle = GetPlayerEHandle(v, &pPlayer);
	if (ehandle == NONREWIND_INVALID_EHANDLE)
		return SQ_ERROR;

	SQFloat time;
	sq_getfloat(v, 2, &time);

	const float fl = static_cast<float>(time);
	s_playerNonRewindData[ehandle].respawnTime = fl;
	WriteSlotMisc(pPlayer, fl, 0, true, false);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_GetNonRewindMusicPack(HSQUIRRELVM v)
{
	const uint32_t ehandle = GetPlayerEHandle(v);
	if (ehandle == NONREWIND_INVALID_EHANDLE)
		return SQ_ERROR;

	auto it = s_playerNonRewindData.find(ehandle);
	int val = (it != s_playerNonRewindData.end()) ? it->second.musicPack : 0;

	sq_pushinteger(v, val);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_SetNonRewindMusicPack(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	const uint32_t ehandle = GetPlayerEHandle(v, &pPlayer);
	if (ehandle == NONREWIND_INVALID_EHANDLE)
		return SQ_ERROR;

	SQInteger pack;
	sq_getinteger(v, 2, &pack);

	const int nPack = static_cast<int>(pack);
	s_playerNonRewindData[ehandle].musicPack = nPack;
	WriteSlotMisc(pPlayer, 0.0f, nPack, false, true);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Level shutdown
//-----------------------------------------------------------------------------
void GlobalNonRewind_LevelShutdown()
{
	s_nonRewindVars.clear();
	s_playerNonRewindData.clear();
	memset(s_slotMiscData, 0, sizeof(s_slotMiscData));
}
