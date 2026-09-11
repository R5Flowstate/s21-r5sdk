#if defined(CLIENT_DLL)
#include "core/stdafx.h"
#include "tier0/commandline.h"
#include "host_cmd.h"
#include "common.h"
#include "client/client.h"
#include "windows/id3dx.h"

// Host_SetupUIMaterials is inlined into Host_Init_DuringVideo on this client build
// and the pak globals it used are unresolved here; SDK paks load from rpak_observe.

/*
==================
Host_Shutdown

 shutdown host
 systems
==================
*/
static void Host_Shutdown()
{
	DirectX_Shutdown();
	v_Host_Shutdown();
}

/*
==================
Host_Status_PrintClient

 Print client info 
 to console
==================
*/
static void Host_Status_PrintClient(CClient* client, bool bShowAddress, void (*print) (const char* fmt, ...))
{
	CNetChan* nci = client->GetNetChan();
	const char* state = "challenging";

	if (client->IsActive())
		state = "active";
	else if (client->IsSpawned())
		state = "spawning";
	else if (client->IsConnected())
		state = "connecting";

	if (nci != NULL)
	{
		print("# %hu \"%s\" %llu %s %i %i %s %d\n",
			client->GetHandle(), client->GetServerName(), client->GetPlatformUserId(), COM_FormatSeconds(static_cast<int>(nci->GetTimeConnected())),
			static_cast<int>(1000.0f * nci->GetAvgLatency(FLOW_OUTGOING)), static_cast<int>(100.0f * nci->GetAvgLoss(FLOW_INCOMING)), state, nci->GetDataRate());

		if (bShowAddress)
		{
			print(" %s\n", nci->GetAddress());
		}
	}
	else
	{
		print("#%2hu \"%s\" %llu %s\n", client->GetHandle(), client->GetServerName(), client->GetPlatformUserId(), state);
	}

	//print("\n");
}

/*
==================
DFS_InitializeFeatureFlagDefinitions

 Initialize feature
 flag definitions
==================
*/
static bool DFS_InitializeFeatureFlagDefinitions(const char* pszFeatureFlags)
{
	if (CommandLine()->CheckParm("-nodfs"))
		return false;

	return v_DFS_InitializeFeatureFlagDefinitions(pszFeatureFlags);
}

///////////////////////////////////////////////////////////////////////////////
EngineParms_t* g_pEngineParms = nullptr;
#else // !CLIENT_DLL
#include "core/stdafx.h"
#include "tier0/commandline.h"
#include "rtech/pak/pakstate.h"
#include "host_cmd.h"
#include "common.h"
#include "client/client.h"


static const char* const s_paksToLoad[] =
{
	// Used to store assets that must be loaded after common_early.rpak, but
	// before common.rpak is being loaded. One use case is to preserve the
	// fixed linked list structure for the player settings layouts, we must
	// load SDK layouts before common.rpak as the Game DLL expects the linked
	// list to be ordered in a specific manner that is determined by bakery.
	//"common_early_sdk.rpak",
	nullptr // Sentinel; also ensures the array is never zero-sized.
};

/*
==================
Host_SetupUIMaterials

 setup and initialize
 UI materials
==================
*/
static void Host_SetupUIMaterials()
{
	// Don't sync during video init as this is where this function is called
	// from. We restore the function pointer after we loaded the pak file.
	/*void* const oldSyncFn = g_pakGlobals->threadSyncFunc;
	g_pakGlobals->threadSyncFunc = DoNothing;

	for (size_t i = 0; i < V_ARRAYSIZE(s_paksToLoad); i++)
	{
		const char* const pakFileName = s_paksToLoad[i];

		// NOTE: make sure to wait for the async load request, as these paks
		// must be loaded before we continue processing anything else.
		const PakHandle_t pakHandle = g_pakLoadApi->LoadAsyncAndWait(pakFileName, AlignedMemAlloc, 3, DoNothing);

		if (pakHandle == PAK_INVALID_HANDLE)
			Error(eDLL_T::ENGINE, EXIT_FAILURE, "Failed to load pak file '%s'\n", pakFileName);
	}

	g_pakGlobals->threadSyncFunc = oldSyncFn;*/

	// For dedicated, we shouldn't continue with setting up ui materials.
	// Return out here. This is the only place we can reliably load core
	// paks directly after common_early.rpak and ui.rpak without having
	// the engine do anything in between.
}

/*
==================
Host_Shutdown

 shutdown host
 systems
==================
*/
static void Host_Shutdown()
{
	v_Host_Shutdown();
}

/*
==================
Host_Status_PrintClient

 Print client info 
 to console
==================
*/
static void Host_Status_PrintClient(CClient* client, bool bShowAddress, void (*print) (const char* fmt, ...))
{
	CNetChan* nci = client->GetNetChan();
	const char* state = "challenging";

	if (client->IsActive())
		state = "active";
	else if (client->IsSpawned())
		state = "spawning";
	else if (client->IsConnected())
		state = "connecting";

	if (nci != NULL)
	{
		print("# %hu \"%s\" %llu %s %i %i %s %d\n",
			client->GetHandle(), client->GetServerName(), client->GetPlatformUserId(), COM_FormatSeconds(static_cast<int>(nci->GetTimeConnected())),
			static_cast<int>(1000.0f * nci->GetAvgLatency(FLOW_OUTGOING)), static_cast<int>(100.0f * nci->GetAvgLoss(FLOW_INCOMING)), state, nci->GetDataRate());

		if (bShowAddress)
		{
			print(" %s\n", nci->GetAddress());
		}
	}
	else
	{
		print("#%2hu \"%s\" %llu %s\n", client->GetHandle(), client->GetServerName(), client->GetPlatformUserId(), state);
	}

	//print("\n");
}

/*
==================
DFS_InitializeFeatureFlagDefinitions

 Initialize feature
 flag definitions
==================
*/
static bool DFS_InitializeFeatureFlagDefinitions(const char* pszFeatureFlags)
{
	if (CommandLine()->CheckParm("-nodfs"))
		return false;

	return v_DFS_InitializeFeatureFlagDefinitions(pszFeatureFlags);
}

///////////////////////////////////////////////////////////////////////////////
void VHostCmd::Detour(const bool bAttach) const
{
	DetourSetup(&v_Host_SetupUIMaterials, &Host_SetupUIMaterials, bAttach);
	DetourSetup(&v_Host_Shutdown, &Host_Shutdown, bAttach);
	DetourSetup(&v_Host_Status_PrintClient, &Host_Status_PrintClient, bAttach);
	DetourSetup(&v_DFS_InitializeFeatureFlagDefinitions, &DFS_InitializeFeatureFlagDefinitions, bAttach);
}

///////////////////////////////////////////////////////////////////////////////
EngineParms_t* g_pEngineParms = nullptr;
#endif // CLIENT_DLL
