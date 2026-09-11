/*-----------------------------------------------------------------------------
 * _imgui_utility.cpp
 *-----------------------------------------------------------------------------*/

#include "core/stdafx.h"
#include "tier0/commandline.h"
#include "tier0/memstd.h"
#include "tier1/keyvalues.h"
#include "filesystem/filesystem.h"
#include "thirdparty/imgui/misc/imgui_utility.h"

#define GAME_CONSOLE_KEY "Console"
#define GAME_BROWSER_KEY "Browser"
#define GAME_LOCAL_KEY "Local"

static void ImguiConfig_ClampedKeyInit(KeyValues* const keyValues, ImGuiConfig::BindPair_s& bindPair, const int fallBackPrimary, const int fallBackSecondary)
{
    bindPair.m_nBind0 = keyValues->GetInt("$primaryKey", fallBackPrimary);

    if (!ImGui::IsNamedKey((ImGuiKey)bindPair.m_nBind0))
        bindPair.m_nBind0 = fallBackPrimary;

    bindPair.m_nBind1 = keyValues->GetInt("$secondaryKey", fallBackSecondary);

    if (!ImGui::IsNamedKey((ImGuiKey)bindPair.m_nBind1))
        bindPair.m_nBind1 = fallBackSecondary;
}

void ImGuiConfig::Load()
{
    const string svPath = Format(SDK_USER_CFG_PATH"%s", IMGUI_BIND_FILE);
    Msg(eDLL_T::MS, "Loading ImGui config file '%s'\n", svPath.c_str());

    FileSystem()->CreateDirHierarchy(SDK_USER_CFG_PATH, "PLATFORM"); // Create directory, so ImGui can load/save 'layout.ini'.

    KeyValues* pKeyMapKV = FileSystem()->LoadKeyValues(IFileSystem::TYPE_COMMON, svPath.c_str(), "PLATFORM");
    if (!pKeyMapKV)
    {
        return;
    }

    KeyValues* pConsoleKV = pKeyMapKV->FindKey(GAME_CONSOLE_KEY);
    if (pConsoleKV)
    {
        ImguiConfig_ClampedKeyInit(pConsoleKV, m_ConsoleConfig, ImGuiKey_GraveAccent, ImGuiKey_F10);
    }

    KeyValues* pBrowserKV = pKeyMapKV->FindKey(GAME_BROWSER_KEY);
    if (pBrowserKV)
    {
        ImguiConfig_ClampedKeyInit(pBrowserKV, m_BrowserConfig, ImGuiKey_Insert, ImGuiKey_F11);
    }

    KeyValues* pLocalKV = pKeyMapKV->FindKey(GAME_LOCAL_KEY);
    if (pLocalKV)
    {
        ImguiConfig_ClampedKeyInit(pLocalKV, m_LocalConfig, ImGuiKey_F12, ImGuiKey_F12);
    }

    pKeyMapKV->DeleteThis();
}

void ImGuiConfig::Save() const
{
    const string svPath = Format(SDK_USER_CFG_PATH"%s", IMGUI_BIND_FILE);
    Msg(eDLL_T::MS, "Saving ImGui config file '%s'\n", svPath.c_str());

    FileSystem()->CreateDirHierarchy(SDK_USER_CFG_PATH, "PLATFORM"); // Create directory, so ImGui can load/save 'layout.ini'.

    KeyValues kv("KeyMap");

    KeyValues* pConsoleKV = kv.FindKey(GAME_CONSOLE_KEY, true);
    pConsoleKV->SetInt("$primaryKey", m_ConsoleConfig.m_nBind0);
    pConsoleKV->SetInt("$secondaryKey", m_ConsoleConfig.m_nBind1);

    KeyValues* pBrowserKV = kv.FindKey(GAME_BROWSER_KEY, true);
    pBrowserKV->SetInt("$primaryKey", m_BrowserConfig.m_nBind0);
    pBrowserKV->SetInt("$secondaryKey", m_BrowserConfig.m_nBind1);

    KeyValues* pLocalKV = kv.FindKey(GAME_LOCAL_KEY, true);
    pLocalKV->SetInt("$primaryKey", m_LocalConfig.m_nBind0);
    pLocalKV->SetInt("$secondaryKey", m_LocalConfig.m_nBind1);

    CUtlBuffer uBuf(0ll, 0, CUtlBuffer::TEXT_BUFFER);

    kv.RecursiveSaveToFile(uBuf, 0);
    FileSystem()->WriteFile(svPath.c_str(), "PLATFORM", uBuf);
}

ImGuiStyle_t ImGuiConfig::InitStyle() const
{
    ImGuiStyle_t selected = ImGuiStyle_t::NONE;

    if (strcmp(CommandLine()->ParmValue("-imgui_theme", ""), "legacy") == 0)
    {
        selected = ImGuiStyle_t::LEGACY;
    }
    else if (strcmp(CommandLine()->ParmValue("-imgui_theme", ""), "modern") == 0)
    {
        selected = ImGuiStyle_t::MODERN;
    }
    else if (strcmp(CommandLine()->ParmValue("-imgui_theme", ""), "og") == 0)
    {
        selected = ImGuiStyle_t::OG;
    }
    else
    {
        selected = ImGuiStyle_t::DEFAULT;
    }

    ImGui_SetStyle(selected);
    return selected;
}

ImGuiConfig g_ImGuiConfig;
