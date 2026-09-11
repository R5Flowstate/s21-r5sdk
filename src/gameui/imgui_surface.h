#if defined(CLIENT_DLL)
#ifndef IMGUI_SURFACE_H
#define IMGUI_SURFACE_H

#define IMGUI_SURFACE_FADE_ANIM_SPEED 5.0f

#include "imgui/misc/imgui_utility.h"

class CImguiSurface
{
public:
	CImguiSurface();
	virtual ~CImguiSurface() { };

	virtual bool Init() = 0;
	virtual void Shutdown() = 0;

	virtual void Animate();
	virtual void RunFrame() = 0;

	virtual bool DrawSurface() = 0;

	virtual void SetStyleVar();
	virtual void SetRect(const float width, const float height, const float x, const float y);

	// inlines -- prefer SetActive over raw XOR so all toggle sites share
	// one edge API (WndProc bind, ConCommand, DX12 fallback).
	inline void SetActive(const bool active) { m_activated = active; }
	inline void ToggleActive() { m_activated = !m_activated; }

	inline bool IsActivated() const { return m_activated; }
	inline bool IsVisible() const { return m_fadeAlpha > 0.0f; }

	// True if this surface, when activated, should capture keyboard/mouse
	// away from the game (e.g. Console, Browser, DevMenu, TopBar). Override
	// to false for read-only HUD-style overlays (ParticleOverlay,
	// StreamOverlay) that auto-activate from a ConVar but never need to
	// consume input -- otherwise their auto-activation routes WASD into
	// imgui and the player can't walk.
	virtual bool IsModal() const { return true; }

protected:
	const char* m_surfaceLabel;

	float m_fadeAlpha;
	ImGuiStyle_t m_surfaceStyle;

	bool m_initialized;
	bool m_activated;
	bool m_rectSet;
	bool m_reclaimFocus;
};

#endif // IMGUI_SURFACE_H
#else // !CLIENT_DLL
#ifndef IMGUI_SURFACE_H
#define IMGUI_SURFACE_H

#define IMGUI_SURFACE_FADE_ANIM_SPEED 5.0f

#include "imgui/misc/imgui_utility.h"

class CImguiSurface
{
public:
	CImguiSurface();
	virtual ~CImguiSurface() { };

	virtual bool Init() = 0;
	virtual void Shutdown() = 0;

	virtual void Animate();
	virtual void RunFrame() = 0;

	virtual bool DrawSurface() = 0;

	virtual void SetStyleVar();
	virtual void SetRect(const float width, const float height, const float x, const float y);

	// inlines
	inline void SetActive(const bool active) { m_activated = active; }
	inline void ToggleActive() { m_activated = !m_activated; }

	inline bool IsActivated() const { return m_activated; }
	inline bool IsVisible() const { return m_fadeAlpha > 0.0f; }

protected:
	const char* m_surfaceLabel;

	float m_fadeAlpha;
	ImGuiStyle_t m_surfaceStyle;

	bool m_initialized;
	bool m_activated;
	bool m_rectSet;
	bool m_reclaimFocus;
};

#endif // IMGUI_SURFACE_H
#endif // CLIENT_DLL
