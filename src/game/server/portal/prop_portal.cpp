//=============================================================================//
//
// Purpose: CProp_Portal networked entity (server half).
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier1/convar.h"
#include "game/server/portal/prop_portal.h"
#include "game/server/translocation.h"
#include "game/shared/portal/portal_shared.h"
#include "game/shared/dt_extend.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/util_shared.h"
#include "engine/enginetrace.h"
#include "game/server/player.h"
#include "game/server/util_server.h"
#include "engine/server/server.h"
#include "engine/server/sv_main.h"
#include "engine/server/snapshot_diag.h"
#include "game/server/mapedit_paks.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include <cstring>
#include <cmath>

extern CGlobalVars* gpGlobals;

static ConVar portal_debug("portal_debug", "0", FCVAR_DEVELOPMENTONLY,
	"Log portal spawn/link/move lines (0=off, 1=on).");

static constexpr const char* kPortalServerClass = "CProp_Portal";
static constexpr const char* kPortalSendTable = "DT_Prop_Portal";
static constexpr const char* kPortalDictName = "prop_portal";
// Shipped by the portal gun mod in both the client and the dedi pak.
static constexpr const char* kPortalModelNames[2] = {
	"mdl/cafefps_portalgun__portalgun/portal_blue.rmdl",
	"mdl/cafefps_portalgun__portalgun/portal_orange.rmdl"
};
static constexpr const char* kPortalParentDict = "prop_dynamic";
static constexpr const char* kPortalParentClass = "CDynamicProp";

static constexpr int kPortalFieldCount = 5;
static constexpr int kPortalTailSize = 16;
static constexpr int kPortalOffLinked = 0;
static constexpr int kPortalOffActive = 4;
static constexpr int kPortalOffIs2 = 5;
static constexpr int kPortalOffHalfW = 8;
static constexpr int kPortalOffHalfH = 12;
static constexpr float kPortalDefaultHalfW = 36.0f;
static constexpr float kPortalDefaultHalfH = 64.0f;
static constexpr float kPortalTraceLen = 4096.0f;
static constexpr uint32_t kInvalidHandle = 0xFFFFFFFFu;
static constexpr int kPortalRegistryCap = 64;

struct PortalRegEntry_t
{
	uint32_t m_ownerHandle;
	uint32_t m_portalHandle;
	const void* m_portalPtr;
	int m_slot;
	Vector3D m_pos;
	QAngle m_ang;
	float m_halfW;
	float m_halfH;
	uint8_t m_tail[16]; // networked portal fields, served by the send proxies
	float m_flOpenStart; // < 0 when not opening
};

static PortalRegEntry_t s_portalRegistry[kPortalRegistryCap] = {};
static bool s_portalRegistered = false;
static uint8_t s_portalSendTable[NR_SENDTABLE_SIZE] = {};
static uint8_t s_portalProps[6 * SP_SIZE] = {};
static uint8_t s_portalFactory[48] = {};
static uintptr_t s_portalEntFactoryVtable[8] = {};
static uintptr_t s_portalEntFactoryObj = 0;
static uintptr_t s_portalParentFactory = 0;
static uintptr_t s_portalParentCreate = 0;
static int s_portalNativeSize = 0;
static int s_portalParityOff = -1;
static int s_portalAbsOriginOff = -1;
static int s_portalAbsAnglesOff = -1;
static int s_portalModelScaleOff = -1;
// Portal 2 opens at 2 units of open amount per second.
static constexpr float kPortalOpenTime = 0.5f;
static constexpr float kPortalOpenMinScale = 0.01f;

int Portal_RequiredAllocSize(void)
{
	return s_portalNativeSize > 0 ? s_portalNativeSize : 0;
}

bool Portal_IsPortalEntity(const void* pEntity)
{
	if (!pEntity)
		return false;
	for (int i = 0; i < kPortalRegistryCap; ++i)
	{
		const PortalRegEntry_t& e = s_portalRegistry[i];
		if (!e.m_portalPtr || e.m_portalPtr != pEntity)
			continue;
		if (!DTExtend_IsSafeToRead(
				reinterpret_cast<const uint8_t*>(pEntity) + 0x08, sizeof(uint32_t)))
			return false;
		const uint32_t live = *reinterpret_cast<const uint32_t*>(
			reinterpret_cast<uintptr_t>(pEntity) + 0x08);
		return live == e.m_portalHandle;
	}
	return false;
}

static uint8_t* Portal_FindTableByName(uintptr_t table, const char* name)
{
	if (!table || !name)
		return nullptr;
	uintptr_t seen[256] = {};
	int nSeen = 0;
	uintptr_t stack[64] = {};
	int nStack = 0;
	stack[nStack++] = table;
	while (nStack > 0)
	{
		uintptr_t cur = stack[--nStack];
		bool dup = false;
		for (int i = 0; i < nSeen; ++i)
		{
			if (seen[i] == cur) { dup = true; break; }
		}
		if (dup)
			continue;
		if (nSeen < 256)
			seen[nSeen++] = cur;
		if (!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(cur), NR_SENDTABLE_SIZE))
			continue;
		const char* tn = *(const char**)(cur + ST_NETTABLENAME);
		if (tn && strcmp(tn, name) == 0)
			return reinterpret_cast<uint8_t*>(cur);
		uint8_t* props = *(uint8_t**)(cur + ST_PROPS);
		const int n = *(int*)(cur + ST_NPROPS);
		if (!props || n <= 0 || n > 512)
			continue;
		if (!DTExtend_IsSafeToRead(props, (size_t)n * SP_SIZE))
			continue;
		for (int j = 0; j < n; ++j)
		{
			uint8_t* p = props + (uint64_t)j * SP_SIZE;
			if (*(int*)(p + SP_TYPE) != 10)
				continue;
			uintptr_t child = *(uintptr_t*)(p + SP_CHILDTABLE);
			if (child && nStack < 64)
				stack[nStack++] = child;
		}
	}
	return nullptr;
}

static bool Portal_FindPropAbs(uintptr_t table, const char* name, int base, int depth, int* outAbs)
{
	if (!table || !name || !outAbs || depth > 6)
		return false;
	if (!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(table), NR_SENDTABLE_SIZE))
		return false;
	uint8_t* props = *(uint8_t**)(table + ST_PROPS);
	const int n = *(int*)(table + ST_NPROPS);
	if (!props || n <= 0 || n > 512)
		return false;
	if (!DTExtend_IsSafeToRead(props, (size_t)n * SP_SIZE))
		return false;
	for (int j = 0; j < n; ++j)
	{
		uint8_t* p = props + (uint64_t)j * SP_SIZE;
		const int ty = *(int*)(p + SP_TYPE);
		if (ty != 10)
		{
			const char* pn = *(const char**)(p + SP_VARNAME);
			if (pn && strcmp(pn, name) == 0)
			{
				*outAbs = base + *(int*)(p + SP_OFFSET);
				return true;
			}
			continue;
		}
		uintptr_t child = *(uintptr_t*)(p + SP_CHILDTABLE);
		if (!child)
			continue;
		int childBase = base;
		if (DTExtend_IsSafeToRead(reinterpret_cast<const void*>(child + 0x4FC), sizeof(int)))
			childBase += *(int*)(child + 0x4FC);
		if (Portal_FindPropAbs(child, name, childBase, depth + 1, outAbs))
			return true;
	}
	return false;
}

// Any non-excluded prop of the wanted type, searched through the nested base
// tables: only its struct shape is reused, the encoding and proxy are ours.
static uint8_t* Portal_FindScalarDonor(uintptr_t table, int wantType, int depth = 0)
{
	if (!table || depth > 8)
		return nullptr;
	if (!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(table), NR_SENDTABLE_SIZE))
		return nullptr;
	uint8_t* props = *(uint8_t**)(table + ST_PROPS);
	const int n = *(int*)(table + ST_NPROPS);
	if (!props || n <= 0 || n > 512)
		return nullptr;
	if (!DTExtend_IsSafeToRead(props, (size_t)n * SP_SIZE))
		return nullptr;
	for (int j = 0; j < n; ++j)
	{
		uint8_t* p = props + (uint64_t)j * SP_SIZE;
		if (*(int*)(p + SP_TYPE) != wantType)
			continue;
		if (*(int*)(p + SP_FLAGS) & 0x40)
			continue;
		return p;
	}
	for (int j = 0; j < n; ++j)
	{
		uint8_t* p = props + (uint64_t)j * SP_SIZE;
		if (*(int*)(p + SP_TYPE) != 10)
			continue;
		uint8_t* const found = Portal_FindScalarDonor(*(uintptr_t*)(p + SP_CHILDTABLE), wantType, depth + 1);
		if (found)
			return found;
	}
	return nullptr;
}

static uint8_t* Portal_FindBaseDonor(uintptr_t table)
{
	if (!table)
		return nullptr;
	if (!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(table), NR_SENDTABLE_SIZE))
		return nullptr;
	uint8_t* props = *(uint8_t**)(table + ST_PROPS);
	const int n = *(int*)(table + ST_NPROPS);
	if (!props || n <= 0 || n > 512)
		return nullptr;
	if (!DTExtend_IsSafeToRead(props, (size_t)n * SP_SIZE))
		return nullptr;
	for (int j = 0; j < n; ++j)
	{
		uint8_t* p = props + (uint64_t)j * SP_SIZE;
		if (*(int*)(p + SP_TYPE) != 10)
			continue;
		if (!*(uintptr_t*)(p + SP_CHILDTABLE))
			continue;
		return p;
	}
	return nullptr;
}

static const uint8_t* Portal_ProxyTail(const void* pStruct, int objectID, int off, size_t n);

static void __fastcall Portal_LinkedProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int objectID)
{
	if (!pOut)
		return;
	const uint8_t* const tail = Portal_ProxyTail(pStruct, objectID, kPortalOffLinked, sizeof(int32_t));
	const int32_t s3Handle = tail ? *reinterpret_cast<const int32_t*>(tail) : -1;
	*reinterpret_cast<int32_t*>(pOut) = SDKEntityState_PackS21RecvEHandle(s3Handle);
}

static uint8_t* Portal_Tail(void* pEntity);

static volatile LONG s_nProxyCalls = 0;
static volatile LONG s_nProxyMisses = 0;

// Resolves by the pack loop's struct pointer, falling back to the edict index
// the loop passes as objectID.
static const uint8_t* Portal_ProxyTail(const void* pStruct, int objectID, int off, size_t n)
{
	const uint8_t* tail = Portal_Tail(const_cast<void*>(pStruct));
	if (!tail && objectID > 0)
	{
		for (int i = 0; i < kPortalRegistryCap && !tail; ++i)
		{
			const uintptr_t e = reinterpret_cast<uintptr_t>(s_portalRegistry[i].m_portalPtr);
			if (e && *reinterpret_cast<const int16_t*>(e + 0x58) == objectID) // edict index
				tail = s_portalRegistry[i].m_tail;
		}
	}
	const LONG nCall = InterlockedIncrement(&s_nProxyCalls);
	if (!tail)
		InterlockedIncrement(&s_nProxyMisses);
	if (nCall == 1 || (!tail && s_nProxyMisses == 1))
	{
		Warning(eDLL_T::SERVER, "[PORTAL] send proxy %s: pStruct=%p objectID=%d registry[0]=%p registry[1]=%p\n",
			tail ? "hit" : "MISS", pStruct, objectID, s_portalRegistry[0].m_portalPtr, s_portalRegistry[1].m_portalPtr);
	}
	return (tail && off + n <= kPortalTailSize) ? tail + off : nullptr;
}

static void __fastcall Portal_ActiveProxy(void*, void* pStruct, void*, void* pOut, int, int objectID)
{
	const uint8_t* const v = Portal_ProxyTail(pStruct, objectID, kPortalOffActive, 1);
	if (pOut)
		*reinterpret_cast<int32_t*>(pOut) = v ? (*v != 0) : 0;
}

static void __fastcall Portal_Is2Proxy(void*, void* pStruct, void*, void* pOut, int, int objectID)
{
	const uint8_t* const v = Portal_ProxyTail(pStruct, objectID, kPortalOffIs2, 1);
	if (pOut)
		*reinterpret_cast<int32_t*>(pOut) = v ? (*v != 0) : 0;
}

static void __fastcall Portal_HalfWProxy(void*, void* pStruct, void*, void* pOut, int, int objectID)
{
	const uint8_t* const v = Portal_ProxyTail(pStruct, objectID, kPortalOffHalfW, sizeof(float));
	if (pOut)
		*reinterpret_cast<float*>(pOut) = v ? *reinterpret_cast<const float*>(v) : 0.0f;
}

static void __fastcall Portal_HalfHProxy(void*, void* pStruct, void*, void* pOut, int, int objectID)
{
	const uint8_t* const v = Portal_ProxyTail(pStruct, objectID, kPortalOffHalfH, sizeof(float));
	if (pOut)
		*reinterpret_cast<float*>(pOut) = v ? *reinterpret_cast<const float*>(v) : 0.0f;
}

bool Portal_IsTailProxy(const void* fn)
{
	return fn && (fn == reinterpret_cast<const void*>(&Portal_LinkedProxy) ||
		fn == reinterpret_cast<const void*>(&Portal_ActiveProxy) ||
		fn == reinterpret_cast<const void*>(&Portal_Is2Proxy) ||
		fn == reinterpret_cast<const void*>(&Portal_HalfWProxy) ||
		fn == reinterpret_cast<const void*>(&Portal_HalfHProxy));
}

static uintptr_t __fastcall Portal_FactoryCreate(uintptr_t self, __int64 a2)
{
	if (!s_portalParentCreate)
		return 0;
	typedef uintptr_t (__fastcall* PFN_RawCreate)(uintptr_t, __int64);
	uintptr_t networkable = ((PFN_RawCreate)s_portalParentCreate)(s_portalParentFactory, a2);
	(void)self;
	if (!networkable)
		return 0;
	if (DTExtend_IsSafeToRead(reinterpret_cast<const void*>(networkable + 0x10), sizeof(uintptr_t), true))
		*(uintptr_t*)(networkable + 0x10) = (uintptr_t)s_portalFactory;
	uintptr_t entity = 0;
	if (DTExtend_IsSafeToRead(reinterpret_cast<const void*>(networkable + 8), sizeof(uintptr_t)))
		entity = *(uintptr_t*)(networkable + 8);
	if (portal_debug.GetBool() && entity)
		Warning(eDLL_T::SERVER, "[PORTAL] factory create entity=0x%p\n", (void*)entity);
	return networkable;
}

static __int64 __fastcall Portal_FactoryGetSize(uintptr_t /*factoryPtr*/)
{
	return Portal_RequiredAllocSize();
}

void Portal_RegisterServerClass(void)
{
	if (s_portalRegistered || !g_pFactoryListHead || !*g_pFactoryListHead || !v_GetEntityFactory)
		return;

	uintptr_t dynNode = 0;
	{
		uintptr_t node = *g_pFactoryListHead;
		for (int guard = 0; node && guard < kFactoryWalkCap; ++guard)
		{
			const char* cn = *(const char**)(node + FACT_CLASSNAME);
			if (cn && strcmp(cn, kPortalParentClass) == 0) { dynNode = node; break; }
			node = *(uintptr_t*)(node + FACT_NEXT);
		}
	}
	if (!dynNode)
	{
		Warning(eDLL_T::SERVER, "[PORTAL] parent ServerClass '%s' not in factory list -- portal off\n",
			kPortalParentClass);
		return;
	}
	uintptr_t dynTable = *(uintptr_t*)(dynNode + FACT_SENDTABLE);
	if (!dynTable)
	{
		Warning(eDLL_T::SERVER, "[PORTAL] parent SendTable null -- portal off\n");
		return;
	}
	const char* dynTableName = *(const char**)(dynTable + ST_NETTABLENAME);
	if (!dynTableName || !dynTableName[0])
	{
		Warning(eDLL_T::SERVER, "[PORTAL] parent table unnamed -- portal off\n");
		return;
	}
	const int nativeSize = *(int*)(dynNode + FACT_ALLOCSIZE);
	if (nativeSize <= 0 || nativeSize > 0x10000)
	{
		Warning(eDLL_T::SERVER, "[PORTAL] parent alloc %d implausible -- portal off\n", nativeSize);
		return;
	}
	s_portalNativeSize = nativeSize;

	uint8_t* intDonor = Portal_FindScalarDonor(dynTable, 0);
	uint8_t* floatDonor = Portal_FindScalarDonor(dynTable, 1);
	uint8_t* baseDonor = Portal_FindBaseDonor(dynTable);
	if (!intDonor || !floatDonor || !baseDonor)
	{
		Warning(eDLL_T::SERVER, "[PORTAL] donors missing (int=%p float=%p base=%p) -- portal off\n",
			(void*)intDonor, (void*)floatDonor, (void*)baseDonor);
		s_portalNativeSize = 0;
		return;
	}

	memcpy(s_portalProps + 0 * SP_SIZE, baseDonor, SP_SIZE);
	*(const char**)(s_portalProps + 0 * SP_SIZE + SP_VARNAME) = dynTableName;
	*(int*)(s_portalProps + 0 * SP_SIZE + SP_OFFSET) = 0;
	*(int*)(s_portalProps + 0 * SP_SIZE + SP_NELEMENTS) = 1;
	*(uintptr_t*)(s_portalProps + 0 * SP_SIZE + SP_CHILDTABLE) = dynTable;

	static const char* const names[kPortalFieldCount] = {
		"m_hLinkedPortal", "m_bActivated", "m_bIsPortal2",
		"m_fNetworkHalfWidth", "m_fNetworkHalfHeight"
	};
	static const int kinds[kPortalFieldCount] = { 0, 0, 0, 1, 1 };
	static const int offs[kPortalFieldCount] = {
		kPortalOffLinked, kPortalOffActive, kPortalOffIs2,
		kPortalOffHalfW, kPortalOffHalfH
	};
	for (int i = 0; i < kPortalFieldCount; ++i)
	{
		uint8_t* dst = s_portalProps + (uint64_t)(i + 1) * SP_SIZE;
		memcpy(dst, kinds[i] == 0 ? intDonor : floatDonor, SP_SIZE);
		*(const char**)(dst + SP_VARNAME) = names[i];
		*(int*)(dst + SP_OFFSET) = nativeSize + offs[i];
		if (kinds[i] == 0 && (strcmp(names[i], "m_bActivated") == 0 || strcmp(names[i], "m_bIsPortal2") == 0))
		{
			*(int*)(dst + SP_NBITS) = 1;
			*(int*)(dst + SP_FLAGS) = 0x1; // SPROP_UNSIGNED, or a set bit decodes as -1
		}
		if (strcmp(names[i], "m_hLinkedPortal") == 0)
		{
			*(int*)(dst + SP_NBITS) = 32;
			*(uintptr_t*)(dst + 0x60) = (uintptr_t)&Portal_LinkedProxy;
		}
		if (kinds[i] == 1)
		{
			// SPROP_NOSCALE: sent as a raw 32-bit float, whatever the donor's range was.
			*(int*)(dst + SP_FLAGS) = 0x4;
			*(int*)(dst + SP_NBITS) = 32;
		}
		static const uintptr_t proxies[kPortalFieldCount] = {
			(uintptr_t)&Portal_LinkedProxy, (uintptr_t)&Portal_ActiveProxy, (uintptr_t)&Portal_Is2Proxy,
			(uintptr_t)&Portal_HalfWProxy, (uintptr_t)&Portal_HalfHProxy
		};
		*(uintptr_t*)(dst + 0x60) = proxies[i];
	}

	memcpy(s_portalSendTable, (void*)dynTable, NR_SENDTABLE_SIZE);
	*(uint8_t**)(s_portalSendTable + ST_PROPS) = s_portalProps;
	*(int*)(s_portalSendTable + ST_NPROPS) = kPortalFieldCount + 1;
	*(const char**)(s_portalSendTable + ST_NETTABLENAME) = kPortalSendTable;
	*(uintptr_t*)(s_portalSendTable + 0x4C0) = 0;
	*(uintptr_t*)(s_portalSendTable + 0x508) = 0;

	memset(s_portalFactory, 0, sizeof(s_portalFactory));
	*(const char**)(s_portalFactory + FACT_CLASSNAME) = kPortalServerClass;
	*(void**)(s_portalFactory + FACT_SENDTABLE) = s_portalSendTable;
	*(uintptr_t*)(s_portalFactory + FACT_NEXT) = 0;
	*(int*)(s_portalFactory + FACT_CLASSID) = 0xFFFF;
	*(int*)(s_portalFactory + FACT_ALLOCSIZE) = nativeSize;
	*(int*)(s_portalFactory + FACT_UNK20) = 0xFFFF;
	{
		uintptr_t tail = *g_pFactoryListHead;
		int guard = 0;
		while (*(uintptr_t*)(tail + FACT_NEXT) && guard++ < kFactoryWalkCap)
			tail = *(uintptr_t*)(tail + FACT_NEXT);
		*(uintptr_t*)(tail + FACT_NEXT) = (uintptr_t)s_portalFactory;
	}

	void** dict = (void**)v_GetEntityFactory();
	bool dictOk = false;
	if (dict && dict[0])
	{
		typedef uintptr_t (__fastcall* PFN_DictFindByName)(void**, const char*);
		PFN_DictFindByName pfnFind = *(PFN_DictFindByName*)((uintptr_t)dict[0] + 0x18);
		uintptr_t parentFactory = pfnFind(dict, kPortalParentDict);
		if (parentFactory)
		{
			s_portalParentFactory = parentFactory;
			uintptr_t parentVtable = *(uintptr_t*)parentFactory;
			if (DTExtend_IsSafeToRead(reinterpret_cast<const void*>(parentVtable), 8 * sizeof(uintptr_t)))
			{
				s_portalParentCreate = ((uintptr_t*)parentVtable)[0];
				memcpy(s_portalEntFactoryVtable, (void*)parentVtable, sizeof(s_portalEntFactoryVtable));
				s_portalEntFactoryVtable[0] = (uintptr_t)&Portal_FactoryCreate;
				s_portalEntFactoryVtable[2] = (uintptr_t)&Portal_FactoryGetSize;
				s_portalEntFactoryObj = (uintptr_t)s_portalEntFactoryVtable;

				typedef __int64 (__fastcall* PFN_DictRegister)(void**, void*, const char*, const char*);
				PFN_DictRegister pfnRegister = *(PFN_DictRegister*)((uintptr_t)dict[0]);
				pfnRegister(dict, &s_portalEntFactoryObj, kPortalDictName, kPortalServerClass);

				dictOk = true;
			}
		}
	}
	if (!dictOk)
	{
		Warning(eDLL_T::SERVER, "[PORTAL] entity factory '%s' missing -- portal off\n",
			kPortalParentDict);
		return;
	}

	Portal_FindPropAbs(dynTable, "m_ubEFNoInterpParity", 0, 0, &s_portalParityOff);
	Portal_FindPropAbs(dynTable, "m_vecAbsOrigin", 0, 0, &s_portalAbsOriginOff);
	if (!Portal_FindPropAbs(dynTable, "m_angAbsRotation", 0, 0, &s_portalAbsAnglesOff))
		Portal_FindPropAbs(dynTable, "m_angRotation", 0, 0, &s_portalAbsAnglesOff);
	if (!Portal_FindPropAbs(dynTable, "m_flModelScale", 0, 0, &s_portalModelScaleOff))
		Warning(eDLL_T::SERVER, "[PORTAL] m_flModelScale not in the send table -- portals open instantly\n");

	s_portalRegistered = true;
	Warning(eDLL_T::ENGINE,
		"[PORTAL] ServerClass '%s' registered (base '%s' tbl '%s' native=%d grown=%d parity=0x%X origin=0x%X angles=0x%X)\n",
		kPortalServerClass, kPortalParentClass, dynTableName, nativeSize,
		nativeSize + kPortalTailSize, s_portalParityOff, s_portalAbsOriginOff,
		s_portalAbsAnglesOff);
}

void Portal_LevelShutdown(void)
{
	for (int i = 0; i < kPortalRegistryCap; ++i)
		s_portalRegistry[i] = PortalRegEntry_t{};
}

unsigned int Portal_GetEntityHandle(const void* pEntity)
{
	if (!pEntity)
		return kInvalidHandle;
	if (!DTExtend_IsSafeToRead(reinterpret_cast<const uint8_t*>(pEntity) + 0x08, sizeof(uint32_t)))
		return kInvalidHandle;
	return *reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(pEntity) + 0x08);
}

void* Portal_ResolveServerEntity(unsigned int nS3Handle)
{
	if (nS3Handle == kInvalidHandle)
		return nullptr;
	return SDKEntityState_Resolve(SDKEntityHandle(nS3Handle), ESide::Server);
}

static bool Portal_ReadPose(const void* pEntity, Vector3D* pOrigin, QAngle* pAngles)
{
	if (!pEntity || !pOrigin || !pAngles)
		return false;
	// The server places every portal, so its pose is the registry copy.
	for (int i = 0; i < kPortalRegistryCap; ++i)
	{
		if (s_portalRegistry[i].m_portalPtr == pEntity)
		{
			*pOrigin = s_portalRegistry[i].m_pos;
			*pAngles = s_portalRegistry[i].m_ang;
			return true;
		}
	}
	return false;
}

static uint8_t* Portal_Tail(void* pEntity);
static bool Portal_IsLive(const PortalRegEntry_t& e);

int Portal_GetTeleportPairs(PortalPairState_t* pOut, int nCap)
{
	if (!pOut || nCap <= 0 || !s_portalRegistered)
		return 0;
	int nPairs = 0;
	for (int i = 0; i < kPortalRegistryCap && nPairs < nCap; ++i)
	{
		const PortalRegEntry_t& e = s_portalRegistry[i];
		if (!Portal_IsLive(e))
			continue;
		uint8_t* tail = Portal_Tail(const_cast<void*>(e.m_portalPtr));
		if (!tail || !tail[kPortalOffActive])
			continue;
		const uint32_t linkHandle = *reinterpret_cast<uint32_t*>(tail + kPortalOffLinked);
		if (linkHandle == kInvalidHandle)
			continue;
		void* pExit = Portal_ResolveServerEntity(linkHandle);
		if (!pExit)
			continue;
		uint8_t* exitTail = Portal_Tail(pExit);
		if (!exitTail || !exitTail[kPortalOffActive])
			continue;
		PortalPairState_t& pair = pOut[nPairs];
		pair.m_pEntryEntity = const_cast<void*>(e.m_portalPtr);
		pair.m_pExitEntity = pExit;
		pair.m_nExitHandle = linkHandle;
		pair.m_bValid = false;
		if (!Portal_ReadPose(e.m_portalPtr, &pair.m_entryOrigin, &pair.m_entryAngles))
			continue;
		if (!Portal_ReadPose(pExit, &pair.m_exitOrigin, &pair.m_exitAngles))
			continue;
		pair.m_flEntryHalfW = *reinterpret_cast<float*>(tail + kPortalOffHalfW);
		pair.m_flEntryHalfH = *reinterpret_cast<float*>(tail + kPortalOffHalfH);
		if (!(pair.m_flEntryHalfW > 0.0f) || !(pair.m_flEntryHalfH > 0.0f) ||
			pair.m_flEntryHalfW > 512.0f || pair.m_flEntryHalfH > 512.0f)
			continue;
		pair.m_bValid = true;
		++nPairs;
	}
	return nPairs;
}

static uint8_t* Portal_Tail(void* pEntity)
{
	if (!pEntity)
		return nullptr;
	for (int i = 0; i < kPortalRegistryCap; ++i)
	{
		if (s_portalRegistry[i].m_portalPtr == pEntity)
			return s_portalRegistry[i].m_tail;
	}
	return nullptr;
}

static PortalRegEntry_t* Portal_FindByPtr(const void* pEntity)
{
	for (int i = 0; i < kPortalRegistryCap; ++i)
	{
		if (s_portalRegistry[i].m_portalPtr == pEntity)
			return &s_portalRegistry[i];
	}
	return nullptr;
}

static bool Portal_IsLive(const PortalRegEntry_t& e)
{
	if (!e.m_portalPtr)
		return false;
	if (!DTExtend_IsSafeToRead(
			reinterpret_cast<const uint8_t*>(e.m_portalPtr) + 0x08, sizeof(uint32_t)))
		return false;
	return *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<uintptr_t>(e.m_portalPtr) + 0x08) == e.m_portalHandle;
}

static void Portal_UpdateLinkage(uint32_t ownerHandle)
{
	PortalRegEntry_t* slots[2] = { nullptr, nullptr };
	for (int i = 0; i < kPortalRegistryCap; ++i)
	{
		PortalRegEntry_t& e = s_portalRegistry[i];
		if (e.m_ownerHandle != ownerHandle || e.m_slot < 0 || e.m_slot > 1)
			continue;
		if (!Portal_IsLive(e))
		{
			e = PortalRegEntry_t{};
			continue;
		}
		slots[e.m_slot] = &e;
	}
	const bool paired = slots[0] && slots[1];
	for (int s = 0; s < 2; ++s)
	{
		if (!slots[s])
			continue;
		uint8_t* tail = Portal_Tail(const_cast<void*>(slots[s]->m_portalPtr));
		if (!tail)
			continue;
		const uint32_t link = paired ? slots[1 - s]->m_portalHandle : kInvalidHandle;
		*reinterpret_cast<uint32_t*>(tail + kPortalOffLinked) = link;
		MarkEntityEdictDirty(const_cast<void*>(slots[s]->m_portalPtr));
	}
	if (portal_debug.GetBool())
	{
		Warning(eDLL_T::SERVER, "[PORTAL] link owner=0x%08X %s (0=0x%08X 1=0x%08X)\n",
			ownerHandle, paired ? "paired" : "cleared",
			slots[0] ? slots[0]->m_portalHandle : kInvalidHandle,
			slots[1] ? slots[1]->m_portalHandle : kInvalidHandle);
	}
}

static void Portal_SetScale(void* pEntity, float flScale)
{
	if (!pEntity || s_portalModelScaleOff <= 0)
		return;
	*reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(pEntity) + s_portalModelScaleOff) = flScale;
	MarkEntityEdictDirty(pEntity);
}

void PortalEntity_Frame(void)
{
	if (!gpGlobals || s_portalModelScaleOff <= 0)
		return;
	for (int i = 0; i < kPortalRegistryCap; ++i)
	{
		PortalRegEntry_t& e = s_portalRegistry[i];
		if (e.m_flOpenStart < 0.0f || !e.m_portalPtr)
			continue;
		if (!Portal_IsLive(e))
		{
			e.m_flOpenStart = -1.0f;
			continue;
		}
		float t = (gpGlobals->curTime - e.m_flOpenStart) / kPortalOpenTime;
		if (t >= 1.0f || t < 0.0f)
		{
			t = 1.0f;
			e.m_flOpenStart = -1.0f;
		}
		Portal_SetScale(const_cast<void*>(e.m_portalPtr), fmaxf(t, kPortalOpenMinScale));
	}
}

static void Portal_Move(void* pEntity, const Vector3D& pos, const QAngle& ang)
{
	if (!pEntity)
		return;
	const float flPos[3] = { pos.x, pos.y, pos.z };
	const float flAng[3] = { ang.x, ang.y, ang.z };
	Translocation_SetAbsOrigin3(pEntity, flPos);
	Translocation_SetAbsAngles3(pEntity, flAng);
	Translocation_SetNoInterpEffect(pEntity);
	uint8_t* tail = Portal_Tail(pEntity);
	if (tail)
		tail[kPortalOffActive] = 1;
	MarkEntityEdictDirty(pEntity);
	if (PortalRegEntry_t* e = Portal_FindByPtr(pEntity))
	{
		e->m_pos = pos;
		e->m_ang = ang;
		if (gpGlobals && s_portalModelScaleOff > 0)
		{
			e->m_flOpenStart = gpGlobals->curTime;
			Portal_SetScale(pEntity, kPortalOpenMinScale);
		}
		Portal_UpdateLinkage(e->m_ownerHandle);
	}
	if (portal_debug.GetBool())
	{
		Warning(eDLL_T::SERVER, "[PORTAL] move ent=0x%p pos=(%.1f %.1f %.1f) ang=(%.1f %.1f %.1f)\n",
			pEntity, pos.x, pos.y, pos.z, ang.x, ang.y, ang.z);
	}
}

static void Portal_AnglesToDir(float pitchDeg, float yawDeg, Vector3D& out)
{
	const float p = pitchDeg * 3.14159265f / 180.0f;
	const float y = yawDeg * 3.14159265f / 180.0f;
	const float sp = sinf(p);
	const float cp = cosf(p);
	out.x = cp * cosf(y);
	out.y = cp * sinf(y);
	out.z = -sp;
}

static void Portal_NormalToAngles(const Vector3D& n, QAngle& out)
{
	float z = n.z;
	if (z > 1.0f) z = 1.0f;
	if (z < -1.0f) z = -1.0f;
	out.y = atan2f(n.y, n.x) * 180.0f / 3.14159265f;
	out.x = -asinf(z) * 180.0f / 3.14159265f;
	out.z = 0.0f;
}

static CPlayer* Portal_FirstPlayer(void)
{
	if (!g_pServer || !g_ServerGlobalVariables)
		return nullptr;
	const int max = g_ServerGlobalVariables->maxClients;
	for (int i = 1; i <= max && i < 128; ++i)
	{
		CPlayer* p = UTIL_PlayerByIndex(i);
		if (p)
			return p;
	}
	return nullptr;
}

// The engine raises a script error (and the host shuts down) when a spawn
// uses a model that was not precached, so the portal precaches its own.
static bool Portal_EnsureModelPrecached(const char* pszModel)
{
	if (!v_Server_PrecacheModelLate)
	{
		Warning(eDLL_T::SERVER, "[PORTAL] late precache helper unresolved -- portal not spawned\n");
		return false;
	}
	v_Server_PrecacheModelLate(pszModel);
	const int64_t nIdx = Server_PrecacheModel_Invoke(pszModel);
	if (nIdx == 0xFFFFFFFFLL || nIdx < 0)
	{
		Warning(eDLL_T::SERVER, "[PORTAL] precache failed for '%s' -- portal not spawned\n", pszModel);
		return false;
	}
	return true;
}

static void* Portal_CreateRaw(uint32_t ownerHandle, int slot)
{
	if (!Portal_EnsureModelPrecached(kPortalModelNames[slot]))
		return nullptr;
	void** dict = (void**)v_GetEntityFactory();
	if (!dict || !dict[0])
		return nullptr;
	typedef __int64 (__fastcall* PFN_DictCreate)(void**, const char*);
	PFN_DictCreate pfnCreate = *(PFN_DictCreate*)((uintptr_t)*dict + 8);
	__int64 netResult = pfnCreate(dict, kPortalDictName);
	if (!netResult)
		return nullptr;
	uintptr_t entity = *(__int64*)(netResult + 8);
	if (!entity)
		return nullptr;
	// CBaseProp spawn removes a prop whose m_ModelName (+0x68) is empty.
	*reinterpret_cast<const char**>(entity + 0x68) = kPortalModelNames[slot];
	if (v_ActivateEntity)
		v_ActivateEntity(0xFFFFFFFF, entity);
	if (v_DispatchSpawn)
		v_DispatchSpawn(entity);
	if (*reinterpret_cast<const uint32_t*>(entity + 0x230) & 1) // m_iEFlags EFL_KILLME
	{
		Warning(eDLL_T::SERVER, "[PORTAL] spawn removed the portal entity -- not registered\n");
		return nullptr;
	}

	const uint32_t portalHandle = *reinterpret_cast<const uint32_t*>(entity + 0x08);
	bool stored = false;
	for (int i = 0; i < kPortalRegistryCap; ++i)
	{
		if (!s_portalRegistry[i].m_portalPtr)
		{
			s_portalRegistry[i].m_ownerHandle = ownerHandle;
			s_portalRegistry[i].m_portalHandle = portalHandle;
			s_portalRegistry[i].m_portalPtr = reinterpret_cast<void*>(entity);
			s_portalRegistry[i].m_slot = slot;
			s_portalRegistry[i].m_halfW = kPortalDefaultHalfW;
			s_portalRegistry[i].m_halfH = kPortalDefaultHalfH;
			s_portalRegistry[i].m_flOpenStart = -1.0f;
			memset(s_portalRegistry[i].m_tail, 0, sizeof(s_portalRegistry[i].m_tail));
			*reinterpret_cast<uint32_t*>(s_portalRegistry[i].m_tail + kPortalOffLinked) = kInvalidHandle;
			s_portalRegistry[i].m_tail[kPortalOffIs2] = slot == 1 ? 1 : 0;
			*reinterpret_cast<float*>(s_portalRegistry[i].m_tail + kPortalOffHalfW) = kPortalDefaultHalfW;
			*reinterpret_cast<float*>(s_portalRegistry[i].m_tail + kPortalOffHalfH) = kPortalDefaultHalfH;
			stored = true;
			break;
		}
	}
	if (!stored)
		Warning(eDLL_T::SERVER, "[PORTAL] registry full -- portal unowned\n");
	return reinterpret_cast<void*>(entity);
}

void* PortalEntity_Acquire(uint32_t ownerHandle, int slot, bool* outReused)
{
	if (outReused)
		*outReused = false;
	if (slot < 0 || slot > 1)
		return nullptr;
	for (int i = 0; i < kPortalRegistryCap; ++i)
	{
		PortalRegEntry_t& e = s_portalRegistry[i];
		if (e.m_ownerHandle == ownerHandle && e.m_slot == slot)
		{
			if (Portal_IsLive(e))
			{
				if (outReused)
					*outReused = true;
				return const_cast<void*>(e.m_portalPtr);
			}
			e = PortalRegEntry_t{};
		}
	}
	return Portal_CreateRaw(ownerHandle, slot);
}

void PortalEntity_NewLocation(void* pEntity, const Vector3D& pos, const QAngle& ang)
{
	Portal_Move(pEntity, pos, ang);
}

bool PortalEntity_GetInfo(const void* pEntity, PortalPlacementInfo_t* out)
{
	if (!pEntity || !out)
		return false;
	for (int i = 0; i < kPortalRegistryCap; ++i)
	{
		const PortalRegEntry_t& e = s_portalRegistry[i];
		if (e.m_portalPtr != pEntity)
			continue;
		if (!Portal_IsLive(e))
			return false;
		out->m_portalPtr = e.m_portalPtr;
		out->m_ownerHandle = e.m_ownerHandle;
		out->m_slot = e.m_slot;
		out->m_pos = e.m_pos;
		out->m_ang = e.m_ang;
		out->m_halfW = e.m_halfW;
		out->m_halfH = e.m_halfH;
		return true;
	}
	return false;
}

int PortalEntity_VisitActive(PortalVisitFn fn, void* ctx)
{
	if (!fn)
		return 0;
	int n = 0;
	for (int i = 0; i < kPortalRegistryCap; ++i)
	{
		const PortalRegEntry_t& e = s_portalRegistry[i];
		if (!e.m_portalPtr || !Portal_IsLive(e))
			continue;
		PortalPlacementInfo_t info;
		info.m_portalPtr = e.m_portalPtr;
		info.m_ownerHandle = e.m_ownerHandle;
		info.m_slot = e.m_slot;
		info.m_pos = e.m_pos;
		info.m_ang = e.m_ang;
		info.m_halfW = e.m_halfW;
		info.m_halfH = e.m_halfH;
		++n;
		if (!fn(&info, ctx))
			break;
	}
	return n;
}

int PortalEntity_CloseOwnerPair(uint32_t ownerHandle)
{
	int n = 0;
	for (int i = 0; i < kPortalRegistryCap; ++i)
	{
		PortalRegEntry_t& e = s_portalRegistry[i];
		if (!e.m_portalPtr || e.m_ownerHandle != ownerHandle)
			continue;
		if (Portal_IsLive(e))
		{
			uint8_t* tail = Portal_Tail(const_cast<void*>(e.m_portalPtr));
			if (tail)
			{
				tail[kPortalOffActive] = 0;
				*reinterpret_cast<uint32_t*>(tail + kPortalOffLinked) = kInvalidHandle;
			}
			MarkEntityEdictDirty(const_cast<void*>(e.m_portalPtr));
			++n;
		}
		e = PortalRegEntry_t{};
	}
	if (n > 0 && portal_debug.GetBool())
		Warning(eDLL_T::SERVER, "[PORTAL] close owner=0x%08X portals=%d\n", ownerHandle, n);
	return n;
}

int PortalEntity_ActiveCount(void)
{
	int n = 0;
	for (int i = 0; i < kPortalRegistryCap; ++i)
	{
		if (s_portalRegistry[i].m_portalPtr && Portal_IsLive(s_portalRegistry[i]))
			++n;
	}
	return n;
}

int PortalEntity_OnEntityDestroyed(const void* pEntity)
{
	if (!pEntity)
		return 0;
	uint32_t entHandle = 0;
	if (DTExtend_IsSafeToRead(reinterpret_cast<const uint8_t*>(pEntity) + 0x08, sizeof(uint32_t)))
		entHandle = *reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(pEntity) + 0x08);
	for (int i = 0; i < kPortalRegistryCap; ++i)
	{
		const PortalRegEntry_t& e = s_portalRegistry[i];
		if (!e.m_portalPtr)
			continue;
		if (e.m_portalPtr == pEntity || (entHandle && e.m_ownerHandle == entHandle))
			return PortalEntity_CloseOwnerPair(e.m_ownerHandle);
	}
	return 0;
}

static void CC_PortalDevSpawn(const CCommand& args)
{
	if (!g_pServer || !g_pServer->IsActive())
	{
		Msg(eDLL_T::SERVER, "portal_dev_spawn: server not active\n");
		return;
	}
	if (!s_portalRegistered)
	{
		Msg(eDLL_T::SERVER, "portal_dev_spawn: portal class not registered\n");
		return;
	}
	int idx = 0;
	if (args.ArgC() >= 2)
		idx = atoi(args.Arg(1));
	if (idx < 0 || idx > 1)
	{
		Msg(eDLL_T::SERVER, "usage 'portal_dev_spawn': <0|1>\n");
		return;
	}
	CPlayer* pPlayer = Portal_FirstPlayer();
	if (!pPlayer)
	{
		Msg(eDLL_T::SERVER, "portal_dev_spawn: no player\n");
		return;
	}
	Vector3D eye(0, 0, 0);
	QAngle eyeAng(0, 0, 0);
	pPlayer->EyePosition(&eye);
	pPlayer->EyeAngles(&eyeAng);
	Vector3D dir(0, 0, 0);
	Portal_AnglesToDir(eyeAng.x, eyeAng.y, dir);
	Vector3D end(eye.x + dir.x * kPortalTraceLen,
		eye.y + dir.y * kPortalTraceLen,
		eye.z + dir.z * kPortalTraceLen);
	if (!g_pEngineTraceServer)
	{
		Msg(eDLL_T::SERVER, "portal_dev_spawn: no engine trace\n");
		return;
	}
	Ray_t ray;
	ray.Init(eye, end, 0x3f800000, 0);
	trace_t tr;
	memset(&tr, 0, sizeof(tr));
	tr.fraction = 1.0f;
	tr.endpos = end;
	CTraceFilterSimple filter(reinterpret_cast<const IHandleEntity*>(pPlayer), 9);
	g_pEngineTraceServer->TraceRayFiltered(ray, TRACE_MASK_PLAYERSOLID, &filter, &tr);
	if (tr.fraction >= 1.0f)
	{
		Msg(eDLL_T::SERVER, "portal_dev_spawn: no surface in range\n");
		return;
	}
	const Vector3D& n = tr.plane.normal;
	Vector3D pos(tr.endpos.x + n.x * 2.0f, tr.endpos.y + n.y * 2.0f, tr.endpos.z + n.z * 2.0f);
	QAngle ang(0, 0, 0);
	Portal_NormalToAngles(n, ang);

	const uint32_t ownerHandle = *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<uintptr_t>(pPlayer) + 0x08);
	bool reused = false;
	void* pPortal = PortalEntity_Acquire(ownerHandle, idx, &reused);
	if (!pPortal)
	{
		Msg(eDLL_T::SERVER, "portal_dev_spawn: registry full\n");
		return;
	}
	Portal_Move(pPortal, pos, ang);
	Warning(eDLL_T::SERVER, "[PORTAL] spawn %s idx=%d ent=0x%p owner=0x%08X pos=(%.1f %.1f %.1f)\n",
		reused ? "moved" : "created", idx, pPortal, ownerHandle, pos.x, pos.y, pos.z);
}

static void CC_PortalDevClear(const CCommand& /*args*/)
{
	int n = 0;
	for (int i = 0; i < kPortalRegistryCap; ++i)
	{
		PortalRegEntry_t& e = s_portalRegistry[i];
		if (!e.m_portalPtr)
			continue;
		if (Portal_IsLive(e))
		{
			uint8_t* tail = Portal_Tail(const_cast<void*>(e.m_portalPtr));
			if (tail)
			{
				tail[kPortalOffActive] = 0;
				*reinterpret_cast<uint32_t*>(tail + kPortalOffLinked) = kInvalidHandle;
			}
			MarkEntityEdictDirty(const_cast<void*>(e.m_portalPtr));
			++n;
		}
		e = PortalRegEntry_t{};
	}
	Warning(eDLL_T::SERVER, "[PORTAL] clear deactivated=%d\n", n);
}

static ConCommand portal_dev_spawn("portal_dev_spawn", CC_PortalDevSpawn,
	"Dev-only: place a portal where the first player looks. Usage: portal_dev_spawn <0|1>",
	FCVAR_DEVELOPMENTONLY);
static ConCommand portal_dev_clear("portal_dev_clear", CC_PortalDevClear,
	"Dev-only: deactivate all portals.", FCVAR_DEVELOPMENTONLY);

static void* Portal_EntityFromStack(HSQUIRRELVM v, SQInteger sqIdx)
{
	const SQObjectPtr& o = stack_get(v, sqIdx);
	if (sq_isnull(o))
		return nullptr;
	if (o._type != OT_ENTITY || !o._unVal.pInstance)
		return nullptr;
	return *reinterpret_cast<void**>(
		reinterpret_cast<uintptr_t>(o._unVal.pInstance) + 0x50);
}

static SQRESULT ServerScript_PortalIsPortal(HSQUIRRELVM v)
{
	void* pEnt = Portal_EntityFromStack(v, 2);
	sq_pushbool(v, (pEnt && Portal_IsPortalEntity(pEnt)) ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void Portal_RegisterServerNatives(CSquirrelVM* s)
{
	if (!s)
		return;
	Script_RegisterFuncNamed(s, "IsPortal",
		"Server_Script_PortalIsPortal",
		"Returns true when the entity is a portal prop",
		"bool", "entity ent", false,
		ServerScript_PortalIsPortal);
	Msg(eDLL_T::SERVER, "[PORTAL] server natives registered\n");
}
