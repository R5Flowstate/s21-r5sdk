//=============================================================================//
//
// Purpose: player class-var natives, client half
//
//=============================================================================//
#ifndef GAME_CLIENT_CLASSVAR_NATIVES_CL_H
#define GAME_CLIENT_CLASSVAR_NATIVES_CL_H

#include "thirdparty/detours/include/idetour.h"
#include <cstddef>
#include <cstdint>

class CSquirrelVM;

void ClassVar_RegisterClientFunctions(CSquirrelVM* s);
void ClassVar_BindShipped(void);

// Local player entity, or null when no slot is bound. Inert until the class-var surface resolves.
void* ClassVar_LocalPlayer(void);

// Live address of a field on the local player, or 0 (no player / no block).
struct ClassVarField_t
{
	const char* pszName;
	uint16_t nType;
	uint32_t nOffset;
	bool bSecondary;
};

int ClassVar_EnumFields(ClassVarField_t* pOut, int nMax);
uintptr_t ClassVar_FieldAddress(const ClassVarField_t& field);
uint32_t ClassVar_SettingsRebuildSerial(void);
uintptr_t ClassVar_LocalBlock(void);
const char* ClassVar_FormatValue(uintptr_t nAddr, uint16_t nType, char* pszBuf, size_t nBufLen);

// Returns true when the player's settings changed and the block was rebuilt.
inline bool(__fastcall* v_C_Player_ApplySettingsChange)(void*) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VClassVarNativesCl : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("C_Player_ApplySettingsChange", v_C_Player_ApplySettingsChange);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // GAME_CLIENT_CLASSVAR_NATIVES_CL_H
