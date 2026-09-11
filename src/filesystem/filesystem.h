#if defined(CLIENT_DLL)
#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include "filesystem/ifilesystem.h"
#include "filesystem/ifile.h"
#include "filesystem/basefilesystem.h"

class CFileSystem_Stdio : public CBaseFileSystem
{
protected:
	// implementation of CBaseFileSystem virtual functions
	virtual FILE* FS_fopen(const char* filename, const char* options, unsigned flags, int64* size) = 0;
	virtual void FS_setbufsize(FILE* fp, unsigned nBytes) = 0;
	virtual void FS_fclose(FILE* fp) = 0;
	virtual void FS_fseek(FILE* fp, int64 pos, int seekType) = 0;
	virtual long FS_ftell(FILE* fp) = 0;
	virtual int FS_feof(FILE* fp) = 0;
	virtual size_t FS_fread(void* dest, size_t destSize, size_t size, FILE* fp) = 0;
	virtual size_t FS_fwrite(const void* src, size_t size, FILE* fp) = 0;
	virtual bool FS_setmode(FILE* fp, FileMode_t mode) = 0;
	virtual size_t FS_vfprintf(FILE* fp, const char* fmt, va_list list) = 0;
	virtual int FS_ferror(FILE* fp) = 0;
	virtual int FS_fflush(FILE* fp) = 0;
	virtual char* FS_fgets(char* dest, unsigned int destSize) = 0;
	virtual int FS_stat(const char* path, struct _stat* buf, bool* pbLoadedFromSteamCache = NULL) = 0;
	virtual int FS_chmod(const char* path, int pmode) = 0;
	virtual HANDLE FS_FindFirstFile(const char* findname, WIN32_FIND_DATA* dat) = 0;
	virtual bool FS_FindNextFile(HANDLE handle, WIN32_FIND_DATA* dat) = 0;
	virtual bool FS_FindClose(HANDLE handle) = 0;
	virtual int FS_GetSectorSize(FILE*) = 0;
};

extern CFileSystem_Stdio** g_pFullFileSystem; // Ptr to g_pFileSystem_Stdio.
extern CFileSystem_Stdio*  g_pFileSystem_Stdio;

//-----------------------------------------------------------------------------
// Singleton FileSystem
//-----------------------------------------------------------------------------
extern CFileSystem_Stdio* FileSystem();

// IBaseFileSystem* with +8 secondary-base adjust. Do not implicit-cast CFileSystem_Stdio*.
extern IBaseFileSystem* BaseFileSystem();

//-----------------------------------------------------------------------------
// Open+Read/Write+Close via IBaseFileSystem. ReadAll buffer has two trailing NULs.
//-----------------------------------------------------------------------------
extern char* FileSystem_ReadAll(const char* pszFileName, const char* pszPathID, ssize_t* pOutSize);
extern bool  FileSystem_WriteAll(const char* pszFileName, const char* pszPathID, const void* pData, ssize_t size);

///////////////////////////////////////////////////////////////////////////////
class VFileSystem_Stdio : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogVarAdr("g_pFullFileSystem", g_pFullFileSystem);
		LogVarAdr("g_pFileSystem_Stdio", g_pFileSystem_Stdio);
	}
	virtual void GetFun(void) const { }
	virtual void GetVar(void) const
	{
		g_pFullFileSystem = Module_FindPattern(g_GameDll, "48 8B 0D ?? ?? ?? ?? 45 33 C0 48 83 C1 08 48 8B 01").ResolveRelativeAddressSelf(0x3, 0x7).RCast<CFileSystem_Stdio**>();

		g_pFileSystem_Stdio = Module_FindPattern(g_GameDll, "48 8D 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? E9 ?? ?? ?? ??")
			.FindPattern("48 89", CMemory::Direction::DOWN, 512, 1).ResolveRelativeAddressSelf(0x3, 0x7).RCast<CFileSystem_Stdio*>();
	}
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const { }
};
///////////////////////////////////////////////////////////////////////////////

#endif // !FILESYSTEM_H
#else // !CLIENT_DLL
#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include "filesystem/ifilesystem.h"
#include "filesystem/ifile.h"
#include "filesystem/basefilesystem.h"

class CFileSystem_Stdio : public CBaseFileSystem
{
protected:
	// implementation of CBaseFileSystem virtual functions
	virtual FILE* FS_fopen(const char* filename, const char* options, unsigned flags, int64* size) = 0;
	virtual void FS_setbufsize(FILE* fp, unsigned nBytes) = 0;
	virtual void FS_fclose(FILE* fp) = 0;
	virtual void FS_fseek(FILE* fp, int64 pos, int seekType) = 0;
	virtual long FS_ftell(FILE* fp) = 0;
	virtual int FS_feof(FILE* fp) = 0;
	virtual size_t FS_fread(void* dest, size_t destSize, size_t size, FILE* fp) = 0;
	virtual size_t FS_fwrite(const void* src, size_t size, FILE* fp) = 0;
	virtual bool FS_setmode(FILE* fp, FileMode_t mode) = 0;
	virtual size_t FS_vfprintf(FILE* fp, const char* fmt, va_list list) = 0;
	virtual int FS_ferror(FILE* fp) = 0;
	virtual int FS_fflush(FILE* fp) = 0;
	virtual char* FS_fgets(char* dest, unsigned int destSize) = 0;
	virtual int FS_stat(const char* path, struct _stat* buf, bool* pbLoadedFromSteamCache = NULL) = 0;
	virtual int FS_chmod(const char* path, int pmode) = 0;
	virtual HANDLE FS_FindFirstFile(const char* findname, WIN32_FIND_DATA* dat) = 0;
	virtual bool FS_FindNextFile(HANDLE handle, WIN32_FIND_DATA* dat) = 0;
	virtual bool FS_FindClose(HANDLE handle) = 0;
	virtual int FS_GetSectorSize(FILE*) = 0;
};

extern CFileSystem_Stdio** g_pFullFileSystem; // Ptr to g_pFileSystem_Stdio.
extern CFileSystem_Stdio*  g_pFileSystem_Stdio;

//-----------------------------------------------------------------------------
// Singleton FileSystem
//-----------------------------------------------------------------------------
extern CFileSystem_Stdio* FileSystem();

///////////////////////////////////////////////////////////////////////////////
class VFileSystem_Stdio : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogVarAdr("g_pFullFileSystem", g_pFullFileSystem);
		LogVarAdr("g_pFileSystem_Stdio", g_pFileSystem_Stdio);
	}
	virtual void GetFun(void) const { }
	virtual void GetVar(void) const
	{
		g_pFullFileSystem = Module_FindPattern(g_GameDll, "48 8B 0D ?? ?? ?? ?? 45 33 C0 48 83 C1 08 48 8B 01").ResolveRelativeAddressSelf(0x3, 0x7).RCast<CFileSystem_Stdio**>();

		g_pFileSystem_Stdio = Module_FindPattern(g_GameDll, "48 8D 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? E9 ?? ?? ?? ??")
			.FindPattern("48 89", CMemory::Direction::DOWN, 512, 1).ResolveRelativeAddressSelf(0x3, 0x7).RCast<CFileSystem_Stdio*>();
	}
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const { }
};
///////////////////////////////////////////////////////////////////////////////

#endif // !FILESYSTEM_H
#endif // CLIENT_DLL
