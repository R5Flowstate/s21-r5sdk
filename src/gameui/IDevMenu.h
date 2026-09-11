#pragma once
//=============================================================================//
//
// Purpose: In-game developer menu UI
//
//=============================================================================//
#ifndef DEDICATED
#include "common/sdkdefs.h"
#include "imgui_surface.h"

class CDevMenu : public CImguiSurface
{
public:
	CDevMenu();
	virtual ~CDevMenu() {}

	virtual bool Init();
	virtual void Shutdown();

	virtual void RunFrame();
	virtual bool DrawSurface();

	void AddPresetCommand(const char* const szPath, const char* const szCommand, const char* const szTooltip);
	void RemovePresetCommand(const char* const szPath);
	void ClearPresetCommands(void);

private:
	struct PresetCommand_t;
	void DrawMenuRecursive(const vector<PresetCommand_t>& commands);

private:
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

	vector<PresetCommand_t> m_vecRootCommands;
};

extern CDevMenu g_DevMenu;
#endif // !DEDICATED


