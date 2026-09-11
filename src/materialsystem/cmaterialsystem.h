#if defined(CLIENT_DLL)
#ifndef MATERIALSYSTEM_H
#define MATERIALSYSTEM_H
#include "cmaterialglue.h"
#include "cmatrendercontext.h"
#include "public/materialsystem/imaterialsystem.h"

class CMaterialSystem
{
public:
	static bool Connect(CMaterialSystem* thisptr, const CreateInterfaceFn factory);
	static void Disconnect(CMaterialSystem* thisptr);

	static InitReturnVal_t Init(CMaterialSystem* thisptr);
	static int Shutdown(CMaterialSystem* thisptr);
#ifndef MATERIALSYSTEM_NODX
	static void* SwapBuffers(CMaterialSystem* pMatSys);
	static CMaterialGlue* FindMaterialEx(CMaterialSystem* pMatSys, const char* pMaterialName, uint8_t nMaterialType, int nUnk, bool bComplain);
	static Vector2D GetScreenSize(CMaterialSystem* pMatSys = nullptr);

	static void CreditModelTextures(CMaterialSystem* const pMatSys, CMaterialGlue* const materialGlue, __int64 a3, __int64 a4, unsigned int a5, const Vector3D* const pViewOrigin, const float tanOfHalfFov, const float viewWidthPixels, int a9);
	static void UpdateStreamCamera(CMaterialSystem* const pMatSys, const Vector3D* const camPos, const QAngle* const camAng, const float halfFovX, const float viewWidth);
#endif // !MATERIALSYSTEM_NODX

	// TODO: reverse the vftable!
	inline int GetCurrentFrameCount()
	{
		const static int index = 74;
		return CallVFunc<int>(index, this);
	}

	inline CMatRenderContext* GetRenderContext()
	{
		// dx11 slot 106; dx12 native DrawAllOverlays/RenderLine call +912 = 114.
		const static int index = SDK_IsDx12Exe() ? 114 : 106;
		return CallVFunc<CMatRenderContext*>(index, this);
	}
};

#ifndef MATERIALSYSTEM_NODX
class CMaterialDeviceMgr
{
public:
	inline const MaterialAdapterInfo_t& GetAdapterInfo(int nIndex) const
	{
		Assert(nIndex >= 0 && nIndex < SDK_ARRAYSIZE(m_AdapterInfo));
		return m_AdapterInfo[nIndex];
	}
	inline const MaterialAdapterInfo_t& GetAdapterInfo() const
	{
		// Retrieve info of the selected adapter.
		return GetAdapterInfo(m_SelectedAdapter);
	}

private:
	enum
	{
		MAX_ADAPTER_COUNT = 4
	};

	IDXGIAdapter* m_Adapters[MAX_ADAPTER_COUNT];
	void* m_pUnknown1[MAX_ADAPTER_COUNT];
	MaterialAdapterInfo_t m_AdapterInfo[MAX_ADAPTER_COUNT];
	size_t m_AdapterMemorySize[MAX_ADAPTER_COUNT];
	int m_NumDisplayAdaptersProcessed;
	int m_SelectedAdapter;
	int m_NumDisplayAdapters;
};

inline CMaterialDeviceMgr* g_pMaterialAdapterMgr = nullptr;
#endif // !MATERIALSYSTEM_NODX

/* ==== MATERIALSYSTEM ================================================================================================================================================== */
inline InitReturnVal_t(*CMaterialSystem__Init)(CMaterialSystem* thisptr);
inline int(*CMaterialSystem__Shutdown)(CMaterialSystem* thisptr);

inline bool(*CMaterialSystem__Connect)(CMaterialSystem*, const CreateInterfaceFn);
inline void(*CMaterialSystem__Disconnect)(CMaterialSystem*);

inline CMaterialSystem* g_pMaterialSystem = nullptr;
inline void* g_pMaterialVFTable = nullptr;
#ifndef MATERIALSYSTEM_NODX
inline void*(*CMaterialSystem__SwapBuffers)(CMaterialSystem* pMatSys);

inline CMaterialGlue*(*CMaterialSystem__FindMaterialEx)(CMaterialSystem* pMatSys, const char* pMaterialName, uint8_t nMaterialType, int nUnk, bool bComplain);
inline void(*CMaterialSystem__GetScreenSize)(CMaterialSystem* pMatSys, float* outX, float* outY);

// Per-type error material array. A miss returns (*g_ppErrorMaterials)[type].
inline CMaterialGlue*** g_ppErrorMaterials = nullptr;

inline void(*CMaterialSystem__CreditModelTextures)(CMaterialSystem* const pMatSys, CMaterialGlue* const materialGlue, __int64 a3, __int64 a4, unsigned int a5, const Vector3D* const pViewOrigin, const float tanOfHalfFov, const float viewWidthPixels, int a9);
inline void(*CMaterialSystem__UpdateStreamCamera)(CMaterialSystem* const pMatSys, const Vector3D* const camPos, const QAngle* const camAng, const float halfFovX, const float viewWidth);

inline void*(*v_DispatchDrawCall)(int64_t a1, uint64_t a2, int a3, int a4, int64_t a5, int a6, uint8_t a7, int64_t a8, uint32_t a9, uint32_t a10, int a11, __m128* a12, int a13, int64_t a14);
inline ssize_t(*v_SpinPresent)(void);

inline int64_t(*v_TextureStreamMgr_ProcessGPUFeedback)(const unsigned int nSlot);
inline void(*v_Material_UnregisterStreaming)(void* const pMaterial);
inline void(*v_TextureStreamMgr_CreditFeedbackEntry)(void* const pHandleBase, int64_t* const pEntry);

// id -> registered material, indexed by the value the GPU reports.
inline uint8_t** g_ppStreamingMaterialById = nullptr;

int64_t TextureStreamMgr_ProcessGPUFeedback(const unsigned int nSlot);
void TextureStreamMgr_CreditFeedbackEntry(void* const pHandleBase, int64_t* const pEntry);
void Material_UnregisterStreaming(void* const pMaterial);
#endif // !MATERIALSYSTEM_NODX

#ifndef MATERIALSYSTEM_NODX
inline void** s_pRenderContext; // NOTE: This is some CMaterial instance or array.
#endif // !MATERIALSYSTEM_NODX

// TODO: move to materialsystem_global.h!
// TODO: reverse the vftable!
inline CMaterialSystem* MaterialSystem()
{
	return g_pMaterialSystem;
}

///////////////////////////////////////////////////////////////////////////////
class VMaterialSystem : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogConAdr("CMaterial::`vftable'", g_pMaterialVFTable);
		LogFunAdr("CMaterialSystem::Init", CMaterialSystem__Init);
		LogFunAdr("CMaterialSystem::Shutdown", CMaterialSystem__Shutdown);
		LogFunAdr("CMaterialSystem::Connect", CMaterialSystem__Connect);
		LogFunAdr("CMaterialSystem::Disconnect", CMaterialSystem__Disconnect);
#ifndef MATERIALSYSTEM_NODX
		LogFunAdr("CMaterialSystem::SwapBuffers", CMaterialSystem__SwapBuffers);
		LogFunAdr("CMaterialSystem::FindMaterialEx", CMaterialSystem__FindMaterialEx);
		LogFunAdr("CMaterialSystem::GetScreenSize", CMaterialSystem__GetScreenSize);
		LogFunAdr("CMaterialSystem::CreditModelTextures", CMaterialSystem__CreditModelTextures);
		LogFunAdr("CMaterialSystem::UpdateStreamCamera", CMaterialSystem__UpdateStreamCamera);
		LogFunAdr("DispatchDrawCall", v_DispatchDrawCall);
		LogFunAdr("SpinPresent", v_SpinPresent);
#endif // !MATERIALSYSTEM_NODX

#ifndef MATERIALSYSTEM_NODX
		LogVarAdr("s_pRenderContext", s_pRenderContext);
		LogVarAdr("g_MaterialAdapterMgr", g_pMaterialAdapterMgr);
#endif // !MATERIALSYSTEM_NODX
		LogVarAdr("g_pMaterialSystem", g_pMaterialSystem);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 70 48 83 3D ?? ?? ?? ?? ??").GetPtr(CMaterialSystem__Init);
		Module_FindPattern(g_GameDll, "48 83 EC 58 48 89 6C 24 ??").GetPtr(CMaterialSystem__Shutdown);

		Module_FindPattern(g_GameDll, "48 89 54 24 ?? 56 48 83 EC 50").GetPtr(CMaterialSystem__Connect);
		Module_FindPattern(g_GameDll, "48 83 EC 28 8B 0D ?? ?? ?? ?? 48 89 6C 24 ??").GetPtr(CMaterialSystem__Disconnect);
#ifndef MATERIALSYSTEM_NODX
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 41 56 41 57 48 83 EC 40 65 48 8B 04 25 ?? ?? ?? ??").GetPtr(CMaterialSystem__SwapBuffers);

		Module_FindPattern(g_GameDll, "44 89 4C 24 ?? 44 88 44 24 ?? 48 89 4C 24 ??").GetPtr(CMaterialSystem__FindMaterialEx);
		Module_FindPattern(g_GameDll, "8B 05 ?? ?? ?? ?? 89 02 8B 05 ?? ?? ?? ?? 41 89 ?? C3 CC CC CC CC CC CC CC CC CC CC CC CC CC CC 8B 05 ?? ?? ?? ??").GetPtr(CMaterialSystem__GetScreenSize);

		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 48 8B 02 48 8B CA 49 8B F9").GetPtr(CMaterialSystem__CreditModelTextures);
		Module_FindPattern(g_GameDll, "48 83 EC ?? 48 8B 05 ?? ?? ?? ?? 44 0F 29 44 24").GetPtr(CMaterialSystem__UpdateStreamCamera);

		Module_FindPattern(g_GameDll, "44 89 4C 24 ?? 44 89 44 24 ?? 48 89 4C 24 ?? 55 53 56").GetPtr(v_DispatchDrawCall);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 81 EC ?? ?? ?? ?? 8B 15 ?? ?? ?? ??").GetPtr(v_SpinPresent);
#endif // !MATERIALSYSTEM_NODX
	}
	virtual void GetVar(void) const
	{
#ifndef MATERIALSYSTEM_NODX
		CMemory(v_DispatchDrawCall).FindPattern("48 8B ?? ?? ?? ?? 01").ResolveRelativeAddressSelf(0x3, 0x7).GetPtr(s_pRenderContext);
		CMemory(CMaterialSystem__Disconnect).FindPattern("48 8D").ResolveRelativeAddressSelf(0x3, 0x7).GetPtr(g_pMaterialAdapterMgr);
#endif // !MATERIALSYSTEM_NODX
		g_pMaterialSystem = Module_FindPattern(g_GameDll, "8B 41 28 85 C0 7F 18").FindPatternSelf("48 8D 0D").ResolveRelativeAddressSelf(3, 7).RCast<CMaterialSystem*>();
	}
	virtual void GetCon(void) const
	{
		g_pMaterialVFTable = g_GameDll.GetVirtualMethodTable(".?AVCMaterial@@").RCast<void*>();
	}
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#ifndef MATERIALSYSTEM_NODX
///////////////////////////////////////////////////////////////////////////////
// Missing-material logger. Hooks FindMaterialEx only; toggle mat_alwaysComplain.
class VMaterialMissingLogS21 : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CMaterialSystem::FindMaterialEx", CMaterialSystem__FindMaterialEx);
		LogVarAdr("g_pErrorMaterials", g_ppErrorMaterials);
	}
	virtual void GetFun(void) const
	{
		// CMaterialSystem::FindMaterialEx -- S21. Unique prologue
		// (push rbp/r14/r15 + sub rsp,430h + the three home-area arg spills at
		// disp 0x430/0x440/0x448). The disp32 stores are wildcarded for safety.
		Module_FindPattern(g_GameDll,
			"40 55 41 56 41 57 48 81 EC 30 04 00 00 48 8D 6C 24 20 "
			"48 89 9D ?? ?? ?? ?? 48 89 B5 ?? ?? ?? ?? 48 89 BD")
			.GetPtr(CMaterialSystem__FindMaterialEx);

		if (!CMaterialSystem__FindMaterialEx)
			Warning(eDLL_T::MS, "[MAT-MISSING] CMaterialSystem::FindMaterialEx pattern unresolved\n");
	}
	virtual void GetVar(void) const
	{
		// Resolve g_pErrorMaterials from the tiny per-type error
		// material accessor: mov rax,[rip+d]; movzx ecx,dl; mov rax,[rax+rcx*8]; ret.
		g_ppErrorMaterials = Module_FindPattern(g_GameDll,
			"48 8B 05 ?? ?? ?? ?? 0F B6 CA 48 8B 04 C8 C3")
			.ResolveRelativeAddressSelf(0x3, 0x7).RCast<CMaterialGlue***>();

		if (!g_ppErrorMaterials)
			Warning(eDLL_T::MS, "[MAT-MISSING] g_pErrorMaterials pattern unresolved\n");
	}
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// Serializes material unload against the GPU-feedback job build.
class VTexStreamFeedbackSyncS21 : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("TextureStreamMgr::ProcessGPUFeedback", v_TextureStreamMgr_ProcessGPUFeedback);
		LogFunAdr("Material::UnregisterStreaming", v_Material_UnregisterStreaming);
		LogFunAdr("TextureStreamMgr::CreditFeedbackEntry", v_TextureStreamMgr_CreditFeedbackEntry);
		LogVarAdr("g_pStreamingMaterialById", g_ppStreamingMaterialById);
	}
	virtual void GetFun(void) const
	{
		// Per-frame GPU-feedback processor. The two client executables differ in
		// the saved register (dx12 keeps r14, dx11 rsi), so neither pattern can
		// match the other binary -- whichever resolves selects the build.
		Module_FindPattern(g_GameDll,
			"40 53 41 56 48 81 EC ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 8B D9 4C 8B 30")
			.GetPtr(v_TextureStreamMgr_ProcessGPUFeedback);

		if (!v_TextureStreamMgr_ProcessGPUFeedback)
		{
			Module_FindPattern(g_GameDll,
				"40 53 56 48 81 EC ?? ?? ?? ?? 65 48 8B 04 25")
				.GetPtr(v_TextureStreamMgr_ProcessGPUFeedback);
		}

		// matl asset unload callback: frees the stream id, then invalidates the
		// feedback state. Same bytes in both executables.
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 57 48 83 EC ?? 48 8B D9 48 8B 0D ?? ?? ?? ?? 48 85 C9")
			.GetPtr(v_Material_UnregisterStreaming);

		// Per-material crediting job. Same bytes in both executables.
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 48 83 EC ?? 45 33 FF")
			.GetPtr(v_TextureStreamMgr_CreditFeedbackEntry);

		if (!v_TextureStreamMgr_ProcessGPUFeedback || !v_Material_UnregisterStreaming || !v_TextureStreamMgr_CreditFeedbackEntry)
		{
			Warning(eDLL_T::MS,
				"[TEXSTREAM-SYNC] pattern unresolved (processor=%p unload=%p) -- a material freed during a feedback build can reach the job\n",
				v_TextureStreamMgr_ProcessGPUFeedback, v_Material_UnregisterStreaming);
		}
	}
	virtual void GetVar(void) const
	{
		// The material id table, read off the store in the register callback.
		g_ppStreamingMaterialById = Module_FindPattern(g_GameDll,
			"48 8D 05 ?? ?? ?? ?? 49 8B 16 45 33 C0 41 8B 4E 08 48 89 3C D8")
			.ResolveRelativeAddressSelf(0x3, 0x7).RCast<uint8_t**>();

		if (!g_ppStreamingMaterialById)
		{
			Warning(eDLL_T::MS,
				"[TEXSTREAM-FB] material id table unresolved -- released materials will stay published to the streamer\n");
		}
	}
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////
#endif // !MATERIALSYSTEM_NODX

#endif // MATERIALSYSTEM_H
#else // !CLIENT_DLL
#ifndef MATERIALSYSTEM_H
#define MATERIALSYSTEM_H
#include "cmaterialglue.h"
#include "cmatrendercontext.h"
#include "public/materialsystem/imaterialsystem.h"

class CMaterialSystem
{
public:
	static bool Connect(CMaterialSystem* thisptr, const CreateInterfaceFn factory);
	static void Disconnect(CMaterialSystem* thisptr);

	static InitReturnVal_t Init(CMaterialSystem* thisptr);
	static int Shutdown(CMaterialSystem* thisptr);
#ifndef MATERIALSYSTEM_NODX
	static void* SwapBuffers(CMaterialSystem* pMatSys);
	static CMaterialGlue* FindMaterialEx(CMaterialSystem* pMatSys, const char* pMaterialName, uint8_t nMaterialType, int nUnk, bool bComplain);
	static Vector2D GetScreenSize(CMaterialSystem* pMatSys = nullptr);

	static void CreditModelTextures(CMaterialSystem* const pMatSys, CMaterialGlue* const materialGlue, __int64 a3, __int64 a4, unsigned int a5, const Vector3D* const pViewOrigin, const float tanOfHalfFov, const float viewWidthPixels, int a9);
	static void UpdateStreamCamera(CMaterialSystem* const pMatSys, const Vector3D* const camPos, const QAngle* const camAng, const float halfFovX, const float viewWidth);
#endif // !MATERIALSYSTEM_NODX

	// TODO: reverse the vftable!
	inline int GetCurrentFrameCount()
	{
		const static int index = 74;
		return CallVFunc<int>(index, this);
	}

	inline CMatRenderContext* GetRenderContext()
	{
		const static int index = 126;
		return CallVFunc<CMatRenderContext*>(index, this);
	}
};

#ifndef MATERIALSYSTEM_NODX
class CMaterialDeviceMgr
{
public:
	inline const MaterialAdapterInfo_t& GetAdapterInfo(int nIndex) const
	{
		Assert(nIndex >= 0 && nIndex < SDK_ARRAYSIZE(m_AdapterInfo));
		return m_AdapterInfo[nIndex];
	}
	inline const MaterialAdapterInfo_t& GetAdapterInfo() const
	{
		// Retrieve info of the selected adapter.
		return GetAdapterInfo(m_SelectedAdapter);
	}

private:
	enum
	{
		MAX_ADAPTER_COUNT = 4
	};

	IDXGIAdapter* m_Adapters[MAX_ADAPTER_COUNT];
	void* m_pUnknown1[MAX_ADAPTER_COUNT];
	MaterialAdapterInfo_t m_AdapterInfo[MAX_ADAPTER_COUNT];
	size_t m_AdapterMemorySize[MAX_ADAPTER_COUNT];
	int m_NumDisplayAdaptersProcessed;
	int m_SelectedAdapter;
	int m_NumDisplayAdapters;
};

inline CMaterialDeviceMgr* g_pMaterialAdapterMgr = nullptr;
#endif // !MATERIALSYSTEM_NODX

/* ==== MATERIALSYSTEM ================================================================================================================================================== */
inline InitReturnVal_t(*CMaterialSystem__Init)(CMaterialSystem* thisptr);
inline int(*CMaterialSystem__Shutdown)(CMaterialSystem* thisptr);

inline bool(*CMaterialSystem__Connect)(CMaterialSystem*, const CreateInterfaceFn);
inline void(*CMaterialSystem__Disconnect)(CMaterialSystem*);

inline CMaterialSystem* g_pMaterialSystem = nullptr;
inline void* g_pMaterialVFTable = nullptr;
#ifndef MATERIALSYSTEM_NODX
inline void*(*CMaterialSystem__SwapBuffers)(CMaterialSystem* pMatSys);

inline CMaterialGlue*(*CMaterialSystem__FindMaterialEx)(CMaterialSystem* pMatSys, const char* pMaterialName, uint8_t nMaterialType, int nUnk, bool bComplain);
inline void(*CMaterialSystem__GetScreenSize)(CMaterialSystem* pMatSys, float* outX, float* outY);

inline void(*CMaterialSystem__CreditModelTextures)(CMaterialSystem* const pMatSys, CMaterialGlue* const materialGlue, __int64 a3, __int64 a4, unsigned int a5, const Vector3D* const pViewOrigin, const float tanOfHalfFov, const float viewWidthPixels, int a9);
inline void(*CMaterialSystem__UpdateStreamCamera)(CMaterialSystem* const pMatSys, const Vector3D* const camPos, const QAngle* const camAng, const float halfFovX, const float viewWidth);

inline void*(*v_DispatchDrawCall)(int64_t a1, uint64_t a2, int a3, int a4, int64_t a5, int a6, uint8_t a7, int64_t a8, uint32_t a9, uint32_t a10, int a11, __m128* a12, int a13, int64_t a14);
inline ssize_t(*v_SpinPresent)(void);
#endif // !MATERIALSYSTEM_NODX

#ifndef MATERIALSYSTEM_NODX
inline void** s_pRenderContext; // NOTE: This is some CMaterial instance or array.
#endif // !MATERIALSYSTEM_NODX

// TODO: move to materialsystem_global.h!
// TODO: reverse the vftable!
inline CMaterialSystem* MaterialSystem()
{
	return g_pMaterialSystem;
}

///////////////////////////////////////////////////////////////////////////////
class VMaterialSystem : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogConAdr("CMaterial::`vftable'", g_pMaterialVFTable);
		LogFunAdr("CMaterialSystem::Init", CMaterialSystem__Init);
		LogFunAdr("CMaterialSystem::Shutdown", CMaterialSystem__Shutdown);
		LogFunAdr("CMaterialSystem::Connect", CMaterialSystem__Connect);
		LogFunAdr("CMaterialSystem::Disconnect", CMaterialSystem__Disconnect);
#ifndef MATERIALSYSTEM_NODX
		LogFunAdr("CMaterialSystem::SwapBuffers", CMaterialSystem__SwapBuffers);
		LogFunAdr("CMaterialSystem::FindMaterialEx", CMaterialSystem__FindMaterialEx);
		LogFunAdr("CMaterialSystem::GetScreenSize", CMaterialSystem__GetScreenSize);
		LogFunAdr("CMaterialSystem::CreditModelTextures", CMaterialSystem__CreditModelTextures);
		LogFunAdr("CMaterialSystem::UpdateStreamCamera", CMaterialSystem__UpdateStreamCamera);
		LogFunAdr("DispatchDrawCall", v_DispatchDrawCall);
		LogFunAdr("SpinPresent", v_SpinPresent);
#endif // !MATERIALSYSTEM_NODX

#ifndef MATERIALSYSTEM_NODX
		LogVarAdr("s_pRenderContext", s_pRenderContext);
		LogVarAdr("g_MaterialAdapterMgr", g_pMaterialAdapterMgr);
#endif // !MATERIALSYSTEM_NODX
		LogVarAdr("g_pMaterialSystem", g_pMaterialSystem);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 70 48 83 3D ?? ?? ?? ?? ??").GetPtr(CMaterialSystem__Init);
		Module_FindPattern(g_GameDll, "48 83 EC 58 48 89 6C 24 ??").GetPtr(CMaterialSystem__Shutdown);

		Module_FindPattern(g_GameDll, "48 89 54 24 ?? 56 48 83 EC 50").GetPtr(CMaterialSystem__Connect);
		Module_FindPattern(g_GameDll, "48 83 EC 28 8B 0D ?? ?? ?? ?? 48 89 6C 24 ??").GetPtr(CMaterialSystem__Disconnect);
#ifndef MATERIALSYSTEM_NODX
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 41 56 41 57 48 83 EC 40 65 48 8B 04 25 ?? ?? ?? ??").GetPtr(CMaterialSystem__SwapBuffers);

		Module_FindPattern(g_GameDll, "44 89 4C 24 ?? 44 88 44 24 ?? 48 89 4C 24 ??").GetPtr(CMaterialSystem__FindMaterialEx);
		Module_FindPattern(g_GameDll, "8B 05 ?? ?? ?? ?? 89 02 8B 05 ?? ?? ?? ?? 41 89 ?? C3 CC CC CC CC CC CC CC CC CC CC CC CC CC CC 8B 05 ?? ?? ?? ??").GetPtr(CMaterialSystem__GetScreenSize);

		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 48 8B 02 48 8B CA 49 8B F9").GetPtr(CMaterialSystem__CreditModelTextures);
		Module_FindPattern(g_GameDll, "48 83 EC ?? 48 8B 05 ?? ?? ?? ?? 44 0F 29 44 24").GetPtr(CMaterialSystem__UpdateStreamCamera);

		Module_FindPattern(g_GameDll, "44 89 4C 24 ?? 44 89 44 24 ?? 48 89 4C 24 ?? 55 53 56").GetPtr(v_DispatchDrawCall);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 81 EC ?? ?? ?? ?? 8B 15 ?? ?? ?? ??").GetPtr(v_SpinPresent);
#endif // !MATERIALSYSTEM_NODX
	}
	virtual void GetVar(void) const
	{
#ifndef MATERIALSYSTEM_NODX
		CMemory(v_DispatchDrawCall).FindPattern("48 8B ?? ?? ?? ?? 01").ResolveRelativeAddressSelf(0x3, 0x7).GetPtr(s_pRenderContext);
		CMemory(CMaterialSystem__Disconnect).FindPattern("48 8D").ResolveRelativeAddressSelf(0x3, 0x7).GetPtr(g_pMaterialAdapterMgr);
#endif // !MATERIALSYSTEM_NODX
		g_pMaterialSystem = Module_FindPattern(g_GameDll, "8B 41 28 85 C0 7F 18").FindPatternSelf("48 8D 0D").ResolveRelativeAddressSelf(3, 7).RCast<CMaterialSystem*>();
	}
	virtual void GetCon(void) const
	{
		g_pMaterialVFTable = g_GameDll.GetVirtualMethodTable(".?AVCMaterial@@").RCast<void*>();
	}
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // MATERIALSYSTEM_H
#endif // CLIENT_DLL
