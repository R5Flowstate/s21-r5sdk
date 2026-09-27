//=============================================================================//
//
// Purpose: loopback line socket for a local agent (MCP server, headless
//          ReMap). Runs script and console commands from the host frame and
//          answers each request with what it printed. Dev only: on with
//          -devsdk or -agentlink [port], off with -noagentlink. Binds
//          127.0.0.1, up to four local clients, per-launch token in
//          platform/agentlink.token (dedi) or agentlink_client.token.
//
//          Protocol (one request per line, replies are single lines):
//            AUTH <token>                 -> OK | ERR
//            INFO <id>                    -> RESULT <id> ok {...}
//            EVAL <id> <server|client|ui> <code>
//            EVAL64 <id> <server|client|ui> <base64 code>  (multi-line)
//            CMD  <id> <console line>
//            LOG  <id> <sinceSeq> [max]
//            SCRIPT <code> / PING / FLUSH   (fire and forget)
//          A script answers an EVAL by calling AgentLink_Reply( string ).
//
//=============================================================================//
#include "core/stdafx.h"
#include "core/logger.h"
#include "engine/agent_link.h"
#include "engine/cmd.h"
#include "vscript/vscript.h"
#include "tier0/threadtools.h"
#include "tier0/commandline.h"
#include "tier1/cvar.h"
#include "tier0/utility.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wincrypt.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"

#if defined(CLIENT_DLL)
#include "vscript/vsquirrel_s21.h"
#else
#include "engine/host_state.h"
#include "game/shared/scriptremotefunctions_server.h"
#endif // CLIENT_DLL

namespace
{
#if defined(CLIENT_DLL)
	constexpr int kDefaultPort = 37020;
	constexpr const char* kTokenFile = "agentlink_client.token";
	constexpr const char* kRole = "client";
#else
	constexpr int kDefaultPort = 37019;
	constexpr const char* kTokenFile = "agentlink.token";
	constexpr const char* kRole = "server";
#endif // CLIENT_DLL

	constexpr int kLineMax = 65536;
	constexpr size_t kQueueCap = 256;
	constexpr size_t kQueueBytesMax = 8u << 20;
	constexpr int kQueueWaitMs = 2000;
	constexpr size_t kLogRingMax = 4000;
	constexpr size_t kLogTextMax = 4096;
	constexpr size_t kCaptureLinesMax = 400;
	constexpr int kLogReplyMax = 1000;
	constexpr size_t kReplyMax = 8u << 20;
	constexpr LONG kMaxClients = 4;
	constexpr DWORD kAuthTimeoutMs = 10000;
	constexpr DWORD kSendTimeoutMs = 30000;
	constexpr size_t kOutQueueMax = 64u << 20;
	constexpr size_t kCmdMax = 8192;
	// Work per host frame; a burst from the agent must not stall the tick.
	constexpr double kFrameBudgetSec = 0.008;

	// Replies are queued here and written by the connection's writer thread,
	// so the host frame never blocks on a slow reader.
	struct Conn_s
	{
		std::mutex m;
		std::condition_variable cv;
		SOCKET s = INVALID_SOCKET;
		bool open = false;
		std::deque<std::string> out;
		size_t outBytes = 0;

		~Conn_s()
		{
			if (s != INVALID_SOCKET)
				closesocket(s);
		}
	};
	using ConnPtr = std::shared_ptr<Conn_s>;

	enum class Op_t
	{
		SCRIPT,
		EVAL,
		CMD
	};

	struct Request_s
	{
		Op_t op;
		std::string id;
		SQCONTEXT ctx;
		std::string text;
		ConnPtr conn;
	};

	void CloseConn(Conn_s& conn)
	{
		conn.open = false;
		conn.out.clear();
		conn.outBytes = 0;
		shutdown(conn.s, SD_BOTH);
		conn.cv.notify_all();
	}

	struct LogEntry_s
	{
		int64_t seq;
		const char* level;
		const char* source;
		std::string text;
	};

	struct Capture_s
	{
		bool hasReply;
		std::string reply;
		bool sawError;
		size_t dropped;
		std::vector<LogEntry_s> lines;
	};

	bool s_bEnabled = false;
	int s_nPort = kDefaultPort;
	SOCKET s_hListen = INVALID_SOCKET;
	char s_szToken[65] = {};

	volatile LONG s_nClients = 0;

	std::mutex& s_QueueMutex = *new std::mutex;
	std::deque<Request_s>& s_Queue = *new std::deque<Request_s>;
	size_t s_nQueueBytes = 0;
	volatile LONG s_nDrained = 0;

	std::mutex& s_LogRingMutex = *new std::mutex;
	std::deque<LogEntry_s>& s_LogRing = *new std::deque<LogEntry_s>;
	int64_t s_nLogSeq = 0;

	// Host frame thread only, except the two atomics the log tap reads.
	Capture_s& s_Capture = *new Capture_s();
	std::atomic<bool> s_bCapturing{ false };
	std::atomic<DWORD> s_nCaptureThread{ 0 };
	bool s_bCmdPending = false;
	Request_s s_PendingCmd;

	void JsonAppendString(std::string& out, const char* psz, size_t n)
	{
		out += '"';
		for (size_t i = 0; i < n; ++i)
		{
			const unsigned char c = static_cast<unsigned char>(psz[i]);
			switch (c)
			{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (c < 0x20)
				{
					char szEsc[8];
					V_snprintf(szEsc, sizeof(szEsc), "\\u%04x", c);
					out += szEsc;
				}
				else
					out += static_cast<char>(c);
			}
		}
		out += '"';
	}

	void JsonAppendString(std::string& out, const std::string& s)
	{
		JsonAppendString(out, s.data(), s.size());
	}

	void JsonAppendLog(std::string& out, const LogEntry_s& e, bool bWithSeq)
	{
		out += '{';
		if (bWithSeq)
		{
			out += "\"seq\":";
			out += std::to_string(e.seq);
			out += ',';
		}
		out += "\"level\":\"";
		out += e.level;
		out += "\",\"source\":\"";
		out += e.source;
		out += "\",\"text\":";
		JsonAppendString(out, e.text);
		out += '}';
	}

	const char* LevelFor(LogType_t type, const char* psz, size_t n)
	{
		if (type == LogType_t::LOG_ERROR)
			return "error";
		if (type == LogType_t::LOG_WARNING || type == LogType_t::SQ_WARNING)
		{
			const std::string_view sv(psz, n);
			if (sv.find("SCRIPT ERROR") != std::string_view::npos)
				return "error";
			return "warn";
		}
		return "info";
	}

	const char* SourceFor(eDLL_T context)
	{
		switch (context)
		{
		case eDLL_T::SCRIPT_SERVER: return "script_server";
		case eDLL_T::SCRIPT_CLIENT: return "script_client";
		case eDLL_T::SCRIPT_UI: return "script_ui";
		case eDLL_T::SERVER: return "server";
		case eDLL_T::CLIENT: return "client";
		case eDLL_T::UI: return "ui";
		default: return "engine";
		}
	}

	bool LogTap(eDLL_T context, LogType_t type, const char* psz, size_t n)
	{
		const bool bCapturing = s_bCapturing.load(std::memory_order_acquire) && GetCurrentThreadId() == s_nCaptureThread.load(std::memory_order_relaxed);

		while (n && (psz[n - 1] == '\n' || psz[n - 1] == '\r'))
			--n;

		if (!n)
			return false;

		LogEntry_s e;
		e.level = LevelFor(type, psz, n);
		e.source = SourceFor(context);
		e.text.assign(psz, n < kLogTextMax ? n : kLogTextMax);

		if (bCapturing)
		{
			if (e.level[0] == 'e')
				s_Capture.sawError = true;
			if (s_Capture.lines.size() < kCaptureLinesMax)
				s_Capture.lines.push_back(e);
			else
				++s_Capture.dropped;
		}

		std::lock_guard<std::mutex> lock(s_LogRingMutex);
		e.seq = ++s_nLogSeq;
		s_LogRing.push_back(std::move(e));
		if (s_LogRing.size() > kLogRingMax)
			s_LogRing.pop_front();
		return false;
	}

	bool SendAll(SOCKET s, const char* psz, size_t n)
	{
		while (n)
		{
			const int r = send(s, psz, static_cast<int>(n > 0x100000 ? 0x100000 : n), 0);
			if (r <= 0)
				return false;
			psz += r;
			n -= static_cast<size_t>(r);
		}
		return true;
	}

	void QueueSend(const ConnPtr& conn, std::string&& line)
	{
		std::lock_guard<std::mutex> lock(conn->m);
		if (!conn->open)
			return;
		if (conn->outBytes + line.size() > kOutQueueMax)
		{
			// The reader stopped reading; drop it rather than buffer without end.
			CloseConn(*conn);
			return;
		}
		conn->outBytes += line.size();
		conn->out.push_back(std::move(line));
		conn->cv.notify_one();
	}

	DWORD WINAPI WriterThread(LPVOID pParam)
	{
		const ConnPtr conn = *static_cast<ConnPtr*>(pParam);
		delete static_cast<ConnPtr*>(pParam);
		for (;;)
		{
			std::string line;
			{
				std::unique_lock<std::mutex> lock(conn->m);
				conn->cv.wait(lock, [&] { return !conn->out.empty() || !conn->open; });
				if (!conn->open)
					return 0;
				line = std::move(conn->out.front());
				conn->out.pop_front();
				conn->outBytes -= line.size();
			}
			if (!SendAll(conn->s, line.data(), line.size()))
			{
				std::lock_guard<std::mutex> lock(conn->m);
				CloseConn(*conn);
				return 0;
			}
		}
	}

	bool IsOpen(const ConnPtr& conn)
	{
		std::lock_guard<std::mutex> lock(conn->m);
		return conn->open;
	}

	void SendLine(const ConnPtr& conn, const char* psz)
	{
		QueueSend(conn, std::string(psz));
	}

	void SendResult(const ConnPtr& conn, const std::string& id, bool bOk, const std::string& json)
	{
		std::string line = "RESULT ";
		line += id;
		line += bOk ? " ok " : " err ";
		line += json;
		line += '\n';

		QueueSend(conn, std::move(line));
	}

	std::string ErrorJson(const char* pszMessage)
	{
		std::string json = "{\"message\":";
		JsonAppendString(json, pszMessage, strlen(pszMessage));
		json += '}';
		return json;
	}

	void BeginCapture(void)
	{
		s_Capture.hasReply = false;
		s_Capture.reply.clear();
		s_Capture.sawError = false;
		s_Capture.dropped = 0;
		s_Capture.lines.clear();
		s_nCaptureThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
		s_bCapturing.store(true, std::memory_order_release);
	}

	std::string EndCapture(bool bRanOk)
	{
		s_bCapturing.store(false, std::memory_order_release);

		std::string json = "{\"ran\":";
		json += bRanOk ? "true" : "false";
		json += ",\"error\":";
		json += (!bRanOk || s_Capture.sawError) ? "true" : "false";
		json += ",\"reply\":";
		if (s_Capture.hasReply)
			JsonAppendString(json, s_Capture.reply);
		else
			json += "null";
		json += ",\"log\":[";
		for (size_t i = 0; i < s_Capture.lines.size(); ++i)
		{
			if (i)
				json += ',';
			JsonAppendLog(json, s_Capture.lines[i], false);
		}
		json += "],\"logDropped\":";
		json += std::to_string(s_Capture.dropped);
		json += '}';
		return json;
	}

	bool RunScript(const char* pszCode, SQCONTEXT ctx, const char** ppszWhy)
	{
#if defined(CLIENT_DLL)
		if (ctx == SQCONTEXT::SERVER)
		{
			*ppszWhy = "the server VM lives on the dedicated server link";
			return false;
		}
		if (!Script_Execute_S21(pszCode, ctx))
		{
			*ppszWhy = "script failed (VM not running, compile error or runtime error; see log)";
			return false;
		}
		return true;
#else
		if (ctx != SQCONTEXT::SERVER)
		{
			*ppszWhy = "client and ui VMs live on the client link";
			return false;
		}
		CSquirrelVM* const s = Script_GetScriptHandle(SQCONTEXT::SERVER);
		if (!s || !s->GetVM())
		{
			*ppszWhy = "server VM is not running (no map loaded)";
			return false;
		}

		ThreadJoinServerJob();

		const HostStates_t iStateBefore = g_pHostState ? g_pHostState->m_iCurrentState : HostStates_t::HS_RUN;
		const HostStates_t iNextBefore = g_pHostState ? g_pHostState->m_iNextState : HostStates_t::HS_RUN;

		const bool bOk = s->Run(pszCode);

		// An uncaught VM error schedules a host shutdown; agent input must
		// never take the match down with it.
		if (g_pHostState
			&& iStateBefore != HostStates_t::HS_GAME_SHUTDOWN && iNextBefore != HostStates_t::HS_GAME_SHUTDOWN
			&& (g_pHostState->m_iCurrentState == HostStates_t::HS_GAME_SHUTDOWN
				|| g_pHostState->m_iNextState == HostStates_t::HS_GAME_SHUTDOWN))
		{
			g_pHostState->m_iCurrentState = iStateBefore;
			g_pHostState->m_iNextState = iNextBefore;
			Warning(eDLL_T::SERVER, "[AGENT-LINK] cancelled host shutdown scheduled by agent script\n");
		}

		ScriptRemoteC2S_DropFnCache();
		if (!bOk)
			*ppszWhy = "script failed (compile or runtime error; see log)";
		return bOk;
#endif // CLIENT_DLL
	}

#if !defined(CLIENT_DLL)
	void RunCommandLine(const std::string& line)
	{
		size_t i = 0;
		while (i < line.size())
		{
			std::string one;
			bool bQuoted = false;
			for (; i < line.size(); ++i)
			{
				const char c = line[i];
				if (c == '"')
					bQuoted = !bQuoted;
				else if (!bQuoted && (c == ';' || c == '\n'))
				{
					++i;
					break;
				}
				one += c;
			}

			const size_t nStart = one.find_first_not_of(" \t");
			if (nStart == std::string::npos)
				continue;
			one.erase(0, nStart);
			while (!one.empty() && (one.back() == ' ' || one.back() == '\t'))
				one.pop_back();
			const std::string name = one.substr(0, one.find_first_of(" \t"));
			Cmd_ExecuteUnrestricted(name.c_str(), one.c_str());
		}
	}
#endif // !CLIENT_DLL

	// Whole-string check; the shared IsValidBase64 wants padding and only
	// searches for a valid run inside the input.
	bool IsStrictBase64(const std::string& s)
	{
		if (s.empty() || s.size() % 4 != 0)
			return false;
		size_t nPad = 0;
		for (size_t i = 0; i < s.size(); ++i)
		{
			const char c = s[i];
			if (c == '=')
			{
				if (i < s.size() - 2)
					return false;
				++nPad;
				continue;
			}
			if (nPad || !(V_isalnum(c) || c == '+' || c == '/'))
				return false;
		}
		return nPad <= 2;
	}

	bool IsValidId(const char* psz, size_t n)
	{
		if (!n || n > 32)
			return false;
		for (size_t i = 0; i < n; ++i)
		{
			if (!V_isalnum(psz[i]) && psz[i] != '-' && psz[i] != '_')
				return false;
		}
		return true;
	}

	// Splits "<id> <rest>" off pszArgs. Returns false on a bad id.
	bool SplitId(const char* pszArgs, std::string& id, const char*& pszRest)
	{
		const char* pSpace = strchr(pszArgs, ' ');
		const size_t n = pSpace ? static_cast<size_t>(pSpace - pszArgs) : strlen(pszArgs);
		if (!IsValidId(pszArgs, n))
			return false;
		id.assign(pszArgs, n);
		pszRest = pSpace ? pSpace + 1 : pszArgs + n;
		return true;
	}

	bool Enqueue(Request_s&& req)
	{
		const DWORD nStart = GetTickCount();
		for (;;)
		{
			{
				std::lock_guard<std::mutex> lock(s_QueueMutex);
				if (s_Queue.size() < kQueueCap && s_nQueueBytes + req.text.size() <= kQueueBytesMax)
				{
					s_nQueueBytes += req.text.size();
					s_Queue.push_back(std::move(req));
					return true;
				}
			}
			if (GetTickCount() - nStart > static_cast<DWORD>(kQueueWaitMs))
				return false;
			Sleep(5);
		}
	}

	size_t QueueCount(void)
	{
		std::lock_guard<std::mutex> lock(s_QueueMutex);
		return s_Queue.size();
	}

	bool ParseContext(const char* psz, size_t n, SQCONTEXT& ctx)
	{
		const std::string_view sv(psz, n);
		if (sv == "server")
			ctx = SQCONTEXT::SERVER;
		else if (sv == "client")
			ctx = SQCONTEXT::CLIENT;
		else if (sv == "ui")
			ctx = SQCONTEXT::UI;
		else
			return false;
		return true;
	}

	void HandleLog(const std::string& id, const char* pszArgs, const ConnPtr& conn)
	{
		const int64_t nSince = _strtoi64(pszArgs, nullptr, 10);
		const char* pMax = strchr(pszArgs, ' ');
		int nMax = pMax ? atoi(pMax + 1) : 200;
		if (nMax <= 0 || nMax > kLogReplyMax)
			nMax = kLogReplyMax;

		std::string json = "{\"lines\":[";
		int64_t nNext = nSince;
		bool bGap = false;
		{
			std::lock_guard<std::mutex> lock(s_LogRingMutex);
			// Newest lines win when the window is larger than max.
			size_t nFirst = 0;
			size_t nNewer = 0;
			for (size_t i = s_LogRing.size(); i-- > 0;)
			{
				if (s_LogRing[i].seq <= nSince)
					break;
				++nNewer;
			}
			nFirst = s_LogRing.size() - nNewer;
			if (nNewer > static_cast<size_t>(nMax))
				nFirst = s_LogRing.size() - static_cast<size_t>(nMax);

			bool bFirst = true;
			for (size_t i = nFirst; i < s_LogRing.size(); ++i)
			{
				if (!bFirst)
					json += ',';
				bFirst = false;
				JsonAppendLog(json, s_LogRing[i], true);
			}
			nNext = s_nLogSeq;
			bGap = nSince > 0 && nFirst < s_LogRing.size() && s_LogRing[nFirst].seq > nSince + 1;
		}
		json += "],\"gap\":";
		json += bGap ? "true" : "false";
		json += ",\"next\":";
		json += std::to_string(nNext);
		json += '}';
		SendResult(conn, id, true, json);
	}

	void HandleInfo(const std::string& id, const ConnPtr& conn)
	{
		int64_t nSeq;
		{
			std::lock_guard<std::mutex> lock(s_LogRingMutex);
			nSeq = s_nLogSeq;
		}
		std::string json = "{\"protocol\":2,\"role\":\"";
		json += kRole;
		json += "\",\"port\":";
		json += std::to_string(s_nPort);
		json += ",\"queued\":";
		json += std::to_string(QueueCount());
		json += ",\"clients\":";
		json += std::to_string(s_nClients);
		json += ",\"drained\":";
		json += std::to_string(s_nDrained);
		json += ",\"logSeq\":";
		json += std::to_string(nSeq);
		json += '}';
		SendResult(conn, id, true, json);
	}

	void HandleLine(const ConnPtr& conn, char* pszLine, bool& bAuthed)
	{
		size_t n = strlen(pszLine);
		while (n && (pszLine[n - 1] == '\r' || pszLine[n - 1] == ' '))
			pszLine[--n] = '\0';
		if (!n)
			return;

		if (strncmp(pszLine, "AUTH ", 5) == 0)
		{
			const char* pszGiven = pszLine + 5;
			int nDiff = static_cast<int>(strlen(pszGiven)) ^ 64;
			for (int i = 0; i < 64 && pszGiven[i]; ++i)
				nDiff |= pszGiven[i] ^ s_szToken[i];
			bAuthed = nDiff == 0;
			if (!bAuthed)
			{
				// Written directly: the connection is dropped before its writer would run.
				send(conn->s, "ERR bad token\n", 14, 0);
				std::lock_guard<std::mutex> lock(conn->m);
				CloseConn(*conn);
				return;
			}
			const DWORD nNoTimeout = 0;
			setsockopt(conn->s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&nNoTimeout), sizeof(nNoTimeout));
			SendLine(conn, "OK\n");
			return;
		}
		if (!bAuthed)
		{
			SendLine(conn, "ERR auth first\n");
			return;
		}

		if (strcmp(pszLine, "PING") == 0)
		{
			char szReply[64];
			V_snprintf(szReply, sizeof(szReply), "OK queued %zu drained %ld\n", QueueCount(), s_nDrained);
			SendLine(conn, szReply);
			return;
		}
		if (strcmp(pszLine, "FLUSH") == 0)
		{
			const DWORD nStart = GetTickCount();
			while (QueueCount() > 0 && GetTickCount() - nStart < static_cast<DWORD>(kQueueWaitMs))
				Sleep(5);
			SendLine(conn, QueueCount() == 0 ? "OK\n" : "BUSY\n");
			return;
		}
		if (strncmp(pszLine, "SCRIPT ", 7) == 0 && pszLine[7])
		{
#if defined(CLIENT_DLL)
			const SQCONTEXT ctx = SQCONTEXT::CLIENT;
#else
			const SQCONTEXT ctx = SQCONTEXT::SERVER;
#endif // CLIENT_DLL
			SendLine(conn, Enqueue({ Op_t::SCRIPT, std::string(), ctx, pszLine + 7, conn }) ? "OK\n" : "BUSY\n");
			return;
		}

		const char* pSpace = strchr(pszLine, ' ');
		if (!pSpace)
		{
			SendLine(conn, "ERR unknown op\n");
			return;
		}
		const std::string_view op(pszLine, static_cast<size_t>(pSpace - pszLine));

		std::string id;
		const char* pszRest = nullptr;
		if (!SplitId(pSpace + 1, id, pszRest))
		{
			SendLine(conn, "ERR bad id\n");
			return;
		}

		if (op == "INFO")
		{
			HandleInfo(id, conn);
			return;
		}
		if (op == "LOG")
		{
			HandleLog(id, pszRest, conn);
			return;
		}
		if (op == "EVAL" || op == "EVAL64")
		{
			const char* pCode = strchr(pszRest, ' ');
			SQCONTEXT ctx;
			if (!pCode || !pCode[1] || !ParseContext(pszRest, static_cast<size_t>(pCode - pszRest), ctx))
			{
				SendResult(conn, id, false, ErrorJson("usage: EVAL <id> <server|client|ui> <code>"));
				return;
			}
			std::string code(pCode + 1);
			if (op == "EVAL64")
			{
				if (!IsStrictBase64(code))
				{
					SendResult(conn, id, false, ErrorJson("EVAL64 payload is not base64"));
					return;
				}
				code = Base64Decode(code);
				if (code.empty() || code.find('\0') != std::string::npos)
				{
					SendResult(conn, id, false, ErrorJson("EVAL64 payload is empty or holds a NUL byte"));
					return;
				}
			}
			if (!Enqueue({ Op_t::EVAL, id, ctx, std::move(code), conn }))
				SendResult(conn, id, false, ErrorJson("queue full"));
			return;
		}
		if (op == "CMD")
		{
			if (!*pszRest)
			{
				SendResult(conn, id, false, ErrorJson("usage: CMD <id> <console line>"));
				return;
			}
			if (strlen(pszRest) > kCmdMax)
			{
				SendResult(conn, id, false, ErrorJson("console line too long"));
				return;
			}
			if (!Enqueue({ Op_t::CMD, id, SQCONTEXT::NONE, pszRest, conn }))
				SendResult(conn, id, false, ErrorJson("queue full"));
			return;
		}
		SendLine(conn, "ERR unknown op\n");
	}

	void ServeClient(const ConnPtr& conn)
	{
		std::unique_ptr<char[]> buf(new char[kLineMax * 2 + 1]);
		const int nBufSize = kLineMax * 2;
		int nUsed = 0;
		bool bAuthed = false;
		bool bDiscarding = false;

		char szHello[64];
		V_snprintf(szHello, sizeof(szHello), "AGENTLINK 2 %s\n", kRole);
		SendLine(conn, szHello);

		for (;;)
		{
			const int r = recv(conn->s, buf.get() + nUsed, nBufSize - nUsed, 0);
			if (r <= 0)
				return;
			nUsed += r;
			buf[nUsed] = '\0';

			char* pStart = buf.get();
			for (;;)
			{
				char* const pNl = static_cast<char*>(memchr(pStart, '\n', buf.get() + nUsed - pStart));
				if (!pNl)
					break;
				*pNl = '\0';
				if (bDiscarding)
					bDiscarding = false;
				else if (pNl - pStart <= kLineMax)
					HandleLine(conn, pStart, bAuthed);
				else
					SendLine(conn, "ERR line too long\n");
				pStart = pNl + 1;
			}

			const int nRest = static_cast<int>(buf.get() + nUsed - pStart);
			memmove(buf.get(), pStart, static_cast<size_t>(nRest));
			nUsed = nRest;
			if (nUsed >= nBufSize)
			{
				SendLine(conn, "ERR line too long\n");
				bDiscarding = true;
				nUsed = 0;
			}
		}
	}

	DWORD WINAPI ClientThread(LPVOID pParam)
	{
		const ConnPtr conn = *static_cast<ConnPtr*>(pParam);
		delete static_cast<ConnPtr*>(pParam);
		ServeClient(conn);
		{
			std::lock_guard<std::mutex> lock(conn->m);
			CloseConn(*conn);
		}
		InterlockedDecrement(&s_nClients);
		return 0;
	}

	DWORD WINAPI ListenThread(LPVOID)
	{
		for (;;)
		{
			sockaddr_in peer{};
			int nLen = sizeof(peer);
			const SOCKET s = accept(s_hListen, reinterpret_cast<sockaddr*>(&peer), &nLen);
			if (s == INVALID_SOCKET)
				break;
			if (peer.sin_addr.S_un.S_addr != htonl(INADDR_LOOPBACK))
			{
				closesocket(s);
				continue;
			}

			// An unauthenticated connection may not hold a slot for long.
			setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&kAuthTimeoutMs), sizeof(kAuthTimeoutMs));
			setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&kSendTimeoutMs), sizeof(kSendTimeoutMs));

			if (InterlockedIncrement(&s_nClients) > kMaxClients)
			{
				InterlockedDecrement(&s_nClients);
				send(s, "ERR too many clients\n", 21, 0);
				closesocket(s);
				continue;
			}
			const ConnPtr conn = std::make_shared<Conn_s>();
			conn->s = s;
			conn->open = true;

			ConnPtr* const pWriter = new ConnPtr(conn);
			const HANDLE hWriter = CreateThread(nullptr, 0, WriterThread, pWriter, 0, nullptr);
			if (!hWriter)
			{
				delete pWriter;
				InterlockedDecrement(&s_nClients);
				continue;
			}
			CloseHandle(hWriter);

			ConnPtr* const pReader = new ConnPtr(conn);
			const HANDLE hReader = CreateThread(nullptr, 0, ClientThread, pReader, 0, nullptr);
			if (!hReader)
			{
				delete pReader;
				{
					std::lock_guard<std::mutex> lock(conn->m);
					CloseConn(*conn);
				}
				InterlockedDecrement(&s_nClients);
				continue;
			}
			CloseHandle(hReader);
		}
		return 0;
	}

	char s_szTokenFile[64] = {};

	bool MakeToken(void)
	{
		unsigned char raw[32];
		HCRYPTPROV hProv = 0;
		if (!CryptAcquireContextA(&hProv, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
			return false;
		const BOOL ok = CryptGenRandom(hProv, sizeof(raw), raw);
		CryptReleaseContext(hProv, 0);
		if (!ok)
			return false;
		for (int i = 0; i < 32; ++i)
			V_snprintf(s_szToken + i * 2, 3, "%02x", raw[i]);
		// Win32 rather than FileSystem(): the client's filesystem interface is
		// not the dedi's, and the token must land next to the exe either way.
		char szPath[MAX_PATH];
		const DWORD nLen = GetModuleFileNameA(nullptr, szPath, sizeof(szPath));
		if (!nLen || nLen >= sizeof(szPath))
			return false;
		char* const pSlash = strrchr(szPath, '\\');
		if (!pSlash)
			return false;
		pSlash[1] = '\0';
		V_strcat_sized(szPath, "platform\\", sizeof(szPath));
		V_strcat_sized(szPath, s_szTokenFile, sizeof(szPath));

		const HANDLE h = CreateFileA(szPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return false;
		DWORD nWritten = 0;
		const BOOL bWrote = WriteFile(h, s_szToken, 64, &nWritten, nullptr);
		CloseHandle(h);
		return bWrote && nWritten == 64;
	}

	void Init(void)
	{
		if (CommandLine()->CheckParm("-noagentlink"))
			return;
		if (!CommandLine()->CheckParm("-devsdk") && !CommandLine()->CheckParm("-agentlink"))
			return;
		s_nPort = CommandLine()->ParmValue("-agentlink", kDefaultPort);
		if (s_nPort <= 0 || s_nPort > 65535)
			s_nPort = kDefaultPort;

		// A second instance on another port must not overwrite the first one's token.
		if (s_nPort == kDefaultPort)
			V_strncpy(s_szTokenFile, kTokenFile, sizeof(s_szTokenFile));
		else
			V_snprintf(s_szTokenFile, sizeof(s_szTokenFile), "agentlink_%s_%d.token", kRole, s_nPort);

		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
			return;
		s_hListen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s_hListen == INVALID_SOCKET)
			return;
		// Without this another local process can bind the same port with
		// SO_REUSEADDR and take the connections.
		const BOOL bExclusive = TRUE;
		setsockopt(s_hListen, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&bExclusive), sizeof(bExclusive));
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(static_cast<u_short>(s_nPort));
		addr.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
		if (bind(s_hListen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s_hListen, SOMAXCONN) != 0)
		{
			Warning(eDLL_T::ENGINE, "[AGENT-LINK] bind 127.0.0.1:%d failed (%d)\n", s_nPort, WSAGetLastError());
			closesocket(s_hListen);
			s_hListen = INVALID_SOCKET;
			return;
		}
		// Only the instance that owns the port writes the token.
		if (!MakeToken())
		{
			Warning(eDLL_T::ENGINE, "[AGENT-LINK] token write failed -- link disabled\n");
			closesocket(s_hListen);
			s_hListen = INVALID_SOCKET;
			return;
		}
		const HANDLE hThread = CreateThread(nullptr, 0, ListenThread, nullptr, 0, nullptr);
		if (!hThread)
			return;
		CloseHandle(hThread);
		g_LogTap = &LogTap;
		s_bEnabled = true;
		Warning(eDLL_T::ENGINE, "[AGENT-LINK] DEV ONLY -- full %s control on 127.0.0.1:%d (token in platform/%s)\n",
			kRole, s_nPort, s_szTokenFile);
	}

	bool PopRequest(Request_s& out)
	{
		std::lock_guard<std::mutex> lock(s_QueueMutex);
		if (s_Queue.empty())
			return false;
		out = std::move(s_Queue.front());
		s_Queue.pop_front();
		s_nQueueBytes -= out.text.size();
		return true;
	}
}

void AgentLink_FrameBegin(void)
{
	static bool s_bInitTried = false;
	if (!s_bInitTried)
	{
		s_bInitTried = true;
		Init();
	}
	if (!s_bEnabled)
		return;

#if defined(CLIENT_DLL)
	// _Host_RunFrame did not come back through FrameEnd (it unwound); answer
	// the command with what was captured.
	if (s_bCmdPending)
		AgentLink_FrameEnd();
#endif // CLIENT_DLL
	// A request that an engine error unwound out of never closed its capture.
	s_bCapturing.store(false, std::memory_order_release);

	const double flStart = Plat_FloatTime();
	for (bool bFirst = true;; bFirst = false)
	{
		if (!bFirst && Plat_FloatTime() - flStart > kFrameBudgetSec)
			break;
		Request_s req;
		if (!PopRequest(req))
			break;
		// Scripts from a client that already left still run; answers need a reader.
		if (req.op != Op_t::SCRIPT && !IsOpen(req.conn))
			continue;

		InterlockedIncrement(&s_nDrained);

		if (req.op == Op_t::SCRIPT)
		{
			const char* pszWhy = nullptr;
			RunScript(req.text.c_str(), req.ctx, &pszWhy);
			continue;
		}
		if (req.op == Op_t::EVAL)
		{
			const char* pszWhy = nullptr;
			BeginCapture();
			const bool bOk = RunScript(req.text.c_str(), req.ctx, &pszWhy);
			std::string json = EndCapture(bOk);
			if (!bOk && pszWhy)
			{
				json.pop_back();
				json += ",\"message\":";
				JsonAppendString(json, pszWhy, strlen(pszWhy));
				json += '}';
			}
			SendResult(req.conn, req.id, bOk, json);
			continue;
		}

#if defined(CLIENT_DLL)
		// Client commands run inside the engine frame; one per frame keeps
		// each reply's output its own.
		std::string line = req.text;
		line += '\n';
		Cbuf_AddText(Cbuf_GetCurrentPlayer(), line.c_str(), cmd_source_t::kCommandSrcCode);
		s_PendingCmd = std::move(req);
		s_bCmdPending = true;
		BeginCapture();
		break;
#else
		BeginCapture();
		RunCommandLine(req.text);
		SendResult(req.conn, req.id, true, EndCapture(true));
#endif // CLIENT_DLL
	}
}

void AgentLink_FrameEnd(void)
{
	if (!s_bCmdPending)
		return;
	s_bCmdPending = false;
	SendResult(s_PendingCmd.conn, s_PendingCmd.id, true, EndCapture(true));
	s_PendingCmd.conn.reset();
}

bool AgentLink_IsCapturing(void)
{
	return s_bCapturing.load(std::memory_order_acquire) && GetCurrentThreadId() == s_nCaptureThread.load(std::memory_order_relaxed);
}

static SQRESULT AgentLink_ScriptReply(HSQUIRRELVM v)
{
	const SQChar* pszText = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &pszText)) || !pszText)
	{
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	if (!AgentLink_IsCapturing())
	{
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	const size_t nLen = strnlen(pszText, kReplyMax);
	s_Capture.hasReply = true;
	s_Capture.reply.assign(pszText, nLen);
	sq_pushbool(v, SQTrue);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT AgentLink_ScriptIsActive(HSQUIRRELVM v)
{
	sq_pushbool(v, s_bEnabled ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

#if defined(CLIENT_DLL)
void AgentLink_RegisterScriptFunctions(CSquirrelVM* s)
{
	if (!s)
		return;
	if (Script_RegisterFuncTC_S21(s, "AgentLink_Reply", reinterpret_cast<void*>(AgentLink_ScriptReply), "bool", "string text") == SQ_ERROR)
		Warning(eDLL_T::CLIENT, "[AGENT-LINK] AgentLink_Reply registration failed\n");
	if (Script_RegisterFuncTC_S21(s, "AgentLink_IsActive", reinterpret_cast<void*>(AgentLink_ScriptIsActive), "bool", "") == SQ_ERROR)
		Warning(eDLL_T::CLIENT, "[AGENT-LINK] AgentLink_IsActive registration failed\n");
}
#else
void AgentLink_RegisterScriptFunctions(CSquirrelVM* s)
{
	Script_RegisterFuncNamed(s, "AgentLink_Reply", "Server_Script_AgentLink_Reply",
		"Answers the agent request being run. Returns false outside one.", "bool", "string text", false, AgentLink_ScriptReply);
	Script_RegisterFuncNamed(s, "AgentLink_IsActive", "Server_Script_AgentLink_IsActive",
		"True when the agent link is listening.", "bool", "", false, AgentLink_ScriptIsActive);
}
#endif // CLIENT_DLL
