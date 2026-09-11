//=============================================================================//
//
// Purpose: Expand the ConCommand script callback pool.
//
// See concommandcallback.h for design overview.
//
//=============================================================================//

#include "core/stdafx.h"
#include "game/client/concommandcallback.h"

//-----------------------------------------------------------------------------
// Resolved addresses
//-----------------------------------------------------------------------------
static uintptr_t* s_pFreeListHead = nullptr; // s_conCommandScriptCallbackFreeListHead

//-----------------------------------------------------------------------------
// Track dynamically allocated blocks so we can free them on detach.
//-----------------------------------------------------------------------------
static constexpr int MAX_GROW_BLOCKS = 32;
static void* s_growBlocks[MAX_GROW_BLOCKS];
static int s_numGrowBlocks = 0;

//-----------------------------------------------------------------------------
// Purpose: Allocate a new batch of callback nodes and prepend them to the
// free list. Each node is 40 bytes with a doubly-linked list at
// offsets 0x18 (prev) and 0x20 (next).
//-----------------------------------------------------------------------------
static void GrowConCommandCallbackPool()
{
	if (s_numGrowBlocks >= MAX_GROW_BLOCKS)
	{
		Warning(eDLL_T::ENGINE,
			"ConCommandCallback: Exceeded maximum grow block count (%d)\n",
			MAX_GROW_BLOCKS);
		return;
	}

	const size_t blockSize =
		static_cast<size_t>(CONCOMMAND_CALLBACK_GROW_COUNT) * CONCOMMAND_CALLBACK_NODE_SIZE;

	uint8_t* pBlock = static_cast<uint8_t*>(
		VirtualAlloc(nullptr, blockSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));

	if (!pBlock)
	{
		Warning(eDLL_T::ENGINE,
			"ConCommandCallback: VirtualAlloc failed for %zu bytes\n", blockSize);
		return;
	}

	s_growBlocks[s_numGrowBlocks++] = pBlock;
	memset(pBlock, 0, blockSize);

	// Build the free list from the new block.
	// Prepend each node to the existing free list head.
	uintptr_t head = *s_pFreeListHead;

	for (int i = 0; i < CONCOMMAND_CALLBACK_GROW_COUNT; i++)
	{
		uint8_t* pNode = pBlock + (i * CONCOMMAND_CALLBACK_NODE_SIZE);

		// node->prev = NULL (already zeroed)
		// node->next = current head
		*reinterpret_cast<uintptr_t*>(pNode + 0x20) = head;

		// old head->prev = new node
		if (head)
			*reinterpret_cast<uintptr_t*>(head + 0x18) = reinterpret_cast<uintptr_t>(pNode);

		head = reinterpret_cast<uintptr_t>(pNode);
	}

	*s_pFreeListHead = head;

	DevMsg(eDLL_T::ENGINE,
		"ConCommandCallback: Grew pool by %d entries (block %d)\n",
		CONCOMMAND_CALLBACK_GROW_COUNT, s_numGrowBlocks);
}

//-----------------------------------------------------------------------------
// Hook: RegisterConCommandTriggeredCallback
//
// Before the original function executes, check if the free list is exhausted.
// If so, grow the pool dynamically to prevent the "Can not register more
// than 50 ConCommand callbacks" script error.
//-----------------------------------------------------------------------------
__int64 h_RegisterConCommandTriggeredCallback(__int64 sqvm)
{
	if (s_pFreeListHead && !*s_pFreeListHead)
		GrowConCommandCallbackPool();

	return v_RegisterConCommandTriggeredCallback(sqvm);
}

//-----------------------------------------------------------------------------
// IDetour: log resolved addresses
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// IDetour: find functions via pattern scan
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// IDetour: resolve variable addresses
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// IDetour: set up hooks
//-----------------------------------------------------------------------------
