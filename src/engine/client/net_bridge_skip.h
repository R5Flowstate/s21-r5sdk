//=============================================================================//
//
// Purpose: Engine-free signon skip-width core for non-SendTable messages.
// Same widths the inject path walks; verdict-only (true = skippable, cursor
// advanced past the body). Logging and fail-open/break policy stay with the
// caller. Shared with the wire tests, which execute this exact code.
//
//=============================================================================//
#ifndef NET_BRIDGE_SKIP_H
#define NET_BRIDGE_SKIP_H

#include <cstdint>

// CreateStringTable entry-count contract: negative counts are corrupt;
// counts past the table maximum overrun the row array. A full table
// (numEntries == maxEntries) is legit and must pass.
inline bool S21Bridge_CstCountInvalid(int nNumEntries, int nMaxEntries)
{
	return nNumEntries < 0 || nNumEntries > nMaxEntries;
}

inline uint32_t S21Bridge_Skip_PeekUBits(const uint8_t* pData, int nTotalBits, int nBitPos, int nBits)
{
	uint32_t val = 0;
	if (nBitPos < 0 || nBits <= 0)
		return 0;
	for (int b = 0; b < nBits && (nBitPos + b) < nTotalBits; b++)
	{
		const int bp = nBitPos + b;
		if (bp < 0)
			return 0;
		if (pData[bp / 8] & (1 << (bp % 8)))
			val |= (1u << b);
	}
	return val;
}

inline uint32_t S21Bridge_Skip_ReadUBits(const uint8_t* pData, int nTotalBits, int& nBitPos, int nBits)
{
	const uint32_t val = S21Bridge_Skip_PeekUBits(pData, nTotalBits, nBitPos, nBits);
	nBitPos += nBits;
	return val;
}

inline bool S21Bridge_Skip_CappedString(const uint8_t* pData, int nTotalBits, int& nBitPos, int nMaxLen)
{
	for (int i = 0; i < nMaxLen; ++i)
	{
		if (nBitPos + 8 > nTotalBits)
			return false;
		const uint32_t ch = S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 8);
		if (ch == 0)
			return true;
	}
	return true;
}

inline bool S21Bridge_Skip_CappedStringStrict(const uint8_t* pData, int nTotalBits, int& nBitPos, int nMaxLen)
{
	for (int i = 0; i < nMaxLen; ++i)
	{
		if (nBitPos + 8 > nTotalBits)
			return false;
		if (S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 8) == 0)
			return true;
	}
	return false;
}

inline bool S21Bridge_SkipNonSendTable(const uint8_t* pData, int nTotalBits, int& nBitPos, uint32_t nMsgType)
{
	switch (nMsgType)
	{
	case 3:
		// net_StringCmd: one NUL-terminated string (S21 RFB reads
		// string(1024); same-family generic path). NUL required.
		return S21Bridge_Skip_CappedStringStrict(pData, nTotalBits, nBitPos, 1024);
	case 4:
	{
		if (nBitPos + 8 > nTotalBits)
			return false;
		const uint32_t pairCount = S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 8);
		for (uint32_t i = 0; i < pairCount; ++i)
		{
			if (!S21Bridge_Skip_CappedString(pData, nTotalBits, nBitPos, 256)
				|| !S21Bridge_Skip_CappedString(pData, nTotalBits, nBitPos, 256))
				return false;
		}
		return true;
	}
	case 5:
	{
		if (nBitPos + 40 > nTotalBits)
			return false;
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 8);
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 32);
		if (!S21Bridge_Skip_CappedString(pData, nTotalBits, nBitPos, 256)
			|| !S21Bridge_Skip_CappedString(pData, nTotalBits, nBitPos, 256))
			return false;
		if (nBitPos + 64 > nTotalBits)
			return false;
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 32);
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 32);
		return S21Bridge_Skip_CappedString(pData, nTotalBits, nBitPos, 256);
	}
	case 6:
	{
		if (nBitPos + 32 > nTotalBits)
			return false;
		const uint32_t nBits = S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 32);
		if (nBits > (uint32_t)(nTotalBits - nBitPos) || nBits > (1u << 20))
			return false;
		nBitPos += (int)nBits;
		return true;
	}
	case 7:
	{
		if (nBitPos + 16 + 32 + 1 + 1 + 1 + 16 + 32 + 16 + 8 + 8 + 32 + 8 > nTotalBits)
			return false;
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 16);
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 32);
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 1);
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 1);
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 1);
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 16);
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 32);
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 16);
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 8);
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 8);
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 32);
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 8);
		for (int si = 0; si < 7; ++si)
		{
			if (!S21Bridge_Skip_CappedString(pData, nTotalBits, nBitPos, 260))
				return false;
		}
		if (!S21Bridge_Skip_CappedString(pData, nTotalBits, nBitPos, 128))
			return false;
		if (nBitPos + 8 > nTotalBits)
			return false;
		const uint32_t flagsByte = S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 8);
		if (flagsByte)
		{
			const int64_t bulk = (int64_t)64000 * 32;
			if (nBitPos + bulk > nTotalBits)
				return false;
			nBitPos += (int)bulk;
		}
		return true;
	}
	case 9:
	{
		if (nBitPos + 16 > nTotalBits)
			return false;
		const uint32_t count = S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 16);
		if (count > 4096)
			return false;
		for (uint32_t i = 0; i < count; ++i)
		{
			if (!S21Bridge_Skip_CappedString(pData, nTotalBits, nBitPos, 256)
				|| !S21Bridge_Skip_CappedString(pData, nTotalBits, nBitPos, 256))
				return false;
		}
		return true;
	}
	case 10:
		if (nBitPos + 1 > nTotalBits)
			return false;
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 1);
		return true;
	case 11:
	{
		// svc_Playlists: 1b compressed + 32b length + blob (cap 0x7D000,
		// S21 msgArray[14] RFB VERIFIED note).
		if (nBitPos + 1 + 32 > nTotalBits)
			return false;
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 1);
		const uint32_t plLen = S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 32);
		if (plLen > 0x7D000 || plLen > (uint32_t)(nTotalBits - nBitPos) / 8)
			return false;
		nBitPos += (int)(plLen * 8);
		return true;
	}
	case 12:
	{
		if (!S21Bridge_Skip_CappedString(pData, nTotalBits, nBitPos, 256))
			return false;
		if (nBitPos + 16 > nTotalBits)
			return false;
		const uint32_t cstMaxEntries = S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 16);
		if (cstMaxEntries == 0)
			return false;
		int cstEntryBits = 0;
		for (uint32_t me = cstMaxEntries >> 1; me > 0; me >>= 1)
			++cstEntryBits;
		++cstEntryBits;
		if (nBitPos + cstEntryBits + 22 + 1 + 1 + 2 > nTotalBits)
			return false;
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, cstEntryBits);
		const uint32_t cstDataLenBits = S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 22);
		const uint32_t cstHasUserData = S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 1);
		if (cstHasUserData)
		{
			if (nBitPos + 12 + 4 > nTotalBits)
				return false;
			S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 12);
			S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 4);
		}
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 1);
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 2);
		if (cstDataLenBits > (uint32_t)(nTotalBits - nBitPos))
			return false;
		nBitPos += (int)cstDataLenBits;
		return true;
	}
	case 13:
	{
		// svc_UpdateStringTable: 5b tableId + 1b count-present + [16b
		// numEntries] + 20b dataLen + dataLen-bit blob. The 16b count
		// exists only when the flag bit is set, else changedEntries = 1.
		if (nBitPos + 5 + 1 > nTotalBits)
			return false;
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 5);
		const uint32_t ustHasCount = S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 1);
		if (ustHasCount)
		{
			if (nBitPos + 16 > nTotalBits)
				return false;
			S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 16);
		}
		if (nBitPos + 20 > nTotalBits)
			return false;
		const uint32_t ustDataLen = S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 20);
		if (ustDataLen > (uint32_t)(nTotalBits - nBitPos))
			return false;
		nBitPos += (int)ustDataLen;
		return true;
	}
	case 16:
		return S21Bridge_Skip_CappedString(pData, nTotalBits, nBitPos, 2047);
	case 22:
		if (nBitPos + 137 > nTotalBits)
			return false;
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 32);
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 32);
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 16);
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 16);
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 1);
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 8);
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 32);
		return true;
	case 23:
	{
		if (nBitPos + 64 + 16 > nTotalBits)
			return false;
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 32);
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 32);
		const uint32_t pdefDataLen = S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 16);
		if (pdefDataLen > 0x28000 || nBitPos + (int)pdefDataLen * 8 > nTotalBits)
			return false;
		nBitPos += (int)pdefDataLen * 8;
		return true;
	}
	case 24:
		if (nBitPos + 64 > nTotalBits)
			return false;
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 32);
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 32);
		return true;
	case 32:
		return S21Bridge_Skip_CappedString(pData, nTotalBits, nBitPos, 64);
	case 34:
	{
		if (nBitPos + 32 > nTotalBits)
			return false;
		const int32_t lenBits = (int32_t)S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 32);
		if (lenBits <= 0 || lenBits >= 0x100000 || nBitPos + lenBits > nTotalBits)
			return false;
		nBitPos += lenBits;
		return true;
	}
	case 37:
	{
		if (nBitPos + 8 + 12 > nTotalBits)
			return false;
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 8);
		const uint32_t umLenBits = S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 12);
		if (nBitPos + (int)umLenBits > nTotalBits)
			return false;
		nBitPos += (int)umLenBits;
		return true;
	}
	case 41:
	{
		// svc_TempEntities S3 wire: 32b tick + 10b entries + 22b len +
		// len-bit blob (inject handler reads the same widths).
		if (nBitPos + 32 + 10 + 22 > nTotalBits)
			return false;
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 32);
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 10);
		const uint32_t teLenBits = S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 22);
		if (teLenBits > (uint32_t)(nTotalBits - nBitPos))
			return false;
		nBitPos += (int)teLenBits;
		return true;
	}
	case 42:
	{
		if (nBitPos + 32 > nTotalBits)
			return false;
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 16);
		const uint32_t lenBytes = S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 16);
		if (lenBytes > 4096 || nBitPos + (int)lenBytes * 8 > nTotalBits)
			return false;
		nBitPos += (int)lenBytes * 8;
		return true;
	}
	case 43:
	{
		if (nBitPos + 32 > nTotalBits)
			return false;
		const uint32_t lenBytes = S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 32);
		if (lenBytes > 0x100000 || lenBytes > (uint32_t)(nTotalBits - nBitPos) / 8)
			return false;
		nBitPos += (int)(lenBytes * 8);
		return true;
	}
	case 44:
	{
		if (nBitPos + 64 > nTotalBits)
			return false;
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 32);
		S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBitPos, 32);
		return S21Bridge_Skip_CappedString(pData, nTotalBits, nBitPos, 512);
	}
	default:
		return false;
	}
}

#endif // NET_BRIDGE_SKIP_H
