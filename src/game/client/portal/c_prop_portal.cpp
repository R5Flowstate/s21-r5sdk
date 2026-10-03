//=============================================================================//
//
// Purpose: C_Prop_Portal client class link + decode (client half).
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier1/convar.h"
#include "game/client/portal/c_prop_portal.h"
#include "game/shared/portal/portal_shared.h"
#include "game/client/cliententitylist.h"
#include "public/client_class.h"
#include "public/game/client/icliententity.h"
#include "engine/client/net_bridge_internal.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/vsquirrel_s21.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include <cstring>

static ConVar portal_cl_debug("portal_cl_debug", "0", FCVAR_DEVELOPMENTONLY,
	"Log every C_Prop_Portal once per second (0=off, 1=on).");

static constexpr int kPortalTailSize = 16;
static constexpr int kPortalOffLinked = 0;
static constexpr int kPortalOffActive = 4;
static constexpr int kPortalOffIs2 = 5;
static constexpr int kPortalOffHalfW = 8;
static constexpr int kPortalOffHalfH = 12;
static constexpr int kPortalCreateCap = 64;

struct PortalClientRec_t
{
	int m_entNum;
	int m_serial;
	const void* m_ptr;
};

static PortalClientRec_t s_portalCreates[kPortalCreateCap] = {};
static bool s_portalLinked = false;
static bool s_portalLinkAttempted = false;
static int s_portalNativeSize = 0;
static int s_portalGrownSize = 0;
static uint8_t s_portalClientClass[48] = {};
static char s_portalNetName[] = "CProp_Portal";
static char s_portalTableName[] = "DT_Prop_Portal";
static CreateClientClassFn s_portalDonorCreate = nullptr;
static const void* s_portalRecvTable = nullptr;

const void* Portal_ClientRecvTable(void)
{
	return s_portalRecvTable;
}

static bool Portal_PtrReadable(const void* p, size_t n)
{
	if (!p || n == 0 || n > 0x10000)
		return false;
	__try
	{
		volatile uint8_t v = 0;
		const volatile uint8_t* b = reinterpret_cast<const volatile uint8_t*>(p);
		v |= b[0];
		v |= b[n - 1];
		(void)v;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
	return true;
}

// S21 C_BaseEntity world pose. Read directly: the SDK's IClientEntity vtable
// is the S3 layout, so its GetAbsOrigin/GetAbsAngles slots are not safe here.
static constexpr ptrdiff_t kClientEntAbsOrigin = 0x188;
static constexpr ptrdiff_t kClientEntAbsAngles = 0x194;

static IClientEntity* Portal_LiveEntity(const PortalClientRec_t& r)
{
	void* const p = ClientEntityList_EntityAt(r.m_entNum, -1);
	return (p && p == r.m_ptr) ? reinterpret_cast<IClientEntity*>(p) : nullptr;
}

static Vector3D Portal_AbsOrigin(const void* pEntity)
{
	return *reinterpret_cast<const Vector3D*>(reinterpret_cast<const uint8_t*>(pEntity) + kClientEntAbsOrigin);
}

static QAngle Portal_AbsAngles(const void* pEntity)
{
	return *reinterpret_cast<const QAngle*>(reinterpret_cast<const uint8_t*>(pEntity) + kClientEntAbsAngles);
}

// The snapshot apply step takes the RecvTable from the networkable's own
// GetClientClass (slot 3), not from the wire class, so a donor-class object
// applies only the donor's props and stops at our appended ones. Portal
// objects get a vtable copy whose slot 3 reports CProp_Portal.
static constexpr int kNetworkableGetClientClassSlot = 3;
static constexpr int kNetworkableVtableCopySlots = 64;
static uintptr_t s_portalDonorClass = 0;
static uintptr_t s_portalNetworkableVtable[kNetworkableVtableCopySlots] = {};
static uintptr_t s_portalDonorNetworkableVtable = 0;

static void* __fastcall Portal_GetClientClass(void* /*pNetworkable*/)
{
	return s_portalClientClass;
}

static bool Portal_AdoptClientClass(IClientNetworkable* pNetworkable)
{
	uintptr_t* const pVtable = reinterpret_cast<uintptr_t*>(pNetworkable);
	const uintptr_t donorVtable = *pVtable;
	if (!s_portalDonorNetworkableVtable)
	{
		typedef uintptr_t(__fastcall* GetClientClassFn)(void*);
		const GetClientClassFn pfnDonor = reinterpret_cast<GetClientClassFn>(
			reinterpret_cast<const uintptr_t*>(donorVtable)[kNetworkableGetClientClassSlot]);
		if (!s_portalDonorClass || pfnDonor(pNetworkable) != s_portalDonorClass)
			return false;
		memcpy(s_portalNetworkableVtable, reinterpret_cast<const void*>(donorVtable), sizeof(s_portalNetworkableVtable));
		s_portalNetworkableVtable[kNetworkableGetClientClassSlot] = reinterpret_cast<uintptr_t>(&Portal_GetClientClass);
		s_portalDonorNetworkableVtable = donorVtable;
	}
	if (donorVtable != s_portalDonorNetworkableVtable)
		return false;
	*pVtable = reinterpret_cast<uintptr_t>(s_portalNetworkableVtable);
	return true;
}

static IClientNetworkable* Portal_CreateWrapper(int entNum, int serialNum)
{
	IClientNetworkable* r = nullptr;
	if (s_portalDonorCreate)
		r = s_portalDonorCreate(entNum, serialNum);
	Msg(eDLL_T::CLIENT, "[PORTAL-CL] create ent=%d serial=%d -> %p\n", entNum, serialNum, (void*)r);
	if (r && !Portal_AdoptClientClass(r))
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::CLIENT, "[PORTAL-CL] networkable vtable is not the donor's -- portal fields will not apply\n");
		}
	}
	if (r)
	{
		for (int i = 0; i < kPortalCreateCap; ++i)
		{
			if (!s_portalCreates[i].m_ptr)
			{
				s_portalCreates[i].m_entNum = entNum;
				s_portalCreates[i].m_serial = serialNum;
				// The S21 create returns the IClientNetworkable sub-object at +0x18;
				// GetClientEntity and the tail offset both use the entity base.
				s_portalCreates[i].m_ptr = reinterpret_cast<IClientNetworkable*>(
					reinterpret_cast<uint8_t*>(r) - 0x18);
				break;
			}
		}
		if (portal_cl_debug.GetBool())
		{
			Msg(eDLL_T::CLIENT, "[PORTAL-CL] create ent=%d serial=%d ptr=0x%p\n",
				entNum, serialNum, (void*)r);
		}
	}
	else
	{
		Warning(eDLL_T::CLIENT, "[PORTAL-CL] donor create returned null ent=%d\n", entNum);
	}
	return r;
}

static const PortalClientRec_t* Portal_FindRec(const void* pEntity)
{
	if (!pEntity)
		return nullptr;
	for (int i = 0; i < kPortalCreateCap; ++i)
	{
		const PortalClientRec_t& r = s_portalCreates[i];
		if (!r.m_ptr)
			continue;
		IClientEntity* const live = Portal_LiveEntity(r);
		if (live && live == pEntity)
			return &r;
	}
	return nullptr;
}

void Portal_LinkClientClass(void)
{
	if (s_portalLinkAttempted)
		return;
	s_portalLinkAttempted = true;

	uintptr_t* pHead = (uintptr_t*)NetObs_NonRewindClientClassHeadAddr();
	if (!pHead || !Portal_PtrReadable(pHead, sizeof(uintptr_t)))
	{
		Warning(eDLL_T::CLIENT, "[PORTAL-CL] class head unreadable -- link skipped\n");
		return;
	}
	uintptr_t donor = 0;
	for (int guard = 0; *pHead && guard < 4096; ++guard)
	{
		uintptr_t cc = *pHead;
		if (!Portal_PtrReadable(reinterpret_cast<const void*>(cc), 48))
			break;
		const char* nm = *(const char**)(cc + 0x10);
		// The ClientClass carries the server's network name, not the C_ class name.
		if (nm && Portal_PtrReadable(nm, 16) && strcmp(nm, "CDynamicProp") == 0)
		{
			donor = cc;
			break;
		}
		pHead = (uintptr_t*)(cc + 0x20);
		if (!Portal_PtrReadable(pHead, sizeof(uintptr_t)))
			break;
	}
	if (!donor)
	{
		Warning(eDLL_T::CLIENT, "[PORTAL-CL] CDynamicProp ClientClass not found -- link skipped\n");
		return;
	}
	const int nativeSize = *(int*)(donor + 0x2C);
	if (nativeSize < 0x1000 || nativeSize > 0x10000)
	{
		Warning(eDLL_T::CLIENT, "[PORTAL-CL] donor size %d implausible -- link skipped\n", nativeSize);
		return;
	}
	uintptr_t donorRecv = *(uintptr_t*)(donor + 0x18);
	CreateClientClassFn donorCreate = *(CreateClientClassFn*)(donor + 0x00); // m_pCreateFn
	if (!donorRecv || !Portal_PtrReadable(reinterpret_cast<const void*>(donorRecv), 0x500) || !donorCreate)
	{
		Warning(eDLL_T::CLIENT, "[PORTAL-CL] donor table/create bad (recv=0x%p create=0x%p) -- link skipped\n",
			(void*)donorRecv, (void*)donorCreate);
		return;
	}
	s_portalNativeSize = nativeSize;
	s_portalGrownSize = nativeSize + kPortalTailSize;

	uint8_t* code = reinterpret_cast<uint8_t*>(donorCreate);
	int allocHits = 0;
	int memsetHits = 0;
	uint8_t* allocAt = nullptr;
	uint8_t* memsetAt = nullptr;
	const uint32_t ns = (uint32_t)nativeSize;
	// The size immediates sit past a thread-local profiling prologue (+0x6D / +0x7B).
	constexpr int kPortalCreateScan = 0xC0;
	for (int i = 0; i + 6 <= kPortalCreateScan; ++i)
	{
		if (!Portal_PtrReadable(code + i, 6))
			break;
		if (code[i] == 0xBA && *(uint32_t*)(code + i + 1) == ns)
		{
			++allocHits;
			allocAt = code + i + 1;
		}
		if (i + 6 <= kPortalCreateScan && code[i] == 0x41 && code[i + 1] == 0xB8 &&
			*(uint32_t*)(code + i + 2) == ns)
		{
			++memsetHits;
			memsetAt = code + i + 2;
		}
	}
	if (allocHits != 1 || memsetHits != 1)
	{
		Warning(eDLL_T::CLIENT,
			"[PORTAL-CL] donor create shape unexpected (alloc=%d memset=%d) -- link skipped\n",
			allocHits, memsetHits);
		return;
	}
	DWORD oldProt = 0;
	if (!VirtualProtect(code, 0x60, PAGE_EXECUTE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::CLIENT, "[PORTAL-CL] VirtualProtect failed -- link skipped\n");
		return;
	}
	*(uint32_t*)allocAt = (uint32_t)s_portalGrownSize;
	*(uint32_t*)memsetAt = (uint32_t)s_portalGrownSize;
	DWORD tmpProt = 0;
	VirtualProtect(code, 0x60, oldProt, &tmpProt);
	FlushInstructionCache(GetCurrentProcess(), code, 0x60);
	s_portalDonorCreate = donorCreate;

	if (!Portal_PtrReadable(reinterpret_cast<const void*>(donorRecv + 0x008), sizeof(uintptr_t)))
		return;
	uint8_t** donorProps = *(uint8_t***)(donorRecv + 0x008);
	const int donorN = *(int*)(donorRecv + 0x010);
	if (!donorProps || donorN <= 0 || donorN > 512)
	{
		Warning(eDLL_T::CLIENT, "[PORTAL-CL] donor props bad (n=%d) -- link skipped\n", donorN);
		return;
	}
	uint8_t* intEx = nullptr;
	uint8_t* floatEx = nullptr;
	uint8_t* int32Ex = nullptr;
	// The dynamic prop's own table holds few scalars; its base tables (RecvProp
	// +0x20 = child RecvTable) hold the rest.
	struct ExemplarWalk
	{
		static void Run(uint8_t** props, int n, int depth, uint8_t*& ie, uint8_t*& fe, uint8_t*& i32)
		{
			if (!props || n <= 0 || n > 512 || depth > 8)
				return;
			for (int j = 0; j < n && !(ie && fe && i32); ++j)
			{
				uint8_t* p = props[j];
				if (!Portal_PtrReadable(p, 0x68))
					continue;
				const int ty = *(int*)(p + 0x00);
				const char* const nm = *(const char**)(p + 0x28);
				if (ty == 0 && !i32 && nm && Portal_PtrReadable(nm, 11) && strcmp(nm, "m_iTeamNum") == 0)
					i32 = p;
				if (ty == 0 && !ie)
					ie = p;
				else if (ty == 1 && !fe)
					fe = p;
			}
			for (int j = 0; j < n && !(ie && fe && i32); ++j)
			{
				uint8_t* p = props[j];
				if (!Portal_PtrReadable(p, 0x68))
					continue;
				const uintptr_t child = *(uintptr_t*)(p + 0x20);
				if (!child || !Portal_PtrReadable(reinterpret_cast<const void*>(child), 0x18))
					continue;
				Run(*(uint8_t***)(child + 0x008), *(int*)(child + 0x010), depth + 1, ie, fe, i32);
			}
		}
	};
	ExemplarWalk::Run(donorProps, donorN, 0, intEx, floatEx, int32Ex);
	if (!intEx || !floatEx || !int32Ex)
	{
		Warning(eDLL_T::CLIENT, "[PORTAL-CL] no int/float/int32 exemplars (int=%p float=%p int32=%p) -- link skipped\n",
			(void*)intEx, (void*)floatEx, (void*)int32Ex);
		return;
	}

	uint8_t* rt = (uint8_t*)VirtualAlloc(nullptr, 0x500, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	uint8_t** arr = (uint8_t**)VirtualAlloc(nullptr, (size_t)(donorN + 5) * sizeof(uint8_t*),
		MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	uint8_t* bodies = (uint8_t*)VirtualAlloc(nullptr, 5 * 0x68, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!rt || !arr || !bodies)
	{
		if (rt) VirtualFree(rt, 0, MEM_RELEASE);
		if (arr) VirtualFree(arr, 0, MEM_RELEASE);
		if (bodies) VirtualFree(bodies, 0, MEM_RELEASE);
		Warning(eDLL_T::CLIENT, "[PORTAL-CL] table alloc failed -- link skipped\n");
		return;
	}
	memcpy(rt, (void*)donorRecv, 0x500);
	for (int j = 0; j < donorN; ++j)
		arr[j] = donorProps[j];
	static const char* const names[5] = {
		"m_hLinkedPortal", "m_bActivated", "m_bIsPortal2",
		"m_fNetworkHalfWidth", "m_fNetworkHalfHeight"
	};
	static const int offs[5] = {
		kPortalOffLinked, kPortalOffActive, kPortalOffIs2,
		kPortalOffHalfW, kPortalOffHalfH
	};
	for (int k = 0; k < 5; ++k)
	{
		uint8_t* dst = bodies + (size_t)k * 0x68;
		// The first int exemplar writes one byte (right for the two flags); the
		// link handle needs a full 32-bit int.
		memcpy(dst, k == 0 ? int32Ex : (k < 3 ? intEx : floatEx), 0x68);
		*(const char**)(dst + 0x28) = names[k];
		*(int*)(dst + 0x04) = nativeSize + offs[k];
		arr[donorN + k] = dst;
	}
	*(uint8_t***)(rt + 0x008) = arr;
	*(int*)(rt + 0x010) = donorN + 5;
	*(const char**)(rt + 0x4C8) = s_portalTableName;
	// The copy inherits the donor's decoder (+0x4C0) and initialized flag (+0x4D1);
	// both must be clear for the table to join the engine's name lookup list.
	*(uintptr_t*)(rt + 0x4C0) = 0;
	*(uint8_t*)(rt + 0x4D1) = 0;

	typedef void(__fastcall* RecvTableInitTableFn)(void* pTable);
	RecvTableInitTableFn pfnInitTable = nullptr;
	Module_FindPattern(g_GameDll, "40 57 48 83 EC 20 80 B9 D1 04 00 00 00 48 8B F9 0F 85")
		.GetPtr(pfnInitTable);
	if (!pfnInitTable)
	{
		Warning(eDLL_T::CLIENT, "[PORTAL-CL] RecvTable init pattern unresolved -- DT_Prop_Portal would have no decoder, link skipped\n");
		return;
	}
	pfnInitTable(rt);

	uintptr_t* headNow = (uintptr_t*)NetObs_NonRewindClientClassHeadAddr();
	memset(s_portalClientClass, 0, sizeof(s_portalClientClass));
	*(uintptr_t*)(s_portalClientClass + 0x00) = (uintptr_t)&Portal_CreateWrapper; // m_pCreateFn
	*(uintptr_t*)(s_portalClientClass + 0x08) = *(uintptr_t*)(donor + 0x08);    // m_pCreateEventFn
	*(uintptr_t*)(s_portalClientClass + 0x10) = (uintptr_t)s_portalNetName;
	*(uintptr_t*)(s_portalClientClass + 0x18) = (uintptr_t)rt;
	*(uintptr_t*)(s_portalClientClass + 0x20) = *headNow;
	*(int*)(s_portalClientClass + 0x28) = -1;
	*(int*)(s_portalClientClass + 0x2C) = s_portalGrownSize;
	*headNow = (uintptr_t)s_portalClientClass;

	s_portalRecvTable = rt;
	s_portalDonorClass = donor;
	s_portalLinked = true;
	Warning(eDLL_T::CLIENT,
		"[PORTAL-CL] linked CProp_Portal (donor size=%d grown=%d donorN=%d create=0x%p)\n",
		nativeSize, s_portalGrownSize, donorN, (void*)donorCreate);
}

void Portal_ClientFrameTick(void)
{
	if (!s_portalLinked)
		return;
	if (!portal_cl_debug.GetBool())
		return;
	static double s_lastDump = 0.0;
	const double now = Plat_FloatTime();
	if (now - s_lastDump < 1.0)
		return;
	s_lastDump = now;
	int n = 0;
	for (int i = 0; i < kPortalCreateCap; ++i)
	{
		const PortalClientRec_t& r = s_portalCreates[i];
		if (!r.m_ptr)
			continue;
		IClientEntity* const live = Portal_LiveEntity(r);
		if (!live || live != r.m_ptr)
			continue;
		Vector3D org(0, 0, 0);
		__try { org = Portal_AbsOrigin(live); }
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
		const uint8_t* tail = reinterpret_cast<const uint8_t*>(live) + s_portalNativeSize;
		if (!Portal_PtrReadable(tail, kPortalTailSize))
			continue;
		uint32_t link = 0;
		uint8_t act = 0;
		uint8_t is2 = 0;
		float hw = 0.0f;
		float hh = 0.0f;
		__try
		{
			link = *reinterpret_cast<const uint32_t*>(tail + kPortalOffLinked);
			act = *(tail + kPortalOffActive);
			is2 = *(tail + kPortalOffIs2);
			hw = *reinterpret_cast<const float*>(tail + kPortalOffHalfW);
			hh = *reinterpret_cast<const float*>(tail + kPortalOffHalfH);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
		Msg(eDLL_T::CLIENT,
			"[PORTAL-CL] ent=%d org=(%.1f %.1f %.1f) link=0x%08X active=%u is2=%u halfW=%.1f halfH=%.1f\n",
			r.m_entNum, org.x, org.y, org.z, link, (unsigned)act, (unsigned)is2,
			(double)hw, (double)hh);
		++n;
	}
	Msg(eDLL_T::CLIENT, "[PORTAL-CL] tick portals=%d\n", n);
}

static void* Portal_ClientEntityFromStack(HSQUIRRELVM v, SQInteger sqIdx)
{
	const SQObjectPtr& o = stack_get(v, sqIdx);
	if (sq_isnull(o))
		return nullptr;
	if (o._type != OT_ENTITY || !o._unVal.pInstance)
		return nullptr;
	return *reinterpret_cast<void**>(
		reinterpret_cast<uintptr_t>(o._unVal.pInstance) + 0x50);
}

static SQRESULT ClientScript_PortalIsPortal(HSQUIRRELVM v)
{
	void* pEnt = Portal_ClientEntityFromStack(v, 2);
	sq_pushbool(v, (pEnt && Portal_FindRec(pEnt)) ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ClientScript_PortalGetLinked(HSQUIRRELVM v)
{
	void* pEnt = Portal_ClientEntityFromStack(v, 2);
	const PortalClientRec_t* rec = pEnt ? Portal_FindRec(pEnt) : nullptr;
	uint32_t link = 0;
	if (rec && s_portalNativeSize > 0)
	{
		__try
		{
			const uint8_t* tail = reinterpret_cast<const uint8_t*>(rec->m_ptr) + s_portalNativeSize;
			if (Portal_PtrReadable(tail, kPortalTailSize))
				link = *reinterpret_cast<const uint32_t*>(tail + kPortalOffLinked);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { link = 0; }
	}
	(void)link;
	static volatile LONG s_noPushWarn = 0;
	if (InterlockedCompareExchange(&s_noPushWarn, 1, 0) == 0)
	{
		Warning(eDLL_T::CLIENT,
			"[PORTAL-CL] Portal_GetLinked: no client PushEntity resolver yet -- returning null\n");
	}
	sq_pushnull(v);
	return SQ_OK;
}

void Portal_RegisterClientNatives(CSquirrelVM* s)
{
	if (!s)
		return;
	if (Script_RegisterFuncTC_S21(s, "IsPortal",
			reinterpret_cast<void*>(ClientScript_PortalIsPortal),
			"bool", "entity ent") == SQ_ERROR)
		Warning(eDLL_T::CLIENT, "[PORTAL-CL] IsPortal registration FAILED\n");
	if (Script_RegisterFuncTC_S21(s, "Portal_GetLinked",
			reinterpret_cast<void*>(ClientScript_PortalGetLinked),
			"entity", "entity portal") == SQ_ERROR)
		Warning(eDLL_T::CLIENT, "[PORTAL-CL] Portal_GetLinked registration FAILED\n");
	Msg(eDLL_T::CLIENT, "[PORTAL-CL] client natives registered\n");
}

// Snapshot of live portal entities for the surface renderer. Reads only;
// every dereference is guarded, a dead entity is skipped, never asserted.
int Portal_GetRenderList(PortalRenderInfo_t* const pOut, const int maxN)
{
	if (!pOut || maxN <= 0 || !s_portalLinked || s_portalNativeSize <= 0)
		return 0;
	int n = 0;
	for (int i = 0; i < kPortalCreateCap && n < maxN; ++i)
	{
		const PortalClientRec_t& r = s_portalCreates[i];
		if (!r.m_ptr)
			continue;
		IClientEntity* const live = Portal_LiveEntity(r);
		if (!live || live != r.m_ptr)
		{
			static bool s_bLoggedPtr = false;
			if (!s_bLoggedPtr)
			{
				s_bLoggedPtr = true;
				Warning(eDLL_T::CLIENT, "[PORTAL-CL] render list: ent=%d list=%p rec=%p -- pointer mismatch\n",
					r.m_entNum, (void*)live, (void*)r.m_ptr);
			}
			continue;
		}
		const uint8_t* const tail = reinterpret_cast<const uint8_t*>(live) + s_portalNativeSize;
		if (!Portal_PtrReadable(tail, kPortalTailSize))
			continue;
		{
			static bool s_bLoggedTail = false;
			if (!s_bLoggedTail)
			{
				s_bLoggedTail = true;
				Warning(eDLL_T::CLIENT, "[PORTAL-CL] render list first tail: ent=%d link=0x%08X active=%u is2=%u halfW=%.1f halfH=%.1f\n",
					r.m_entNum, *reinterpret_cast<const uint32_t*>(tail + kPortalOffLinked), (unsigned)tail[kPortalOffActive],
					(unsigned)tail[kPortalOffIs2], *reinterpret_cast<const float*>(tail + kPortalOffHalfW),
					*reinterpret_cast<const float*>(tail + kPortalOffHalfH));
			}
		}
		PortalRenderInfo_t& o = pOut[n];
		__try
		{
			const Vector3D org = Portal_AbsOrigin(live);
			const QAngle ang = Portal_AbsAngles(live);
			o.pEntity = live;
			o.entNum = r.m_entNum;
			o.origin[0] = org.x; o.origin[1] = org.y; o.origin[2] = org.z;
			o.angles[0] = ang.x; o.angles[1] = ang.y; o.angles[2] = ang.z;
			o.link = *reinterpret_cast<const uint32_t*>(tail + kPortalOffLinked);
			o.bActive = *(tail + kPortalOffActive) != 0;
			o.bIsPortal2 = *(tail + kPortalOffIs2) != 0;
			o.halfW = *reinterpret_cast<const float*>(tail + kPortalOffHalfW);
			o.halfH = *reinterpret_cast<const float*>(tail + kPortalOffHalfH);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
		if (!isfinite(o.origin[0]) || !isfinite(o.origin[1]) || !isfinite(o.origin[2])
			|| o.halfW < 0.0f || o.halfW > 512.0f || o.halfH < 0.0f || o.halfH > 512.0f)
			continue;
		++n;
	}
	return n;
}

static const PortalClientRec_t* Portal_FindRecByEntNum(int nEntNum)
{
	for (int i = 0; i < kPortalCreateCap; ++i)
	{
		const PortalClientRec_t& r = s_portalCreates[i];
		if (!r.m_ptr || r.m_entNum != nEntNum)
			continue;
		IClientEntity* const live = Portal_LiveEntity(r);
		if (live && live == r.m_ptr)
			return &r;
	}
	return nullptr;
}

static bool Portal_ReadClientTail(const void* pEntity, unsigned int* pLink, unsigned int* pActive,
	float* pHalfW, float* pHalfH)
{
	if (!pEntity || s_portalNativeSize <= 0)
		return false;
	const uint8_t* tail = reinterpret_cast<const uint8_t*>(pEntity) + s_portalNativeSize;
	if (!Portal_PtrReadable(tail, kPortalTailSize))
		return false;
	__try
	{
		if (pLink) *pLink = *reinterpret_cast<const uint32_t*>(tail + kPortalOffLinked);
		if (pActive) *pActive = *(tail + kPortalOffActive);
		if (pHalfW) *pHalfW = *reinterpret_cast<const float*>(tail + kPortalOffHalfW);
		if (pHalfH) *pHalfH = *reinterpret_cast<const float*>(tail + kPortalOffHalfH);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	return true;
}

int PortalClient_GetTeleportPairs(PortalPairState_t* pOut, int nCap)
{
	if (!pOut || nCap <= 0 || !s_portalLinked || s_portalNativeSize <= 0)
		return 0;
	int nPairs = 0;
	for (int i = 0; i < kPortalCreateCap && nPairs < nCap; ++i)
	{
		const PortalClientRec_t& r = s_portalCreates[i];
		if (!r.m_ptr)
			continue;
		IClientEntity* const live = Portal_LiveEntity(r);
		if (!live || live != r.m_ptr)
			continue;
		unsigned int nLink = 0, nActive = 0;
		float flHalfW = 0.0f, flHalfH = 0.0f;
		if (!Portal_ReadClientTail(live, &nLink, &nActive, &flHalfW, &flHalfH))
			continue;
		if (!nActive || nLink == 0 || nLink == 0xFFFFFFFFu)
			continue;
		if (!(flHalfW > 0.0f) || !(flHalfH > 0.0f) || flHalfW > 512.0f || flHalfH > 512.0f)
			continue;
		const int nExitNum = static_cast<int>(nLink & 0x3FFF);
		const PortalClientRec_t* pExitRec = Portal_FindRecByEntNum(nExitNum);
		if (!pExitRec)
			continue;
		IClientEntity* const pExitLive = Portal_LiveEntity(*pExitRec);
		if (!pExitLive || pExitLive != pExitRec->m_ptr)
			continue;
		unsigned int nExitActive = 0;
		if (!Portal_ReadClientTail(pExitLive, nullptr, &nExitActive, nullptr, nullptr) || !nExitActive)
			continue;
		Vector3D entryOrg(0, 0, 0), exitOrg(0, 0, 0);
		QAngle entryAng(0, 0, 0), exitAng(0, 0, 0);
		__try
		{
			entryOrg = Portal_AbsOrigin(live);
			entryAng = Portal_AbsAngles(live);
			exitOrg = Portal_AbsOrigin(pExitLive);
			exitAng = Portal_AbsAngles(pExitLive);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
		PortalPairState_t& pair = pOut[nPairs];
		pair.m_entryOrigin = entryOrg;
		pair.m_entryAngles = entryAng;
		pair.m_flEntryHalfW = flHalfW;
		pair.m_flEntryHalfH = flHalfH;
		pair.m_exitOrigin = exitOrg;
		pair.m_exitAngles = exitAng;
		pair.m_pEntryEntity = live;
		pair.m_pExitEntity = pExitLive;
		pair.m_nExitHandle = nLink;
		pair.m_bValid = true;
		++nPairs;
	}
	return nPairs;
}
