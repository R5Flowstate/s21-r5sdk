//====== Copyright © 1996-2005, Valve Corporation, All rights reserved. =======//
//
// Purpose
//
// $NoKeywords: $
//=============================================================================//

#ifndef ENGINE_ICOLLIDEABLE_H
#define ENGINE_ICOLLIDEABLE_H
#ifdef _WIN32
#pragma once
#endif

#include "tier0/platform.h"

class IHandleEntity;
struct model_t;

struct WorldSpaceTriggerBounds_s
{
	float triggerBloat;
	Vector3D vecWorldMins;
	Vector3D vecWorldMaxs;
};

abstract_class ICollideable
{
public:
	// Gets at the entity handle associated with the collideable
	virtual IHandleEntity* GetEntityHandle( ) = 0;

	// These methods return the bounds of an OBB measured in "collision" space
	// which can be retreived through the CollisionToWorldTransform or
	// GetCollisionOrigin/GetCollisionAngles methods
	virtual const Vector3D&	OBBMins( ) const = 0;
	virtual const Vector3D&	OBBMaxs( ) const = 0;

	virtual void WorldSpaceAABB( WorldSpaceTriggerBounds_s* const wstb ) const = 0;
	virtual void stub_0() const { }; // unknown.

	// Returns the hit box test radius.
	virtual float GetHitBoxTestRadius( ) const = 0;

	// Returns the bounds of a world-space box used when the collideable is being traced
	// against as a trigger. It's only valid to call these methods if the solid flags
	// have the FSOLID_USE_TRIGGER_BOUNDS flag set.
	virtual void WorldSpaceTriggerBounds( WorldSpaceTriggerBounds_s* const wstb ) const = 0;
	virtual void WorldSpaceTriggerBounds( Vector3D* const pVecWorldMins, Vector3D* const pVecWorldMaxs ) const = 0;

	virtual void stub_1( ) const { }; // unknown.
	virtual void stub_2( ) const { }; // unknown.
	virtual void stub_3( ) const { }; // unknown.
	virtual void stub_4( ) const { }; // unknown.
	virtual void stub_5( ) const { }; // unknown.

	// Returns the BRUSH model index if this is a brush model. Otherwise, returns -1.
	virtual int				GetCollisionModelIndex() const = 0;

	// Return the model, if it's a studio model.
	virtual const model_t* GetCollisionModel( ) const = 0;

	// Get angles and origin.
	virtual const Vector3D& GetCollisionOrigin( ) const = 0;
	virtual const QAngle& GetCollisionAngles( QAngle& out ) const = 0;
	virtual const matrix3x4_t& CollisionToWorldTransform( ) const = 0;

	// TODO: figure out the rest !!!
};

#endif // ENGINE_ICOLLIDEABLE_H
