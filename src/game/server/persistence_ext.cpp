//=============================================================================
//
// Purpose: raise S3 persistence DataDef caps to S21 capacity. Stock offsets stay.
//
//=============================================================================
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier0/memory_patch.h"
#include "tier0/commandline.h"
#include "tier1/convar.h"
#include "persistence_ext.h"
#include "persistence_basevar_overflow.h"
#include "game/shared/heap_canary.h"

#include <cstdint>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Runtime gate. Disable to keep the stock 16000-entry cap.
static ConVar sdk_pdef_expand("sdk_pdef_expand", "1",
	FCVAR_REPLICATED,
	"S21-parity persistence DataDef expansion (26000 entries vs S3's 16000). "
	"Allocates a heap buffer, redirects all engine references, raises limit "
	"constants. Disable + restart to run with stock S3 limits.");

//-----------------------------------------------------------------------------
// S3 layout constants (where things live in r5apex.exe's static.data).
//-----------------------------------------------------------------------------
namespace s3 {
	// `unk_1634F4630` (pdef base) — at module_base + 0x234F4630 in r5apex.exe.
	// shows it at absolute address 0x1634F4630 with image base.
	constexpr uint32_t kBaseRVA       = 0x234F4630;

	// Sub-global offsets within the pdef struct.
	constexpr uint32_t kEnumDefsOff   = 0x20740;   // unk_163514D70 (kept)
	constexpr uint32_t kSubTableOff   = 0x22530;   // unk_163516B60 (kept)
	constexpr uint32_t kArrayBaseOff  = 0x22CB8;   // unk_1635172E8 (kept)
	constexpr uint32_t kArrayField1   = 0x22CC0;   // unk_1635172F0 (kept)
	constexpr uint32_t kCounterOff    = 0x9FCB8;   // qword_1635942E8 (MOVED)
	constexpr uint32_t kMemsetEnd     = 0x9FCC0;
	constexpr uint32_t kSub1BaseOff   = 0x9FCC0;   // substruct1 begin (MOVED)
	constexpr uint32_t kSub1InitBase  = 0x9FCD8;   // unk_163594308 (MOVED)
	constexpr uint32_t kSub2BaseOff   = 0xA26C0;   // substruct2 begin (MOVED)
	constexpr uint32_t kSub2InitBase  = 0xA26D8;   // unk_163596D08 (MOVED)
	constexpr uint32_t kCtrlBaseOff   = 0xA2F80;   // qword_1635975B0 (MOVED)
	constexpr uint32_t kCtrlF8Off     = 0xA2F88;   // qword_1635975B8 (MOVED, ctrl+8)
	constexpr uint32_t kCtrlF10Off    = 0xA2F90;   // xmmword_1635975C0 (MOVED, ctrl+0x10)
	constexpr uint32_t kCtrlF20Off    = 0xA2FA0;   // xmmword_1635975D0 (MOVED, ctrl+0x20)
	constexpr uint32_t kCtrlF30Off    = 0xA2FB0;   // xmmword_1635975E0 (MOVED, ctrl+0x30)
	constexpr uint32_t kCtrlF40Off    = 0xA2FC0;   // qword_1635975F0 (MOVED, ctrl+0x40)
	constexpr uint32_t kCtrlEndOff    = 0xA3030;   // end of pdef-internal region
}

// SHIFT_HEAD = 0x18900 for script meta/enumDefs/subTable/array.
// SHIFT_TAIL must be a multiple of 56 so (idx + K)*56 substruct math stays integer.
namespace ours {
	constexpr uint32_t kItemsCap      = 4096;                        // S21 cap
	constexpr uint32_t kItemsBytes    = 48 * kItemsCap;              // 0x30000
	constexpr uint32_t kItemsCountOff = kItemsBytes;                 // 0x30000
	constexpr uint32_t kHeadShift     = kItemsCountOff - 0x17700;    // 0x18900

	// Head-shifted offsets (script meta, enumDefs, subTable, array).
	constexpr uint32_t kEnumValueNamesPoolUsedOff = 0x20708 + kHeadShift;    // 0x39008
	constexpr uint32_t kScriptFlagOff   = 0x20710 + kHeadShift;       // 0x39010
	constexpr uint32_t kScriptVerArgOff = 0x20718 + kHeadShift;       // 0x39018
	constexpr uint32_t kDataSizeOff     = 0x20720 + kHeadShift;       // 0x39020
	// S3-only extra fields between dataSize and enumDefs.
	constexpr uint32_t kS3Off20728      = 0x20728;
	constexpr uint32_t kS3Off20730      = 0x20730;
	constexpr uint32_t kS3Off20738      = 0x20738;
	constexpr uint32_t kEnumDefsOff     = s3::kEnumDefsOff + kHeadShift;     // 0x39040
	// Field between enumDefs end and structDefs base. Likely enumDefsUsed or pad.
	constexpr uint32_t kS3Off22528      = 0x22528;
	constexpr uint32_t kSubTableOff     = s3::kSubTableOff + kHeadShift;     // 0x3AE30 (= structDefs)
	constexpr uint32_t kSubTableFarOff  = 0x22CB0 + kHeadShift;       // 0x3B5B0 (= structDefsUsed)
	constexpr uint32_t kArrayBaseOff    = s3::kArrayBaseOff + kHeadShift;    // 0x3B5B8 (= itemIndexes.f0)
	constexpr uint32_t kArrayField1Off  = s3::kArrayField1 + kHeadShift;     // 0x3B5C0 (= itemIndexes.f1)

	constexpr uint32_t kEntryCount    = 26000;                       // entries
	constexpr uint32_t kArrayBytes    = 32 * kEntryCount;            // 0xCB200
	constexpr uint32_t kArrayEndOff   = kArrayBaseOff + kArrayBytes; // 0x1067B8
	constexpr uint32_t kCounterOff    = kArrayEndOff;                // 0x1067B8
	constexpr uint32_t kMemsetEnd     = kCounterOff + 8;             // 0x1067C0

	constexpr uint32_t kSub1Bytes     = 0xA26C0 - 0x9FCC0;            // 10752 (192*56)
	constexpr uint32_t kSub2Bytes     = 0xA2F80 - 0xA26C0;            // 2240 ( 40*56)
	constexpr uint32_t kCtrlBytes     = 0xA3030 - 0xA2F80;            // 176

	// Tail shift in K elements (1 elem = 56 bytes). Smallest N with new_sub1_base >= memset_end.
	constexpr uint32_t kSubElemShift  = 7511;
	constexpr uint32_t kSubByteShift  = kSubElemShift * 56;           // 0x66B08

	constexpr uint32_t kSub1BaseOff   = s3::kSub1BaseOff + kSubByteShift; // 0x1067C8
	constexpr uint32_t kSub1InitBase  = kSub1BaseOff + 0x18;              // 0x1067E0
	constexpr uint32_t kSub2BaseOff   = s3::kSub2BaseOff + kSubByteShift; // 0x1091C8
	constexpr uint32_t kSub2InitBase  = kSub2BaseOff + 0x18;              // 0x1091E0
	constexpr uint32_t kCtrlBaseOff   = s3::kCtrlBaseOff + kSubByteShift; // 0x109A88
	constexpr uint32_t kCtrlEndOff    = kCtrlBaseOff + kCtrlBytes;        // 0x109B38

	// Implicit-arithmetic K constants (engine baked).
	constexpr uint32_t kS3Sub1ElemK   = 0x2DA8;                       // 0x9FCC0 / 56
	constexpr uint32_t kS3Sub2ElemK   = 0x2E68;                       // 0xA26C0 / 56
	constexpr uint32_t kNewSub1ElemK  = kS3Sub1ElemK + kSubElemShift; // 0x4AFF
	constexpr uint32_t kNewSub2ElemK  = kS3Sub2ElemK + kSubElemShift; // 0x4BBF

	// SubTable also uses K-arithmetic with stride 48: `pdef + 48*(idx + K)`.
	// K = subTable_base / 48. kHeadShift is a multiple of 48 (= 2096 * 48), so
	// K_new = K_old + (kHeadShift / 48) is integer.
	constexpr uint32_t kHeadElemShift   = kHeadShift / 48;            // 2096
	constexpr uint32_t kS3SubTableElemK = 0xB71;                      // 0x22530 / 48 = 2929
	constexpr uint32_t kNewSubTableElemK= kS3SubTableElemK + kHeadElemShift; // 0x13A1

	// EnumDefs (stride 40) and enumValueNamesPool (stride 8) use a different K than entries (56).
	constexpr uint32_t kHeadQwordShift  = kHeadShift / 8;             // 0x3120
	constexpr uint32_t kS3EnumDefsQwordK    = 0x40E5;                 // 0x20728 / 8 = 16613
	constexpr uint32_t kNewEnumDefsQwordK   = kS3EnumDefsQwordK + kHeadQwordShift;   // 0x7205
	constexpr uint32_t kS3EVNPoolQwordK     = 0x2EE1;                 // 0x17708 / 8 = 12001
	constexpr uint32_t kNewEVNPoolQwordK    = kS3EVNPoolQwordK + kHeadQwordShift;    // 0x6001

	constexpr size_t   kBufferSize    = 0x200000;                     // 2 MB (headroom)
}

//-----------------------------------------------------------------------------
// Public globals (header)
//-----------------------------------------------------------------------------
uint8_t* g_pSdkPdataDef  = nullptr;
uint8_t* g_pOrigPdataDef = nullptr;

const size_t   kSdkPdefBufferSize    = ours::kBufferSize;
const uint32_t kSdkPdefArrayBaseOff  = ours::kArrayBaseOff;
const uint32_t kSdkPdefEntryCount    = ours::kEntryCount;
const uint32_t kSdkPdefCounterOff    = ours::kCounterOff;
const uint32_t kSdkPdefSubTableOff   = ours::kSubTableOff;

// LEA / RIP-relative xref table. Byte-validate before write; too many mismatches abort onto stock limits.
struct LeaXref
{
	uint32_t    rva;         // instruction RVA (relative to module base)
	uint8_t     disp_off;    // offset of disp32 within instruction
	uint32_t    s3_off;      // expected target offset from orig pdef base
	uint32_t    new_off;     // target offset within our buffer
	const char* tag;         // diagnostic
};

static constexpr LeaXref kLeaXrefs[] = {
	// === Base unk_1634F4630 (21 xrefs) ===
	// All are 7-byte LEA forms: `48 8D xx [disp32]` or `4C 8D xx [disp32]`.
	// disp_off = 3 for every one. Target: buffer + 0.
	{ 0x1FF5EC, 3, 0x0, 0x0, "base@1FF5EC" },
	{ 0x1FF630, 3, 0x0, 0x0, "base@1FF630" },
	{ 0x1FF652, 3, 0x0, 0x0, "base@1FF652" },
	{ 0x1FF6A5, 3, 0x0, 0x0, "base@1FF6A5" },
	{ 0x1FF700, 3, 0x0, 0x0, "base@1FF700" },
	{ 0x1FF72A, 3, 0x0, 0x0, "base@1FF72A" },
	{ 0x1FF7C9, 3, 0x0, 0x0, "base@1FF7C9" },
	{ 0x1FF9EC, 3, 0x0, 0x0, "base@1FF9EC" },
	{ 0x1FFB67, 3, 0x0, 0x0, "base@1FFB67" },
	{ 0x303B5E, 3, 0x0, 0x0, "base@303B5E" },
	{ 0x315F11, 3, 0x0, 0x0, "base@315F11" },
	{ 0x315F40, 3, 0x0, 0x0, "base@315F40" },
	{ 0x316086, 3, 0x0, 0x0, "base@316086" },
	{ 0x31626D, 3, 0x0, 0x0, "base@31626D" },
	{ 0x360743, 3, 0x0, 0x0, "base@360743 (server pdef load)" },
	{ 0x360E42, 3, 0x0, 0x0, "base@360E42 (client pdef receive)" },
	{ 0x3611CA, 3, 0x0, 0x0, "base@3611CA" },
	{ 0x361286, 3, 0x0, 0x0, "base@361286" },
	{ 0x36249C, 3, 0x0, 0x0, "base@36249C" },
	{ 0x36270F, 3, 0x0, 0x0, "base@36270F" },
	{ 0x3628F7, 3, 0x0, 0x0, "base@3628F7" },

	// === Array entry[0].field0 unk_1635172E8 (10 xrefs) ===
	// Array stays at S3 offset 0x22CB8 in our buffer.
	{ 0x1FF823, 3, s3::kArrayBaseOff, ours::kArrayBaseOff, "arr.f0@1FF823" },
	{ 0x1FF9E2, 3, s3::kArrayBaseOff, ours::kArrayBaseOff, "arr.f0@1FF9E2" },
	{ 0x1FFA89, 3, s3::kArrayBaseOff, ours::kArrayBaseOff, "arr.f0@1FFA89" },
	{ 0x28D4B4, 3, s3::kArrayBaseOff, ours::kArrayBaseOff, "arr.f0@28D4B4 (writer)" },
	{ 0x315D34, 3, s3::kArrayBaseOff, ours::kArrayBaseOff, "arr.f0@315D34" },
	{ 0x315EEB, 3, s3::kArrayBaseOff, ours::kArrayBaseOff, "arr.f0@315EEB" },
	{ 0x315FB8, 3, s3::kArrayBaseOff, ours::kArrayBaseOff, "arr.f0@315FB8" },
	{ 0x316156, 3, s3::kArrayBaseOff, ours::kArrayBaseOff, "arr.f0@316156" },
	{ 0x316241, 3, s3::kArrayBaseOff, ours::kArrayBaseOff, "arr.f0@316241" },
	{ 0x36272B, 3, s3::kArrayBaseOff, ours::kArrayBaseOff, "arr.f0@36272B" },

	// === Array entry[0].field1 unk_1635172F0 (12 xrefs) ===
	{ 0x1FF872, 3, s3::kArrayField1, ours::kArrayBaseOff + 8, "arr.f1@1FF872" },
	{ 0x1FF8D2, 3, s3::kArrayField1, ours::kArrayBaseOff + 8, "arr.f1@1FF8D2" },
	{ 0x1FF932, 3, s3::kArrayField1, ours::kArrayBaseOff + 8, "arr.f1@1FF932" },
	{ 0x1FF992, 3, s3::kArrayField1, ours::kArrayBaseOff + 8, "arr.f1@1FF992" },
	{ 0x315D87, 3, s3::kArrayField1, ours::kArrayBaseOff + 8, "arr.f1@315D87" },
	{ 0x315DB8, 3, s3::kArrayField1, ours::kArrayBaseOff + 8, "arr.f1@315DB8" },
	{ 0x315DE1, 3, s3::kArrayField1, ours::kArrayBaseOff + 8, "arr.f1@315DE1" },
	{ 0x315E37, 3, s3::kArrayField1, ours::kArrayBaseOff + 8, "arr.f1@315E37" },
	{ 0x315E6A, 3, s3::kArrayField1, ours::kArrayBaseOff + 8, "arr.f1@315E6A" },
	{ 0x315E91, 3, s3::kArrayField1, ours::kArrayBaseOff + 8, "arr.f1@315E91" },
	{ 0x3558BE, 3, s3::kArrayField1, ours::kArrayBaseOff + 8, "arr.f1@3558BE" },
	{ 0x36133B, 3, s3::kArrayField1, ours::kArrayBaseOff + 8, "arr.f1@36133B" },

	// === enumDefs pool unk_163514D70 (2 xrefs) — MOVED past items_dict ===
	{ 0x28D4F4, 3, s3::kEnumDefsOff, ours::kEnumDefsOff, "enumDefs@28D4F4" },
	{ 0x36292B, 3, s3::kEnumDefsOff, ours::kEnumDefsOff, "enumDefs@36292B" },

	// === Subtable unk_163516B60 (3 xrefs) — MOVED past items_dict ===
	{ 0x1FFB9C, 3, s3::kSubTableOff, ours::kSubTableOff, "subtable@1FFB9C" },
	{ 0x362495, 3, s3::kSubTableOff, ours::kSubTableOff, "subtable@362495" },
	{ 0x3628E3, 3, s3::kSubTableOff, ours::kSubTableOff, "subtable@3628E3" },

	// === Counter qword_1635942E8 (6 xrefs) — MOVED to ours::kCounterOff ===
	{ 0x1FFB8E, 2, s3::kCounterOff, ours::kCounterOff, "counter@1FFB8E (mov eax)" },
	{ 0x28D4A7, 3, s3::kCounterOff, ours::kCounterOff, "counter@28D4A7" },
	{ 0x3558B1, 3, s3::kCounterOff, ours::kCounterOff, "counter@3558B1" },
	{ 0x355998, 3, s3::kCounterOff, ours::kCounterOff, "counter@355998" },
	{ 0x361328, 3, s3::kCounterOff, ours::kCounterOff, "counter@361328" },
	{ 0x361749, 3, s3::kCounterOff, ours::kCounterOff, "counter@361749" },

	// === Substruct init bases (init only — both run once at exe load) ===
	// Patches are harmless either way, but keep them consistent for catalog
	// completeness; the runtime data is preserved via SeedSubstructs.
	{ 0x859F9, 3, s3::kSub1InitBase, ours::kSub1InitBase, "sub1init@859F9" },
	{ 0x85A59, 3, s3::kSub2InitBase, ours::kSub2InitBase, "sub2init@85A59" },

	// === Control block field 0 (qword_1635975B0) — 2 xrefs ===
	{ 0x85AB3,  3, s3::kCtrlBaseOff, ours::kCtrlBaseOff + 0x00, "ctrl.f0@85AB3 (init)" },
	{ 0x35FDF4, 3, s3::kCtrlBaseOff, ours::kCtrlBaseOff + 0x00, "ctrl.f0@35FDF4 (reinit)" },

	// === Control block field +8 (qword_1635975B8) — 5 xrefs ===
	{ 0x85AC1,  3, s3::kCtrlF8Off,   ours::kCtrlBaseOff + 0x08, "ctrl.f8@85AC1 (init)" },
	{ 0x85B17,  3, s3::kCtrlF8Off,   ours::kCtrlBaseOff + 0x08, "ctrl.f8@85B17 (init)" },
	{ 0x35FE0B, 3, s3::kCtrlF8Off,   ours::kCtrlBaseOff + 0x08, "ctrl.f8@35FE0B" },
	{ 0x35FE28, 3, s3::kCtrlF8Off,   ours::kCtrlBaseOff + 0x08, "ctrl.f8@35FE28" },
	{ 0x35FE40, 3, s3::kCtrlF8Off,   ours::kCtrlBaseOff + 0x08, "ctrl.f8@35FE40" },

	// === Control block field +0x10 (xmmword_1635975C0) — 3 xrefs ===
	// 0x85AC8 is `movdqa` with prefix 66 0F 7F -> 8-byte instr, disp_off = 4.
	{ 0x85AC8,  4, s3::kCtrlF10Off,  ours::kCtrlBaseOff + 0x10, "ctrl.f10@85AC8 (movdqa init)" },
	{ 0x35FE47, 3, s3::kCtrlF10Off,  ours::kCtrlBaseOff + 0x10, "ctrl.f10@35FE47" },
	{ 0x35FE53, 3, s3::kCtrlF10Off,  ours::kCtrlBaseOff + 0x10, "ctrl.f10@35FE53" },

	// === Control block field +0x20 (xmmword_1635975D0) — 1 xref (init) ===
	{ 0x85B07,  4, s3::kCtrlF20Off,  ours::kCtrlBaseOff + 0x20, "ctrl.f20@85B07 (movdqa init)" },

	// === Control block field +0x30 (xmmword_1635975E0) — 1 xref (init) ===
	{ 0x85B0F,  4, s3::kCtrlF30Off,  ours::kCtrlBaseOff + 0x30, "ctrl.f30@85B0F (movdqa init)" },

	// === Control block field +0x40 (qword_1635975F0) — 1 xref (init) ===
	{ 0x85B1E,  3, s3::kCtrlF40Off,  ours::kCtrlBaseOff + 0x40, "ctrl.f40@85B1E (init)" },
};

// Disp32-from-base patches: `[r12 + disp32]` inside the pdef consumers.
struct Disp32Patch
{
	uint32_t    rva;
	uint8_t     disp_off;
	uint32_t    s3_imm;
	uint32_t    new_imm;
	const char* tag;
};

static constexpr Disp32Patch kDisp32Patches[] = {
	// --- (RecursiveBuildIndexes) ---
	// Counter access uses [rsi + 0x9FCB8] (7-byte mov, disp_off=3).
	{ 0x35E79F, 3, s3::kCounterOff, ours::kCounterOff, "RBI counter READ" },
	{ 0x35E7BA, 3, s3::kCounterOff, ours::kCounterOff, "RBI counter WRITE" },

	// --- (LoadDef) ---
	// Sets r13 = pdef + 0xA2F80 (control block); patch to point at moved ctrl.
	{ 0x35F1FB, 4, s3::kCtrlBaseOff, ours::kCtrlBaseOff, "LoadDef ctrl LEA r13" },

	// Sets rbx = pdef + 0xA26C0 (substruct2 base); rbx is then passed to
	// and as their 'a1' arg.
	{ 0x35F220, 4, s3::kSub2BaseOff, ours::kSub2BaseOff, "LoadDef sub2 LEA rbx" },

	// Counter check before the rebuild loop: `cmp [r12+0x9FCB8], r14`.
	{ 0x35F757, 4, s3::kCounterOff, ours::kCounterOff, "LoadDef counter if-check" },

	// Counter check at loop tail: `cmp r14, [r12+0x9FCB8]`.
	{ 0x35F856, 4, s3::kCounterOff, ours::kCounterOff, "LoadDef counter loop tail" },

	// Helper called by client pdef receive.
	{ 0x360EEF, 3, s3::kCtrlBaseOff, ours::kCtrlBaseOff, "ctrl LEA rdi" },

	// `mov rax, [rbp+0xA2F88]` -> control block field +8 (pool ptr).
	{ 0x360F71, 3, s3::kCtrlF8Off,   ours::kCtrlBaseOff + 0x08, "ctrl+8 MOV" },

	// --- (per-client persistence-session initializer) ---
	// `mov eax, [rdx+0x9FCB8]` -> reads counter into local field. Uses rdx as
	// pdef base. 6-byte instruction (no REX prefix; eax is 32-bit).
	{ 0x361CFB, 2, s3::kCounterOff, ours::kCounterOff, "counter READ" },

	// items_dict count slot: S3 0x17700 -> 0x30000.
	{ 0x35D7BA, 3, 0x17700, ours::kItemsCountOff, "items count READ @items_dict alloc (rcx-base)" },
	{ 0x35D7E4, 3, 0x17700, ours::kItemsCountOff, "items count RELOAD post-error (rbx-base)" },
	{ 0x35D7EF, 3, 0x17700, ours::kItemsCountOff, "items count WRITE @items_dict alloc (rbx-base)" },

	// --- (LoadDef inline allocator) ---
	{ 0x35F691, 4, 0x17700, ours::kItemsCountOff, "items count READ @LoadDef (r12-base SIB)" },
	{ 0x35F6B6, 4, 0x17700, ours::kItemsCountOff, "items count RELOAD @LoadDef (r12-base SIB)" },
	{ 0x35F6C2, 4, 0x17700, ours::kItemsCountOff, "items count WRITE @LoadDef (r12-base SIB)" },

	// === SCRIPT META FIELDS (S3 0x20710..0x20720 -> ours 0x39010..0x39020) ===

	// scriptVersion stored DWORD at S3 0x20710
	{ 0x35EA52, 3, 0x20710, ours::kScriptFlagOff,   "scriptVer READ @ (mov r8d, [r14+...])" },
	{ 0x35EA7E, 3, 0x20710, ours::kScriptFlagOff,   "scriptVer WRITE @ (mov [r14+...], eax)" },
	{ 0x35F87F, 4, 0x20710, ours::kScriptFlagOff,   "scriptVer WRITE @LoadDef (mov [r12+...], eax SIB)" },

	// scriptVersion ARG storage qword at S3 0x20718
	{ 0x35F26B, 4, 0x20718, ours::kScriptVerArgOff, "scriptVerArg WRITE @LoadDef (mov [r12+...], rax)" },
	{ 0x35F86F, 4, 0x20718, ours::kScriptVerArgOff, "scriptVerArg READ @LoadDef (mov rax, [r12+...])" },
	{ 0x1FFC93, 3, 0x20718, ours::kScriptVerArgOff, "scriptVerArg READ @ (rax-base getter)" },

	// dataSize stored qword at S3 0x20720
	{ 0x35F6F3, 4, 0x20720, ours::kDataSizeOff,     "dataSize WRITE @LoadDef (mov [r12+...], rdx SIB)" },

	// subTable locale base: S3 0x22530 -> 0x3AE30.
	{ 0x35DA24, 3, 0x22530, ours::kSubTableOff, "subTable LEA r11 @ (lea r11, [rdx+...])" },
	{ 0x35F215, 4, 0x22530, ours::kSubTableOff, "subTable LEA r15 @LoadDef (lea r15, [r12+...] SIB)" },
	{ 0x360EA5, 3, 0x22530, ours::kSubTableOff, "subTable LEA r8 @ (lea r8, [rcx+...])" },

	// === SUBTABLE +0x10 LEA (S3 0x22540 -> ours 0x3AE40) ===
	{ 0x35E1BE, 3, 0x22540, ours::kSubTableOff + 0x10, "subTable+0x10 LEA rsi @" },

	// === SUBTABLE FAR FIELD (S3 0x22CB0 -> ours 0x3B5B0) ===

	{ 0x35F22F, 4, 0x22CB0, ours::kSubTableFarOff, "subTable far WRITE 1 @LoadDef (r12-base SIB)" },
	{ 0x35DA0F, 3, 0x22CB0, ours::kSubTableFarOff, "subTable far READ @ (mov rdi, [rdx+...])" },
	{ 0x35E1A3, 3, 0x22CB0, ours::kSubTableFarOff, "subTable far CMP @ (cmp rbx, [r13+...])" },
	{ 0x35EF7A, 3, 0x22CB0, ours::kSubTableFarOff, "subTable far READ @ (mov rbx, [r14+...])" },
	{ 0x35EFAF, 3, 0x22CB0, ours::kSubTableFarOff, "subTable far WRITE @ (mov [r14+...], rax)" },

	// enumDefs disp32-from-register: S3 0x20740 -> 0x39040.
	{ 0x1FFA21, 4, 0x20740, ours::kEnumDefsOff, "enumDefs SIB cmp @ (rcx-base, r8 idx)" },
	{ 0x1FFD88, 4, 0x20740, ours::kEnumDefsOff, "enumDefs SIB cmp @ (rdx-base, r8*8 idx)" },
	{ 0x315F20, 4, 0x20740, ours::kEnumDefsOff, "enumDefs SIB cmp @ (rcx-base, rdx idx)" },
	{ 0x35E1E3, 3, 0x20740, ours::kEnumDefsOff, "enumDefs READ @ (mov rdx, [r13+...])" },
	{ 0x35E72A, 3, 0x20740, ours::kEnumDefsOff, "enumDefs READ @RBI (mov r14, [rax+...])" },
	{ 0x35FB5F, 3, 0x20740, ours::kEnumDefsOff, "enumDefs READ @ (mov r10, [rax+...])" },
	{ 0x361DB1, 3, 0x20740, ours::kEnumDefsOff, "enumDefs READ @ (mov r9, [rax+...])" },
	{ 0x362534, 3, 0x20740, ours::kEnumDefsOff, "enumDefs READ (mov rdx, [rax+...])" },

	// === ARRAY.F0 DISP32-FROM-REGISTER (S3 0x22CB8 -> ours 0x3B5B8) ===
	// 2 real sites (8 false positives rejected: many `mov eax, 0x22C` immediates
	// also start with 0xb8 followed by 0x2c 0x02 0x00).
	{ 0x315F4B, 4, 0x22CB8, ours::kArrayBaseOff, "arr.f0 SIB read @ (r8-base, rdx idx)" },
	{ 0x35E7C8, 3, 0x22CB8, ours::kArrayBaseOff, "arr.f0 WRITE @RBI (mov [rdx+...], rdi)" },

	// === ARRAY.F1 DISP32-FROM-REGISTER (S3 0x22CC0 -> ours 0x3B5C0) ===
	// All 6 hits are real disp32 accesses (verified by ModRM/SIB context).
	{ 0x303BBD, 4, 0x22CC0, ours::kArrayField1Off, "arr.f1 SIB read @ (rdx-base, rdi idx)" },
	{ 0x3160A4, 4, 0x22CC0, ours::kArrayField1Off, "arr.f1 SIB read @ (r8-base, rdx idx)" },
	{ 0x35E7CF, 3, 0x22CC0, ours::kArrayField1Off, "arr.f1 WRITE @RBI (mov [rdx+...], rax)" },
	{ 0x35F765, 4, 0x22CC0, ours::kArrayField1Off, "arr.f1 LEA r15 @LoadDef (lea r15, [r12+...])" },
	{ 0x3611E0, 4, 0x22CC0, ours::kArrayField1Off, "arr.f1 SIB read @ (rdx-base, r9 idx)" },
	{ 0x3612A0, 4, 0x22CC0, ours::kArrayField1Off, "arr.f1 SIB read @ (rdx-base, r9 idx 2)" },

	// === ARRAY.F2 (S3 0x22CC8 = arr_base + 16) -> ours 0x3B5C8 ===
	// pdataIndexInfo struct field at +16 of each 32B entry. Not in original
	// catalog because we treated array as just 2 fields; symbols show 4.
	{ 0x35E7D6, 3, 0x22CC8, ours::kArrayField1Off + 0x08, "arr.f2 WRITE @RBI (mov [rdx+...], r8)" },
	{ 0x3160DA, 4, 0x22CC8, ours::kArrayField1Off + 0x08, "arr.f2 SIB read @ (r8+rdx)" },
	{ 0x361215, 4, 0x22CC8, ours::kArrayField1Off + 0x08, "arr.f2 SIB read @ (rdx+r9)" },
	{ 0x3612D8, 4, 0x22CC8, ours::kArrayField1Off + 0x08, "arr.f2 SIB read @ (rdx+r9 2)" },

	// === ARRAY.F3 (S3 0x22CD0 = arr_base + 24) -> ours 0x3B5D0 ===
	// 11-byte instruction: `mov qword [rdx+0x22CD0], imm32`.
	{ 0x35E7DD, 3, 0x22CD0, ours::kArrayField1Off + 0x10, "arr.f3 WRITE imm @RBI" },

	// === ENUMVALUENAMESPOOLUSED (S3 0x20708 -> ours 0x39008) ===
	// Field at +0x20708 (qword): enumValueNamesPool used count.
	// All 3 hits are in (struct/enum start handler region).
	{ 0x35EC76, 3, 0x20708, ours::kEnumValueNamesPoolUsedOff, "enumValueNamesPoolUsed READ @ (rcx)" },
	{ 0x35ECBC, 3, 0x20708, ours::kEnumValueNamesPoolUsedOff, "enumValueNamesPoolUsed READ @ (rcx 2)" },
	{ 0x35ECC7, 3, 0x20708, ours::kEnumValueNamesPoolUsedOff, "enumValueNamesPoolUsed WRITE @ (rax)" },

	// Head-region field at 0x20728 (between dataSize and enumDefs).
	{ 0x315F5B, 4, 0x20728, ours::kS3Off20728 + ours::kHeadShift, "head@0x20728 SIB read @" },
	{ 0x35D834, 3, 0x20728, ours::kS3Off20728 + ours::kHeadShift, "head@0x20728 LEA r11 @" },

	// === HEAD-REGION FIELD AT 0x20730 ===
	{ 0x35FC07, 3, 0x20730, ours::kS3Off20730 + ours::kHeadShift, "head@0x20730 READ @" },

	// === HEAD-REGION FIELD AT 0x20738 ===
	{ 0x1FFA19, 4, 0x20738, ours::kS3Off20738 + ours::kHeadShift, "head@0x20738 SIB read @" },
	{ 0x1FFD80, 4, 0x20738, ours::kS3Off20738 + ours::kHeadShift, "head@0x20738 SIB read @" },
	{ 0x315F18, 4, 0x20738, ours::kS3Off20738 + ours::kHeadShift, "head@0x20738 SIB read @" },

	// === HEAD-REGION FIELD AT 0x22528 (just before structDefs) ===
	// Likely enumDefsUsed or related. 3 hits all in XXX/EXXX area.
	{ 0x35D81F, 3, 0x22528, ours::kS3Off22528 + ours::kHeadShift, "head@0x22528 READ @ (rdi)" },
	{ 0x35EC42, 3, 0x22528, ours::kS3Off22528 + ours::kHeadShift, "head@0x22528 READ @ (rbx)" },
	{ 0x35EC81, 3, 0x22528, ours::kS3Off22528 + ours::kHeadShift, "head@0x22528 WRITE @ (rax)" },
};

//-----------------------------------------------------------------------------
// Immediate-constant patches.
//-----------------------------------------------------------------------------
struct ImmPatch
{
	uint32_t    rva;
	uint8_t     imm_off;
	uint32_t    s3_imm;
	uint32_t    new_imm;
	const char* tag;
};

static constexpr ImmPatch kItemsAllocatedRaise =
	{ 0x35F24C, 4, 0xB4, 0x1000, "itemsAllocated 180->4096 (heap-backed)" };

static constexpr ImmPatch kImmPatches[] = {
	// === Limit constants ===

	// Entry-cap cmp at +0xF6: `cmp r8, 0x3E7F`. (7B, imm at +3)
	{ 0x35E7A6, 3, 0x3E7F,  0x658F,            "entry cap 16000->26000" },

	// Error-message arg `mov edx, 0x3E80` at. (5B, imm at +1)
	{ 0x35E859, 1, 0x3E80,  0x6590,            "entry cap msg arg 16000->26000" },

	// memset size at: `mov r8d, 0x9FCC0`. (6B, imm at +2)
	{ 0x35F1ED, 2, 0x9FCC0, ours::kMemsetEnd,  "memset size 0x9FCC0->ours" },

	// PDATA_MAX_ITEMS_DEFS check at: `cmp rax, 0x7D0`. (6B, imm at +2)
	// S21 native cap is 4096; same pdef parses cleanly on S21, so 4096 is correct.
	{ 0x35F69D, 2, 0x7D0,   0x1000,            "items cap 2000->4096" },

	// Items error msg arg at: `mov edx, 0x7D0`. (5B, imm at +1)
	{ 0x35F6A5, 1, 0x7D0,   0x1000,            "items cap msg arg 2000->4096" },

	// SECOND items-cap check (helper) at: `cmp rax, 0x7D0`. (6B, imm at +2)
	// Missed in original catalog — fires from a different allocator path during pdef parse.
	{ 0x35D7CB, 2, 0x7D0,   0x1000,            "items cap helper 2000->4096" },

	// Items error msg arg at: `mov edx, 0x7D0`. (5B, imm at +1)
	{ 0x35D7D3, 1, 0x7D0,   0x1000,            "items cap helper msg arg 2000->4096" },

	// dataSize cap at: `cmp edx, 0x18000`. (6B, imm at +2)
	{ 0x35F6FC, 2, 0x18000, 0x20000,           "dataSize 96K->128K" },

	// dataSize error msg arg at: `mov r8d, 0x18000`. (6B, imm at +2)
	{ 0x35F704, 2, 0x18000, 0x20000,           "dataSize msg arg 96K->128K" },

	// itemsAllocated cap: `mov qword ptr [r15+0x20], 0xB4`.
	kItemsAllocatedRaise,

	// Implicit-arithmetic K constants for moved substructs. IMUL/SHL/ADD bake offsets into immediates.
	{ 0x35D951, 2, ours::kS3Sub1ElemK, ours::kNewSub1ElemK, "sub1 K @ (add rax)" },
	{ 0x35DB41, 2, ours::kS3Sub1ElemK, ours::kNewSub1ElemK, "sub1 K @ (add rax)" },
	{ 0x35ECF5, 3, ours::kS3Sub1ElemK, ours::kNewSub1ElemK, "sub1 K @ (lea [rbx+])" },
	{ 0x35FC13, 3, ours::kS3Sub1ElemK, ours::kNewSub1ElemK, "sub1 K @ (add rcx)" },

	// Substruct2 K patches: K=0x2E68 -> kNewSub2ElemK (=0x4BBF at S21-mirror shift)
	{ 0x35DFB0, 2, ours::kS3Sub2ElemK, ours::kNewSub2ElemK, "sub2 K @ (add rax)" },
	{ 0x35EFF4, 3, ours::kS3Sub2ElemK, ours::kNewSub2ElemK, "sub2 K @ (lea [rbx+])" },
	{ 0x35FA83, 2, ours::kS3Sub2ElemK, ours::kNewSub2ElemK, "sub2 K @ (add rax)" },

	// SubTable K patches (stride 48): K=0xB71 -> 0x13A1.
	{ 0x35E761, 3, ours::kS3SubTableElemK, ours::kNewSubTableElemK, "subTable K @ (add rcx)" },

	// `lea rsi, [rbx+0xB71]` at +0x656 ($STRUCT_START dedup). 7B, imm@3.
	{ 0x35EFB6, 3, ours::kS3SubTableElemK, ours::kNewSubTableElemK, "subTable K @ (lea rsi)" },

	// `add rax, 0xB71` (short-form REX.W + 05 + imm32). 6B, imm@2.
	{ 0x35FD8C, 2, ours::kS3SubTableElemK, ours::kNewSubTableElemK, "subTable K @ (add rax)" },

	// `add rax, 0xB71` short-form. 6B, imm@2.
	{ 0x361E78, 2, ours::kS3SubTableElemK, ours::kNewSubTableElemK, "subTable K @ (add rax)" },

	// `add rcx, 0xB71` long-form. 7B, imm@3.
	{ 0x36260B, 3, ours::kS3SubTableElemK, ours::kNewSubTableElemK, "subTable K (add rcx)" },

	// enumDefs qword K: 0x40E5 -> 0x7205.

	{ 0x35EC88, 4, ours::kS3EnumDefsQwordK, ours::kNewEnumDefsQwordK, "enumDef K_qword @$ENUM_START allocator (lea [rbx*4+K])" },
	{ 0x316266, 3, ours::kS3EnumDefsQwordK, ours::kNewEnumDefsQwordK, "enumDef K_qword @ (lea [rax+K])" },
	{ 0x362795, 4, ours::kS3EnumDefsQwordK, ours::kNewEnumDefsQwordK, "enumDef K_qword @ (lea [rax*4+K])" },

	// enumValueNamesPool K: 0x2EE1 -> 0x6001.
	{ 0x35ECCE, 3, ours::kS3EVNPoolQwordK, ours::kNewEVNPoolQwordK, "enumValueNamesPool K @$ENUM_START allocator (lea rcx)" },
};

// Copy original pre-initialized substruct/control bytes into the new buffer.
static void SeedSubstructs(uint8_t* dst, const uint8_t* origBase)
{
	std::memcpy(dst + ours::kSub1BaseOff,
		origBase + s3::kSub1BaseOff, ours::kSub1Bytes);
	std::memcpy(dst + ours::kSub2BaseOff,
		origBase + s3::kSub2BaseOff, ours::kSub2Bytes);
	std::memcpy(dst + ours::kCtrlBaseOff,
		origBase + s3::kCtrlBaseOff, ours::kCtrlBytes);
}

//-----------------------------------------------------------------------------
// Per-site validating patchers
//-----------------------------------------------------------------------------
static int PatchLeaXrefs(uintptr_t moduleBase, uintptr_t origBase, uint8_t* newBase, bool apply)
{
	int ok = 0, fail = 0;
	for (const LeaXref& xr : kLeaXrefs)
	{
		uint8_t*  pInstr     = (uint8_t*)(moduleBase + xr.rva);
		uint8_t*  pDisp      = pInstr + xr.disp_off;
		uintptr_t nextRIP    = (uintptr_t)pDisp + 4;
		int32_t   curDisp    = *(int32_t*)pDisp;
		uintptr_t resolved   = nextRIP + (intptr_t)curDisp;
		uintptr_t expected   = origBase + xr.s3_off;

		if (resolved != expected)
		{
			if (fail < 8)
			{
				Warning(eDLL_T::ENGINE,
					"[PDEF-EXT] LEA xref MISMATCH '%s' RVA=0x%X "
					"resolved=0x%llX expected=0x%llX (disp32=0x%X)\n",
					xr.tag, xr.rva,
					(unsigned long long)resolved, (unsigned long long)expected,
					(unsigned)curDisp);
			}
			++fail;
			continue;
		}

		const uintptr_t newTarget = (uintptr_t)newBase + xr.new_off;
		const int64_t   newDisp64 = (int64_t)newTarget - (int64_t)nextRIP;
		if (newDisp64 > INT32_MAX || newDisp64 < INT32_MIN)
		{
			Warning(eDLL_T::ENGINE,
				"[PDEF-EXT] LEA xref RANGE OVERFLOW '%s' RVA=0x%X "
				"newDisp=0x%llX -- buffer not within +-2GB?\n",
				xr.tag, xr.rva, (unsigned long long)newDisp64);
			++fail;
			continue;
		}

		if (apply)
		{
			const int32_t newDisp = (int32_t)newDisp64;
			if (!Mem_PatchCode(pDisp, &newDisp, 4))
			{
				Warning(eDLL_T::ENGINE,
					"[PDEF-EXT] LEA xref VirtualProtect failed '%s' RVA=0x%X\n",
					xr.tag, xr.rva);
				++fail;
				continue;
			}
		}
		++ok;
	}

	Msg(eDLL_T::ENGINE, "[PDEF-EXT] LEA xrefs: %d OK, %d failed (of %zd)%s\n",
		ok, fail, sizeof(kLeaXrefs)/sizeof(kLeaXrefs[0]),
		apply ? "" : " (validate)");
	return fail;
}

static int PatchDisp32Sites(uintptr_t moduleBase, bool apply)
{
	int ok = 0, fail = 0;
	for (const Disp32Patch& p : kDisp32Patches)
	{
		uint8_t* pDisp   = (uint8_t*)(moduleBase + p.rva + p.disp_off);
		uint32_t curImm  = *(uint32_t*)pDisp;

		if (curImm != p.s3_imm)
		{
			Warning(eDLL_T::ENGINE,
				"[PDEF-EXT] disp32 MISMATCH '%s' RVA=0x%X "
				"got=0x%X expected=0x%X\n",
				p.tag, p.rva, curImm, p.s3_imm);
			++fail;
			continue;
		}

		if (apply && !Mem_PatchCode(pDisp, &p.new_imm, 4))
		{
			Warning(eDLL_T::ENGINE,
				"[PDEF-EXT] disp32 VirtualProtect failed '%s' RVA=0x%X\n",
				p.tag, p.rva);
			++fail;
			continue;
		}
		++ok;
	}
	Msg(eDLL_T::ENGINE, "[PDEF-EXT] disp32 sites: %d OK, %d failed (of %zd)%s\n",
		ok, fail, sizeof(kDisp32Patches)/sizeof(kDisp32Patches[0]),
		apply ? "" : " (validate)");
	return fail;
}

static int PatchImmediates(uintptr_t moduleBase, bool apply)
{
	int ok = 0, fail = 0;
	for (const ImmPatch& p : kImmPatches)
	{
		if (p.rva == kItemsAllocatedRaise.rva && !PersistenceBvo_IsArmed())
		{
			Msg(eDLL_T::ENGINE,
				"[PDEF-EXT] skipped itemsAllocated raise (BVO not armed).\n");
			continue;
		}

		uint8_t* pImm   = (uint8_t*)(moduleBase + p.rva + p.imm_off);
		uint32_t curImm = *(uint32_t*)pImm;

		if (curImm != p.s3_imm)
		{
			Warning(eDLL_T::ENGINE,
				"[PDEF-EXT] imm MISMATCH '%s' RVA=0x%X "
				"got=0x%X expected=0x%X\n",
				p.tag, p.rva, curImm, p.s3_imm);
			++fail;
			continue;
		}

		if (apply && !Mem_PatchCode(pImm, &p.new_imm, 4))
		{
			Warning(eDLL_T::ENGINE,
				"[PDEF-EXT] imm VirtualProtect failed '%s' RVA=0x%X\n",
				p.tag, p.rva);
			++fail;
			continue;
		}
		++ok;
	}
	Msg(eDLL_T::ENGINE, "[PDEF-EXT] immediates: %d OK, %d failed (of %zd)%s\n",
		ok, fail, sizeof(kImmPatches)/sizeof(kImmPatches[0]),
		apply ? "" : " (validate)");
	return fail;
}

void PersistenceExt_ApplyItemsAllocatedRaise(void)
{
	if (!PersistenceBvo_IsArmed() || !g_pSdkPdataDef)
	{
		Msg(eDLL_T::ENGINE,
			"[PDEF-EXT] skipped itemsAllocated raise (BVO not armed or persist_ext buffer absent).\n");
		return;
	}

	const uintptr_t moduleBase = g_GameDll.GetModuleBase();
	const ImmPatch& p = kItemsAllocatedRaise;
	uint8_t* pImm = (uint8_t*)(moduleBase + p.rva + p.imm_off);
	const uint32_t curImm = *(uint32_t*)pImm;

	if (curImm == p.new_imm)
		return;

	if (curImm != p.s3_imm)
	{
		Warning(eDLL_T::ENGINE,
			"[PDEF-EXT] imm MISMATCH '%s' RVA=0x%X "
			"got=0x%X expected=0x%X\n",
			p.tag, p.rva, curImm, p.s3_imm);
		return;
	}

	if (!Mem_PatchCode(pImm, &p.new_imm, 4))
	{
		Warning(eDLL_T::ENGINE,
			"[PDEF-EXT] imm VirtualProtect failed '%s' RVA=0x%X\n",
			p.tag, p.rva);
		return;
	}

	Msg(eDLL_T::ENGINE,
		"[PDEF-EXT] itemsAllocated raise 180->4096 applied (BVO armed)\n");
}

//-----------------------------------------------------------------------------
// VPersistenceExt
//-----------------------------------------------------------------------------
void VPersistenceExt::GetAdr(void) const
{
	if (g_pSdkPdataDef)
	{
		Msg(eDLL_T::ENGINE,
			"[PDEF-EXT] addresses: orig=0x%p new=0x%p (size=0x%zX)\n",
			(void*)g_pOrigPdataDef, (void*)g_pSdkPdataDef, ours::kBufferSize);
	}
}

void VPersistenceExt::GetVar(void) const { }

// Dedi compares pdef version against the client's reported version. Mismatch is fatal; keep the stock seed.
static void Pdef_ForceFullDefFileSend()
{
	CMemory pat = Module_FindPattern(g_GameDll,
		"48 8B 0D ?? ?? ?? ?? 48 3B 8F 98 87 04 00 0F 85");
	if (!pat)
	{
		Warning(eDLL_T::ENGINE,
			"[PDEF-EXT] UseCached gate pattern NOT FOUND -- bridge will "
			"break if dedi sends svc_UseCachedPersistenceDefFile\n");
		return;
	}

	uint8_t* pJne = pat.Offset(14).RCast<uint8_t*>();
	DWORD oldProt;
	if (!VirtualProtect(pJne, 2, PAGE_EXECUTE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::ENGINE,
			"[PDEF-EXT] UseCached gate VirtualProtect failed at 0x%p\n",
			(void*)pJne);
		return;
	}

	pJne[0] = 0x90; // NOP
	pJne[1] = 0xE9; // JMP rel32 (rel32 displacement reused as-is)
	VirtualProtect(pJne, 2, oldProt, &oldProt);
	FlushInstructionCache(GetCurrentProcess(), pJne, 2);
	Msg(eDLL_T::ENGINE,
		"[PDEF-EXT] patched UseCached gate at 0x%p -- dedi will "
		"always send svc_PersistenceDefFile (cmd 23), never "
		"svc_UseCachedPersistenceDefFile (cmd 24)\n", (void*)pJne);
}

void VPersistenceExt::Detour(const bool bAttach) const
{
	if (!bAttach) return;

	Pdef_ForceFullDefFileSend();

	if (!sdk_pdef_expand.GetBool() ||
		CommandLine()->CheckParm("-disable_pdef"))
	{
		Msg(eDLL_T::ENGINE,
			"[PDEF-EXT] disabled (cmdline -disable_pdef or "
			"sdk_pdef_expand=0) -- stock S3 limits "
			"(16000-entry cap, fatal overflow)\n");
		return;
	}

	const uintptr_t moduleBase = g_GameDll.GetModuleBase();
	g_pOrigPdataDef = (uint8_t*)(moduleBase + s3::kBaseRVA);

	// Compile-time sanity on the layout math (S21-mirror layout).
	static_assert(ours::kItemsCountOff == 0x30000,  "items count slot mismatch (4096*48)");
	static_assert(ours::kHeadShift     == 0x18900,  "head shift mismatch");
	static_assert(ours::kScriptFlagOff == 0x39010,  "scriptFlag offset mismatch");
	static_assert(ours::kEnumDefsOff   == 0x39040,  "enumDefs offset mismatch");
	static_assert(ours::kSubTableOff   == 0x3AE30,  "subTable offset mismatch");
	static_assert(ours::kSubTableFarOff== 0x3B5B0,  "subTable far offset mismatch");
	static_assert(ours::kArrayBaseOff  == 0x3B5B8,  "array base mismatch");
	static_assert(ours::kArrayEndOff   == 0x1067B8, "array end mismatch");
	static_assert(ours::kCounterOff    == 0x1067B8, "counter offset mismatch");
	static_assert(ours::kMemsetEnd     == 0x1067C0, "memset end mismatch");
	static_assert(ours::kSub1BaseOff   == 0x1067C8, "sub1 base mismatch");
	static_assert(ours::kSub2BaseOff   == 0x1091C8, "sub2 base mismatch");
	static_assert(ours::kCtrlBaseOff   == 0x109A88, "ctrl base mismatch");
	static_assert(ours::kSub1BaseOff   >= ours::kMemsetEnd, "sub1 overlaps memset region");
	static_assert(ours::kCtrlEndOff    <= ours::kBufferSize, "ctrl past buffer end");
	static_assert(ours::kNewSub1ElemK  == 0x4AFF, "sub1 K patch value mismatch");
	static_assert(ours::kNewSub2ElemK  == 0x4BBF, "sub2 K patch value mismatch");
	static_assert(ours::kSubByteShift  == 0x66B08, "shift bytes mismatch");
	static_assert(ours::kArrayBaseOff  > ours::kItemsCountOff, "array overlaps items_dict");
	static_assert(ours::kSubTableOff   > ours::kItemsCountOff, "subTable overlaps items_dict");

	// 1) Allocate the buffer within +-2 GB of r5apex module base.
	g_pSdkPdataDef = Mem_AllocNearModule(g_GameDll, ours::kBufferSize + HeapCanary::kTailBytes);
	if (!g_pSdkPdataDef)
	{
		Warning(eDLL_T::ENGINE,
			"[PDEF-EXT] FAILED to allocate %zu-byte buffer within +-2GB of "
			"module base 0x%llX -- expansion disabled, stock S3 limits in "
			"effect\n", ours::kBufferSize, (unsigned long long)moduleBase);
		return;
	}
	HeapCanary::RegisterTail("pdef-ext", g_pSdkPdataDef, ours::kBufferSize);

	// 2) Seed pre-initialized substructs from the original.data location.
	// ran at exe load and populated function pointers and
	// a heap-pool pointer in those regions; preserve them.
	SeedSubstructs(g_pSdkPdataDef, g_pOrigPdataDef);

	const int leaFail = PatchLeaXrefs(moduleBase,
		(uintptr_t)g_pOrigPdataDef, g_pSdkPdataDef, false);
	const int dispFail = PatchDisp32Sites(moduleBase, false);
	const int immFail = PatchImmediates(moduleBase, false);

	if (leaFail + dispFail + immFail != 0)
	{
		Warning(eDLL_T::ENGINE,
			"[PDEF-EXT] expansion OFF -- %d LEA + %d disp32 + %d imm mismatches; "
			"no sites written, stock S3 dest+size kept together.\n",
			leaFail, dispFail, immFail);
		HeapCanary::Unregister(g_pSdkPdataDef);
		VirtualFree(g_pSdkPdataDef, 0, MEM_RELEASE);
		g_pSdkPdataDef = nullptr;
		g_pOrigPdataDef = nullptr;
		return;
	}

	if (PatchLeaXrefs(moduleBase, (uintptr_t)g_pOrigPdataDef, g_pSdkPdataDef, true)
		+ PatchDisp32Sites(moduleBase, true)
		+ PatchImmediates(moduleBase, true) != 0)
	{
		for (const LeaXref& xr : kLeaXrefs)
		{
			uint8_t* pDisp = (uint8_t*)(moduleBase + xr.rva + xr.disp_off);
			const uintptr_t nextRIP = (uintptr_t)pDisp + 4;
			const int64_t disp64 = (int64_t)((uintptr_t)g_pOrigPdataDef + xr.s3_off) - (int64_t)nextRIP;
			if (disp64 >= INT32_MIN && disp64 <= INT32_MAX)
			{
				const int32_t d = (int32_t)disp64;
				Mem_PatchCode(pDisp, &d, 4);
			}
		}
		for (const Disp32Patch& p : kDisp32Patches)
			Mem_PatchCode((uint8_t*)(moduleBase + p.rva + p.disp_off), &p.s3_imm, 4);
		for (const ImmPatch& p : kImmPatches)
			Mem_PatchCode((uint8_t*)(moduleBase + p.rva + p.imm_off), &p.s3_imm, 4);
		Warning(eDLL_T::ENGINE,
			"[PDEF-EXT] expansion OFF -- apply failed after a clean validate; "
			"sites restored to stock dest+size.\n");
		HeapCanary::Unregister(g_pSdkPdataDef);
		VirtualFree(g_pSdkPdataDef, 0, MEM_RELEASE);
		g_pSdkPdataDef = nullptr;
		g_pOrigPdataDef = nullptr;
		return;
	}
}
