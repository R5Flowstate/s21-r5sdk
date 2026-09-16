//=============================================================================//
//
// Purpose: Dedicated-server zipline validation shim.
// Guard a null lookup into the native validator; rebuild GUID chains at Activate.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier0/commandline.h"
#include "tier1/cvar.h"
#include "public/tier0/memaddr.h"
#include "game/shared/dt_extend.h"
#include "game/shared/edict_dirty.h"
#include "const.h"
#include "game/shared/basehandle.h"
#include "game/server/entitylist.h"
#include "game/server/baseentity.h"
#include "zipline_validation.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cfloat>

// Default-on (compiled sdk_ziprail_enable 1). CheckParm wins so
// +sdk_ziprail_enable 0 still parks at DetourAttach; no arg falls through
// to the ConVar default. Must match Ziprail_PromoteRequestedAtLaunch.
static bool ZiprailDedi_IsLive(void)
{
	const char* val = nullptr;
	if (CommandLine()->CheckParm("+sdk_ziprail_enable", &val))
		return !val || val[0] != '0';
	return DTExtend_IsZiprailPromoteEnabled();
}

static void* (*v_ZiplineEntityValidationDedi)(void* ent) = nullptr;
static void* (*v_ZiplineEntityLookupDedi)(int64_t a1, int64_t a2, int64_t a3, void* a4) = nullptr;
static char (*v_BaseEntityKeyValueDedi)(uintptr_t ent, const char* key, const char* value) = nullptr;
static char (*v_ZiplineKeyValueDedi)(uintptr_t ent, const char* key, const char* value) = nullptr;
static char (*v_ZiplineRebuildDedi)(uintptr_t ent) = nullptr;
static char (*v_ZiplineActivateDedi)(uintptr_t ent) = nullptr;
// CZiplinePhysics::Init(physics, points, count, ownerEntity). physics = ent + 0xB10.
// Resolved but only called manually as a fallback when native Activate's own partner-
// entity resolution fails (chain starts never resolve through the native link system).
static int64_t (*v_CZiplinePhysicsInitDedi)(void* physics, const void* points,
	unsigned int count, void* ownerEntity) = nullptr;
// Publishes the ride polyline the engine actually rides (count mirror, positions,
// per-segment distances). Resolved but only called manually after Physics::Init.
static void (*v_ZiplinePublishRopeShapeDedi)(uintptr_t ent) = nullptr;
// Token-scan for "link_to_guid_0". Native demotes a "zipline" with none to "zipline_end".
static char (*v_ZiplineBlockHasLinkToDedi)(const unsigned char* blockText) = nullptr;

static std::atomic<uint32_t> s_nullValidationCount{ 0 };
static std::atomic<uint32_t> s_ziprailRecordOverflowCount{ 0 };
static std::atomic<uint32_t> s_ziprailStartKeyCount{ 0 };
static std::atomic<uint32_t> s_ziprailChainWalkFailLogs{ 0 };
static std::atomic<uint32_t> s_ziprailChainAppliedLogs{ 0 };
static std::atomic<uint32_t> s_ziprailChainRegistryOverflowCount{ 0 };
static std::atomic<uint32_t> s_envmapVolumeNeutralizeCount{ 0 };
static std::atomic<uint32_t> s_ziprailStartWipeLogs{ 0 };
static std::atomic<uint32_t> s_ziplineSpawnHookCount{ 0 };
static std::atomic<uint32_t> s_ziplineRebuildHookCount{ 0 };
static std::atomic<uint32_t> s_zipSpawnPartnerNudgeCount{ 0 };
// CZipline vtable pointer, captured off the first CZipline::KeyValue call. Used by
// the SV_ActivateServer materialize sweep as a liveness check before touching a
// meta-table entity pointer recorded seconds earlier at map parse time.
static std::atomic<uintptr_t> s_ziplineVtable{ 0 };
static std::atomic<uint32_t> s_ziprailKeepClassnameLogs{ 0 };
static std::atomic<uint32_t> s_ziprailSpawnLogs{ 0 };

struct ZiprailVec3
{
	float x;
	float y;
	float z;
};

// Classname discriminator recorded off the "classname" KeyValue so the chain walker
// can prefer a script_mover_train_node when multiple recorded entities happen to link
// to the same GUID (ambiguous reverse-link ties). See Ziprail_ClassnameTagFor.
enum class ZiprailClassTag : uint8_t
{
	kNone = 0,
	kZipline,
	kZiplineEnd,
	kTrainNode,
};

struct ZiprailEntityMeta
{
	// Serial-validated EHANDLE dword (entity+0x08). 0 = empty slot.
	// Never store naked entity pointers across parse/sweep -- addresses recycle.
	uint32_t handle;
	uint64_t guid;
	uint64_t linkToGuid;
	ZiprailVec3 origin;
	int numSmoothPoints;
	int tangentType;
	int useAutoDetachSpeed;
	float mountReverseDistance;
	float autoDetachDistance;
	float speedScale;
	float width;
	float fadeDist;
	float lengthScale;
	ZiprailClassTag classTag;
	bool dropToBottom;
	bool detachEndOnUse;
	bool detachEndOnSpawn;
	bool vertical;
	bool pushOffX;
	bool preserveVelocity;
	bool preventManualDetach;
	bool hasGuid;
	bool hasLinkToGuid;
	bool hasOrigin;
	bool hasNumSmoothPoints;
	bool hasTangentType;
	bool hasUseAutoDetachSpeed;
	bool hasZiprailAutoDetachSpeed;
	bool hasMountReverseDistance;
	bool hasAutoDetachDistance;
	bool hasSpeedScale;
	bool hasWidth;
	bool hasFadeDist;
	bool hasLengthScale;
	bool hasDropToBottom;
	bool hasDetachEndOnUse;
	bool hasDetachEndOnSpawn;
	bool hasVertical;
	bool hasPushOffX;
	bool hasPreserveVelocity;
	bool hasPreventManualDetach;
	bool isZiprailStart;
	bool classnameSeen;
};

static SRWLOCK s_ziprailMetaLock = SRWLOCK_INIT;
static ZiprailEntityMeta s_ziprailMeta[2048] = {};
static SRWLOCK s_envmapVolumeLock = SRWLOCK_INIT;
static uintptr_t s_envmapVolumeEnts[4096] = {};

// Append-only ledger of every entity that ever recorded isZiprailStart=1.
// Never wiped by recycling -- lets the sweep report per-start fate.
static SRWLOCK s_ziprailStartLedgerLock = SRWLOCK_INIT;
static uint32_t s_ziprailStartLedger[192] = {};
static uint32_t s_ziprailStartLedgerCount = 0;

// Chain registry keyed by start entity.
static constexpr int kZiprailMaxChainNodes = 32;
static constexpr int kZiprailMaxRestPoints = 16; // native CUtlVector inline cap; Activate purges >16
static constexpr int kZiprailBakedPointsMax = 192;
static constexpr float kZiprailDefaultMountReverseDist = 200.0f;

// Hermite derivative samples for ride direction / closest-point. Separate from
// the 16-cap rest polyline: joins keep BOTH segment endpoints so arc lookups
// never invent a direction across a node.
struct ZiprailBakedPath
{
	int   count;
	int   nodeStart[kZiprailWireMaxNodes];
	float pos[kZiprailBakedPointsMax][3];
	float dir[kZiprailBakedPointsMax][3];
	float dist[kZiprailBakedPointsMax];
};

struct ZiprailChain
{
	uint32_t startHandle;
	uint64_t startGuid;
	int nodeCount;
	float nodesX[kZiprailMaxChainNodes];
	float nodesY[kZiprailMaxChainNodes];
	float nodesZ[kZiprailMaxChainNodes];
	int numSmooth[kZiprailMaxChainNodes];
	int tangentType[kZiprailMaxChainNodes];
	int restCount;
	float rest[kZiprailMaxRestPoints][3];
	uint32_t farHandle;
	int useAutoDetachSpeed;
	float mountReverseDistance;
	float autoDetachDistance;
	float speedScale;
	float width;
	float fadeDist;
	float lengthScale;
	bool dropToBottom;
	bool detachEndOnUse;
	bool detachEndOnSpawn;
	bool vertical;
	bool pushOffX;
	bool preserveVelocity;
	bool preventManualDetach;
	bool hasAutoDetachDistance;
	bool hasSpeedScale;
	bool hasWidth;
	bool hasFadeDist;
	bool hasLengthScale;
	bool hasDropToBottom;
	bool hasDetachEndOnUse;
	bool hasDetachEndOnSpawn;
	bool hasVertical;
	bool hasPushOffX;
	bool hasPreserveVelocity;
	bool hasPreventManualDetach;
	int memberCount;
	uint32_t memberHandles[kZiprailMaxChainNodes];
	ZiprailClassTag memberTags[kZiprailMaxChainNodes];
	bool applied;
	ZiprailWireBlock wire;
	bool wireValid;
	ZiprailBakedPath baked;
};

static SRWLOCK s_ziprailChainLock = SRWLOCK_INIT;
static ZiprailChain s_ziprailChains[256] = {};

static bool Ziprail_StrEq(const char* a, const char* b)
{
	return a && b && _stricmp(a, b) == 0;
}

static bool Ziprail_StrStartsWith(const char* text, const char* prefix)
{
	if (!text || !prefix)
		return false;
	const size_t prefixLen = strlen(prefix);
	return _strnicmp(text, prefix, prefixLen) == 0;
}

static bool EnvmapVolume_IsClassname(const char* classname)
{
	return Ziprail_StrEq(classname, "envmap_volume");
}

static bool EnvmapVolume_IsTracked(uintptr_t ent)
{
	if (!ent)
		return false;

	AcquireSRWLockShared(&s_envmapVolumeLock);
	for (const uintptr_t trackedEnt : s_envmapVolumeEnts)
	{
		if (trackedEnt == ent)
		{
			ReleaseSRWLockShared(&s_envmapVolumeLock);
			return true;
		}
	}
	ReleaseSRWLockShared(&s_envmapVolumeLock);
	return false;
}

static void EnvmapVolume_Track(uintptr_t ent)
{
	if (!ent || EnvmapVolume_IsTracked(ent))
		return;

	AcquireSRWLockExclusive(&s_envmapVolumeLock);
	for (uintptr_t& trackedEnt : s_envmapVolumeEnts)
	{
		if (!trackedEnt)
		{
			trackedEnt = ent;
			break;
		}
	}
	ReleaseSRWLockExclusive(&s_envmapVolumeLock);
}

static void EnvmapVolume_NeutralizeNetworkModel(uintptr_t ent, const char* reason)
{
	if (!ent)
		return;

	// Keep the entity; networked model empty (brush model is not a studio mdl).
	uint16_t oldModelIndex = *reinterpret_cast<uint16_t*>(ent + 0xDE);
	*reinterpret_cast<uint16_t*>(ent + 0xDE) = 0; // CBaseEntity::m_nModelIndex

	MarkEntityEdictDirty(reinterpret_cast<void*>(ent));

	const uint32_t n = s_envmapVolumeNeutralizeCount.fetch_add(
		1, std::memory_order_relaxed);
	if (n < 64 || (n % 256) == 0)
	{
		Warning(eDLL_T::SERVER,
			"[S21-ENVMAP] neutralized envmap_volume network model #%u "
			"ent=0x%p oldModel=%u reason='%s'\n",
			n + 1, reinterpret_cast<void*>(ent), oldModelIndex,
			reason ? reason : "?");
	}
}

static bool Ziprail_IsCandidateClassname(const char* classname)
{
	return Ziprail_StrEq(classname, "zipline") ||
		Ziprail_StrEq(classname, "zipline_end") ||
		Ziprail_StrEq(classname, "script_mover_train_node");
}

static ZiprailClassTag Ziprail_ClassnameTagFor(const char* classname)
{
	if (Ziprail_StrEq(classname, "zipline"))
		return ZiprailClassTag::kZipline;
	if (Ziprail_StrEq(classname, "zipline_end"))
		return ZiprailClassTag::kZiplineEnd;
	if (Ziprail_StrEq(classname, "script_mover_train_node"))
		return ZiprailClassTag::kTrainNode;
	return ZiprailClassTag::kNone;
}

static bool Ziprail_ParseGuid(const char* value, uint64_t* out)
{
	if (!value || !out)
		return false;

	unsigned long long parsed = 0;
	if (sscanf_s(value, "%llx", &parsed) != 1)
		return false;

	*out = static_cast<uint64_t>(parsed);
	return true;
}

static bool Ziprail_ParseVec3(const char* value, ZiprailVec3* out)
{
	if (!value || !out)
		return false;

	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	if (sscanf_s(value, "%f %f %f", &x, &y, &z) != 3)
		return false;

	out->x = x;
	out->y = y;
	out->z = z;
	return true;
}

static bool Ziprail_ParseBool(const char* value)
{
	return value && atoi(value) != 0;
}

static int Ziprail_ParseInt(const char* value)
{
	return value ? atoi(value) : 0;
}

static float Ziprail_ParseFloat(const char* value)
{
	return value ? static_cast<float>(atof(value)) : 0.0f;
}


// EHANDLE capture; serial mismatch => null. No SEH on stale pointers.
static constexpr ptrdiff_t kZiprailEntOffRefEHandle = 0x08;
static constexpr ptrdiff_t kZiprailEntOffEdictIdx   = 0x58;
static constexpr ptrdiff_t kZiprailEntOffAbsOrigin  = 0x450;

// CZipline layout used by the Spawn partner-nudge path.
static constexpr ptrdiff_t kZipEntOffLocalOrigin      = 0x554; // m_vecOrigin (local)
static constexpr ptrdiff_t kZipEntOffEFlags           = 0x230; // bit 0x800 = abs dirty
static constexpr ptrdiff_t kZipEntOffSetupPointCount  = 0x1138; // _zipline_rest_point_N count
static constexpr ptrdiff_t kZipEntOffPhysInit         = 0xB18; // physics initialized
static constexpr ptrdiff_t kZipEntOffRopeNodeCount    = 0xD38;
static constexpr ptrdiff_t kZipEntOffLengthScale      = 0xD40; // m_springDistanceScale (ZiplineLengthScale)
static constexpr ptrdiff_t kZipEntOffNextZipline      = 0xE68; // m_nextZipline EHANDLE
static constexpr ptrdiff_t kZipEntOffDetachEndOnUse   = 0xE6C; // m_detachEndOnUse
static constexpr ptrdiff_t kZipEntOffDropToBottom     = 0xE6D; // m_dropToBottom
static constexpr ptrdiff_t kZipEntOffAutoDetachDist   = 0xE70; // m_ziplineAutoDetachDistance
static constexpr ptrdiff_t kZipEntOffVertPushOff      = 0xE74; // m_ziplineVerticalPushOffInDirectionX
static constexpr ptrdiff_t kZipEntOffVertPreserve     = 0xE75; // m_ziplineVerticalPreserveVelocity
static constexpr ptrdiff_t kZipEntOffWidth            = 0xE78; // m_ziplineWidth
static constexpr ptrdiff_t kZipEntOffZiplineEnabled   = 0xE7C; // m_ziplineEnabled; mount search gate
static constexpr ptrdiff_t kZipEntOffRestPositions    = 0xE80; // m_ziplineRestPositions float[3] x 16
static constexpr ptrdiff_t kZipEntOffRestCount        = 0xF40; // m_numZiplineRestPositions
static constexpr ptrdiff_t kZipEntOffFadeDist         = 0xF44; // m_ziplineFadeDist
static constexpr ptrdiff_t kZipEntOffSpeedScale       = 0xF48; // m_ziplineSpeedScale
static constexpr ptrdiff_t kZipEntOffDetachEndOnSpawn = 0xF4C; // DetachEndOnSpawn (not networked)
static constexpr ptrdiff_t kZipEntOffRopeShapeCount   = 0xF50; // published ride polyline count mirror
static constexpr ptrdiff_t kZipEntOffRidePositions    = 0xF54; // ride positions float[3] x count
static constexpr ptrdiff_t kZipEntOffSegmentDistances = 0x1014; // per-segment distances float x (count-1)
// 1/1024 -- exact binary float; square is a normal float strictly > 0 for distSq gate.
static constexpr float kZipSpawnPartnerNudgeZ = 0.0009765625f;
static constexpr int kZipEFlagsAbsDirty = 0x800;

static uint32_t Ziprail_CaptureHandle(uintptr_t ent)
{
	if (!ent)
		return INVALID_EHANDLE_INDEX;
	return *reinterpret_cast<const uint32_t*>(ent + kZiprailEntOffRefEHandle);
}

static uintptr_t Ziprail_ResolveHandle(uint32_t rawHandle)
{
	if (rawHandle == 0 || rawHandle == INVALID_EHANDLE_INDEX)
		return 0;
	if (!g_serverEntityList)
		return 0;
	const CBaseHandle h = CBaseHandle::UnsafeFromIndex(static_cast<int>(rawHandle));
	void* p = g_serverEntityList->LookupEntity(h);
	return reinterpret_cast<uintptr_t>(p);
}


static ZiprailEntityMeta* Ziprail_FindByHandleLocked(uint32_t handle)
{
	if (!handle || handle == INVALID_EHANDLE_INDEX)
		return nullptr;

	for (ZiprailEntityMeta& meta : s_ziprailMeta)
	{
		if (meta.handle == handle)
			return &meta;
	}

	return nullptr;
}

static ZiprailEntityMeta* Ziprail_FindByEntLocked(uintptr_t ent)
{
	if (!ent)
		return nullptr;
	return Ziprail_FindByHandleLocked(Ziprail_CaptureHandle(ent));
}

static ZiprailEntityMeta* Ziprail_FindOrCreateByEntLocked(uintptr_t ent)
{
	if (!ent)
		return nullptr;

	if (ZiprailEntityMeta* existing = Ziprail_FindByEntLocked(ent))
		return existing;

	const uint32_t handle = Ziprail_CaptureHandle(ent);
	if (!handle || handle == INVALID_EHANDLE_INDEX)
		return nullptr;

	for (ZiprailEntityMeta& meta : s_ziprailMeta)
	{
		if (!meta.handle)
		{
			memset(&meta, 0, sizeof(meta));
			meta.handle = handle;
			return &meta;
		}
	}

	const uint32_t n = s_ziprailRecordOverflowCount.fetch_add(
		1, std::memory_order_relaxed);
	if (n < 4 || (n % 128) == 0)
	{
		Warning(eDLL_T::SERVER,
			"[ZIPRAIL-DEDI] metadata table full while recording ent=0x%p "
			"(#%u)\n", reinterpret_cast<void*>(ent), n + 1);
	}
	return nullptr;
}

// Log every wipe of a start-flagged record (path + key).
static void Ziprail_LogStartWipeLocked(const ZiprailEntityMeta* meta,
	const char* reason, const char* key, const char* value)
{
	if (!meta || !meta->isZiprailStart)
		return;

	const uint32_t n = s_ziprailStartWipeLogs.fetch_add(1, std::memory_order_relaxed);
	if (n < 24 || (n % 128) == 0)
	{
		Warning(eDLL_T::SERVER,
			"[ZIPRAIL-DEDI] WIPE start-record #%u ent=0x%p guid=0x%llX reason=%s "
			"key='%s' value='%s'\n",
			n + 1, reinterpret_cast<void*>(Ziprail_ResolveHandle(meta->handle)),
			static_cast<unsigned long long>(meta->guid), reason,
			key ? key : "", value ? value : "");
	}
}

// "classname" terminates a block. Recycled entity address after classnameSeen wipes the stale record.
static bool Ziprail_RecordBaseKeyValue(uintptr_t ent, const char* key, const char* value)
{
	if (!ZiprailDedi_IsLive())
		return false;
	if (!ent || !key || !value)
		return false;

	const bool isOrigin = Ziprail_StrEq(key, "origin");
	const bool isGuid = Ziprail_StrEq(key, "link_guid");
	const bool isLinkToGuid = Ziprail_StrStartsWith(key, "link_to_guid_");
	const bool isNumSmoothPoints = Ziprail_StrEq(key, "num_smooth_points");
	const bool isTangentType = Ziprail_StrEq(key, "tangent_type");
	const bool isClassname = Ziprail_StrEq(key, "classname");
	if (!isOrigin && !isGuid && !isLinkToGuid &&
		!isNumSmoothPoints && !isTangentType && !isClassname)
		return false;

	AcquireSRWLockExclusive(&s_ziprailMetaLock);

	if (isClassname)
	{
		if (Ziprail_IsCandidateClassname(value))
		{
			ZiprailEntityMeta* meta = Ziprail_FindOrCreateByEntLocked(ent);
			if (meta)
			{
				if (meta->classnameSeen)
				{
					// Stale record at a recycled address -- wipe, keep the slot.
					Ziprail_LogStartWipeLocked(meta, "classname-candidate-stale", key, value);
					memset(meta, 0, sizeof(*meta));
					meta->handle = Ziprail_CaptureHandle(ent);
				}
				meta->classTag = Ziprail_ClassnameTagFor(value);
				meta->classnameSeen = true;
			}
		}
		else
		{
			// Not a ziprail-relevant classname -- free the slot if one was
			// accumulating for this ent (mirrors the block-terminator semantics).
			if (ZiprailEntityMeta* meta = Ziprail_FindByEntLocked(ent))
			{
				Ziprail_LogStartWipeLocked(meta, "classname-noncandidate", key, value);
				memset(meta, 0, sizeof(*meta));
			}
		}
		ReleaseSRWLockExclusive(&s_ziprailMetaLock);
		return true;
	}

	if (isOrigin)
	{
		if (ZiprailEntityMeta* meta = Ziprail_FindByEntLocked(ent))
		{
			if (meta->classnameSeen)
			{
				// Stale record at a recycled address -- wipe, keep the slot, then
				// record origin into the fresh record (may create a "partial"
				// record; acceptable, see header comment).
				Ziprail_LogStartWipeLocked(meta, "origin-stale", key, value);
				memset(meta, 0, sizeof(*meta));
				meta->handle = Ziprail_CaptureHandle(ent);
			}

			ZiprailVec3 origin = {};
			if (Ziprail_ParseVec3(value, &origin))
			{
				meta->origin = origin;
				meta->hasOrigin = true;
			}
		}
		ReleaseSRWLockExclusive(&s_ziprailMetaLock);
		return true;
	}

	// Remaining keys (link_guid / link_to_guid_* / num_smooth_points /
	// tangent_type) are creation keys.
	ZiprailEntityMeta* meta = Ziprail_FindOrCreateByEntLocked(ent);
	if (meta)
	{
		if (meta->classnameSeen)
		{
			// Stale record at a recycled address -- wipe, keep the slot.
			Ziprail_LogStartWipeLocked(meta, "datakey-stale", key, value);
			memset(meta, 0, sizeof(*meta));
			meta->handle = Ziprail_CaptureHandle(ent);
		}

		if (isGuid)
		{
			uint64_t guid = 0;
			if (Ziprail_ParseGuid(value, &guid))
			{
				for (ZiprailEntityMeta& other : s_ziprailMeta)
				{
					if (other.handle && other.handle != Ziprail_CaptureHandle(ent) && other.hasGuid &&
						other.guid == guid)
					{
						Ziprail_LogStartWipeLocked(&other, "guid-dedup", key, value);
						memset(&other, 0, sizeof(other));
					}
				}
				meta->guid = guid;
				meta->hasGuid = true;
			}
		}
		else if (isLinkToGuid)
		{
			uint64_t guid = 0;
			if (Ziprail_ParseGuid(value, &guid))
			{
				meta->linkToGuid = guid;
				meta->hasLinkToGuid = true;
			}
		}
		else if (isNumSmoothPoints)
		{
			meta->numSmoothPoints = Ziprail_ParseInt(value);
			meta->hasNumSmoothPoints = true;
		}
		else if (isTangentType)
		{
			meta->tangentType = Ziprail_ParseInt(value);
			meta->hasTangentType = true;
		}
	}
	ReleaseSRWLockExclusive(&s_ziprailMetaLock);
	return true;
}

static bool Ziprail_IsRecordedZiplineKey(const char* key)
{
	return Ziprail_StrEq(key, "isZiprailStart")
		|| Ziprail_StrEq(key, "useAutoDetachSpeed")
		|| Ziprail_StrEq(key, "useZiprailAutoDetachSpeed")
		|| Ziprail_StrEq(key, "ziprailMountReverseDistance")
		|| Ziprail_StrEq(key, "ZiplineAutoDetachDistance")
		|| Ziprail_StrEq(key, "ZiplineSpeedScale")
		|| Ziprail_StrEq(key, "ZiplineDropToBottom")
		|| Ziprail_StrEq(key, "Width")
		|| Ziprail_StrEq(key, "ZiplineFadeDistance")
		|| Ziprail_StrEq(key, "ZiplineLengthScale")
		|| Ziprail_StrEq(key, "DetachEndOnUse")
		|| Ziprail_StrEq(key, "DetachEndOnSpawn")
		|| Ziprail_StrEq(key, "ZiplineVertical")
		|| Ziprail_StrEq(key, "ZiplinePushOffInDirectionX")
		|| Ziprail_StrEq(key, "ZiplinePreserveVelocity")
		|| Ziprail_StrEq(key, "PreventManualDetach")
		|| Ziprail_StrEq(key, "ziplinePreventManualDetach");
}

static bool Ziprail_RecordZiplineKeyValue(uintptr_t ent, const char* key, const char* value)
{
	if (!ZiprailDedi_IsLive())
		return false;
	if (!ent || !key || !value)
		return false;
	if (!Ziprail_IsRecordedZiplineKey(key))
		return false;

	const bool isZiprailStart = Ziprail_StrEq(key, "isZiprailStart");

	AcquireSRWLockExclusive(&s_ziprailMetaLock);
	ZiprailEntityMeta* meta = Ziprail_FindOrCreateByEntLocked(ent);
	if (meta)
	{
		if (meta->classnameSeen)
		{
			// Stale record at a recycled address -- wipe, keep the slot.
			Ziprail_LogStartWipeLocked(meta, "ziplinekey-stale", key, value);
			memset(meta, 0, sizeof(*meta));
			meta->handle = Ziprail_CaptureHandle(ent);
		}

		if (isZiprailStart)
			meta->isZiprailStart = Ziprail_ParseBool(value);
		else if (Ziprail_StrEq(key, "useZiprailAutoDetachSpeed"))
		{
			meta->useAutoDetachSpeed = Ziprail_ParseInt(value);
			meta->hasUseAutoDetachSpeed = true;
			meta->hasZiprailAutoDetachSpeed = true;
		}
		else if (Ziprail_StrEq(key, "useAutoDetachSpeed") && !meta->hasZiprailAutoDetachSpeed)
		{
			meta->useAutoDetachSpeed = Ziprail_ParseInt(value);
			meta->hasUseAutoDetachSpeed = true;
		}
		else if (Ziprail_StrEq(key, "ziprailMountReverseDistance"))
		{
			meta->mountReverseDistance = Ziprail_ParseFloat(value);
			meta->hasMountReverseDistance = true;
		}
		else if (Ziprail_StrEq(key, "ZiplineAutoDetachDistance"))
		{
			meta->autoDetachDistance = Ziprail_ParseFloat(value);
			meta->hasAutoDetachDistance = true;
		}
		else if (Ziprail_StrEq(key, "ZiplineSpeedScale"))
		{
			meta->speedScale = Ziprail_ParseFloat(value);
			meta->hasSpeedScale = true;
		}
		else if (Ziprail_StrEq(key, "Width"))
		{
			meta->width = Ziprail_ParseFloat(value);
			meta->hasWidth = true;
		}
		else if (Ziprail_StrEq(key, "ZiplineFadeDistance"))
		{
			meta->fadeDist = Ziprail_ParseFloat(value);
			meta->hasFadeDist = true;
		}
		else if (Ziprail_StrEq(key, "ZiplineLengthScale"))
		{
			meta->lengthScale = Ziprail_ParseFloat(value);
			meta->hasLengthScale = true;
		}
		else if (Ziprail_StrEq(key, "ZiplineDropToBottom"))
		{
			meta->dropToBottom = Ziprail_ParseBool(value);
			meta->hasDropToBottom = true;
		}
		else if (Ziprail_StrEq(key, "DetachEndOnUse"))
		{
			meta->detachEndOnUse = Ziprail_ParseBool(value);
			meta->hasDetachEndOnUse = true;
		}
		else if (Ziprail_StrEq(key, "DetachEndOnSpawn"))
		{
			meta->detachEndOnSpawn = Ziprail_ParseBool(value);
			meta->hasDetachEndOnSpawn = true;
		}
		else if (Ziprail_StrEq(key, "ZiplineVertical"))
		{
			meta->vertical = Ziprail_ParseBool(value);
			meta->hasVertical = true;
		}
		else if (Ziprail_StrEq(key, "ZiplinePushOffInDirectionX"))
		{
			meta->pushOffX = Ziprail_ParseBool(value);
			meta->hasPushOffX = true;
		}
		else if (Ziprail_StrEq(key, "ZiplinePreserveVelocity"))
		{
			meta->preserveVelocity = Ziprail_ParseBool(value);
			meta->hasPreserveVelocity = true;
		}
		else if (Ziprail_StrEq(key, "PreventManualDetach")
			|| Ziprail_StrEq(key, "ziplinePreventManualDetach"))
		{
			meta->preventManualDetach = Ziprail_ParseBool(value);
			meta->hasPreventManualDetach = true;
		}
	}
	ReleaseSRWLockExclusive(&s_ziprailMetaLock);

	if (isZiprailStart && Ziprail_ParseBool(value))
	{
		const uint32_t n = s_ziprailStartKeyCount.fetch_add(
			1, std::memory_order_relaxed);
		if (n < 8 || (n % 128) == 0)
		{
			Warning(eDLL_T::SERVER,
				"[ZIPRAIL-DEDI] observed isZiprailStart #%u ent=0x%p\n",
				n + 1, reinterpret_cast<void*>(ent));
		}

		const uint32_t startH = Ziprail_CaptureHandle(ent);
		AcquireSRWLockExclusive(&s_ziprailStartLedgerLock);
		bool alreadyLedgered = false;
		for (uint32_t i = 0; i < s_ziprailStartLedgerCount; ++i)
		{
			if (s_ziprailStartLedger[i] == startH)
			{
				alreadyLedgered = true;
				break;
			}
		}
		if (!alreadyLedgered && s_ziprailStartLedgerCount < 192)
			s_ziprailStartLedger[s_ziprailStartLedgerCount++] = startH;
		ReleaseSRWLockExclusive(&s_ziprailStartLedgerLock);
	}
	return true;
}

// Plain POD reads on a ResolveHandle-validated live entity (no SEH).
static bool Ziprail_ReadVtable(uintptr_t ent, uintptr_t* out)
{
	if (!ent || !out)
		return false;
	*out = *reinterpret_cast<uintptr_t*>(ent);
	return true;
}

static int16_t Ziprail_ReadEdictIdx(uintptr_t ent)
{
	if (!ent)
		return -1;
	return *reinterpret_cast<int16_t*>(ent + kZiprailEntOffEdictIdx);
}

// ---------------------------------------------------------------------------
// Chain registry accessors (public via header)
// ---------------------------------------------------------------------------
bool ZiprailDedi_HasPath(uintptr_t ent)
{
	if (!ent)
		return false;

	const uint32_t h = Ziprail_CaptureHandle(ent);
	bool found = false;
	AcquireSRWLockShared(&s_ziprailChainLock);
	for (const ZiprailChain& chain : s_ziprailChains)
	{
		if (chain.startHandle == h && chain.applied)
		{
			found = true;
			break;
		}
	}
	ReleaseSRWLockShared(&s_ziprailChainLock);
	return found;
}

// The class instance baseline packs a defaults entity that was never a chain
// start, so an empty wire block there is correct rather than a fault.
bool ZiprailDedi_IsKnownChainStart(uintptr_t ent)
{
	if (!ent)
		return false;

	const uint32_t h = Ziprail_CaptureHandle(ent);
	if (h == INVALID_EHANDLE_INDEX)
		return false;

	bool found = false;
	AcquireSRWLockShared(&s_ziprailStartLedgerLock);
	for (uint32_t i = 0; i < s_ziprailStartLedgerCount && i < 192; ++i)
	{
		if (s_ziprailStartLedger[i] == h)
		{
			found = true;
			break;
		}
	}
	ReleaseSRWLockShared(&s_ziprailStartLedgerLock);
	return found;
}

void ZiprailDedi_LevelShutdown()
{
	// Always clear tables (cheap, prevents cross-map pointer reuse if something
	// armed mid-session). Log only when the system is live.
	const bool live = ZiprailDedi_IsLive();
	AcquireSRWLockExclusive(&s_ziprailChainLock);
	memset(s_ziprailChains, 0, sizeof(s_ziprailChains));
	ReleaseSRWLockExclusive(&s_ziprailChainLock);

	AcquireSRWLockExclusive(&s_ziprailMetaLock);
	memset(s_ziprailMeta, 0, sizeof(s_ziprailMeta));
	ReleaseSRWLockExclusive(&s_ziprailMetaLock);

	AcquireSRWLockExclusive(&s_ziprailStartLedgerLock);
	s_ziprailStartLedgerCount = 0;
	memset(s_ziprailStartLedger, 0, sizeof(s_ziprailStartLedger));
	ReleaseSRWLockExclusive(&s_ziprailStartLedgerLock);

	if (live)
	{
		Warning(eDLL_T::SERVER,
			"[ZIPRAIL-DEDI] LevelShutdown: cleared chain registry + KeyValue meta table\n");
	}
}

static ZiprailChain* Ziprail_FindChainByStartHandleLocked(uint32_t startHandle)
{
	if (!startHandle || startHandle == INVALID_EHANDLE_INDEX)
		return nullptr;

	for (ZiprailChain& chain : s_ziprailChains)
	{
		if (chain.startHandle == startHandle)
			return &chain;
	}
	return nullptr;
}

static ZiprailChain* Ziprail_FindChainByStartLocked(uintptr_t startEnt)
{
	if (!startEnt)
		return nullptr;
	return Ziprail_FindChainByStartHandleLocked(Ziprail_CaptureHandle(startEnt));
}

static ZiprailChain* Ziprail_FindOrCreateChainLocked(uintptr_t startEnt)
{
	if (!startEnt)
		return nullptr;

	if (ZiprailChain* existing = Ziprail_FindChainByStartLocked(startEnt))
		return existing;

	const uint32_t startHandle = Ziprail_CaptureHandle(startEnt);
	if (!startHandle || startHandle == INVALID_EHANDLE_INDEX)
		return nullptr;

	for (ZiprailChain& chain : s_ziprailChains)
	{
		if (!chain.startHandle)
		{
			memset(&chain, 0, sizeof(chain));
			chain.startHandle = startHandle;
			return &chain;
		}
	}

	const uint32_t n = s_ziprailChainRegistryOverflowCount.fetch_add(
		1, std::memory_order_relaxed);
	if (n < 4 || (n % 128) == 0)
	{
		Warning(eDLL_T::SERVER,
			"[ZIPRAIL-DEDI] chain registry full while registering start=0x%p (#%u)\n",
			reinterpret_cast<void*>(startEnt), n + 1);
	}
	return nullptr;
}

// m_vecAbsOrigin at +0x450. Fallback when "origin" KeyValue preceded record-creating keys.
static bool Ziprail_ReadEntityOrigin(uintptr_t ent, float* x, float* y, float* z)
{
	if (!ent || !x || !y || !z)
		return false;
	*x = *reinterpret_cast<float*>(ent + kZiprailEntOffAbsOrigin);
	*y = *reinterpret_cast<float*>(ent + kZiprailEntOffAbsOrigin + 4);
	*z = *reinterpret_cast<float*>(ent + kZiprailEntOffAbsOrigin + 8);
	return true;
}

// Walk start -> far end via reverse link (linkToGuid == cur->guid). Cap kZiprailMaxChainNodes.
static int Ziprail_WalkChainLocked(const ZiprailEntityMeta* start,
	float outX[kZiprailMaxChainNodes], float outY[kZiprailMaxChainNodes],
	float outZ[kZiprailMaxChainNodes], int outNumSmooth[kZiprailMaxChainNodes],
	int outTangentType[kZiprailMaxChainNodes],
	uint32_t outHandles[kZiprailMaxChainNodes],
	ZiprailClassTag outTags[kZiprailMaxChainNodes], int* outUseAutoDetach,
	float* outMountReverse, uintptr_t* outFarEnt)
{
	if (!start || !start->hasGuid)
		return 0;

	uint64_t seenGuids[kZiprailMaxChainNodes] = {};
	int seenCount = 0;

	const ZiprailEntityMeta* cur = start;
	int count = 0;
	int autoDetach = -1;
	float mountRev = -1.0f;
	uintptr_t farEnt = Ziprail_ResolveHandle(start->handle);

	while (cur && count < kZiprailMaxChainNodes)
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
		if (cur->hasOrigin)
		{
			x = cur->origin.x;
			y = cur->origin.y;
			z = cur->origin.z;
		}
		else if (!Ziprail_ReadEntityOrigin(Ziprail_ResolveHandle(cur->handle), &x, &y, &z))
			break; // node lacks origin -- abort this chain, retry next rebuild

		outX[count] = x;
		outY[count] = y;
		outZ[count] = z;
		outNumSmooth[count] = cur->hasNumSmoothPoints ? cur->numSmoothPoints : -1;
		outTangentType[count] = cur->hasTangentType ? cur->tangentType : 0;
		outHandles[count] = cur->handle;
		if (outTags)
			outTags[count] = cur->classTag;
		if (cur->hasUseAutoDetachSpeed && autoDetach < 0)
			autoDetach = cur->useAutoDetachSpeed;
		if (cur->hasMountReverseDistance && mountRev < 0.0f)
			mountRev = cur->mountReverseDistance;
		farEnt = Ziprail_ResolveHandle(cur->handle);
		++count;

		if (cur->hasGuid)
		{
			bool cycle = false;
			for (int i = 0; i < seenCount; ++i)
			{
				if (seenGuids[i] == cur->guid)
				{
					cycle = true;
					break;
				}
			}
			if (cycle)
				break;
			if (seenCount < kZiprailMaxChainNodes)
				seenGuids[seenCount++] = cur->guid;
		}

		if (!cur->hasGuid)
			break;

		// Next node = the meta whose link_to_guid_0 == cur->guid. Prefer a
		// script_mover_train_node when the reverse link is ambiguous.
		const ZiprailEntityMeta* next = nullptr;
		int matchCount = 0;
		for (const ZiprailEntityMeta& candidate : s_ziprailMeta)
		{
			if (!candidate.handle || !candidate.hasLinkToGuid)
				continue;
			if (candidate.linkToGuid != cur->guid)
				continue;
			++matchCount;
			if (!next)
			{
				next = &candidate;
				continue;
			}
			// Ambiguous: prefer the train-node-tagged entry.
			if (candidate.classTag == ZiprailClassTag::kTrainNode &&
				next->classTag != ZiprailClassTag::kTrainNode)
			{
				next = &candidate;
			}
		}
		if (matchCount > 1)
		{
			Warning(eDLL_T::SERVER,
				"[ZIPRAIL-DEDI] ambiguous chain link: %d entities link_to_guid=0x%llX "
				"(chain start=0x%p) -- preferring train-node-tagged entry ent=0x%p\n",
				matchCount, static_cast<unsigned long long>(cur->guid),
				reinterpret_cast<void*>(Ziprail_ResolveHandle(start->handle)),
				next ? reinterpret_cast<void*>(Ziprail_ResolveHandle(next->handle)) : nullptr);
		}
		cur = next;
	}

	if (outUseAutoDetach)
		*outUseAutoDetach = autoDetach;
	if (outMountReverse)
		*outMountReverse = mountRev;
	if (outFarEnt)
		*outFarEnt = farEnt;
	return count;
}

// Native-shape wire block. Plain float; matches client MakePathSmoothPoints magnitudes.
static float Ziprail_Vec3Dot(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float Ziprail_Vec3Length(const float v[3])
{
	return sqrtf(Ziprail_Vec3Dot(v, v));
}

static void Ziprail_Vec3Sub(const float a[3], const float b[3], float out[3])
{
	out[0] = a[0] - b[0];
	out[1] = a[1] - b[1];
	out[2] = a[2] - b[2];
}

static bool Ziprail_Vec3ExactEqual(const float a[3], const float b[3])
{
	return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

// Perpendicular distance from p to the chord a -> b (t clamped to [0,1]).
static float Ziprail_ChordPerpDist(const float p[3], const float a[3], const float b[3])
{
	float ab[3];
	Ziprail_Vec3Sub(b, a, ab);
	const float ab2 = Ziprail_Vec3Dot(ab, ab);
	float t = 0.0f;
	if (ab2 > 0.0f)
	{
		float ap[3];
		Ziprail_Vec3Sub(p, a, ap);
		t = Ziprail_Vec3Dot(ap, ab) / ab2;
		if (t < 0.0f) t = 0.0f;
		if (t > 1.0f) t = 1.0f;
	}
	const float d[3] = {
		p[0] - (a[0] + t * ab[0]),
		p[1] - (a[1] + t * ab[1]),
		p[2] - (a[2] + t * ab[2]) };
	return Ziprail_Vec3Length(d);
}

static void Ziprail_Vec3NormalizeScale(float v[3], float scale)
{
	const float len = Ziprail_Vec3Length(v);
	if (len > 0.0f)
	{
		const float inv = scale / len;
		v[0] *= inv;
		v[1] *= inv;
		v[2] *= inv;
	}
	else
	{
		v[0] = 0.0f;
		v[1] = 0.0f;
		v[2] = 0.0f;
	}
}

static void Ziprail_ExtendAabb(float mins[3], float maxs[3], const float p[3])
{
	for (int axis = 0; axis < 3; ++axis)
	{
		if (p[axis] < mins[axis]) mins[axis] = p[axis];
		if (p[axis] > maxs[axis]) maxs[axis] = p[axis];
	}
}

// TangentForNode -- exact client twin ( GetTangentForNode).
static void Ziprail_TangentForNode(const float pos[][3], const int tangentTypes[],
	int i, int numNodes, float outT[3])
{
	const int next = i + 1;
	if (next >= numNodes)
	{
		outT[0] = 0.0f; outT[1] = 0.0f; outT[2] = 0.0f;
		return;
	}
	const int prev = (i - 1) > 0 ? (i - 1) : 0;

	switch (tangentTypes[i])
	{
	case 1:
		Ziprail_Vec3Sub(pos[i], pos[prev], outT);
		break;
	case 2:
		Ziprail_Vec3Sub(pos[next], pos[i], outT);
		break;
	default:
		Ziprail_Vec3Sub(pos[next], pos[prev], outT);
		break;
	}
}

// Cubic Hermite basis: h00*p1 + h10*t1 + h01*p2 + h11*t2.
static void Ziprail_HermiteSpline3(const float p1[3], const float p2[3],
	const float t1[3], const float t2[3], float u, float out[3])
{
	const float u2 = u * u;
	const float u3 = u2 * u;
	const float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
	const float h10 = u3 - 2.0f * u2 + u;
	const float h01 = -2.0f * u3 + 3.0f * u2;
	const float h11 = u3 - u2;

	for (int axis = 0; axis < 3; ++axis)
	{
		out[axis] = h00 * p1[axis] + h10 * t1[axis] +
			h01 * p2[axis] + h11 * t2[axis];
	}
}

// Analytic derivative of the cubic Hermite basis (dh/du).
static void Ziprail_DHermiteSpline3(const float p1[3], const float p2[3],
	const float t1[3], const float t2[3], float u, float out[3])
{
	const float u2 = u * u;
	const float dh00 = 6.0f * u2 - 6.0f * u;
	const float dh10 = 3.0f * u2 - 4.0f * u + 1.0f;
	const float dh01 = -6.0f * u2 + 6.0f * u;
	const float dh11 = 3.0f * u2 - 2.0f * u;

	for (int axis = 0; axis < 3; ++axis)
	{
		out[axis] = dh00 * p1[axis] + dh10 * t1[axis] +
			dh01 * p2[axis] + dh11 * t2[axis];
	}
}

static const float kZiprailSqrtFltMin = sqrtf(FLT_MIN);

static void Ziprail_Vec3NormalizeSafe(float v[3])
{
	const float flInv = 1.0f / fmaxf(Ziprail_Vec3Length(v), kZiprailSqrtFltMin);
	v[0] *= flInv;
	v[1] *= flInv;
	v[2] *= flInv;
}

// Bake Hermite samples; m_ziplineRestPositions is a 16-point arc-length decimation.
static constexpr int kZiprailSmoothPointsMax = 32; // client per-segment cap

struct ZiprailSegment
{
	float p1[3];
	float p2[3];
	float t1[3];
	float t2[3];
	int   count;    // smooth points published for this segment (2..32)
	float sagitta;  // max deviation of the curve from its own chord
	float arc;      // length of this segment's smooth-point polyline
	bool  degenerate;
};

// Segment tangents, exact client twin: GetTangentForNode with a chord fallback
// for a zero-length result, then both tangents rescaled to the chord length.
static void Ziprail_BuildSegmentEnds(const float pos[][3], const int tangentTypes[],
	int i, int numNodes, ZiprailSegment* seg)
{
	for (int axis = 0; axis < 3; ++axis)
	{
		seg->p1[axis] = pos[i][axis];
		seg->p2[axis] = pos[i + 1][axis];
	}

	seg->degenerate = Ziprail_Vec3ExactEqual(seg->p1, seg->p2);
	seg->count = 2;
	seg->sagitta = 0.0f;
	seg->arc = 0.0f;
	if (seg->degenerate)
		return;

	float chord[3];
	Ziprail_Vec3Sub(seg->p2, seg->p1, chord);
	const float segLen = Ziprail_Vec3Length(chord);

	Ziprail_TangentForNode(pos, tangentTypes, i, numNodes, seg->t1);
	if (Ziprail_Vec3Dot(seg->t1, seg->t1) == 0.0f)
	{
		seg->t1[0] = chord[0]; seg->t1[1] = chord[1]; seg->t1[2] = chord[2];
	}
	Ziprail_TangentForNode(pos, tangentTypes, i + 1, numNodes, seg->t2);
	if (Ziprail_Vec3Dot(seg->t2, seg->t2) == 0.0f)
	{
		seg->t2[0] = chord[0]; seg->t2[1] = chord[1]; seg->t2[2] = chord[2];
	}

	Ziprail_Vec3NormalizeScale(seg->t1, segLen);
	Ziprail_Vec3NormalizeScale(seg->t2, segLen);
}

static void Ziprail_SegmentPoint(const ZiprailSegment* seg, int k, float out[3])
{
	if (seg->degenerate || seg->count < 2)
	{
		out[0] = seg->p1[0]; out[1] = seg->p1[1]; out[2] = seg->p1[2];
		return;
	}
	const float u = static_cast<float>(k) / static_cast<float>(seg->count - 1);
	Ziprail_HermiteSpline3(seg->p1, seg->p2, seg->t1, seg->t2, u, out);
}

// Max distance from the curve to its own chord -- drives the point budget.
static void Ziprail_MeasureSegmentSagitta(ZiprailSegment* seg)
{
	seg->sagitta = 0.0f;
	if (seg->degenerate)
		return;

	float ab[3];
	Ziprail_Vec3Sub(seg->p2, seg->p1, ab);
	const float ab2 = Ziprail_Vec3Dot(ab, ab);
	if (ab2 <= 0.0f)
		return;

	static constexpr int kSamples = 32;
	for (int j = 1; j < kSamples; ++j)
	{
		const float u = static_cast<float>(j) / static_cast<float>(kSamples);
		float h[3];
		Ziprail_HermiteSpline3(seg->p1, seg->p2, seg->t1, seg->t2, u, h);

		float ap[3];
		Ziprail_Vec3Sub(h, seg->p1, ap);
		float t = Ziprail_Vec3Dot(ap, ab) / ab2;
		if (t < 0.0f) t = 0.0f;
		if (t > 1.0f) t = 1.0f;

		const float d[3] = {
			h[0] - (seg->p1[0] + t * ab[0]),
			h[1] - (seg->p1[1] + t * ab[1]),
			h[2] - (seg->p1[2] + t * ab[2]) };
		const float dist = Ziprail_Vec3Length(d);
		if (dist > seg->sagitta)
			seg->sagitta = dist;
	}
}

static ConVar sdk_ziprail_curve_budget("sdk_ziprail_curve_budget", "96", FCVAR_RELEASE,
	"Total polyline point budget for a ziprail chain. Shared by the client bake and "
	"the dedicated server's baked path table that the rail ride walks.",
	true, 2.f, true, 128.f);
static ConVar sdk_ziprail_node_headroom("sdk_ziprail_node_headroom", "0", FCVAR_RELEASE,
	"Nodes reserved off the DT path-node array so an over-long chain can be reduced. "
	"Trades map fidelity for wire size; leave at 0 unless a chain exceeds the array.",
	true, 0.f, true, 24.f);
static ConVar sdk_ziprail_curve_tol("sdk_ziprail_curve_tol", "1", FCVAR_RELEASE,
	"Residual interpolation error, in units, above which a ziprail segment is "
	"reported as under-budgeted.", true, 0.f, true, 64.f);
static ConVar sdk_ziprail_extents_pad("sdk_ziprail_extents_pad", "48", FCVAR_RELEASE,
	"Units added to every axis of the published ziprail path extents. The client "
	"culls the rope against this box and nothing else, so a box that only bounds "
	"the path points leaves the rendered cord outside its own bounds.",
	true, 0.f, true, 512.f);
static ConVar sdk_ziprail_publish_bounds("sdk_ziprail_publish_bounds", "1", FCVAR_RELEASE,
	"publish the promoted rail's collision bounds from the path extents so "
	"the client can render it. 0 leaves the entity's inherited bounds.");
static ConVar sdk_ziprail_hide_members("sdk_ziprail_hide_members", "1", FCVAR_RELEASE,
	"Disable non-promoted CZipline members of a materialized ziprail chain and "
	"unpublish their networked rope (rest/point counts). The promoted start "
	"draws the cord; members stay networked but C_Zipline::DrawModel skips at "
	"pointCount<2 and the mount search rejects m_ziplineEnabled 0.");

// Spend the point budget where it buys the most accuracy. A c-point segment
// leaves ~sagitta/(c-1)^2 of interpolation error, so hand each extra point to
// whichever segment is currently worst by that measure.
static int Ziprail_BudgetSmoothCounts(ZiprailSegment* segs, int nSeg, int budget)
{
	if (budget < 2)
		budget = 2;
	if (budget > kZiprailBakedPointsMax)
		budget = kZiprailBakedPointsMax;

	int total = nSeg + 1; // every count starts at 2; joins are shared
	int guard = 0;
	while (total < budget && guard++ < budget)
	{
		int best = -1;
		float bestErr = 0.0f;
		for (int s = 0; s < nSeg; ++s)
		{
			if (segs[s].degenerate || segs[s].count >= kZiprailSmoothPointsMax)
				continue;
			const float sub = static_cast<float>(segs[s].count - 1);
			const float err = segs[s].sagitta / (sub * sub);
			if (err > bestErr)
			{
				bestErr = err;
				best = s;
			}
		}
		if (best < 0)
			break;
		++segs[best].count;
		++total;
	}
	return total;
}

// A chain longer than the node cap cannot be matched point-for-point, so drop
// the interior nodes that bend the path least until it fits. Both engines then
// agree on the reduced node list; only fidelity to the map is lost.
static int Ziprail_ReduceNodesToRestCap(ZiprailChain* chain, int nodeCap)
{
	if (nodeCap < 2)
		nodeCap = 2;

	int n = chain->nodeCount;
	if (n <= nodeCap)
		return n;

	while (n > nodeCap)
	{
		int drop = -1;
		float straightest = -2.0f;
		for (int i = 1; i < n - 1; ++i)
		{
			const float prev[3] = { chain->nodesX[i - 1], chain->nodesY[i - 1], chain->nodesZ[i - 1] };
			const float cur[3] = { chain->nodesX[i], chain->nodesY[i], chain->nodesZ[i] };
			const float next[3] = { chain->nodesX[i + 1], chain->nodesY[i + 1], chain->nodesZ[i + 1] };

			float a[3], b[3];
			Ziprail_Vec3Sub(cur, prev, a);
			Ziprail_Vec3Sub(next, cur, b);
			const float la = Ziprail_Vec3Length(a);
			const float lb = Ziprail_Vec3Length(b);
			const float cosTurn = (la > 0.0f && lb > 0.0f)
				? (Ziprail_Vec3Dot(a, b) / (la * lb)) : 1.0f;
			if (cosTurn > straightest)
			{
				straightest = cosTurn;
				drop = i;
			}
		}
		if (drop < 0)
			break;

		for (int i = drop; i < n - 1; ++i)
		{
			chain->nodesX[i] = chain->nodesX[i + 1];
			chain->nodesY[i] = chain->nodesY[i + 1];
			chain->nodesZ[i] = chain->nodesZ[i + 1];
			chain->numSmooth[i] = chain->numSmooth[i + 1];
			chain->tangentType[i] = chain->tangentType[i + 1];
		}
		--n;
	}

	Warning(eDLL_T::SERVER,
		"[ZIPRAIL-PATH] start=0x%p chain has %d nodes but the node cap is %d -- "
		"reduced to %d nodes so both engines walk the same path\n",
		reinterpret_cast<void*>(Ziprail_ResolveHandle(chain->startHandle)),
		chain->nodeCount, nodeCap, n);

	chain->nodeCount = n;
	return n;
}

static void Ziprail_ComputeWireBlock(ZiprailChain* chain)
{
	// Client builds rail geometry once from the path props in the entity's
	// first update and never rebuilds it; this block must be final before first network.
	if (!chain)
		return;

	ZiprailWireBlock& block = chain->wire;
	memset(&block, 0, sizeof(block));
	chain->restCount = 0;
	memset(&chain->baked, 0, sizeof(chain->baked));

	block.useAutoDetachSpeed = chain->useAutoDetachSpeed ? 1 : 0;
	block.preventManualDetach = chain->preventManualDetach ? 1 : 0;
	block.autoDetachDistance = chain->autoDetachDistance;
	block.speedScale = (chain->speedScale > 0.0f) ? chain->speedScale : 1.0f;
	block.mountReverseDistance = (chain->mountReverseDistance > 0.0f)
		? chain->mountReverseDistance : kZiprailDefaultMountReverseDist;
	block.ropeColorModulation[0] = 1.0f;
	block.ropeColorModulation[1] = 1.0f;
	block.ropeColorModulation[2] = 1.0f;

	int budget = sdk_ziprail_curve_budget.GetInt();
	if (budget < 2)
		budget = 2;
	if (budget > kZiprailBakedPointsMax)
		budget = kZiprailBakedPointsMax;

	int headroom = sdk_ziprail_node_headroom.GetInt();
	if (headroom < 0)
		headroom = 0;
	if (headroom > 24)
		headroom = 24;

	int nodeCap = kZiprailWireMaxNodes - headroom;
	if (nodeCap < 2)
		nodeCap = 2;

	const int N = Ziprail_ReduceNodesToRestCap(chain, nodeCap);
	block.numNodes = N;
	if (N <= 0)
	{
		chain->wireValid = true;
		return;
	}

	float pos[kZiprailWireMaxNodes][3] = {};
	for (int i = 0; i < N; ++i)
	{
		pos[i][0] = chain->nodesX[i];
		pos[i][1] = chain->nodesY[i];
		pos[i][2] = chain->nodesZ[i];
		block.positions[i][0] = pos[i][0];
		block.positions[i][1] = pos[i][1];
		block.positions[i][2] = pos[i][2];
		block.tangentTypes[i] = chain->tangentType[i];
	}

	if (N == 1)
	{
		block.numSmooth[0] = 0;
		chain->rest[0][0] = pos[0][0];
		chain->rest[0][1] = pos[0][1];
		chain->rest[0][2] = pos[0][2];
		chain->restCount = 1;
		chain->wireValid = true;
		return;
	}

	const int nSeg = N - 1;
	ZiprailSegment segs[kZiprailWireMaxNodes] = {};
	for (int i = 0; i < nSeg; ++i)
	{
		Ziprail_BuildSegmentEnds(pos, block.tangentTypes, i, N, &segs[i]);
		Ziprail_MeasureSegmentSagitta(&segs[i]);
	}

	const int polyPoints = Ziprail_BudgetSmoothCounts(segs, nSeg, budget);

	int flatSegs = 0;
	float worstSag = 0.0f;
	float worstResidual = 0.0f;
	int starvedSegs = 0;
	const float flTol = sdk_ziprail_curve_tol.GetFloat();
	for (int i = 0; i < nSeg; ++i)
	{
		if (segs[i].degenerate)
			continue;
		if (segs[i].count == 2)
			++flatSegs;
		if (segs[i].sagitta > worstSag)
			worstSag = segs[i].sagitta;
		if (segs[i].count >= 2)
		{
			const float sub = static_cast<float>(segs[i].count - 1);
			const float residual = segs[i].sagitta / (sub * sub);
			if (residual > worstResidual)
				worstResidual = residual;
			if (residual > flTol)
				++starvedSegs;
		}
	}

	for (int i = 0; i < nSeg; ++i)
	{
		ZiprailSegment& seg = segs[i];
		// Authored per-node count from the map; -1 means the client subdivides
		// at its own density (segmentLength / 100, clamped to 32).
		block.numSmooth[i] = chain->numSmooth[i];

		float prev[3] = { seg.p1[0], seg.p1[1], seg.p1[2] };
		seg.arc = 0.0f;

		for (int k = 0; k < seg.count; ++k)
		{
			float p[3];
			Ziprail_SegmentPoint(&seg, k, p);

			float delta[3];
			Ziprail_Vec3Sub(p, prev, delta);
			seg.arc += Ziprail_Vec3Length(delta);
			prev[0] = p[0]; prev[1] = p[1]; prev[2] = p[2];
		}
	}
	block.numSmooth[N - 1] = 0; // last node owns no segment

	// m_smoothDistanceToNode is the cumulative arc at each node, [0] = 0. The
	// client's node search requires the last entry to be >= m_ziprailPathLen, so
	// pathLen is taken from it rather than accumulated separately.
	block.smoothDistToNode[0] = 0.0f;
	for (int i = 1; i < N; ++i)
		block.smoothDistToNode[i] = block.smoothDistToNode[i - 1] + segs[i - 1].arc;
	block.pathLen = block.smoothDistToNode[N - 1];

	{
		ZiprailBakedPath& baked = chain->baked;
		int nBaked = 0;
		float flCum = 0.0f;
		float prevPos[3] = {};
		bool bOverflow = false;

		for (int i = 0; i < nSeg; ++i)
		{
			const ZiprailSegment& seg = segs[i];
			baked.nodeStart[i] = nBaked;

			const int nSegPts = (seg.degenerate || seg.count < 2) ? 2 : seg.count;
			for (int k = 0; k < nSegPts; ++k)
			{
				if (nBaked >= kZiprailBakedPointsMax)
				{
					bOverflow = true;
					break;
				}

				float p[3];
				float d[3];
				if (seg.degenerate || seg.count < 2)
				{
					p[0] = seg.p1[0]; p[1] = seg.p1[1]; p[2] = seg.p1[2];
					d[0] = seg.p2[0] - seg.p1[0];
					d[1] = seg.p2[1] - seg.p1[1];
					d[2] = seg.p2[2] - seg.p1[2];
				}
				else
				{
					const float u = static_cast<float>(k)
						/ static_cast<float>(seg.count - 1);
					Ziprail_HermiteSpline3(seg.p1, seg.p2, seg.t1, seg.t2, u, p);
					Ziprail_DHermiteSpline3(seg.p1, seg.p2, seg.t1, seg.t2, u, d);
				}
				Ziprail_Vec3NormalizeSafe(d);

				if (nBaked > 0)
				{
					float delta[3];
					Ziprail_Vec3Sub(p, prevPos, delta);
					flCum += Ziprail_Vec3Length(delta);
				}

				baked.pos[nBaked][0] = p[0];
				baked.pos[nBaked][1] = p[1];
				baked.pos[nBaked][2] = p[2];
				baked.dir[nBaked][0] = d[0];
				baked.dir[nBaked][1] = d[1];
				baked.dir[nBaked][2] = d[2];
				baked.dist[nBaked] = flCum;
				prevPos[0] = p[0]; prevPos[1] = p[1]; prevPos[2] = p[2];
				++nBaked;
			}

			if (bOverflow)
				break;
		}

		if (bOverflow)
		{
			baked.count = 0;
			static bool s_bakedOverflowWarned = false;
			if (!s_bakedOverflowWarned)
			{
				s_bakedOverflowWarned = true;
				Warning(eDLL_T::SERVER,
					"[ZIPRAIL-PATH] start=0x%p baked path would exceed %d points -- "
					"leaving count=0; ride direction falls back to the polyline chord\n",
					reinterpret_cast<void*>(Ziprail_ResolveHandle(chain->startHandle)),
					kZiprailBakedPointsMax);
			}
		}
		else
		{
			baked.count = nBaked;
		}
	}

	const ZiprailBakedPath& baked = chain->baked;

	float mins[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
	float maxs[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	if (baked.count >= 1)
	{
		mins[0] = maxs[0] = baked.pos[0][0];
		mins[1] = maxs[1] = baked.pos[0][1];
		mins[2] = maxs[2] = baked.pos[0][2];
		for (int i = 1; i < baked.count; ++i)
			Ziprail_ExtendAabb(mins, maxs, baked.pos[i]);
		for (int i = 0; i < N; ++i)
			Ziprail_ExtendAabb(mins, maxs, pos[i]);
	}
	else
	{
		for (int i = 0; i < N; ++i)
			Ziprail_ExtendAabb(mins, maxs, pos[i]);
	}

	// The client culls a ziprail against m_pathExtentsMins/Maxs plus a small pad of
	// its own; an axis-aligned rail otherwise publishes a zero-thickness axis.
	const float flPad = sdk_ziprail_extents_pad.GetFloat();
	for (int a = 0; a < 3; ++a)
	{
		mins[a] -= flPad;
		maxs[a] += flPad;
	}

	block.extentsMins[0] = mins[0];
	block.extentsMins[1] = mins[1];
	block.extentsMins[2] = mins[2];
	block.extentsMaxs[0] = maxs[0];
	block.extentsMaxs[1] = maxs[1];
	block.extentsMaxs[2] = maxs[2];

	// Rope array is 16 wide; pin path nodes into rest[], fill leftovers with farthest outliers.
	int written = 0;
	int nWant = 0;
	int nodesPinned = 0;
	int refined = 0;
	int nodesDropped = 0;
	if (baked.count < 2)
	{
		for (int i = 0; i < N && written < kZiprailMaxRestPoints; ++i)
		{
			const float p[3] = { pos[i][0], pos[i][1], pos[i][2] };
			if (written > 0 && Ziprail_Vec3ExactEqual(chain->rest[written - 1], p))
				continue;
			chain->rest[written][0] = p[0];
			chain->rest[written][1] = p[1];
			chain->rest[written][2] = p[2];
			++written;
		}
	}
	else
	{
		int nodeIdx[kZiprailWireMaxNodes] = {};
		int nKeep = 0;
		for (int i = 0; i < N && nKeep < kZiprailWireMaxNodes; ++i)
		{
			int idx = (i < N - 1) ? baked.nodeStart[i] : (baked.count - 1);
			if (idx < 0)
				idx = 0;
			if (idx >= baked.count)
				idx = baked.count - 1;
			if (nKeep > 0 && idx <= nodeIdx[nKeep - 1])
				continue;
			nodeIdx[nKeep++] = idx;
		}

		while (nKeep > kZiprailMaxRestPoints)
		{
			int dropAt = -1;
			float bestDist = FLT_MAX;
			for (int k = 1; k < nKeep - 1; ++k)
			{
				const float dist = Ziprail_ChordPerpDist(
					baked.pos[nodeIdx[k]],
					baked.pos[nodeIdx[k - 1]],
					baked.pos[nodeIdx[k + 1]]);
				if (dist < bestDist)
				{
					bestDist = dist;
					dropAt = k;
				}
			}
			if (dropAt < 0)
				break;
			for (int i = dropAt; i < nKeep - 1; ++i)
				nodeIdx[i] = nodeIdx[i + 1];
			--nKeep;
			++nodesDropped;
		}

		if (nodesDropped > 0)
		{
			Warning(eDLL_T::SERVER,
				"[ZIPRAIL-PATH] start=0x%p rest polyline dropped %d path node(s) "
				"to fit the %d-point cap\n",
				reinterpret_cast<void*>(Ziprail_ResolveHandle(chain->startHandle)),
				nodesDropped, kZiprailMaxRestPoints);
		}

		nodesPinned = nKeep;

		int budgetLeft = kZiprailMaxRestPoints - nKeep;
		while (budgetLeft > 0)
		{
			int bestBaked = -1;
			int bestAfter = -1;
			float bestDist = 0.0f;

			for (int j = 0; j < nKeep - 1; ++j)
			{
				const int a = nodeIdx[j];
				const int b = nodeIdx[j + 1];
				if (a < 0 || b <= a + 1 || b >= baked.count)
					continue;
				for (int m = a + 1; m < b && m < baked.count; ++m)
				{
					const float dist = Ziprail_ChordPerpDist(
						baked.pos[m], baked.pos[a], baked.pos[b]);
					if (dist > bestDist)
					{
						bestDist = dist;
						bestBaked = m;
						bestAfter = j;
					}
				}
			}

			if (bestBaked < 0 || bestDist <= 0.0f || bestAfter < 0)
				break;
			if (nKeep >= kZiprailMaxRestPoints)
				break;

			for (int i = nKeep; i > bestAfter + 1; --i)
				nodeIdx[i] = nodeIdx[i - 1];
			nodeIdx[bestAfter + 1] = bestBaked;
			++nKeep;
			--budgetLeft;
			++refined;
		}

		nWant = nKeep;
		for (int i = 0; i < nKeep && written < kZiprailMaxRestPoints; ++i)
		{
			const float* const p = baked.pos[nodeIdx[i]];
			if (written > 0 && Ziprail_Vec3ExactEqual(chain->rest[written - 1], p))
				continue;
			chain->rest[written][0] = p[0];
			chain->rest[written][1] = p[1];
			chain->rest[written][2] = p[2];
			++written;
		}

		if (written >= 1)
		{
			chain->rest[0][0] = baked.pos[0][0];
			chain->rest[0][1] = baked.pos[0][1];
			chain->rest[0][2] = baked.pos[0][2];
		}
		if (written >= 2)
		{
			chain->rest[written - 1][0] = baked.pos[baked.count - 1][0];
			chain->rest[written - 1][1] = baked.pos[baked.count - 1][1];
			chain->rest[written - 1][2] = baked.pos[baked.count - 1][2];
		}
	}
	chain->restCount = written;

	chain->wireValid = true;

	float restLen = 0.0f;
	for (int i = 1; i < written; ++i)
	{
		float d[3];
		Ziprail_Vec3Sub(chain->rest[i], chain->rest[i - 1], d);
		restLen += Ziprail_Vec3Length(d);
	}

	Warning(eDLL_T::SERVER,
		"[ZIPRAIL-PATH] start=0x%p nodes=%d poly=%d rest=%d pathLen=%.3f "
		"useAutoDetach=%d autoDetach=%.1f speedScale=%.3f restLen=%.3f "
		"nodesPinned=%d refined=%d nodesDropped=%d "
		"ext=(%.0f %.0f %.0f)-(%.0f %.0f %.0f) pad=%.0f "
		"budget=%d flatSegs=%d worstSag=%.3f\n",
		reinterpret_cast<void*>(Ziprail_ResolveHandle(chain->startHandle)),
		N, polyPoints, written, block.pathLen,
		block.useAutoDetachSpeed, block.autoDetachDistance, block.speedScale, restLen,
		nodesPinned, refined, nodesDropped,
		mins[0], mins[1], mins[2], maxs[0], maxs[1], maxs[2], flPad,
		budget, flatSegs, worstSag);

	if (starvedSegs > 0)
	{
		Warning(eDLL_T::SERVER,
			"[ZIPRAIL-PATH] start=0x%p CURVE STARVED -- %d of %d segments exceed "
			"residual tol %.2f (worst residual %.2f units, budget=%d, nodes=%d). "
			"Raise sdk_ziprail_curve_budget.\n",
			reinterpret_cast<void*>(Ziprail_ResolveHandle(chain->startHandle)),
			starvedSegs, nSeg, flTol, worstResidual, budget, N);
	}

	if (nWant > 0 && written < nWant)
	{
		Warning(eDLL_T::SERVER,
			"[ZIPRAIL-PATH] start=0x%p folded %d coincident point(s) out of the rest "
			"polyline (zero-length segment in the chain)\n",
			reinterpret_cast<void*>(Ziprail_ResolveHandle(chain->startHandle)),
			nWant - written);
	}
}

static void Ziprail_EvalPathAtArcDistance(const ZiprailChain* chain, float dist, float out[3])
{
	out[0] = out[1] = out[2] = 0.0f;
	if (!chain || chain->baked.count <= 0)
		return;

	const ZiprailBakedPath& baked = chain->baked;
	out[0] = baked.pos[0][0];
	out[1] = baked.pos[0][1];
	out[2] = baked.pos[0][2];
	if (baked.count < 2)
		return;

	if (dist < 0.0f)
		dist = 0.0f;
	const float flEnd = baked.dist[baked.count - 1];
	if (dist > flEnd)
		dist = flEnd;

	int next = 0;
	while (next < baked.count && baked.dist[next] < dist)
		++next;
	const int prev = (next > 0) ? (next - 1) : 0;
	if (next >= baked.count)
		next = baked.count - 1;

	const float flSpan = baked.dist[next] - baked.dist[prev];
	const float flPct = (flSpan > 0.0f) ? ((dist - baked.dist[prev]) / flSpan) : 0.0f;
	out[0] = baked.pos[prev][0] + (baked.pos[next][0] - baked.pos[prev][0]) * flPct;
	out[1] = baked.pos[prev][1] + (baked.pos[next][1] - baked.pos[prev][1]) * flPct;
	out[2] = baked.pos[prev][2] + (baked.pos[next][2] - baked.pos[prev][2]) * flPct;
}

static void Ziprail_EvalRestAtArcDistance(const ZiprailChain* chain, float dist, float out[3])
{
	out[0] = out[1] = out[2] = 0.0f;
	if (!chain || chain->restCount <= 0)
		return;

	out[0] = chain->rest[0][0];
	out[1] = chain->rest[0][1];
	out[2] = chain->rest[0][2];
	if (chain->restCount < 2)
		return;

	float remain = (dist > 0.0f) ? dist : 0.0f;
	for (int i = 1; i < chain->restCount; ++i)
	{
		float d[3];
		Ziprail_Vec3Sub(chain->rest[i], chain->rest[i - 1], d);
		const float len = Ziprail_Vec3Length(d);
		if (len >= remain)
		{
			const float t = (len > 0.0f) ? (remain / len) : 0.0f;
			out[0] = chain->rest[i - 1][0] + d[0] * t;
			out[1] = chain->rest[i - 1][1] + d[1] * t;
			out[2] = chain->rest[i - 1][2] + d[2] * t;
			return;
		}
		remain -= len;
	}

	out[0] = chain->rest[chain->restCount - 1][0];
	out[1] = chain->rest[chain->restCount - 1][1];
	out[2] = chain->rest[chain->restCount - 1][2];
}

static ConVar bridge_ziprail_cord_material("bridge_ziprail_cord_material", "1", FCVAR_RELEASE,
	"Stamp promoted ziprail chains with the 'cable/ziprail_cord_01' rope material index.");
static ConVar bridge_zip_spawn_partner_nudge("bridge_zip_spawn_partner_nudge", "1",
	FCVAR_RELEASE,
	"Offset a zipline's own origin by 1/1024 across CZipline::Spawn so the native "
	"partner search accepts a co-located zipline_end. 0 = stock behaviour (deployed "
	"ziplines never build a rope).");
static ConVar bridge_zip_rail_repartition("bridge_zip_rail_repartition", "1", FCVAR_RELEASE,
	"Re-derive a ziprail chain start's spatial-partition list mask once its chain is "
	"materialized. The mask is cached at spawn and carries the mount-search list bit "
	"only if IsZipline() was true then. 0 = leave the cached mask alone (rails are "
	"invisible to the mount search and cannot be ridden).");
static ConVar bridge_ziprail_path_verify("bridge_ziprail_path_verify", "0", FCVAR_DEVELOPMENTONLY,
	"Report the residual between the dedi ride polyline and the client path per chain.");

// Drop start/end zipline anchors from the published path when at least two
// non-anchor nodes remain. Members and far stay the full walk.
static int Ziprail_CopyPathDroppingAnchors(
	const float inX[], const float inY[], const float inZ[],
	const int inSmooth[], const int inTangent[],
	const ZiprailClassTag inTags[], int inCount,
	float outX[], float outY[], float outZ[],
	int outSmooth[], int outTangent[])
{
	if (!inX || !inY || !inZ || !inSmooth || !inTangent || !inTags ||
		!outX || !outY || !outZ || !outSmooth || !outTangent)
		return 0;

	int nKeep = 0;
	for (int i = 0; i < inCount; ++i)
	{
		if (inTags[i] == ZiprailClassTag::kZipline ||
			inTags[i] == ZiprailClassTag::kZiplineEnd)
			continue;
		++nKeep;
	}
	if (nKeep < 2)
		return 0;

	int n = 0;
	for (int i = 0; i < inCount && n < kZiprailMaxChainNodes; ++i)
	{
		if (inTags[i] == ZiprailClassTag::kZipline ||
			inTags[i] == ZiprailClassTag::kZiplineEnd)
			continue;
		outX[n] = inX[i];
		outY[n] = inY[i];
		outZ[n] = inZ[i];
		outSmooth[n] = inSmooth[i];
		outTangent[n] = inTangent[i];
		++n;
	}
	return n;
}

static void Ziprail_ReportRideResidual(const ZiprailChain* chain)
{
	if (!chain || !bridge_ziprail_path_verify.GetBool() || chain->baked.count < 2)
		return;
	if (chain->restCount < 2)
		return;

	const float A = chain->wire.pathLen;
	if (A <= 0.0f)
		return;

	float restLen = 0.0f;
	for (int i = 1; i < chain->restCount; ++i)
	{
		float d[3];
		Ziprail_Vec3Sub(chain->rest[i], chain->rest[i - 1], d);
		restLen += Ziprail_Vec3Length(d);
	}
	if (!(restLen > 0.0f))
		return;

	static constexpr int kSteps = 256;
	float maxErr = 0.0f;
	for (int s = 0; s <= kSteps; ++s)
	{
		const float alpha = static_cast<float>(s) / static_cast<float>(kSteps);
		float restPos[3], bakedPos[3];
		Ziprail_EvalRestAtArcDistance(chain, alpha * restLen, restPos);
		Ziprail_EvalPathAtArcDistance(chain, alpha * A, bakedPos);
		float d[3];
		Ziprail_Vec3Sub(restPos, bakedPos, d);
		const float err = Ziprail_Vec3Length(d);
		if (err > maxErr)
			maxErr = err;
	}

	Warning(eDLL_T::SERVER,
		"[ZIPRAIL-PATH] start=0x%p ride residual max=%.4fu over %d points restPts=%d\n",
		reinterpret_cast<void*>(Ziprail_ResolveHandle(chain->startHandle)),
		maxErr, chain->baked.count, chain->restCount);
}

// Materials string-table stamp for "cable/ziprail_cord_01". AddString returns existing index.

using ZiprailFindTable_t = void* (__fastcall*)(void* container, const char* tableName);
using ZiprailAddString_t = int   (__fastcall*)(void* table, char bIsServer,
	const char* value, int length, const void* userdata);

// Server-side stringtable container singleton RVA -- same global used by
// precache_natives.cpp's SkinNames/modelprecache resolvers.
static constexpr uintptr_t kZiprailStringTableContainerServer_RVA = 0xD4ECBA0;

static inline bool Ziprail_IsPlausibleHeapPtr(const void* p)
{
	const uintptr_t u = reinterpret_cast<uintptr_t>(p);
	if (!u || u == ~uintptr_t{0}) return false;
	if ((u & 0xFFFF000000000000ULL) != 0) return false; // non-canonical/kernel
	if ((u & 0x7ULL) != 0) return false;                // unaligned (vtable ptrs are 8-byte)
	return true;
}

// -1 = unresolved (retry next chain), -2 = resolve failed permanently this level
// (logged once), >=0 = resolved Materials string-table index for the cord material.
static int s_ziprailCordMaterialIndex = -1;

static int Ziprail_ResolveCordMaterialIndex()
{
	if (s_ziprailCordMaterialIndex >= 0)
		return s_ziprailCordMaterialIndex;

	const uintptr_t base = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
	void** const containerSlot = reinterpret_cast<void**>(
		base + kZiprailStringTableContainerServer_RVA);
	void* const container = *containerSlot;
	if (!Ziprail_IsPlausibleHeapPtr(container))
	{
		s_ziprailCordMaterialIndex = -2;
		return -1;
	}

	void** const containerVtable = *reinterpret_cast<void***>(container);
	if (!Ziprail_IsPlausibleHeapPtr(containerVtable))
	{
		s_ziprailCordMaterialIndex = -2;
		return -1;
	}
	const ZiprailFindTable_t FindTable =
		reinterpret_cast<ZiprailFindTable_t>(containerVtable[3]);
	if (!Ziprail_IsPlausibleHeapPtr(reinterpret_cast<void*>(FindTable)))
	{
		s_ziprailCordMaterialIndex = -2;
		return -1;
	}

	void* const table = FindTable(container, "Materials");
	if (!Ziprail_IsPlausibleHeapPtr(table))
	{
		s_ziprailCordMaterialIndex = -2;
		return -1;
	}

	void** const tableVtable = *reinterpret_cast<void***>(table);
	if (!Ziprail_IsPlausibleHeapPtr(tableVtable))
	{
		s_ziprailCordMaterialIndex = -2;
		return -1;
	}
	const ZiprailAddString_t AddString =
		reinterpret_cast<ZiprailAddString_t>(tableVtable[8]);
	if (!Ziprail_IsPlausibleHeapPtr(reinterpret_cast<void*>(AddString)))
	{
		s_ziprailCordMaterialIndex = -2;
		return -1;
	}

	// Duplicate-safe: the S21 scripts already precache "cable/ziprail_cord_01",
	// so this returns the EXISTING index without growing the table in practice.
	const int index = AddString(table, /*bIsServer*/1, "cable/ziprail_cord_01",
		-1, nullptr);
	if (index < 0)
	{
		s_ziprailCordMaterialIndex = -2;
		return -1;
	}

	s_ziprailCordMaterialIndex = index;
	return index;
}

// Stamp m_ziplineMaterialIndex (ent+0xE60) with the Materials table index.
static void Ziprail_StampCordMaterial(uintptr_t ent)
{
	if (!ent || !bridge_ziprail_cord_material.GetBool())
		return;

	const int index = Ziprail_ResolveCordMaterialIndex();
	if (index < 0)
	{
		if (s_ziprailCordMaterialIndex == -2)
		{
			static bool s_loggedFailure = false;
			if (!s_loggedFailure)
			{
				s_loggedFailure = true;
				Warning(eDLL_T::SERVER,
					"[ZIPRAIL-WIREBLK] Materials string-table resolve failed -- "
					"promoted ziprail chains will keep material index 0\n");
			}
		}
		return;
	}

	// m_ziplineMaterialIndex, server SendProp offset (DT_Zipline ctor)
	*reinterpret_cast<int*>(ent + 0xE60) = index;

	Warning(eDLL_T::SERVER,
		"[ZIPRAIL-WIREBLK] matIdx=%d stamped ent=0x%p\n",
		index, reinterpret_cast<void*>(ent));
}

// Wire block accessor. Hot path: no allocation.
const ZiprailWireBlock* ZiprailDedi_GetWireBlock(uintptr_t ent)
{
	if (!ent)
		return nullptr;

	const ZiprailWireBlock* result = nullptr;
	AcquireSRWLockShared(&s_ziprailChainLock);
	const uint32_t h = Ziprail_CaptureHandle(ent);
	for (const ZiprailChain& chain : s_ziprailChains)
	{
		if (chain.startHandle == h && chain.applied && chain.wireValid)
		{
			result = &chain.wire;
			break;
		}
	}
	ReleaseSRWLockShared(&s_ziprailChainLock);
	return result;
}

// Nearest materialized rail plus collector gates. Collector only sees ents within 120u of origin+72.
static constexpr float kZiprailMountSearchRadius = 120.0f; // collector sphere
static constexpr ptrdiff_t kZipEntOffRealmsBitMask = 0xAE8;
static constexpr ptrdiff_t kZipEntOffPlayerEyeRaise = 72;  // z added to the search origin

// Precondition gates Find/Use/MoveUpdateMount/Slide.
static constexpr ptrdiff_t kZipPlayerOffNoUseFlag   = 0x499;  // must be 0
static constexpr ptrdiff_t kZipPlayerOffSettings    = 0x5F08; // settings block ptr
static constexpr ptrdiff_t kZipPlayerOffDeny        = 0xDC0;  // must be 0
static constexpr ptrdiff_t kZipPlayerOffHeldHandle  = 0x310;  // must not resolve
static constexpr ptrdiff_t kZipPlayerOffPhase       = 0x17FC; // must be 0 or 9
static constexpr ptrdiff_t kZipPlayerOffCanZipline  = 0x67D0; // must be non-zero

static constexpr ptrdiff_t kZipVtblIsZipline    = 0x310;
static constexpr ptrdiff_t kZipVtblIsZiplineEnd = 0x320;

// Use-permission: settings "context_action_can_use" OR candidate bit 0x100000 at +0x624.
// Collector queries partition list 0x10; that bit is set iff IsZipline() at mask-compute time.
static constexpr ptrdiff_t kZipEntOffCollisionProp = 0x328;
static constexpr ptrdiff_t kZipPropOffPartitionHandle = 54;
static constexpr ptrdiff_t kZipPropOffPartitionMask   = 56;
static constexpr ptrdiff_t kZipEntOffSolidFlags      = 0x350;
static constexpr ptrdiff_t kZipEntOffSolidType       = 0x354;
static constexpr uint16_t kZipPartitionListMountSearch = 0x10;

// Balanced suspend/resume around a collision prop's partition entry. The resume
// half recomputes the partition mask and pushes it to the BVH, so
// the pair is the engine's own way of asking for a re-derive.
static void(__fastcall* v_CollisionProp_SuspendPartition)(uintptr_t prop) = nullptr;
static void(__fastcall* v_CollisionProp_ResumePartition)(uintptr_t prop) = nullptr;

// Queue the partition flush; mask rewrite alone does not push AABB into the BVH.
static void(__fastcall* v_CollisionProp_QueuePartitionUpdate)(uintptr_t prop) = nullptr;

static constexpr ptrdiff_t kZipEntOffUseFlags = 0x624;
static constexpr uint32_t kZipUseFlagContextAction = 0x100000;
static const uint32_t* g_pZipCtxActionCanUseOffset = nullptr;

// Zipline_Find's candidate collector. Hooked purely to observe: it fills two
// arrays, and whether a rail reaches them separates a collector reject from a
// Zipline_Find reject -- two different bugs with two different fixes.
static char(__fastcall* v_Zipline_CollectCandidates)(const float* pos, float radius,
	uintptr_t* outNear, int* ioNearCount, uintptr_t* outGlobal, int* ioGlobalCount) = nullptr;

static bool Ziprail_CallVtblBool(uintptr_t ent, ptrdiff_t nVtblByteOff)
{
	uintptr_t vtbl = 0;
	if (!ent || !Ziprail_ReadVtable(ent, &vtbl) || !vtbl)
		return false;

	auto pfn = *reinterpret_cast<bool(__fastcall**)(uintptr_t)>(vtbl + nVtblByteOff);
	return pfn ? pfn(ent) : false;
}

static void Ziprail_ReportZiplinePrecondition(uintptr_t player)
{
	const int nNoUse = *reinterpret_cast<const uint8_t*>(player + kZipPlayerOffNoUseFlag);
	const int nDeny = *reinterpret_cast<const uint8_t*>(player + kZipPlayerOffDeny);
	const uint32_t nHeld = *reinterpret_cast<const uint32_t*>(player + kZipPlayerOffHeldHandle);
	const int nPhase = *reinterpret_cast<const int*>(player + kZipPlayerOffPhase);
	const int nCanZip = *reinterpret_cast<const uint8_t*>(player + kZipPlayerOffCanZipline);
	const uintptr_t settings = *reinterpret_cast<const uintptr_t*>(player + kZipPlayerOffSettings);
	const uintptr_t heldEnt = Ziprail_ResolveHandle(nHeld);

	Warning(eDLL_T::SERVER,
		"[ZIPRAIL-WHYNOT] precondition: noUse@499=%d(need 0) deny@DC0=%d(need 0) "
		"held@310=0x%08X->%p(need none) phase@17FC=%d(need 0 or 9) "
		"canZip@67D0=%d(need non-zero) settings=%p -> %s\n",
		nNoUse, nDeny, nHeld, reinterpret_cast<void*>(heldEnt), nPhase, nCanZip,
		reinterpret_cast<void*>(settings),
		(nNoUse == 0 && nDeny == 0 && !heldEnt && (nPhase == 0 || nPhase == 9)
			&& nCanZip != 0) ? "the readable gates pass" : "BLOCKED HERE");
}

void ZiprailDedi_ReportNearestRail(uintptr_t player, const char* reason)
{
	if (!player || !ZiprailDedi_IsLive())
		return;

	if (!reason)
		reason = "?";

	Ziprail_ReportZiplinePrecondition(player);

	float pos[3] = {};
	if (!Ziprail_ReadEntityOrigin(player, &pos[0], &pos[1], &pos[2]))
		return;
	pos[2] += static_cast<float>(kZipEntOffPlayerEyeRaise);

	uint32_t nearestHandle = 0;
	float flNearest = FLT_MAX;
	float closest[3] = {};
	int nApplied = 0;

	AcquireSRWLockShared(&s_ziprailChainLock);
	for (const ZiprailChain& chain : s_ziprailChains)
	{
		if (!chain.applied || !chain.wireValid || chain.baked.count < 1)
			continue;
		++nApplied;

		for (int i = 0; i < chain.baked.count; ++i)
		{
			const float dx = chain.baked.pos[i][0] - pos[0];
			const float dy = chain.baked.pos[i][1] - pos[1];
			const float dz = chain.baked.pos[i][2] - pos[2];
			const float flDistSqr = (dx * dx) + (dy * dy) + (dz * dz);
			if (flDistSqr < flNearest)
			{
				flNearest = flDistSqr;
				nearestHandle = chain.startHandle;
				closest[0] = chain.baked.pos[i][0];
				closest[1] = chain.baked.pos[i][1];
				closest[2] = chain.baked.pos[i][2];
			}
		}
	}
	ReleaseSRWLockShared(&s_ziprailChainLock);

	if (nApplied == 0)
	{
		Warning(eDLL_T::SERVER,
			"[ZIPRAIL-WHYNOT] %s at (%.1f %.1f %.1f) -- no materialized "
			"rail chains exist at all\n", reason, pos[0], pos[1], pos[2]);
		return;
	}

	const float flDist = sqrtf(flNearest);
	const uintptr_t ent = Ziprail_ResolveHandle(nearestHandle);
	if (!ent)
	{
		Warning(eDLL_T::SERVER,
			"[ZIPRAIL-WHYNOT] %s at (%.1f %.1f %.1f) chains=%d nearest "
			"dist=%.1f -- its start entity no longer resolves\n",
			reason, pos[0], pos[1], pos[2], nApplied, flDist);
		return;
	}

	const int nEnabled = *reinterpret_cast<const uint8_t*>(ent + kZipEntOffZiplineEnabled);
	const uint32_t nextZip = *reinterpret_cast<const uint32_t*>(ent + kZipEntOffNextZipline);
	const uintptr_t farEnt = Ziprail_ResolveHandle(nextZip);
	const int nRestCount = *reinterpret_cast<const int*>(ent + kZipEntOffRestCount);

	// The collector's at-rest predicate: rest[0] must equal this entity's abs
	// origin and rest[last] the partner's, or it walks the simulated rope array
	// instead of the rest points we injected.
	int nSelfMatch = 0;
	int nFarMatch = 0;
	if (nRestCount >= 1 && nRestCount <= kZiprailMaxRestPoints)
	{
		float selfOrigin[3] = {};
		Ziprail_ReadEntityOrigin(ent, &selfOrigin[0], &selfOrigin[1], &selfOrigin[2]);
		const float* rest0 = reinterpret_cast<const float*>(ent + kZipEntOffRestPositions);
		nSelfMatch = Ziprail_Vec3ExactEqual(selfOrigin, rest0) ? 1 : 0;

		if (farEnt)
		{
			float farOrigin[3] = {};
			Ziprail_ReadEntityOrigin(farEnt, &farOrigin[0], &farOrigin[1], &farOrigin[2]);
			const float* restLast = reinterpret_cast<const float*>(
				ent + kZipEntOffRestPositions + static_cast<ptrdiff_t>(12) * (nRestCount - 1));
			nFarMatch = Ziprail_Vec3ExactEqual(farOrigin, restLast) ? 1 : 0;
		}
	}

	const uint64_t nEntRealms = *reinterpret_cast<const uint64_t*>(ent + kZipEntOffRealmsBitMask);
	const uint64_t nPlayerRealms = *reinterpret_cast<const uint64_t*>(player + kZipEntOffRealmsBitMask);
	const uintptr_t settings = *reinterpret_cast<const uintptr_t*>(player + kZipPlayerOffSettings);

	// The collector's very first test, and the one nothing above covers: a
	// candidate that answers false to both is skipped before any field is read.
	const int nIsZip = Ziprail_CallVtblBool(ent, kZipVtblIsZipline) ? 1 : 0;
	const int nIsZipEnd = Ziprail_CallVtblBool(ent, kZipVtblIsZiplineEnd) ? 1 : 0;

	// Broad phase: 2D distance to the straight chord rest[0]..rest[last], not to
	// the curve. A bent rail can hold the player close to the rope and still be
	// far from its own chord.
	float flChord2D = -1.0f;
	if (nRestCount >= 2 && nRestCount <= kZiprailMaxRestPoints)
	{
		const float* const rest = reinterpret_cast<const float*>(ent + kZipEntOffRestPositions);
		const float* const last = rest + static_cast<ptrdiff_t>(3) * (nRestCount - 1);
		const float dx = last[0] - rest[0];
		const float dy = last[1] - rest[1];
		const float flLenSqr = (dx * dx) + (dy * dy);

		float flT = 0.0f;
		if (flLenSqr >= 0.01f)
		{
			flT = (((pos[0] - rest[0]) * dx) + ((pos[1] - rest[1]) * dy)) / flLenSqr;
			flT = (flT < 0.0f) ? 0.0f : ((flT > 1.0f) ? 1.0f : flT);
		}

		const float ex = pos[0] - (rest[0] + (dx * flT));
		const float ey = pos[1] - (rest[1] + (dy * flT));
		flChord2D = sqrtf((ex * ex) + (ey * ey));
	}

	// The use-permission gate. A clear settings bool is a player-side problem;
	// a missing entity bit is something the promotion should be stamping.
	const uint32_t nUseFlags = *reinterpret_cast<const uint32_t*>(ent + kZipEntOffUseFlags);
	const int nEntUsable = (nUseFlags & kZipUseFlagContextAction) ? 1 : 0;
	int nCtxCanUse = -1;
	if (g_pZipCtxActionCanUseOffset && settings)
	{
		nCtxCanUse = *reinterpret_cast<const uint8_t*>(
			settings + *g_pZipCtxActionCanUseOffset) ? 1 : 0;
	}

	// Partition membership -- the collector queries list mask 0x10 and enumerates
	// nothing else, so this decides whether the entity is ever offered at all.
	const uintptr_t prop = ent + kZipEntOffCollisionProp;
	const uint16_t nPartMask = *reinterpret_cast<const uint16_t*>(prop + kZipPropOffPartitionMask);
	const uint16_t nPartHandle = *reinterpret_cast<const uint16_t*>(prop + kZipPropOffPartitionHandle);
	const uint32_t nSolidFlags = *reinterpret_cast<const uint32_t*>(ent + kZipEntOffSolidFlags);
	const uint8_t nSolidType = *reinterpret_cast<const uint8_t*>(ent + kZipEntOffSolidType);
	const uint32_t nEFlags = *reinterpret_cast<const uint32_t*>(ent + kZipEntOffEFlags);
	const bool bBoundsGate = (nSolidType && (nSolidFlags & 0x44) == 0)
		|| (nSolidFlags & 8) != 0 || (nEFlags & 0x40000) != 0;

	Warning(eDLL_T::SERVER,
		"[ZIPRAIL-WHYNOT] %s at (%.1f %.1f %.1f) chains=%d nearest=0x%p "
		"dist=%.1f (radius %.0f, %s) chord2D=%.1f(%s) isZipline=%d isZiplineEnd=%d(%s) "
		"ctxCanUse=%d entFlags624=0x%08X entUsableBit=%d(%s) "
		"partMask=0x%04X bit0x10=%d(%s) partHandle=0x%04X solidFlags=0x%08X "
		"solidType=%d boundsGate=%d "
		"closest=(%.1f %.1f %.1f) enabled=%d "
		"nextZip=0x%08X far=0x%p restCount=%d selfMatch=%d farMatch=%d "
		"realmsEnt=0x%llX realmsPlayer=0x%llX realmsAnd=%s\n",
		reason, pos[0], pos[1], pos[2], nApplied, reinterpret_cast<void*>(ent),
		flDist, kZiprailMountSearchRadius,
		flDist <= kZiprailMountSearchRadius ? "IN RANGE" : "out of range",
		flChord2D,
		(flChord2D >= 0.0f && flChord2D < kZiprailMountSearchRadius) ? "pass" : "REJECT",
		nIsZip, nIsZipEnd, (nIsZip || nIsZipEnd) ? "pass" : "REJECT",
		nCtxCanUse, nUseFlags, nEntUsable,
		(nCtxCanUse == 1 || nEntUsable) ? "pass" : "REJECT",
		nPartMask, (nPartMask & kZipPartitionListMountSearch) ? 1 : 0,
		(nPartMask & kZipPartitionListMountSearch) ? "pass" : "REJECT -- never enumerated",
		nPartHandle, nSolidFlags, nSolidType, bBoundsGate ? 1 : 0,
		closest[0], closest[1], closest[2],
		nEnabled, nextZip, reinterpret_cast<void*>(farEnt), nRestCount,
		nSelfMatch, nFarMatch,
		static_cast<unsigned long long>(nEntRealms),
		static_cast<unsigned long long>(nPlayerRealms),
		(nEntRealms & nPlayerRealms) ? "pass" : "REJECT");
}

// Engine collector output: in the arrays = accepted here, died in Zipline_Find.
static std::atomic<uint32_t> s_ziprailCollectLogs(0);
static constexpr uint32_t kZiprailCollectReportEvery = 24;
static constexpr float kZiprailCollectReportRange = 400.0f; // silence far from any rail

static ConVar sdk_ziprail_collect_diag("sdk_ziprail_collect_diag", "0",
	FCVAR_DEVELOPMENTONLY | FCVAR_GAMEDLL,
	"Log whether the engine's zipline candidate collector returned the nearest bridge ziprail.");

static ConVar bridge_zip_rail_collect_merge("bridge_zip_rail_collect_merge", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Append a promoted ziprail START to the native candidate list when the search "
	"sphere hits its path extents or baked polyline. S3 collect has no rail walk.");

static bool Ziprail_SphereHitsAabb(const float pos[3], float radius,
	const float mins[3], const float maxs[3])
{
	if (!pos || !mins || !maxs || radius < 0.0f)
		return false;
	float d[3];
	for (int a = 0; a < 3; ++a)
	{
		if (pos[a] < mins[a])
			d[a] = mins[a] - pos[a];
		else if (pos[a] > maxs[a])
			d[a] = pos[a] - maxs[a];
		else
			d[a] = 0.0f;
	}
	return (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) <= (radius * radius);
}

static float Ziprail_AabbDistSqr(const float pos[3],
	const float mins[3], const float maxs[3])
{
	float d[3];
	for (int a = 0; a < 3; ++a)
	{
		if (pos[a] < mins[a])
			d[a] = mins[a] - pos[a];
		else if (pos[a] > maxs[a])
			d[a] = pos[a] - maxs[a];
		else
			d[a] = 0.0f;
	}
	return (d[0] * d[0]) + (d[1] * d[1]) + (d[2] * d[2]);
}

static bool Ziprail_SearchHitsChain(const ZiprailChain& chain,
	const float pos[3], float radius)
{
	if (!chain.applied || !chain.wireValid || !pos)
		return false;

	const float padXy = 128.0f;
	const float padZ = 32.0f;
	const float mins[3] = {
		chain.wire.extentsMins[0] - padXy,
		chain.wire.extentsMins[1] - padXy,
		chain.wire.extentsMins[2] - padZ };
	const float maxs[3] = {
		chain.wire.extentsMaxs[0] + padXy,
		chain.wire.extentsMaxs[1] + padXy,
		chain.wire.extentsMaxs[2] + padZ };
	return Ziprail_SphereHitsAabb(pos, radius, mins, maxs);
}

static uint32_t Ziprail_NearestChainHandleTo(const float pos[3], float* outDist)
{
	uint32_t nearest = 0;
	float flBestSqr = FLT_MAX;

	AcquireSRWLockShared(&s_ziprailChainLock);
	for (const ZiprailChain& chain : s_ziprailChains)
	{
		if (!chain.applied || !chain.wireValid)
			continue;

		const float boxDistSqr = Ziprail_AabbDistSqr(pos,
			chain.wire.extentsMins, chain.wire.extentsMaxs);
		if (boxDistSqr >= flBestSqr)
			continue;

		for (int i = 0; i < chain.baked.count; ++i)
		{
			const float dx = chain.baked.pos[i][0] - pos[0];
			const float dy = chain.baked.pos[i][1] - pos[1];
			const float dz = chain.baked.pos[i][2] - pos[2];
			const float flSqr = (dx * dx) + (dy * dy) + (dz * dz);
			if (flSqr < flBestSqr)
			{
				flBestSqr = flSqr;
				nearest = chain.startHandle;
			}
		}
	}
	ReleaseSRWLockShared(&s_ziprailChainLock);

	if (outDist)
		*outDist = (flBestSqr == FLT_MAX) ? -1.0f : sqrtf(flBestSqr);
	return nearest;
}

uintptr_t ZiprailDedi_FindNearestRail(const float pos[3], float flMaxDist, float* outDist)
{
	if (!pos)
		return 0;

	float flDist = -1.0f;
	const uintptr_t ent = Ziprail_ResolveHandle(Ziprail_NearestChainHandleTo(pos, &flDist));

	if (outDist)
		*outDist = flDist;

	if (!ent || flDist < 0.0f || flDist > flMaxDist)
		return 0;

	return ent;
}

static int Ziprail_IndexOfEntity(const uintptr_t* list, int count, uintptr_t ent)
{
	if (!list || !ent)
		return -1;
	for (int i = 0; i < count; ++i)
	{
		if (list[i] == ent)
			return i;
	}
	return -1;
}

static char __fastcall Hook_Zipline_CollectCandidates(const float* pos, float radius,
	uintptr_t* outNear, int* ioNearCount, uintptr_t* outGlobal, int* ioGlobalCount)
{
	const int nNearCap = ioNearCount ? *ioNearCount : 0;
	const char result = v_Zipline_CollectCandidates(pos, radius, outNear, ioNearCount,
		outGlobal, ioGlobalCount);

	if (!pos || !ZiprailDedi_IsLive())
		return result;

	if (bridge_zip_rail_collect_merge.GetBool() && outNear && ioNearCount && nNearCap > 0)
	{
		int nNear = *ioNearCount;
		if (nNear < 0)
			nNear = 0;
		if (nNear > nNearCap)
			nNear = nNearCap;

		AcquireSRWLockShared(&s_ziprailChainLock);
		for (const ZiprailChain& chain : s_ziprailChains)
		{
			if (nNear >= nNearCap)
				break;
			if (!Ziprail_SearchHitsChain(chain, pos, radius))
				continue;
			const uintptr_t start = Ziprail_ResolveHandle(chain.startHandle);
			if (!start)
				continue;
			if (Ziprail_IndexOfEntity(outNear, nNear, start) >= 0)
				continue;
			outNear[nNear++] = start;
		}
		ReleaseSRWLockShared(&s_ziprailChainLock);
		*ioNearCount = nNear;
	}

	if (!sdk_ziprail_collect_diag.GetBool())
		return result;

	const int nNear = ioNearCount ? *ioNearCount : 0;
	const int nGlobal = ioGlobalCount ? *ioGlobalCount : 0;

	float flDist = -1.0f;
	const uintptr_t ent = Ziprail_ResolveHandle(Ziprail_NearestChainHandleTo(pos, &flDist));
	if (!ent || flDist < 0.0f || flDist > kZiprailCollectReportRange)
		return result;

	if ((s_ziprailCollectLogs.fetch_add(1, std::memory_order_relaxed)
		% kZiprailCollectReportEvery) != 0)
	{
		return result;
	}

	const int nNearIdx = Ziprail_IndexOfEntity(outNear, nNear, ent);
	const int nGlobalIdx = Ziprail_IndexOfEntity(outGlobal, nGlobal, ent);

	Warning(eDLL_T::SERVER,
		"[ZIPRAIL-COLLECT] searchAt=(%.1f %.1f %.1f) radius=%.0f near=%d global=%d "
		"nearestRail=0x%p dist=%.1f nearIdx=%d globalIdx=%d -> %s\n",
		pos[0], pos[1], pos[2], radius, nNear, nGlobal,
		reinterpret_cast<void*>(ent), flDist, nNearIdx, nGlobalIdx,
		(nNearIdx >= 0 || nGlobalIdx >= 0)
			? "collector ACCEPTED it -- Zipline_Find is what rejects"
			: "collector never returned it -- the reject is inside the collector");

	return result;
}

static const ZiprailChain* Ziprail_FindAppliedChainLocked(uint32_t h)
{
	for (const ZiprailChain& chain : s_ziprailChains)
	{
		if (chain.startHandle == h && chain.applied && chain.wireValid)
			return &chain;
	}
	return nullptr;
}

static bool Ziprail_BakedDirAtDist(const ZiprailBakedPath& baked, float pathLen,
	float dist, float outDir[3])
{
	if (!outDir || baked.count < 1)
		return false;

	if (dist < 0.0f)
		dist = 0.0f;
	if (pathLen > 0.0f && dist > pathLen)
		dist = pathLen;
	else if (baked.count > 0 && dist > baked.dist[baked.count - 1])
		dist = baked.dist[baked.count - 1];

	int next = 0;
	while (next < baked.count && baked.dist[next] < dist)
		++next;

	const int prev = (next > 0) ? (next - 1) : 0;
	if (next >= baked.count)
		next = baked.count - 1;

	const float flSpan = baked.dist[next] - baked.dist[prev];
	const float flPct = (flSpan > 0.0f) ? ((dist - baked.dist[prev]) / flSpan) : 0.0f;

	outDir[0] = baked.dir[prev][0] + (baked.dir[next][0] - baked.dir[prev][0]) * flPct;
	outDir[1] = baked.dir[prev][1] + (baked.dir[next][1] - baked.dir[prev][1]) * flPct;
	outDir[2] = baked.dir[prev][2] + (baked.dir[next][2] - baked.dir[prev][2]) * flPct;
	Ziprail_Vec3NormalizeSafe(outDir);
	return true;
}

static bool Ziprail_BakedPosAtDist(const ZiprailBakedPath& baked, float pathLen,
	float dist, float outPos[3])
{
	if (!outPos || baked.count < 1)
		return false;

	if (dist < 0.0f)
		dist = 0.0f;
	if (pathLen > 0.0f && dist > pathLen)
		dist = pathLen;
	else if (baked.count > 0 && dist > baked.dist[baked.count - 1])
		dist = baked.dist[baked.count - 1];

	int next = 0;
	while (next < baked.count && baked.dist[next] < dist)
		++next;

	const int prev = (next > 0) ? (next - 1) : 0;
	if (next >= baked.count)
		next = baked.count - 1;

	const float flSpan = baked.dist[next] - baked.dist[prev];
	const float flPct = (flSpan > 0.0f) ? ((dist - baked.dist[prev]) / flSpan) : 0.0f;

	outPos[0] = baked.pos[prev][0] + (baked.pos[next][0] - baked.pos[prev][0]) * flPct;
	outPos[1] = baked.pos[prev][1] + (baked.pos[next][1] - baked.pos[prev][1]) * flPct;
	outPos[2] = baked.pos[prev][2] + (baked.pos[next][2] - baked.pos[prev][2]) * flPct;
	return true;
}

bool ZiprailDedi_GetPathDirectionAtArcDistance(uintptr_t ent, float dist, float outDir[3])
{
	if (!ent || !outDir)
		return false;

	bool ok = false;
	AcquireSRWLockShared(&s_ziprailChainLock);
	const ZiprailChain* const chain = Ziprail_FindAppliedChainLocked(Ziprail_CaptureHandle(ent));
	if (chain && chain->baked.count > 0)
		ok = Ziprail_BakedDirAtDist(chain->baked, chain->wire.pathLen, dist, outDir);
	ReleaseSRWLockShared(&s_ziprailChainLock);
	return ok;
}

bool ZiprailDedi_GetPathPointAtArcDistance(uintptr_t ent, float dist, float outPos[3])
{
	if (!ent || !outPos)
		return false;

	bool ok = false;
	AcquireSRWLockShared(&s_ziprailChainLock);
	const ZiprailChain* const chain = Ziprail_FindAppliedChainLocked(Ziprail_CaptureHandle(ent));
	if (chain && chain->baked.count > 0)
		ok = Ziprail_BakedPosAtDist(chain->baked, chain->wire.pathLen, dist, outPos);
	ReleaseSRWLockShared(&s_ziprailChainLock);
	return ok;
}

// Squared distance from P to the closest point on segment AB; t in [0,1].
static float Ziprail_DistSqrToLineSegment(const float p[3], const float a[3],
	const float b[3], float* outT)
{
	float ab[3];
	Ziprail_Vec3Sub(b, a, ab);
	float ap[3];
	Ziprail_Vec3Sub(p, a, ap);
	const float ab2 = Ziprail_Vec3Dot(ab, ab);
	float t = (ab2 > 0.0f) ? (Ziprail_Vec3Dot(ap, ab) / ab2) : 0.0f;
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	if (outT)
		*outT = t;

	const float q[3] = {
		a[0] + t * ab[0],
		a[1] + t * ab[1],
		a[2] + t * ab[2] };
	const float d[3] = { p[0] - q[0], p[1] - q[1], p[2] - q[2] };
	return Ziprail_Vec3Dot(d, d);
}

bool ZiprailDedi_ClosestPointOnPath(uintptr_t ent, const float pos[3],
	float outClosest[3], float outDir[3], float* outArcDist)
{
	if (!ent || !pos || !outClosest || !outDir || !outArcDist)
		return false;

	bool ok = false;
	AcquireSRWLockShared(&s_ziprailChainLock);
	const ZiprailChain* const chain = Ziprail_FindAppliedChainLocked(Ziprail_CaptureHandle(ent));
	if (chain && chain->baked.count >= 2)
	{
		const ZiprailBakedPath& baked = chain->baked;
		float bestD2 = FLT_MAX;
		float bestT = 0.0f;
		int bestI = 0;

		for (int i = 0; i < baked.count - 1; ++i)
		{
			float t = 0.0f;
			const float d2 = Ziprail_DistSqrToLineSegment(pos, baked.pos[i],
				baked.pos[i + 1], &t);
			if (d2 < bestD2)
			{
				bestD2 = d2;
				bestT = t;
				bestI = i;
			}
		}

		outClosest[0] = baked.pos[bestI][0]
			+ (baked.pos[bestI + 1][0] - baked.pos[bestI][0]) * bestT;
		outClosest[1] = baked.pos[bestI][1]
			+ (baked.pos[bestI + 1][1] - baked.pos[bestI][1]) * bestT;
		outClosest[2] = baked.pos[bestI][2]
			+ (baked.pos[bestI + 1][2] - baked.pos[bestI][2]) * bestT;

		*outArcDist = baked.dist[bestI]
			+ (baked.dist[bestI + 1] - baked.dist[bestI]) * bestT;

		ok = Ziprail_BakedDirAtDist(baked, chain->wire.pathLen, *outArcDist, outDir);
	}
	else if (chain && chain->baked.count == 1)
	{
		outClosest[0] = chain->baked.pos[0][0];
		outClosest[1] = chain->baked.pos[0][1];
		outClosest[2] = chain->baked.pos[0][2];
		outDir[0] = chain->baked.dir[0][0];
		outDir[1] = chain->baked.dir[0][1];
		outDir[2] = chain->baked.dir[0][2];
		*outArcDist = chain->baked.dist[0];
		ok = true;
	}
	ReleaseSRWLockShared(&s_ziprailChainLock);
	return ok;
}

// Pre-original Activate: inject rest points before native rope-build. Parked when promote is off.
static void ZiprailDedi_TryMaterialize(uintptr_t ent)
{
	if (!ent || !DTExtend_IsZiprailPromoteEnabled())
		return;

	ZiprailEntityMeta startCopy = {};
	bool isStart = false;
	AcquireSRWLockShared(&s_ziprailMetaLock);
	if (const ZiprailEntityMeta* meta = Ziprail_FindByEntLocked(ent))
	{
		if (meta->isZiprailStart)
		{
			startCopy = *meta;
			isStart = true;
		}
	}
	ReleaseSRWLockShared(&s_ziprailMetaLock);

	if (!isStart)
		return; // not a chain start -- cheap return, this hook fires for every zipline

	// Idempotent: a chain already applied for this start doesn't get re-walked.
	AcquireSRWLockShared(&s_ziprailChainLock);
	if (const ZiprailChain* existing = Ziprail_FindChainByStartLocked(ent))
	{
		if (existing->applied)
		{
			ReleaseSRWLockShared(&s_ziprailChainLock);
			return;
		}
	}
	ReleaseSRWLockShared(&s_ziprailChainLock);

	float nodeX[kZiprailMaxChainNodes];
	float nodeY[kZiprailMaxChainNodes];
	float nodeZ[kZiprailMaxChainNodes];
	int numSmooth[kZiprailMaxChainNodes];
	int tangentType[kZiprailMaxChainNodes];
	uint32_t nodeHandles[kZiprailMaxChainNodes] = {};
	ZiprailClassTag nodeTags[kZiprailMaxChainNodes] = {};
	int walkAutoDetach = -1;
	float walkMountRev = -1.0f;
	uintptr_t farEnt = 0;

	AcquireSRWLockShared(&s_ziprailMetaLock);
	const int nodeCount = Ziprail_WalkChainLocked(&startCopy, nodeX, nodeY, nodeZ,
		numSmooth, tangentType, nodeHandles, nodeTags, &walkAutoDetach,
		&walkMountRev, &farEnt);
	ReleaseSRWLockShared(&s_ziprailMetaLock);

	if (nodeCount < 2 || !farEnt)
	{
		const uint32_t n = s_ziprailChainWalkFailLogs.fetch_add(1, std::memory_order_relaxed);
		if (n < 96 || (n % 128) == 0)
		{
			int nextMatches = 0;
			AcquireSRWLockShared(&s_ziprailMetaLock);
			for (const ZiprailEntityMeta& candidate : s_ziprailMeta)
			{
				if (candidate.handle && candidate.hasLinkToGuid &&
					candidate.linkToGuid == startCopy.guid)
				{
					++nextMatches;
				}
			}
			ReleaseSRWLockShared(&s_ziprailMetaLock);

			Warning(eDLL_T::SERVER,
				"[ZIPRAIL-DEDI] chain walk not ready #%u start=0x%p nodeCount=%d "
				"startGuid=0x%llX nextMatches=%d -- deferring\n",
				n + 1, reinterpret_cast<void*>(ent), nodeCount,
				static_cast<unsigned long long>(startCopy.guid), nextMatches);
		}
		return;
	}

	// Register the chain (applied=false) before injecting/linking.
	AcquireSRWLockExclusive(&s_ziprailChainLock);
	ZiprailChain* chain = Ziprail_FindOrCreateChainLocked(ent);
	if (chain)
	{
		chain->startHandle = Ziprail_CaptureHandle(ent);
		chain->startGuid = startCopy.guid;
		chain->memberCount = nodeCount;
		for (int i = 0; i < nodeCount; ++i)
		{
			chain->memberHandles[i] = nodeHandles[i];
			chain->memberTags[i] = nodeTags[i];
		}
		chain->farHandle = Ziprail_CaptureHandle(farEnt);
		chain->useAutoDetachSpeed = startCopy.hasUseAutoDetachSpeed
			? startCopy.useAutoDetachSpeed
			: ((walkAutoDetach >= 0) ? walkAutoDetach : 0);
		chain->mountReverseDistance = startCopy.hasMountReverseDistance
			? startCopy.mountReverseDistance
			: ((walkMountRev >= 0.0f) ? walkMountRev : kZiprailDefaultMountReverseDist);
		if (chain->mountReverseDistance <= 0.0f)
			chain->mountReverseDistance = kZiprailDefaultMountReverseDist;
		chain->autoDetachDistance = startCopy.hasAutoDetachDistance
			? startCopy.autoDetachDistance : 0.0f;
		chain->speedScale = (startCopy.hasSpeedScale && startCopy.speedScale > 0.0f)
			? startCopy.speedScale : 1.0f;
		chain->width = startCopy.hasWidth ? startCopy.width : 0.0f;
		chain->fadeDist = startCopy.hasFadeDist ? startCopy.fadeDist : 0.0f;
		chain->lengthScale = (startCopy.hasLengthScale && startCopy.lengthScale > 0.0f)
			? startCopy.lengthScale : 1.0f;
		chain->dropToBottom = startCopy.dropToBottom;
		chain->detachEndOnUse = startCopy.detachEndOnUse;
		chain->detachEndOnSpawn = startCopy.detachEndOnSpawn;
		chain->vertical = startCopy.vertical;
		chain->pushOffX = startCopy.pushOffX;
		chain->preserveVelocity = startCopy.preserveVelocity;
		chain->preventManualDetach = startCopy.preventManualDetach;
		chain->hasAutoDetachDistance = startCopy.hasAutoDetachDistance;
		chain->hasSpeedScale = startCopy.hasSpeedScale;
		chain->hasWidth = startCopy.hasWidth;
		chain->hasFadeDist = startCopy.hasFadeDist;
		chain->hasLengthScale = startCopy.hasLengthScale;
		chain->hasDropToBottom = startCopy.hasDropToBottom;
		chain->hasDetachEndOnUse = startCopy.hasDetachEndOnUse;
		chain->hasDetachEndOnSpawn = startCopy.hasDetachEndOnSpawn;
		chain->hasVertical = startCopy.hasVertical;
		chain->hasPushOffX = startCopy.hasPushOffX;
		chain->hasPreserveVelocity = startCopy.hasPreserveVelocity;
		chain->hasPreventManualDetach = startCopy.hasPreventManualDetach;
		if (!startCopy.hasUseAutoDetachSpeed && walkAutoDetach >= 0)
		{
			Warning(eDLL_T::SERVER,
				"[ZIPRAIL-DEDI] start=0x%p useAutoDetachSpeed=%d adopted from a chain "
				"member (the start entity does not author the key)\n",
				reinterpret_cast<void*>(ent), walkAutoDetach);
		}

		float pathX[kZiprailMaxChainNodes];
		float pathY[kZiprailMaxChainNodes];
		float pathZ[kZiprailMaxChainNodes];
		int pathSmooth[kZiprailMaxChainNodes];
		int pathTangent[kZiprailMaxChainNodes];
		const int pathCount = Ziprail_CopyPathDroppingAnchors(
			nodeX, nodeY, nodeZ, numSmooth, tangentType, nodeTags, nodeCount,
			pathX, pathY, pathZ, pathSmooth, pathTangent);
		if (pathCount >= 2)
		{
			chain->nodeCount = pathCount;
			for (int i = 0; i < pathCount; ++i)
			{
				chain->nodesX[i] = pathX[i];
				chain->nodesY[i] = pathY[i];
				chain->nodesZ[i] = pathZ[i];
				chain->numSmooth[i] = pathSmooth[i];
				chain->tangentType[i] = pathTangent[i];
			}
			Warning(eDLL_T::SERVER,
				"[ZIPRAIL-PATH] start=0x%p train-nodes-only path=%d walked=%d far=0x%p\n",
				reinterpret_cast<void*>(ent), pathCount, nodeCount,
				reinterpret_cast<void*>(farEnt));
		}
		else
		{
			chain->nodeCount = nodeCount;
			for (int i = 0; i < nodeCount; ++i)
			{
				chain->nodesX[i] = nodeX[i];
				chain->nodesY[i] = nodeY[i];
				chain->nodesZ[i] = nodeZ[i];
				chain->numSmooth[i] = numSmooth[i];
				chain->tangentType[i] = tangentType[i];
			}
			Warning(eDLL_T::SERVER,
				"[ZIPRAIL-PATH] start=0x%p fewer than 2 non-anchor nodes "
				"(walked=%d) -- path keeps walked anchors\n",
				reinterpret_cast<void*>(ent), nodeCount);
		}

		Ziprail_ComputeWireBlock(chain);
		Ziprail_ReportRideResidual(chain);
	}
	ReleaseSRWLockExclusive(&s_ziprailChainLock);

	if (!chain)
		return;

	// Stamp the networked rope material index now that materialization is
	// final (touches entity memory, so it stays outside the chain lock).
	Ziprail_StampCordMaterial(ent);

	// Inject the full (possibly resampled) node polyline as "_zipline_rest_point_%d"
	// through the NATIVE parser -- explicit indices 0..k-1, safe per verified
	// ( index-addresses the CUtlVector at ent+0x1058, count ent+0x1138).
	for (int i = 0; i < chain->restCount; ++i)
	{
		char key[40];
		char val[96];
		snprintf(key, sizeof(key), "_zipline_rest_point_%d", i);
		// %.9g is the shortest decimal that round-trips a float exactly; %g would
		// round to 6 digits and break the exact-compare at-rest test.
		snprintf(val, sizeof(val), "%.9g %.9g %.9g",
			chain->rest[i][0], chain->rest[i][1], chain->rest[i][2]);
		if (v_ZiplineKeyValueDedi)
			v_ZiplineKeyValueDedi(ent, key, val);
	}
}

static void Ziprail_StampAuthoredTable(uintptr_t ent, const ZiprailChain* chain)
{
	if (!ent || !chain)
		return;

	if (chain->hasDetachEndOnUse)
		*reinterpret_cast<uint8_t*>(ent + kZipEntOffDetachEndOnUse) =
			chain->detachEndOnUse ? 1 : 0;
	if (chain->hasDropToBottom)
		*reinterpret_cast<uint8_t*>(ent + kZipEntOffDropToBottom) =
			chain->dropToBottom ? 1 : 0;
	if (chain->hasAutoDetachDistance)
		*reinterpret_cast<float*>(ent + kZipEntOffAutoDetachDist) =
			chain->autoDetachDistance;
	if (chain->hasPushOffX)
		*reinterpret_cast<uint8_t*>(ent + kZipEntOffVertPushOff) =
			chain->pushOffX ? 1 : 0;
	if (chain->hasPreserveVelocity)
		*reinterpret_cast<uint8_t*>(ent + kZipEntOffVertPreserve) =
			chain->preserveVelocity ? 1 : 0;
	if (chain->hasWidth)
		*reinterpret_cast<float*>(ent + kZipEntOffWidth) = chain->width;
	if (chain->hasFadeDist)
		*reinterpret_cast<float*>(ent + kZipEntOffFadeDist) = chain->fadeDist;
	if (chain->hasSpeedScale)
		*reinterpret_cast<float*>(ent + kZipEntOffSpeedScale) = chain->speedScale;
	if (chain->hasLengthScale)
		*reinterpret_cast<float*>(ent + kZipEntOffLengthScale) = chain->lengthScale;
	if (chain->hasDetachEndOnSpawn)
		*reinterpret_cast<uint8_t*>(ent + kZipEntOffDetachEndOnSpawn) =
			chain->detachEndOnSpawn ? 1 : 0;

	Msg(eDLL_T::SERVER,
		"[ZIPRAIL-TABLE] start=0x%p speedScale=%.3f autoDetach=%.1f "
		"useAutoDetach=%d dropToBottom=%d width=%.2f fade=%.0f lenScale=%.3f "
		"pushOff=%d preserve=%d detachEnd=%d mountRev=%.0f preventManual=%d\n",
		reinterpret_cast<void*>(ent),
		chain->speedScale, chain->autoDetachDistance,
		chain->useAutoDetachSpeed, chain->dropToBottom ? 1 : 0,
		chain->width, chain->fadeDist, chain->lengthScale,
		chain->pushOffX ? 1 : 0, chain->preserveVelocity ? 1 : 0,
		chain->detachEndOnUse ? 1 : 0, chain->mountReverseDistance,
		chain->preventManualDetach ? 1 : 0);
}

// Mount-search census at PostActivate. Report only; do not overwrite m_ziplineEnabled or dirty origin.
static void Ziprail_ReportSpawnGate(uintptr_t ent, uint32_t farHandle)
{
	if (!ent)
		return;

	const uint8_t enabled = *reinterpret_cast<const uint8_t*>(ent + kZipEntOffZiplineEnabled);
	const uint8_t physInit = *reinterpret_cast<const uint8_t*>(ent + kZipEntOffPhysInit);
	const int ropeNodes = *reinterpret_cast<const int*>(ent + kZipEntOffRopeNodeCount);
	const int restCount = *reinterpret_cast<const int*>(ent + kZipEntOffRestCount);
	const uint32_t nextZip = *reinterpret_cast<const uint32_t*>(ent + kZipEntOffNextZipline);
	const int eflags = *reinterpret_cast<const int*>(ent + kZipEntOffEFlags);

	float selfOrigin[3] = {};
	Ziprail_ReadEntityOrigin(ent, &selfOrigin[0], &selfOrigin[1], &selfOrigin[2]);

	const uintptr_t farEnt = Ziprail_ResolveHandle(farHandle);
	float farOrigin[3] = {};
	if (farEnt)
		Ziprail_ReadEntityOrigin(farEnt, &farOrigin[0], &farOrigin[1], &farOrigin[2]);

	bool selfMatch = false;
	bool farMatch = false;
	if (restCount >= 1 && restCount <= 16)
	{
		const float* rest0 = reinterpret_cast<const float*>(ent + kZipEntOffRestPositions);
		const float* restLast = reinterpret_cast<const float*>(
			ent + kZipEntOffRestPositions + static_cast<ptrdiff_t>(12) * (restCount - 1));
		selfMatch = Ziprail_Vec3ExactEqual(selfOrigin, rest0);
		farMatch = farEnt != 0 && Ziprail_Vec3ExactEqual(farOrigin, restLast);
	}

	const bool selfDirty = (eflags & kZipEFlagsAbsDirty) != 0;
	bool farDirty = false;
	if (farEnt)
	{
		const int farEflags = *reinterpret_cast<const int*>(farEnt + kZipEntOffEFlags);
		farDirty = (farEflags & kZipEFlagsAbsDirty) != 0;
	}

	const uint32_t n = s_ziprailSpawnLogs.fetch_add(1, std::memory_order_relaxed);
	if (n < 32 || (n % 128) == 0)
	{
		Warning(eDLL_T::SERVER,
			"[ZIPRAIL-SPAWN] ent=0x%p enabled=%u physInit=%u ropeNodes=%d "
			"restCount=%d nextZip=0x%08X selfMatch=%d farMatch=%d selfDirty=%d farDirty=%d "
			"far=0x%p\n",
			reinterpret_cast<void*>(ent),
			static_cast<unsigned>(enabled),
			static_cast<unsigned>(physInit),
			ropeNodes,
			restCount,
			nextZip,
			selfMatch ? 1 : 0,
			farMatch ? 1 : 0,
			selfDirty ? 1 : 0,
			farDirty ? 1 : 0,
			reinterpret_cast<void*>(farEnt));
	}
}

static bool Ziprail_MemberIsCZipline(ZiprailClassTag tag, uintptr_t member)
{
	if (tag != ZiprailClassTag::kZipline || !member)
		return false;
	const uintptr_t vt = s_ziplineVtable.load(std::memory_order_relaxed);
	if (vt && *reinterpret_cast<const uintptr_t*>(member) != vt)
		return false;
	return true;
}

static void Ziprail_HideChainMembers(const ZiprailChain* chain, uintptr_t startEnt)
{
	if (!chain || !sdk_ziprail_hide_members.GetBool())
		return;

	int nHidden = 0;
	int nSkipped = 0;
	for (int i = 0; i < chain->memberCount && i < kZiprailMaxChainNodes; ++i)
	{
		const uint32_t h = chain->memberHandles[i];
		if (h == 0 || h == chain->startHandle)
			continue;
		const uintptr_t member = Ziprail_ResolveHandle(h);
		if (!member || member == startEnt)
			continue;
		if (!Ziprail_MemberIsCZipline(chain->memberTags[i], member))
		{
			++nSkipped;
			continue;
		}
		*reinterpret_cast<uint8_t*>(member + kZipEntOffZiplineEnabled) = 0;
		*reinterpret_cast<int*>(member + kZipEntOffRestCount) = 0;
		*reinterpret_cast<int*>(member + kZipEntOffRopeShapeCount) = 0;
		MarkEntityEdictDirty(reinterpret_cast<void*>(member));
		++nHidden;
	}

	Warning(eDLL_T::SERVER,
		"[ZIPRAIL-MEMBERS] start=0x%p hid %d CZipline members "
		"(skipped %d train/end) of %d chain ents\n",
		reinterpret_cast<void*>(startEnt), nHidden, nSkipped, chain->memberCount);
}

// After original Activate: if partner resolution failed, mirror rest points and Init physics.
static void ZiprailDedi_PostActivate(uintptr_t ent)
{
	if (!ent || !DTExtend_IsZiprailPromoteEnabled())
		return;

	ZiprailChain chainCopy = {};
	bool haveChain = false;
	AcquireSRWLockShared(&s_ziprailChainLock);
	if (const ZiprailChain* chain = Ziprail_FindChainByStartLocked(ent))
	{
		if (!chain->applied)
		{
			chainCopy = *chain;
			haveChain = true;
		}
	}
	ReleaseSRWLockShared(&s_ziprailChainLock);

	if (!haveChain)
		return;

	// Live start ent (null + sweep vtable gate); pure POD after null.
	uint8_t physInit = *reinterpret_cast<uint8_t*>(ent + 0xB18);
	int ropeNodes = *reinterpret_cast<int*>(ent + 0xD38);

	// Rest is the train path. Native Activate may have built an origin-to-far
	// chord because the mount post is not a path node -- always author ours.
	{
		for (int i = 0; i < chainCopy.restCount && i < kZiprailMaxRestPoints; ++i)
		{
			float* dst = reinterpret_cast<float*>(ent + 0xE80 + static_cast<uintptr_t>(i) * 12);
			dst[0] = chainCopy.rest[i][0];
			dst[1] = chainCopy.rest[i][1];
			dst[2] = chainCopy.rest[i][2];
		}
		*reinterpret_cast<int*>(ent + 0xF40) = chainCopy.restCount;

		if (v_CZiplinePhysicsInitDedi)
		{
			__try
			{
				v_CZiplinePhysicsInitDedi(
					reinterpret_cast<void*>(ent + 0xB10), chainCopy.rest,
					static_cast<unsigned int>(chainCopy.restCount),
					reinterpret_cast<void*>(ent));
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
			}
		}
		else
		{
			Warning(eDLL_T::SERVER,
				"[ZIPRAIL-DEDI] CZiplinePhysics::Init pattern unresolved -- chain "
				"start=0x%p stays a dead zipline (no manual physics init)\n",
				reinterpret_cast<void*>(ent));
		}

		physInit = *reinterpret_cast<uint8_t*>(ent + 0xB18);
		ropeNodes = *reinterpret_cast<int*>(ent + 0xD38);

		if (v_ZiplinePublishRopeShapeDedi && ropeNodes >= 2)
		{
			__try
			{
				// Shape is published straight from the physics nodes with no
				// relaxation pass -- a rail does not sag.
				v_ZiplinePublishRopeShapeDedi(ent);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
			}

			const int pubCount = *reinterpret_cast<const int*>(ent + kZipEntOffRopeNodeCount);
			const int shapeCount = *reinterpret_cast<const int*>(ent + kZipEntOffRopeShapeCount);
			if (shapeCount != pubCount)
			{
				Warning(eDLL_T::SERVER,
					"[ZIPRAIL-DEDI] rope shape count mirror mismatch start=0x%p "
					"pubCount=%d shapeCount=%d\n",
					reinterpret_cast<void*>(ent), pubCount, shapeCount);
			}

			float ropeLen = 0.f;
			float first[3] = {};
			float last[3] = {};
			if (pubCount >= 2)
			{
				const float* dists = reinterpret_cast<const float*>(
					ent + kZipEntOffSegmentDistances);
				const int nSeg = pubCount - 1;
				for (int i = 0; i < nSeg && i < 16; ++i)
					ropeLen += dists[i];

				const float* ride = reinterpret_cast<const float*>(
					ent + kZipEntOffRidePositions);
				const int nBound = (pubCount < 16) ? pubCount : 16;
				first[0] = ride[0];
				first[1] = ride[1];
				first[2] = ride[2];
				const int iLast = nBound - 1;
				last[0] = ride[iLast * 3 + 0];
				last[1] = ride[iLast * 3 + 1];
				last[2] = ride[iLast * 3 + 2];
			}

			if (ropeLen > 0.f)
			{
				Msg(eDLL_T::SERVER,
					"[ZIPRAIL-DEDI] published rope shape start=0x%p nodes=%d "
					"length=%.3f first=(%.1f %.1f %.1f) last=(%.1f %.1f %.1f)\n",
					reinterpret_cast<void*>(ent), pubCount, ropeLen,
					first[0], first[1], first[2],
					last[0], last[1], last[2]);
			}
			else
			{
				Warning(eDLL_T::SERVER,
					"[ZIPRAIL-DEDI] rope still degenerate after publish "
					"start=0x%p nodes=%d length=%.3f\n",
					reinterpret_cast<void*>(ent), pubCount, ropeLen);
			}
		}
	}

	// Networked m_nextZipline (ent+0xE68) -> far-end entity handle. NEVER touch
	// ent+0xB74/ent+0xD68 -- those offsets are CZiplinePhysics node memory on the
	// server (client-side-only fields on the wire twin), not a runtime link slot here.
	const uint32_t farHandle = chainCopy.farHandle;
	if (farHandle != 0 && farHandle != INVALID_EHANDLE_INDEX)
		*reinterpret_cast<uint32_t*>(ent + 0xE68) = farHandle;

	// Reverse link: native CZipline::Spawn's second-endpoint pass stamps
	// *(ent+0xE64) with the partner handle. Resolve far end via EHANDLE first.
	const uint32_t startHandle = Ziprail_CaptureHandle(ent);
	const uintptr_t farLive = Ziprail_ResolveHandle(chainCopy.farHandle);
	if (startHandle != INVALID_EHANDLE_INDEX && farLive)
		*reinterpret_cast<uint32_t*>(farLive + 0xE64) = startHandle;

	// Networked rest positions last (Init resets the count). Array at +3712, count at +3904.
	int restWritten = 0;
	for (int i = 0; i < chainCopy.restCount && i < kZiprailMaxRestPoints; ++i)
	{
		float* dst = reinterpret_cast<float*>(ent + 0xE80 + static_cast<uintptr_t>(i) * 12);
		dst[0] = chainCopy.rest[i][0];
		dst[1] = chainCopy.rest[i][1];
		dst[2] = chainCopy.rest[i][2];
	}
	restWritten = (chainCopy.restCount < kZiprailMaxRestPoints)
		? chainCopy.restCount : kZiprailMaxRestPoints;
	*reinterpret_cast<int*>(ent + 0xF40) = restWritten;

	Ziprail_StampAuthoredTable(ent, &chainCopy);

	MarkEntityEdictDirty(reinterpret_cast<void*>(ent));
	if (uintptr_t farLiveDirty = Ziprail_ResolveHandle(chainCopy.farHandle))
		MarkEntityEdictDirty(reinterpret_cast<void*>(farLiveDirty));

	// Re-derive partition list via suspend/resume; do not write the mask. Bit 0x10 from IsZipline().
	if (bridge_zip_rail_repartition.GetBool()
		&& v_CollisionProp_SuspendPartition && v_CollisionProp_ResumePartition)
	{
		const uintptr_t prop = ent + kZipEntOffCollisionProp;
		__try
		{
			v_CollisionProp_SuspendPartition(prop);
			v_CollisionProp_ResumePartition(prop);

			// Resume refreshes the mask but leaves the leaf's bounds as spawn set
			// them; only the deferred flush computes and pushes an AABB.
			if (v_CollisionProp_QueuePartitionUpdate)
				v_CollisionProp_QueuePartitionUpdate(prop);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			Warning(eDLL_T::SERVER,
				"[ZIPRAIL-DEDI] partition re-derive raised on chain start=0x%p -- the "
				"rope is built but the mount search cannot see it\n",
				reinterpret_cast<void*>(ent));
		}
	}

	Ziprail_ReportSpawnGate(ent, chainCopy.farHandle);

	DTExtend_QueueNativeZiplineAsZiprail(ent, "ziprail chain start");

	AcquireSRWLockExclusive(&s_ziprailChainLock);
	if (ZiprailChain* chain = Ziprail_FindChainByStartLocked(ent))
		chain->applied = true;
	ReleaseSRWLockExclusive(&s_ziprailChainLock);

	Ziprail_HideChainMembers(&chainCopy, ent);

	{
		const ZiprailWireBlock* wire = ZiprailDedi_GetWireBlock(ent);
		if (!wire)
		{
			Warning(eDLL_T::SERVER,
				"[ZIPRAIL-WIRE] start=0x%p wire lookup fails for a chain that was "
				"just applied -- every path prop will encode zeros\n",
				reinterpret_cast<void*>(ent));
		}
		else if (wire->numNodes < 2 || wire->pathLen <= 0.0f)
		{
			Warning(eDLL_T::SERVER,
				"[ZIPRAIL-WIRE] start=0x%p wire block degenerate after apply "
				"nodes=%d pathLen=%.3f\n",
				reinterpret_cast<void*>(ent),
				wire->numNodes, wire->pathLen);
		}
		else
		{
			// The client reads a rail's abs origin to turn the published extents
			// into local bounds. Report what this side holds so both halves of
			// that subtraction are on the record.
			const float* const org =
				reinterpret_cast<const float*>(ent + kZipEntOffLocalOrigin);

			Msg(eDLL_T::SERVER,
				"[ZIPRAIL-WIRE] start=0x%p lookup ok nodes=%d pathLen=%.3f "
				"useAutoDetach=%d org=(%.1f %.1f %.1f) node0=(%.1f %.1f %.1f)\n",
				reinterpret_cast<void*>(ent),
				wire->numNodes, wire->pathLen, wire->useAutoDetachSpeed,
				org[0], org[1], org[2],
				wire->positions[0][0], wire->positions[0][1], wire->positions[0][2]);

			if (!sdk_ziprail_publish_bounds.GetBool())
			{
				Warning(eDLL_T::SERVER,
					"[ZIPRAIL-BOUNDS] start=0x%p skip -- sdk_ziprail_publish_bounds 0\n",
					reinterpret_cast<void*>(ent));
			}
			else
			{
				const bool orgZero =
					org[0] == 0.0f && org[1] == 0.0f && org[2] == 0.0f;
				const bool extentsNonZero =
					wire->extentsMins[0] != 0.0f || wire->extentsMins[1] != 0.0f
					|| wire->extentsMins[2] != 0.0f
					|| wire->extentsMaxs[0] != 0.0f || wire->extentsMaxs[1] != 0.0f
					|| wire->extentsMaxs[2] != 0.0f;
				if (orgZero && extentsNonZero)
				{
					Warning(eDLL_T::SERVER,
						"[ZIPRAIL-BOUNDS] start=0x%p skip -- entity origin is "
						"(0 0 0) while path extents are non-zero; publishing "
						"would write world-space into local bounds\n",
						reinterpret_cast<void*>(ent));
				}
				else
				{
					CBaseEntity* const pBase =
						reinterpret_cast<CBaseEntity*>(ent);
					CCollisionProperty* const pColl = pBase->CollisionProp();
					if (!pColl)
					{
						Warning(eDLL_T::SERVER,
							"[ZIPRAIL-BOUNDS] start=0x%p skip -- CollisionProp null\n",
							reinterpret_cast<void*>(ent));
					}
					else
					{
						// Collision bounds are entity-local; published extents are world space.
						const Vector3D localMins(
							wire->extentsMins[0] - org[0],
							wire->extentsMins[1] - org[1],
							wire->extentsMins[2] - org[2]);
						const Vector3D localMaxs(
							wire->extentsMaxs[0] - org[0],
							wire->extentsMaxs[1] - org[1],
							wire->extentsMaxs[2] - org[2]);
						pColl->SetBounds(localMins, localMaxs);
						pColl->ExcludeFromSpatialQueries();
						MarkEntityEdictDirty(reinterpret_cast<void*>(ent));

						Msg(eDLL_T::SERVER,
							"[ZIPRAIL-BOUNDS] start=0x%p local=(%.0f %.0f %.0f)-"
							"(%.0f %.0f %.0f) org=(%.1f %.1f %.1f)\n",
							reinterpret_cast<void*>(ent),
							localMins.x, localMins.y, localMins.z,
							localMaxs.x, localMaxs.y, localMaxs.z,
							org[0], org[1], org[2]);
					}
				}
			}
		}
	}

	const uint32_t n = s_ziprailChainAppliedLogs.fetch_add(1, std::memory_order_relaxed);
	if (n < 32 || (n % 128) == 0)
	{
		const int16_t edictIdx = *reinterpret_cast<int16_t*>(ent + 0x58);
		const int restField = *reinterpret_cast<int*>(ent + 0xF40);
		const float* r = reinterpret_cast<const float*>(ent + 0xE80);
		const float rest0[3] = { r[0], r[1], r[2] };
		Warning(eDLL_T::SERVER,
			"[ZIPRAIL-DEDI] chain start=0x%p edict=%d nodes=%d rest=%d restField@F40=%d "
			"rest0=(%.1f %.1f %.1f) far=0x%p farHandle=0x%08X physInit=%d ropeNodes=%d\n",
			reinterpret_cast<void*>(ent), (int)edictIdx,
			chainCopy.nodeCount, chainCopy.restCount, restField,
			rest0[0], rest0[1], rest0[2],
			reinterpret_cast<void*>(Ziprail_ResolveHandle(chainCopy.farHandle)), farHandle,
			static_cast<int>(physInit), ropeNodes);
	}
}

// ZiprailDedi_ReportStartFates: fate report over the append-only start ledger
// (EHANDLE dwords; resolve serial-validates liveness -- no SEH).
static void ZiprailDedi_ReportStartFates(void)
{
	uint32_t ledger[192] = {};
	uint32_t ledgerCount = 0;
	AcquireSRWLockShared(&s_ziprailStartLedgerLock);
	ledgerCount = s_ziprailStartLedgerCount;
	for (uint32_t i = 0; i < ledgerCount; ++i)
		ledger[i] = s_ziprailStartLedger[i];
	ReleaseSRWLockShared(&s_ziprailStartLedgerLock);

	Warning(eDLL_T::SERVER,
		"[ZIPRAIL-DEDI] sweep ledger: %u starts ever recorded\n", ledgerCount);

	const uintptr_t vtable = s_ziplineVtable.load(std::memory_order_relaxed);
	uint32_t loggedCount = 0;
	uint32_t intact = 0, metaGone = 0, flagLost = 0, entDead = 0;

	for (uint32_t i = 0; i < ledgerCount; ++i)
	{
		const uint32_t h = ledger[i];
		const uintptr_t ent = Ziprail_ResolveHandle(h);

		bool metaExists = false;
		bool metaIsStart = false;
		uint64_t metaGuid = 0;
		AcquireSRWLockShared(&s_ziprailMetaLock);
		if (const ZiprailEntityMeta* meta = Ziprail_FindByHandleLocked(h))
		{
			metaExists = true;
			metaIsStart = meta->isZiprailStart;
			metaGuid = meta->guid;
		}
		ReleaseSRWLockShared(&s_ziprailMetaLock);

		uintptr_t entVtable = 0;
		const bool vtableOk = ent && (vtable != 0) &&
			Ziprail_ReadVtable(ent, &entVtable) && (entVtable == vtable);
		const int16_t edictIdx = ent ? Ziprail_ReadEdictIdx(ent) : -1;

		if (!metaExists)
			++metaGone;
		if (metaExists && !metaIsStart)
			++flagLost;
		if (!vtableOk)
			++entDead;
		if (metaExists && metaIsStart && vtableOk)
			++intact;

		if (!metaExists || !metaIsStart || !vtableOk)
		{
			if (loggedCount < 150)
			{
				Warning(eDLL_T::SERVER,
					"[ZIPRAIL-DEDI] start fate ent=0x%p handle=0x%08X meta=%d isStart=%d "
					"vtableOk=%d edict=%d guid=0x%llX\n",
					reinterpret_cast<void*>(ent), h, metaExists ? 1 : 0,
					metaIsStart ? 1 : 0, vtableOk ? 1 : 0, (int)edictIdx,
					static_cast<unsigned long long>(metaGuid));
				++loggedCount;
			}
		}
	}

	Warning(eDLL_T::SERVER,
		"[ZIPRAIL-DEDI] fate summary: ledger=%u intact=%u metaGone=%u "
		"flagLost=%u entDead=%u\n",
		ledgerCount, intact, metaGone, flagLost, entDead);
}

// Level-load sweep of recorded chain starts from SV_ActivateServer (pre-original), after spawn, before baselines.
void ZiprailDedi_MaterializeAllPending(const char* reason)
{
	if (!DTExtend_IsZiprailPromoteEnabled())
		return;

	// Census first: this is the verified for how many ziprail entities the
	// dedi's converted map actually carries vs the S21 reference.ent.
	uint32_t metas = 0, starts = 0, origins = 0, guids = 0, links = 0, trainNodes = 0;
	uint32_t startHandles[256] = {};
	uint32_t startCount = 0;
	AcquireSRWLockShared(&s_ziprailMetaLock);
	for (const ZiprailEntityMeta& meta : s_ziprailMeta)
	{
		if (!meta.handle)
			continue;
		++metas;
		if (meta.hasOrigin) ++origins;
		if (meta.hasGuid) ++guids;
		if (meta.hasLinkToGuid) ++links;
		if (meta.classTag == ZiprailClassTag::kTrainNode) ++trainNodes;
		if (meta.isZiprailStart)
		{
			++starts;
			if (startCount < 256)
				startHandles[startCount++] = meta.handle;
		}
	}
	ReleaseSRWLockShared(&s_ziprailMetaLock);

	Warning(eDLL_T::SERVER,
		"[ZIPRAIL-DEDI] sweep(%s): metas=%u starts=%u origins=%u guids=%u "
		"links=%u trainNodes=%u spawnHookCalls=%u rebuildHookCalls=%u\n",
		reason ? reason : "?", metas, starts, origins, guids, links, trainNodes,
		s_ziplineSpawnHookCount.load(std::memory_order_relaxed),
		s_ziplineRebuildHookCount.load(std::memory_order_relaxed));

	ZiprailDedi_ReportStartFates();

	const uintptr_t vtable = s_ziplineVtable.load(std::memory_order_relaxed);
	uint32_t applied = 0, skippedDead = 0;
	for (uint32_t i = 0; i < startCount; ++i)
	{
		const uint32_t h = startHandles[i];
		const uintptr_t ent = Ziprail_ResolveHandle(h);

		// Liveness: serial-validated resolve + optional vtable match.
		bool alive = false;
		if (ent && vtable != 0)
		{
			uintptr_t entVt = 0;
			if (Ziprail_ReadVtable(ent, &entVt) && entVt == vtable)
				alive = true;
		}
		if (!alive)
		{
			++skippedDead;
			if (skippedDead <= 8)
			{
				Warning(eDLL_T::SERVER,
					"[ZIPRAIL-DEDI] sweep: start handle=0x%08X no longer live "
					"(resolve/vtable) -- skipped\n",
					h);
			}
			continue;
		}

		ZiprailDedi_TryMaterialize(ent);
		ZiprailDedi_PostActivate(ent);
		if (ZiprailDedi_HasPath(ent))
			++applied;
	}

	Warning(eDLL_T::SERVER,
		"[ZIPRAIL-DEDI] sweep(%s) done: starts=%u applied=%u skippedDead=%u\n",
		reason ? reason : "?", startCount, applied, skippedDead);
}

// Token-scan for "isZiprailStart" "1" so chain heads are not demoted to zipline_end.
static char __fastcall ZiplineBlockHasLinkToDedi_Hook(const unsigned char* blockText)
{
	char result = v_ZiplineBlockHasLinkToDedi(blockText);
	if (result)
		return result;

	if (!blockText || !DTExtend_IsZiprailPromoteEnabled())
		return result;

	static const unsigned char kNeedle[] = "isZiprailStart";
	static const size_t kNeedleLen = 14;
	bool found = false;

	__try
	{
		size_t i = 0;
		while (i < 32768)
		{
			const unsigned char b = blockText[i];
			if (b == '\0' || b == '}')
				break;

			bool matched = true;
			for (size_t k = 0; k < kNeedleLen; ++k)
			{
				if (blockText[i + k] != kNeedle[k])
				{
					matched = false;
					break;
				}
			}

			if (matched)
			{
				size_t j = i + kNeedleLen;
				if (blockText[j] == '"')
					++j;
				while (blockText[j] == ' ' || blockText[j] == '\t')
					++j;
				if (blockText[j] == '"' && blockText[j + 1] == '1' &&
					blockText[j + 2] == '"')
				{
					found = true;
					break;
				}
			}

			++i;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return result;
	}

	if (found)
	{
		const uint32_t n = s_ziprailKeepClassnameLogs.fetch_add(1, std::memory_order_relaxed);
		if (n < 8 || (n % 128) == 0)
		{
			Warning(eDLL_T::SERVER,
				"[ZIPRAIL-DEDI] keep-classname #%u: ziprail chain head retained as "
				"'zipline' (native demotes no-link_to ziplines to zipline_end)\n",
				n + 1);
		}
		return 1;
	}

	return result;
}

static void* ZiplineEntityValidationDedi_Hook(void* ent)
{
	if (!ent)
	{
		const uint32_t n = s_nullValidationCount.fetch_add(1, std::memory_order_relaxed);
		if (n < 8 || (n % 128) == 0)
		{
			Warning(eDLL_T::SERVER,
				"[ZIPLINE-DEDI] null linked entity reached native validator "
				"(#%u); treating as invalid zipline endpoint instead of crashing\n",
				n + 1);
		}
		return nullptr;
	}

	return v_ZiplineEntityValidationDedi(ent);
}

static char __fastcall BaseEntityKeyValueDedi_Hook(uintptr_t ent,
	const char* key, const char* value)
{
	// Ziprail_Record* is a no-op when parked. Envmap neutralize stays live
	// (unrelated to curved ziprails; prevents brush models leaking to clients).
	Ziprail_RecordBaseKeyValue(ent, key, value);
	const char result = v_BaseEntityKeyValueDedi(ent, key, value);
	if (Ziprail_StrEq(key, "classname") && EnvmapVolume_IsClassname(value))
	{
		EnvmapVolume_Track(ent);
		EnvmapVolume_NeutralizeNetworkModel(ent, "classname");
	}
	else if (Ziprail_StrEq(key, "model") && EnvmapVolume_IsTracked(ent))
	{
		// Some maps apply "model" after "classname"; if the class was already
		// tracked in this hook, keep the brush model from leaking to clients.
		EnvmapVolume_NeutralizeNetworkModel(ent, "model");
	}
	return result;
}

static char __fastcall ZiplineKeyValueDedi_Hook(uintptr_t ent,
	const char* key, const char* value)
{
	if (key && Ziprail_StrStartsWith(key, "_zipline_rest_point_"))
	{
		const char* const suffix = key + strlen("_zipline_rest_point_");
		const int index = Ziprail_ParseInt(suffix);
		if (index < 0 || index >= kZiprailMaxRestPoints)
		{
			static volatile LONG s_nRestRefuse;
			const LONG n = InterlockedIncrement(&s_nRestRefuse);
			if (n <= 4 || (n % 128) == 0)
			{
				Warning(eDLL_T::SERVER,
					"[ZIPRAIL-DEDI] refusing rest-point key '%s' index %d\n",
					key, index);
			}
			return 1;
		}
	}

	// Ziprail-only observer -- not attached when parked.
	if (ent && s_ziplineVtable.load(std::memory_order_relaxed) == 0)
	{
		s_ziplineVtable.store(*reinterpret_cast<uintptr_t*>(ent),
			std::memory_order_relaxed);
	}
	Ziprail_RecordZiplineKeyValue(ent, key, value);
	return v_ZiplineKeyValueDedi(ent, key, value);
}

static char __fastcall ZiplineRebuildDedi_Hook(uintptr_t ent)
{
	// Pure pass-through + census when live. Not attached when parked -- that
	// was the [ZIPRAIL-DEDI] rebuild spam on every native zipline think.
	if (!ZiprailDedi_IsLive())
		return v_ZiplineRebuildDedi(ent);

	s_ziplineRebuildHookCount.fetch_add(1, std::memory_order_relaxed);
	return v_ZiplineRebuildDedi(ent);
}

// CZipline::Spawn (DispatchSpawn vtable slot 23). Always attached: the
// partner-nudge path recovers co-located script endpoints. Ziprail census
// logging stays gated on ZiprailDedi_IsLive().
static char __fastcall ZiplineActivateDedi_Hook(uintptr_t ent)
{
	bool nudged = false;
	uint32_t savedZBits = 0;
	int64_t setupCount = 0;

	// Nudge local origin Z only. Spawn overwrites abs origin. Restore exact float bits.
	if (bridge_zip_spawn_partner_nudge.GetBool() && ent)
	{
		__try
		{
			setupCount = *reinterpret_cast<const int64_t*>(ent + kZipEntOffSetupPointCount);
			if (setupCount >= 2 && setupCount <= 4096)
			{
				float* const pLocalOrigin =
					reinterpret_cast<float*>(ent + kZipEntOffLocalOrigin);
				memcpy(&savedZBits, &pLocalOrigin[2], sizeof(savedZBits));
				float savedZ = 0.0f;
				memcpy(&savedZ, &savedZBits, sizeof(savedZ));
				const float nudgedZ = savedZ + kZipSpawnPartnerNudgeZ;
				memcpy(&pLocalOrigin[2], &nudgedZ, sizeof(nudgedZ));
				*reinterpret_cast<int*>(ent + kZipEntOffEFlags) |= kZipEFlagsAbsDirty;
				nudged = true;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			nudged = false;
		}
	}

	if (ZiprailDedi_IsLive())
		s_ziplineSpawnHookCount.fetch_add(1, std::memory_order_relaxed);

	const char result = v_ZiplineActivateDedi(ent);

	if (nudged && ent)
	{
		uint8_t physInit = 0;
		int ropeNodes = 0;
		uint32_t nextZip = 0;

		__try
		{
			float* const pLocalOrigin =
				reinterpret_cast<float*>(ent + kZipEntOffLocalOrigin);
			memcpy(&pLocalOrigin[2], &savedZBits, sizeof(savedZBits));
			*reinterpret_cast<int*>(ent + kZipEntOffEFlags) |= kZipEFlagsAbsDirty;

			physInit = *reinterpret_cast<const uint8_t*>(ent + kZipEntOffPhysInit);
			ropeNodes = *reinterpret_cast<const int*>(ent + kZipEntOffRopeNodeCount);
			nextZip = *reinterpret_cast<const uint32_t*>(ent + kZipEntOffNextZipline);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}

		s_zipSpawnPartnerNudgeCount.fetch_add(1, std::memory_order_relaxed);
	}

	return result;
}

void VZiplineValidationDedi::GetVar(void) const
{
	// "context_action_can_use" settings bool. Pair with m_realmsBitMask; the mov is not unique.
	g_pZipCtxActionCanUseOffset = Module_FindPattern(g_GameDll,
		"48 8B 87 E8 0A 00 00 49 85 85 E8 0A 00 00 0F 84 ?? ?? ?? ?? "
		"8B 0D ?? ?? ?? ?? 49 8B 85 08 5F 00 00 80 3C 01 00")
		.Offset(20).ResolveRelativeAddress(2, 6).RCast<const uint32_t*>();

	if (!g_pZipCtxActionCanUseOffset)
		Warning(eDLL_T::SERVER,
			"[ZIPRAIL-WHYNOT] context_action_can_use field-offset pattern unresolved "
			"-- the mount search's use-permission gate cannot be reported\n");
}

void VZiplineValidationDedi::GetFun(void) const
{
	// Zipline_CollectCandidates: anchor on AllZiplineEntitiesEnumerator vftable load (+0x70). No wildcards on the rip.
	Module_FindPattern(g_GameDll,
		"0F 29 94 24 10 12 00 00 41 0F 28 C1 F3 44 0F 10 51 04 48 8D 0D 77 28 5D 00")
		.Offset(-0x70)
		.GetPtr(v_Zipline_CollectCandidates);

	if (!v_Zipline_CollectCandidates)
		Warning(eDLL_T::SERVER,
			"[ZIPRAIL-COLLECT] candidate collector pattern unresolved -- cannot tell "
			"a collector reject from a Zipline_Find reject\n");

	// CCollisionProperty partition suspend / resume. The resume half is the only
	// caller of the mask derivation, so the pair is how a mask gets refreshed.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 0F B6 51 31 48 8B D9 8D 42 01 88 41 31 84 D2 75 6B")
		.GetPtr(v_CollisionProp_SuspendPartition);

	Module_FindPattern(g_GameDll,
		"40 57 48 83 EC 30 80 41 31 FF 48 8B F9 0F 85 E0 01 00 00")
		.GetPtr(v_CollisionProp_ResumePartition);

	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 80 79 31 00 48 8B D9 0F 85 C3 00 00 00")
		.GetPtr(v_CollisionProp_QueuePartitionUpdate);

	if (!v_CollisionProp_QueuePartitionUpdate)
		Warning(eDLL_T::SERVER,
			"[ZIPRAIL-DEDI] partition queue pattern unresolved -- a chain start's BVH "
			"bounds stay as spawn left them and the mount search cannot intersect them\n");

	if (!v_CollisionProp_SuspendPartition || !v_CollisionProp_ResumePartition)
		Warning(eDLL_T::SERVER,
			"[ZIPRAIL-DEDI] partition suspend/resume pattern unresolved -- a chain "
			"start keeps its spawn-time list mask and stays invisible to the mount search\n");

	// in the S3 dedicated binary
	// sub rsp, 28h
	// cmp dword ptr [rcx+0B1Ch], 1
	Module_FindPattern(g_GameDll,
		"48 83 EC 28 83 B9 1C 0B 00 00 01")
		.GetPtr(v_ZiplineEntityValidationDedi);

	// Keep this resolved/logged for crash correlation. We do not detour it yet
	// the validation hook is the narrowest place to prevent the known null AV.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 20 80 3D")
		.GetPtr(v_ZiplineEntityLookupDedi);

	// -- CBaseEntity::KeyValue. Used to observe origin and
	// GUID/link keys for both zipline endpoints and script_mover_train_node
	// path nodes. The original.ent remains untouched.
	Module_FindPattern(g_GameDll,
		"40 55 56 57 41 56 48 8D 6C 24 C1 48 81 EC E8 00 00 00 "
		"48 8B F2 48 8B F9 48 8B CE BA 23 00 00 00")
		.GetPtr(v_BaseEntityKeyValueDedi);

	// -- CZipline::KeyValue. It already knows how to ingest
	// _zipline_rest_point_%d, so the S21 ziprail GUID chain is fed back through
	// this native parser at runtime instead of rewriting map entity text.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 60 "
		"48 8B F2 48 8B D9 48 8B CE")
		.GetPtr(v_ZiplineKeyValueDedi);

	// -- CZipline rebuild/validate path. Think-driven; gated on
	// ent+0xB18/ent+0xD38 (see ZiplineRebuildDedi_Hook).
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 80 B9 18 0B 00 00 00 48 8B D9")
		.GetPtr(v_ZiplineRebuildDedi);


	// 1 hit): builds the rope via the native partner-entity link system, which
	// never resolves a ziprail chain start (it has no native "linked entity" --
	// the far end is a GUID-chain hop away, not the legacy zipline-pair lookup).
	Module_FindPattern(g_GameDll,
		"48 8B C4 55 53 48 8D A8 68 FF FF FF 48 81 EC 88 01 00 00")
		.GetPtr(v_ZiplineActivateDedi);

	// -- CZiplinePhysics::Init(physics, points, count, owner).
	// Resolved but NOT hooked; called manually from ZiprailDedi_PostActivate only
	// when native Activate's own partner resolution left ent+0xB18 unset.
	Module_FindPattern(g_GameDll,
		"48 8B C4 55 53 41 56 48 8B EC 48 83 EC 60 48 89 70 08 4C 8B F1 48 89 78 10 "
		"49 8B F1 4C 89 68 E0 48 8B FA 4C 89 78 D8 48 8D 91 38 02 00 00")
		.GetPtr(v_CZiplinePhysicsInitDedi);

	// -- rope-shape publisher: count mirror 0xF50, positions 0xF54, segment dists 0x1014.
	// Native CZipline::Activate calls it as its last shape step. Resolved but NOT
	// hooked; called manually from ZiprailDedi_PostActivate after Physics::Init.
	Module_FindPattern(g_GameDll,
		"40 57 48 81 EC 10 01 00 00 44 8B 81 38 0D 00 00 48 8B F9 41 BB 00 02 00 00 "
		"48 89 B4 24 30 01 00 00 44 39 81 50 0F 00 00")
		.GetPtr(v_ZiplinePublishRopeShapeDedi);

	// -- zipline block link_to_guid_0 token scan (see hook comment).
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 81 EC ?? ?? ?? ?? 48 8B D9 48 85 C9")
		.GetPtr(v_ZiplineBlockHasLinkToDedi);

	if (!v_ZiplineEntityValidationDedi)
	{
		Warning(eDLL_T::SERVER,
			"[ZIPLINE-DEDI] validation pattern unresolved -- S21 ziprail "
			"startup crash guard inactive\n");
	}
	if (!v_ZiplineActivateDedi)
	{
		Warning(eDLL_T::SERVER,
			"[ZIPRAIL-DEDI] CZipline::Activate pattern unresolved -- S21 ziprail "
			"chain materialization inactive\n");
	}
	if (!v_CZiplinePhysicsInitDedi)
	{
		Warning(eDLL_T::SERVER,
			"[ZIPRAIL-DEDI] CZiplinePhysics::Init pattern unresolved -- chain "
			"starts will stay dead ziplines even with sdk_ziprail_enable 1\n");
	}
	if (!v_ZiplinePublishRopeShapeDedi)
	{
		Warning(eDLL_T::SERVER,
			"[ZIPRAIL-DEDI] rope-shape publisher pattern unresolved -- promoted "
			"ziprails will have a zero-length rope and will detach on mount\n");
	}
	if (!v_ZiplineBlockHasLinkToDedi)
	{
		Warning(eDLL_T::SERVER,
			"[ZIPRAIL-DEDI] zipline block link-scan pattern unresolved -- chain "
			"heads will be demoted to zipline_end and stay invisible\n");
	}
}

void VZiplineValidationDedi::GetAdr(void) const
{
	LogFunAdr("ZiplineEntityValidationDedi", v_ZiplineEntityValidationDedi);
	LogFunAdr("ZiplineEntityLookupDedi", v_ZiplineEntityLookupDedi);
	LogFunAdr("BaseEntityKeyValueDedi", v_BaseEntityKeyValueDedi);
	LogFunAdr("ZiplineKeyValueDedi", v_ZiplineKeyValueDedi);
	LogFunAdr("ZiplineRebuildDedi", v_ZiplineRebuildDedi);
	LogFunAdr("ZiplineActivateDedi", v_ZiplineActivateDedi);
	LogFunAdr("CZiplinePhysicsInitDedi", v_CZiplinePhysicsInitDedi);
	LogFunAdr("ZiplineBlockHasLinkToDedi", v_ZiplineBlockHasLinkToDedi);
}

void VZiplineValidationDedi::Detour(const bool bAttach) const
{
	// Parked (+sdk_ziprail_enable 0): null validator, envmap neutralize, Spawn partner-nudge. Live: full chain.
	const bool live = ZiprailDedi_IsLive();

	if (bAttach)
	{
		Msg(eDLL_T::SERVER,
			live
				? "[ZIPRAIL-DEDI] LIVE -- full chain system armed\n"
				: "[ZIPRAIL-DEDI] PARKED (sdk_ziprail_enable 0) -- null-zipline "
				  "validator + envmap KV guard + spawn partner-nudge; "
				  "no rebuild/KV chain hooks\n");
	}

	// Always: null linked-entity guard (maps with broken ends must not AV).
	if (v_ZiplineEntityValidationDedi)
	{
		DetourSetup(&v_ZiplineEntityValidationDedi,
			&ZiplineEntityValidationDedi_Hook, bAttach);
	}

	// Observation only, and only while the chain system is live.
	if (live && v_Zipline_CollectCandidates)
	{
		DetourSetup(&v_Zipline_CollectCandidates,
			&Hook_Zipline_CollectCandidates, bAttach);
	}
	// Always: envmap model neutralize rides the CBaseEntity::KeyValue hook.
	// Ziprail_RecordBaseKeyValue is a no-op while parked.
	if (v_BaseEntityKeyValueDedi)
	{
		DetourSetup(&v_BaseEntityKeyValueDedi,
			&BaseEntityKeyValueDedi_Hook, bAttach);
	}
	// Always: Spawn partner-nudge so co-located DeployZipline endpoints build a rope.
	if (v_ZiplineActivateDedi)
	{
		DetourSetup(&v_ZiplineActivateDedi,
			&ZiplineActivateDedi_Hook, bAttach);
	}

	if (!live)
		return; // live-only ziprail hooks never attached -- nothing more to attach/detach

	// Live-only ziprail machinery. When parked these stay unhooked so native
	// CZipline rebuild never touch our observers or print [ZIPRAIL-DEDI].
	if (v_ZiplineKeyValueDedi)
	{
		DetourSetup(&v_ZiplineKeyValueDedi,
			&ZiplineKeyValueDedi_Hook, bAttach);
	}
	if (v_ZiplineRebuildDedi)
	{
		DetourSetup(&v_ZiplineRebuildDedi,
			&ZiplineRebuildDedi_Hook, bAttach);
	}
	if (v_ZiplineBlockHasLinkToDedi)
	{
		DetourSetup(&v_ZiplineBlockHasLinkToDedi,
			&ZiplineBlockHasLinkToDedi_Hook, bAttach);
	}
}
