//=============================================================================//
//
// Purpose: Public interfaces to vphysics DLL
//
// $NoKeywords: $
//=============================================================================//
#ifndef VPHYSICS_INTERFACE_H
#define VPHYSICS_INTERFACE_H
#include "public/vphysics/vcollide.h"

#define VPHYSICS_COLLISION_INTERFACE_VERSION	"VPhysicsCollision007"

abstract_class IPhysicsCollision
{
public:
	virtual ~IPhysicsCollision(void) {}

private:
	// Unmapped collision vtable slots; keep for layout.
	virtual void PhysicsCollisionUnknown0() = 0;
	virtual void PhysicsCollisionUnknown1() = 0;
	virtual void PhysicsCollisionUnknown2() = 0;
	virtual void PhysicsCollisionUnknown3() = 0;
	virtual void PhysicsCollisionUnknown4() = 0;
	virtual void PhysicsCollisionUnknown5() = 0;
	virtual void PhysicsCollisionUnknown6() = 0;
	virtual void PhysicsCollisionUnknown7() = 0;
	virtual void PhysicsCollisionUnknown8() = 0;
	virtual void PhysicsCollisionUnknown9() = 0;
	virtual void PhysicsCollisionUnknown10() = 0;
	virtual void PhysicsCollisionUnknown11() = 0;

public:
	virtual void VCollideLoad(vcollide_t* const pOutput, const int numSolids, const char* const pBuffer) = 0;
	virtual void VCollideUnload(vcollide_t* const pVCollide) = 0;
};

abstract_class IVPhysicsDebugOverlay
{
public:
	virtual void AddPhysicsEntityTextOverlay(const int entIndex, const int lineOffset, const float duration, const int r, const int g, const int b, const int a, PRINTF_FORMAT_STRING const char* const format, ...) = 0;
	virtual void AddPhysicsBoxOverlay(const Vector3D& origin, const Vector3D& mins, const Vector3D& max, QAngle const& orientation, const int r, const int g, const int b, const int a, const float duration) = 0;
	virtual void AddPhysicsTriangleOverlay(const Vector3D& p1, const Vector3D& p2, const Vector3D& p3, const int r, const int g, const int b, const int a, const bool noDepthTest, const float duration) = 0;
	virtual void AddPhysicsLineOverlay(const Vector3D& origin, const Vector3D& dest, const int r, const int g, const int b, const bool noDepthTest, const float duration) = 0;
	virtual void AddPhysicsTextOverlay(const Vector3D& origin, const float duration, PRINTF_FORMAT_STRING const char* const format, ...) = 0;
	virtual void AddPhysicsTextOverlayAtOffset(const Vector3D& origin, const int lineOffset, const float duration, PRINTF_FORMAT_STRING const char* const format, ...) = 0;
	virtual void AddPhysicsScreenTextOverlay(const Vector2D& screenPos, const int lineOffset, const float flDuration, const int r, const int g, const int b, const int a, const char* const text) = 0;
	virtual void AddPhysicsSweptBoxOverlay(const Vector3D& start, const Vector3D& end, const Vector3D& mins, const Vector3D& max, const QAngle& angles, const int r, const int g, const int b, const int a, const float flDuration) = 0;
	virtual void AddPhysicsTextOverlayRGB(const Vector3D& origin, const int lineOffset, const float duration, const float r, const float g, const float b, const float alpha, PRINTF_FORMAT_STRING const char* const format, ...) = 0;
};

#endif // VPHYSICS_INTERFACE_H
