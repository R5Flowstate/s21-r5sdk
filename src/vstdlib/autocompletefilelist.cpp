#if defined(CLIENT_DLL)
//===== Copyright © 1996-2005, Valve Corporation, All rights reserved. ======//
//
// Purpose
//
// $NoKeywords: $
//===========================================================================//
#include "core/stdafx.h"
#include "common/completion.h"
#include "autocompletefilelist.h"

// Forward decl -- definition below AutoCompletionFunc.
static bool _stricmp_substr(const char* haystack, const char* needle);

//-----------------------------------------------------------------------------
// S21 has no engine AutoCompletionFunc; Win32 FindFirstFile on <subdir>/*.<ext>.
//-----------------------------------------------------------------------------
int CBaseAutoCompleteFileList::AutoCompletionFunc(
	const char* partial,
	char commands[COMMAND_COMPLETION_MAXITEMS][COMMAND_COMPLETION_ITEM_LENGTH])
{
	if (!partial || !m_pszCommandName || !m_pszSubDir || !m_pszExtension)
		return 0;

	// Strip the command-name prefix from `partial`; engine may pass the full line or just the suffix.
	const char* prefix = partial;
	const size_t cmdLen = strlen(m_pszCommandName);
	if (cmdLen > 0 && _strnicmp(partial, m_pszCommandName, cmdLen) == 0)
	{
		prefix += cmdLen;
		while (*prefix == ' ' || *prefix == '\t') ++prefix;
	}
	const size_t prefixLen = strlen(prefix);

	// FindFirstFileA on "<subdir>/*.<ext>" against the process cwd.
	char searchPattern[MAX_PATH];
	_snprintf_s(searchPattern, sizeof(searchPattern), _TRUNCATE,
		"%s\\*.%s", m_pszSubDir, m_pszExtension);

	// Prefix match first; substring fill if prefix hits are under MAXITEMS.
	int total = 0;
	for (int pass = 0; pass < 2 && total < COMMAND_COMPLETION_MAXITEMS; ++pass)
	{
		if (pass == 1 && prefixLen == 0)
			break;

		WIN32_FIND_DATAA fd{};
		HANDLE hFind = FindFirstFileA(searchPattern, &fd);
		if (hFind == INVALID_HANDLE_VALUE)
			break;

		do
		{
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;

			const bool prefixMatch = (prefixLen == 0) ||
				(_strnicmp(fd.cFileName, prefix, prefixLen) == 0);
			const bool subStrMatch = (prefixLen > 0) && !prefixMatch &&
				_stricmp_substr(fd.cFileName, prefix);

			if ((pass == 0 && prefixMatch) || (pass == 1 && subStrMatch))
			{
				_snprintf_s(commands[total], COMMAND_COMPLETION_ITEM_LENGTH, _TRUNCATE,
					"%s %s", m_pszCommandName, fd.cFileName);
				commands[total][COMMAND_COMPLETION_ITEM_LENGTH - 1] = '\0';
				++total;
			}
		}
		while (total < COMMAND_COMPLETION_MAXITEMS && FindNextFileA(hFind, &fd));

		FindClose(hFind);
	}

	return total;
}

// Case-insensitive substring search. Returns true if `needle` is found
// anywhere inside `haystack`. Local helper to avoid pulling shlwapi.
static bool _stricmp_substr(const char* haystack, const char* needle)
{
	if (!haystack || !needle || !*needle) return false;
	const size_t hlen = strlen(haystack);
	const size_t nlen = strlen(needle);
	if (nlen > hlen) return false;
	for (size_t i = 0; i <= hlen - nlen; ++i)
		if (_strnicmp(haystack + i, needle, nlen) == 0)
			return true;
	return false;
}
#else // !CLIENT_DLL
//===== Copyright © 1996-2005, Valve Corporation, All rights reserved. ======//
//
// Purpose
//
// $NoKeywords: $
//===========================================================================//
#include "core/stdafx.h"
#include "common/completion.h"
#include "autocompletefilelist.h"

//-----------------------------------------------------------------------------
// Purpose: Fills in a list of commands based on specified subdirectory and extension into the format
// commandname subdir/filename.ext
// commandname subdir/filename2.ext
// Returns number of files in list for autocompletion
//-----------------------------------------------------------------------------
int CBaseAutoCompleteFileList::AutoCompletionFunc(const char* partial, char commands[COMMAND_COMPLETION_MAXITEMS][COMMAND_COMPLETION_ITEM_LENGTH])
{
	return CBaseAutoCompleteFileList__AutoCompletionFunc(this, partial, commands);
}
#endif // CLIENT_DLL
