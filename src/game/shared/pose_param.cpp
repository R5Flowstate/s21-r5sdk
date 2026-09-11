//=============================================================================//
//
// Purpose: Clamp OOB pose-parameter indices in Studio_LocalPoseParameter.
//
//=============================================================================//
#include "core/stdafx.h"
#include "core/logdef.h"
#include "tier0/dbg.h"
#include "tier0/memaddr.h"
#include "tier0/module.h"
#include "tier1/cvar.h"
#include "game/shared/pose_param.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// CStudioHdr / virtualmodel / seqdesc offsets (S21 client).
static constexpr ptrdiff_t CSTUDIO_OFF_STUDIOHDR = 0x08;
static constexpr ptrdiff_t CSTUDIO_OFF_VMODEL    = 0x10;
static constexpr ptrdiff_t VMODEL_OFF_POSECOUNT  = 0x06; // u16 m_poseCount
static constexpr ptrdiff_t VMODEL_OFF_SEQ        = 0x10; // m_seq*
static constexpr ptrdiff_t VMODEL_OFF_GROUP      = 0x28; // m_group*
static constexpr ptrdiff_t SEQ_STRIDE           = 0x10;
static constexpr ptrdiff_t SEQ_OFF_GROUP        = 0x04; // i16 group
static constexpr ptrdiff_t GROUP_STRIDE         = 0x48; // 72
static constexpr ptrdiff_t GROUP_OFF_MASTERPOSE = 0x08; // i16 masterPoseCount
static constexpr ptrdiff_t GROUP_OFF_POSEMAP    = 0x28; // u16* masterPose
static constexpr ptrdiff_t HDR_OFF_NUMLOCALPOSE = 0xA2; // u16 numLocalPoseParameters
static constexpr ptrdiff_t SEQDESC_OFF_PARAMIDX = 0x2C; // i16 paramIndex[2]

static ConVar sdk_pose_param_guard("sdk_pose_param_guard", "1", FCVAR_RELEASE,
	"Guard Studio_LocalPoseParameter against OOB virtualmodel pose-index "
	"remaps that AV the pose value array. 0 = pass-through.");

static int s_logBudget = 32;

//-----------------------------------------------------------------------------
// CStudioHdr::GetNumPoseParameters -- vmodel m_poseCount else hdr local count.
//-----------------------------------------------------------------------------
static int GetNumPoseParameters(const __int64 studioHdr)
{
	if (!studioHdr)
		return 0;

	__try
	{
		const __int64 vmodel = *reinterpret_cast<const __int64*>(studioHdr + CSTUDIO_OFF_VMODEL);
		if (vmodel)
			return *reinterpret_cast<const unsigned __int16*>(vmodel + VMODEL_OFF_POSECOUNT);

		const __int64 hdr = *reinterpret_cast<const __int64*>(studioHdr + CSTUDIO_OFF_STUDIOHDR);
		if (!hdr)
			return 0;
		return *reinterpret_cast<const unsigned __int16*>(hdr + HDR_OFF_NUMLOCALPOSE);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return 0;
	}
}

//-----------------------------------------------------------------------------
// CStudioHdr::GetSharedPoseParameter with masterPoseCount bounds restored.
// Returns -1 for invalid / 0xFFFF.
//-----------------------------------------------------------------------------
static int GetSharedPoseParameter(
	const __int64 studioHdr,
	const unsigned __int16 sequence,
	const int localPoseIdx)
{
	if (localPoseIdx < 0 || localPoseIdx == 0xFFFF)
		return -1;

	__try
	{
		const __int64 vmodel = *reinterpret_cast<const __int64*>(studioHdr + CSTUDIO_OFF_VMODEL);
		if (!vmodel)
			return localPoseIdx;

		const __int64 seqBase = *reinterpret_cast<const __int64*>(vmodel + VMODEL_OFF_SEQ);
		if (!seqBase)
			return -1;

		const int group = *reinterpret_cast<const __int16*>(
			seqBase + SEQ_STRIDE * static_cast<__int64>(sequence) + SEQ_OFF_GROUP);
		if (group < 0)
			return -1;

		const __int64 groupBase = *reinterpret_cast<const __int64*>(vmodel + VMODEL_OFF_GROUP);
		if (!groupBase)
			return -1;

		const __int64 groupPtr = groupBase + GROUP_STRIDE * static_cast<__int64>(group);
		const int masterPoseCount = *reinterpret_cast<const __int16*>(groupPtr + GROUP_OFF_MASTERPOSE);
		if (localPoseIdx >= masterPoseCount)
			return -1;

		const __int64 poseMap = *reinterpret_cast<const __int64*>(groupPtr + GROUP_OFF_POSEMAP);
		if (!poseMap)
			return -1;

		return *reinterpret_cast<const unsigned __int16*>(poseMap + 2LL * localPoseIdx);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return -1;
	}
}

static __int64 __fastcall CallOriginalSEH(
	__int64 studioHdr,
	const float* poseValues,
	const void* seqdesc,
	unsigned __int16 sequence,
	unsigned __int16 localIndex,
	float* flSetting)
{
	__try
	{
		return v_Studio_LocalPoseParameter(
			studioHdr, poseValues, seqdesc, sequence, localIndex, flSetting);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		if (flSetting)
			*flSetting = 0.0f;
		if (s_logBudget > 0)
		{
			--s_logBudget;
			Warning(eDLL_T::ENGINE,
				"[POSE-PARAM-GUARD] SEH caught AV in Studio_LocalPoseParameter "
				"seq=%u dim=%u -- zeroed blend\n",
				sequence, localIndex);
		}
		return 0;
	}
}

static __int64 __fastcall Hook_Studio_LocalPoseParameter(
	__int64 studioHdr,
	const float* poseValues,
	const void* seqdesc,
	unsigned __int16 sequence,
	unsigned __int16 localIndex,
	float* flSetting)
{
	if (!sdk_pose_param_guard.GetBool() || !studioHdr || !seqdesc || !flSetting)
	{
		if (!v_Studio_LocalPoseParameter)
			return 0;
		return v_Studio_LocalPoseParameter(
			studioHdr, poseValues, seqdesc, sequence, localIndex, flSetting);
	}

	// Preflight the same index the native will load so a garbage remap never
	// reaches poseValues[idx].
	int localPoseIdx = -1;
	__try
	{
		if (localIndex <= 1)
		{
			localPoseIdx = *reinterpret_cast<const __int16*>(
				reinterpret_cast<const char*>(seqdesc)
				+ SEQDESC_OFF_PARAMIDX + 2LL * localIndex);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		*flSetting = 0.0f;
		return 0;
	}

	if (localPoseIdx == static_cast<__int16>(0xFFFF) || localPoseIdx < 0)
	{
		// Native treats 0xFFFF as "no param" -- pass through.
		return CallOriginalSEH(
			studioHdr, poseValues, seqdesc, sequence, localIndex, flSetting);
	}

	const int shared = GetSharedPoseParameter(studioHdr, sequence, localPoseIdx);
	const int numPose = GetNumPoseParameters(studioHdr);

	if (shared < 0 || (numPose > 0 && shared >= numPose) || (numPose == 0 && shared > 0))
	{
		*flSetting = 0.0f;
		if (s_logBudget > 0)
		{
			--s_logBudget;
			Warning(eDLL_T::ENGINE,
				"[POSE-PARAM-GUARD] blocked OOB pose idx local=%d shared=%d "
				"numPose=%d seq=%u dim=%u\n",
				localPoseIdx, shared, numPose, sequence, localIndex);
		}
		return 0;
	}

	return CallOriginalSEH(
		studioHdr, poseValues, seqdesc, sequence, localIndex, flSetting);
}

void VPoseParamGuardS21::GetFun(void) const
{
	// Studio_LocalPoseParameter -- unique movsx localIndex after frame setup.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 7C 24 ?? "
		"41 56 48 83 EC ?? 48 0F BF AC 24")
		.GetPtr(v_Studio_LocalPoseParameter);

	if (!v_Studio_LocalPoseParameter)
		Warning(eDLL_T::ENGINE,
			"[POSE-PARAM-GUARD] pattern UNRESOLVED -- guard NOT installed\n");
}

void VPoseParamGuardS21::Detour(const bool bAttach) const
{
	if (v_Studio_LocalPoseParameter)
		DetourSetup(&v_Studio_LocalPoseParameter, &Hook_Studio_LocalPoseParameter, bAttach);
}
