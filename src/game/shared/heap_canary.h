//=============================================================================
//
// Purpose: heap tail canaries for SDK buffers the engine writes into.
//
//=============================================================================
#ifndef HEAP_CANARY_H
#define HEAP_CANARY_H

#include <cstdint>
#include <cstddef>

namespace HeapCanary
{
	// Callers must over-allocate by kTailBytes. Page-multiple VirtualAlloc has zero slack -- grow the reservation.
	static constexpr size_t kTailBytes = 64;

	// 8-byte pattern repeated kTailBytes/8 times. Distinctive value chosen so
	// that any non-zero stomp is obvious in a hex dump.
	static constexpr uint64_t kTailPattern = 0xBADDC0DEFEEDFACEull;

	// Writes kTailPattern at [bufStart+bufBytes, +kTailBytes). Caller must have over-allocated.
	void RegisterTail(const char* name, void* bufStart, size_t bufBytes);

	// Drop every tracked entry whose origBase equals bufStart. No-op when
	// dormant or the pointer is unknown. Required before free/realloc of a
	// long-lived buffer that can grow.
	void Unregister(void* bufStart);

	// Fill [regionStart, regionStart+regionBytes) when the canary sits inside a structure.
	void RegisterRegion(const char* name, void* regionStart, size_t regionBytes);

	// True only when sdk_heap_canary is 1. Callers must not emit [CANARY] lines when false.
	bool Armed(void);

	// Scan every registered tail; log a [CANARY] STOMPED warning per stomped
	// entry, including the offset and observed-vs-expected bytes. Returns the
	// count of stomped entries.
	int CheckAll();

	// Convenience: logs a header line + CheckAll summary. Wired to the
	// sdk_canary_check ConCommand.
	void DumpAll();

	// Cheap first-qword poll. First stomp fires DumpAll once, then stays silent.
	// sdk_canary_period > 0 also Checkpoints every N frames.
	void PollTick();

	// Full canary + mspace tree-bin walk. Unlatched; tag with phase.
	bool Checkpoint(const char* phase);

	// Resolve mspace_malloc and the mspace pointer. PollTick walks treebins[0..31].
	void InitMspaceMonitor();
}

#endif // HEAP_CANARY_H
