//=============================================================================//
//
// Purpose: Challenge-bind the connect password tag.
// ConnectClientParams: challenge +0x1C, incoming pw tag ptr +0x38.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier1/convar.h"
#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/detours.h"
#include "connect_password_gate.h"
#include "engine/shared/connect_password_tag.h"

#include <cstdint>

static ConVar sv_connect_challenge_bind("sv_connect_challenge_bind", "1",
	FCVAR_RELEASE,
	"Require the challenge-bound connect password tag from bridge clients. "
	"Deploy client.dll and server.dll in lockstep when toggling.");

static constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
static constexpr uint64_t kFnvPrime = 1099511628211ULL;

static uint64_t PasswordFnv(const char* const pszPassword)
{
	uint64_t h = kFnvOffsetBasis;
	for (const unsigned char* p = reinterpret_cast<const unsigned char*>(pszPassword); *p; ++p)
	{
		h ^= *p;
		h *= kFnvPrime;
	}
	return h;
}

void ConnectPasswordGate_FilterTag(void* pChallenge)
{
	ConVar* const pPassword = g_pCVar ? g_pCVar->FindVar("sv_password") : nullptr;
	const char* const pszPassword = pPassword ? pPassword->GetString() : "";

	if (!sv_connect_challenge_bind.GetBool() || !pszPassword[0])
		return;

	const int64_t a2 = reinterpret_cast<int64_t>(pChallenge);
	const uint32_t nChallenge = *reinterpret_cast<const uint32_t*>(a2 + 0x1C);
	const char* const pszIncTag = *reinterpret_cast<char* const*>(a2 + 0x38);

	unsigned char key[32];
	uint64_t wire = 0;
	if (!ConnectPw_DeriveKey(pszPassword, key) || !ConnectPw_WireU64(key, nChallenge, &wire))
		return;

	char szExpected[24];
	V_snprintf(szExpected, sizeof(szExpected), "pw:%016llx", wire);
	memset(key, 0, sizeof(key));

	// "pw:" + 16 hex digits; the buffer is wider than the tag it holds.
	const size_t nTagLen = strlen(szExpected);

	const bool bOk = pszIncTag
		&& strnlen(pszIncTag, nTagLen + 1) == nTagLen
		&& memcmp(pszIncTag, szExpected, nTagLen) == 0;

	if (!bOk)
	{
		static int s_nRejects = 0;
		if (++s_nRejects <= 16)
			Warning(eDLL_T::ENGINE, "[BRIDGE-PW] challenge-bound tag rejected (challenge=0x%08X)\n", nChallenge);
	}

	// On success swap to the static tag so serverFilter matches; empty never matches.
	// Thread-local: outlives this call so the engine still sees it at ConnectClient.
	static thread_local char t_szStaticTag[24];
	V_snprintf(t_szStaticTag, sizeof(t_szStaticTag), "pw:%016llx", PasswordFnv(pszPassword));

	*reinterpret_cast<const char**>(a2 + 0x38) = bOk ? t_szStaticTag : "";
}

//-----------------------------------------------------------------------------
void VConnectPasswordGate::GetFun(void) const
{
	// CServer::ConnectClient. cmp [rcx+8],2 / r15=[rdx+0x28] anchors it.
	// Same target as VServer's prefix pattern; resolved here for verification
	// only, never attached (see Detour).
	Module_FindPattern(g_GameDll,
		"40 55 57 41 55 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? "
		"83 79 08 02 4C 8B EA 0F 10 02 48 8B F9 4C 8B 7A 28")
		.GetPtr(v_CServer_ConnectClient);

	if (!v_CServer_ConnectClient)
		Warning(eDLL_T::ENGINE, "[BRIDGE-PW] CServer::ConnectClient pattern unresolved\n");
}

void VConnectPasswordGate::Detour(const bool bAttach) const
{
	(void)bAttach;
	// Attaches nothing: VServer owns the only ConnectClient attach and runs this
	// TU through ConnectPasswordGate_FilterTag. A second attach on the same
	// target orphans one of the two hooks.
}
