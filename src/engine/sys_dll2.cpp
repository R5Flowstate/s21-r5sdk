#if defined(CLIENT_DLL)
//===== Copyright © 1996-2005, Valve Corporation, All rights reserved. ======//
//
// Purpose
//
// $NoKeywords: $
//===========================================================================//

#include "core/stdafx.h"
#include "tier0/commandline.h"
#include "tier1/cmd.h"
#include "tier1/cvar.h"
#include "tier1/strtools.h"
#include "engine/sys_engine.h"
#include "engine/sys_dll.h"
#include "engine/sys_dll2.h"
#include "engine/host_cmd.h"
#include "engine/traceinit.h"
#include "engine/sys_mainwind.h"
#include "inputsystem/inputsystem.h"
#include "vgui/vgui_baseui_interface.h"
#include "materialsystem/cmaterialsystem.h"
#include "windows/id3dx.h"
#include "client/vengineclient_impl.h"
#include "filesystem/filesystem.h"
constexpr char DFS_ENABLE_PATH[] = "/vpk/enable.txt";

//-----------------------------------------------------------------------------
// Figure out if we're running a Valve mod or not.
//-----------------------------------------------------------------------------
static bool IsValveMod(const char* pModName)
{
	return (Q_stricmp(pModName, "cstrike") == 0 ||
		Q_stricmp(pModName, "dod") == 0 ||
		Q_stricmp(pModName, "hl1mp") == 0 ||
		Q_stricmp(pModName, "tf") == 0 ||
		Q_stricmp(pModName, "hl2mp") == 0 ||
		Q_stricmp(pModName, "csgo") == 0);
}

//-----------------------------------------------------------------------------
// Figure out if we're running a Respawn mod or not.
//-----------------------------------------------------------------------------
static bool IsRespawnMod(const char* pModName)
{
	return (Q_stricmp(pModName, "r1") == 0 ||
		Q_stricmp(pModName, "r2") == 0 ||
		Q_stricmp(pModName, "r5") == 0);
}

//-----------------------------------------------------------------------------
// Initialize the VPK and file cache system
//-----------------------------------------------------------------------------
static void InitVPKSystem()
{
    char szCacheEnableFilePath[MAX_OSPATH];
    char bFixSlashes = FileSystem()->GetCurrentDirectory(szCacheEnableFilePath, sizeof(szCacheEnableFilePath)) ? szCacheEnableFilePath[0] : '\0';

    size_t nCachePathLen = strlen(szCacheEnableFilePath);
    size_t nCacheFileLen = sizeof(DFS_ENABLE_PATH)-1;

    if ((nCachePathLen + nCacheFileLen) < MAX_OSPATH || (nCacheFileLen = (MAX_OSPATH-1) - nCachePathLen, nCachePathLen != (MAX_OSPATH-1)))
    {
        strncat(szCacheEnableFilePath, DFS_ENABLE_PATH, nCacheFileLen)[sizeof(szCacheEnableFilePath)-1] = '\0';
        bFixSlashes = szCacheEnableFilePath[0];
    }
    if (bFixSlashes)
    {
        V_FixSlashes(szCacheEnableFilePath, '/');
    }
    if (!CommandLine()->CheckParm("-novpk") && FileSystem()->FileExists(szCacheEnableFilePath, nullptr))
    {
        FileSystem()->AddSearchPath(".", "MAIN", SearchPathAdd_t::PATH_ADD_TO_TAIL);
        FileSystem()->SetVPKCacheModeClient();
        FileSystem()->MountVPKFile("vpk/client_frontend.bsp");
    }
}

InitReturnVal_t CEngineAPI::VInit(CEngineAPI* pEngineAPI)
{
    return CEngineAPI__Init(pEngineAPI);
}

//-----------------------------------------------------------------------------
// Initialization, shutdown of a mod.
//-----------------------------------------------------------------------------
bool CEngineAPI::VModInit(CEngineAPI* pEngineAPI, const char* pModName, const char* pGameDir)
{
    // Register new Pak Assets here!
    //RTech_RegisterAsset(0, 1, "", nullptr, nullptr, nullptr, CMemory(0x1660AD0A8).RCast<void**>, 8, 8, 8, 0, 0xFFFFFFC);

	const bool results = CEngineAPI__ModInit(pEngineAPI, pModName, pGameDir);
	if (!IsValveMod(pModName) && !IsRespawnMod(pModName))
	{
		g_pEngineClient->SetRestrictServerCommands(true); // Restrict server commands.
		g_pEngineClient->SetRestrictClientCommands(true); // Restrict client commands.
	}

	return results;
}

//-----------------------------------------------------------------------------
// One-time setup, based on the initially selected mod
//-----------------------------------------------------------------------------
bool CEngineAPI::OnStartup(CEngineAPI* pEngineAPI, void* pInstance, const char* pStartupModName)
{
	const bool results =  CEngineAPI__OnStartup(pEngineAPI, pInstance, pStartupModName);
	return results;
}

//-----------------------------------------------------------------------------
// Sets startup info
//-----------------------------------------------------------------------------
void CEngineAPI::VSetStartupInfo(CEngineAPI* pEngineAPI, StartupInfo_t* pStartupInfo)
{
    if (*g_bStartupInfoSet)
    {
        return;
    }

    const size_t nBufLen = sizeof(pStartupInfo->m_szBaseDirectory);
    strncpy(g_szBaseDir, pStartupInfo->m_szBaseDirectory, nBufLen);

    g_pEngineParms->baseDirectory = g_szBaseDir;
    g_szBaseDir[nBufLen-1] = '\0';

    pEngineAPI->m_StartupInfo = *pStartupInfo;
    InitVPKSystem();

    v_TRACEINIT(NULL, "COM_InitFilesystem( m_StartupInfo.m_szInitialMod )", "COM_ShutdownFileSystem()");
    v_COM_InitFilesystem(pEngineAPI->m_StartupInfo.m_szInitialMod);

    *g_bStartupInfoSet = true;
}

//-----------------------------------------------------------------------------
// Purpose
//-----------------------------------------------------------------------------
void CEngineAPI::PumpMessages()
{
	CEngineAPI__PumpMessages();
}

//-----------------------------------------------------------------------------
// Purpose
//-----------------------------------------------------------------------------
bool CEngineAPI::MainLoop()
{
    // Main message pump
    while (true)
    {
        // Pump messages unless someone wants to quit
        if (g_pEngine->GetQuitting() != IEngine::QUIT_NOTQUITTING)
        {
            if (g_pEngine->GetQuitting() != IEngine::QUIT_TODESKTOP) {
                return true;
            }

            return false;
        }

        CEngineAPI::PumpMessages();

        g_pEngine->Frame();
    }
}
#else // !CLIENT_DLL
//===== Copyright © 1996-2005, Valve Corporation, All rights reserved. ======//
//
// Purpose
//
// $NoKeywords: $
//===========================================================================//

#include "core/stdafx.h"
#include "tier0/commandline.h"
#include "tier1/cmd.h"
#include "tier1/cvar.h"
#include "tier1/strtools.h"
#include "engine/sys_engine.h"
#include "engine/sys_dll.h"
#include "engine/sys_dll2.h"
#include "engine/host_cmd.h"
#include "engine/traceinit.h"
#include "filesystem/filesystem.h"
constexpr char DFS_ENABLE_PATH[] = "/vpk/enable.txt";

//-----------------------------------------------------------------------------
// Figure out if we're running a Valve mod or not.
//-----------------------------------------------------------------------------
static bool IsValveMod(const char* pModName)
{
	return (Q_stricmp(pModName, "cstrike") == 0 ||
		Q_stricmp(pModName, "dod") == 0 ||
		Q_stricmp(pModName, "hl1mp") == 0 ||
		Q_stricmp(pModName, "tf") == 0 ||
		Q_stricmp(pModName, "hl2mp") == 0 ||
		Q_stricmp(pModName, "csgo") == 0);
}

//-----------------------------------------------------------------------------
// Figure out if we're running a Respawn mod or not.
//-----------------------------------------------------------------------------
static bool IsRespawnMod(const char* pModName)
{
	return (Q_stricmp(pModName, "r1") == 0 ||
		Q_stricmp(pModName, "r2") == 0 ||
		Q_stricmp(pModName, "r5") == 0);
}

//-----------------------------------------------------------------------------
// Initialize the VPK and file cache system
//-----------------------------------------------------------------------------
static void InitVPKSystem()
{
    char szCacheEnableFilePath[MAX_OSPATH];
    char bFixSlashes = FileSystem()->GetCurrentDirectory(szCacheEnableFilePath, sizeof(szCacheEnableFilePath)) ? szCacheEnableFilePath[0] : '\0';

    size_t nCachePathLen = strlen(szCacheEnableFilePath);
    size_t nCacheFileLen = sizeof(DFS_ENABLE_PATH)-1;

    if ((nCachePathLen + nCacheFileLen) < MAX_OSPATH || (nCacheFileLen = (MAX_OSPATH-1) - nCachePathLen, nCachePathLen != (MAX_OSPATH-1)))
    {
        strncat(szCacheEnableFilePath, DFS_ENABLE_PATH, nCacheFileLen)[sizeof(szCacheEnableFilePath)-1] = '\0';
        bFixSlashes = szCacheEnableFilePath[0];
    }
    if (bFixSlashes)
    {
        V_FixSlashes(szCacheEnableFilePath, '/');
    }
    if (!CommandLine()->CheckParm("-novpk") && FileSystem()->FileExists(szCacheEnableFilePath, nullptr))
    {
        FileSystem()->AddSearchPath(".", "MAIN", SearchPathAdd_t::PATH_ADD_TO_TAIL);
        FileSystem()->SetVPKCacheModeServer();
        FileSystem()->MountVPKFile("vpk/server_mp_common.bsp");
    }
}

InitReturnVal_t CEngineAPI::VInit(CEngineAPI* pEngineAPI)
{
    return CEngineAPI__Init(pEngineAPI);
}

//-----------------------------------------------------------------------------
// Initialization, shutdown of a mod.
//-----------------------------------------------------------------------------
bool CEngineAPI::VModInit(CEngineAPI* pEngineAPI, const char* pModName, const char* pGameDir)
{
    // Register new Pak Assets here!
    //RTech_RegisterAsset(0, 1, "", nullptr, nullptr, nullptr, CMemory(0x1660AD0A8).RCast<void**>, 8, 8, 8, 0, 0xFFFFFFC);

	const bool results = CEngineAPI__ModInit(pEngineAPI, pModName, pGameDir);
	if (!IsValveMod(pModName) && !IsRespawnMod(pModName))
	{
	}

	return results;
}

//-----------------------------------------------------------------------------
// One-time setup, based on the initially selected mod
//-----------------------------------------------------------------------------
bool CEngineAPI::OnStartup(CEngineAPI* pEngineAPI, void* pInstance, const char* pStartupModName)
{
	const bool results =  CEngineAPI__OnStartup(pEngineAPI, pInstance, pStartupModName);
	return results;
}

//-----------------------------------------------------------------------------
// Sets startup info
//-----------------------------------------------------------------------------
void CEngineAPI::VSetStartupInfo(CEngineAPI* pEngineAPI, StartupInfo_t* pStartupInfo)
{
    if (*g_bStartupInfoSet)
    {
        return;
    }

    const size_t nBufLen = sizeof(pStartupInfo->m_szBaseDirectory);
    strncpy(g_szBaseDir, pStartupInfo->m_szBaseDirectory, nBufLen);

    g_pEngineParms->baseDirectory = g_szBaseDir;
    g_szBaseDir[nBufLen-1] = '\0';

    pEngineAPI->m_StartupInfo = *pStartupInfo;
    InitVPKSystem();

    v_TRACEINIT(NULL, "COM_InitFilesystem( m_StartupInfo.m_szInitialMod )", "COM_ShutdownFileSystem()");
    v_COM_InitFilesystem(pEngineAPI->m_StartupInfo.m_szInitialMod);

    *g_bStartupInfoSet = true;
}

//-----------------------------------------------------------------------------
// Purpose
//-----------------------------------------------------------------------------
void CEngineAPI::PumpMessages()
{
}

//-----------------------------------------------------------------------------
// Purpose
//-----------------------------------------------------------------------------
bool CEngineAPI::MainLoop()
{
    // Main message pump
    while (true)
    {
        // Pump messages unless someone wants to quit
        if (g_pEngine->GetQuitting() != IEngine::QUIT_NOTQUITTING)
        {
            if (g_pEngine->GetQuitting() != IEngine::QUIT_TODESKTOP) {
                return true;
            }

            return false;
        }


        g_pEngine->Frame();
    }
}

///////////////////////////////////////////////////////////////////////////////
void VSys_Dll2::Detour(const bool bAttach) const
{
	DetourSetup(&CEngineAPI__Init, &CEngineAPI::VInit, bAttach);
	DetourSetup(&CEngineAPI__ModInit, &CEngineAPI::VModInit, bAttach);
	DetourSetup(&CEngineAPI__OnStartup, &CEngineAPI::OnStartup, bAttach);
	DetourSetup(&CEngineAPI__MainLoop, &CEngineAPI::MainLoop, bAttach);
	DetourSetup(&CEngineAPI__SetStartupInfo, &CEngineAPI::VSetStartupInfo, bAttach);
}
#endif // CLIENT_DLL
