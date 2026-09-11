//=============================================================================
//
// Purpose: raise S3 persistence DataDef caps to S21 capacity.
// Stock offsets stay: implicit IMUL/SHL/ADD bakes pdef-internal offsets into immediates.
//
//=============================================================================
#ifndef PERSISTENCE_EXT_H
#define PERSISTENCE_EXT_H

#include "thirdparty/detours/include/idetour.h"
#include <cstdint>
#include <cstddef>

///////////////////////////////////////////////////////////////////////////////
// Heap buffer used as the runtime pdef storage. nullptr if expansion was
// disabled at startup, allocation failed, or critical xref validation failed.
extern uint8_t* g_pSdkPdataDef;

// Runtime address of the original pdef storage. Set when the buffer is
// allocated; used as the source for substruct seeding and as the validation
// target for per-site xref checks.
extern uint8_t* g_pOrigPdataDef;

// Buffer layout constants. Defined in the.cpp; exposed here so the
// diagnostic detour (in persistence_diag.cpp, future) can read them when
// emitting the LoadDef summary line.
extern const size_t   kSdkPdefBufferSize;
extern const uint32_t kSdkPdefArrayBaseOff;
extern const uint32_t kSdkPdefEntryCount;
extern const uint32_t kSdkPdefCounterOff;
extern const uint32_t kSdkPdefSubTableOff;  // baseStruct ptr offset within g_pSdkPdataDef

///////////////////////////////////////////////////////////////////////////////
class VPersistenceExt : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const { }
	virtual void GetVar(void) const;
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // PERSISTENCE_EXT_H
