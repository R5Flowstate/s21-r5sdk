//=============================================================================//
//
// Purpose: General system utilities.
//
//=============================================================================//

#include "core/stdafx.h"
#include "tier0/commandline.h"
#include "tier1/cvar.h"
#include "engine/sys_utils.h"
#ifdef DEDICATED
#include "engine/server/sv_rcon.h"
#else
#include "vgui/vgui_debugpanel.h"
#endif // !DEDICATED

#if !defined( _X360 )
#define	MAXPRINTMSG	4096
#else
#define	MAXPRINTMSG	1024
#endif

//-----------------------------------------------------------------------------
// Purpose: Show error in the console
// Input: *error - 
//			... - 
// Output: void _Error
//-----------------------------------------------------------------------------
void _Error(const char* fmt, ...)
{
	char buf[4096];
	bool shouldNewline = true;
	{/////////////////////////////
		va_list args;
		va_start(args, fmt);

		const int len = V_vsnprintf(buf, sizeof(buf), fmt, args);

		if (len < 0)
			buf[0] = '\0';
		else
			shouldNewline = buf[len - 1] != '\n';

		va_end(args);
	}/////////////////////////////

	Error(eDLL_T::ENGINE, NO_ERROR, shouldNewline ? "%s\n" : "%s", buf);

	// Pattern may be unresolved on this build; the SDK log above already ran.
	if (!v_Error)
	{
		static bool s_warned = false;
		if (!s_warned)
		{
			s_warned = true;
			Warning(eDLL_T::ENGINE, "[SYS_UTILS] engine Error unresolved -- not forwarding\n");
		}
		return;
	}
	v_Error("%s", buf);
}

//-----------------------------------------------------------------------------
// Purpose: Show warning in the console, exit engine with error when level 5
// Input: level -
// *error -... - 
// Output: void* _Warning
//-----------------------------------------------------------------------------
void _Warning(int level, const char* fmt, ...)
{
	char buf[10000];
	bool shouldNewline = true;
	{/////////////////////////////
		va_list args;
		va_start(args, fmt);

		const int len = V_vsnprintf(buf, sizeof(buf), fmt, args);

		if (len < 0)
			buf[0] = '\0';
		else
			shouldNewline = buf[len - 1] != '\n';

		va_end(args);
	}/////////////////////////////

	if (level < 5)
	{
		Warning(eDLL_T::ENGINE, shouldNewline ? "Warning(%d):%s\n" : "Warning(%d):%s", level, buf);
	}

	// Pattern may be unresolved on this build; the SDK log above already ran.
	if (!v_Warning)
	{
		static bool s_warned = false;
		if (!s_warned)
		{
			s_warned = true;
			Warning(eDLL_T::ENGINE, "[SYS_UTILS] engine Warning unresolved -- not forwarding\n");
		}
		return;
	}
	v_Warning(level, "%s", buf);
}

#ifndef DEDICATED
//-----------------------------------------------------------------------------
// Purpose: Builds log to be displayed on the screen
// Input: pos - 
// *fmt -... - 
// Output: void NPrintf
//-----------------------------------------------------------------------------
void _Con_NPrintf(int pos, const char* fmt, ...)
{
	char buf[MAXPRINTMSG];
	{/////////////////////////////
		va_list args;
		va_start(args, fmt);

		const int ret = V_vsnprintf(buf, sizeof(buf), fmt, args);

		if (ret < 0)
			buf[0] = '\0';

		va_end(args);
	}/////////////////////////////

	g_TextOverlay.m_nCon_NPrintf_Idx = pos;
	snprintf(g_TextOverlay.m_szCon_NPrintf_Buf,
		sizeof(g_TextOverlay.m_szCon_NPrintf_Buf), "%s", buf);
}
#endif // !DEDICATED

//-----------------------------------------------------------------------------
// Purpose: Gets the process up time (input buffer should be at least 4096 bytes in size)
// Input: *szBuffer - 
// Output: snprintf_s ret val
//-----------------------------------------------------------------------------
int Sys_GetProcessUpTime(char* szBuffer)
{
	return v_Sys_GetProcessUpTime(szBuffer);
}

//-----------------------------------------------------------------------------
// Purpose: Gets the build string of the game (defined in build.txt), if the file
// is absent, the changelist # will be returned instead
//-----------------------------------------------------------------------------
const char* Sys_GetBuildString(void)
{
	return v_Sys_GetBuildString();
}

//-----------------------------------------------------------------------------
// Purpose: Gets the platform string
//-----------------------------------------------------------------------------
const char* Sys_GetPlatformString(void)
{
	return "PC";
}

#ifndef CLIENT_DLL
void VSys_Utils::Detour(const bool bAttach) const
{
	DetourSetup(&v_Error, &_Error, bAttach);
	DetourSetup(&v_Warning, &_Warning, bAttach);
}
#endif // !CLIENT_DLL
