//=============================================================================//
//
// Purpose: Live snapshot frame-store ring deepen (Snapshot_Initialize).
//
//=============================================================================//
#ifndef ENGINE_SERVER_SNAPSHOT_RING_H
#define ENGINE_SERVER_SNAPSHOT_RING_H

#include "thirdparty/detours/include/idetour.h"

inline int64_t (*v_Snapshot_Initialize)(int64_t, uint8_t, uint8_t, int, int, int, int) = nullptr;

uintptr_t SnapshotRing_Mgr(void);
int SnapshotRing_Depth(void);
bool SnapshotRing_HasTick(int nTick, int* pOldest, int* pNewest);

///////////////////////////////////////////////////////////////////////////////
class VSnapshotRingDeepen : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("Snapshot_Initialize", v_Snapshot_Initialize);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // ENGINE_SERVER_SNAPSHOT_RING_H
