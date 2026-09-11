/******************************************************************************
-------------------------------------------------------------------------------
File : IBrowser.cpp
Date : 09:06:2021
Author : Sal
Purpose: Implements the in-game server browser front-end
-------------------------------------------------------------------------------
History
- 09:06:2021 21:07 : Created by Sal
- 25:07:2021 14:26 : Implement private servers connect dialog and password field

******************************************************************************/

#include "core/stdafx.h"
#include "core/init.h"
#include "core/resource.h"
#include "engine/client/bridge_connect_password.h"
#include "tier0/fasttimer.h"
#include "tier0/frametask.h"
#include "tier0/commandline.h"
#include "windows/id3dx.h"
#include "windows/console.h"
#include "windows/resource.h"
#include "engine/net.h"
#include "engine/cmd.h"
#include "engine/cmodel_bsp.h"
#include "engine/host.h"
#include "engine/host_state.h"
#include "engine/client/clientstate.h"
#include "engine/client/cl_rcon_launcher.h"
#include "networksystem/serverlisting.h"
#include "networksystem/spire.h"
#include "networksystem/hostmanager.h"
#include "networksystem/listmanager.h"
#include "rtech/playlists/playlists.h"
#include "common/callback.h"
#include "gameui/IBrowser.h"
#include "gameui/IConsole.h"
#include "game/shared/r1/weapon_parse.h"
#include "public/edict.h"
#include "game/shared/vscript_shared.h"
#include "game/server/gameinterface.h"
#include <engine/server/sv_main.cpp>
#include "pluginsystem/modsystem.h"
#include <algorithm>
#include "thirdparty/imgui/imgui_internal.h" // FindWindowByName

extern const char* Bridge_GetLevelBaseName();

static ConCommand togglebrowser("togglebrowser", CBrowser::ToggleBrowser_f, "Show/hide the server browser tab", FCVAR_CLIENTDLL | FCVAR_RELEASE);
static ConCommand togglelocal("togglelocal", CBrowser::ToggleLocal_f, "Show/hide the Manage Local tab", FCVAR_CLIENTDLL | FCVAR_RELEASE);

static constexpr int kBrowserModIdCap = 64;

struct BrowserModSnapshot_t
{
	char szRaw[MAX_MODS_TO_LOAD][kBrowserModIdCap];
	char szNorm[MAX_MODS_TO_LOAD][kBrowserModIdCap];
	int nCount;
	bool bEnabled;
};

static BrowserModSnapshot_t s_browserMods;

static void Browser_SnapshotLocalMods(BrowserModSnapshot_t& snap)
{
	snap.nCount = 0;
	snap.bEnabled = ModSystem()->IsEnabled();
	if (!snap.bEnabled)
		return;

	ModSystem()->LockModList();

	int nSafety = 0;
	FOR_EACH_VEC(ModSystem()->GetModList(), i)
	{
		if (++nSafety > MAX_MODS_TO_LOAD)
			break;

		const CModSystem::ModInstance_t* const pMod = ModSystem()->GetModList()[i];
		if (!pMod || !pMod->IsEnabled())
			continue;
		if (snap.nCount >= MAX_MODS_TO_LOAD)
			break;

		V_strncpy(snap.szRaw[snap.nCount], pMod->id.String(), kBrowserModIdCap);
		V_strncpy(snap.szNorm[snap.nCount],
			ModSystem()->GetNormalizedModID(pMod).String(), kBrowserModIdCap);
		++snap.nCount;
	}

	ModSystem()->UnlockModList();
}

static bool Browser_SnapshotHasId(const BrowserModSnapshot_t& snap, const char* pszReq)
{
	if (!pszReq || !pszReq[0])
		return false;

	for (int i = 0; i < snap.nCount; ++i)
	{
		if (!V_stricmp(snap.szRaw[i], pszReq))
			return true;
	}

	for (int i = 0; i < snap.nCount; ++i)
	{
		if (!V_stricmp(snap.szNorm[i], pszReq))
			return true;
	}

	return false;
}

// Returns true when the listing requires mods this client does not have enabled,
// and fills outMissing with a comma separated list of them.
static bool Browser_CollectMissingMods(const NetGameServer_t& server,
	const BrowserModSnapshot_t& snap, std::string& outMissing)
{
	outMissing.clear();
	if (server.requiredMods.empty())
		return false;

	bool bAny = false;
	for (const std::string& req : server.requiredMods)
	{
		if (snap.bEnabled && Browser_SnapshotHasId(snap, req.c_str()))
			continue;

		if (bAny)
			outMissing.append(", ");
		outMissing.append(req);
		bAny = true;
	}

	return bAny;
}

static bool Browser_HasRequiredMods(const NetGameServer_t& server, std::string& outMissing)
{
	Browser_SnapshotLocalMods(s_browserMods);
	return !Browser_CollectMissingMods(server, s_browserMods, outMissing);
}

// Column identifiers, handed to ImGui as the per-column user id so the sort
// specs stay meaningful after the user reorders or hides columns. Numbering
// starts at 1: a user id of 0 is what ImGui reports for a column that was never
// given one.
enum eBrowserColumn
{
	BROWSER_COLUMN_LOCK = 1,
	BROWSER_COLUMN_NAME,
	BROWSER_COLUMN_REGION,
	BROWSER_COLUMN_MAP,
	BROWSER_COLUMN_PLAYLIST,
	BROWSER_COLUMN_MODS,
	BROWSER_COLUMN_PLAYERS,
	BROWSER_COLUMN_PORT,
	BROWSER_COLUMN_CONNECT,
};

// One visible row: the listing plus the compatibility verdict computed while
// filtering, so neither the sort nor the draw has to recompute it.
struct BrowserRow_t
{
	const NetGameServer_t* server;
	std::string missingMods; // empty when this client can join
};

static int Browser_CompareColumn(const ImGuiID columnId, const NetGameServer_t& lhs, const NetGameServer_t& rhs)
{
	switch (columnId)
	{
	case BROWSER_COLUMN_LOCK:     return (lhs.hasPassword ? 1 : 0) - (rhs.hasPassword ? 1 : 0);
	case BROWSER_COLUMN_NAME:     return V_stricmp(lhs.name.c_str(), rhs.name.c_str());
	case BROWSER_COLUMN_REGION:   return V_stricmp(lhs.region.c_str(), rhs.region.c_str());
	case BROWSER_COLUMN_MAP:      return V_stricmp(lhs.map.c_str(), rhs.map.c_str());
	case BROWSER_COLUMN_PLAYLIST: return V_stricmp(lhs.playlist.c_str(), rhs.playlist.c_str());
	case BROWSER_COLUMN_MODS:     return int(lhs.requiredMods.size()) - int(rhs.requiredMods.size());
	case BROWSER_COLUMN_PLAYERS:  return lhs.numPlayers - rhs.numPlayers;
	case BROWSER_COLUMN_PORT:     return lhs.port - rhs.port;
	default:                      return 0;
	}
}

static void Browser_DrawServerTooltip(const NetGameServer_t& server, const std::string& missingMods)
{
	if (!ImGui::BeginItemTooltip())
		return;

	ImGui::TextUnformatted(server.name.c_str());

	if (!server.description.empty())
		ImGui::TextDisabled("%s", server.description.c_str());

	ImGui::Separator();

	ImGui::Text("Address  : %s:%d", server.address.c_str(), server.port);
	ImGui::Text("Region   : %s", server.region.empty() ? "unknown" : server.region.c_str());
	ImGui::Text("Map      : %s", server.map.c_str());
	ImGui::Text("Playlist : %s", server.playlist.c_str());
	ImGui::Text("Players  : %d/%d", server.numPlayers, server.maxPlayers);
	ImGui::Text("Version  : %s (checksum %08X)", server.versionId.c_str(), server.checksum);

	if (!server.modsProfile.empty())
		ImGui::Text("Profile  : %s", server.modsProfile.c_str());

	if (!server.requiredMods.empty())
	{
		ImGui::Separator();
		ImGui::TextUnformatted("Required mods:");

		for (const std::string& mod : server.requiredMods)
			ImGui::BulletText("%s", mod.c_str());
	}

	if (!missingMods.empty())
	{
		ImGui::Separator();
		ImGui::TextColored(ImVec4(1.00f, 0.35f, 0.35f, 1.00f), "Missing: %s", missingMods.c_str());
	}

	ImGui::EndTooltip();
}

//-----------------------------------------------------------------------------
// Purpose
//-----------------------------------------------------------------------------
CBrowser::CBrowser(void)
    : m_reclaimFocusOnTokenField(false)
    , m_queryNewListNonRecursive(false)
    , m_queryGlobalBanList(true)
    , m_lockedIconShaderResource(nullptr)
    , m_hostMessageColor(1.00f, 1.00f, 1.00f, 1.00f)
    , m_hiddenServerMessageColor(0.00f, 1.00f, 0.00f, 1.00f)
{
    m_surfaceLabel = "Server Browser";

    memset(m_serverTokenTextBuf, '\0', sizeof(m_serverTokenTextBuf));
    memset(m_serverAddressTextBuf, '\0', sizeof(m_serverAddressTextBuf));
    memset(m_serverNetKeyTextBuf, '\0', sizeof(m_serverNetKeyTextBuf));
    memset(m_serverPasswordTextBuf, '\0', sizeof(m_serverPasswordTextBuf));

    m_levelName = "mp_lobby";
    m_modeName = "dev_default";
}

//-----------------------------------------------------------------------------
// Purpose
//-----------------------------------------------------------------------------
CBrowser::~CBrowser(void)
{
    Shutdown();
}

//-----------------------------------------------------------------------------
// Purpose
//-----------------------------------------------------------------------------
bool CBrowser::Init(void)
{
    SetStyleVar();

    HMODULE sdkModule = reinterpret_cast<HMODULE>(g_SDKDll.GetModuleBase());
    m_lockedIconDataResource = GetModuleResource(sdkModule, IDB_PNG2);

    const bool ret = LoadTextureBuffer(reinterpret_cast<unsigned char*>(m_lockedIconDataResource.m_pData), int(m_lockedIconDataResource.m_nSize),
        &m_lockedIconShaderResource, &m_lockedIconDataResource.m_nWidth, &m_lockedIconDataResource.m_nHeight);

    IM_ASSERT(ret && m_lockedIconShaderResource);
    return ret;
}

//-----------------------------------------------------------------------------
// Purpose
//-----------------------------------------------------------------------------
void CBrowser::Shutdown(void)
{
    if (m_lockedIconShaderResource)
    {
        m_lockedIconShaderResource->Release();
    }
}

//-----------------------------------------------------------------------------
// Purpose: draws the main browser front-end
//-----------------------------------------------------------------------------
void CBrowser::RunFrame(void)
{
    if (!m_initialized)
    {
        Init();
        m_initialized = true;
    }

    RunTask();
}

//-----------------------------------------------------------------------------
// Purpose: runs tasks for the browser while not being drawn 
//-----------------------------------------------------------------------------
void CBrowser::RunTask()
{
    static bool bInitialized = false;
    static CFastTimer timer;

    // Pick up a finished refresh. The worker cannot touch ImGui state itself,
    // and the frame task queue it used to hand back through never runs here.
    if (m_refreshComplete.load(std::memory_order_acquire))
    {
        SetServerListMessage(m_pendingListMessage.c_str());

        m_refreshComplete.store(false, std::memory_order_relaxed);
        m_refreshInFlight.store(false, std::memory_order_release);
    }

    if (!bInitialized)
    {
        timer.Start();
        bInitialized = true;
    }

    if (timer.GetDurationInProgress().GetSeconds() > spire_host_update_interval.GetFloat())
    {
        UpdateHostingStatus();
        timer.Start();
    }

    if (g_Console.IsActivated())
    {
        if (m_queryNewListNonRecursive)
        {
            m_queryNewListNonRecursive = false;
            RefreshServerList();
        }
        else if (m_autoRefresh && !m_refreshInFlight && m_autoRefreshInterval > 0.f
            && (ImGui::GetTime() - m_lastRefreshTime) >= double(m_autoRefreshInterval))
        {
            RefreshServerList();
        }
    }
    else
    {
        m_reclaimFocusOnTokenField = true;
        m_queryNewListNonRecursive = true;
    }
}

//-----------------------------------------------------------------------------
// Purpose: draws the server browser's main surface
// Output: true if a frame has been drawn, false otherwise
//-----------------------------------------------------------------------------
bool CBrowser::DrawSurface(void)
{
    return false;
}

static char s_passwordBuf[128] = { 0 };

//-----------------------------------------------------------------------------
// Purpose: draws the server browser section
//-----------------------------------------------------------------------------
void CBrowser::DrawBrowserPanel(void)
{
    ImGui::BeginGroup();
    m_serverBrowserTextFilter.Draw("Filter (inc,-exc)##ServerBrowser");
    ImGui::SameLine();

    ImGui::BeginDisabled(m_refreshInFlight);

    if (ImGui::Button(m_refreshInFlight
        ? "Refreshing...##ServerBrowser_DrawBrowserPanel"
        : "Refresh##ServerBrowser_DrawBrowserPanel"))
    {
        m_serverListMessage.clear();
        RefreshServerList();
    }

    ImGui::EndDisabled();

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Matchmaking host: %s", spire_matchmaking_hostname.GetString());

    ImGui::SameLine();

    if (ImGui::Checkbox("Auto##ServerBrowser_AutoRefresh", &m_autoRefresh) && m_autoRefresh)
    {
        // Turning it on refreshes now rather than after the first interval.
        m_serverListMessage.clear();
        RefreshServerList();
    }

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Refresh every %.0f seconds while the browser is open.", m_autoRefreshInterval);

    ImGui::EndGroup();

    ImGui::BeginGroup();
    ImGui::Checkbox("Hide locked##ServerBrowser_HideLocked", &m_hideLockedServers);
    ImGui::SameLine();
    ImGui::Checkbox("Hide empty##ServerBrowser_HideEmpty", &m_hideEmptyServers);
    ImGui::SameLine();
    ImGui::Checkbox("Hide full##ServerBrowser_HideFull", &m_hideFullServers);
    ImGui::SameLine();
    ImGui::Checkbox("Hide incompatible##ServerBrowser_HideIncompatible", &m_hideIncompatibleServers);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hide servers requiring mods that are not enabled locally.");

    ImGui::EndGroup();

    ImGui::TextColored(ImVec4(1.00f, 0.00f, 0.00f, 1.00f), "%s", m_serverListMessage.c_str());
    ImGui::Separator();

    int frameStyleVars = 0; // Eliminate borders around server list table.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ 1.f, 0.f });  frameStyleVars++;

    // The footer holds the direct connect row plus the one line list summary.
    const float fFooterHeight = ImGui::GetStyle().ItemSpacing.y
        + ImGui::GetFrameHeightWithSpacing()
        + ImGui::GetTextLineHeightWithSpacing();

    // Connecting is deferred out of the row loop: the engine connect path runs
    // long and the list mutex is held across the whole table.
    bool pendingConnect = false;
    string connectAddress;
    string connectNetKey;
    int connectPort = 0;

    int listedServers = 0;
    int listedPlayers = 0;
    int totalServers = 0;

    const ImGuiTableFlags tableFlags =
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_Reorderable |
        ImGuiTableFlags_Hideable |
        ImGuiTableFlags_Sortable |
        ImGuiTableFlags_SortMulti |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY;

    // The id carries the column set: saved layouts are keyed by it and applied
    // by column index, so reusing the old id would drop the previous Map width
    // onto the newly inserted Region column.
    if (ImGui::BeginTable("##ServerBrowser_DrawBrowserPanel_ServerListTable_v3", 9, tableFlags, { 0, -fFooterHeight }))
    {
        if (m_surfaceStyle == ImGuiStyle_t::MODERN)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{ 8.f, 0.f }); frameStyleVars++;
        }
        else
        {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{ 4.f, 0.f }); frameStyleVars++;
        }

        ImGui::TableSetupColumn("##lock", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 12, BROWSER_COLUMN_LOCK);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort, 25, BROWSER_COLUMN_NAME);
        ImGui::TableSetupColumn("Region", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultHide, 8, BROWSER_COLUMN_REGION);
        ImGui::TableSetupColumn("Map", ImGuiTableColumnFlags_WidthStretch, 18, BROWSER_COLUMN_MAP);
        ImGui::TableSetupColumn("Playlist", ImGuiTableColumnFlags_WidthStretch, 10, BROWSER_COLUMN_PLAYLIST);
        ImGui::TableSetupColumn("Required Mods", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultHide, 14, BROWSER_COLUMN_MODS);
        ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortDescending, 6, BROWSER_COLUMN_PLAYERS);
        ImGui::TableSetupColumn("Port", ImGuiTableColumnFlags_WidthStretch, 5, BROWSER_COLUMN_PORT);
        ImGui::TableSetupColumn("##connect", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_NoHide, 8, BROWSER_COLUMN_CONNECT);

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        Browser_SnapshotLocalMods(s_browserMods);

        g_ServerListManager.m_Mutex.Lock();
        vector<BrowserRow_t> rows;

        // Filter the server list first before running it over the ImGui list
        // clipper, if we do this within the clipper, clipper.Step will fail
        // as the calculation for the remainder will be off.
        for (size_t i = 0; i < g_ServerListManager.m_vServerList.size(); i++)
        {
            const NetGameServer_t& server = g_ServerListManager.m_vServerList[i];
            totalServers++;

            const char* pszHostName = server.name.c_str();
            const char* pszHostMap = server.map.c_str();
            const char* pszPlaylist = server.playlist.c_str();
            const char* pszRegion = server.region.c_str();

            if (!m_serverBrowserTextFilter.PassFilter(pszHostName, &pszHostName[server.name.length()])
                && !m_serverBrowserTextFilter.PassFilter(pszHostMap, &pszHostMap[server.map.length()])
                && !m_serverBrowserTextFilter.PassFilter(pszPlaylist, &pszPlaylist[server.playlist.length()])
                && !m_serverBrowserTextFilter.PassFilter(pszRegion, &pszRegion[server.region.length()]))
                continue;

            if (m_hideLockedServers && server.hasPassword)
                continue;

            if (m_hideEmptyServers && server.numPlayers <= 0)
                continue;

            if (m_hideFullServers && server.maxPlayers > 0 && server.numPlayers >= server.maxPlayers)
                continue;

            BrowserRow_t row;
            row.server = &server;

            Browser_CollectMissingMods(server, s_browserMods, row.missingMods);

            if (m_hideIncompatibleServers && !row.missingMods.empty())
                continue;

            listedPlayers += server.numPlayers;
            rows.push_back(std::move(row));
        }

        listedServers = static_cast<int>(rows.size());

        if (ImGuiTableSortSpecs* const sortSpecs = ImGui::TableGetSortSpecs())
        {
            if (sortSpecs->SpecsCount > 0)
            {
                // Stable, so listings the sort cannot separate stay in the order
                // the masterserver returned them.
                std::stable_sort(rows.begin(), rows.end(),
                    [sortSpecs](const BrowserRow_t& lhs, const BrowserRow_t& rhs)
                    {
                        for (int n = 0; n < sortSpecs->SpecsCount; n++)
                        {
                            const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[n];
                            const int delta = Browser_CompareColumn(spec.ColumnUserID, *lhs.server, *rhs.server);

                            if (delta != 0)
                                return spec.SortDirection == ImGuiSortDirection_Ascending ? delta < 0 : delta > 0;
                        }

                        return false;
                    });
            }

            sortSpecs->SpecsDirty = false;
        }

        ImGuiListClipper clipper;
        clipper.Begin(listedServers);

        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
            {
                const BrowserRow_t& row = rows[i];
                const NetGameServer_t* const server = row.server;

                const bool isLocked = server->hasPassword;
                const bool isJoinable = row.missingMods.empty();
                const ImGuiTextFlags textFlags = ImGuiTextFlags_NoWidthForLargeClippedText;

                ImGui::TableNextRow();
                ImGui::PushID(i);

                // Lock icon column
                ImGui::TableNextColumn();
                if (isLocked && m_lockedIconShaderResource)
                {
                    ImGui::Image((ImTextureID)(intptr_t)m_lockedIconShaderResource, ImVec2(12.f, 12.f));
                }

                // Name column
                ImGui::TableNextColumn();
                const char* const pszHostName = server->name.c_str();
                const bool tintLocked = isLocked;
                if (tintLocked)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.85f, 0.00f, 1.00f));
                ImGui::TextEx(pszHostName, &pszHostName[server->name.length()], textFlags);
                if (tintLocked)
                    ImGui::PopStyleColor();

                // Everything the row cannot fit lives in the hover card.
                Browser_DrawServerTooltip(*server, row.missingMods);

                ImGui::TableNextColumn();

                if (server->region.empty())
                {
                    ImGui::TextDisabled("-");
                }
                else
                {
                    const char* const pszRegion = server->region.c_str();
                    ImGui::TextEx(pszRegion, &pszRegion[server->region.length()], textFlags);
                }

                ImGui::TableNextColumn();

                const char* const pszHostMap = server->map.c_str();
                ImGui::TextEx(pszHostMap, &pszHostMap[server->map.length()], textFlags);

                ImGui::TableNextColumn();

                const char* const pszPlaylist = server->playlist.c_str();
                ImGui::TextEx(pszPlaylist, &pszPlaylist[server->playlist.length()], textFlags);

                // Mods column
                ImGui::TableNextColumn();
                if (server->requiredMods.empty())
                {
                    ImGui::TextDisabled("-");
                }
                else
                {
                    std::string modsJoined;
                    modsJoined.reserve(64);

                    for (size_t m = 0; m < server->requiredMods.size(); ++m)
                    {
                        if (m) modsJoined.append(", ");
                        modsJoined.append(server->requiredMods[m]);
                    }

                    const char* const pszMods = modsJoined.c_str();

                    if (!isJoinable)
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.35f, 0.35f, 1.00f));
                    ImGui::TextEx(pszMods, &pszMods[modsJoined.length()], textFlags);
                    if (!isJoinable)
                        ImGui::PopStyleColor();
                }

                ImGui::TableNextColumn();

                const std::string playerNums = Format("%3d/%3d", server->numPlayers, server->maxPlayers);

                const char* const pszPlayerNums = playerNums.c_str();
                ImGui::TextEx(pszPlayerNums, &pszPlayerNums[playerNums.length()], textFlags);

                ImGui::TableNextColumn();
                ImGui::Text("%d", server->port);

                ImGui::TableNextColumn();

                ImGui::BeginDisabled(!isJoinable);

                if (ImGui::Button(isLocked ? "Enter password##row" : "Connect##row"))
                {
                    if (isLocked)
                    {
                        memset(s_passwordBuf, '\0', sizeof(s_passwordBuf));

                        m_passwordPromptName = server->name;
                        m_passwordPromptAddress = server->address;
                        m_passwordPromptNetKey = server->netKey;
                        m_passwordPromptPort = server->port;
                        m_openPasswordPrompt = true;
                    }
                    else
                    {
                        pendingConnect = true;
                        connectAddress = server->address;
                        connectNetKey = server->netKey;
                        connectPort = server->port;
                    }
                }

                ImGui::EndDisabled();

                if (!isJoinable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Missing required mods: %s", row.missingMods.c_str());

                ImGui::PopID();
            }
        }

        rows.clear();
        g_ServerListManager.m_Mutex.Unlock();

        ImGui::PopStyleVar(frameStyleVars);
        ImGui::EndTable();
    }
    else
    {
        ImGui::PopStyleVar(frameStyleVars);
    }

    if (pendingConnect)
    {
        Bridge_SetConnectPassword("");
        g_ServerListManager.ConnectToServer(connectAddress, connectPort, connectNetKey, "");
    }

    PasswordPromptModal();

    ImGui::Separator();

    if (m_lastRefreshTime <= 0.0)
    {
        ImGui::TextDisabled("Server list has not been refreshed yet.");
    }
    else
    {
        const char* const serverSuffix = listedServers == 1 ? "" : "s";
        const char* const playerSuffix = listedPlayers == 1 ? "" : "s";
        const double age = ImGui::GetTime() - m_lastRefreshTime;

        if (listedServers == totalServers)
            ImGui::TextDisabled("%d server%s, %d player%s -- refreshed %.0fs ago",
                listedServers, serverSuffix, listedPlayers, playerSuffix, age);
        else
            ImGui::TextDisabled("%d of %d server%s, %d player%s -- refreshed %.0fs ago",
                listedServers, totalServers, serverSuffix, listedPlayers, playerSuffix, age);
    }

    const ImVec2 regionAvail = ImGui::GetContentRegionAvail();
    const ImGuiStyle& style = ImGui::GetStyle();

    // 4 elements means 3 spacings between items, this has to be subtracted
    // from the remaining available region to get correct results on all
    // window padding values!!!
    const float itemWidth = (regionAvail.x - (3 * style.ItemSpacing.x)) / 4;

    ImGui::PushItemWidth(itemWidth);
    {
        ImGui::InputTextWithHint("##ServerBrowser_DrawBrowserPanel_ServerAddress", "Server address and port", m_serverAddressTextBuf, sizeof(m_serverAddressTextBuf));

        ImGui::SameLine();
        ImGui::InputTextWithHint("##ServerBrowser_DrawBrowserPanel_ServerKey", "Encryption key", m_serverNetKeyTextBuf, sizeof(m_serverNetKeyTextBuf));

        ImGui::SameLine();
        ImGui::InputTextWithHint("##ServerBrowser_DrawBrowserPanel_ServerPassword", "Server Password", m_serverPasswordTextBuf, sizeof(m_serverPasswordTextBuf), ImGuiInputTextFlags_Password);

        ImGui::SameLine();
        if (ImGui::Button("Connect##ServerBrowser_DrawBrowserPanel_ServerConnect", ImVec2(itemWidth, ImGui::GetFrameHeight())))
        {
            if (m_serverAddressTextBuf[0])
            {
                g_ServerListManager.ConnectToServer(m_serverAddressTextBuf, m_serverNetKeyTextBuf, m_serverPasswordTextBuf);
            }
        }

        ImGui::SameLine();

        // NOTE: -9 to prevent the last button from clipping/colliding with the
        // window drag handle! -9 makes the distance between the handle and the
        // last button equal as that of the developer console.
        if (ImGui::Button("Private servers##ServerBrowser_DrawBrowserPanel", ImVec2(itemWidth - 9, ImGui::GetFrameHeight())))
        {
            ImGui::OpenPopup("Private Server##ServerBrowser_HiddenServersModal");
        }

        HiddenServersModal();
    }

    ImGui::PopItemWidth();
}

//-----------------------------------------------------------------------------
// Purpose: draws the password prompt for a locked listing
//-----------------------------------------------------------------------------
void CBrowser::PasswordPromptModal(void)
{
    const char* const popupName = "Enter Password##ServerBrowser_PasswordPrompt";

    if (m_openPasswordPrompt)
    {
        ImGui::OpenPopup(popupName);
        m_openPasswordPrompt = false;
    }

    if (!ImGui::BeginPopupModal(popupName, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::Text("'%s' requires a password.", m_passwordPromptName.c_str());
    ImGui::Spacing();

    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();

    const bool submitted = ImGui::InputText("Password##ServerBrowser_PasswordPrompt", s_passwordBuf, sizeof(s_passwordBuf),
        ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);

    ImGui::Spacing();

    if (ImGui::Button("Connect", ImVec2(120, 0)) || submitted)
    {
        g_ServerListManager.ConnectToServer(m_passwordPromptAddress, m_passwordPromptPort, m_passwordPromptNetKey, s_passwordBuf);

        memset(s_passwordBuf, '\0', sizeof(s_passwordBuf));
        ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine();

    if (ImGui::Button("Cancel", ImVec2(120, 0)))
    {
        memset(s_passwordBuf, '\0', sizeof(s_passwordBuf));
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

//-----------------------------------------------------------------------------
// Purpose: refreshes the server browser list with available servers
//-----------------------------------------------------------------------------
void CBrowser::RefreshServerList(void)
{
    // Each click used to spawn its own detached request thread; one in flight at
    // a time is enough, and it keeps the auto refresh from stacking up behind a
    // masterserver that is timing out.
    if (m_refreshInFlight.load(std::memory_order_acquire))
        return;

    Msg(eDLL_T::CLIENT, "Refreshing server list with matchmaking host '%s'\n", spire_matchmaking_hostname.GetString());

    m_refreshInFlight.store(true, std::memory_order_release);
    m_refreshComplete.store(false, std::memory_order_release);
    m_lastRefreshTime = ImGui::GetTime();

    // The in-flight flag keeps this to one writer, so the message needs no lock
    // of its own; the release/acquire pair on m_refreshComplete publishes it.
    std::thread request([this]
        {
            std::string listMessage;
            size_t numServers = 0;

            g_ServerListManager.RefreshServerList(listMessage, numServers);

            m_pendingListMessage = listMessage;
            m_refreshComplete.store(true, std::memory_order_release);
        }
    );

    request.detach();
}

//-----------------------------------------------------------------------------
// Purpose: draws the hidden private server modal
//-----------------------------------------------------------------------------
void CBrowser::HiddenServersModal(void)
{
    float modalWindowHeight; // Set the padding accordingly for each theme.
    switch (m_surfaceStyle)
    {
    case ImGuiStyle_t::LEGACY:
        modalWindowHeight = 207.f;
        break;
    case ImGuiStyle_t::MODERN:
        modalWindowHeight = 214.f;
        break;
    default:
        modalWindowHeight = 206.f;
        break;
    }

    int modalStyleVars = 0;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(408.f, modalWindowHeight));    modalStyleVars++;

    bool isModalStillOpen = true;
    if (ImGui::BeginPopupModal("Private Server##ServerBrowser_HiddenServersModal", &isModalStillOpen, ImGuiWindowFlags_NoResize))
    {
        ImGui::SetWindowSize(ImVec2(408.f, modalWindowHeight), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.00f, 0.00f, 0.00f, 0.00f)); // Override the style color for child bg.

        ImGui::BeginChild("##ServerBrowser_HiddenServersModal_IconParent", ImVec2(float(m_lockedIconDataResource.m_nWidth), float(m_lockedIconDataResource.m_nHeight)));
        ImGui::Image((ImTextureID)(intptr_t)m_lockedIconShaderResource, ImVec2(float(m_lockedIconDataResource.m_nWidth), float(m_lockedIconDataResource.m_nHeight))); // Display texture.
        ImGui::EndChild();

        ImGui::PopStyleColor(); // Pop the override for the child bg.

        ImGui::SameLine();
        ImGui::Text("Enter token to connect");

        const ImVec2 contentRegionMax = ImGui::GetContentRegionAvail();
        ImGui::PushItemWidth(contentRegionMax.x); // Override item width.

        const bool hitEnter = ImGui::InputTextWithHint("##ServerBrowser_HiddenServersModal_TokenInput", "Token (required)", 
            m_serverTokenTextBuf, sizeof(m_serverTokenTextBuf), ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::PopItemWidth();

        if (m_reclaimFocusOnTokenField)
        {
            ImGui::SetKeyboardFocusHere(-1); // -1 means previous widget.
            m_reclaimFocusOnTokenField = false;
        }

        ImGui::Dummy(ImVec2(contentRegionMax.x, 19.f)); // Place a dummy, basically making space inserting a blank element.

        ImGui::TextColored(m_hiddenServerMessageColor, "%s", m_hiddenServerRequestMessage.c_str());
        ImGui::Separator();

        if (ImGui::Button("Connect##ServerBrowser_HiddenServersModal", ImVec2(contentRegionMax.x, 24)) || hitEnter)
        {
            m_hiddenServerRequestMessage.clear();
            m_reclaimFocusOnTokenField = true;

            if (m_serverTokenTextBuf[0])
            {
                NetGameServer_t server;
                const bool result = g_Spire.GetServerByToken(server, m_hiddenServerRequestMessage, m_serverTokenTextBuf); // Send token connect request.

                if (result && !server.name.empty())
                {
                    std::string missing;
                    if (!Browser_HasRequiredMods(server, missing))
                    {
                        m_hiddenServerRequestMessage = Format("Missing required mods: %s", missing.c_str());
                        m_hiddenServerMessageColor = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
                    }
                    else
                    {
                        g_ServerListManager.ConnectToServer(server.address, server.port, server.netKey, ""); // Connect to the server
                        m_hiddenServerRequestMessage = Format("Found server: %s", server.name.c_str());
                        m_hiddenServerMessageColor = ImVec4(0.00f, 1.00f, 0.00f, 1.00f);
                        ImGui::CloseCurrentPopup();
                    }
                }
                else
                {
                    if (m_hiddenServerRequestMessage.empty())
                    {
                        m_hiddenServerRequestMessage = "Unknown error.";
                    }
                    else // Display error message.
                    {
                        m_hiddenServerRequestMessage = Format("Error: %s", m_hiddenServerRequestMessage.c_str());
                    }
                    m_hiddenServerMessageColor = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
                }
            }
            else
            {
                m_hiddenServerRequestMessage = "Token is required.";
                m_hiddenServerMessageColor = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
            }
        }

        if (ImGui::Button("Close##ServerBrowser_HiddenServersModal", ImVec2(contentRegionMax.x, 24)))
        {
            m_hiddenServerRequestMessage.clear();
            m_reclaimFocusOnTokenField = true;

            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
        ImGui::PopStyleVar(modalStyleVars);
    }
    else if (!isModalStillOpen)
    {
        m_hiddenServerRequestMessage.clear();
        m_reclaimFocusOnTokenField = true;

        ImGui::PopStyleVar(modalStyleVars);
    }
    else
    {
        ImGui::PopStyleVar(modalStyleVars);
    }
}

void CBrowser::HandleInvalidFields(const bool offline)
{
    if (!offline && m_serverName.empty())
    {
        m_hostMessage = "Server name is required.";
        m_hostMessageColor = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
    }
    else if (m_levelName.empty())
    {
        m_hostMessage = "Level name is required.";
        m_hostMessageColor = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
    }
    else if (m_modeName.empty())
    {
        m_hostMessage = "Mode name is required.";
        m_hostMessageColor = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
    }
}


//-----------------------------------------------------------------------------
// Purpose: draws the host section
//-----------------------------------------------------------------------------
void CBrowser::DrawHostPanel(void)
{
}

//-----------------------------------------------------------------------------
// Purpose: updates the host status
//-----------------------------------------------------------------------------
void CBrowser::UpdateHostingStatus(void)
{
}

//-----------------------------------------------------------------------------
// Purpose: sends the hosting POST request to the comp server and installs the
// host data on the server browser
// Input: &gameServer - 
//-----------------------------------------------------------------------------
void CBrowser::SendHostingPostRequest(NetGameServer_t& gameServer)
{
}

//-----------------------------------------------------------------------------
// Purpose: installs the host data on the server browser
// Input: postFailed - 
// *hostMessage - 
// *hostToken - 
// &hostIp - 
//-----------------------------------------------------------------------------
void CBrowser::InstallHostingDetails(const bool postFailed, const string& hostMessage, const string& hostToken, const string& hostIp)
{
}

//-----------------------------------------------------------------------------
// Purpose: processes submitted commands
// Input: *pszCommand - 
//-----------------------------------------------------------------------------
void CBrowser::ProcessCommand(const char* pszCommand) const
{
    Cbuf_AddText(Cbuf_GetCurrentPlayer(), pszCommand, cmd_source_t::kCommandSrcCode);
}

static bool Browser_IsSafeIdent(const char* const psz)
{
	if (!psz || !psz[0])
		return false;

	const size_t len = V_strlen(psz);
	if (len < 3 || len > 63)
		return false;

	for (size_t i = 0; i < len; ++i)
	{
		const char c = psz[i];
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
			continue;
		return false;
	}

	return true;
}

static void Browser_AddUniqueMap(CUtlVector<CUtlString>& out, const char* psz)
{
	if (!psz || !psz[0])
		return;

	FOR_EACH_VEC(out, i)
	{
		if (out[i].IsEqual_CaseInsensitive(psz))
			return;
	}

	out.AddToTail(psz);
}

static void Browser_CollectMapsFromPlaylistKV(KeyValues* pPlaylists, KeyValues* pPl,
	CUtlVector<CUtlString>& out, int depth)
{
	if (!pPlaylists || !pPl || depth > 8)
		return;

	const char* const inherit = pPl->GetString("inherit", "");
	if (inherit && inherit[0])
		Browser_CollectMapsFromPlaylistKV(pPlaylists, pPlaylists->FindKey(inherit), out, depth + 1);

	KeyValues* const pGamemodes = pPl->FindKey("gamemodes");
	if (!pGamemodes)
		return;

	for (KeyValues* pMode = pGamemodes->GetFirstTrueSubKey(); pMode; pMode = pMode->GetNextTrueSubKey())
	{
		KeyValues* const pMaps = pMode->FindKey("maps");
		if (!pMaps)
			continue;

		for (KeyValues* pMap = pMaps->GetFirstValue(); pMap; pMap = pMap->GetNextValue())
			Browser_AddUniqueMap(out, pMap->GetName());
	}
}

static void Browser_CollectPlaylistMaps(const char* pszPlaylist, CUtlVector<CUtlString>& out)
{
	out.RemoveAll();

	KeyValues* const pRoot = Playlists_GetRootKV();
	if (!pRoot || !pszPlaylist || !pszPlaylist[0])
		return;

	KeyValues* const pPlaylists = pRoot->FindKey("Playlists");
	if (!pPlaylists)
		return;

	Browser_CollectMapsFromPlaylistKV(pPlaylists, pPlaylists->FindKey(pszPlaylist), out, 0);
}

static void Browser_CollectOwnPlaylistMaps(KeyValues* pPl, CUtlVector<CUtlString>& out)
{
	out.RemoveAll();
	if (!pPl)
		return;

	KeyValues* const pGamemodes = pPl->FindKey("gamemodes");
	if (!pGamemodes)
		return;

	for (KeyValues* pMode = pGamemodes->GetFirstTrueSubKey(); pMode; pMode = pMode->GetNextTrueSubKey())
	{
		KeyValues* const pMaps = pMode->FindKey("maps");
		if (!pMaps)
			continue;

		for (KeyValues* pMap = pMaps->GetFirstValue(); pMap; pMap = pMap->GetNextValue())
			Browser_AddUniqueMap(out, pMap->GetName());
	}
}

static void Browser_CollectAllPlaylistMaps(CUtlVector<CUtlString>& out)
{
	KeyValues* const pRoot = Playlists_GetRootKV();
	if (!pRoot)
		return;

	KeyValues* const pPlaylists = pRoot->FindKey("Playlists");
	if (!pPlaylists)
		return;

	for (KeyValues* pPl = pPlaylists->GetFirstTrueSubKey(); pPl; pPl = pPl->GetNextTrueSubKey())
		Browser_CollectMapsFromPlaylistKV(pPlaylists, pPl, out, 0);
}

static const char* Browser_PlaylistLabel(const char* pszName)
{
	if (!pszName || !pszName[0])
		return "";

	KeyValues* const pRoot = Playlists_GetRootKV();
	KeyValues* const pPlaylists = pRoot ? pRoot->FindKey("Playlists") : nullptr;
	KeyValues* const pPl = pPlaylists ? pPlaylists->FindKey(pszName) : nullptr;
	KeyValues* const pVars = pPl ? pPl->FindKey("vars") : nullptr;
	const char* const title = pVars ? pVars->GetString("r5f_mode_title", "") : "";
	if (title && title[0] && title[0] != '#')
		return title;

	const char* const pretty = pVars ? pVars->GetString("name", "") : "";
	if (pretty && pretty[0] && pretty[0] != '#')
		return pretty;

	return pszName;
}

static bool Browser_PassFilter(const char* pszHay, const char* pszNeedle)
{
	if (!pszNeedle || !pszNeedle[0])
		return true;
	return pszHay && V_stristr(pszHay, pszNeedle);
}

static void Browser_EnsureLocalCatalog(void)
{
	Playlists_LoadOverlayCatalog();
}

static bool Browser_NameInList(const CUtlVector<CUtlString>& list, const char* psz)
{
	if (!psz || !psz[0])
		return false;

	FOR_EACH_VEC(list, i)
	{
		if (list[i].IsEqual_CaseInsensitive(psz))
			return true;
	}

	return false;
}

static void Browser_CurateMaps(const CUtlVector<CUtlString>& declared, CUtlVector<CUtlString>& out)
{
	out.RemoveAll();

	if (g_vecOverlayMaps.Count() > 0)
	{
		FOR_EACH_VEC(g_vecOverlayMaps, i)
		{
			if (Browser_NameInList(declared, g_vecOverlayMaps[i].String()))
				Browser_AddUniqueMap(out, g_vecOverlayMaps[i].String());
		}
	}

	if (out.IsEmpty())
	{
		FOR_EACH_VEC(declared, i)
			Browser_AddUniqueMap(out, declared[i].String());
	}
}

static const char* Browser_ModeFamilyId(KeyValues* pVars, const char* pszId)
{
	const char* const pszFamily = pVars ? pVars->GetString("r5f_mode_family", "") : "";
	if (pszFamily && pszFamily[0])
		return pszFamily;
	return pszId ? pszId : "";
}

static void Browser_DropPinnedMaps(CUtlVector<CUtlString>& stems, const char* pszPlaylist)
{
	CUtlVector<CUtlString> familyName;
	CUtlVector<int> familyCount;
	CUtlVector<CUtlString> reserved;
	KeyValues* const pRoot = Playlists_GetRootKV();
	KeyValues* const pPlaylists = pRoot ? pRoot->FindKey("Playlists") : nullptr;

	FOR_EACH_VEC(g_vecAllPlaylists, i)
	{
		const char* const pszId = g_vecAllPlaylists[i].String();
		KeyValues* const pPl = pPlaylists ? pPlaylists->FindKey(pszId) : nullptr;
		KeyValues* const pVars = pPl ? pPl->FindKey("vars") : nullptr;
		if (!pVars || pVars->GetInt("r5f_mode", 0) != 1)
			continue;

		const char* const pszFamily = Browser_ModeFamilyId(pVars, pszId);
		int idx = -1;
		FOR_EACH_VEC(familyName, f)
		{
			if (familyName[f].IsEqual_CaseInsensitive(pszFamily))
			{
				idx = f;
				break;
			}
		}
		if (idx < 0)
		{
			idx = familyName.AddToTail(pszFamily);
			familyCount.AddToTail(0);
		}
		familyCount[idx]++;
	}

	FOR_EACH_VEC(g_vecAllPlaylists, i)
	{
		const char* const pszId = g_vecAllPlaylists[i].String();
		if (pszPlaylist && V_stricmp(pszId, pszPlaylist) == 0)
			continue;

		KeyValues* const pPl = pPlaylists ? pPlaylists->FindKey(pszId) : nullptr;
		KeyValues* const pVars = pPl ? pPl->FindKey("vars") : nullptr;
		if (!pVars || pVars->GetInt("r5f_mode", 0) != 1)
			continue;

		const char* const pszPin = pVars->GetString("r5f_mode_map", "");
		if (!pszPin || !pszPin[0])
			continue;

		const char* const pszFamily = Browser_ModeFamilyId(pVars, pszId);
		int nFamily = 0;
		FOR_EACH_VEC(familyName, f)
		{
			if (familyName[f].IsEqual_CaseInsensitive(pszFamily))
			{
				nFamily = familyCount[f];
				break;
			}
		}

		// A TDM/Control pin is still just a map. Only a family of one
		// (Firing Range) owns its arena as a separate mode button.
		if (nFamily != 1)
			continue;

		Browser_AddUniqueMap(reserved, pszPin);
	}

	if (!pszPlaylist || V_stricmp(pszPlaylist, "lobby") != 0)
		Browser_AddUniqueMap(reserved, "mp_lobby");

	if (reserved.IsEmpty())
		return;

	CUtlVector<CUtlString> kept;
	FOR_EACH_VEC(stems, i)
	{
		if (!Browser_NameInList(reserved, stems[i].String()))
			kept.AddToTail(stems[i]);
	}

	if (kept.Count() > 0)
	{
		stems.RemoveAll();
		FOR_EACH_VEC(kept, i)
			stems.AddToTail(kept[i]);
	}
}

static void Browser_BuildMapChoices(const char* pszPlaylist, CUtlVector<CUtlString>& out)
{
	out.RemoveAll();

	KeyValues* const pRoot = Playlists_GetRootKV();
	KeyValues* const pPlaylists = pRoot ? pRoot->FindKey("Playlists") : nullptr;
	KeyValues* const pPl = (pPlaylists && pszPlaylist && pszPlaylist[0])
		? pPlaylists->FindKey(pszPlaylist) : nullptr;
	KeyValues* const pVars = pPl ? pPl->FindKey("vars") : nullptr;
	const char* const pszPinned = pVars ? pVars->GetString("r5f_mode_map", "") : "";

	if (pszPinned && pszPinned[0])
	{
		Browser_AddUniqueMap(out, pszPinned);
		return;
	}

	CUtlVector<CUtlString> own;
	Browser_CollectOwnPlaylistMaps(pPl, own);

	const bool overlayOnly = pVars && pVars->GetInt("r5f_overlay", 0) == 1;

	if (own.Count() >= 2)
		Browser_CurateMaps(own, out);
	else if (overlayOnly && own.Count() == 1)
		Browser_CurateMaps(own, out);
	else if (g_vecOverlayMaps.Count() > 0)
	{
		FOR_EACH_VEC(g_vecOverlayMaps, i)
			Browser_AddUniqueMap(out, g_vecOverlayMaps[i].String());
	}
	else
		Browser_CollectPlaylistMaps(pszPlaylist, out);

	if (out.IsEmpty())
	{
		AUTO_LOCK(g_InstalledMapsMutex);
		FOR_EACH_VEC(g_InstalledMaps, i)
			Browser_AddUniqueMap(out, g_InstalledMaps[i].String());
	}

	if (out.IsEmpty())
		Browser_CollectAllPlaylistMaps(out);

	Browser_DropPinnedMaps(out, pszPlaylist);
}

//-----------------------------------------------------------------------------
// Purpose: hosted-dedi controls. This client is not a listen server.
//-----------------------------------------------------------------------------
void CBrowser::DrawManageLocalPanel(void)
{
	Browser_EnsureLocalCatalog();

	const char* const pszLiveMap = Bridge_GetLevelBaseName();
	const bool inMatch = pszLiveMap && pszLiveMap[0];
	const bool rconReady = RCON_LauncherClient_Ready();
	const bool rconActive = RCON_LauncherClient_Active();

	CUtlVector<CUtlString> playlistChoices;
	FOR_EACH_VEC(g_vecAllPlaylists, i)
		playlistChoices.AddToTail(g_vecAllPlaylists[i]);

	const char* const pszLive = Playlists_GetCurrentName();
	if (pszLive && pszLive[0] && !Browser_NameInList(playlistChoices, pszLive))
		playlistChoices.AddToTail(pszLive);

	if (playlistChoices.Count() > 0 && !Browser_NameInList(playlistChoices, m_modeName.c_str()))
	{
		if (pszLive && Browser_NameInList(playlistChoices, pszLive))
			m_modeName = pszLive;
		else
			m_modeName = playlistChoices[0].String();
	}

	CUtlVector<CUtlString> mapChoices;
	Browser_BuildMapChoices(m_modeName.c_str(), mapChoices);

	if (mapChoices.Count() > 0 && !Browser_NameInList(mapChoices, m_levelName.c_str()))
		m_levelName = mapChoices[0].String();

	if (inMatch)
	{
		const char* mode = Playlists_GetCurrentName();
		if (!mode || !mode[0])
			mode = "unknown";
		ImGui::Text("Now: %s  /  %s", pszLiveMap, mode);
	}
	else
	{
		ImGui::TextDisabled("Not connected.");
	}

	ImGui::SameLine();
	if (rconReady)
		ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.40f, 1.00f), "  host console ready");
	else if (rconActive)
		ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.20f, 1.00f), "  host console connecting...");
	else
		ImGui::TextDisabled("  host console offline");

	if (!m_hostMessage.empty())
		ImGui::TextColored(m_hostMessageColor, "%s", m_hostMessage.c_str());

	ImGui::Separator();

	static char s_playlistFilter[64] = {};
	static char s_mapFilter[64] = {};

	const float btnRow = ImGui::GetFrameHeightWithSpacing() + 4.f;
	const float listHeight = ImMax(160.f, ImGui::GetContentRegionAvail().y - btnRow);
	const float colWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

	ImGui::BeginChild("##ml_playlists", ImVec2(colWidth, listHeight), ImGuiChildFlags_Border);
	ImGui::TextUnformatted("Playlist");
	ImGui::SetNextItemWidth(-1.f);
	ImGui::InputTextWithHint("##ml_plfilter", "Filter...", s_playlistFilter, sizeof(s_playlistFilter));

	if (ImGui::BeginListBox("##ml_pllist", ImVec2(-1.f, -1.f)))
	{
		FOR_EACH_VEC(playlistChoices, i)
		{
			const char* const pszName = playlistChoices[i].String();
			const char* const pszLabel = Browser_PlaylistLabel(pszName);

			if (!Browser_PassFilter(pszName, s_playlistFilter)
				&& !Browser_PassFilter(pszLabel, s_playlistFilter))
				continue;

			char row[192];
			if (pszLabel[0] && V_stricmp(pszLabel, pszName) != 0)
				V_snprintf(row, sizeof(row), "%s  (%s)", pszLabel, pszName);
			else
				V_strncpy(row, pszName, sizeof(row));

			ImGui::PushID(i);
			if (ImGui::Selectable(row, playlistChoices[i].IsEqual_CaseInsensitive(m_modeName.c_str())))
				m_modeName = pszName;
			ImGui::PopID();
		}

		if (playlistChoices.IsEmpty())
			ImGui::TextDisabled("No playlists loaded.");

		ImGui::EndListBox();
	}
	ImGui::EndChild();

	ImGui::SameLine();

	ImGui::BeginChild("##ml_maps", ImVec2(0.f, listHeight), ImGuiChildFlags_Border);
	ImGui::TextUnformatted("Map");
	ImGui::SetNextItemWidth(-1.f);
	ImGui::InputTextWithHint("##ml_mapfilter", "Filter...", s_mapFilter, sizeof(s_mapFilter));

	if (ImGui::BeginListBox("##ml_maplist", ImVec2(-1.f, -1.f)))
	{
		FOR_EACH_VEC(mapChoices, i)
		{
			const char* const pszMap = mapChoices[i].String();
			if (!Browser_PassFilter(pszMap, s_mapFilter))
				continue;

			ImGui::PushID(i);
			if (ImGui::Selectable(pszMap, mapChoices[i].IsEqual_CaseInsensitive(m_levelName.c_str())))
				m_levelName = pszMap;
			ImGui::PopID();
		}

		if (mapChoices.IsEmpty())
			ImGui::TextDisabled("No maps for this playlist.");

		ImGui::EndListBox();
	}
	ImGui::EndChild();

	const float spacing = ImGui::GetStyle().ItemSpacing.x;
	const float btnWidth = (ImGui::GetContentRegionAvail().x - spacing * 2.f) / 3.f;

	ImGui::BeginDisabled(!rconReady);
	if (ImGui::Button("Change level##ManageLocal", ImVec2(btnWidth, 0.f)))
	{
		if (!Browser_IsSafeIdent(m_levelName.c_str()) || !Browser_IsSafeIdent(m_modeName.c_str()))
		{
			m_hostMessage = "Map and playlist must be [a-z0-9_] (3-63).";
			m_hostMessageColor = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
		}
		else
		{
			char cmd[192];
			V_snprintf(cmd, sizeof(cmd), "bridge_setmode %s %s",
				m_modeName.c_str(), m_levelName.c_str());

			if (RCON_LauncherClient_QueueExec(cmd))
			{
				m_hostMessage = "Change level sent to host.";
				m_hostMessageColor = ImVec4(0.35f, 0.85f, 0.40f, 1.00f);
				Msg(eDLL_T::CLIENT, "[MANAGE-LOCAL] %s\n", cmd);
			}
			else
			{
				m_hostMessage = "Failed to queue change level.";
				m_hostMessageColor = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
			}
		}
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !rconReady)
		ImGui::SetTooltip("Requires a Play Local host console.");

	ImGui::SameLine();
	if (ImGui::Button("Reload##ManageLocal", ImVec2(btnWidth, 0.f)))
	{
		if (RCON_LauncherClient_QueueExec("reload"))
		{
			m_hostMessage = "Reload sent to host.";
			m_hostMessageColor = ImVec4(0.35f, 0.85f, 0.40f, 1.00f);
			Msg(eDLL_T::CLIENT, "[MANAGE-LOCAL] reload\n");
		}
		else
		{
			m_hostMessage = "Failed to queue reload.";
			m_hostMessageColor = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
		}
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !rconReady)
		ImGui::SetTooltip("Requires a Play Local host console.");

	ImGui::SameLine();
	if (ImGui::Button("Reparse weapons##ManageLocal", ImVec2(btnWidth, 0.f)))
	{
		const bool queued = RCON_LauncherClient_QueueExec("weapon_reparse");
		if (WeaponParse_LoadClientData)
			WeaponParse_LoadClientData(true, true);

		if (queued)
		{
			m_hostMessage = "Weapon reparse sent to host.";
			m_hostMessageColor = ImVec4(0.35f, 0.85f, 0.40f, 1.00f);
			Msg(eDLL_T::CLIENT, "[MANAGE-LOCAL] weapon_reparse\n");
		}
		else
		{
			m_hostMessage = "Failed to queue weapon reparse.";
			m_hostMessageColor = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
		}
	}
	ImGui::EndDisabled();
}

//-----------------------------------------------------------------------------
// Purpose: toggles the server browser
//-----------------------------------------------------------------------------
void CBrowser::ToggleBrowser_f()
{
	// Same serial-gate as toggleconsole -- WndProc bind + ConCommand must
	// not XOR cancel each other on one keypress.
	extern volatile LONG g_imguiWndProcToggleSerial;
	static LONG s_seenWndProcSerial = 0;
	const LONG wndSerial = InterlockedCompareExchange(&g_imguiWndProcToggleSerial, 0, 0);
	if (wndSerial != s_seenWndProcSerial)
	{
		s_seenWndProcSerial = wndSerial;
		return;
	}

	g_Console.ToggleTab(CConsole::kTabBrowser);
}

void CBrowser::ToggleLocal_f()
{
	extern volatile LONG g_imguiWndProcToggleSerial;
	static LONG s_seenWndProcSerial = 0;
	const LONG wndSerial = InterlockedCompareExchange(&g_imguiWndProcToggleSerial, 0, 0);
	if (wndSerial != s_seenWndProcSerial)
	{
		s_seenWndProcSerial = wndSerial;
		return;
	}

	g_Console.ToggleTab(CConsole::kTabManageLocal);
}

CBrowser g_Browser;
