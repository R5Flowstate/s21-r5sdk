//=============================================================================//
//
// Purpose: Settings-layout (stlt) field-finder rescue. See header for design.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "public/tier0/memaddr.h"
#include "public/tier0/tier0_iface.h"
#include "tier1/cvar.h"
#include "thirdparty/detours/include/detours.h"
#include "stlt_field_rescue.h"

#include <cstdint>
#include <unordered_set>
#include <mutex>
#include <atomic>

//-----------------------------------------------------------------------------
// FieldFinder 24 B: +0x00 offset, +0x04 type, +0x08 name, +0x10 next.
//-----------------------------------------------------------------------------
struct StltFieldFinder
{
	uint32_t offset;
	int32_t  type;
	const char* name;
	StltFieldFinder* next;
};
static_assert(sizeof(StltFieldFinder) == 0x18, "FieldFinder size mismatch");

// HeaderFinder: +0x28 children, +0x38 siblings.
static constexpr uintptr_t kSiblingHeadOff = 0x38;
static constexpr uint32_t  kMissSentinel   = 0xFFFFFFFFu;

// Pointer types rescue to offset 0 (empty-string ptr); value types to offset 8 (zero).
static constexpr uint32_t kRescueOffsetPointer = 0u;
static constexpr uint32_t kRescueOffsetValue   = 8u;

static inline bool IsPointerLikeStltType(uint32_t type)
{
	return type == 5 /*string*/ || type == 8 /*array*/ || type == 9 /*array_dyn*/;
}

static inline uint32_t RescueOffsetForType(uint32_t type)
{
	return IsPointerLikeStltType(type) ? kRescueOffsetPointer : kRescueOffsetValue;
}

// Misses (glide*/jetpack* and other S21-removed fields) get a type-aware safe default.
static inline uint32_t RescueOffsetForField(const char* /*name*/, uint32_t type)
{
	return RescueOffsetForType(type);
}

static int64_t (*v_CSettingsLayout__ResolveFieldFinders)(void* node, void* parent, void* layoutCtx) = nullptr;
static int64_t (*v_SettingsLayout_LookupField)(void* hashTable, const char* fieldName) = nullptr;

// One-shot miss dump per (layout, header, field) to stlt_resolver_misses.log.
static std::mutex g_deepDiagMutex;
static std::unordered_set<uint64_t> g_deepDiagKeys; // (layoutPtr ^ namePtr) keys logged
static FILE* g_deepDiagFp = nullptr;
static std::atomic<bool> g_deepDiagOpened{false};

static void EnsureDeepDiagOpen()
{
	bool expected = false;
	if (!g_deepDiagOpened.compare_exchange_strong(expected, true)) return;
	fopen_s(&g_deepDiagFp, "stlt_resolver_misses.log", "w");
	if (g_deepDiagFp)
		fprintf(g_deepDiagFp,
			"# stlt-resolver miss dump\n"
			"# columns: layoutPtr, layoutName, headerName, fieldName, fieldType, "
			"htEntries, htMask, htNameBufBase, htSublayoutArrBase, ht[0..3] entries\n");
}

// layoutCtx+8 is a pointer-to-pointer-to-name. SEH: rpak memory can move.
static const char* SafeLayoutName(void* layoutCtx)
{
	if (!layoutCtx) return "<null-ctx>";
	__try
	{
		const char** pp = *reinterpret_cast<const char***>(
			reinterpret_cast<uint8_t*>(layoutCtx) + 8);
		if (!pp) return "<null-pp>";
		const char* name = *pp;
		if (!name) return "<null-name>";
		// Printable ASCII, max 128.
		for (int i = 0; i < 128; ++i)
		{
			const unsigned char c = static_cast<unsigned char>(name[i]);
			if (c == 0) return name;
			if (c < 0x20 || c > 0x7E) return "<non-printable>";
		}
		return "<too-long>";
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return "<fault>"; }
}

// Layout HT: +8 entries, +24 buckets, +56 name buf, +64 sublayout arr (72 B).
static void DumpLayoutHt(void* layoutPtr)
{
	if (!g_deepDiagFp || !layoutPtr) return;
	__try
	{
		const uint8_t* L = reinterpret_cast<const uint8_t*>(layoutPtr);
		const uint64_t entriesBase = *reinterpret_cast<const uint64_t*>(L + 8);
		const uint32_t bucketCount = *reinterpret_cast<const uint32_t*>(L + 24);
		const uint64_t nameBufBase = *reinterpret_cast<const uint64_t*>(L + 56);
		const uint64_t sublayArrBase = *reinterpret_cast<const uint64_t*>(L + 64);
		fprintf(g_deepDiagFp,
			"  layoutPtr=%p entries=%p buckets=%u nameBuf=%p sublayArr=%p\n",
			layoutPtr, (void*)entriesBase, bucketCount,
			(void*)nameBufBase, (void*)sublayArrBase);
		if (entriesBase && bucketCount > 0 && bucketCount <= 1024)
		{
			const uint32_t showN = bucketCount < 8u ? bucketCount : 8u;
			fprintf(g_deepDiagFp, "  ht[0..%u]: ", showN - 1);
			for (uint32_t i = 0; i < showN; ++i)
			{
				const uint8_t* e = reinterpret_cast<const uint8_t*>(entriesBase + 8 * i);
				const uint16_t typ = *reinterpret_cast<const uint16_t*>(e);
				const uint16_t nameOff = *reinterpret_cast<const uint16_t*>(e + 2);
				const uint32_t tail = *reinterpret_cast<const uint32_t*>(e + 4);
				const char* nm = "";
				if (nameBufBase && nameOff)
				{
					const char* candidate = reinterpret_cast<const char*>(nameBufBase + nameOff);
					if (((uintptr_t)candidate & 0xFFFFFFFFFF000000ull) != 0)
					{
						bool ok = true;
						for (int k = 0; k < 32 && candidate[k]; ++k)
						{
							const unsigned char c = static_cast<unsigned char>(candidate[k]);
							if (c < 0x20 || c > 0x7E) { ok = false; break; }
						}
						if (ok) nm = candidate;
					}
				}
				fprintf(g_deepDiagFp, "[typ=%u nm=%s tail=%08X] ", typ, nm[0]?nm:"?", tail);
			}
			fprintf(g_deepDiagFp, "\n");
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		fprintf(g_deepDiagFp, "  <fault reading layout %p>\n", layoutPtr);
	}
}

//-----------------------------------------------------------------------------
// One log per interned field-name pointer.
//-----------------------------------------------------------------------------
static std::mutex g_rescueLogMutex;
static std::unordered_set<const char*> g_rescueLogged;

static const char* TypeName(int32_t fieldType)
{
	// 0=bool 1=int 2=float 3=time 4=vec 5=string 6=int 7=enum 8=array 9=array_dyn
	switch (fieldType)
	{
		case 0: return "bool";
		case 1: return "int";
		case 2: return "float";
		case 3: return "time";
		case 4: return "vec";
		case 5: return "string";
		case 6: return "int(6)";
		case 7: return "enum";
		case 8: return "array";
		case 9: return "array_dyn";
		default: return "?";
	}
}

static void LogRescueOnce_Zeroed(const char* fieldName, int32_t fieldType)
{
	std::lock_guard<std::mutex> lock(g_rescueLogMutex);
	if (g_rescueLogged.insert(fieldName).second)
	{
		(void)fieldType;
	}
}

//-----------------------------------------------------------------------------
// After the resolver: type-aware default for misses. HeaderFinder +8 name, +16 layout.
//-----------------------------------------------------------------------------
struct NodeCtx { const char* headerName; void* nodeLayout; };
__declspec(noinline) static NodeCtx ReadNodeCtx(void* node)
{
	NodeCtx out = { nullptr, nullptr };
	if (!node)
		return out;
	out.headerName = *reinterpret_cast<const char**>(
		reinterpret_cast<uint8_t*>(node) + 8);
	out.nodeLayout = *reinterpret_cast<void**>(
		reinterpret_cast<uint8_t*>(node) + 16);
	return out;
}

static void RescueSiblingList(void* node, void* layoutCtx)
{
	if (!node) return;
	StltFieldFinder* head = *reinterpret_cast<StltFieldFinder**>(
		reinterpret_cast<uint8_t*>(node) + kSiblingHeadOff);
	if (!head) return;

	// node+8 header name, node+16 sublayout.
	const NodeCtx nodeCtx = ReadNodeCtx(node);

	bool firstMissThisNode = true;

	for (StltFieldFinder* miss = head; miss; miss = miss->next)
	{
		if (miss->offset != kMissSentinel) continue;

		// No ConVar gate: ConVars init too late.
		{
			EnsureDeepDiagOpen();
			const char* layoutName = SafeLayoutName(layoutCtx);
			const uint64_t key = reinterpret_cast<uint64_t>(nodeCtx.nodeLayout) ^
			                     reinterpret_cast<uint64_t>(miss->name);
			std::lock_guard<std::mutex> lock(g_deepDiagMutex);
			if (g_deepDiagFp && g_deepDiagKeys.insert(key).second)
			{
				if (firstMissThisNode)
				{
					fprintf(g_deepDiagFp,
						"\n[node=%p layoutCtx=%p layoutName=%s headerName=%s "
						"nodeLayout=%p]\n",
						node, layoutCtx, layoutName,
						nodeCtx.headerName ? nodeCtx.headerName : "<top>",
						nodeCtx.nodeLayout);
					DumpLayoutHt(nodeCtx.nodeLayout);
					firstMissThisNode = false;
				}
				fprintf(g_deepDiagFp,
					"  MISS field=\"%s\" type=%d/%s\n",
					miss->name ? miss->name : "<null>",
					miss->type, TypeName(miss->type));
				fflush(g_deepDiagFp);
			}
		}

		miss->offset = RescueOffsetForField(miss->name, static_cast<uint32_t>(miss->type));
		LogRescueOnce_Zeroed(miss->name, miss->type);
	}
}

//-----------------------------------------------------------------------------
// Call original, then rescue this node's siblings (hook sees every recursive call).
//-----------------------------------------------------------------------------
static void ZeroDefaultPlayerLayoutHeader();

static std::atomic<uint32_t> g_resolveHookCalls{0};

// Forward declaration for dispatcher hook.
static int64_t Hook_ResolveFieldFinders(void* node, void* parent, void* layoutCtx);

//-----------------------------------------------------------------------------
// Walk FieldFinder tree at slot[3]. LookupField +0x04: low24 offset, high8 sublayout idx.
//-----------------------------------------------------------------------------
static std::atomic<int> g_manualResolveFields{0};
static std::atomic<int> g_manualResolveHeaders{0};
static std::atomic<int> g_manualResolveTrees{0};
static std::atomic<int> g_manualRescuedFields{0};

//-----------------------------------------------------------------------------
// Per-FieldFinder resolve/rescue trace to stlt_resolve_dump.log.
//-----------------------------------------------------------------------------
static FILE*             g_stltResolveDumpFp = nullptr;
static std::atomic<bool> g_stltResolveDumpOpened{false};
static std::atomic<int>  g_stltResolveTreeNo{0};

static void EnsureResolveDumpOpen()
{
	bool expected = false;
	if (!g_stltResolveDumpOpened.compare_exchange_strong(expected, true)) return;
	fopen_s(&g_stltResolveDumpFp, "stlt_resolve_dump.log", "w");
	if (g_stltResolveDumpFp)
		fprintf(g_stltResolveDumpFp,
			"# stlt manual-resolve dump -- per-FieldFinder resolve/rescue trace\n"
			"# RESOLVED  = found in loaded layout, was -1 (correct fix)\n"
			"# RECONFIRM = found, offset already equalled the resolved value\n"
			"# OVERWRITE = found, but CHANGED an already-set offset (suspect)\n"
			"# RESCUED   = NOT found in layout, was -1, forced to safe default\n"
			"# KEPT      = NOT found, offset already set, left untouched\n");
}

__declspec(noinline) static void ManualResolveHeader(
	uint8_t* node, uint8_t* parent, uint64_t* layoutCtx)
{
	if (!node) return;
	__try
	{
		uint64_t nodeLayout = 0;
		if (parent == nullptr)
		{
			// Top-level: layout is the loaded asset data pointer.
			nodeLayout = layoutCtx[1];
		}
		else
		{
			// Nested: parent lookup high-8 is the sublayout index.
			const char* name = *reinterpret_cast<const char* const*>(node + 8);
			const uint64_t parentLayout = *reinterpret_cast<const uint64_t*>(parent + 16);
			if (name && parentLayout && v_SettingsLayout_LookupField)
			{
				const uint8_t* entry = reinterpret_cast<const uint8_t*>(
					v_SettingsLayout_LookupField(
						reinterpret_cast<void*>(parentLayout), name));
				if (entry)
				{
					const uint32_t entry4 = *reinterpret_cast<const uint32_t*>(entry + 4);
					const uint32_t sublayoutIdx = (entry4 >> 24) & 0xFFu;
					const uint64_t sublayoutArrBase =
						*reinterpret_cast<const uint64_t*>(parentLayout + 64);
					if (sublayoutArrBase)
					{
						nodeLayout = sublayoutArrBase + 72 * sublayoutIdx;
					}
				}
			}
		}

		*reinterpret_cast<uint64_t*>(node + 16) = nodeLayout;
		g_manualResolveHeaders++;

		const char* hdrName = *reinterpret_cast<const char* const*>(node + 8);
		if (g_stltResolveDumpFp)
			fprintf(g_stltResolveDumpFp,
				"[header \"%s\"  %s  nodeLayout=0x%016llX]\n",
				hdrName ? hdrName : "<top>",
				parent ? "nested" : "top-level",
				(unsigned long long)nodeLayout);

		// Resolve siblings by name in this node's layout HT.
		uint8_t* sibling = *reinterpret_cast<uint8_t**>(node + 0x38);
		int guard = 0;
		while (sibling && guard < 16384)
		{
			const char* fieldName = *reinterpret_cast<const char* const*>(sibling + 8);
			uint32_t* const pOffset   = reinterpret_cast<uint32_t*>(sibling);
			const uint32_t  fieldType = *reinterpret_cast<const uint32_t*>(sibling + 4);
			const uint32_t  offBefore = *pOffset;
			bool resolved = false;
			const char* action = "KEPT";

			if (fieldName && nodeLayout && v_SettingsLayout_LookupField)
			{
				const uint8_t* entry = reinterpret_cast<const uint8_t*>(
					v_SettingsLayout_LookupField(
						reinterpret_cast<void*>(nodeLayout), fieldName));
				if (entry)
				{
					const uint32_t entry4 = *reinterpret_cast<const uint32_t*>(entry + 4);
					const uint32_t realOff = entry4 & 0xFFFFFFu;
					// Classify whether we are changing an already-set offset.
					action = (offBefore == kMissSentinel) ? "RESOLVED"
					       : (offBefore == realOff)        ? "RECONFIRM"
					       :                                 "OVERWRITE";
					*pOffset = realOff;
					g_manualResolveFields++;
					resolved = true;
				}
			}

			// Absent field: -1 sentinel -> type-aware default (not layoutBase+0xFFFFFFFF).
			if (!resolved && *pOffset == kMissSentinel)
			{
				*pOffset = RescueOffsetForField(fieldName, fieldType);
				g_manualRescuedFields++;
				action = "RESCUED";
				LogRescueOnce_Zeroed(fieldName, static_cast<int32_t>(fieldType));
			}

			if (g_stltResolveDumpFp)
				fprintf(g_stltResolveDumpFp,
					"  %-42s type=%-2u before=0x%08X after=0x%08X  %s\n",
					fieldName ? fieldName : "<null>",
					fieldType, offBefore, *pOffset, action);

			sibling = *reinterpret_cast<uint8_t**>(sibling + 0x10);
			guard++;
		}

		// Nested array element schemas.
		uint8_t* child = *reinterpret_cast<uint8_t**>(node + 0x28);
		guard = 0;
		while (child && guard < 4096)
		{
			ManualResolveHeader(child, node, layoutCtx);
			child = *reinterpret_cast<uint8_t**>(child + 0x30);
			guard++;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(noinline) static void ManualResolveTree(uint64_t* slot, void* a4)
{
	__try
	{
		// Dispatcher should write slot[1] = a4; do it here if it did not.
		slot[1] = reinterpret_cast<uint64_t>(a4);

		// slot[+0x10] low byte = 1 (resolver-completed).
		reinterpret_cast<uint8_t*>(slot)[0x10] = 1;

		EnsureResolveDumpOpen();
		const int treeNo = ++g_stltResolveTreeNo;
		if (g_stltResolveDumpFp)
			fprintf(g_stltResolveDumpFp,
				"\n=== TREE #%d  slot.guid=0x%016llX  slot.data=0x%016llX  "
				"headerRoot=0x%016llX ===\n",
				treeNo,
				(unsigned long long)slot[0],
				(unsigned long long)reinterpret_cast<uint64_t>(a4),
				(unsigned long long)slot[3]);

		// Top-level HeaderFinder chain from slot[3].
		uint8_t* header = *reinterpret_cast<uint8_t**>(slot + 3);
		int guard = 0;
		while (header && guard < 4096)
		{
			ManualResolveHeader(header, nullptr, slot);
			header = *reinterpret_cast<uint8_t**>(header + 0x30);
			guard++;
		}
		g_manualResolveTrees++;
		if (g_stltResolveDumpFp) fflush(g_stltResolveDumpFp);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

// STLT FOURCC 1937009780. Manual resolve if the organic resolver call never hits our detour.
static int64_t (*v_StltDispatch_1405C2EF0)(int64_t a1, uint32_t a2, int a3, void* a4, int a5, int a6, int64_t a7) = nullptr;
static int64_t (*v_StltDispatch_140D5A250)(int64_t a1, uint32_t a2, int a3, void* a4, int a5, int a6, int64_t a7) = nullptr;
static std::atomic<uint32_t> g_dispatch1Calls{0};
static std::atomic<uint32_t> g_dispatch2Calls{0};
static std::atomic<uint32_t> g_dispatch1StltCalls{0};
static std::atomic<uint32_t> g_dispatch2StltCalls{0};

static int64_t Hook_StltDispatch1(int64_t a1, uint32_t a2, int a3, void* a4, int a5, int a6, int64_t a7)
{
	const uint32_t n = ++g_dispatch1Calls;
	const bool isStlt = (a3 == 1937009780);
	if (isStlt) ++g_dispatch1StltCalls;
	if (n <= 20 || isStlt)
	{
		Warning(eDLL_T::ENGINE,
			"[stlt-dispatch1] call#%u a2=%u a3=0x%08X(%c%c%c%c) a4=%p a7=0x%llX%s\n",
			n, a2, a3,
			(char)(a3 & 0xFF), (char)((a3>>8) & 0xFF),
			(char)((a3>>16) & 0xFF), (char)((a3>>24) & 0xFF),
			a4, (unsigned long long)a7,
			isStlt ? " <STLT>" : "");
	}
	return v_StltDispatch_1405C2EF0(a1, a2, a3, a4, a5, a6, a7);
}

static int64_t Hook_StltDispatch2(int64_t a1, uint32_t a2, int a3, void* a4, int a5, int a6, int64_t a7)
{
	++g_dispatch2Calls;
	const bool isStlt = (a3 == 1937009780);
	if (isStlt) ++g_dispatch2StltCalls;

	// If slot[3] is set and our resolver heartbeat stays 0, the organic call bypassed the detour.
	void* slot3_pre = nullptr;
	int slotIdx = -1;
	if (a3 == 1937009780 && (a2 == 4 || a2 == 16))
	{
		const uintptr_t base = g_GameDll.GetModuleBase();
		uint64_t* slotTable = reinterpret_cast<uint64_t*>(base + 0xD4ED1F0);
		for (int probe = 0; probe < 64; ++probe)
		{
			const int idx = (probe + static_cast<int>(a7 & 0xFF)) & 0x3F;
			const uint64_t s0 = slotTable[5 * idx];
			if (s0 == static_cast<uint64_t>(a7))
			{
				slotIdx = idx;
				slot3_pre = reinterpret_cast<void*>(slotTable[5 * idx + 3]);
				break;
			}
			if (s0 == 0) break;
		}
	}

	const uint32_t resolverCallsBefore = g_resolveHookCalls.load();

	// Snapshot slot[1] before the trampoline (`mov [rbx+8], rsi`).
	uint64_t slot1_pre_trampoline = 0xDEADDEAD;
	if (a3 == 1937009780 && (a2 == 4 || a2 == 16) && slotIdx >= 0)
	{
		const uintptr_t base2 = g_GameDll.GetModuleBase();
		uint64_t* st = reinterpret_cast<uint64_t*>(base2 + 0xD4ED1F0);
		slot1_pre_trampoline = st[5 * slotIdx + 1];
	}


	const int64_t result = v_StltDispatch_140D5A250(a1, a2, a3, a4, a5, a6, a7);

	if (isStlt)
	{
		const uint32_t resolverCallsAfter = g_resolveHookCalls.load();
		const uint32_t delta = resolverCallsAfter - resolverCallsBefore;
		if (delta == 0 && slot3_pre != nullptr)
		{
			// Organic resolver call bypassed the detour; ManualResolveTree fills offsets.
			const uintptr_t baseLocal = g_GameDll.GetModuleBase();
			uint64_t* slot = reinterpret_cast<uint64_t*>(
				baseLocal + 0xD4ED1F0 + slotIdx * 40);
			ManualResolveTree(slot, a4);
		}
	}

	return result;
}

static int64_t Hook_ResolveFieldFinders(void* node, void* parent, void* layoutCtx)
{
	const uint32_t n = ++g_resolveHookCalls;
	if (n == 1 || n == 10 || n == 100 || n == 1000 || (n % 5000) == 0)
	{
		const char* layoutName = SafeLayoutName(layoutCtx);
		Warning(eDLL_T::ENGINE,
			"[stlt-rescue] Hook_ResolveFieldFinders call #%u node=%p parent=%p "
			"layoutCtx=%p layoutName=%s\n",
			n, node, parent, layoutCtx, layoutName);
	}
	const int64_t result = v_CSettingsLayout__ResolveFieldFinders(node, parent, layoutCtx);
	RescueSiblingList(node, layoutCtx);
	// Zero the spectator fallback layout header once it is populated.
	ZeroDefaultPlayerLayoutHeader();
	return result;
}

//-----------------------------------------------------------------------------
// VStltFieldRescue
//-----------------------------------------------------------------------------
void VStltFieldRescue::GetAdr() const
{
	LogFunAdr("CSettingsLayout::ResolveFieldFinders",
		v_CSettingsLayout__ResolveFieldFinders);
	LogFunAdr("SettingsLayout::LookupField",
		v_SettingsLayout_LookupField);
}

void VStltFieldRescue::GetFun() const
{
	// Unique 24-byte prologue (single match).
	Module_FindPattern(g_GameDll,
		"40 53 55 57 48 83 EC 30 48 89 74 24 58 49 8B E8 4C 89 74 24 60 4C 8B F2")
		.GetPtr(v_CSettingsLayout__ResolveFieldFinders);

	// STLT asset dispatcher #1 (rpak callback table).
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 18 48 89 74 24 20 57 41 54 41 56 48 83 EC 30 4D 8B E1 8B C2")
		.GetPtr(v_StltDispatch_1405C2EF0);

	// STLT asset dispatcher #2.
	Module_FindPattern(g_GameDll,
		"48 89 74 24 18 57 48 83 EC 30 49 8B F1 8B C2 41 81 F8 72 69 6E 61")
		.GetPtr(v_StltDispatch_140D5A250);

	// SettingsLayout hash-table field lookup.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 57 44 0F B6 0A 45 33 C0 48 8B FA 4C 8B D2 45 84 C9")
		.GetPtr(v_SettingsLayout_LookupField);
}

// NOP the 5-byte Engine_Error CALL after the -1 sentinel write. JMP to next sibling stays.
static bool PatchMissingFieldErrorCall()
{
	CMemory hit = Module_FindPattern(g_GameDll,
		"C7 03 FF FF FF FF 48 8D 0D ?? ?? ?? ?? 4C 8B 45 08 49 8B D6 4D 8B 00 E8");
	if (!hit)
	{
		Warning(eDLL_T::ENGINE,
			"[stlt-rescue] missing-field Error() call site not found -- "
			"server will still crash on first unknown stlt field. Update the "
			"pattern in stlt_field_rescue.cpp::PatchMissingFieldErrorCall.\n");
		return false;
	}

	uint8_t* pCall = reinterpret_cast<uint8_t*>(hit.GetPtr() + 23);
	if (pCall[0] != 0xE8)
	{
		Warning(eDLL_T::ENGINE,
			"[stlt-rescue] missing-field Error() call site mismatch "
			"(expected E8 at %p, got %02X) -- not patching.\n",
			(void*)pCall, pCall[0]);
		return false;
	}

	DWORD oldProt = 0;
	if (!VirtualProtect(pCall, 5, PAGE_EXECUTE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::ENGINE,
			"[stlt-rescue] VirtualProtect failed for missing-field NOP patch "
			"at %p (gle=%lu)\n", (void*)pCall, GetLastError());
		return false;
	}

	memset(pCall, 0x90, 5);
	VirtualProtect(pCall, 5, oldProt, &oldProt);
	FlushInstructionCache(GetCurrentProcess(), pCall, 5);

	Msg(eDLL_T::ENGINE,
		"[stlt-rescue] NOP'd Engine_Error call at %p (sentinel write + JMP "
		"to next sibling intact -- rescue will alias the missing field)\n",
		(void*)pCall);
	return true;
}

// NOP the type-mismatch Engine_Error CALL. Offset is already set; wrong-type reads do not crash.
static bool PatchTypeMismatchErrorCall()
{
	CMemory hit = Module_FindPattern(g_GameDll,
		"48 63 4B 04 0F B7 C6 3B C8 74 33 66 83 FE 06 75 05 83 F9 07 74 28 "
		"48 8B 45 08 4D 8B 0C CF 48 8D 0D ?? ?? ?? ?? 48 8B 53 08 44 0F B7 C6 "
		"48 8B 00 48 89 44 24 20 4F 8B 04 C7 E8");
	if (!hit)
	{
		Warning(eDLL_T::ENGINE,
			"[stlt-rescue] type-mismatch Error() call site not found -- "
			"server will still crash on first wrong-type stlt field. Update "
			"the pattern in stlt_field_rescue.cpp::PatchTypeMismatchErrorCall.\n");
		return false;
	}

	uint8_t* pCall = reinterpret_cast<uint8_t*>(hit.GetPtr() + 57);
	if (pCall[0] != 0xE8)
	{
		Warning(eDLL_T::ENGINE,
			"[stlt-rescue] type-mismatch Error() call site mismatch "
			"(expected E8 at %p, got %02X) -- not patching.\n",
			(void*)pCall, pCall[0]);
		return false;
	}

	DWORD oldProt = 0;
	if (!VirtualProtect(pCall, 5, PAGE_EXECUTE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::ENGINE,
			"[stlt-rescue] VirtualProtect failed for type-mismatch NOP patch "
			"at %p (gle=%lu)\n", (void*)pCall, GetLastError());
		return false;
	}

	memset(pCall, 0x90, 5);
	VirtualProtect(pCall, 5, oldProt, &oldProt);
	FlushInstructionCache(GetCurrentProcess(), pCall, 5);

	Msg(eDLL_T::ENGINE,
		"[stlt-rescue] NOP'd type-mismatch Error call at %p (offset already "
		"set from layout -- engine will read with declared shape)\n",
		(void*)pCall);
	return true;
}

// Force six type=5 asset-ref sites onto the missing path: layoutBase[0] is a 0xFF sentinel.
struct AssetRefSite { const char* fieldName; uintptr_t cmpAddr; };
static const AssetRefSite kPlayerLayoutAssetRefSites[] = {
	{"landingImpactTable",   0x140fb3859ull},
	{"footstepImpactTable",  0x140fb388eull},
	{"dodgeImpactTable",     0x140fb38c4ull},
	{"slideEffectTable",     0x140fb38faull},
	{"viewPunchSpring",      0x140fb3937ull},
	{"ui_targetinfo",        0x140fb3977ull},
};

// Spectator fallback layout starts with 0xFF..FF; zero first 16 bytes so offset-0 rescues read zeros.
static std::atomic<bool> g_defaultLayoutHeaderZeroed{false};
static constexpr uintptr_t kDefaultLayoutPtrAddr = 0x14D4E6F48ull;

// Non-static: snapshot_diag.cpp calls this after the layout pointer is populated.
void ZeroDefaultPlayerLayoutHeader()
{
	if (g_defaultLayoutHeaderZeroed.load(std::memory_order_acquire)) return;

	HMODULE hExe = GetModuleHandleA(NULL);
	if (!hExe) return;
	const uintptr_t base = reinterpret_cast<uintptr_t>(hExe);
	constexpr uintptr_t kImageBase = 0x140000000ull;

	// Global pointer to the spectator/fallback layout.
	uint64_t* const ptrSlot = reinterpret_cast<uint64_t*>(
		base + (kDefaultLayoutPtrAddr - kImageBase));
	const uint64_t layoutPtr = *ptrSlot;
	if (!layoutPtr) return;

	// First thread to see a populated pointer does the write.
	bool expected = false;
	if (!g_defaultLayoutHeaderZeroed.compare_exchange_strong(
		expected, true, std::memory_order_acq_rel))
		return;

	uint8_t* const layoutBytes = reinterpret_cast<uint8_t*>(layoutPtr);

	// Bail if the header is not the 0xFF sentinel.
	uint64_t firstQword = 0;
	__try { firstQword = *reinterpret_cast<const uint64_t*>(layoutBytes); }
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Warning(eDLL_T::ENGINE,
			"[stlt-rescue] default layout header read SEH @ %p\n",
			(void*)layoutBytes);
		return;
	}

	if (firstQword != 0xFFFFFFFFFFFFFFFFull)
	{
		Msg(eDLL_T::ENGINE,
			"[stlt-rescue] default player layout header @ %p already non-"
			"sentinel (firstQword=0x%llX); skipping zero patch\n",
			(void*)layoutBytes, (unsigned long long)firstQword);
		return;
	}

	DWORD oldProt = 0;
	if (!VirtualProtect(layoutBytes, 16, PAGE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::ENGINE,
			"[stlt-rescue] VirtualProtect failed zeroing default layout "
			"header @ %p (gle=%lu)\n", (void*)layoutBytes, GetLastError());
		return;
	}
	memset(layoutBytes, 0, 16);
	VirtualProtect(layoutBytes, 16, oldProt, &oldProt);

	Msg(eDLL_T::ENGINE,
		"[stlt-rescue] zeroed first 16 bytes of default player layout "
		"@ %p (was 0xFFFFFFFFFFFFFFFF sentinel) -- rescued field reads now "
		"return safe zeros\n", (void*)layoutBytes);
}

static void PatchPlayerLayoutAssetRefs()
{
	HMODULE hExe = GetModuleHandleA(NULL);
	if (!hExe) return;
	const uintptr_t base = reinterpret_cast<uintptr_t>(hExe);
	constexpr uintptr_t kImageBase = 0x140000000ull;

	int patched = 0, skipped = 0;
	for (const AssetRefSite& s : kPlayerLayoutAssetRefSites)
	{
		uint8_t* p = reinterpret_cast<uint8_t*>(base + (s.cmpAddr - kImageBase));

		// cmp [r8], r14b (45 38 30) or cmp [rcx], r14b (44 38 31)
		const bool isR8  = (p[0] == 0x45 && p[1] == 0x38 && p[2] == 0x30);
		const bool isRcx = (p[0] == 0x44 && p[1] == 0x38 && p[2] == 0x31);
		if ((!isR8 && !isRcx) || p[3] != 0x74)
		{
			Warning(eDLL_T::ENGINE,
				"[stlt-rescue] asset-ref signature mismatch @ %p for "
				"\"%s\" (got %02X %02X %02X %02X); not patching.\n",
				p, s.fieldName, p[0], p[1], p[2], p[3]);
			skipped++;
			continue;
		}

		const int8_t origRel8 = static_cast<int8_t>(p[4]);
		const int newRel = origRel8 + 3;
		if (newRel < -128 || newRel > 127)
		{
			Warning(eDLL_T::ENGINE,
				"[stlt-rescue] asset-ref new rel8 %d out of range for "
				"\"%s\"; not patching.\n", newRel, s.fieldName);
			skipped++;
			continue;
		}

		const uint8_t newBytes[5] = {
			0xEB, static_cast<uint8_t>(newRel), 0x90, 0x90, 0x90,
		};

		DWORD oldProt = 0;
		if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProt))
		{
			Warning(eDLL_T::ENGINE,
				"[stlt-rescue] VirtualProtect failed for asset-ref \"%s\" "
				"@ %p (gle=%lu)\n", s.fieldName, p, GetLastError());
			skipped++;
			continue;
		}
		memcpy(p, newBytes, 5);
		VirtualProtect(p, 5, oldProt, &oldProt);
		FlushInstructionCache(GetCurrentProcess(), p, 5);
		patched++;
	}

	Msg(eDLL_T::ENGINE,
		"[stlt-rescue] player-layout asset-ref patches: %d/%zu sites "
		"forced to missing-path; skipped=%d\n",
		patched,
		sizeof(kPlayerLayoutAssetRefSites) / sizeof(kPlayerLayoutAssetRefSites[0]),
		skipped);
}

void VStltFieldRescue::Detour(const bool bAttach) const
{
	if (!v_CSettingsLayout__ResolveFieldFinders)
	{
		Warning(eDLL_T::ENGINE,
			"[stlt-rescue] CSettingsLayout::ResolveFieldFinders pattern "
			"unresolved -- missing-field crashes will NOT be guarded. "
			"Update the pattern in stlt_field_rescue.cpp.\n");
		return;
	}

	const LONG attachResult = DetourSetup(
		&v_CSettingsLayout__ResolveFieldFinders,
		&Hook_ResolveFieldFinders, bAttach);

	if (v_StltDispatch_1405C2EF0)
	{
		const LONG d1 = DetourSetup(&v_StltDispatch_1405C2EF0, &Hook_StltDispatch1, bAttach);
		if (bAttach)
			Msg(eDLL_T::ENGINE,
				"[stlt-rescue] DetourAttach StltDispatch1 result=0x%lX (target=0x%p)\n",
				d1, (void*)v_StltDispatch_1405C2EF0);
	}
	else if (bAttach)
		Warning(eDLL_T::ENGINE, "[stlt-rescue] StltDispatch1 pattern not found\n");

	if (v_StltDispatch_140D5A250)
	{
		const LONG d2 = DetourSetup(&v_StltDispatch_140D5A250, &Hook_StltDispatch2, bAttach);
		if (bAttach)
			Msg(eDLL_T::ENGINE,
				"[stlt-rescue] DetourAttach StltDispatch2 result=0x%lX (target=0x%p)\n",
				d2, (void*)v_StltDispatch_140D5A250);
	}
	else if (bAttach)
		Warning(eDLL_T::ENGINE, "[stlt-rescue] StltDispatch2 pattern not found\n");

	if (bAttach)
	{
		Msg(eDLL_T::ENGINE,
			"[stlt-rescue] DetourAttach result=0x%lX (target=0x%p)\n",
			attachResult, (void*)v_CSettingsLayout__ResolveFieldFinders);

		// NOP resolver Engine_Error so RescueSiblingList can run.
		PatchMissingFieldErrorCall();
		PatchTypeMismatchErrorCall();

		// Type=5 sites: layoutBase[0] is a sentinel, not a string pointer.
		PatchPlayerLayoutAssetRefs();

		// Scalar bodyModel/modelScale patches live in snapshot_diag SettingsBlock apply.
		if (v_CSettingsLayout__ResolveFieldFinders)
		{
			const uint8_t* p = reinterpret_cast<const uint8_t*>(
				v_CSettingsLayout__ResolveFieldFinders);
			Msg(eDLL_T::ENGINE,
				"[stlt-verify] first16 bytes @ %p"
				"%02X %02X %02X %02X %02X %02X %02X %02X "
				"%02X %02X %02X %02X %02X %02X %02X %02X\n",
				p,
				p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
				p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);

			Msg(eDLL_T::ENGINE,
				"[stlt-verify] manual call: resolverCallsBefore=%u\n",
				(unsigned)g_resolveHookCalls.load());
		}
		{
			const uintptr_t base = g_GameDll.GetModuleBase();
			uint64_t* slotTable = reinterpret_cast<uint64_t*>(base + 0xD4ED1F0);
			int nonZeroSlot3 = 0, nonZeroSlot4 = 0, occupiedSlots = 0;
			Msg(eDLL_T::ENGINE, "[stlt-slots] dumping (64 slots * 5 qwords):\n");
			for (int i = 0; i < 64; ++i)
			{
				const uint64_t s0 = slotTable[5 * i + 0];
				const uint64_t s1 = slotTable[5 * i + 1];
				const uint64_t s3 = slotTable[5 * i + 3];
				const uint64_t s4 = slotTable[5 * i + 4];
				if (s0 == 0) continue;
				occupiedSlots++;
				if (s3) nonZeroSlot3++;
				if (s4) nonZeroSlot4++;
				Msg(eDLL_T::ENGINE,
					"[stlt-slots] slot[%2d] guid=0x%016llX data=0x%016llX "
					"headerTree=0x%016llX extraData=0x%016llX\n",
					i, (unsigned long long)s0, (unsigned long long)s1,
					(unsigned long long)s3, (unsigned long long)s4);
			}
			Msg(eDLL_T::ENGINE,
				"[stlt-slots] occupied=%d nonZeroSlot3=%d nonZeroSlot4=%d\n",
				occupiedSlots, nonZeroSlot3, nonZeroSlot4);

			// Static HeaderFinder list head; 0xFFFFFFFF offset means resolve never ran.
			uint64_t* hfHead = reinterpret_cast<uint64_t*>(base + 0x2385BE0);
			Msg(eDLL_T::ENGINE,
				"[stlt-slots] @ %p first 4 qwords"
				"0x%016llX 0x%016llX 0x%016llX 0x%016llX\n",
				hfHead,
				(unsigned long long)hfHead[0], (unsigned long long)hfHead[1],
				(unsigned long long)hfHead[2], (unsigned long long)hfHead[3]);
		}
	}
}
