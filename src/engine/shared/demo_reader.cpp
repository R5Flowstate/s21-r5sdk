//=============================================================================//
//
// Purpose: bounded .r5dem reader and demo directory helpers
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "demo_reader.h"
#include "demo_json.h"
#include "demo_writer.h"
#include <algorithm>

static constexpr int kDemoMaxChunks = 4 * 1024 * 1024;

CDemoReader::CDemoReader(void)
	: m_hFile(nullptr)
	, m_nFileSize(0)
	, m_bTruncated(false)
	, m_nBadOffset(0)
{
	memset(&m_Header, 0, sizeof(m_Header));
	m_szPath[0] = '\0';
}

CDemoReader::~CDemoReader(void)
{
	Close();
}

void CDemoReader::Close(void)
{
	if (m_hFile)
	{
		CloseHandle(static_cast<HANDLE>(m_hFile));
		m_hFile = nullptr;
	}
	m_Chunks.clear();
	m_Chunks.shrink_to_fit();
	m_Povs.clear();
	m_Events.clear();
	m_Meta.clear();
	m_bTruncated = false;
	m_nBadOffset = 0;
	m_nFileSize = 0;
}

static bool Demo_ReadAt(HANDLE hFile, const uint64_t nOffset, void* pOut, const uint32_t nLen)
{
	LARGE_INTEGER li;
	li.QuadPart = static_cast<LONGLONG>(nOffset);
	if (!SetFilePointerEx(hFile, li, nullptr, FILE_BEGIN))
		return false;
	DWORD nRead = 0;
	return ReadFile(hFile, pOut, nLen, &nRead, nullptr) && nRead == nLen;
}

bool CDemoReader::Open(const char* pszPath, char* pszErr, const size_t nErrLen)
{
	Close();

	auto fail = [&](const char* pszWhy) -> bool
	{
		if (pszErr && nErrLen)
			strncpy_s(pszErr, nErrLen, pszWhy, _TRUNCATE);
		Close();
		return false;
	};

	if (!pszPath || strncpy_s(m_szPath, pszPath, _TRUNCATE) != 0)
		return fail("bad path");

	const HANDLE hFile = CreateFileA(pszPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
		return fail("cannot open file");
	m_hFile = hFile;

	LARGE_INTEGER size;
	if (!GetFileSizeEx(hFile, &size) || size.QuadPart < static_cast<LONGLONG>(sizeof(R5DemHeader_s)))
		return fail("file too small");
	if (static_cast<uint64_t>(size.QuadPart) > R5DEM_MAX_FILE)
		return fail("file larger than 2 GiB");
	m_nFileSize = static_cast<uint64_t>(size.QuadPart);

	if (!Demo_ReadAt(hFile, 0, &m_Header, sizeof(m_Header)))
		return fail("cannot read header");
	if (memcmp(m_Header.magic, R5DEM_MAGIC, sizeof(m_Header.magic)) != 0)
		return fail("not an r5dem file");
	if (m_Header.version != R5DEM_VERSION)
		return fail("unsupported container version");
	if (m_Header.protocol != R5DEM_PROTOCOL)
	{
		char sz[128];
		snprintf(sz, sizeof(sz), "protocol mismatch: file %u, client %u -- this build cannot play it",
			m_Header.protocol, R5DEM_PROTOCOL);
		return fail(sz);
	}
	if (m_Header.povCount == 0 || m_Header.povCount > R5DEM_MAX_POVS)
		return fail("bad pov count");
	m_Header.map[sizeof(m_Header.map) - 1] = '\0';
	m_Header.mode[sizeof(m_Header.mode) - 1] = '\0';

	// Buffered sequential scan of the chunk headers.
	std::vector<uint8_t> window(1024 * 1024);
	uint64_t winStart = 0;
	uint32_t winLen = 0;
	auto readHeader = [&](const uint64_t off, R5DemChunkHeader_s* pHdr) -> bool
	{
		if (off + sizeof(*pHdr) > m_nFileSize)
			return false;
		if (off < winStart || off + sizeof(*pHdr) > winStart + winLen)
		{
			const uint64_t nAvail = m_nFileSize - off;
			winLen = static_cast<uint32_t>(nAvail < window.size() ? nAvail : window.size());
			winStart = off;
			if (!Demo_ReadAt(hFile, off, window.data(), winLen))
				return false;
		}
		memcpy(pHdr, window.data() + (off - winStart), sizeof(*pHdr));
		return true;
	};

	std::vector<uint8_t> payload;
	uint64_t off = sizeof(R5DemHeader_s);
	uint32_t prevSeq = 0;
	bool bFirst = true;
	while (off < m_nFileSize)
	{
		R5DemChunkHeader_s hdr;
		if (!readHeader(off, &hdr))
		{
			m_bTruncated = true;
			m_nBadOffset = off;
			break;
		}

		const uint32_t ceiling = R5Dem_Ceiling(hdr.type);
		const bool bPovOk = hdr.pov == R5DEM_POV_ALL || hdr.pov < m_Header.povCount;
		if (!ceiling || hdr.len > ceiling || !bPovOk
			|| off + sizeof(hdr) + hdr.len > m_nFileSize
			|| (!bFirst && hdr.seq <= prevSeq))
		{
			m_bTruncated = true;
			m_nBadOffset = off;
			break;
		}

		const R5DemChunk_t type = static_cast<R5DemChunk_t>(hdr.type);
		if (type == R5DemChunk_t::META || type == R5DemChunk_t::EVENT)
		{
			payload.resize(hdr.len);
			if (hdr.len && !Demo_ReadAt(hFile, off + sizeof(hdr), payload.data(), hdr.len))
			{
				m_bTruncated = true;
				m_nBadOffset = off;
				break;
			}
			if (R5Dem_Crc32(payload.data(), hdr.len) != hdr.crc32)
			{
				m_bTruncated = true;
				m_nBadOffset = off;
				break;
			}
			if (type == R5DemChunk_t::META)
				m_Meta.assign(reinterpret_cast<const char*>(payload.data()), hdr.len);
		}

		if (type != R5DemChunk_t::INDEX)
		{
			if (m_Chunks.size() >= static_cast<size_t>(kDemoMaxChunks))
			{
				m_bTruncated = true;
				m_nBadOffset = off;
				break;
			}
			DemoChunkRef_s ref;
			ref.offset = off;
			ref.hdr = hdr;
			m_Chunks.push_back(ref);
		}

		if (type == R5DemChunk_t::EVENT)
			ParseEvent(reinterpret_cast<const char*>(payload.data()), hdr.len);

		prevSeq = hdr.seq;
		bFirst = false;
		off += sizeof(hdr) + hdr.len;
	}

	if (m_bTruncated)
		Warning(eDLL_T::ENGINE, "[DEMO] %s: chunk list ends at offset %llu (damaged or unfinished tail)\n",
			pszPath, static_cast<unsigned long long>(m_nBadOffset));

	ParseMeta();

	const float flTick = m_Header.tickIntervalUs ? m_Header.tickIntervalUs / 1000000.0f : 0.05f;
	const uint32_t firstTick = GetFirstFullTick(0);
	for (DemoEvent_s& e : m_Events)
		e.timeSec = (firstTick != UINT32_MAX && e.tick >= firstTick) ? (e.tick - firstTick) * flTick : 0.0f;
	std::stable_sort(m_Events.begin(), m_Events.end(),
		[](const DemoEvent_s& a, const DemoEvent_s& b) { return a.tick < b.tick; });

	return true;
}

bool CDemoReader::ReadPayload(const DemoChunkRef_s& ref, std::vector<uint8_t>& out)
{
	if (!m_hFile || ref.hdr.len > R5Dem_Ceiling(ref.hdr.type))
		return false;
	out.resize(ref.hdr.len);
	if (ref.hdr.len == 0)
		return true;
	if (!Demo_ReadAt(static_cast<HANDLE>(m_hFile), ref.offset + sizeof(R5DemChunkHeader_s), out.data(), ref.hdr.len))
		return false;
	return R5Dem_Crc32(out.data(), ref.hdr.len) == ref.hdr.crc32;
}

void CDemoReader::ParseMeta(void)
{
	m_Povs.clear();
	const char* const s = m_Meta.c_str();
	const size_t n = m_Meta.size();

	DemoJson::ForEachObject(s, n, "povs", [&](const char* o, size_t len)
	{
		DemoPovInfo_s pov;
		memset(&pov, 0, sizeof(pov));
		double v = 0.0;
		pov.id = DemoJson::GetNumber(o, len, "id", &v) ? static_cast<int>(v) : static_cast<int>(m_Povs.size());
		pov.slot = DemoJson::GetNumber(o, len, "slot", &v) ? static_cast<int>(v) : -1;
		pov.eh = DemoJson::GetNumber(o, len, "eh", &v) ? static_cast<uint32_t>(v) : 0xFFFFFFFFu;
		pov.team = DemoJson::GetNumber(o, len, "team", &v) ? static_cast<int>(v) : 0;
		DemoJson::GetString(o, len, "name", pov.name, sizeof(pov.name));
		DemoJson::GetString(o, len, "legend", pov.legend, sizeof(pov.legend));
		if (pov.id >= 0 && pov.id < R5DEM_MAX_POVS)
			m_Povs.push_back(pov);
	}, R5DEM_MAX_POVS);

	std::sort(m_Povs.begin(), m_Povs.end(),
		[](const DemoPovInfo_s& a, const DemoPovInfo_s& b) { return a.id < b.id; });
}

void CDemoReader::ParseEvent(const char* pszJson, const size_t nLen)
{
	if (!pszJson || !nLen || m_Events.size() >= 65536)
		return;

	DemoEvent_s e;
	memset(e.type, 0, sizeof(e.type));
	memset(e.weapon, 0, sizeof(e.weapon));
	double v = 0.0;
	DemoJson::GetString(pszJson, nLen, "t", e.type, sizeof(e.type));
	e.tick = DemoJson::GetNumber(pszJson, nLen, "tick", &v) ? static_cast<uint32_t>(v) : 0;
	e.attacker = DemoJson::GetNumber(pszJson, nLen, "a", &v) ? static_cast<int>(v) : -1;
	e.victim = DemoJson::GetNumber(pszJson, nLen, "v", &v) ? static_cast<int>(v) : -1;
	e.damage = DemoJson::GetNumber(pszJson, nLen, "d", &v) ? static_cast<float>(v) : 0.0f;
	DemoJson::GetString(pszJson, nLen, "w", e.weapon, sizeof(e.weapon));
	memset(e.attackerName, 0, sizeof(e.attackerName));
	memset(e.victimName, 0, sizeof(e.victimName));
	DemoJson::GetString(pszJson, nLen, "an", e.attackerName, sizeof(e.attackerName));
	DemoJson::GetString(pszJson, nLen, "vn", e.victimName, sizeof(e.victimName));
	e.timeSec = 0.0f;
	e.json.assign(pszJson, nLen);
	if (!e.type[0])
		return;
	m_Events.push_back(std::move(e));
}

uint32_t CDemoReader::GetFirstFullWallMs(const int nPov) const
{
	uint32_t firstAny = UINT32_MAX;
	for (const DemoChunkRef_s& c : m_Chunks)
	{
		if (c.hdr.type != static_cast<uint8_t>(R5DemChunk_t::PACKET) || c.hdr.pov != nPov)
			continue;
		if (c.hdr.kind & R5DEM_KIND_PRELUDE)
			continue;
		if (firstAny == UINT32_MAX)
			firstAny = c.hdr.wallMs;
		if (c.hdr.kind & R5DEM_KIND_FULL)
			return c.hdr.wallMs;
	}
	return firstAny;
}

uint32_t CDemoReader::GetFirstFullTick(const int nPov) const
{
	uint32_t firstAny = UINT32_MAX;
	for (const DemoChunkRef_s& c : m_Chunks)
	{
		if (c.hdr.type != static_cast<uint8_t>(R5DemChunk_t::PACKET) || c.hdr.pov != nPov)
			continue;
		if (c.hdr.kind & R5DEM_KIND_PRELUDE)
			continue;
		if (firstAny == UINT32_MAX && c.hdr.tick)
			firstAny = c.hdr.tick;
		if ((c.hdr.kind & R5DEM_KIND_FULL) && c.hdr.tick)
			return c.hdr.tick;
	}
	return firstAny;
}

uint32_t CDemoReader::GetLastWallMs(const int nPov) const
{
	for (auto it = m_Chunks.rbegin(); it != m_Chunks.rend(); ++it)
	{
		if (it->hdr.type == static_cast<uint8_t>(R5DemChunk_t::PACKET) && it->hdr.pov == nPov)
			return it->hdr.wallMs;
	}
	return 0;
}

int CDemoReader::GetPacketCount(const int nPov) const
{
	int n = 0;
	for (const DemoChunkRef_s& c : m_Chunks)
	{
		if (c.hdr.type == static_cast<uint8_t>(R5DemChunk_t::PACKET) && c.hdr.pov == nPov)
			++n;
	}
	return n;
}

//-----------------------------------------------------------------------------
// Directory helpers
//-----------------------------------------------------------------------------
bool Demo_IsSafeRoot(const char* pszRoot)
{
	if (!pszRoot || !pszRoot[0] || strlen(pszRoot) > 180)
		return false;
	if (strstr(pszRoot, ".."))
		return false;
	if (pszRoot[0] == '\\' && pszRoot[1] == '\\')
		return false;
	for (const char* p = pszRoot; *p; ++p)
	{
		const unsigned char c = static_cast<unsigned char>(*p);
		if (c < 0x20 || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
			return false;
	}
	return true;
}

void Demo_SanitizeName(const char* pszIn, char* pszOut, const size_t nOutLen)
{
	if (!pszOut || !nOutLen)
		return;
	size_t o = 0;
	for (size_t i = 0; pszIn && pszIn[i] && o + 1 < nOutLen && o < 64; ++i)
	{
		const char c = pszIn[i];
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
			|| (c >= '0' && c <= '9') || c == '_' || c == '-';
		pszOut[o++] = ok ? c : '_';
	}
	pszOut[o] = '\0';
}

static uint64_t Demo_FileTime(const FILETIME& ft)
{
	return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// Date directories under the root, newest name first ("yyyy-mm-dd" sorts lexically).
static void Demo_ListDateDirs(const char* pszRoot, std::vector<std::string>& out)
{
	char szGlob[MAX_PATH];
	if (_snprintf_s(szGlob, _TRUNCATE, "%s\\*", pszRoot) < 0)
		return;

	WIN32_FIND_DATAA fd;
	const HANDLE h = FindFirstFileA(szGlob, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;
	int nSafety = 0;
	do
	{
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.')
			continue;
		if (!R5Dem_IsValidName(fd.cFileName))
			continue;
		out.emplace_back(fd.cFileName);
	} while (FindNextFileA(h, &fd) && ++nSafety < 10000);
	FindClose(h);

	std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return a > b; });
}

bool Demo_FindFile(const char* pszRoot, const char* pszName, char* pszOut, const size_t nOutLen)
{
	if (!Demo_IsSafeRoot(pszRoot) || !R5Dem_IsValidName(pszName) || !pszOut || !nOutLen)
		return false;

	std::vector<std::string> dirs;
	Demo_ListDateDirs(pszRoot, dirs);

	static const char* const s_exts[] = { ".r5dem", ".part" };
	for (const char* ext : s_exts)
	{
		char sz[MAX_PATH];
		for (const std::string& d : dirs)
		{
			_snprintf_s(sz, _TRUNCATE, "%s\\%s\\%s%s", pszRoot, d.c_str(), pszName, ext);
			const DWORD attr = GetFileAttributesA(sz);
			if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
				return strncpy_s(pszOut, nOutLen, sz, _TRUNCATE) == 0;
		}
		_snprintf_s(sz, _TRUNCATE, "%s\\%s%s", pszRoot, pszName, ext);
		const DWORD attr = GetFileAttributesA(sz);
		if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
			return strncpy_s(pszOut, nOutLen, sz, _TRUNCATE) == 0;
	}
	return false;
}

static void Demo_CollectDir(const char* pszDir, std::vector<DemoFileInfo_s>& out)
{
	static const char* const s_exts[] = { ".r5dem", ".part" };
	for (const char* ext : s_exts)
	{
		char szGlob[MAX_PATH];
		if (_snprintf_s(szGlob, _TRUNCATE, "%s\\*%s", pszDir, ext) < 0)
			continue;

		WIN32_FIND_DATAA fd;
		const HANDLE h = FindFirstFileA(szGlob, &fd);
		if (h == INVALID_HANDLE_VALUE)
			continue;
		int nSafety = 0;
		do
		{
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			const size_t nLen = strlen(fd.cFileName);
			const size_t nExt = strlen(ext);
			if (nLen <= nExt || _stricmp(fd.cFileName + nLen - nExt, ext) != 0)
				continue;

			DemoFileInfo_s info;
			memset(&info, 0, sizeof(info));
			const size_t nStem = nLen - nExt;
			if (nStem >= sizeof(info.name))
				continue;
			memcpy(info.name, fd.cFileName, nStem);
			info.name[nStem] = '\0';
			if (!R5Dem_IsValidName(info.name))
				continue;
			_snprintf_s(info.path, _TRUNCATE, "%s\\%s", pszDir, fd.cFileName);
			info.mtime = Demo_FileTime(fd.ftLastWriteTime);
			info.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
			info.bPartial = (ext[1] == 'p');
			out.push_back(info);
		} while (FindNextFileA(h, &fd) && ++nSafety < 100000);
		FindClose(h);
	}
}

void Demo_ListFiles(const char* pszRoot, std::vector<DemoFileInfo_s>& out, const size_t nMax)
{
	out.clear();
	if (!Demo_IsSafeRoot(pszRoot))
		return;

	std::vector<std::string> dirs;
	Demo_ListDateDirs(pszRoot, dirs);
	for (const std::string& d : dirs)
	{
		char sz[MAX_PATH];
		_snprintf_s(sz, _TRUNCATE, "%s\\%s", pszRoot, d.c_str());
		Demo_CollectDir(sz, out);
	}
	Demo_CollectDir(pszRoot, out);

	std::sort(out.begin(), out.end(),
		[](const DemoFileInfo_s& a, const DemoFileInfo_s& b) { return a.mtime > b.mtime; });
	if (out.size() > nMax)
		out.resize(nMax);
}

bool Demo_DeleteFile(const char* pszRoot, const char* pszName)
{
	char sz[MAX_PATH];
	if (!Demo_FindFile(pszRoot, pszName, sz, sizeof(sz)))
		return false;

	const size_t nLen = strlen(sz);
	const bool bDemo = nLen > 6 && _stricmp(sz + nLen - 6, ".r5dem") == 0;
	const bool bPart = nLen > 5 && _stricmp(sz + nLen - 5, ".part") == 0;
	if (!bDemo && !bPart)
		return false;

	const DWORD attr = GetFileAttributesA(sz);
	if (attr == INVALID_FILE_ATTRIBUTES || (attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
		return false;
	return DeleteFileA(sz) != FALSE;
}

int Demo_RecoverParts(const char* pszRoot)
{
	std::vector<DemoFileInfo_s> files;
	Demo_ListFiles(pszRoot, files, 100000);

	int nRecovered = 0;
	for (const DemoFileInfo_s& f : files)
	{
		if (!f.bPartial)
			continue;
		const size_t nLen = strlen(f.path);
		if (nLen <= 5 || nLen - 5 + 7 > MAX_PATH)
			continue;
		char szFinal[MAX_PATH];
		memcpy(szFinal, f.path, nLen - 5);
		strcpy_s(szFinal + nLen - 5, sizeof(szFinal) - (nLen - 5), ".r5dem");
		if (MoveFileExA(f.path, szFinal, 0))
			++nRecovered;
	}
	return nRecovered;
}

static void Demo_LoadPinned(const char* pszRoot, std::vector<std::string>& out)
{
	out.clear();
	char sz[MAX_PATH];
	if (_snprintf_s(sz, _TRUNCATE, "%s\\pinned.txt", pszRoot) <= 0)
		return;
	const HANDLE h = CreateFileA(sz, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return;
	char buf[16384];
	DWORD n = 0;
	const BOOL bRead = ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr);
	CloseHandle(h);
	if (!bRead)
		return;
	buf[n] = '\0';
	for (char* pLine = buf; *pLine && out.size() < 4096;)
	{
		char* pEnd = pLine;
		while (*pEnd && *pEnd != '\n' && *pEnd != '\r')
			++pEnd;
		const char c = *pEnd;
		*pEnd = '\0';
		if (R5Dem_IsValidName(pLine))
			out.emplace_back(pLine);
		pLine = c ? pEnd + 1 : pEnd;
	}
}

void Demo_Prune(const char* pszRoot, const int nKeepDays, const int nMaxMb)
{
	std::vector<DemoFileInfo_s> files;
	Demo_ListFiles(pszRoot, files, 100000);
	if (files.empty())
		return;

	std::vector<std::string> pinned;
	Demo_LoadPinned(pszRoot, pinned);
	auto isPinned = [&](const DemoFileInfo_s& f)
	{
		for (const std::string& p : pinned)
		{
			if (_stricmp(p.c_str(), f.name) == 0)
				return true;
		}
		return false;
	};

	FILETIME ftNow;
	GetSystemTimeAsFileTime(&ftNow);
	const uint64_t now = Demo_FileTime(ftNow);
	const uint64_t kDay = 24ull * 3600ull * 10000000ull;

	uint64_t total = 0;
	int nRemoved = 0;
	std::vector<DemoFileInfo_s> kept;
	for (const DemoFileInfo_s& f : files)
	{
		const uint64_t age = now > f.mtime ? now - f.mtime : 0;
		if ((f.bPartial && age < kDay) || isPinned(f))
		{
			kept.push_back(f);
			continue;
		}
		if (nKeepDays > 0 && age > static_cast<uint64_t>(nKeepDays) * kDay)
		{
			if (DeleteFileA(f.path))
				++nRemoved;
			continue;
		}
		kept.push_back(f);
		total += f.size;
	}

	const uint64_t cap = nMaxMb > 0 ? static_cast<uint64_t>(nMaxMb) * 1024ull * 1024ull : 0;
	if (cap)
	{
		// kept is newest first; drop from the back.
		for (auto it = kept.rbegin(); it != kept.rend() && total > cap; ++it)
		{
			if (it->bPartial || isPinned(*it))
				continue;
			if (DeleteFileA(it->path))
			{
				total -= it->size;
				++nRemoved;
			}
		}
	}

	if (nRemoved)
		Msg(eDLL_T::ENGINE, "[DEMO] pruned %d old demo file(s) under '%s'\n", nRemoved, pszRoot);
}

bool Demo_BuildRecordPath(const char* pszRoot, const char* pszBase, char* pszOut, const size_t nOutLen)
{
	if (!Demo_IsSafeRoot(pszRoot) || !R5Dem_IsValidName(pszBase))
		return false;

	SYSTEMTIME st;
	GetLocalTime(&st);
	return _snprintf_s(pszOut, nOutLen, _TRUNCATE, "%s\\%04u-%02u-%02u\\%s.r5dem",
		pszRoot, st.wYear, st.wMonth, st.wDay, pszBase) > 0;
}
