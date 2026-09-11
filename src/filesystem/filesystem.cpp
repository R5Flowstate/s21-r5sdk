#if defined(CLIENT_DLL)
#include "core/stdafx.h"
#include "filesystem/filesystem.h"

///////////////////////////////////////////////////////////////////////////////
CFileSystem_Stdio** g_pFullFileSystem  = nullptr;
CFileSystem_Stdio* g_pFileSystem_Stdio = nullptr;

CFileSystem_Stdio* FileSystem()
{
	return g_pFileSystem_Stdio;
}

//-----------------------------------------------------------------------------
// Purpose: IBaseFileSystem is the second base (+8). Implicit S3 cast is wrong on S21.
//-----------------------------------------------------------------------------
IBaseFileSystem* BaseFileSystem()
{
	CFileSystem_Stdio* pFS = g_pFullFileSystem ? *g_pFullFileSystem : g_pFileSystem_Stdio;
	if (!pFS)
		return nullptr;
	return reinterpret_cast<IBaseFileSystem*>(reinterpret_cast<uintptr_t>(pFS) + 8);
}

//-----------------------------------------------------------------------------
// Purpose: Read a file via IBaseFileSystem Open/Size/Read/Close (not IFileSystem).
//-----------------------------------------------------------------------------
char* FileSystem_ReadAll(const char* pszFileName, const char* pszPathID, ssize_t* pOutSize)
{
	if (pOutSize)
		*pOutSize = 0;

	IBaseFileSystem* const pFS = BaseFileSystem();
	if (!pFS || !pszFileName)
		return nullptr;

	FileHandle_t hFile = pFS->Open(pszFileName, "rb", pszPathID);
	if (!hFile)
		return nullptr;

	const ssize_t fileSize = pFS->Size(hFile);
	if (fileSize <= 0)
	{
		pFS->Close(hFile);
		return nullptr;
	}

	// +2 for the double null terminator -- parsers that detect Unicode via
	// trailing \0\0 work without special-casing, and strlen-based consumers
	// see a clean C string.
	char* const pBuf = new char[fileSize + 2];

	const ssize_t nRead = pFS->Read(pBuf, fileSize, hFile);
	pFS->Close(hFile);

	if (nRead <= 0)
	{
		delete[] pBuf;
		return nullptr;
	}

	pBuf[fileSize]     = '\0';
	pBuf[fileSize + 1] = '\0';

	if (pOutSize)
		*pOutSize = fileSize;

	return pBuf;
}

//-----------------------------------------------------------------------------
// Purpose: Writes a buffer to a file, overwriting any existing contents.
// Uses only IBaseFileSystem Open/Write/Close -- never the unsafe
// IFileSystem::WriteFile convenience wrapper.
//-----------------------------------------------------------------------------
bool FileSystem_WriteAll(const char* pszFileName, const char* pszPathID, const void* pData, ssize_t size)
{
	IBaseFileSystem* const pFS = BaseFileSystem();
	if (!pFS || !pszFileName || !pData || size <= 0)
		return false;

	FileHandle_t hFile = pFS->Open(pszFileName, "wb", pszPathID);
	if (!hFile)
		return false;

	const ssize_t nWritten = pFS->Write(pData, size, hFile);
	pFS->Close(hFile);

	return nWritten == size;
}
#else // !CLIENT_DLL
#include "core/stdafx.h"
#include "filesystem/filesystem.h"

///////////////////////////////////////////////////////////////////////////////
CFileSystem_Stdio** g_pFullFileSystem  = nullptr;
CFileSystem_Stdio* g_pFileSystem_Stdio = nullptr;

CFileSystem_Stdio* FileSystem()
{
	return g_pFileSystem_Stdio;
}
#endif // CLIENT_DLL
