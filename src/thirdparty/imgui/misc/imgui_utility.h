#pragma once
#include "imgui_style.h"

constexpr char IMGUI_BIND_FILE[] = "keymap.vdf";

class ImGuiConfig
{
public:
	ImGuiConfig()
	{
		m_ConsoleConfig.m_nBind0 = ImGuiKey_GraveAccent;
		m_ConsoleConfig.m_nBind1 = ImGuiKey_F10;

		m_BrowserConfig.m_nBind0 = ImGuiKey_Insert;
		m_BrowserConfig.m_nBind1 = ImGuiKey_F11;

		m_LocalConfig.m_nBind0 = ImGuiKey_F12;
		m_LocalConfig.m_nBind1 = ImGuiKey_F12;

		m_DevMenuConfig.m_nBind0 = ImGuiKey_F3;
		m_DevMenuConfig.m_nBind1 = ImGuiKey_Delete;
	}

	struct BindPair_s
	{
		int m_nBind0;
		int m_nBind1;
	};
	
	BindPair_s m_ConsoleConfig;
	BindPair_s m_BrowserConfig;
	BindPair_s m_LocalConfig;
	BindPair_s m_DevMenuConfig;

	void Load();
	void Save() const;
	ImGuiStyle_t InitStyle() const;

	inline bool KeyUsed(const int key) const
	{
		return (key == m_ConsoleConfig.m_nBind0 || key == m_ConsoleConfig.m_nBind1)
			|| (key == m_BrowserConfig.m_nBind0 || key == m_BrowserConfig.m_nBind1)
			|| (key == m_LocalConfig.m_nBind0 || key == m_LocalConfig.m_nBind1)
			|| (key == m_DevMenuConfig.m_nBind0 || key == m_DevMenuConfig.m_nBind1);
	};
};

extern ImGuiConfig g_ImGuiConfig;
