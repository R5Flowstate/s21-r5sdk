#if defined(CLIENT_DLL)
#ifndef SCRIPTNETDATA_LIMITS_H
#define SCRIPTNETDATA_LIMITS_H

//=============================================================================//
//
// Purpose: ScriptNetData defensive engine patch (IDetour).
//
//=============================================================================//

#include "thirdparty/detours/include/idetour.h"

class VScriptNetDataLimits : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const {}
	virtual void Detour(const bool bAttach) const;
};

#endif // SCRIPTNETDATA_LIMITS_H
#else // !CLIENT_DLL
#ifndef SCRIPTNETDATA_LIMITS_H
#define SCRIPTNETDATA_LIMITS_H

//=============================================================================//
//
// Purpose: ScriptNetData overflow handling via SDK extension buffers.
//
//=============================================================================//

#include "thirdparty/detours/include/idetour.h"

//-----------------------------------------------------------------------------
// Internal type indices (match engine's AllocateInternalVar second arg)
//-----------------------------------------------------------------------------
enum SNDCInternalType
{
	SNDC_ITYPE_BOOL   = 0,
	SNDC_ITYPE_RANGE  = 1,
	SNDC_ITYPE_BIGINT = 2,
	SNDC_ITYPE_TIME   = 3,
	SNDC_ITYPE_ENTITY = 4,
	SNDC_ITYPE_COUNT  = 5
};

//-----------------------------------------------------------------------------
// Entity type indices
//-----------------------------------------------------------------------------
enum SNDCEntityType
{
	SNDC_ENT_GLOBAL            = 0,
	SNDC_ENT_PLAYER_GLOBAL     = 1,
	SNDC_ENT_PLAYER_EXCLUSIVE  = 2,
	SNDC_ENT_TITAN_SOUL        = 3,
	SNDC_ENT_DEATH_BOX         = 4,
	SNDC_ENT_COUNT             = 5
};

//-----------------------------------------------------------------------------
// Per-entity-type configuration
//-----------------------------------------------------------------------------
struct SNDCArrayConfig
{
	int origCount;
	int targetCount;
	int origOffset;
	int elemSize;
};

struct SNDCEntityConfig
{
	const char* name;
	int origEntitySize;
	SNDCArrayConfig arrays[SNDC_ITYPE_COUNT];
	int extEntitySize;
	int extOffsets[SNDC_ITYPE_COUNT];
};

//-----------------------------------------------------------------------------
// NET_ScriptMessage-based extension replication
//-----------------------------------------------------------------------------
static constexpr uint8_t SNDC_MSG_MAGIC   = 0xA7;
static constexpr uint8_t SNDC_MSG_VERSION = 1;
static constexpr uint8_t SNDC_SUBMSG_DELTA    = 0;
static constexpr uint8_t SNDC_SUBMSG_FULLSYNC = 1;
static constexpr int     SNDC_MAX_EXT_VARS = 128;

struct SNDCExtVarMeta
{
	uint8_t internalType;
	uint8_t sizeBytes;
};

struct SNDCExtCategory
{
	int totalExtVars;
	SNDCExtVarMeta meta[SNDC_MAX_EXT_VARS];
	int typeStartIdx[SNDC_ITYPE_COUNT];
	int typeExtCount[SNDC_ITYPE_COUNT];

	int32_t  values[MAX_PLAYERS][SNDC_MAX_EXT_VARS];
	uint8_t  dirty[MAX_PLAYERS][(SNDC_MAX_EXT_VARS + 7) / 8];
	bool     anyDirty[MAX_PLAYERS];
	int32_t  clientValues[MAX_PLAYERS][SNDC_MAX_EXT_VARS];
};

extern SNDCExtCategory g_sndcExt[SNDC_ENT_COUNT];
extern bool g_sndcExtActive;

// Replication
void SNDC_FlushDirtyVars(void* pServer);
class CClient;
void SNDC_SendFullSync(CClient* pClient);
class NET_ScriptMessage;
bool SNDC_ProcessClientMessage(NET_ScriptMessage* pMsg);

// Lifecycle
void SNDC_ExtensionLevelShutdown();

// PE_EXPANDED twin-surface fix: C++ vtable with S21 array offsets so
// SetPlayerNet* writes match PE_EXPANDED SendProp (see scriptnetdata_limits.cpp).
void* SNDC_GetPEExpandedVtable(void);
void  SNDC_ApplyPEExpandedVtable(void* entity);
void  SNDC_ApplyNativeLayoutIfBacked(void);

// Callbacks
void SNDC_QueueDeferredCallback(const char* name, int32_t oldVal, int32_t newVal);
void SNDC_TrackNonRewindCallback(const char* name);

//-----------------------------------------------------------------------------
// Script natives
//-----------------------------------------------------------------------------
#include "vscript/languages/squirrel_re/include/squirrel.h"

// Registration + detection (all VMs)
SQRESULT ServerScript_SDKRegisterNetVar(HSQUIRRELVM v);
SQRESULT ServerScript_SDKIsOverflowVar(HSQUIRRELVM v);
SQRESULT Script_SDKGetNetworkedVariableIndex(HSQUIRRELVM v);
inline SQRESULT ClientScript_SDKRegisterNetVar(HSQUIRRELVM v) { return ServerScript_SDKRegisterNetVar(v); }
inline SQRESULT UIScript_SDKRegisterNetVar(HSQUIRRELVM v) { return ServerScript_SDKRegisterNetVar(v); }
inline SQRESULT ClientScript_SDKIsOverflowVar(HSQUIRRELVM v) { return ServerScript_SDKIsOverflowVar(v); }
inline SQRESULT UIScript_SDKIsOverflowVar(HSQUIRRELVM v) { return ServerScript_SDKIsOverflowVar(v); }
inline SQRESULT ServerScript_SDKGetNetworkedVariableIndex(HSQUIRRELVM v) { return Script_SDKGetNetworkedVariableIndex(v); }
inline SQRESULT ClientScript_SDKGetNetworkedVariableIndex(HSQUIRRELVM v) { return Script_SDKGetNetworkedVariableIndex(v); }
inline SQRESULT UIScript_SDKGetNetworkedVariableIndex(HSQUIRRELVM v) { return Script_SDKGetNetworkedVariableIndex(v); }

// Server Set (5 types)
SQRESULT ServerScript_SDKSetGlobalNetBool(HSQUIRRELVM v);
SQRESULT ServerScript_SDKSetGlobalNetInt(HSQUIRRELVM v);
SQRESULT ServerScript_SDKSetGlobalNetFloat(HSQUIRRELVM v);
SQRESULT ServerScript_SDKSetGlobalNetTime(HSQUIRRELVM v);
SQRESULT ServerScript_SDKSetGlobalNetEntity(HSQUIRRELVM v);

// Server notify (script-side callback trigger)
SQRESULT ServerScript_SDKNotifyVarChanged(HSQUIRRELVM v);
SQRESULT ServerScript_SDKNotifyFloatVarChanged(HSQUIRRELVM v);

// Server Get (5 types) — reads from values
SQRESULT ServerScript_SDKGetGlobalNetBool(HSQUIRRELVM v);
SQRESULT ServerScript_SDKGetGlobalNetInt(HSQUIRRELVM v);
SQRESULT ServerScript_SDKGetGlobalNetFloat(HSQUIRRELVM v);
SQRESULT ServerScript_SDKGetGlobalNetTime(HSQUIRRELVM v);
SQRESULT ServerScript_SDKGetGlobalNetEntity(HSQUIRRELVM v);

// Client/UI Get (5 types) — reads from clientValues
SQRESULT ClientScript_SDKGetGlobalNetInt(HSQUIRRELVM v);
SQRESULT ClientScript_SDKGetGlobalNetFloat(HSQUIRRELVM v);
SQRESULT ClientScript_SDKGetGlobalNetBool(HSQUIRRELVM v);
SQRESULT ClientScript_SDKGetGlobalNetTime(HSQUIRRELVM v);
SQRESULT ClientScript_SDKGetGlobalNetEntity(HSQUIRRELVM v);
inline SQRESULT UIScript_SDKGetGlobalNetInt(HSQUIRRELVM v) { return ClientScript_SDKGetGlobalNetInt(v); }
inline SQRESULT UIScript_SDKGetGlobalNetFloat(HSQUIRRELVM v) { return ClientScript_SDKGetGlobalNetFloat(v); }
inline SQRESULT UIScript_SDKGetGlobalNetBool(HSQUIRRELVM v) { return ClientScript_SDKGetGlobalNetBool(v); }
inline SQRESULT UIScript_SDKGetGlobalNetTime(HSQUIRRELVM v) { return ClientScript_SDKGetGlobalNetTime(v); }
inline SQRESULT UIScript_SDKGetGlobalNetEntity(HSQUIRRELVM v) { return ClientScript_SDKGetGlobalNetEntity(v); }

// Client callbacks
SQRESULT ClientScript_SDKProcessCallbacks(HSQUIRRELVM v);
inline SQRESULT UIScript_SDKProcessCallbacks(HSQUIRRELVM v) { return ClientScript_SDKProcessCallbacks(v); }

//-----------------------------------------------------------------------------
// IDetour registration
//-----------------------------------------------------------------------------
class VScriptNetDataLimits : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const {}
	virtual void Detour(const bool bAttach) const;
};

#endif // SCRIPTNETDATA_LIMITS_H
#endif // CLIENT_DLL
