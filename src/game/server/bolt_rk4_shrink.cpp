//=============================================================================//
//
// Purpose: Bolt re-simulation surface-retry step shrink. When an RK4 step of
// the server bolt re-simulation ends inside a surface, the engine retries the
// same step up to four times with the same dt. The S21 client shrinks the
// retried step by 0.7 each time, so a wall-scraping bolt that the client
// threads through is stopped by the server. Feed the retry the shrunk step
// while the outer clock keeps the full one, as the client does.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "public/tier0/memory_patch.h"
#include "bolt_rk4_shrink.h"

static ConVar bridge_bolt_rk4_shrink("bridge_bolt_rk4_shrink", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Shrink the bolt re-simulation step by 0.7 on each surface retry, matching the S21 "
	"client. Read once at boot; 0 leaves the engine loop untouched.");

// 'movaps xmm3, xmm10' (the step handed to the RK4 call) + 'movaps xmm2, xmm6'.
static const uint8_t s_siteStock[7] = { 0x41, 0x0F, 0x28, 0xDA, 0x0F, 0x28, 0xD6 };
static constexpr size_t kCaveBytes = 96;

static uint8_t* s_pCave = nullptr;

static bool Rk4Shrink_BuildCave(uint8_t* const site)
{
	s_pCave = Mem_AllocNearModule(g_GameDll, kCaveBytes);

	if (!s_pCave)
		return false;

	DWORD old = 0;
	VirtualProtect(s_pCave, kCaveBytes, PAGE_EXECUTE_READWRITE, &old);
	memset(s_pCave, 0xCC, kCaveBytes);

	// Data lives at the head of the cave: retry step, then the 0.7 factor.
	float* const pShrunk = reinterpret_cast<float*>(s_pCave);
	float* const pFactor = reinterpret_cast<float*>(s_pCave + 4);
	*pShrunk = 0.0f;
	*pFactor = 0.7f;

	uint8_t* p = s_pCave + 16;
	auto emitRel32 = [&p](const uint8_t* const dest)
	{
		const int32_t rel = static_cast<int32_t>(dest - (p + 4));
		memcpy(p, &rel, 4);
		p += 4;
	};
	auto emitRipF3 = [&p, &emitRel32](const uint8_t op, const uint8_t modrm, const uint8_t* const data)
	{
		*p++ = 0xF3; *p++ = 0x0F; *p++ = op; *p++ = modrm;
		emitRel32(data);
	};
	const uint8_t* const shrunk = reinterpret_cast<const uint8_t*>(pShrunk);
	const uint8_t* const factor = reinterpret_cast<const uint8_t*>(pFactor);

	// test edi, edi ; jnz retry            (edi = retry index, 0 on the first pass)
	*p++ = 0x85; *p++ = 0xFF;
	*p++ = 0x75;
	uint8_t* const jnzRetry = p++;

	// first pass: shrunk = xmm10 ; xmm3 = xmm10 ; jmp done
	*p++ = 0xF3; *p++ = 0x44; *p++ = 0x0F; *p++ = 0x11; *p++ = 0x15;
	emitRel32(shrunk);
	*p++ = 0x41; *p++ = 0x0F; *p++ = 0x28; *p++ = 0xDA;
	*p++ = 0xEB;
	uint8_t* const jmpDone = p++;

	// retry: xmm3 = shrunk * 0.7 ; shrunk = xmm3
	*jnzRetry = static_cast<uint8_t>(p - (jnzRetry + 1));
	emitRipF3(0x10, 0x1D, shrunk);
	emitRipF3(0x59, 0x1D, factor);
	emitRipF3(0x11, 0x1D, shrunk);

	// done: movaps xmm2, xmm6 ; jmp back past the patched pair
	*jmpDone = static_cast<uint8_t>(p - (jmpDone + 1));
	*p++ = 0x0F; *p++ = 0x28; *p++ = 0xD6;
	*p++ = 0xE9;
	emitRel32(site + sizeof(s_siteStock));

	return static_cast<size_t>(p - s_pCave) <= kCaveBytes;
}

///////////////////////////////////////////////////////////////////////////////
void VBoltRk4Shrink::GetVar(void) const
{
	// Inside the server bolt re-simulation step loop:
	// the sv_gravity load, the RK4 argument setup and the call frame stores.
	g_pBoltResimRetrySite = Module_FindPattern(g_GameDll,
		"48 8B 05 ?? ?? ?? ?? 48 8D 4D D0 41 0F 28 DA 0F 28 D6 F3 0F 59 40 68 "
		"48 8D 44 24 70 48 89 44 24 28 48 8D 45 C0")
		.Offset(11).RCast<uint8_t*>();

	if (!g_pBoltResimRetrySite)
		Warning(eDLL_T::SERVER,
			"[RK4-SHRINK] bolt re-sim retry site unresolved -- shrink disabled\n");
}

void VBoltRk4Shrink::Detour(const bool bAttach) const
{
	if (!bAttach || !g_pBoltResimRetrySite || !bridge_bolt_rk4_shrink.GetBool())
		return;

	if (memcmp(g_pBoltResimRetrySite, s_siteStock, sizeof(s_siteStock)) != 0)
	{
		Warning(eDLL_T::SERVER,
			"[RK4-SHRINK] retry site bytes differ from stock -- shrink disabled\n");
		return;
	}

	if (!Rk4Shrink_BuildCave(g_pBoltResimRetrySite))
	{
		Warning(eDLL_T::SERVER, "[RK4-SHRINK] cave alloc failed -- shrink disabled\n");
		return;
	}

	uint8_t jmp[sizeof(s_siteStock)] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
	const int32_t rel = static_cast<int32_t>((s_pCave + 16) - (g_pBoltResimRetrySite + 5));
	memcpy(jmp + 1, &rel, 4);

	if (!Mem_PatchCode(g_pBoltResimRetrySite, jmp, sizeof(jmp)))
	{
		Warning(eDLL_T::SERVER, "[RK4-SHRINK] site patch failed -- shrink disabled\n");
		return;
	}

	Msg(eDLL_T::SERVER, "[RK4-SHRINK] bolt re-sim retry step shrink installed\n");
}
///////////////////////////////////////////////////////////////////////////////
