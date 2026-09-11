//=====================================================================================//
//
// Purpose
//
//=====================================================================================//

#include <core/stdafx.h>
#include <tier1/strtools.h>
#include <localize/localize.h>
#include <engine/common.h>

/*
==============================
COM_FormatSeconds

==============================
*/
const char* COM_FormatSeconds(int seconds)
{
	static char string[64];

	int hours = 0;
	int minutes = seconds / 60;

	if (minutes > 0)
	{
		seconds -= (minutes * 60);
		hours = minutes / 60;

		if (hours > 0)
		{
			minutes -= (hours * 60);
		}
	}

	if (hours > 0)
	{
		Q_snprintf(string, sizeof(string), "%2i:%02i:%02i", hours, minutes, seconds);
	}
	else
	{
		Q_snprintf(string, sizeof(string), "%02i:%02i", minutes, seconds);
	}

	return string;
}

/*
==============================
COM_ExplainDisconnection

==============================
*/
void COM_ExplainDisconnection(bool bPrint, const char* fmt, ...)
{
	char szBuf[1024];
	{/////////////////////////////
		va_list vArgs;
		va_start(vArgs, fmt);

		const int ret = V_vsnprintf(szBuf, sizeof(szBuf), fmt, vArgs);

		if (ret < 0)
			szBuf[0] = '\0';

		va_end(vArgs);
	}/////////////////////////////

	// Strip trailing control characters (CR/LF etc.) so UI won't render squares
	{
		size_t n = strlen(szBuf);
		while (n > 0)
		{
			unsigned char c = (unsigned char)szBuf[n - 1];
			if (c >= 32 && c != 0x7F) break;
			szBuf[--n] = '\0';
		}
	}

	if (bPrint)
	{
		if (szBuf[0] == '#' && g_ppVGuiLocalize && *g_ppVGuiLocalize)
		{
			wchar_t formatStr[1024];
			const wchar_t* wpchReason = (*g_ppVGuiLocalize)->Find(szBuf);
			if (wpchReason)
			{
				wcsncpy(formatStr, wpchReason, sizeof(formatStr) / sizeof(wchar_t));

				char conStr[256];
				(*g_ppVGuiLocalize)->ConvertUnicodeToANSI(formatStr, conStr, sizeof(conStr));
				Error(eDLL_T::CLIENT, NO_ERROR, "%s\n", conStr);
			}
			else
				Error(eDLL_T::CLIENT, NO_ERROR, "%s\n", szBuf);
		}
		else
		{
			Error(eDLL_T::CLIENT, NO_ERROR, "%s\n", szBuf);
		}
	}

	v_COM_ExplainDisconnection(bPrint, "%s", szBuf);
}

#ifndef CLIENT_DLL
void VCommon::Detour(const bool bAttach) const
{
	DetourSetup(&v_COM_ExplainDisconnection, COM_ExplainDisconnection, bAttach);
	if (v_COM_Notify)
	{
		DetourSetup(&v_COM_Notify, H_COM_Notify, bAttach);
	}
}
#endif // !CLIENT_DLL

__int64 __fastcall H_COM_Notify(void* a1, unsigned int a2, __int64 a3, const char* fmt, ...)
{
    if (!v_COM_Notify) return 0; // safety

    char buffer[1024]; buffer[0] = '\0';
    va_list va; va_start(va, fmt);
    _vsnprintf_s(buffer, _countof(buffer), _TRUNCATE, fmt, va);
    va_end(va);

    // strip trailing control chars
    size_t n = strlen(buffer);
    while (n > 0)
    {
        unsigned char c = (unsigned char)buffer[n - 1];
        if (c >= 32 && c != 0x7F) break;
        buffer[--n] = '\0';
    }

    if (v_COM_NotifySink)
    {
        return v_COM_NotifySink(a2, a3, "%c%s", 75, buffer);
    }
    // Fallback: call original to preserve behavior
    return v_COM_Notify(a1, a2, a3, "%s", buffer);
}
