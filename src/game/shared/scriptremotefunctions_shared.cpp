//=============================================================================//
//
// Purpose: Script remote-function shared path
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier1/bitbuf.h"
#include "tier1/convar.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "scriptremotefunctions_shared.h"
#include "game/shared/sdk_entity_state.h"
#include "game/server/baseentity.h"

#include <cmath>

// === C->S allowlist storage ===
static ScriptRemoteFuncDesc_t s_allowlist[SCRIPT_REMOTE_SERVER_MAX_FUNCTIONS];
static int s_nAllowlistCount = 0;
static bool s_bRegistrationLocked = false;

bool ScriptRemoteServer_RegisterFunction(const char* pszName, int nParamCount,
	const ScriptRemoteParamDesc_t* pParams)
{
	if (s_bRegistrationLocked)
	{
		Warning(eDLL_T::SERVER, "ScriptRemoteServer: registration locked, rejecting '%s'\n", pszName);
		return false;
	}

	if (!pszName || !*pszName)
	{
		Warning(eDLL_T::SERVER, "ScriptRemoteServer: empty function name\n");
		return false;
	}

	if (nParamCount < 0 || nParamCount > SCRIPT_REMOTE_SERVER_MAX_PARAMS)
	{
		Warning(eDLL_T::SERVER, "ScriptRemoteServer: '%s' has %d params (max %d)\n",
			pszName, nParamCount, SCRIPT_REMOTE_SERVER_MAX_PARAMS);
		return false;
	}

	if (s_nAllowlistCount >= SCRIPT_REMOTE_SERVER_MAX_FUNCTIONS)
	{
		Warning(eDLL_T::SERVER, "ScriptRemoteServer: allowlist full\n");
		return false;
	}

	for (int i = 0; i < s_nAllowlistCount; i++)
	{
		if (strcmp(s_allowlist[i].szName, pszName) == 0)
			return true;
	}

	ScriptRemoteFuncDesc_t& entry = s_allowlist[s_nAllowlistCount];
	V_strncpy(entry.szName, pszName, sizeof(entry.szName));
	entry.nIndex = static_cast<uint16_t>(s_nAllowlistCount);
	entry.nParamCount = nParamCount;

	for (int i = 0; i < nParamCount; i++)
		entry.params[i] = pParams[i];

	s_nAllowlistCount++;

	DevMsg(eDLL_T::SERVER, "ScriptRemoteServer: registered '%s' (index=%d, params=%d)\n",
		pszName, entry.nIndex, nParamCount);
	return true;
}

const ScriptRemoteFuncDesc_t* ScriptRemoteServer_FindFunction(const char* pszName)
{
	for (int i = 0; i < s_nAllowlistCount; i++)
	{
		if (strcmp(s_allowlist[i].szName, pszName) == 0)
			return &s_allowlist[i];
	}
	return nullptr;
}

const ScriptRemoteFuncDesc_t* ScriptRemoteServer_GetFunctionByIndex(uint16_t nIndex)
{
	if (nIndex >= static_cast<uint16_t>(s_nAllowlistCount))
		return nullptr;
	return &s_allowlist[nIndex];
}

int ScriptRemoteServer_GetFunctionCount()
{
	return s_nAllowlistCount;
}

uint32_t ScriptRemoteServer_CalcChecksum()
{
	constexpr uint32_t FNV_PRIME = 0x01000193;
	constexpr uint32_t FNV_OFFSET = 0x811c9dc5;

	uint32_t hash = FNV_OFFSET;

	for (int i = 0; i < s_nAllowlistCount; i++)
	{
		const ScriptRemoteFuncDesc_t& entry = s_allowlist[i];

		for (const char* p = entry.szName; *p; p++)
		{
			hash ^= static_cast<uint32_t>(*p);
			hash *= FNV_PRIME;
		}

		hash ^= static_cast<uint32_t>(entry.nParamCount);
		hash *= FNV_PRIME;

		for (int j = 0; j < entry.nParamCount; j++)
		{
			const uint8_t* pBytes = reinterpret_cast<const uint8_t*>(&entry.params[j]);
			for (size_t k = 0; k < sizeof(entry.params[j]); k++)
			{
				hash ^= pBytes[k];
				hash *= FNV_PRIME;
			}
		}
	}

	return hash;
}

void ScriptRemoteServer_LockRegistrations()
{
	s_bRegistrationLocked = true;
	DevMsg(eDLL_T::SERVER, "ScriptRemoteServer: locked (%d functions)\n", s_nAllowlistCount);
}

void ScriptRemoteServer_ClearRegistrations()
{
	s_nAllowlistCount = 0;
	s_bRegistrationLocked = false;
	memset(s_allowlist, 0, sizeof(s_allowlist));
}


//-----------------------------------------------------------------------------
// Shared bit-pack codec. Wire format is identical both directions.
//-----------------------------------------------------------------------------

#include "public/vscript/ivscript.h"

uint32_t ScriptRemote_BfReadAdapter::ReadUBits(int nBits) { return m_in.ReadUBitLong(nBits); }
float ScriptRemote_BfReadAdapter::ReadFloat() { return m_in.ReadFloat(); }
int32_t ScriptRemote_BfReadAdapter::ReadLong() { return m_in.ReadLong(); }
int ScriptRemote_BfReadAdapter::ReadOneBit() { return m_in.ReadOneBit(); }

// Entity arg at sqIdx: OT_ENTITY userdata at SQInstance+0x50. Bare v_sq_getentity
// reads a fixed slot and is wrong here; +0x38 is an unrelated instance field.
static constexpr int SCRIPT_REMOTE_ENTITY_USERPOINTER_OFFSET = 0x50;

static void* ScriptRemote_EntityPtrFromStack(HSQUIRRELVM v, SQInteger sqIdx)
{
	const SQObjectPtr& o = stack_get(v, sqIdx);
	if (sq_isnull(o))
		return nullptr;
	if (o._type != OT_ENTITY || !o._unVal.pInstance)
		return nullptr;

	return *reinterpret_cast<void**>(
		reinterpret_cast<uintptr_t>(o._unVal.pInstance) + SCRIPT_REMOTE_ENTITY_USERPOINTER_OFFSET);
}

bool ScriptRemote_EncodeParam(
	bf_write& out, HSQUIRRELVM v, SQInteger sqIdx,
	const ScriptRemoteParamDesc_t& desc,
	const char* errorPrefix, const char* fnName, int paramIdx)
{
	switch (desc.type)
	{
	case ScriptRemoteParamType_e::SRP_BOOL:
	{
		SQBool val = false;
		sq_getbool(v, sqIdx, &val);
		out.WriteOneBit(val ? 1 : 0);
		return true;
	}
	case ScriptRemoteParamType_e::SRP_INT:
	{
		SQInteger val = 0;
		sq_getinteger(v, sqIdx, &val);

		const int intVal = static_cast<int>(val);
		if (intVal < desc.intMin || intVal > desc.intMax)
		{
			v_SQVM_RaiseError(v, "%s: '%s' arg %d value %d out of range [%d, %d]\n",
				errorPrefix, fnName, paramIdx, intVal, desc.intMin, desc.intMax);
			return false;
		}

		const int bits = ScriptRemote_IntBitsForRange(desc.intMin, desc.intMax);
		out.WriteUBitLong(static_cast<unsigned int>(intVal - desc.intMin), bits);
		return true;
	}
	case ScriptRemoteParamType_e::SRP_FLOAT:
	{
		SQFloat val = 0.0f;
		sq_getfloat(v, sqIdx, &val);

		if (desc.floatBits >= 32)
		{
			out.WriteFloat(val);
		}
		else
		{
			const float range = desc.floatMax - desc.floatMin;
			if (range <= 0.0f || desc.floatMin >= desc.floatMax)
			{
				v_SQVM_RaiseError(v, "%s: '%s' arg %d float quantize range invalid [%f, %f]\n",
					errorPrefix, fnName, paramIdx, desc.floatMin, desc.floatMax);
				return false;
			}
			if (val < desc.floatMin || val > desc.floatMax)
			{
				v_SQVM_RaiseError(v, "%s: '%s' arg %d value %f out of range [%f, %f]\n",
					errorPrefix, fnName, paramIdx, val, desc.floatMin, desc.floatMax);
				return false;
			}
			const unsigned int maxVal = (1u << desc.floatBits) - 1;
			const unsigned int encoded = static_cast<unsigned int>(
				((val - desc.floatMin) / range) * maxVal + 0.5f);
			out.WriteUBitLong(encoded, desc.floatBits);
		}
		return true;
	}
	case ScriptRemoteParamType_e::SRP_VECTOR:
	{
		const SQVector3D* vec = nullptr;
		sq_getvector(v, sqIdx, &vec);

		float c[3] = { vec ? vec->x : 0.0f, vec ? vec->y : 0.0f, vec ? vec->z : 0.0f };

		if (desc.floatBits >= 32)
		{
			for (int i = 0; i < 3; i++)
				out.WriteFloat(c[i]);
		}
		else
		{
			const float range = desc.floatMax - desc.floatMin;
			if (range <= 0.0f || desc.floatMin >= desc.floatMax)
			{
				v_SQVM_RaiseError(v, "%s: '%s' arg %d vector quantize range invalid [%f, %f]\n",
					errorPrefix, fnName, paramIdx, desc.floatMin, desc.floatMax);
				return false;
			}
			const unsigned int maxVal = (1u << desc.floatBits) - 1;
			for (int i = 0; i < 3; i++)
			{
				if (c[i] < desc.floatMin || c[i] > desc.floatMax)
				{
					v_SQVM_RaiseError(v, "%s: '%s' arg %d vector component %d out of range\n",
						errorPrefix, fnName, paramIdx, i);
					return false;
				}
				const unsigned int encoded = static_cast<unsigned int>(
					((c[i] - desc.floatMin) / range) * maxVal + 0.5f);
				out.WriteUBitLong(encoded, desc.floatBits);
			}
		}
		return true;
	}
	case ScriptRemoteParamType_e::SRP_ENTITY:
	case ScriptRemoteParamType_e::SRP_TYPED_ENTITY:
	{
		void* pEnt = ScriptRemote_EntityPtrFromStack(v, sqIdx);
		if (pEnt)
		{
			// m_RefEHandle.m_Index at entity+0x8
			const int ehandle = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(pEnt) + 8);
			out.WriteLong(ehandle);
		}
		else
		{
			out.WriteLong(0);
		}
		return true;
	}
	case ScriptRemoteParamType_e::SRP_ITEMFLAVOR:
	{
		SQInteger val = 0;
		sq_getinteger(v, sqIdx, &val);
		out.WriteLong(static_cast<int>(val));
		return true;
	}
	default:
		v_SQVM_RaiseError(v, "%s: '%s' arg %d unknown type %d\n",
			errorPrefix, fnName, paramIdx, (int)desc.type);
		return false;
	}
}

//-----------------------------------------------------------------------------
// Same-layout accessor for CBaseEntity::m_iClassname. Zero-offset downcast.
//-----------------------------------------------------------------------------
namespace
{
	class ScriptRemoteEntityFieldAccess : public CBaseEntity
	{
	public:
		using CBaseEntity::m_iClassname;
	};
}

static const char* ScriptRemote_EntityClassname(CBaseEntity* pEnt)
{
	if (!pEnt)
		return nullptr;

	auto* const accessor = static_cast<ScriptRemoteEntityFieldAccess*>(pEnt);
	if (!accessor->m_iClassname)
		return nullptr;

	return STRING(accessor->m_iClassname);
}

bool ScriptRemote_DecodeParam(
	bf_read& in, const ScriptRemoteParamDesc_t& desc,
	ScriptVariant_t* outArgs, int& outArgWriteIdx, int outArgsCapacity,
	const char* errorPrefix, const char* fnName, int paramIdx)
{
	auto needSlots = [&](int n) -> bool {
		if (outArgWriteIdx + n > outArgsCapacity)
		{
			Warning(eDLL_T::COMMON, "%s: '%s' arg %d: ScriptVariant capacity exceeded\n",
				errorPrefix, fnName, paramIdx);
			return false;
		}
		return true;
	};

	switch (desc.type)
	{
	case ScriptRemoteParamType_e::SRP_BOOL:
	case ScriptRemoteParamType_e::SRP_INT:
	case ScriptRemoteParamType_e::SRP_FLOAT:
	case ScriptRemoteParamType_e::SRP_VECTOR:
	case ScriptRemoteParamType_e::SRP_ITEMFLAVOR:
	{
		if (!needSlots(1)) return false;
		ScriptRemote_BfReadAdapter reader{ in };
		ScriptRemoteScalarOut_t scalar;
		if (!ScriptRemote_DecodeScalar(reader,
			static_cast<uint8_t>(desc.type),
			desc.intMin, desc.intMax, desc.floatMin, desc.floatMax, desc.floatBits,
			scalar))
		{
			switch (scalar.reject)
			{
			case ScriptRemoteScalarReject_e::SCALAR_INT_RANGE:
				Warning(eDLL_T::COMMON, "%s: '%s' arg %d value %d out of range [%d, %d]\n",
					errorPrefix, fnName, paramIdx, scalar.nRejectAux, desc.intMin, desc.intMax);
				return false;
			case ScriptRemoteScalarReject_e::SCALAR_FLOAT_INVALID:
				Warning(eDLL_T::COMMON, "%s: '%s' arg %d float invalid (%f, range [%f,%f])\n",
					errorPrefix, fnName, paramIdx, scalar.flRejectVal, desc.floatMin, desc.floatMax);
				return false;
			case ScriptRemoteScalarReject_e::SCALAR_VECTOR_INVALID:
				Warning(eDLL_T::COMMON, "%s: '%s' arg %d vector component %d invalid (%f, range [%f,%f])\n",
					errorPrefix, fnName, paramIdx, scalar.nRejectAux, scalar.flRejectVal, desc.floatMin, desc.floatMax);
				return false;
			case ScriptRemoteScalarReject_e::SCALAR_UNKNOWN_TYPE:
				Warning(eDLL_T::COMMON, "%s: '%s' arg %d unknown type %d\n",
					errorPrefix, fnName, paramIdx, (int)desc.type);
				return false;
			default:
				return false;
			}
		}
		switch (desc.type)
		{
		case ScriptRemoteParamType_e::SRP_BOOL:
			outArgs[outArgWriteIdx++] = (scalar.bBool);
			return true;
		case ScriptRemoteParamType_e::SRP_INT:
		case ScriptRemoteParamType_e::SRP_ITEMFLAVOR:
			outArgs[outArgWriteIdx++] = scalar.nInt;
			return true;
		case ScriptRemoteParamType_e::SRP_FLOAT:
			outArgs[outArgWriteIdx++] = scalar.flFloat;
			return true;
		default:
			// Package as ONE Vector3D-typed ScriptVariant_t (FIELD_VECTOR), matching
			// live "vector"-registered handlers' formal params.
			outArgs[outArgWriteIdx++] = Vector3D(scalar.vec[0], scalar.vec[1], scalar.vec[2]);
			return true;
		}
	}
	case ScriptRemoteParamType_e::SRP_ENTITY:
	case ScriptRemoteParamType_e::SRP_TYPED_ENTITY:
	{
		// Wire carries a CBaseHandle-style EHandle (see ScriptRemote_EncodeParam's
		// mirror case: entity+0x8, WriteLong(0) if unbound).
		if (!needSlots(1)) return false;
		const int ehandle = in.ReadLong();

		HSCRIPT hEnt = nullptr;
		if (ehandle == 0)
		{
			// Encode side's explicit "no entity bound" sentinel -- a normal,
			// expected value (e.g. an unset optional preferred-loot-entity
			// arg), not an error. Script already null-checks via IsValid.
		}
		else
		{
			void* const pEnt = SDKEntityState_Resolve(
				SDKEntityHandle(static_cast<uint32_t>(ehandle)), ESide::Server);
			if (!pEnt)
			{
				// Stale handle (entity destroyed between click and packet
				// arrival) -- a benign race, not a protocol violation. Let the
				// script's own IsValid-style guard handle the null.
				Warning(eDLL_T::COMMON, "%s: '%s' arg %d: stale/unresolved entity handle 0x%08X\n",
					errorPrefix, fnName, paramIdx, static_cast<uint32_t>(ehandle));
			}
			else if (desc.type == ScriptRemoteParamType_e::SRP_TYPED_ENTITY && desc.szEntityClass[0])
			{
				// Script input is attacker-controlled; enforce the registered classname.
				CBaseEntity* const pTypedEnt = reinterpret_cast<CBaseEntity*>(pEnt);
				const char* const pszActual = ScriptRemote_EntityClassname(pTypedEnt);
				if (!pszActual || strcmp(pszActual, desc.szEntityClass) != 0)
				{
					Warning(eDLL_T::COMMON, "%s: '%s' arg %d: entity classname '%s' does not match required '%s' (handle 0x%08X) -- rejecting\n",
						errorPrefix, fnName, paramIdx, pszActual ? pszActual : "(null)", desc.szEntityClass,
						static_cast<uint32_t>(ehandle));
					return false;
				}
				hEnt = pTypedEnt->GetScriptInstance();
			}
			else
			{
				hEnt = reinterpret_cast<CBaseEntity*>(pEnt)->GetScriptInstance();
			}
		}

		outArgs[outArgWriteIdx++] = hEnt;
		return true;
	}
	default:
		Warning(eDLL_T::COMMON, "%s: '%s' arg %d unknown type %d\n",
			errorPrefix, fnName, paramIdx, (int)desc.type);
		return false;
	}
}

//-----------------------------------------------------------------------------
// Bridge S->C ScriptRemote frame encode. See header for layout. Byte-aligned
// throughout (WriteByte/WriteLong/WriteFloat only -- no bit-packing), so the
// frame is a straightforward memory-safe walk with no width negotiation.
//-----------------------------------------------------------------------------
bool Bridge_S2C_ScriptRemote_EncodeFrame(
	const char* pszName, int nameLen, bool bIsUI, uint8_t replayMode,
	uint32_t snapshotTick,
	int argCount, const BridgeS2CScriptRemoteArg_t* pArgs,
	bf_write& out)
{
	if (!pszName || nameLen <= 0 || nameLen > BRIDGE_S2C_SCRIPTREMOTE_MAX_NAME_LEN)
	{
		Warning(eDLL_T::SERVER, "[BRIDGE-S2C-SR] encode: bad name length %d\n", nameLen);
		return false;
	}

	if (argCount < 0 || argCount > BRIDGE_S2C_SCRIPTREMOTE_MAX_ARGS)
	{
		Warning(eDLL_T::SERVER, "[BRIDGE-S2C-SR] encode: bad arg count %d for '%.*s'\n",
			argCount, nameLen, pszName);
		return false;
	}

	// Pre-validate every arg BEFORE writing anything -- fail closed, never
	// send a partially-encoded frame (frozen spec requirement).
	for (int i = 0; i < argCount; i++)
	{
		const BridgeS2CScriptRemoteArg_t& a = pArgs[i];
		if (a.type == BridgeS2CScriptRemoteType_e::STRING)
		{
			if (!a.str || a.strLen < 0 || a.strLen > BRIDGE_S2C_SCRIPTREMOTE_MAX_STRING_LEN)
			{
				Warning(eDLL_T::SERVER, "[BRIDGE-S2C-SR] encode: '%.*s' arg %d bad string length %d\n",
					nameLen, pszName, i, a.strLen);
				return false;
			}
		}
	}

	out.WriteLong(static_cast<int>(BRIDGE_S2C_SCRIPTREMOTE_MAGIC));
	out.WriteLong(static_cast<int>(snapshotTick));
	out.WriteByte(bIsUI ? 1 : 0);
	out.WriteByte(static_cast<int>(replayMode));
	out.WriteByte(nameLen);
	out.WriteBytes(pszName, nameLen);
	out.WriteByte(argCount);

	for (int i = 0; i < argCount; i++)
	{
		const BridgeS2CScriptRemoteArg_t& a = pArgs[i];
		out.WriteByte(static_cast<int>(a.type));

		switch (a.type)
		{
		case BridgeS2CScriptRemoteType_e::FLOAT:
			out.WriteFloat(a.f);
			break;
		case BridgeS2CScriptRemoteType_e::VECTOR:
			out.WriteFloat(a.vec[0]);
			out.WriteFloat(a.vec[1]);
			out.WriteFloat(a.vec[2]);
			break;
		case BridgeS2CScriptRemoteType_e::INT:
			out.WriteLong(a.i);
			break;
		case BridgeS2CScriptRemoteType_e::BOOL:
			out.WriteByte(a.b ? 1 : 0);
			break;
		case BridgeS2CScriptRemoteType_e::STRING:
			out.WriteByte(a.strLen);
			out.WriteBytes(a.str, a.strLen);
			break;
		case BridgeS2CScriptRemoteType_e::ENTITY:
		case BridgeS2CScriptRemoteType_e::TYPED_ENTITY:
			out.WriteLong(static_cast<int>(a.ehandle));
			break;
		default:
			// Unreachable: caller (the send-side hook) never places an
			// unrecognized type into pArgs -- it fails the whole call
			// before reaching here. Loud stub in case that contract breaks.
			Warning(eDLL_T::SERVER, "[BRIDGE-S2C-SR] encode: '%.*s' arg %d has unencodable type %d (bug)\n",
				nameLen, pszName, i, static_cast<int>(a.type));
			return false;
		}
	}

	if (out.IsOverflowed())
	{
		Warning(eDLL_T::SERVER, "[BRIDGE-S2C-SR] encode: '%.*s' overflowed the output buffer\n",
			nameLen, pszName);
		return false;
	}

	return true;
}
