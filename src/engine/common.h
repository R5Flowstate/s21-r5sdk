#pragma once

/* ==== COMMON ========================================================================================================================================================== */
inline void*(*v_COM_InitFilesystem)(const char* pFullModPath);
inline char* const(*v_COM_GetPrintMessageBuffer)(void);
inline void(*v_COM_ExplainDisconnection)(bool bPrint, const char* fmt, ...);
// Engine notify formatter 
inline __int64(__fastcall* v_COM_Notify)(void* a1, unsigned int a2, __int64 a3, const char* fmt, ...);
// Engine notify sink prints with "%c%s"
inline __int64(__fastcall* v_COM_NotifySink)(unsigned int a2, __int64 a3, const char* fmt, ...);

const char* COM_FormatSeconds(int seconds);
void COM_ExplainDisconnection(bool bPrint, const char* fmt, ...);
__int64 __fastcall H_COM_Notify(void* a1, unsigned int a2, __int64 a3, const char* fmt, ...);
///////////////////////////////////////////////////////////////////////////////
class VCommon : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("COM_InitFilesystem", v_COM_InitFilesystem);
		LogFunAdr("COM_GetPrintMessageBuffer", v_COM_GetPrintMessageBuffer);
		LogFunAdr("COM_ExplainDisconnection", v_COM_ExplainDisconnection);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 81 EC ?? ?? ?? ?? 48 8B F9 48 C7 44 24 ?? ?? ?? ?? ??").GetPtr(v_COM_InitFilesystem);
		Module_FindPattern(g_GameDll, "48 8D 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC 4C 89 44 24 ??").GetPtr(v_COM_GetPrintMessageBuffer);
		Module_FindPattern(g_GameDll, "48 8B C4 48 89 50 10 4C 89 40 18 4C 89 48 20 48 81 EC ?? ?? ?? ??").GetPtr(v_COM_ExplainDisconnection);
		// signature
		Module_FindPattern(g_GameDll, "4C 89 4C 24 20 53 55 56 57 48 81 EC 38 04 00 00 49 8B D9 48 8D AC 24 80 04 00 00 49 8B F8 8B F2").GetPtr(v_COM_Notify);
		// notify sink references "%c%s"
		CMemory fmt = Module_FindPattern(g_GameDll, "25 63 25 73 00"); // "%c%s\0"
		// walk xref back to function
		// NOTE: this is a best-effort; if it fails, v_COM_NotifySink stays null and hook becomes no-op
		if (fmt)
		{
			// try to find a nearby call in the notify function that uses this string
			// fallback: hard pattern the prologue of if needed.
			// For now leave it unresolved; we only use sink when detouring.
		}
	}
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////
