//=============================================================================//
//
// Purpose: Seqlock sidecar for DT_Player / DT_BCC appends. Snapshot pack
// workers read; the game thread writes.
//
//=============================================================================//
#include "core/stdafx.h"
#include "game/shared/player_extend_sidecar.h"
#include "game/shared/sdk_entity_state.h"
#include "tier0/dbg.h"
#include <cstring>


struct PlayerExtendSlot
{
	volatile uint64_t handleKey;
	volatile LONG seq;
	PlayerExtendBundle bundle;
};

static PlayerExtendSlot s_slots[128];
static LONG s_cursor = 0;
static volatile LONG s_used = 0;

static inline uint64_t PackHandle(const SDKEntityHandle& h)
{
	return h.IsValid() ? static_cast<uint64_t>(h.Raw()) : 0;
}

static PlayerExtendSlot* FindSlot(uint64_t key)
{
	if (!key || !s_used)
		return nullptr;
	for (PlayerExtendSlot& slot : s_slots)
	{
		if (slot.handleKey == key)
			return &slot;
	}
	return nullptr;
}

static PlayerExtendSlot* EnsureSlot(uint64_t key)
{
	if (!key)
		return nullptr;

	PlayerExtendSlot* slot = FindSlot(key);
	if (slot)
		return slot;

	for (PlayerExtendSlot& cand : s_slots)
	{
		if (cand.handleKey == 0)
		{
			slot = &cand;
			break;
		}
	}
	if (slot)
		InterlockedIncrement(&s_used);
	else
	{
		const LONG idx = (InterlockedIncrement(&s_cursor) - 1) & 127;
		slot = &s_slots[idx];
	}
	slot->handleKey = 0;
	InterlockedIncrement(&slot->seq);
	memset(&slot->bundle, 0, sizeof(slot->bundle));
	InterlockedIncrement(&slot->seq);
	slot->handleKey = key;
	return slot;
}

static volatile LONG s_nPackReads   = 0;
static volatile LONG s_nPackNoSlot  = 0;
static volatile LONG s_nPackSeqFail = 0;

static bool CopyBundle(const void* pEntity, PlayerExtendBundle* pOut, volatile LONG* pNoSlot = nullptr,
	volatile LONG* pSeqFail = nullptr)
{
	if (!pEntity || !pOut)
		return false;

	const uint64_t key = PackHandle(SDKEntityState_GetHandle(pEntity));
	PlayerExtendSlot* const slot = FindSlot(key);
	if (!slot)
	{
		if (pNoSlot)
			InterlockedIncrement(pNoSlot);
		return false;
	}

	for (int attempt = 0; attempt < 8; ++attempt)
	{
		const LONG before = slot->seq;
		if (before & 1)
			continue;
		const PlayerExtendBundle copy = slot->bundle;
		MemoryBarrier();
		if (slot->seq == before && slot->handleKey == key)
		{
			*pOut = copy;
			return true;
		}
	}
	if (pSeqFail)
		InterlockedIncrement(pSeqFail);
	return false;
}

bool PlayerExtend_GetBundle(const void* pEntity, PlayerExtendBundle* pOut)
{
	InterlockedIncrement(&s_nPackReads);
	return CopyBundle(pEntity, pOut, &s_nPackNoSlot, &s_nPackSeqFail);
}

void PlayerExtend_GetPackReadStats(uint32_t* pReads, uint32_t* pNoSlot, uint32_t* pSeqFail)
{
	if (pReads)   *pReads   = static_cast<uint32_t>(s_nPackReads);
	if (pNoSlot)  *pNoSlot  = static_cast<uint32_t>(s_nPackNoSlot);
	if (pSeqFail) *pSeqFail = static_cast<uint32_t>(s_nPackSeqFail);
}

void PlayerExtend_LevelShutdown(void)
{
	InterlockedExchange(&s_used, 0);
	memset(s_slots, 0, sizeof(s_slots));
	InterlockedExchange(&s_cursor, 0);
}

static PlayerExtendSlot* BeginWrite(void* pEntity, PlayerExtendBundle* pScratch)
{
	if (!pEntity || !pScratch)
		return nullptr;
	const uint64_t key = PackHandle(SDKEntityState_GetHandle(pEntity));
	PlayerExtendSlot* const slot = EnsureSlot(key);
	if (!slot)
		return nullptr;
	if (!CopyBundle(pEntity, pScratch))
		memset(pScratch, 0, sizeof(*pScratch));
	return slot;
}

static void CommitWrite(PlayerExtendSlot* slot, uint64_t key, const PlayerExtendBundle& bundle)
{
	InterlockedIncrement(&slot->seq);
	slot->bundle = bundle;
	InterlockedIncrement(&slot->seq);
	slot->handleKey = key;
}

void PlayerExtend_SetI32(void* pEntity, size_t fieldOff, int32_t value)
{
	PlayerExtendBundle scratch;
	PlayerExtendSlot* const slot = BeginWrite(pEntity, &scratch);
	if (!slot)
		return;
	*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&scratch.player) + fieldOff) = value;
	CommitWrite(slot, PackHandle(SDKEntityState_GetHandle(pEntity)), scratch);
}

void PlayerExtend_SetF32(void* pEntity, size_t fieldOff, float value)
{
	PlayerExtendBundle scratch;
	PlayerExtendSlot* const slot = BeginWrite(pEntity, &scratch);
	if (!slot)
		return;
	*reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(&scratch.player) + fieldOff) = value;
	CommitWrite(slot, PackHandle(SDKEntityState_GetHandle(pEntity)), scratch);
}

void PlayerExtend_SetI64(void* pEntity, size_t fieldOff, int64_t value)
{
	PlayerExtendBundle scratch;
	PlayerExtendSlot* const slot = BeginWrite(pEntity, &scratch);
	if (!slot)
		return;
	*reinterpret_cast<int64_t*>(reinterpret_cast<uint8_t*>(&scratch.player) + fieldOff) = value;
	CommitWrite(slot, PackHandle(SDKEntityState_GetHandle(pEntity)), scratch);
}

void PlayerExtend_SetVec(void* pEntity, size_t fieldOff, const float xyz[3])
{
	if (!xyz)
		return;
	PlayerExtendBundle scratch;
	PlayerExtendSlot* const slot = BeginWrite(pEntity, &scratch);
	if (!slot)
		return;
	float* const dst = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(&scratch.player) + fieldOff);
	dst[0] = xyz[0];
	dst[1] = xyz[1];
	dst[2] = xyz[2];
	CommitWrite(slot, PackHandle(SDKEntityState_GetHandle(pEntity)), scratch);
}

int32_t PlayerExtend_GetI32(const void* pEntity, size_t fieldOff)
{
	PlayerExtendBundle bundle;
	if (!CopyBundle(pEntity, &bundle))
		return 0;
	return *reinterpret_cast<const int32_t*>(
		reinterpret_cast<const uint8_t*>(&bundle.player) + fieldOff);
}

float PlayerExtend_GetF32(const void* pEntity, size_t fieldOff)
{
	PlayerExtendBundle bundle;
	if (!CopyBundle(pEntity, &bundle))
		return 0.0f;
	return *reinterpret_cast<const float*>(
		reinterpret_cast<const uint8_t*>(&bundle.player) + fieldOff);
}

void BCCExtend_SetI32(void* pEntity, size_t fieldOff, int32_t value)
{
	PlayerExtendBundle scratch;
	PlayerExtendSlot* const slot = BeginWrite(pEntity, &scratch);
	if (!slot)
		return;
	*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&scratch.bcc) + fieldOff) = value;
	CommitWrite(slot, PackHandle(SDKEntityState_GetHandle(pEntity)), scratch);
}

void BCCExtend_SetF32(void* pEntity, size_t fieldOff, float value)
{
	PlayerExtendBundle scratch;
	PlayerExtendSlot* const slot = BeginWrite(pEntity, &scratch);
	if (!slot)
		return;
	*reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(&scratch.bcc) + fieldOff) = value;
	CommitWrite(slot, PackHandle(SDKEntityState_GetHandle(pEntity)), scratch);
}

int32_t BCCExtend_GetI32(const void* pEntity, size_t fieldOff)
{
	PlayerExtendBundle bundle;
	if (!CopyBundle(pEntity, &bundle))
		return 0;
	return *reinterpret_cast<const int32_t*>(
		reinterpret_cast<const uint8_t*>(&bundle.bcc) + fieldOff);
}

float BCCExtend_GetF32(const void* pEntity, size_t fieldOff)
{
	PlayerExtendBundle bundle;
	if (!CopyBundle(pEntity, &bundle))
		return 0.0f;
	return *reinterpret_cast<const float*>(
		reinterpret_cast<const uint8_t*>(&bundle.bcc) + fieldOff);
}

