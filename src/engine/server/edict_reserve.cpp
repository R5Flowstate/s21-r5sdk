//=============================================================================//
//
// Purpose: Relocate the forced global_non_rewinding edict to the S21 slot.
//
// Level init forces CGlobalNonRewinding onto edict 129, below the allocator's
// dynamic floor of 137. The S21 client reads that entity off a fixed slot,
// MAX_PLAYERS * 2 + 1 = 257, so the game timescale, observer state and
// respawn data all resolve to whatever dynamic entity happens to sit at 257.
//
// num_edicts is a process-lifetime high-water mark and the dynamic floor is
// 137, so by the time the global entity is created 257 already belongs to a
// map entity. The hook raises the dynamic floor to 258 (patching ED_Alloc's
// immediate) and keeps num_edicts at or above it, so slots 129..257 are never
// handed out dynamically and the forced index is always free. A forced index
// never raises num_edicts and the append path ignores the in-use table, so the
// high-water mark is also bumped past every forced allocation.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier1/convar.h"
#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/detours.h"
#include "edict_reserve.h"

#include <cstdint>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static constexpr int16_t EDICT_GNR_ENGINE   = 129;
static constexpr int16_t EDICT_GNR_S21      = 257;
static constexpr int     EDICT_FORCED_LIMIT = 0x4000;

typedef int64_t (__fastcall *PFN_ED_Alloc)(int16_t forcedIndex);
static PFN_ED_Alloc v_ED_Alloc = nullptr;
static int*         g_pNumEdicts = nullptr;
static int32_t*     g_pDynamicFloorImm = nullptr;
static int32_t      s_nDynamicFloor = 137;

static ConVar bridge_gnr_edict("bridge_gnr_edict", "257", FCVAR_RELEASE,
	"Edict index for the forced global_non_rewinding entity. The S21 client "
	"reads it from slot 257; 0 keeps the engine default of 129.",
	true, 0.f, true, 16383.f);

static int64_t __fastcall Hook_ED_Alloc(int16_t forcedIndex)
{
	int16_t wantIndex = forcedIndex;
	const int relocate = bridge_gnr_edict.GetInt();

	if (forcedIndex == EDICT_GNR_ENGINE && relocate > 0 && relocate < EDICT_FORCED_LIMIT)
		wantIndex = static_cast<int16_t>(relocate);

	// Below the floor the dynamic branch appends at num_edicts, which would
	// hand out the reserved band.
	if (forcedIndex == -1 && g_pNumEdicts && *g_pNumEdicts < s_nDynamicFloor)
	{
		Msg(eDLL_T::SERVER, "[GNR-EDICT] num_edicts %d -> %d (dynamic floor)\n",
			*g_pNumEdicts, s_nDynamicFloor);
		*g_pNumEdicts = s_nDynamicFloor;
	}

	int64_t result = v_ED_Alloc(wantIndex);

	if (wantIndex != forcedIndex && static_cast<int>(result) == -1)
	{
		Warning(eDLL_T::SERVER,
			"[GNR-EDICT] edict %d is in use, falling back to engine edict %d\n",
			wantIndex, forcedIndex);
		wantIndex = forcedIndex;
		result = v_ED_Alloc(wantIndex);
	}

	if (forcedIndex != -1 && static_cast<int>(result) != -1 && g_pNumEdicts
		&& static_cast<int>(result) >= *g_pNumEdicts)
	{
		const int prev = *g_pNumEdicts;
		*g_pNumEdicts = static_cast<int>(result) + 1;
		Msg(eDLL_T::SERVER,
			"[GNR-EDICT] forced edict %d raised num_edicts %d -> %d\n",
			static_cast<int>(result), prev, *g_pNumEdicts);
	}

	if (wantIndex != forcedIndex && static_cast<int>(result) != -1)
		Msg(eDLL_T::SERVER,
			"[GNR-EDICT] global_non_rewinding on edict %d (engine default %d)\n",
			static_cast<int>(result), forcedIndex);

	return result;
}

void VEdictReserve::GetAdr(void) const
{
	LogFunAdr("ED_Alloc", v_ED_Alloc);
	LogVarAdr("sv.num_edicts", g_pNumEdicts);
}

void VEdictReserve::GetFun(void) const
{
	// ED_Alloc(int16 forcedIndex): the forced branch compares against 0x4000.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 20 41 56 48 83 EC 60 0F B7 D9 66 83 F9 FF 74 ?? "
		"B8 00 40 00 00 66 3B C8")
		.GetPtr(v_ED_Alloc);

	if (!v_ED_Alloc)
		Warning(eDLL_T::SERVER, "[GNR-EDICT] ED_Alloc pattern unresolved -- "
			"global_non_rewinding stays on edict 129\n");
}

void VEdictReserve::GetVar(void) const
{
	if (!v_ED_Alloc)
		return;

	// Dynamic branch opens with 'mov ebx, [num_edicts+4]' (the search cursor).
	CMemory load = CMemory(reinterpret_cast<uintptr_t>(v_ED_Alloc)).Offset(0x4D);
	if (!load.CheckOpCodes({ 0x8B, 0x1D }))
	{
		Warning(eDLL_T::SERVER, "[GNR-EDICT] num_edicts load shape mismatch at %p -- "
			"forced edicts will not raise the high-water mark\n", load.GetPtr());
		return;
	}

	g_pNumEdicts = load.ResolveRelativeAddress(2, 6).Offset(-4).RCast<int*>();

	// 'mov eax, 89h' -- the dynamic floor the scan starts from.
	CMemory floorImm = CMemory(reinterpret_cast<uintptr_t>(v_ED_Alloc)).Offset(0x60);
	if (!floorImm.CheckOpCodes({ 0xB8, 0x89, 0x00, 0x00, 0x00 }))
	{
		Warning(eDLL_T::SERVER, "[GNR-EDICT] dynamic floor immediate mismatch at %p -- "
			"reserved band not enforced\n", floorImm.GetPtr());
		return;
	}
	g_pDynamicFloorImm = floorImm.Offset(1).RCast<int32_t*>();
}

static void EdictReserve_WriteFloor(const int32_t floor)
{
	DWORD oldProt = 0;
	if (!VirtualProtect(g_pDynamicFloorImm, sizeof(int32_t), PAGE_EXECUTE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::SERVER, "[GNR-EDICT] VirtualProtect failed on the floor immediate (gle=%lu)\n",
			GetLastError());
		return;
	}
	*g_pDynamicFloorImm = floor;
	VirtualProtect(g_pDynamicFloorImm, sizeof(int32_t), oldProt, &oldProt);
	FlushInstructionCache(GetCurrentProcess(), g_pDynamicFloorImm, sizeof(int32_t));
	s_nDynamicFloor = floor;
	Msg(eDLL_T::SERVER, "[GNR-EDICT] ED_Alloc dynamic floor set to %d\n", floor);
}

void VEdictReserve::Detour(const bool bAttach) const
{
	if (!v_ED_Alloc || !g_pNumEdicts)
		return;

	const int relocate = bridge_gnr_edict.GetInt();
	if (g_pDynamicFloorImm && relocate > 0 && relocate < EDICT_FORCED_LIMIT)
		EdictReserve_WriteFloor(bAttach ? relocate + 1 : 137);

	DetourSetup(&v_ED_Alloc, &Hook_ED_Alloc, bAttach);
}
