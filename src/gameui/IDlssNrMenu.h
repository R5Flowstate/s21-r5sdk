#pragma once
#ifndef DEDICATED
#include "imgui_surface.h"

class CDlssNrMenu : public CImguiSurface
{
public:
	CDlssNrMenu();
	virtual ~CDlssNrMenu() {}

	virtual bool Init();
	virtual void Shutdown();
	virtual void RunFrame();
	virtual bool DrawSurface();
};

extern CDlssNrMenu g_DlssNrMenu;
#endif // !DEDICATED
