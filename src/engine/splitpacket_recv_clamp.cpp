//=============================================================================//
//
// Purpose: Clamp native split-packet reassembly to the LONGPACKET buffer.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier1/convar.h"
#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/detours.h"
#include "engine/splitpacket_recv_clamp.h"

#include <cstdint>

static char (*v_NET_DoWeHaveAllParts)(int sock, void* packet) = nullptr;

static ConVar sdk_splitpacket_recv_clamp("sdk_splitpacket_recv_clamp", "1",
	FCVAR_RELEASE,
	"Drop split fragments whose index*splitSize misses the reassembly buffer. 0 = stock (OOB).");

static constexpr int kSplitHdr = 12;
static constexpr int kSplitMinFrag = 564;
static constexpr int kSplitMaxSpan = 0x270; // accepted splitSize 564..1188
static constexpr uint32_t kSplitFlag = 0xFFFFFFFEu;

#if defined(CLIENT_DLL)
static constexpr int kSplitBuf = 262176;
// netpacket_t: scratch pointer at +0x30, size at +0x78.
static constexpr int kPktScratch = 0x30;
static constexpr int kPktSize = 0x78;
#else
// The S3 entry stride is 264060 with the fragment region starting at +1896.
static constexpr int kSplitBuf = 262164;
// netpacket_t: scratch pointer at +0x28, size at +0x70.
static constexpr int kPktScratch = 0x28;
static constexpr int kPktSize = 0x70;
#endif // CLIENT_DLL

bool SplitPacketRecvClamp_Enabled(void)
{
	return sdk_splitpacket_recv_clamp.GetBool();
}

void SplitPacketRecvClamp_Drop(const char* why, int a, int b)
{
	static int s_nWarns = 0;
	if (s_nWarns >= 16)
		return;
	++s_nWarns;
	Warning(eDLL_T::ENGINE, "[SPLIT-CLAMP] drop %s a=%d b=%d\n", why, a, b);
}

bool SplitPacket_WireWouldOOB(const unsigned char* p, int n)
{
	if (!p || n < 4)
		return false;
	if (*reinterpret_cast<const uint32_t*>(p) != kSplitFlag)
		return false;
	if (n < kSplitHdr)
		return true;

	const uint8_t count = p[8];
	const uint8_t index = p[9];
	const int splitSize = static_cast<int>(*reinterpret_cast<const int16_t*>(p + 10));
	const int payload = n - kSplitHdr;

	if (count == 0)
		return true;
	// Neither engine bounds index against count; index alone reaches 255.
	if (index >= count)
		return true;
	if (static_cast<unsigned int>(splitSize - kSplitMinFrag) > static_cast<unsigned int>(kSplitMaxSpan))
		return true;
	if (payload < 0)
		return true;

	const int64_t offset = static_cast<int64_t>(index) * static_cast<int64_t>(splitSize);
	if (offset < 0 || offset > kSplitBuf)
		return true;
	if (offset + payload > kSplitBuf)
		return true;
	return false;
}

static char Hook_NET_DoWeHaveAllParts(int sock, void* packet)
{
	if (!v_NET_DoWeHaveAllParts)
		return 0;

	if (sdk_splitpacket_recv_clamp.GetBool() && packet)
	{
		const int size = *reinterpret_cast<const int*>(
			reinterpret_cast<const char*>(packet) + kPktSize);
		unsigned char* const scratch = *reinterpret_cast<unsigned char**>(
			reinterpret_cast<char*>(packet) + kPktScratch);
		if (scratch && SplitPacket_WireWouldOOB(scratch, size))
		{
			const int index = (size >= 10) ? scratch[9] : -1;
			const int splitSize = (size >= 12)
				? static_cast<int>(*reinterpret_cast<const int16_t*>(scratch + 10)) : -1;
			SplitPacketRecvClamp_Drop("native", index, splitSize);
			return 0;
		}
	}

	return v_NET_DoWeHaveAllParts(sock, packet);
}

void VSplitPacketRecvClamp::GetAdr(void) const
{
	LogFunAdr("NET_DoWeHaveAllPartsOfThisMessage", v_NET_DoWeHaveAllParts);
}

void VSplitPacketRecvClamp::GetFun(void) const
{
#if defined(CLIENT_DLL)
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 18 57 48 83 EC 20 83 7A 78 0C 48 8B FA")
		.GetPtr(v_NET_DoWeHaveAllParts);
#else
	// 83 7A 70 0C is the m_nSizeInBytes >= 12 gate; the dedi
	// keeps size at +0x70 where the S21 client has it at +0x78.
	Module_FindPattern(g_GameDll,
		"40 57 48 83 EC 20 83 7A 70 0C 48 8B FA 7D 08 32 C0")
		.GetPtr(v_NET_DoWeHaveAllParts);
#endif // CLIENT_DLL

	if (!v_NET_DoWeHaveAllParts)
		Warning(eDLL_T::ENGINE, "[SPLIT-CLAMP] NET_DoWeHaveAllParts pattern unresolved\n");
}

void VSplitPacketRecvClamp::Detour(const bool bAttach) const
{
	if (v_NET_DoWeHaveAllParts)
		DetourSetup(&v_NET_DoWeHaveAllParts, &Hook_NET_DoWeHaveAllParts, bAttach);
}
