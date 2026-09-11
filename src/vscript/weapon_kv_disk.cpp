//=============================================================================//
// Purpose: Load weapon KeyValues from loose disk .txt files.
//=============================================================================//

#include "core/stdafx.h"
#include "weapon_kv_disk.h"
#include "common/global.h"
#include "tier0/dbg.h"
#include "tier1/cvar.h"
#include "filesystem/filesystem.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <climits>
#include <string>
#include <mutex>
#include <unordered_map>

// MemAllocator_s ABI: the object's first qword is Alloc(self, size, align, flag).
typedef void* (__fastcall* RTechAlloc_fn)(void* self, size_t size, size_t align, int flag);
// GetSymbolString(this=symbol-table object, unsigned symbol) -> const char*.
typedef const char* (__fastcall* KVGetSymStr_fn)(void* self, unsigned int symbol);

static ConVar weapon_kv_disk(
	"weapon_kv_disk", "1", FCVAR_RELEASE,
	"When 1, a loose scripts/weapons/<name>.txt under platform/ fully replaces "
	"the packed weapon KV: the .txt is parsed and a KVGroup is built from it, so "
	"you can modify/add/remove any entry. 0 = packed weapon data only. 1 = default.");

// Built disk groups, keyed by weapon name. Bounded (one per unique weapon); the
// group lives for the process (like the packed data). NULL is never cached, so a
// weapon with no disk file falls through to the pak on every parse.
static std::unordered_map<std::string, void*> s_diskGroups;
static std::mutex s_diskGroupsMtx;

// Per-build diagnostics (reset each BuildDiskWeaponGroup call).
static thread_local int s_tls_nUnsignedNegRecovered = 0;

// Engine KeyValues node offsets (not SDK tier1 KeyValues).
static inline void*    KVN_Sub (void* n) { return *reinterpret_cast<void**>(reinterpret_cast<char*>(n) + 0x30); }
static inline void*    KVN_Peer(void* n) { return *reinterpret_cast<void**>(reinterpret_cast<char*>(n) + 0x28); }
static inline uint8_t  KVN_Type(void* n) { return *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(n) + 0x06); }
static inline const char* KVN_Str(void* n) { return *reinterpret_cast<const char**>(reinterpret_cast<char*>(n) + 0x08); }
static inline int      KVN_Int (void* n) { return *reinterpret_cast<int*>(reinterpret_cast<char*>(n) + 0x18); }
static inline float    KVN_Flt (void* n) { return *reinterpret_cast<float*>(reinterpret_cast<char*>(n) + 0x18); }

// Resolve a node's key name via the engine symbol table (name symbol = 24 bits
// at node+3). Returns a persistent symbol-table string (no copy needed).
static const char* KVN_Name(void* n)
{
	if (!g_pKVSymbolTable_S21)
		return nullptr;
	const unsigned int symbol =
		static_cast<unsigned int>(*reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(n) + 0x03))
		| (static_cast<unsigned int>(*reinterpret_cast<uint16_t*>(reinterpret_cast<char*>(n) + 0x04)) << 8);
	void** vtbl = *reinterpret_cast<void***>(g_pKVSymbolTable_S21);
	KVGetSymStr_fn getSym = reinterpret_cast<KVGetSymStr_fn>(vtbl[4]);   // vtable+0x20
	return getSym(g_pKVSymbolTable_S21, symbol);
}

// Exact rtech HashName -- aligned variant for 4-aligned strings, unaligned else
// (same result; matches the pak + parser KVKEY hashes). Returns the u32 keyHash.
static uint32_t KV_Hash(const char* name)
{
	if (!name)
		return 0;
	const bool aligned = (reinterpret_cast<uintptr_t>(name) & 3) == 0;
	return static_cast<uint32_t>(aligned ? v_KVHashAligned_S21(name)
										 : v_KVHashUnaligned_S21(name));
}

// A plain decimal number is [0-9] plus only + -. e E, with at least one digit.
static bool KV_IsPlainNumeric(const char* s)
{
	if (!s || !*s)
		return false;
	bool hasDigit = false;
	for (const char* p = s; *p; ++p)
	{
		const char c = *p;
		if (c >= '0' && c <= '9')
			hasDigit = true;
		else if (c != '+' && c != '-' && c != '.' && c != 'e' && c != 'E')
			return false;
	}
	return hasDigit;
}

// True when the string is an integer form (no decimal point / exponent). Leading
// +/- is allowed. Used to separate int recovery from the float path.
static bool KV_IsIntegerForm(const char* s)
{
	if (!s || !*s)
		return false;
	if (*s == '+' || *s == '-')
		++s;
	if (!*s)
		return false;
	for (const char* p = s; *p; ++p)
	{
		if (*p < '0' || *p > '9')
			return false;
	}
	return true;
}

// Dump-as-unsigned negatives: recover via signed reinterpret when the bit pattern fits int32.
static bool KV_TryParseInt32(const char* s, int* out)
{
	if (!s || !out || !KV_IsIntegerForm(s))
		return false;

	char* end = nullptr;
	if (s[0] == '-' || s[0] == '+')
	{
		const long long iv = strtoll(s, &end, 10);
		if (!end || *end != '\0' || iv < INT_MIN || iv > INT_MAX)
			return false;
		*out = static_cast<int>(iv);
		return true;
	}

	// Unsigned digit string. Prefer signed recovery of dump-as-u64 negatives
	// (high bit set, value fits int32 when reinterpreted as int64).
	const unsigned long long uv = strtoull(s, &end, 10);
	if (!end || *end != '\0')
		return false;

	const long long asSigned = static_cast<long long>(uv);
	if (asSigned >= INT_MIN && asSigned <= INT_MAX)
	{
		// Count only the dump-as-unsigned case (high bit set as int64).
		if (asSigned < 0)
			++s_tls_nUnsignedNegRecovered;
		*out = static_cast<int>(asSigned);
		return true;
	}
	return false;
}

// Deep-copy a C string via the rtech globalHeap (same allocator MergeOverrides
// uses). Returns nullptr on failure; caller should fall back to aliasing.
static const char* KV_DupString(void* gh, RTechAlloc_fn alloc, const char* s)
{
	if (!s || !gh || !alloc)
		return s;
	size_t n = 0;
	while (s[n])
		++n;
	++n; // NUL
	void* buf = alloc(gh, n, 1, 0);
	if (!buf)
		return s;
	std::memcpy(buf, s, n);
	return reinterpret_cast<const char*>(buf);
}

// 24B KVValue from a leaf; numeric strings become INT/FLOAT.
static void KV_SetValue(char* v, void* node, uint8_t dt, void* gh, RTechAlloc_fn alloc)
{
	// KVValue: dataType@0xC (0=INT,1=FLOAT,2=STRING), value@0x10 (8B union).
	if (dt == 1)   // KeyValues STRING
	{
		const char* s = KVN_Str(node);
		if (KV_IsPlainNumeric(s))
		{
			int iv = 0;
			if (KV_TryParseInt32(s, &iv))
			{
				*reinterpret_cast<long long*>(v + 0x10) = static_cast<long long>(iv);
				*reinterpret_cast<uint32_t*>(v + 0x0C) = 0;   // INT
				return;
			}
			char* end = nullptr;
			const double fv = strtod(s, &end);
			if (end && *end == '\0')
			{
				*reinterpret_cast<uint32_t*>(v + 0x14) = 0;
				*reinterpret_cast<float*>(v + 0x10) = static_cast<float>(fv);
				*reinterpret_cast<uint32_t*>(v + 0x0C) = 1;   // FLOAT
				return;
			}
		}
		// Vectors ("0 13 -0.15"), paths, enum names, multipliers ("*0.85"), etc.
		// Deep-copy so the group does not alias the transient KeyValues tree.
		*reinterpret_cast<const char**>(v + 0x10) = KV_DupString(gh, alloc, s);
		*reinterpret_cast<uint32_t*>(v + 0x0C) = 2;            // STRING
	}
	else if (dt == 2 || dt == 4)   // KeyValues INT
	{
		*reinterpret_cast<long long*>(v + 0x10) = static_cast<long long>(KVN_Int(node));
		*reinterpret_cast<uint32_t*>(v + 0x0C) = 0;            // INT
	}
	else if (dt == 3)   // KeyValues FLOAT
	{
		*reinterpret_cast<uint32_t*>(v + 0x14) = 0;
		*reinterpret_cast<float*>(v + 0x10) = KVN_Flt(node);
		*reinterpret_cast<uint32_t*>(v + 0x0C) = 1;            // FLOAT
	}
	else   // wstring/uint64/other -- absent in weapon KV; safe INT 0.
	{
		*reinterpret_cast<long long*>(v + 0x10) = 0;
		*reinterpret_cast<uint32_t*>(v + 0x0C) = 0;
	}
}

// Recursively build KVGroup `outGroup` from the children of KeyValues `kvNode`.
// outGroup must be a zeroed 48B block; its keyName/keyHash are set by the caller.
static void KV_BuildGroup(void* kvNode, void* outGroup, void* gh,
						  RTechAlloc_fn alloc, int depth, int& nV, int& nG)
{
	char* const g = reinterpret_cast<char*>(outGroup);
	if (depth > 64)
		return;

	// Count value-children and group-children (dataType byte 0 == group).
	uint16_t valueCount = 0, groupCount = 0;
	int guard = 0;
	for (void* c = KVN_Sub(kvNode); c && ++guard < 8192; c = KVN_Peer(c))
	{
		if (KVN_Type(c) == 0) ++groupCount; else ++valueCount;
	}
	nV = valueCount; nG = groupCount;

	char* values = valueCount ? reinterpret_cast<char*>(alloc(gh, 24ull * valueCount, 8, 0)) : nullptr;
	char* groups = groupCount ? reinterpret_cast<char*>(alloc(gh, 48ull * groupCount, 8, 0)) : nullptr;

	*reinterpret_cast<uint16_t*>(g + 0x0C) = valueCount;
	*reinterpret_cast<uint16_t*>(g + 0x0E) = groupCount;
	*reinterpret_cast<void**>(g + 0x10) = values;
	*reinterpret_cast<void**>(g + 0x18) = groups;
	*reinterpret_cast<void**>(g + 0x20) = nullptr;   // valueOrder (null -> linear lookup)
	*reinterpret_cast<void**>(g + 0x28) = nullptr;   // groupOrder

	uint16_t vi = 0, gi = 0;
	guard = 0;
	for (void* c = KVN_Sub(kvNode); c && ++guard < 8192; c = KVN_Peer(c))
	{
		// Symbol-table name strings are process-lifetime; no copy needed.
		const char* name = KVN_Name(c);
		const uint32_t h = KV_Hash(name);
		const uint8_t dt = KVN_Type(c);
		if (dt == 0)   // subgroup
		{
			if (!groups || gi >= groupCount) continue;
			char* sg = groups + static_cast<size_t>(gi++) * 48;
			std::memset(sg, 0, 48);
			*reinterpret_cast<const char**>(sg + 0x00) = name;
			*reinterpret_cast<uint32_t*>(sg + 0x08) = h;
			int sv = 0, sgc = 0;
			KV_BuildGroup(c, sg, gh, alloc, depth + 1, sv, sgc);
		}
		else           // value
		{
			if (!values || vi >= valueCount) continue;
			char* v = values + static_cast<size_t>(vi++) * 24;
			std::memset(v, 0, 24);
			*reinterpret_cast<const char**>(v + 0x00) = name;
			*reinterpret_cast<uint32_t*>(v + 0x08) = h;
			KV_SetValue(v, c, dt, gh, alloc);
		}
	}
}

//-----------------------------------------------------------------------------
// Parse platform/scripts/weapons/<name>.txt; NULL falls back to packed.
//-----------------------------------------------------------------------------
static void* BuildDiskWeaponGroup(const char* weaponName)
{
	void* const globalHeap = g_ppRTechGlobalHeap_S21 ? *g_ppRTechGlobalHeap_S21 : nullptr;
	if (!globalHeap || !g_pKVSymbolTable_S21 || !v_KVHashAligned_S21 || !v_KVHashUnaligned_S21)
		return nullptr;

	// IBaseFileSystem sub-object lives at (*g_pFullFileSystem) + 8.
	if (!g_pFullFileSystem || !*g_pFullFileSystem)
		return nullptr;
	void* const ifs = reinterpret_cast<void*>(
		reinterpret_cast<uintptr_t>(*g_pFullFileSystem) + 8);

	char rel[260];
	std::snprintf(rel, sizeof(rel), "scripts/weapons/%s.txt", weaponName);

	// Zeroed buffer is a valid empty engine KeyValues (no vtable).
	alignas(16) unsigned char rootKV[0x60];
	std::memset(rootKV, 0, sizeof(rootKV));

	if (!v_KV_LoadFromFile_S21(rootKV, ifs, rel, 0, 0, "GAME", nullptr))
		return nullptr;

	RTechAlloc_fn alloc = *reinterpret_cast<RTechAlloc_fn*>(globalHeap);
	void* group = alloc(globalHeap, 48, 8, 0);
	if (!group)
		return nullptr;
	std::memset(group, 0, 48);

	// Root group name = the file's outer block name (cosmetic; the caller reads
	// the group's children, not its own name).
	const char* rootName = KVN_Name(rootKV);
	*reinterpret_cast<const char**>(reinterpret_cast<char*>(group) + 0x00) = rootName;
	*reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(group) + 0x08) = KV_Hash(rootName);

	s_tls_nUnsignedNegRecovered = 0;
	int nV = 0, nG = 0;
	KV_BuildGroup(rootKV, group, globalHeap, alloc, 0, nV, nG);
	(void)nV;
	(void)nG;

	// STRING payloads are heap copies; the KeyValues tree is not freed.
	return group;
}

//-----------------------------------------------------------------------------
static void* __fastcall Hook_ReadKVWeaponFile_S21(const char* weaponName)
{
	if (!weaponName || !v_ReadKVWeaponFile_S21 || !weapon_kv_disk.GetBool()
		|| !v_KV_LoadFromFile_S21 || !g_ppRTechGlobalHeap_S21
		|| !g_pKVSymbolTable_S21 || !v_KVHashAligned_S21 || !v_KVHashUnaligned_S21)
	{
		return v_ReadKVWeaponFile_S21 ? v_ReadKVWeaponFile_S21(weaponName) : nullptr;
	}

	// Already built this weapon from disk?
	{
		std::lock_guard<std::mutex> lk(s_diskGroupsMtx);
		auto it = s_diskGroups.find(weaponName);
		if (it != s_diskGroups.end())
			return it->second;
	}

	// Build outside the lock; a concurrent double-build discards the loser.
	void* diskGroup = BuildDiskWeaponGroup(weaponName);
	if (!diskGroup)
		return v_ReadKVWeaponFile_S21(weaponName);   // no disk file / parse fail -> packed

	std::lock_guard<std::mutex> lk(s_diskGroupsMtx);
	auto res = s_diskGroups.emplace(weaponName, diskGroup);
	return res.second ? diskGroup : res.first->second;   // keep the race winner
}

//-----------------------------------------------------------------------------
void VWeaponKVDiskS21::GetFun(void) const
{
	// ReadKVWeaponFile.
	Module_FindPattern(g_GameDll,
		"40 55 53 56 57 41 56 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B D9")
		.GetPtr(v_ReadKVWeaponFile_S21);

	// KeyValues::LoadFromFile_Internal.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 55 41 54 41 55 41 56 41 57 "
		"48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 4D 8B F0")
		.GetPtr(v_KV_LoadFromFile_S21);

	// rtech HashName aligned / unaligned.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 41 56 45 33 C9")
		.GetPtr(v_KVHashAligned_S21);
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 41 56 45 33 DB")
		.GetPtr(v_KVHashUnaligned_S21);

	if (!v_ReadKVWeaponFile_S21)
		Warning(eDLL_T::FS, "[WEAP-DISK] ReadKVWeaponFile pattern unresolved -- disk weapon override disabled\n");
	if (!v_KV_LoadFromFile_S21)
		Warning(eDLL_T::FS, "[WEAP-DISK] KeyValues::LoadFromFile pattern unresolved -- disk weapon override disabled\n");
	if (!v_KVHashAligned_S21 || !v_KVHashUnaligned_S21)
		Warning(eDLL_T::FS, "[WEAP-DISK] HashName pattern(s) unresolved -- disk weapon override disabled\n");
}

//-----------------------------------------------------------------------------
void VWeaponKVDiskS21::GetVar(void) const
{
	// rtech globalHeap slot from `mov rdx, cs:` inside ReadKVWeaponFile.
	g_ppRTechGlobalHeap_S21 = Module_FindPattern(g_GameDll,
		"48 8B 15 ?? ?? ?? ?? 48 8B 4F ?? E8")
		.ResolveRelativeAddressSelf(0x3, 0x7).RCast<void**>();

	// Symbol table: lea rcx inside ReadKVWeaponFile.
	g_pKVSymbolTable_S21 = Module_FindPattern(g_GameDll,
		"48 8D 0D ?? ?? ?? ?? 4C 89 A4 24 ?? ?? ?? ?? BA 40 00 00 00")
		.ResolveRelativeAddressSelf(0x3, 0x7).RCast<void*>();

	if (!g_ppRTechGlobalHeap_S21)
		Warning(eDLL_T::FS, "[WEAP-DISK] globalHeap slot unresolved -- disk weapon override disabled\n");
	if (!g_pKVSymbolTable_S21)
		Warning(eDLL_T::FS, "[WEAP-DISK] KV symbol-table unresolved -- disk weapon override disabled\n");
}

//-----------------------------------------------------------------------------
void VWeaponKVDiskS21::Detour(const bool bAttach) const
{
	DetourSetup(&v_ReadKVWeaponFile_S21, &Hook_ReadKVWeaponFile_S21, bAttach);
}
