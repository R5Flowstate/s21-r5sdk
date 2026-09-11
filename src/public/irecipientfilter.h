//========= Copyright (c) 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose
//
// $NoKeywords: $
//=============================================================================//

#ifndef IRECIPIENTFILTER_H
#define IRECIPIENTFILTER_H

//-----------------------------------------------------------------------------
// Purpose: Generic interface for routing messages to users
//-----------------------------------------------------------------------------
class IRecipientFilter
{
public:
	virtual			~IRecipientFilter() {}

	virtual bool	IsReliable(void) const = 0;
	virtual void	MakeReliable(void) = 0;

	virtual bool	IsInitMessage(void) const = 0;

	virtual int		GetRecipientCount(void) const = 0;
	virtual int		GetRecipientIndex(int nSlot) const = 0;

	virtual bool	IsLocalPlayer(int nSlot) const = 0;
	virtual int		DistTo(int nSlot, const Vector3D& pos) const = 0;
};

#endif // IRECIPIENTFILTER_H
