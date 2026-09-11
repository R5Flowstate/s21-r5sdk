#pragma once

/* ==== CMATSYSTEMSURFACE =============================================================================================================================================== */
inline void(*CMatSystemSurface__DrawColoredText)(void* thisptr, short font, int fontHeight, int offsetX, int offsetY, int red, int green, int blue, int alpha, const char* fmt, ...);
inline void(*CMatSystemSurface__DrawColoredTextInternal)(void* thisptr, short font, int fontHeight, int offsetX, int offsetY, int red, int green, int blue, int alpha, int unk1, const char* fmt, va_list argptr);

void MatSystemSurface_DrawColoredText(void* thisptr, short font, int fontHeight, int offsetX, int offsetY, int red, int green, int blue, int alpha, const char* fmt, ...);

class CMatSystemSurface
{
};

inline CMatSystemSurface* g_pMatSystemSurface;
inline CMatSystemSurface* g_pVGuiSurface;

///////////////////////////////////////////////////////////////////////////////
class VMatSystemSurface : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CMatSystemSurface::DrawColoredText", CMatSystemSurface__DrawColoredText);
		LogFunAdr("CMatSystemSurface::DrawColoredTextInternal", CMatSystemSurface__DrawColoredTextInternal);
		LogVarAdr("g_pMatSystemSurface", g_pMatSystemSurface);
		LogVarAdr("g_pVGuiSurface", g_pVGuiSurface);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "4C 8B DC 48 83 EC 68 49 8D 43 58 0F 57 C0").GetPtr(CMatSystemSurface__DrawColoredText);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 55 41 56 41 57 48 8D AC 24 ? ? ? ? B8 ? ? ? ? E8 ? ? ? ? 48 2B E0 48 8B F9").GetPtr(CMatSystemSurface__DrawColoredTextInternal);
	}
	virtual void GetVar(void) const
	{
#if defined(CLIENT_DLL)
		const CMemory paint(Module_FindPattern(g_GameDll, "48 8B C4 89 50 10 48 89 48 08 55 56 57 48 8D 68 A1 48 81 EC C0 00 00 00"));
		g_pVGuiSurface = paint.Offset(0xB0).FindPattern("48 8B 0D", CMemory::Direction::DOWN, 32).ResolveRelativeAddress(0x3, 0x7).RCast<CMatSystemSurface*>();
		g_pMatSystemSurface = g_pVGuiSurface;
#else
		g_pMatSystemSurface = Module_FindPattern(g_GameDll, "48 83 EC 28 48 83 3D ?? ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ??")
			.FindPatternSelf("48 83 3D", CMemory::Direction::DOWN, 40).ResolveRelativeAddressSelf(0x3, 0x8).RCast<CMatSystemSurface*>();

		g_pVGuiSurface = Module_FindPattern(g_GameDll, "48 8B 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC 48 8B 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC 8B 81 ?? ?? ?? ??")
			.ResolveRelativeAddressSelf(0x3, 0x7).RCast<CMatSystemSurface*>();
#endif // CLIENT_DLL
	}
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const { }
};
///////////////////////////////////////////////////////////////////////////////
