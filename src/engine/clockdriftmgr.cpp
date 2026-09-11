//===== Copyright (c) 1996-2005, Valve Corporation, All rights reserved. ======//
//
// Purpose
//
// $Workfile: $
// $Date: $
//
//------------------------------------------------------------------------------
// $Log: $
//
// $NoKeywords: $
//=============================================================================//

#include "core/stdafx.h"
#include "engine/clockdriftmgr.h"

void CClockDriftMgr::Clear()
{
	m_nServerTick = 0;
	m_nClientTick = 0;
	m_iCurClockOffset = 0;
	memset(m_ClockOffsets, 0, sizeof(m_ClockOffsets));
}

