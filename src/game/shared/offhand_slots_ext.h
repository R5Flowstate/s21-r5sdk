//=============================================================================
//
// Purpose: SDK-side replication for offhand slots 6 and 7.
// Server storage is a side-band map; a SendProp proxy ships it. Do not write engine inventory bytes.
//
//=============================================================================
#ifndef OFFHAND_SLOTS_EXT_H
#define OFFHAND_SLOTS_EXT_H

#include <cstdint>

class CSquirrelVM;
struct SQVM;
struct ScriptClassDescriptor_t;

// Invalid-handle sentinel returned when the slot holds no weapon.
static constexpr uint32_t kOffhandSlotExtInvalidHandle = 0xFFFFFFFFu;

// Extended slot ID range covered by this module. The engine natively handles
// 0..5; we own 6 and 7. Bounds-check in every public entry point so callers
// can't accidentally shadow an engine slot via the extension API.
static constexpr int kOffhandSlotExtFirst = 6;
static constexpr int kOffhandSlotExtLast  = 7;

inline bool OffhandSlotsExt_IsExtendedSlot(int slot)
{
	return slot >= kOffhandSlotExtFirst && slot <= kOffhandSlotExtLast;
}

// Diagnostic-only switch for offhandWeapons[6/7] SendProp identity logging.
// Controlled by ConVar sdk_offhand_ext_diag (default 0). Launch
// +sdk_offhand_ext_diag 1 forces ON at SendTable-init time.
bool OffhandSlotsExt_IsDiagEnabled();

// Writes L1 and, on the server VM, enqueues an S->C broadcast. InvalidHandle clears.
bool OffhandSlotsExt_Set(void* pPlayer, int slot, uint32_t weaponEHandle, SQVM* v);

// Read-only getter. Returns the side-local L1 map value or
// kOffhandSlotExtInvalidHandle if the slot is empty / pPlayer is null /
// slot is outside the extended range.
uint32_t OffhandSlotsExt_Get(void* pPlayer, int slot, SQVM* v);

// Read-only getter for contexts without a VM handle (e.g. the mid-engine
// trampolines installed in /). Explicitly
// selects the server-side L1 map. Returns same sentinel on miss.
uint32_t OffhandSlotsExt_GetServer(void* pPlayer, int slot);

// Forward declaration of ESide (defined in sdk_entity_state.h) -- proper
// enum-class forward decl needs the underlying type so callers can pass
// `ESide::Server` / `ESide::Client` without including sdk_entity_state.h.
enum class ESide : uint8_t;

// Probes both L1 maps (listen-server ticks both sides; maps are disjoint by EHandle).
uint32_t OffhandSlotsExt_GetAnySide(void* pPlayer, int slot, ESide* outSide);

// Spawn + bind, then L1 write + L2 broadcast instead of native slot-store (past offhandWeapons[5] is aliased).
// Dedup: InvalidHandle if the slot is occupied; Take first to swap.
uint32_t OffhandSlotsExt_Give(void* pPlayer, int slot, const char* pszClassName,
                              uint32_t modsBitfield, SQVM* v);

// Destroy the slot weapon, clear L1, broadcast null. Stale handles still clear + broadcast.
bool OffhandSlotsExt_Take(void* pPlayer, int slot, SQVM* v);

// Resolve slot 6/7 to a live entity. Lazy-evicts stale L1 entries.
void* OffhandSlotsExt_GetEntity(void* pPlayer, int slot, SQVM* v);

// Called once from Script_RegisterPlayerScriptFunctions. Registers the
// player.{Set,Get}OffhandSlotExtEHandle script natives on the player class.
void OffhandSlotsExt_Register(ScriptClassDescriptor_t* playerStruct);

// Clear all slot maps. Called from the generic SDKEntityState level shutdown
// path; explicit hook here keeps the level-shutdown ordering documented.
void OffhandSlotsExt_LevelShutdown();

#ifndef CLIENT_DLL
// Script TrySelectOffhand(6/7) cannot poke a listen-server command byte.
// Arm a one-shot; the dispatcher entry hook applies it after usercmd write.
void OffhandSlotsExt_RequestSelect(void* pPlayer, int slot);
void OffhandSlotsExt_ApplyPendingSelect(void* pPlayer);

// Ends the button-hold window for an extended slot once its toss has fired.
// The failsafe restore deadline is left running.
void OffhandSlotsExt_EndSelectHold(void* pPlayer);

// Toss completed; end the button hold AND disarm the failsafe restore so the
// unwind never runs on a successful toss.
void OffhandSlotsExt_EndTossWindow(void* pPlayer);
#endif // !CLIENT_DLL

// Recover the owning player from the address of offhandWeapons[0] -- the object the
// engine hands a value proxy for this array. Returns null if pArrayBase is null.
void* OffhandSlotsExt_PlayerFromOffhandArrayBase(void* pArrayBase);

// Map a wire prop offset (relative to the array base) to a slot id, or -1 if the
// offset is not one of the extended slots.
int OffhandSlotsExt_SlotFromPropOffset(int propOffset);

#endif // OFFHAND_SLOTS_EXT_H
