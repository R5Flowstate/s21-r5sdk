//=============================================================================//
//
// Purpose: Script remote-function shared path
//
//=============================================================================//
#if defined(CLIENT_DLL)
#ifndef SCRIPTREMOTEFUNCTIONS_SHARED_H
#define SCRIPTREMOTEFUNCTIONS_SHARED_H

constexpr int SCRIPT_REMOTE_SERVER_MAX_PARAMS = 8;
constexpr int SCRIPT_REMOTE_SERVER_MAX_STRING_LEN = 255;
constexpr int SCRIPT_REMOTE_SERVER_MAX_FUNCTIONS = 768;
constexpr int SCRIPT_REMOTE_FUNC_INDEX_BITS = 10;

// Remote function parameter type byte values
enum class ScriptRemoteParamType_e : uint8_t
{
	SRP_FLOAT         = 1,
	SRP_VECTOR        = 3,
	SRP_INT           = 5,
	SRP_BOOL          = 6,
	SRP_ENTITY        = 40,
	SRP_TYPED_ENTITY  = 41,
	SRP_ITEMFLAVOR    = 42,
};

struct ScriptRemoteParamDesc_t
{
	ScriptRemoteParamType_e type;
	int intMin;
	int intMax;
	float floatMin;
	float floatMax;
	int floatBits;
	char szEntityClass[64]; // SRP_TYPED_ENTITY only: required native classname (e.g. "prop_death_box"); empty = SRP_ENTITY (no check)
};

struct ScriptRemoteFuncDesc_t
{
	char szName[128];
	uint16_t nIndex;
	int nParamCount;
	ScriptRemoteParamDesc_t params[SCRIPT_REMOTE_SERVER_MAX_PARAMS];
};

bool ScriptRemoteServer_RegisterFunction(const char* pszName, int nParamCount,
	const ScriptRemoteParamDesc_t* pParams);
const ScriptRemoteFuncDesc_t* ScriptRemoteServer_FindFunction(const char* pszName);
const ScriptRemoteFuncDesc_t* ScriptRemoteServer_GetFunctionByIndex(uint16_t nIndex);
int ScriptRemoteServer_GetFunctionCount();
uint32_t ScriptRemoteServer_CalcChecksum();
void ScriptRemoteServer_LockRegistrations();
void ScriptRemoteServer_ClearRegistrations();


// Variable-width int bits for [intMin, intMax]; range via int64, clamped 0..32.
int ScriptRemote_IntBitsForRange(int intMin, int intMax);

#endif // SCRIPTREMOTEFUNCTIONS_SHARED_H
#else // !CLIENT_DLL
#ifndef SCRIPTREMOTEFUNCTIONS_SHARED_H
#define SCRIPTREMOTEFUNCTIONS_SHARED_H

#include "vscript/languages/squirrel_re/include/squirrel.h"

#include <cmath>
#include <cstdint>

class bf_write;
class bf_read;
struct ScriptVariant_t;

constexpr int SCRIPT_REMOTE_SERVER_MAX_PARAMS = 8;
constexpr int SCRIPT_REMOTE_SERVER_MAX_STRING_LEN = 255;
constexpr int SCRIPT_REMOTE_SERVER_MAX_FUNCTIONS = 768;
constexpr int SCRIPT_REMOTE_FUNC_INDEX_BITS = 10;

// Remote function parameter type byte values
enum class ScriptRemoteParamType_e : uint8_t
{
	SRP_FLOAT         = 1,
	SRP_VECTOR        = 3,
	SRP_INT           = 5,
	SRP_BOOL          = 6,
	SRP_ENTITY        = 40,
	SRP_TYPED_ENTITY  = 41,
	SRP_ITEMFLAVOR    = 42,
};

struct ScriptRemoteParamDesc_t
{
	ScriptRemoteParamType_e type;
	int intMin;
	int intMax;
	float floatMin;
	float floatMax;
	int floatBits;
	char szEntityClass[64]; // SRP_TYPED_ENTITY only: required native classname (e.g. "prop_death_box"); empty = SRP_ENTITY (no check)
};

// If set, L2 calls this with decoded args instead of looking up an SQ function.
typedef void(*ScriptRemoteRecvNativeCb_t)(const ScriptVariant_t* args, int argCount);

struct ScriptRemoteFuncDesc_t
{
	char szName[128];
	uint16_t nIndex;
	int nParamCount;
	ScriptRemoteParamDesc_t params[SCRIPT_REMOTE_SERVER_MAX_PARAMS];
	ScriptRemoteRecvNativeCb_t nativeRecvCb;   // nullptr = dispatch via SQ FindFunction
};

// C->S allowlist: server-callable functions on the SERVER VM. Registered via Remote_RegisterServerFunction.
bool ScriptRemoteServer_RegisterFunction(const char* pszName, int nParamCount,
	const ScriptRemoteParamDesc_t* pParams);
const ScriptRemoteFuncDesc_t* ScriptRemoteServer_FindFunction(const char* pszName);
const ScriptRemoteFuncDesc_t* ScriptRemoteServer_GetFunctionByIndex(uint16_t nIndex);
int ScriptRemoteServer_GetFunctionCount();
uint32_t ScriptRemoteServer_CalcChecksum();
void ScriptRemoteServer_LockRegistrations();
void ScriptRemoteServer_ClearRegistrations();


//-----------------------------------------------------------------------------
// Shared bit-pack codec. Returns false on validation failure.
//-----------------------------------------------------------------------------
#include "game/shared/scriptremote_scalecodec.h"

// Live reader policy over the engine bitbuffer. Out-of-line in the shared
// TU so includers without bitbuf.h never need the complete bf_read type.
struct ScriptRemote_BfReadAdapter
{
	bf_read& m_in;
	uint32_t ReadUBits(int nBits);
	float ReadFloat();
	int32_t ReadLong();
	int ReadOneBit();
};

bool ScriptRemote_EncodeParam(
	bf_write& out, HSQUIRRELVM v, SQInteger sqIdx,
	const ScriptRemoteParamDesc_t& desc,
	const char* errorPrefix, const char* fnName, int paramIdx);

bool ScriptRemote_DecodeParam(
	bf_read& in, const ScriptRemoteParamDesc_t& desc,
	ScriptVariant_t* outArgs, int& outArgWriteIdx, int outArgsCapacity,
	const char* errorPrefix, const char* fnName, int paramIdx);

//-----------------------------------------------------------------------------
// S->C ScriptRemote: name-carried NET_ScriptMessage (m_bIsTyped=0). snapshotTick holds until current.
//-----------------------------------------------------------------------------

// Must byte-match the client copy. Distinct from SNDC_MSG_MAGIC (0xA7).
constexpr uint32_t BRIDGE_S2C_SCRIPTREMOTE_MAGIC = 0x53523244u;

// Mantle-boost verdict frame on the same lane: magic + packed state (MantleBoost_EncodeState).
constexpr uint32_t BRIDGE_S2C_MANTLEBOOST_MAGIC = 0x3156424Du;

constexpr int BRIDGE_S2C_SCRIPTREMOTE_MAX_NAME_LEN = 64;
constexpr int BRIDGE_S2C_SCRIPTREMOTE_MAX_ARGS = 16;
constexpr int BRIDGE_S2C_SCRIPTREMOTE_MAX_STRING_LEN = 255;

// Native S21 schema type codes as the wire typeTag. Excludes itemflavor (0x2A).
enum class BridgeS2CScriptRemoteType_e : uint8_t
{
	FLOAT        = 1,
	VECTOR       = 3,
	INT          = 5,
	BOOL         = 6,
	STRING       = 0x22,
	ENTITY       = 0x28,
	TYPED_ENTITY = 0x29,
};

// One normalized argument, ready to encode. Only the field matching `type`
// is read; `str`/`strLen` are only valid when type==STRING.
struct BridgeS2CScriptRemoteArg_t
{
	BridgeS2CScriptRemoteType_e type;
	union
	{
		float    f;
		int32_t  i;
		bool     b;
		uint32_t ehandle;
		float    vec[3];
	};
	const char* str;
	int strLen;
};

// Encodes one S2C ScriptRemote frame. Fail closed; never send a partial frame.
bool Bridge_S2C_ScriptRemote_EncodeFrame(
	const char* pszName, int nameLen, bool bIsUI, uint8_t replayMode,
	uint32_t snapshotTick,
	int argCount, const BridgeS2CScriptRemoteArg_t* pArgs,
	bf_write& out);

#endif // SCRIPTREMOTEFUNCTIONS_SHARED_H
#endif // CLIENT_DLL
