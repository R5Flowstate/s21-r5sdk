//=============================================================================//
//
// Purpose: F9 overlay matching the community DLSS 5 Neural Rendering panel.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "windows/id3dx.h"
#include "windows/dlssnr.h"
#include "windows/dlss_sr.h"
#include "imgui/misc/imgui_utility.h"
#include "gameui/IDlssNrMenu.h"

CDlssNrMenu::CDlssNrMenu()
{
	m_surfaceLabel = "DLSS 5 Neural Rendering";
}

bool CDlssNrMenu::Init()
{
	SetStyleVar();
	return true;
}

void CDlssNrMenu::Shutdown()
{
}

void CDlssNrMenu::RunFrame()
{
	if (!m_activated)
		return;

	if (!m_initialized)
	{
		Init();
		m_initialized = true;
	}

	DrawSurface();
}

bool CDlssNrMenu::DrawSurface()
{
	if (!m_activated)
		return false;

	int stylePushed = 0;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f); stylePushed++;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f)); stylePushed++;
	ImGui::SetNextWindowSize(ImVec2(460, 520), ImGuiCond_FirstUseEver);

	if (!ImGui::Begin(m_surfaceLabel, &m_activated, ImGuiWindowFlags_NoCollapse, &ResetInput))
	{
		ImGui::End();
		ImGui::PopStyleVar(stylePushed);
		return false;
	}

	ImGui::TextUnformatted("F9  toggle");
	ImGui::Separator();

	ConVar* enable = g_pCVar ? g_pCVar->FindVar("settings_dlssnr") : nullptr;
	ConVar* upscale = g_pCVar ? g_pCVar->FindVar("settings_dlss_sr") : nullptr;
	ConVar* intensity = g_pCVar ? g_pCVar->FindVar("settings_dlssnr_intensity") : nullptr;
	ConVar* style = g_pCVar ? g_pCVar->FindVar("settings_dlssnr_style") : nullptr;
	ConVar* preset = g_pCVar ? g_pCVar->FindVar("settings_dlssnr_preset") : nullptr;

	if (enable)
	{
		bool on = enable->GetInt() != 0;
		if (ImGui::Checkbox("Enable DLSS Neural Rendering", &on))
			enable->SetValue(on ? 1 : 0);
	}
	if (upscale)
	{
		bool on = upscale->GetInt() != 0;
		if (ImGui::Checkbox("Enable Upscaling", &on))
			upscale->SetValue(on ? 1 : 0);
	}

	const bool nvidia = DlssNr_IsNvidia();
	const bool dll = DlssNr_DllPresent();
	const bool canTune = (!DlssNr_VendorKnown() || nvidia) && dll;
	if (!canTune)
		ImGui::BeginDisabled();

	if (intensity)
	{
		float v = intensity->GetFloat();
		if (ImGui::SliderFloat("NR Intensity", &v, 0.0f, 2.0f, "%.2f"))
			intensity->SetValue(v);
	}
	if (style)
	{
		int v = style->GetInt();
		if (ImGui::SliderInt("Style", &v, 0, 7))
			style->SetValue(v);
	}
	if (preset)
	{
		int v = preset->GetInt();
		if (ImGui::SliderInt("Preset", &v, 0, 7))
			preset->SetValue(v);
	}

	if (!canTune)
		ImGui::EndDisabled();

	ImGui::Separator();
	ImGui::TextUnformatted("Status");

	const char* nrState = DlssNr_NrStatusText();
	ImVec4 nrCol = ImVec4(0.75f, 0.75f, 0.35f, 1.0f);
	if (strcmp(nrState, "RUNNING") == 0)
		nrCol = ImVec4(0.35f, 0.85f, 0.40f, 1.0f);
	else if (strcmp(nrState, "FAILED") == 0)
		nrCol = ImVec4(0.90f, 0.35f, 0.30f, 1.0f);
	ImGui::Text("DLSSNR:");
	ImGui::SameLine();
	ImGui::TextColored(nrCol, "%s", nrState);

	if (!DlssNr_VendorKnown())
		ImGui::TextUnformatted("GPU:     probing");
	else if (nvidia)
		ImGui::TextUnformatted("GPU:     NVIDIA");
	else
		ImGui::TextUnformatted("GPU:     not NVIDIA -- idle");
	ImGui::Text("DLL:     %s", dll ? "nvngx_dlssnr.dll present" : "missing (not shipped)");
	ImGui::Text("NGX:     %s", DlssNr_HookText());
	ImGui::Text("Snippet: %s", DlssNr_NrHostText());
	ImGui::Text("Feature 18 probe         0x%08X",
		static_cast<unsigned>(DlssNr_NrProbeResult()));
	ImGui::Text("NGX feature 18   %s | evaluations %d",
		DlssNr_FeatureLive() ? "created" : (DlssNr_Latched() ? "latched" : "..."),
		DlssNr_NrEvals());
	ImGui::Text("Successful NR frames    %d", DlssNr_NrEvals());
	ImGui::Text("Latest NR NGX result     0x%08X %s",
		static_cast<unsigned>(DlssNr_LastNrResult()), DlssNr_LastNrResultName());
	ImGui::Text("SuperSampling            %s",
		DlssSr_Live() ? "replacing TSAA"
		: (!DlssSr_TsaaHooked() ? "hook missing"
		: (DlssSr_TsaaCount() == 0 ? "hooked, 0 resolves"
		: ((DlssSr_LastFail() && DlssSr_LastFail()[0]) ? DlssSr_LastFail()
		: "TSAA (stock)"))));
	if (DlssSr_TsaaHooked() && !DlssSr_Live())
		ImGui::Text("  tsaa %llu  srFail %llu",
			DlssSr_TsaaCount(), DlssSr_EvalFailCount());
	if (DlssSr_Live())
		ImGui::Text("  %ux%u -> %ux%u  ss-evals %d",
			DlssSr_RenderWidth(), DlssSr_RenderHeight(),
			DlssSr_DisplayWidth(), DlssSr_DisplayHeight(), DlssNr_SsEvals());

	ImGui::Separator();
	if (ImGui::Button("Reset NR feature and clear failure latch"))
		DlssNr_ResetLatch();
	if (ImGui::Button("Write dlssnr.log"))
		DlssNr_DumpStatus();
	ImGui::TextUnformatted("log: <exe folder>\\dlssnr.log");

	ImGui::End();
	ImGui::PopStyleVar(stylePushed);
	return true;
}

CDlssNrMenu g_DlssNrMenu;
