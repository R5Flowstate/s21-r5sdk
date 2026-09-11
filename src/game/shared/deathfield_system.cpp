#if defined(CLIENT_DLL)
// No client-side deathfield code. The S21 client binds these natives itself
// against CWorld RecvTable arrays. The SDK must not shadow them.

#else // !CLIENT_DLL
//=============================================================================//
//
// Purpose: Multi-index deathfield system for realm-based gameplay.
// Side-table[64] is source of truth; index 0 also writes CWorld scalars.
//
//=============================================================================//

#include "core/stdafx.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "deathfield_system.h"
#include "sdk_entity_state.h"
#include "public/globalvars_base.h"
#include "tier0/module.h"
#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/detours.h"

#include "public/edict.h"
#include "game/shared/edict_dirty.h"
#include "game/server/util_server.h"
#include "game/server/player.h"
#include "engine/server/server.h"
#include "engine/client/client.h"
extern CGlobalVars* gpGlobals;

#include <cmath>
#include <utility>

//-----------------------------------------------------------------------------
// Constants
//-----------------------------------------------------------------------------
static constexpr int MAX_DEATHFIELDS = 64;
static constexpr float DF_INACTIVE_RADIUS = 3.4028235e38f;

// S3 CWorld single-ring scalars. SetDeathFieldParams writes these.
static constexpr int WORLD_DF_ISACTIVE     = 0xB3C;
static constexpr int WORLD_DF_ORIGIN       = 0xB40;
static constexpr int WORLD_DF_RADIUS_START = 0xB4C;
static constexpr int WORLD_DF_RADIUS_END   = 0xB50;
static constexpr int WORLD_DF_TIME_START   = 0xB54;
static constexpr int WORLD_DF_TIME_END     = 0xB58;

//-----------------------------------------------------------------------------
// Per-deathfield data
//-----------------------------------------------------------------------------
struct DeathFieldData_t
{
	bool isActive = false;
	float originX = 0.0f;
	float originY = 0.0f;
	float originZ = 0.0f;
	float radiusStart = 0.0f;
	float radiusEnd = 0.0f;
	float timeStart = 0.0f;
	float timeEnd = 0.0f;
};

static DeathFieldData_t s_deathFields[MAX_DEATHFIELDS];

static void (__fastcall* v_SetDeathFieldParams)(float* center, float radiusStart,
	float radiusEnd, float timeStart, float timeEnd) = nullptr;

// Script DisableHibernation writes +0xEC=4 then this. No-ops when handle is -1
// (CreateEntity, before DispatchSpawn). Must run again after the edict exists.
static void (__fastcall* v_CBaseEntity_DisableHibernationApply)(void* entity) = nullptr;
// UpdateTransmitState: desired mode is +0xF0, applied is +0xF4. 4 = always awake.
static void (__fastcall* v_CBaseEntity_UpdateTransmitState)(void* entity) = nullptr;
// Clears hibernating (+0x234 bit 0x8000000) and edict flag 0x400.
static void (__fastcall* v_CBaseEntity_LeaveHibernation)(void* entity) = nullptr;
// SendProxy for DPT_String string_t: pOut = *pData (or ""). Offset 0x238 is a
// pointer. An in-place StringToString proxy here ships the pointer bytes.
static void (__fastcall* v_SendProxy_StringT)(void*, void*, const char**, const char**) = nullptr;

//-----------------------------------------------------------------------------
// Helpers
//-----------------------------------------------------------------------------
static inline void* GetWorldEntity()
{
	if (!g_ppWorldEntity || !*g_ppWorldEntity)
		return nullptr;
	return *g_ppWorldEntity;
}

static float GetCurrentRadius_SideTable(const DeathFieldData_t& df, float time)
{
	if (!df.isActive)
		return DF_INACTIVE_RADIUS;

	if (df.timeStart == df.timeEnd)
	{
		if ((time - df.timeEnd) < 0.0f)
			return df.radiusStart;
		return df.radiusEnd;
	}

	float t = (time - df.timeStart) / (df.timeEnd - df.timeStart);
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	return df.radiusStart + (df.radiusEnd - df.radiusStart) * t;
}

static float GetCurrentRadius(int index, float time)
{
	if (index < 0 || index >= MAX_DEATHFIELDS)
		return DF_INACTIVE_RADIUS;

	return GetCurrentRadius_SideTable(s_deathFields[index], time);
}

static void DeathField_DirtyWorld()
{
	if (void* w = GetWorldEntity())
		MarkEntityEdictDirty(w);
}

static void DeathField_WriteNativeRing0(const DeathFieldData_t& df)
{
	void* w = GetWorldEntity();
	if (!w)
		return;

	const uintptr_t b = reinterpret_cast<uintptr_t>(w);
	*reinterpret_cast<uint8_t*>(b + WORLD_DF_ISACTIVE) = df.isActive ? 1 : 0;
	*reinterpret_cast<float*>(b + WORLD_DF_ORIGIN + 0) = df.originX;
	*reinterpret_cast<float*>(b + WORLD_DF_ORIGIN + 4) = df.originY;
	*reinterpret_cast<float*>(b + WORLD_DF_ORIGIN + 8) = df.originZ;
	*reinterpret_cast<float*>(b + WORLD_DF_RADIUS_START) = df.radiusStart;
	*reinterpret_cast<float*>(b + WORLD_DF_RADIUS_END) = df.radiusEnd;
	*reinterpret_cast<float*>(b + WORLD_DF_TIME_START) = df.timeStart;
	*reinterpret_cast<float*>(b + WORLD_DF_TIME_END) = df.timeEnd;
}

static void DeathField_CaptureNativeRing0()
{
	void* w = GetWorldEntity();
	if (!w)
		return;

	const uintptr_t b = reinterpret_cast<uintptr_t>(w);
	DeathFieldData_t& df = s_deathFields[0];
	df.isActive    = *reinterpret_cast<uint8_t*>(b + WORLD_DF_ISACTIVE) != 0;
	df.originX     = *reinterpret_cast<float*>(b + WORLD_DF_ORIGIN + 0);
	df.originY     = *reinterpret_cast<float*>(b + WORLD_DF_ORIGIN + 4);
	df.originZ     = *reinterpret_cast<float*>(b + WORLD_DF_ORIGIN + 8);
	df.radiusStart = *reinterpret_cast<float*>(b + WORLD_DF_RADIUS_START);
	df.radiusEnd   = *reinterpret_cast<float*>(b + WORLD_DF_RADIUS_END);
	df.timeStart   = *reinterpret_cast<float*>(b + WORLD_DF_TIME_START);
	df.timeEnd     = *reinterpret_cast<float*>(b + WORLD_DF_TIME_END);
}

static uint16_t DeathField_ReadEdictFlags(int16_t edictIdx);

static void DeathField_Commit(int index)
{
	if (index == 0)
		DeathField_WriteNativeRing0(s_deathFields[0]);

	void* const world = GetWorldEntity();
	int16_t edictIdx = -1;
	uint16_t eflagsBefore = 0;
	if (world)
	{
		edictIdx = *reinterpret_cast<int16_t*>(
			reinterpret_cast<uint8_t*>(world) + 0x58);
		eflagsBefore = DeathField_ReadEdictFlags(edictIdx);
	}

	DeathField_DirtyWorld();

	static volatile LONG s_commitN = 0;
	const LONG n = InterlockedIncrement(&s_commitN);
	if (n <= 8)
	{
		if (!world)
		{
			Warning(eDLL_T::SERVER,
				"[DEATHFIELD-COMMIT] #%ld idx=%d world=(null) edict=0 eflags=0x0000->0x0000 active=0 r=0->0\n",
				static_cast<long>(n), index);
		}
		else
		{
			const uint16_t eflagsAfter = DeathField_ReadEdictFlags(edictIdx);
			const DeathFieldData_t& df = s_deathFields[index];
			Warning(eDLL_T::SERVER,
				"[DEATHFIELD-COMMIT] #%ld idx=%d world=%p edict=%d eflags=0x%04X->0x%04X active=%d r=%.0f->%.0f\n",
				static_cast<long>(n), index, world, static_cast<int>(edictIdx),
				eflagsBefore, eflagsAfter,
				df.isActive ? 1 : 0, df.radiusStart, df.radiusEnd);
		}
	}
}

// S21: (player.mask & ent.mask)==0 is match-none. Staging strips bit 0
// (DEFAULT); the ring entity is created in DEFAULT only.
static constexpr ptrdiff_t DF_ENT_OFF_REALMSBITMASK = 0xAE8;
static constexpr uint64_t DF_REALM_BIT_DEFAULT = 1ull;

static void DeathField_EnsurePlayersInDefaultRealm(void)
{
	if (!gpGlobals || !g_pServer)
		return;

	const int nMax = gpGlobals->maxClients;
	for (int i = 0; i < nMax; i++)
	{
		const CClient* const pClient = g_pServer->GetClient(i);
		if (!pClient || !pClient->IsActive())
			continue;

		CPlayer* const pPlayer = UTIL_PlayerByIndex(pClient->GetHandle());
		if (!pPlayer || !pPlayer->IsConnected())
			continue;

		uint64_t* const pMask = reinterpret_cast<uint64_t*>(
			reinterpret_cast<uint8_t*>(pPlayer) + DF_ENT_OFF_REALMSBITMASK);
		const uint64_t before = *pMask;
		if ((before & DF_REALM_BIT_DEFAULT) != 0)
			continue;

		*pMask = before | DF_REALM_BIT_DEFAULT;
		MarkEntityEdictDirty(pPlayer);

		static volatile LONG s_stampN = 0;
		if (InterlockedIncrement(&s_stampN) <= 8)
		{
			Warning(eDLL_T::SERVER,
				"[DEATHFIELD] player handle=%d realmMask 0x%llx -> 0x%llx (DEFAULT)\n",
				pClient->GetHandle(),
				static_cast<unsigned long long>(before),
				static_cast<unsigned long long>(*pMask));
		}
	}
}

static void DeathField_ApplyParams(int index, float x, float y, float z,
	float radiusStart, float radiusEnd, float timeStart, float timeEnd)
{
	if (index < 0 || index >= MAX_DEATHFIELDS)
		return;

	DeathFieldData_t& df = s_deathFields[index];
	df.isActive    = true;
	df.originX     = x;
	df.originY     = y;
	df.originZ     = z;
	df.radiusStart = radiusStart;
	df.radiusEnd   = radiusEnd;
	df.timeStart   = timeStart;
	df.timeEnd     = timeEnd;
	DeathField_EnsurePlayersInDefaultRealm();
	DeathField_Commit(index);
}

static bool DeathField_PrintableCString(const char* s)
{
	if (!s)
		return false;
	const unsigned char c = static_cast<unsigned char>(s[0]);
	return c >= 0x20 && c < 0x7F;
}

static uint16_t DeathField_ReadEdictFlags(int16_t edictIdx)
{
	if (edictIdx < 0 || !gpGlobals || !gpGlobals->m_pEdicts)
		return 0;
	uint16_t* const flagsSlot =
		reinterpret_cast<uint16_t*>(gpGlobals->m_pEdicts) + edictIdx + 32;
	return *flagsSlot;
}

static constexpr uint32_t DF_ST_PROPS        = 0x00;
static constexpr uint32_t DF_ST_NPROPS       = 0x08;
static constexpr uint32_t DF_ST_NETTABLENAME = 0x4B8;
static constexpr uint32_t DF_SP_SIZE         = 0x88;
static constexpr uint32_t DF_SP_TYPE         = 0x00;
static constexpr uint32_t DF_SP_VARNAME      = 0x40;
static constexpr uint32_t DF_SP_FLAGS        = 0x58;
static constexpr uint32_t DF_SP_PROXY        = 0x60;
static constexpr uint32_t DF_SP_CHILDTABLE   = 0x70;
static constexpr uint32_t DF_SP_OFFSET       = 0x78;
static constexpr int      DF_NAME_OFF        = 0x238;
static constexpr int      DF_SIGNIFIER_OFF   = 0x518;

static uint8_t* DeathField_FindSendProp(uint8_t* table, const char* propName, int depth)
{
	if (!table || !propName || depth > 16)
		return nullptr;
	uint8_t* const props = *reinterpret_cast<uint8_t**>(table + DF_ST_PROPS);
	const int nProps = *reinterpret_cast<int*>(table + DF_ST_NPROPS);
	if (!props || nProps <= 0 || nProps > 4096)
		return nullptr;
	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* const p = props + static_cast<uint64_t>(i) * DF_SP_SIZE;
		const char* const nm = *reinterpret_cast<const char**>(p + DF_SP_VARNAME);
		if (DeathField_PrintableCString(nm) && strcmp(nm, propName) == 0)
			return p;
	}
	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* const p = props + static_cast<uint64_t>(i) * DF_SP_SIZE;
		if (*reinterpret_cast<int*>(p + DF_SP_TYPE) != 10)
			continue;
		uint8_t* const child = *reinterpret_cast<uint8_t**>(p + DF_SP_CHILDTABLE);
		if (uint8_t* const found = DeathField_FindSendProp(child, propName, depth + 1))
			return found;
	}
	return nullptr;
}

static void DeathField_DumpAndFixNameProp(void* entity)
{
	static volatile LONG s_once = 0;
	if (InterlockedCompareExchange(&s_once, 1, 0) != 0)
		return;

	uint8_t* const pEnt = reinterpret_cast<uint8_t*>(entity);
	const uintptr_t serverClass = *reinterpret_cast<const uintptr_t*>(pEnt + 0x50);
	uint8_t* const table = serverClass
		? *reinterpret_cast<uint8_t**>(serverClass + 0x08)
		: nullptr;
	const char* const tableName = table
		? *reinterpret_cast<const char**>(table + DF_ST_NETTABLENAME)
		: nullptr;
	uint8_t* const prop = DeathField_FindSendProp(table, "m_iName", 0);
	if (!prop)
	{
		Warning(eDLL_T::SERVER,
			"[DEATHFIELD] m_iName sendprop MISSING table='%s'\n",
			DeathField_PrintableCString(tableName) ? tableName : "?");
		return;
	}

	const int type = *reinterpret_cast<int*>(prop + DF_SP_TYPE);
	const int flags = *reinterpret_cast<int*>(prop + DF_SP_FLAGS);
	const int offRaw = *reinterpret_cast<int*>(prop + DF_SP_OFFSET);
	const int off = offRaw & 0xFFFFF;
	void* const proxy = *reinterpret_cast<void**>(prop + DF_SP_PROXY);
	const bool proxyOk = v_SendProxy_StringT && proxy == v_SendProxy_StringT;
	Warning(eDLL_T::SERVER,
		"[DEATHFIELD] m_iName table='%s' type=%d off=0x%X flags=0x%X proxy=%p stock=%p\n",
		DeathField_PrintableCString(tableName) ? tableName : "?",
		type, off, flags, proxy, reinterpret_cast<void*>(v_SendProxy_StringT));

	bool repaired = false;
	if (type != 4)
	{
		*reinterpret_cast<int*>(prop + DF_SP_TYPE) = 4;
		repaired = true;
	}
	if (off != DF_NAME_OFF)
	{
		*reinterpret_cast<int*>(prop + DF_SP_OFFSET) =
			(offRaw & ~0xFFFFF) | DF_NAME_OFF;
		repaired = true;
	}
	if (v_SendProxy_StringT && !proxyOk)
	{
		*reinterpret_cast<void**>(prop + DF_SP_PROXY) =
			reinterpret_cast<void*>(v_SendProxy_StringT);
		repaired = true;
	}
	if (repaired)
		Warning(eDLL_T::SERVER,
			"[DEATHFIELD] m_iName sendprop repaired type=%d off=0x%X proxy=%p\n",
			*reinterpret_cast<int*>(prop + DF_SP_TYPE),
			*reinterpret_cast<int*>(prop + DF_SP_OFFSET) & 0xFFFFF,
			*reinterpret_cast<void**>(prop + DF_SP_PROXY));

	uint8_t* const sigProp = DeathField_FindSendProp(table, "m_iSignifierName", 0);
	if (!sigProp)
	{
		Warning(eDLL_T::SERVER,
			"[DEATHFIELD] m_iSignifierName sendprop MISSING table='%s'\n",
			DeathField_PrintableCString(tableName) ? tableName : "?");
		return;
	}
	const int sigType = *reinterpret_cast<int*>(sigProp + DF_SP_TYPE);
	const int sigOffRaw = *reinterpret_cast<int*>(sigProp + DF_SP_OFFSET);
	const int sigOff = sigOffRaw & 0xFFFFF;
	void* const sigProxy = *reinterpret_cast<void**>(sigProp + DF_SP_PROXY);
	Warning(eDLL_T::SERVER,
		"[DEATHFIELD] m_iSignifierName table='%s' type=%d off=0x%X proxy=%p stock=%p\n",
		DeathField_PrintableCString(tableName) ? tableName : "?",
		sigType, sigOff, sigProxy, reinterpret_cast<void*>(v_SendProxy_StringT));
	if (sigType != 4)
	{
		*reinterpret_cast<int*>(sigProp + DF_SP_TYPE) = 4;
		Warning(eDLL_T::SERVER,
			"[DEATHFIELD] m_iSignifierName type repaired -> 4\n");
	}
	if (v_SendProxy_StringT && sigProxy != v_SendProxy_StringT)
	{
		*reinterpret_cast<void**>(sigProp + DF_SP_PROXY) =
			reinterpret_cast<void*>(v_SendProxy_StringT);
		Warning(eDLL_T::SERVER,
			"[DEATHFIELD] m_iSignifierName proxy repaired -> stock\n");
	}
}

static void Signifier_PublishForSpawn(void* entity)
{
	if (!entity)
		return;

	uint8_t* const pEnt = reinterpret_cast<uint8_t*>(entity);
	const uintptr_t serverClass = *reinterpret_cast<const uintptr_t*>(pEnt + 0x50);
	if (!serverClass)
		return;
	uint8_t* const table = *reinterpret_cast<uint8_t**>(serverClass + 0x08);
	if (!table)
		return;

	struct SignifierOffCache_t
	{
		uintptr_t serverClass;
		int off;
	};
	static SignifierOffCache_t s_offCache[128] = {};
	static int s_offCacheN = 0;

	int off = 0;
	bool cached = false;
	for (int i = 0; i < s_offCacheN; ++i)
	{
		if (s_offCache[i].serverClass == serverClass)
		{
			off = s_offCache[i].off;
			cached = true;
			break;
		}
	}
	if (!cached)
	{
		uint8_t* const prop = DeathField_FindSendProp(table, "m_iSignifierName", 0);
		if (!prop)
			return;
		off = *reinterpret_cast<int*>(prop + DF_SP_OFFSET) & 0xFFFFF;
		if (off <= 0 || off > 0x4000)
			return;
		if (s_offCacheN < 128)
		{
			s_offCache[s_offCacheN].serverClass = serverClass;
			s_offCache[s_offCacheN].off = off;
			++s_offCacheN;
		}
	}

	const char* const existing = *reinterpret_cast<const char* const*>(pEnt + off);
	if (DeathField_PrintableCString(existing))
		return;

	const char* const mapClass = *reinterpret_cast<const char* const*>(pEnt + 0x78);
	if (!DeathField_PrintableCString(mapClass))
		return;

	*reinterpret_cast<const char**>(pEnt + off) = mapClass;
	MarkEntityEdictDirty(entity);

	static volatile LONG s_writeN = 0;
	if (InterlockedIncrement(&s_writeN) <= 8)
	{
		const char* const className = *reinterpret_cast<const char* const*>(serverClass);
		Warning(eDLL_T::SERVER, "[SIGNIFIER] class=%s off=0x%X -> '%s'\n",
			DeathField_PrintableCString(className) ? className : "?",
			static_cast<unsigned>(off), mapClass);
	}
}

void DeathField_OnEntitySpawned(void* entity)
{
	if (!entity)
		return;

	Signifier_PublishForSpawn(entity);

	uint8_t* const pEnt = reinterpret_cast<uint8_t*>(entity);
	const uintptr_t serverClass = *reinterpret_cast<const uintptr_t*>(pEnt + 0x50);
	const char* className = nullptr;
	if (serverClass)
		className = *reinterpret_cast<const char* const*>(serverClass);
	const char* const mapClass = *reinterpret_cast<const char* const*>(pEnt + 0x78);
	const char* const modelName = *reinterpret_cast<const char* const*>(pEnt + 0x68);

	const bool isCScriptMover = DeathField_PrintableCString(className) &&
		strcmp(className, "CScriptMover") == 0;
	if (!isCScriptMover)
		return;

	const int16_t edictIdx = *reinterpret_cast<int16_t*>(pEnt + 0x58);
	const uint32_t handle = *reinterpret_cast<uint32_t*>(pEnt + 0x08);
	const uint16_t modelIdx = *reinterpret_cast<uint16_t*>(pEnt + 0xDE);
	const uint32_t hibModeWant = *reinterpret_cast<uint32_t*>(pEnt + 0xF0);
	const uint32_t hibModeHave = *reinterpret_cast<uint32_t*>(pEnt + 0xF4);
	const uint32_t hibFieldEc = *reinterpret_cast<uint32_t*>(pEnt + 0xEC);
	const uint32_t hibFlags = *reinterpret_cast<uint32_t*>(pEnt + 0x234);
	const uint16_t edictFlagsBefore = DeathField_ReadEdictFlags(edictIdx);

	uint64_t* const pMask = reinterpret_cast<uint64_t*>(pEnt + DF_ENT_OFF_REALMSBITMASK);
	const uint64_t before = *pMask;

	if ((before & DF_REALM_BIT_DEFAULT) == 0)
		*pMask = before | DF_REALM_BIT_DEFAULT;

	// Script native writes +0xEC. DispatchSpawn / UpdateTransmitState honor +0xF0/+0xF4.
	*reinterpret_cast<uint32_t*>(pEnt + 0xEC) = 4;
	*reinterpret_cast<uint32_t*>(pEnt + 0xF0) = 4;

	if (v_CBaseEntity_UpdateTransmitState)
		v_CBaseEntity_UpdateTransmitState(entity);
	if (v_CBaseEntity_LeaveHibernation)
		v_CBaseEntity_LeaveHibernation(entity);
	if (v_CBaseEntity_DisableHibernationApply)
		v_CBaseEntity_DisableHibernationApply(entity);

	int sigOff = DF_SIGNIFIER_OFF;
	const uintptr_t sc = *reinterpret_cast<const uintptr_t*>(pEnt + 0x50);
	uint8_t* const sigTable = sc ? *reinterpret_cast<uint8_t**>(sc + 0x08) : nullptr;
	if (uint8_t* const sigProp = DeathField_FindSendProp(sigTable, "m_iSignifierName", 0))
		sigOff = *reinterpret_cast<int*>(sigProp + DF_SP_OFFSET) & 0xFFFFF;
	if (sigOff <= 0 || sigOff > 0x4000)
		sigOff = DF_SIGNIFIER_OFF;
	const char* signifier = *reinterpret_cast<const char* const*>(pEnt + sigOff);
	if (!DeathField_PrintableCString(signifier) && DeathField_PrintableCString(mapClass))
	{
		*reinterpret_cast<const char**>(pEnt + sigOff) = mapClass;
		signifier = mapClass;
	}

	MarkEntityEdictDirty(entity);

	const uint16_t edictFlagsAfter = DeathField_ReadEdictFlags(edictIdx);
	const char* const iName = *reinterpret_cast<const char* const*>(pEnt + 0x238);
	const int scriptIdx = *reinterpret_cast<const int*>(pEnt + 0x240);
	const float* const localOrg = reinterpret_cast<const float*>(pEnt + 0x554);
	DeathField_DumpAndFixNameProp(entity);
	Warning(eDLL_T::SERVER,
		"[DEATHFIELD] spawn class=%s map=%s name='%s' signifier='%s' scriptIdx=%d ent=%p edict=%d handle=0x%08X "
		"local=(%.0f,%.0f,%.0f) "
		"mask=0x%llx->0x%llx +0xEC=%u->%u +0xF0=%u->%u +0xF4=%u->%u "
		"+0x234=0x%x->0x%x eflags=0x%04X->0x%04X model=%u '%s'\n",
		DeathField_PrintableCString(className) ? className : "?",
		DeathField_PrintableCString(mapClass) ? mapClass : "?",
		DeathField_PrintableCString(iName) ? iName : "",
		DeathField_PrintableCString(signifier) ? signifier : "",
		scriptIdx,
		entity, static_cast<int>(edictIdx), handle,
		localOrg[0], localOrg[1], localOrg[2],
		static_cast<unsigned long long>(before),
		static_cast<unsigned long long>(*pMask),
		hibFieldEc, *reinterpret_cast<uint32_t*>(pEnt + 0xEC),
		hibModeWant, *reinterpret_cast<uint32_t*>(pEnt + 0xF0),
		hibModeHave, *reinterpret_cast<uint32_t*>(pEnt + 0xF4),
		hibFlags, *reinterpret_cast<uint32_t*>(pEnt + 0x234),
		edictFlagsBefore, edictFlagsAfter,
		static_cast<unsigned>(modelIdx),
		DeathField_PrintableCString(modelName) ? modelName : "?");
}

//-----------------------------------------------------------------------------
// Native-DT value proxies -- one function per (field, ring). Ring is baked into
// the function identity; SP_OFFSET is only a non-colliding changeframe slot.
// pOut is the 24-byte DVariant (scalar at [0], Vector at [0..2]).
//-----------------------------------------------------------------------------
static ConVar bridge_deathfield_proxy_log("bridge_deathfield_proxy_log", "0",
	FCVAR_DEVELOPMENTONLY | FCVAR_ACCESSIBLE_FROM_THREADS,
	"Bridge [DEATHFIELD-PROXY]: log the first calls into each DT_World m_deathField* "
	"value proxy with the recovered ring index and the value served.");

static void DF_LogProxyCall(const char* pszField, void* pStruct, void* pData,
	int fieldId, int ring, float flValue)
{
	if (fieldId < 0 || fieldId >= DF_FIELD_COUNT)
		return;

	static volatile LONG s_nPerField[DF_FIELD_COUNT] = {};
	const LONG n = InterlockedIncrement(&s_nPerField[fieldId]);

	// First call per field always logs (loud stub); ConVar extends to first 8.
	if (n != 1)
	{
		if (!bridge_deathfield_proxy_log.GetBool())
			return;
		if (n > 8)
			return;
	}

	const long long diff = static_cast<long long>(
		reinterpret_cast<intptr_t>(pData) - reinterpret_cast<intptr_t>(pStruct));
	int active = -1;
	if (ring >= 0 && ring < MAX_DEATHFIELDS)
		active = s_deathFields[ring].isActive ? 1 : 0;

	Warning(eDLL_T::SERVER,
		"[DEATHFIELD-PROXY] %s call=%ld pStruct=%p pData=%p diff=%lld ring=%d value=%.1f active=%d\n",
		pszField ? pszField : "?", static_cast<long>(n), pStruct, pData,
		diff, ring, flValue, active);
}

static inline void DF_ZeroVariant(void* pOut)
{
	if (!pOut) return;
	*reinterpret_cast<uint64_t*>(pOut)       = 0;
	*(reinterpret_cast<uint64_t*>(pOut) + 1) = 0;
	*(reinterpret_cast<uint64_t*>(pOut) + 2) = 0;
}

template <int K>
static void __fastcall DF_IsActiveProxyK(void*, void* pStruct, void* pData, void* pOut, int, int)
{
	static_assert(K >= 0 && K < MAX_DEATHFIELDS, "ring out of range");
	DF_ZeroVariant(pOut);
	if (!pOut)
		return;
	const DeathFieldData_t& r = s_deathFields[K];
	*reinterpret_cast<int*>(pOut) = r.isActive ? 1 : 0;
	DF_LogProxyCall("IsActive", pStruct, pData, DF_ISACTIVE, K, r.isActive ? 1.f : 0.f);
}

template <int K>
static void __fastcall DF_OriginProxyK(void*, void* pStruct, void* pData, void* pOut, int, int)
{
	static_assert(K >= 0 && K < MAX_DEATHFIELDS, "ring out of range");
	DF_ZeroVariant(pOut);
	if (!pOut)
		return;
	const DeathFieldData_t& r = s_deathFields[K];
	reinterpret_cast<float*>(pOut)[0] = r.originX;
	reinterpret_cast<float*>(pOut)[1] = r.originY;
	reinterpret_cast<float*>(pOut)[2] = r.originZ;
	DF_LogProxyCall("Origin", pStruct, pData, DF_ORIGIN, K, r.originX);
}

template <int K>
static void __fastcall DF_RadiusStartProxyK(void*, void* pStruct, void* pData, void* pOut, int, int)
{
	static_assert(K >= 0 && K < MAX_DEATHFIELDS, "ring out of range");
	DF_ZeroVariant(pOut);
	if (!pOut)
		return;
	const DeathFieldData_t& r = s_deathFields[K];
	*reinterpret_cast<float*>(pOut) = r.radiusStart;
	DF_LogProxyCall("RadiusStart", pStruct, pData, DF_RADSTART, K, r.radiusStart);
}

template <int K>
static void __fastcall DF_RadiusEndProxyK(void*, void* pStruct, void* pData, void* pOut, int, int)
{
	static_assert(K >= 0 && K < MAX_DEATHFIELDS, "ring out of range");
	DF_ZeroVariant(pOut);
	if (!pOut)
		return;
	const DeathFieldData_t& r = s_deathFields[K];
	*reinterpret_cast<float*>(pOut) = r.radiusEnd;
	DF_LogProxyCall("RadiusEnd", pStruct, pData, DF_RADEND, K, r.radiusEnd);
}

template <int K>
static void __fastcall DF_TimeStartProxyK(void*, void* pStruct, void* pData, void* pOut, int, int)
{
	static_assert(K >= 0 && K < MAX_DEATHFIELDS, "ring out of range");
	DF_ZeroVariant(pOut);
	if (!pOut)
		return;
	const DeathFieldData_t& r = s_deathFields[K];
	*reinterpret_cast<float*>(pOut) = r.timeStart;
	DF_LogProxyCall("TimeStart", pStruct, pData, DF_TIMESTART, K, r.timeStart);
}

template <int K>
static void __fastcall DF_TimeEndProxyK(void*, void* pStruct, void* pData, void* pOut, int, int)
{
	static_assert(K >= 0 && K < MAX_DEATHFIELDS, "ring out of range");
	DF_ZeroVariant(pOut);
	if (!pOut)
		return;
	const DeathFieldData_t& r = s_deathFields[K];
	*reinterpret_cast<float*>(pOut) = r.timeEnd;
	DF_LogProxyCall("TimeEnd", pStruct, pData, DF_TIMEEND, K, r.timeEnd);
}

template <size_t... I>
static void* const* DF_MakeIsActiveTable(std::index_sequence<I...>)
{
	static void* const s_table[sizeof...(I)] = {
		reinterpret_cast<void*>(&DF_IsActiveProxyK<static_cast<int>(I)>)...
	};
	return s_table;
}

template <size_t... I>
static void* const* DF_MakeOriginTable(std::index_sequence<I...>)
{
	static void* const s_table[sizeof...(I)] = {
		reinterpret_cast<void*>(&DF_OriginProxyK<static_cast<int>(I)>)...
	};
	return s_table;
}

template <size_t... I>
static void* const* DF_MakeRadiusStartTable(std::index_sequence<I...>)
{
	static void* const s_table[sizeof...(I)] = {
		reinterpret_cast<void*>(&DF_RadiusStartProxyK<static_cast<int>(I)>)...
	};
	return s_table;
}

template <size_t... I>
static void* const* DF_MakeRadiusEndTable(std::index_sequence<I...>)
{
	static void* const s_table[sizeof...(I)] = {
		reinterpret_cast<void*>(&DF_RadiusEndProxyK<static_cast<int>(I)>)...
	};
	return s_table;
}

template <size_t... I>
static void* const* DF_MakeTimeStartTable(std::index_sequence<I...>)
{
	static void* const s_table[sizeof...(I)] = {
		reinterpret_cast<void*>(&DF_TimeStartProxyK<static_cast<int>(I)>)...
	};
	return s_table;
}

template <size_t... I>
static void* const* DF_MakeTimeEndTable(std::index_sequence<I...>)
{
	static void* const s_table[sizeof...(I)] = {
		reinterpret_cast<void*>(&DF_TimeEndProxyK<static_cast<int>(I)>)...
	};
	return s_table;
}

static void* const* const s_dfIsActiveProxies =
	DF_MakeIsActiveTable(std::make_index_sequence<MAX_DEATHFIELDS>{});
static void* const* const s_dfOriginProxies =
	DF_MakeOriginTable(std::make_index_sequence<MAX_DEATHFIELDS>{});
static void* const* const s_dfRadiusStartProxies =
	DF_MakeRadiusStartTable(std::make_index_sequence<MAX_DEATHFIELDS>{});
static void* const* const s_dfRadiusEndProxies =
	DF_MakeRadiusEndTable(std::make_index_sequence<MAX_DEATHFIELDS>{});
static void* const* const s_dfTimeStartProxies =
	DF_MakeTimeStartTable(std::make_index_sequence<MAX_DEATHFIELDS>{});
static void* const* const s_dfTimeEndProxies =
	DF_MakeTimeEndTable(std::make_index_sequence<MAX_DEATHFIELDS>{});

void* DeathField_GetRingProxy(int fieldId, int ring)
{
	if (ring < 0 || ring >= MAX_DEATHFIELDS)
		return nullptr;
	switch (fieldId)
	{
	case DF_ISACTIVE:
		return s_dfIsActiveProxies[ring];
	case DF_ORIGIN:
		return s_dfOriginProxies[ring];
	case DF_RADSTART:
		return s_dfRadiusStartProxies[ring];
	case DF_RADEND:
		return s_dfRadiusEndProxies[ring];
	case DF_TIMESTART:
		return s_dfTimeStartProxies[ring];
	case DF_TIMEEND:
		return s_dfTimeEndProxies[ring];
	default:
		return nullptr;
	}
}

bool DeathField_IsRingProxy(const void* fn)
{
	if (!fn)
		return false;
	static void* const* const s_tables[DF_FIELD_COUNT] = {
		s_dfIsActiveProxies,
		s_dfOriginProxies,
		s_dfRadiusStartProxies,
		s_dfRadiusEndProxies,
		s_dfTimeStartProxies,
		s_dfTimeEndProxies,
	};
	for (int fi = 0; fi < DF_FIELD_COUNT; ++fi)
	{
		void* const* const table = s_tables[fi];
		if (!table)
			continue;
		for (int k = 0; k < MAX_DEATHFIELDS; ++k)
		{
			if (table[k] == fn)
				return true;
		}
	}
	return false;
}

//-----------------------------------------------------------------------------
// Player DeathFieldIndex
//-----------------------------------------------------------------------------
static SDKEntityMap<int> s_deathFieldIndexMapServer(ESide::Server, "deathFieldIndex.srv");
static SDKEntityMap<int> s_deathFieldIndexMapClient(ESide::Client, "deathFieldIndex.cli");

static SDKEntityMap<int>& GetDeathFieldIndexMap(void)
{
	return s_deathFieldIndexMapServer;
}

static SDKEntityMap<int>& GetDeathFieldIndexMap(HSQUIRRELVM v)
{
	return (v && v->GetContext() == SQCONTEXT::SERVER) ? s_deathFieldIndexMapServer
	                                                   : s_deathFieldIndexMapClient;
}

int DeathField_GetIndexForPlayer(void* pPlayer)
{
	if (!pPlayer)
		return 0;

	const int* p = GetDeathFieldIndexMap().Find(pPlayer);
	return p ? *p : 0;
}

SQRESULT Script_DeathFieldIndex(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	const int* p = GetDeathFieldIndexMap(v).Find(pPlayer);
	sq_pushinteger(v, p ? *p : 0);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_SetDeathFieldIndex(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	SQInteger index;
	sq_getinteger(v, 2, &index);

	if (index < 0 || index >= MAX_DEATHFIELDS)
	{
		Warning(eDLL_T::SERVER, "SetDeathFieldIndex: index %d out of range [0, %d)\n",
			static_cast<int>(index), MAX_DEATHFIELDS);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	GetDeathFieldIndexMap(v)[pPlayer] = static_cast<int>(index);
	MarkEntityEdictDirty(pPlayer);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Deathfield query functions -- S21 signatures (index last / only).
//-----------------------------------------------------------------------------
SQRESULT Script_DeathField_IsActive(HSQUIRRELVM v)
{
	SQInteger index;
	sq_getinteger(v, 2, &index);

	const int idx = static_cast<int>(index);
	const bool active = (idx >= 0 && idx < MAX_DEATHFIELDS) && s_deathFields[idx].isActive;
	sq_pushbool(v, active);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_DeathField_PointDistanceFromFrontier(HSQUIRRELVM v)
{
	const SQVector3D* point = nullptr;
	sq_getvector(v, 2, &point);
	if (!point) return SQ_ERROR;

	SQInteger index;
	sq_getinteger(v, 3, &index);

	const int idx = static_cast<int>(index);
	const float curTime = gpGlobals ? gpGlobals->curTime : 0.0f;
	const float radius = GetCurrentRadius(idx, curTime);

	float originX = 0.0f;
	float originY = 0.0f;
	if (idx >= 0 && idx < MAX_DEATHFIELDS)
	{
		originX = s_deathFields[idx].originX;
		originY = s_deathFields[idx].originY;
	}

	const float dx = point->x - originX;
	const float dy = point->y - originY;
	const float dist2D = sqrtf(dx * dx + dy * dy);

	sq_pushfloat(v, radius - dist2D);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_DeathField_GetRadiusForNow(HSQUIRRELVM v)
{
	SQInteger index;
	sq_getinteger(v, 2, &index);

	const float curTime = gpGlobals ? gpGlobals->curTime : 0.0f;
	sq_pushfloat(v, GetCurrentRadius(static_cast<int>(index), curTime));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_DeathField_GetRadiusForTime(HSQUIRRELVM v)
{
	SQFloat time;
	sq_getfloat(v, 2, &time);

	SQInteger index;
	sq_getinteger(v, 3, &index);

	sq_pushfloat(v, GetCurrentRadius(static_cast<int>(index), static_cast<float>(time)));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Deathfield configuration (server-only)
//-----------------------------------------------------------------------------
SQRESULT Script_DeathField_SetActive(HSQUIRRELVM v)
{
	SQInteger index;
	sq_getinteger(v, 2, &index);

	SQBool active;
	sq_getbool(v, 3, &active);

	if (index >= 0 && index < MAX_DEATHFIELDS)
	{
		s_deathFields[index].isActive = (active != 0);
		DeathField_Commit(static_cast<int>(index));
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_DeathField_SetOrigin(HSQUIRRELVM v)
{
	SQInteger index;
	sq_getinteger(v, 2, &index);

	const SQVector3D* origin = nullptr;
	sq_getvector(v, 3, &origin);
	if (!origin) return SQ_ERROR;

	if (index >= 0 && index < MAX_DEATHFIELDS)
	{
		s_deathFields[index].originX = origin->x;
		s_deathFields[index].originY = origin->y;
		s_deathFields[index].originZ = origin->z;
		DeathField_Commit(static_cast<int>(index));
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_DeathField_SetRadiusStartEnd(HSQUIRRELVM v)
{
	SQInteger index;
	sq_getinteger(v, 2, &index);

	SQFloat start, end;
	sq_getfloat(v, 3, &start);
	sq_getfloat(v, 4, &end);

	if (index >= 0 && index < MAX_DEATHFIELDS)
	{
		s_deathFields[index].radiusStart = static_cast<float>(start);
		s_deathFields[index].radiusEnd = static_cast<float>(end);
		DeathField_Commit(static_cast<int>(index));
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_DeathField_SetTimeStartEnd(HSQUIRRELVM v)
{
	SQInteger index;
	sq_getinteger(v, 2, &index);

	SQFloat start, end;
	sq_getfloat(v, 3, &start);
	sq_getfloat(v, 4, &end);

	if (index >= 0 && index < MAX_DEATHFIELDS)
	{
		s_deathFields[index].timeStart = static_cast<float>(start);
		s_deathFields[index].timeEnd = static_cast<float>(end);
		DeathField_Commit(static_cast<int>(index));
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// SetDeathFieldParams(center, radiusStart, radiusEnd, timeStart, timeEnd,
// deathFieldIndex). Sixth arg optional, defaults 0 (ring 0).
//-----------------------------------------------------------------------------
SQRESULT Script_SetDeathFieldParams(HSQUIRRELVM v)
{
	const SQVector3D* center = nullptr;
	sq_getvector(v, 2, &center);
	if (!center)
		return SQ_ERROR;

	SQFloat radiusStart, radiusEnd, timeStart, timeEnd;
	sq_getfloat(v, 3, &radiusStart);
	sq_getfloat(v, 4, &radiusEnd);
	sq_getfloat(v, 5, &timeStart);
	sq_getfloat(v, 6, &timeEnd);

	SQInteger index = 0;
	if (sq_gettop(v) >= 7)
		sq_getinteger(v, 7, &index);

	if (index < 0 || index >= MAX_DEATHFIELDS)
	{
		Warning(eDLL_T::SERVER, "SetDeathFieldParams: index %d out of range [0, %d)\n",
			static_cast<int>(index), MAX_DEATHFIELDS);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	DeathField_ApplyParams(static_cast<int>(index),
		center->x, center->y, center->z,
		static_cast<float>(radiusStart), static_cast<float>(radiusEnd),
		static_cast<float>(timeStart), static_cast<float>(timeEnd));

	static volatile LONG s_hitN = 0;
	const LONG n = InterlockedIncrement(&s_hitN);
	if (n <= 4)
	{
		Warning(eDLL_T::SERVER,
			"[DEATHFIELD] SetDeathFieldParams #%d idx=%d origin=(%.0f,%.0f) r=%.0f->%.0f t=%.1f->%.1f\n",
			static_cast<int>(n), static_cast<int>(index),
			center->x, center->y,
			static_cast<float>(radiusStart), static_cast<float>(radiusEnd),
			static_cast<float>(timeStart), static_cast<float>(timeEnd));
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static void __fastcall Hook_SetDeathFieldParams(float* center, float radiusStart,
	float radiusEnd, float timeStart, float timeEnd)
{
	v_SetDeathFieldParams(center, radiusStart, radiusEnd, timeStart, timeEnd);
	DeathField_CaptureNativeRing0();
	DeathField_DirtyWorld();
}

//-----------------------------------------------------------------------------
// Level shutdown
//-----------------------------------------------------------------------------
void DeathField_LevelShutdown()
{
	for (int i = 0; i < MAX_DEATHFIELDS; i++)
		s_deathFields[i] = DeathFieldData_t();
}

//-----------------------------------------------------------------------------
// Script_RegisterFuncNamed uses one static binding; the engine re-registers
// SetDeathFieldParams after VM Init. Each overwrite needs its own live binding.
//-----------------------------------------------------------------------------
static ScriptFunctionBinding_t s_bindSetDeathFieldParams;
static ScriptFunctionBinding_t s_bindIsActive;
static ScriptFunctionBinding_t s_bindPointDistance;
static ScriptFunctionBinding_t s_bindRadiusNow;
static ScriptFunctionBinding_t s_bindRadiusTime;

void DeathField_RegisterOnVM(CSquirrelVM* s)
{
	if (!s)
		return;

	s_bindSetDeathFieldParams.Init(
		"SetDeathFieldParams",
		"Script_SetDeathFieldParams",
		"Sets a deathfield ring by index",
		"void",
		"vector center, float radiusStart, float radiusEnd, float timeStart, float timeEnd, int deathFieldIndex",
		true,
		Script_SetDeathFieldParams);
	s->RegisterFunction(&s_bindSetDeathFieldParams, true);

	s_bindIsActive.Init(
		"DeathField_IsActive",
		"Script_DeathField_IsActive",
		"Returns whether a deathfield is active by index",
		"bool", "int deathFieldIndex", false,
		Script_DeathField_IsActive);
	s->RegisterFunction(&s_bindIsActive, true);

	s_bindPointDistance.Init(
		"DeathField_PointDistanceFromFrontier",
		"Script_DeathField_PointDistanceFromFrontier",
		"Distance from deathfield frontier by index",
		"float", "vector point, int deathFieldIndex", false,
		Script_DeathField_PointDistanceFromFrontier);
	s->RegisterFunction(&s_bindPointDistance, true);

	s_bindRadiusNow.Init(
		"DeathField_GetRadiusForNow",
		"Script_DeathField_GetRadiusForNow",
		"Gets current deathfield radius by index",
		"float", "int deathFieldIndex", false,
		Script_DeathField_GetRadiusForNow);
	s->RegisterFunction(&s_bindRadiusNow, true);

	s_bindRadiusTime.Init(
		"DeathField_GetRadiusForTime",
		"Script_DeathField_GetRadiusForTime",
		"Gets deathfield radius at given time by index",
		"float", "float time, int deathFieldIndex", false,
		Script_DeathField_GetRadiusForTime);
	s->RegisterFunction(&s_bindRadiusTime, true);
}

//-----------------------------------------------------------------------------
// VDeathFieldSystem
//-----------------------------------------------------------------------------
void VDeathFieldSystem::GetAdr(void) const
{
	LogFunAdr("SetDeathFieldParams", v_SetDeathFieldParams);
	LogFunAdr("CBaseEntity::DisableHibernationApply", v_CBaseEntity_DisableHibernationApply);
	LogFunAdr("CBaseEntity::UpdateTransmitState", v_CBaseEntity_UpdateTransmitState);
	LogFunAdr("CBaseEntity::LeaveHibernation", v_CBaseEntity_LeaveHibernation);
	LogFunAdr("SendProxy_StringT", v_SendProxy_StringT);
	LogVarAdr("g_pWorldEntity", g_ppWorldEntity);
}

void VDeathFieldSystem::GetFun(void) const
{
	// SetDeathFieldParams: mov r8, [g_pWorldEntity]; cmp byte [r8+0xB3C], 1
	const CMemory fn = Module_FindPattern(g_GameDll,
		"4C 8B 05 ?? ?? ?? ?? 0F 28 E1 4C 8B C9 41 BA 00 02 00 00 "
		"41 80 B8 3C 0B 00 00 01");
	fn.GetPtr(v_SetDeathFieldParams);

	if (fn.IsValid())
		g_ppWorldEntity = fn.ResolveRelativeAddress(3, 7).RCast<void**>();
	else
		Warning(eDLL_T::SERVER, "[DEATHFIELD] SetDeathFieldParams pattern unresolved\n");

	if (!g_ppWorldEntity)
		Warning(eDLL_T::SERVER, "[DEATHFIELD] g_pWorldEntity unresolved\n");

	// DisableHibernation companion: sets +0x234 bit 0x20000000, bails if handle==-1.
	Module_FindPattern(g_GameDll,
		"48 89 6C 24 18 56 48 83 EC 20 48 89 5C 24 30 "
		"48 8D 2D ?? ?? ?? ?? 48 89 7C 24 38 48 8B F1")
		.GetPtr(v_CBaseEntity_DisableHibernationApply);
	if (!v_CBaseEntity_DisableHibernationApply)
		Warning(eDLL_T::SERVER, "[DEATHFIELD] DisableHibernationApply pattern unresolved\n");

	// UpdateTransmitState: mov edx, [rcx+0xF0]
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 4C 8B 05 ?? ?? ?? ?? 48 8B D9 8B 91 F0 00 00 00")
		.GetPtr(v_CBaseEntity_UpdateTransmitState);
	if (!v_CBaseEntity_UpdateTransmitState)
		Warning(eDLL_T::SERVER, "[DEATHFIELD] UpdateTransmitState pattern unresolved\n");

	// LeaveHibernation: test bit 0x1B (0x8000000) of [rcx+0x234], jnb = already awake.
	Module_FindPattern(g_GameDll,
		"48 89 74 24 10 57 48 83 EC 20 8B 81 34 02 00 00 "
		"48 8B F1 0F BA E0 1B 73 50")
		.GetPtr(v_CBaseEntity_LeaveHibernation);
	if (!v_CBaseEntity_LeaveHibernation)
		Warning(eDLL_T::SERVER, "[DEATHFIELD] LeaveHibernation pattern unresolved\n");

	Module_FindPattern(g_GameDll,
		"49 8B 00 48 8D 0D ?? ?? ?? ?? 48 85 C0 48 0F 45 C8 49 89 09 C3")
		.GetPtr(v_SendProxy_StringT);
	if (!v_SendProxy_StringT)
		Warning(eDLL_T::SERVER, "[DEATHFIELD] SendProxy_StringT pattern unresolved\n");
}

void VDeathFieldSystem::Detour(const bool bAttach) const
{
	if (v_SetDeathFieldParams)
		DetourSetup(&v_SetDeathFieldParams, &Hook_SetDeathFieldParams, bAttach);
}

#endif // CLIENT_DLL
