//=============================================================================//
//
// Purpose: fallback skydive sim, one step per usercmd. Stands down when
// FullTossMove already authors the dive -- two writers cannot converge.
//
//=============================================================================//
#ifndef SKYDIVE_BRIDGE_H
#define SKYDIVE_BRIDGE_H

#include "thirdparty/detours/include/idetour.h"
#include "mathlib/vector.h"

class CPlayer;
class CUserCmd;

// Server freefall state ordinals. Both builds use the same domain; the wire
// renames m_freefallState -> m_skydiveState without remapping the values.
enum PlayerFreefallState_t
{
	PLAYER_FREEFALL_STATE_NONE         = 0,
	PLAYER_FREEFALL_STATE_DIVING       = 1,
	PLAYER_FREEFALL_STATE_ANTICIPATING = 2
};

// Per-usercmd tick before movement. No-ops if not freefalling or engine already simulates.
void SkydiveBridge_Think(CPlayer* player, CUserCmd* ucmd, float flFrameTime);

// Raise engine freefall. Pass current velocity so the handoff stays continuous.
bool SkydiveBridge_BeginFreefall(void* pPlayer, const Vector3D& vecInitialVelocity);

// Force the landing-anticipation sub-state. Requires a live dive.
bool SkydiveBridge_BeginFreefallAnticipate(void* pPlayer);

// True while this player is freefalling at all (state != NONE).
bool SkydiveBridge_IsFreefalling(const void* pPlayer);

// PLAYER_FREEFALL_STATE_*
int  SkydiveBridge_GetFreefallState(const void* pPlayer);

// True while the ride should hold the landing-anticipation pose. From-updraft
// is the second gate when the S21 settings field is unavailable.
bool SkydiveBridge_ForceAnticipation(const void* pPlayer);

// Thin wrapper over the engine's class-mod accessor. False when unresolved.
bool SkydiveBridge_IsClassModActive(void* pPlayer, const char* pszModName);

struct ScriptClassDescriptor_t;
void SkydiveBridge_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct);

///////////////////////////////////////////////////////////////////////////////
class VSkydiveBridge : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // SKYDIVE_BRIDGE_H
