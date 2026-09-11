//===========================================================================//
//
// Purpose: Implements the debug panels.
//
// $NoKeywords: $
//===========================================================================//

#include <core/stdafx.h>
#include <tier1/cvar.h>
#include <tier1/keyvalues.h>
#include <windows/id3dx.h>
#include <mathlib/color.h>
#include <rtech/rui/rui.h>
#include <vgui/vgui_debugpanel.h>
#include <vguimatsurface/MatSystemSurface.h>
#include <materialsystem/cmaterialsystem.h>
#include <materialsystem/texturestreaming.h>
#include <engine/sys_engine.h>
#include <engine/sys_mainwind.h>
#include <engine/gl_rmain.h>
#include <engine/debugoverlay.h>
#include <engine/client/clientstate.h>
#include <game/client/viewrender.h>

// Various cvars that enable/disable debug text overlays
static ConVar con_drawnotify("con_drawnotify", "0", FCVAR_RELEASE | FCVAR_MATERIAL_SYSTEM_THREAD, "Draw the most recent lines of the console to the HUD");
static ConVar cl_showFrameMetrics("cl_showFrameMetrics", "0", FCVAR_DEVELOPMENTONLY, "Shows the tick counter for the server/client simulation and the render frame, along with statistics regarding texture streaming memory");
static ConVar cl_showMaterialCrosshair("cl_showMaterialCrosshair", "0", FCVAR_DEVELOPMENTONLY, "Draw info for the material under the crosshair on the screen");

// Various cvars that dictate how many lines and how long the text is shown
static ConVar con_notifylines("con_notifylines", "3", FCVAR_MATERIAL_SYSTEM_THREAD, "Number of recent console lines to show on the HUD", true, 1.f, false, 0.f);
static ConVar con_notifytime("con_notifytime", "6", FCVAR_MATERIAL_SYSTEM_THREAD, "How long to display recent console text on the HUD", true, 1.f, false, 0.f);

// Various cvars that dictate where the debug text is shown on the screen
static ConVar con_notify_pos_x("con_notify_pos_x", "0.002f", FCVAR_MATERIAL_SYSTEM_THREAD, "X position for the HUD console", true, 0.f, true, 1.f);
static ConVar con_notify_pos_y("con_notify_pos_y", "0.002f", FCVAR_MATERIAL_SYSTEM_THREAD, "Y position for the HUD console", true, 0.f, true, 1.f);

static ConVar cl_nprintf_pos_x("cl_nprintf_pos_x", "0.002f", FCVAR_DEVELOPMENTONLY, "X position for the notify print debug overlay", true, 0.f, true, 1.f);
static ConVar cl_nprintf_pos_y("cl_nprintf_pos_y", "0.002f", FCVAR_DEVELOPMENTONLY, "Y position for the notify print debug overlay", true, 0.f, true, 1.f);

static ConVar cl_frameMetrics_pos_x("cl_frameMetrics_pos_x", "0.65f", FCVAR_DEVELOPMENTONLY, "X position for the frame metrics debug overlay", true, 0.f, true, 1.f);
static ConVar cl_frameMetrics_pos_y("cl_frameMetrics_pos_y", "0.80f", FCVAR_DEVELOPMENTONLY, "Y position for the frame metrics debug overlay", true, 0.f, true, 1.f);

static ConVar cl_materialCrosshair_pos_x("cl_materialCrosshair_pos_x", "0.002f", FCVAR_DEVELOPMENTONLY, "X position for material debug info overlay", true, 0.f, true, 1.f);
static ConVar cl_materialCrosshair_pos_y("cl_materialCrosshair_pos_y", "0.5f", FCVAR_DEVELOPMENTONLY, "Y position for material debug info overlay", true, 0.f, true, 1.f);

//-----------------------------------------------------------------------------
// Purpose: update the mini console
//-----------------------------------------------------------------------------
void CTextOverlay::UpdateMiniConsole(void)
{
	if (!g_pMatSystemSurface)
	{
		return;
	}

	m_updateFontFace = true;

	if (con_drawnotify.GetBool())
	{
		DrawNotify();
	}
}

//-----------------------------------------------------------------------------
// Purpose: update the in-game debug panels
//-----------------------------------------------------------------------------
void CTextOverlay::UpdateInGamePanels(void)
{
	if (!g_pMatSystemSurface)
	{
		return;
	}

	m_updateFontFace = true;

	if (cl_showFrameMetrics.GetBool())
	{
		DrawFrameMetrics();
	}
	if (cl_showMaterialCrosshair.GetBool())
	{
		DrawCrosshairMaterial();
	}

	Con_NPrintf();

	if (enable_debug_text_overlays.GetBool())
	{
		DrawDebugOverlay();
	}
}

//-----------------------------------------------------------------------------
// Purpose: add a log to the vector.
//-----------------------------------------------------------------------------
void CTextOverlay::AddLog(const eDLL_T context, const char* pszText, const ssize_t textLen)
{
	Assert(pszText);

	if (!con_drawnotify.GetBool() || !VALID_CHARSTAR(pszText))
	{
		return;
	}

	AUTO_LOCK(m_Mutex);

	const int newLine = m_NotifyLines.AddToTail();
	TextNotify_s& notify = m_NotifyLines[newLine];

	notify.Init(context, con_notifytime.GetFloat(), pszText, textLen);

	while (m_NotifyLines.Count() > 0 &&
		(m_NotifyLines.Count() > con_notifylines.GetInt()))
	{
		m_NotifyLines.Remove(0);
	}
}

//-----------------------------------------------------------------------------
// Purpose: draw notify logs on screen.
//-----------------------------------------------------------------------------
void CTextOverlay::DrawNotify(void)
{
	Vector2D screenPos;
	ScreenPosition(*g_pViewRender->GetMainView(), con_notify_pos_x.GetFloat(), con_notify_pos_y.GetFloat(), &screenPos);

	AUTO_LOCK(m_Mutex);

	for (int i = 0, j = m_NotifyLines.Count(); i < j; i++)
	{
		const TextNotify_s& notify = m_NotifyLines[i];
		Color c = GetLogColorForType(notify.m_Type);

		const float flTimeleft = notify.m_flLifeRemaining;

		if (flTimeleft < 1.0f)
		{
			const float f = clamp(flTimeleft, 0.0f, 1.0f) / 1.0f;
			c[3] = uint8_t(f * 255.0f);

			if (i == 0 && f < 0.2f)
			{
				screenPos.y -= int((m_nLineSpacing+1) * (float(1.0f - f / 0.2f)));
			}
		}
		else
		{
			c[3] = 255;
		}
		MatSystemSurface_DrawColoredText(g_pMatSystemSurface, GetFontFace(),
			m_nFontHeight, (int)screenPos.x, (int)screenPos.y, c.r(), c.g(), c.b(), c.a(), "%s", notify.m_Text.String());

		screenPos.y += (m_nLineSpacing+1);
	}
}

//-----------------------------------------------------------------------------
// Purpose: draws debug text overlay
//-----------------------------------------------------------------------------
void CTextOverlay::DrawDebugOverlay(void)
{
	if (!g_pDebugOverlay || !g_pMatSystemSurface)
		return;

	const OverlayText_t* pCurrText = g_pDebugOverlay->GetFirstText();

	for (; pCurrText; pCurrText = g_pDebugOverlay->GetNextText(pCurrText))
	{
		// If this gets fired, an empty overlay was added.
		Assert(pCurrText->textBuf);
		Assert(pCurrText->textLen > 0);

		if (!g_pViewRender)
			continue;

		const CViewSetup* const viewSetup = g_pViewRender->GetMainView();
		if (!viewSetup)
			continue;

		Vector2D screenPos;
		bool onScreen = false;

		if (pCurrText->bUseOrigin)
		{
			const VMatrix* const viewMatrix = g_pViewRender->GetViewProjectionMatrix(VMATRIX_TYPE_VIEW);

			if (viewMatrix)
				onScreen = ScreenTransform(*viewSetup, *viewMatrix, pCurrText->origin, &screenPos);
		}
		else
			onScreen = ScreenPosition(*viewSetup, pCurrText->screenPos, &screenPos);

		if (onScreen)
		{
			screenPos.y += (pCurrText->lineOffset * m_nLineSpacing);

			MatSystemSurface_DrawColoredText(g_pMatSystemSurface, GetFontFace(), m_nFontHeight,
				(int)screenPos.x, (int)screenPos.y, pCurrText->r, pCurrText->g, pCurrText->b, pCurrText->a,
				"%s", pCurrText->textBuf);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: checks if the notify text is expired
// Input: flFrameTime - 
//-----------------------------------------------------------------------------
void CTextOverlay::ShouldDraw(const float flFrameTime)
{
	if (con_drawnotify.GetBool())
	{
		AUTO_LOCK(m_Mutex);

		FOR_EACH_VEC_BACK(m_NotifyLines, i)
		{
			TextNotify_s& notify = m_NotifyLines[i];
			notify.m_flLifeRemaining -= flFrameTime;

			if (notify.m_flLifeRemaining <= 0.0f)
			{
				m_NotifyLines.Remove(i);
				continue;
			}
		}
	}
	else if (!m_NotifyLines.IsEmpty())
	{
		AUTO_LOCK(m_Mutex);
		m_NotifyLines.Purge();
	}
}

//-----------------------------------------------------------------------------
// Purpose: draws console messages on screen (only used for 'host_speeds'!, deprecated!!).
//-----------------------------------------------------------------------------
void CTextOverlay::Con_NPrintf(void)
{
	if (!m_szCon_NPrintf_Buf[0] || !g_pViewRender)
	{
		return;
	}

	Vector2D screenPos;
	ScreenPosition(*g_pViewRender->GetMainView(), cl_nprintf_pos_x.GetFloat(), cl_nprintf_pos_y.GetFloat(), &screenPos);

	screenPos.y += m_nCon_NPrintf_Idx * m_nLineSpacing;
	MatSystemSurface_DrawColoredText(g_pMatSystemSurface, GetFontFace(), m_nFontHeight, (int)screenPos.x, (int)screenPos.y, 255, 255, 255, 255, "%s", m_szCon_NPrintf_Buf);

	m_nCon_NPrintf_Idx = 0;
	m_szCon_NPrintf_Buf[0] = '\0';
}

//-----------------------------------------------------------------------------
// Purpose: draws live simulation and texture memory statistics on screen.
//-----------------------------------------------------------------------------
void CTextOverlay::DrawFrameMetrics(void)
{
	Vector2D screenPos;
	ScreenPosition(*g_pViewRender->GetMainView(), cl_frameMetrics_pos_x.GetFloat(), cl_frameMetrics_pos_y.GetFloat(), &screenPos);

	const u16 fontFace = GetFontFace();

	MatSystemSurface_DrawColoredText(g_pMatSystemSurface, fontFace, m_nFontHeight, (int)screenPos.x, (int)screenPos.y, 255, 255, 255, 255,
		"Server Frame: (%d) Client Frame: (%d) Render Frame: (%d)\n", g_pClientState->GetServerTickCount(), g_pClientState->GetClientTickCount(), *g_nRenderTickCount);

	screenPos.y += m_nLineSpacing;

	MatSystemSurface_DrawColoredText(g_pMatSystemSurface, fontFace, m_nFontHeight, (int)screenPos.x, (int)screenPos.y, 255, 255, 255, 255,
		"%8zd/%8zd/%8zdkiB unusable/unfree/total GPU Streaming Texture memory\n",
		g_textureStreamMemoryUsed[TML_TRACKER_UNUSABE] / 1024, g_textureStreamMemoryUsed[TML_TRACKER_UNFREE] / 1024, *g_textureStreamMemoryTarget / 1024);
}

//-----------------------------------------------------------------------------
// Purpose: draws currently traced material info on screen.
//-----------------------------------------------------------------------------
void CTextOverlay::DrawCrosshairMaterial(void)
{
	const CMaterialGlue* const materialGlue = v_GetMaterialAtCrossHair();

	if (!materialGlue)
		return;

	const MaterialGlue_s* const material = materialGlue->Get();

	Vector2D screenPos;
	ScreenPosition(*g_pViewRender->GetMainView(), cl_materialCrosshair_pos_x.GetFloat(), cl_materialCrosshair_pos_y.GetFloat(), &screenPos);

	MatSystemSurface_DrawColoredText(g_pMatSystemSurface, GetFontFace(), m_nFontHeight, (int)screenPos.x, (int)screenPos.y, 255, 255, 255, 255,
		"name: %s\nguid: %llx\ndimensions: %hu x %hu\nsurface: %s/%s\nstc: %hu\ntc: %hu",
		material->name,
		material->guid,
		material->width, material->height,
		material->surfaceProp, material->surfaceProp2,
		material->streamingTextureHandleCount,
		material->shaderset->m_nTextureInputCount);
}

//-----------------------------------------------------------------------------
// Purpose: retrieves the current font face, updates the handle if necessary
//-----------------------------------------------------------------------------
u16 CTextOverlay::GetFontFace()
{
	if (m_updateFontFace)
	{
		m_currentFontFace = v_Rui_GetFontFace();
		m_updateFontFace = false;
	}

	return m_currentFontFace;
}

//-----------------------------------------------------------------------------
// Purpose: gets the log color for context.
// Input: context - 
// Output: Color
//-----------------------------------------------------------------------------
Color CTextOverlay::GetLogColorForType(const eDLL_T context) const
{
	switch (context)
	{
	case eDLL_T::SCRIPT_SERVER:
		return { 130, 120, 245, 255, };
	case eDLL_T::SCRIPT_CLIENT:
		return { 117, 116, 139, 255 };
	case eDLL_T::SCRIPT_UI:
		return { 200, 110, 110, 255 };
	case eDLL_T::SERVER:
		return { 20, 50, 248, 255 };
	case eDLL_T::CLIENT:
		return { 70, 70, 70, 255 };
	case eDLL_T::UI:
		return { 200, 60, 60, 255 };
	case eDLL_T::ENGINE:
		return { 255, 255, 255, 255 };
	case eDLL_T::FS:
		return { 0, 100, 225, 255 };
	case eDLL_T::RTECH:
		return { 25, 120, 20, 255 };
	case eDLL_T::MS:
		return { 200, 20, 180, 255 };
	case eDLL_T::AUDIO:
		return { 238, 43, 10, 255 };
	case eDLL_T::VIDEO:
		return { 115, 0, 235, 255 };
	case eDLL_T::NETCON:
		return { 255, 255, 255, 255 };
	case eDLL_T::COMMON:
		return { 255, 140, 80, 255 };
	case eDLL_T::SYSTEM_WARNING:
		return { 180, 180, 20, 255 };
	case eDLL_T::SYSTEM_ERROR:
		return { 225, 20, 20, 255 };
	case eDLL_T::MODSYSTEM:
		return { 255, 205, 0, 255 };
	default:
		return { 255, 255, 255, 255 };
	}
}

///////////////////////////////////////////////////////////////////////////////
CTextOverlay g_TextOverlay;
