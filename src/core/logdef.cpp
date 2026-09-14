#if defined(CLIENT_DLL)
#include "core/stdafx.h"
#include "core/logdef.h"
#ifndef _TOOLS
#include "tier0/commandline.h"
#endif // !_TOOLS
#include <cstdlib>

std::shared_ptr<spdlog::logger> g_TermLogger;
std::shared_ptr<spdlog::logger> g_SuppementalToolsLogger;

static void SpdLog_AtExitMarkDead(void)
{
	g_bSpdLogAlive = false;
}

#ifndef _TOOLS
//-----------------------------------------------------------------------------
// File sinks only with -devsdk/-dev/-developer/-logfiles; -nologfiles forces off.
//-----------------------------------------------------------------------------
bool SpdLog_FileLogsEnabled(void)
{
	static int s_fileLogs = -1;
	if (s_fileLogs < 0)
	{
		// CommandLine() is a non-null empty singleton until
		// CommandLine_CreateFromProcess. Caching CheckParm against that
		// locks file-logs OFF for the whole process, even with -devsdk.
		if (!g_bCommandLineCreated || !CommandLine())
			return false;

		const bool bDeveloper = CommandLine()->CheckParm("-devsdk")
			|| CommandLine()->CheckParm("-dev")
			|| CommandLine()->CheckParm("-developer");

		s_fileLogs = CommandLine()->CheckParm("-nologfiles") ? 0 :
			(bDeveloper || CommandLine()->CheckParm("-logfiles")) ? 1 : 0;
	}
	return s_fileLogs != 0;
}

static void SpdLog_CreateRotatingLoggers()
{
	/************************
	 * ROTATE LOGGER SETUP *
	 ************************/
	// async_factory_nonblock: a sync sink fwrite blocks the emitting (often net) thread.
	// overrun_oldest drops old lines instead of stalling; timestamps are taken at the call.
	spdlog::rotating_logger_mt<spdlog::async_factory_nonblock>("squirrel_re(warning)"
		, fmt::format("{:s}/{:s}", g_LogSessionDirectory, "script_warning.log"), SPDLOG_MAX_SIZE, SPDLOG_NUM_FILE)->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v");
	spdlog::rotating_logger_mt<spdlog::async_factory_nonblock>("squirrel_re"
		, fmt::format("{:s}/{:s}", g_LogSessionDirectory, "script.log"), SPDLOG_MAX_SIZE, SPDLOG_NUM_FILE)->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v");
	spdlog::rotating_logger_mt<spdlog::async_factory_nonblock>("sdk"
		, fmt::format("{:s}/{:s}", g_LogSessionDirectory, "message.log"), SPDLOG_MAX_SIZE, SPDLOG_NUM_FILE)->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v");
	spdlog::rotating_logger_mt<spdlog::async_factory_nonblock>("sdk(warning)"
		, fmt::format("{:s}/{:s}", g_LogSessionDirectory, "warning.log"), SPDLOG_MAX_SIZE, SPDLOG_NUM_FILE)->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v");
	spdlog::rotating_logger_mt<spdlog::async_factory_nonblock>("sdk(error)"
		, fmt::format("{:s}/{:s}", g_LogSessionDirectory, "error.log"), SPDLOG_MAX_SIZE, SPDLOG_NUM_FILE)->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v");
	spdlog::rotating_logger_mt<spdlog::async_factory_nonblock>("net_trace"
		, fmt::format("{:s}/{:s}", g_LogSessionDirectory, "net_trace.log"), SPDLOG_MAX_SIZE, SPDLOG_NUM_FILE)->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v");
	// High-volume net_observer telemetry, off the live console.
	spdlog::rotating_logger_mt<spdlog::async_factory_nonblock>("bridge_trace"
		, fmt::format("{:s}/{:s}", g_LogSessionDirectory, "bridge_trace.log"), SPDLOG_MAX_SIZE, SPDLOG_NUM_FILE)->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v");
	spdlog::rotating_logger_mt<spdlog::async_factory_nonblock>("netconsole"
		, fmt::format("{:s}/{:s}", g_LogSessionDirectory, "netconsole.log"), SPDLOG_MAX_SIZE, SPDLOG_NUM_FILE)->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v");
	spdlog::rotating_logger_mt<spdlog::async_factory_nonblock>("filesystem"
		, fmt::format("{:s}/{:s}", g_LogSessionDirectory, "filesystem.log"), SPDLOG_MAX_SIZE, SPDLOG_NUM_FILE)->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v");
}
#endif // !_TOOLS

#ifdef _TOOLS
// NOTE: used for tools as additional file logger on top of the existing terminal logger.
void SpdLog_InstallSupplementalLogger(const char* pszLoggerName, const char* pszLogFileName, const char* pszPattern, const bool bTruncate)
{
	g_SuppementalToolsLogger = spdlog::basic_logger_mt(pszLoggerName, pszLogFileName, bTruncate);
	g_SuppementalToolsLogger->set_pattern(pszPattern);
}
#endif // _TOOLS

//#############################################################################
// SPDLOG INIT
//#############################################################################
void SpdLog_Init(const bool bAnsiColor)
{
	static bool bInitialized = false;

	if (bInitialized)
	{
		Assert(bInitialized, "'SpdLog_Init()' has already been called.");
		return;
	}

#ifndef _TOOLS
	g_LogSessionUUID = CreateUUID();

	if (g_LogSessionUUID.empty())
	{
		// Fall-back directory in case of a failure.
		g_LogSessionUUID = "00000000-0000-0000-0000-000000000000";
	}

	// Role subdir so client.dll and server.dll never mix GUID sessions under
	// one platform/logs tree (unified s21-full install).
	constexpr const char* kLogRole = "client";
	g_LogSessionDirectory = fmt::format("platform/logs/{:s}/{:s}", kLogRole, g_LogSessionUUID);
	CreateDirectoryA("platform", nullptr);
	CreateDirectoryA("platform/logs", nullptr);
	CreateDirectoryA(fmt::format("platform/logs/{:s}", kLogRole).c_str(), nullptr);
	CreateDirectoryA(g_LogSessionDirectory.c_str(), nullptr);
	{
		FILE* pLatest = nullptr;
		if (fopen_s(&pLatest, fmt::format("platform/logs/{:s}/latest.txt", kLogRole).c_str(), "w") == 0 && pLatest)
		{
			fprintf(pLatest, "%s\n", g_LogSessionUUID.c_str());
			fclose(pLatest);
		}
	}
#endif // !_TOOLS
	/************************
	 * WINDOWS LOGGER SETUP *
	 ************************/
	{
#ifdef _TOOLS
		g_TermLogger = spdlog::default_logger();
#else
		g_TermLogger = spdlog::stdout_logger_mt("win_console");
#endif // _TOOLS

		// Determine if user wants ansi-color logging in the terminal.
		if (bAnsiColor)
		{
			g_TermLogger->set_pattern("%v\u001b[0m");
			g_bSpdLog_UseAnsiClr = true;
		}
		else
		{
			g_TermLogger->set_pattern("%v");
		}
	}

#ifndef _TOOLS
	spdlog::set_default_logger(g_TermLogger); // Set as default.

	// Backs every rotating file sink below. One worker so lines keep their
	// relative order across channels; the queue is the burst reserve that lets a
	// slow log volume fall behind without ever pushing back on the game thread.
	spdlog::init_thread_pool(SPDLOG_ASYNC_QUEUE_SIZE, 1);
	if (SpdLog_FileLogsEnabled())
	{
		SpdLog_CreateRotatingLoggers();
	}
	else
	{
		g_TermLogger->info("File logs disabled (release minimum): stdout + crash files only. "
			"-logfiles or -devsdk re-enables.\n");
	}
#endif // !_TOOLS

	spdlog::set_level(spdlog::level::trace);
	spdlog::flush_every(std::chrono::seconds(5));

	// Per-line flush on terminal + errors only. flush_on(trace) on every logger stalls the main thread.
	g_TermLogger->flush_on(spdlog::level::trace);
#ifndef _TOOLS
	static const char* const s_FlushErrLoggerNames[] = {
		"sdk(warning)", "sdk(error)", "squirrel_re(warning)",
	};
	for (const char* const pszName : s_FlushErrLoggerNames)
	{
		if (auto logger = spdlog::get(pszName))
			logger->flush_on(spdlog::level::err);
	}
#endif // !_TOOLS

	// Lets the crash handler drain the async queue on its way out.
	g_pfnPlatLogDrain = []() { spdlog::shutdown(); };

	std::atexit(SpdLog_AtExitMarkDead);
	g_bSpdLogAlive = true;
	bInitialized = true;
}

//#############################################################################
// SPDLOG SHUTDOWN
//#############################################################################
void SpdLog_Shutdown()
{
	g_bSpdLogAlive = false;
	g_pfnPlatLogDrain = nullptr;
	spdlog::shutdown();
#ifdef _TOOLS
	// Destroy the tools logger to flush it.
	g_SuppementalToolsLogger.reset();
#endif // !_TOOLS
}
#else // !CLIENT_DLL
#include "core/stdafx.h"
#include "core/logdef.h"
#ifndef _TOOLS
#include "tier0/commandline.h"
#endif // !_TOOLS
#include <cstdlib>

std::shared_ptr<spdlog::logger> g_TermLogger;
std::shared_ptr<spdlog::logger> g_SuppementalToolsLogger;

static void SpdLog_AtExitMarkDead(void)
{
	g_bSpdLogAlive = false;
}

#ifndef _TOOLS
//-----------------------------------------------------------------------------
// Full rotating set only with -devsdk/-dev/-developer/-logfiles.
// -nologfiles forces everything off, including the always-on warning trio.
//-----------------------------------------------------------------------------
bool SpdLog_FileLogsEnabled(void)
{
	static int s_fileLogs = -1;
	if (s_fileLogs < 0)
	{
		if (!g_bCommandLineCreated || !CommandLine())
			return false;

		const bool bDeveloper = CommandLine()->CheckParm("-devsdk")
			|| CommandLine()->CheckParm("-dev")
			|| CommandLine()->CheckParm("-developer");

		s_fileLogs = CommandLine()->CheckParm("-nologfiles") ? 0 :
			(bDeveloper || CommandLine()->CheckParm("-logfiles")) ? 1 : 0;
	}
	return s_fileLogs != 0;
}

static void SpdLog_CreateNamedRotating(const char* pszName, const char* pszFile)
{
	spdlog::rotating_logger_mt<spdlog::async_factory_nonblock>(pszName
		, fmt::format("{:s}/{:s}", g_LogSessionDirectory, pszFile), SPDLOG_MAX_SIZE, SPDLOG_NUM_FILE)
		->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v");
}

// Dedi-only when the full file set is off: C2S-SR / SCRIPT ERROR / Error()
// must stay greppable after printt is muted. Do not add squirrel_re / sdk
// here -- those are the printt and boot-chatter sinks. The client stays
// minimum-disk (crash files only) unless -devsdk / -logfiles.
static void SpdLog_CreateAlwaysOnLoggers()
{
	SpdLog_CreateNamedRotating("squirrel_re(warning)", "script_warning.log");
	SpdLog_CreateNamedRotating("sdk(warning)", "warning.log");
	SpdLog_CreateNamedRotating("sdk(error)", "error.log");
}

static void SpdLog_CreateRotatingLoggers()
{
	/************************
	 * ROTATE LOGGER SETUP *
	 ************************/
	// async_factory_nonblock: a sync sink fwrite blocks the emitting (often net) thread.
	// overrun_oldest drops old lines instead of stalling; timestamps are taken at the call.
	SpdLog_CreateAlwaysOnLoggers();
	SpdLog_CreateNamedRotating("squirrel_re", "script.log");
	SpdLog_CreateNamedRotating("sdk", "message.log");
	SpdLog_CreateNamedRotating("net_trace", "net_trace.log");
#ifndef DEDICATED
	SpdLog_CreateNamedRotating("netconsole", "netconsole.log");
#endif // !DEDICATED
	SpdLog_CreateNamedRotating("filesystem", "filesystem.log");
}
#endif // !_TOOLS

#ifdef _TOOLS
// NOTE: used for tools as additional file logger on top of the existing terminal logger.
void SpdLog_InstallSupplementalLogger(const char* pszLoggerName, const char* pszLogFileName, const char* pszPattern, const bool bTruncate)
{
	g_SuppementalToolsLogger = spdlog::basic_logger_mt(pszLoggerName, pszLogFileName, bTruncate);
	g_SuppementalToolsLogger->set_pattern(pszPattern);
}
#endif // _TOOLS

//#############################################################################
// SPDLOG INIT
//#############################################################################
void SpdLog_Init(const bool bAnsiColor)
{
	static bool bInitialized = false;

	if (bInitialized)
	{
		Assert(bInitialized, "'SpdLog_Init()' has already been called.");
		return;
	}

#ifndef _TOOLS
	g_LogSessionUUID = CreateUUID();

	if (g_LogSessionUUID.empty())
	{
		// Fall-back directory in case of a failure.
		g_LogSessionUUID = "00000000-0000-0000-0000-000000000000";
	}

	// Role subdir so client.dll and server.dll never mix GUID sessions under
	// one platform/logs tree (unified s21-full install).
	constexpr const char* kLogRole = "server";
	g_LogSessionDirectory = fmt::format("platform/logs/{:s}/{:s}", kLogRole, g_LogSessionUUID);
	CreateDirectoryA("platform", nullptr);
	CreateDirectoryA("platform/logs", nullptr);
	CreateDirectoryA(fmt::format("platform/logs/{:s}", kLogRole).c_str(), nullptr);
	CreateDirectoryA(g_LogSessionDirectory.c_str(), nullptr);
	{
		FILE* pLatest = nullptr;
		if (fopen_s(&pLatest, fmt::format("platform/logs/{:s}/latest.txt", kLogRole).c_str(), "w") == 0 && pLatest)
		{
			fprintf(pLatest, "%s\n", g_LogSessionUUID.c_str());
			fclose(pLatest);
		}
	}
#endif // !_TOOLS
	/************************
	 * WINDOWS LOGGER SETUP *
	 ************************/
	{
#ifdef _TOOLS
		g_TermLogger = spdlog::default_logger();
#else
		g_TermLogger = spdlog::stdout_logger_mt("win_console");
#endif // _TOOLS

		// Determine if user wants ansi-color logging in the terminal.
		if (bAnsiColor)
		{
			g_TermLogger->set_pattern("%v\u001b[0m");
			g_bSpdLog_UseAnsiClr = true;
		}
		else
		{
			g_TermLogger->set_pattern("%v");
		}
	}

#ifndef _TOOLS
	spdlog::set_default_logger(g_TermLogger); // Set as default.

	// Backs every rotating file sink below. One worker so lines keep their
	// relative order across channels; the queue is the burst reserve that lets a
	// slow log volume fall behind without ever pushing back on the game thread.
	spdlog::init_thread_pool(SPDLOG_ASYNC_QUEUE_SIZE, 1);
	if (SpdLog_FileLogsEnabled())
	{
		SpdLog_CreateRotatingLoggers();
	}
#ifdef DEDICATED
	else if (!CommandLine()->CheckParm("-nologfiles"))
	{
		SpdLog_CreateAlwaysOnLoggers();
		g_TermLogger->info("Minimum-disk: warning.log / error.log / script_warning.log on. "
			"Full file set: -logfiles or -devsdk.\n");
	}
	else
	{
		g_TermLogger->info("File logs disabled (-nologfiles): stdout + crash files only.\n");
	}
#else
	else
	{
		g_TermLogger->info("File logs disabled (release minimum): stdout + crash files only. "
			"-logfiles or -devsdk re-enables.\n");
	}
#endif // DEDICATED
#endif // !_TOOLS

	spdlog::set_level(spdlog::level::trace);
	spdlog::flush_every(std::chrono::seconds(5));

	// Lets the crash handler drain the async queue on its way out.
	g_pfnPlatLogDrain = []() { spdlog::shutdown(); };

	std::atexit(SpdLog_AtExitMarkDead);
	g_bSpdLogAlive = true;
	bInitialized = true;
}

//#############################################################################
// SPDLOG SHUTDOWN
//#############################################################################
void SpdLog_Shutdown()
{
	g_bSpdLogAlive = false;
	g_pfnPlatLogDrain = nullptr;
	spdlog::shutdown();
#ifdef _TOOLS
	// Destroy the tools logger to flush it.
	g_SuppementalToolsLogger.reset();
#endif // !_TOOLS
}
#endif // CLIENT_DLL
