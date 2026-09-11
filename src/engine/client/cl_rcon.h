#if defined(CLIENT_DLL)
#pragma once
#include "tier1/NetAdr.h"
#include "tier2/socketcreator.h"
#include "protoc/netcon.pb.h"
#include "engine/shared/base_rcon.h"

class CRConClient : public CNetConBase
{
public:
	CRConClient(void);
	~CRConClient(void);

	void Init(const char* pNetKey = nullptr);
	void Shutdown(void);
	void RunFrame(void);

	virtual void Disconnect(const char* szReason = nullptr) override;
	virtual bool ProcessMessage(ConnectedNetConsoleData_s& data, const u32 nMaxLen) override;

	bool Serialize(ConnectedNetConsoleData_s& data, vector<byte>& vecBuf, const char* szReqBuf, const size_t nReqMsgLen,
		const char* szReqVal, const size_t nReqValLen, const netcon::request_e requestType) const;

	void RequestConsoleLog(const bool bWantLog);
	bool ShouldReceive(void);

	bool IsRemoteLocal(void);
	bool IsInitialized(void) const;
	bool IsConnected(void);

	ConnectedNetConsoleData_s* GetData(void);
	SocketHandle_t GetSocket(void);

private:
	bool m_bInitialized;
};

CRConClient* RCONClient();

// Whole-token (space-delimited, case-insensitive) membership test against
// 'cl_bridge_server_cmds' -- console verbs that are server-authority and must
// not run as a local no-op on the client.
bool RCON_IsServerAuthorityCmd(const char* pszArg0);
#else // !CLIENT_DLL
#pragma once
#include "tier1/NetAdr.h"
#include "tier2/socketcreator.h"
#include "protoc/netcon.pb.h"
#include "engine/shared/base_rcon.h"

class CRConClient : public CNetConBase
{
public:
	CRConClient(void);
	~CRConClient(void);

	void Init(const char* pNetKey = nullptr);
	void Shutdown(void);
	void RunFrame(void);

	virtual void Disconnect(const char* szReason = nullptr) override;
	virtual bool ProcessMessage(ConnectedNetConsoleData_s& data, const u32 nMaxLen) override;

	bool Serialize(ConnectedNetConsoleData_s& data, vector<byte>& vecBuf, const char* szReqBuf, const size_t nReqMsgLen,
		const char* szReqVal, const size_t nReqValLen, const netcon::request_e requestType) const;

	void RequestConsoleLog(const bool bWantLog);
	bool ShouldReceive(void);

	bool IsRemoteLocal(void);
	bool IsInitialized(void) const;
	bool IsConnected(void);

	ConnectedNetConsoleData_s* GetData(void);
	SocketHandle_t GetSocket(void);

private:
	bool m_bInitialized;
};

CRConClient* RCONClient();
#endif // CLIENT_DLL
