#if defined(CLIENT_DLL)
#pragma once
#include "public/gamebspfile.h"

struct GatherProps_t
{
	// TODO: reverse structure.
	int field_0;
	int field_4;
	int field_8;
	int field_C;
	_BYTE gap10[8];
	int field_18;
	int field_1C;
	int field_20;
	int field_24;
	int field_28;
	__int64 field_30;
	int field_38;
	int field_3C;
	__int64 field_40;
};

class CStaticProp
{
public:
	static void* Init(CStaticProp* thisptr, int64_t a2, unsigned int idx, unsigned int a4, StaticPropLump_t* lump, int64_t a6, int64_t a7, unsigned int a8);

private: // TODO: reverse structure.
};

inline void*(*CStaticProp__Init)(CStaticProp* thisptr, int64_t a2, unsigned int idx, unsigned int a4, StaticPropLump_t* lump, int64_t a6, int64_t a7, unsigned int a8);
inline void*(*v_GatherStaticPropsSecondPass_PreInit)(GatherProps_t* gather);
inline void* (*v_GatherStaticPropsSecondPass_PostInit)(GatherProps_t* gather);
inline __int64(*v_StaticPropInstanceSubmit)(unsigned int, __int64, int, __int64, int);

///////////////////////////////////////////////////////////////////////////////
class VStaticPropMgr : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CStaticProp::Init", CStaticProp__Init);
		LogFunAdr("StaticPropInstanceSubmit", v_StaticPropInstanceSubmit);

	}
	virtual void GetFun(void) const
	{
		// CStaticProp::Init -- resolved by signature, not a hardcoded RVA. The old

		// function lives at 0x3C4040, so the hardcoded offset hooked the WRONG
		// function -> garbage args (a7/modelData) -> AV. This prologue signature is
		// verified unique (n=1) on BOTH builds (DX11 / DX12 ).
		// CStaticProp::Init -- unique both DX11/DX12.
		Module_FindPattern(g_GameDll,
			"48 8B C4 48 89 58 10 44 89 40 18 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 98 48 81 EC 30 01 00 00 48 8B 9D 90 00 00 00")
			.GetPtr(CStaticProp__Init);
		// PreInit twin of PostInit: S21 ends with mov r12,rcx (4C 8B E1), not
		// mov r15,rcx (4C 8B F9) -- old pattern n=0 ( unique).
		Module_FindPattern(g_GameDll,
			"48 89 4C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
			"48 2B E0 48 8D 6C 24 ?? 48 8B 05 ?? ?? ?? ?? 4C 8B E1")
			.GetPtr(v_GatherStaticPropsSecondPass_PreInit);
		// PostInit -- mov r14,rcx.
		Module_FindPattern(g_GameDll,
			"48 89 4C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
			"48 2B E0 48 8D 6C 24 ?? 48 8B 05 ?? ?? ?? ?? 4C 8B F1")
			.GetPtr(v_GatherStaticPropsSecondPass_PostInit);
		// Instance submit: 64-byte records, material ptr at +0. The prologue is
		// unique up to the frame setup, which differs between DX11 and DX12.
		Module_FindPattern(g_GameDll,
			"48 8B C4 4C 89 48 20 44 89 40 18 48 89 50 10 89 48 08 "
			"55 53 56 57 41 54 41 55 41 56 41 57 48 8D")
			.GetPtr(v_StaticPropInstanceSubmit);
	}
	// Client-only: resolves g_pEngineTraceClient + the render-view camera vectors
	// used by the 'mat_staticprop' console command (defined in staticpropmgr.cpp).
	virtual void GetVar(void) const;
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////
#else // !CLIENT_DLL
#pragma once
#include "public/gamebspfile.h"

struct GatherProps_t
{
	// TODO: reverse structure.
	int field_0;
	int field_4;
	int field_8;
	int field_C;
	_BYTE gap10[8];
	int field_18;
	int field_1C;
	int field_20;
	int field_24;
	int field_28;
	__int64 field_30;
	int field_38;
	int field_3C;
	__int64 field_40;
};

class CStaticProp
{
public:
	static void* Init(CStaticProp* thisptr, int64_t a2, unsigned int idx, unsigned int a4, StaticPropLump_t* lump, int64_t a6, int64_t a7);

private: // TODO: reverse structure.
};

inline void*(*CStaticProp__Init)(CStaticProp* thisptr, int64_t a2, unsigned int idx, unsigned int a4, StaticPropLump_t* lump, int64_t a6, int64_t a7);
inline void*(*v_GatherStaticPropsSecondPass_PreInit)(GatherProps_t* gather);
inline void* (*v_GatherStaticPropsSecondPass_PostInit)(GatherProps_t* gather);

///////////////////////////////////////////////////////////////////////////////
class VStaticPropMgr : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CStaticProp::Init", CStaticProp__Init);

	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "48 8B C4 44 89 40 18 48 89 50 10 55").GetPtr(CStaticProp__Init);
		Module_FindPattern(g_GameDll, "48 89 4C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 2B E0 48 8D 6C 24 ?? 48 8B 05 ?? ?? ?? ?? 4C 8B F9").GetPtr(v_GatherStaticPropsSecondPass_PreInit);
		Module_FindPattern(g_GameDll, "48 89 4C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 2B E0 48 8D 6C 24 ?? 48 8B 05 ?? ?? ?? ?? 4C 8B F1").GetPtr(v_GatherStaticPropsSecondPass_PostInit);
	}
	virtual void GetVar(void) const
	{

	}
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////
#endif // CLIENT_DLL
