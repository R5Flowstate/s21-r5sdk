//=============================================================================//
//
// Purpose: CL_ParsePacketEntities from-cursor is 116384 when the from-frame
//          is exhausted. Native still indexes the 16384-slot table with it.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/memory_patch.h"
#include "cl_parse_ents.h"

#include <cstring>
#include <cstdint>
#include <climits>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static constexpr uint32_t kMaxEdicts = 0x4000;
static constexpr size_t kParseScan = 0xB00;

static uint8_t* s_pCave = nullptr;
static bool s_bPatched = false;

static bool Rel32Fits(const uint8_t* const from, const uint8_t* const to)
{
	const int64_t rel = static_cast<int64_t>(to - (from + 5));
	return rel >= INT32_MIN && rel <= INT32_MAX;
}

static bool WriteRel32Jmp(uint8_t* const site, const size_t steal, uint8_t* const dest)
{
	if (steal < 5 || !Rel32Fits(site, dest))
		return false;

	uint8_t buf[16] = {};
	buf[0] = 0xE9;
	const int32_t rel = static_cast<int32_t>(dest - (site + 5));
	memcpy(buf + 1, &rel, 4);
	for (size_t i = 5; i < steal; ++i)
		buf[i] = 0x90;
	return Mem_PatchCode(site, buf, steal);
}

static void EmitRel32Jmp(uint8_t*& p, uint8_t* const dest)
{
	*p++ = 0xE9;
	const int32_t rel = static_cast<int32_t>(dest - (p + 4));
	memcpy(p, &rel, 4);
	p += 4;
}

static uint8_t* FindSeq(uint8_t* const start, const size_t len,
	const uint8_t* const needle, const size_t nlen, const int nth)
{
	int seen = 0;
	for (size_t i = 0; i + nlen <= len; ++i)
	{
		if (memcmp(start + i, needle, nlen) == 0)
		{
			if (seen++ == nth)
				return start + i;
		}
	}
	return nullptr;
}

void VCLParsePacketEntitiesBound::Detour(const bool bAttach) const
{
	if (!bAttach || !v_CL_ParsePacketEntities || s_bPatched)
		return;

	uint8_t* const fn = static_cast<uint8_t*>(v_CL_ParsePacketEntities);
	const uint8_t kWrite[] = { 0x45, 0x89, 0xA4, 0x82, 0xD0, 0x79, 0xC5, 0x02 };
	uint8_t* writes[4] = {};
	for (int i = 0; i < 4; ++i)
	{
		writes[i] = FindSeq(fn, kParseScan, kWrite, sizeof(kWrite), i);
		if (!writes[i])
		{
			Warning(eDLL_T::CLIENT, "[PARSE-ENTS] table write %d missing -- not patched\n", i);
			return;
		}
	}

	const uint8_t kPreserve[] = { 0x48, 0x8B, 0x03, 0x48, 0x85, 0xC0 };
	uint8_t* const preserve = FindSeq(fn, kParseScan, kPreserve, sizeof(kPreserve), 0);
	const uint8_t kLeaveStore[] = { 0x48, 0x8B, 0x43, 0x08, 0x89, 0x78, 0x04 };
	uint8_t* const leaveStore = FindSeq(fn, kParseScan, kLeaveStore, sizeof(kLeaveStore), 0);
	if (!preserve || !leaveStore)
	{
		Warning(eDLL_T::CLIENT, "[PARSE-ENTS] preserve/leave site missing -- not patched\n");
		return;
	}
	if (preserve[6] != 0x0F || preserve[7] != 0x84)
	{
		Warning(eDLL_T::CLIENT, "[PARSE-ENTS] preserve jz missing -- not patched\n");
		return;
	}

	int32_t jzRel = 0;
	memcpy(&jzRel, preserve + 8, 4);
	uint8_t* const exitSite = preserve + 12 + jzRel;

	s_pCave = Mem_AllocNearModule(g_GameDll, 0x200);
	if (!s_pCave)
	{
		Warning(eDLL_T::CLIENT, "[PARSE-ENTS] exec cave alloc failed -- not patched\n");
		return;
	}

	DWORD oldCave = 0;
	VirtualProtect(s_pCave, 0x200, PAGE_EXECUTE_READWRITE, &oldCave);

	uint8_t* p = s_pCave;
	uint8_t* writeEntry[4] = {};
	for (int i = 0; i < 4; ++i)
	{
		writeEntry[i] = p;
		*p++ = 0x3D;
		*p++ = 0x00;
		*p++ = 0x40;
		*p++ = 0x00;
		*p++ = 0x00;
		*p++ = 0x73;
		*p++ = 0x08;
		memcpy(p, kWrite, sizeof(kWrite));
		p += sizeof(kWrite);
		if (!Rel32Fits(p, writes[i] + 8))
		{
			Warning(eDLL_T::CLIENT, "[PARSE-ENTS] write stub %d out of range -- not patched\n", i);
			VirtualFree(s_pCave, 0, MEM_RELEASE);
			s_pCave = nullptr;
			return;
		}
		EmitRel32Jmp(p, writes[i] + 8);
	}

	uint8_t* const preserveEntry = p;
	*p++ = 0x81;
	*p++ = 0xFF;
	*p++ = 0x00;
	*p++ = 0x40;
	*p++ = 0x00;
	*p++ = 0x00;
	*p++ = 0x73;
	uint8_t* const jaeSlot = p++;
	memcpy(p, kPreserve, sizeof(kPreserve));
	p += sizeof(kPreserve);
	if (!Rel32Fits(p, preserve + 6))
	{
		Warning(eDLL_T::CLIENT, "[PARSE-ENTS] preserve stub out of range -- not patched\n");
		VirtualFree(s_pCave, 0, MEM_RELEASE);
		s_pCave = nullptr;
		return;
	}
	EmitRel32Jmp(p, preserve + 6);
	uint8_t* const doExit = p;
	*jaeSlot = static_cast<uint8_t>(doExit - (jaeSlot + 1));
	if (!Rel32Fits(p, exitSite))
	{
		Warning(eDLL_T::CLIENT, "[PARSE-ENTS] preserve exit out of range -- not patched\n");
		VirtualFree(s_pCave, 0, MEM_RELEASE);
		s_pCave = nullptr;
		return;
	}
	EmitRel32Jmp(p, exitSite);

	uint8_t* const leaveEntry = p;
	memcpy(p, kLeaveStore, 4);
	p += 4;
	*p++ = 0x81;
	*p++ = 0xFF;
	*p++ = 0x00;
	*p++ = 0x40;
	*p++ = 0x00;
	*p++ = 0x00;
	*p++ = 0x73;
	*p++ = 0x03;
	memcpy(p, kLeaveStore + 4, 3);
	p += 3;
	if (!Rel32Fits(p, leaveStore + 7))
	{
		Warning(eDLL_T::CLIENT, "[PARSE-ENTS] leave stub out of range -- not patched\n");
		VirtualFree(s_pCave, 0, MEM_RELEASE);
		s_pCave = nullptr;
		return;
	}
	EmitRel32Jmp(p, leaveStore + 7);

	for (int i = 0; i < 4; ++i)
	{
		if (!WriteRel32Jmp(writes[i], 8, writeEntry[i]))
		{
			Warning(eDLL_T::CLIENT, "[PARSE-ENTS] table write %d patch failed\n", i);
			return;
		}
	}
	if (!WriteRel32Jmp(preserve, 6, preserveEntry) ||
		!WriteRel32Jmp(leaveStore, 7, leaveEntry))
	{
		Warning(eDLL_T::CLIENT, "[PARSE-ENTS] preserve/leave patch failed\n");
		return;
	}

	FlushInstructionCache(GetCurrentProcess(), s_pCave, 0x200);
	s_bPatched = true;
	Msg(eDLL_T::CLIENT, "[PARSE-ENTS] from-cursor table writes bound to %u\n", kMaxEdicts);
}
