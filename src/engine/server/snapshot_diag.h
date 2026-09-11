//=============================================================================//
//
// Purpose: Exclude NaN-bbox entities from the spatial-grid build.
// PRESPAWN CPlayer bbox readers can return NaN; fminf/fmaxf stick the world
// sentinels and the grid memset wedges.
//
//=============================================================================//
#ifndef SNAPSHOT_DIAG_H
#define SNAPSHOT_DIAG_H

#include "thirdparty/detours/include/idetour.h"
#include <cstdint>
#include <cstddef>

uint64_t Bridge_PakHash(const char* pakPath);
uint64_t Bridge_PakFindAssetVoid(uint64_t hash);

// Returns the settings-data byte offset of a player-layout field, or
// 0xFFFFFFFF when the loaded layout does not declare it.
uint32_t Bridge_LookupPlayerSettingsFieldOffset(const char* pszFieldName);

// True once settings_player_layout has been resolved from the runtime table.
bool Bridge_HasPlayerSettingsLayout(void);

void WeaponSelectMirror_TickServer();
void WeapSelMirror_SetSuppress(bool bSuppress);
void WeaponSelectMirror_GetAdr(void);

// Clears per-entity settings tracking. Arms pack freeze so workers do not
// memmove off entities LevelShutdown is freeing.
void SnapshotDiag_LevelShutdown();

// When frozen, SendSnapshot takes the keepalive path and does not pack entities.
void SnapshotDiag_SetPackFrozen(bool bFrozen);
bool SnapshotDiag_IsPackFrozen(void);
int SnapshotDiag_PackFreezeLogInc(void);

void Bridge_RunCanonicalClassSettingsInit(const char* siteTag);

int Bridge_GetWireSnapshotTick(void);

void EnsurePlayerSettingsApplied(uint64_t entPtr, const char* siteTag);
void EnsureObserverModeTrackers(void);

uintptr_t SnapshotRing_Mgr(void);
int SnapshotRing_Depth(void);
bool SnapshotRing_HasTick(int nTick, int* pOldest, int* pNewest);

struct S21Bridge_ClassMeta
{
	int classId;
	uint16_t propCount;
};
S21Bridge_ClassMeta S21Bridge_LookupClassMeta(const char* className);

static constexpr uint32_t kS3_RVA_ServerClassesByID = 0xC05F740;
static constexpr uint32_t kS3_RVA_NumServerClasses  = 0xBFCBA9C;
static constexpr uint32_t kS3_SC_NetworkName        = 0x00;
static constexpr uint32_t kS3_SC_SendTable          = 0x08;
static constexpr uint32_t kS3_SC_ClassID            = 0x18;
static constexpr uint32_t kS3_ST_NetTableName       = 0x4B8;
static constexpr uint32_t kS3_ST_Precalc            = 0x4C0;
static constexpr uint32_t kS3_PC_FlatCount          = 0x10;

inline bool S3_PtrLooksHeap(uintptr_t p)
{
	return p >= 0x10000ULL && p < 0x800000000000ULL;
}

inline uintptr_t S3_RdQ(uintptr_t a)
{
	if (!a || !S3_PtrLooksHeap(a))
		return 0;
	return *reinterpret_cast<uintptr_t*>(a);
}

inline int S3_RdD(uintptr_t a)
{
	if (!a || !S3_PtrLooksHeap(a))
		return 0;
	return *reinterpret_cast<int*>(a);
}

inline void S3_RdStr(uintptr_t a, char* out, size_t sz)
{
	out[0] = 0;
	if (!a || sz < 2 || !S3_PtrLooksHeap(a))
		return;
	const char* s = reinterpret_cast<const char*>(a);
	size_t i = 0;
	for (; i < sz - 1; ++i)
	{
		const char c = s[i];
		if (!c)
			break;
		out[i] = (c >= 32 && c < 127) ? c : '.';
	}
	out[i] = 0;
}

inline bool SGE_TryReadU64(uintptr_t addr, uint64_t& out)
{
	__try
	{
		out = *reinterpret_cast<const uint64_t*>(addr);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		out = 0;
		return false;
	}
}

inline bool SGE_TryReadU32(uintptr_t addr, uint32_t& out)
{
	__try
	{
		out = *reinterpret_cast<const uint32_t*>(addr);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		out = 0;
		return false;
	}
}

inline void (*v_CSpatialPartition_BuildEntityGrid)(int64_t a1) = nullptr;
inline int64_t (*v_Server_PrecacheModel)(uint8_t* a1) = nullptr;
inline int64_t (*v_PlayerAnimUpdate)(int64_t player) = nullptr;
inline uint8_t* s_settingsStrPatchSite = nullptr;
inline int64_t (__fastcall* v_MapEntity_ParseEntity)(void** a1, int64_t a2, int64_t a3) = nullptr;
inline bool (__fastcall* v_HasGibModel)(int64_t entity) = nullptr;
inline bool (__fastcall* v_HasGibModel_Twin)(int64_t entity) = nullptr;
inline void (__fastcall* v_GibSpawn)(int64_t thisptr) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VSnapshotDiag : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CSpatialPartition::BuildEntityGrid",
			v_CSpatialPartition_BuildEntityGrid);
		WeaponSelectMirror_GetAdr();
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

int64_t Server_PrecacheModel_Invoke(const char* modelName);

// Engine bail is `!a1 || !*a1`; (BYTE*)-1 from an uninit SettingsBlock still AVs.
///////////////////////////////////////////////////////////////////////////////
class VPrecacheModelGuard : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("Server_PrecacheModel", v_Server_PrecacheModel);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

// Apply spectator SettingsBlock before PlayerAnimUpdate reads a -1 layout sentinel.
///////////////////////////////////////////////////////////////////////////////
class VPlayerAnimUpdateGuard : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("PlayerAnimUpdate", v_PlayerAnimUpdate);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

// Engine NULL-checks classActivityModifier; -1 still passes and AVs on deref.
///////////////////////////////////////////////////////////////////////////////
class VSettingsStringGuard : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CPlayer settings string guard site",
			reinterpret_cast<void*>(s_settingsStrPatchSite));
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

// Peek classname and return the stream unchanged so the brace-skipper stays in sync.
///////////////////////////////////////////////////////////////////////////////
class VMapEntitySkipper : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("MapEntity_ParseEntity", v_MapEntity_ParseEntity);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
class VGibFinderGuard : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("HasGibModel", v_HasGibModel);
		LogFunAdr("HasGibModel_Twin", v_HasGibModel_Twin);
		LogFunAdr("GibSpawn", v_GibSpawn);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////


#endif // SNAPSHOT_DIAG_H
