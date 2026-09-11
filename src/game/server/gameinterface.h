//=============================================================================//
//
// Purpose: Interface server dll virtual functions to the SDK.
//
// $NoKeywords: $
//=============================================================================//
#ifndef GAMEINTERFACE_H
#define GAMEINTERFACE_H
#include "public/eiface.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"

inline ConVar sv_commsBannedClientsCanReceiveComms("sv_commsBannedClientsCanReceiveComms", "0", FCVAR_RELEASE, "Enabling this will allow players who are communication banned to receive communication from other players", false, 0.f, true, 1.f);

//-----------------------------------------------------------------------------
// Forward declarations
//-----------------------------------------------------------------------------
class ServerClass;
class CPlayer;
class IRecipientFilter;
//-----------------------------------------------------------------------------
// Utilities
//-----------------------------------------------------------------------------
int UTIL_GetCommandClientIndex(void);
CPlayer* UTIL_GetCommandClient(void);

//-----------------------------------------------------------------------------
// User Message
//-----------------------------------------------------------------------------
void MessageEnd(void);
void MessageWriteByte(int iValue);
void MessageWriteString(const char* pszString);
void MessageWriteBool(bool bValue);

//-----------------------------------------------------------------------------
// 
//-----------------------------------------------------------------------------
class CServerGameDLL
{
public:
	bool GameInit(void);
	void PrecompileScriptsJob(void);
	void LevelShutdown(void);
	void GameShutdown(void);
	float GetTickInterval(void);
	ServerClass* GetAllServerClasses(void);

public: // Hook statics
	static bool DLLInit(CServerGameDLL* thisptr, CreateInterfaceFn appSystemFactory, CreateInterfaceFn physicsFactory,
		CreateInterfaceFn fileSystemFactory, CGlobalVars* pGlobals);
	static void OnReceivedSayTextMessage(CServerGameDLL* thisptr, int senderId, const char* text, bool isTeamChat);
	static __int64 GameFrame(void* thisptr, unsigned char simulating);
};

//-----------------------------------------------------------------------------
// 
//-----------------------------------------------------------------------------
class CServerGameClients : public IServerGameClients
{
public:
	static void _ProcessUserCmds(CServerGameClients* thisp, edict_t edict, bf_read* buf,
		int numCmds, int totalCmds, int droppedPackets, bool ignore, bool paused);
private:
};

//-----------------------------------------------------------------------------
// 
//-----------------------------------------------------------------------------
class CServerGameEnts : public IServerGameEnts
{
};

//-----------------------------------------------------------------------------
// 
//-----------------------------------------------------------------------------
class CServerRandomStream : public IUniformRandomStream
{
public:
	// Sets the seed of the random number generator
	virtual void	SetSeed(const int iSeed)
	{
		m_random.SetSeed(iSeed);
	}
	virtual int		GetSeed() const
	{
		return m_random.GetSeed();
	}

	// Generates random numbers
	virtual float	RandomFloat(const float flMinVal = 0.0f, const float flMaxVal = 1.0f)
	{
		CheckAndForceScriptError();
		return m_random.RandomFloat(flMinVal, flMaxVal);
	}
	virtual int		RandomInt(const int iMinVal, const int iMaxVal)
	{
		CheckAndForceScriptError();
		return m_random.RandomInt(iMinVal, iMaxVal);
	}
	virtual float	RandomFloatExp(const float flMinVal = 0.0f, const float flMaxVal = 1.0f, const float flExponent = 1.0f)
	{
		CheckAndForceScriptError();
		return m_random.RandomFloatExp(flMinVal, flMaxVal, flExponent);
	}
	virtual int		RandomShortMax()
	{
		CheckAndForceScriptError();
		return m_random.RandomShortMax();
	}

	static inline void CheckAndForceScriptError()
	{
		if (sm_makeInvalid)
			v_SQVM_ScriptError("blah");
	}

	static inline void SetInvalid(const bool invalid)
	{
		sm_makeInvalid = invalid;
	}

private:
	CUniformRandomStream m_random;
	static bool sm_makeInvalid;
};

inline bool(*CServerGameDLL__DLLInit)(CServerGameDLL* thisptr, CreateInterfaceFn appSystemFactory, CreateInterfaceFn physicsFactory, CreateInterfaceFn fileSystemFactory, CGlobalVars* pGlobals);
inline bool(*CServerGameDLL__GameInit)(void);
inline void(*CServerGameDLL__OnReceivedSayTextMessage)(CServerGameDLL* thisptr, int senderId, const char* text, bool isTeamChat);
inline __int64(*CServerGameDLL__GameFrame)(void* thisptr, unsigned char simulating);

inline void(*CServerGameClients__ProcessUserCmds)(CServerGameClients* thisp, edict_t edict, bf_read* buf,
	int numCmds, int totalCmds, int droppedPackets, bool ignore, bool paused);

inline void(*v_DispatchFrameServerJob)(double flFrameTime, bool bRunOverlays, bool bUniformUpdate);
inline void(*v_ExecuteFrameServerJob)(double flFrameTime, bool bRunOverlays, bool bUniformUpdate);
inline void(*v_UserMessageBegin)(IRecipientFilter* filter, const char* pszMessageName, int messageIndex);

inline float* g_pflServerFrameTimeBase = nullptr;
inline bf_write** g_ppUsrMessageBuffer = nullptr;

extern CThreadMutex* g_serverFrameMutex;

extern CServerGameDLL* g_pServerGameDLL;
extern CServerGameClients* g_pServerGameClients;
extern CServerGameEnts* g_pServerGameEntities;
extern CServerRandomStream* g_randomStream;

extern CGlobalVars* gpGlobals;

#if !defined(CLIENT_DLL)
// Runs every SDK per-level reset (the non-vfunc body of
// CServerGameDLL::LevelShutdown). Safe to call from any level-transition path.
void ServerGameDLL_RunSdkLevelReset(const char* pszReason);

// Engine level-init boundary, called from the CreateNetworkStringTables hook.
// Runs the SDK reset if no reset ran since the previous level came up, then
// bumps the level generation and arms for the next level. Returns the new
// generation.
unsigned int ServerGameDLL_OnLevelStringTablesCreated(void);

// Monotonic counter, one increment per level bring-up. Cached engine pointers
// stamp themselves with this so a cross-level stale pointer is detectable.
unsigned int ServerGameDLL_GetLevelGeneration(void);
#endif // !CLIENT_DLL

///////////////////////////////////////////////////////////////////////////////
class VServerGameDLL : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CServerGameDLL::DLLInit", CServerGameDLL__DLLInit);
		LogFunAdr("CServerGameDLL::GameInit", CServerGameDLL__GameInit);
		LogFunAdr("CServerGameDLL::GameFrame", CServerGameDLL__GameFrame);
		LogFunAdr("CServerGameDLL::OnReceivedSayTextMessage", CServerGameDLL__OnReceivedSayTextMessage);
		LogFunAdr("CServerGameClients::ProcessUserCmds", CServerGameClients__ProcessUserCmds);
		LogFunAdr("DispatchFrameServerJob", v_DispatchFrameServerJob);
		LogFunAdr("ExecuteFrameServerJob", v_ExecuteFrameServerJob);
		LogFunAdr("UserMessageBegin", v_UserMessageBegin);
		LogVarAdr("g_flServerFrameTimeBase", g_pflServerFrameTimeBase);
		LogVarAdr("g_ppUsrMessageBuffer", g_ppUsrMessageBuffer);
		LogVarAdr("g_serverFrameMutex", g_serverFrameMutex);
		LogVarAdr("g_pServerGameDLL", g_pServerGameDLL);
		LogVarAdr("g_pServerGameClients", g_pServerGameClients);
		LogVarAdr("g_pServerGameEntities", g_pServerGameEntities);
		LogVarAdr("g_randomStream", g_randomStream);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 41 56 41 57 48 83 EC ?? 48 8B 05 ?? ?? ?? ?? 4C 8D 15").GetPtr(CServerGameDLL__DLLInit);
		Module_FindPattern(g_GameDll, "48 83 EC 28 48 8B 0D ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8B 01 FF 90 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01").GetPtr(CServerGameDLL__GameInit);
		// CServerGameDLL::GameFrame. sub rsp,0B8h plus the pause-gate cmp
		// pins the server half; the client twin does not have this frame.
		Module_FindPattern(g_GameDll,
			"48 8B C4 48 89 48 08 41 54 41 57 48 81 EC B8 00 00 00 80 3D ?? ?? ?? ?? 00")
			.GetPtr(CServerGameDLL__GameFrame);
		Module_FindPattern(g_GameDll, "85 D2 0F 8E ?? ?? ?? ?? 4C 8B DC").GetPtr(CServerGameDLL__OnReceivedSayTextMessage);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 55 41 55 41 57").GetPtr(CServerGameClients__ProcessUserCmds);

		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 57 48 83 EC 30 0F 29 74 24 ?? 48 8D 0D ?? ?? ?? ??").GetPtr(v_DispatchFrameServerJob);
		Module_FindPattern(g_GameDll, "48 89 6C 24 ?? 56 41 54 41 56").GetPtr(v_ExecuteFrameServerJob);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 41 56 48 83 EC 40 41 8B E8").GetPtr(v_UserMessageBegin);
	}
	virtual void GetVar(void) const
	{
		g_pflServerFrameTimeBase = CMemory(CServerGameDLL__GameInit).FindPatternSelf("F3 0F 11 0D").ResolveRelativeAddressSelf(0x4, 0x8).RCast<float*>();
		g_randomStream = CMemory(CServerGameDLL__DLLInit).OffsetSelf(0x130).FindPatternSelf("48 8B").ResolveRelativeAddressSelf(0x3, 0x7).RCast<CServerRandomStream*>();
		CMemory(v_UserMessageBegin).OffsetSelf(0xB1).FindPatternSelf("48 89").ResolveRelativeAddressSelf(0x3, 0x7).GetPtr(g_ppUsrMessageBuffer);
		CMemory(v_DispatchFrameServerJob).FindPatternSelf("48 8D").ResolveRelativeAddressSelf(3, 7).GetPtr(g_serverFrameMutex);
	}
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // GAMEINTERFACE_H
