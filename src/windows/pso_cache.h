//=============================================================================//
//
// Purpose: Mid-run serialize of the S21 DX12 pipeline library to psoCache.pso.
//
//=============================================================================//
#ifndef PSO_CACHE_S21_H
#define PSO_CACHE_S21_H
#ifndef DEDICATED

#include "thirdparty/detours/include/idetour.h"

struct ID3D12PipelineLibrary;

//-----------------------------------------------------------------------------
// Engine PSO cache state block. The base is the ID3D12PipelineLibrary slot;
// the engine's "psoCache.pso" name buffer sits kPsoCacheNameOffset below it.
//-----------------------------------------------------------------------------
struct PsoCacheCtx_t
{
	ID3D12PipelineLibrary* m_pLibrary;  // +0x00
	void* m_SRWLock;                    // +0x08, SRWLOCK
	void* m_pLoadedBlob;                // +0x10
	uint32_t m_nMaxCacheMB;             // +0x18, mirrors pso_max_cache_size_MB
	uint8_t m_bDirty;                   // +0x1C, set by every StorePipeline
};

static constexpr ptrdiff_t kPsoCacheNameOffset = -0x40;

// First dword of the file; the engine refuses a blob without it.
static constexpr uint32_t kPsoCacheMagic = 0x0011000F;

// Builds "<Saved Games>/Respawn/<profile>/local" honouring -fnf and the loader's
// Apex_r5f retarget. nWhich: 0 = local, 1 = profile, 2 = savegames.
inline bool(__fastcall* v_PsoCache_GetUserDir)(char* pszBuf, int nSize, int nWhich) = nullptr;

// Engine WriteWholeFile. Hooked so psoCache.pso never takes the .deleteme path.
inline bool(__fastcall* v_PsoCache_WriteFile)(const char* pszPath, uint8_t nLogChannel, const void* pData, size_t nSize) = nullptr;

inline PsoCacheCtx_t* g_pPsoCacheCtx = nullptr;
inline uint8_t* g_pPsoCacheEnabled = nullptr;   // cleared by -no_pso_caching / -renderdoc
inline volatile long* g_pPsoCreateCount = nullptr;
inline volatile long* g_pPsoCreateMs = nullptr;

// Graphics create + library Load/Store (2nd arg is the cache ctx).
inline void* (__fastcall* v_PsoCreateGraphics)(void* pA1, void* pCtx) = nullptr;
// Compute create + library Load/Store.
inline void* (__fastcall* v_PsoCreateCompute)(void* pThis) = nullptr;

void PsoCache_Frame(void);
bool PsoCache_Flush(const bool bForce);
void PsoCache_PrintStatus(void);

///////////////////////////////////////////////////////////////////////////////
class VPsoCacheS21 : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("PsoCache_GetUserDir", v_PsoCache_GetUserDir);
		LogFunAdr("PsoCache_WriteFile", v_PsoCache_WriteFile);
		LogFunAdr("PsoCreateGraphics", v_PsoCreateGraphics);
		LogFunAdr("PsoCreateCompute", v_PsoCreateCompute);
		LogVarAdr("PsoCacheCtx", g_pPsoCacheCtx);
		LogVarAdr("PsoCacheEnabled", g_pPsoCacheEnabled);
		LogVarAdr("PsoCreateCount", const_cast<const long*>(g_pPsoCreateCount));
		LogVarAdr("PsoCreateMs", const_cast<const long*>(g_pPsoCreateMs));
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // !DEDICATED
#endif // PSO_CACHE_S21_H
