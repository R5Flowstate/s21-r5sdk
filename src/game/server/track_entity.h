//=============================================================================//
//
// Purpose: S21 third-person track-entity camera fields S3 CPlayer does not
// have (blend-out, right offset, OverTime lerps). Sidecar + DT_ThirdPersonView
// value proxies; the nested S3 ThirdPersonViewData is not grown.
//
//=============================================================================//
#ifndef TRACK_ENTITY_H
#define TRACK_ENTITY_H

#include "thirdparty/detours/include/idetour.h"
#include "game/shared/dt_extend.h"
#include <cstdint>

struct ScriptClassDescriptor_t;

struct TrackEntityWire
{
	float   m_blendOutDuration;
	float   m_fixedRight;
	float   m_varDistStart;
	float   m_varDistEnd;
	float   m_varDistStartTime;
	float   m_varDistEndTime;
	int32_t m_varDistLerpType;
	int32_t m_varDistLogGrowth;
	float   m_varHeightStart;
	float   m_varHeightEnd;
	float   m_varHeightStartTime;
	float   m_varHeightEndTime;
	int32_t m_varHeightLerpType;
	int32_t m_varHeightLogGrowth;
	float   m_varRightStart;
	float   m_varRightEnd;
	float   m_varRightStartTime;
	float   m_varRightEndTime;
	int32_t m_varRightLerpType;
	int32_t m_varRightLogGrowth;
};

bool TrackEntity_GetWire(const void* pPlayer, TrackEntityWire* pOut);
void TrackEntity_LevelShutdown(void);
DTExtendProxyFn TrackEntity_ValueProxyForProp(const char* propName);
void TrackEntity_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct);
void TrackEntity_RegisterScriptConstants(class CSquirrelVM* s);

///////////////////////////////////////////////////////////////////////////////
class VTrackEntity : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // TRACK_ENTITY_H
