//=============================================================================//
// Purpose: Load weapon KeyValues from loose platform/scripts/weapons/<name>.txt.
//=============================================================================//

#ifndef WEAPON_KV_DISK_S21_H
#define WEAPON_KV_DISK_S21_H

#include "thirdparty/detours/include/idetour.h"

// ReadKVWeaponFile(const char* weaponName) -> KVGroup_s* (opaque to us).
// Returns the packed weapon KVGroup, or NULL if the pak asset is missing.
inline void* (__fastcall *v_ReadKVWeaponFile_S21)(const char* weaponName) = nullptr;

// KeyValues::LoadFromFile_Internal. MS x64: rcx,rdx,r8,r9 then stack.
// char f(KeyValues* root, IBaseFileSystem* fs, const char* resourceName,
// __int64 forceSkipDFS, char flag, const char* pathID, void* pfnEval)
inline char (__fastcall *v_KV_LoadFromFile_S21)(
	void* root, void* fs, const char* resourceName,
	__int64 forceSkipDFS, char flag, const char* pathID, void* pfnEval) = nullptr;

// rtech HashName; KVValue keyHash uses the low 32 bits.
inline unsigned __int64 (__fastcall *v_KVHashAligned_S21)(const char* str) = nullptr;
inline unsigned __int64 (__fastcall *v_KVHashUnaligned_S21)(const char* str) = nullptr;

// rtech globalHeap slot; Alloc is the first qword of MemAllocator_s.
inline void** g_ppRTechGlobalHeap_S21 = nullptr;

// KeyValues symbol-table object address. GetSymbolString is
// vtable slot 4 (offset 0x20): const char* f(this=object, unsigned symbol).
inline void* g_pKVSymbolTable_S21 = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VWeaponKVDiskS21 : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("ReadKVWeaponFile_S21",        v_ReadKVWeaponFile_S21);
		LogFunAdr("KeyValues::LoadFromFile_S21", v_KV_LoadFromFile_S21);
		LogFunAdr("HashName::aligned_S21",        v_KVHashAligned_S21);
		LogFunAdr("HashName::unaligned_S21",      v_KVHashUnaligned_S21);
		LogVarAdr("RTechGlobalHeapSlot_S21",     g_ppRTechGlobalHeap_S21);
		LogVarAdr("KVSymbolTable_S21",            g_pKVSymbolTable_S21);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const {}
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // WEAPON_KV_DISK_S21_H
