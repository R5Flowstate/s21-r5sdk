//=============================================================================//
//
// Purpose: Cafe dedicated server-VM natives (Register* helpers)
//
//=============================================================================//
#ifndef VSCRIPT_SERVER_NATIVES_H
#define VSCRIPT_SERVER_NATIVES_H

#include "vscript/languages/squirrel_re/include/squirrel.h"

class CSquirrelVM;
struct ScriptClassDescriptor_t;
struct ScriptVariant_t;

void Script_RegisterDedicatedS21ServerNatives(CSquirrelVM* s);
void Script_RegisterDedicatedEntityNatives(ScriptClassDescriptor_t* entityStruct);
void Script_RegisterDedicatedPlayerNatives(ScriptClassDescriptor_t* playerStruct);
bool ServerScript_IsDedicatedRuntime(void);
void* ServerScript_EntityPtrFromStackIdx(HSQUIRRELVM v, SQInteger sqIdx);
SQRESULT ServerScript_FS_StatsIngest(HSQUIRRELVM v);
void Script_DedicatedTraceDetour(const bool bAttach);
bool BreachTrace_CallBoolCallback(const HSCRIPT hFunc, const ScriptVariant_t& arg);
void BreachTrace_LevelShutdown(void);

#endif // VSCRIPT_SERVER_NATIVES_H
