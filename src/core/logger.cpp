#if defined(CLIENT_DLL)
#include "tier0/utility.h"
#ifndef _TOOLS
#include "tier0/commandline.h"
#endif // !_TOOLS
#include "init.h"
#include "logdef.h"
#include "logger.h"
#include "vgui/vgui_debugpanel.h"
#include "gameui/IConsole.h"
extern bool g_bSdkShutdownCallInitiated;
#ifndef _TOOLS
#include "vscript/languages/squirrel_re/include/sqstdaux.h"
#endif // !_TOOLS
static const boost::regex s_AnsiRowRegex(R"(\x1b\[[\d;]+m)");
static std::mutex s_LogMutex;

static bool Logger_IsConsoleNoise(eDLL_T context, const std::string& formatted)
{
	if (context == eDLL_T::SCRIPT_UI || context == eDLL_T::SCRIPT_CLIENT ||
		context == eDLL_T::SCRIPT_SERVER)
	{
		return formatted.rfind("compile scripts/vscripts/", 0) == 0;
	}

	if (formatted.rfind("[FS-DIAG slot15]", 0) == 0)
		return true;
	if (formatted.rfind("ScriptRemoteServer: registered", 0) == 0)
		return true;
	if (formatted.rfind("ScriptRemoteClient: registered", 0) == 0)
		return true;

	if (formatted.size() < 3 || formatted[0] != '[')
		return false;

	const size_t close = formatted.find(']');
	if (close == std::string::npos || close < 2 || close > 64)
		return false;

	if (formatted.compare(0, 14, "[BRIDGE-READY]") == 0)
		return false;
	if (formatted.compare(0, 6, "[AUTH]") == 0)
		return false;

	static const char* const kKeep[] = {
		"FAIL", "FAILED", "unresolved", "CRASH", "FATAL",
		"DIVERGENT", "OVERRUN", "exception", "not attaching",
		"NON-ZERO", "hold queue full"
	};
	for (const char* needle : kKeep)
	{
		if (formatted.find(needle) != std::string::npos)
			return false;
	}

	return true;
}

#if !defined (DEDICATED) && !defined (_TOOLS)
ImVec4 CheckForWarnings(LogType_t type, eDLL_T context, const ImVec4& defaultCol)
{
	ImVec4 color = defaultCol;
	if (type == LogType_t::LOG_WARNING || context == eDLL_T::SYSTEM_WARNING)
	{
		color = ImVec4(1.00f, 1.00f, 0.00f, 0.80f);
	}
	else if (type == LogType_t::LOG_ERROR || context == eDLL_T::SYSTEM_ERROR)
	{
		color = ImVec4(1.00f, 0.00f, 0.00f, 0.80f);
	}

	return color;
}

ImVec4 GetColorForContext(LogType_t type, eDLL_T context)
{
	switch (context)
	{
	case eDLL_T::SCRIPT_SERVER:
		return CheckForWarnings(type, context, ImVec4(0.59f, 0.58f, 0.73f, 1.00f));
	case eDLL_T::SCRIPT_CLIENT:
		return CheckForWarnings(type, context, ImVec4(0.59f, 0.58f, 0.63f, 1.00f));
	case eDLL_T::SCRIPT_UI:
		return CheckForWarnings(type, context, ImVec4(0.59f, 0.48f, 0.53f, 1.00f));
	case eDLL_T::SERVER:
		return CheckForWarnings(type, context, ImVec4(0.23f, 0.47f, 0.85f, 1.00f));
	case eDLL_T::CLIENT:
		return CheckForWarnings(type, context, ImVec4(0.46f, 0.46f, 0.46f, 1.00f));
	case eDLL_T::UI:
		return CheckForWarnings(type, context, ImVec4(0.59f, 0.35f, 0.46f, 1.00f));
	case eDLL_T::ENGINE:
		return CheckForWarnings(type, context, ImVec4(0.70f, 0.70f, 0.70f, 1.00f));
	case eDLL_T::FS:
		return CheckForWarnings(type, context, ImVec4(0.32f, 0.64f, 0.72f, 1.00f));
	case eDLL_T::RTECH:
		return CheckForWarnings(type, context, ImVec4(0.36f, 0.70f, 0.35f, 1.00f));
	case eDLL_T::MS:
		return CheckForWarnings(type, context, ImVec4(0.75f, 0.30f, 0.68f, 1.00f));
	case eDLL_T::AUDIO:
		return CheckForWarnings(type, context, ImVec4(0.93f, 0.42f, 0.12f, 1.00f));
	case eDLL_T::VIDEO:
		return CheckForWarnings(type, context, ImVec4(0.73f, 0.00f, 0.92f, 1.00f));
	case eDLL_T::NETCON:
		return CheckForWarnings(type, context, ImVec4(0.81f, 0.81f, 0.81f, 1.00f));
	case eDLL_T::COMMON:
		return CheckForWarnings(type, context, ImVec4(1.00f, 0.80f, 0.60f, 1.00f));
	case eDLL_T::MODSYSTEM:
		return CheckForWarnings(type, context, ImVec4(1.00f, 0.81f, 0.00f, 1.00f));
	case eDLL_T::STEAM:
		return CheckForWarnings(type, context, ImVec4(0.38f, 0.69f, 1.00f, 1.00f));
	default:
		return CheckForWarnings(type, context, ImVec4(0.81f, 0.81f, 0.81f, 1.00f));
	}
}
#endif // !DEDICATED && !_TOOLS

static const char* GetContextNameByIndex(eDLL_T context, size_t& numTotalChars, size_t& numAnsiChars, const bool ansiColor)
{
	const int index = static_cast<int>(context);
	const char* contextName;

	switch (context)
	{
	case eDLL_T::SCRIPT_SERVER:
		contextName = s_ScriptAnsiColor[0];
		numTotalChars = s_FullAnsiContextPrefixTextSize;
		break;
	case eDLL_T::SCRIPT_CLIENT:
		contextName = s_ScriptAnsiColor[1];
		numTotalChars = s_FullAnsiContextPrefixTextSize;
		break;
	case eDLL_T::SCRIPT_UI:
		contextName = s_ScriptAnsiColor[2];
		numTotalChars = s_FullAnsiContextPrefixTextSize;
		break;
	case eDLL_T::SERVER:
	case eDLL_T::CLIENT:
	case eDLL_T::UI:
	case eDLL_T::ENGINE:
	case eDLL_T::FS:
	case eDLL_T::RTECH:
	case eDLL_T::MS:
	case eDLL_T::AUDIO:
	case eDLL_T::VIDEO:
	case eDLL_T::NETCON:
	case eDLL_T::MODSYSTEM:
	case eDLL_T::STEAM:
	case eDLL_T::COMMON:
	case eDLL_T::SYSTEM_WARNING:
	case eDLL_T::SYSTEM_ERROR:
		contextName = s_DllAnsiColor[index];
		numTotalChars = context >= eDLL_T::COMMON ? s_AnsiColorTextSize : s_FullAnsiContextPrefixTextSize;
		break;
	default:
		contextName = s_DefaultAnsiColor;
		numTotalChars = s_AnsiColorTextSize;
		break;
	}

	if (!ansiColor)
	{
		// Shift # chars to skip ANSI row.
		contextName += s_AnsiColorTextSize;
		numTotalChars -= s_AnsiColorTextSize;
	}
	else
		numAnsiChars = s_AnsiColorTextSize;

	return contextName;
}

bool LoggedFromClient(eDLL_T context)
{
	return (context == eDLL_T::NETCON);
}

//-----------------------------------------------------------------------------
// Purpose: Show logs to all console interfaces (va_list version)
// Input: logType - 
// logLevel - 
// context - 
// *pszLogger - 
// *pszFormat -
// args - 
// exitCode - 
// *pszUptimeOverride - 
//-----------------------------------------------------------------------------
void EngineLoggerSink(LogType_t logType, LogLevel_t logLevel, eDLL_T context,
	const char* pszLogger, const char* pszFormat, va_list args,
	const UINT exitCode /*= NO_ERROR*/, const char* pszUptimeOverride /*= nullptr*/)
{
	// Process-exit / static-dtor: spdlog registry may already be dead.
	if (!g_bSpdLogAlive)
	{
		if (exitCode)
			TerminateProcess(GetCurrentProcess(), exitCode);
		return;
	}

	const char* pszUpTime = pszUptimeOverride ? pszUptimeOverride : Plat_GetProcessUpTime();
	string message(pszUpTime);

	// Also represents the length of the up time string (the "[0.000] " prefix before each log).
	const size_t contextTextStartIndex = message.length();

	const bool bToConsole = (logLevel >= LogLevel_t::LEVEL_CONSOLE);
	const bool bUseColor = (bToConsole && g_bSpdLog_UseAnsiClr);

	size_t numTotalContextTextChars = 0;
	size_t numAnsiContextChars = 0;

	const char* pszContext = GetContextNameByIndex(context, numTotalContextTextChars, numAnsiContextChars, bUseColor);
	message.append(pszContext, numTotalContextTextChars);

#if !defined (DEDICATED) && !defined (_TOOLS)
	ImVec4 overlayColor = GetColorForContext(logType, context);
	eDLL_T overlayContext = context;
#endif // !DEDICATED && !_TOOLS

#if !defined (_TOOLS)
	bool bSquirrel = false;
	bool bWarning = false;
	bool bError = false;
#else
	NOTE_UNUSED(pszLogger);
#endif // !_TOOLS

	const size_t messageTextStartIndex = message.length();
	size_t numMessageAnsiChars = 0;

	//-------------------------------------------------------------------------
	// Setup logger and context
	//-------------------------------------------------------------------------
	switch (logType)
	{
	case LogType_t::LOG_WARNING:
#if !defined (DEDICATED) && !defined (_TOOLS)
		overlayContext = eDLL_T::SYSTEM_WARNING;
#endif // !DEDICATED && !_TOOLS
		if (bUseColor)
		{
			message.append(g_svYellowF);
			numMessageAnsiChars = g_svYellowF.length();
		}
		break;
	case LogType_t::LOG_ERROR:
#if !defined (DEDICATED) && !defined (_TOOLS)
		overlayContext = eDLL_T::SYSTEM_ERROR;
#endif // !DEDICATED && !_TOOLS
		if (bUseColor)
		{
			message.append(g_svRedF);
			numMessageAnsiChars = g_svRedF.length();
		}
		break;
#ifndef _TOOLS
	case LogType_t::SQ_INFO:
		bSquirrel = true;
		break;
	case LogType_t::SQ_WARNING:
		overlayContext = eDLL_T::SYSTEM_WARNING;
		overlayColor = ImVec4(1.00f, 1.00f, 0.00f, 0.80f);
		bSquirrel = true;
		bWarning = true;
		break;
#endif // !_TOOLS
	default:
		break;
	}

	//-------------------------------------------------------------------------
	// Format actual input
	//-------------------------------------------------------------------------
	va_list argsCopy;
	va_copy(argsCopy, args);
	const string formatted = FormatV(pszFormat, argsCopy);
	va_end(argsCopy);

#ifndef _TOOLS
	//-------------------------------------------------------------------------
	// Colorize script warnings and errors
	//-------------------------------------------------------------------------
	if (bToConsole && bSquirrel)
	{
		if (bWarning && g_bSQAuxError)
		{
			if (formatted.find("SCRIPT ERROR:") != string::npos ||
				formatted.find(" -> ") != string::npos)
			{
				bError = true;
			}
		}
		else if (g_bSQAuxBadLogic)
		{
			if (formatted.find("There was a problem processing game logic.") != string::npos)
			{
				bError = true;
				g_bSQAuxBadLogic = false;
			}
		}

		// Append warning/error color before appending the formatted text,
		// so that this gets marked as such while preserving context colors.
		if (bError)
		{
			overlayContext = eDLL_T::SYSTEM_ERROR;
			overlayColor = ImVec4(1.00f, 0.00f, 0.00f, 0.80f);

			if (bUseColor)
			{
				if (logType != LogType_t::LOG_ERROR)
				{
					if (numMessageAnsiChars > 0)
						message.replace(messageTextStartIndex, numMessageAnsiChars, g_svRedF);
					else
						message.append(g_svRedF);

					numMessageAnsiChars = g_svRedF.length();
				}
			}
		}
		else if (bUseColor && bWarning)
		{
			if (logType != LogType_t::LOG_ERROR)
			{
				if (numMessageAnsiChars > 0)
					message.replace(messageTextStartIndex, numMessageAnsiChars, g_svYellowF);
				else
					message.append(g_svYellowF);

				numMessageAnsiChars = g_svYellowF.length();
			}
		}
	}
#endif // !_TOOLS
	message.append(formatted);

	// Script compile + SDK [TAG] success chatter stay in the file sinks.
	// Console / overlay / boot bar keep failures and [BRIDGE-READY].
	const bool bConsoleSpam = Logger_IsConsoleNoise(context, formatted);

	//-------------------------------------------------------------------------
	// Emit to all interfaces
	//-------------------------------------------------------------------------
	// Emission can stall the calling thread; declared before the lock so it outlives it.
	CPlatStallScope stallScope;
	std::lock_guard<std::mutex> lock(s_LogMutex);
	if (bToConsole)
	{
		if (!bConsoleSpam)
			g_TermLogger->debug(message);

		// Remove ANSI rows if we have them, before emitting to file or over wire.
		if (bUseColor)
		{
			// Start with the message first because else the indices will shift.
			// The message colors comes after the context colors.
			if (numMessageAnsiChars > 0)
			{
				message.erase(messageTextStartIndex, numMessageAnsiChars);
				numMessageAnsiChars = 0;
			}

			if (numAnsiContextChars > 0)
			{
				message.erase(contextTextStartIndex, numAnsiContextChars);
				numAnsiContextChars = 0;
			}

			// Remove anything else that was passed in as a format argument.
			message = boost::regex_replace(message, s_AnsiRowRegex, "");
		}
	}

	// If a debugger is attached, emit the text there too
	if (Plat_IsInDebugSession())
		Plat_DebugString(message.c_str());

#ifndef _TOOLS
	// File loggers are absent in minimum-disk mode; the terminal sink above is
	// then the only output for these lines.
	std::shared_ptr<spdlog::logger> ntlogger = spdlog::get(pszLogger); // <-- Obtain by 'pszLogger'.

	if (ntlogger)
		ntlogger->debug(message);

	if (bToConsole)
	{
		// Skip overlay sinks once SDK_Shutdown has begun -- g_Console is already torn down.
		if (!g_bSdkShutdownCallInitiated && !bConsoleSpam)
		{
			g_Console.AddLog(message.c_str(), ImGui::ColorConvertFloat4ToU32(overlayColor));

			// We can only log to the in-game overlay console when the SDK has
			// been fully initialized, due to the use of ConVar's.
			if (g_bSdkInitialized && logLevel >= LogLevel_t::LEVEL_NOTIFY)
			{
				// Draw to mini console.
				g_TextOverlay.AddLog(overlayContext, message.c_str(), (ssize_t)message.length());
			}
		}
	}

#else
	if (g_SuppementalToolsLogger)
	{
		g_SuppementalToolsLogger->debug(message);
	}
#endif

	if (exitCode) // Terminate the process if an exit code was passed.
	{
		// The file sinks are async, so the fatal line above is still sitting in
		// the queue. Drain it before the process goes away.
		g_bSpdLogAlive = false;
		spdlog::shutdown();
#ifndef _TOOLS
		if (!CommandLine()->CheckParm("-nomessagebox"))
#endif // !_TOOLS
		{
			MessageBoxA(NULL, Format("%s- %s", pszUpTime, formatted.c_str()).c_str(), "SDK Error", MB_ICONERROR | MB_OK);
		}
		TerminateProcess(GetCurrentProcess(), exitCode);
	}
}
#else // !CLIENT_DLL
#include "tier0/utility.h"
#ifndef _TOOLS
#include "tier0/commandline.h"
#endif // !_TOOLS
#include "init.h"
#include "logdef.h"
#include "logger.h"
#ifndef DEDICATED
#include "vgui/vgui_debugpanel.h"
#include "gameui/IConsole.h"
#endif // !DEDICATED
#include "engine/server/sv_rcon.h"
#ifndef _TOOLS
#include "vscript/languages/squirrel_re/include/sqstdaux.h"
#endif // !_TOOLS
static const boost::regex s_AnsiRowRegex(R"(\x1b\[[\d;]+m)");
static std::mutex s_LogMutex;

static bool Logger_IsConsoleNoise(eDLL_T context, const std::string& formatted)
{
	if (context == eDLL_T::SCRIPT_UI || context == eDLL_T::SCRIPT_CLIENT ||
		context == eDLL_T::SCRIPT_SERVER)
	{
		return formatted.rfind("compile scripts/vscripts/", 0) == 0;
	}

	if (formatted.rfind("[FS-DIAG slot15]", 0) == 0)
		return true;
	if (formatted.rfind("ScriptRemoteServer: registered", 0) == 0)
		return true;
	if (formatted.rfind("ScriptRemoteClient: registered", 0) == 0)
		return true;

	if (formatted.size() < 3 || formatted[0] != '[')
		return false;

	const size_t close = formatted.find(']');
	if (close == std::string::npos || close < 2 || close > 64)
		return false;

	if (formatted.compare(0, 14, "[BRIDGE-READY]") == 0)
		return false;
	if (formatted.compare(0, 6, "[AUTH]") == 0)
		return false;

	static const char* const kKeep[] = {
		"FAIL", "FAILED", "unresolved", "CRASH", "FATAL",
		"DIVERGENT", "OVERRUN", "exception", "not attaching",
		"NON-ZERO", "hold queue full"
	};
	for (const char* needle : kKeep)
	{
		if (formatted.find(needle) != std::string::npos)
			return false;
	}

	return true;
}

#if !defined (DEDICATED) && !defined (_TOOLS)
ImVec4 CheckForWarnings(LogType_t type, eDLL_T context, const ImVec4& defaultCol)
{
	ImVec4 color = defaultCol;
	if (type == LogType_t::LOG_WARNING || context == eDLL_T::SYSTEM_WARNING)
	{
		color = ImVec4(1.00f, 1.00f, 0.00f, 0.80f);
	}
	else if (type == LogType_t::LOG_ERROR || context == eDLL_T::SYSTEM_ERROR)
	{
		color = ImVec4(1.00f, 0.00f, 0.00f, 0.80f);
	}

	return color;
}

ImVec4 GetColorForContext(LogType_t type, eDLL_T context)
{
	switch (context)
	{
	case eDLL_T::SCRIPT_SERVER:
		return CheckForWarnings(type, context, ImVec4(0.59f, 0.58f, 0.73f, 1.00f));
	case eDLL_T::SCRIPT_CLIENT:
		return CheckForWarnings(type, context, ImVec4(0.59f, 0.58f, 0.63f, 1.00f));
	case eDLL_T::SCRIPT_UI:
		return CheckForWarnings(type, context, ImVec4(0.59f, 0.48f, 0.53f, 1.00f));
	case eDLL_T::SERVER:
		return CheckForWarnings(type, context, ImVec4(0.23f, 0.47f, 0.85f, 1.00f));
	case eDLL_T::CLIENT:
		return CheckForWarnings(type, context, ImVec4(0.46f, 0.46f, 0.46f, 1.00f));
	case eDLL_T::UI:
		return CheckForWarnings(type, context, ImVec4(0.59f, 0.35f, 0.46f, 1.00f));
	case eDLL_T::ENGINE:
		return CheckForWarnings(type, context, ImVec4(0.70f, 0.70f, 0.70f, 1.00f));
	case eDLL_T::FS:
		return CheckForWarnings(type, context, ImVec4(0.32f, 0.64f, 0.72f, 1.00f));
	case eDLL_T::RTECH:
		return CheckForWarnings(type, context, ImVec4(0.36f, 0.70f, 0.35f, 1.00f));
	case eDLL_T::MS:
		return CheckForWarnings(type, context, ImVec4(0.75f, 0.30f, 0.68f, 1.00f));
	case eDLL_T::AUDIO:
		return CheckForWarnings(type, context, ImVec4(0.93f, 0.42f, 0.12f, 1.00f));
	case eDLL_T::VIDEO:
		return CheckForWarnings(type, context, ImVec4(0.73f, 0.00f, 0.92f, 1.00f));
	case eDLL_T::NETCON:
		return CheckForWarnings(type, context, ImVec4(0.81f, 0.81f, 0.81f, 1.00f));
	case eDLL_T::COMMON:
		return CheckForWarnings(type, context, ImVec4(1.00f, 0.80f, 0.60f, 1.00f));
	case eDLL_T::MODSYSTEM:
		return CheckForWarnings(type, context, ImVec4(1.00f, 0.81f, 0.00f, 1.00f));
	case eDLL_T::STEAM:
		return CheckForWarnings(type, context, ImVec4(0.38f, 0.69f, 1.00f, 1.00f));
	default:
		return CheckForWarnings(type, context, ImVec4(0.81f, 0.81f, 0.81f, 1.00f));
	}
}
#endif // !DEDICATED && !_TOOLS

static const char* GetContextNameByIndex(eDLL_T context, size_t& numTotalChars, size_t& numAnsiChars, const bool ansiColor)
{
	const int index = static_cast<int>(context);
	const char* contextName;

	switch (context)
	{
	case eDLL_T::SCRIPT_SERVER:
		contextName = s_ScriptAnsiColor[0];
		numTotalChars = s_FullAnsiContextPrefixTextSize;
		break;
	case eDLL_T::SCRIPT_CLIENT:
		contextName = s_ScriptAnsiColor[1];
		numTotalChars = s_FullAnsiContextPrefixTextSize;
		break;
	case eDLL_T::SCRIPT_UI:
		contextName = s_ScriptAnsiColor[2];
		numTotalChars = s_FullAnsiContextPrefixTextSize;
		break;
	case eDLL_T::SERVER:
	case eDLL_T::CLIENT:
	case eDLL_T::UI:
	case eDLL_T::ENGINE:
	case eDLL_T::FS:
	case eDLL_T::RTECH:
	case eDLL_T::MS:
	case eDLL_T::AUDIO:
	case eDLL_T::VIDEO:
	case eDLL_T::NETCON:
	case eDLL_T::MODSYSTEM:
	case eDLL_T::STEAM:
	case eDLL_T::COMMON:
	case eDLL_T::SYSTEM_WARNING:
	case eDLL_T::SYSTEM_ERROR:
		contextName = s_DllAnsiColor[index];
		numTotalChars = context >= eDLL_T::COMMON ? s_AnsiColorTextSize : s_FullAnsiContextPrefixTextSize;
		break;
	default:
		contextName = s_DefaultAnsiColor;
		numTotalChars = s_AnsiColorTextSize;
		break;
	}

	if (!ansiColor)
	{
		// Shift # chars to skip ANSI row.
		contextName += s_AnsiColorTextSize;
		numTotalChars -= s_AnsiColorTextSize;
	}
	else
		numAnsiChars = s_AnsiColorTextSize;

	return contextName;
}

bool LoggedFromClient(eDLL_T context)
{
#ifndef DEDICATED
	return (context == eDLL_T::NETCON);
#else
	NOTE_UNUSED(context);
	return false;
#endif // !DEDICATED
}

//-----------------------------------------------------------------------------
// Purpose: Show logs to all console interfaces (va_list version)
// Input: logType - 
// logLevel - 
// context - 
// *pszLogger - 
// *pszFormat -
// args - 
// exitCode - 
// *pszUptimeOverride - 
//-----------------------------------------------------------------------------
void EngineLoggerSink(LogType_t logType, LogLevel_t logLevel, eDLL_T context,
	const char* pszLogger, const char* pszFormat, va_list args,
	const UINT exitCode /*= NO_ERROR*/, const char* pszUptimeOverride /*= nullptr*/)
{
	// Process-exit / static-dtor: spdlog registry may already be dead.
	if (!g_bSpdLogAlive)
	{
		if (exitCode)
			TerminateProcess(GetCurrentProcess(), exitCode);
		return;
	}

	const char* pszUpTime = pszUptimeOverride ? pszUptimeOverride : Plat_GetProcessUpTime();
	string message(pszUpTime);

	// Also represents the length of the up time string (the "[0.000] " prefix before each log).
	const size_t contextTextStartIndex = message.length();

	const bool bToConsole = (logLevel >= LogLevel_t::LEVEL_CONSOLE);
	const bool bUseColor = (bToConsole && g_bSpdLog_UseAnsiClr);

	size_t numTotalContextTextChars = 0;
	size_t numAnsiContextChars = 0;

	const char* pszContext = GetContextNameByIndex(context, numTotalContextTextChars, numAnsiContextChars, bUseColor);
	message.append(pszContext, numTotalContextTextChars);

#if !defined (DEDICATED) && !defined (_TOOLS)
	ImVec4 overlayColor = GetColorForContext(logType, context);
	eDLL_T overlayContext = context;
#endif // !DEDICATED && !_TOOLS

#if !defined (_TOOLS)
	bool bSquirrel = false;
	bool bWarning = false;
	bool bError = false;
#else
	NOTE_UNUSED(pszLogger);
#endif // !_TOOLS

	const size_t messageTextStartIndex = message.length();
	size_t numMessageAnsiChars = 0;

	//-------------------------------------------------------------------------
	// Setup logger and context
	//-------------------------------------------------------------------------
	switch (logType)
	{
	case LogType_t::LOG_WARNING:
#if !defined (DEDICATED) && !defined (_TOOLS)
		overlayContext = eDLL_T::SYSTEM_WARNING;
#endif // !DEDICATED && !_TOOLS
		if (bUseColor)
		{
			message.append(g_svYellowF);
			numMessageAnsiChars = g_svYellowF.length();
		}
		break;
	case LogType_t::LOG_ERROR:
#if !defined (DEDICATED) && !defined (_TOOLS)
		overlayContext = eDLL_T::SYSTEM_ERROR;
#endif // !DEDICATED && !_TOOLS
		if (bUseColor)
		{
			message.append(g_svRedF);
			numMessageAnsiChars = g_svRedF.length();
		}
		break;
#ifndef _TOOLS
	case LogType_t::SQ_INFO:
		bSquirrel = true;
		break;
	case LogType_t::SQ_WARNING:
#ifndef DEDICATED
		overlayContext = eDLL_T::SYSTEM_WARNING;
		overlayColor = ImVec4(1.00f, 1.00f, 0.00f, 0.80f);
#endif // !DEDICATED
		bSquirrel = true;
		bWarning = true;
		break;
#endif // !_TOOLS
	default:
		break;
	}

	//-------------------------------------------------------------------------
	// Format actual input
	//-------------------------------------------------------------------------
	va_list argsCopy;
	va_copy(argsCopy, args);
	const string formatted = FormatV(pszFormat, argsCopy);
	va_end(argsCopy);

#ifndef _TOOLS
	//-------------------------------------------------------------------------
	// Colorize script warnings and errors
	//-------------------------------------------------------------------------
	if (bToConsole && bSquirrel)
	{
		if (bWarning && g_bSQAuxError)
		{
			if (formatted.find("SCRIPT ERROR:") != string::npos ||
				formatted.find(" -> ") != string::npos)
			{
				bError = true;
			}
		}
		else if (g_bSQAuxBadLogic)
		{
			if (formatted.find("There was a problem processing game logic.") != string::npos)
			{
				bError = true;
				g_bSQAuxBadLogic = false;
			}
		}

		// Append warning/error color before appending the formatted text,
		// so that this gets marked as such while preserving context colors.
		if (bError)
		{
#ifndef DEDICATED
			overlayContext = eDLL_T::SYSTEM_ERROR;
			overlayColor = ImVec4(1.00f, 0.00f, 0.00f, 0.80f);
#endif // !DEDICATED

			if (bUseColor)
			{
				if (logType != LogType_t::LOG_ERROR)
				{
					if (numMessageAnsiChars > 0)
						message.replace(messageTextStartIndex, numMessageAnsiChars, g_svRedF);
					else
						message.append(g_svRedF);

					numMessageAnsiChars = g_svRedF.length();
				}
			}
		}
		else if (bUseColor && bWarning)
		{
			if (logType != LogType_t::LOG_ERROR)
			{
				if (numMessageAnsiChars > 0)
					message.replace(messageTextStartIndex, numMessageAnsiChars, g_svYellowF);
				else
					message.append(g_svYellowF);

				numMessageAnsiChars = g_svYellowF.length();
			}
		}
	}
#endif // !_TOOLS
	message.append(formatted);

	const bool bConsoleSpam = Logger_IsConsoleNoise(context, formatted);

	// -bridgediaglogs keeps DT/NETVAR firehoses; default drops them from console and file.
	static int s_bridgeDiagLogs = -1;
#ifndef _TOOLS
	// Tools link tier0 without the command line singleton; they never emit these
	// prefixes, so leaving the flag unresolved keeps the same drop behaviour.
	if (s_bridgeDiagLogs < 0 && CommandLine())
		s_bridgeDiagLogs = CommandLine()->CheckParm("-bridgediaglogs") ? 1 : 0;
#endif // !_TOOLS
	const bool bDropFirehose = (s_bridgeDiagLogs != 1) &&
		(formatted.rfind("[DT-DUMP]", 0) == 0 ||
		 formatted.rfind("[DT-TREE]", 0) == 0 ||
		 formatted.rfind("[OFFHAND-SUB-DUMP]", 0) == 0 ||
		 formatted.rfind("[NETVAR-REG]", 0) == 0 ||
		 formatted.rfind("[PACK-DIFF]", 0) == 0 ||
		 formatted.rfind("[IB-DUMP-DEDI]", 0) == 0 ||
		 formatted.rfind("[FINDER-DIAG]", 0) == 0 ||
		 formatted.rfind("[ARR-DIAG]", 0) == 0 ||
		 formatted.rfind("[ARR-DIAGv2]", 0) == 0 ||
		 formatted.rfind("[GAP-TRIAGE]", 0) == 0 ||
		 formatted.rfind("[PROP-COPY]", 0) == 0);

	//-------------------------------------------------------------------------
	// Emit to all interfaces
	//-------------------------------------------------------------------------
	// Emission can stall the calling thread; declared before the lock so it outlives it.
	CPlatStallScope stallScope;
	std::lock_guard<std::mutex> lock(s_LogMutex);
	if (bToConsole)
	{
		if (!bConsoleSpam && !bDropFirehose)
			g_TermLogger->debug(message);

		// Remove ANSI rows if we have them, before emitting to file or over wire.
		if (bUseColor)
		{
			// Start with the message first because else the indices will shift.
			// The message colors comes after the context colors.
			if (numMessageAnsiChars > 0)
			{
				message.erase(messageTextStartIndex, numMessageAnsiChars);
				numMessageAnsiChars = 0;
			}

			if (numAnsiContextChars > 0)
			{
				message.erase(contextTextStartIndex, numAnsiContextChars);
				numAnsiContextChars = 0;
			}

			// Remove anything else that was passed in as a format argument.
			message = boost::regex_replace(message, s_AnsiRowRegex, "");
		}
	}

	// If a debugger is attached, emit the text there too
	if (Plat_IsInDebugSession())
		Plat_DebugString(message.c_str());

#ifndef _TOOLS
	// File loggers are absent in minimum-disk mode; the terminal sink above is
	// then the only output for these lines.
	std::shared_ptr<spdlog::logger> ntlogger = spdlog::get(pszLogger); // <-- Obtain by 'pszLogger'.

	if (ntlogger && !bDropFirehose)
		ntlogger->debug(message);

	if (bToConsole && !bConsoleSpam && !bDropFirehose)
	{
		if (!LoggedFromClient(context) && RCONServer()->ShouldSend(netcon::response_e::SERVERDATA_RESPONSE_CONSOLE_LOG))
		{
			RCONServer()->SendEncoded(formatted.c_str(), formatted.length(), pszUpTime, contextTextStartIndex, netcon::response_e::SERVERDATA_RESPONSE_CONSOLE_LOG,
				int(context), int(logType));
		}
#ifndef DEDICATED
		g_Console.AddLog(message.c_str(), ImGui::ColorConvertFloat4ToU32(overlayColor));

		// We can only log to the in-game overlay console when the SDK has
		// been fully initialized, due to the use of ConVar's.
		if (g_bSdkInitialized && logLevel >= LogLevel_t::LEVEL_NOTIFY)
		{
			// Draw to mini console.
			g_TextOverlay.AddLog(overlayContext, message.c_str(), (ssize_t)message.length());
		}
#endif // !DEDICATED
	}

#else
	if (g_SuppementalToolsLogger)
	{
		g_SuppementalToolsLogger->debug(message);
	}
#endif

	if (exitCode) // Terminate the process if an exit code was passed.
	{
		// The file sinks are async, so the fatal line above is still sitting in
		// the queue. Drain it before the process goes away.
		g_bSpdLogAlive = false;
		spdlog::shutdown();

#ifndef _TOOLS
		if (!CommandLine()->CheckParm("-nomessagebox"))
#endif // !_TOOLS
		{
			MessageBoxA(NULL, Format("%s- %s", pszUpTime, formatted.c_str()).c_str(), "SDK Error", MB_ICONERROR | MB_OK);
		}
		TerminateProcess(GetCurrentProcess(), exitCode);
	}
}
#endif // CLIENT_DLL
