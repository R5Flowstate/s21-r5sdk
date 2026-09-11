//===== Copyright © 1996-2005, Valve Corporation, All rights reserved. ========//
//
// Purpose: engine/launcher interface
//
// $NoKeywords: $
//=============================================================================//
#ifndef ENGINE_LAUNCHER_APIH
#define ENGINE_LAUNCHER_APIH

#include "public/appframework/IAppSystem.h"

class CAppSystemGroup;

struct StartupInfo_t
{
	void* m_pInstance;
	char m_szBaseDirectory[MAX_OSPATH];
	char m_szInitialMod[MAX_OSPATH];
	char m_szInitialGame[MAX_OSPATH];
	CAppSystemGroup* m_pParentAppSystemGroup;
};

//-----------------------------------------------------------------------------
// Return values from the initialization stage of the application framework
//-----------------------------------------------------------------------------
enum
{
	INIT_RESTART = INIT_LAST_VAL,
	RUN_FIRST_VAL,
};


//-----------------------------------------------------------------------------
// Return values from IEngineAPI::Run.
//-----------------------------------------------------------------------------
enum
{
	RUN_OK = RUN_FIRST_VAL,
	RUN_RESTART,
};


//-----------------------------------------------------------------------------
// Main engine interface to launcher + tools
//-----------------------------------------------------------------------------
#define VENGINE_LAUNCHER_API_VERSION "VENGINE_LAUNCHER_API_VERSION004"

abstract_class IEngineAPI : public IAppSystem
{
	// Functions
	public:
		// This function must be called before init
		virtual bool SetStartupInfo(StartupInfo_t & info) = 0;

		// Run the engine
		virtual int Run() = 0;

		// Sets the engine to run in a particular editor window
		virtual void PostConsoleCommand(const char* pConsoleCommand) = 0;

		// Are we running the simulation?
		virtual bool IsRunningSimulation() const = 0;

		// Start/stop running the simulation
		virtual void ActivateSimulation(bool bActive) = 0;

		// Reset the map we're on
		virtual void SetMap(const char* pMapName) = 0;
};

#endif // ENGINE_LAUNCHER_APIH
