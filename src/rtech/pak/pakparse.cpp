#if defined(CLIENT_DLL)
//=============================================================================//
//
// Purpose: pak file loading and unloading
//
//=============================================================================//
#include <atomic>
#include <unordered_map>

#include "tier2/zstdutils.h"

#include "rtech/ipakfile.h"
#include "rtech/async/asyncio.h"

#include "paktools.h"
#include "pakstate.h"
#include "pakpatch.h"
#include "pakalloc.h"
#include "pakparse.h"
#include "pakdecode.h"
#include "pakstream.h"
#include "rpak_observe.h"

#include "materialsystem/cmaterialglue.h"

constexpr uint32_t RPAK_ASSET_FOURCC_MATERIAL = 0x6C74616D; // 'matl'
static constexpr const char* const kUnknownMaterialName = "<unknown>";

static const char* Pak_GetMaterialName(const PakAssetShort_s& assetInfo)
{
    if (const CMaterialGlue* const material = reinterpret_cast<const CMaterialGlue*>(assetInfo.head))
    {
        const MaterialGlue_s* const materialDef = material->Get();

        if (materialDef && materialDef->name && materialDef->name[0])
            return materialDef->name;
    }

    return kUnknownMaterialName;
}


static ConVar pak_debugrelations("pak_debugrelations", "0", FCVAR_DEVELOPMENTONLY | FCVAR_ACCESSIBLE_FROM_THREADS, "Debug RPAK asset dependency resolving");
static ConVar pak_debugmaterialdeps("pak_debugmaterialdeps", "0", FCVAR_DEVELOPMENTONLY | FCVAR_ACCESSIBLE_FROM_THREADS,
    "Log which asset dependency resolution requests material assets");

static void Pak_LogMaterialDependency(const PakFile_s* const pak, const PakAsset_s* const dependentAsset,
    const PakAssetBinding_s& dependentBinding, const PakAssetShort_s& dependencyAsset,
    const PakAssetBinding_s& dependencyBinding, const PakGuid_t dependencyGuid)
{
    const char* const dependentType = dependentBinding.description ? dependentBinding.description : "<unknown>";
    const char* const dependencyName = Pak_GetMaterialName(dependencyAsset);
    const char* const pakName = pak ? pak->GetName() : "<unknown>";

    Msg(eDLL_T::RTECH,
        "[pak] %s asset 0x%llX requested material \"%s\" (guid 0x%llX) while loading \"%s\"\n",
        dependentType,
        dependentAsset->guid,
        dependencyName,
        dependencyGuid,
        pakName);
}

static void Pak_TryLogMaterialDependency(const PakFile_s* const pak, const PakAsset_s* const dependentAsset,
    const PakGuid_t dependencyGuid, const int dependencyAssetIndex)
{
    if (!pak_debugmaterialdeps.GetBool())
        return;

    if (!g_pakGlobals)
        return;

    if (dependencyAssetIndex < 0 || dependencyAssetIndex >= PAK_MAX_LOADED_ASSETS)
        return;

    const PakAssetShort_s& dependencyAsset = g_pakGlobals->loadedAssets[dependencyAssetIndex];
    const uint32_t trackerIndex = dependencyAsset.trackerIndex;

    if (trackerIndex >= PAK_MAX_TRACKED_ASSETS)
        return;

    const PakAssetTracker_s& tracker = g_pakGlobals->trackedAssets[trackerIndex];
    const uint8_t dependencyTypeIdx = tracker.assetTypeHashIdx;

    if (dependencyTypeIdx >= PAK_MAX_TRACKED_TYPES)
        return;

    const PakAssetBinding_s& dependencyBinding = g_pakGlobals->assetBindings[dependencyTypeIdx];

    if (dependencyBinding.extension != RPAK_ASSET_FOURCC_MATERIAL)
        return;

    const uint8_t dependentTypeIdx = dependentAsset->HashTableIndexForAssetType();

    if (dependentTypeIdx >= PAK_MAX_TRACKED_TYPES)
        return;

    const PakAssetBinding_s& dependentBinding = g_pakGlobals->assetBindings[dependentTypeIdx];

    Pak_LogMaterialDependency(pak, dependentAsset, dependentBinding, dependencyAsset, dependencyBinding, dependencyGuid);
}

static ConVar pak_debugchannel("pak_debugchannel", "0", FCVAR_DEVELOPMENTONLY | FCVAR_ACCESSIBLE_FROM_THREADS, "Log RPAK files loaded or unloaded with this channel ID", false, 0.f, false, 0.f, "0 = disabled, -1 = all");
static ConVar pak_debugassetunload("pak_debugassetunload", "0", FCVAR_DEVELOPMENTONLY | FCVAR_ACCESSIBLE_FROM_THREADS, "Log individual asset unloads in real time (per-asset callback)");

//-----------------------------------------------------------------------------
// asset unload callback wrapper system
// wraps each asset type's unloadAssetFunc to log when assets are unloaded
//-----------------------------------------------------------------------------
typedef void(*UnloadAssetFunc_t)(void*);
static UnloadAssetFunc_t s_originalUnloadFuncs[PAK_MAX_TRACKED_TYPES] = {};
static bool s_unloadWrappersInstalled[PAK_MAX_TRACKED_TYPES] = {};

// maps asset memPage pointer (trackedAssets.memPage) -> guid, populated at load time
// the unload callback receives memPage (raw slab header), not loadedAssets.head
static std::unordered_map<void*, PakGuid_t> s_assetHeadToGuid;

template<int SLOT>
static void Pak_UnloadAssetWrapper(void* assetHeader)
{
    PakGuid_t guid = 0;

    if (!s_assetHeadToGuid.empty())
    {
        auto it = s_assetHeadToGuid.find(assetHeader);

        if (it != s_assetHeadToGuid.end())
        {
            guid = it->second;
            s_assetHeadToGuid.erase(it);
        }
    }

    if (pak_debugassetunload.GetBool() && g_pakGlobals)
    {
        const PakAssetBinding_s& binding = g_pakGlobals->assetBindings[SLOT];
        FourCCString_t ext;
        FourCCToString(ext, binding.extension);

        const char* desc = binding.description ? binding.description : "<unknown>";

        Msg(eDLL_T::RTECH, "Asset unload: type '%.4s' (%s) guid 0x%llX header %p\n", ext, desc, guid, assetHeader);
    }

    if (s_originalUnloadFuncs[SLOT])
        s_originalUnloadFuncs[SLOT](assetHeader);
}

// generate wrapper function pointers for all 64 slots via recursive template
template<int N>
struct UnloadWrapperTable
{
    static void Init(UnloadAssetFunc_t* table)
    {
        table[N] = &Pak_UnloadAssetWrapper<N>;
        UnloadWrapperTable<N - 1>::Init(table);
    }
};

template<>
struct UnloadWrapperTable<0>
{
    static void Init(UnloadAssetFunc_t* table)
    {
        table[0] = &Pak_UnloadAssetWrapper<0>;
    }
};

static UnloadAssetFunc_t s_unloadWrappers[PAK_MAX_TRACKED_TYPES];
static bool s_unloadWrapperTableInit = false;

static void Pak_InstallUnloadWrappers()
{
    if (!g_pakGlobals)
        return;

    if (!s_unloadWrapperTableInit)
    {
        UnloadWrapperTable<PAK_MAX_TRACKED_TYPES - 1>::Init(s_unloadWrappers);
        s_unloadWrapperTableInit = true;
    }

    for (int i = 0; i < PAK_MAX_TRACKED_TYPES; i++)
    {
        PakAssetBinding_s& binding = g_pakGlobals->assetBindings[i];

        if (binding.unloadAssetFunc
            && binding.unloadAssetFunc != (void*)s_unloadWrappers[i]
            && !s_unloadWrappersInstalled[i])
        {
            s_originalUnloadFuncs[i] = (UnloadAssetFunc_t)binding.unloadAssetFunc;
            binding.unloadAssetFunc = (void*)s_unloadWrappers[i];
            s_unloadWrappersInstalled[i] = true;
        }
    }
}

//-----------------------------------------------------------------------------
// resolve the target guid from lookup table
//-----------------------------------------------------------------------------
static bool Pak_ResolveAssetDependency(const PakFile_s* const pak, PakGuid_t currentGuid,
    const PakGuid_t targetGuid, int& currentIndex, const bool shouldCheckTwo)
{
    while (true)
    {
        if (shouldCheckTwo && currentGuid == 2)
        {
            if (pak->memoryData.pakHeader.assetCount)
                return false;
        }

        currentIndex++;

        if (currentIndex >= PAK_MAX_LOADED_ASSETS)
            return false;

        currentIndex &= PAK_MAX_LOADED_ASSETS_MASK;
        currentGuid = g_pakGlobals->loadedAssets[currentIndex].guid;

        if (currentGuid == targetGuid)
            return true;
    }

    UNREACHABLE();
}

//-----------------------------------------------------------------------------
// resolve guid relations for asset
//-----------------------------------------------------------------------------
struct S21PakPageDesc_s
{
    uint32_t index;
    uint32_t offset;
};

const char* const Pak_AllowedModuleStems[] =
{
    "ui",
    "ui_fs",
};
const size_t Pak_AllowedModuleStemCount = SDK_ARRAYSIZE(Pak_AllowedModuleStems);
static constexpr size_t kS21Asset_GuidOffset = 0x00;
static constexpr size_t kS21Asset_UsesIndexOffset = 0x38;
static constexpr size_t kS21Asset_RelationCountOffset = 0x40;
static constexpr size_t kS21Asset_RelationSkipCountOffset = 0x42;
static constexpr size_t kS21PakGlobal_LoadedAssetHashOffset = 0x000000;
static constexpr size_t kS21PakGlobal_LoadedAssetsOffset = 0x400000;
static constexpr size_t kS21PakGlobal_LoadedPakSlotsOffset = 0x17A2910;
static constexpr size_t kS21LoadedAssetStride = 0x10;
static constexpr size_t kS21LoadedAsset_FlagsOffset = 0x0C;
static constexpr uint32_t kS21LoadedAssetHashSlots = 8;

static const char* Pak_GetS21PakName(const uint32_t pakId)
{
    const uintptr_t slotBase = Pak_GetSlotBase_S21();
    if (!slotBase)
        return "<unknown>";

    const uint32_t slot = pakId & PAK_MAX_LOADED_PAKS_MASK;
    const char** const pName = reinterpret_cast<const char**>(
        slotBase + static_cast<size_t>(slot) * kS21_PakSlotStride + kS21_PakSlot_Name);

    __try
    {
        if (pName && *pName && **pName)
            return *pName;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    return "<unknown>";
}

static bool Pak_FindLoadedAssetHead_S21(const PakGuid_t targetGuid, uint64_t* const outHead)
{
    if (!s_PakGlobalState_S21)
        return false;

    uint8_t* const globalBase = reinterpret_cast<uint8_t*>(s_PakGlobalState_S21);
    uint64_t* const hashBase = reinterpret_cast<uint64_t*>(globalBase + kS21PakGlobal_LoadedAssetHashOffset);
    uint8_t* const loadedAssets = globalBase + kS21PakGlobal_LoadedAssetsOffset;

    for (uint32_t probe = 0; probe <= UINT16_MAX; ++probe)
    {
        const uint16_t bucket = static_cast<uint16_t>(targetGuid + probe * probe);
        uint64_t* const bucketGuids = reinterpret_cast<uint64_t*>(
            reinterpret_cast<uint8_t*>(hashBase) + static_cast<size_t>(bucket) * 64);

        for (uint32_t slot = 0; slot < kS21LoadedAssetHashSlots; ++slot)
        {
            const uint64_t loadedGuid = bucketGuids[slot];
            if (!loadedGuid)
                return false;

            if (loadedGuid != targetGuid)
                continue;

            const uint32_t loadedIndex = (static_cast<uint32_t>(bucket) << 3) | (slot & 7);
            uint8_t* const loadedAsset = loadedAssets + static_cast<size_t>(loadedIndex) * kS21LoadedAssetStride;
            _InterlockedOr(reinterpret_cast<volatile long*>(loadedAsset + kS21LoadedAsset_FlagsOffset), 2);

            *outHead = *reinterpret_cast<uint64_t*>(loadedAsset);
            return true;
        }
    }

    return false;
}

static __int64 Pak_ResolveAssetRelations(PakFile_s* const pak, const PakAsset_s* const asset)
{
    uint8_t* const pakRaw = reinterpret_cast<uint8_t*>(pak);
    const uint8_t* const assetRaw = reinterpret_cast<const uint8_t*>(asset);

    const PakGuid_t assetGuid = *reinterpret_cast<const PakGuid_t*>(assetRaw + kS21Asset_GuidOffset);
    const uint32_t usesIndex = *reinterpret_cast<const uint32_t*>(assetRaw + kS21Asset_UsesIndexOffset);
    const uint16_t relationCount = *reinterpret_cast<const uint16_t*>(assetRaw + kS21Asset_RelationCountOffset);
    const uint16_t relationSkipCount = *reinterpret_cast<const uint16_t*>(assetRaw + kS21Asset_RelationSkipCountOffset);
    const uint32_t relationDescriptorCount = relationCount >= relationSkipCount
        ? static_cast<uint32_t>(relationCount - relationSkipCount)
        : relationCount;
    const uint32_t pakId = *reinterpret_cast<const uint32_t*>(pakRaw + kS21Pak_PakIdOffset);
    const char* const pakName = Pak_GetS21PakName(pakId);
    const uint32_t nUses = Pak_S21GetGuidDescCount(pak);
    const uint32_t nPages = Pak_S21GetPageCount(pak);
    if (usesIndex > nUses || relationCount > nUses - usesIndex)
    {
        static std::atomic<uint64_t> s_badUses{ 0 };
        const uint64_t n = s_badUses.fetch_add(1) + 1;
        if (n <= 20 || (n % 200) == 0)
        {
            Warning(eDLL_T::RTECH,
                "[pak-fixup] usesIndex OOB in '%s': usesIndex=%u count=%u cap=%u (#%llu)\n",
                pakName, usesIndex, relationCount, nUses,
                static_cast<unsigned long long>(n));
        }
        return 0;
    }

    S21PakPageDesc_s* const pageDescBase =
        *reinterpret_cast<S21PakPageDesc_s**>(pakRaw + kS21Pak_PageDescriptorsOffset);
    uint8_t** const memPageBuffers = *reinterpret_cast<uint8_t***>(pakRaw + kS21Pak_MemPageBuffersOffset);
    if (!pageDescBase || !memPageBuffers)
        return 0;
    S21PakPageDesc_s* const pageDescriptors = pageDescBase + usesIndex;

    volatile long* guidDescriptors = nullptr;
    if (s_PakGlobalState_S21)
    {
        guidDescriptors = *reinterpret_cast<volatile long**>(
            s_PakGlobalState_S21 + kS21PakGlobal_LoadedPakSlotsOffset
            + static_cast<size_t>(pakId & PAK_MAX_LOADED_PAKS_MASK) * kS21_PakSlotStride);
    }

    if (pak_debugrelations.GetBool())
        Msg(eDLL_T::RTECH, "Resolving S21 relations for asset: '0x%-16llX', dependencies: %-4u; in pak '%s'\n",
            assetGuid, relationCount, pakName);

    uint64_t lastResolvedHead = 0;
    for (uint32_t i = 0; i < relationCount; ++i)
    {
        const S21PakPageDesc_s& descriptor = pageDescriptors[i];
        if (descriptor.index >= nPages)
            continue;
        uint64_t* const pCurrentGuid = reinterpret_cast<uint64_t*>(
            memPageBuffers[descriptor.index] + descriptor.offset);
        const PakGuid_t targetGuid = *pCurrentGuid;

        if (i < relationDescriptorCount && guidDescriptors)
        {
            const long descriptorIndex = _InterlockedExchangeAdd(guidDescriptors, 1);
            uint64_t* const out = reinterpret_cast<uint64_t*>(
                const_cast<long*>(&guidDescriptors[descriptorIndex * 4 + 2]));
            out[0] = targetGuid;
            out[1] = assetGuid;
        }

        uint64_t resolvedHead = 0;
        if (Pak_FindLoadedAssetHead_S21(targetGuid, &resolvedHead))
        {
            *pCurrentGuid = resolvedHead;
            lastResolvedHead = resolvedHead;
            continue;
        }

        static std::atomic<uint64_t> s_unresolved{ 0 };
        const uint64_t n = s_unresolved.fetch_add(1) + 1;
        if (n <= 50 || (n % 200) == 0)
        {
            Warning(eDLL_T::RTECH,
                "[pak-fixup] unresolved S21 dep %u/%u in '%s': "
                "asset=0x%llX target=0x%llX (#%llu)\n",
                i, relationCount, pakName, assetGuid, targetGuid,
                static_cast<unsigned long long>(n));
        }

        *pCurrentGuid = 0;
    }

    return static_cast<__int64>(lastResolvedHead);
}

static uint32_t Pak_ProcessRemainingPagePointers(PakFile_s* const pak)
{
    uint32_t processedPointers = 0;

    for (processedPointers = pak->numProcessedPointers; processedPointers < pak->GetPointerCount(); ++processedPointers)
    {
        PakPage_u* const curPage = &pak->memoryData.virtualPointers[processedPointers];
        int curCount = curPage->index - pak->firstPageIdx;

        if (curCount < 0)
            curCount += pak->memoryData.pakHeader.memPageCount;

        if (curCount >= pak->processedPageCount)
            break;

        PakPage_u* const ptr = reinterpret_cast<PakPage_u*>(pak->GetPointerForPageOffset(curPage));
        ptr->ptr = pak->memoryData.memPageBuffers[ptr->index] + ptr->offset;
    }

    return processedPointers;
}

static void Pak_RunAssetLoadingJobs(PakFile_s* const pak)
{
    pak->numProcessedPointers = Pak_ProcessRemainingPagePointers(pak);

    const uint32_t numAssets = pak->processedAssetCount;

    if (numAssets == pak->memoryData.pakHeader.assetCount)
        return;

    PakAsset_s* pakAsset = &pak->memoryData.assetEntries[numAssets];

    if (pakAsset->pageEnd > pak->processedPageCount)
        return;

    for (uint32_t currentAsset = numAssets; g_pakGlobals->numAssetLoadJobs <= 0xC8u;)
    {
        pak->memoryData.loadedAssetIndices[currentAsset] = Pak_TrackAsset(pak, pakAsset);

        _InterlockedIncrement16(&g_pakGlobals->numAssetLoadJobs);

        const uint8_t assetBind = pakAsset->HashTableIndexForAssetType();

        if (g_pakGlobals->assetBindings[assetBind].loadAssetFunc)
        {
            const JobTypeID_t jobTypeId = g_pakGlobals->assetBindJobTypes[assetBind];

            // have to cast it to a bigger size to send it as param to JTGuts_AddJob.
            const int64_t pakId = pak->memoryData.pakId;

            JTGuts_AddJob(jobTypeId, pak->memoryData.assetLoadJobGroupId, (void*)pakId, (void*)(uint64_t)currentAsset);
        }
        else
        {
            if (_InterlockedExchangeAdd16(&pakAsset->numRemainingDependencies, -1) == 1)
                Pak_ProcessAssetRelationsAndResolveDependencies(pak, pakAsset, currentAsset, assetBind);

            _InterlockedDecrement16(&g_pakGlobals->numAssetLoadJobs);
        }

        currentAsset = ++pak->processedAssetCount;

        if (currentAsset == pak->memoryData.pakHeader.assetCount)
        {
            JT_EndJobGroup(pak->memoryData.assetLoadJobGroupId);
            return;
        }

        pakAsset = &pak->memoryData.assetEntries[currentAsset];

        if (pakAsset->pageEnd > pak->processedPageCount)
            return;
    }
}

//-----------------------------------------------------------------------------
// load user-requested pak files on-demand
//-----------------------------------------------------------------------------
static PakHandle_t Pak_LoadAsync(const char* const fileName, CAlignedMemAlloc* const allocator, const int logChannel, const bool bUnk)
{
    const int selectedLogChannel = pak_debugchannel.GetInt();

    if (selectedLogChannel && (selectedLogChannel == -1 || logChannel == selectedLogChannel))
        Msg(eDLL_T::RTECH, "pak loading. '%s'\n", fileName);

    const PakHandle_t pakId = v_Pak_LoadAsync(fileName, allocator, logChannel, bUnk);
    return pakId;
}

//-----------------------------------------------------------------------------
// unloads loaded pak files
//-----------------------------------------------------------------------------
static void Pak_UnloadAsync(const PakHandle_t handle)
{
    const PakLoadedInfo_s* const pakInfo = Pak_GetPakInfo(handle);

    if (pakInfo->fileName)
    {
        const int selectedLogChannel = pak_debugchannel.GetInt();

        if (selectedLogChannel && (selectedLogChannel == -1 || pakInfo->logChannel == selectedLogChannel))
            Msg(eDLL_T::RTECH, "pak unloading. '%s'\n", pakInfo->fileName);
    }

    v_Pak_UnloadAsync(handle);
}

// paks get decoded one at a time, even for patches. therefore we can just use
// a single context for all paks and save a bunch of runtime overhead
static ZSTDDecoder_s s_zstdPakDecoder;

#define CMD_INVALID -1

// only patch cmds 4,5,6 use this array to determine their data size
static const int s_patchCmdToBytesToProcess[] = { CMD_INVALID, CMD_INVALID, CMD_INVALID, CMD_INVALID, 3, 7, 6, 0 };
#undef CMD_INVALID
//----------------------------------------------------------------------------------
// loads and processes a pak file (handles decompression and patching)
//----------------------------------------------------------------------------------
static bool Pak_ProcessPakFile(PakFile_s* const pak)
{
    PakFileStream_s* const fileStream = &pak->fileStream;
    PakMemoryData_s* const memoryData = &pak->memoryData;

    // first request is always just the header.
    size_t readStart = sizeof(PakFileHeader_s);

    if (fileStream->numDataChunks > 0)
        readStart = fileStream->numDataChunks * PAK_READ_DATA_CHUNK_SIZE;

    for (; fileStream->numDataChunksProcessed != fileStream->numDataChunks; fileStream->numDataChunksProcessed++)
    {
        const int currentDataChunkIndex = fileStream->numDataChunksProcessed & PAK_MAX_DATA_CHUNKS_PER_STREAM_MASK;
        const uint8_t dataChunkStatus = fileStream->dataChunkStatuses[currentDataChunkIndex];

        if (dataChunkStatus != 1)
        {
            size_t bytesProcessed = 0;
            const char* statusMsg = "(no reason)";

            const uint8_t currentStatus = g_pakLoadApi->CheckAsyncRequest(fileStream->asyncRequestHandles[currentDataChunkIndex], &bytesProcessed, &statusMsg);

            if (currentStatus == AsyncHandleStatus_s::FS_ASYNC_ERROR)
                Error(eDLL_T::RTECH, EXIT_FAILURE, "Error reading pak file \"%s\" -- %s\n", pak->memoryData.fileName, statusMsg);
            else if (currentStatus == AsyncHandleStatus_s::FS_ASYNC_PENDING)
                break;

            fileStream->bytesStreamed += bytesProcessed;
            if (dataChunkStatus)
            {
                const PakFileHeader_s* pakHeader = &pak->memoryData.pakHeader;
                const uint64_t totalDataChunkSizeProcessed = fileStream->numDataChunksProcessed * PAK_READ_DATA_CHUNK_SIZE;

                if (dataChunkStatus == 2)
                {
                    fileStream->bytesStreamed = bytesProcessed + totalDataChunkSizeProcessed;
                    pakHeader = (PakFileHeader_s*)&fileStream->buffer[totalDataChunkSizeProcessed & fileStream->bufferMask];
                }

                const uint8_t fileIndex = fileStream->numLoadedFiles++ & PAK_MAX_ASYNC_STREAMED_LOAD_REQUESTS_MASK;

                fileStream->descriptors[fileIndex].dataOffset = totalDataChunkSizeProcessed + sizeof(PakFileHeader_s);
                fileStream->descriptors[fileIndex].compressedSize = totalDataChunkSizeProcessed + pakHeader->compressedSize;
                fileStream->descriptors[fileIndex].decompressedSize = pakHeader->decompressedSize;
                fileStream->descriptors[fileIndex].compressionMode = pakHeader->GetCompressionMode();
            }
        }
    }

    size_t currentOutBytePos = memoryData->processedPatchedDataSize;

    for (; pak->processedStreamCount != fileStream->numLoadedFiles; pak->processedStreamCount++)
    {
        PakFileStream_s::Descriptor* const streamDesc = &fileStream->descriptors[pak->processedStreamCount & PAK_MAX_ASYNC_STREAMED_LOAD_REQUESTS_MASK];

        if (pak->resetInBytePos)
        {
            pak->resetInBytePos = false;
            pak->inputBytePos = streamDesc->dataOffset;

            if (streamDesc->compressionMode != PakDecodeMode_e::MODE_DISABLED)
            {
                pak->updateBytePosPostProcess = false;
                pak->isCompressed = true;

                memoryData->processedPatchedDataSize = sizeof(PakFileHeader_s);
            }
            else
            {
                pak->updateBytePosPostProcess = true;
                pak->isCompressed = false;

                memoryData->processedPatchedDataSize = streamDesc->dataOffset;
            }

            if (pak->isCompressed)
            {
                if (streamDesc->compressionMode == PakDecodeMode_e::MODE_ZSTD)
                    pak->pakDecoder.zstreamContext = &s_zstdPakDecoder.dctx;

                const size_t decompressedSize = Pak_InitDecoder(&pak->pakDecoder,
                    fileStream->buffer, pak->decompBuffer,
                    PAK_DECODE_IN_RING_BUFFER_MASK, PAK_DECODE_OUT_RING_BUFFER_MASK,
                    streamDesc->compressedSize - (streamDesc->dataOffset - sizeof(PakFileHeader_s)),
                    streamDesc->dataOffset - sizeof(PakFileHeader_s), sizeof(PakFileHeader_s), streamDesc->compressionMode);

                if (decompressedSize != streamDesc->decompressedSize)
                    Error(eDLL_T::RTECH, EXIT_FAILURE,
                        "Error reading pak file \"%s\" with decoder \"%s\" -- decompressed size %zu doesn't match expected value %zu\n",
                        pak->memoryData.fileName,
                        Pak_DecoderToString(streamDesc->compressionMode),
                        decompressedSize,
                        pak->memoryData.pakHeader.decompressedSize);
            }
        }

        if (pak->isCompressed)
        {
            currentOutBytePos = pak->pakDecoder.outBufBytePos;

            if (currentOutBytePos != pak->pakDecoder.decompSize)
            {
                if (streamDesc->compressionMode == PakDecodeMode_e::MODE_ZSTD)
                    pak->pakDecoder.allChunksStreamed = fileStream->numDataChunksProcessed == fileStream->numDataChunks;

                const bool didDecode = Pak_StreamToBufferDecode(&pak->pakDecoder, 
                    fileStream->bytesStreamed, (memoryData->processedPatchedDataSize + PAK_DECODE_OUT_RING_BUFFER_SIZE), streamDesc->compressionMode);

                currentOutBytePos = pak->pakDecoder.outBufBytePos;
                pak->inputBytePos = pak->pakDecoder.inBufBytePos;

                if (didDecode)
                {
                    DevMsg(eDLL_T::RTECH, "%s: pak '%s' decoded successfully\n", __FUNCTION__, pak->GetName());
                    pak->pakDecoder.zstreamContext = nullptr;
                }
            }
        }
        else
        {
            currentOutBytePos = Min(streamDesc->compressedSize, fileStream->bytesStreamed);
        }

        if (pak->inputBytePos != streamDesc->compressedSize || memoryData->processedPatchedDataSize != currentOutBytePos)
            break;

        pak->resetInBytePos = true;
        currentOutBytePos = memoryData->processedPatchedDataSize;
    }

    size_t numBytesToProcess = currentOutBytePos - memoryData->processedPatchedDataSize;

    while (memoryData->patchSrcSize + memoryData->field_2A8)
    {
        // if there are no bytes left to process in this patch operation
        if (!memoryData->numPatchBytesToProcess)
        {
            RBitRead& bitbuf = memoryData->bitBuf;
            bitbuf.ConsumeData(memoryData->patchData, bitbuf.BitsAvailable());

            // advance patch data buffer by the number of bytes that have just been fetched
            memoryData->patchData = &memoryData->patchData[bitbuf.BitsAvailable() >> 3];

            // store the number of bits remaining to complete the data read
            bitbuf.m_bitsAvailable = bitbuf.BitsAvailable() & 7; // number of bits above a whole byte

            const __int8 cmd = memoryData->patchCommands[bitbuf.ReadBits(6)];

            bitbuf.DiscardBits(memoryData->PATCH_field_68[bitbuf.ReadBits(6)]);

            if (cmd < 0 || cmd > 6)
            {
                Warning(eDLL_T::RTECH, "Pak_ProcessPakFile: patch cmd %d out of range in '%s'\n",
                    (int)cmd, pak->GetName());
                break;
            }

            // get the next patch function to execute
            memoryData->patchFunc = g_pakPatchApi[cmd];

            if (cmd <= 3u)
            {
                const uint8_t bitExponent = memoryData->PATCH_unk2[bitbuf.ReadBits(8)]; // number of stored bits for the data size

                bitbuf.DiscardBits(memoryData->PATCH_unk3[bitbuf.ReadBits(8)]);

                memoryData->numPatchBytesToProcess = (1ull << bitExponent) + bitbuf.ReadBits(bitExponent);

                bitbuf.DiscardBits(bitExponent);
            }
            else
            {
                memoryData->numPatchBytesToProcess = s_patchCmdToBytesToProcess[cmd];
            }
        }

        if (!pak->memoryData.patchFunc(pak, &numBytesToProcess))
            break;
    }

    if (pak->updateBytePosPostProcess)
        pak->inputBytePos = memoryData->processedPatchedDataSize;

    if (!fileStream->finishedLoadingPatches)
    {
        const size_t numDataChunksProcessed = Min<size_t>(fileStream->numDataChunksProcessed, pak->inputBytePos >> 19);

        while (fileStream->numDataChunks != numDataChunksProcessed + 32)
        {
            const int8_t requestIdx = fileStream->numDataChunks & PAK_MAX_DATA_CHUNKS_PER_STREAM_MASK;
            const size_t readOffsetEnd = (fileStream->numDataChunks + 1ull) * PAK_READ_DATA_CHUNK_SIZE;

            if (fileStream->fileReadStatus == 1)
            {
                fileStream->asyncRequestHandles[requestIdx] = FS_ASYNC_REQ_INVALID;
                fileStream->dataChunkStatuses[requestIdx] = 1;

                if (((requestIdx + 1) & PAK_MAX_ASYNC_STREAMED_LOAD_REQUESTS_MASK) == 0)
                    fileStream->fileReadStatus = 2;

                ++fileStream->numDataChunks;
                readStart = readOffsetEnd;
            }
            else
            {
                if (readStart < fileStream->fileSize)
                {
                    const size_t lenToRead = Min(fileStream->fileSize, readOffsetEnd);

                    const size_t readOffset = readStart - fileStream->readOffset;
                    const size_t readSize = lenToRead - readStart;

                    fileStream->asyncRequestHandles[requestIdx] = v_FS_ReadAsyncFile(
                        fileStream->fileHandle,
                        readOffset,
                        readSize,
                        &fileStream->buffer[readStart & fileStream->bufferMask],
                        nullptr,
                        nullptr,
                        4);

                    fileStream->dataChunkStatuses[requestIdx] = fileStream->fileReadStatus;
                    fileStream->fileReadStatus = 0;

                    ++fileStream->numDataChunks;
                    readStart = readOffsetEnd;
                }
                else
                {
                    if (pak->patchCount >= pak->memoryData.pakHeader.patchIndex)
                    {
                        FS_CloseAsyncFile(fileStream->fileHandle);
                        fileStream->fileHandle = PAK_INVALID_HANDLE;
                        fileStream->readOffset = 0;
                        fileStream->finishedLoadingPatches = true;

                        return memoryData->patchSrcSize == 0;
                    }

                    if (!pak->dword14)
                        return memoryData->patchSrcSize == 0;

                    char pakPatchPath[MAX_PATH] = {};
                    sprintf(pakPatchPath, "%s%s", Pak_GetReadPath(), pak->memoryData.fileName);

                    // get path of next patch rpak to load
                    if (pak->memoryData.patchIndices[pak->patchCount])
                    {
                        char* pExtension = nullptr;

                        char* it = pakPatchPath;
                        while (*it)
                        {
                            if (*it == '.')
                                pExtension = it;
                            else if (*it == '\\' || *it == '/')
                                pExtension = nullptr;

                            ++it;
                        }

                        if (pExtension)
                            it = pExtension;

                        // replace extension '.rpak' with '(xx).rpak'
                        snprintf(it, &pakPatchPath[sizeof(pakPatchPath)] - it,
                            "(%02u).rpak", pak->memoryData.patchIndices[pak->patchCount]);
                    }

                    const int patchFileHandle = FS_OpenAsyncFile(pakPatchPath, 5, &numBytesToProcess);

                    if (patchFileHandle == FS_ASYNC_FILE_INVALID)
                        Error(eDLL_T::RTECH, EXIT_FAILURE, "Couldn't open file \"%s\".\n", pakPatchPath);

                    if (numBytesToProcess < pak->memoryData.patchHeaders[pak->patchCount].compressedSize)
                        Error(eDLL_T::RTECH, EXIT_FAILURE, "File \"%s\" appears truncated; read size: %zu < expected size: %zu.\n",
                            pakPatchPath, numBytesToProcess, pak->memoryData.patchHeaders[pak->patchCount].compressedSize);

                    FS_CloseAsyncFile(fileStream->fileHandle);

                    fileStream->fileHandle = patchFileHandle;

                    const size_t readOffset = ALIGN_VALUE(fileStream->numDataChunks, 8ull) * PAK_READ_DATA_CHUNK_SIZE;
                    fileStream->fileReadStatus = (fileStream->numDataChunks == ALIGN_VALUE(fileStream->numDataChunks, 8ull)) + 1;

                    fileStream->readOffset = readOffset;
                    fileStream->fileSize = readOffset + pak->memoryData.patchHeaders[pak->patchCount].compressedSize;

                    pak->patchCount++;
                }
            }
        }
    }

    return memoryData->patchSrcSize == 0;
}

// sets patch variables for copying the next unprocessed page into the relevant slab buffer
// if this is a header page, fetch info from the next unprocessed asset and copy over the asset's header
static bool Pak_PrepareNextPageForPatching(PakLoadedInfo_s* const loadedInfo, PakFile_s* const pak)
{
    Pak_RunAssetLoadingJobs(pak);

    // numProcessedPointers has just been set in the above function call
    pak->memoryData.numShiftedPointers = pak->numProcessedPointers;

    if (pak->processedPageCount == pak->GetPageCount())
        return false;

    const uint32_t highestProcessedPageIdx = pak->processedPageCount + pak->firstPageIdx;
    pak->processedPageCount++;

    const int currentPageIndex = highestProcessedPageIdx < pak->GetPageCount()
        ? highestProcessedPageIdx
        : highestProcessedPageIdx - pak->GetPageCount();

    const PakPageHeader_s* const nextMemPageHeader = &pak->memoryData.pageHeaders[currentPageIndex];
    if ((pak->memoryData.slabHeaders[nextMemPageHeader->slabIndex].typeFlags & (SF_CPU | SF_TEMP)) != 0)
    {
        pak->memoryData.patchSrcSize = nextMemPageHeader->dataSize;
        pak->memoryData.patchDstPtr = reinterpret_cast<char*>(pak->memoryData.memPageBuffers[currentPageIndex]);

        return true;
    }

    // headers
    PakAsset_s* const pakAsset = pak->memoryData.ppAssetEntries[pak->memoryData.someAssetCount];

    pak->memoryData.patchSrcSize = pakAsset->headerSize;
    const int assetTypeIdx = pakAsset->HashTableIndexForAssetType();

    pak->memoryData.patchDstPtr = reinterpret_cast<char*>(loadedInfo->slabBuffers[0]) + pak->memoryData.unkAssetTypeBindingSizes[assetTypeIdx];
    pak->memoryData.unkAssetTypeBindingSizes[assetTypeIdx] += g_pakGlobals->assetBindings[assetTypeIdx].structSize;

    return true;
}

static bool Pak_ProcessAssets(PakLoadedInfo_s* const loadedInfo)
{
    PakFile_s* const pak = loadedInfo->pakFile;

    while (pak->processedAssetCount != pak->GetAssetCount())
    {
        // TODO: invert condition and make the branch encompass the whole loop
        if (!(pak->memoryData.patchSrcSize + pak->memoryData.field_2A8))
        {
            if (Pak_PrepareNextPageForPatching(loadedInfo, pak))
                continue;

            break;
        }

        if (!Pak_ProcessPakFile(pak))
            return false;

        // processedPageCount must be greater than 0 here otherwise the page index will be negative and cause a crash
        // if this happens, something probably went wrong with the patch data condition at the start of the loop, as that
        // function call should increment processedPageCount if it succeeded
        assert(pak->processedPageCount > 0);

        const uint32_t pageCount = pak->GetPageCount();
        const uint32_t v4 = (pak->firstPageIdx - 1) + pak->processedPageCount;

        uint32_t shiftedPageIndex = v4;

        if (v4 >= pageCount)
            shiftedPageIndex -= pageCount;

        // if "temp_" slab
        if ((pak->memoryData.slabHeaders[pak->memoryData.pageHeaders[shiftedPageIndex].slabIndex].typeFlags & (SF_CPU | SF_TEMP)) != 0)
        {
            if (Pak_PrepareNextPageForPatching(loadedInfo, pak))
                continue;

            break;
        }

        PakAsset_s* const asset = pak->memoryData.ppAssetEntries[pak->memoryData.someAssetCount];
        const uint32_t headPageOffset = asset->headPtr.offset;
        char* const v8 = pak->memoryData.patchDstPtr - asset->headerSize;

        const uint32_t newOffsetFromSlabBufferToHeader = LODWORD(pak->memoryData.patchDstPtr)
            - asset->headerSize
            - LODWORD(loadedInfo->slabBuffers[0]);
        asset->headPtr.offset = newOffsetFromSlabBufferToHeader;

        const uint32_t offsetSize = newOffsetFromSlabBufferToHeader - headPageOffset;

        for (uint32_t i = pak->memoryData.numShiftedPointers; i < pak->GetPointerCount(); pak->memoryData.numShiftedPointers = i)
        {
            PakPage_u* ptr = &pak->memoryData.virtualPointers[i];

            ASSERT_PAKPTR_VALID(pak, ptr);

            if (ptr->index != shiftedPageIndex)
                break;

            const uint32_t offsetToPointer = ptr->offset - headPageOffset;
            if (offsetToPointer >= asset->headerSize)
                break;

            PakPage_u* const pagePtr = reinterpret_cast<PakPage_u*>(v8 + offsetToPointer);

            ASSERT_PAKPTR_VALID(pak, ptr);

            ptr->offset += offsetSize;

            if (pagePtr->index == shiftedPageIndex)
                pagePtr->offset += offsetSize;

            i = pak->memoryData.numShiftedPointers + 1;
        }

        const uint32_t nUses = pak->GetHeader().usesCount;
        const uint32_t nPages = pak->GetPageCount();
        if (asset->usesIndex <= nUses && asset->usesCount <= nUses - asset->usesIndex)
        {
            for (uint32_t j = 0; j < asset->usesCount; ++j)
            {
                PakPage_u* const descriptor = &pak->memoryData.pageDescriptors[asset->usesIndex + j];

                if (descriptor->index >= nPages)
                    continue;
                if (descriptor->index == shiftedPageIndex)
                    descriptor->offset += offsetSize;
            }
        }

        const uint32_t v16 = ++pak->memoryData.someAssetCount;

        PakAsset_s* v17 = nullptr;
        if (v16 < pak->GetAssetCount() && (v17 = pak->memoryData.ppAssetEntries[v16], v17->headPtr.index == shiftedPageIndex))
        {
            pak->memoryData.field_2A8 = v17->headPtr.offset - headPageOffset - asset->headerSize;
            pak->memoryData.patchSrcSize = v17->headerSize;
            const uint8_t assetTypeIdx = v17->HashTableIndexForAssetType();

            pak->memoryData.patchDstPtr = reinterpret_cast<char*>(loadedInfo->slabBuffers[0]) + pak->memoryData.unkAssetTypeBindingSizes[assetTypeIdx];

            pak->memoryData.unkAssetTypeBindingSizes[assetTypeIdx] += g_pakGlobals->assetBindings[assetTypeIdx].structSize;
        }
        else
        {
            if (Pak_PrepareNextPageForPatching(loadedInfo, pak))
                continue;

            break;
        }
    }

    if (!JT_IsJobDone(pak->memoryData.assetLoadJobGroupId))
        return false;

    uint32_t i = 0;
    PakAsset_s* pAsset = nullptr;

    for (int j = pak->memoryData.pakId & PAK_MAX_LOADED_PAKS_MASK; i < pak->GetHeader().assetCount; loadedInfo->assetGuids[i - 1] = pAsset->guid)
    {
        pAsset = &pak->memoryData.assetEntries[i];
        if (pAsset->numRemainingDependencies)
        {
            //printf("[%s] processing deps for %llX (%.4s)\n", pak->GetName, pAsset->guid, (char*)&pAsset->magic);
            Pak_ResolveAssetRelations(pak, pAsset);

            const int assetIndex = pak->memoryData.loadedAssetIndices[i];
            const PakAssetShort_s& loadedAsset = g_pakGlobals->loadedAssets[assetIndex];

            if (g_pakGlobals->trackedAssets[loadedAsset.trackerIndex].loadedPakIndex == j)
            {
                PakTracker_s* pakTracker = g_pakGlobals->pakTracker;

                if (pakTracker)
                {
                    if (pakTracker->numPaksTracked)
                    {
                        int* trackerIndices = g_pakGlobals->pakTracker->loadedAssetIndices;
                        uint32_t count = 0;

                        while (*trackerIndices != assetIndex)
                        {
                            ++count;
                            ++trackerIndices;

                            if (count >= pakTracker->numPaksTracked)
                                goto LABEL_41;
                        }

                        goto LABEL_42;
                    }
                }
                else
                {
                   pakTracker = reinterpret_cast<PakTracker_s*>(AlignedMemAlloc()->Alloc(sizeof(PakTracker_s), 8));
                   pakTracker->numPaksTracked = 0;
                   pakTracker->unk_4 = 0;
                   pakTracker->unk_8 = 0;

                   g_pakGlobals->pakTracker = pakTracker;
                }
            LABEL_41:

                pakTracker->loadedAssetIndices[pakTracker->numPaksTracked] = assetIndex;
                ++pakTracker->numPaksTracked;
            }
        }
    LABEL_42:
        ++i;
    }

    if (g_pakGlobals->pakTracker)
        Pak_FinalizeLoadedInfo(loadedInfo, 0);

    loadedInfo->status = PakStatus_e::PAK_STATUS_LOADED;

    // install unload wrappers for any newly registered asset types
    Pak_InstallUnloadWrappers();

    // The unload callback receives trackedAssets.memPage (the raw slab header
    // ptr), not loadedAssets.head, so the guid lookup is keyed on memPage.
    for (uint32_t assetIdx = 0; pak_debugassetunload.GetBool() && assetIdx < loadedInfo->assetCount; assetIdx++)
    {
        const int loadedIndex = pak->memoryData.loadedAssetIndices[assetIdx];
        const PakAssetShort_s& loadedAsset = g_pakGlobals->loadedAssets[loadedIndex];
        const uint32_t trackerIdx = loadedAsset.trackerIndex;

        if (trackerIdx >= PAK_MAX_TRACKED_ASSETS)
            continue;

        void* const memPage = g_pakGlobals->trackedAssets[trackerIdx].memPage;
        const PakGuid_t guid = loadedInfo->assetGuids[assetIdx];

        if (memPage && guid)
            s_assetHeadToGuid[memPage] = guid;
    }

    return true;
}

static void Pak_StubInvalidAssetBinds(PakFile_s* const pak, PakSlabDescriptor_s* const desc)
{
    for (uint32_t i = 0; i < pak->GetAssetCount(); ++i)
    {
        PakAsset_s* const asset = &pak->memoryData.assetEntries[i];
        pak->memoryData.ppAssetEntries[i] = asset;

        const uint8_t assetTypeIndex = asset->HashTableIndexForAssetType();
        desc->assetTypeCount[assetTypeIndex]++;

        PakAssetBinding_s* const assetBinding = &g_pakGlobals->assetBindings[assetTypeIndex];

        if (assetBinding->type == PakAssetBinding_s::NONE)
        {
            assetBinding->extension = asset->magic;
            assetBinding->version = asset->version;
            assetBinding->description = "<unknown>";
            assetBinding->loadAssetFunc = nullptr;
            assetBinding->unloadAssetFunc = nullptr;
            assetBinding->replaceAssetFunc = nullptr;
            assetBinding->allocator = AlignedMemAlloc();
            assetBinding->headerSize = asset->headerSize;
            assetBinding->structSize = asset->headerSize;
            assetBinding->headerAlignment = pak->memoryData.pageHeaders[asset->headPtr.index].pageAlignment;
            assetBinding->type = PakAssetBinding_s::STUB;
        }

        // this is dev only because it could spam a lot on older paks
        // which isn't much help to the average user that can't rebuild other people's paks
        if (asset->version != assetBinding->version)
        {
            FourCCString_t assetMagic;
            FourCCToString(assetMagic, asset->magic);

            DevWarning(eDLL_T::RTECH,
                "Unexpected asset version for \"%s\" (%.4s) asset with guid 0x%llX (asset %u in pakfile '%s'). Expected %u, found %u.\n",
                assetBinding->description,
                assetMagic,
                asset->guid,
                i, pak->GetName(),
                assetBinding->version, asset->version
            );
        }
    }
}

static bool Pak_StartLoadingPak(PakLoadedInfo_s* const loadedInfo)
{
    PakFile_s* const pakFile = loadedInfo->pakFile;

    if (pakFile->memoryData.patchSrcSize && !Pak_ProcessPakFile(pakFile))
        return false;

    PakSlabDescriptor_s slabDesc = {};

    Pak_StubInvalidAssetBinds(pakFile, &slabDesc);

    const uint32_t numAssets = pakFile->GetAssetCount();

    if (pakFile->memoryData.pakHeader.patchIndex)
        pakFile->firstPageIdx = pakFile->memoryData.patchDataHeader->pageCount;

    Pak_SortOrInsertAssetEntries(pakFile->memoryData.ppAssetEntries, &pakFile->memoryData.ppAssetEntries[numAssets], numAssets, pakFile);

    // pak must have no more than PAK_MAX_SLABS slabs as otherwise we will overrun the above "slabSizes" array
    // and write to arbitrary locations on the stack
    if (pakFile->GetSlabCount() > PAK_MAX_SLABS)
    {
        Error(eDLL_T::RTECH, EXIT_FAILURE, "Too many slabs in pakfile '%s'. Max %hu, found %hu.\n", pakFile->GetName(), PAK_MAX_SLABS, pakFile->GetSlabCount());
        return false;
    }

    Pak_AlignSlabHeaders(pakFile, &slabDesc);
    Pak_AlignSlabData(pakFile, &slabDesc);

    // allocate slab buffers with predetermined alignments; pages will be
    // copied into here
    for (int8_t i = 0; i < PAK_SLAB_BUFFER_TYPES; ++i)
    {
        if (slabDesc.slabSizeForType[i])
            loadedInfo->slabBuffers[i] = AlignedMemAlloc()->Alloc(slabDesc.slabSizeForType[i], slabDesc.slabAlignmentForType[i]);
    }

    Pak_CopyPagesToSlabs(pakFile, loadedInfo, &slabDesc);
    if (loadedInfo->status == PakStatus_e::PAK_STATUS_ERROR)
        return false;

    const PakFileHeader_s& pakHdr = pakFile->GetHeader();

    if (Pak_StreamingEnabled())
        Pak_LoadStreamingData(loadedInfo);

    const __int64 v106 = pakHdr.pointerCount + 2 * (pakHdr.patchIndex + pakHdr.assetCount + 4ull * pakHdr.assetCount + pakHdr.memSlabCount);
    const __int64 patchDestOffset = pakHdr.GetTotalHeaderSize() + 2 * (pakHdr.patchIndex + 6ull * pakHdr.memPageCount + 4 * v106);

    pakFile->dword14 = 1;

    PakMemoryData_s& memoryData = pakFile->memoryData;

    memoryData.patchSrcSize = pakFile->memoryData.fileSize - patchDestOffset;
    memoryData.patchDstPtr = (char*)&pakHdr + patchDestOffset;

    loadedInfo->status = PakStatus_e::PAK_STATUS_LOAD_PAKHDR;

    return true;
}

//-----------------------------------------------------------------------------
// retrieves the patch index to load for given rpak file, returns NULL if no
// patches are to be loaded for given pak file.
//-----------------------------------------------------------------------------
static uint32 Pak_GetPatchIndexForPak(const char* const pakName)
{
    int totalPatchCount = g_pakGlobals->numPatchedPaks;

    if (!totalPatchCount)
        return 0;

    int iterator = 0;

    while (iterator < totalPatchCount)
    {
        const int index = (totalPatchCount + iterator) >> 1;
        const int compareResult = stricmp(pakName, g_pakGlobals->patchedPakFiles[index]);

        if (compareResult < 0)
            totalPatchCount = index;
        else if (compareResult > 0)
            iterator = index + 1;
        else
            return g_pakGlobals->patchNumbers[index];
    }

    return 0; // Found nothing.
}

static bool Pak_SetupBuffersAndLoad(const PakHandle_t pakId)
{
    PakLoadedInfo_s* const loadedInfo = Pak_GetPakInfo(pakId);
    loadedInfo->status = PAK_STATUS_LOAD_STARTING;

    const char* pakFilePath = loadedInfo->fileName;
    assert(pakFilePath);

    const char* nameUnqualified = V_UnqualifiedFileName(pakFilePath);
    char relativeFilePath[MAX_OSPATH];

    // patches are only supported for paks that reside in the directory returned
    // by the API 'Pak_GetBaseLoadPath' relative from the executable. We only
    // load a single patch master asset and it only tracks core paks.
    if (nameUnqualified == pakFilePath)
    {
        snprintf(relativeFilePath, sizeof(relativeFilePath), "%s%s", Pak_GetReadPath(), pakFilePath);
        // if this pak is patched, load the last patch file first before proceeding
        // with any other pak that is getting patched. note that the patch number
        // does not indicate which pak file is the actual last patch file; a patch
        // with number (01) can also patch (02) and base, these details are
        // determined from the pak file header
        const uint32_t patchIndexToLoad = Pak_GetPatchIndexForPak(pakFilePath);

        if (patchIndexToLoad)
        {
            char* const extension = (char*)V_GetFileExtension(relativeFilePath, true);
            snprintf(extension, &relativeFilePath[sizeof(relativeFilePath)] - extension, "(%02u).rpak", patchIndexToLoad);
        }

        pakFilePath = relativeFilePath;
    }

    // This stored the actual load path of our pak file; if the pak is being
    // loaded from a mod path, then that path will be stored here. This is
    // needed because if a pak file has a module, then we need to load that
    // from the same path as the pak file.
    char actualLoadPath[MAX_OSPATH];
    size_t totalPakFileBufSize;

    const int pakFileHandle = FS_OpenAsyncFile(pakFilePath, loadedInfo->logChannel, &totalPakFileBufSize, actualLoadPath, sizeof(actualLoadPath));

    if (pakFileHandle == FS_ASYNC_FILE_INVALID)
    {
        Error(eDLL_T::RTECH, loadedInfo->logChannel == 5 ? EXIT_FAILURE : 0, "Couldn't read package file \"%s\".\n", pakFilePath);

        loadedInfo->status = PAK_STATUS_ERROR;
        return false;
    }

    loadedInfo->fileHandle = pakFileHandle;
    pakFilePath = actualLoadPath; // Update it to our actual load path.

    // File is truncated or corrupt.
    if (totalPakFileBufSize < sizeof(PakFileHeader_s))
    {
        loadedInfo->status = PAK_STATUS_ERROR;
        return false;
    }

    PakFileHeader_s pakHdr;
    const int asyncFile = v_FS_ReadAsyncFile(pakFileHandle, 0, sizeof(PakFileHeader_s), &pakHdr, 0, 0, 4);

    const char* statusMsg = "(no reason)";
    const uint8_t currentStatus = g_pakLoadApi->WaitAndCheckAsyncRequest(asyncFile, nullptr, &statusMsg);

    if (currentStatus == AsyncHandleStatus_s::Status_e::FS_ASYNC_ERROR)
    {
        Error(eDLL_T::RTECH, EXIT_FAILURE, "Error reading pak file \"%s\" -- %s\n", pakFilePath, statusMsg);

        loadedInfo->status = PAK_STATUS_ERROR;
        return false;
    }

    if (pakHdr.magic != PAK_HEADER_MAGIC ||
        pakHdr.version != PAK_HEADER_VERSION)
    {
        loadedInfo->status = PAK_STATUS_ERROR;
        return false;
    }

    char libraryFilePath[MAX_OSPATH];

    if (pakHdr.flags & PAK_HEADER_FLAGS_HAS_MODULE_EXTENDED)
    {
        const char* const extStart = V_GetFileExtension(pakFilePath, true);

        // Loaded a pak file without an extension.
        if (extStart == pakFilePath)
        {
            loadedInfo->status = PAK_STATUS_ERROR;
            return false;
        }

        const size_t unqualifiedFileNameLen = (extStart - pakFilePath);
        const size_t numCopyBytesLeft = sizeof(libraryFilePath) - unqualifiedFileNameLen;

        const static char dllExt[] = ".dll";

        // String buffer couldn't contain new extension. This happens when the
        //.dll extension is larger than the file's original extension, and the
        // original file path was using everything but the last 4 or less bytes
        // of the buffer. This should never happen on this platform. All paks
        // always have the.rpak extension as well so this really is to prevent
        // stack smashes in exceptional cases.
        if (numCopyBytesLeft < sizeof(dllExt))
        {
            loadedInfo->status = PAK_STATUS_ERROR;
            return false;
        }

        memcpy(libraryFilePath, pakFilePath, unqualifiedFileNameLen);
        memcpy(&libraryFilePath[unqualifiedFileNameLen], dllExt, sizeof(dllExt));

        char moduleStem[MAX_OSPATH];
        if (unqualifiedFileNameLen >= sizeof(moduleStem))
        {
            loadedInfo->status = PAK_STATUS_ERROR;
            return false;
        }
        memcpy(moduleStem, pakFilePath, unqualifiedFileNameLen);
        moduleStem[unqualifiedFileNameLen] = '\0';

        bool bModuleAllowed = false;

        for (size_t i = 0; i < Pak_AllowedModuleStemCount; ++i)
        {
            if (_stricmp(moduleStem, Pak_AllowedModuleStems[i]) == 0)
            {
                bModuleAllowed = true;
                break;
            }
        }

        if (!bModuleAllowed)
        {
            Warning(eDLL_T::RTECH, "Pak_SetupBuffersAndLoad: refusing module load for \"%s\" (module not allowlisted)\n",
                pakFilePath);
        }
        else
        {
            // The engine passes a bare pak name, so a plain LoadLibraryA is
            // resolved against CWD/PATH. That is a hijack surface, and it only
            // ever succeeded for ui.dll because that module is already resident
            // by the time any pak asks for it. Resolve beside the pak instead.
            char szExeDir[MAX_OSPATH];
            char szModulePath[MAX_OSPATH];

            GetModuleFileNameA(NULL, szExeDir, sizeof(szExeDir));

            char* const pLastSlash = strrchr(szExeDir, '\\');

            if (pLastSlash)
                *pLastSlash = '\0';
            else
                szExeDir[0] = '\0';

            V_snprintf(szModulePath, sizeof(szModulePath), "%s\\paks\\Win64\\%s",
                szExeDir, libraryFilePath);

            const HMODULE hModule = LoadLibraryA(szModulePath);
            loadedInfo->hModule = hModule;

            if (!hModule)
            {
                Warning(eDLL_T::RTECH,
                    "Pak_SetupBuffersAndLoad: LoadLibrary failed for \"%s\" (err=%lu)\n",
                    szModulePath, GetLastError());

                loadedInfo->status = PAK_STATUS_ERROR;
                return false;
            }

            Msg(eDLL_T::RTECH, "Pak_SetupBuffersAndLoad: module loaded \"%s\"\n",
                szModulePath);
        }
    }

    loadedInfo->fileTime = pakHdr.fileTime;

    uint32_t assetCount = pakHdr.assetCount;
    const uint16_t patchIndex = pakHdr.patchIndex;
    const uint16_t memPageCount = pakHdr.memPageCount;
    const uint16_t memSlabCount = pakHdr.memSlabCount;
    const __int64 v32 = *(unsigned int*)&pakHdr.unk2[4];

    if (assetCount > PAK_MAX_LOADED_ASSETS)
    {
        Warning(eDLL_T::RTECH, "[PAK-PARSE] assetCount %u exceeds %u\n",
            assetCount, PAK_MAX_LOADED_ASSETS);
        loadedInfo->status = PakStatus_e::PAK_STATUS_ERROR;
        return false;
    }
    loadedInfo->assetCount = assetCount;

    loadedInfo->assetGuids = (PakGuid_t*)loadedInfo->allocator->Alloc(sizeof(PakGuid_t) * assetCount, 8);
    if (assetCount && !loadedInfo->assetGuids)
    {
        Warning(eDLL_T::RTECH, "[PAK-PARSE] assetGuids alloc failed (count %u)\n", assetCount);
        loadedInfo->status = PakStatus_e::PAK_STATUS_ERROR;
        return false;
    }

    const size_t streamingFilesBufSize = pakHdr.streamingFilesBufSize[STREAMING_SET_OPTIONAL] + pakHdr.streamingFilesBufSize[STREAMING_SET_MANDATORY];
    const size_t memPagePointersBufSize = 8 * memPageCount;

    totalPakFileBufSize = streamingFilesBufSize
        + (patchIndex != 0 ? 8 : 0)
        + 2
        * (patchIndex
            + 2
            * (pakHdr.dependentsCount
                + *(unsigned int*)pakHdr.unk2
                + 3 * memPageCount
                + 2 * (pakHdr.pointerCount + pakHdr.usesCount + 16i64 + 2 * (assetCount + patchIndex + 4 * assetCount + memSlabCount))))
        + v32;

    const __int64 v80 = 4 * assetCount;
    const uint64_t v90 = totalPakFileBufSize + 2080;
    const __int64 v33 = -((__int64)totalPakFileBufSize + 2080 + 4 * (__int64)assetCount) & 7;
    const __int64 v89 = v33;
    const __int64 v34 = 4 * assetCount + totalPakFileBufSize + 2080 + v33 + 8 * memPageCount + 12 * assetCount;
    const __int64 v35 = (-(4 * (__int64)assetCount + (__int64)totalPakFileBufSize + 2080 + (__int64)v33 + 8 * (__int64)memPageCount + 12 * (__int64)assetCount) & 7) + 4088i64;

    uint64_t ringBufferStreamSize;
    uint64_t ringBufferOutSize;

    const bool isCompressed = (pakHdr.flags & (PAK_HEADER_FLAGS_RTECH_ENCODED | PAK_HEADER_FLAGS_ZSTD_ENCODED)) != 0;

    if (isCompressed)
    {
        ringBufferStreamSize = PAK_DECODE_IN_RING_BUFFER_SIZE;
        ringBufferOutSize = PAK_DECODE_OUT_RING_BUFFER_SIZE;

        if (pakHdr.compressedSize < PAK_DECODE_IN_RING_BUFFER_SIZE && !patchIndex)
            ringBufferStreamSize = (pakHdr.compressedSize + PAK_DECODE_IN_RING_BUFFER_SMALL_MASK) & 0xFFFFFFFFFFFFF000ui64;
    }
    else
    {
        ringBufferStreamSize = 0;
        ringBufferOutSize = PAK_DECODE_IN_RING_BUFFER_SIZE;
    }

    if (ringBufferOutSize > pakHdr.decompressedSize && !patchIndex)
        ringBufferOutSize = (pakHdr.decompressedSize + PAK_DECODE_IN_RING_BUFFER_SMALL_MASK) & 0xFFFFFFFFFFFFF000ui64;

    PakFile_s* const pak = (PakFile_s*)AlignedMemAlloc()->Alloc(v34 + v35 + ringBufferOutSize + ringBufferStreamSize, 8);

    if (!pak)
    {
        loadedInfo->status = PAK_STATUS_ERROR;
        return false;
    }

    loadedInfo->pakFile = pak;

    pak->processedPageCount = 0;
    pak->firstPageIdx = 0;
    pak->patchCount = 0;
    pak->dword14 = 0;
    pak->numProcessedPointers = 0;
    pak->processedAssetCount = 0;
    pak->memoryData.someAssetCount = 0;
    pak->memoryData.numShiftedPointers = 0;

    PakGuidDescriptor_s* const guidBuf = (PakGuidDescriptor_s*)loadedInfo->allocator->Alloc(sizeof(PakGuidDescriptor_s) * pakHdr.usesCount + 8, 8);
    loadedInfo->guidDestriptors = guidBuf;

    guidBuf->unk1 = 0;
    guidBuf->unk2 = 0;

    pak->memoryData.pakHeader = pakHdr;

    pak->memoryData.pakId = pakId;
    pak->memoryData.fileSize = totalPakFileBufSize;

    pak->memoryData.assetLoadJobGroupId = pakHdr.assetCount
        ? JT_BeginJobGroup(0)
        : JT_JOB_GROUP_BASE_ID;

    uint8_t** v44 = (uint8_t**)((char*)pak + v89 + v90 + v80);
    pak->memoryData.memPageBuffers = v44;
    PakAsset_s** v45 = (PakAsset_s**)((char*)v44 + memPagePointersBufSize);
    pak->memoryData.ppAssetEntries = v45;
    pak->memoryData.loadedAssetIndices = (int*)&v45[pakHdr.assetCount];

    PakPatchFileHeader_s* p_patchDataHeader = &pak->memoryData.patchHeader;
    PakPatchFileHeader_s* p_patchFileHeader = (PakPatchFileHeader_s*)&pak->memoryData.patchHeader.decompressedSize;

    const uint16_t patchIndexCached = pak->memoryData.pakHeader.patchIndex;

    if (!patchIndexCached)
    {
        p_patchDataHeader = nullptr;
        p_patchFileHeader = &pak->memoryData.patchHeader;
    }

    pak->memoryData.patchDataHeader = (PakPatchDataHeader_s*)p_patchDataHeader;
    pak->memoryData.patchHeaders = p_patchFileHeader;
    uint16_t* v51 = (unsigned __int16*)&p_patchFileHeader[patchIndexCached];
    pak->memoryData.patchIndices = v51;
    char* v52 = (char*)&v51[patchIndexCached];
    char* v53 = &v52[pak->memoryData.pakHeader.streamingFilesBufSize[STREAMING_SET_MANDATORY]];
    pak->memoryData.streamingFilePaths[STREAMING_SET_MANDATORY] = v52;
    __int64 v54 = pak->memoryData.pakHeader.streamingFilesBufSize[STREAMING_SET_OPTIONAL];
    pak->memoryData.streamingFilePaths[STREAMING_SET_OPTIONAL] = v53;
    PakSlabHeader_s* v55 = (PakSlabHeader_s*)&v53[v54];
    __int64 v56 = pak->memoryData.pakHeader.memSlabCount;
    pak->memoryData.slabHeaders = v55;
    __int64 v57 = pak->memoryData.pakHeader.memPageCount;
    PakPageHeader_s* const v58 = (PakPageHeader_s*)&v55[v56];
    pak->memoryData.pageHeaders = v58;
    const uint32_t pointerCount = pak->memoryData.pakHeader.pointerCount;
    PakPage_u* const v60 = (PakPage_u*)&v58[v57];
    pak->memoryData.virtualPointers = v60;
    assetCount = pak->memoryData.pakHeader.assetCount;
    PakAsset_s* const v62 = (PakAsset_s*)&v60[pointerCount];
    pak->memoryData.assetEntries = v62;
    const uint32_t usesCount = pak->memoryData.pakHeader.usesCount;
    PakPage_u* pageDescriptors = (PakPage_u*)&v62[assetCount];
    pak->memoryData.pageDescriptors = pageDescriptors;
    const uint32_t dependentsCount = pak->memoryData.pakHeader.dependentsCount;
    uint32_t* const v66 = (uint32_t*)&pageDescriptors[usesCount];
    pak->memoryData.fileRelations = v66;
    const __int64 v67 = *(unsigned int*)pak->memoryData.pakHeader.unk2;
    uint32_t* const v68 = &v66[dependentsCount];
    pak->memoryData.ptr5E0 = v68;
    const __int64 v69 = *(unsigned int*)&pak->memoryData.pakHeader.unk2[4];
    pak->memoryData.ptr5F0 = nullptr;
    uint32_t* v70 = &v68[v67];
    pak->memoryData.ptr5E8 = v70;
    const __int64 v71 = (__int64)v70 + v69;
    pak->memoryData.ptr5F8 = (char*)v70 + v69;

    if (patchIndexCached)
    {
        PakPatchDataHeader_s* const patchDataHeader = pak->memoryData.patchDataHeader;
        if (patchDataHeader->editStreamSize)
        {
            *(_QWORD*)&pak->memoryData.ptr5F0 = v71;
            *(_QWORD*)&pak->memoryData.ptr5F8 = v71 + (unsigned int)patchDataHeader->editStreamSize;
        }
    }

    pak->memoryData.fileName = loadedInfo->fileName;
    pak->fileStream.readOffset = 0;
    pak->fileStream.fileSize = pakHdr.compressedSize;
    pak->fileStream.fileHandle = loadedInfo->fileHandle;
    loadedInfo->fileHandle = FS_ASYNC_FILE_INVALID;

    pak->fileStream.bufferMask = PAK_DECODE_IN_RING_BUFFER_MASK;

    uint8_t* const ringBuffer = (uint8_t*)(((unsigned __int64)pak + v35 + v34) & 0xFFFFFFFFFFFFF000ui64);

    pak->fileStream.buffer = ringBuffer;
    pak->fileStream.numDataChunksProcessed = 0;
    pak->fileStream.numDataChunks = 0;
    pak->fileStream.fileReadStatus = AsyncHandleStatus_s::Status_e::FS_ASYNC_CANCELLED;
    pak->fileStream.finishedLoadingPatches = 0;
    pak->fileStream.numLoadedFiles = 0;
    pak->fileStream.bytesStreamed = sizeof(PakFileHeader_s);
    pak->decompBuffer = &ringBuffer[ringBufferStreamSize];
    pak->inputBytePos = sizeof(PakFileHeader_s);
    pak->processedStreamCount = 0;
    pak->resetInBytePos = true;
    pak->updateBytePosPostProcess = false;
    pak->isCompressed = false;

    pak->headerSize = sizeof(PakFileHeader_s);

    pak->maxCopySize = isCompressed
        ? PAK_DECODE_OUT_RING_BUFFER_MASK
        : PAK_DECODE_IN_RING_BUFFER_MASK;

    memset(&pak->pakDecoder, 0, sizeof(pak->pakDecoder));

    pak->pakDecoder.outBufBytePos = sizeof(PakFileHeader_s);
    pak->pakDecoder.decompSize = sizeof(PakFileHeader_s);
    pak->memoryData.processedPatchedDataSize = sizeof(PakFileHeader_s);
    const uint64_t v77 = pakHdr.pointerCount + 2 * (pakHdr.patchIndex + (unsigned __int64)pakHdr.assetCount + 4i64 * pakHdr.assetCount + pakHdr.memSlabCount);

    pak->memoryData.field_2A8 = 0ull;
    pak->memoryData.patchData = nullptr;
    pak->memoryData.patchDataPtr = nullptr;
    pak->memoryData.bitBuf.m_dataBuf = 0;
    pak->memoryData.bitBuf.m_bitsAvailable = 0;
    pak->memoryData.patchDataOffset = 0;
    pak->memoryData.patchSrcSize = streamingFilesBufSize + (pakHdr.patchIndex != 0 ? 8 : 0) + 2 * (pakHdr.patchIndex + 6 * pakHdr.memPageCount + 4 * v77);
    pak->memoryData.patchDstPtr = (char*)&pak->memoryData.patchHeader;
    pak->memoryData.patchFunc = g_pakPatchApi[0];

    const bool isBasePatch = pakHdr.patchIndex == 0;
    pak->memoryData.numPatchBytesToProcess = pak->memoryData.pakHeader.decompressedSize + isBasePatch - sizeof(PakFileHeader_s);

    Pak_ProcessPakFile(pak);
    return true;
}

#else // !CLIENT_DLL
//=============================================================================//
//
// Purpose: pak file loading and unloading
//
//=============================================================================//
#include <unordered_map>
#include <atomic>
#include <vector>

#include "tier2/zstdutils.h"

#include "rtech/ipakfile.h"
#include "rtech/async/asyncio.h"

#include "paktools.h"
#include "pakstate.h"
#include "pakpatch.h"
#include "pakalloc.h"
#include "pakparse.h"
#include "pakdecode.h"
#include "pakstream.h"

#include "materialsystem/cmaterialglue.h"

constexpr uint32_t RPAK_ASSET_FOURCC_MATERIAL = 0x6C74616D; // 'matl'
static constexpr const char* const kUnknownMaterialName = "<unknown>";


static const char* Pak_GetMaterialName(const PakAssetShort_s& assetInfo)
{
    if (const CMaterialGlue* const material = reinterpret_cast<const CMaterialGlue*>(assetInfo.head))
    {
        const MaterialGlue_s* const materialDef = material->Get();

        if (materialDef && materialDef->name && materialDef->name[0])
            return materialDef->name;
    }

    return kUnknownMaterialName;
}


static ConVar pak_debugrelations("pak_debugrelations", "0", FCVAR_DEVELOPMENTONLY | FCVAR_ACCESSIBLE_FROM_THREADS, "Debug RPAK asset dependency resolving");
static ConVar pak_debugmaterialdeps("pak_debugmaterialdeps", "0", FCVAR_DEVELOPMENTONLY | FCVAR_ACCESSIBLE_FROM_THREADS,
    "Log which asset dependency resolution requests material assets");

static void Pak_LogMaterialDependency(const PakFile_s* const pak, const PakAsset_s* const dependentAsset,
    const PakAssetBinding_s& dependentBinding, const PakAssetShort_s& dependencyAsset,
    const PakAssetBinding_s& dependencyBinding, const PakGuid_t dependencyGuid)
{
    const char* const dependentType = dependentBinding.description ? dependentBinding.description : "<unknown>";
    const char* const dependencyName = Pak_GetMaterialName(dependencyAsset);
    const char* const pakName = pak ? pak->GetName() : "<unknown>";

    Msg(eDLL_T::RTECH,
        "[pak] %s asset 0x%llX requested material \"%s\" (guid 0x%llX) while loading \"%s\"\n",
        dependentType,
        dependentAsset->guid,
        dependencyName,
        dependencyGuid,
        pakName);
}

static void Pak_TryLogMaterialDependency(const PakFile_s* const pak, const PakAsset_s* const dependentAsset,
    const PakGuid_t dependencyGuid, const int dependencyAssetIndex)
{
    if (!pak_debugmaterialdeps.GetBool())
        return;

    if (!g_pakGlobals)
        return;

    if (dependencyAssetIndex < 0 || dependencyAssetIndex >= PAK_MAX_LOADED_ASSETS)
        return;

    const PakAssetShort_s& dependencyAsset = g_pakGlobals->loadedAssets[dependencyAssetIndex];
    const uint32_t trackerIndex = dependencyAsset.trackerIndex;

    if (trackerIndex >= PAK_MAX_TRACKED_ASSETS)
        return;

    const PakAssetTracker_s& tracker = g_pakGlobals->trackedAssets[trackerIndex];
    const uint8_t dependencyTypeIdx = tracker.assetTypeHashIdx;

    if (dependencyTypeIdx >= PAK_MAX_TRACKED_TYPES)
        return;

    const PakAssetBinding_s& dependencyBinding = g_pakGlobals->assetBindings[dependencyTypeIdx];

    if (dependencyBinding.extension != RPAK_ASSET_FOURCC_MATERIAL)
        return;

    const uint8_t dependentTypeIdx = dependentAsset->HashTableIndexForAssetType();

    if (dependentTypeIdx >= PAK_MAX_TRACKED_TYPES)
        return;

    const PakAssetBinding_s& dependentBinding = g_pakGlobals->assetBindings[dependentTypeIdx];

    Pak_LogMaterialDependency(pak, dependentAsset, dependentBinding, dependencyAsset, dependencyBinding, dependencyGuid);
}

static ConVar pak_debugchannel("pak_debugchannel", "0", FCVAR_DEVELOPMENTONLY | FCVAR_ACCESSIBLE_FROM_THREADS, "Log RPAK files loaded or unloaded with this channel ID", false, 0.f, false, 0.f, "0 = disabled, -1 = all");
static ConVar pak_debugassetunload("pak_debugassetunload", "0", FCVAR_DEVELOPMENTONLY | FCVAR_ACCESSIBLE_FROM_THREADS, "Log individual asset unloads in real time (per-asset callback)");

static void* Pak_GetLoadedAssetPointer(PakFile_s* const pak, const PakPage_u& ptr, const bool allowRelocatedHeaderOffset)
{
    if (!pak)
        return nullptr;

    if (IS_PAKPTR_VALID(pak, &ptr))
        return pak->memoryData.memPageBuffers[ptr.index] + ptr.offset;

    // Header assets are relocated into the single runtime HEAD slab during
    // Pak_PrepareNextPageForPatching. After that point headPtr.offset is no
    // longer page-local, so the normal pak pointer bounds check can fail even
    // though memPageBuffers[index] + offset is the materialized header.
    if (allowRelocatedHeaderOffset && ptr.index < pak->GetPageCount())
        return pak->memoryData.memPageBuffers[ptr.index] + ptr.offset;

    // If the low 32-bit index still names a pak page, this is an invalid
    // page-offset pair, not a raw pointer. Reinterpreting it as a pointer can
    // turn { page, relocatedOffset } into a bogus address like 0x000fb90800000000.
    if (ptr.index < pak->GetPageCount())
        return nullptr;

    if (!ptr.ptr || ptr.ptr == reinterpret_cast<void*>(UINTPTR_MAX))
        return nullptr;

    return ptr.ptr;
}

//-----------------------------------------------------------------------------
// asset unload callback wrapper system
// wraps each asset type's unloadAssetFunc to log when assets are unloaded
//-----------------------------------------------------------------------------
typedef void(*UnloadAssetFunc_t)(void*);
static UnloadAssetFunc_t s_originalUnloadFuncs[PAK_MAX_TRACKED_TYPES] = {};
static bool s_unloadWrappersInstalled[PAK_MAX_TRACKED_TYPES] = {};

// maps asset memPage pointer (trackedAssets.memPage) -> guid, populated at load time
// the unload callback receives memPage (raw slab header), not loadedAssets.head
static std::unordered_map<void*, PakGuid_t> s_assetHeadToGuid;

template<int SLOT>
static void Pak_UnloadAssetWrapper(void* assetHeader)
{
    PakGuid_t guid = 0;

    if (!s_assetHeadToGuid.empty())
    {
        auto it = s_assetHeadToGuid.find(assetHeader);

        if (it != s_assetHeadToGuid.end())
        {
            guid = it->second;
            s_assetHeadToGuid.erase(it);
        }
    }

    if (pak_debugassetunload.GetBool() && g_pakGlobals)
    {
        const PakAssetBinding_s& binding = g_pakGlobals->assetBindings[SLOT];
        FourCCString_t ext;
        FourCCToString(ext, binding.extension);

        const char* desc = binding.description ? binding.description : "<unknown>";

        Msg(eDLL_T::RTECH, "Asset unload: type '%.4s' (%s) guid 0x%llX header %p\n", ext, desc, guid, assetHeader);
    }

    if (s_originalUnloadFuncs[SLOT])
        s_originalUnloadFuncs[SLOT](assetHeader);
}

// generate wrapper function pointers for all 64 slots via recursive template
template<int N>
struct UnloadWrapperTable
{
    static void Init(UnloadAssetFunc_t* table)
    {
        table[N] = &Pak_UnloadAssetWrapper<N>;
        UnloadWrapperTable<N - 1>::Init(table);
    }
};

template<>
struct UnloadWrapperTable<0>
{
    static void Init(UnloadAssetFunc_t* table)
    {
        table[0] = &Pak_UnloadAssetWrapper<0>;
    }
};

static UnloadAssetFunc_t s_unloadWrappers[PAK_MAX_TRACKED_TYPES];
static bool s_unloadWrapperTableInit = false;

static void Pak_InstallUnloadWrappers()
{
    if (!g_pakGlobals)
        return;

    if (!s_unloadWrapperTableInit)
    {
        UnloadWrapperTable<PAK_MAX_TRACKED_TYPES - 1>::Init(s_unloadWrappers);
        s_unloadWrapperTableInit = true;
    }

    for (int i = 0; i < PAK_MAX_TRACKED_TYPES; i++)
    {
        PakAssetBinding_s& binding = g_pakGlobals->assetBindings[i];

        if (binding.unloadAssetFunc
            && binding.unloadAssetFunc != (void*)s_unloadWrappers[i]
            && !s_unloadWrappersInstalled[i])
        {
            s_originalUnloadFuncs[i] = (UnloadAssetFunc_t)binding.unloadAssetFunc;
            binding.unloadAssetFunc = (void*)s_unloadWrappers[i];
            s_unloadWrappersInstalled[i] = true;
        }
    }
}

//-----------------------------------------------------------------------------
// resolve the target guid from lookup table
//-----------------------------------------------------------------------------
static bool Pak_ResolveAssetDependency(const PakFile_s* const pak, PakGuid_t currentGuid,
    const PakGuid_t targetGuid, int& currentIndex, const bool shouldCheckTwo)
{
    while (true)
    {
        if (shouldCheckTwo && currentGuid == 2)
        {
            if (pak->memoryData.pakHeader.assetCount)
                return false;
        }

        currentIndex++;

        if (currentIndex >= PAK_MAX_LOADED_ASSETS)
            return false;

        currentIndex &= PAK_MAX_LOADED_ASSETS_MASK;
        currentGuid = g_pakGlobals->loadedAssets[currentIndex].guid;

        if (currentGuid == targetGuid)
            return true;
    }

    UNREACHABLE();
}

//-----------------------------------------------------------------------------
// resolve guid relations for asset
//-----------------------------------------------------------------------------
static void Pak_ResolveAssetRelations(PakFile_s* const pak, const PakAsset_s* const asset)
{
    const uint32_t nUses = pak->GetHeader().usesCount;
    const uint32_t nPages = pak->GetPageCount();
    if (!pak->memoryData.pageDescriptors || !pak->memoryData.memPageBuffers
        || asset->usesIndex > nUses || asset->usesCount > nUses - asset->usesIndex)
    {
        static std::atomic<uint64_t> s_badUses{ 0 };
        const uint64_t n = s_badUses.fetch_add(1) + 1;
        if (n <= 20 || (n % 200) == 0)
        {
            Warning(eDLL_T::RTECH,
                "[pak-fixup] usesIndex OOB in '%s': usesIndex=%u count=%u cap=%u (#%llu)\n",
                pak->memoryData.fileName, asset->usesIndex, asset->usesCount, nUses,
                static_cast<unsigned long long>(n));
        }
        return;
    }

    PakPage_u* const pageDescriptors = &pak->memoryData.pageDescriptors[asset->usesIndex];
    uint32_t* const guidDestriptors = (uint32_t*)g_pakGlobals->loadedPaks[pak->memoryData.pakId & PAK_MAX_LOADED_PAKS_MASK].guidDestriptors;

    if (pak_debugrelations.GetBool())
        Msg(eDLL_T::RTECH, "Resolving relations for asset: '0x%-16llX', dependencies: %-4u; in pak '%s'\n",
            asset->guid, asset->usesCount, pak->memoryData.fileName);

    for (uint32_t i = 0; i < asset->usesCount; i++)
    {
        if (pageDescriptors[i].index >= nPages)
            continue;
        void** const pCurrentGuid = reinterpret_cast<void**>(pak->memoryData.memPageBuffers[pageDescriptors[i].index] + pageDescriptors[i].offset);

        // get current guid
        const PakGuid_t targetGuid = reinterpret_cast<uint64_t>(*pCurrentGuid);

        // get asset index
        int currentIndex = targetGuid & PAK_MAX_LOADED_ASSETS_MASK;
        const PakGuid_t currentGuid = g_pakGlobals->loadedAssets[currentIndex].guid;

        const int64_t v9 = 2i64 * InterlockedExchangeAdd(guidDestriptors, 1u);
        *reinterpret_cast<PakGuid_t*>(const_cast<uint32_t*>(&guidDestriptors[2 * v9 + 2])) = targetGuid;
        *reinterpret_cast<PakGuid_t*>(const_cast<uint32_t*>(&guidDestriptors[2 * v9 + 4])) = asset->guid;

        if (currentGuid != targetGuid)
        {
            // are we some special asset with the guid 2?
            if (!Pak_ResolveAssetDependency(pak, currentGuid, targetGuid, currentIndex, true))
            {
                PakAsset_s* assetEntries = pak->memoryData.assetEntries;
                uint64_t a = 0;

                for (; assetEntries->guid != targetGuid; a++, assetEntries++)
                {
                    if (a >= pak->memoryData.pakHeader.assetCount)
                    {
                        if (!Pak_ResolveAssetDependency(pak, currentGuid, targetGuid, currentIndex, false))
                        {
                            // Unresolved settings-rPak model ref: write NULL.
                            static std::atomic<uint64_t> s_unresolved{0};
                            const uint64_t n = s_unresolved.fetch_add(1) + 1;
                            if (n <= 20 || (n % 200) == 0)
                            {
                                Warning(eDLL_T::RTECH,
                                    "[pak-fixup] unresolved dep %u/%u in '%s': "
                                    "asset=0x%llX target=0x%llX (#%llu)\n",
                                    i, asset->usesCount,
                                    pak->memoryData.fileName,
                                    asset->guid, targetGuid,
                                    (unsigned long long)n);
                            }
                            *pCurrentGuid = nullptr;
                            continue;
                        }

                        break;
                    }
                }

                currentIndex = pak->memoryData.loadedAssetIndices[a];
            }
        }

        // finally write the pointer to the guid entry
        Pak_TryLogMaterialDependency(pak, asset, targetGuid, currentIndex);

        // Log a NULL resolved head.
        if (!g_pakGlobals->loadedAssets[currentIndex].head)
        {
            static std::atomic<uint64_t> s_nullHead{0};
            const uint64_t n = s_nullHead.fetch_add(1) + 1;
            if (n <= 30 || (n % 500) == 0)
            {
                Warning(eDLL_T::RTECH,
                    "[pak-fixup] NULL head for dep %u/%u in '%s': "
                    "asset=0x%llX target=0x%llX idx=%d (#%llu)\n",
                    i, asset->usesCount,
                    pak->memoryData.fileName,
                    asset->guid, targetGuid,
                    currentIndex, (unsigned long long)n);
            }
        }

        *pCurrentGuid = g_pakGlobals->loadedAssets[currentIndex].head;
    }
}

static uint32_t Pak_ProcessRemainingPagePointers(PakFile_s* const pak)
{
    uint32_t processedPointers = 0;

    for (processedPointers = pak->numProcessedPointers; processedPointers < pak->GetPointerCount(); ++processedPointers)
    {
        PakPage_u* const curPage = &pak->memoryData.virtualPointers[processedPointers];
        int curCount = curPage->index - pak->firstPageIdx;

        if (curCount < 0)
            curCount += pak->memoryData.pakHeader.memPageCount;

        if (curCount >= pak->processedPageCount)
            break;

        PakPage_u* const ptr = reinterpret_cast<PakPage_u*>(pak->GetPointerForPageOffset(curPage));
        uint8_t* const tgtBuf = pak->memoryData.memPageBuffers[ptr->index];
        ptr->ptr = tgtBuf + ptr->offset;
    }

    return processedPointers;
}

static void Pak_RunAssetLoadingJobs(PakFile_s* const pak)
{
    pak->numProcessedPointers = Pak_ProcessRemainingPagePointers(pak);

    const uint32_t numAssets = pak->processedAssetCount;

    if (numAssets == pak->memoryData.pakHeader.assetCount)
        return;

    PakAsset_s* pakAsset = &pak->memoryData.assetEntries[numAssets];

    if (pakAsset->pageEnd > pak->processedPageCount)
        return;

    for (uint32_t currentAsset = numAssets; g_pakGlobals->numAssetLoadJobs <= 0xC8u;)
    {
        pak->memoryData.loadedAssetIndices[currentAsset] = Pak_TrackAsset(pak, pakAsset);

        _InterlockedIncrement16(&g_pakGlobals->numAssetLoadJobs);

        const uint8_t assetBind = pakAsset->HashTableIndexForAssetType();

        if (g_pakGlobals->assetBindings[assetBind].loadAssetFunc)
        {
            const JobTypeID_t jobTypeId = g_pakGlobals->assetBindJobTypes[assetBind];

            // have to cast it to a bigger size to send it as param to JTGuts_AddJob.
            const int64_t pakId = pak->memoryData.pakId;

            JTGuts_AddJob(jobTypeId, pak->memoryData.assetLoadJobGroupId, (void*)pakId, (void*)(uint64_t)currentAsset);
        }
        else
        {
            if (_InterlockedExchangeAdd16(&pakAsset->numRemainingDependencies, -1) == 1)
                Pak_ProcessAssetRelationsAndResolveDependencies(pak, pakAsset, currentAsset, assetBind);

            _InterlockedDecrement16(&g_pakGlobals->numAssetLoadJobs);
        }

        currentAsset = ++pak->processedAssetCount;

        if (currentAsset == pak->memoryData.pakHeader.assetCount)
        {
            JT_EndJobGroup(pak->memoryData.assetLoadJobGroupId);
            return;
        }

        pakAsset = &pak->memoryData.assetEntries[currentAsset];

        if (pakAsset->pageEnd > pak->processedPageCount)
            return;
    }
}

//-----------------------------------------------------------------------------
// load user-requested pak files on-demand
//-----------------------------------------------------------------------------
static PakHandle_t Pak_LoadAsync(const char* const fileName, CAlignedMemAlloc* const allocator, const int logChannel, const bool bUnk)
{
    const int selectedLogChannel = pak_debugchannel.GetInt();

    if (selectedLogChannel && (selectedLogChannel == -1 || logChannel == selectedLogChannel))
        Msg(eDLL_T::RTECH, "Loading pak file: '%s'\n", fileName);

    const PakHandle_t pakId = v_Pak_LoadAsync(fileName, allocator, logChannel, bUnk);
    return pakId;
}

//-----------------------------------------------------------------------------
// unloads loaded pak files
//-----------------------------------------------------------------------------
static void Pak_UnloadAsync(const PakHandle_t handle)
{
    const PakLoadedInfo_s* const pakInfo = Pak_GetPakInfo(handle);

    if (pakInfo->fileName)
    {
        const int selectedLogChannel = pak_debugchannel.GetInt();

        if (selectedLogChannel && (selectedLogChannel == -1 || pakInfo->logChannel == selectedLogChannel))
            Msg(eDLL_T::RTECH, "Unloading pak file: '%s'\n", pakInfo->fileName);
    }

    v_Pak_UnloadAsync(handle);
}

// paks get decoded one at a time, even for patches. therefore we can just use
// a single context for all paks and save a bunch of runtime overhead
static ZSTDDecoder_s s_zstdPakDecoder;

#define CMD_INVALID -1

// only patch cmds 4,5,6 use this array to determine their data size
static const int s_patchCmdToBytesToProcess[] = { CMD_INVALID, CMD_INVALID, CMD_INVALID, CMD_INVALID, 3, 7, 6, 0 };
#undef CMD_INVALID
//----------------------------------------------------------------------------------
// loads and processes a pak file (handles decompression and patching)
//----------------------------------------------------------------------------------
static bool Pak_ProcessPakFile(PakFile_s* const pak)
{
    PakFileStream_s* const fileStream = &pak->fileStream;
    PakMemoryData_s* const memoryData = &pak->memoryData;

    // first request is always just the header.
    size_t readStart = sizeof(PakFileHeader_s);

    if (fileStream->numDataChunks > 0)
        readStart = fileStream->numDataChunks * PAK_READ_DATA_CHUNK_SIZE;

    for (; fileStream->numDataChunksProcessed != fileStream->numDataChunks; fileStream->numDataChunksProcessed++)
    {
        const int currentDataChunkIndex = fileStream->numDataChunksProcessed & PAK_MAX_DATA_CHUNKS_PER_STREAM_MASK;
        const uint8_t dataChunkStatus = fileStream->dataChunkStatuses[currentDataChunkIndex];

        if (dataChunkStatus != 1)
        {
            size_t bytesProcessed = 0;
            const char* statusMsg = "(no reason)";

            const uint8_t currentStatus = g_pakLoadApi->CheckAsyncRequest(fileStream->asyncRequestHandles[currentDataChunkIndex], &bytesProcessed, &statusMsg);

            if (currentStatus == AsyncHandleStatus_s::FS_ASYNC_ERROR)
                Error(eDLL_T::RTECH, EXIT_FAILURE, "Error reading pak file \"%s\" -- %s\n", pak->memoryData.fileName, statusMsg);
            else if (currentStatus == AsyncHandleStatus_s::FS_ASYNC_PENDING)
                break;

            fileStream->bytesStreamed += bytesProcessed;
            if (dataChunkStatus)
            {
                const PakFileHeader_s* pakHeader = &pak->memoryData.pakHeader;
                const uint64_t totalDataChunkSizeProcessed = fileStream->numDataChunksProcessed * PAK_READ_DATA_CHUNK_SIZE;

                if (dataChunkStatus == 2)
                {
                    fileStream->bytesStreamed = bytesProcessed + totalDataChunkSizeProcessed;
                    pakHeader = (PakFileHeader_s*)&fileStream->buffer[totalDataChunkSizeProcessed & fileStream->bufferMask];
                }

                const uint8_t fileIndex = fileStream->numLoadedFiles++ & PAK_MAX_ASYNC_STREAMED_LOAD_REQUESTS_MASK;

                fileStream->descriptors[fileIndex].dataOffset = totalDataChunkSizeProcessed + sizeof(PakFileHeader_s);
                fileStream->descriptors[fileIndex].compressedSize = totalDataChunkSizeProcessed + pakHeader->compressedSize;
                fileStream->descriptors[fileIndex].decompressedSize = pakHeader->decompressedSize;
                fileStream->descriptors[fileIndex].compressionMode = pakHeader->GetCompressionMode();
            }
        }
    }

    size_t currentOutBytePos = memoryData->processedPatchedDataSize;

    for (; pak->processedStreamCount != fileStream->numLoadedFiles; pak->processedStreamCount++)
    {
        PakFileStream_s::Descriptor* const streamDesc = &fileStream->descriptors[pak->processedStreamCount & PAK_MAX_ASYNC_STREAMED_LOAD_REQUESTS_MASK];

        if (pak->resetInBytePos)
        {
            pak->resetInBytePos = false;
            pak->inputBytePos = streamDesc->dataOffset;

            if (streamDesc->compressionMode != PakDecodeMode_e::MODE_DISABLED)
            {
                pak->updateBytePosPostProcess = false;
                pak->isCompressed = true;

                memoryData->processedPatchedDataSize = sizeof(PakFileHeader_s);
            }
            else
            {
                pak->updateBytePosPostProcess = true;
                pak->isCompressed = false;

                memoryData->processedPatchedDataSize = streamDesc->dataOffset;
            }

            if (pak->isCompressed)
            {
                if (streamDesc->compressionMode == PakDecodeMode_e::MODE_ZSTD)
                    pak->pakDecoder.zstreamContext = &s_zstdPakDecoder.dctx;

                const size_t decompressedSize = Pak_InitDecoder(&pak->pakDecoder,
                    fileStream->buffer, pak->decompBuffer,
                    PAK_DECODE_IN_RING_BUFFER_MASK, PAK_DECODE_OUT_RING_BUFFER_MASK,
                    streamDesc->compressedSize - (streamDesc->dataOffset - sizeof(PakFileHeader_s)),
                    streamDesc->dataOffset - sizeof(PakFileHeader_s), sizeof(PakFileHeader_s), streamDesc->compressionMode);

                if (decompressedSize != streamDesc->decompressedSize)
                    Error(eDLL_T::RTECH, EXIT_FAILURE,
                        "Error reading pak file \"%s\" with decoder \"%s\" -- decompressed size %zu doesn't match expected value %zu\n",
                        pak->memoryData.fileName,
                        Pak_DecoderToString(streamDesc->compressionMode),
                        decompressedSize,
                        pak->memoryData.pakHeader.decompressedSize);
            }
        }

        if (pak->isCompressed)
        {
            currentOutBytePos = pak->pakDecoder.outBufBytePos;

            if (currentOutBytePos != pak->pakDecoder.decompSize)
            {
                if (streamDesc->compressionMode == PakDecodeMode_e::MODE_ZSTD)
                    pak->pakDecoder.allChunksStreamed = fileStream->numDataChunksProcessed == fileStream->numDataChunks;

                const bool didDecode = Pak_StreamToBufferDecode(&pak->pakDecoder, 
                    fileStream->bytesStreamed, (memoryData->processedPatchedDataSize + PAK_DECODE_OUT_RING_BUFFER_SIZE), streamDesc->compressionMode);

                currentOutBytePos = pak->pakDecoder.outBufBytePos;
                pak->inputBytePos = pak->pakDecoder.inBufBytePos;

                if (didDecode)
                {
                    DevMsg(eDLL_T::RTECH, "%s: pak '%s' decoded successfully\n", __FUNCTION__, pak->GetName());
                    pak->pakDecoder.zstreamContext = nullptr;
                }
            }
        }
        else
        {
            currentOutBytePos = Min(streamDesc->compressedSize, fileStream->bytesStreamed);
        }

        if (pak->inputBytePos != streamDesc->compressedSize || memoryData->processedPatchedDataSize != currentOutBytePos)
            break;

        pak->resetInBytePos = true;
        currentOutBytePos = memoryData->processedPatchedDataSize;
    }

    size_t numBytesToProcess = currentOutBytePos - memoryData->processedPatchedDataSize;

    while (memoryData->patchSrcSize + memoryData->field_2A8)
    {
        // if there are no bytes left to process in this patch operation
        if (!memoryData->numPatchBytesToProcess)
        {
            RBitRead& bitbuf = memoryData->bitBuf;
            bitbuf.ConsumeData(memoryData->patchData, bitbuf.BitsAvailable());

            // advance patch data buffer by the number of bytes that have just been fetched
            memoryData->patchData = &memoryData->patchData[bitbuf.BitsAvailable() >> 3];

            // store the number of bits remaining to complete the data read
            bitbuf.m_bitsAvailable = bitbuf.BitsAvailable() & 7; // number of bits above a whole byte

            const __int8 cmd = memoryData->patchCommands[bitbuf.ReadBits(6)];

            bitbuf.DiscardBits(memoryData->PATCH_field_68[bitbuf.ReadBits(6)]);

            if (cmd < 0 || cmd > 6)
            {
                Warning(eDLL_T::RTECH, "Pak_ProcessPakFile: patch cmd %d out of range in '%s'\n",
                    (int)cmd, pak->GetName());
                break;
            }

            // get the next patch function to execute
            memoryData->patchFunc = g_pakPatchApi[cmd];

            if (cmd <= 3u)
            {
                const uint8_t bitExponent = memoryData->PATCH_unk2[bitbuf.ReadBits(8)]; // number of stored bits for the data size

                bitbuf.DiscardBits(memoryData->PATCH_unk3[bitbuf.ReadBits(8)]);

                memoryData->numPatchBytesToProcess = (1ull << bitExponent) + bitbuf.ReadBits(bitExponent);

                bitbuf.DiscardBits(bitExponent);
            }
            else
            {
                memoryData->numPatchBytesToProcess = s_patchCmdToBytesToProcess[cmd];
            }
        }

        if (!pak->memoryData.patchFunc(pak, &numBytesToProcess))
            break;
    }

    if (pak->updateBytePosPostProcess)
        pak->inputBytePos = memoryData->processedPatchedDataSize;

    if (!fileStream->finishedLoadingPatches)
    {
        const size_t numDataChunksProcessed = Min<size_t>(fileStream->numDataChunksProcessed, pak->inputBytePos >> 19);

        while (fileStream->numDataChunks != numDataChunksProcessed + 32)
        {
            const int8_t requestIdx = fileStream->numDataChunks & PAK_MAX_DATA_CHUNKS_PER_STREAM_MASK;
            const size_t readOffsetEnd = (fileStream->numDataChunks + 1ull) * PAK_READ_DATA_CHUNK_SIZE;

            if (fileStream->fileReadStatus == 1)
            {
                fileStream->asyncRequestHandles[requestIdx] = FS_ASYNC_REQ_INVALID;
                fileStream->dataChunkStatuses[requestIdx] = 1;

                if (((requestIdx + 1) & PAK_MAX_ASYNC_STREAMED_LOAD_REQUESTS_MASK) == 0)
                    fileStream->fileReadStatus = 2;

                ++fileStream->numDataChunks;
                readStart = readOffsetEnd;
            }
            else
            {
                if (readStart < fileStream->fileSize)
                {
                    const size_t lenToRead = Min(fileStream->fileSize, readOffsetEnd);

                    const size_t readOffset = readStart - fileStream->readOffset;
                    const size_t readSize = lenToRead - readStart;

                    fileStream->asyncRequestHandles[requestIdx] = v_FS_ReadAsyncFile(
                        fileStream->fileHandle,
                        readOffset,
                        readSize,
                        &fileStream->buffer[readStart & fileStream->bufferMask],
                        nullptr,
                        nullptr,
                        4);

                    fileStream->dataChunkStatuses[requestIdx] = fileStream->fileReadStatus;
                    fileStream->fileReadStatus = 0;

                    ++fileStream->numDataChunks;
                    readStart = readOffsetEnd;
                }
                else
                {
                    if (pak->patchCount >= pak->memoryData.pakHeader.patchIndex)
                    {
                        FS_CloseAsyncFile(fileStream->fileHandle);
                        fileStream->fileHandle = PAK_INVALID_HANDLE;
                        fileStream->readOffset = 0;
                        fileStream->finishedLoadingPatches = true;

                        return memoryData->patchSrcSize == 0;
                    }

                    if (!pak->dword14)
                        return memoryData->patchSrcSize == 0;

                    char pakPatchPath[MAX_PATH] = {};
                    sprintf(pakPatchPath, "%s%s", Pak_GetReadPath(), pak->memoryData.fileName);

                    // get path of next patch rpak to load
                    if (pak->memoryData.patchIndices[pak->patchCount])
                    {
                        char* pExtension = nullptr;

                        char* it = pakPatchPath;
                        while (*it)
                        {
                            if (*it == '.')
                                pExtension = it;
                            else if (*it == '\\' || *it == '/')
                                pExtension = nullptr;

                            ++it;
                        }

                        if (pExtension)
                            it = pExtension;

                        // replace extension '.rpak' with '(xx).rpak'
                        snprintf(it, &pakPatchPath[sizeof(pakPatchPath)] - it,
                            "(%02u).rpak", pak->memoryData.patchIndices[pak->patchCount]);
                    }

                    const int patchFileHandle = FS_OpenAsyncFile(pakPatchPath, 5, &numBytesToProcess);

                    if (patchFileHandle == FS_ASYNC_FILE_INVALID)
                        Error(eDLL_T::RTECH, EXIT_FAILURE, "Couldn't open file \"%s\".\n", pakPatchPath);

                    if (numBytesToProcess < pak->memoryData.patchHeaders[pak->patchCount].compressedSize)
                        Error(eDLL_T::RTECH, EXIT_FAILURE, "File \"%s\" appears truncated; read size: %zu < expected size: %zu.\n",
                            pakPatchPath, numBytesToProcess, pak->memoryData.patchHeaders[pak->patchCount].compressedSize);

                    FS_CloseAsyncFile(fileStream->fileHandle);

                    fileStream->fileHandle = patchFileHandle;

                    const size_t readOffset = ALIGN_VALUE(fileStream->numDataChunks, 8ull) * PAK_READ_DATA_CHUNK_SIZE;
                    fileStream->fileReadStatus = (fileStream->numDataChunks == ALIGN_VALUE(fileStream->numDataChunks, 8ull)) + 1;

                    fileStream->readOffset = readOffset;
                    fileStream->fileSize = readOffset + pak->memoryData.patchHeaders[pak->patchCount].compressedSize;

                    pak->patchCount++;
                }
            }
        }
    }

    return memoryData->patchSrcSize == 0;
}

// sets patch variables for copying the next unprocessed page into the relevant slab buffer
// if this is a header page, fetch info from the next unprocessed asset and copy over the asset's header
static bool Pak_PrepareNextPageForPatching(PakLoadedInfo_s* const loadedInfo, PakFile_s* const pak)
{
    Pak_RunAssetLoadingJobs(pak);

    // numProcessedPointers has just been set in the above function call
    pak->memoryData.numShiftedPointers = pak->numProcessedPointers;

    if (pak->processedPageCount == pak->GetPageCount())
        return false;

    const uint32_t highestProcessedPageIdx = pak->processedPageCount + pak->firstPageIdx;
    pak->processedPageCount++;

    const int currentPageIndex = highestProcessedPageIdx < pak->GetPageCount()
        ? highestProcessedPageIdx
        : highestProcessedPageIdx - pak->GetPageCount();

    const PakPageHeader_s* const nextMemPageHeader = &pak->memoryData.pageHeaders[currentPageIndex];
    if ((pak->memoryData.slabHeaders[nextMemPageHeader->slabIndex].typeFlags & (SF_CPU | SF_TEMP)) != 0)
    {
        pak->memoryData.patchSrcSize = nextMemPageHeader->dataSize;
        pak->memoryData.patchDstPtr = reinterpret_cast<char*>(pak->memoryData.memPageBuffers[currentPageIndex]);

        return true;
    }

    // headers
    PakAsset_s* const pakAsset = pak->memoryData.ppAssetEntries[pak->memoryData.someAssetCount];

    pak->memoryData.patchSrcSize = pakAsset->headerSize;
    const int assetTypeIdx = pakAsset->HashTableIndexForAssetType();

    pak->memoryData.patchDstPtr = reinterpret_cast<char*>(loadedInfo->slabBuffers[0]) + pak->memoryData.unkAssetTypeBindingSizes[assetTypeIdx];
    pak->memoryData.unkAssetTypeBindingSizes[assetTypeIdx] += g_pakGlobals->assetBindings[assetTypeIdx].structSize;

    return true;
}

static bool Pak_ProcessAssets(PakLoadedInfo_s* const loadedInfo)
{
    PakFile_s* const pak = loadedInfo->pakFile;

    while (pak->processedAssetCount != pak->GetAssetCount())
    {
        // TODO: invert condition and make the branch encompass the whole loop
        if (!(pak->memoryData.patchSrcSize + pak->memoryData.field_2A8))
        {
            if (Pak_PrepareNextPageForPatching(loadedInfo, pak))
                continue;

            break;
        }

        if (!Pak_ProcessPakFile(pak))
            return false;

        // processedPageCount must be greater than 0 here otherwise the page index will be negative and cause a crash
        // if this happens, something probably went wrong with the patch data condition at the start of the loop, as that
        // function call should increment processedPageCount if it succeeded
        assert(pak->processedPageCount > 0);

        const uint32_t pageCount = pak->GetPageCount();
        const uint32_t v4 = (pak->firstPageIdx - 1) + pak->processedPageCount;

        uint32_t shiftedPageIndex = v4;

        if (v4 >= pageCount)
            shiftedPageIndex -= pageCount;

        // if "temp_" slab
        if ((pak->memoryData.slabHeaders[pak->memoryData.pageHeaders[shiftedPageIndex].slabIndex].typeFlags & (SF_CPU | SF_TEMP)) != 0)
        {
            if (Pak_PrepareNextPageForPatching(loadedInfo, pak))
                continue;

            break;
        }

        PakAsset_s* const asset = pak->memoryData.ppAssetEntries[pak->memoryData.someAssetCount];
        const uint32_t headPageOffset = asset->headPtr.offset;
        char* const v8 = pak->memoryData.patchDstPtr - asset->headerSize;

        const uint32_t newOffsetFromSlabBufferToHeader = LODWORD(pak->memoryData.patchDstPtr)
            - asset->headerSize
            - LODWORD(loadedInfo->slabBuffers[0]);
        asset->headPtr.offset = newOffsetFromSlabBufferToHeader;

        const uint32_t offsetSize = newOffsetFromSlabBufferToHeader - headPageOffset;

        for (uint32_t i = pak->memoryData.numShiftedPointers; i < pak->GetPointerCount(); pak->memoryData.numShiftedPointers = i)
        {
            PakPage_u* ptr = &pak->memoryData.virtualPointers[i];

            ASSERT_PAKPTR_VALID(pak, ptr);

            if (ptr->index != shiftedPageIndex)
                break;

            const uint32_t offsetToPointer = ptr->offset - headPageOffset;
            if (offsetToPointer >= asset->headerSize)
                break;

            PakPage_u* const pagePtr = reinterpret_cast<PakPage_u*>(v8 + offsetToPointer);

            ASSERT_PAKPTR_VALID(pak, ptr);

            ptr->offset += offsetSize;

            if (pagePtr->index == shiftedPageIndex)
                pagePtr->offset += offsetSize;

            i = pak->memoryData.numShiftedPointers + 1;
        }

        const uint32_t nUses = pak->GetHeader().usesCount;
        const uint32_t nPages = pak->GetPageCount();
        if (asset->usesIndex <= nUses && asset->usesCount <= nUses - asset->usesIndex)
        {
            for (uint32_t j = 0; j < asset->usesCount; ++j)
            {
                PakPage_u* const descriptor = &pak->memoryData.pageDescriptors[asset->usesIndex + j];

                if (descriptor->index >= nPages)
                    continue;
                if (descriptor->index == shiftedPageIndex)
                    descriptor->offset += offsetSize;
            }
        }

        const uint32_t v16 = ++pak->memoryData.someAssetCount;

        PakAsset_s* v17 = nullptr;
        if (v16 < pak->GetAssetCount() && (v17 = pak->memoryData.ppAssetEntries[v16], v17->headPtr.index == shiftedPageIndex))
        {
            pak->memoryData.field_2A8 = v17->headPtr.offset - headPageOffset - asset->headerSize;
            pak->memoryData.patchSrcSize = v17->headerSize;
            const uint8_t assetTypeIdx = v17->HashTableIndexForAssetType();

            pak->memoryData.patchDstPtr = reinterpret_cast<char*>(loadedInfo->slabBuffers[0]) + pak->memoryData.unkAssetTypeBindingSizes[assetTypeIdx];

            pak->memoryData.unkAssetTypeBindingSizes[assetTypeIdx] += g_pakGlobals->assetBindings[assetTypeIdx].structSize;
        }
        else
        {
            if (Pak_PrepareNextPageForPatching(loadedInfo, pak))
                continue;

            break;
        }
    }

    if (!JT_IsJobDone(pak->memoryData.assetLoadJobGroupId))
        return false;

    uint32_t i = 0;
    PakAsset_s* pAsset = nullptr;

    for (int j = pak->memoryData.pakId & PAK_MAX_LOADED_PAKS_MASK; i < pak->GetHeader().assetCount; loadedInfo->assetGuids[i - 1] = pAsset->guid)
    {
        pAsset = &pak->memoryData.assetEntries[i];
        if (pAsset->numRemainingDependencies)
        {
            //printf("[%s] processing deps for %llX (%.4s)\n", pak->GetName, pAsset->guid, (char*)&pAsset->magic);
            Pak_ResolveAssetRelations(pak, pAsset);

            const int assetIndex = pak->memoryData.loadedAssetIndices[i];
            const PakAssetShort_s& loadedAsset = g_pakGlobals->loadedAssets[assetIndex];

            if (g_pakGlobals->trackedAssets[loadedAsset.trackerIndex].loadedPakIndex == j)
            {
                PakTracker_s* pakTracker = g_pakGlobals->pakTracker;

                if (pakTracker)
                {
                    if (pakTracker->numPaksTracked)
                    {
                        int* trackerIndices = g_pakGlobals->pakTracker->loadedAssetIndices;
                        uint32_t count = 0;

                        while (*trackerIndices != assetIndex)
                        {
                            ++count;
                            ++trackerIndices;

                            if (count >= pakTracker->numPaksTracked)
                                goto LABEL_41;
                        }

                        goto LABEL_42;
                    }
                }
                else
                {
                   pakTracker = reinterpret_cast<PakTracker_s*>(AlignedMemAlloc()->Alloc(sizeof(PakTracker_s), 8));
                   pakTracker->numPaksTracked = 0;
                   pakTracker->unk_4 = 0;
                   pakTracker->unk_8 = 0;

                   g_pakGlobals->pakTracker = pakTracker;
                }
            LABEL_41:

                pakTracker->loadedAssetIndices[pakTracker->numPaksTracked] = assetIndex;
                ++pakTracker->numPaksTracked;
            }
        }
    LABEL_42:
        ++i;
    }

    if (g_pakGlobals->pakTracker)
        Pak_FinalizeLoadedInfo(loadedInfo, 0);

    loadedInfo->status = PakStatus_e::PAK_STATUS_LOADED;

    // install unload wrappers for any newly registered asset types
    Pak_InstallUnloadWrappers();

    // The unload callback receives trackedAssets.memPage (the raw slab header
    // ptr), not loadedAssets.head, so the guid lookup is keyed on memPage.
    for (uint32_t assetIdx = 0; pak_debugassetunload.GetBool() && assetIdx < loadedInfo->assetCount; assetIdx++)
    {
        const int loadedIndex = pak->memoryData.loadedAssetIndices[assetIdx];
        const PakAssetShort_s& loadedAsset = g_pakGlobals->loadedAssets[loadedIndex];
        const uint32_t trackerIdx = loadedAsset.trackerIndex;

        if (trackerIdx >= PAK_MAX_TRACKED_ASSETS)
            continue;

        void* const memPage = g_pakGlobals->trackedAssets[trackerIdx].memPage;
        const PakGuid_t guid = loadedInfo->assetGuids[assetIdx];

        if (memPage && guid)
            s_assetHeadToGuid[memPage] = guid;
    }

    return true;
}

static void Pak_StubInvalidAssetBinds(PakFile_s* const pak, PakSlabDescriptor_s* const desc)
{
    for (uint32_t i = 0; i < pak->GetAssetCount(); ++i)
    {
        PakAsset_s* const asset = &pak->memoryData.assetEntries[i];
        pak->memoryData.ppAssetEntries[i] = asset;

        const uint8_t assetTypeIndex = asset->HashTableIndexForAssetType();
        desc->assetTypeCount[assetTypeIndex]++;

        PakAssetBinding_s* const assetBinding = &g_pakGlobals->assetBindings[assetTypeIndex];

        if (assetBinding->type == PakAssetBinding_s::NONE)
        {
            assetBinding->extension = asset->magic;
            assetBinding->version = asset->version;
            assetBinding->description = "<unknown>";
            assetBinding->loadAssetFunc = nullptr;
            assetBinding->unloadAssetFunc = nullptr;
            assetBinding->replaceAssetFunc = nullptr;
            assetBinding->allocator = AlignedMemAlloc();
            assetBinding->headerSize = asset->headerSize;
            assetBinding->structSize = asset->headerSize;
            assetBinding->headerAlignment = pak->memoryData.pageHeaders[asset->headPtr.index].pageAlignment;
            assetBinding->type = PakAssetBinding_s::STUB;
        }

        (void)assetBinding;
    }
}

static bool Pak_StartLoadingPak(PakLoadedInfo_s* const loadedInfo)
{
    PakFile_s* const pakFile = loadedInfo->pakFile;

    if (pakFile->memoryData.patchSrcSize && !Pak_ProcessPakFile(pakFile))
        return false;

    PakSlabDescriptor_s slabDesc = {};

    Pak_StubInvalidAssetBinds(pakFile, &slabDesc);

    const uint32_t numAssets = pakFile->GetAssetCount();

    if (pakFile->memoryData.pakHeader.patchIndex)
        pakFile->firstPageIdx = pakFile->memoryData.patchDataHeader->pageCount;

    Pak_SortOrInsertAssetEntries(pakFile->memoryData.ppAssetEntries, &pakFile->memoryData.ppAssetEntries[numAssets], numAssets, pakFile);

    // pak must have no more than PAK_MAX_SLABS slabs as otherwise we will overrun the above "slabSizes" array
    // and write to arbitrary locations on the stack
    if (pakFile->GetSlabCount() > PAK_MAX_SLABS)
    {
        Error(eDLL_T::RTECH, EXIT_FAILURE, "Too many slabs in pakfile '%s'. Max %hu, found %hu.\n", pakFile->GetName(), PAK_MAX_SLABS, pakFile->GetSlabCount());
        return false;
    }

    Pak_AlignSlabHeaders(pakFile, &slabDesc);
    Pak_AlignSlabData(pakFile, &slabDesc);

    // allocate slab buffers with predetermined alignments; pages will be
    // copied into here
    for (int8_t i = 0; i < PAK_SLAB_BUFFER_TYPES; ++i)
    {
        if (slabDesc.slabSizeForType[i])
            loadedInfo->slabBuffers[i] = AlignedMemAlloc()->Alloc(slabDesc.slabSizeForType[i], slabDesc.slabAlignmentForType[i]);
    }

    Pak_CopyPagesToSlabs(pakFile, loadedInfo, &slabDesc);
    if (loadedInfo->status == PakStatus_e::PAK_STATUS_ERROR)
        return false;

    const PakFileHeader_s& pakHdr = pakFile->GetHeader();

    if (Pak_StreamingEnabled())
        Pak_LoadStreamingData(loadedInfo);

    const __int64 v106 = pakHdr.pointerCount + 2 * (pakHdr.patchIndex + pakHdr.assetCount + 4ull * pakHdr.assetCount + pakHdr.memSlabCount);
    const __int64 patchDestOffset = pakHdr.GetTotalHeaderSize() + 2 * (pakHdr.patchIndex + 6ull * pakHdr.memPageCount + 4 * v106);

    pakFile->dword14 = 1;

    PakMemoryData_s& memoryData = pakFile->memoryData;

    memoryData.patchSrcSize = pakFile->memoryData.fileSize - patchDestOffset;
    memoryData.patchDstPtr = (char*)&pakHdr + patchDestOffset;

    loadedInfo->status = PakStatus_e::PAK_STATUS_LOAD_PAKHDR;

    return true;
}

//-----------------------------------------------------------------------------
// retrieves the patch index to load for given rpak file, returns NULL if no
// patches are to be loaded for given pak file.
//-----------------------------------------------------------------------------
static uint32 Pak_GetPatchIndexForPak(const char* const pakName)
{
    int totalPatchCount = g_pakGlobals->numPatchedPaks;

    if (!totalPatchCount)
        return 0;

    int iterator = 0;

    while (iterator < totalPatchCount)
    {
        const int index = (totalPatchCount + iterator) >> 1;
        const int compareResult = stricmp(pakName, g_pakGlobals->patchedPakFiles[index]);

        if (compareResult < 0)
            totalPatchCount = index;
        else if (compareResult > 0)
            iterator = index + 1;
        else
            return g_pakGlobals->patchNumbers[index];
    }

    return 0; // Found nothing.
}

static bool Pak_SetupBuffersAndLoad(const PakHandle_t pakId)
{
    PakLoadedInfo_s* const loadedInfo = Pak_GetPakInfo(pakId);
    loadedInfo->status = PAK_STATUS_LOAD_STARTING;

    const char* pakFilePath = loadedInfo->fileName;
    assert(pakFilePath);

    const char* nameUnqualified = V_UnqualifiedFileName(pakFilePath);
    char relativeFilePath[MAX_OSPATH];

    // patches are only supported for paks that reside in the directory returned
    // by the API 'Pak_GetBaseLoadPath' relative from the executable. We only
    // load a single patch master asset and it only tracks core paks.
    if (nameUnqualified == pakFilePath)
    {
        snprintf(relativeFilePath, sizeof(relativeFilePath), "%s%s", Pak_GetReadPath(), pakFilePath);
        // if this pak is patched, load the last patch file first before proceeding
        // with any other pak that is getting patched. note that the patch number
        // does not indicate which pak file is the actual last patch file; a patch
        // with number (01) can also patch (02) and base, these details are
        // determined from the pak file header
        const uint32_t patchIndexToLoad = Pak_GetPatchIndexForPak(pakFilePath);

        if (patchIndexToLoad)
        {
            char* const extension = (char*)V_GetFileExtension(relativeFilePath, true);
            snprintf(extension, &relativeFilePath[sizeof(relativeFilePath)] - extension, "(%02u).rpak", patchIndexToLoad);
        }

        pakFilePath = relativeFilePath;
    }

    // This stored the actual load path of our pak file; if the pak is being
    // loaded from a mod path, then that path will be stored here. This is
    // needed because if a pak file has a module, then we need to load that
    // from the same path as the pak file.
    char actualLoadPath[MAX_OSPATH];
    size_t totalPakFileBufSize;

    const int pakFileHandle = FS_OpenAsyncFile(pakFilePath, loadedInfo->logChannel, &totalPakFileBufSize, actualLoadPath, sizeof(actualLoadPath));

    if (pakFileHandle == FS_ASYNC_FILE_INVALID)
    {
        Error(eDLL_T::RTECH, loadedInfo->logChannel == 5 ? EXIT_FAILURE : 0, "Couldn't read package file \"%s\".\n", pakFilePath);

        loadedInfo->status = PAK_STATUS_ERROR;
        return false;
    }

    loadedInfo->fileHandle = pakFileHandle;
    pakFilePath = actualLoadPath; // Update it to our actual load path.

    // File is truncated or corrupt.
    if (totalPakFileBufSize < sizeof(PakFileHeader_s))
    {
        loadedInfo->status = PAK_STATUS_ERROR;
        return false;
    }

    PakFileHeader_s pakHdr;
    const int asyncFile = v_FS_ReadAsyncFile(pakFileHandle, 0, sizeof(PakFileHeader_s), &pakHdr, 0, 0, 4);

    const char* statusMsg = "(no reason)";
    const uint8_t currentStatus = g_pakLoadApi->WaitAndCheckAsyncRequest(asyncFile, nullptr, &statusMsg);

    if (currentStatus == AsyncHandleStatus_s::Status_e::FS_ASYNC_ERROR)
    {
        Error(eDLL_T::RTECH, EXIT_FAILURE, "Error reading pak file \"%s\" -- %s\n", pakFilePath, statusMsg);

        loadedInfo->status = PAK_STATUS_ERROR;
        return false;
    }

    if (pakHdr.magic != PAK_HEADER_MAGIC ||
        pakHdr.version != PAK_HEADER_VERSION)
    {
        loadedInfo->status = PAK_STATUS_ERROR;
        return false;
    }

    char libraryFilePath[MAX_OSPATH];

    if (pakHdr.flags & PAK_HEADER_FLAGS_HAS_MODULE_EXTENDED)
    {
        const char* const extStart = V_GetFileExtension(pakFilePath, true);

        // Loaded a pak file without an extension.
        if (extStart == pakFilePath)
        {
            loadedInfo->status = PAK_STATUS_ERROR;
            return false;
        }

        const size_t unqualifiedFileNameLen = (extStart - pakFilePath);
        const size_t numCopyBytesLeft = sizeof(libraryFilePath) - unqualifiedFileNameLen;

        const static char dllExt[] = ".dll";

        // String buffer couldn't contain new extension. This happens when the
        //.dll extension is larger than the file's original extension, and the
        // original file path was using everything but the last 4 or less bytes
        // of the buffer. This should never happen on this platform. All paks
        // always have the.rpak extension as well so this really is to prevent
        // stack smashes in exceptional cases.
        if (numCopyBytesLeft < sizeof(dllExt))
        {
            loadedInfo->status = PAK_STATUS_ERROR;
            return false;
        }

        memcpy(libraryFilePath, pakFilePath, unqualifiedFileNameLen);
        memcpy(&libraryFilePath[unqualifiedFileNameLen], dllExt, sizeof(dllExt));

        // rtech_game has no DEDICATED; dedi never loads pak sibling modules.
        Warning(eDLL_T::RTECH, "Pak_SetupBuffersAndLoad: refusing module load for \"%s\"\n",
            pakFilePath);
    }

    loadedInfo->fileTime = pakHdr.fileTime;

    uint32_t assetCount = pakHdr.assetCount;
    const uint16_t patchIndex = pakHdr.patchIndex;
    const uint16_t memPageCount = pakHdr.memPageCount;
    const uint16_t memSlabCount = pakHdr.memSlabCount;
    const __int64 v32 = *(unsigned int*)&pakHdr.unk2[4];

    if (assetCount > PAK_MAX_LOADED_ASSETS)
    {
        Warning(eDLL_T::RTECH, "[PAK-PARSE] assetCount %u exceeds %u\n",
            assetCount, PAK_MAX_LOADED_ASSETS);
        loadedInfo->status = PakStatus_e::PAK_STATUS_ERROR;
        return false;
    }
    loadedInfo->assetCount = assetCount;

    loadedInfo->assetGuids = (PakGuid_t*)loadedInfo->allocator->Alloc(sizeof(PakGuid_t) * assetCount, 8);
    if (assetCount && !loadedInfo->assetGuids)
    {
        Warning(eDLL_T::RTECH, "[PAK-PARSE] assetGuids alloc failed (count %u)\n", assetCount);
        loadedInfo->status = PakStatus_e::PAK_STATUS_ERROR;
        return false;
    }

    const size_t streamingFilesBufSize = pakHdr.streamingFilesBufSize[STREAMING_SET_OPTIONAL] + pakHdr.streamingFilesBufSize[STREAMING_SET_MANDATORY];
    const size_t memPagePointersBufSize = 8 * memPageCount;

    totalPakFileBufSize = streamingFilesBufSize
        + (patchIndex != 0 ? 8 : 0)
        + 2
        * (patchIndex
            + 2
            * (pakHdr.dependentsCount
                + *(unsigned int*)pakHdr.unk2
                + 3 * memPageCount
                + 2 * (pakHdr.pointerCount + pakHdr.usesCount + 16i64 + 2 * (assetCount + patchIndex + 4 * assetCount + memSlabCount))))
        + v32;

    const __int64 v80 = 4 * assetCount;
    const uint64_t v90 = totalPakFileBufSize + 2080;
    const __int64 v33 = -((__int64)totalPakFileBufSize + 2080 + 4 * (__int64)assetCount) & 7;
    const __int64 v89 = v33;
    const __int64 v34 = 4 * assetCount + totalPakFileBufSize + 2080 + v33 + 8 * memPageCount + 12 * assetCount;
    const __int64 v35 = (-(4 * (__int64)assetCount + (__int64)totalPakFileBufSize + 2080 + (__int64)v33 + 8 * (__int64)memPageCount + 12 * (__int64)assetCount) & 7) + 4088i64;

    uint64_t ringBufferStreamSize;
    uint64_t ringBufferOutSize;

    const bool isCompressed = (pakHdr.flags & (PAK_HEADER_FLAGS_RTECH_ENCODED | PAK_HEADER_FLAGS_ZSTD_ENCODED)) != 0;

    if (isCompressed)
    {
        ringBufferStreamSize = PAK_DECODE_IN_RING_BUFFER_SIZE;
        ringBufferOutSize = PAK_DECODE_OUT_RING_BUFFER_SIZE;

        if (pakHdr.compressedSize < PAK_DECODE_IN_RING_BUFFER_SIZE && !patchIndex)
            ringBufferStreamSize = (pakHdr.compressedSize + PAK_DECODE_IN_RING_BUFFER_SMALL_MASK) & 0xFFFFFFFFFFFFF000ui64;
    }
    else
    {
        ringBufferStreamSize = 0;
        ringBufferOutSize = PAK_DECODE_IN_RING_BUFFER_SIZE;
    }

    if (ringBufferOutSize > pakHdr.decompressedSize && !patchIndex)
        ringBufferOutSize = (pakHdr.decompressedSize + PAK_DECODE_IN_RING_BUFFER_SMALL_MASK) & 0xFFFFFFFFFFFFF000ui64;

    PakFile_s* const pak = (PakFile_s*)AlignedMemAlloc()->Alloc(v34 + v35 + ringBufferOutSize + ringBufferStreamSize, 8);

    if (!pak)
    {
        loadedInfo->status = PAK_STATUS_ERROR;
        return false;
    }

    loadedInfo->pakFile = pak;

    pak->processedPageCount = 0;
    pak->firstPageIdx = 0;
    pak->patchCount = 0;
    pak->dword14 = 0;
    pak->numProcessedPointers = 0;
    pak->processedAssetCount = 0;
    pak->memoryData.someAssetCount = 0;
    pak->memoryData.numShiftedPointers = 0;

    PakGuidDescriptor_s* const guidBuf = (PakGuidDescriptor_s*)loadedInfo->allocator->Alloc(sizeof(PakGuidDescriptor_s) * pakHdr.usesCount + 8, 8);
    loadedInfo->guidDestriptors = guidBuf;

    guidBuf->unk1 = 0;
    guidBuf->unk2 = 0;

    pak->memoryData.pakHeader = pakHdr;

    pak->memoryData.pakId = pakId;
    pak->memoryData.fileSize = totalPakFileBufSize;

    pak->memoryData.assetLoadJobGroupId = pakHdr.assetCount
        ? JT_BeginJobGroup(0)
        : JT_JOB_GROUP_BASE_ID;

    uint8_t** v44 = (uint8_t**)((char*)pak + v89 + v90 + v80);
    pak->memoryData.memPageBuffers = v44;
    PakAsset_s** v45 = (PakAsset_s**)((char*)v44 + memPagePointersBufSize);
    pak->memoryData.ppAssetEntries = v45;
    pak->memoryData.loadedAssetIndices = (int*)&v45[pakHdr.assetCount];

    PakPatchFileHeader_s* p_patchDataHeader = &pak->memoryData.patchHeader;
    PakPatchFileHeader_s* p_patchFileHeader = (PakPatchFileHeader_s*)&pak->memoryData.patchHeader.decompressedSize;

    const uint16_t patchIndexCached = pak->memoryData.pakHeader.patchIndex;

    if (!patchIndexCached)
    {
        p_patchDataHeader = nullptr;
        p_patchFileHeader = &pak->memoryData.patchHeader;
    }

    pak->memoryData.patchDataHeader = (PakPatchDataHeader_s*)p_patchDataHeader;
    pak->memoryData.patchHeaders = p_patchFileHeader;
    uint16_t* v51 = (unsigned __int16*)&p_patchFileHeader[patchIndexCached];
    pak->memoryData.patchIndices = v51;
    char* v52 = (char*)&v51[patchIndexCached];
    char* v53 = &v52[pak->memoryData.pakHeader.streamingFilesBufSize[STREAMING_SET_MANDATORY]];
    pak->memoryData.streamingFilePaths[STREAMING_SET_MANDATORY] = v52;
    __int64 v54 = pak->memoryData.pakHeader.streamingFilesBufSize[STREAMING_SET_OPTIONAL];
    pak->memoryData.streamingFilePaths[STREAMING_SET_OPTIONAL] = v53;
    PakSlabHeader_s* v55 = (PakSlabHeader_s*)&v53[v54];
    __int64 v56 = pak->memoryData.pakHeader.memSlabCount;
    pak->memoryData.slabHeaders = v55;
    __int64 v57 = pak->memoryData.pakHeader.memPageCount;
    PakPageHeader_s* const v58 = (PakPageHeader_s*)&v55[v56];
    pak->memoryData.pageHeaders = v58;
    const uint32_t pointerCount = pak->memoryData.pakHeader.pointerCount;
    PakPage_u* const v60 = (PakPage_u*)&v58[v57];
    pak->memoryData.virtualPointers = v60;
    assetCount = pak->memoryData.pakHeader.assetCount;
    PakAsset_s* const v62 = (PakAsset_s*)&v60[pointerCount];
    pak->memoryData.assetEntries = v62;
    const uint32_t usesCount = pak->memoryData.pakHeader.usesCount;
    PakPage_u* pageDescriptors = (PakPage_u*)&v62[assetCount];
    pak->memoryData.pageDescriptors = pageDescriptors;
    const uint32_t dependentsCount = pak->memoryData.pakHeader.dependentsCount;
    uint32_t* const v66 = (uint32_t*)&pageDescriptors[usesCount];
    pak->memoryData.fileRelations = v66;
    const __int64 v67 = *(unsigned int*)pak->memoryData.pakHeader.unk2;
    uint32_t* const v68 = &v66[dependentsCount];
    pak->memoryData.ptr5E0 = v68;
    const __int64 v69 = *(unsigned int*)&pak->memoryData.pakHeader.unk2[4];
    pak->memoryData.ptr5F0 = nullptr;
    uint32_t* v70 = &v68[v67];
    pak->memoryData.ptr5E8 = v70;
    const __int64 v71 = (__int64)v70 + v69;
    pak->memoryData.ptr5F8 = (char*)v70 + v69;

    if (patchIndexCached)
    {
        PakPatchDataHeader_s* const patchDataHeader = pak->memoryData.patchDataHeader;
        if (patchDataHeader->editStreamSize)
        {
            *(_QWORD*)&pak->memoryData.ptr5F0 = v71;
            *(_QWORD*)&pak->memoryData.ptr5F8 = v71 + (unsigned int)patchDataHeader->editStreamSize;
        }
    }

    pak->memoryData.fileName = loadedInfo->fileName;
    pak->fileStream.readOffset = 0;
    pak->fileStream.fileSize = pakHdr.compressedSize;
    pak->fileStream.fileHandle = loadedInfo->fileHandle;
    loadedInfo->fileHandle = FS_ASYNC_FILE_INVALID;

    pak->fileStream.bufferMask = PAK_DECODE_IN_RING_BUFFER_MASK;

    uint8_t* const ringBuffer = (uint8_t*)(((unsigned __int64)pak + v35 + v34) & 0xFFFFFFFFFFFFF000ui64);

    pak->fileStream.buffer = ringBuffer;
    pak->fileStream.numDataChunksProcessed = 0;
    pak->fileStream.numDataChunks = 0;
    pak->fileStream.fileReadStatus = AsyncHandleStatus_s::Status_e::FS_ASYNC_CANCELLED;
    pak->fileStream.finishedLoadingPatches = 0;
    pak->fileStream.numLoadedFiles = 0;
    pak->fileStream.bytesStreamed = sizeof(PakFileHeader_s);
    pak->decompBuffer = &ringBuffer[ringBufferStreamSize];
    pak->inputBytePos = sizeof(PakFileHeader_s);
    pak->processedStreamCount = 0;
    pak->resetInBytePos = true;
    pak->updateBytePosPostProcess = false;
    pak->isCompressed = false;

    pak->headerSize = sizeof(PakFileHeader_s);

    pak->maxCopySize = isCompressed
        ? PAK_DECODE_OUT_RING_BUFFER_MASK
        : PAK_DECODE_IN_RING_BUFFER_MASK;

    memset(&pak->pakDecoder, 0, sizeof(pak->pakDecoder));

    pak->pakDecoder.outBufBytePos = sizeof(PakFileHeader_s);
    pak->pakDecoder.decompSize = sizeof(PakFileHeader_s);
    pak->memoryData.processedPatchedDataSize = sizeof(PakFileHeader_s);
    const uint64_t v77 = pakHdr.pointerCount + 2 * (pakHdr.patchIndex + (unsigned __int64)pakHdr.assetCount + 4i64 * pakHdr.assetCount + pakHdr.memSlabCount);

    pak->memoryData.field_2A8 = 0ull;
    pak->memoryData.patchData = nullptr;
    pak->memoryData.patchDataPtr = nullptr;
    pak->memoryData.bitBuf.m_dataBuf = 0;
    pak->memoryData.bitBuf.m_bitsAvailable = 0;
    pak->memoryData.patchDataOffset = 0;
    pak->memoryData.patchSrcSize = streamingFilesBufSize + (pakHdr.patchIndex != 0 ? 8 : 0) + 2 * (pakHdr.patchIndex + 6 * pakHdr.memPageCount + 4 * v77);
    pak->memoryData.patchDstPtr = (char*)&pak->memoryData.patchHeader;
    pak->memoryData.patchFunc = g_pakPatchApi[0];

    const bool isBasePatch = pakHdr.patchIndex == 0;
    pak->memoryData.numPatchBytesToProcess = pak->memoryData.pakHeader.decompressedSize + isBasePatch - sizeof(PakFileHeader_s);

    Pak_ProcessPakFile(pak);
    return true;
}

void V_PakParse::Detour(const bool bAttach) const
{
    DetourSetup(&v_Pak_SetupBuffersAndLoad, &Pak_SetupBuffersAndLoad, bAttach);

    DetourSetup(&v_Pak_LoadAsync, &Pak_LoadAsync, bAttach);
    DetourSetup(&v_Pak_UnloadAsync, &Pak_UnloadAsync, bAttach);

    DetourSetup(&v_Pak_StartLoadingPak, &Pak_StartLoadingPak, bAttach);

    DetourSetup(&v_Pak_ProcessPakFile, &Pak_ProcessPakFile, bAttach);
    DetourSetup(&v_Pak_ResolveAssetRelations, &Pak_ResolveAssetRelations, bAttach);
    DetourSetup(&v_Pak_ProcessAssets, &Pak_ProcessAssets, bAttach);

    DetourSetup(&v_Pak_RunAssetLoadingJobs, &Pak_RunAssetLoadingJobs, bAttach);
}
#endif // CLIENT_DLL
