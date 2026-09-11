//======== Copyright (c) Valve Corporation, All rights reserved. ============//
//
// Purpose
//
//===========================================================================//
#include "recipientfilter.h"
#include "util_server.h"

CRecipientFilter::CRecipientFilter()
{
	Reset();
}

CRecipientFilter::~CRecipientFilter()
{
}

void CRecipientFilter::Reset()
{
	m_bReliable		= false;
	m_bInitMessage	= false;
	m_Recipients.RemoveAll();
	m_bUsingPredictionRules = false;
	m_bIgnorePredictionCull = false;
}

bool CRecipientFilter::IsReliable(void) const
{
	return m_bReliable;
}

void CRecipientFilter::MakeReliable( void )
{
	m_bReliable = true;
}

bool CRecipientFilter::IsInitMessage( void ) const
{
	return m_bInitMessage;
}

int CRecipientFilter::GetRecipientCount( void ) const
{
	return m_Recipients.Count();
}

int CRecipientFilter::GetRecipientIndex( int nSlot ) const
{
	if ( nSlot < 0 || nSlot >= GetRecipientCount() )
		return -1;

	return m_Recipients[nSlot].m_nIndex;
}

int	CRecipientFilter::FindSlotForIndex( int nIndex ) const
{
	FOR_EACH_VEC(m_Recipients, slot)
	{
		if (m_Recipients[slot].m_nIndex == nIndex)
		{
			return slot;
		}
	}

	return m_Recipients.InvalidIndex();
}

bool CRecipientFilter::IsLocalPlayer( int nSlot ) const
{
	Assert( nSlot >= 0 && nSlot < GetRecipientCount() );
	return m_Recipients[nSlot].m_bIsLocalPlayer;
}

int CRecipientFilter::DistTo( int nSlot, const Vector3D& pos ) const
{
	Assert(nSlot >= 0 && nSlot < GetRecipientCount());

	const CPlayer* pPlayer = UTIL_PlayerByIndex(m_Recipients[nSlot].m_nIndex);
	const Vector3D origin = pPlayer->GetViewOffset() + pPlayer->GetVecPrevAbsOrigin();
	const vec_t distance = origin.DistTo(pos);

	return distance > 65535.f ? 65535 : static_cast<int>(distance);
}
