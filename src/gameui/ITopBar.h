#pragma once
//=============================================================================//
//
// Purpose: In-game top bar UI
//
//=============================================================================//
#ifndef DEDICATED
#include "common/sdkdefs.h"
#include "imgui_surface.h"

class CTopBar : public CImguiSurface
{
public:
	CTopBar();
	virtual ~CTopBar() {}

	virtual bool Init();
	virtual void Shutdown();

	virtual void RunFrame();
	virtual void RenderShowMainBar();
	virtual bool DrawSurface();

	// Command callback
	static void DevMenuAdd_f(const CCommand& args);
	static void DevMenuRemove_f(const CCommand& args);
	static void DevMenuClear_f(const CCommand& args);

	// inlines
	inline void SetUnlockMouse(const bool unlock)
	{
		m_unlockMouse = unlock;
		m_activated = unlock; // drive input capture via IsSurfaceActive
		m_requestCloseMenus = true; // close any open sub menus on toggle
	}
	inline bool IsMouseUnlocked() const { return m_unlockMouse; }

private:
	struct PresetCommand_t; // forward declaration for use in prototypes
	void DrawMenuBar();
	void DrawMenuRecursive(const vector<PresetCommand_t>& commands);

private:
	bool m_unlockMouse;
	bool m_requestCloseMenus;

	// Preset dev menu
	struct PresetCommand_t
	{
		string m_svLabel;
		string m_svCommand;
		string m_svTooltip;
		vector<PresetCommand_t> m_vecChildren;

		PresetCommand_t(const string& label) : m_svLabel(label) {}
		PresetCommand_t(const string& label, const string& command, const string& tooltip)
			: m_svLabel(label), m_svCommand(command), m_svTooltip(tooltip) {}
	};

	void AddPresetCommand(const char* const szPath, const char* const szCommand, const char* const szTooltip);
	void RemovePresetCommand(const char* const szPath);
	void ClearPresetCommands(void);

	vector<PresetCommand_t> m_vecRootCommands;
};

extern CTopBar g_TopBar;
#endif // !DEDICATED


