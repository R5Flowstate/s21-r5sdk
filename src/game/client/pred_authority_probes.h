//=============================================================================//
//
// Purpose: Default-off prediction-authority probes -- per-kind residual
// census, zip-alpha rail dump, and kick-row lag histogram. Self-gated;
// read-only w.r.t. game state.
//
//=============================================================================//
#ifndef CLIENT_PRED_AUTHORITY_PROBES_H
#define CLIENT_PRED_AUTHORITY_PROBES_H

//-----------------------------------------------------------------------------
// [PRED-CENSUS] post-mask field names that still diverge. Self-gated, read-only.
//-----------------------------------------------------------------------------
void PredAuth_CensusObserve(void* pEntity, unsigned int nCmd);

//-----------------------------------------------------------------------------
// [ZIP-ALPHA] rail param on both sides of the wire. Alpha is FTYPEDESC_SKIP.
//-----------------------------------------------------------------------------
void PredAuth_ZipAlphaProbe(void* pEntity, unsigned int nCmd);

// Compare-only kick-row lag histogram. Self-gated on bridge_kick_row_tap.
void PredAuth_KickRowHistNote(float d, float ad, float flKickLag, bool bEqualized);
const char* PredAuth_KickRowClassName(float d, float ad);

#endif // CLIENT_PRED_AUTHORITY_PROBES_H
