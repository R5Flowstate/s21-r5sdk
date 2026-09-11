//=============================================================================//
//
// Purpose: C2S connect password tag (hashed serverFilter).
//
//=============================================================================//
#ifndef BRIDGE_CONNECT_PASSWORD_H
#define BRIDGE_CONNECT_PASSWORD_H

// Empty or null clears the key. Plaintext is hashed immediately and discarded.
void Bridge_SetConnectPassword(const char* pszPassword);

// Non-empty when a password key is stored. Never the plaintext.
const char* Bridge_GetConnectPasswordTag(void);

// Challenge-bound wire tag. "" when no password is configured.
void Bridge_WirePasswordTag(const uint32_t nChallenge, char* const out, const size_t outLen);

#endif // BRIDGE_CONNECT_PASSWORD_H
