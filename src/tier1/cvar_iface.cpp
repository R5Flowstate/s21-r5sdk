#if defined(CLIENT_DLL)
#include "tier1/cvar.h"
#include "tier0/tier0_iface.h"
#include "tier0/commandline.h"
#include "common/global.h"
#include "core/sdk_stage.h"
#include "engine/client/net_bridge_addrs.h"

extern void Bridge_ApplyLaunchConVarOverrides();
extern void ConVarStubs_Init();
extern void ConVarStubs_InitScriptConVars();

static void Cvar_ForceS21DeveloperBackingValue()
{
	// FindVar("developer") -- a hardcoded DX11 RVA is the wrong object on DX12.
	// VEH runs before frame __try; VirtualQuery the write instead.
	if (!g_pCVar)
		return;

	ConVar* const dev = g_pCVar->FindVar("developer");
	if (!dev)
		return;

	// Backing float @ +0x60, int @ +0x64. S3 SetValue writes the old offsets.
	const uintptr_t cvAddr = reinterpret_cast<uintptr_t>(dev);
	MEMORY_BASIC_INFORMATION mbi = {};
	if (VirtualQuery(reinterpret_cast<void*>(cvAddr + 0x64), &mbi, sizeof(mbi)) != sizeof(mbi))
		return;
	if (mbi.State != MEM_COMMIT ||
		!(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
		return;

	*reinterpret_cast<float*>(cvAddr + 0x60) = 1.0f;
	*reinterpret_cast<int*>(cvAddr + 0x64) = 1;
}

void Cvar_ForceCommandLineDevModeForScripts()
{
	if (!CommandLine())
		return;
	if (!CommandLine()->CheckParm("-dev") && !CommandLine()->CheckParm("-developer"))
		return;

	Cvar_ForceS21DeveloperBackingValue();
}

// Plain body: shared by SdkStage_Run wrapper and ResolveSingleton stage (no nest).
static void Cvar_ApplyCommandLineDevMode_Stage()
{
	cv->EnableDevCvars();

	const bool devScripts = CommandLine()->CheckParm("-dev") || CommandLine()->CheckParm("-developer");

	ConVar* const dev = g_pCVar->FindVar("developer");
	if (dev && devScripts)
	{
		developer = dev;
		dev->SetValue(1);
	}

	if (devScripts)
		Cvar_ForceS21DeveloperBackingValue();
}

static void Cvar_ApplyCommandLineDevMode()
{
	if (!g_pCVar || !CommandLine())
		return;
	const bool devScripts = CommandLine()->CheckParm("-dev") || CommandLine()->CheckParm("-developer");
	const bool devSdk = CommandLine()->CheckParm("-devsdk");
	if (!devScripts && !devSdk)
		return;

	SdkStage_Run("Cvar_ApplyCommandLineDevMode", Cvar_ApplyCommandLineDevMode_Stage);
}

//-----------------------------------------------------------------------------
// Capture the engine CCvar singleton and register SDK ConVars into it.
// Singleton RVA 0xAA5DD30 (DX11); ConCommandBase is 0x38 (no static usage string).
//-----------------------------------------------------------------------------

static bool Cvar_IsDx12Exe()
{
	char exePath[MAX_PATH] = {};
	GetModuleFileNameA(NULL, exePath, SDK_ARRAYSIZE(exePath));
	return V_stristr(exePath, "dx12") != nullptr;
}

// Plain Register+Apply so a fault fails the stage and nulls g_pCVar.
// Calls Apply stage body directly (no nested SdkStage_Run).
static void Cvar_ResolveSingleton_Stage()
{
	ConVarStubs_Init();
	ConVar_Register();
	ConVarStubs_InitScriptConVars();
	Bridge_ApplyLaunchConVarOverrides();

	if (!g_pCVar || !CommandLine())
		return;
	const bool devScripts = CommandLine()->CheckParm("-dev") || CommandLine()->CheckParm("-developer");
	const bool devSdk = CommandLine()->CheckParm("-devsdk");
	if (!devScripts && !devSdk)
		return;

	Cvar_ApplyCommandLineDevMode_Stage();
}

bool Cvar_ResolveSingletonFromEngine()
{
	if (g_pCVar)
	{
		Cvar_ApplyCommandLineDevMode();
		return true;
	}

	Module_FindPattern(g_GameDll,
		"48 83 EC 28 48 8B 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48 85 C0 48 0F 45 C8 FF 05 ?? ?? ?? ?? 48 89 0D ?? ?? ?? ??")
		.FindPatternSelf("48 8D 0D").ResolveRelativeAddressSelf(3, 7).GetPtr(g_pCVar);

	const uintptr_t base = g_GameDll.GetModuleBase();
	if (!base)
		return false;

	if (!g_pCVar && !Cvar_IsDx12Exe())
	{
		// The CCvar is a statically-allocated object, not a pointer variable --
		// so we take the address of the instance directly (no dereference).
		g_pCVar = reinterpret_cast<CCvar*>(NetObs_Sym(NetObsSym_t::CCvarInstance));
	}

	if (!g_pCVar)
		return false;

	// ConVar_Register drains s_pConCommandBases into the engine registry.
	// Re-apply +sdk_*/+bridge_* here: the engine consumed them before inject.
	if (!SdkStage_Run("Cvar_ResolveSingletonFromEngine", Cvar_ResolveSingleton_Stage))
	{
		g_pCVar = nullptr;
		return false;
	}
	return true;
}

/*
=====================
CON_Help_f

  Shows the colors and
  description of each
  context.
=====================
*/
static void CON_Help_f()
{
	Msg(eDLL_T::COMMON, "Contexts:\n");

	Msg(eDLL_T::SCRIPT_SERVER, " = Server DLL (Script)\n");
	Msg(eDLL_T::SCRIPT_CLIENT, " = Client DLL (Script)\n");
	Msg(eDLL_T::SCRIPT_UI, " = UI DLL (Script)\n");

	Msg(eDLL_T::SERVER, " = Server DLL (Code)\n");
	Msg(eDLL_T::CLIENT, " = Client DLL (Code)\n");
	Msg(eDLL_T::UI, " = UI DLL (Code)\n");

	Msg(eDLL_T::ENGINE, " = Engine DLL (Code)\n");
	Msg(eDLL_T::FS, " = FileSystem (Code)\n");
	Msg(eDLL_T::RTECH, " = PakLoad API (Code)\n");
	Msg(eDLL_T::MS, " = MaterialSystem (Code)\n");

	Msg(eDLL_T::AUDIO, " = Audio DLL (Code)\n");
	Msg(eDLL_T::VIDEO, " = Video DLL (Code)\n");
	Msg(eDLL_T::NETCON, " = NetConsole (Code)\n");
	Msg(eDLL_T::MODSYSTEM, " = Mod System (Code)\n");
}

static ConCommand con_help("con_help", CON_Help_f, "Shows the colors and description of each context", FCVAR_RELEASE);
#else // !CLIENT_DLL
#include "tier1/cvar.h"

extern void ConVarStubs_Init();
extern void ConVarStubs_InitScriptConVars();
extern void Bridge_ApplyLaunchConVarOverrides();

static bool CVar_Connect(CCvar* thisptr, CreateInterfaceFn factory)
{
	CCvar__Connect(thisptr, factory);

	ConVar_InitShipped();
	ConVar_PurgeShipped();
	ConCommand_InitShipped();
	ConCommand_PurgeShipped();

	ConVarStubs_Init();
	ConVar_Register();
	ConVarStubs_InitScriptConVars();

	// Re-apply +convar launch args after ConVar_Register so SDK vars exist.
	Bridge_ApplyLaunchConVarOverrides();

	// CCvar::Connect always returns true in the implementation of the engine
	return true;
}

static void CVar_Disconnect(CCvar* thisptr)
{
	ConVar_Unregister();
	CCvar__Disconnect(thisptr);
}

/*
=====================
CON_Help_f

  Shows the colors and
  description of each
  context.
=====================
*/
static void CON_Help_f()
{
	Msg(eDLL_T::COMMON, "Contexts:\n");

	Msg(eDLL_T::SCRIPT_SERVER, " = Server DLL (Script)\n");
	Msg(eDLL_T::SCRIPT_CLIENT, " = Client DLL (Script)\n");
	Msg(eDLL_T::SCRIPT_UI, " = UI DLL (Script)\n");

	Msg(eDLL_T::SERVER, " = Server DLL (Code)\n");
	Msg(eDLL_T::CLIENT, " = Client DLL (Code)\n");
	Msg(eDLL_T::UI, " = UI DLL (Code)\n");

	Msg(eDLL_T::ENGINE, " = Engine DLL (Code)\n");
	Msg(eDLL_T::FS, " = FileSystem (Code)\n");
	Msg(eDLL_T::RTECH, " = PakLoad API (Code)\n");
	Msg(eDLL_T::MS, " = MaterialSystem (Code)\n");

	Msg(eDLL_T::AUDIO, " = Audio DLL (Code)\n");
	Msg(eDLL_T::VIDEO, " = Video DLL (Code)\n");
	Msg(eDLL_T::NETCON, " = NetConsole (Code)\n");
	Msg(eDLL_T::MODSYSTEM, " = Mod System (Code)\n");
}

static ConCommand con_help("con_help", CON_Help_f, "Shows the colors and description of each context", FCVAR_RELEASE);

extern void ConVar_PrintDescription(const ConCommandBase* const pVar);

///////////////////////////////////////////////////////////////////////////////
void VCVar::Detour(const bool bAttach) const
{
	DetourSetup(&CCvar__Connect, &CVar_Connect, bAttach);
	DetourSetup(&CCvar__Disconnect, &CVar_Disconnect, bAttach);

	DetourSetup(&v_ConVar_PrintDescription, &ConVar_PrintDescription, bAttach);
}
#endif // CLIENT_DLL
