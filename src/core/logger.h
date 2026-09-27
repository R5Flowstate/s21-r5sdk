#ifndef LOGGER_H
#define LOGGER_H

void EngineLoggerSink(LogType_t logType, LogLevel_t logLevel, eDLL_T context,
	const char* pszLogger, const char* pszFormat, va_list args,
	const UINT exitCode /*= NO_ERROR*/, const char* pszUptimeOverride /*= nullptr*/);

// Sees every line before it is emitted; returning true swallows the line.
typedef bool (*PFN_LogTap)(eDLL_T context, LogType_t logType, const char* pszText, size_t nLen);
extern PFN_LogTap g_LogTap;

#endif // LOGGER_H
