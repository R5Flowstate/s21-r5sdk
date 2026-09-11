//=============================================================================
//
// Purpose: [ANIM-ANCHOR-PROXY] Encode-time anim-anchor hold for non-player
// entities. The dedi re-anchors m_animStartTime/m_animStartCycle to "now"
// every tick; the S21 client extrapolates cycle from those stamps, so a
// per-tick re-anchored wire pins every server-animated entity at frame 0
// (dummies, deployables, 1p viewmodel, scripted anims). Players are held
// in-place by [ANIM-ANCHOR-HOLD] (snapshot_writer.cpp); for every other class
// the pair is live server integrator state and must never be written
// in-place, so the hold happens here at snapshot encode -- only the wire copy
// is transformed, server memory stays native. Server-only TU.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier1/cvar.h"
#include "game/shared/dt_extend.h"
#include "game/shared/sdk_entity_state.h"
#include <cstring>

static ConVar bridge_anim_anchor_proxy("bridge_anim_anchor_proxy", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Encode-time anchor hold for non-player entities: latch m_animStartTime/"
	"m_animStartCycle on the wire at sequence change / cycle restart instead of the "
	"per-tick re-anchor, so the S21 client extrapolates anim cycles. Server memory "
	"stays native. Players are held in-place by bridge_anim_anchor_hold. "
	"0 = raw re-anchored wire.");

static int      s_aapLastSeq[MAX_EDICTS];
static float    s_aapHeldStart[MAX_EDICTS];
static float    s_aapHeldStartCyc[MAX_EDICTS];
static float    s_aapPrevQCyc[MAX_EDICTS];
static uint32_t s_aapOccupant[MAX_EDICTS];
static bool     s_aapInit = false;

// Previous proxy per armed prop, so a stock proxy's transform is preserved.
struct AAPChainEntry
{
	uintptr_t       prop;
	DTExtendProxyFn prev;
};
static constexpr int kAapChainCap = 256;
static constexpr int kAnimAnchorHoldEdicts = 2048;
static_assert(MAX_EDICTS >= kAnimAnchorHoldEdicts,
	"proxy MAX_EDICTS arrays must cover hold-path 2048");

static AAPChainEntry s_aapChain[kAapChainCap];
static int           s_aapChainCount = 0;

static float s_aapTimeShiftEnt[MAX_EDICTS];
static uint8_t s_aapTimeShiftHaveEnt[MAX_EDICTS];

static DTExtendProxyFn AAP_PrevFor(const void* prop)
{
	for (int i = 0; i < s_aapChainCount; ++i)
	{
		if (s_aapChain[i].prop == reinterpret_cast<uintptr_t>(prop))
			return s_aapChain[i].prev;
	}
	return nullptr;
}

static bool AnimAnchorProxy_Latch(const void* pStruct, const int objectID)
{
	if (!pStruct || objectID < 0 || objectID >= MAX_EDICTS)
		return false;

	const uint32_t occupant = SDKEntityState_GetHandle(pStruct).Raw();
	if ((occupant & ENT_ENTRY_MASK) != static_cast<uint32_t>(objectID))
	{
		Warning(eDLL_T::ENGINE,
			"[ANIM-ANCHOR-PROXY] latch skew objectID=%d handle=0x%08X\n",
			objectID, occupant);
		return false;
	}

	if (!s_aapInit)
	{
		for (int i = 0; i < MAX_EDICTS; ++i)
		{
			s_aapLastSeq[i]      = -2;
			s_aapHeldStart[i]    = 0.0f;
			s_aapHeldStartCyc[i] = 0.0f;
			s_aapPrevQCyc[i]     = -1.0f;
			s_aapOccupant[i]     = 0;
			s_aapTimeShiftEnt[i] = 0.0f;
			s_aapTimeShiftHaveEnt[i] = 0;
		}
		s_aapInit = true;
	}

	// Recycled edict: drop the previous tenant's anchor before it publishes.
	if (occupant != s_aapOccupant[objectID])
	{
		s_aapOccupant[objectID]     = occupant;
		s_aapLastSeq[objectID]      = -2;
		s_aapHeldStart[objectID]    = 0.0f;
		s_aapHeldStartCyc[objectID] = 0.0f;
		s_aapPrevQCyc[objectID]     = -1.0f;
	}

	// pStruct is the entity base for DT_BaseAnimating leaf props:
	// m_animSequence +0xFE0, m_animStartTime +0xFEC, m_animStartCycle +0xFF0.
	const uint8_t* const ent = reinterpret_cast<const uint8_t*>(pStruct);
	const int   seq  = *reinterpret_cast<const int*>(ent + 0xFE0);
	const float nowT = *reinterpret_cast<const float*>(ent + 0xFEC);
	const float qCyc = *reinterpret_cast<const float*>(ent + 0xFF0);

	const bool seqChanged   = (seq != s_aapLastSeq[objectID]);
	const bool cycleRestart = (s_aapPrevQCyc[objectID] >= 0.0f && qCyc < s_aapPrevQCyc[objectID] - 0.05f)
	                       || (nowT < s_aapHeldStart[objectID]);
	if (seqChanged || cycleRestart)
	{
		s_aapHeldStart[objectID]    = nowT;
		s_aapHeldStartCyc[objectID] = qCyc;
		s_aapLastSeq[objectID]      = seq;
	}
	s_aapPrevQCyc[objectID] = qCyc;
	return true;
}

// Players (edicts 1..64) are held in-place by [ANIM-ANCHOR-HOLD].
static bool AnimAnchorProxy_Scope(const void* pStruct, const void* pOut, const int objectID)
{
	if (!bridge_anim_anchor_proxy.GetBool() || !pOut || !pStruct)
		return false;
	if (objectID >= 1 && objectID <= 64)
		return false;
	if (objectID < 0 || objectID >= MAX_EDICTS)
		return false;
	return true;
}

static void __fastcall AnimStartTime_HoldProxy(void* pProp, void* pStruct,
	void* pData, void* pOut, int iElement, int objectID)
{
	float* const out = reinterpret_cast<float*>(pOut);
	const DTExtendProxyFn prev = AAP_PrevFor(pProp);

	if (!AnimAnchorProxy_Scope(pStruct, pOut, objectID))
	{
		if (prev) prev(pProp, pStruct, pData, pOut, iElement, objectID);
		else if (out) *out = pData ? *reinterpret_cast<const float*>(pData) : 0.0f;
		return;
	}

	if (!AnimAnchorProxy_Latch(pStruct, objectID))
	{
		if (prev) prev(pProp, pStruct, pData, pOut, iElement, objectID);
		else if (out) *out = pData ? *reinterpret_cast<const float*>(pData) : 0.0f;
		return;
	}
	const float held = s_aapHeldStart[objectID];

	if (prev)
	{
		prev(pProp, pStruct, pData, pOut, iElement, objectID);
		const float live = pData ? *reinterpret_cast<const float*>(pData) : 0.0f;
		if (live > 0.0f)
		{
			s_aapTimeShiftEnt[objectID] = *out - live;
			s_aapTimeShiftHaveEnt[objectID] = 1;
		}
		const float shift = s_aapTimeShiftHaveEnt[objectID] ? s_aapTimeShiftEnt[objectID] : 0.0f;
		*out = (held > 0.0f) ? held + shift : held;
	}
	else
	{
		*out = held;
	}
}

static void __fastcall AnimStartCycle_HoldProxy(void* pProp, void* pStruct,
	void* pData, void* pOut, int iElement, int objectID)
{
	float* const out = reinterpret_cast<float*>(pOut);
	const DTExtendProxyFn prev = AAP_PrevFor(pProp);

	if (prev) prev(pProp, pStruct, pData, pOut, iElement, objectID);
	else if (out) *out = pData ? *reinterpret_cast<const float*>(pData) : 0.0f;

	if (!AnimAnchorProxy_Scope(pStruct, pOut, objectID))
		return;
	if (!AnimAnchorProxy_Latch(pStruct, objectID))
		return;
	*out = s_aapHeldStartCyc[objectID];
}

static int AnimAnchorProxy_InstallInTree(uint8_t* table, const char* propName,
	const SendPropType expectedType, DTExtendProxyFn ourProxy, int& nFound, int depth = 0)
{
	if (!table || depth > 32)
		return 0;

	uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
	const int nProps = *reinterpret_cast<const int*>(table + ST_NPROPS);
	if (!props || nProps <= 0 || nProps > 4096)
		return 0;

	int patched = 0;
	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* prop = props + static_cast<uint64_t>(i) * SP_SIZE;
		const char* name = *reinterpret_cast<const char**>(prop + SP_VARNAME);
		if (!name || strcmp(name, propName) != 0)
			continue;

		++nFound;
		const char* tableName = *reinterpret_cast<const char**>(table + ST_NETTABLENAME);

		const int spType = *reinterpret_cast<const int*>(prop + SP_TYPE);
		if (spType != static_cast<int>(expectedType))
		{
			Warning(eDLL_T::ENGINE,
				"[ANIM-ANCHOR-PROXY] %s.%s has type=%d (expected %d) -- occurrence NOT armed\n",
				tableName ? tableName : "?", propName, spType, static_cast<int>(expectedType));
			continue;
		}

		const DTExtendProxyFn curProxy = *reinterpret_cast<DTExtendProxyFn*>(prop + 0x60);
		if (curProxy == ourProxy)
			continue; // already armed (re-entry / shared sub-table)

		int slot = -1;
		for (int c = 0; c < s_aapChainCount; ++c)
		{
			if (s_aapChain[c].prop == reinterpret_cast<uintptr_t>(prop))
			{
				slot = c;
				break;
			}
		}
		if (slot < 0)
		{
			if (s_aapChainCount >= kAapChainCap)
			{
				Warning(eDLL_T::ENGINE,
					"[ANIM-ANCHOR-PROXY] chain table full -- %s.%s occurrence NOT armed\n",
					tableName ? tableName : "?", propName);
				continue;
			}
			slot = s_aapChainCount++;
		}
		s_aapChain[slot].prop = reinterpret_cast<uintptr_t>(prop);
		s_aapChain[slot].prev = curProxy;

		*reinterpret_cast<uintptr_t*>(prop + 0x60) = reinterpret_cast<uintptr_t>(ourProxy);
		++patched;
	}

	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* prop = props + static_cast<uint64_t>(i) * SP_SIZE;
		if (*reinterpret_cast<const int*>(prop + SP_TYPE) != static_cast<int>(SendPropType::DPT_DataTable))
			continue;
		uint8_t* child = *reinterpret_cast<uint8_t**>(prop + SP_CHILDTABLE);
		patched += AnimAnchorProxy_InstallInTree(child, propName, expectedType, ourProxy, nFound, depth + 1);
	}

	return patched;
}

// Arms every occurrence across all top-level trees (the props live on
// DT_BaseAnimating, inherited everywhere). Must run BEFORE the precalc flatten
// (the flatten copies SendProps; a post-flatten install lands on tree props the
// encoder no longer reads) and is idempotent for the common-tail re-arm.
void AnimAnchorProxy_Install(void** tables, int count)
{
	if (!tables || count <= 0)
		return;

	struct AnchorPropInstall_t
	{
		const char*     pszName;
		SendPropType    expectedType;
		DTExtendProxyFn pProxy;
	};
	const AnchorPropInstall_t kAnchorProps[2] = {
		{ "m_animStartTime",  SendPropType::DPT_Time,  &AnimStartTime_HoldProxy  },
		{ "m_animStartCycle", SendPropType::DPT_Float, &AnimStartCycle_HoldProxy },
	};

	for (int p = 0; p < 2; ++p)
	{
		int nFound = 0;
		int nPatched = 0;
		for (int t = 0; t < count; ++t)
		{
			uint8_t* topTable = reinterpret_cast<uint8_t*>(tables[t]);
			if (!topTable)
				continue;
			nPatched += AnimAnchorProxy_InstallInTree(topTable, kAnchorProps[p].pszName,
				kAnchorProps[p].expectedType, kAnchorProps[p].pProxy, nFound);
		}

		if (nFound == 0)
		{
			Warning(eDLL_T::ENGINE,
				"[ANIM-ANCHOR-PROXY] %s prop not found -- encode-time anchor hold NOT armed\n",
				kAnchorProps[p].pszName);
			continue;
		}
		if (nPatched > 0)
		{
			Msg(eDLL_T::ENGINE,
				"[ANIM-ANCHOR-PROXY] %s hold armed found=%d armed=%d\n",
				kAnchorProps[p].pszName, nFound, nPatched);
		}
	}
}

// Latch state is per-map (clock domain + edict tenancy); the chain table is
// process-lifetime like the SendTables it indexes.
void AnimAnchorProxy_LevelShutdown()
{
	s_aapInit = false;
	memset(s_aapTimeShiftEnt, 0, sizeof(s_aapTimeShiftEnt));
	memset(s_aapTimeShiftHaveEnt, 0, sizeof(s_aapTimeShiftHaveEnt));
}
