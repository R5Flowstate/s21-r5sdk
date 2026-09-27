//=============================================================================//
//
// Purpose: append-only .r5dem writer. Producers never block: chunks go into a
//          fixed ring and a low-priority thread writes them to disk.
//
//=============================================================================//
#ifndef ENGINE_SHARED_DEMO_WRITER_H
#define ENGINE_SHARED_DEMO_WRITER_H

#include "public/demo/r5dem.h"
#include <atomic>
#include <vector>

class CDemoWriter
{
public:
	CDemoWriter(void);
	~CDemoWriter(void);

	// Writes to <pszFinalPath>.part until Close(true) renames it.
	bool Open(const char* pszFinalPath, const R5DemHeader_s& header);
	bool Append(const R5DemChunk_t type, const uint8_t nPov, const uint8_t nSignon,
		const uint8_t nKind, const uint32_t nTick, const void* pData, const uint32_t nLen);
	void Close(const bool bKeep);

	bool IsOpen(void) const { return m_hFile != nullptr; }
	bool HasFailed(void) const { return m_bFailed; }
	bool LostSignon(void) const { return m_bLostSignon; }
	uint32_t GetDropped(void) const { return m_nDropped; }
	uint32_t GetWallMs(void) const;
	uint64_t GetBytesWritten(void) const { return m_nFileOffset; }
	const char* GetFinalPath(void) const { return m_szFinalPath; }

	// Recording stops once this file passes nOwnLimit bytes, or once the shared
	// pool (bytes left for every writer drawing on it) runs dry. 0 / null = none.
	void SetByteBudget(const uint64_t nOwnLimit, std::atomic<int64_t>* pSharedPool)
	{ m_nByteLimit = nOwnLimit; m_pSharedPool = pSharedPool; }

private:
	static unsigned long __stdcall ThreadProc(void* pParam);
	void WriterLoop(void);
	bool DrainOne(void);
	bool WriteRaw(const void* pData, const uint32_t nLen);
	void RingRead(uint64_t nPos, void* pOut, uint32_t nLen) const;
	void RingWrite(uint64_t nPos, const void* pIn, uint32_t nLen);
	void Fail(const char* pszWhy);

	void*    m_hFile;
	void*    m_hThread;
	void*    m_hWake;
	SRWLOCK  m_Lock;

	uint8_t* m_pRing;
	uint64_t m_nRingWritten;
	uint64_t m_nRingRead;

	std::vector<uint8_t>           m_Staging;
	std::vector<R5DemIndexEntry_s> m_Index;

	R5DemHeader_s m_Header;
	uint64_t m_nFileOffset;
	uint64_t m_nOpenQpc;
	uint32_t m_nSeq;
	uint32_t m_nDropped;
	volatile bool m_bStop;
	volatile bool m_bFailed;
	bool     m_bLostSignon;
	bool     m_bFailWarned;

	uint64_t m_nByteLimit;
	std::atomic<int64_t>* m_pSharedPool;

	char m_szFinalPath[260];
	char m_szPartPath[270];
};

// Creates every missing directory of a relative or absolute file path.
bool Demo_CreateDirsForFile(const char* pszPath);

#endif // ENGINE_SHARED_DEMO_WRITER_H
