#if defined(CLIENT_DLL)
//=====================================================================================//
//
// model loading and caching
//
// $NoKeywords: $
//=====================================================================================//

#include "core/stdafx.h"
#include "tier0/threadtools.h"
#include "tier1/cvar.h"
#include "tier1/cmd.h"
#include "tier1/utldict.h"
#include "datacache/mdlcache.h"
#include "datacache/imdlcache.h"
#include "datacache/idatacache.h"
#include "rtech/pak/paktools.h"
#include "rtech/pak/rpak_observe.h"
#include "public/studio.h"

CStudioFallbackHandler g_StudioMdlFallbackHandler;

//-----------------------------------------------------------------------------
// Purpose: finds an MDL
// Input: *this - 
// handle - 
// *a3 - 
// Output: a pointer to the studiohdr_t object
//-----------------------------------------------------------------------------
studiohdr_t* CMDLCache::FindMDL(CMDLCache* const cache, const MDLHandle_t handle, void* a3)
{
    studiodata_t* const studioData = cache->GetStudioData(handle);

    if (!studioData)
    {
        studiohdr_t* const pStudioHdr = GetErrorModel();

        if (!IsKnownBadModel(handle))
        {
            if (!pStudioHdr)
                Error(eDLL_T::ENGINE, EXIT_FAILURE, "Model with handle \"%hu\" not found and \"%s\" couldn't be loaded.\n", handle, ERROR_MODEL);
            else
                Error(eDLL_T::ENGINE, NO_ERROR, "Model with handle \"%hu\" not found; replacing with \"%s\".\n", handle, ERROR_MODEL);
        }

        return pStudioHdr;
    }

    studiomodelcache_t* modelCache = studioData->GetModelCache();

    // Store error and empty fallback models.
    if (IsValidDataCacheHandle(modelCache))
    {
        studiohdr_t* const studioHdr = studioData->GetModelCache()->GetStudioHdr();

        if (studioHdr)
        {
            // Typically, you would only check for '(m_nFlags & STUDIODATA_ERROR_MODEL)',
            // but for some reason this game doesn't have this flag set on that model.
            if (!HasErrorModel() && ((studioData->flags & STUDIODATA_ERROR_MODEL) 
                || V_ComparePath(studioHdr->name, ERROR_MODEL)))
            {
                g_StudioMdlFallbackHandler.SetFallbackModel(studioHdr, handle);
            }
        }
    }

    const int nFlags = STUDIOHDR_FLAGS_NEEDS_DEFERRED_ADDITIVE | STUDIOHDR_FLAGS_OBSOLETE;

    if ((studioData->flags & nFlags))
    {
        if (IsValidDataCacheHandle(modelCache))
        {
            if (a3)
            {
                FindCachedMDL(cache, studioData, a3);
                modelCache = studioData->GetModelCache();
            }

            studiohdr_t* const pStudioHdr = modelCache->GetStudioHdr();

            if (pStudioHdr)
                return pStudioHdr;

            return FindUncachedMDL(cache, handle, studioData, a3);
        }

        studioanimcache_t* const animCache = studioData->GetAnimCache();

        if (IsValidDataCacheHandle(animCache))
        {
            studiohdr_t* const pStudioHdr = animCache->GetStudioHdr();

            if (pStudioHdr)
                return pStudioHdr;
        }
    }
    return FindUncachedMDL(cache, handle, studioData, a3);
}

//-----------------------------------------------------------------------------
// Purpose: finds an MDL cached
// Input: *this - 
// *pStudioData - 
// *a3 - 
//-----------------------------------------------------------------------------
void CMDLCache::FindCachedMDL(CMDLCache* const cache, studiodata_t* const pStudioData, void* a3)
{
    if (a3)
    {
        AUTO_LOCK(pStudioData->mutex);

        *(_QWORD*)((int64_t)a3 + 0x880) = *(_QWORD*)&pStudioData->pad[0x24];
        int64_t v6 = *(_QWORD*)&pStudioData->pad[0x24];
        if (v6)
            *(_QWORD*)(v6 + 0x878) = (int64_t)a3;
        *(_QWORD*)&pStudioData->pad[0x24] = (int64_t)a3;
        *(_QWORD*)((int64_t)a3 + 0x870) = (int64_t)cache;
        *(_WORD*)((int64_t)a3 + 0x888) = pStudioData->modelHandle;
    }
}

//-----------------------------------------------------------------------------
// Purpose: finds an MDL uncached
// Input: *this - 
// handle - 
// *pStudioData - 
// *a4 - 
// Output: a pointer to the studiohdr_t object
//-----------------------------------------------------------------------------
studiohdr_t* CMDLCache::FindUncachedMDL(CMDLCache* const cache, const MDLHandle_t handle, studiodata_t* pStudioData, void* a4)
{
    AUTO_LOCK(pStudioData->mutex);

    const char* modelName = cache->GetModelName(handle);
    const size_t fileNameLen = strlen(modelName);

    studiohdr_t* studioHdr = nullptr;

    if (fileNameLen < 5 ||
        (Q_stricmp(&modelName[fileNameLen - 5], ".rmdl") != 0) &&
        (Q_stricmp(&modelName[fileNameLen - 5], ".rrig") != 0) &&
        (Q_stricmp(&modelName[fileNameLen - 5], ".rpak") != 0))
    {
        studioHdr = GetErrorModel();
        if (!IsKnownBadModel(handle))
        {
            if (!studioHdr)
                Error(eDLL_T::ENGINE, EXIT_FAILURE, "Attempted to load old model \"%s\" and \"%s\" couldn't be loaded.\n", modelName, ERROR_MODEL);
            else
                Error(eDLL_T::ENGINE, NO_ERROR, "Attempted to load old model \"%s\"; replacing with \"%s\".\n", modelName, ERROR_MODEL);
        }

        return studioHdr;
    }

    pStudioData->processing = true;
    Pak_StringToGuid(modelName);
    pStudioData->processing = false;

    studiomodelcache_t* const modelCache = pStudioData->GetModelCache();

    if (IsValidDataCacheHandle(modelCache))
    {
        FindCachedMDL(cache, pStudioData, a4);
        studioHdr = modelCache->GetStudioHdr();
    }
    else
    {
        // Attempt to get studio header from anim cache.
        studioanimcache_t* const animCache = pStudioData->GetAnimCache();

        if (IsValidDataCacheHandle(animCache))
        {
            studioHdr = animCache->GetStudioHdr();
        }
        else
        {
            studioHdr = GetErrorModel();

            if (!IsKnownBadModel(handle))
            {
                if (!studioHdr)
                    Error(eDLL_T::ENGINE, EXIT_FAILURE, "Model \"%s\" not found and \"%s\" couldn't be loaded.\n", modelName, ERROR_MODEL);
                else
                    Error(eDLL_T::ENGINE, NO_ERROR, "Model \"%s\" not found; replacing with \"%s\".\n", modelName, ERROR_MODEL);
            }
        }
    }

    assert(studioHdr);
    return studioHdr;
}

//-----------------------------------------------------------------------------
// Purpose: gets the model cache by handle
// Input: handle - 
// Output: a pointer to the studiomodelcache_t object
//-----------------------------------------------------------------------------
studiomodelcache_t* CMDLCache::GetModelCache(const MDLHandle_t handle)
{
    if (handle == MDLHANDLE_INVALID)
        return nullptr;

    const studiodata_t* const studioData = GetStudioData(handle);

    if (!studioData)
        return nullptr;

    studiomodelcache_t* const modelCache = studioData->GetModelCache();
    return modelCache;
}

//-----------------------------------------------------------------------------
// Purpose: gets the vcollide data from cache pool by handle
// Input: *this - 
// handle - 
// Output: a pointer to the vcollide_t object
//-----------------------------------------------------------------------------
vcollide_t* CMDLCache::GetVCollide(CMDLCache* const cache, const MDLHandle_t handle)
{
    studiomodelcache_t* const modelCache = cache->GetModelCache(handle);

    if (!IsValidDataCacheHandle(modelCache))
    {
        Warning(eDLL_T::ENGINE, "Attempted to load collision data on model \"%s\" with invalid studio data!\n", cache->GetModelName(handle));
        return nullptr;
    }

    studiophysicsref_t* const physicsCache = modelCache->GetPhysicsCache();

    if (!physicsCache)
        return nullptr;

    CStudioVCollide* const pVCollide = physicsCache->GetStudioVCollide();

    if (!pVCollide)
        return nullptr;

    return pVCollide->GetVCollide();
}

//-----------------------------------------------------------------------------
// Purpose: gets the physics geometry data from cache pool by handle
// Input: *this - 
// handle - 
// Output: a pointer to the physics geometry descriptor
//-----------------------------------------------------------------------------
void* CMDLCache::GetPhysicsGeometry(CMDLCache* const cache, const MDLHandle_t handle)
{
    studiomodelcache_t* const modelCache = cache->GetModelCache(handle);

    if (!IsValidDataCacheHandle(modelCache))
    {
        Warning(eDLL_T::ENGINE, "Attempted to load physics geometry on model \"%s\" with invalid studio data!\n", cache->GetModelName(handle));
        return nullptr;
    }

    studiophysicsref_t* const physicsRef = modelCache->GetPhysicsCache();

    if (!physicsRef)
        return nullptr;

    CStudioPhysicsGeoms* const physicsGeoms = physicsRef->GetPhysicsGeoms();

    if (!physicsGeoms)
        return nullptr;

    return physicsGeoms->GetGeometryData();
}

//-----------------------------------------------------------------------------
// Purpose: gets the studio hardware data from cache pool by handle
// Input: *this - 
// handle - 
// Output: a pointer to the studiohwdata_t object
//-----------------------------------------------------------------------------
studiohwdata_t* CMDLCache::GetHardwareData(CMDLCache* const cache, const MDLHandle_t handle)
{
    const studiodata_t* studioData = nullptr; cache->GetStudioData(handle);
    const studiomodelcache_t* modelCache = cache->GetModelCache(handle);

    if (!IsValidDataCacheHandle(modelCache))
    {
        if (!HasErrorModel())
        {
            Error(eDLL_T::ENGINE, NO_ERROR, "Studio hardware for model \"%s\" not found and \"%s\" couldn't be loaded!\n",
                cache->GetModelName(handle), ERROR_MODEL);

            assert(0); // Should never be hit!
            return nullptr;
        }
        else
        {
            // Only spew the message once.
            if (g_StudioMdlFallbackHandler.AddToSuppressionList(handle))
            {
                Warning(eDLL_T::ENGINE, "Studio hardware for model \"%s\" not found; replacing with \"%s\".\n",
                    cache->GetModelName(handle), ERROR_MODEL);
            }
        }

        studioData = cache->GetStudioData(GetErrorModelHandle());
        modelCache = studioData->GetModelCache();
    }
    else
    {
        studioData = cache->GetStudioData(handle);
    }

    studiophysicsref_t* const physicsRef = modelCache->GetPhysicsCache();

    AcquireSRWLockExclusive(g_pMDLLock);
    CMDLCache__CheckData(physicsRef, 1i64); // !!! DECLARED INLINE IN < S3 !!!
    ReleaseSRWLockExclusive(g_pMDLLock);

    if ((studioData->flags & STUDIODATA_FLAGS_STUDIOMESH_LOADED))
        return studioData->GetHardwareDataRef()->GetHardwareData();

    return nullptr;
}

//-----------------------------------------------------------------------------
// Purpose: gets the error model
//-----------------------------------------------------------------------------
studiohdr_t* CMDLCache::GetErrorModel(void)
{
    return g_StudioMdlFallbackHandler.GetFallbackModelHeader();
}
const char* CMDLCache::GetErrorModelName(void)
{
    const studiohdr_t* const errorStudioHdr = g_StudioMdlFallbackHandler.GetFallbackModelHeader();
    assert(errorStudioHdr);

    return errorStudioHdr ? errorStudioHdr->name : "(invalid)";
}
MDLHandle_t CMDLCache::GetErrorModelHandle(void)
{
    return g_StudioMdlFallbackHandler.GetFallbackModelHandle();
}
bool CMDLCache::HasErrorModel(void)
{
    return g_StudioMdlFallbackHandler.HasFallbackModel();
}

//-----------------------------------------------------------------------------
// Purpose: checks if this model handle is within the set of bad models
// Input: handle - 
// Output: true if exist, false otherwise
//-----------------------------------------------------------------------------
bool CMDLCache::IsKnownBadModel(const MDLHandle_t handle)
{
    // Only adds if it didn't exist yet, else it returns false.
    return g_StudioMdlFallbackHandler.AddBadModelHandle(handle);
}

//-----------------------------------------------------------------------------
// S21 missing-model console logger (see VModelMissingLogS21 in mdlcache.h).
//-----------------------------------------------------------------------------
static ConVar mdl_alwaysComplain("mdl_alwaysComplain", "0", FCVAR_RELEASE,
    "Log every model that is missing from the loaded paks and gets replaced with mdl/error.rmdl.");

// Loaded-asset GUID hash (64-byte buckets of 8, quadratic probe). Empty slot
// before a match means the GUID is not loaded == mdl/error.rmdl fallback.
static bool Mdl_GuidResolves(const uintptr_t hashBase, const uint64_t guid)
{
    for (unsigned int probe = 0; probe < 512; ++probe)
    {
        const uint16_t bucketIdx = static_cast<uint16_t>(guid + static_cast<uint64_t>(probe) * probe);
        const uint64_t* const bucket = reinterpret_cast<const uint64_t*>(hashBase + 64ull * bucketIdx);

        bool bucketFull = true;
        for (int s = 0; s < 8; ++s)
        {
            const uint64_t slot = bucket[s];
            if (slot == 0)    { bucketFull = false; break; } // empty -> miss
            if (slot == guid) { return true; }               // found
        }
        if (!bucketFull)
            return false;
    }
    return false;
}

static int64_t __fastcall Hook_CMDLCache_LoadModelAsset(int64_t a1, void* a2, const char* a3)
{
    if (mdl_alwaysComplain.GetBool() && a3 && *a3 &&
        Q_stricmp(a3, "mdl/error.rmdl") != 0)
    {
        const PakGuid_t guid = Pak_StringToGuid(a3);
        const uintptr_t hashBase = S21Pak_AssetGuidHashBase();

        if (!Mdl_GuidResolves(hashBase, guid))
        {
            Error(eDLL_T::ENGINE, NO_ERROR,
                "Model \"%s\" not found; replacing with \"mdl/error.rmdl\".\n", a3);
        }
    }
    return v_CMDLCache_LoadModelAsset(a1, a2, a3);
}

void VModelMissingLogS21::Detour(const bool bAttach) const
{
    if (v_CMDLCache_LoadModelAsset)
        DetourSetup(&v_CMDLCache_LoadModelAsset, &Hook_CMDLCache_LoadModelAsset, bAttach);
}
#else // !CLIENT_DLL
//=====================================================================================//
//
// model loading and caching
//
// $NoKeywords: $
//=====================================================================================//

#include "core/stdafx.h"
#include "tier0/threadtools.h"
#include "tier1/cvar.h"
#include "tier1/utldict.h"
#include "datacache/mdlcache.h"
#include "datacache/imdlcache.h"
#include "datacache/idatacache.h"
#include "rtech/pak/paktools.h"
#include "public/studio.h"
#include <unordered_set>
#include <mutex>

CStudioFallbackHandler g_StudioMdlFallbackHandler;

// [PHY-KEEP] mdl_ load builds CStudioVCollide from header pPhyData (+0x20) then
// zeroes the pointer (one-shot). Lazy prop rebuilds then see null pPhyData and
// get empty vcollide. NOP that store so pPhyData survives rebuilds.

// Kept pPhyData: BRANCH B memmoves geoms before relocate; BRANCH A uses VCollideLoad.
// Read at detour attach. death_box FakePhysicsThrow needs this path.
static ConVar sdk_phy_keep("sdk_phy_keep", "1",
	FCVAR_DEVELOPMENTONLY | FCVAR_REPLICATED,
	"Keep the model header's pPhyData after the load callback builds the "
	"vcollide (NOP the native one-shot zeroing) so lazily-(re)built prop_physics "
	"rebuild a real vcollide+geoms. 0=off (native discards the phy), 1=on (default).");

// Dynamic physics is GEOMS (Physics_CreatePolyObject), not vcollide.solids.
// solids==NULL is healthy for id=1 (Apex geoms) phy.
static void* Hook_PhysModelCreate(int64_t pEntity, uint32_t nModelIndex, void* pOrigin,
	void* pAngles, uint8_t* pSolid)
{
	// Plain passthrough (former sdk_phys_create_diag gate removed).
	return v_PhysModelCreate(pEntity, nModelIndex, pOrigin, pAngles, pSolid);
}

static void** Hook_Studio_RmdlLoadCallback(int64_t* pHdr, int64_t a2, int64_t a3, void* a4)
{
	// Plain passthrough (former sdk_phy_build_diag gate removed).
	return v_Studio_RmdlLoadCallback(pHdr, a2, a3, a4);
}

static int64_t Hook_CStudioPhysicsGeoms_Destroy(int64_t pThis, int64_t a2)
{
	// Plain passthrough (former sdk_phy_build_diag gate removed).
	return v_CStudioPhysicsGeoms_Destroy(pThis, a2);
}

// [GEO-CAP] Lift GetPhysicsGeo 4096-index cap for WLEG-EXT-grown precache
// (idx>=4096) so script-precached studio models still resolve geoms.
static ConVar sdk_modelinfo_geo_cap_fix("sdk_modelinfo_geo_cap_fix", "1",
	FCVAR_DEVELOPMENTONLY | FCVAR_ACCESSIBLE_FROM_THREADS,
	"Lift CModelInfo::GetPhysicsGeo's native 4096 model-index cap (the WLEG-EXT "
	"modelprecache-grow-to-16384 missed this reader, so idx>=4096 studio models always "
	"got geoms=NULL -> 'No physics object'). 0=off (native cap stands).");

// model_t offsets (datacache cannot include engine/gl_model_private.h).
static constexpr ptrdiff_t GEOCAP_MDL_OFF_NAME   = 0x004; // model_t::szPathName
static constexpr ptrdiff_t GEOCAP_MDL_OFF_TYPE   = 0x110; // model_t::type (modtype_t)
static constexpr ptrdiff_t GEOCAP_MDL_OFF_STUDIO = 0x140; // model_t union: MDLHandle_t studio
static constexpr int       GEOCAP_MODTYPE_STUDIO = 3;     // modtype_t::mod_studio

// idx <= 0xFFF: stock. 0x1000..0x3FFF: studio branch via GetModel then GetPhysicsGeometry.
// idx >= 0x4000 refused. Brush side-table is OOB for script-precache indices.
static void* Hook_CModelInfo_GetPhysicsGeo(void* pModelInfo, int modelIndex)
{
	if (!sdk_modelinfo_geo_cap_fix.GetBool() || static_cast<uint32_t>(modelIndex) <= 0xFFF)
		return v_CModelInfo_GetPhysicsGeo(pModelInfo, modelIndex);

	if (static_cast<uint32_t>(modelIndex) >= 0x4000 || !pModelInfo)
		return nullptr;

	void** const vtbl = *reinterpret_cast<void***>(pModelInfo);
	using GetModelFn = uint8_t* (__fastcall*)(void*, int);
	uint8_t* const pModel = reinterpret_cast<GetModelFn>(vtbl[1])(pModelInfo, modelIndex);

	if (!pModel)
		return nullptr;

	const int modelType = *reinterpret_cast<const int*>(pModel + GEOCAP_MDL_OFF_TYPE);
	if (modelType != GEOCAP_MODTYPE_STUDIO)
	{
		// The native's other branch here is a brush side-table read sized by BSP submodel
		// count -- genuinely OOB for a script-precache index. Refuse instead of risking it.
		static int s_geoCapWarnCount = 0;
		if (s_geoCapWarnCount < 10)
		{
			++s_geoCapWarnCount;
			Warning(eDLL_T::SERVER,
				"[GEO-CAP] idx=%d type=%d is not mod_studio at a WLEG-EXT high index -- "
				"refusing geoms (native would read the brush side-table OOB here)\n",
				modelIndex, modelType);
		}
		return nullptr;
	}

	const MDLHandle_t handle = *reinterpret_cast<const MDLHandle_t*>(pModel + GEOCAP_MDL_OFF_STUDIO);
	void* const geoms = CMDLCache::GetPhysicsGeometry(g_pMDLCache, handle);

	static bool s_geoCapAnnounced = false;
	if (!s_geoCapAnnounced)
	{
		s_geoCapAnnounced = true;
		Msg(eDLL_T::SERVER,
			"[GEO-CAP] lifted native 4096 model-index cap: idx=%d -> geoms=%p ('%s')\n",
			modelIndex, geoms, reinterpret_cast<const char*>(pModel + GEOCAP_MDL_OFF_NAME));
	}

	return geoms;
}


//-----------------------------------------------------------------------------
// Purpose: finds an MDL
// Input: *this - 
// handle - 
// *a3 - 
// Output: a pointer to the studiohdr_t object
//-----------------------------------------------------------------------------
studiohdr_t* CMDLCache::FindMDL(CMDLCache* const cache, const MDLHandle_t handle, void* a3)
{
    studiodata_t* const studioData = cache->GetStudioData(handle);

    if (!studioData)
    {
        studiohdr_t* const pStudioHdr = GetErrorModel();

        if (!IsKnownBadModel(handle))
        {
            if (!pStudioHdr)
                Error(eDLL_T::ENGINE, EXIT_FAILURE, "Model with handle \"%hu\" not found and \"%s\" couldn't be loaded.\n", handle, ERROR_MODEL);
            else
                Error(eDLL_T::ENGINE, NO_ERROR, "Model with handle \"%hu\" not found; replacing with \"%s\".\n", handle, ERROR_MODEL);
        }

        return pStudioHdr;
    }

    studiomodelcache_t* modelCache = studioData->GetModelCache();

    // Store error and empty fallback models.
    if (IsValidDataCacheHandle(modelCache))
    {
        studiohdr_t* const studioHdr = studioData->GetModelCache()->GetStudioHdr();

        if (studioHdr)
        {
            // Typically, you would only check for '(m_nFlags & STUDIODATA_ERROR_MODEL)',
            // but for some reason this game doesn't have this flag set on that model.
            if (!HasErrorModel() && ((studioData->flags & STUDIODATA_ERROR_MODEL) 
                || V_ComparePath(studioHdr->name, ERROR_MODEL)))
            {
                g_StudioMdlFallbackHandler.SetFallbackModel(studioHdr, handle);
            }
        }
    }

    const int nFlags = STUDIOHDR_FLAGS_NEEDS_DEFERRED_ADDITIVE | STUDIOHDR_FLAGS_OBSOLETE;

    if ((studioData->flags & nFlags))
    {
        if (IsValidDataCacheHandle(modelCache))
        {
            if (a3)
            {
                FindCachedMDL(cache, studioData, a3);
                modelCache = studioData->GetModelCache();
            }

            studiohdr_t* const pStudioHdr = modelCache->GetStudioHdr();

            if (pStudioHdr)
                return pStudioHdr;

            return FindUncachedMDL(cache, handle, studioData, a3);
        }

        studioanimcache_t* const animCache = studioData->GetAnimCache();

        if (IsValidDataCacheHandle(animCache))
        {
            studiohdr_t* const pStudioHdr = animCache->GetStudioHdr();

            if (pStudioHdr)
                return pStudioHdr;
        }
    }
    return FindUncachedMDL(cache, handle, studioData, a3);
}

//-----------------------------------------------------------------------------
// Purpose: finds an MDL cached
// Input: *this - 
// *pStudioData - 
// *a3 - 
//-----------------------------------------------------------------------------
void CMDLCache::FindCachedMDL(CMDLCache* const cache, studiodata_t* const pStudioData, void* a3)
{
    if (a3)
    {
        AUTO_LOCK(pStudioData->mutex);

        *(_QWORD*)((int64_t)a3 + 0x880) = *(_QWORD*)&pStudioData->pad[0x24];
        int64_t v6 = *(_QWORD*)&pStudioData->pad[0x24];
        if (v6)
            *(_QWORD*)(v6 + 0x878) = (int64_t)a3;
        *(_QWORD*)&pStudioData->pad[0x24] = (int64_t)a3;
        *(_QWORD*)((int64_t)a3 + 0x870) = (int64_t)cache;
        *(_WORD*)((int64_t)a3 + 0x888) = pStudioData->modelHandle;
    }
}

//-----------------------------------------------------------------------------
// Purpose: finds an MDL uncached
// Input: *this - 
// handle - 
// *pStudioData - 
// *a4 - 
// Output: a pointer to the studiohdr_t object
//-----------------------------------------------------------------------------
studiohdr_t* CMDLCache::FindUncachedMDL(CMDLCache* const cache, const MDLHandle_t handle, studiodata_t* pStudioData, void* a4)
{
    AUTO_LOCK(pStudioData->mutex);

    const char* modelName = cache->GetModelName(handle);
    const size_t fileNameLen = strlen(modelName);

    studiohdr_t* studioHdr = nullptr;

    if (fileNameLen < 5 ||
        (Q_stricmp(&modelName[fileNameLen - 5], ".rmdl") != 0) &&
        (Q_stricmp(&modelName[fileNameLen - 5], ".rrig") != 0) &&
        (Q_stricmp(&modelName[fileNameLen - 5], ".rpak") != 0))
    {
        studioHdr = GetErrorModel();

        if (!IsKnownBadModel(handle))
        {
            if (!studioHdr)
                Error(eDLL_T::ENGINE, EXIT_FAILURE, "Attempted to load old model \"%s\" and \"%s\" couldn't be loaded.\n", modelName, ERROR_MODEL);
            else
                Error(eDLL_T::ENGINE, NO_ERROR, "Attempted to load old model \"%s\"; replacing with \"%s\".\n", modelName, ERROR_MODEL);
        }

        return studioHdr;
    }

    pStudioData->processing = true;
    const PakGuid_t guid = Pak_StringToGuid(modelName);
    pStudioData->processing = false;
    (void)guid;

    studiomodelcache_t* const modelCache = pStudioData->GetModelCache();

    if (IsValidDataCacheHandle(modelCache))
    {
        FindCachedMDL(cache, pStudioData, a4);
        studioHdr = modelCache->GetStudioHdr();
    }
    else
    {
        // Attempt to get studio header from anim cache.
        studioanimcache_t* const animCache = pStudioData->GetAnimCache();

        if (IsValidDataCacheHandle(animCache))
        {
            studioHdr = animCache->GetStudioHdr();
        }
        else
        {
            studioHdr = GetErrorModel();

            if (!IsKnownBadModel(handle))
            {
                if (!studioHdr)
                    Error(eDLL_T::ENGINE, EXIT_FAILURE, "Model \"%s\" not found and \"%s\" couldn't be loaded.\n", modelName, ERROR_MODEL);
                else
                    Error(eDLL_T::ENGINE, NO_ERROR, "Model \"%s\" not found; replacing with \"%s\".\n", modelName, ERROR_MODEL);
            }
        }
    }

    assert(studioHdr);
    return studioHdr;
}

//-----------------------------------------------------------------------------
// Purpose: gets the model cache by handle
// Input: handle - 
// Output: a pointer to the studiomodelcache_t object
//-----------------------------------------------------------------------------
studiomodelcache_t* CMDLCache::GetModelCache(const MDLHandle_t handle)
{
    if (handle == MDLHANDLE_INVALID)
        return nullptr;

    const studiodata_t* const studioData = GetStudioData(handle);

    if (!studioData)
        return nullptr;

    studiomodelcache_t* const modelCache = studioData->GetModelCache();
    return modelCache;
}

//-----------------------------------------------------------------------------
// Purpose: gets the vcollide data from cache pool by handle
// Input: *this -
// handle -
// Output: a pointer to the vcollide_t object
//-----------------------------------------------------------------------------
vcollide_t* CMDLCache::GetVCollide(CMDLCache* const cache, const MDLHandle_t handle)
{
    // Native passthrough.
    return CMDLCache__GetVCollide(cache, handle);
}

//-----------------------------------------------------------------------------
// Purpose: gets the physics geometry data from cache pool by handle
// Input: *this - 
// handle - 
// Output: a pointer to the physics geometry descriptor
//-----------------------------------------------------------------------------
void* CMDLCache::GetPhysicsGeometry(CMDLCache* const cache, const MDLHandle_t handle)
{
    // Native passthrough: door/static-prop path, geoms trace BVH.
    return CMDLCache__GetPhysicsGeometry(cache, handle);
}

//-----------------------------------------------------------------------------
// Purpose: gets the studio hardware data from cache pool by handle
// Input: *this - 
// handle - 
// Output: a pointer to the studiohwdata_t object
//-----------------------------------------------------------------------------
studiohwdata_t* CMDLCache::GetHardwareData(CMDLCache* const cache, const MDLHandle_t handle)
{
    const studiodata_t* studioData = nullptr; cache->GetStudioData(handle);
    const studiomodelcache_t* modelCache = cache->GetModelCache(handle);

    if (!IsValidDataCacheHandle(modelCache))
    {
        if (!HasErrorModel())
        {
            Error(eDLL_T::ENGINE, NO_ERROR, "Studio hardware for model \"%s\" not found and \"%s\" couldn't be loaded!\n",
                cache->GetModelName(handle), ERROR_MODEL);

            assert(0); // Should never be hit!
            return nullptr;
        }
        else
        {
            // Only spew the message once.
            if (g_StudioMdlFallbackHandler.AddToSuppressionList(handle))
            {
                Warning(eDLL_T::ENGINE, "Studio hardware for model \"%s\" not found; replacing with \"%s\".\n",
                    cache->GetModelName(handle), ERROR_MODEL);
            }
        }

        studioData = cache->GetStudioData(GetErrorModelHandle());
        modelCache = studioData->GetModelCache();
    }
    else
    {
        studioData = cache->GetStudioData(handle);
    }

    studiophysicsref_t* const physicsRef = modelCache->GetPhysicsCache();

    AcquireSRWLockExclusive(g_pMDLLock);
    CMDLCache__CheckData(physicsRef, 1i64); // !!! DECLARED INLINE IN < S3 !!!
    ReleaseSRWLockExclusive(g_pMDLLock);

    if ((studioData->flags & STUDIODATA_FLAGS_STUDIOMESH_LOADED))
        return studioData->GetHardwareDataRef()->GetHardwareData();

    return nullptr;
}

//-----------------------------------------------------------------------------
// Purpose: gets the error model
//-----------------------------------------------------------------------------
studiohdr_t* CMDLCache::GetErrorModel(void)
{
    return g_StudioMdlFallbackHandler.GetFallbackModelHeader();
}
const char* CMDLCache::GetErrorModelName(void)
{
    const studiohdr_t* const errorStudioHdr = g_StudioMdlFallbackHandler.GetFallbackModelHeader();
    assert(errorStudioHdr);

    return errorStudioHdr ? errorStudioHdr->name : "(invalid)";
}
MDLHandle_t CMDLCache::GetErrorModelHandle(void)
{
    return g_StudioMdlFallbackHandler.GetFallbackModelHandle();
}
bool CMDLCache::HasErrorModel(void)
{
    return g_StudioMdlFallbackHandler.HasFallbackModel();
}

//-----------------------------------------------------------------------------
// Purpose: checks if this model handle is within the set of bad models
// Input: handle - 
// Output: true if exist, false otherwise
//-----------------------------------------------------------------------------
bool CMDLCache::IsKnownBadModel(const MDLHandle_t handle)
{
    // Only adds if it didn't exist yet, else it returns false.
    return g_StudioMdlFallbackHandler.AddBadModelHandle(handle);
}

//-----------------------------------------------------------------------------
// [PHY-KEEP] NOP the mdl_ load callback one-shot pPhyData zero so the pointer
// survives rebuilds. See sdk_phy_keep.
//-----------------------------------------------------------------------------
static uint8_t  s_phyKeepOrig[4] = { 0 };
static uint8_t* s_phyKeepSite    = nullptr;

static void Studio_PatchKeepPhyData(const bool bAttach)
{
    if (bAttach)
    {
        if (!sdk_phy_keep.GetBool())
        {
            Msg(eDLL_T::ENGINE,
                "[PHY-KEEP] disabled (sdk_phy_keep=0) -- native discards pPhyData after "
                "the load build; lazily-spawned prop_physics will lose their vcollide.\n");
            return;
        }

        // The 4-byte store `4C 89 77 20` (mov [rdi+0x20],r14; r14=0) lives 5 bytes into
        // this unique signature (mov rsi,[rsp+40] | <store> | mov rdi,[rsp+48] | add rsp,20).
        CMemory hit = Module_FindPattern(g_GameDll,
            "48 8B 74 24 40 4C 89 77 20 48 8B 7C 24 48 48 83 C4 20");
        if (!hit)
        {
            Warning(eDLL_T::ENGINE,
                "[PHY-KEEP] mdl_ load-callback pattern NOT found -- pPhyData zeroing "
                "left intact; death_box/prop_physics will keep losing their phy.\n");
            return;
        }

        uint8_t* const site = reinterpret_cast<uint8_t*>(hit.GetPtr()) + 5;
        if (!(site[0] == 0x4C && site[1] == 0x89 && site[2] == 0x77 && site[3] == 0x20))
        {
            Warning(eDLL_T::ENGINE,
                "[PHY-KEEP] opcode mismatch @ %p (got %02X %02X %02X %02X, expected "
                "4C 89 77 20) -- NOT patching.\n",
                site, site[0], site[1], site[2], site[3]);
            return;
        }

        memcpy(s_phyKeepOrig, site, 4);
        s_phyKeepSite = site;

        const uint8_t nop4[4] = { 0x90, 0x90, 0x90, 0x90 };
        DWORD oldProt = 0;
        if (!VirtualProtect(site, 4, PAGE_EXECUTE_READWRITE, &oldProt))
        {
            Warning(eDLL_T::ENGINE, "[PHY-KEEP] VirtualProtect failed @ %p (gle=%lu)\n",
                site, GetLastError());
            s_phyKeepSite = nullptr;
            return;
        }
        memcpy(site, nop4, 4);
        VirtualProtect(site, 4, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), site, 4);

        Msg(eDLL_T::ENGINE,
            "[PHY-KEEP] active: NOP'd pPhyData discard `mov [rdi+0x20],r14` @ %p "
            "-- pPhyData survives load build; lazy prop_physics rebuild real vcollide.\n", site);
    }
    else
    {
        if (s_phyKeepSite)
        {
            DWORD oldProt = 0;
            if (VirtualProtect(s_phyKeepSite, 4, PAGE_EXECUTE_READWRITE, &oldProt))
            {
                memcpy(s_phyKeepSite, s_phyKeepOrig, 4);
                VirtualProtect(s_phyKeepSite, 4, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), s_phyKeepSite, 4);
            }
            s_phyKeepSite = nullptr;
        }
    }
}

void VMDLCache::Detour(const bool bAttach) const
{
    Studio_PatchKeepPhyData(bAttach);

    DetourSetup(&CMDLCache__FindMDL, &CMDLCache::FindMDL, bAttach);
    DetourSetup(&CMDLCache__FindCachedMDL, &CMDLCache::FindCachedMDL, bAttach);
    DetourSetup(&CMDLCache__FindUncachedMDL, &CMDLCache::FindUncachedMDL, bAttach);

    DetourSetup(&CMDLCache__GetVCollide, &CMDLCache::GetVCollide, bAttach);
    DetourSetup(&CMDLCache__GetPhysicsGeometry, &CMDLCache::GetPhysicsGeometry, bAttach);

    DetourSetup(&CMDLCache__GetHardwareData, &CMDLCache::GetHardwareData, bAttach);

    // [PHYS-CREATE] dynamic-physics gate logger (see Hook_PhysModelCreate).
    if (v_PhysModelCreate)
        DetourSetup(&v_PhysModelCreate, &Hook_PhysModelCreate, bAttach);
    else
        Warning(eDLL_T::SERVER, "[PHYS-CREATE] PhysModelCreate pattern unresolved -- gate logger inactive\n");

    // [PHY-BUILD] build-side probe on the rmdl load callback + geoms dtor logger.
    if (v_Studio_RmdlLoadCallback)
        DetourSetup(&v_Studio_RmdlLoadCallback, &Hook_Studio_RmdlLoadCallback, bAttach);
    else
        Warning(eDLL_T::SERVER, "[PHY-BUILD] rmdl load-callback pattern unresolved -- build logger inactive\n");
    if (v_CStudioPhysicsGeoms_Destroy)
        DetourSetup(&v_CStudioPhysicsGeoms_Destroy, &Hook_CStudioPhysicsGeoms_Destroy, bAttach);
    else
        Warning(eDLL_T::SERVER, "[PHY-GEOMS-DTOR] geoms dtor pattern unresolved -- dtor logger inactive\n");

    // [GEO-CAP] CModelInfo::GetPhysicsGeo 4096 model-index cap lift (see Hook_CModelInfo_GetPhysicsGeo).
    if (v_CModelInfo_GetPhysicsGeo)
        DetourSetup(&v_CModelInfo_GetPhysicsGeo, &Hook_CModelInfo_GetPhysicsGeo, bAttach);
    else
        Warning(eDLL_T::SERVER, "[GEO-CAP] CModelInfo::GetPhysicsGeo pattern unresolved -- native 4096 model-index cap left in place\n");
}
#endif // CLIENT_DLL
