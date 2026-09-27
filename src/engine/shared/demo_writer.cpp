//=============================================================================//
//
// Purpose: append-only .r5dem writer
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "demo_writer.h"

static constexpr uint32_t kDemoRingSize    = 8u * 1024u * 1024u;
static constexpr uint32_t kDemoWakeBytes   = 1u * 1024u * 1024u;
static constexpr DWORD    kDemoWakeMs      = 250;
// Headroom kept below the file cap for the INDEX trailer.
static constexpr uint64_t kDemoFileHeadroom = 32ull * 1024ull * 1024ull;

bool Demo_CreateDirsForFile(const char* pszPath)
{
	if (!pszPath || !pszPath[0])
		return false;

	char szDir[MAX_PATH];
	if (strncpy_s(szDir, pszPath, _TRUNCATE) != 0)
		return false;

	for (char* p = szDir; *p; ++p)
	{
		if (*p == '/')
			*p = '\\';
	}

	char* const pLast = strrchr(szDir, '\\');
	if (!pLast)
		return true;
	*pLast = '\0';

	for (char* p = szDir; *p; ++p)
	{
		if (*p != '\\' || p == szDir || p[-1] == ':')
			continue;
		*p = '\0';
		CreateDirectoryA(szDir, nullptr);
		*p = '\\';
	}
	CreateDirectoryA(szDir, nullptr);

	const DWORD attr = GetFileAttributesA(szDir);
	return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

CDemoWriter::CDemoWriter(void)
	: m_hFile(nullptr)
	, m_hThread(nullptr)
	, m_hWake(nullptr)
	, m_pRing(nullptr)
	, m_nRingWritten(0)
	, m_nRingRead(0)
	, m_nFileOffset(0)
	, m_nOpenQpc(0)
	, m_nSeq(0)
	, m_nDropped(0)
	, m_bStop(false)
	, m_bFailed(false)
	, m_bLostSignon(false)
	, m_bFailWarned(false)
	, m_nByteLimit(0)
	, m_pSharedPool(nullptr)
{
	InitializeSRWLock(&m_Lock);
	memset(&m_Header, 0, sizeof(m_Header));
	m_szFinalPath[0] = '\0';
	m_szPartPath[0] = '\0';
}

CDemoWriter::~CDemoWriter(void)
{
	if (IsOpen())
		Close(true);
}

uint32_t CDemoWriter::GetWallMs(void) const
{
	LARGE_INTEGER now, freq;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&freq);
	if (freq.QuadPart <= 0 || !m_nOpenQpc)
		return 0;
	return static_cast<uint32_t>((static_cast<uint64_t>(now.QuadPart) - m_nOpenQpc) * 1000ull
		/ static_cast<uint64_t>(freq.QuadPart));
}

bool CDemoWriter::Open(const char* pszFinalPath, const R5DemHeader_s& header)
{
	if (IsOpen() || !pszFinalPath || !pszFinalPath[0])
		return false;

	if (strncpy_s(m_szFinalPath, pszFinalPath, _TRUNCATE) != 0)
		return false;

	// "<name>.r5dem" -> "<name>.part"
	strncpy_s(m_szPartPath, m_szFinalPath, _TRUNCATE);
	char* const pExt = strrchr(m_szPartPath, '.');
	if (pExt && _stricmp(pExt, ".r5dem") == 0)
		*pExt = '\0';
	strncat_s(m_szPartPath, ".part", _TRUNCATE);

	if (!Demo_CreateDirsForFile(m_szPartPath))
	{
		Warning(eDLL_T::ENGINE, "[DEMO] cannot create the directory for '%s'\n", m_szPartPath);
		return false;
	}

	const HANDLE hFile = CreateFileA(m_szPartPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		Warning(eDLL_T::ENGINE, "[DEMO] cannot open '%s' for writing (err=%lu)\n",
			m_szPartPath, GetLastError());
		return false;
	}

	m_pRing = static_cast<uint8_t*>(malloc(kDemoRingSize));
	if (!m_pRing)
	{
		CloseHandle(hFile);
		DeleteFileA(m_szPartPath);
		Warning(eDLL_T::ENGINE, "[DEMO] ring allocation failed\n");
		return false;
	}

	m_hFile = hFile;
	m_Header = header;
	memcpy(m_Header.magic, R5DEM_MAGIC, sizeof(m_Header.magic));
	m_Header.version = R5DEM_VERSION;
	m_Header.protocol = R5DEM_PROTOCOL;
	m_Header.indexOffset = 0;
	m_Header.flags &= ~R5DEM_FLAG_INDEX;

	m_nRingWritten = 0;
	m_nRingRead = 0;
	m_nFileOffset = 0;
	m_nSeq = 0;
	m_nDropped = 0;
	m_bStop = false;
	m_bFailed = false;
	m_bLostSignon = false;
	m_bFailWarned = false;
	m_Index.clear();

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	m_nOpenQpc = static_cast<uint64_t>(now.QuadPart);

	if (!WriteRaw(&m_Header, sizeof(m_Header)))
	{
		Close(false);
		return false;
	}

	m_hWake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
	m_hThread = CreateThread(nullptr, 0, &CDemoWriter::ThreadProc, this, 0, nullptr);
	if (!m_hWake || !m_hThread)
	{
		Warning(eDLL_T::ENGINE, "[DEMO] writer thread failed to start\n");
		Close(false);
		return false;
	}
	SetThreadPriority(m_hThread, THREAD_PRIORITY_BELOW_NORMAL);
	return true;
}

void CDemoWriter::RingWrite(uint64_t nPos, const void* pIn, uint32_t nLen)
{
	const uint8_t* src = static_cast<const uint8_t*>(pIn);
	while (nLen > 0)
	{
		const uint32_t off = static_cast<uint32_t>(nPos % kDemoRingSize);
		const uint32_t run = (nLen < kDemoRingSize - off) ? nLen : (kDemoRingSize - off);
		memcpy(m_pRing + off, src, run);
		src += run;
		nPos += run;
		nLen -= run;
	}
}

void CDemoWriter::RingRead(uint64_t nPos, void* pOut, uint32_t nLen) const
{
	uint8_t* dst = static_cast<uint8_t*>(pOut);
	while (nLen > 0)
	{
		const uint32_t off = static_cast<uint32_t>(nPos % kDemoRingSize);
		const uint32_t run = (nLen < kDemoRingSize - off) ? nLen : (kDemoRingSize - off);
		memcpy(dst, m_pRing + off, run);
		dst += run;
		nPos += run;
		nLen -= run;
	}
}

bool CDemoWriter::Append(const R5DemChunk_t type, const uint8_t nPov, const uint8_t nSignon,
	const uint8_t nKind, const uint32_t nTick, const void* pData, const uint32_t nLen)
{
	if (!IsOpen() || m_bFailed)
		return false;

	const uint8_t nType = static_cast<uint8_t>(type);
	if (nLen > R5Dem_Ceiling(nType) || (nLen && !pData))
		return false;

	R5DemChunkHeader_s hdr;
	hdr.type = nType;
	hdr.pov = nPov;
	hdr.signon = nSignon;
	hdr.kind = nKind;
	hdr.len = nLen;
	hdr.tick = nTick;
	hdr.wallMs = GetWallMs();
	hdr.crc32 = nLen ? R5Dem_Crc32(pData, nLen) : 0;

	const uint64_t nNeed = sizeof(hdr) + static_cast<uint64_t>(nLen);

	AcquireSRWLockExclusive(&m_Lock);
	const uint64_t nUsed = m_nRingWritten - m_nRingRead;
	if (nUsed + nNeed > kDemoRingSize)
	{
		++m_nDropped;
		if (type == R5DemChunk_t::SIGNON)
			m_bLostSignon = true;
		ReleaseSRWLockExclusive(&m_Lock);
		return false;
	}

	hdr.seq = m_nSeq++;
	RingWrite(m_nRingWritten, &hdr, sizeof(hdr));
	if (nLen)
		RingWrite(m_nRingWritten + sizeof(hdr), pData, nLen);
	m_nRingWritten += nNeed;
	const uint64_t nPending = m_nRingWritten - m_nRingRead;
	ReleaseSRWLockExclusive(&m_Lock);

	if (nPending >= kDemoWakeBytes && m_hWake)
		SetEvent(m_hWake);
	return true;
}

void CDemoWriter::Fail(const char* pszWhy)
{
	m_bFailed = true;
	if (!m_bFailWarned)
	{
		m_bFailWarned = true;
		Warning(eDLL_T::ENGINE, "[DEMO] recording to '%s' stopped: %s\n", m_szPartPath, pszWhy);
	}
}

bool CDemoWriter::WriteRaw(const void* pData, const uint32_t nLen)
{
	if (m_bFailed)
		return false;

	if (m_nFileOffset + nLen > R5DEM_MAX_FILE - kDemoFileHeadroom)
	{
		Fail("file size cap reached");
		return false;
	}

	if (m_nByteLimit && m_nFileOffset + nLen > m_nByteLimit)
	{
		Fail("demo size budget reached");
		return false;
	}

	if (m_pSharedPool && m_pSharedPool->fetch_sub(nLen, std::memory_order_relaxed) < static_cast<int64_t>(nLen))
	{
		m_pSharedPool->fetch_add(nLen, std::memory_order_relaxed);
		Fail("demo directory budget reached");
		return false;
	}

	DWORD nWritten = 0;
	if (!WriteFile(static_cast<HANDLE>(m_hFile), pData, nLen, &nWritten, nullptr) || nWritten != nLen)
	{
		Fail("disk write failed");
		return false;
	}
	m_nFileOffset += nLen;
	return true;
}

bool CDemoWriter::DrainOne(void)
{
	R5DemChunkHeader_s hdr;

	AcquireSRWLockExclusive(&m_Lock);
	if (m_nRingWritten - m_nRingRead < sizeof(hdr))
	{
		ReleaseSRWLockExclusive(&m_Lock);
		return false;
	}
	RingRead(m_nRingRead, &hdr, sizeof(hdr));
	const uint32_t nTotal = static_cast<uint32_t>(sizeof(hdr)) + hdr.len;
	if (m_Staging.size() < nTotal)
		m_Staging.resize(nTotal);
	RingRead(m_nRingRead, m_Staging.data(), nTotal);
	m_nRingRead += nTotal;
	ReleaseSRWLockExclusive(&m_Lock);

	const uint64_t nChunkOffset = m_nFileOffset;
	if (!WriteRaw(m_Staging.data(), nTotal))
		return true;

	const R5DemChunk_t type = static_cast<R5DemChunk_t>(hdr.type);
	const bool bKeyed = type == R5DemChunk_t::SIGNON || type == R5DemChunk_t::RELIABLE
		|| type == R5DemChunk_t::KEYFRAME
		|| (type == R5DemChunk_t::PACKET && (hdr.kind & R5DEM_KIND_FULL));
	if (bKeyed && m_Index.size() < R5DEM_MAX_INDEX / sizeof(R5DemIndexEntry_s))
	{
		R5DemIndexEntry_s e;
		e.tick = hdr.tick;
		e.pov = hdr.pov;
		e.kind = static_cast<uint32_t>(hdr.type) | (static_cast<uint32_t>(hdr.kind) << 8);
		e.fileOffset = nChunkOffset;
		m_Index.push_back(e);
	}
	return true;
}

void CDemoWriter::WriterLoop(void)
{
	for (;;)
	{
		WaitForSingleObject(static_cast<HANDLE>(m_hWake), kDemoWakeMs);

		int nSafety = 0;
		while (DrainOne() && ++nSafety < 100000)
			;

		if (m_bStop)
		{
			AcquireSRWLockShared(&m_Lock);
			const bool bEmpty = (m_nRingWritten == m_nRingRead);
			ReleaseSRWLockShared(&m_Lock);
			if (bEmpty)
				break;
		}
	}
}

unsigned long __stdcall CDemoWriter::ThreadProc(void* pParam)
{
	static_cast<CDemoWriter*>(pParam)->WriterLoop();
	return 0;
}

void CDemoWriter::Close(const bool bKeep)
{
	if (!IsOpen())
		return;

	if (m_hThread)
	{
		m_bStop = true;
		SetEvent(static_cast<HANDLE>(m_hWake));
		WaitForSingleObject(static_cast<HANDLE>(m_hThread), INFINITE);
		CloseHandle(static_cast<HANDLE>(m_hThread));
		m_hThread = nullptr;
	}
	if (m_hWake)
	{
		CloseHandle(static_cast<HANDLE>(m_hWake));
		m_hWake = nullptr;
	}

	const HANDLE hFile = static_cast<HANDLE>(m_hFile);
	const bool bFailedWriting = m_bFailed;

	if (bKeep && !m_bFailed)
	{
		const uint32_t nIndexBytes = static_cast<uint32_t>(m_Index.size() * sizeof(R5DemIndexEntry_s));
		std::vector<uint8_t> payload(sizeof(uint32_t) + nIndexBytes);
		const uint32_t nCount = static_cast<uint32_t>(m_Index.size());
		memcpy(payload.data(), &nCount, sizeof(nCount));
		if (nIndexBytes)
			memcpy(payload.data() + sizeof(nCount), m_Index.data(), nIndexBytes);

		R5DemChunkHeader_s hdr;
		hdr.type = static_cast<uint8_t>(R5DemChunk_t::INDEX);
		hdr.pov = R5DEM_POV_ALL;
		hdr.signon = 0;
		hdr.kind = 0;
		hdr.len = static_cast<uint32_t>(payload.size());
		hdr.seq = m_nSeq++;
		hdr.tick = 0;
		hdr.wallMs = GetWallMs();
		hdr.crc32 = R5Dem_Crc32(payload.data(), payload.size());

		const uint64_t nIndexOffset = m_nFileOffset;
		if (WriteRaw(&hdr, sizeof(hdr)) && WriteRaw(payload.data(), hdr.len))
		{
			m_Header.indexOffset = nIndexOffset;
			m_Header.flags |= R5DEM_FLAG_INDEX;

			LARGE_INTEGER zero;
			zero.QuadPart = 0;
			DWORD nWritten = 0;
			if (SetFilePointerEx(hFile, zero, nullptr, FILE_BEGIN))
				WriteFile(hFile, &m_Header, sizeof(m_Header), &nWritten, nullptr);
		}
	}

	FlushFileBuffers(hFile);
	CloseHandle(hFile);
	m_hFile = nullptr;

	free(m_pRing);
	m_pRing = nullptr;
	m_Staging.clear();
	m_Staging.shrink_to_fit();
	m_Index.clear();

	if (!bKeep)
	{
		DeleteFileA(m_szPartPath);
		return;
	}

	// A failed recording keeps its .part: everything up to the failure plays.
	if (bFailedWriting)
		return;

	if (!MoveFileExA(m_szPartPath, m_szFinalPath, MOVEFILE_REPLACE_EXISTING))
		Warning(eDLL_T::ENGINE, "[DEMO] could not rename '%s' (err=%lu)\n", m_szPartPath, GetLastError());
}
