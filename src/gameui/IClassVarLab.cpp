//=============================================================================//
//
// Purpose: Movement Lab tab over the engine class-var system
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "engine/cmd.h"
#include "game/client/classvar_natives.h"
#include "gameui/IClassVarLab.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

static constexpr int CVLAB_ENUM_MAX = 8192;
static constexpr int CVLAB_QUEUE_MAX = 256;
static constexpr double CVLAB_SEND_INTERVAL = 0.05;
static constexpr const char* CVLAB_PROFILE_DIR = "platform/cfg/movementlab";
static constexpr int CVLAB_PROFILE_MAX_ROWS = 512;

CClassVarLab::CClassVarLab()
{
	m_nSerial = 0;
	m_nBlock = 0;
	m_flLastSend = 0.0;
	m_bNeedRefresh = true;
	m_bModifiedOnly = false;
	m_bShowReadOnly = false;
	m_bApplyToAll = false;
	m_bProfilesScanned = false;
	m_szFilter[0] = '\0';
	m_szProfile[0] = '\0';
}

static bool ClassVarLab_ProfileNameOk(const char* pszName)
{
	if (!pszName || !pszName[0])
		return false;
	for (const char* p = pszName; *p; p++)
	{
		const char c = *p;
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
		if (!ok || (p - pszName) >= 32)
			return false;
	}
	return true;
}

static const char* ClassVarLab_TypeText(const uint16_t nType)
{
	switch (nType)
	{
	case 0:
		return "bool";
	case 1:
		return "int";
	case 2:
		return "float";
	case 3:
		return "float2";
	case 4:
		return "float3";
	default:
		break;
	}
	return "string";
}

static string ClassVarLab_Prefix(const char* pszName)
{
	string out;
	for (const char* p = pszName; *p; p++)
	{
		if ((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')
			break;
		out.push_back(*p);
	}
	return out;
}

void CClassVarLab::Refresh(void)
{
	vector<ClassVarField_t> fields;
	fields.resize(CVLAB_ENUM_MAX);
	const int nCount = ClassVar_EnumFields(fields.data(), CVLAB_ENUM_MAX);

	m_fields.clear();
	m_baseline.clear();
	m_scratch.clear();
	m_order.clear();

	for (int i = 0; i < nCount; i++)
	{
		m_fields.push_back(fields[i]);
		const float flLive = LiveValue(fields[i]);
		m_baseline.push_back(flLive);
		m_scratch.push_back(flLive);
		m_order.push_back(m_order.size());
	}

	std::sort(m_order.begin(), m_order.end(), [this](const size_t a, const size_t b)
	{
		return strcmp(m_fields[a].pszName, m_fields[b].pszName) < 0;
	});

	m_nSerial = ClassVar_SettingsRebuildSerial();
	m_nBlock = ClassVar_LocalBlock();
	m_bNeedRefresh = false;
}

void CClassVarLab::Queue(const char* pszKey, const char* pszValue)
{
	if (!pszKey || !pszKey[0] || !pszValue)
		return;

	if (m_queue.size() >= CVLAB_QUEUE_MAX)
	{
		Warning(eDLL_T::CLIENT, "[CVLAB] queue full -- dropping 'set %s %s'\n", pszKey, pszValue);
		return;
	}

	char szLine[256];
	V_snprintf(szLine, sizeof(szLine), "%s %s %s", m_bApplyToAll ? "setall" : "set", pszKey, pszValue);
	m_queue.emplace_back(szLine);
}

void CClassVarLab::ScanProfiles(void)
{
	m_profiles.clear();
	m_bProfilesScanned = true;

	char szGlob[MAX_PATH];
	V_snprintf(szGlob, sizeof(szGlob), "%s/*.csv", CVLAB_PROFILE_DIR);

	WIN32_FIND_DATAA fd;
	HANDLE hFind = FindFirstFileA(szGlob, &fd);
	if (hFind == INVALID_HANDLE_VALUE)
		return;

	int nGuard = 0;
	do
	{
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		string name(fd.cFileName);
		if (name.size() > 4)
			name.resize(name.size() - 4);
		if (ClassVarLab_ProfileNameOk(name.c_str()))
			m_profiles.push_back(name);
	} while (FindNextFileA(hFind, &fd) && ++nGuard < 512);
	FindClose(hFind);

	std::sort(m_profiles.begin(), m_profiles.end());
}

bool CClassVarLab::SaveProfile(const char* pszName)
{
	if (!ClassVarLab_ProfileNameOk(pszName))
	{
		Warning(eDLL_T::CLIENT, "[CVLAB] profile name must be 1-32 chars of [A-Za-z0-9_-]\n");
		return false;
	}

	CreateDirectoryA("platform/cfg", nullptr);
	CreateDirectoryA(CVLAB_PROFILE_DIR, nullptr);

	char szPath[MAX_PATH];
	V_snprintf(szPath, sizeof(szPath), "%s/%s.csv", CVLAB_PROFILE_DIR, pszName);

	FILE* pFile = nullptr;
	if (fopen_s(&pFile, szPath, "w") != 0 || !pFile)
	{
		Warning(eDLL_T::CLIENT, "[CVLAB] cannot write %s\n", szPath);
		return false;
	}

	int nRows = 0;
	for (size_t i = 0; i < m_fields.size(); i++)
	{
		const ClassVarField_t& field = m_fields[i];
		if (field.nType > 2 || !IsModified(i))
			continue;
		const float flLive = LiveValue(field);
		if (field.nType == 0)
			fprintf(pFile, "%s,%d\n", field.pszName, flLive >= 0.5f ? 1 : 0);
		else if (field.nType == 1)
			fprintf(pFile, "%s,%d\n", field.pszName, static_cast<int>(flLive));
		else
			fprintf(pFile, "%s,%g\n", field.pszName, static_cast<double>(flLive));
		nRows++;
	}
	fclose(pFile);

	Msg(eDLL_T::CLIENT, "[CVLAB] saved %d field(s) to %s\n", nRows, szPath);
	ScanProfiles();
	return true;
}

bool CClassVarLab::LoadProfile(const char* pszName)
{
	if (!ClassVarLab_ProfileNameOk(pszName))
		return false;

	char szPath[MAX_PATH];
	V_snprintf(szPath, sizeof(szPath), "%s/%s.csv", CVLAB_PROFILE_DIR, pszName);

	FILE* pFile = nullptr;
	if (fopen_s(&pFile, szPath, "r") != 0 || !pFile)
	{
		Warning(eDLL_T::CLIENT, "[CVLAB] cannot read %s\n", szPath);
		return false;
	}

	int nRows = 0;
	int nSkipped = 0;
	char szLine[256];
	while (nRows + nSkipped < CVLAB_PROFILE_MAX_ROWS && fgets(szLine, sizeof(szLine), pFile))
	{
		char* pszComma = strchr(szLine, ',');
		if (!pszComma)
			continue;
		*pszComma = '\0';
		const char* const pszKey = szLine;
		char* pszValue = pszComma + 1;
		while (*pszValue == ' ' || *pszValue == '\t')
			pszValue++;
		char* pszEnd = pszValue + strlen(pszValue);
		while (pszEnd > pszValue && (pszEnd[-1] == '\n' || pszEnd[-1] == '\r' || pszEnd[-1] == ' '))
			*--pszEnd = '\0';

		// Only keys the live table knows, only numeric values -- the file is user-editable.
		const ClassVarField_t* pField = nullptr;
		for (const ClassVarField_t& field : m_fields)
		{
			if (field.nType <= 2 && V_stricmp(field.pszName, pszKey) == 0)
			{
				pField = &field;
				break;
			}
		}
		char* pszNumEnd = nullptr;
		const float flVal = strtof(pszValue, &pszNumEnd);
		if (!pField || !pszValue[0] || !pszNumEnd || *pszNumEnd || !std::isfinite(flVal) || fabsf(flVal) > 100000.f)
		{
			Warning(eDLL_T::CLIENT, "[CVLAB] %s: skipped '%s,%s'\n", pszName, pszKey, pszValue);
			nSkipped++;
			continue;
		}

		Queue(pField->pszName, pszValue);
		nRows++;
	}
	fclose(pFile);

	Msg(eDLL_T::CLIENT, "[CVLAB] loaded %d field(s) from %s (%d skipped)\n", nRows, szPath, nSkipped);
	return true;
}

float CClassVarLab::LiveValue(const ClassVarField_t& field) const
{
	const uintptr_t nAddr = ClassVar_FieldAddress(field);
	if (!nAddr)
		return 0.0f;

	switch (field.nType)
	{
	case 0:
		return *reinterpret_cast<const uint8_t*>(nAddr) ? 1.0f : 0.0f;
	case 1:
		return static_cast<float>(*reinterpret_cast<const int32_t*>(nAddr));
	case 2:
	case 3:
	case 4:
		return *reinterpret_cast<const float*>(nAddr);
	default:
		break;
	}
	return 0.0f;
}

bool CClassVarLab::IsModified(size_t nRow) const
{
	const ClassVarField_t& field = m_fields[nRow];
	if (field.nType >= 3)
		return false;

	const float flBase = m_baseline[nRow];
	const float flTol = 1e-6f * (fabsf(flBase) > 1.0f ? fabsf(flBase) : 1.0f);
	return fabsf(LiveValue(field) - flBase) > flTol;
}

void CClassVarLab::DrawRow(size_t nRow)
{
	const ClassVarField_t& field = m_fields[nRow];
	const bool bModified = IsModified(nRow);

	ImGui::PushID(field.pszName);
	ImGui::TableNextRow();
	if (bModified)
		ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(0.45f, 0.35f, 0.12f, 0.65f)));

	ImGui::TableNextColumn();
	ImGui::TextUnformatted(field.pszName);

	ImGui::TableNextColumn();
	ImGui::Text("%s", ClassVarLab_TypeText(field.nType));

	ImGui::TableNextColumn();
	const uintptr_t nAddr = ClassVar_FieldAddress(field);
	if (field.nType == 0)
	{
		bool bVal = nAddr ? (*reinterpret_cast<const uint8_t*>(nAddr) != 0) : (m_baseline[nRow] >= 0.5f);
		if (ImGui::Checkbox("##value", &bVal))
			Queue(field.pszName, bVal ? "1" : "0");
	}
	else if (field.nType == 1)
	{
		int nVal = static_cast<int>(m_scratch[nRow]);
		ImGui::InputInt("##value", &nVal, 0, 0);
		if (ImGui::IsItemDeactivatedAfterEdit())
		{
			char szValue[32];
			V_snprintf(szValue, sizeof(szValue), "%d", nVal);
			Queue(field.pszName, szValue);
		}
		else if (!ImGui::IsItemActive())
			nVal = nAddr ? *reinterpret_cast<const int32_t*>(nAddr) : static_cast<int>(m_baseline[nRow]);
		m_scratch[nRow] = static_cast<float>(nVal);
	}
	else if (field.nType == 2)
	{
		const float flStep = fabsf(m_baseline[nRow]) * 0.005f;
		const float flSpeed = flStep > 0.01f ? flStep : 0.01f;
		ImGui::DragFloat("##value", &m_scratch[nRow], flSpeed, 0.0f, 0.0f, "%.4g");
		if (ImGui::IsItemDeactivatedAfterEdit())
		{
			char szValue[32];
			V_snprintf(szValue, sizeof(szValue), "%g", static_cast<double>(m_scratch[nRow]));
			Queue(field.pszName, szValue);
		}
		else if (!ImGui::IsItemActive())
			m_scratch[nRow] = nAddr ? *reinterpret_cast<const float*>(nAddr) : m_baseline[nRow];
	}
	else
	{
		char szValue[256];
		if (nAddr)
			ClassVar_FormatValue(nAddr, field.nType, szValue, sizeof(szValue));
		else
			V_snprintf(szValue, sizeof(szValue), "%g", static_cast<double>(m_baseline[nRow]));
		ImGui::TextDisabled("%s", szValue);
	}

	ImGui::TableNextColumn();
	if (field.nType == 0)
		ImGui::Text("%s", m_baseline[nRow] >= 0.5f ? "true" : "false");
	else if (field.nType == 1)
		ImGui::Text("%d", static_cast<int>(m_baseline[nRow]));
	else if (field.nType == 2)
		ImGui::Text("%g", static_cast<double>(m_baseline[nRow]));
	else
		ImGui::TextDisabled("n/a");

	ImGui::TableNextColumn();
	if (!bModified)
		ImGui::BeginDisabled();
	if (ImGui::Button("Reset"))
	{
		char szValue[64];
		if (field.nType == 0)
			V_snprintf(szValue, sizeof(szValue), "%d", m_baseline[nRow] >= 0.5f ? 1 : 0);
		else if (field.nType == 1)
			V_snprintf(szValue, sizeof(szValue), "%d", static_cast<int>(m_baseline[nRow]));
		else
			V_snprintf(szValue, sizeof(szValue), "%g", static_cast<double>(m_baseline[nRow]));
		Queue(field.pszName, szValue);
	}
	if (!bModified)
		ImGui::EndDisabled();

	ImGui::PopID();
}

void CClassVarLab::DrawGroup(const char* pszLabel, const std::vector<size_t>& rows, bool bForceOpen)
{
	if (bForceOpen)
		ImGui::SetNextItemOpen(true);
	else
		ImGui::SetNextItemOpen(false, ImGuiCond_FirstUseEver);

	if (!ImGui::CollapsingHeader(pszLabel))
		return;

	char szId[128];
	V_snprintf(szId, sizeof(szId), "##cvlab_%s", pszLabel);
	if (ImGui::BeginTable(szId, 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.35f);
		ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 70.0f);
		ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.35f);
		ImGui::TableSetupColumn("Default", ImGuiTableColumnFlags_WidthStretch, 0.2f);
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70.0f);
		ImGui::TableHeadersRow();

		for (size_t n : rows)
			DrawRow(n);

		ImGui::EndTable();
	}
}

void CClassVarLab::Draw(void)
{
	if (m_bNeedRefresh || ClassVar_SettingsRebuildSerial() != m_nSerial
		|| ClassVar_LocalBlock() != m_nBlock)
		Refresh();

	ConVar* const pCheats = g_pCVar ? g_pCVar->FindVar("sv_cheats") : nullptr;
	const bool bCheats = pCheats && pCheats->GetBool();
	const bool bPlayer = ClassVar_LocalBlock() != 0;

	int nModified = 0;
	for (size_t i = 0; i < m_fields.size(); i++)
	{
		if (IsModified(i))
			nModified++;
	}

	ImGui::Text("sv_cheats %d | local player %s | fields %d | modified %d | queued %d",
		bCheats ? 1 : 0, bPlayer ? "yes" : "no",
		static_cast<int>(m_fields.size()), nModified, static_cast<int>(m_queue.size()));

	if (!bCheats)
		ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.25f, 1.0f), "set is cheat-gated: enable sv_cheats on the server");

	if (!bCheats)
		ImGui::BeginDisabled();

	if (ImGui::Button("Moon Gravity"))
		Queue("gravityScale", "0.2");
	ImGui::SameLine();
	if (ImGui::Button("Low Gravity"))
		Queue("gravityScale", "0.5");
	ImGui::SameLine();
	if (ImGui::Button("Normal Gravity"))
		Queue("gravityScale", "1.0");
	ImGui::SameLine();
	if (ImGui::Button("Slide Lab"))
	{
		Queue("slideSpeedBoost", "3.0");
		Queue("slideRequiredStartSpeed", "50");
	}
	ImGui::SameLine();
	if (ImGui::Button("Wallrun Lab"))
	{
		Queue("wallrunMaxSpeedVertical", "600");
		Queue("wallrun_timeLimit", "30");
	}
	ImGui::SameLine();
	if (ImGui::Button("Floaty Air"))
	{
		Queue("gravityScale", "0.6");
		Queue("airSpeed", "500");
	}
	ImGui::SameLine();
	if (ImGui::Button("Long Dodge"))
	{
		Queue("dodgeSpeed", "600");
		Queue("dodgeDuration", "0.6");
	}
	ImGui::SameLine();
	if (ImGui::Button("Reset All"))
	{
		for (size_t i = 0; i < m_fields.size(); i++)
		{
			if (!IsModified(i))
				continue;
			const ClassVarField_t& field = m_fields[i];
			if (field.nType > 2)
				continue;
			char szValue[64];
			if (field.nType == 0)
				V_snprintf(szValue, sizeof(szValue), "%d", m_baseline[i] >= 0.5f ? 1 : 0);
			else if (field.nType == 1)
				V_snprintf(szValue, sizeof(szValue), "%d", static_cast<int>(m_baseline[i]));
			else
				V_snprintf(szValue, sizeof(szValue), "%g", static_cast<double>(m_baseline[i]));
			Queue(field.pszName, szValue);
		}
	}

	if (!bCheats)
		ImGui::EndDisabled();

	auto toLower = [](const string& s)
	{
		string out(s);
		std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		return out;
	};
	const string needle = toLower(string(m_szFilter));
	const bool bFiltering = m_szFilter[0] != '\0';

	ImGui::PushItemWidth(ImGui::GetFontSize() * 16);
	ImGui::InputTextWithHint("##cvlab_filter", "Filter fields...", m_szFilter, IM_ARRAYSIZE(m_szFilter));
	ImGui::PopItemWidth();
	ImGui::SameLine();
	ImGui::Checkbox("Modified only", &m_bModifiedOnly);
	ImGui::SameLine();
	ImGui::Checkbox("Show read-only", &m_bShowReadOnly);
	ImGui::SameLine();
	if (ImGui::Button("Refresh"))
		Refresh();
	ImGui::SameLine();
	ImGui::Checkbox("Apply to all players", &m_bApplyToAll);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Routes every edit through 'setall' -- host seat only, the dedi refuses anyone else.");

	if (!m_bProfilesScanned)
		ScanProfiles();

	if (!bCheats)
		ImGui::BeginDisabled();
	ImGui::PushItemWidth(ImGui::GetFontSize() * 12);
	ImGui::InputTextWithHint("##cvlab_profile", "Profile name", m_szProfile, IM_ARRAYSIZE(m_szProfile));
	ImGui::PopItemWidth();
	ImGui::SameLine();
	if (ImGui::Button("Save"))
		SaveProfile(m_szProfile);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Writes the modified fields to %s/<name>.csv (key,value per line).", CVLAB_PROFILE_DIR);
	ImGui::SameLine();
	if (ImGui::Button("Load"))
		LoadProfile(m_szProfile);
	ImGui::SameLine();
	ImGui::PushItemWidth(ImGui::GetFontSize() * 12);
	if (ImGui::BeginCombo("##cvlab_profiles", m_profiles.empty() ? "no saved profiles" : "saved profiles..."))
	{
		for (const string& name : m_profiles)
		{
			if (ImGui::Selectable(name.c_str()))
			{
				V_strncpy(m_szProfile, name.c_str(), sizeof(m_szProfile));
				LoadProfile(m_szProfile);
			}
		}
		ImGui::EndCombo();
	}
	ImGui::PopItemWidth();
	if (!bCheats)
		ImGui::EndDisabled();

	struct Group_t
	{
		string m_svPrefix;
		vector<size_t> m_rows;
		bool m_bModified;
	};

	vector<size_t> secondary;
	vector<Group_t> groups;
	for (size_t n : m_order)
	{
		const ClassVarField_t& field = m_fields[n];
		if (field.nType >= 3 && !m_bShowReadOnly)
			continue;
		if (m_bModifiedOnly && !IsModified(n))
			continue;
		if (bFiltering && toLower(field.pszName).find(needle) == string::npos)
			continue;
		if (field.bSecondary)
		{
			secondary.push_back(n);
			continue;
		}
		string svPrefix = ClassVarLab_Prefix(field.pszName);
		if (svPrefix.empty())
			svPrefix = "other";
		if (!groups.empty() && groups.back().m_svPrefix == svPrefix)
			groups.back().m_rows.push_back(n);
		else
		{
			Group_t group;
			group.m_svPrefix = svPrefix;
			group.m_rows.push_back(n);
			group.m_bModified = false;
			groups.push_back(group);
		}
	}

	Group_t other;
	other.m_svPrefix = "other";
	other.m_bModified = false;
	vector<Group_t> named;
	for (auto& group : groups)
	{
		if (group.m_rows.size() < 2)
		{
			for (size_t n : group.m_rows)
			{
				other.m_rows.push_back(n);
				if (IsModified(n))
					other.m_bModified = true;
			}
		}
		else
		{
			group.m_bModified = false;
			for (size_t n : group.m_rows)
			{
				if (IsModified(n))
				{
					group.m_bModified = true;
					break;
				}
			}
			named.push_back(group);
		}
	}

	if (!bCheats)
		ImGui::BeginDisabled();

	ImGui::BeginChild("##cvlab_child", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysUseWindowPadding);
	for (auto& group : named)
		DrawGroup(group.m_svPrefix.c_str(), group.m_rows, bFiltering || group.m_bModified);
	if (!other.m_rows.empty())
		DrawGroup("other", other.m_rows, bFiltering || other.m_bModified);
	if (!secondary.empty())
		DrawGroup("poseSettings (current pose)", secondary, bFiltering);
	ImGui::EndChild();

	if (!bCheats)
		ImGui::EndDisabled();
}

void CClassVarLab::RunFrame(void)
{
	ClassVar_BindShipped();

	if (m_queue.empty())
		return;

	const double flNow = Plat_FloatTime();
	if (flNow - m_flLastSend < CVLAB_SEND_INTERVAL)
		return;
	m_flLastSend = flNow;

	const string& line = m_queue.front();
	Cbuf_AddText(Cbuf_GetCurrentPlayer(), line.c_str(), cmd_source_t::kCommandSrcCode);
	Msg(eDLL_T::CLIENT, "[CVLAB] %s\n", line.c_str());
	m_queue.erase(m_queue.begin());
}

CClassVarLab g_ClassVarLab;
