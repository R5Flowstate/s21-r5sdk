//=============================================================================//
//
// Purpose: In-game top bar UI
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "engine/cmd.h"
#include "windows/id3dx.h"
#include "imgui/misc/imgui_utility.h"
#include "gameui/ITopBar.h"
#include "gameui/IDevMenu.h"
#include "IConsole.h"

// Dev menu commands (moved from console)
static ConCommand dev_menu_add("dev_menu_add", CTopBar::DevMenuAdd_f, "Adds a hierarchical preset command to the top bar.", FCVAR_DEVELOPMENTONLY);
static ConCommand dev_menu_remove("dev_menu_remove", CTopBar::DevMenuRemove_f, "Removes a preset command from the top bar by its path.", FCVAR_DEVELOPMENTONLY);
static ConCommand dev_menu_clear("dev_menu_clear", CTopBar::DevMenuClear_f, "Removes all preset commands from the top bar.", FCVAR_DEVELOPMENTONLY);

CTopBar::CTopBar() : m_unlockMouse(false)
{
	m_surfaceLabel = "Top Bar";
}

bool CTopBar::Init()
{
	SetStyleVar();
	return true;
}

void CTopBar::Shutdown()
{
}

void CTopBar::RunFrame()
{
	if (!m_initialized)
	{
		Init();
		m_initialized = true;
		m_activated = false;
	}
}

void CTopBar::RenderShowMainBar()
{
	Animate();
	DrawSurface();
}

bool CTopBar::DrawSurface()
{
	if (!g_Console.IsVisible())
		return false;

	// Draw a full-width menu bar docked at the top of the screen
	ImGuiViewport* const vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, 10));

	// Style adjustments: remove rounding; dim when mouse is locked
	int styleVarsPushed = 0;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f); styleVarsPushed++;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5.0f, 5.0f)); styleVarsPushed++;

	if (ImGui::BeginMainMenuBar())
	{
		DrawMenuBar();
		ImGui::EndMainMenuBar();
	}

	//ImGui::End;
	ImGui::PopStyleVar(styleVarsPushed);
	return true;
}

void CTopBar::DrawMenuBar()
{
	/*if (ImGui::BeginMenu("Game"))
	{
		if (ImGui::MenuItem("Toggle Cursor", nullptr, m_unlockMouse))
		{
			
		}
		ImGui::EndMenu;
	}

	if (ImGui::BeginMenu("View"))
	{
		bool enabled = ui_topbar_enable.GetBool;
		if (ImGui::MenuItem("Show Top Bar", nullptr, &enabled))
		{
			ui_topbar_enable.SetValue(enabled);
		}
		ImGui::EndMenu;
	}*/

	// Draw developer preset commands if any
	if (!m_vecRootCommands.empty())
	{
		DrawMenuRecursive(m_vecRootCommands);
	}
}

void CTopBar::DrawMenuRecursive(const vector<PresetCommand_t>& commands)
{
	for (const auto& command : commands)
	{
		if (!command.m_vecChildren.empty())
		{
			if (ImGui::BeginMenu(command.m_svLabel.c_str()))
			{
				DrawMenuRecursive(command.m_vecChildren);
				ImGui::EndMenu();
			}
		}
		else
		{
			if (ImGui::MenuItem(command.m_svLabel.c_str()))
			{
				Cbuf_AddText(Cbuf_GetCurrentPlayer(), command.m_svCommand.c_str(), cmd_source_t::kCommandSrcCode);
				Msg(eDLL_T::CLIENT, "(DevMenu) Executed command: %s\n", command.m_svCommand.c_str());
			}
			if (ImGui::IsItemHovered() && !command.m_svTooltip.empty())
			{
				ImGui::SetTooltip("%s", command.m_svTooltip.c_str());
			}
		}
	}
}

void CTopBar::AddPresetCommand(const char* const szPath, const char* const szCommand, const char* const szTooltip)
{
	std::stringstream ss(szPath);
	string segment;
	vector<string> pathParts;

	while (std::getline(ss, segment, '/'))
	{
		pathParts.push_back(segment);
	}
	if (pathParts.empty()) return;

	vector<PresetCommand_t>* currentLevel = &m_vecRootCommands;
	for (size_t i = 0; i < pathParts.size() - 1; ++i)
	{
		const string& part = pathParts[i];
		auto it = std::find_if(currentLevel->begin(), currentLevel->end(),
			[&part](const PresetCommand_t& cmd) { return cmd.m_svLabel == part; });
		if (it == currentLevel->end())
		{
			currentLevel->emplace_back(part);
			currentLevel = &currentLevel->back().m_vecChildren;
		}
		else
		{
			currentLevel = &it->m_vecChildren;
		}
	}

	const string& finalPart = pathParts.back();
	auto it = std::find_if(currentLevel->begin(), currentLevel->end(),
		[&finalPart](const PresetCommand_t& cmd) { return cmd.m_svLabel == finalPart; });
	if (it != currentLevel->end())
	{
		it->m_svCommand = szCommand;
		it->m_svTooltip = szTooltip;
		it->m_vecChildren.clear();
	}
	else
	{
		currentLevel->emplace_back(finalPart, szCommand, szTooltip);
	}
}

void CTopBar::RemovePresetCommand(const char* const szPath)
{
	std::stringstream ss(szPath);
	string segment;
	vector<string> pathParts;
	while (std::getline(ss, segment, '/'))
	{
		pathParts.push_back(segment);
	}
	if (pathParts.empty()) return;

	vector<PresetCommand_t>* parentLevel = &m_vecRootCommands;
	for (size_t i = 0; i < pathParts.size() - 1; ++i)
	{
		const string& part = pathParts[i];
		auto it = std::find_if(parentLevel->begin(), parentLevel->end(),
			[&part](const PresetCommand_t& cmd) { return cmd.m_svLabel == part; });
		if (it == parentLevel->end())
			return;
		parentLevel = &it->m_vecChildren;
	}

	const string& finalPart = pathParts.back();
	auto it = std::remove_if(parentLevel->begin(), parentLevel->end(),
		[&finalPart](const PresetCommand_t& cmd) { return cmd.m_svLabel == finalPart; });
	if (it != parentLevel->end())
		parentLevel->erase(it, parentLevel->end());
}

void CTopBar::ClearPresetCommands(void)
{
	m_vecRootCommands.clear();
}

void CTopBar::DevMenuAdd_f(const CCommand& args)
{
	if (args.ArgC() < 3)
	{
		Msg(eDLL_T::CLIENT, "Usage: dev_menu_add \"<path/to/item>\" \"<command>\" [\"<tooltip>\"]\n");
		return;
	}
	const char* path = args[1];
	const char* command = args[2];
	const char* tooltip = (args.ArgC() > 3) ? args[3] : "";
	g_TopBar.AddPresetCommand(path, command, tooltip);
	g_DevMenu.AddPresetCommand(path, command, tooltip);
}

void CTopBar::DevMenuRemove_f(const CCommand& args)
{
	if (args.ArgC() != 2)
	{
		Msg(eDLL_T::CLIENT, "Usage: dev_menu_remove \"<path/to/item>\"\n");
		return;
	}
	g_TopBar.RemovePresetCommand(args[1]);
	g_DevMenu.RemovePresetCommand(args[1]);
}

void CTopBar::DevMenuClear_f(const CCommand& args)
{
	g_TopBar.ClearPresetCommands();
	g_DevMenu.ClearPresetCommands();
}

CTopBar g_TopBar;



