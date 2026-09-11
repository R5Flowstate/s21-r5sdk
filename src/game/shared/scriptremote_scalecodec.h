//=============================================================================//
//
// Purpose: Engine-free scalar decode core for ScriptRemote wire params.
// The reader is a policy (live: adapter over bf_read; tests: bit cursor),
// values come out as plain data. Wire type bytes are the frozen S3 protocol
// (FLOAT 1, VECTOR 3, INT 5, BOOL 6, ITEMFLAVOR 42); both sides pass their
// enum cast to uint8_t. Reject detail travels out for the caller's log line;
// the core itself never logs, allocates, or touches engine state.
//
//=============================================================================//
#ifndef SCRIPTREMOTE_SCALARCODEC_H
#define SCRIPTREMOTE_SCALARCODEC_H

#include <cmath>
#include <cstdint>

// Variable-width int bits for [intMin, intMax]; range via int64, clamped 0..32.
inline int ScriptRemote_IntBitsForRange(int intMin, int intMax)
{
	if (intMax < intMin)
		return 0;

	const int64_t range64 = static_cast<int64_t>(intMax) - static_cast<int64_t>(intMin);
	if (range64 <= 0)
		return 0;

	uint64_t range = static_cast<uint64_t>(range64);
	int bits = 0;
	for (uint64_t r = range; r; r >>= 1)
		bits++;
	if (bits > 32)
		bits = 32;
	return bits;
}

enum class ScriptRemoteScalarReject_e : uint8_t
{
	SCALAR_OK = 0,
	SCALAR_INT_RANGE,
	SCALAR_FLOAT_INVALID,
	SCALAR_FLOAT_EMPTY,
	SCALAR_VECTOR_INVALID,
	SCALAR_VECTOR_EMPTY,
	SCALAR_UNKNOWN_TYPE,
};

struct ScriptRemoteScalarOut_t
{
	bool bBool;
	int nInt;
	float flFloat;
	float vec[3];
	int nRejectAux;
	float flRejectVal;
	ScriptRemoteScalarReject_e reject;
};

// R models a sequential bit reader: uint32_t ReadUBits(int nBits);
// float ReadFloat(); int32_t ReadLong(); int ReadOneBit().
template<typename R>
inline bool ScriptRemote_DecodeScalar(R& in, uint8_t type,
	int intMin, int intMax, float floatMin, float floatMax, int floatBits,
	ScriptRemoteScalarOut_t& out)
{
	switch (type)
	{
	case 6: // BOOL
		out.bBool = (in.ReadOneBit() != 0);
		out.reject = ScriptRemoteScalarReject_e::SCALAR_OK;
		return true;
	case 5: // INT
	{
		const int bits = ScriptRemote_IntBitsForRange(intMin, intMax);
		const int val = static_cast<int>(in.ReadUBits(bits)) + intMin;
		if (val < intMin || val > intMax)
		{
			out.nRejectAux = val;
			out.reject = ScriptRemoteScalarReject_e::SCALAR_INT_RANGE;
			return false;
		}
		out.nInt = val;
		out.reject = ScriptRemoteScalarReject_e::SCALAR_OK;
		return true;
	}
	case 1: // FLOAT
	{
		float val;
		if (floatBits >= 32)
		{
			val = in.ReadFloat();
		}
		else
		{
			const unsigned int maxVal = (1u << floatBits) - 1;
			if (maxVal == 0)
			{
				out.reject = ScriptRemoteScalarReject_e::SCALAR_FLOAT_EMPTY;
				return false;
			}
			const unsigned int encoded = in.ReadUBits(floatBits);
			const float range = floatMax - floatMin;
			val = floatMin + (static_cast<float>(encoded) / static_cast<float>(maxVal)) * range;
		}
		// At >=32 bits the encoder writes a raw float without consulting the
		// range, so it is not a wire contract here either -- only finiteness is.
		if (!std::isfinite(val)
			|| (floatBits < 32 && (val < floatMin || val > floatMax)))
		{
			out.flRejectVal = val;
			out.reject = ScriptRemoteScalarReject_e::SCALAR_FLOAT_INVALID;
			return false;
		}
		out.flFloat = val;
		out.reject = ScriptRemoteScalarReject_e::SCALAR_OK;
		return true;
	}
	case 3: // VECTOR
	{
		float c[3];
		if (floatBits >= 32)
		{
			for (int i = 0; i < 3; i++)
				c[i] = in.ReadFloat();
		}
		else
		{
			const unsigned int maxVal = (1u << floatBits) - 1;
			if (maxVal == 0)
			{
				out.reject = ScriptRemoteScalarReject_e::SCALAR_VECTOR_EMPTY;
				return false;
			}
			const float range = floatMax - floatMin;
			for (int i = 0; i < 3; i++)
			{
				const unsigned int encoded = in.ReadUBits(floatBits);
				c[i] = floatMin + (static_cast<float>(encoded) / static_cast<float>(maxVal)) * range;
			}
		}
		for (int i = 0; i < 3; i++)
		{
			if (!std::isfinite(c[i])
				|| (floatBits < 32 && (c[i] < floatMin || c[i] > floatMax)))
			{
				out.nRejectAux = i;
				out.flRejectVal = c[i];
				out.reject = ScriptRemoteScalarReject_e::SCALAR_VECTOR_INVALID;
				return false;
			}
		}
		out.vec[0] = c[0];
		out.vec[1] = c[1];
		out.vec[2] = c[2];
		out.reject = ScriptRemoteScalarReject_e::SCALAR_OK;
		return true;
	}
	case 42: // ITEMFLAVOR: plain int, never an entity handle.
		out.nInt = in.ReadLong();
		out.reject = ScriptRemoteScalarReject_e::SCALAR_OK;
		return true;
	default:
		out.reject = ScriptRemoteScalarReject_e::SCALAR_UNKNOWN_TYPE;
		return false;
	}
}

#endif // SCRIPTREMOTE_SCALARCODEC_H
