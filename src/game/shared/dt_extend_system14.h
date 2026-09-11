//=============================================================================//
//
// Purpose: dt_extend SYSTEM 14 -- legacy SendTable graft path
//
//=============================================================================//
#ifndef DT_EXTEND_SYSTEM14_H
#define DT_EXTEND_SYSTEM14_H

#include "game/shared/dt_extend.h"

void DTExtend_Apply(void** tables, int count);
void DTExtend_RelocateProps(void);
void DTExtend_CopyProps(void);
void DTExtend_InsertWrapperTables(void);
void DTExtend_BracketizeSubTableChildren(void** tables, int count);
void DTExtend_ExtendOffhandWeaponsTo8(void** tables, int count);
void DTExtend_BuildDeathFieldArrays(void** tables, int count);
void DTExtend_BuildNonRewindMiscArray(void** tables, int count);
void DTExtend_BuildConnectionQualityIndex(void** tables, int count);
void DTExtend_PostApplyS21Overrides(void** tables, int count);
int DTExtend_AuditAllClassBacking(void);
void DTExtend_ZiprailBuildOwnPrecalc(void);
void DTExtend_DeathFieldReinstallPostInit(void);
void DTExtend_NonRewindMiscReinstallPostInit(void);
void DTExtend_NonRewindMiscLevelShutdown(void);
void DTExtend_S21ChainAudit(void);

#endif // DT_EXTEND_SYSTEM14_H
