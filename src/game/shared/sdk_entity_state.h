//=============================================================================
//
// Purpose: EHandle-keyed entity state. SDKEntityMap auto-evicts on destroy.
// Single-threaded per side -- do not call from background threads.
//
//=============================================================================
#ifndef SDK_ENTITY_STATE_H
#define SDK_ENTITY_STATE_H

#include "thirdparty/detours/include/idetour.h"
#include "public/const.h"
#include "public/game/shared/basehandle.h"
#include <unordered_map>
#include <cstdint>

// Listen-server runs both lists; subscribers register for one side.
enum class ESide : uint8_t
{
	Server = 0,
	Client = 1,
	Count  = 2
};

// Distinct type so a raw entity index cannot be passed where a handle is required.
struct SDKEntityHandle
{
	uint32_t m_Index;   // mirrors CBaseHandle::m_Index encoding

	SDKEntityHandle() : m_Index(INVALID_EHANDLE_INDEX) {}
	explicit SDKEntityHandle(uint32_t raw) : m_Index(raw) {}
	explicit SDKEntityHandle(const CBaseHandle& h) : m_Index(static_cast<uint32_t>(h.ToInt())) {}

	bool        IsValid() const         { return m_Index != INVALID_EHANDLE_INDEX; }
	int         GetSerialNumber() const { return static_cast<int>(m_Index >> NUM_SERIAL_NUM_SHIFT_BITS); }
	uint32_t    Raw() const             { return m_Index; }

	CBaseHandle ToBaseHandle() const
	{
		// CBaseHandle has no public ctor that accepts a raw uint32, so we use
		// UnsafeFromIndex which just stores the value verbatim.
		return CBaseHandle::UnsafeFromIndex(static_cast<int>(m_Index));
	}

	bool operator==(const SDKEntityHandle& o) const { return m_Index == o.m_Index; }
	bool operator!=(const SDKEntityHandle& o) const { return m_Index != o.m_Index; }
};

// S3 memory EHANDLE is (serial<<16)|index. S21 RecvProxy_IntToEHandle does
// Init(value&0x3FFF, value>>14) with invalid 0xFFFFFF. Native SendPropEHandle
// already packs; only our value-proxied ints need this.
inline int32_t SDKEntityState_PackS21RecvEHandle(int32_t s3Handle)
{
	if (s3Handle == static_cast<int32_t>(INVALID_EHANDLE_INDEX) || s3Handle == 0)
		return 0x00FFFFFF;
	const int index = s3Handle & 0xFFFF;
	const int serial = (s3Handle >> 16) & 0xFFFF;
	return (serial << 14) | (index & 0x3FFF);
}

struct SDKEntityHandleHasher
{
	size_t operator()(const SDKEntityHandle& h) const noexcept
	{
		return std::hash<uint32_t>{}(h.m_Index);
	}
};

// m_RefEHandle.m_Index at entity+0x8 (IHandleEntity).
SDKEntityHandle SDKEntityState_GetHandle(const void* pEntity);

// LookupEntity serial-validates; never returns a reused slot as the old entity.
void* SDKEntityState_Resolve(SDKEntityHandle h, ESide side);

// Fires before chaining to OnRemoveEntity so the pointer is still valid.
using EntityDestroyCb = void(*)(SDKEntityHandle handle, void* pEntity, ESide side, void* userData);

struct EntityDestroySubscription;   // opaque

EntityDestroySubscription* SDKEntityState_SubscribeDestroy(EntityDestroyCb cb, void* userData, ESide side);
void                       SDKEntityState_UnsubscribeDestroy(EntityDestroySubscription* sub);

// Level shutdown: fire destroy with INVALID handle so maps drop storage.
void SDKEntityState_FlushAll(ESide side);

// Install vtable hooks. Called once after g_serverEntityList /
// g_clientEntityList globals have been resolved. Idempotent. Logs the
// resolved function addresses so they can be cross-checked against.
void SDKEntityState_InstallHooks();
void SDKEntityState_UninstallHooks();

// Key on EHandle (index+serial), not a raw pointer: ABA reuse is impossible; auto-evicts on destroy.
class ISDKEntityMapDumpable
{
public:
	virtual const char* DebugName() const = 0;
	virtual ESide       Side()      const = 0;
	virtual size_t      Size()      const = 0;
	virtual uint64_t    Inserts()   const = 0;
	virtual uint64_t    Erases()    const = 0;
};

void SDKEntityState_RegisterDumpable(ISDKEntityMapDumpable* p);
void SDKEntityState_UnregisterDumpable(ISDKEntityMapDumpable* p);

template <typename T>
class SDKEntityMap : public ISDKEntityMapDumpable
{
public:
	SDKEntityMap(ESide side, const char* debugName)
		: m_side(side), m_debugName(debugName), m_inserts(0), m_erases(0)
	{
		m_sub = SDKEntityState_SubscribeDestroy(&SDKEntityMap::OnDestroyTrampoline, this, side);
		SDKEntityState_RegisterDumpable(this);
	}

	~SDKEntityMap()
	{
		SDKEntityState_UnregisterDumpable(this);
		if (m_sub)
			SDKEntityState_UnsubscribeDestroy(m_sub);
	}

	// --- pointer-keyed conveniences (drop-in replacement for the old API) ---
	T& operator[](const void* pEntity)             { return (*this)[SDKEntityState_GetHandle(pEntity)]; }
	T* Find(const void* pEntity)                   { return Find(SDKEntityState_GetHandle(pEntity)); }
	bool Erase(const void* pEntity)                { return Erase(SDKEntityState_GetHandle(pEntity)); }

	// --- handle-keyed primitives ---
	T& operator[](SDKEntityHandle h)
	{
		// Invalid-handle writes go to a sink slot so they cannot corrupt storage.
		if (!h.IsValid())
			return m_invalidSink;

		auto it = m_storage.find(h);
		if (it == m_storage.end())
		{
			m_inserts++;
			it = m_storage.emplace(h, T{}).first;
		}
		return it->second;
	}

	T* Find(SDKEntityHandle h)
	{
		if (!h.IsValid()) return nullptr;
		auto it = m_storage.find(h);
		return (it == m_storage.end()) ? nullptr : &it->second;
	}

	bool Erase(SDKEntityHandle h)
	{
		if (!h.IsValid()) return false;
		auto it = m_storage.find(h);
		if (it == m_storage.end()) return false;
		m_storage.erase(it);
		m_erases++;
		return true;
	}

	void Clear()
	{
		m_erases += static_cast<uint64_t>(m_storage.size());
		m_storage.clear();
	}

	// ISDKEntityMapDumpable
	const char* DebugName() const override { return m_debugName; }
	ESide       Side()      const override { return m_side; }
	size_t      Size()      const override { return m_storage.size(); }
	uint64_t    Inserts()   const override { return m_inserts; }
	uint64_t    Erases()    const override { return m_erases; }

	using const_iterator = typename std::unordered_map<SDKEntityHandle, T, SDKEntityHandleHasher>::const_iterator;
	const_iterator begin() const { return m_storage.begin(); }
	const_iterator end()   const { return m_storage.end();   }

private:
	static void OnDestroyTrampoline(SDKEntityHandle h, void* /*pEnt*/, ESide /*side*/, void* userData)
	{
		auto* self = static_cast<SDKEntityMap<T>*>(userData);

		// FlushAll fires with an invalid handle as the "drop everything" signal.
		if (!h.IsValid())
		{
			self->Clear();
			return;
		}

		auto it = self->m_storage.find(h);
		if (it != self->m_storage.end())
		{
			self->m_storage.erase(it);
			self->m_erases++;
		}
	}

	std::unordered_map<SDKEntityHandle, T, SDKEntityHandleHasher> m_storage;
	T                                  m_invalidSink{};   // operator target for invalid handles
	EntityDestroySubscription*         m_sub = nullptr;
	ESide                              m_side;
	const char*                        m_debugName;
	uint64_t                           m_inserts;
	uint64_t                           m_erases;
};

// Register after VServerEntityList / VClientEntityList so list pointers are live.
class VSDKEntityState : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const { }
	virtual void GetVar(void) const;
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // SDK_ENTITY_STATE_H
