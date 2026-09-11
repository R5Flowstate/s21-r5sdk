//=============================================================================//
//
// Purpose: In-game developer menu UI
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "engine/cmd.h"
#include "windows/id3dx.h"
#include "imgui/misc/imgui_utility.h"
#include "gameui/IDevMenu.h"

// DevMenu visibility convar (not bound to top bar anymore)
static ConVar ui_devmenu_enable("ui_devmenu_enable", "0", FCVAR_CLIENTDLL | FCVAR_RELEASE, "Show/hide the developer menu.");

// Dev menu commands reuse
extern ConCommand dev_menu_add; // declared in ITopBar.cpp
extern ConCommand dev_menu_remove;
extern ConCommand dev_menu_clear;

CDevMenu::CDevMenu()
{
	m_surfaceLabel = "Developer Menu";
}

bool CDevMenu::Init()
{
	SetStyleVar();
	return true;
}

void CDevMenu::Shutdown()
{
}

void CDevMenu::RunFrame()
{
	const bool enabled = ui_devmenu_enable.GetBool();

	if (!enabled)
	{
		// Cvar off but m_activated left true keeps IsSurfaceActive latched
		// (invisible modal: free multi-mon cursor + blocked game input).
		if (m_activated)
		{
			SetActive(false);
			ResetInput();
		}
		return;
	}

	if (!m_initialized)
	{
		Init();
		m_initialized = true;
	}

	// Rising-edge open: bind/cvar just enabled, or bind called SetActive.
	// Do not re-force true every frame -- that fights the window X (p_open).
	if (!m_activated)
		SetActive(true);

	DrawSurface();

	// User closed via X: keep cvar in sync so we do not re-activate above.
	if (!m_activated && ui_devmenu_enable.GetBool())
		ui_devmenu_enable.SetValue(false);
}

bool CDevMenu::DrawSurface()
{
	if (!ui_devmenu_enable.GetBool() || !m_activated)
		return false;

	int stylePushed = 0;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f); stylePushed++;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f)); stylePushed++;

	ImGui::SetNextWindowSize(ImVec2(700, 520), ImGuiCond_FirstUseEver);
	// p_open + close_callback so the window X clears m_activated and
	// re-enables game input / reclip (same pattern as console/browser).
	if (!ImGui::Begin("Developer Menu", &m_activated, ImGuiWindowFlags_NoCollapse, &ResetInput))
	{
		ImGui::End();
		ImGui::PopStyleVar(stylePushed);
		return false;
	}

	//ImGui::TextColored(ImVec4(1.00f, 0.85f, 0.25f, 1.00f), "Developer Menu");
	ImGui::Separator();

	if (ImGui::BeginTabBar("##DevMenuTabs"))
	{
		if (ImGui::BeginTabItem("Commands"))
		{
			ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 18.0f); stylePushed++;
			ImGui::BeginChild("##DevCmdsChild", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysUseWindowPadding);
			if (m_vecRootCommands.empty())
				ImGui::TextDisabled("No commands. Add one with: dev_menu_add \"path/name\" \"command\"");
			else
				DrawMenuRecursive(m_vecRootCommands);
			ImGui::EndChild();
			ImGui::PopStyleVar(); stylePushed--;
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Search"))
		{
			static char s_filter[128] = { 0 };
			ImGui::PushItemWidth(ImGui::GetFontSize() * 16);
			ImGui::InputTextWithHint("##devmenu_filter", "Search commands...", s_filter, IM_ARRAYSIZE(s_filter));
			ImGui::PopItemWidth();
			ImGui::Spacing();

			ImGui::BeginChild("##DevSearchChild", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysUseWindowPadding);
			if (ImGui::BeginTable("##DevSearchTable", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
			{
				ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.35f);
				ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch, 0.55f);
				ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 90.0f);
				ImGui::TableHeadersRow();

				auto toLower = [](const string& s)
				{
					string out(s);
					std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char)std::tolower(c); });
					return out;
				};
				const string needle = toLower(string(s_filter));

				std::function<void(const vector<PresetCommand_t>&)> drawSearch;
				drawSearch = [&](const vector<PresetCommand_t>& vec)
				{
					for (const auto& node : vec)
					{
						if (!node.m_vecChildren.empty())
						{
							drawSearch(node.m_vecChildren);
						}
						else if (!node.m_svCommand.empty())
						{
							if (needle.empty() || toLower(node.m_svLabel).find(needle) != string::npos || toLower(node.m_svCommand).find(needle) != string::npos)
							{
								ImGui::TableNextRow();
								ImGui::TableNextColumn();
								ImGui::TextUnformatted(node.m_svLabel.c_str());

								ImGui::TableNextColumn();
								ImGui::TextDisabled("%s", node.m_svCommand.c_str());

								ImGui::TableNextColumn();
								ImGui::PushID(&node);
								const float execWidth = 90.0f;
								if (ImGui::Button("Exec", ImVec2(execWidth, 0)))
								{
									Cbuf_AddText(Cbuf_GetCurrentPlayer(), node.m_svCommand.c_str(), cmd_source_t::kCommandSrcCode);
									Msg(eDLL_T::CLIENT, "(DevMenu) Executed command: %s\n", node.m_svCommand.c_str());
								}
								ImGui::PopID();

								if (!node.m_svTooltip.empty() && ImGui::IsItemHovered())
									ImGui::SetTooltip("%s", node.m_svTooltip.c_str());
							}
						}
					}
				};
				drawSearch(m_vecRootCommands);
				ImGui::EndTable();
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	ImGui::End();
	ImGui::PopStyleVar(stylePushed);
	return true;
}

void CDevMenu::DrawMenuRecursive(const vector<PresetCommand_t>& commands)
{
	for (const auto& command : commands)
	{
		if (!command.m_vecChildren.empty())
		{
			const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;
			if (ImGui::TreeNodeEx(command.m_svLabel.c_str(), flags))
			{
				DrawMenuRecursive(command.m_vecChildren);
				ImGui::TreePop();
			}
		}
		else
		{
			ImGui::PushID(&command);
			bool clicked = ImGui::Selectable(command.m_svLabel.c_str(), false, ImGuiSelectableFlags_SpanAvailWidth);
			if (clicked && !command.m_svCommand.empty())
			{
				Cbuf_AddText(Cbuf_GetCurrentPlayer(), command.m_svCommand.c_str(), cmd_source_t::kCommandSrcCode);
				Msg(eDLL_T::CLIENT, "(DevMenu) Executed command: %s\n", command.m_svCommand.c_str());
			}
			ImGui::PopID();
			if (ImGui::IsItemHovered() && !command.m_svTooltip.empty())
			{
				ImGui::SetTooltip("%s", command.m_svTooltip.c_str());
			}
		}
	}
}

void CDevMenu::AddPresetCommand(const char* const szPath, const char* const szCommand, const char* const szTooltip)
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

void CDevMenu::RemovePresetCommand(const char* const szPath)
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

void CDevMenu::ClearPresetCommands(void)
{
	m_vecRootCommands.clear();
}

CDevMenu g_DevMenu;

