//=============================================================================//
//
// Purpose: Extend script pose-parameter accessors past the S3 native 12 slots.
// Indices 12..23 live in a side table keyed by edict; wire via dt_extend.
// Also authors move_yaw onto the native array after server animstate Update.
//
//=============================================================================//
#ifndef POSEPARAM_EXT_DEDI_H
#define POSEPARAM_EXT_DEDI_H

#include "thirdparty/detours/include/idetour.h"

// Wire value proxy for m_flPoseParameter elements 12..23. Installed by
// dt_extend's grow pass on the grown child SendTable; the element index is
// read back from the prop's stashed SP_OFFSET.
void __fastcall PoseParamExt_WireProxy(void* pProp, void* pStruct,
	void* pData, void* pOut, int iElement, int objectID);

// Extended slots published on the wire (24 - 12).
int PoseParamExt_GetWireSlotCount(void);
void PoseParamExt_LevelShutdown(void);

///////////////////////////////////////////////////////////////////////////////
class VPoseParamExt : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // POSEPARAM_EXT_DEDI_H
