#if defined(CLIENT_DLL)
#ifndef GAME_WEAPON_PARSE_H
#define GAME_WEAPON_PARSE_H

inline void (*WeaponParse_LoadServerData)(const bool parseScripts);
inline void (*WeaponParse_LoadClientData)(const bool parseScripts, const bool setupRumble);

///////////////////////////////////////////////////////////////////////////////
class V_Weapon_Parse : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("WeaponParse_LoadServerData", WeaponParse_LoadServerData);
		LogFunAdr("WeaponParse_LoadClientData", WeaponParse_LoadClientData);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "40 57 41 54 48 81 EC").GetPtr(WeaponParse_LoadServerData);
		Module_FindPattern(g_GameDll, "48 8B C4 88 50 ?? 48 81 EC").GetPtr(WeaponParse_LoadClientData);
	}
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const { };
};
///////////////////////////////////////////////////////////////////////////////

#endif // GAME_WEAPON_PARSE_H
#else // !CLIENT_DLL
#ifndef GAME_WEAPON_PARSE_H
#define GAME_WEAPON_PARSE_H

inline void (*WeaponParse_LoadServerData)(const bool parseScripts);
inline void (*WeaponParse_LoadClientData)(const bool parseScripts, const bool setupRumble);

///////////////////////////////////////////////////////////////////////////////
class V_Weapon_Parse : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("WeaponParse_LoadServerData", WeaponParse_LoadServerData);
		LogFunAdr("WeaponParse_LoadClientData", WeaponParse_LoadClientData);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "40 57 41 54 48 81 EC").GetPtr(WeaponParse_LoadServerData);
		Module_FindPattern(g_GameDll, "48 8B C4 88 50 ?? 48 81 EC").GetPtr(WeaponParse_LoadClientData);
	}
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const { };
};
///////////////////////////////////////////////////////////////////////////////

// S21 melee_anim_1p is a string; S3 schema only has melee_anim_1p_number (1/2/3).
// Inject the integer before the stock parser stores the var.
inline void (*v_WeaponSchemaParse)(__int64 a1, __int64 a2, char* a3, int a4);

///////////////////////////////////////////////////////////////////////////////
class V_WeaponMeleeAnimFix : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("WeaponSchemaParse", v_WeaponSchemaParse);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll,
			"48 89 6C 24 10 48 89 74 24 18 4C 89 4C 24 20 57 41 54 41 55 41 56 41 57 48 81 EC 80 00 00 00 4D 8B E0 44 0F 29 4C 24 40 4C 8B EA 4C 8B F1 49 8B CC 33 D2 41 B8 50 11 00 00")
			.GetPtr(v_WeaponSchemaParse);
	}
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // GAME_WEAPON_PARSE_H
#endif // CLIENT_DLL
