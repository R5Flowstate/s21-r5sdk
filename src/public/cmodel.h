//====== Copyright © 1996-2005, Valve Corporation, All rights reserved. =======//
//
// Purpose
//
// $Workfile: $
// $Date: $
// $NoKeywords: $
//=============================================================================//

#ifndef CMODEL_H
#define CMODEL_H
#ifdef _WIN32
#pragma once
#endif
#include "mathlib/mathlib.h"

// S21 Ray_t layout: sizeof == 0x70. m_Extents.w is a derived term
// (extents.z - extents.x) the sweep relies on; a hull ray built by hand must
// set it or the collision walk runs on a bad bound.
struct Ray_t
{
	VectorAligned m_Start;
	VectorAligned m_Delta;
	VectorAligned m_StartOffset;
	VectorAligned m_Extents;  // half-extents; .w = m_Extents.z - m_Extents.x
	VectorAligned m_upDir;    // hull orientation axis, (0,0,1) for an axis-aligned box
	const matrix3x4_t* m_pWorldAxisTransform;
	float m_flHitboxRadius;
	bool m_IsRay;
	bool m_IsSwept;
	int m_nSolidType;
	int m_nDetailLevel;       // TraceDetailLevel: 0 NORMAL, 1 HIGH_AT_RAY_START, 2 HIGH

	Ray_t() : m_pWorldAxisTransform(nullptr) {};
	// nFlags1/nFlags2 keep their historical names at the call sites; they are really
	// m_upDir.z (0x3f800000 == 1.0f, the world-up default) and m_nSolidType.
	Ray_t(Vector3D const& start, Vector3D const& end, int nFlags1 = 0x3f800000, int nFlags2 = NULL) { Init(start, end, nFlags1, nFlags2); };

	void Init(Vector3D const& start, Vector3D const& end, int nFlags1, int nFlags2)
	{
		VectorSubtract(end, start, m_Delta);
		m_IsSwept = (m_Delta.LengthSqr() != 0);

		m_Extents.x = 0.0f;
		m_Extents.y = 0.0f;
		m_Extents.z = 0.0f;
		m_Extents.w = 0.0f;

		m_upDir.x = 0.0f;
		m_upDir.y = 0.0f;
		*reinterpret_cast<int*>(&m_upDir.z) = nFlags1;
		m_upDir.w = 0.0f;

		m_pWorldAxisTransform = nullptr;
		m_IsRay = true;

		m_flHitboxRadius = 0.0f;
		m_nSolidType = nFlags2;
		m_nDetailLevel = NULL;

		VectorClear(m_StartOffset);
		VectorCopy(start, m_Start);
	}
};
static_assert(sizeof(Ray_t) == 0x70);
static_assert(offsetof(Ray_t, m_Extents) == 0x30);
static_assert(offsetof(Ray_t, m_upDir) == 0x40);
static_assert(offsetof(Ray_t, m_IsRay) == 0x5C);
static_assert(offsetof(Ray_t, m_nDetailLevel) == 0x64);

struct csurface_t
{
	const char* name;
	unsigned char surfaceProp;
	uint16_t flags;
};

struct cplanetrace_t
{
	Vector3D normal;
	float dist;
};

struct dsurfaceproperty_t
{
	__int16 unk;
	byte unk2;
	byte contentMaskOffset;
	int surfaceNameOffset;
};

struct dbvhnode_t
{
	__int16 mins_x[4];
	__int16 mins_y[4];
	__int16 mins_z[4];
	__int16 maxs_x[4];
	__int16 maxs_y[4];
	__int16 maxs_z[4];
	int index_child01_types;
	int index_child23_types;
	int index_cm;
	int index_pad;
};

struct bspmodel_t
{
	dbvhnode_t* bvhnodes;
	dsurfaceproperty_t* surfaceproperties;
	int* bvhleafdata;
	void* vertices;
	int* contentmasks;
	char* texdatastringdata;
	int unk;
	int unk2;
	float unk_f1;
	float unk_f2;
	float unk_f3;
	float unk_f4;
};

#endif // CMODEL_H
