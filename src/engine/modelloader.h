#if defined(CLIENT_DLL)
#pragma once
#include "engine/gl_model_private.h"
#include "public/bspfile.h"
// v_HashNameAligned / v_HashNameUnaligned live here (shared with the datatable
// resolver; both anchor the same engine functions).
#include "rtech/datatable/datatable.h"

//-----------------------------------------------------------------------------
// Purpose
//-----------------------------------------------------------------------------
class IModelLoader
{
public:
	enum REFERENCETYPE
	{
		// The name is allocated, but nothing else is in memory or being referenced
		FMODELLOADER_NOTLOADEDORREFERENCED = 0,
		// The model has been loaded into memory
		FMODELLOADER_LOADED = (1 << 0),

		// The model is being referenced by the server code
		FMODELLOADER_SERVER = (1 << 1),
		// The model is being referenced by the client code
		FMODELLOADER_CLIENT = (1 << 2),
		// The model is being referenced in the client.dll
		FMODELLOADER_CLIENTDLL = (1 << 3),
		// The model is being referenced by static props
		FMODELLOADER_STATICPROP = (1 << 4),
		// The model is a detail prop
		FMODELLOADER_DETAILPROP = (1 << 5),
		// The model is the simple version of the world geometry
		FMODELLOADER_SIMPLEWORLD = (1 << 6),
		// The model is dynamically loaded
		FMODELLOADER_DYNSERVER = (1 << 7),
		FMODELLOADER_DYNCLIENT = (1 << 8),
		FMODELLOADER_COMBINED = (1 << 9),
		FMODELLOADER_DYNAMIC = FMODELLOADER_DYNSERVER | FMODELLOADER_DYNCLIENT | FMODELLOADER_COMBINED,

		FMODELLOADER_REFERENCEMASK = (FMODELLOADER_SERVER | FMODELLOADER_CLIENT | FMODELLOADER_CLIENTDLL | FMODELLOADER_STATICPROP | FMODELLOADER_DETAILPROP | FMODELLOADER_DYNAMIC | FMODELLOADER_SIMPLEWORLD),

		// The model was touched by the preload method
		FMODELLOADER_TOUCHED_BY_PRELOAD = (1 << 15),
		// The model was loaded by the preload method, a postload fixup is required
		FMODELLOADER_LOADED_BY_PRELOAD = (1 << 16),
		// The model touched its materials as part of its load
		FMODELLOADER_TOUCHED_MATERIALS = (1 << 17),
	};
};

class CModelLoader
{
public:
	static void LoadModel(CModelLoader* loader, model_t* model);
	static uint64_t Map_LoadModelGuts(CModelLoader* loader, model_t* model);
};

class CMapLoadHelper
{
public:
	static void Constructor(CMapLoadHelper* helper, int lumpToLoad);
	static void Init(void* pMapModel, const char* loadname);

public:
	int m_nLumpSize;
	int m_nLumpOffset;
	int m_nLumpVersion;
	byte* m_pRawData;
	byte* m_pData;
	byte* m_pUncompressedData;
	int m_nUncompressedLumpSize;
	bool m_bUncompressedDataExternal;
	bool m_bExternal;
	bool m_bUnk;
	int m_nLumpID;
	char m_szLumpFilename[MAX_OSPATH];
};

inline void*(*CModelLoader__FindModel)(CModelLoader* loader, const char* pszModelName);
inline void(*CModelLoader__LoadModel)(CModelLoader* loader, model_t* model);
inline uint64_t(*CModelLoader__UnloadModel)(CModelLoader* loader, model_t* model);
inline void*(*CModelLoader__Studio_LoadModel)(CModelLoader* loader);
inline uint64_t(*CModelLoader__Map_LoadModelGuts)(CModelLoader* loader, model_t* model);
inline bool(*CModelLoader__Map_IsValid)(CModelLoader* loader, const char* pszMapName);
inline void(*CMapLoadHelper__CMapLoadHelper)(CMapLoadHelper * helper, int lumpToLoad);
inline void(*CMapLoadHelper__Init)(void* pMapModel, const char* loadname);
inline const char*(*CMapLoadHelper__GetDiskName)(void);
inline __int64(*Mod_LoadCubemapArray)(void* pMap, const char* loadName, void* samplePoints, const float* ambientRcp, int sampleCount);
inline void*(*v_Pak_FindAssetVoid)(uint64_t nGuid, void* pOutHandle);
inline uint8_t* s_mapCubemapSet; // TextureAsset*, samples, ambient, count @+0x18

inline void(*v_AddGameLump)(void);
inline void(*v_Map_LoadModel)(void);

inline void*(*v_GetSpriteInfo)(const char* pName, bool bIsAVI, bool bIsBIK, int& nWidth, int& nHeight, int& nFrameCount, void* a7);
inline void*(*v_BuildSpriteLoadName)(const char* pName, char* pOut, int outLen, bool& bIsAVI, bool& bIsBIK);

inline CModelLoader* g_pModelLoader;
inline FileHandle_t* s_MapFileHandle;
inline BSPHeader_t* s_MapHeader;
inline char* s_szMapPathName; /*size = 260*/
inline bool* s_UsingMapFileLumps; // per-lump-file mode flag (bit0 of the.bsp header flags)
inline lump_t* s_GameLump;        // s_MapHeader copy of the GAME_LUMP entry
inline int* s_MapLoadCount;       // CMapLoadHelper::Init ref-count (setup runs at 1)

///////////////////////////////////////////////////////////////////////////////
class VModelLoader : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CModelLoader::FindModel", CModelLoader__FindModel);
		LogFunAdr("CModelLoader::LoadModel", CModelLoader__LoadModel);
		LogFunAdr("CModelLoader::UnloadModel", CModelLoader__UnloadModel);
		LogFunAdr("CModelLoader::Map_LoadModelGuts", CModelLoader__Map_LoadModelGuts);
		LogFunAdr("CModelLoader::Map_IsValid", CModelLoader__Map_IsValid);
		LogFunAdr("CModelLoader::Studio_LoadModel", CModelLoader__Studio_LoadModel);

		LogFunAdr("CMapLoadHelper::CMapLoadHelper", CMapLoadHelper__CMapLoadHelper);
		LogFunAdr("CMapLoadHelper::Init", CMapLoadHelper__Init);
		LogFunAdr("CMapLoadHelper::GetDiskName", CMapLoadHelper__GetDiskName);
		LogFunAdr("Mod_LoadCubemapArray", Mod_LoadCubemapArray);
		LogFunAdr("HashNameAligned", v_HashNameAligned);
		LogFunAdr("Pak_FindAssetVoid", v_Pak_FindAssetVoid);

		LogFunAdr("AddGameLump", v_AddGameLump);
		LogFunAdr("Map_LoadModel", v_Map_LoadModel);

		LogFunAdr("GetSpriteInfo", v_GetSpriteInfo);
		LogFunAdr("BuildSpriteLoadName", v_BuildSpriteLoadName);

		LogVarAdr("g_pModelLoader", g_pModelLoader);
		LogVarAdr("s_MapFileHandle", s_MapFileHandle);
		LogVarAdr("s_MapHeader", s_MapHeader);
		LogVarAdr("s_szMapPathName", s_szMapPathName);
		LogVarAdr("s_UsingMapFileLumps", s_UsingMapFileLumps);
		LogVarAdr("s_GameLump", s_GameLump);
		LogVarAdr("s_MapLoadCount", s_MapLoadCount);
		LogVarAdr("s_mapCubemapSet", s_mapCubemapSet);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "40 55 41 57 48 83 EC 48 80 3A 2A").GetPtr(CModelLoader__FindModel);
		Module_FindPattern(g_GameDll, "40 53 57 41 57 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ??").GetPtr(CModelLoader__LoadModel);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 6C 24 ?? 57 48 81 EC ?? ?? ?? ?? 48 8B F9 33 ED").GetPtr(CModelLoader__UnloadModel);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 55 56 57 41 54 41 57 48 81 EC ?? ?? ?? ??").GetPtr(CModelLoader__Studio_LoadModel);
		Module_FindPattern(g_GameDll, "48 89 54 24 ?? 48 89 4C 24 ?? 55 53 56 57 41 54 41 55 41 57").GetPtr(CModelLoader__Map_LoadModelGuts); // BSP.
		Module_FindPattern(g_GameDll, "40 53 48 81 EC ?? ?? ?? ?? 48 8B DA 48 85 D2 0F 84 ?? ?? ?? ?? 80 3A ?? 0F 84 ?? ?? ?? ?? 4C 8B CA").GetPtr(CModelLoader__Map_IsValid);

		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 41 54 41 55 41 56 41 57 48 83 EC 30 4C 8B BC 24 ?? ?? ?? ??").GetPtr(v_GetSpriteInfo);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 41 56 48 81 EC ?? ?? ?? ?? 4D 8B F1 48 8B F2").GetPtr(v_BuildSpriteLoadName);
		
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 7C 24 ?? 41 56 48 81 EC 60").GetPtr(CMapLoadHelper__CMapLoadHelper);
		Module_FindPattern(g_GameDll, "4C 8B DC 53 48 81 EC 40 01 00 00 8B 05 ?? ?? ?? ?? 48 8B DA FF C0 89 05").GetPtr(CMapLoadHelper__Init);
		Module_FindPattern(g_GameDll, "48 89 6C 24 ?? 56 48 83 EC 20 48 C7 C5 FF FF FF FF 48 89 7C 24").GetPtr(CMapLoadHelper__GetDiskName);
		// Mod_LoadCubemapArray: unique prologue (push rbx/rbp/rsi/rdi/r12-r15 + 0x230 frame).
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 41 54 41 55 41 56 41 57 48 81 EC ?? ?? ?? ?? 8B AC 24")
			.GetPtr(Mod_LoadCubemapArray);
		if (Mod_LoadCubemapArray)
		{
			CMemory(Mod_LoadCubemapArray).Offset(0x15D).ResolveRelativeAddress(3, 7).GetPtr(v_HashNameAligned);
			CMemory(Mod_LoadCubemapArray).Offset(0x164).ResolveRelativeAddress(3, 7).GetPtr(v_HashNameUnaligned);
			CMemory(Mod_LoadCubemapArray).Offset(0x118).FollowNearCallSelf().GetPtr(v_Pak_FindAssetVoid);
		}
		Module_FindPattern(g_GameDll, "40 ?? 57 48 83 EC 48 33 ?? 48 8D").GetPtr(v_AddGameLump);
		Module_FindPattern(g_GameDll, "48 83 EC 28 8B 05 ?? ?? ?? ?? FF C8").GetPtr(v_Map_LoadModel);
	}
	virtual void GetVar(void) const
	{
		// g_pModelLoader / s_szMapPathName unresolved -- GetDiskName is the map path.
		// s_MapFileHandle / s_MapHeader come from v_Map_LoadModel.
		s_MapFileHandle = CMemory(v_Map_LoadModel).FindPattern("48 8B").ResolveRelativeAddressSelf(0x3, 0x7).RCast<FileHandle_t*>();
		s_MapHeader = CMemory(v_Map_LoadModel).FindPattern("48 8D").ResolveRelativeAddressSelf(0x3, 0x7).RCast<BSPHeader_t*>();

		// s_UsingMapFileLumps: the 'cmp [rip+s_UsingMapFileLumps], bpl' inside the
		// CMapLoadHelper ctor that selects the per-lump-file path.
		s_UsingMapFileLumps = Module_FindPattern(g_GameDll, "40 38 2D ?? ?? ?? ?? 74 ?? E8 ?? ?? ?? ?? 4C 8B C8").ResolveRelativeAddressSelf(0x3, 0x7).RCast<bool*>();
		// s_GameLump: the 'movups [rip+s_GameLump], xmm0' store in CMapLoadHelper::Init.
		s_GameLump = Module_FindPattern(g_GameDll, "0F 28 05 ?? ?? ?? ?? 0F 11 05 ?? ?? ?? ?? F6 C2 02").OffsetSelf(0x7).ResolveRelativeAddressSelf(0x3, 0x7).RCast<lump_t*>();
		// s_MapLoadCount: the 'mov eax, [rip+s_MapLoadCount]' ref-count load at the
		// top of CMapLoadHelper::Init (the setup body runs only when it reaches 1).
		s_MapLoadCount = CMemory(CMapLoadHelper__Init).FindPattern("8B 05").ResolveRelativeAddressSelf(0x2, 0x6).RCast<int*>();
		if (Mod_LoadCubemapArray)
			s_mapCubemapSet = CMemory(Mod_LoadCubemapArray).Offset(0x1A0).ResolveRelativeAddress(3, 7).RCast<uint8_t*>();
	}
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////
#else // !CLIENT_DLL
#pragma once
#include "engine/gl_model_private.h"
#include "public/bspfile.h"

//-----------------------------------------------------------------------------
// Purpose
//-----------------------------------------------------------------------------
class IModelLoader
{
public:
	enum REFERENCETYPE
	{
		// The name is allocated, but nothing else is in memory or being referenced
		FMODELLOADER_NOTLOADEDORREFERENCED = 0,
		// The model has been loaded into memory
		FMODELLOADER_LOADED = (1 << 0),

		// The model is being referenced by the server code
		FMODELLOADER_SERVER = (1 << 1),
		// The model is being referenced by the client code
		FMODELLOADER_CLIENT = (1 << 2),
		// The model is being referenced in the client.dll
		FMODELLOADER_CLIENTDLL = (1 << 3),
		// The model is being referenced by static props
		FMODELLOADER_STATICPROP = (1 << 4),
		// The model is a detail prop
		FMODELLOADER_DETAILPROP = (1 << 5),
		// The model is the simple version of the world geometry
		FMODELLOADER_SIMPLEWORLD = (1 << 6),
		// The model is dynamically loaded
		FMODELLOADER_DYNSERVER = (1 << 7),
		FMODELLOADER_DYNCLIENT = (1 << 8),
		FMODELLOADER_COMBINED = (1 << 9),
		FMODELLOADER_DYNAMIC = FMODELLOADER_DYNSERVER | FMODELLOADER_DYNCLIENT | FMODELLOADER_COMBINED,

		FMODELLOADER_REFERENCEMASK = (FMODELLOADER_SERVER | FMODELLOADER_CLIENT | FMODELLOADER_CLIENTDLL | FMODELLOADER_STATICPROP | FMODELLOADER_DETAILPROP | FMODELLOADER_DYNAMIC | FMODELLOADER_SIMPLEWORLD),

		// The model was touched by the preload method
		FMODELLOADER_TOUCHED_BY_PRELOAD = (1 << 15),
		// The model was loaded by the preload method, a postload fixup is required
		FMODELLOADER_LOADED_BY_PRELOAD = (1 << 16),
		// The model touched its materials as part of its load
		FMODELLOADER_TOUCHED_MATERIALS = (1 << 17),
	};
};

class CModelLoader
{
public:
	static void LoadModel(CModelLoader* loader, model_t* model);
	static uint64_t Map_LoadModelGuts(CModelLoader* loader, model_t* model);
};

class CMapLoadHelper
{
public:
	static void Constructor(CMapLoadHelper* helper, int lumpToLoad);

public:
	int m_nLumpSize;
	int m_nLumpOffset;
	int m_nLumpVersion;
	byte* m_pRawData;
	byte* m_pData;
	byte* m_pUncompressedData;
	int m_nUncompressedLumpSize;
	bool m_bUncompressedDataExternal;
	bool m_bExternal;
	bool m_bUnk;
	int m_nLumpID;
	char m_szLumpFilename[MAX_OSPATH];
};

inline void*(*CModelLoader__FindModel)(CModelLoader* loader, const char* pszModelName);
inline void(*CModelLoader__LoadModel)(CModelLoader* loader, model_t* model);
inline uint64_t(*CModelLoader__UnloadModel)(CModelLoader* loader, model_t* model);
inline void*(*CModelLoader__Studio_LoadModel)(CModelLoader* loader);
inline uint64_t(*CModelLoader__Map_LoadModelGuts)(CModelLoader* loader, model_t* model);
inline bool(*CModelLoader__Map_IsValid)(CModelLoader* loader, const char* pszMapName);
inline void(*CMapLoadHelper__CMapLoadHelper)(CMapLoadHelper * helper, int lumpToLoad);

inline void(*v_AddGameLump)(void);
inline void(*v_Map_LoadModel)(void);

#ifndef DEDICATED
inline void*(*v_GetSpriteInfo)(const char* pName, bool bIsAVI, bool bIsBIK, int& nWidth, int& nHeight, int& nFrameCount, void* a7);
inline void*(*v_BuildSpriteLoadName)(const char* pName, char* pOut, int outLen, bool& bIsAVI, bool& bIsBIK);
#endif // !DEDICATED

inline CModelLoader* g_pModelLoader;
inline FileHandle_t* s_MapFileHandle;
inline BSPHeader_t* s_MapHeader;
inline char* s_szMapPathName; /*size = 260*/

///////////////////////////////////////////////////////////////////////////////
class VModelLoader : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CModelLoader::FindModel", CModelLoader__FindModel);
		LogFunAdr("CModelLoader::LoadModel", CModelLoader__LoadModel);
		LogFunAdr("CModelLoader::UnloadModel", CModelLoader__UnloadModel);
		LogFunAdr("CModelLoader::Map_LoadModelGuts", CModelLoader__Map_LoadModelGuts);
		LogFunAdr("CModelLoader::Map_IsValid", CModelLoader__Map_IsValid);
		LogFunAdr("CModelLoader::Studio_LoadModel", CModelLoader__Studio_LoadModel);

		LogFunAdr("CMapLoadHelper::CMapLoadHelper", CMapLoadHelper__CMapLoadHelper);

		LogFunAdr("AddGameLump", v_AddGameLump);
		LogFunAdr("Map_LoadModel", v_Map_LoadModel);

#ifndef DEDICATED
		LogFunAdr("GetSpriteInfo", v_GetSpriteInfo);
		LogFunAdr("BuildSpriteLoadName", v_BuildSpriteLoadName);
#endif // !DEDICATED

		LogVarAdr("g_pModelLoader", g_pModelLoader);
		LogVarAdr("s_MapFileHandle", s_MapFileHandle);
		LogVarAdr("s_MapHeader", s_MapHeader);
		LogVarAdr("s_szMapPathName", s_szMapPathName);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "40 55 41 57 48 83 EC 48 80 3A 2A").GetPtr(CModelLoader__FindModel);
		Module_FindPattern(g_GameDll, "40 53 57 41 57 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ??").GetPtr(CModelLoader__LoadModel);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 6C 24 ?? 57 48 81 EC ?? ?? ?? ?? 48 8B F9 33 ED").GetPtr(CModelLoader__UnloadModel);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 55 56 57 41 54 41 57 48 81 EC ?? ?? ?? ??").GetPtr(CModelLoader__Studio_LoadModel);
		Module_FindPattern(g_GameDll, "48 89 54 24 ?? 48 89 4C 24 ?? 55 53 56 57 41 54 41 55 41 57").GetPtr(CModelLoader__Map_LoadModelGuts); // BSP.
		Module_FindPattern(g_GameDll, "40 53 48 81 EC ?? ?? ?? ?? 48 8B DA 48 85 D2 0F 84 ?? ?? ?? ?? 80 3A ?? 0F 84 ?? ?? ?? ?? 4C 8B CA").GetPtr(CModelLoader__Map_IsValid);

#ifndef DEDICATED
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 41 54 41 55 41 56 41 57 48 83 EC 30 4C 8B BC 24 ?? ?? ?? ??").GetPtr(v_GetSpriteInfo);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 41 56 48 81 EC ?? ?? ?? ?? 4D 8B F1 48 8B F2").GetPtr(v_BuildSpriteLoadName);
#endif // !DEDICATED
		
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 7C 24 ?? 41 56 48 81 EC 60").GetPtr(CMapLoadHelper__CMapLoadHelper);
		Module_FindPattern(g_GameDll, "40 ?? 57 48 83 EC 48 33 ?? 48 8D").GetPtr(v_AddGameLump);
		Module_FindPattern(g_GameDll, "48 83 EC 28 8B 05 ?? ?? ?? ?? FF C8").GetPtr(v_Map_LoadModel);
	}
	virtual void GetVar(void) const
	{
		g_pModelLoader = Module_FindPattern(g_GameDll, 
			"48 89 4C 24 ?? 53 55 56 41 54 41 55 41 56 41 57 48 81 EC ?? ?? ?? ??").FindPatternSelf("48 ?? 0D", CMemory::Direction::DOWN).ResolveRelativeAddressSelf(3, 7).RCast<CModelLoader*>();

		s_MapFileHandle = CMemory(v_Map_LoadModel).FindPattern("48 8B").ResolveRelativeAddressSelf(0x3, 0x7).RCast<FileHandle_t*>();
		s_MapHeader = CMemory(v_Map_LoadModel).FindPattern("48 8D").ResolveRelativeAddressSelf(0x3, 0x7).RCast<BSPHeader_t*>();
		s_szMapPathName = CMemory(CMapLoadHelper__CMapLoadHelper).FindPattern("4C 8D").ResolveRelativeAddressSelf(0x3, 0x7).RCast<char*>();
	}
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////
#endif // CLIENT_DLL
