//===== Copyright © 1996-2005, Valve Corporation, All rights reserved. ======//
//
// Purpose: Debug overlay engine interface.
//
//===========================================================================//
//
///////////////////////////////////////////////////////////////////////////////
#ifndef IDEBUGOVERLAY_H
#define IDEBUGOVERLAY_H

#include "mathlib/vector.h"

class OverlayText_t;

class IVDebugOverlay
{
public:
	virtual void AddEntityTextOverlay(const int entIndex, const int lineOffset, const float duration, const int r, const int g, const int b, const int a, PRINTF_FORMAT_STRING const char* const format, ...) = 0;
	virtual void AddTransformedBoxOverlay(const matrix3x4_t& vTransforms, const Vector3D& vMins, const Vector3D& vMaxs, const int r, const int g, const int b, const int a, const bool noDepthTest, const float flDuration) = 0;
	virtual void AddBoxOverlay(const Vector3D& VOrigin, const Vector3D& vMins, const Vector3D& vMaxs, const int r, const int g, const int b, const int a, const bool noDepthTest, const float flDuration) = 0;
	virtual void AddSphereOverlay(const Vector3D& vOrigin, const float flRadius, const int nTheta, const int nPhi, const int r, const int g, const int b, const int a, const bool noDepthTest, const float flDuration) = 0;
	virtual void AddTriangleOverlay(const Vector3D& p1, const Vector3D& p2, const Vector3D& p3, const int r, const int g, const int b, const int a, const bool noDepthTest, const float flDuration) = 0;
	virtual void AddLineOverlay(const Vector3D& vStart, const Vector3D& vEnd, const int r, const int g, const int b, const bool noDepthTest, const float flDuration) = 0;
	virtual void AddSplineOverlay(const Vector3D& vStart, const Vector3D& vEnd, const int r, const int g, const int b, const bool noDepthTest, const float flDuration) = 0;

	virtual void stub_0() = 0; // Appears to adds a 128 * matrix3x4_t overlay, but this is never used anywhere in the game code and the renderer doesn't exist. Stubbed for now.

	virtual void AddTextOverlayAtOffset(const Vector3D& origin, const int lineOffset, const float duration, PRINTF_FORMAT_STRING const char* const format, ...) FMTFUNCTION(5, 6) = 0;
	virtual void AddTextOverlay(const Vector3D& origin, const float duration, PRINTF_FORMAT_STRING const char* const format, ...) FMTFUNCTION(4, 5) = 0;

	// Also a AddScreenTextOverlay function, but never used in the engine and gutted so its impossible to know the parameters.
	// Since this isn't used, it will be replaced with a new implementation in the future.
	virtual void AddScreenTextOverlayAtOffset(const float flXPos, const float flYPos, const int lineOffset, const float flDuration, const int r, const int g, const int b, const int a, const char* const text) = 0;
	virtual void AddScreenTextOverlay(const float flXPos, const float flYPos, const float flDuration, const int r, const int g, const int b, const int a, const char* const text) = 0;

	virtual void AddSweptBoxOverlay(const Vector3D& start, const Vector3D& end, const Vector3D& mins, const Vector3D& max, const QAngle& angles, const int r, const int g, const int b, const int a, const bool noDepthTest, const float flDuration) = 0;
	virtual void AddGridOverlay(const Vector3D& origin) = 0;
	virtual void AddCoordFrameOverlay(const matrix3x4_t& frame, float flScale, int vColorTable[3][3] = NULL) = 0;

	virtual OverlayText_t* GetFirstText(void) = 0;
	virtual OverlayText_t* GetNextText(const OverlayText_t* const current) = 0;

	virtual void stub_2() = 0; // Something with text overlays, checks creation and overlay ticks, reverse this.

	virtual void ClearDeadTextOverlays(void) = 0;
	virtual void ClearAllOverlays(void) = 0;

	virtual void stub_3() = 0; // This is DebugDebugOverlays, parameters need reversing but that can only be done once this entire interface is mapped out.
	virtual void stub_4() = 0; // Increments 'g_nOverlayStage' if not paused and when a2 > 0.
	virtual void stub_5() = 0; // Sets 'g_nRenderTickCount' to 'g_ClientGlobalVariables.frameCount' if not paused.

	virtual bool DebugDebugOverlaysEnabled(void) const = 0;

	virtual void AddTextOverlayRGBu32(const Vector3D& origin, const int lineOffset, const float duration, const int r, const int g, const int b, const int a, PRINTF_FORMAT_STRING const char* const format, ...) FMTFUNCTION(9, 10) = 0;
	virtual void AddTextOverlayRGBf32(const Vector3D& origin, const int lineOffset, const float duration, const float r, const float g, const float b, const float a, PRINTF_FORMAT_STRING const char* const format, ...) FMTFUNCTION(9, 10) = 0;

	virtual void AddLineOverlayWithAlpha(const Vector3D& vStart, const Vector3D& vEnd, const int r, const int g, const int b, const int a, const bool noDepthTest, const float flDuration) = 0;
	virtual void AddCapsuleOverlay(const Vector3D& vStart, const Vector3D& vEnd, const float flRadius, const int r, const int g, const int b, const int a, const bool noDepthTest, const float flDuration) = 0;
};

#endif // IDEBUGOVERLAY_H
