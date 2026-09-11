#if defined(CLIENT_DLL)
//====== Copyright © 1996-2005, Valve Corporation, All rights reserved. =======//
//
// Purpose
//
//=============================================================================//

#ifndef GAMETRACE_H
#define GAMETRACE_H
#ifdef _WIN32
#pragma once
#endif
#include "trace.h"
#include "cmodel.h"

// !TODO: Remove these and include properly.
class C_BaseEntity;
//-----------------------------------------------------------------------------
// Purpose: A trace is returned when a box is swept through the world
//-----------------------------------------------------------------------------
class CGameTrace : public CBaseTrace
{
public:
	char gap3A[0x4];
	csurface_t surface;
	float fractionleftsolid;
	int hitgroup;
	short physicsBone;
	char gap5A[0x6];
	C_BaseEntity* hit_entity;
	int hitbox;
	// staticPropID @ +0x6C: sprp index, or -1 if the ray missed a static prop.
	int staticPropID;
	char gap70[0x110];
};
static_assert(sizeof(CGameTrace) == 0x180);
typedef CGameTrace trace_t;

//-----------------------------------------------------------------------------
// Purpose: A collision query
//-----------------------------------------------------------------------------
struct CollisionQuery_t
{
	bspmodel_t* pModel;
	char* pVertices;
	char unk_10[16];
	int trace_contents;
	char unk[556];
	void** func_table;
	void* funcs_250[6];
	char unk_288[80];
	trace_t* trace;
	char unk_2D8[54];
};
static_assert(sizeof(CollisionQuery_t) == 0x318);

#endif // GAMETRACE_H
#else // !CLIENT_DLL
//====== Copyright © 1996-2005, Valve Corporation, All rights reserved. =======//
//
// Purpose
//
//=============================================================================//

#ifndef GAMETRACE_H
#define GAMETRACE_H
#ifdef _WIN32
#pragma once
#endif
#include "trace.h"
#include "cmodel.h"

// !TODO: Remove these and include properly.
class CBaseEntity;
//-----------------------------------------------------------------------------
// Purpose: A trace is returned when a box is swept through the world
//-----------------------------------------------------------------------------
class CGameTrace : public CBaseTrace
{
public:
	char gap3A[0x4];
	csurface_t surface;
	float fractionleftsolid;
	int hitgroup;
	short physicsBone;
	char gap5A[0x6];
	CBaseEntity* hit_entity;
	int hitbox;
	char gap6C[0x114];
};
static_assert(sizeof(CGameTrace) == 0x180);
typedef CGameTrace trace_t;

//-----------------------------------------------------------------------------
// Purpose: A collision query
//-----------------------------------------------------------------------------
struct CollisionQuery_t
{
	bspmodel_t* pModel;
	char* pVertices;
	char unk_10[16];
	int trace_contents;
	char unk[556];
	void** func_table;
	void* funcs_250[6];
	char unk_288[80];
	trace_t* trace;
	char unk_2D8[54];
};
static_assert(sizeof(CollisionQuery_t) == 0x318);

#endif // GAMETRACE_H
#endif // CLIENT_DLL
