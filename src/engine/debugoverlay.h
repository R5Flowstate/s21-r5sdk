#pragma once
#include "mathlib/vector.h"
#include "mathlib/vector4d.h"
#include "mathlib/color.h"
#include "mathlib/ssemath.h"
#include "public/idebugoverlay.h"
#include "public/vphysics/vphysics_interface.h"

// When used as a duration by a server-side NDebugOverlay:: call,
// causes the overlay to persist until the next server update.
constexpr auto NDEBUG_PERSIST_TILL_NEXT_SERVER = (0.01023f);

// When used as a duration by a client-side script function call,
// causes the overlay to persist until the next client update.
constexpr auto NDEBUG_PERSIST_TILL_NEXT_CLIENT = (0.02046f);

extern ConVar enable_debug_text_overlays;

enum class OverlayType_t
{
	OVERLAY_BOX = 0,
	OVERLAY_SPHERE,
	OVERLAY_LINE,
	OVERLAY_CUSTOM_MESH,
	OVERLAY_SPLINE,
	OVERLAY_TRIANGLE,
	OVERLAY_SWEPT_BOX,
	OVERLAY_CAPSULE, // see, possibly a tetrahedron or quadrilateral? Never used. Now replaced with capsule.
	OVERLAY_DESTROYED, // see, DestroyOverlay sets all destroyed overlays to this.
};

struct OverlayBase_t
{
	OverlayBase_t(void)
	{
		m_Type          = OverlayType_t::OVERLAY_BOX;
		m_nCreationTick = -1;
		m_flEndTime     = 0.0f;
		m_pNextOverlay  = nullptr;
		m_nOverlayTick  = -1;
	}
	bool IsDead(void) const;
	void SetEndTime(const float duration);

	OverlayType_t   m_Type;          // What type of overlay is it?
	int             m_nCreationTick; // Duration -1 means go away after this frame #
	float           m_flEndTime;     // When does this box go away
	// There is 4 bytes padding here.
	OverlayBase_t*  m_pNextOverlay;  // The next overlay
	int             m_nOverlayTick;  // 24
	// There is 4 bytes padding here.
};

struct OverlayBox_t : public OverlayBase_t
{
	OverlayBox_t(void) { m_Type = OverlayType_t::OVERLAY_BOX; }

	matrix3x4a_t    transforms;
	Vector3D        mins;
	Vector3D        maxs;
	int             r;
	int             g;
	int             b;
	int             a;
	bool            noDepthTest;
};

struct OverlaySphere_t : public OverlayBase_t
{
	OverlaySphere_t(void) { m_Type = OverlayType_t::OVERLAY_SPHERE; }

	Vector3D        vOrigin;
	float           flRadius;
	int             nTheta;
	int             nPhi;
	int             r;
	int             g;
	int             b;
	int             a;
	bool            noDepthTest;
};

struct OverlayLine_t : public OverlayBase_t
{
	OverlayLine_t(void) { m_Type = OverlayType_t::OVERLAY_LINE; }

	Vector3D        origin;
	Vector3D        dest;
	int             r;
	int             g;
	int             b;
	int             a;
	bool            noDepthTest;
};

struct OverlayCustomMesh_t : public OverlayBase_t
{
	OverlayCustomMesh_t(void) { m_Type = OverlayType_t::OVERLAY_CUSTOM_MESH; }

	matrix3x4_t     matrices[128];
	int             r;
	int             g;
	int             b;
	int             a;
	bool            noDepthTest;
};

struct OverlayTriangle_t : public OverlayBase_t
{
	OverlayTriangle_t() { m_Type = OverlayType_t::OVERLAY_TRIANGLE; }

	Vector3D        p1;
	Vector3D        p2;
	Vector3D        p3;
	int             r;
	int             g;
	int             b;
	int             a;
	bool            noDepthTest;
};

struct OverlaySweptBox_t : public OverlayBase_t
{
	OverlaySweptBox_t() { m_Type = OverlayType_t::OVERLAY_SWEPT_BOX; }

	Vector3D        start;
	Vector3D        end;
	Vector3D        mins;
	Vector3D        maxs;
	QAngle          angles;
	int             r;
	int             g;
	int             b;
	int             a;
	bool            noDepthTest;
};

struct OverlayCapsule_t : public OverlayBase_t
{
	OverlayCapsule_t() { m_Type = OverlayType_t::OVERLAY_CAPSULE; }

	Vector3D        start;
	Vector3D        end;
	float           radius;
	int             r;
	int             g;
	int             b;
	int             a;
	bool            noDepthTest;
};

class OverlayText_t
{
public:
	OverlayText_t()
	{
		origin.Init();
		bUseOrigin = false;
		lineOffset = 0;
		screenPos.Init();
		m_nServerCount = -1;
		textLen = 0;
		textBuf = nullptr;
		m_flEndTime = 0.0f;
		m_nCreationTick = -1;
		m_nOverlayTick = -1;
		r = g = b = a = 255;
		nextOverlayText = 0;
	}

	~OverlayText_t()
	{
		if (textBuf)
		{
			delete[] textBuf;
			textBuf = nullptr;
		}
	}

	void SetEndTime(const float duration);

	Vector3D origin;
	bool bUseOrigin;
	int lineOffset;
	Vector2D screenPos;
	int m_nServerCount;
	char unk[24];
	ssize_t textLen;
	char* textBuf;
	float m_flEndTime;
	int m_nCreationTick;
	int m_nOverlayTick;
	int r;
	int g;
	int b;
	int a;
	OverlayText_t* nextOverlayText;
};

class CIVDebugOverlay : public IVDebugOverlay, public IVPhysicsDebugOverlay
{
public: // Hook statics
	static void AddEntityTextOverlay(CIVDebugOverlay* const thisptr, const int entIndex, const int lineOffset, const float duration, const int r, const int g, const int b, const int a, const char* const format, ...);

	static void AddTextOverlay(CIVDebugOverlay* const thisptr, const Vector3D& origin, const float duration, const char* const format, ...);
	static void AddTextOverlayAtOffset(CIVDebugOverlay* const thisptr, const Vector3D& origin, const int lineOffset, const float duration, const char* const format, ...);

	static void AddScreenTextOverlayAtOffsetInternal(CIVDebugOverlay* const thisptr, const float flXPos, const float flYPos, const int lineOffset, const float flDuration, const int r, const int g, const int b, const int a, const char* const text);
	static void AddScreenTextOverlayInternal(CIVDebugOverlay* const thisptr, const float flXPos, const float flYPos, const float flDuration, const int r, const int g, const int b, const int a, const char* const text);

	static void AddTextOverlayRGBu32(CIVDebugOverlay* const thisptr, const Vector3D& origin, const int lineOffset, const float duration,
		const int r, const int g, const int b, const int a, PRINTF_FORMAT_STRING const char* const format, ...) FMTFUNCTION(9, 10);

	static void AddTextOverlayRGBf32(CIVDebugOverlay* const thisptr, const Vector3D& origin, const int lineOffset, const float duration,
		const float r, const float g, const float b, const float a, PRINTF_FORMAT_STRING const char* const format, ...) FMTFUNCTION(9, 10);

public:
	static void AddSphereOverlayInternal(CIVDebugOverlay* const thisptr, const Vector3D& vOrigin, const float flRadius, const int nTheta, const int nPhi, const int r, const int g, const int b, const int a, const bool noDepthTest, const float flDuration);
	static void AddSweptBoxInternal(CIVDebugOverlay* const thisptr, const Vector3D& start, const Vector3D& end, const Vector3D& mins, const Vector3D& max, const QAngle& angles, const int r, const int g, const int b, const int a, const bool noDepthTest, const float flDuration);
	static void AddCapsuleOverlayInternal(CIVDebugOverlay* const thisptr, const Vector3D& vStart, const Vector3D& vEnd, const float flRadius, const int r, const int g, const int b, const int a, const bool noDepthTest, const float flDuration);

	static void AddPhysicsEntityTextOverlay(CIVDebugOverlay* const thisptr, const int entIndex, const int lineOffset, const float duration, const int r, const int g, const int b, const int a, const char* const format, ...);
	static void AddPhysicsTextOverlay(CIVDebugOverlay* const thisptr, const Vector3D& origin, const float duration, const char* const format, ...);
	static void AddPhysicsTextOverlayAtOffset(CIVDebugOverlay* const thisptr, const Vector3D& origin, const int lineOffset, const float duration, const char* const format, ...);
	static void AddPhysicsTextOverlayRGBf32(CIVDebugOverlay* const thisptr, const Vector3D& origin, const int lineOffset, const float duration,
		const float r, const float g, const float b, const float a, PRINTF_FORMAT_STRING const char* const format, ...) FMTFUNCTION(9, 10);

private:
	char m_text[1024];
	va_list m_argptr;
};

inline CIVDebugOverlay* g_pDebugOverlay = nullptr;

extern void DebugOverlay_HandleDecayed();

// True when the process was launched with -devsdk / -dev / -developer.
extern bool DebugOverlay_DevModeEnabled();

// S2C overlay batch cap. Over-budget items drop with a warning; count is a
// byte, payload length a short.
static constexpr int kDebugOverlayS2CMax = 64;

// Wire shape ids, named on both sides so the writer and the reader cannot drift.
// An oriented box is one item rather than the twelve its edges would cost, which
// is what lets a continuous shape stream fit in the frame budget.
static constexpr uint8_t kS2CLine = 0;
static constexpr uint8_t kS2CBox = 1;
static constexpr uint8_t kS2CSphere = 2;
static constexpr uint8_t kS2CCapsule = 3;
static constexpr uint8_t kS2CTriangle = 4;
static constexpr uint8_t kS2CTransformedBox = 5;
static constexpr int kDebugOverlayS2CItemBytes = 58;
static constexpr int kDebugOverlayS2CMaxBytes = 1 + kDebugOverlayS2CMax * kDebugOverlayS2CItemBytes;

#if defined(CLIENT_DLL)
// S3 type 69 -- dedi replicates script/engine overlay adds to the client list.
void DebugOverlay_ApplyS2CPayload(const uint8_t* data, int nBytes);
#endif // CLIENT_DLL

inline void(*v_DebugOverlay_DrawAllOverlays)(bool bDraw);
inline void(*v_DebugOverlay_ClearAllOverlays)(void);
inline void(*v_DebugOverlay_DebugDebugOverlays)(void* unk1, unsigned short unk2, unsigned int unk3, float unk4);
#if defined(CLIENT_DLL)
inline void(*v_DebugOverlay_DestroyOverlay)(OverlayBase_t* const pOverlay);
inline void(*v_DebugOverlay_AddLineOverlay)(const Vector3D* origin, const Vector3D* dest,
	int r, int g, int b, int a, bool noDepthTest, float duration);
#endif // CLIENT_DLL

inline void (*v_DebugOverlay_AddEntityTextOverlay)(CIVDebugOverlay* const thisptr, const int entIndex, const int lineOffset, const float duration,
	const int r, const int g, const int b, const int a, const char* const format, ...);

inline void(*v_DebugOverlay_SetEndTime)(OverlayBase_t* const pOverlay, const float flDuration);

inline OverlayBase_t** s_pOverlays = nullptr;
inline OverlayText_t** s_pOverlayText = nullptr;
inline CThreadMutex* s_OverlayMutex = nullptr;
inline bool* s_bDrawGrid = nullptr;

inline int* g_nRenderTickCount = nullptr;
inline int* g_nOverlayTickCount = nullptr;

inline int* g_nOverlayStage = nullptr;

// Overlay expire clock: tick*interval if live, else float time.
// g_pClientState is unresolved on the client product.
inline bool* s_pOverlayUseTickClock = nullptr;
inline int* s_pOverlayTickCount = nullptr;
inline float* s_pOverlayTickInterval = nullptr;
inline float* s_pOverlayCurTime = nullptr;

inline int* g_nNewOtherOverlays = nullptr;
inline int* g_nNewTextOverlays = nullptr;

inline void* g_pIVPhysicsDebugOverlay_VFTable = nullptr;
inline void* g_pIVDebugOverlay_VFTable = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VDebugOverlay : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("DebugOverlay_DrawAllOverlays", v_DebugOverlay_DrawAllOverlays);
		LogFunAdr("DebugOverlay_ClearAllOverlays", v_DebugOverlay_ClearAllOverlays);
		LogFunAdr("DebugOverlay_DebugDebugOverlays", v_DebugOverlay_DebugDebugOverlays);
		LogFunAdr("DebugOverlay_AddEntityTextOverlay", v_DebugOverlay_AddEntityTextOverlay);
		LogFunAdr("DebugOverlay_SetEndTime", v_DebugOverlay_SetEndTime);
#if defined(CLIENT_DLL)
		LogFunAdr("DebugOverlay_DestroyOverlay", v_DebugOverlay_DestroyOverlay);
		LogFunAdr("DebugOverlay_AddLineOverlay", v_DebugOverlay_AddLineOverlay);
#endif // CLIENT_DLL
		LogVarAdr("s_pOverlays", s_pOverlays);
		LogVarAdr("s_pOverlayText", s_pOverlayText);
		LogVarAdr("s_OverlayMutex", s_OverlayMutex);
		LogVarAdr("s_bDrawGrid", s_bDrawGrid);
		LogVarAdr("g_nOverlayTickCount", g_nOverlayTickCount);
		LogVarAdr("g_nRenderTickCount", g_nRenderTickCount);
		LogVarAdr("g_nNewOtherOverlays", g_nNewOtherOverlays);
		LogVarAdr("g_nNewTextOverlays", g_nNewTextOverlays);
#if defined(CLIENT_DLL)
		LogVarAdr("s_pOverlayCurTime", s_pOverlayCurTime);
#endif // CLIENT_DLL
	}
	virtual void GetFun(void) const
	{
#if defined(CLIENT_DLL)
		// mov [rsp+8],cl ; sub rsp,38h ; mov rax,[cvar] ; cmp dword [rax+64h],0 ; jz end
		Module_FindPattern(g_GameDll, "88 4C 24 08 48 83 EC 38 48 8B 05 ?? ?? ?? ?? 83 78 64 00 0F 84").GetPtr(v_DebugOverlay_DrawAllOverlays);
		Module_FindPattern(g_GameDll, "40 53 48 83 EC ?? 48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 48 8B 0D").GetPtr(v_DebugOverlay_ClearAllOverlays);
		Module_FindPattern(g_GameDll, "4C 8B DC 45 89 43 ?? 66 89 54 24").GetPtr(v_DebugOverlay_DebugDebugOverlays);
		Module_FindPattern(g_GameDll, "40 53 48 83 EC ?? 48 8B D9 48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 48 63 03").GetPtr(v_DebugOverlay_DestroyOverlay);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 48 8B 05 ?? ?? ?? ?? 41 8B F1 41 8B E8 48 8B DA").GetPtr(v_DebugOverlay_AddLineOverlay);

		if (!v_DebugOverlay_DrawAllOverlays || !v_DebugOverlay_ClearAllOverlays)
			Warning(eDLL_T::CLIENT, "[DBGDRAW] overlay manager patterns unresolved; debug drawing disabled\n");
		if (!v_DebugOverlay_DestroyOverlay)
			Warning(eDLL_T::CLIENT, "[DBGDRAW] DestroyOverlay unresolved; native-allocated overlays cannot be freed\n");
		if (!v_DebugOverlay_AddLineOverlay)
			Warning(eDLL_T::CLIENT, "[DBGDRAW] AddLineOverlay unresolved; server overlay replicate cannot insert lines\n");
#else
		Module_FindPattern(g_GameDll, "40 55 48 83 EC 30 48 8B 05 ?? ?? ?? ?? 0F B6 E9").GetPtr(v_DebugOverlay_DrawAllOverlays);
		Module_FindPattern(g_GameDll, "40 53 48 83 EC ?? 48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 48 8B 0D").GetPtr(v_DebugOverlay_ClearAllOverlays);
		Module_FindPattern(g_GameDll, "4C 8B DC 45 89 43 ?? 66 89 54 24").GetPtr(v_DebugOverlay_DebugDebugOverlays);
		Module_FindPattern(g_GameDll, "40 53 56 57 48 83 EC ?? 48 8D B4 24").GetPtr(v_DebugOverlay_AddEntityTextOverlay);
		Module_FindPattern(g_GameDll, "48 83 EC ?? FF 05 ?? ?? ?? ?? 48 8B D1").GetPtr(v_DebugOverlay_SetEndTime);
#endif // CLIENT_DLL
	}
	virtual void GetVar(void) const
	{
#if defined(CLIENT_DLL)
		s_OverlayMutex   = CMemory(v_DebugOverlay_ClearAllOverlays).Offset(0x04).FindPatternSelf("48 8D 0D", CMemory::Direction::DOWN, 16).ResolveRelativeAddressSelf(0x3, 0x7).RCast<CThreadMutex*>();
		s_pOverlays      = CMemory(v_DebugOverlay_ClearAllOverlays).Offset(0x10).FindPatternSelf("48 8B 0D", CMemory::Direction::DOWN, 32).ResolveRelativeAddressSelf(0x3, 0x7).RCast<OverlayBase_t**>();
		s_pOverlayText   = CMemory(v_DebugOverlay_ClearAllOverlays).Offset(0x30).FindPatternSelf("48 8B 1D", CMemory::Direction::DOWN, 64).ResolveRelativeAddressSelf(0x3, 0x7).RCast<OverlayText_t**>();

		// Both tick compares are rip-relative reads in the decay walk, in
		// render-then-overlay order. Index them by occurrence: chaining off a
		// resolved site would restart the scan inside the data section.
		const CMemory drawBase(v_DebugOverlay_DrawAllOverlays);
		g_nRenderTickCount  = drawBase.FindPattern("3B 05", CMemory::Direction::DOWN, 0x120, 1).ResolveRelativeAddress(0x2, 0x6).RCast<int*>();
		g_nOverlayTickCount = drawBase.FindPattern("3B 05", CMemory::Direction::DOWN, 0x120, 2).ResolveRelativeAddress(0x2, 0x6).RCast<int*>();

		// Overlay clock, mined off the decay compare in the same walk. The
		// byte test appears twice: occurrence 1 is the engine-running flag.
		s_pOverlayUseTickClock = drawBase.FindPattern("80 3D", CMemory::Direction::DOWN, 0x220, 2).ResolveRelativeAddress(0x2, 0x7).RCast<bool*>();
		s_pOverlayTickCount    = drawBase.FindPattern("66 0F 6E 05", CMemory::Direction::DOWN, 0x220, 1).ResolveRelativeAddress(0x4, 0x8).RCast<int*>();
		s_pOverlayTickInterval = drawBase.FindPattern("F3 0F 59 05", CMemory::Direction::DOWN, 0x220, 1).ResolveRelativeAddress(0x4, 0x8).RCast<float*>();
		s_pOverlayCurTime      = drawBase.FindPattern("F3 0F 10 05", CMemory::Direction::DOWN, 0x220, 1).ResolveRelativeAddress(0x4, 0x8).RCast<float*>();

		if (!s_pOverlayUseTickClock || !s_pOverlayTickCount || !s_pOverlayTickInterval || !s_pOverlayCurTime)
			Warning(eDLL_T::CLIENT, "[DBGDRAW] overlay clock unresolved; duration overlays cannot expire\n");

		const CMemory debugBase(v_DebugOverlay_DebugDebugOverlays);
		g_nOverlayStage = debugBase.FindPattern("8B 05", CMemory::Direction::DOWN, 0x120, 3).ResolveRelativeAddress(0x2, 0x6).RCast<int*>();

		// The two new-overlay counters are zeroed back to back at the tail.
		g_nNewOtherOverlays = debugBase.FindPattern("89 3D", CMemory::Direction::DOWN, 0x1200, 1).ResolveRelativeAddress(0x2, 0x6).RCast<int*>();
		g_nNewTextOverlays  = debugBase.FindPattern("89 3D", CMemory::Direction::DOWN, 0x1200, 2).ResolveRelativeAddress(0x2, 0x6).RCast<int*>();

		if (!s_pOverlays || !s_pOverlayText || !g_nRenderTickCount || !g_nOverlayTickCount ||
			!g_nOverlayStage || !g_nNewOtherOverlays || !g_nNewTextOverlays)
			Warning(eDLL_T::CLIENT, "[DBGDRAW] globals unresolved: overlays=0x%llX text=0x%llX renderTick=0x%llX overlayTick=0x%llX stage=0x%llX newOther=0x%llX newText=0x%llX\n",
				(unsigned long long)(uintptr_t)s_pOverlays,
				(unsigned long long)(uintptr_t)s_pOverlayText,
				(unsigned long long)(uintptr_t)g_nRenderTickCount,
				(unsigned long long)(uintptr_t)g_nOverlayTickCount,
				(unsigned long long)(uintptr_t)g_nOverlayStage,
				(unsigned long long)(uintptr_t)g_nNewOtherOverlays,
				(unsigned long long)(uintptr_t)g_nNewTextOverlays);
#else
		s_pOverlays = CMemory(v_DebugOverlay_DrawAllOverlays).Offset(0x10).FindPatternSelf("48 8B 3D", CMemory::Direction::DOWN, 150).ResolveRelativeAddressSelf(0x3, 0x7).RCast<OverlayBase_t**>();
		s_pOverlayText = CMemory(v_DebugOverlay_ClearAllOverlays).Offset(0x3A).FindPatternSelf("48 8B 1D", CMemory::Direction::DOWN, 150).ResolveRelativeAddressSelf(0x3, 0x7).RCast<OverlayText_t**>();
		s_OverlayMutex = CMemory(v_DebugOverlay_DrawAllOverlays).Offset(0x10).FindPatternSelf("48 8D 0D", CMemory::Direction::DOWN, 150).ResolveRelativeAddressSelf(0x3, 0x7).RCast<CThreadMutex*>();
		s_bDrawGrid = CMemory(v_DebugOverlay_ClearAllOverlays).Offset(0xC0).FindPatternSelf("C6 05", CMemory::Direction::DOWN, 150).ResolveRelativeAddressSelf(0x2, 0x7).RCast<bool*>();

		g_nRenderTickCount = CMemory(v_DebugOverlay_DrawAllOverlays).Offset(0x50).FindPatternSelf("3B 05", CMemory::Direction::DOWN, 150).ResolveRelativeAddressSelf(0x2, 0x6).RCast<int*>();
		g_nOverlayTickCount = CMemory(v_DebugOverlay_DrawAllOverlays).Offset(0x70).FindPatternSelf("3B 05", CMemory::Direction::DOWN, 150).ResolveRelativeAddressSelf(0x2, 0x6).RCast<int*>();

		g_nOverlayStage = CMemory(v_DebugOverlay_DebugDebugOverlays).Offset(0x70).FindPatternSelf("8B 05", CMemory::Direction::DOWN, 150).ResolveRelativeAddressSelf(0x2, 0x6).RCast<int*>();

		g_nNewOtherOverlays = CMemory(v_DebugOverlay_DebugDebugOverlays).Offset(0x1100).FindPatternSelf("44 89", CMemory::Direction::DOWN, 150).ResolveRelativeAddressSelf(0x3, 0x7).RCast<int*>();
		g_nNewTextOverlays = CMemory(v_DebugOverlay_DebugDebugOverlays).Offset(0x1104).FindPatternSelf("44 89", CMemory::Direction::DOWN, 150).ResolveRelativeAddressSelf(0x3, 0x7).RCast<int*>();

		g_GameDll.GetVirtualMethodTable(".?AVCIVDebugOverlay@@", 1).GetPtr(g_pIVPhysicsDebugOverlay_VFTable);
		g_GameDll.GetVirtualMethodTable(".?AVCIVDebugOverlay@@", 2).GetPtr(g_pIVDebugOverlay_VFTable);
#endif // CLIENT_DLL
	}
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////
