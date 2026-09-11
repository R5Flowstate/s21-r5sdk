//=============================================================================//
//
// Purpose: Client join-token mint at the single connect chokepoint.
//
//=============================================================================//
#ifndef BRIDGE_JOIN_AUTH_H
#define BRIDGE_JOIN_AUTH_H

// Returns true when the connect may proceed. reasonBuf carries the refusal text on
// false. Idempotent within bridge_join_token_ttl for the same host.
bool Bridge_EnsureJoinToken(const char* host, char* reasonBuf, size_t reasonBufLen);

// True when this launch cannot authenticate onto a public server (-offline,
// -noorigin, or cl_onlineAuthEnable 0). Loopback Play Local still proceeds.
bool Bridge_IsOfflineMultiplayerBlocked(void);

// Instant UI error (lobby OK dialog). No-op if the UI VM is not up yet.
void Bridge_ShowClientError(void);

// True when the last mint failure was a parked sign-in, not a refusal.
bool Bridge_JoinAuthWasDeferred(void);

// True when a failed mint should block the connect locally.
bool Bridge_JoinAuthBlocksOnFailure(void);

// One-line summary of what the next connect will present. Never prints the token.
void Bridge_LogJoinAuthState(const char* host);

// Strip optional [brackets] and :port; writes into out and returns out (or "").
const char* Bridge_HostBase(const char* host, char* const out, const size_t outLen);

// True for localhost / 127.0.0.0/8 / ::1 after Bridge_HostBase normalisation.
bool Bridge_IsTrueLoopbackHost(const char* host);

// Flush a connect queued by bridge_connect. Call at Cbuf_Execute entry so
// it runs outside the UI ClientCommand CLIENTCMD marker window.
void Bridge_PumpDeferredConnect(void);

// Hold a connect until Origin identity is real (launcher +connect races it).
void Bridge_ParkConnect(const char* host);

// True when this host is already parked, or was just dispatched.
bool Bridge_ShouldSuppressConnect(const char* host);

// Mark that a connect for this host was handed to the engine.
void Bridge_NoteConnectDispatched(const char* host);

// Drop the same-host swallow so a post-timeout `connect` is not eaten.
void Bridge_NotifyConnectSessionEnded(void);

// Address of the last connect the engine accepted; "" before the first one.
const char* Bridge_LastConnectHost(void);

// Disconnect and re-join that address, re-minting the join token.
void Bridge_Reconnect(void);

// Listing / UI connect. Validates host charset then DispatchConnect -- never
// interpolates the address into a Cbuf line (semicolon in a listing IP is ACE).
bool Bridge_ConnectToHost(const char* host, int port);

#endif // BRIDGE_JOIN_AUTH_H
