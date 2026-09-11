#ifndef TIER2_RENDERUTILS_H
#define TIER2_RENDERUTILS_H
#include "mathlib/vector.h"
#include "materialsystem/cmatqueuedrendercontext.h"

class IMaterial;
class CMaterialSystem;

void DebugDrawBox(const Vector3D& vOrigin, const QAngle& vAngles, const Vector3D& vMins, const Vector3D& vMaxs, Color color, bool bZBuffer = true);
void DebugDrawCylinder(const Vector3D& vOrigin, const QAngle& vAngles, const float flRadius, const float flHeight, const Color color, const int nSides = 16, const bool bZBuffer = true);
void DebugDrawSphere(const Vector3D& vOrigin, const float flRadius, const Color color, const int nSegments = 16, const bool bZBuffer = true);
void DebugDrawHemiSphere(const Vector3D& vOrigin, const QAngle& vAngles, const Vector3D& vRadius, const Color color, const int nSegments = 8, const bool bZBuffer = true);
void DebugDrawCircle(const Vector3D& vOrigin, const QAngle& vAngles, const float flRadius, const Color color, const int nSegments = 16, const bool bZBuffer = true);
void DebugDrawSquare(const Vector3D& vOrigin, const QAngle& vAngles, const float flSquareSize, const Color color, const bool bZBuffer = true);
void DebugDrawTriangle(const Vector3D& vOrigin, const QAngle& vAngles, const float flTriangleSize, const Color color, const bool bZBuffer = true);
void DebugDrawMark(const Vector3D& vOrigin, const float flRadius, const Color c, const bool bZBuffer = true);
void DrawStar(const Vector3D& vRrigin, const float flRadius, const bool bZBuffer = true);
void DebugDrawArrow(const Vector3D& vOrigin, const Vector3D& vEnd, const float flArraySize, const Color color, const bool bZBuffer = true);
void DebugDrawAxis(const Vector3D& vOrigin, const QAngle& vAngles = { 0, 0, 0 }, const float flScale = 50.f, const bool bZBuffer = true);

///////////////////////////////////////////////////////////////////////////////
void RenderLine(const Vector3D& v1, const Vector3D& v2, Color color, bool bZBuffer);
void RenderBox(const matrix3x4_t& vTransforms, const Vector3D& vMins, const Vector3D& vMaxs, const Color c, bool bZBuffer);
void RenderWireframeBox(const matrix3x4_t& vTransforms, const Vector3D& vMins, const Vector3D& vMaxs, const Color c, bool bZBuffer);
void RenderWireframeSweptBox(const Vector3D& vStart, const Vector3D& vEnd, const QAngle& angles,
	const Vector3D& vMins, const Vector3D& vMaxs, const Color c, const bool bZBuffer);
void RenderTriangle(const Vector3D& p1, const Vector3D& p2, const Vector3D& p3, Color c, const bool bZBuffer);
void RenderSphere(const Vector3D& vCenter, const float flRadius, const int nTheta, const int nPhi, const Color c, const bool bZBuffer);
void RenderWireframeSphere(const Vector3D& vCenter, const float flRadius, const int nTheta, const int nPhi, const Color c, const bool bZBuffer);
void RenderCapsule(const Vector3D& vStart, const Vector3D& vEnd, const float flRadius, const Color c, const bool bZBuffer);

///////////////////////////////////////////////////////////////////////////////
inline void(*v_InitializeStandardMaterials)();

inline void* (*v_RenderWireframeBox)(const matrix3x4_t& vTransforms, const Vector3D& vMins, const Vector3D& vMaxs, Color color, bool bZBuffer);
inline void* (*v_RenderWireframeSphere)(const Vector3D& vCenter, float flRadius, int nTheta, int nPhi, Color color, bool bZBuffer);
inline void* (*v_RenderLine)(const Vector3D& vOrigin, const Vector3D& vDest, Color color, bool bZBuffer);
inline IMaterial** s_engineWireMaterialSlots[4] = {};
inline CMaterialSystem** s_engineMaterialSystemSlot = nullptr;

///////////////////////////////////////////////////////////////////////////////
class V_RenderUtils : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("InitializeStandardMaterials", v_InitializeStandardMaterials);
		LogFunAdr("RenderWireframeBox", v_RenderWireframeBox);
		LogFunAdr("RenderWireframeSphere", v_RenderWireframeSphere);
		LogFunAdr("RenderLine", v_RenderLine);
	}
	virtual void GetFun(void) const
	{
#if defined(CLIENT_DLL)
		Module_FindPattern(g_GameDll, "48 83 EC ? 65 48 8B 04 25 ? ? ? ? BA ? ? ? ? 48 8B 08 8B 04 0A 39 05 ? ? ? ? 0F 8F ? ? ? ? 48 8D 0D").GetPtr(v_InitializeStandardMaterials);
		Module_FindPattern(g_GameDll, "48 89 6C 24 ?? 48 89 74 24 ?? 44 89 44 24 ?? 57 41 56 41 57 48 83 EC 60 41 0F B6 E9 41 8B F8 4C 8B F2 4C 8B F9 E8 ?? ?? ?? ?? FF 15").GetPtr(v_RenderLine);

		if (!v_InitializeStandardMaterials)
			Warning(eDLL_T::MS, "[DBGDRAW] InitializeStandardMaterials unresolved\n");

		if (!v_RenderLine)
			Warning(eDLL_T::MS, "[DBGDRAW] native RenderLine unresolved\n");
#else
		Module_FindPattern(g_GameDll, "48 83 EC ? 65 48 8B 04 25 ? ? ? ? BA ? ? ? ? 48 8B 08 8B 04 0A 39 05 ? ? ? ? 0F 8F ? ? ? ? 48 8D 0D").GetPtr(v_InitializeStandardMaterials);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 6C 24 ?? 44 89 4C 24 ??").GetPtr(v_RenderWireframeBox);
		Module_FindPattern(g_GameDll, "40 56 41 54 41 55 48 81 EC ?? ?? ?? ??").GetPtr(v_RenderWireframeSphere);
		Module_FindPattern(g_GameDll, "48 89 74 24 ?? 44 89 44 24 ?? 57 41 56").GetPtr(v_RenderLine);
#endif // CLIENT_DLL
	}
	virtual void GetVar(void) const
	{
#if defined(CLIENT_DLL)
		if (v_InitializeStandardMaterials)
		{
			// Four wire materials, rip-relative stores, must resolve before the detour trampoline.
			const CMemory base(v_InitializeStandardMaterials);

			for (int i = 0; i < 4; i++)
			{
				const CMemory storeSite = base.FindPattern("48 89 05", CMemory::Direction::DOWN, 0x200, i + 1);
				s_engineWireMaterialSlots[i] = storeSite.ResolveRelativeAddress(0x3, 0x7).RCast<IMaterial**>();
			}
		}
		else
		{
			Warning(eDLL_T::MS, "[DBGDRAW] InitializeStandardMaterials unresolved; cannot bind standard materials\n");
		}

		if (v_RenderLine)
		{
			// RenderLine: three call-queue FF 15s, then the material-system rip-relative load.
			const CMemory base(v_RenderLine);

			base.FindPattern("FF 15", CMemory::Direction::DOWN, 0x220, 1).ResolveRelativeAddress(0x2, 0x6).GetPtr(g_fnHasRenderCallQueue);
			base.FindPattern("FF 15", CMemory::Direction::DOWN, 0x220, 2).ResolveRelativeAddress(0x2, 0x6).GetPtr(g_fnAddRenderCallQueueItem);
			base.FindPattern("FF 15", CMemory::Direction::DOWN, 0x220, 3).ResolveRelativeAddress(0x2, 0x6).GetPtr(g_fnAdvanceRenderCallQueue);
			base.FindPattern("48 8B 0D", CMemory::Direction::DOWN, 0x220, 1).ResolveRelativeAddress(0x3, 0x7).GetPtr(s_engineMaterialSystemSlot);
		}

		if (!s_engineWireMaterialSlots[0] || !s_engineWireMaterialSlots[1] ||
			!s_engineWireMaterialSlots[2] || !s_engineWireMaterialSlots[3])
		{
			Warning(eDLL_T::MS, "[DBGDRAW] standard wire material slots unresolved: %p %p %p %p\n",
				s_engineWireMaterialSlots[0], s_engineWireMaterialSlots[1],
				s_engineWireMaterialSlots[2], s_engineWireMaterialSlots[3]);
		}

		if (!g_fnHasRenderCallQueue || !g_fnAddRenderCallQueueItem ||
			!g_fnAdvanceRenderCallQueue || !s_engineMaterialSystemSlot)
		{
			Warning(eDLL_T::MS, "[DBGDRAW] render call-queue globals unresolved: has=%p add=%p advance=%p matsys=%p\n",
				g_fnHasRenderCallQueue, g_fnAddRenderCallQueueItem,
				g_fnAdvanceRenderCallQueue, s_engineMaterialSystemSlot);
		}
#endif // CLIENT_DLL
	}
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // TIER2_RENDERUTILS_H
