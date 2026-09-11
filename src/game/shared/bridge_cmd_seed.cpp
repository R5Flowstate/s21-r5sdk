//=============================================================================//
//
// Purpose: cross-build per-usercmd prediction-RNG seed parity (dedi side).
//
//=============================================================================//
#include "core/stdafx.h"
#include "bridge_cmd_seed.h"

//-----------------------------------------------------------------------------
// RFC 1321 MD5, identical to the client copy and the engine MD5_PseudoRandom recipe.
//-----------------------------------------------------------------------------
namespace BridgeSeedMD5
{
	static const uint32_t K[64] = {
		0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
		0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
		0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
		0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
		0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
		0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
		0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
		0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
		0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
		0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
		0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
		0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
		0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
		0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
		0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
		0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
	static const uint8_t S[64] = {
		7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
		5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
		4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
		6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };

	static inline uint32_t Rotl(const uint32_t x, const int c)
	{
		return (x << c) | (x >> (32 - c));
	}

	struct Ctx
	{
		uint32_t a, b, c, d;
	};

	static void Transform(Ctx& st, const uint8_t block[64])
	{
		uint32_t m[16];
		memcpy(m, block, 64);
		uint32_t a = st.a, b = st.b, c = st.c, d = st.d;
		for (int i = 0; i < 64; ++i)
		{
			uint32_t f;
			int g;
			if (i < 16)      { f = (b & c) | (~b & d); g = i; }
			else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
			else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) & 15; }
			else             { f = c ^ (b | ~d);       g = (7 * i) & 15; }
			const uint32_t tmp = d;
			d = c;
			c = b;
			b = b + Rotl(a + f + K[i] + m[g], S[i]);
			a = tmp;
		}
		st.a += a; st.b += b; st.c += c; st.d += d;
	}
}

//-----------------------------------------------------------------------------
// Purpose: MD5_PseudoRandom(commandNumber) & 0x7FFFFFFF. Digest u32 at byte 6.
//-----------------------------------------------------------------------------
uint32_t BridgeSeed_FromCommandNumber(const uint32_t commandNumber)
{
	using namespace BridgeSeedMD5;

	// 4-byte message fits one padded block: data, 0x80 terminator, bit length
	// (32) as LE u64 at +56.
	uint8_t block[64] = {};
	memcpy(block, &commandNumber, 4);
	block[4] = 0x80;
	block[56] = 32;

	Ctx st = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u };
	Transform(st, block);

	// Digest = LE serialization of a/b/c/d; the u32 at byte 6 spans b's top
	// half and c's bottom half (Valve reads *(u32*)&digest[6]).
	uint8_t digest[16];
	memcpy(digest + 0,  &st.a, 4);
	memcpy(digest + 4,  &st.b, 4);
	memcpy(digest + 8,  &st.c, 4);
	memcpy(digest + 12, &st.d, 4);
	uint32_t out;
	memcpy(&out, digest + 6, 4);
	return out & 0x7FFFFFFFu;
}

#ifndef CLIENT_DLL

typedef int (__fastcall* PFN_UserCmdComputeSeed)(uint8_t* cmd);
static PFN_UserCmdComputeSeed v_UserCmd_ComputeRandomSeed_Srv = nullptr; // (server CUserCmd layout copy)
static PFN_UserCmdComputeSeed v_UserCmd_ComputeRandomSeed_Cl  = nullptr; // (client-layout twin linked into the dedi; also seeds bot/replayed cmds via )

static ConVar bridge_cmd_seed_parity("bridge_cmd_seed_parity", "1", FCVAR_RELEASE,
	"Derive each usercmd's prediction-RNG seed from command_number (engine-legacy "
	"MD5_PseudoRandom recipe) instead of the native MD5 over version-specific cmd "
	"fields. Must match the CLIENT bridge_cmd_seed_parity so the S21 client's "
	"predicted weapon RNG (view-kick jitter, pellet scatter) pairs bit-exact with "
	"this dedi's authoritative rolls. 0 = native derivation (cross-build mismatch), "
	"1 = command-number derivation (default).");

static bool s_bSeedSelfTestOk = false;

// CUserCmd::command_number at +0x00, random_seed at +0x184.
static constexpr ptrdiff_t S3CMD_OFF_COMMANDNUMBER = 0x00;
static constexpr ptrdiff_t S3CMD_OFF_RANDOMSEED    = 0x184;

static int __fastcall BridgeCmdSeed_Compute(uint8_t* const cmd, const PFN_UserCmdComputeSeed pOriginal)
{
	if (!cmd || !s_bSeedSelfTestOk || !bridge_cmd_seed_parity.GetBool())
		return pOriginal(cmd);

	uint32_t cmdNum;
	memcpy(&cmdNum, cmd + S3CMD_OFF_COMMANDNUMBER, 4);
	const uint32_t seed = BridgeSeed_FromCommandNumber(cmdNum);
	memcpy(cmd + S3CMD_OFF_RANDOMSEED, &seed, 4);

	return static_cast<int>(seed);
}

static int __fastcall Hook_UserCmd_ComputeRandomSeed_Srv(uint8_t* cmd)
{
	return BridgeCmdSeed_Compute(cmd, v_UserCmd_ComputeRandomSeed_Srv);
}

static int __fastcall Hook_UserCmd_ComputeRandomSeed_Cl(uint8_t* cmd)
{
	return BridgeCmdSeed_Compute(cmd, v_UserCmd_ComputeRandomSeed_Cl);
}

void VBridgeCmdSeed::GetAdr(void) const
{
	LogFunAdr("UserCmd_ComputeRandomSeed_Srv", v_UserCmd_ComputeRandomSeed_Srv);
	LogFunAdr("UserCmd_ComputeRandomSeed_Cl", v_UserCmd_ComputeRandomSeed_Cl);
}

void VBridgeCmdSeed::GetFun(void) const
{
	// Known-answer MD5 self-test; fail loud and leave native derivation untouched.
	s_bSeedSelfTestOk =
		BridgeSeed_FromCommandNumber(0u) == 0x2D863277u &&
		BridgeSeed_FromCommandNumber(1u) == 0x770B7539u &&
		BridgeSeed_FromCommandNumber(0x12345678u) == 0x6C8AB800u;
	if (!s_bSeedSelfTestOk)
		Warning(eDLL_T::SERVER, "[CMD-SEED] MD5 self-test FAILED -- parity seed disabled, native derivation kept\n");

	// Twins differ by one displacement: server [rcx+1C8h] vs client [rcx+260h].


	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 55 48 8B EC 48 81 EC 80 00 00 00 48 8B 05 ?? ?? ?? ?? "
		"48 8B D9 83 78 6C 00 74 0C 8B 09 E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? 33 C0 "
		"C7 45 A0 01 23 45 67 48 8D 91 C8 01 00 00 C7 45 A4 89")
		.GetPtr(v_UserCmd_ComputeRandomSeed_Srv);
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 55 48 8B EC 48 81 EC 80 00 00 00 48 8B 05 ?? ?? ?? ?? "
		"48 8B D9 83 78 6C 00 74 0C 8B 09 E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? 33 C0 "
		"C7 45 A0 01 23 45 67 48 8D 91 60 02 00 00 C7 45 A4 89")
		.GetPtr(v_UserCmd_ComputeRandomSeed_Cl);

	if (!v_UserCmd_ComputeRandomSeed_Srv || !v_UserCmd_ComputeRandomSeed_Cl)
		Warning(eDLL_T::SERVER, "[CMD-SEED] seed-derivation pattern unresolved (srv=%p cl=%p) -- parity seed inactive\n",
			reinterpret_cast<void*>(v_UserCmd_ComputeRandomSeed_Srv),
			reinterpret_cast<void*>(v_UserCmd_ComputeRandomSeed_Cl));
	else if (!s_bSeedSelfTestOk)
		Warning(eDLL_T::SERVER, "[CMD-SEED] resolved: srv @ %p, cl-twin @ %p, selftest=FAILED\n",
			reinterpret_cast<void*>(v_UserCmd_ComputeRandomSeed_Srv),
			reinterpret_cast<void*>(v_UserCmd_ComputeRandomSeed_Cl));
}

void VBridgeCmdSeed::GetVar(void) const { }

void VBridgeCmdSeed::GetCon(void) const { }

void VBridgeCmdSeed::Detour(const bool bAttach) const
{
	if (v_UserCmd_ComputeRandomSeed_Srv)
		DetourSetup(&v_UserCmd_ComputeRandomSeed_Srv, &Hook_UserCmd_ComputeRandomSeed_Srv, bAttach);
	if (v_UserCmd_ComputeRandomSeed_Cl)
		DetourSetup(&v_UserCmd_ComputeRandomSeed_Cl, &Hook_UserCmd_ComputeRandomSeed_Cl, bAttach);
}

#else // CLIENT_DLL

// Server-only detour -- not registered on the client role at all (see
// init.cpp's #ifndef CLIENT_DLL REGISTER(VBridgeCmdSeed) site), but the class
// must stay a complete type for the client-role client.dll target.
void VBridgeCmdSeed::GetAdr(void) const { }
void VBridgeCmdSeed::GetFun(void) const { }
void VBridgeCmdSeed::GetVar(void) const { }
void VBridgeCmdSeed::GetCon(void) const { }
void VBridgeCmdSeed::Detour(const bool bAttach) const { }

#endif // !CLIENT_DLL
