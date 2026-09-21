//=============================================================================//
//
// Purpose: Drives fake players with user commands: replay of a recorded input
// stream, or a synthesized program (look, move, strafe, waypoint).
//
//=============================================================================//
#ifndef BOT_CMD_H
#define BOT_CMD_H

class CPlayer;
struct ScriptClassDescriptor_t;

// True when the bot consumed this server frame (the caller must not run the
// null command). False when the bot has no program.
bool BotCmd_RunFrame(CPlayer* pPlayer);

void BotCmd_OnRecordingFreed(int nRecordingId);
void BotCmd_OnPlayerGone(int nSlot);
void BotCmd_LevelShutdown(void);

void BotCmd_RegisterPlayerFuncs(ScriptClassDescriptor_t* pPlayerStruct);

#endif // BOT_CMD_H
