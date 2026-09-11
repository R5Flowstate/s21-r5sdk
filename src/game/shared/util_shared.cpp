//===== Copyright © 1996-2005, Valve Corporation, All rights reserved. ======//
//
// Purpose
//
//===========================================================================//
#include "core/stdafx.h"
#include "util_shared.h"

//-----------------------------------------------------------------------------
// Purpose: returns the class name, script name, and edict of the entity
// returns "<<null>>" on NULL entity
//-----------------------------------------------------------------------------
const char* UTIL_GetEntityScriptInfo(CBaseEntity* pEnt)
{
	assert(pEnt != nullptr);
	return v_UTIL_GetEntityScriptInfo(pEnt);
}

CTraceFilterSimple::CTraceFilterSimple(const IHandleEntity* pPassEntity, const int collisionGroup, ShouldHitFunc_t pExtraShouldHitCheckFn)
{
	m_reserved = 0;
	m_pPassEntity = pPassEntity;
	m_collisionGroup = collisionGroup;
	m_pExtraShouldHitCheckFunction = pExtraShouldHitCheckFn;
}

bool CTraceFilterSimple::ShouldHitEntity(IHandleEntity* const pEntity, const int contentsMask)
{
	// Engine helper arg3 is the other entity for ShouldCollide, not extra.
	if (!v_TraceFilter_ShouldHitEntity(pEntity, m_pPassEntity, nullptr, contentsMask, m_collisionGroup))
		return false;

	if (m_pExtraShouldHitCheckFunction)
		return m_pExtraShouldHitCheckFunction(pEntity, contentsMask);

	return true;
}

bool CTraceFilterSimple::ShouldBlockTrace(trace_t* const pTrace)
{
	NOTE_UNUSED(pTrace);
	return false;
}
