//=============================================================================//
//
// Purpose: rig_extend.h implementation. See that header.
//
// A model's virtual model gathers its sequences from each animrig header's
// {count, pSequences[]} (entries are aseq asset heads). The group binder is
// hooked so a manifest rig gets its list replaced by the resident list plus
// the appended aseq heads before the binder walks it. Append only: the rig
// blob's transition tables index this list, and the tail must keep the
// list name-sorted (see RigExtend_Apply).
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/memaddr.h"
#include "tier0/module.h"
#include "tier1/convar.h"
#include "rig_extend.h"
#if defined(CLIENT_DLL)
#include "rtech/pak/rpak_observe.h"
#else
#include "rtech/pak/pakstate.h"
#endif // CLIENT_DLL

#include <algorithm>
#include <vector>

static ConVar sdk_rig_extend("sdk_rig_extend", "1", FCVAR_RELEASE,
	"Append platform/rig_extend.txt sequences to resident animrigs. 0 = off.");

static constexpr const char* kManifestPath = "platform/rig_extend.txt";
static constexpr size_t kMaxSeqsPerRig = 4096;

#if defined(CLIENT_DLL)
// S21 arig header: u16 externalCount +0x10, u16 count +0x12, heads +0x18.
static constexpr uintptr_t kRigSeqCountOffset = 0x12;
#else
// S3 arig header: u32 unk +0x10, u32 count +0x14, heads +0x18.
static constexpr uintptr_t kRigSeqCountOffset = 0x14;
#endif // CLIENT_DLL
static constexpr uintptr_t kRigSeqArrayOffset = 0x18;
static constexpr uintptr_t kBinderListOffset  = 0x10;

struct RigExtension_t
{
	uint64_t rigGuid;
	std::vector<uint64_t> seqGuids;
	void* pRigHead;
	bool applied;
};

static std::vector<RigExtension_t> s_extensions;
static bool s_manifestLoaded = false;
static int s_pendingCount = 0;

static void* RigExtend_FindHead(const uint64_t guid)
{
#if defined(CLIENT_DLL)
	return Pak_FindInstalledHead_S21(guid);
#else
	if (!g_pakGlobals)
		return nullptr;
	for (unsigned int probe = 0; probe < PAK_MAX_LOADED_ASSETS; ++probe)
	{
		const PakAssetShort_s& asset = g_pakGlobals->loadedAssets[(guid + probe) & PAK_MAX_LOADED_ASSETS_MASK];
		if (asset.guid <= 1)
			return nullptr;
		if (asset.guid == guid)
			return asset.head;
	}
	return nullptr;
#endif // CLIENT_DLL
}

static void RigExtend_LoadManifest(void)
{
	if (s_manifestLoaded)
		return;
	s_manifestLoaded = true;

	FILE* const f = fopen(kManifestPath, "r");
	if (!f)
	{
		Msg(eDLL_T::ENGINE, "[RIG-EXTEND] no %s -- nothing to append\n", kManifestPath);
		return;
	}

	char line[512];
	int lines = 0;
	while (fgets(line, sizeof(line), f))
	{
		unsigned long long rigGuid = 0, seqGuid = 0;
		if (line[0] == '#' || sscanf(line, "%llx %llx", &rigGuid, &seqGuid) != 2 || !rigGuid || !seqGuid)
			continue;

		RigExtension_t* ext = nullptr;
		for (RigExtension_t& e : s_extensions)
		{
			if (e.rigGuid == rigGuid)
			{
				ext = &e;
				break;
			}
		}
		if (!ext)
		{
			s_extensions.push_back({ rigGuid, {}, nullptr, false });
			ext = &s_extensions.back();
		}
		if (ext->seqGuids.size() < kMaxSeqsPerRig)
			ext->seqGuids.push_back(seqGuid);
		++lines;
	}
	fclose(f);

	s_pendingCount = static_cast<int>(s_extensions.size());
	Msg(eDLL_T::ENGINE, "[RIG-EXTEND] %s: %d entries over %d rigs\n",
		kManifestPath, lines, s_pendingCount);
}

// Replaces the rig's sequence array with resident + appended heads when the
// binder is about to walk this rig. Returns without touching it when the rig
// or any appended sequence is not resident yet; the next build retries.
static void RigExtend_Apply(void* const pRigHead)
{
	if (s_pendingCount <= 0 || !pRigHead)
		return;

	for (RigExtension_t& ext : s_extensions)
	{
		if (ext.applied)
			continue;
		if (!ext.pRigHead)
			ext.pRigHead = RigExtend_FindHead(ext.rigGuid);
		if (ext.pRigHead != pRigHead)
			continue;

		std::vector<void*> heads;
		heads.reserve(ext.seqGuids.size());
		int missing = 0;
		for (const uint64_t seqGuid : ext.seqGuids)
		{
			void* const pHead = RigExtend_FindHead(seqGuid);
			if (!pHead || !*reinterpret_cast<void**>(pHead))
			{
				++missing;
				continue;
			}
			heads.push_back(pHead);
		}
		if (missing)
		{
			Warning(eDLL_T::ENGINE, "[RIG-EXTEND] rig 0x%016llX: %d of %zu sequences not resident, appending %zu\n",
				ext.rigGuid, missing, ext.seqGuids.size(), heads.size());
		}

		char* const pHdr = reinterpret_cast<char*>(pRigHead);
#if defined(CLIENT_DLL)
		const size_t oldCount = *reinterpret_cast<const uint16_t*>(pHdr + kRigSeqCountOffset);
#else
		const size_t oldCount = *reinterpret_cast<const uint32_t*>(pHdr + kRigSeqCountOffset);
#endif // CLIENT_DLL
		void** const pOld = *reinterpret_cast<void***>(pHdr + kRigSeqArrayOffset);
		if (heads.empty())
			return;
		if (!pOld || oldCount + heads.size() > 0xFFFF)
		{
			Warning(eDLL_T::ENGINE, "[RIG-EXTEND] rig 0x%016llX: cannot grow (%zu -> %zu)\n", ext.rigGuid, oldCount, oldCount + heads.size());
			ext.applied = true;
			--s_pendingCount;
			return;
		}

		// Autolayer references resolve by a case-insensitive binary search over the
		// list's asset names, so the appended tail is sorted and anything that
		// cannot follow the resident list is left out. A guid the stock paks
		// already own resolves to the stock head, whose name lacks the '~' prefix.
		const auto seqName = [](const void* pHead) -> const char*
		{
			return *reinterpret_cast<const char* const*>(reinterpret_cast<const char*>(pHead) + 8);
		};
		std::stable_sort(heads.begin(), heads.end(), [&](const void* a, const void* b)
		{
			const char* const na = seqName(a);
			const char* const nb = seqName(b);
			return _stricmp(na ? na : "", nb ? nb : "") < 0;
		});
		const char* prev = oldCount ? seqName(pOld[oldCount - 1]) : nullptr;
		size_t kept = 0;
		for (void* const pHead : heads)
		{
			const char* const name = seqName(pHead);
			if (!name || (prev && _stricmp(prev, name) >= 0))
			{
				Warning(eDLL_T::ENGINE, "[RIG-EXTEND] rig 0x%016llX: '%s' does not sort after '%s' -- skipped\n",
					ext.rigGuid, name ? name : "(null)", prev ? prev : "(null)");
				continue;
			}
			heads[kept++] = pHead;
			prev = name;
		}
		heads.resize(kept);
		if (heads.empty())
		{
			ext.applied = true;
			--s_pendingCount;
			return;
		}
		const size_t newCount = oldCount + heads.size();

		void** const pNew = static_cast<void**>(malloc(newCount * sizeof(void*)));
		if (!pNew)
			return;
		memcpy(pNew, pOld, oldCount * sizeof(void*));
		memcpy(pNew + oldCount, heads.data(), heads.size() * sizeof(void*));

		*reinterpret_cast<void***>(pHdr + kRigSeqArrayOffset) = pNew;
#if defined(CLIENT_DLL)
		*reinterpret_cast<uint16_t*>(pHdr + kRigSeqCountOffset) = static_cast<uint16_t>(newCount);
#else
		*reinterpret_cast<uint32_t*>(pHdr + kRigSeqCountOffset) = static_cast<uint32_t>(newCount);
#endif // CLIENT_DLL

		ext.applied = true;
		--s_pendingCount;
		Msg(eDLL_T::ENGINE, "[RIG-EXTEND] rig 0x%016llX: %zu resident + %zu appended = %zu sequences\n",
			ext.rigGuid, oldCount, heads.size(), newCount);
		return;
	}
}

#if defined(CLIENT_DLL)
typedef __int16(__fastcall* PFN_BindRigSeqs)(__int64 pVirtualModel, __int16 group, __int64 pSeqList);
static PFN_BindRigSeqs v_BindRigSeqs = nullptr;

static __int16 __fastcall Hook_BindRigSeqs(__int64 pVirtualModel, __int16 group, __int64 pSeqList)
{
	if (sdk_rig_extend.GetBool() && pSeqList)
	{
		RigExtend_LoadManifest();
		RigExtend_Apply(reinterpret_cast<void*>(pSeqList - kBinderListOffset));
	}
	return v_BindRigSeqs(pVirtualModel, group, pSeqList);
}
#else
typedef __int64(__fastcall* PFN_BindRigSeqs)(__int64 pVirtualModel, int group, __int64 pSeqList);
static PFN_BindRigSeqs v_BindRigSeqs = nullptr;

static __int64 __fastcall Hook_BindRigSeqs(__int64 pVirtualModel, int group, __int64 pSeqList)
{
	if (sdk_rig_extend.GetBool() && pSeqList)
	{
		RigExtend_LoadManifest();
		RigExtend_Apply(reinterpret_cast<void*>(pSeqList - kBinderListOffset));
	}
	return v_BindRigSeqs(pVirtualModel, group, pSeqList);
}
#endif // CLIENT_DLL

void VRigExtend::GetAdr(void) const
{
	LogFunAdr("VirtualModel_BindRigSeqs", v_BindRigSeqs);
}

void VRigExtend::GetFun(void) const
{
#if defined(CLIENT_DLL)
	// Virtual-model group binder: takes the rig header's sequence list (+0x10)
	// and appends its entries to the model's sequence table.
	Module_FindPattern(g_GameDll,
		"66 89 54 24 10 53 56 41 54 41 55 41 56 41 57 48 83 EC 48 48 8B 41 28 4D 8B F0 45 0F B7 78 02 48 8B D9 66 45 03 38")
		.GetPtr(v_BindRigSeqs);
#else
	Module_FindPattern(g_GameDll,
		"48 8B C4 4C 89 40 18 89 50 10 48 89 48 08 56 41 54 41 55 41 57 48 83 EC 78 4C 63 79 20 4C 8B E9 48 89 58 D8 48 8B 59 68")
		.GetPtr(v_BindRigSeqs);
#endif // CLIENT_DLL
	if (!v_BindRigSeqs)
		Warning(eDLL_T::ENGINE, "[RIG-EXTEND] virtual-model group binder pattern unresolved -- rig extension disabled\n");
}

void VRigExtend::Detour(const bool bAttach) const
{
	if (v_BindRigSeqs)
		DetourSetup(&v_BindRigSeqs, &Hook_BindRigSeqs, bAttach);
}
