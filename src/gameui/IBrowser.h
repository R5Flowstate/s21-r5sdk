#pragma once
#ifndef DEDICATED
#include "common/sdkdefs.h"
#include "windows/resource.h"
#include "networksystem/serverlisting.h"
#include "networksystem/spire.h"
#include "thirdparty/imgui/misc/imgui_utility.h"

#include "imgui_surface.h"
#include <atomic>

class CBrowser : public CImguiSurface
{
public:
    CBrowser(void);
    virtual ~CBrowser(void);

    virtual bool Init(void);
    virtual void Shutdown(void);

    virtual void RunFrame(void);
    void RunTask(void);

    virtual bool DrawSurface(void);

    void DrawBrowserPanel(void);
    void RefreshServerList(void);

    void HiddenServersModal(void);
#if defined(CLIENT_DLL)
    virtual bool IsModal(void) const override { return false; }
    void DrawManageLocalPanel(void);
    void PasswordPromptModal(void);
#endif // CLIENT_DLL

    void HandleInvalidFields(const bool offline);
    void DrawHostPanel(void);

    void UpdateHostingStatus(void);
    void InstallHostingDetails(const bool postFailed, const string& hostMessage, const string& hostToken, const string& hostIp);
    void SendHostingPostRequest(NetGameServer_t& gameServer);

    void ProcessCommand(const char* pszCommand) const;

public:
    // Command callbacks
    static void ToggleBrowser_f();
#if defined(CLIENT_DLL)
    static void ToggleLocal_f();
#endif // CLIENT_DLL

private:
    inline void SetServerListMessage(const char* const message) { m_serverListMessage = message; };
    inline void SetHostMessage(const char* const message) { m_hostMessage = message; }
    inline void SetHostToken(const char* const message) { m_hostToken = message; }

private:
    bool m_reclaimFocusOnTokenField;
    bool m_queryNewListNonRecursive; // When set, refreshes the server list once the next frame.
    bool m_queryGlobalBanList;
    char m_serverTokenTextBuf[128];
    char m_serverAddressTextBuf[128];
    char m_serverNetKeyTextBuf[45];
    char m_serverPasswordTextBuf[1128];

    ID3D11ShaderResourceView* m_lockedIconShaderResource;
    MODULERESOURCE m_lockedIconDataResource;

    ////////////////////
    // Server List //
    ////////////////////
    ImGuiTextFilter m_serverBrowserTextFilter;
    string m_serverListMessage;

#if defined(CLIENT_DLL)
    // Row filters, all off by default so the browser opens showing everything
    // the masterserver returned.
    bool m_hideLockedServers = false;
    bool m_hideEmptyServers = false;
    bool m_hideFullServers = false;
    bool m_hideIncompatibleServers = false;

    bool m_autoRefresh = false;
    float m_autoRefreshInterval = 30.f;

    // layout.ini restores the window wherever it was last left, which can be
    // off the edge of a smaller viewport with no way to drag it back. Track the
    // open edge so the window can be re-parked beside the console when it is.
    bool m_wasActivated = false;
    bool m_placeOnOpen = true;

    // ImGui time base, not wall clock; only ever compared against itself.
    double m_lastRefreshTime = 0.0;

    // The worker hands its result back through these rather than through
    // g_TaskQueue: that queue is pumped by _Host_RunFrame, and VHost is not in
    // the safe-mode allowlist, so a dispatched callback never runs on this
    // build and the request would look permanently in flight. Only one request
    // exists at a time, so the pending message has a single writer.
    std::atomic<bool> m_refreshInFlight{ false };
    std::atomic<bool> m_refreshComplete{ false };
    string m_pendingListMessage;

    // The password prompt is drawn once outside the table rather than per row:
    // a row-owned popup is keyed by display index, so sorting or filtering
    // between the click and the submit would aim it at a different server.
    bool m_openPasswordPrompt = false;
    string m_passwordPromptName;
    string m_passwordPromptAddress;
    string m_passwordPromptNetKey;
    int m_passwordPromptPort = 0;
#endif // CLIENT_DLL

    ////////////////////
    // Host Server //
    ////////////////////
    string m_hostMessage;
    string m_hostToken;
    ImVec4 m_hostMessageColor;

    ////////////////////
    // Private Server //
    ////////////////////
    string m_hiddenServerRequestMessage;
    ImVec4 m_hiddenServerMessageColor;

    string m_serverName;
    string m_serverDescription;

    string m_levelName;
    string m_modeName;
};

extern CBrowser g_Browser;
#endif
