//=============================================================================//
//
// Purpose: Clamp ProcessDataBlock writes to the real 768KB scratch allocation.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier1/convar.h"
#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/detours.h"
#include "engine/shared/datablock.h"
#include "datablock_recv_clamp.h"

#include <cstdint>

static bool (*v_ProcessDataBlock)(void* thisptr, const double startTime,
	const short transferId, const int transferSize, const short transferNr,
	const short currentBlockId, const void* const blockBuffer,
	const int blockBufferBytes) = nullptr;

static ConVar sdk_datablock_recv_clamp("sdk_datablock_recv_clamp", "1",
	FCVAR_RELEASE,
	"Clamp DataBlock fragment writes to the 768KB scratch. 0 = stock (OOB).");

static void DataBlockRecvClamp_Warn(const char* why, int a, int b)
{
	static int s_nWarns = 0;
	if (s_nWarns >= 16)
		return;
	++s_nWarns;
	Warning(eDLL_T::ENGINE, "[DATABLOCK-CLAMP] drop %s a=%d b=%d\n", why, a, b);
}

static bool Hook_ProcessDataBlock(void* thisptr, const double startTime,
	const short transferId, const int transferSize, const short transferNr,
	const short currentBlockId, const void* const blockBuffer,
	const int blockBufferBytes)
{
	if (!v_ProcessDataBlock)
		return false;

	if (!sdk_datablock_recv_clamp.GetBool())
	{
		return v_ProcessDataBlock(thisptr, startTime, transferId, transferSize,
			transferNr, currentBlockId, blockBuffer, blockBufferBytes);
	}

	if (transferSize < 1 || transferSize > MAX_DATABLOCK_TRANSFER_SIZE)
	{
		DataBlockRecvClamp_Warn("transferSize", transferSize, MAX_DATABLOCK_TRANSFER_SIZE);
		return false;
	}

	if (currentBlockId < 0 || currentBlockId >= MAX_DATABLOCK_FRAGMENTS)
	{
		DataBlockRecvClamp_Warn("blockId", currentBlockId, MAX_DATABLOCK_FRAGMENTS);
		return false;
	}

	if (blockBufferBytes < 1 || blockBufferBytes > MAX_DATABLOCK_FRAGMENT_SIZE || !blockBuffer)
	{
		DataBlockRecvClamp_Warn("fragBytes", blockBufferBytes, MAX_DATABLOCK_FRAGMENT_SIZE);
		return false;
	}

	const int scratchOffset = currentBlockId * MAX_DATABLOCK_FRAGMENT_SIZE;
	if (scratchOffset + blockBufferBytes > transferSize)
	{
		DataBlockRecvClamp_Warn("fragEnd", scratchOffset + blockBufferBytes, transferSize);
		return false;
	}

	return v_ProcessDataBlock(thisptr, startTime, transferId, transferSize,
		transferNr, currentBlockId, blockBuffer, blockBufferBytes);
}

void VDataBlockRecvClamp::GetAdr(void) const
{
	LogFunAdr("ClientDataBlockReceiver::ProcessDataBlock", v_ProcessDataBlock);
}

void VDataBlockRecvClamp::GetFun(void) const
{
	// Unique on r5apex_ds: movzx eax, word [rsp+60h] is transferId.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? "
		"48 89 7C 24 ?? 41 56 48 83 EC ?? 0F B7 44 24 ??")
		.GetPtr(v_ProcessDataBlock);

	if (!v_ProcessDataBlock)
		Warning(eDLL_T::ENGINE, "[DATABLOCK-CLAMP] ProcessDataBlock pattern unresolved\n");
}

void VDataBlockRecvClamp::Detour(const bool bAttach) const
{
	if (v_ProcessDataBlock)
		DetourSetup(&v_ProcessDataBlock, &Hook_ProcessDataBlock, bAttach);
}
