//=============================================================================//
//
// Purpose: Prevent fatal error when visible object limit is reached.
//
// CClientLeafSystem allocates visible object handles from a fixed bitfield
// (128 chunks x 64 bits = 8192); exhaustion raises a fatal error. This module
// patches that path to fail gracefully, reserves handle 8191 as an overflow
// handle shared by entities past the cap (minor visual glitches, no crash),
// and adds budget warnings approaching the limit.
//
//=============================================================================//

#ifndef GAME_CLIENTLEAFSYSTEM_H
#define GAME_CLIENTLEAFSYSTEM_H

#include "thirdparty/detours/include/idetour.h"

//-----------------------------------------------------------------------------
// Visible object limits
//-----------------------------------------------------------------------------
inline constexpr int VISIBLE_OBJECTS_CHUNK_COUNT = 128;
inline constexpr int VISIBLE_OBJECTS_MAX = VISIBLE_OBJECTS_CHUNK_COUNT * 64; // 8192

// Budget threshold - warn when exceeding this count.
inline constexpr int VISIBLE_OBJECTS_BUDGET = 7168;

// Overflow handle: the last valid index, reserved for entities that can't
// get a real handle. Multiple overflow entities share this slot.
inline constexpr unsigned short VISIBLE_OBJECTS_OVERFLOW_HANDLE =
	static_cast<unsigned short>(VISIBLE_OBJECTS_MAX - 1); // 8191

// Overflow handle's chunk and bit position in the IsInUse bitfield.
inline constexpr int OVERFLOW_CHUNK = VISIBLE_OBJECTS_OVERFLOW_HANDLE >> 6;  // 127
inline constexpr int OVERFLOW_BIT   = VISIBLE_OBJECTS_OVERFLOW_HANDLE & 63;  // 63

// IsInUse bitfield offset from struct base (array of uint64[128]).
inline constexpr int CLIENTLEAF_ISINUSE_OFFSET = 0x38;

//-----------------------------------------------------------------------------
// Original function pointers (resolved in GetFun)
//-----------------------------------------------------------------------------
inline unsigned short(*v_CClientLeafSystem_AllocVisibleObject)();
inline void(*v_CClientLeafSystem_FreeVisibleObject)(__int64 a1, unsigned short handle);

//-----------------------------------------------------------------------------
// Hook functions
//-----------------------------------------------------------------------------
unsigned short h_CClientLeafSystem_AllocVisibleObject();
void h_CClientLeafSystem_FreeVisibleObject(__int64 a1, unsigned short handle);

//-----------------------------------------------------------------------------
// Public accessors (for VScript bindings, etc.)
//-----------------------------------------------------------------------------
int  ClientLeafSystem_GetVisibleObjectCount();
int  ClientLeafSystem_GetVisibleObjectBudget();
int  ClientLeafSystem_GetVisibleObjectMax();
bool ClientLeafSystem_IsOverflowing();


#endif // GAME_CLIENTLEAFSYSTEM_H
