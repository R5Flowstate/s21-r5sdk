//=============================================================================//
//
// Purpose: C2S ScriptRemote + chat reliability (seq + N-shot + dedi drop-dup).
//
//=============================================================================//
#include "core/stdafx.h"
#include "engine/client/net_bridge_internal.h"
#include "engine/net.h"
#include "engine/net_chan.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "engine/client/net_bridge_split.h"
#include "tier1/bitbuf.h"
#include "tier1/cvar.h"
#include "tier1/strtools.h"

static ConVar bridge_c2s_rel_seq("bridge_c2s_rel_seq", "1", FCVAR_RELEASE,
	"Stamp a sequence on relayed ScriptRemote (reserved field) and chat (brc prefix) "
	"and resend; the dedi drop-dups. 0 = single unreliable send.");
static ConVar bridge_c2s_rel_redundancy("bridge_c2s_rel_redundancy", "2", FCVAR_RELEASE,
	"Extra redundant sends per relayed ScriptRemote/chat.", true, 0.f, true, 7.f);
static ConVar bridge_c2s_rel_resend_ms("bridge_c2s_rel_resend_ms", "50", FCVAR_RELEASE,
	"Spacing in milliseconds between redundant ScriptRemote/chat sends.",
	true, 10.f, true, 1000.f);
static ConVar bridge_c2s_rel_log("bridge_c2s_rel_log", "0", FCVAR_DEVELOPMENTONLY,
	"[C2S-REL] log first/every-Nth ScriptRemote and chat seq sends.");

static constexpr int kC2SRelRing = 32;
static constexpr int kC2SRelMaxBytes = 1024;

struct C2SRelSlot_s
{
	uint8_t  data[kC2SRelMaxBytes];
	int      nBits;
	int      sendsLeft;
	double   nextDueMs;
	bool     active;
};

static uint32_t     s_c2sRelSeq = 0;
static int          s_c2sRelHead = 0;
static C2SRelSlot_s s_c2sRelRing[kC2SRelRing];

static void C2SRel_Announce(void)
{
	static bool s_bAnnounced = false;
	if (s_bAnnounced)
		return;
	s_bAnnounced = true;
	Warning(eDLL_T::ENGINE,
		"[C2S-REL] seq lane ACTIVE (redundancy=%d spacing=%dms)\n",
		bridge_c2s_rel_redundancy.GetInt(),
		bridge_c2s_rel_resend_ms.GetInt());
}

static bool C2SRel_CommitBits(const uint8_t* pData, int nBits)
{
	if (nBits <= 0 || nBits > (kC2SRelMaxBytes * 8))
		return false;
	if (s_c2sPend.GetNumBitsLeft() < nBits + 8)
		return false;

	bf_read rb(pData, (nBits + 7) >> 3, nBits);
	s_c2sPend.WriteBitsFromBuffer(&rb, nBits);
	return !s_c2sPend.IsOverflowed();
}

static bool C2SRel_StoreAndSend(const uint8_t* pData, int nBits)
{
	if (nBits <= 0 || ((nBits + 7) >> 3) > kC2SRelMaxBytes)
		return false;

	AcquireSRWLockExclusive(&s_c2sTxLock);
	if (!C2SRel_CommitBits(pData, nBits))
	{
		ReleaseSRWLockExclusive(&s_c2sTxLock);
		return false;
	}

	C2SRelSlot_s& slot = s_c2sRelRing[s_c2sRelHead];
	if (slot.active && slot.sendsLeft > 0)
	{
		static long long s_drop = 0;
		if ((++s_drop % 64) == 1)
			Warning(eDLL_T::ENGINE, "[C2S-REL] ring full, dropping oldest (#%lld)\n", s_drop);
	}
	memset(slot.data, 0, sizeof(slot.data));
	memcpy(slot.data, pData, (nBits + 7) >> 3);
	slot.nBits = nBits;
	slot.sendsLeft = bridge_c2s_rel_redundancy.GetInt();
	slot.nextDueMs = static_cast<double>(GetTickCount64())
		+ static_cast<double>(bridge_c2s_rel_resend_ms.GetFloat());
	slot.active = true;
	s_c2sRelHead = (s_c2sRelHead + 1) % kC2SRelRing;
	ReleaseSRWLockExclusive(&s_c2sTxLock);

	C2SRel_Announce();
	return true;
}

void S21Bridge_C2SRel_PumpLocked(void)
{
	if (!bridge_c2s_rel_seq.GetBool())
		return;

	const double flNow = static_cast<double>(GetTickCount64());
	const double flSpacing = static_cast<double>(bridge_c2s_rel_resend_ms.GetFloat());

	for (int i = 0; i < kC2SRelRing; ++i)
	{
		C2SRelSlot_s& e = s_c2sRelRing[i];
		if (!e.active || e.sendsLeft <= 0 || flNow < e.nextDueMs)
			continue;
		if (!C2SRel_CommitBits(e.data, e.nBits))
			continue;
		--e.sendsLeft;
		e.nextDueMs = flNow + flSpacing;
		if (e.sendsLeft <= 0)
			e.active = false;
	}
}

void S21Bridge_C2SRel_Reset(void)
{
	AcquireSRWLockExclusive(&s_c2sTxLock);
	s_c2sRelSeq = 0;
	s_c2sRelHead = 0;
	for (int i = 0; i < kC2SRelRing; ++i)
	{
		s_c2sRelRing[i].active = false;
		s_c2sRelRing[i].sendsLeft = 0;
		s_c2sRelRing[i].nBits = 0;
	}
	ReleaseSRWLockExclusive(&s_c2sTxLock);
}

bool S21Bridge_C2SRel_RelayScriptRemote(const uint8_t* d, int sliceBytes, int b0, int b1)
{
	if (!bridge_c2s_rel_seq.GetBool() || !d || sliceBytes <= 0 || (b1 - b0) < 66)
		return false;

	bf_read r(d, sliceBytes, b1);
	if (!r.Seek(b0) || r.ReadUBitLong(7) != 4)
		return false;

	const unsigned flag = r.ReadOneBit();
	const unsigned payloadByteLen = r.ReadUBitLong(15);
	const unsigned sign = r.ReadOneBit();
	const unsigned funcIndex = r.ReadUBitLong(10);
	r.ReadUBitLong(31);
	r.ReadOneBit();
	if (r.IsOverflowed())
		return false;

	const int argBits = b1 - static_cast<int>(r.GetNumBitsRead());
	if (argBits < 0)
		return false;

	const uint32_t nSeq = ++s_c2sRelSeq;
	uint8_t scratch[kC2SRelMaxBytes];
	memset(scratch, 0, sizeof(scratch));
	bf_write w("c2srel_sr", scratch, sizeof(scratch));
	w.WriteUBitLong(68, 7);
	w.WriteOneBit(static_cast<int>(flag));
	w.WriteUBitLong(payloadByteLen, 15);
	w.WriteOneBit(static_cast<int>(sign));
	w.WriteUBitLong(funcIndex, 10);
	w.WriteUBitLong(nSeq, 31);
	w.WriteOneBit(0);
	if (argBits > 0)
		w.WriteBitsFromBuffer(&r, argBits);
	if (w.IsOverflowed())
		return false;

	if (bridge_c2s_rel_log.GetBool())
	{
		static long long s_n = 0;
		if (++s_n <= 16 || (s_n % 128) == 0)
			Warning(eDLL_T::ENGINE,
				"[C2S-REL] scriptremote seq=%u funcIndex=%u bits=%d\n",
				nSeq, funcIndex, w.GetNumBitsWritten());
	}

	return C2SRel_StoreAndSend(scratch, w.GetNumBitsWritten());
}

bool S21Bridge_C2SRel_RelayChat(const uint8_t* d, int sliceBytes, int b0, int b1)
{
	if (!bridge_c2s_rel_seq.GetBool() || !d || sliceBytes <= 0 || (b1 - b0) < 8)
		return false;

	bf_read r(d, sliceBytes, b1);
	if (!r.Seek(b0 + 7))
		return false;

	char szOrig[256];
	szOrig[0] = '\0';
	if (!r.ReadString(szOrig, sizeof(szOrig)))
		return false;
	const bool bTeam = r.ReadOneBit() != 0;

	const int nLen = static_cast<int>(strlen(szOrig));
	if (nLen <= 0 || nLen + 16 >= static_cast<int>(sizeof(szOrig)))
		return false;

	const uint32_t nSeq = ++s_c2sRelSeq;
	char szPrefixed[256];
	const int nWrote = V_snprintf(szPrefixed, sizeof(szPrefixed), "brc %u %s", nSeq, szOrig);
	if (nWrote <= 0 || nWrote >= static_cast<int>(sizeof(szPrefixed)))
		return false;

	uint8_t scratch[kC2SRelMaxBytes];
	memset(scratch, 0, sizeof(scratch));
	bf_write w("c2srel_chat", scratch, sizeof(scratch));
	w.WriteUBitLong(61, 7);
	w.WriteString(szPrefixed);
	w.WriteOneBit(bTeam ? 1 : 0);
	if (w.IsOverflowed())
		return false;

	if (bridge_c2s_rel_log.GetBool())
	{
		static long long s_n = 0;
		if (++s_n <= 16 || (s_n % 128) == 0)
			Warning(eDLL_T::ENGINE, "[C2S-REL] chat seq=%u bits=%d\n", nSeq, w.GetNumBitsWritten());
	}

	return C2SRel_StoreAndSend(scratch, w.GetNumBitsWritten());
}
