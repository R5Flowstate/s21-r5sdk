//=============================================================================//
//
// Purpose: S21 command-line parse / CheckParm helpers.
// S21 does not export CCommandLine; this is the SDK's own ICommandLine.
//
//=============================================================================//

#include "tier0/commandline.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>

class CCommandLine_S21 : public ICommandLine
{
public:
	CCommandLine_S21()
	{
		m_pszCmdLine = nullptr;
		m_nParmCount = 0;
		memset(m_ppParms, 0, sizeof(m_ppParms));
	}

	void CreateCmdLine(const char* pszCommandline) override
	{
		if (m_pszCmdLine)
			free(m_pszCmdLine);

		m_pszCmdLine = _strdup(pszCommandline ? pszCommandline : "");
		m_nParmCount = 0;

		// Parse into tokens
		char* pCopy = _strdup(m_pszCmdLine);
		char* p = pCopy;
		while (*p && m_nParmCount < MAX_PARAMETERS)
		{
			// Skip whitespace
			while (*p == ' ' || *p == '\t') p++;
			if (!*p) break;

			char* start;
			if (*p == '"')
			{
				p++;
				start = p;
				while (*p && *p != '"') p++;
			}
			else
			{
				start = p;
				while (*p && *p != ' ' && *p != '\t') p++;
			}

			if (*p) *p++ = '\0';
			m_ppParms[m_nParmCount++] = _strdup(start);
		}
		free(pCopy);
	}

	void CreateCmdLine(int argc, char** argv) override
	{
		// Build command line from argv
		char buf[4096] = {};
		for (int i = 0; i < argc; i++)
		{
			if (i > 0) strcat_s(buf, " ");
			strcat_s(buf, argv[i]);
		}
		CreateCmdLine(buf);
	}

	void CreatePool(void* pMem) override {}

	const char* GetCmdLine(void) const override
	{
		return m_pszCmdLine ? m_pszCmdLine : "";
	}

	const char* CheckParm(const char* pszParm, const char** ppszValue = NULL) const override
	{
		for (int i = 0; i < m_nParmCount; i++)
		{
			if (_stricmp(m_ppParms[i], pszParm) == 0)
			{
				if (ppszValue && i + 1 < m_nParmCount)
					*ppszValue = m_ppParms[i + 1];
				return m_ppParms[i];
			}
		}
		return NULL;
	}

	void RemoveParm(const char* pszParm) override {}

	void AppendParm(const char* pszParm, const char* pszValues) override
	{
		if (m_nParmCount < MAX_PARAMETERS)
			m_ppParms[m_nParmCount++] = _strdup(pszParm);
		if (pszValues && pszValues[0] && m_nParmCount < MAX_PARAMETERS)
			m_ppParms[m_nParmCount++] = _strdup(pszValues);
	}

	const char* ParmValue(const char* pszParm, const char* pDefaultVal = NULL) const override
	{
		for (int i = 0; i < m_nParmCount - 1; i++)
		{
			if (_stricmp(m_ppParms[i], pszParm) == 0)
				return m_ppParms[i + 1];
		}
		return pDefaultVal;
	}

	int ParmValue(const char* pszParm, int nDefaultVal) const override
	{
		const char* v = ParmValue(pszParm, (const char*)NULL);
		return v ? atoi(v) : nDefaultVal;
	}

	float ParmValue(const char* pszParm, float flDefaultVal) const override
	{
		const char* v = ParmValue(pszParm, (const char*)NULL);
		return v ? (float)atof(v) : flDefaultVal;
	}

	int ParmCount(void) const override { return m_nParmCount; }

	int FindParm(const char* pszParm) const override
	{
		for (int i = 0; i < m_nParmCount; i++)
			if (_stricmp(m_ppParms[i], pszParm) == 0)
				return i;
		return 0;
	}

	const char* GetParm(int nIndex) const override
	{
		if (nIndex >= 0 && nIndex < m_nParmCount)
			return m_ppParms[nIndex];
		return "";
	}

	bool GuardLocked(void) const override { return false; }

	void SetParm(int nIndex, char const* pParm) override
	{
		if (nIndex >= 0 && nIndex < MAX_PARAMETERS)
		{
			if (m_ppParms[nIndex]) free(m_ppParms[nIndex]);
			m_ppParms[nIndex] = _strdup(pParm);
		}
	}

	void CleanUpParms(void) override
	{
		for (int i = 0; i < m_nParmCount; i++)
		{
			if (m_ppParms[i]) { free(m_ppParms[i]); m_ppParms[i] = nullptr; }
		}
		m_nParmCount = 0;
	}

private:
	enum { MAX_PARAMETERS = 256 };
	char* m_pszCmdLine;
	int m_nParmCount;
	char* m_ppParms[MAX_PARAMETERS];
};

///////////////////////////////////////////////////////////////////////////////
// Global instance
static CCommandLine_S21 s_CmdLine_S21;
CCommandLine* g_pCmdLine = reinterpret_cast<CCommandLine*>(&s_CmdLine_S21);
bool g_bCommandLineCreated = false;

void CommandLine_CreateFromProcess(const char* psz)
{
	s_CmdLine_S21.CreateCmdLine(psz ? psz : "");
	g_pCmdLine = reinterpret_cast<CCommandLine*>(&s_CmdLine_S21);
	g_bCommandLineCreated = true;
}
