#pragma once
//=============================================================================//
//
// Purpose: Movement Lab tab over the engine class-var system
//
//=============================================================================//
#ifndef DEDICATED
#include <cstdint>
#include <string>
#include <vector>
#include "game/client/classvar_natives.h"

class CClassVarLab
{
public:
	CClassVarLab();

	void Draw(void);
	void RunFrame(void);
private:
	void Refresh(void);
	void Queue(const char* pszKey, const char* pszValue);
	void ScanProfiles(void);
	bool SaveProfile(const char* pszName);
	bool LoadProfile(const char* pszName);
	float LiveValue(const ClassVarField_t& field) const;
	bool IsModified(size_t nRow) const;
	void DrawRow(size_t nRow);
	void DrawGroup(const char* pszLabel, const std::vector<size_t>& rows, bool bForceOpen);

	std::vector<ClassVarField_t> m_fields;
	std::vector<float> m_baseline;
	std::vector<float> m_scratch;
	std::vector<size_t> m_order;
	std::vector<std::string> m_queue;
	uint32_t m_nSerial;
	uintptr_t m_nBlock;
	double m_flLastSend;
	bool m_bNeedRefresh;
	bool m_bModifiedOnly;
	bool m_bShowReadOnly;
	bool m_bApplyToAll;
	bool m_bProfilesScanned;
	char m_szFilter[128];
	char m_szProfile[48];
	std::vector<std::string> m_profiles;
};

extern CClassVarLab g_ClassVarLab;
#endif // !DEDICATED
