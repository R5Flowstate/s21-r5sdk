#if defined(CLIENT_DLL)
#include "core/stdafx.h"
#include "datacache/mdlcache.h"
#include "engine/staticpropmgr.h"
#include "tier1/cvar.h"
#include "tier1/cmd.h"
#include "engine/enginetrace.h"
#include "public/gametrace.h"
#include "public/cmodel.h"
#include "public/bspflags.h"
#include <vector>
#include <string>
#include <mutex>

namespace
{
	// m_Skin is uint16 at +0x20. Engine also movzx word for the remap table; a uint8 clamp is not enough.
	constexpr size_t S21_STATIC_PROP_SKIN_OFFSET = 0x20;
	constexpr size_t S21_STATIC_PROP_MODEL_SKIN_FAMILY_COUNT_OFFSET = 0x96;
	constexpr unsigned int S21_STATIC_PROP_CLAMP_LOG_LIMIT = 128;
	constexpr unsigned int S21_STATIC_PROP_MODELDATA_LOG_LIMIT = 32;

	unsigned int s_StaticPropClampLogCount = 0;
	unsigned int s_StaticPropModelDataLogCount = 0;

	// Native Init does studiohdr=*a7 at +0 then reads +0xAA/+0x96/+0x9A.
	// numbodyparts at +0x9A == 0 returns after writing lump origin. Zero hdr.
	alignas(16) static uint8_t s_emptyStudioHdr[256];
	static const void* s_emptyStudioHdrPtr = s_emptyStudioHdr;

	// Direct SEH read. VirtualQuery per prop serializes on the address-space lock during BSP load.
	bool TryReadPointer(const uintptr_t address, uintptr_t& value)
	{
		__try
		{
			value = *reinterpret_cast<const uintptr_t*>(address);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			value = 0;
			return false;
		}
	}

	bool TryReadUInt16(const uintptr_t address, uint16_t& value)
	{
		__try
		{
			value = *reinterpret_cast<const uint16_t*>(address);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			value = 0;
			return false;
		}
	}

	uint16_t& S21StaticPropSkin(StaticPropLump_t* const lump)
	{
		return *reinterpret_cast<uint16_t*>(reinterpret_cast<uintptr_t>(lump) + S21_STATIC_PROP_SKIN_OFFSET);
	}

	bool S21StaticPropModelSkinFamilyCount(const int64_t modelDataHandle, uintptr_t& modelData, uint16_t& skinFamilyCount)
	{
		modelData = 0;
		skinFamilyCount = 0;

		if (!modelDataHandle)
			return false;

		if (!TryReadPointer(static_cast<uintptr_t>(modelDataHandle), modelData) || !modelData)
			return false;

		return TryReadUInt16(modelData + S21_STATIC_PROP_MODEL_SKIN_FAMILY_COUNT_OFFSET, skinFamilyCount);
	}
}

//-----------------------------------------------------------------------------
// Prop index -> studiohdr* at Init. Name is uint16 offset at studiohdr+8. Rebuilt per map.
//-----------------------------------------------------------------------------
namespace
{
	std::mutex               s_propHdrMutex;
	std::vector<const void*> s_propStudioHdr;

	// SEH-isolated studiohdr-name decode (no C++ objects can share a __try frame).
	bool TryDecodeStudioHdrName(const void* const hdr, char* const out, const size_t outSize)
	{
		if (!hdr || !out || outSize == 0)
			return false;

		__try
		{
			const uintptr_t base = reinterpret_cast<uintptr_t>(hdr);
			const unsigned int off = *reinterpret_cast<const unsigned short*>(base + 8);
			if ((off & 0xFFFF) == 0)
			{
				out[0] = '\0';
				return false;
			}
			const char* const s = reinterpret_cast<const char*>(
				base + ((off & 0xFFFFFFFE) << (4 * (off & 1))));
			size_t i = 0;
			for (; i + 1 < outSize && s[i]; ++i)
				out[i] = s[i];
			out[i] = '\0';
			return i > 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			out[0] = '\0';
			return false;
		}
	}

	void StaticProp_RecordStudioHdr(const unsigned int idx, const void* const hdr)
	{
		std::lock_guard<std::mutex> lock(s_propHdrMutex);

		// New map: props are re-inited starting at index 0 -> drop the old table.
		if (idx == 0)
			s_propStudioHdr.clear();

		if (idx >= s_propStudioHdr.size())
			s_propStudioHdr.resize(idx + 1, nullptr);

		s_propStudioHdr[idx] = hdr;
	}
}

// Decodes prop 'idx's model name into 'out'. Returns false (and empties 'out') if
// nothing was recorded for that index or the name can't be read.
bool StaticProp_LookupModelName(const unsigned int idx, char* const out, const size_t outSize)
{
	const void* hdr = nullptr;
	{
		std::lock_guard<std::mutex> lock(s_propHdrMutex);
		if (idx < s_propStudioHdr.size())
			hdr = s_propStudioHdr[idx];
	}
	return TryDecodeStudioHdrName(hdr, out, outSize);
}

//-----------------------------------------------------------------------------
// Purpose: initialises static props from the static prop gamelump
//-----------------------------------------------------------------------------
void* CStaticProp::Init(CStaticProp* thisptr, int64_t a2, unsigned int idx, unsigned int a4, StaticPropLump_t* lump, int64_t a6, int64_t a7, unsigned int a8)
{
	// mat_staticprop: record idx -> the model's studiohdr (*(a7)); the name is
	// decoded from it on demand. Just a pointer store here (no alloc) -- important
	// on ~400k-prop maps.
	{
		uintptr_t modelData = 0;
		if (a7)
			TryReadPointer(static_cast<uintptr_t>(a7), modelData);

		StaticProp_RecordStudioHdr(idx, reinterpret_cast<const void*>(modelData));
	}

	if (lump)
	{
		uintptr_t modelData = 0;
		uint16_t skinFamilyCount = 0;
		uint16_t& skin = S21StaticPropSkin(lump);

		if (S21StaticPropModelSkinFamilyCount(a7, modelData, skinFamilyCount))
		{
			if ((skinFamilyCount == 0 && skin != 0) || (skinFamilyCount != 0 && skin >= skinFamilyCount))
			{
				const uint16_t oldSkin = skin;
				skin = 0;

				if (s_StaticPropClampLogCount++ < S21_STATIC_PROP_CLAMP_LOG_LIMIT)
				{
					Warning(eDLL_T::ENGINE,
						"[s21-staticprop] clamped bad skin index for prop #%u type=%i oldSkin=%u skinFamilies=%u modelData=0x%p origin=(%.1f %.1f %.1f)\n",
						idx, lump->m_PropType, oldSkin, skinFamilyCount, reinterpret_cast<void*>(modelData),
						lump->m_Origin.x, lump->m_Origin.y, lump->m_Origin.z);
				}
			}
		}
		else if (s_StaticPropModelDataLogCount++ < S21_STATIC_PROP_MODELDATA_LOG_LIMIT)
		{
			Warning(eDLL_T::ENGINE,
				"[s21-staticprop] could not read model data for prop #%u type=%i skin=%u modelDataHandle=0x%p origin=(%.1f %.1f %.1f)\n",
				idx, lump->m_PropType, skin, reinterpret_cast<void*>(a7),
				lump->m_Origin.x, lump->m_Origin.y, lump->m_Origin.z);
		}
	}

	int64_t initHandle = a7;
	uintptr_t studioHdr = 0;
	if (!a7 || !TryReadPointer(static_cast<uintptr_t>(a7), studioHdr) || !studioHdr)
		initHandle = reinterpret_cast<int64_t>(&s_emptyStudioHdrPtr);

	return CStaticProp__Init(thisptr, a2, idx, a4, lump, a6, initHandle, a8);
}

//-----------------------------------------------------------------------------
// NOTE: the following gather props functions have been hooked as we must
// enable the old gather props logic for fall back models to draw !!! The
// new solution won't call CMDLCache::GetHardwareData on bad model handles.
//-----------------------------------------------------------------------------
void* GatherStaticPropsSecondPass_PreInit(GatherProps_t* gather)
{
    if (g_StudioMdlFallbackHandler.HasInvalidModelHandles())
        g_StudioMdlFallbackHandler.EnableLegacyGatherProps();

    return v_GatherStaticPropsSecondPass_PreInit(gather);
}
void* GatherStaticPropsSecondPass_PostInit(GatherProps_t* gather)
{
    if (g_StudioMdlFallbackHandler.HasInvalidModelHandles())
        g_StudioMdlFallbackHandler.EnableLegacyGatherProps();

    return v_GatherStaticPropsSecondPass_PostInit(gather);
}

// Default off. Unresolved pass slot is a pak defect; omit $depth* so Repak writes the default + relation.
static ConVar sdk_matl_pass_guard("sdk_matl_pass_guard", "0", FCVAR_RELEASE,
	"Drop instance-draw records whose material slot is NULL or a raw pak GUID.");

static constexpr unsigned int kSubmitCap = 2048;
static constexpr size_t kRecStride = 64;
static int s_passSkipLogBudget = 96;
static unsigned long long s_passDropCount = 0;

enum MatlReject_e
{
	MATL_OK = 0,
	MATL_NOT_CANONICAL,
	MATL_VTABLE,
	MATL_VFUNC,
};

// Unfixed pass slot keeps a raw pak GUID. Module .data also holds code_private depth materials.
static MatlReject_e ClassifyMaterial(const uintptr_t p, uintptr_t& vt, uintptr_t& fn)
{
	vt = 0;
	fn = 0;

	if (!p || (p >> 47) != 0 || (p & 7) != 0 || p < 0x10000)
		return MATL_NOT_CANONICAL;

	const QWORD base = g_GameDll.GetModuleBase();
	const QWORD end = base + g_GameDll.GetModuleSize();

	vt = *reinterpret_cast<const uintptr_t*>(p);
	if (vt < base || vt >= end || (vt & 7) != 0)
		return MATL_VTABLE;

	fn = *reinterpret_cast<const uintptr_t*>(vt);
	if (fn < base || fn >= end)
		return MATL_VFUNC;

	return MATL_OK;
}

static __int64 __fastcall Hook_StaticPropInstanceSubmit(unsigned int count, __int64 transforms, int a3, __int64 records, int a5)
{
	if (!v_StaticPropInstanceSubmit)
		return 0;

	if (!sdk_matl_pass_guard.GetBool() || !records || count == 0)
		return v_StaticPropInstanceSubmit(count, transforms, a3, records, a5);

	unsigned int n = count;
	if (a3 > 0 && static_cast<unsigned int>(a3) < n)
		n = static_cast<unsigned int>(a3);
	if (n > kSubmitCap)
		n = kSubmitCap;

	unsigned int firstBad = n;
	for (unsigned int i = 0; i < n; ++i)
	{
		const uintptr_t mat = *reinterpret_cast<uintptr_t*>(
			records + static_cast<uintptr_t>(i) * kRecStride);

		uintptr_t vt = 0, fn = 0;
		const MatlReject_e why = ClassifyMaterial(mat, vt, fn);
		if (why == MATL_OK)
			continue;

		if (firstBad == n)
			firstBad = i;
		if (s_passSkipLogBudget > 0)
		{
			--s_passSkipLogBudget;
			Warning(eDLL_T::ENGINE,
				"[s21-staticprop] bad instance mat=0x%llX why=%d vt=0x%llX fn=0x%llX idx=%u/%u a1=%u a3=%d\n",
				static_cast<unsigned long long>(mat), static_cast<int>(why),
				static_cast<unsigned long long>(vt), static_cast<unsigned long long>(fn),
				i, n, count, a3);
		}
	}

	if (firstBad == n)
		return v_StaticPropInstanceSubmit(count, transforms, a3, records, a5);
	if (firstBad == 0)
	{
		// Dropping the batch removes geometry from the frame. Never let that
		// go unannounced -- a silent drop here reads in game as "the map is
		// missing its props and half its walls".
		if (++s_passDropCount <= 16 || (s_passDropCount % 4096) == 0)
			Warning(eDLL_T::ENGINE,
				"[s21-staticprop] dropped instance batch #%llu (%u records)\n",
				static_cast<unsigned long long>(s_passDropCount), count);
		return 0;
	}
	const int a3out = (a3 == static_cast<int>(count)) ? static_cast<int>(firstBad) : a3;
	return v_StaticPropInstanceSubmit(firstBad, transforms, a3out, records, a5);
}

void VStaticPropMgr::Detour(const bool bAttach) const
{
	// Guard each target so a single unresolved pattern cannot inflate null_skips
	// while the other hooks attach and work.
	if (CStaticProp__Init)
		DetourSetup(&CStaticProp__Init, &CStaticProp::Init, bAttach);
	else
		Warning(eDLL_T::ENGINE, "[s21-staticprop] CStaticProp::Init pattern unresolved\n");

	if (v_GatherStaticPropsSecondPass_PreInit)
		DetourSetup(&v_GatherStaticPropsSecondPass_PreInit, &GatherStaticPropsSecondPass_PreInit, bAttach);
	else
		Warning(eDLL_T::ENGINE, "[s21-staticprop] GatherStaticPropsSecondPass_PreInit pattern unresolved\n");

	if (v_GatherStaticPropsSecondPass_PostInit)
		DetourSetup(&v_GatherStaticPropsSecondPass_PostInit, &GatherStaticPropsSecondPass_PostInit, bAttach);
	else
		Warning(eDLL_T::ENGINE, "[s21-staticprop] GatherStaticPropsSecondPass_PostInit pattern unresolved\n");

	if (v_StaticPropInstanceSubmit)
		DetourSetup(&v_StaticPropInstanceSubmit, &Hook_StaticPropInstanceSubmit, bAttach);
	else
		Warning(eDLL_T::ENGINE, "[s21-staticprop] InstanceSubmit pattern unresolved\n");
}

//-----------------------------------------------------------------------------
// mat_staticprop -- pattern-resolved globals + the console command.
//-----------------------------------------------------------------------------
namespace
{
	// The global slot that HOLDS the client engine-trace singleton. Resolved by
	// pattern at init; dereferenced at command time because the object itself
	// does not exist until a map is loaded. (*g_ppEngineTraceClient) -> object.
	CEngineTraceClient** g_ppEngineTraceClient = nullptr;

	// Render-view camera vectors -- exactly what the crosshair points at (same
	// source GetMaterialAtCrossHair / mat_crosshair reads).
	const Vector3D* g_pMainViewOrigin  = nullptr;
	const Vector3D* g_pMainViewForward = nullptr;
}

//-----------------------------------------------------------------------------
// Print the static prop model name + id under the crosshair. Traces the render
// view ray against the collision system (which, unlike mat_crosshair's world-
// brush-only R_TraceCellBsp_r, includes static props) and reads the hit prop
// index from trace_t.staticPropID (+0x6C), then the load-time name table.
//-----------------------------------------------------------------------------
void Mat_StaticProp_f(const CCommand& args)
{
	if (!g_ppEngineTraceClient || !g_pMainViewOrigin || !g_pMainViewForward)
	{
		Warning(eDLL_T::MS, "%s: trace/view globals unresolved; command disabled\n", __FUNCTION__);
		return;
	}

	CEngineTraceClient* const traceClient = *g_ppEngineTraceClient;
	if (!traceClient)
	{
		Warning(eDLL_T::MS, "%s: engine trace client not ready (load into a map first)\n", __FUNCTION__);
		return;
	}

	const Vector3D start = *g_pMainViewOrigin;
	const Vector3D fwd   = *g_pMainViewForward;

	// Match the crosshair's reach (GetMaterialAtCrossHair uses this exact length).
	const float kTraceDist = 227023.36f;
	Vector3D end;
	end.x = start.x + fwd.x * kTraceDist;
	end.y = start.y + fwd.y * kTraceDist;
	end.z = start.z + fwd.z * kTraceDist;

	Ray_t ray(start, end);

	trace_t trace;
	memset(&trace, 0, sizeof(trace));
	trace.staticPropID = -1; // engine overwrites on a prop hit; -1 = "not a prop"
	traceClient->TraceRay(ray, TRACE_MASK_SOLID, &trace);

	Msg(eDLL_T::MS, "______________________________________________________________\n");
	Msg(eDLL_T::MS, "-+ Static prop under crosshair -------------------------------\n");

	if (trace.fraction >= 1.0f)
	{
		Msg(eDLL_T::MS, " |-- No hit within reach.\n");
		Msg(eDLL_T::MS, "--------------------------------------------------------------\n");
		return;
	}

	Msg(eDLL_T::MS, " |-- Hit pos: %.2f %.2f %.2f (fraction %.3f)\n",
		trace.endpos.x, trace.endpos.y, trace.endpos.z, trace.fraction);
	Msg(eDLL_T::MS, " |-- Surface: %s\n", trace.surface.name ? trace.surface.name : "<none>");

	// staticPropID = (propIndex & 0xFFFFFF) + 1; miss is <= 0.
	if (trace.staticPropID <= 0)
	{
		Msg(eDLL_T::MS, " |-- Not a static prop (staticPropID=%d -- world brush, entity, or sky).\n", trace.staticPropID);
		if (trace.hit_entity)
			Msg(eDLL_T::MS, " |-- (crosshair is on a dynamic entity, not a static prop)\n");
	}
	else
	{
		const unsigned int propIndex = (static_cast<unsigned int>(trace.staticPropID) & 0xFFFFFF) - 1;
		Msg(eDLL_T::MS, " |-- staticPropID: %d  (prop index %u)\n", trace.staticPropID, propIndex);

		char nameBuf[128];
		if (StaticProp_LookupModelName(propIndex, nameBuf, sizeof(nameBuf)))
			Msg(eDLL_T::MS, " |-- Model name: %s\n", nameBuf);
		else
			Msg(eDLL_T::MS, " |-- Model name: <not recorded> -- offline: python tools/prop_id_diag.py <mapdir> <mapname> <dec_rpak> %u\n",
				propIndex);
	}

	Msg(eDLL_T::MS, "--------------------------------------------------------------\n");
}

// FCVAR_RELEASE so Create does not auto-add DEVELOPMENTONLY.
static ConCommand mat_staticprop("mat_staticprop", Mat_StaticProp_f,
	"Print the static prop (model name + id) under the crosshair.",
	FCVAR_RELEASE | FCVAR_CLIENTDLL);

//-----------------------------------------------------------------------------
// mat_staticprop: engine-trace singleton + render-view camera, by byte pattern.
//-----------------------------------------------------------------------------
void VStaticPropMgr::GetVar(void) const
{
	// PlayerMelee_AttackTrace: mov r8d, 4640403Bh is the unique trace-mask site.
	g_ppEngineTraceClient = Module_FindPattern(g_GameDll,
		"48 8B 0D ?? ?? ?? ?? 48 8D 54 24 ?? C7 85 ?? ?? ?? ?? 02 00 00 00 4C 8B CE C7 85 ?? ?? ?? ?? 00 00 00 00 41 B8 3B 40 40 46")
		.ResolveRelativeAddressSelf(0x3, 0x7).RCast<CEngineTraceClient**>();

	// Render-view camera origin + forward: the distinctive back-to-back movss
	// block in GetMaterialAtCrossHair. origin from the leading
	// 'movss xmm0,[origin]', forward from 'movss xmm5,[forward]' at offset 0x1A.
	const CMemory viewBlock = Module_FindPattern(g_GameDll,
		"F3 0F 10 05 ?? ?? ?? ?? 33 C9 F3 0F 10 15 ?? ?? ?? ?? F3 0F 10 25 ?? ?? ?? ?? F3 0F 10 2D");

	if (viewBlock)
	{
		g_pMainViewOrigin  = viewBlock.Offset(0x00).ResolveRelativeAddressSelf(0x4, 0x8).RCast<const Vector3D*>();
		g_pMainViewForward = viewBlock.Offset(0x1A).ResolveRelativeAddressSelf(0x4, 0x8).RCast<const Vector3D*>();
	}

	if (!g_ppEngineTraceClient || !g_pMainViewOrigin || !g_pMainViewForward)
		Warning(eDLL_T::MS, "[mat_staticprop] resolve failed: traceClient=%p viewOrigin=%p viewForward=%p; command disabled\n",
			reinterpret_cast<void*>(g_ppEngineTraceClient), reinterpret_cast<const void*>(g_pMainViewOrigin), reinterpret_cast<const void*>(g_pMainViewForward));
}
#else // !CLIENT_DLL
#include "core/stdafx.h"
#include "datacache/mdlcache.h"
#include "engine/staticpropmgr.h"
#include "engine/debugoverlay.h"
#include "tier1/cvar.h"

// ConVars to control debug text for static props
ConVar debug_staticprop_text("debug_staticprop_text", "0", FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT, "Enable debug text for static props");
ConVar debug_staticprop_text_duration("debug_staticprop_text_duration", "5.0", FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT, "Duration (seconds) for static prop debug text");

//-----------------------------------------------------------------------------
// Purpose: initialises static props from the static prop gamelump
//-----------------------------------------------------------------------------
void* CStaticProp::Init(CStaticProp* thisptr, int64_t a2, unsigned int idx, unsigned int a4, StaticPropLump_t* lump, int64_t a6, int64_t a7)
{
    MDLHandle_t handle = *reinterpret_cast<uint16_t*>(a7 + 0x140);
    studiohdr_t* pStudioHdr = g_pMDLCache->FindMDL(g_pMDLCache, handle, nullptr);

    if (lump->m_Skin >= pStudioHdr->numskinfamilies)
    {
        Error(eDLL_T::ENGINE, NO_ERROR,
            "Invalid skin index for static prop #%i with model '%s' (got %i, max %i)\n",
            idx, pStudioHdr->name, lump->m_Skin, pStudioHdr->numskinfamilies-1);

        lump->m_Skin = 0;
    }

    // Call original init first — some static props are initialised before
    // the debug overlay system is ready; adding the overlay afterwards
    // increases the chance the text persists and is rendered.
    void* ret = CStaticProp__Init(thisptr, a2, idx, a4, lump, a6, a7);

    // Show debug text next to static props so tools can inspect map-placed models.
    // Uses the existing debug overlay interface which respects `enable_debug_text_overlays`.

    return ret;
}

//-----------------------------------------------------------------------------
// NOTE: the following gather props functions have been hooked as we must
// enable the old gather props logic for fall back models to draw !!! The
// new solution won't call CMDLCache::GetHardwareData on bad model handles.
//-----------------------------------------------------------------------------
void* GatherStaticPropsSecondPass_PreInit(GatherProps_t* gather)
{
    if (g_StudioMdlFallbackHandler.HasInvalidModelHandles())
        g_StudioMdlFallbackHandler.EnableLegacyGatherProps();

    return v_GatherStaticPropsSecondPass_PreInit(gather);
}
void* GatherStaticPropsSecondPass_PostInit(GatherProps_t* gather)
{
    if (g_StudioMdlFallbackHandler.HasInvalidModelHandles())
        g_StudioMdlFallbackHandler.EnableLegacyGatherProps();

    return v_GatherStaticPropsSecondPass_PostInit(gather);
}

void VStaticPropMgr::Detour(const bool bAttach) const
{

    DetourSetup(&v_GatherStaticPropsSecondPass_PreInit, &GatherStaticPropsSecondPass_PreInit, bAttach);
    DetourSetup(&v_GatherStaticPropsSecondPass_PostInit, &GatherStaticPropsSecondPass_PostInit, bAttach);
}
#endif // CLIENT_DLL
