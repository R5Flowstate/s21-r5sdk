//=============================================================================//
//
// Purpose: Graft S21-new networked props onto S3 SendTables. Hooks
// SendTable_Init before SetupFlatPropertyArray. Server-only.
//
//=============================================================================//
#ifndef DT_EXTEND_H
#define DT_EXTEND_H

#include "thirdparty/detours/include/idetour.h"
#include "public/dt_common.h"
#include <cstdint>
#include <cstddef>

//-----------------------------------------------------------------------------
// One S21-new prop grafted onto an S3 SendTable. valBytes = per-element size;
// nElements = 0 for a scalar. insertAfter memmove's the prop after that name.
//-----------------------------------------------------------------------------
struct DTExtendProp
{
	const char*  tableName;   // e.g. "DT_BaseCombatCharacter"
	const char*  propName;    // e.g. "m_weaponTypeDisabledFlags" -- must match S21's RecvProp name
	SendPropType type;
	int          nBits;
	int          flags;       // SPROP_* (16-bit networked subset; e.g. SPROP_UNSIGNED=1)
	float        lowValue;
	float        highValue;
	int          nElements;   // 0 = scalar
	int          valBytes;    // per-element size in bytes
	const char*  insertAfter; // nullptr = append at tail
};

// Override SPROP_* on an existing SendProp after append, before precalc.
struct DTFlagOverride
{
	const char* tableName;
	const char* propName;
	int         flags;
	bool        recursive;
};

// Entity-memory offset of a registered prop. <= 0 means no slot (proxied or
// unregistered) -- 0 is the vptr.
int DTExtend_GetOffset(const char* tableName, const char* propName);

// Native SendProp offset on this entity's ServerClass. Twin-proof: CWeaponX
// twins register the same names 0x110 apart. Cached per (ServerClass, propName).
int DTExtend_FindNativePropOffset(const void* pEntity, const char* propName);

// True for an s_s21Classes instance, including before DispatchSpawn. Bridge
// fields live in the grown allocation -- OOB on a parent-sized entity.
bool DTExtend_EntityIsS21Class(const void* pEntity, const char* className);

// True when the entity's SendTable hierarchy contains tableName. ServerClass at entity+0x50.
bool DTExtend_EntityHasSendTable(const void* pEntity, const char* tableName);

// True once DTExtend_Apply has run (SendTable_Init was hit).
bool DTExtend_Applied();

// DT_BaseAnimating.m_itemFlavorGUID sidecar (entity-keyed, 64-bit). Get
// returns false when never set.
bool DTExtend_SetItemFlavorGUID(const void* pEntity, int64_t guid);
bool DTExtend_GetItemFlavorGUID(const void* pEntity, int64_t* outGuid);
bool DTExtend_ClearItemFlavorGUID(const void* pEntity);

// DT_BaseEntity.m_ignoreParentRotation. Sidecar + value proxy.
void DTExtend_SetIgnoreParentRotation(const void* pEntity, bool bIgnore);
bool DTExtend_GetIgnoreParentRotation(const void* pEntity);

// DT_BaseAnimating.m_animRelativeToGroundEnabled. Sidecar + value proxy.
void DTExtend_SetAnimRelativeToGround(const void* pEntity, bool bEnable);
bool DTExtend_GetAnimRelativeToGround(const void* pEntity);

// DT_Player.connectionQualityIndex, 0 (best) to 5 (no netchan). Script
// override wins while set. Getters return false when the entity is not a player.
void ConnQuality_TickServer(float deltaTime);

// DT_GlobalNonRewinding.m_gameTimescale -- dirty the GNR singleton when the
// freeze ConVar (or the SetTimescale store, while freeze is off) moves.
void GameTimescale_TickServer(void);
float GameTimescale_WorldScale(void);
bool ConnQuality_GetForPlayer(const void* pPlayer, int* outIndex);
bool ConnQuality_GetNetStatsForPlayer(const void* pPlayer, float* outLatencyMs, float* outLossPct);
void ConnQuality_SetOverrideForPlayer(const void* pPlayer, int index);
void ConnQuality_ClearOverrideForPlayer(const void* pPlayer);
void ConnQuality_LevelShutdown();

// Per-map reset of live-entity caches and pending swaps. Process-lifetime
// SendTable/factory state is preserved.
void DTExtend_LevelShutdown();

// [ANIM-ANCHOR-PROXY] encode-time anim-anchor hold for non-player entities
// (bridge_anim_anchor.cpp). Install runs pre-flatten and re-arms idempotently
// from the common tail.
void AnimAnchorProxy_Install(void** tables, int count);
void AnimAnchorProxy_LevelShutdown();

// Queue a native CZipline for the post-DispatchSpawn CZiprail ServerClass swap.
bool DTExtend_QueueNativeZiplineAsZiprail(uintptr_t entity, const char* reason);

// True when sdk_ziprail_enable is on.
bool DTExtend_IsZiprailPromoteEnabled();

// Dump one model's sequence table; dedupes by model name.
void DTExtend_DumpSeqTableForStudioHdr(uintptr_t studioHdrPtr, const char* reason);

#ifndef CLIENT_DLL
class ConVar;

// SendTable / SendProp S3 layout. Shared by sibling TUs.
constexpr uint32_t ST_PROPS         = 0x00;
constexpr uint32_t ST_NPROPS        = 0x08;
constexpr uint32_t ST_NETTABLENAME  = 0x4B8;
constexpr uint32_t SP_SIZE          = 0x88;
constexpr uint32_t SP_TYPE          = 0x00;
constexpr uint32_t SP_NBITS         = 0x04;
constexpr uint32_t SP_LOW           = 0x10;
constexpr uint32_t SP_HIGH          = 0x14;
constexpr uint32_t SP_ARRAYPROP     = 0x18;
constexpr uint32_t SP_NELEMENTS     = 0x28;
constexpr uint32_t SP_ELEMSTRIDE    = 0x2C;
constexpr uint32_t SP_VARNAME       = 0x40;
constexpr uint32_t SP_SIZEOFVAR     = 0x48;
constexpr uint32_t SP_PRIORITY      = 0x54;
constexpr uint32_t SP_FLAGS         = 0x58;
constexpr uint32_t SP_OFFSET        = 0x78;
constexpr uint32_t SP_EXCLUDEDTNAME = 0x30;
constexpr uint32_t SP_CHILDTABLE    = 0x70;
constexpr uint32_t SP_PARENTTABLE   = 0x08;

constexpr uint32_t FACT_CLASSNAME = 0x00;
constexpr uint32_t FACT_SENDTABLE = 0x08;
constexpr uint32_t FACT_NEXT      = 0x10;
constexpr uint32_t FACT_CLASSID   = 0x18;
constexpr uint32_t FACT_ALLOCSIZE = 0x1C;
constexpr uint32_t FACT_UNK20     = 0x20;

constexpr int NR_SENDTABLE_SIZE = 0x510;
constexpr int kNumSNDCFamily = 6;
constexpr int kFactoryWalkCap = 4096; // safety cap for factory-list walks
constexpr size_t CLONE_POOL_SIZE = 256 * 1024;
constexpr int kMaxCachedSendTables = 2048;
constexpr int kTriggerHeavyNativeAlloc = 3440;
constexpr int kTriggerHeavyGrownAlloc  = 3584;
constexpr int kTriggerHeavyAppendBase  = 3440;

struct AssignedOffset { const char* tableName; const char* propName; int offset; bool resolved; };
struct SNDCArrayInfo {
	const char* name;
	int nElements;
	int elemSize;
	int offset;
};
struct SNDCTableSpec {
	const char* tableName;
	SNDCArrayInfo arrays[5];
	int requiredAllocSize;
};
struct PEExpandedArrayInfo { const char* name; int nElements; int offset; };
struct CanonS3Tab { const char* name; uintptr_t table; };
struct OffhandFindCtx {
	const char*  targetName;
	uintptr_t    visited[256];
	int          visitedN;
	uint8_t*     parentTable;
	const char*  parentTableName;
	uint8_t*     matchedProp;
	int          depth;
};
struct ClonePool {
	uint8_t* base;
	size_t   used;
	void* alloc(size_t sz, size_t align = 8)
	{
		used = (used + align - 1) & ~(align - 1);
		if (used + sz > CLONE_POOL_SIZE) return nullptr;
		void* p = base + used;
		used += sz;
		return p;
	}
};
struct S21ClassDef {
	const char* className;
	const char* dtName;
	const char* entityName;
	const char* parentFactory;
	const char* parentDT;
	int classSize;
	const int* propNElementsOverride;
	int propNElementsCount;
	bool nestRoot;
	const char* wireParentDT;
};
struct S21ClassSlot {
	uint8_t factory[48];
	uint8_t wrapperTable[0x520];
	ClonePool treePool;
	uintptr_t entFactoryVtable[8];
	uint8_t entFactoryObj[64];
	uintptr_t entFactoryObjPtr;
	uintptr_t parentFactoryRawPtr;
	int actualAllocSize;
};

using DTExtendProxyFn = void(__fastcall*)(void*, void*, void*, void*, int, int);
typedef void** (__fastcall* PFN_GetEntityFactory)();

extern const DTExtendProp s_extendProps[];
extern const int kNumExtendProps;
extern const SNDCTableSpec s_sndcSpecs[];
extern const int kNumSNDCSpecs;
extern AssignedOffset s_assigned[512];
extern int s_assignedCount;
extern bool s_applied;
extern uintptr_t* g_pFactoryListHead;
extern PFN_GetEntityFactory v_GetEntityFactory;
extern void (*v_SendTable_BuildPrecalc)(void* precalc, unsigned char bServerSide);
extern const S21ClassDef s_s21Classes[];
extern const int kNumS21Classes;
extern S21ClassSlot* s_s21Slots;
extern int s_ziprailSlotIndex;
extern int s_ziprailWireMapCount;
extern bool s_triggerHeavyGrown;
extern bool s_sndcFamilyCreateOk[kNumSNDCFamily];
extern const PEExpandedArrayInfo s_peExpandedLayout[5];
extern CanonS3Tab s_canonS3Index[2048];
extern int s_canonS3IndexCount;
extern const uint8_t* s_canonTmpl[16];
extern uintptr_t s_cachedSendTablePtrs[kMaxCachedSendTables];
extern int s_cachedSendTableCount;
extern ConVar bridge_pe_expanded;
extern ConVar bridge_dt_dup_append_suppress;
extern ConVar sdk_deathfield_native_dt;
extern ConVar sdk_nonrewind_misc_dt;

bool DTExtend_IsSafeToRead(const void* addr, size_t size, bool forWrite = false);
int DTExtend_BaseOffset(const char* tableName);
void DTExtend_EnsureTriggerCylinderHeavyGrown(void);
int DTExtend_OverrideSNDCFactoryGetSize(void);
int DTExtend_GrowWorldEntity(void);
void DTExtend_RebuildHighlightSettings(void** tables, int count);
bool DTExtend_ShouldZeroProxyAppendedProp(const char* tableName, const char* propName);
DTExtendProxyFn DTExtend_ValueProxyForAppendedProp(const char* tableName, const char* propName);
DTExtendProxyFn DTExtend_ZeroProxyForProp(const char* propName);
uint8_t* DTExtend_FindTableByName(const char* name);
int DTExtend_FindPropIdx(uint8_t* table, const char* propName);
const uint8_t* DTExtend_FindPropTemplateInTree(const uint8_t* table, int type, int depth = 0);
const uint8_t* DTExtend_FindCleanTemplateProp(int type);
int DTExtend_FindExistingPropIdx(const uint8_t* props, const int nProps, const char* propName);
void DTExtend_FixParentTablesInTree(uint8_t* table, int depth = 0);
void DTExtend_RebuildSendTableCache(void** tables, int count, const char* reason);
void DTExtend_RecursiveOffhandFind(uint8_t* table, OffhandFindCtx& ctx, int depth);
void DTExtend_RetargetNonRewindSendProps(void);
bool DTExtend_RetargetTriggerSlipSphereWrapper(uint8_t* wrapperRoot);
bool DTExtend_BuildMaterialHarvesterWrapper(uint8_t* wrapperRoot, uint8_t* parentWrapper, ClonePool& pool);
bool DTExtend_MaterialHarvesterStateHasBacking(void);
bool DTExtend_AppendSuppressedByRename(const char* tableName, const char* propName);
bool DTExtend_AppendSuppressedByWireLever(const char* tableName, const char* propName);
int PatchEntityCreateAllocImm(uintptr_t createFn, uint32_t targetSize, const char* tag,
	uint32_t expectedNativeSize = 0);
uint8_t* DeepCloneSendTable(const void* srcTable, ClonePool& pool);
void CanonDiscoverTable(uintptr_t table, int depth);
uintptr_t CanonS3TableFind(const char* name);
bool DeathField_NativeDTRequestedAtLaunch(void);
int DeathField_RingCountAtLaunch(void);
int NonRewindMisc_CountAtLaunch(void);
bool NonRewindMisc_DTRequestedAtLaunch(void);
bool ODP_IsReadable(const void* p, size_t n);
int ODP_StrLenSafe(const char* p, int maxLen);
void __fastcall Passives_TailZeroProxy(void* pProp, void* pStruct, void* pData, void* pOut, int iElement, int objectID);
bool ZiprailWire_AddMap(const uint8_t* prop, int blockOff, int kind);
bool ZiprailWire_BuildArray(uint8_t* dst, const uint8_t* arrDonor, const uint8_t* elemDonor,
	const char* name, int blockOff, int elemStride, ClonePool& pool);
bool ZiprailWire_BuildScalar(uint8_t* dst, const uint8_t* donor, const char* name, int blockOff, int kind);
uint8_t* ZiprailWire_FindRootProp(uint8_t* table, const char* name, int wantType);
void __fastcall ZiprailWire_ScalarFloatProxy(void* pProp, void* pStruct, void* pData, void* pOut, int iElement, int objectID);
void __fastcall ZiprailWire_ScalarIntProxy(void* pProp, void* pStruct, void* pData, void* pOut, int iElement, int objectID);
void __fastcall ZiprailWire_ScalarVectorProxy(void* pProp, void* pStruct, void* pData, void* pOut, int iElement, int objectID);
#endif // !CLIENT_DLL

///////////////////////////////////////////////////////////////////////////////
class VDTExtend : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // DT_EXTEND_H
