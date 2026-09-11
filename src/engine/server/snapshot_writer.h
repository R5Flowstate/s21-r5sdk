//=============================================================================//
//
// Purpose: Live snapshot encode/write path (pack, bf_write, time encode).
//
//=============================================================================//
#ifndef ENGINE_SERVER_SNAPSHOT_WRITER_H
#define ENGINE_SERVER_SNAPSHOT_WRITER_H

#include "thirdparty/detours/include/idetour.h"
#include <cstdint>

inline char (*v_SGE_WriteSnapshotMsg28)(int64_t a1, int a2, int64_t a3,
	int64_t a4, uint32_t* a5) = nullptr;
inline int64_t (*v_SGE_WriteEntityProps)(int64_t a1, int64_t a2, int64_t a3,
	int a4, int64_t a5, int* a6, int* a7, int64_t* a8) = nullptr;
inline int64_t (*v_DT_EncodePropValue)(int a1, int a2, int a3, uint32_t a4,
	uint32_t* a5, int64_t a6, int64_t* a7) = nullptr;
inline int64_t (*v_bf_WriteUBitLong)(int64_t a1, unsigned int a2, int a3) = nullptr;
inline int64_t (*v_bf_WriteUBitLongRaw)(int64_t a1, unsigned int a2, int a3) = nullptr;
inline bool (*v_bf_WriteBits)(int64_t a1, unsigned int* a2, int64_t a3) = nullptr;
inline unsigned __int64 (*v_SGE_CollectChangedProps)(int64_t a1,
	unsigned int a2, unsigned int a3, int a4, int64_t a5, int64_t a6,
	int64_t a7, int64_t a8) = nullptr;
inline int64_t (*v_SGE_MergeSnapshotBucket)(int64_t a1, uint64_t* src,
	uint64_t* dst, int srcIndex, int dstIndex, uint32_t* cursor) = nullptr;
inline void* (*v_SGE_ResetSnapshotBucket)(int64_t bucket) = nullptr;
inline int64_t (*v_SGE_PackEntityProps)(int64_t a1, uint64_t* a2, int64_t a3,
	int16_t a4, int16_t a5, int a6, void* a7, int64_t a8, int64_t a9,
	int a10) = nullptr;
inline void (*v_SGE_SnapPoolCtor)(uint64_t* pool, int count, int a3,
	int a4) = nullptr;
inline void (*v_SGE_SnapPoolDtor)(uint64_t* pool) = nullptr;
inline void (__fastcall *v_AnimAnchorUpdate)(__int64 ent) = nullptr;

void SnapshotWriter_LevelShutdown(void);

///////////////////////////////////////////////////////////////////////////////
class VSnapshotWriterTrace : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("SGE_WriteSnapshotMsg28", v_SGE_WriteSnapshotMsg28);
		LogFunAdr("SGE_WriteEntityProps", v_SGE_WriteEntityProps);
		LogFunAdr("DT_EncodePropValue", v_DT_EncodePropValue);
		LogFunAdr("bf_WriteUBitLong", v_bf_WriteUBitLong);
		LogFunAdr("bf_WriteUBitLongRaw", v_bf_WriteUBitLongRaw);
		LogFunAdr("bf_WriteBits", v_bf_WriteBits);
		LogFunAdr("SGE_CollectChangedProps", v_SGE_CollectChangedProps);
		LogFunAdr("SGE_MergeSnapshotBucket", v_SGE_MergeSnapshotBucket);
		LogFunAdr("SGE_ResetSnapshotBucket", v_SGE_ResetSnapshotBucket);
		LogFunAdr("SGE_PackEntityProps", v_SGE_PackEntityProps);
		LogFunAdr("SGE_SnapPoolCtor", v_SGE_SnapPoolCtor);
		LogFunAdr("SGE_SnapPoolDtor", v_SGE_SnapPoolDtor);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // ENGINE_SERVER_SNAPSHOT_WRITER_H
