//=============================================================================//
//
// Purpose: dedicated-server side of the structured chat builder.
//
//=============================================================================//
#ifndef GAME_SERVER_CHATBUILDER_SERVER_H
#define GAME_SERVER_CHATBUILDER_SERVER_H

#include "game/shared/chatbuilder.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"

class CClient;

bool ChatBuilder_SendToClient(CClient* const pClient, const ChatBuilderSeg_t* const pSegs,
	const int nCount, const bool bAdminMsg);

void ChatBuilder_Broadcast(const ChatBuilderSeg_t* const pSegs, const int nCount,
	const bool bAdminMsg);

// One player's chat line, name included, so the recipient never has to resolve
// the sender's entity to render it.
bool ChatBuilder_SendPlayerChat(CClient* const pClient, const int nSenderSlot,
	const char* const pszName, const char* const pszText, const bool bTeamChat,
	const bool bReliable);

// Reads an array of segment tables off the VM stack. Returns the number of
// usable segments, or -1 if the argument is not an array of tables.
int ChatBuilder_ParseSegArray(HSQUIRRELVM v, const SQInteger nStackIdx,
	ChatBuilderSeg_t* const pOut, const int nOutMax);

// Optional trailing bool on the native. Missing or non-bool -> bDefault.
bool ChatBuilder_ReadOptionalBool(HSQUIRRELVM v, const SQInteger nStackIdx, const bool bDefault);

#endif // GAME_SERVER_CHATBUILDER_SERVER_H
