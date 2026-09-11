//=============================================================================//
//
// Purpose: Server-side head-glitch abuse detection.
//
//=============================================================================//
#ifndef GAME_SERVER_HEADGLITCH_DETECT_H
#define GAME_SERVER_HEADGLITCH_DETECT_H

class CPlayer;

void HeadGlitch_Frame(void);

// Armed state, honouring the bridge_headglitch_detect 2 gamemode gate.
bool HeadGlitch_IsEnabled(void);

// Per-player readouts. A slot with no tracked state reads zero / false, and
// exposure reads 1.0 (fully visible) when the player is not in the pose.
float HeadGlitch_GetScore(CPlayer* pPlayer);
float HeadGlitch_GetPeakScore(CPlayer* pPlayer);
float HeadGlitch_GetExposure(CPlayer* pPlayer);
float HeadGlitch_GetHoldTime(CPlayer* pPlayer);
float HeadGlitch_GetTotalTime(CPlayer* pPlayer);
int HeadGlitch_GetEpisodeCount(CPlayer* pPlayer);
int HeadGlitch_GetFlagCount(CPlayer* pPlayer);
bool HeadGlitch_IsActive(CPlayer* pPlayer);
bool HeadGlitch_IsFlagged(CPlayer* pPlayer);

// Ends the live pose and its score, banking any open episode. The session
// tally survives -- this is the per-round reset.
void HeadGlitch_ResetScore(CPlayer* pPlayer);

// Clears everything including the session tally.
void HeadGlitch_ClearPlayer(CPlayer* pPlayer);

#endif // GAME_SERVER_HEADGLITCH_DETECT_H
