//=============================================================================
//
// Purpose: entity-destruction observer + handle helpers.
// OnRemoveEntity slot 1; dispatch before the original so the pointer is still valid.
//
//=============================================================================
#include "core/stdafx.h"
#include "tier0/memaddr.h"
#include "tier0/dbg.h"
#include "game/shared/sdk_entity_state.h"

#include <vector>
#include <cstdio>

// Resolve list pointers via pattern; this TU is game_shared_static and dedi has no g_clientEntityList.
static void* s_serverEntList = nullptr;
static void* s_clientEntList = nullptr;

// Intrusive list so add/remove during fan-out cannot invalidate the walk pointer.
struct SubscriberNode
{
	EntityDestroyCb cb;
	void*           userData;
	SubscriberNode* next;
	bool            alive;     // false = pending reap
};

struct EntityDestroySubscription
{
	SubscriberNode node;
	ESide          side;
};

static SubscriberNode*  s_subHead[(int)ESide::Count]      = { nullptr, nullptr };
static bool             s_hooksInstalled[(int)ESide::Count] = { false, false };
static uint64_t         s_destroyFiredCount[(int)ESide::Count]   = { 0, 0 };
static uint64_t         s_destroyCallbackCount[(int)ESide::Count] = { 0, 0 };
static int              s_dispatchDepth = 0;

// Originals stashed by HookVirtualMethod
using OnRemoveEntityFn = void(__fastcall*)(void* pThis, void* pHandleEntity, const CBaseHandle& handle);
static OnRemoveEntityFn s_origOnRemove[(int)ESide::Count] = { nullptr, nullptr };

// Dumpable registry (debug)
static std::vector<ISDKEntityMapDumpable*> s_dumpables;

// OnAdd (slot 0), OnRemove (slot 1) -- verified by reading
// entitylist_serverbase.h:50-54 and entitylist_clientbase.h:47-51.
static constexpr ptrdiff_t kOnRemoveEntityVtableSlot = 1;

//-----------------------------------------------------------------------------
// Subscriber list management
//-----------------------------------------------------------------------------
static void ReapDeadSubscribers(ESide side)
{
	SubscriberNode** pp = &s_subHead[(int)side];
	while (*pp)
	{
		SubscriberNode* n = *pp;
		if (!n->alive)
		{
			*pp = n->next;
			// Free the wrapping subscription struct, not the bare node
			delete reinterpret_cast<EntityDestroySubscription*>(
				reinterpret_cast<char*>(n) - offsetof(EntityDestroySubscription, node));
		}
		else
		{
			pp = &n->next;
		}
	}
}

EntityDestroySubscription* SDKEntityState_SubscribeDestroy(EntityDestroyCb cb, void* userData, ESide side)
{
	if (!cb) return nullptr;

	auto* sub = new EntityDestroySubscription();
	sub->side          = side;
	sub->node.cb       = cb;
	sub->node.userData = userData;
	sub->node.alive    = true;
	sub->node.next     = s_subHead[(int)side];
	s_subHead[(int)side] = &sub->node;
	return sub;
}

void SDKEntityState_UnsubscribeDestroy(EntityDestroySubscription* sub)
{
	if (!sub) return;
	sub->node.alive = false;
	// Defer actual unlink to the next reap pass (avoids invalidating an in-flight dispatch).
	if (s_dispatchDepth == 0)
		ReapDeadSubscribers(sub->side);
}

void SDKEntityState_RegisterDumpable(ISDKEntityMapDumpable* p)
{
	if (p) s_dumpables.push_back(p);
}

void SDKEntityState_UnregisterDumpable(ISDKEntityMapDumpable* p)
{
	for (auto it = s_dumpables.begin(); it != s_dumpables.end(); ++it)
	{
		if (*it == p) { s_dumpables.erase(it); return; }
	}
}

//-----------------------------------------------------------------------------
// Dispatch
//-----------------------------------------------------------------------------
static void DispatchDestroy(SDKEntityHandle h, void* pEnt, ESide side)
{
	s_destroyFiredCount[(int)side]++;

	s_dispatchDepth++;
	SubscriberNode* n = s_subHead[(int)side];
	while (n)
	{
		SubscriberNode* next = n->next;   // capture before callback (it may unsubscribe)
		if (n->alive)
		{
			s_destroyCallbackCount[(int)side]++;
			n->cb(h, pEnt, side, n->userData);
		}
		n = next;
	}
	s_dispatchDepth--;

	if (s_dispatchDepth == 0)
		ReapDeadSubscribers(side);
}

// Both trampolines compile; install only if that side's list pointer resolved.
static void __fastcall Hook_ServerOnRemoveEntity(void* pThis, void* pHandleEntity, const CBaseHandle& h)
{
	DispatchDestroy(SDKEntityHandle(h), pHandleEntity, ESide::Server);
	if (s_origOnRemove[(int)ESide::Server])
		s_origOnRemove[(int)ESide::Server](pThis, pHandleEntity, h);
}

static void __fastcall Hook_ClientOnRemoveEntity(void* pThis, void* pHandleEntity, const CBaseHandle& h)
{
	DispatchDestroy(SDKEntityHandle(h), pHandleEntity, ESide::Client);
	if (s_origOnRemove[(int)ESide::Client])
		s_origOnRemove[(int)ESide::Client](pThis, pHandleEntity, h);
}

// Vtables off s_serverEntList / s_clientEntList. Patterns resolve in GetVar.
void SDKEntityState_InstallHooks()
{
	if (s_serverEntList && !s_hooksInstalled[(int)ESide::Server])
	{
		uintptr_t vtbl = *reinterpret_cast<uintptr_t*>(s_serverEntList);
		uintptr_t origSlot = *reinterpret_cast<uintptr_t*>(vtbl + kOnRemoveEntityVtableSlot * sizeof(void*));
		CMemory::HookVirtualMethod(vtbl, (void*)Hook_ServerOnRemoveEntity,
			kOnRemoveEntityVtableSlot, (void**)&s_origOnRemove[(int)ESide::Server]);
		s_hooksInstalled[(int)ESide::Server] = true;
		DevMsg(eDLL_T::SERVER,
			"[SDKEntState] server OnRemoveEntity hooked (entlist=%p vtbl=%p slot=%lld orig=%p hook=%p)\n",
			s_serverEntList, (void*)vtbl, (long long)kOnRemoveEntityVtableSlot,
			(void*)origSlot, (void*)Hook_ServerOnRemoveEntity);
	}
	else if (!s_serverEntList)
	{
		Warning(eDLL_T::SERVER, "[SDKEntState] server entity list pattern not resolved; server hook skipped\n");
	}

	if (s_clientEntList && !s_hooksInstalled[(int)ESide::Client])
	{
		uintptr_t vtbl = *reinterpret_cast<uintptr_t*>(s_clientEntList);
		uintptr_t origSlot = *reinterpret_cast<uintptr_t*>(vtbl + kOnRemoveEntityVtableSlot * sizeof(void*));
		CMemory::HookVirtualMethod(vtbl, (void*)Hook_ClientOnRemoveEntity,
			kOnRemoveEntityVtableSlot, (void**)&s_origOnRemove[(int)ESide::Client]);
		s_hooksInstalled[(int)ESide::Client] = true;
		DevMsg(eDLL_T::CLIENT,
			"[SDKEntState] client OnRemoveEntity hooked (entlist=%p vtbl=%p slot=%lld orig=%p hook=%p)\n",
			s_clientEntList, (void*)vtbl, (long long)kOnRemoveEntityVtableSlot,
			(void*)origSlot, (void*)Hook_ClientOnRemoveEntity);
	}
	// Note: client entlist won't resolve in dedicated build (no client engine);
	// silently skipping is correct in that case. We Warn elsewhere only if the
	// dedicated side itself is missing, which would be a real problem.
}

void SDKEntityState_UninstallHooks()
{
	if (s_hooksInstalled[(int)ESide::Server] && s_origOnRemove[(int)ESide::Server] && s_serverEntList)
	{
		uintptr_t vtbl = *reinterpret_cast<uintptr_t*>(s_serverEntList);
		void* dummy = nullptr;
		CMemory::HookVirtualMethod(vtbl, (void*)s_origOnRemove[(int)ESide::Server],
			kOnRemoveEntityVtableSlot, (void**)&dummy);
		s_hooksInstalled[(int)ESide::Server] = false;
	}
	if (s_hooksInstalled[(int)ESide::Client] && s_origOnRemove[(int)ESide::Client] && s_clientEntList)
	{
		uintptr_t vtbl = *reinterpret_cast<uintptr_t*>(s_clientEntList);
		void* dummy = nullptr;
		CMemory::HookVirtualMethod(vtbl, (void*)s_origOnRemove[(int)ESide::Client],
			kOnRemoveEntityVtableSlot, (void**)&dummy);
		s_hooksInstalled[(int)ESide::Client] = false;
	}
}

// FlushAll: destroy with INVALID handle so maps drop storage.
void SDKEntityState_FlushAll(ESide side)
{
	DispatchDestroy(SDKEntityHandle(), /*pEnt=*/nullptr, side);
}

//-----------------------------------------------------------------------------
// Helpers
//-----------------------------------------------------------------------------
SDKEntityHandle SDKEntityState_GetHandle(const void* pEntity)
{
	if (!pEntity) return SDKEntityHandle();
	// IHandleEntity::m_RefEHandle.m_Index sits at entity+0x8 (vtable at +0,
	// m_RefEHandle is the first member). Confirmed against
	// vscript_remotefunctions.cpp:219 which reads the same offset.
	uint32_t raw = *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<uintptr_t>(pEntity) + 0x8);
	return SDKEntityHandle(raw);
}

// LookupEntity without a type include: lists are opaque void* (shared TU, no C_BaseEntityList on dedi).
static constexpr ptrdiff_t kEntPtrArrayOffset    = 0x08;
static constexpr size_t    kServerEntInfoStride  = 48; // CEntInfo with m_iName+m_iClassName
static constexpr size_t    kClientEntInfoStride  = 32; // C_EntInfo without
static constexpr ptrdiff_t kEntInfoEntityOffset  = 0x00;
static constexpr ptrdiff_t kEntInfoSerialOffset  = 0x08;

void* SDKEntityState_Resolve(SDKEntityHandle h, ESide side)
{
	if (!h.IsValid())
		return nullptr;

	void* const pList = (side == ESide::Server) ? s_serverEntList : s_clientEntList;
	if (!pList)
		return nullptr;

	const uint32_t entryIdx = h.m_Index & ENT_ENTRY_MASK;
	if (entryIdx >= static_cast<uint32_t>(NUM_ENT_ENTRIES))
		return nullptr;

	const size_t stride = (side == ESide::Server)
		? kServerEntInfoStride
		: kClientEntInfoStride;

	const uint8_t* pEntry = reinterpret_cast<const uint8_t*>(pList)
		+ kEntPtrArrayOffset
		+ static_cast<size_t>(entryIdx) * stride;

	void* const pEntity = *reinterpret_cast<void* const*>(pEntry + kEntInfoEntityOffset);
	const int   serial  = *reinterpret_cast<const int*>(pEntry + kEntInfoSerialOffset);

	if (!pEntity)
		return nullptr;
	if (serial != h.GetSerialNumber())
		return nullptr; // slot reused -- handle is stale

	return pEntity;
}

//-----------------------------------------------------------------------------
// IDetour entry -- called from core/init.cpp
//-----------------------------------------------------------------------------
void VSDKEntityState::GetAdr(void) const
{
	LogVarAdr("s_serverEntList(SDKEntState)", s_serverEntList);
	LogVarAdr("s_clientEntList(SDKEntState)", s_clientEntList);
}

// Same patterns as VServerEntityList / VClientEntityList. Client pattern misses on dedi; skip that hook.
void VSDKEntityState::GetVar(void) const
{
	{
		CMemory m = Module_FindPattern(g_GameDll,
			"48 8D 0D ?? ?? ?? ?? 66 0F 7F 05 ?? ?? ?? ?? 44 89 0D");
		if (m)
			m.ResolveRelativeAddressSelf(3, 7).ResolveRelativeAddressSelf(3, 7).GetPtr(s_serverEntList);
	}
	{
		CMemory m = Module_FindPattern(g_GameDll,
			"48 8D 0D ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 44 89 0D");
		if (m)
			m.ResolveRelativeAddressSelf(3, 7).ResolveRelativeAddressSelf(3, 7).GetPtr(s_clientEntList);
	}
}

void VSDKEntityState::Detour(const bool bAttach) const
{
	if (bAttach)
		SDKEntityState_InstallHooks();
	else
		SDKEntityState_UninstallHooks();
}
