#ifndef SOUND_BRIDGE_H
#define SOUND_BRIDGE_H
//=============================================================================//
//
// Purpose: Server sound-event broadcast bridge
//
//=============================================================================//
#pragma once
#include "thirdparty/detours/include/idetour.h"

class CBaseEntity;

//-----------------------------------------------------------------------------
// Purpose: CSOMET cache is empty on dedicated, so every EmitSound* skips
// send. A synthetic record lets CTESoundDispatch through.
struct SoundBridgeMilesRecord_t
{
	uint64_t m_nMilesHash;
	uint32_t m_nNextChainIndex;
	float    m_flMaxAudibleDist;  // record+0x0C, 0 = the engine skips distance culling
	float    m_flSoundClass;      // record+0x10, 0<v<5 = one filter-wide dispatch
	uint32_t m_nUnused;
};

inline void* (*v_SoundBridge_MilesEventLookup)(const char* pszName, uint64_t* pOutHash, int* pOutSoundClass) = nullptr;
inline __m128(*v_EmitSoundOnEntity)(CBaseEntity* pEntity, const char* pszSoundName) = nullptr;
inline char (*v_SoundBridge_FilterPopulate)(void* pFilter, const float* pOrigin, void* pRecord) = nullptr;
inline void (*v_SoundBridge_RemoveRecipient)(void* pFilter, int nEdictSlot) = nullptr;

void* Hook_SoundBridge_MilesEventLookup(const char* pszName, uint64_t* pOutHash, int* pOutSoundClass);
__m128 Hook_EmitSoundOnEntity(CBaseEntity* pEntity, const char* pszSoundName);
char Hook_SoundBridge_FilterPopulate(void* pFilter, const float* pOrigin, void* pRecord);

// Remove these edicts from the next recipient list the filter builds; consumed on first populate.
void SoundBridge_ArmRecipientExcludes(const int* pEdictSlots, int nCount);
void SoundBridge_DisarmRecipientExcludes(void);

// 64-bit FNV-1a Miles event hash. Multiply-then-XOR (not textbook XOR-then-multiply).
// 'A'-'Z' -> lowercase, '.' -> '_'.
uint64_t SoundBridge_ComputeMilesHash(const char* pszName);

///////////////////////////////////////////////////////////////////////////////
class VSoundBridge : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("MilesEventLookup", v_SoundBridge_MilesEventLookup);
		LogFunAdr("EmitSoundOnEntity", v_EmitSoundOnEntity);
		LogFunAdr("SoundBridge_FilterPopulate", v_SoundBridge_FilterPopulate);
		LogFunAdr("SoundBridge_RemoveRecipient", v_SoundBridge_RemoveRecipient);
	}
	virtual void GetFun(void) const
	{
		// Anchored on the call site inside EmitSoundOnEntity. A sibling probe
		// shares the prologue; both emit sites resolve to the same target.
		Module_FindPattern(g_GameDll, "E8 ?? ?? ?? ?? 4C 8B C0 48 8D 4D F7 48 8D 55 E7 E8")
			.FollowNearCallSelf()
			.GetPtr(v_SoundBridge_MilesEventLookup);

		// Guard against landing on the sibling: only the real probe opens with
		// `sub rsp, 8; movzx r9d, [rcx]; mov r10, FNV1a-64 basis`.
		if (v_SoundBridge_MilesEventLookup && !CMemory(reinterpret_cast<const void*>(v_SoundBridge_MilesEventLookup)).CheckOpCodes(
			{ 0x48, 0x83, 0xEC, 0x08, 0x44, 0x0F, 0xB6, 0x09, 0x49, 0xBA, 0x25, 0x23, 0x22, 0x84, 0xE4, 0x9C, 0xF2, 0xCB }))
		{
			Warning(eDLL_T::SERVER, "[SOUND-BRIDGE] MilesEventLookup resolved to a non-matching prologue at %p -- "
				"CSOMET bypass disabled\n", reinterpret_cast<void*>(v_SoundBridge_MilesEventLookup));
			v_SoundBridge_MilesEventLookup = nullptr;
		}

		if (!v_SoundBridge_MilesEventLookup)
			Warning(eDLL_T::SERVER, "[SOUND-BRIDGE] MilesEventLookup pattern unresolved -- "
				"every server sound stays silent (S21 CSOMET gate)\n");

		Module_FindPattern(g_GameDll,
			"48 89 74 24 18 48 89 7C 24 20 55 48 8D 6C 24 A9 48 81 EC A0 00 00 00 48 8B FA 48 8B F1")
			.GetPtr(v_EmitSoundOnEntity);

		if (!v_EmitSoundOnEntity)
			Warning(eDLL_T::SERVER, "[SOUND-BRIDGE] EmitSoundOnEntity pattern unresolved -- "
				"server-authoritative sound broadcast disabled\n");

		// Shared recipient-list builder for the whole EmitSound* family.
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 18 48 89 6C 24 20 57 48 83 EC 70 49 8B D8 48 8B EA 48 8B F9 E8")
			.GetPtr(v_SoundBridge_FilterPopulate);

		if (!v_SoundBridge_FilterPopulate)
			Warning(eDLL_T::SERVER, "[SOUND-BRIDGE] FilterPopulate pattern unresolved -- "
				"EmitSoundOnEntityExceptToPlayers multi-exclude disabled\n");

		Module_FindPattern(g_GameDll,
			"40 53 48 83 EC 20 4C 63 49 28 33 C0 48 8B D9 4D 85 C9 74 66 48 8B 49 10")
			.GetPtr(v_SoundBridge_RemoveRecipient);

		if (!v_SoundBridge_RemoveRecipient)
			Warning(eDLL_T::SERVER, "[SOUND-BRIDGE] RemoveRecipient pattern unresolved -- "
				"EmitSoundOnEntityExceptToPlayers multi-exclude disabled\n");
	}
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const
	{
#ifndef CLIENT_DLL
		if (v_SoundBridge_MilesEventLookup)
			DetourSetup(&v_SoundBridge_MilesEventLookup, &Hook_SoundBridge_MilesEventLookup, bAttach);

		if (v_EmitSoundOnEntity)
			DetourSetup(&v_EmitSoundOnEntity, &Hook_EmitSoundOnEntity, bAttach);

		if (v_SoundBridge_FilterPopulate)
			DetourSetup(&v_SoundBridge_FilterPopulate, &Hook_SoundBridge_FilterPopulate, bAttach);
#endif // !CLIENT_DLL
	}
};
///////////////////////////////////////////////////////////////////////////////

#endif // SOUND_BRIDGE_H
