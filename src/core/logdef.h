#pragma once

#include <sstream>
#include "thirdparty/spdlog/spdlog.h"
#include "thirdparty/spdlog/async.h"
#include "thirdparty/spdlog/sinks/ostream_sink.h"
#include "thirdparty/spdlog/sinks/basic_file_sink.h"
#include "thirdparty/spdlog/sinks/stdout_sinks.h"
#include "thirdparty/spdlog/sinks/stdout_color_sinks.h"
#include "thirdparty/spdlog/sinks/ansicolor_sink.h"
#include "thirdparty/spdlog/sinks/rotating_file_sink.h"

constexpr int SPDLOG_MAX_SIZE = 10 * 1024 * 1024; // Sets number of bytes before rotating logger.
// Sets number of files to rotate to. A roll walks EVERY index, so this count is
// paid in filesystem calls by whichever thread happened to write the line that
// filled the sink. At 512 that is a multi-hundred-millisecond stall on a slow
// log volume -- long enough for the net process-time watchdog to drop a client
// mid-game. 16 x 10 MiB is still 160 MiB of history per channel.
constexpr int SPDLOG_NUM_FILE = 16;

// Slots in the async file-sink queue. Sized to ride out a multi-second write
// stall on the log volume at the observed steady-state rate (~20 lines/s) with
// room for a connect-time burst. Overflow drops the oldest line rather than
// blocking the emitting thread -- see the factory comment in logdef.cpp.
constexpr size_t SPDLOG_ASYNC_QUEUE_SIZE = 32768;

inline bool g_bSpdLog_UseAnsiClr = false;

// False after SpdLog_Shutdown / atexit. EngineLoggerSink must check before
// spdlog access -- static CSocketCreator dtors call Error() after registry death.
inline bool g_bSpdLogAlive = false;

extern std::shared_ptr<spdlog::logger> g_TermLogger;

#ifdef _TOOLS
extern std::shared_ptr<spdlog::logger> g_SuppementalToolsLogger;
#endif // _TOOLS

void SpdLog_Init(const bool bAnsiColor);
void SpdLog_Shutdown(void);

// False unless -devsdk/-dev/-developer or -logfiles is on the command line
// (-nologfiles wins over all three). When false, no rotating file sinks are
// created: stdout is the log, crash-time files still land in the session dir.
bool SpdLog_FileLogsEnabled(void);

#ifdef _TOOLS
void SpdLog_InstallSupplementalLogger(const char* pszLoggerName, const char* pszLogFileName,
	const char* pszPattern = "[%Y-%m-%d %H:%M:%S.%e] %v", const bool bTruncate = true);
#endif // _TOOLS
