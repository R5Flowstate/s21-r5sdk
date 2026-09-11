#ifndef ANIMATION_H
#define ANIMATION_H
#include "mathlib/vector.h"
#include "game/shared/ihandleentity.h"
#include "studio.h"
#include "predictioncopy.h"

class CAnimationLayer
{
	bool m_bSequenceFinished;
	char gap_1[3];
	int m_fFlags;
	int m_layerIndex;
	int m_modelIndex;
	float m_flKillRate;
	float m_flKillDelay;
	char m_nActivity[4];
	int m_nPriority;
	float m_flLastEventCheck;
	char gap_24[4];
	IHandleEntity* m_animationLayerOwner; // !TODO: CBaseEntity/C_BaseEntity?
};

struct PredictedAnimEventData
{
	void* __vftable;
	float m_predictedAnimEventTimes[8];
	int m_predictedAnimEventIndices[8];
	int m_predictedAnimEventCount;
	EHANDLE m_predictedAnimEventTarget;
	int m_predictedAnimEventSequence;
	int m_predictedAnimEventModel;
	float m_predictedAnimEventsReadyToFireTime;
	char gap_5C[4]; // <-- 64-BIT ALIGNMENT
};

struct AnimRelativeData
{
	void* __vftable;
	Vector3D m_animInitialPos;
	Vector3D m_animInitialVel;
	Quaternion m_animInitialRot;
	Vector3D m_animInitialCorrectPos;
	Quaternion m_animInitialCorrectRot;
	Vector3D m_animEntityToRefOffset;
	Quaternion m_animEntityToRefRotation;
	float m_animBlendBeginTime;
	float m_animBlendEndTime;
	int m_animScriptSequence;
	int m_animScriptModel;
	bool m_animIgnoreParentRot;
	char gap_79[3];
	int m_animMotionMode;
	bool m_safePushMode;
	char gap_81[7]; // <-- 64-BIT ALIGNMENT
};

struct Player_AnimViewEntityData
{
	void* __vftable;
	EHANDLE animViewEntityHandle;
	float animViewEntityAngleLerpInDuration;
	float animViewEntityOriginLerpInDuration;
	float animViewEntityLerpOutDuration;
	bool animViewEntityStabilizePlayerEyeAngles;
	char gap_19[3];
	int animViewEntityThirdPersonCameraParity;
	int animViewEntityThirdPersonCameraAttachment[6];
	int animViewEntityNumThirdPersonCameraAttachments;
	bool animViewEntityThirdPersonCameraVisibilityChecks;
	bool animViewEntityDrawPlayer;
	char gap_3e[2];
	float fovTarget;
	float fovSmoothTime;
	int animViewEntityParity;
	int lastAnimViewEntityParity;
	int lastAnimViewEntityParityTick;
	Vector3D animViewEntityCameraPosition;
	Vector3D animViewEntityCameraAngles;
	float animViewEntityBlendStartTime;
	Vector3D animViewEntityBlendStartEyePosition;
	Vector3D animViewEntityBlendStartEyeAngles;
};

inline int(*CStudioHdr__LookupSequence)(CStudioHdr* pStudio, const char* pszName);
inline int(*CBaseAnimating__LookupSequence)(void* pEntity, const char* pszName);
int Hook_CBaseAnimating_LookupSequence(void* pEntity, const char* pszName);

///////////////////////////////////////////////////////////////////////////////
class VAnimation : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CStudioHdr::LookupSequence", CStudioHdr__LookupSequence);
		LogFunAdr("CBaseAnimating::LookupSequence", CBaseAnimating__LookupSequence);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "40 53 48 83 EC 20 48 8B D9 4C 8B C2 48 8B 89 ?? ?? ?? ??").GetPtr(CStudioHdr__LookupSequence);
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 08 57 48 83 EC 20 48 83 B9 D8 0F 00 00 00 48 8B FA 48 8B D9 75 ?? "
			"0F BF 91 DE 00 00 00 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 08 48 85 C0 74 ?? "
			"48 8B CB E8 ?? ?? ?? ?? 48 8B 9B D8 0F 00 00 48 85 DB 74 ?? 48 83 7B 08 00 "
			"75 ?? 33 DB 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D7 48 8B CB "
			"48 8B 5C 24 30 48 83 C4 20 5F E9").GetPtr(CBaseAnimating__LookupSequence);
	}
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // ANIMATION_H
