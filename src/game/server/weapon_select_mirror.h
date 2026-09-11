//=============================================================================//
//
// Purpose: Mirror server-side weapon activations into networked
// m_selectedWeapons so the S21 client follows script-driven gives.
//
//=============================================================================//
#ifndef WEAPON_SELECT_MIRROR_H
#define WEAPON_SELECT_MIRROR_H

#include "thirdparty/detours/include/idetour.h"

void WeaponSelectMirror_TickServer();
void WeapSelMirror_SetSuppress(bool bSuppress);
void WeaponSelectMirror_GetFun(void);
void WeaponSelectMirror_GetAdr(void);
void WeaponSelectMirror_Detour(const bool bAttach);

inline __int64(__fastcall* v_SetActiveWeapon)(void*, unsigned int, __int64) = nullptr;
inline __int16(__fastcall* v_WeaponDirectSelect)(void*, unsigned __int16) = nullptr;

#endif // WEAPON_SELECT_MIRROR_H
