//=============================================================================//
//
// Purpose: Native PdefParse logs Hit PDATA_MAX_ITEMS_DEFS then still stores.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/memory_patch.h"
#include "thirdparty/detours/include/detours.h"
#include "pdef_parse.h"

#include <cstring>
#include <cstdint>
#include <climits>

static bool s_bFlushPatched = false;

static constexpr uint64_t kPdefItemCountOff = 0x30000;
static constexpr uint64_t kPdefItemMax = 0x1000;
static constexpr uint64_t kPdefEnumCountOff = 0x3AE28;
static constexpr uint64_t kPdefEnumNamePoolOff = 0x39008;
static constexpr uint64_t kPdefEnumCountMax = 192;
static constexpr uint64_t kPdefEnumNamePoolMax = 4608;

// Refuse here, not in the item allocator: the struct's caller fails closed on null,
// while a null item block would be stored and dereferenced later.
static __int64 __fastcall Hook_PdefStartNewStruct(char* name, __int64 pdef, __int64 nAdd)
{
	if (!pdef)
		return 0;

	const uint64_t cur = *reinterpret_cast<const uint64_t*>(pdef + kPdefItemCountOff);
	if (nAdd < 0 || static_cast<uint64_t>(nAdd) > kPdefItemMax
		|| cur > kPdefItemMax || cur + static_cast<uint64_t>(nAdd) > kPdefItemMax)
	{
		static bool s_itemRefuse = false;
		if (!s_itemRefuse)
		{
			s_itemRefuse = true;
			Warning(eDLL_T::CLIENT, "[PDEF] struct item refuse count=%llu nAdd=%lld\n", cur, nAdd);
		}
		return 0;
	}

	return v_PdefStartNewStruct(name, pdef, nAdd);
}

static __int64 __fastcall Hook_PdefStartNewEnum(__int64 pdef, char* name, __int64 nAdd)
{
	if (!pdef || nAdd <= 0 || static_cast<uint64_t>(nAdd) > kPdefEnumNamePoolMax)
		return 0;

	const uint64_t enumCount = *reinterpret_cast<const uint64_t*>(pdef + kPdefEnumCountOff);
	const uint64_t nameCursor = *reinterpret_cast<const uint64_t*>(pdef + kPdefEnumNamePoolOff);
	if (enumCount >= kPdefEnumCountMax
		|| nameCursor >= kPdefEnumNamePoolMax
		|| nameCursor + static_cast<uint64_t>(nAdd) > kPdefEnumNamePoolMax)
	{
		static bool s_enumRefuse = false;
		if (!s_enumRefuse)
		{
			s_enumRefuse = true;
			Warning(eDLL_T::CLIENT,
				"[PDEF] enum name pool refuse count=%llu cursor=%llu nAdd=%lld\n",
				enumCount, nameCursor, nAdd);
		}
		return 0;
	}

	return v_PdefStartNewEnum(pdef, name, nAdd);
}

void VPdefParseBound::Detour(const bool bAttach) const
{
	if (v_PdefStartNewStruct)
		DetourSetup(&v_PdefStartNewStruct, &Hook_PdefStartNewStruct, bAttach);
	if (v_PdefStartNewEnum)
		DetourSetup(&v_PdefStartNewEnum, &Hook_PdefStartNewEnum, bAttach);

	if (!bAttach || !v_PdefParse || s_bFlushPatched)
		return;

	uint8_t* const parse = static_cast<uint8_t*>(v_PdefParse);
	const uint8_t kCmp[] = { 0x48, 0x3D, 0x00, 0x10, 0x00, 0x00, 0x76, 0x18 };
	uint8_t* cmp = nullptr;
	for (size_t i = 0; i + sizeof(kCmp) <= 0x1000; ++i)
	{
		if (memcmp(parse + i, kCmp, sizeof(kCmp)) == 0)
		{
			cmp = parse + i;
			break;
		}
	}
	if (!cmp)
	{
		Warning(eDLL_T::CLIENT, "[PDEF] flush overflow cmp missing -- struct hook only\n");
		return;
	}

	uint8_t* const afterLog = cmp + 0x19;
	if (afterLog[0] != 0x48 || afterLog[1] != 0x8B)
	{
		Warning(eDLL_T::CLIENT, "[PDEF] flush fallthrough mismatch -- struct hook only\n");
		return;
	}

	uint8_t* const fail = cmp + 0x81;
	if (fail[0] != 0x32 || fail[1] != 0xDB)
	{
		Warning(eDLL_T::CLIENT, "[PDEF] flush fail xor missing -- struct hook only\n");
		return;
	}

	const int64_t rel64 = static_cast<int64_t>(fail - (afterLog + 5));
	if (rel64 < INT32_MIN || rel64 > INT32_MAX)
	{
		Warning(eDLL_T::CLIENT, "[PDEF] flush fail out of range -- struct hook only\n");
		return;
	}

	uint8_t patch[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
	const int32_t rel = static_cast<int32_t>(rel64);
	memcpy(patch + 1, &rel, 4);
	if (!Mem_PatchCode(afterLog, patch, sizeof(patch)))
	{
		Warning(eDLL_T::CLIENT, "[PDEF] flush patch failed -- struct hook only\n");
		return;
	}

	s_bFlushPatched = true;
	Msg(eDLL_T::CLIENT, "[PDEF] item overflow fail-closed\n");
}
