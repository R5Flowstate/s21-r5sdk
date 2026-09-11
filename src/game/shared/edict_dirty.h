//=============================================================================//
//
// Purpose: Mark an entity's edict dirty so the DT encoder re-encodes it next tick.
//
//=============================================================================//
#ifndef EDICT_DIRTY_H
#define EDICT_DIRTY_H

#include "public/edict.h"
#include <cstdint>
#include <intrin.h>

extern CGlobalVars* gpGlobals;

inline void MarkEntityEdictDirty(void* entity)
{
	if (!entity || !gpGlobals || !gpGlobals->m_pEdicts) return;

	int16_t edictIdx = *reinterpret_cast<int16_t*>(
		reinterpret_cast<uintptr_t>(entity) + 0x58);
	if (edictIdx == -1) return;

	_InterlockedOr16(
		(SHORT*)gpGlobals->m_pEdicts + edictIdx + 32,
		0x200);
}

#endif // EDICT_DIRTY_H
