//=============================================================================//
//
// Purpose: Pathfinder grapple rope draws from chest instead of hand when
// m_playAnimationType ordinal drifts (wire 5 vs local 6). Three hooks rewrite
// 5->6 while grapple-active so IsPlayingGrappleAnimation / rope origin stay correct.
//
//=============================================================================//
#ifndef CLIENT_GRAPPLE_ROPE_DIAG_H
#define CLIENT_GRAPPLE_ROPE_DIAG_H

#include "thirdparty/detours/include/idetour.h"

// The sparse gating/reselect helper (S21 ). a2 is a pointer
// (real call sites do `lea rdx, [rsp+local_buf]`), not an int -- do not
// change this back to int, it truncates the pointer and crashes.
inline char (*v_GrappleAnimSelectSequence)(void* player, void* a2, char a3) = nullptr;

// The actual per-frame gate (S21 ) RopeRenderer_DrawGrappleRope
// checks every frame via IsPlayingGrappleAnimation_Client.
inline bool (*v_IsPlayingGrappleAnimation_Client)(void* player) = nullptr;

// The per-tick movement/think function (S21 ) whose
// switch(m_playAnimationType) misroutes case 5 to case 4's handler.
inline int64_t (*v_MovePostThinkAnimSwitch)(void* player) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VGrappleRopeDiag : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("GrappleAnimSelectSequence", v_GrappleAnimSelectSequence);
		LogFunAdr("IsPlayingGrappleAnimation_Client", v_IsPlayingGrappleAnimation_Client);
		LogFunAdr("MovePostThinkAnimSwitch", v_MovePostThinkAnimSwitch);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // CLIENT_GRAPPLE_ROPE_DIAG_H
