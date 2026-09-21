//=============================================================================//
//
// Purpose: Synthesize stgs from platform/settings/**/*.json (donor clone + named fields).
//
//=============================================================================//

#include "core/stdafx.h"
#include "settings_disk.h"
#include "rtech/pak/paktools.h"
#include "tier0/dbg.h"
#include "tier1/cvar.h"
#include "tier1/strtools.h"
#include "mathlib/crc32.h"
#include <tier2/jsonutils.h>

#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <unordered_set>

//-----------------------------------------------------------------------------
// Limits
//-----------------------------------------------------------------------------
static constexpr size_t SETTINGS_DISK_MAX_FILE_SIZE   = 1 * 1024 * 1024; // 1 MiB
static constexpr size_t SETTINGS_DISK_MAX_FILES       = 256;
static constexpr int    SETTINGS_DISK_MAX_DEPTH       = 8;
static constexpr int    SETTINGS_DISK_MAX_FIELDS      = 256;
static constexpr int    SETTINGS_DISK_MAX_UID_RETRY   = 64;
static constexpr int    SETTINGS_DISK_MAX_BUILD_RETRY = 64;
static constexpr size_t SETTINGS_DISK_MAX_PATH_LEN    = 260;
// platform\settings\<path>.json -> settings/<path>.rpak.
static constexpr const char* s_settingsDiskBasePath  = "platform\\settings\\";

#if !defined(CLIENT_DLL)
// guid->asset slot table is 0x1000 past the asset-TYPE registry.
static constexpr uintptr_t PAK_ASSET_SLOT_TABLE_OFFSET = 0x1000;
static constexpr uint32_t  PAK_ASSET_SLOT_MASK        = 0x3FFFFu; // 262144 slots

// Consumers read guid + header. trackIndex is only on the engine duplicate-guid path.
struct PakAssetSlot_t
{
	uint64_t guid;       // 0x00  0=empty, 1=tombstone, 2=loading, else published
	uint32_t trackIndex; // 0x08
	uint32_t _unused;    // 0x0C
	void*    header;     // 0x10
	uint64_t _reserved;  // 0x18
};
static_assert(sizeof(PakAssetSlot_t) == 32, "PakAssetSlot_t size");
#endif // !CLIENT_DLL

//-----------------------------------------------------------------------------
// Settings asset layout
//-----------------------------------------------------------------------------
struct SettingsLayoutFieldData
{
	uint16_t dataType;   // 0x00
	uint16_t nameOffset; // 0x02  index into layout->strings; 0 = empty slot
	uint32_t offset;     // 0x04  low 24 bits = byte offset into data
};
static_assert(sizeof(SettingsLayoutFieldData) == 8, "SettingsLayoutFieldData size");

struct SettingsLayoutHeader
{
	const char*              name;              // 0x00
	SettingsLayoutFieldData* fieldTable;        // 0x08
	void*                    fieldDocs;         // 0x10
	uint32_t                 tableSize;         // 0x18  power of two
	uint32_t                 fieldCount;        // 0x1C
	uint32_t                 settingsAssetType; // 0x20
	uint32_t                 magicHashMul;      // 0x24
	uint32_t                 magicHashAdd;      // 0x28
	uint32_t                 arraySize;         // 0x2C
	uint32_t                 dataSize;          // 0x30
	uint32_t                 _pad;              // 0x34
	char*                    strings;           // 0x38  name pool
	SettingsLayoutHeader*    childHashTables;   // 0x40
};
static_assert(sizeof(SettingsLayoutHeader) == 0x48, "SettingsLayoutHeader size");

#if defined(CLIENT_DLL)
struct SettingsHeader
{
	SettingsLayoutHeader* layoutHeader; // 0x00
	char*                 data;         // 0x08
	const char*           name;         // 0x10  "settings/....rpak"
	char*                 strings;      // 0x18
	void**                assetRefs;    // 0x20
	uint32_t              uniqueId;     // 0x28  31-bit, script-visible
	uint32_t              pad;          // 0x2C
	void*                 mods;         // 0x30  SettingsMod*
	void*                 modOps;       // 0x38  SettingsModOp*
	uint32_t              dataSize;     // 0x40  real size of the data block
	uint32_t              SPModOpCount; // 0x44
	uint32_t              modCount;     // 0x48
	uint32_t              modOpCount;   // 0x4C
};
static_assert(sizeof(SettingsHeader) == 0x50, "SettingsHeader size");
#else
// S3 has no assetRefs; uniqueId onward is -8 vs S21.
struct SettingsHeader
{
	SettingsLayoutHeader* layoutHeader; // 0x00
	char*                 data;         // 0x08
	const char*           name;         // 0x10
	char*                 strings;      // 0x18
	uint32_t              uniqueId;     // 0x20
	uint32_t              pad;          // 0x24
	void*                 mods;         // 0x28  SettingsMod* (8-byte entries)
	void*                 modOps;       // 0x30  SettingsModOp* (12-byte entries)
	uint32_t              dataSize;     // 0x38
	uint32_t              SPModOpCount; // 0x3C
	uint32_t              modCount;     // 0x40
	uint32_t              modOpCount;   // 0x44
};
static_assert(sizeof(SettingsHeader) == 0x48, "SettingsHeader size");
#endif // CLIENT_DLL

// g_SettingsDataTypeNames
enum SettingsDataType_e : uint16_t
{
	SDT_BOOL          = 0,
	SDT_INT           = 1,
	SDT_FLOAT         = 2,
	SDT_FLOAT2        = 3,
	SDT_FLOAT3        = 4,
	SDT_STRING        = 5,
	SDT_ASSET         = 6,
	SDT_ASSET2        = 7,
	SDT_ARRAY_FIXED   = 8,
	SDT_ARRAY_DYNAMIC = 9,
};

//-----------------------------------------------------------------------------
// JSON scalar copied at parse time so the pending entry outlives the parse buffer.
//-----------------------------------------------------------------------------
struct PendingFieldValue
{
	bool        isBool    = false;
	bool        isNumber  = false;
	bool        isString  = false;
	bool        boolVal   = false;
	int64_t     intVal    = 0;
	double      numVal    = 0.0;
	std::string strVal;
	std::string jsonTypeName;
};

struct PendingField
{
	std::string        name;
	PendingFieldValue  value;
};

//-----------------------------------------------------------------------------
// Parsed JSON, not yet built. Mutable fields only under s_buildMutex after phase 1.
//-----------------------------------------------------------------------------
struct PendingEntry
{
	std::string assetName;
	std::string donorName;
	std::string layoutAsset;
	std::string jsonPath;
	uint64_t    guid = 0;
	uint32_t    uniqueId = 0;
	uint32_t    preferredUid = 0;
	bool        hasPreferredUid = false;
	std::vector<PendingField> fields;

	int  buildAttempts = 0;
	bool built = false;
	bool giveUpWarned = false;
	bool layoutWarned = false;
	bool uidPackedWarned = false;
	SettingsHeader* header = nullptr;
#if !defined(CLIENT_DLL)
	int  slotIndex = -1;
#endif // !CLIENT_DLL
};

//-----------------------------------------------------------------------------
// Phase 1 builds the maps; keys never change after that. Entry fields mutate under s_buildMutex.
//-----------------------------------------------------------------------------
static std::unordered_map<uint64_t, PendingEntry> s_pendingByGuid;
static std::unordered_set<uint64_t>                s_guidIndex;
static std::unordered_map<uint32_t, uint64_t>      s_uidToGuid;
static std::mutex s_buildMutex;
#if !defined(CLIENT_DLL)
// Publish-hook remaining count.
static std::atomic<int> s_pendingRemaining{ 0 };
#endif // !CLIENT_DLL

static bool s_patternsReady = false;
static std::once_flag s_parseOnce;

static ConVar sdk_settings_disk(
	"sdk_settings_disk", "1", FCVAR_RELEASE,
	"When 1, synthesize missing settings (stgs) assets from platform/settings/**/*.json "
	"(donor-clone + named field writes). Packed assets always win. 0 = pass-through.");

static ConVar sdk_settings_disk_verbose(
	"sdk_settings_disk_verbose", "0", FCVAR_DEVELOPMENTONLY,
	"When 1, log every field write from a settings_disk JSON (asset, field, type, value).");

//-----------------------------------------------------------------------------
static bool SettingsDisk_IsSafeComponent(const char* name)
{
	if (!name || !name[0])
		return false;
	if (name[0] == '.')
		return false;

	const size_t len = strlen(name);
	if (len >= SETTINGS_DISK_MAX_PATH_LEN)
		return false;

	for (size_t i = 0; i < len; i++)
	{
		const char c = name[i];
		if (c == '/' || c == '\\' || c == ':')
			return false;
		if (c == '.' && i + 1 < len && name[i + 1] == '.')
			return false;
	}
	return true;
}

// Process-lifetime C string.
static char* SettingsDisk_StrDup(const char* s)
{
	if (!s)
		return nullptr;
	const size_t n = strlen(s) + 1;
	char* out = static_cast<char*>(malloc(n));
	if (!out)
		return nullptr;
	memcpy(out, s, n);
	return out;
}

//-----------------------------------------------------------------------------
// Field-name hash lookup (engine algorithm; do not call into engine).
//-----------------------------------------------------------------------------
static SettingsLayoutFieldData* SettingsDisk_FindField(
	const SettingsLayoutHeader* layout, const char* fieldName)
{
	if (!layout || !fieldName || !layout->fieldTable || !layout->strings || layout->tableSize == 0)
		return nullptr;

	uint32_t h = 0;
	for (const char* p = fieldName; *p; ++p)
		h = h * layout->magicHashMul + static_cast<uint32_t>(static_cast<unsigned char>(*p))
			+ layout->magicHashAdd;

	const uint32_t probe = h ^ (h >> 4);
	const uint32_t mask = layout->tableSize - 1;
	for (uint32_t i = 0; i < layout->tableSize; ++i)
	{
		SettingsLayoutFieldData* e = &layout->fieldTable[(probe + i) & mask];
		if (e->nameOffset == 0)
			return nullptr;
		if (strcmp(&layout->strings[e->nameOffset], fieldName) == 0)
			return e;
	}
	return nullptr;
}

//-----------------------------------------------------------------------------
// uniqueId: JSON value, else 0x40000000 | (crc32(name) & 0x3FFFFFFF). Phase 1 is disk-vs-disk only.
//-----------------------------------------------------------------------------
static uint32_t SettingsDisk_AllocUniqueId(
	uint32_t preferred,
	bool hasPreferred,
	const char* assetName,
	const std::unordered_map<uint32_t, std::string>& usedByDisk)
{
	uint32_t candidate;
	if (hasPreferred)
		candidate = preferred & 0x7FFFFFFFu;
	else
	{
		const uint32_t crc = crc32::update(0,
			reinterpret_cast<const uint8_t*>(assetName), strlen(assetName));
		candidate = 0x40000000u | (crc & 0x3FFFFFFFu);
	}

	if (candidate == 0)
		candidate = 1;

	for (int attempt = 0; attempt < SETTINGS_DISK_MAX_UID_RETRY; ++attempt)
	{
		const auto it = usedByDisk.find(candidate);
		if (it == usedByDisk.end())
			return candidate;

		Warning(eDLL_T::RTECH,
			"[SETTINGS-DISK] uniqueId %u collision between '%s' and '%s' (disk) -- trying %u\n",
			candidate, assetName, it->second.c_str(), (candidate + 1u) & 0x7FFFFFFFu);

		candidate = (candidate + 1u) & 0x7FFFFFFFu;
		if (candidate == 0)
			candidate = 1;
	}

	Warning(eDLL_T::RTECH,
		"[SETTINGS-DISK] uniqueId exhausted disk-collision retries for '%s' -- using %u\n",
		assetName, candidate);
	return candidate;
}

//-----------------------------------------------------------------------------
// SEH-safe donor snapshot (engine pointers only inside __try).
//-----------------------------------------------------------------------------
struct DonorSnap
{
	SettingsLayoutHeader* layoutHeader;
	char*                 data;
	char*                 strings;
#if defined(CLIENT_DLL)
	void**                assetRefs;
#endif // CLIENT_DLL
	uint32_t              dataSize;
	const char*           layoutName;
	bool                  ok;
};

static DonorSnap SettingsDisk_ReadDonor(void* donorVoid)
{
	DonorSnap snap = {};
	if (!donorVoid)
		return snap;

	__try
	{
		SettingsHeader* donor = static_cast<SettingsHeader*>(donorVoid);
		if (!donor->layoutHeader || !donor->data || donor->dataSize == 0 || donor->dataSize > (16u * 1024u * 1024u))
			return snap;

		snap.layoutHeader = donor->layoutHeader;
		snap.data         = donor->data;
		snap.strings      = donor->strings;
#if defined(CLIENT_DLL)
		snap.assetRefs    = donor->assetRefs;
#endif // CLIENT_DLL
		snap.dataSize     = donor->dataSize;
		snap.layoutName   = donor->layoutHeader->name;
		// Touch layout fields so a bad donor faults here.
		volatile uint32_t touch = donor->layoutHeader->tableSize
			^ donor->layoutHeader->magicHashMul
			^ donor->layoutHeader->magicHashAdd;
		(void)touch;
		if (donor->layoutHeader->fieldTable)
			(void)donor->layoutHeader->fieldTable[0].dataType;
		if (donor->layoutHeader->strings)
			(void)donor->layoutHeader->strings[0];
		snap.ok = true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		snap = {};
	}
	return snap;
}

static bool SettingsDisk_SafeMemcpyData(char* dst, const char* src, uint32_t size)
{
	__try
	{
		memcpy(dst, src, size);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

//-----------------------------------------------------------------------------
static void* SettingsDisk_FindAsset(uint64_t guid)
{
#if defined(CLIENT_DLL)
	return v_Pak_FindAssetVoid_S21 ? v_Pak_FindAssetVoid_S21(guid, nullptr) : nullptr;
#else
	if (!g_pPakAssetTypeRegistry_S3 || !guid)
		return nullptr;

	PakAssetSlot_t* const slots =
		reinterpret_cast<PakAssetSlot_t*>(g_pPakAssetTypeRegistry_S3 + PAK_ASSET_SLOT_TABLE_OFFSET);

	uint32_t i = static_cast<uint32_t>(guid) & PAK_ASSET_SLOT_MASK;
	for (uint32_t n = 0; n <= PAK_ASSET_SLOT_MASK; ++n)
	{
		const uint64_t slotGuid = slots[i].guid;
		if (slotGuid == 0)
			return nullptr;
		if (slotGuid == guid)
			return slots[i].header;
		i = (i + 1) & PAK_ASSET_SLOT_MASK;
	}
	return nullptr;
#endif // CLIENT_DLL
}

#if !defined(CLIENT_DLL)
//-----------------------------------------------------------------------------
// Insert: header before guid so another thread never sees a published guid with a null header.
//-----------------------------------------------------------------------------
static bool SettingsDisk_InsertAsset(uint64_t guid, void* header, int* outSlotIndex)
{
	if (!g_pPakAssetTypeRegistry_S3 || !guid || !header)
		return false;

	PakAssetSlot_t* const slots =
		reinterpret_cast<PakAssetSlot_t*>(g_pPakAssetTypeRegistry_S3 + PAK_ASSET_SLOT_TABLE_OFFSET);

	uint32_t i = static_cast<uint32_t>(guid) & PAK_ASSET_SLOT_MASK;
	for (uint32_t n = 0; n <= PAK_ASSET_SLOT_MASK; ++n)
	{
		const uint64_t slotGuid = slots[i].guid;
		if (slotGuid == guid)
		{
			Warning(eDLL_T::RTECH,
				"[SETTINGS-DISK] insert failed: guid 0x%llX already in slot %u\n",
				static_cast<unsigned long long>(guid), i);
			return false;
		}
		if (slotGuid == 0)
		{
			slots[i].header = header;
			slots[i].trackIndex = 0;
			slots[i]._unused = 0;
			slots[i]._reserved = 0;
			// guid last; fence so the compiler does not hoist it.
			std::atomic_thread_fence(std::memory_order_release);
			slots[i].guid = guid;
			if (outSlotIndex)
				*outSlotIndex = static_cast<int>(i);
			return true;
		}
		i = (i + 1) & PAK_ASSET_SLOT_MASK;
	}

	Warning(eDLL_T::RTECH,
		"[SETTINGS-DISK] insert failed: slot table full for guid 0x%llX\n",
		static_cast<unsigned long long>(guid));
	return false;
}
#endif // !CLIENT_DLL

//-----------------------------------------------------------------------------
// Copy a JSON scalar while the parse buffer is still alive.
//-----------------------------------------------------------------------------
static void SettingsDisk_CaptureFieldValue(const rapidjson::Value& v, PendingFieldValue& out)
{
	out = PendingFieldValue();
	out.jsonTypeName = JSON_InternalTypeToString(v.GetType());

	if (v.IsBool())
	{
		out.isBool = true;
		out.boolVal = v.GetBool();
	}
	else if (v.IsNumber())
	{
		out.isNumber = true;
		out.numVal = v.GetDouble();
		if (v.IsInt64())
			out.intVal = v.GetInt64();
		else if (v.IsUint64())
			out.intVal = static_cast<int64_t>(v.GetUint64());
		else
			out.intVal = static_cast<int64_t>(v.GetDouble());
	}
	else if (v.IsString())
	{
		out.isString = true;
		out.strVal.assign(v.GetString(), v.GetStringLength());
	}
}

//-----------------------------------------------------------------------------
static bool SettingsDisk_WriteField(
	char* data,
	uint32_t dataSize,
	const SettingsLayoutHeader* layout,
	const PendingField& pf,
	const char* assetName,
	int* fieldsWritten,
	int* fieldsSkipped)
{
	SettingsLayoutFieldData* field = nullptr;
	__try
	{
		field = SettingsDisk_FindField(layout, pf.name.c_str());
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Warning(eDLL_T::RTECH,
			"[SETTINGS-DISK] SEH during field lookup '%s' in '%s' -- skip field\n",
			pf.name.c_str(), assetName);
		++(*fieldsSkipped);
		return false;
	}

	if (!field)
	{
		Warning(eDLL_T::RTECH,
			"[SETTINGS-DISK] unknown field '%s' in '%s' -- skip\n",
			pf.name.c_str(), assetName);
		++(*fieldsSkipped);
		return false;
	}

	const uint16_t dataType = field->dataType;
	const uint32_t byteOff  = field->offset & 0x00FFFFFFu;

	uint32_t storeBytes = 0;
	switch (dataType)
	{
	case SDT_BOOL:
		storeBytes = 1;
		break;
	case SDT_INT:
	case SDT_FLOAT:
		storeBytes = 4;
		break;
	case SDT_STRING:
	case SDT_ASSET:
	case SDT_ASSET2:
		storeBytes = sizeof(char*);
		break;
	default:
		break;
	}

	if (storeBytes && (byteOff + storeBytes > dataSize))
	{
		Warning(eDLL_T::RTECH,
			"[SETTINGS-DISK] field '%s' in '%s' write past data block (off=%u size=%u dataSize=%u) -- skip\n",
			pf.name.c_str(), assetName, byteOff, storeBytes, dataSize);
		++(*fieldsSkipped);
		return false;
	}

	char* dst = data + byteOff;

	if (dataType == SDT_FLOAT2 || dataType == SDT_FLOAT3
		|| dataType == SDT_ARRAY_FIXED || dataType == SDT_ARRAY_DYNAMIC)
	{
		Warning(eDLL_T::RTECH,
			"[SETTINGS-DISK] unsupported dataType %u for field '%s' in '%s' -- skip\n",
			static_cast<unsigned>(dataType), pf.name.c_str(), assetName);
		++(*fieldsSkipped);
		return false;
	}

	switch (dataType)
	{
	case SDT_BOOL:
	{
		bool b;
		if (pf.value.isBool)
			b = pf.value.boolVal;
		else if (pf.value.isNumber)
			b = pf.value.numVal != 0.0;
		else
		{
			Warning(eDLL_T::RTECH,
				"[SETTINGS-DISK] field '%s' in '%s' expects bool, got %s -- skip\n",
				pf.name.c_str(), assetName, pf.value.jsonTypeName.c_str());
			++(*fieldsSkipped);
			return false;
		}
		*reinterpret_cast<uint8_t*>(dst) = b ? 1 : 0;
		if (sdk_settings_disk_verbose.GetBool())
			Msg(eDLL_T::RTECH,
				"[SETTINGS-DISK] write '%s'.%s type=bool val=%d\n",
				assetName, pf.name.c_str(), b ? 1 : 0);
		break;
	}
	case SDT_INT:
	{
		if (!pf.value.isNumber)
		{
			Warning(eDLL_T::RTECH,
				"[SETTINGS-DISK] field '%s' in '%s' expects int, got %s -- skip\n",
				pf.name.c_str(), assetName, pf.value.jsonTypeName.c_str());
			++(*fieldsSkipped);
			return false;
		}
		const int32_t iv = static_cast<int32_t>(pf.value.intVal);
		*reinterpret_cast<int32_t*>(dst) = iv;
		if (sdk_settings_disk_verbose.GetBool())
			Msg(eDLL_T::RTECH,
				"[SETTINGS-DISK] write '%s'.%s type=int val=%d\n",
				assetName, pf.name.c_str(), iv);
		break;
	}
	case SDT_FLOAT:
	{
		if (!pf.value.isNumber)
		{
			Warning(eDLL_T::RTECH,
				"[SETTINGS-DISK] field '%s' in '%s' expects float, got %s -- skip\n",
				pf.name.c_str(), assetName, pf.value.jsonTypeName.c_str());
			++(*fieldsSkipped);
			return false;
		}
		const float f = static_cast<float>(pf.value.numVal);
		*reinterpret_cast<float*>(dst) = f;
		if (sdk_settings_disk_verbose.GetBool())
			Msg(eDLL_T::RTECH,
				"[SETTINGS-DISK] write '%s'.%s type=float val=%f\n",
				assetName, pf.name.c_str(), f);
		break;
	}
	case SDT_STRING:
	case SDT_ASSET:
	case SDT_ASSET2:
	{
		if (!pf.value.isString)
		{
			Warning(eDLL_T::RTECH,
				"[SETTINGS-DISK] field '%s' in '%s' expects string/asset, got %s -- skip\n",
				pf.name.c_str(), assetName, pf.value.jsonTypeName.c_str());
			++(*fieldsSkipped);
			return false;
		}
		char* heapStr = SettingsDisk_StrDup(pf.value.strVal.c_str());
		if (!heapStr)
		{
			Warning(eDLL_T::RTECH,
				"[SETTINGS-DISK] strdup failed for field '%s' in '%s' -- skip\n",
				pf.name.c_str(), assetName);
			++(*fieldsSkipped);
			return false;
		}
		*reinterpret_cast<const char**>(dst) = heapStr;
		if (sdk_settings_disk_verbose.GetBool())
			Msg(eDLL_T::RTECH,
				"[SETTINGS-DISK] write '%s'.%s type=string/asset val='%s'\n",
				assetName, pf.name.c_str(), heapStr);
		break;
	}
	default:
		Warning(eDLL_T::RTECH,
			"[SETTINGS-DISK] unknown dataType %u for field '%s' in '%s' -- skip\n",
			static_cast<unsigned>(dataType), pf.name.c_str(), assetName);
		++(*fieldsSkipped);
		return false;
	}

	++(*fieldsWritten);
	return true;
}

//-----------------------------------------------------------------------------
// Phase 2: clone donor under s_buildMutex. false = donor not resident yet.
//-----------------------------------------------------------------------------
static bool SettingsDisk_TryBuildEntry(
	PendingEntry& entry,
	int* pFieldsWritten,
	int* pFieldsSkipped)
{
	const PakGuid_t donorGuid = Pak_StringToGuid(entry.donorName.c_str());
	void* donorVoid = SettingsDisk_FindAsset(donorGuid);
	if (!donorVoid)
		return false;

	const DonorSnap snap = SettingsDisk_ReadDonor(donorVoid);
	if (!snap.ok)
		return false;

	if (!entry.layoutWarned && !entry.layoutAsset.empty() && snap.layoutName
		&& _stricmp(entry.layoutAsset.c_str(), snap.layoutName) != 0)
	{
		Warning(eDLL_T::RTECH,
			"[SETTINGS-DISK] layoutAsset mismatch for '%s': json='%s' donor='%s' -- using donor\n",
			entry.assetName.c_str(), entry.layoutAsset.c_str(), snap.layoutName);
	}
	entry.layoutWarned = true;

	char* dataBlock = new (std::nothrow) char[snap.dataSize];
	if (!dataBlock)
		return false;
	if (!SettingsDisk_SafeMemcpyData(dataBlock, snap.data, snap.dataSize))
	{
		delete[] dataBlock;
		return false;
	}

	char* nameDup = SettingsDisk_StrDup(entry.assetName.c_str());
	if (!nameDup)
	{
		delete[] dataBlock;
		return false;
	}

#if defined(CLIENT_DLL)
	// Packed uniqueId collision: packed owner wins. Do not re-derive the phase-1 id.
	if (v_Settings_GetSettingsHeaderForUniqueId_S21)
	{
		void* packedOwner = v_Settings_GetSettingsHeaderForUniqueId_S21(entry.uniqueId);
		if (packedOwner)
		{
			if (!entry.uidPackedWarned)
			{
				Warning(eDLL_T::RTECH,
					"[SETTINGS-DISK] uniqueId %u for '%s' collides with a packed asset -- "
					"skipping disk override\n",
					entry.uniqueId, entry.assetName.c_str());
				entry.uidPackedWarned = true;
			}
			free(nameDup);
			delete[] dataBlock;
			return false;
		}
	}
#endif // CLIENT_DLL

	SettingsHeader* hdr = new (std::nothrow) SettingsHeader();
	if (!hdr)
	{
		free(nameDup);
		delete[] dataBlock;
		return false;
	}
	memset(hdr, 0, sizeof(*hdr));
	hdr->layoutHeader = snap.layoutHeader;
	hdr->data         = dataBlock;
	hdr->name         = nameDup;
	hdr->strings      = snap.strings;
#if defined(CLIENT_DLL)
	hdr->assetRefs    = snap.assetRefs;
#endif // CLIENT_DLL
	hdr->uniqueId     = entry.uniqueId;
	hdr->dataSize     = snap.dataSize;
	// mods/modOps stay zero.

	int fieldsWritten = 0;
	int fieldsSkipped = 0;
	for (const PendingField& pf : entry.fields)
	{
		SettingsDisk_WriteField(dataBlock, snap.dataSize, snap.layoutHeader, pf,
			entry.assetName.c_str(), &fieldsWritten, &fieldsSkipped);
	}

	if (pFieldsWritten)
		*pFieldsWritten = fieldsWritten;
	if (pFieldsSkipped)
		*pFieldsSkipped = fieldsSkipped;

#if defined(CLIENT_DLL)
	Msg(eDLL_T::RTECH,
		"[SETTINGS-DISK] built '%s' donor='%s' layout='%s' uniqueId=%u written=%d skipped=%d (attempt %d)\n",
		entry.assetName.c_str(), entry.donorName.c_str(),
		snap.layoutName ? snap.layoutName : "?",
		entry.uniqueId, fieldsWritten, fieldsSkipped, entry.buildAttempts);
#endif // CLIENT_DLL

	entry.header = hdr;
	return true;
}

//-----------------------------------------------------------------------------
// Phase 1: parse one JSON into a PendingEntry. Deep-copy strings before the buffer is freed.
//-----------------------------------------------------------------------------
static bool SettingsDisk_ParseJsonFile(const char* jsonPath, const char* assetName, PendingEntry& outEntry)
{
	const HANDLE hFile = CreateFileA(jsonPath, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		Warning(eDLL_T::RTECH, "[SETTINGS-DISK] cannot open '%s'\n", jsonPath);
		return false;
	}

	const DWORD fileSize = GetFileSize(hFile, nullptr);
	if (fileSize == INVALID_FILE_SIZE || fileSize == 0
		|| static_cast<size_t>(fileSize) > SETTINGS_DISK_MAX_FILE_SIZE)
	{
		Warning(eDLL_T::RTECH, "[SETTINGS-DISK] bad size for '%s' (%u)\n", jsonPath, fileSize);
		CloseHandle(hFile);
		return false;
	}

	char* buf = static_cast<char*>(malloc(static_cast<size_t>(fileSize) + 1));
	if (!buf)
	{
		CloseHandle(hFile);
		return false;
	}

	DWORD nRead = 0;
	const BOOL readOk = ReadFile(hFile, buf, fileSize, &nRead, nullptr);
	CloseHandle(hFile);
	if (!readOk || nRead != fileSize)
	{
		Warning(eDLL_T::RTECH, "[SETTINGS-DISK] read failed for '%s'\n", jsonPath);
		free(buf);
		return false;
	}
	buf[fileSize] = '\0';

	rapidjson::Document doc;
	doc.ParseInsitu(buf);
	if (doc.HasParseError() || !doc.IsObject())
	{
		Warning(eDLL_T::RTECH, "[SETTINGS-DISK] JSON parse failed for '%s'\n", jsonPath);
		free(buf);
		return false;
	}

	std::string donorName;
	if (!JSON_GetValue(doc, "_donor", donorName) || donorName.empty())
	{
		Warning(eDLL_T::RTECH,
			"[SETTINGS-DISK] missing required '_donor' in '%s' -- skip\n", jsonPath);
		free(buf);
		return false;
	}

	if (!doc.HasMember("settings") || !doc["settings"].IsObject())
	{
		Warning(eDLL_T::RTECH,
			"[SETTINGS-DISK] missing required 'settings' object in '%s' -- skip\n", jsonPath);
		free(buf);
		return false;
	}

	outEntry.assetName = assetName;
	outEntry.donorName = donorName;
	outEntry.jsonPath  = jsonPath;
	JSON_GetValue(doc, "layoutAsset", outEntry.layoutAsset);

	if (doc.HasMember("uniqueId") && doc["uniqueId"].IsUint())
	{
		outEntry.preferredUid = doc["uniqueId"].GetUint();
		outEntry.hasPreferredUid = true;
	}
	else if (doc.HasMember("uniqueId") && doc["uniqueId"].IsInt() && doc["uniqueId"].GetInt() >= 0)
	{
		outEntry.preferredUid = static_cast<uint32_t>(doc["uniqueId"].GetInt());
		outEntry.hasPreferredUid = true;
	}

	const rapidjson::Value& settingsObj = doc["settings"];
	int fieldsSeen = 0;
	for (auto it = settingsObj.MemberBegin(); it != settingsObj.MemberEnd(); ++it)
	{
		if (fieldsSeen >= SETTINGS_DISK_MAX_FIELDS)
		{
			Warning(eDLL_T::RTECH,
				"[SETTINGS-DISK] field cap %d hit for '%s' -- remaining ignored\n",
				SETTINGS_DISK_MAX_FIELDS, assetName);
			break;
		}
		++fieldsSeen;

		if (!it->name.IsString())
			continue;

		PendingField pf;
		pf.name = it->name.GetString();
		SettingsDisk_CaptureFieldValue(it->value, pf.value);
		outEntry.fields.push_back(std::move(pf));
	}

	free(buf);
	return true;
}

//-----------------------------------------------------------------------------
static void SettingsDisk_MakeAssetName(const char* relPathNoExt, char* out, size_t outSize)
{
	// Scan uses backslashes; convert + prefix.
	V_snprintf(out, outSize, "settings/%s.rpak", relPathNoExt);
	for (char* p = out; *p; ++p)
	{
		if (*p == '\\')
			*p = '/';
		else if (*p >= 'A' && *p <= 'Z')
			*p = static_cast<char>(*p - 'A' + 'a');
	}
}

//-----------------------------------------------------------------------------
struct ScanState
{
	std::unordered_map<uint64_t, PendingEntry> pendingByGuid;
	std::unordered_map<uint32_t, std::string>  uidUsedByDisk;
	int  filesFound = 0;
	int  failed = 0;
	bool fileCapWarned = false;
	bool depthCapWarned = false;
};

static void SettingsDisk_ScanDir(ScanState& st, const char* relDir, int depth);

static void SettingsDisk_TryLoadFile(ScanState& st, const char* relPathWithJson)
{
	if (static_cast<size_t>(st.filesFound) >= SETTINGS_DISK_MAX_FILES)
	{
		if (!st.fileCapWarned)
		{
			Warning(eDLL_T::RTECH,
				"[SETTINGS-DISK] max file count %zu exceeded -- further files ignored\n",
				SETTINGS_DISK_MAX_FILES);
			st.fileCapWarned = true;
		}
		return;
	}
	++st.filesFound;

	// Strip .json.
	char relNoExt[SETTINGS_DISK_MAX_PATH_LEN];
	V_strncpy(relNoExt, relPathWithJson, sizeof(relNoExt));
	const size_t len = strlen(relNoExt);
	if (len > 5 && _stricmp(relNoExt + len - 5, ".json") == 0)
		relNoExt[len - 5] = '\0';
	else
	{
		Warning(eDLL_T::RTECH, "[SETTINGS-DISK] unexpected name '%s' -- skip\n", relPathWithJson);
		++st.failed;
		return;
	}

	char assetName[SETTINGS_DISK_MAX_PATH_LEN * 2];
	SettingsDisk_MakeAssetName(relNoExt, assetName, sizeof(assetName));

	const PakGuid_t guid = Pak_StringToGuid(assetName);
	if (st.pendingByGuid.find(guid) != st.pendingByGuid.end())
	{
		Warning(eDLL_T::RTECH,
			"[SETTINGS-DISK] duplicate guid for '%s' -- first wins, skip\n", assetName);
		++st.failed;
		return;
	}

	char fullPath[SETTINGS_DISK_MAX_PATH_LEN * 2];
	V_snprintf(fullPath, sizeof(fullPath), "%s%s", s_settingsDiskBasePath, relPathWithJson);

	PendingEntry entry;
	entry.guid = guid;
	if (!SettingsDisk_ParseJsonFile(fullPath, assetName, entry))
	{
		++st.failed;
		return;
	}

	entry.uniqueId = SettingsDisk_AllocUniqueId(
		entry.preferredUid, entry.hasPreferredUid, assetName, st.uidUsedByDisk);
	st.uidUsedByDisk.emplace(entry.uniqueId, assetName);

	st.pendingByGuid.emplace(guid, std::move(entry));
}

static void SettingsDisk_ScanDir(ScanState& st, const char* relDir, int depth)
{
	if (depth > SETTINGS_DISK_MAX_DEPTH)
	{
		if (!st.depthCapWarned)
		{
			Warning(eDLL_T::RTECH,
				"[SETTINGS-DISK] max recursion depth %d exceeded -- further dirs ignored\n",
				SETTINGS_DISK_MAX_DEPTH);
			st.depthCapWarned = true;
		}
		return;
	}

	char searchPath[SETTINGS_DISK_MAX_PATH_LEN * 2];
	if (relDir && relDir[0])
		V_snprintf(searchPath, sizeof(searchPath), "%s%s\\*", s_settingsDiskBasePath, relDir);
	else
		V_snprintf(searchPath, sizeof(searchPath), "%s*", s_settingsDiskBasePath);

	WIN32_FIND_DATAA fd;
	const HANDLE hFind = FindFirstFileA(searchPath, &fd);
	if (hFind == INVALID_HANDLE_VALUE)
		return;

	do
	{
		if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
			continue;

		if (!SettingsDisk_IsSafeComponent(fd.cFileName))
		{
			Warning(eDLL_T::RTECH,
				"[SETTINGS-DISK] rejected path component '%s' under '%s'\n",
				fd.cFileName, relDir && relDir[0] ? relDir : ".");
			continue;
		}

		char childRel[SETTINGS_DISK_MAX_PATH_LEN];
		if (relDir && relDir[0])
			V_snprintf(childRel, sizeof(childRel), "%s\\%s", relDir, fd.cFileName);
		else
			V_strncpy(childRel, fd.cFileName, sizeof(childRel));

		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			SettingsDisk_ScanDir(st, childRel, depth + 1);
			continue;
		}

		const size_t n = strlen(fd.cFileName);
		if (n > 5 && _stricmp(fd.cFileName + n - 5, ".json") == 0)
			SettingsDisk_TryLoadFile(st, childRel);
	}
	while (FindNextFileA(hFind, &fd));

	FindClose(hFind);
}

//-----------------------------------------------------------------------------
// Phase 1 once: publish pending maps. No pak calls.
//-----------------------------------------------------------------------------
static void SettingsDisk_ParseAll(void)
{
	ScanState st;
	SettingsDisk_ScanDir(st, "", 0);

	s_pendingByGuid = std::move(st.pendingByGuid);

	s_guidIndex.clear();
	s_guidIndex.reserve(s_pendingByGuid.size());
	s_uidToGuid.clear();
	s_uidToGuid.reserve(s_pendingByGuid.size());
	for (const auto& kv : s_pendingByGuid)
	{
		s_guidIndex.insert(kv.first);
		s_uidToGuid.emplace(kv.second.uniqueId, kv.first);
	}

#if !defined(CLIENT_DLL)
	s_pendingRemaining.store(static_cast<int>(s_pendingByGuid.size()), std::memory_order_relaxed);
#endif // !CLIENT_DLL

	if (st.failed > 0)
	{
		Warning(eDLL_T::RTECH,
			"[SETTINGS-DISK] scan complete: %d file(s) found, %zu parsed, %d failed\n",
			st.filesFound, s_pendingByGuid.size(), st.failed);
	}
}

static void SettingsDisk_EnsureParsed(void)
{
	if (!s_patternsReady)
		return;
	std::call_once(s_parseOnce, SettingsDisk_ParseAll);
}

#if !defined(CLIENT_DLL)
//-----------------------------------------------------------------------------
// Phase 2: build+insert when donor is resident. Miss does not consume retry budget.
//-----------------------------------------------------------------------------
static void SettingsDisk_TryBuildPending(void)
{
	std::lock_guard<std::mutex> lk(s_buildMutex);

	for (auto& kv : s_pendingByGuid)
	{
		PendingEntry& entry = kv.second;
		if (entry.built)
			continue;

		if (entry.buildAttempts >= SETTINGS_DISK_MAX_BUILD_RETRY)
		{
			if (!entry.giveUpWarned)
			{
				Warning(eDLL_T::RTECH,
					"[SETTINGS-DISK] donor '%s' for '%s' never became available after %d attempt(s) -- giving up\n",
					entry.donorName.c_str(), entry.assetName.c_str(), entry.buildAttempts);
				entry.giveUpWarned = true;
			}
			continue;
		}

		int fieldsWritten = 0;
		int fieldsSkipped = 0;

		// Already cloned; skip rebuild.
		if (!entry.header)
		{
			const PakGuid_t donorGuid = Pak_StringToGuid(entry.donorName.c_str());
			if (!SettingsDisk_FindAsset(donorGuid))
				continue;

			if (!SettingsDisk_TryBuildEntry(entry, &fieldsWritten, &fieldsSkipped))
			{
				// Build failure consumes budget.
				++entry.buildAttempts;
				continue;
			}
		}

		int slotIndex = -1;
		if (!SettingsDisk_InsertAsset(entry.guid, entry.header, &slotIndex))
		{
			++entry.buildAttempts;
			continue;
		}

		entry.built = true;
		entry.slotIndex = slotIndex;
		s_pendingRemaining.fetch_sub(1, std::memory_order_relaxed);

		const char* layoutName = "?";
		if (entry.header && entry.header->layoutHeader && entry.header->layoutHeader->name)
			layoutName = entry.header->layoutHeader->name;

		Msg(eDLL_T::RTECH,
			"[SETTINGS-DISK] built '%s' donor='%s' layout='%s' uniqueId=%u written=%d skipped=%d (attempt %d) slot=%d\n",
			entry.assetName.c_str(), entry.donorName.c_str(), layoutName,
			entry.uniqueId, fieldsWritten, fieldsSkipped, entry.buildAttempts,
			entry.slotIndex);
	}
}

static __int64 __fastcall Hook_Pak_AssetPublish_S3(void* a1, __int64 a2)
{
	const __int64 result = v_Pak_AssetPublish_S3(a1, a2);

	if (sdk_settings_disk.GetBool() && s_patternsReady)
	{
		SettingsDisk_EnsureParsed();
		if (s_pendingRemaining.load(std::memory_order_relaxed) > 0)
			SettingsDisk_TryBuildPending();
	}
	return result;
}

static void SettingsDisk_Status_f(const CCommand& args)
{
	(void)args;

	if (!s_patternsReady)
	{
		Msg(eDLL_T::RTECH, "[SETTINGS-DISK] status: patterns not ready\n");
		return;
	}

	SettingsDisk_EnsureParsed();

	std::lock_guard<std::mutex> lk(s_buildMutex);
	Msg(eDLL_T::RTECH,
		"[SETTINGS-DISK] status: remaining=%d entries=%zu registry=0x%llX\n",
		s_pendingRemaining.load(std::memory_order_relaxed),
		s_pendingByGuid.size(),
		static_cast<unsigned long long>(g_pPakAssetTypeRegistry_S3));

	for (const auto& kv : s_pendingByGuid)
	{
		const PendingEntry& e = kv.second;
		if (e.built)
		{
			Msg(eDLL_T::RTECH,
				"[SETTINGS-DISK]   '%s' donor='%s' uniqueId=%u built=1 attempts=%d slot=%d header=%p\n",
				e.assetName.c_str(), e.donorName.c_str(), e.uniqueId,
				e.buildAttempts, e.slotIndex, static_cast<void*>(e.header));
		}
		else
		{
			Msg(eDLL_T::RTECH,
				"[SETTINGS-DISK]   '%s' donor='%s' uniqueId=%u built=0 attempts=%d\n",
				e.assetName.c_str(), e.donorName.c_str(), e.uniqueId, e.buildAttempts);
		}
	}
}

static ConCommand sdk_settings_disk_status(
	"sdk_settings_disk_status", SettingsDisk_Status_f,
	"Dump settings_disk pending entries (name, donor, uniqueId, built, attempts, slot, header).",
	FCVAR_RELEASE);
#endif // !CLIENT_DLL

#if defined(CLIENT_DLL)
//-----------------------------------------------------------------------------
// Purpose: run the engine's own guid lookup over a "type asset ref own|ext" list
// (platform/<file>) and name every ref the engine cannot find. [PAK-REF-PROBE]
//-----------------------------------------------------------------------------
static void PakRefProbe_f(const CCommand& args)
{
	const char* const file = args.ArgC() > 1 ? args.Arg(1) : "akimbo_refs.txt";
	char path[MAX_PATH];
	V_snprintf(path, sizeof(path), "platform/%s", file);
	FILE* const f = fopen(path, "r");
	if (!f)
	{
		Warning(eDLL_T::RTECH, "[PAK-REF-PROBE] cannot open %s\n", path);
		return;
	}

	char line[256];
	int total = 0, missing = 0;
	while (fgets(line, sizeof(line), f))
	{
		char type[16];
		unsigned long long asset = 0, ref = 0;
		char kind[8] = {};
		if (sscanf(line, "%15s %llx %llx %7s", type, &asset, &ref, kind) < 3)
			continue;
		++total;
		if (!v_Pak_FindAssetVoid_S21(ref, nullptr))
		{
			++missing;
			Warning(eDLL_T::RTECH, "[PAK-REF-PROBE] %s 0x%016llX -> MISSING 0x%016llX (%s)\n",
				type, asset, ref, kind);
		}
	}
	fclose(f);
	Msg(eDLL_T::RTECH, "[PAK-REF-PROBE] %d refs, %d missing\n", total, missing);
}

static ConCommand sdk_pak_ref_probe("sdk_pak_ref_probe", PakRefProbe_f,
	"Run the engine guid lookup over platform/<file> (default akimbo_refs.txt) and list unresolvable refs.",
	FCVAR_DEVELOPMENTONLY);

//-----------------------------------------------------------------------------
// Fast path: native then lock-free index miss. Slow path: s_buildMutex, then retry native.
//-----------------------------------------------------------------------------
static void* __fastcall Hook_Pak_FindAssetVoid_S21(uint64_t guid, uint32_t* handleOut)
{
	if (!v_Pak_FindAssetVoid_S21)
		return nullptr;

	if (!sdk_settings_disk.GetBool() || !s_patternsReady)
		return v_Pak_FindAssetVoid_S21(guid, handleOut);

	SettingsDisk_EnsureParsed();

	void* native = v_Pak_FindAssetVoid_S21(guid, handleOut);
	if (native)
		return native;

	if (s_guidIndex.find(guid) == s_guidIndex.end())
		return native;

	std::lock_guard<std::mutex> lk(s_buildMutex);
	const auto it = s_pendingByGuid.find(guid);
	if (it == s_pendingByGuid.end())
		return native;

	PendingEntry& entry = it->second;
	if (entry.built)
		return entry.header;

	if (entry.buildAttempts >= SETTINGS_DISK_MAX_BUILD_RETRY)
	{
		if (!entry.giveUpWarned)
		{
			Warning(eDLL_T::RTECH,
				"[SETTINGS-DISK] donor '%s' for '%s' never became available after %d attempt(s) -- giving up\n",
				entry.donorName.c_str(), entry.assetName.c_str(), entry.buildAttempts);
			entry.giveUpWarned = true;
		}
		return native;
	}

	++entry.buildAttempts;
	if (SettingsDisk_TryBuildEntry(entry, nullptr, nullptr))
	{
		entry.built = true;
		return entry.header;
	}

	return native;
}

static void* __fastcall Hook_Settings_GetSettingsHeaderForUniqueId_S21(uint32_t uniqueId)
{
	if (!v_Settings_GetSettingsHeaderForUniqueId_S21)
		return nullptr;

	if (!sdk_settings_disk.GetBool() || !s_patternsReady)
		return v_Settings_GetSettingsHeaderForUniqueId_S21(uniqueId);

	SettingsDisk_EnsureParsed();

	void* native = v_Settings_GetSettingsHeaderForUniqueId_S21(uniqueId);
	if (native)
		return native;

	const auto uidIt = s_uidToGuid.find(uniqueId);
	if (uidIt == s_uidToGuid.end())
		return native;
	const uint64_t guid = uidIt->second;

	std::lock_guard<std::mutex> lk(s_buildMutex);
	const auto it = s_pendingByGuid.find(guid);
	if (it == s_pendingByGuid.end())
		return native;

	PendingEntry& entry = it->second;
	if (entry.built)
		return entry.header;

	if (entry.buildAttempts >= SETTINGS_DISK_MAX_BUILD_RETRY)
	{
		if (!entry.giveUpWarned)
		{
			Warning(eDLL_T::RTECH,
				"[SETTINGS-DISK] donor '%s' for '%s' never became available after %d attempt(s) -- giving up\n",
				entry.donorName.c_str(), entry.assetName.c_str(), entry.buildAttempts);
			entry.giveUpWarned = true;
		}
		return native;
	}

	++entry.buildAttempts;
	if (SettingsDisk_TryBuildEntry(entry, nullptr, nullptr))
	{
		entry.built = true;
		return entry.header;
	}

	return native;
}
#endif // CLIENT_DLL

//=============================================================================
// Detour class
//=============================================================================
void VSettingsDiskS21::GetFun(void) const
{
	s_patternsReady = false;

#if defined(CLIENT_DLL)
	// Pak_FindAssetVoid
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 20 "
		// Trailing TLS slot imm differs per image (0x40 / 0x5C).
		"65 48 8B 04 25 58 00 00 00 4C 8D 2D ?? ?? ?? ?? 33 ED 48 8B F2 4C 8B E1 4C 8B 38 "
		"B8 ?? 00 00 00")
		.GetPtr(v_Pak_FindAssetVoid_S21);

	// Settings_GetSettingsHeaderForUniqueId
	Module_FindPattern(g_GameDll,
		"4C 8B DC 48 83 EC 78 49 8D 43 A8 89 4C 24 48 49 8D 53 10 49 89 43 10 E8")
		.GetPtr(v_Settings_GetSettingsHeaderForUniqueId_S21);

	if (!v_Pak_FindAssetVoid_S21)
		Warning(eDLL_T::RTECH,
			"[SETTINGS-DISK] Pak_FindAssetVoid pattern unresolved -- feature disabled\n");
	if (!v_Settings_GetSettingsHeaderForUniqueId_S21)
		Warning(eDLL_T::RTECH,
			"[SETTINGS-DISK] Settings_GetSettingsHeaderForUniqueId pattern unresolved -- feature disabled\n");

	s_patternsReady = (v_Pak_FindAssetVoid_S21 != nullptr
		&& v_Settings_GetSettingsHeaderForUniqueId_S21 != nullptr);

	if (!s_patternsReady)
	{
		Warning(eDLL_T::RTECH,
			"[SETTINGS-DISK] Feature fully disabled (pattern resolve failed)\n");
	}
#else
	// Asset-TYPE registry (lea at +14); slots at base+0x1000. GetPtr(), not GetPtr(T*&).
	g_pPakAssetTypeRegistry_S3 = Module_FindPattern(g_GameDll,
		"B8 FF FF FF FF 48 83 C4 20 5B C3 41 8B C8 4C 8D 0D ?? ?? ?? ?? "
		"81 E1 FF FF 03 00 8B C1 48 C1 E0 05 4A 8B 94 08 00 10 00 00")
		.Offset(14)
		.ResolveRelativeAddress(3, 7)
		.GetPtr();

	// Asset-publish walk entry.
	Module_FindPattern(g_GameDll,
		"40 56 57 41 55 41 57 48 83 EC 38 45 33 ED 4C 8B FA 41 8B FD 48 8B F1 "
		"39 7A 3C 0F 84 ?? ?? ?? ?? 48 89 5C 24 60 4C 89 64 24 70 4C 8D 25 ?? ?? ?? ??")
		.GetPtr(v_Pak_AssetPublish_S3);

	if (!g_pPakAssetTypeRegistry_S3)
		Warning(eDLL_T::RTECH,
			"[SETTINGS-DISK] pak asset type registry pattern unresolved -- feature disabled\n");
	if (!v_Pak_AssetPublish_S3)
		Warning(eDLL_T::RTECH,
			"[SETTINGS-DISK] Pak_AssetPublish pattern unresolved -- feature disabled\n");

	s_patternsReady = (g_pPakAssetTypeRegistry_S3 != 0 && v_Pak_AssetPublish_S3 != nullptr);

	if (!s_patternsReady)
	{
		Warning(eDLL_T::RTECH,
			"[SETTINGS-DISK] Feature fully disabled (pattern resolve failed)\n");
	}
#endif // CLIENT_DLL
}

void VSettingsDiskS21::Detour(const bool bAttach) const
{
	// FileSystem/pak tables are not ready at attach time.
	if (!s_patternsReady)
		return;

#if defined(CLIENT_DLL)
	DetourSetup(&v_Pak_FindAssetVoid_S21, &Hook_Pak_FindAssetVoid_S21, bAttach);
	DetourSetup(&v_Settings_GetSettingsHeaderForUniqueId_S21,
		&Hook_Settings_GetSettingsHeaderForUniqueId_S21, bAttach);
#else
	DetourSetup(&v_Pak_AssetPublish_S3, &Hook_Pak_AssetPublish_S3, bAttach);
#endif // CLIENT_DLL
}
