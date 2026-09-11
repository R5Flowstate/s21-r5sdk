#if defined(CLIENT_DLL)
#pragma once

#define MAX_PERSONA_NAME_LEN 64 // sizeof( g_PersonaName )
#define FAKE_BASE_NUCLEUD_ID 9990000

// Defined (non-static) in core/dllmain.cpp. True when running the DX12 r5apex

bool SDK_IsDx12Exe();

inline void(*EbisuSDK_Tier0_Init)(void);
inline void(*EbisuSDK_CVar_Init)(void);
inline void(*EbisuSDK_RunFrame)(void);
inline const char*(*EbisuSDK_GetLanguage)(void);

inline uint64_t* g_NucleusID = nullptr;
inline char* g_NucleusToken = nullptr; /*SIZE = 1024*/
inline char* g_NucleusTokenClient = nullptr; /*SIZE = 10240*/
inline char* g_OriginAuthCode = nullptr; /*SIZE = 256*/
inline char* g_PersonaName = nullptr; /*SIZE = 64*/
inline int* g_OriginErrorLevel = nullptr;
inline bool* g_EbisuSDKInit = nullptr;
inline bool* g_EbisuProfileInit = nullptr;

///////////////////////////////////////////////////////////////////////////////
void HEbisuSDK_Init();
const char* HEbisuSDK_GetLanguage();

bool IsOriginDisabled();
bool IsOriginInitialized();

// After VEbisuSDK attach: if the RunFrame hook missed, stub the native poll.
void EbisuSDK_StubNativePollIfUnhooked(void);

bool IsValidPersonaName(const char* pszName, int nMinLen, int nMaxLen);

// Signed platform token proving this account to the master server. The getter
// never blocks: the platform mints it on this same thread, so a caller that finds
// it missing must hold its work and retry from the frame loop.
bool EbisuSDK_IsPlatformIdentityExpected();
const char* EbisuSDK_GetPlatformToken();
float EbisuSDK_PlatformIdentityWaitSeconds();

// True when a connect may publish a real account (offline uses the fixed
// sentinel; online needs a live Nucleus id + persona).
bool EbisuSDK_IsConnectIdentityReady();

// Mirrored by ePlatformIdentity in vscripts/ui/bridge_lobby.nut. Keep in step.
enum PlatformIdentityState_t
{
	PLATFORM_IDENTITY_NOT_REQUIRED = 0, // offline, disabled, or unavailable on this build
	PLATFORM_IDENTITY_PENDING = 1,      // being obtained
	PLATFORM_IDENTITY_READY = 2,        // in hand
	PLATFORM_IDENTITY_SLOW = 3,         // overdue, still being retried
};

int EbisuSDK_GetPlatformIdentityState();

///////////////////////////////////////////////////////////////////////////////
// S21 Origin/Nucleus: module-base + DX11/DX12 RVAs. Client token is the JWT.
// EbisuSDK_GetLanguage is not resolved on S21 (hook skipped).
class VEbisuSDK : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("EbisuSDK_Tier0_Init", EbisuSDK_Tier0_Init);
		LogFunAdr("EbisuSDK_CVar_Init", EbisuSDK_CVar_Init);
		LogFunAdr("EbisuSDK_RunFrame", EbisuSDK_RunFrame);
		LogVarAdr("g_NucleusID", g_NucleusID);
		LogVarAdr("g_PersonaName", g_PersonaName);
		LogVarAdr("g_OriginAuthCode", g_OriginAuthCode);
		LogVarAdr("g_NucleusToken", g_NucleusToken);
		LogVarAdr("g_NucleusTokenClient", g_NucleusTokenClient);
		LogVarAdr("g_OriginErrorLevel", g_OriginErrorLevel);
		LogVarAdr("g_EbisuProfileInit", g_EbisuProfileInit);
		LogVarAdr("g_EbisuSDKInit", g_EbisuSDKInit);
	}
	virtual void GetFun(void) const
	{
		const bool bDx12 = SDK_IsDx12Exe();
		const uintptr_t base = g_GameDll.GetModuleBase();
		EbisuSDK_Tier0_Init = reinterpret_cast<void(*)(void)>(base + (bDx12 ? 0x3F4C00 : 0x3D95E0));
		EbisuSDK_CVar_Init  = reinterpret_cast<void(*)(void)>(base + (bDx12 ? 0x3F46C0 : 0x3D90A0));
		EbisuSDK_RunFrame   = reinterpret_cast<void(*)(void)>(base + (bDx12 ? 0x3F7530 : 0x3DBF10));
		// EbisuSDK_GetLanguage: not resolved on S21; left null (hook skipped).
	}
	virtual void GetVar(void) const
	{
		const bool bDx12 = SDK_IsDx12Exe();
		const uintptr_t base = g_GameDll.GetModuleBase();
		g_NucleusID        = reinterpret_cast<uint64_t*>(base + (bDx12 ? 0x54FF860 : 0x954F670));
		g_PersonaName      = reinterpret_cast<char*>(base + (bDx12 ? 0x550CD30 : 0x955CB40));
		g_EbisuSDKInit     = reinterpret_cast<bool*>(base + (bDx12 ? 0x54FF841 : 0x954F651));
		g_EbisuProfileInit = reinterpret_cast<bool*>(base + (bDx12 ? 0x54FF846 : 0x954F656));
		g_OriginErrorLevel = reinterpret_cast<int*>(base + (bDx12 ? 0x54FF85C : 0x954F66C));
		g_OriginAuthCode   = reinterpret_cast<char*>(base + (bDx12 ? 0x5502C30 : 0x9552A40));
		// 10240 bytes here, not the 1024 the declaration carries from the older
		// engine. Only ever read as a C string, so the difference is a bound on
		// how much may be read, never a write.
		g_NucleusToken     = reinterpret_cast<char*>(base + (bDx12 ? 0x5502D30 : 0x9552B40));
		g_NucleusTokenClient = reinterpret_cast<char*>(base + (bDx12 ? 0x5507D30 : 0x9557B40));
	}
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////
#else // !CLIENT_DLL
#pragma once

#define MAX_PERSONA_NAME_LEN 64 // sizeof( g_PersonaName )
#define FAKE_BASE_NUCLEUD_ID 9990000

inline void(*EbisuSDK_Tier0_Init)(void);
inline void(*EbisuSDK_CVar_Init)(void);
inline void(*EbisuSDK_RunFrame)(void);
inline const char*(*EbisuSDK_GetLanguage)(void);

inline uint64_t* g_NucleusID = nullptr;
inline char* g_NucleusToken = nullptr; /*SIZE = 1024*/
inline char* g_OriginAuthCode = nullptr; /*SIZE = 256*/
inline char* g_PersonaName = nullptr; /*SIZE = 64*/
inline int* g_OriginErrorLevel = nullptr;
inline bool* g_EbisuSDKInit = nullptr;
inline bool* g_EbisuProfileInit = nullptr;

///////////////////////////////////////////////////////////////////////////////
void HEbisuSDK_Init();
const char* HEbisuSDK_GetLanguage();

bool IsOriginDisabled();
bool IsOriginInitialized();

bool IsValidPersonaName(const char* pszName, int nMinLen, int nMaxLen);

///////////////////////////////////////////////////////////////////////////////
class VEbisuSDK : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("EbisuSDK_Tier0_Init", EbisuSDK_Tier0_Init);
		LogFunAdr("EbisuSDK_CVar_Init", EbisuSDK_CVar_Init);
		LogFunAdr("EbisuSDK_RunFrame", EbisuSDK_RunFrame);
		LogFunAdr("EbisuSDK_GetLanguage", EbisuSDK_GetLanguage);
		LogVarAdr("g_NucleusID", g_NucleusID);
		LogVarAdr("g_NucleusToken", g_NucleusToken);
		LogVarAdr("g_OriginAuthCode", g_OriginAuthCode);
		LogVarAdr("g_PersonaName", g_PersonaName);
		LogVarAdr("g_OriginErrorLevel", g_OriginErrorLevel);
		LogVarAdr("g_EbisuProfileInit", g_EbisuProfileInit);
		LogVarAdr("g_EbisuSDKInit", g_EbisuSDKInit);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "48 83 EC 28 80 3D ?? ?? ?? ?? ?? 0F 85 ?? 02 ?? ?? 48 89 5C 24 20").GetPtr(EbisuSDK_Tier0_Init);
		Module_FindPattern(g_GameDll, "40 57 48 83 EC 40 83 3D").GetPtr(EbisuSDK_CVar_Init);
		Module_FindPattern(g_GameDll, "48 81 EC ?? ?? ?? ?? 80 3D ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 80 3D ?? ?? ?? ?? ?? 74 5B").GetPtr(EbisuSDK_RunFrame);
		Module_FindPattern(g_GameDll, "48 8B C4 48 81 EC ?? ?? ?? ?? 80 3D ?? ?? ?? ?? ?? 0F 85 ?? ?? ?? ??").GetPtr(EbisuSDK_GetLanguage);
	}
	virtual void GetVar(void) const
	{
		g_NucleusID = CMemory(EbisuSDK_CVar_Init).Offset(0x20).FindPatternSelf("4C 89 05", CMemory::Direction::DOWN, 150).ResolveRelativeAddressSelf(0x3, 0x7).RCast<uint64_t*>();
		g_NucleusToken = CMemory(EbisuSDK_RunFrame).Offset(0x1EF).FindPatternSelf("80 3D", CMemory::Direction::DOWN, 150).ResolveRelativeAddressSelf(0x2, 0x7).RCast<char*>();
		g_OriginAuthCode = CMemory(EbisuSDK_RunFrame).Offset(0x1BF).FindPatternSelf("0F B6", CMemory::Direction::DOWN, 150).ResolveRelativeAddressSelf(0x3, 0x7).RCast<char*>();
		g_PersonaName = CMemory(EbisuSDK_CVar_Init).Offset(0x120).FindPatternSelf("48 8D 0D", CMemory::Direction::DOWN, 150).ResolveRelativeAddressSelf(0x3, 0x7).RCast<char*>();
		g_OriginErrorLevel = CMemory(EbisuSDK_RunFrame).Offset(0x20).FindPatternSelf("89 05", CMemory::Direction::DOWN, 150).ResolveRelativeAddressSelf(0x2, 0x6).RCast<int*>();
		g_EbisuProfileInit = CMemory(EbisuSDK_CVar_Init).Offset(0x12A).FindPatternSelf("C6 05", CMemory::Direction::DOWN, 150).ResolveRelativeAddressSelf(0x2, 0x7).RCast<bool*>();
		g_EbisuSDKInit = CMemory(EbisuSDK_Tier0_Init).Offset(0x0).FindPatternSelf("80 3D", CMemory::Direction::DOWN, 150).ResolveRelativeAddressSelf(0x2, 0x7).RCast<bool*>();
	}
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////
#endif // CLIENT_DLL
