//=============================================================================//
//
// Purpose: bounded .r5dem reader and the demo directory helpers shared by
//          the player, the server tools and the replay list.
//
//=============================================================================//
#ifndef ENGINE_SHARED_DEMO_READER_H
#define ENGINE_SHARED_DEMO_READER_H

#include "public/demo/r5dem.h"
#include <string>
#include <vector>

struct DemoChunkRef_s
{
	uint64_t           offset; // of the chunk header
	R5DemChunkHeader_s hdr;
};

struct DemoPovInfo_s
{
	int      id;
	int      slot;
	uint32_t eh;
	int      team;
	char     name[68];
	char     legend[68];
};

struct DemoEvent_s
{
	char     type[16];
	uint32_t tick;
	float    timeSec;   // seconds since the first full snapshot of pov 0
	int      attacker;
	int      victim;
	char     weapon[48];
	float    damage;
	char     attackerName[64];   // client recordings name players; server ones use pov ids
	char     victimName[64];
	std::string json;
};

class CDemoReader
{
public:
	CDemoReader(void);
	~CDemoReader(void);

	// Scans the whole file. A damaged or truncated tail ends the chunk list at
	// the last good chunk; Open still succeeds so the demo plays up to it.
	bool Open(const char* pszPath, char* pszErr, const size_t nErrLen);
	void Close(void);
	bool IsOpen(void) const { return m_hFile != nullptr; }

	bool ReadPayload(const DemoChunkRef_s& ref, std::vector<uint8_t>& out);

	const R5DemHeader_s& GetHeader(void) const { return m_Header; }
	const std::vector<DemoChunkRef_s>& GetChunks(void) const { return m_Chunks; }
	const std::string& GetMetaJson(void) const { return m_Meta; }
	const std::vector<DemoPovInfo_s>& GetPovs(void) const { return m_Povs; }
	const std::vector<DemoEvent_s>& GetEvents(void) const { return m_Events; }
	const char* GetPath(void) const { return m_szPath; }

	bool IsTruncated(void) const { return m_bTruncated; }
	uint64_t GetBadOffset(void) const { return m_nBadOffset; }

	// Wall-clock of the first full snapshot recorded for this pov; falls back to
	// the first packet. UINT32_MAX when the pov has no packets.
	uint32_t GetFirstFullWallMs(const int nPov) const;
	uint32_t GetLastWallMs(const int nPov) const;
	uint32_t GetFirstFullTick(const int nPov) const;
	int      GetPacketCount(const int nPov) const;

private:
	void ParseMeta(void);
	void ParseEvent(const char* pszJson, const size_t nLen);

	void*    m_hFile;
	uint64_t m_nFileSize;
	R5DemHeader_s m_Header;
	std::vector<DemoChunkRef_s> m_Chunks;
	std::vector<DemoPovInfo_s>  m_Povs;
	std::vector<DemoEvent_s>    m_Events;
	std::string m_Meta;
	bool     m_bTruncated;
	uint64_t m_nBadOffset;
	char     m_szPath[260];
};

struct DemoFileInfo_s
{
	char     name[72];
	char     path[260];
	uint64_t mtime;       // FILETIME as u64
	uint64_t size;
	bool     bPartial;
};

// Directory root for demo files: an absolute or game-relative path that must
// not contain "..".
bool Demo_IsSafeRoot(const char* pszRoot);

// <root>/<yyyy-mm-dd>/<name>.r5dem (newest date first), then <root>/<name>.r5dem,
// then the same for .part. pszName must pass R5Dem_IsValidName.
bool Demo_FindFile(const char* pszRoot, const char* pszName, char* pszOut, const size_t nOutLen);

// Newest first, at most nMax entries.
void Demo_ListFiles(const char* pszRoot, std::vector<DemoFileInfo_s>& out, const size_t nMax);

bool Demo_DeleteFile(const char* pszRoot, const char* pszName);

// Removes demos older than nKeepDays, then oldest-first until the total is
// under nMaxMb. Open .part files younger than a day are left alone, and so is
// every name listed in <root>/pinned.txt (one per line).
void Demo_Prune(const char* pszRoot, const int nKeepDays, const int nMaxMb);
// Renames every .part left by a killed process to .r5dem. A file still open
// by its writer fails the rename and is left alone.
int  Demo_RecoverParts(const char* pszRoot);

// "<root>/<yyyy-mm-dd>/<base>.r5dem" for a new recording.
bool Demo_BuildRecordPath(const char* pszRoot, const char* pszBase, char* pszOut, const size_t nOutLen);

// Keeps [a-zA-Z0-9_-], replaces the rest with '_', truncates to 64.
void Demo_SanitizeName(const char* pszIn, char* pszOut, const size_t nOutLen);

#endif // ENGINE_SHARED_DEMO_READER_H
