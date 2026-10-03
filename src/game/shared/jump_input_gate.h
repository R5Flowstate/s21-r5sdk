//=============================================================================//
//
// Purpose: movement-ability input at the Jump decision both engines share.
// A ground jump answers IN_JUMP; an airborne double jump or dodge answers
// IN_DODGE. The client presses IN_DODGE along with jump while +dodge is unbound
// or on jump's key, so only a split bind changes what a press does.
//
// The site is `test ground8,ground8 / jnz skip / test dodge8,dodge8 / jnz skip`
// (both REX-extended byte registers), falling through into the double
// jump / dodge selection. RAX and the flags are dead there.
//
//=============================================================================//
#ifndef SHARED_JUMP_INPUT_GATE_H
#define SHARED_JUMP_INPUT_GATE_H

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "public/game/shared/in_buttons.h"

enum JumpGateVerdict_t : int
{
	JUMPGATE_REPLAY = -1, // run the displaced tests unchanged
	JUMPGATE_BAIL   = 0,  // Jump returns 0
	JUMPGATE_AIR    = 1,  // clear the dodge flag and pick double jump or dodge
};

struct JumpGateLayout_t
{
	ptrdiff_t plrClassSettings;
	ptrdiff_t plrTimeLastJumped;
	ptrdiff_t mvButtonsPressed;
};

static constexpr ptrdiff_t JUMPGATE_CTX_OFF_PLAYER   = 0x8;
static constexpr ptrdiff_t JUMPGATE_CTX_OFF_MOVEDATA = 0x10;
static constexpr uint32_t  JUMPGATE_SETTINGS_OFF_CAP = 0x100000u;

// super_jump_debounce_time: no double jump this soon after the previous jump.
static constexpr float JUMPGATE_SUPERJUMP_DEBOUNCE = 0.1f;

// pDoubleJumpField is the engine's settings-field global; it is filled when the
// settings layout loads, so read it per call.
inline int JumpGate_Decide(const uintptr_t ctx, const bool bGround, const JumpGateLayout_t& L,
	const uint32_t* const pDoubleJumpField, const float flNow)
{
	const uintptr_t player = *reinterpret_cast<const uintptr_t*>(ctx + JUMPGATE_CTX_OFF_PLAYER);
	const uintptr_t mv = *reinterpret_cast<const uintptr_t*>(ctx + JUMPGATE_CTX_OFF_MOVEDATA);
	if (!player || !mv)
		return JUMPGATE_REPLAY;

	const uint32_t nPressed = *reinterpret_cast<const uint32_t*>(mv + L.mvButtonsPressed);
	if (!(nPressed & (IN_JUMP | IN_DODGE)))
		return JUMPGATE_REPLAY;
	if (bGround)
		return (nPressed & IN_JUMP) ? JUMPGATE_REPLAY : JUMPGATE_BAIL;
	if (!(nPressed & IN_DODGE))
		return JUMPGATE_BAIL;

	const uint8_t* const pClass = *reinterpret_cast<const uint8_t* const*>(player + L.plrClassSettings);
	const uint32_t nField = pDoubleJumpField ? *pDoubleJumpField : JUMPGATE_SETTINGS_OFF_CAP;
	if (pClass && nField < JUMPGATE_SETTINGS_OFF_CAP && pClass[nField]
		&& flNow - *reinterpret_cast<const float*>(player + L.plrTimeLastJumped) < JUMPGATE_SUPERJUMP_DEBOUNCE)
		return JUMPGATE_BAIL;

	return JUMPGATE_AIR;
}

typedef int (__fastcall* JumpGateDecideFn_t)(uintptr_t ctx, int bGround);

class CJumpInputGatePatch
{
public:
	CJumpInputGatePatch(void) : m_pSite(nullptr), m_pTramp(nullptr) { memset(m_origBytes, 0, sizeof(m_origBytes)); }

	// nCtxReg: low 3 bits of the non-extended register holding the movement context (3 = rbx, 7 = rdi).
	bool Install(uint8_t* const pSite, const uint8_t* const pBail, const uint8_t nCtxReg, const JumpGateDecideFn_t pfnDecide)
	{
		if (m_pSite)
			return true;
		if (!pSite || !pBail || !pfnDecide || nCtxReg > 7 || !IsSite(pSite))
			return false;

		const uint8_t* const pSkip = pSite + 5 + static_cast<int8_t>(pSite[4]);
		const uint8_t* const pSelect = pSite + SITE_LEN;

		if (!m_pTramp)
			m_pTramp = AllocNear(pSite, 256);
		if (!m_pTramp)
			return false;

		uint8_t* p = m_pTramp;
		p = Emit(p, "\x51\x52\x41\x50\x41\x51\x41\x52\x41\x53", 10);    // push rcx, rdx, r8-r11
		p = Emit(p, "\x48\x81\xEC\x80\x00\x00\x00", 7);                // sub rsp, 80h
		for (uint8_t i = 0; i < 6; ++i)                                 // movdqu [rsp+20h+i*10h], xmm<i>
			p = EmitXmm(p, 0x7F, i);
		const uint8_t movCtx[3] = { 0x48, 0x8B, static_cast<uint8_t>(0xC8 | nCtxReg) };
		p = Emit(p, movCtx, 3);                                          // mov rcx, ctx
		const uint8_t movGround[4] = { 0x41, 0x0F, 0xB6, static_cast<uint8_t>(0xD0 | (pSite[2] & 7)) };
		p = Emit(p, movGround, 4);                                       // movzx edx, ground8
		p = Emit(p, "\x48\xB8", 2);                                      // mov rax, imm64
		const uint64_t fn = reinterpret_cast<uint64_t>(pfnDecide);
		p = Emit(p, &fn, 8);
		p = Emit(p, "\xFF\xD0", 2);                                      // call rax
		for (uint8_t i = 0; i < 6; ++i)                                 // movdqu xmm<i>, [rsp+20h+i*10h]
			p = EmitXmm(p, 0x6F, i);
		p = Emit(p, "\x48\x81\xC4\x80\x00\x00\x00", 7);                // add rsp, 80h
		p = Emit(p, "\x41\x5B\x41\x5A\x41\x59\x41\x58\x5A\x59", 10);    // pop r11-r8, rdx, rcx

		p = Emit(p, "\x83\xF8\x01", 3);                                  // cmp eax, JUMPGATE_AIR
		p = Emit(p, "\x0F\x84", 2);                                      // je air
		uint8_t* const pAirRel = p;
		p += 4;
		p = Emit(p, "\x85\xC0", 2);                                      // test eax, eax
		p = Emit(p, "\x0F\x84", 2);                                      // je bail
		p = EmitRel32(p, pBail);

		p = Emit(p, pSite, 3);                                           // replay: test ground8
		p = Emit(p, "\x0F\x85", 2);
		p = EmitRel32(p, pSkip);
		p = Emit(p, pSite + 5, 3);                                       // test dodge8
		p = Emit(p, "\x0F\x85", 2);
		p = EmitRel32(p, pSkip);
		p = EmitJmp(p, pSelect);

		EmitRel32(pAirRel, p);
		const uint8_t xorDodge[3] = { 0x45, 0x32, pSite[7] };
		p = Emit(p, xorDodge, 3);                                        // air: xor dodge8, dodge8
		p = EmitJmp(p, pSelect);

		FlushInstructionCache(GetCurrentProcess(), m_pTramp, static_cast<size_t>(p - m_pTramp));

		uint8_t patch[SITE_LEN];
		memset(patch, 0x90, sizeof(patch));
		patch[0] = 0xE9;
		const int32_t rel = static_cast<int32_t>(m_pTramp - (pSite + 5));
		memcpy(patch + 1, &rel, 4);

		memcpy(m_origBytes, pSite, SITE_LEN);
		if (!WriteCode(pSite, patch, SITE_LEN))
			return false;

		m_pSite = pSite;
		return true;
	}

	void Remove(void)
	{
		if (!m_pSite)
			return;
		WriteCode(m_pSite, m_origBytes, SITE_LEN);
		m_pSite = nullptr;
	}

	bool IsInstalled(void) const { return m_pSite != nullptr; }

	static bool IsSite(const uint8_t* const s)
	{
		return s[0] == 0x45 && s[1] == 0x84 && (s[2] & 0xC0) == 0xC0 && s[3] == 0x75
			&& s[5] == 0x45 && s[6] == 0x84 && (s[7] & 0xC0) == 0xC0 && s[8] == 0x75
			&& s + 5 + static_cast<int8_t>(s[4]) == s + SITE_LEN + static_cast<int8_t>(s[9]);
	}

private:
	static constexpr int SITE_LEN = 10;

	static uint8_t* Emit(uint8_t* p, const void* src, size_t len)
	{
		memcpy(p, src, len);
		return p + len;
	}
	// movdqu with op 7F (store) or 6F (load), [rsp+20h+i*10h].
	static uint8_t* EmitXmm(uint8_t* p, const uint8_t op, const uint8_t i)
	{
		const uint8_t insn[6] = { 0xF3, 0x0F, op, static_cast<uint8_t>(0x44 | (i << 3)), 0x24,
			static_cast<uint8_t>(0x20 + i * 0x10) };
		return Emit(p, insn, sizeof(insn));
	}
	static uint8_t* EmitRel32(uint8_t* p, const uint8_t* target)
	{
		const int32_t rel = static_cast<int32_t>(target - (p + 4));
		memcpy(p, &rel, 4);
		return p + 4;
	}
	static uint8_t* EmitJmp(uint8_t* p, const uint8_t* target)
	{
		*p++ = 0xE9;
		return EmitRel32(p, target);
	}

	// A rel32 jump reaches 2 GB either way; walk 64 KB granules outward from the site.
	static uint8_t* AllocNear(const uint8_t* const pSite, const size_t len)
	{
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
		const uintptr_t base = reinterpret_cast<uintptr_t>(pSite) & ~(gran - 1);

		for (uintptr_t step = gran; step < 0x7FF00000ull; step += gran)
		{
			for (int dir = 0; dir < 2; ++dir)
			{
				const uintptr_t addr = dir ? base - step : base + step;
				if (addr < reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress) ||
					addr > reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress))
					continue;
				void* const mem = VirtualAlloc(reinterpret_cast<void*>(addr), len,
					MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
				if (mem)
					return static_cast<uint8_t*>(mem);
			}
		}
		return nullptr;
	}

	static bool WriteCode(uint8_t* const site, const uint8_t* const bytes, const int len)
	{
		DWORD oldProt = 0;
		if (!VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &oldProt))
			return false;
		memcpy(site, bytes, len);
		VirtualProtect(site, len, oldProt, &oldProt);
		FlushInstructionCache(GetCurrentProcess(), site, len);
		return true;
	}

	uint8_t* m_pSite;
	uint8_t* m_pTramp;
	uint8_t  m_origBytes[SITE_LEN];
};

#endif // SHARED_JUMP_INPUT_GATE_H
