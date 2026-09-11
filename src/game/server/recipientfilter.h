#if defined(CLIENT_DLL)
//========= Copyright (c) 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose
//
// $NoKeywords: $
//=============================================================================//

#ifndef RECIPIENTFILTER_H
#define RECIPIENTFILTER_H
#include "player.h"
#include "irecipientfilter.h"

inline void(*CRecipientFilter__AddRecipient)(CRecipientFilter* thisp, const CPlayer* pPlayer);
inline void(*CRecipientFilter__RemoveRecipient)(CRecipientFilter* thisp, const CPlayer* pPlayer);

//-----------------------------------------------------------------------------
// Purpose: A generic filter for determining whom to send message/sounds etc. to and
// providing a bit of additional state information
//-----------------------------------------------------------------------------
class CRecipientFilter : public IRecipientFilter
{
	struct Recipient_s
	{
		int m_nIndex;
		bool m_bIsLocalPlayer;
	};

	static_assert( sizeof( Recipient_s ) == 0x8 );

public:
						CRecipientFilter();
	virtual			~CRecipientFilter();

	virtual bool	IsReliable( void ) const;
	virtual void	MakeReliable( void );

	virtual bool	IsInitMessage( void ) const;

	virtual int		GetRecipientCount( void ) const;
	virtual int		GetRecipientIndex( int nSlot ) const;

	virtual bool	IsLocalPlayer( int nSlot ) const;
	virtual int		DistTo( int nSlot, const Vector3D& pos ) const;

	void			    Reset( void );
	int				FindSlotForIndex( int nIndex ) const;

	FORCEINLINE void	AddRecipient( const CPlayer* pPlayer )
	{
		CRecipientFilter__AddRecipient(this, pPlayer);
	}

	FORCEINLINE void	RemoveRecipient( const CPlayer* pPlayer )
	{
		CRecipientFilter__RemoveRecipient(this, pPlayer);
	}

private:
	bool m_bReliable;
	bool m_bInitMessage;
	CUtlVector<Recipient_s> m_Recipients;
	bool m_bUsingPredictionRules;
	bool m_bIgnorePredictionCull;
};

static_assert(sizeof(CRecipientFilter) == 0x38);

class CSingleUserRecipientFilter :  public CRecipientFilter
{
public:
	CSingleUserRecipientFilter(const CPlayer* pPlayer)
	{
		AddRecipient(pPlayer);
	}
};

class VRecipientFilter : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CRecipientFilter::AddRecipient", CRecipientFilter__AddRecipient);
		LogFunAdr("CRecipientFilter::RemoveRecipient", CRecipientFilter__RemoveRecipient);
	}
	virtual void GetFun(void) const 
	{
		Module_FindPattern(g_GameDll, "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 0F BF 42 ? 33 F6").GetPtr(CRecipientFilter__AddRecipient);
		Module_FindPattern(g_GameDll, "44 0F BF 42 ? 33 C0").GetPtr(CRecipientFilter__RemoveRecipient);
	};
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const { };
};

#endif
#else // !CLIENT_DLL
//========= Copyright (c) 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose
//
// $NoKeywords: $
//=============================================================================//

#ifndef RECIPIENTFILTER_H
#define RECIPIENTFILTER_H
#include "player.h"
#include "irecipientfilter.h"
#include "util_server.h"
#include "public/eiface.h"   // gpGlobals

inline void(*CRecipientFilter__AddRecipient)(CRecipientFilter* thisp, const CPlayer* pPlayer);
inline void(*CRecipientFilter__RemoveRecipient)(CRecipientFilter* thisp, const CPlayer* pPlayer);

//-----------------------------------------------------------------------------
// Purpose: A generic filter for determining whom to send message/sounds etc. to and
// providing a bit of additional state information
//-----------------------------------------------------------------------------
class CRecipientFilter : public IRecipientFilter
{
	struct Recipient_s
	{
		int m_nIndex;
		bool m_bIsLocalPlayer;
	};

	static_assert( sizeof( Recipient_s ) == 0x8 );

public:
						CRecipientFilter();
	virtual			~CRecipientFilter();

	virtual bool	IsReliable( void ) const;
	virtual void	MakeReliable( void );

	virtual bool	IsInitMessage( void ) const;

	virtual int		GetRecipientCount( void ) const;
	virtual int		GetRecipientIndex( int nSlot ) const;

	virtual bool	IsLocalPlayer( int nSlot ) const;
	virtual int		DistTo( int nSlot, const Vector3D& pos ) const;

	void			    Reset( void );
	int				FindSlotForIndex( int nIndex ) const;

	FORCEINLINE void	AddRecipient( const CPlayer* pPlayer )
	{
		CRecipientFilter__AddRecipient(this, pPlayer);
	}

	FORCEINLINE void	RemoveRecipient( const CPlayer* pPlayer )
	{
		CRecipientFilter__RemoveRecipient(this, pPlayer);
	}

private:
	bool m_bReliable;
	bool m_bInitMessage;
	CUtlVector<Recipient_s> m_Recipients;
	bool m_bUsingPredictionRules;
	bool m_bIgnorePredictionCull;
};

static_assert(sizeof(CRecipientFilter) == 0x38);

class CSingleUserRecipientFilter :  public CRecipientFilter
{
public:
	CSingleUserRecipientFilter(const CPlayer* pPlayer)
	{
		AddRecipient(pPlayer);
	}
};

//-----------------------------------------------------------------------------
// Sends to every connected client. Used by L2 S->C broadcast (e.g. replicated
// state updates that all clients should observe).
//-----------------------------------------------------------------------------
class CBroadcastRecipientFilter : public CRecipientFilter
{
public:
	CBroadcastRecipientFilter()
	{
		const int nMaxClients = gpGlobals ? gpGlobals->maxClients : 0;
		for (int i = 1; i <= nMaxClients; i++)
		{
			const CPlayer* const p = UTIL_PlayerByIndex(i);
			if (p && p->IsConnected())
				AddRecipient(p);
		}
	}
};

class CReliableBroadcastRecipientFilter : public CBroadcastRecipientFilter
{
public:
	CReliableBroadcastRecipientFilter()
	{
		MakeReliable();
	}
};

//-----------------------------------------------------------------------------
// Sends to every connected client whose team matches teamNum. Used by L2 S->C
// team-scoped broadcast (e.g. squad/team-private replicated state).
//-----------------------------------------------------------------------------
class CTeamRecipientFilter : public CRecipientFilter
{
public:
	CTeamRecipientFilter(int teamNum, bool bReliable = true)
	{
		const int nMaxClients = gpGlobals ? gpGlobals->maxClients : 0;
		for (int i = 1; i <= nMaxClients; i++)
		{
			const CPlayer* const p = UTIL_PlayerByIndex(i);
			if (p && p->IsConnected() && p->GetTeamNum() == teamNum)
				AddRecipient(p);
		}
		if (bReliable)
			MakeReliable();
	}
};

class VRecipientFilter : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CRecipientFilter::AddRecipient", CRecipientFilter__AddRecipient);
		LogFunAdr("CRecipientFilter::RemoveRecipient", CRecipientFilter__RemoveRecipient);
	}
	virtual void GetFun(void) const 
	{
		Module_FindPattern(g_GameDll, "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 0F BF 42 ? 33 F6").GetPtr(CRecipientFilter__AddRecipient);
		Module_FindPattern(g_GameDll, "44 0F BF 42 ? 33 C0").GetPtr(CRecipientFilter__RemoveRecipient);
	};
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const { };
};

#endif
#endif // CLIENT_DLL
