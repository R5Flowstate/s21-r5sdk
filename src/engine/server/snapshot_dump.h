//=============================================================================//
//
// Purpose: Snapshot dump/census (FLATN, MB-PACK, bonefollow, ring watch).
//
//=============================================================================//
#ifndef ENGINE_SERVER_SNAPSHOT_DUMP_H
#define ENGINE_SERVER_SNAPSHOT_DUMP_H

#include "thirdparty/detours/include/idetour.h"
#include <cstdint>

inline char(__fastcall* v_CreatePhysicsFollower)(int64_t a1, int64_t ownerEnt,
	int* out, int boneIndex) = nullptr;

void SnapshotDump_OnWriteEntityProps(int64_t frameObj, int* propIdxCursor, int serverClassId);
void SnapshotDump_OnFirstPack(void);
void SnapshotDump_OnRingInit(uintptr_t ring, uintptr_t entry0);

///////////////////////////////////////////////////////////////////////////////
class VSnapshotDump : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CreatePhysicsFollower", v_CreatePhysicsFollower);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // ENGINE_SERVER_SNAPSHOT_DUMP_H
