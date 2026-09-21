//=============================================================================//
//
// Purpose: Implement things from GameInterface.cpp. Mostly the engine interfaces.
//
// $NoKeywords: $
//=============================================================================//

#include "core/stdafx.h"
#include "game/server/cmd_recorder.h"
#include "game/server/mapedit_paks.h"
#include "game/server/agent_link.h"
#include "game/server/bot_cmd.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include "tier1/cvar.h"
#include "public/server_class.h"
#include "public/eiface.h"
#include "public/const.h"
#include "common/protocol.h"
#include "common/callback.h"
#include "engine/server/sv_main.h"
#include "gameinterface.h"
#include "entitylist.h"
#include "baseanimating.h"
#include "engine/server/server.h"
#include "engine/client/client.h"
#include "common/netmessages.h"
#include "tier1/cmd.h"
#include "game/shared/usercmd.h"
#include "game/server/util_server.h"
#include "pluginsystem/pluginsystem.h"
#include "game/server/recipientfilter.h"
#include "game/shared/weapon_script_vars.h"
#include "game/shared/weapon_heat.h"
#include "game/server/weapon_ammo_pool_mod.h"
#include "game/shared/offhand_slots_ext.h"
#include "game/server/energize.h"
#include "game/server/akimbo.h"
#include "game/shared/globalnonrewind_vars.h"
#include "game/shared/scriptnetdata_ext.h"
#include "game/shared/scriptnetdata_limits.h"
#include "game/shared/deathfield_system.h"
#include "game/shared/highlight_context.h"
#include "game/shared/sdk_entity_state.h"
#include "game/server/chatbuilder.h"
#include "game/server/jetdrive.h"
#include "game/server/player_launch.h"
#include "game/server/trigger_gravity.h"
#include "game/server/trigger_updraft.h"
#include "engine/server/precache_natives.h"
#include "engine/server/skinnames_table_inject.h"
#include "engine/server/snapshot_diag.h"
#include "game/shared/dt_extend.h"
#include "game/shared/player_extend_sidecar.h"
#include "game/server/translocation.h"
#include "game/server/extended_range_use.h"
#include "game/server/vscript_server.h"
#include "game/server/vscript_server_natives.h"
#include "game/client/vscript_player.h"
#include "game/shared/vscript_remotefunctions_sdk.h"
#include "game/shared/scriptremotefunctions.h"
#include "game/shared/scriptremotefunctions_server.h"
#include "game/shared/vscript_remotefunctions_sdk.h"
#include "game/client/scriptnetdata_client.h"

//-----------------------------------------------------------------------------
// Purpose: retrieves the index of the client that issued the last command
// Output: int
//-----------------------------------------------------------------------------
int UTIL_GetCommandClientIndex(void)
{

	// Convert to 1 based offset
	return (*g_nCommandClientIndex)+1;
}

//-----------------------------------------------------------------------------
// Purpose: retrieves the player of the client that issued the last command
// Output: CPlayer*
//-----------------------------------------------------------------------------
CPlayer* UTIL_GetCommandClient(void)
{
	const int idx = UTIL_GetCommandClientIndex();
	if (idx > 0)
	{
		CPlayer* const player = UTIL_PlayerByIndex(idx);

		if (!player || !player->IsConnected())
			return NULL;

		return player;
	}

	// HLDS console issued command
	return NULL;
}

bool CServerGameDLL::DLLInit(CServerGameDLL* thisptr, CreateInterfaceFn appSystemFactory, CreateInterfaceFn physicsFactory,
	CreateInterfaceFn fileSystemFactory, CGlobalVars* pGlobals)
{
	gpGlobals = pGlobals;
	return CServerGameDLL__DLLInit(thisptr, appSystemFactory, physicsFactory, fileSystemFactory, pGlobals);
}

//-----------------------------------------------------------------------------
// This is called when a new game is started. (restart, map)
//-----------------------------------------------------------------------------
bool CServerGameDLL::GameInit(void)
{
	const static int index = 1;
	return CallVFunc<bool>(index, this);
}

//-----------------------------------------------------------------------------
// This is called when scripts are getting recompiled. (restart, map, changelevel)
//-----------------------------------------------------------------------------
void CServerGameDLL::PrecompileScriptsJob(void)
{
	const static int index = 2;
	CallVFunc<void>(index, this);
}

//-----------------------------------------------------------------------------
// SDK per-level reset state. Armed after each level's string-table create so
// a transition that never reaches LevelShutdown still resets before the next
// level caches table pointers.
//-----------------------------------------------------------------------------
static std::atomic<unsigned int> s_nSdkLevelGeneration{0};
static std::atomic<bool>         s_bSdkLevelResetArmed{false};

void ServerGameDLL_RunSdkLevelReset(const char* pszReason)
{
	// Arm pack freeze before entity teardown so snapshot workers cannot pack freed BCC.
	SnapshotDiag_SetPackFrozen(true);

	// Drop per-entity SDK shadow state so stale entries do not survive the next map.
	SDKEntityState_FlushAll(ESide::Server);
	SDKEntityState_FlushAll(ESide::Client);

	WeaponScriptVars_LevelShutdown();
	WeaponScriptVars_PhaseShift_LevelShutdown();
	WeaponScriptVars_WeaponLockedSet_LevelShutdown();
	WeaponScriptVars_InfiniteAmmo_LevelShutdown();
	JetDrive_Wire_LevelShutdown();
	PlayerLaunch_LevelShutdown();
	PlayerExtend_LevelShutdown();
	TriggerGravity_Wire_LevelShutdown();
	UpdraftBridge_Wire_LevelShutdown();
	WeaponHeat_LevelShutdown();
	WeaponAmmoPoolMod_LevelShutdown();
	OffhandSlotsExt_LevelShutdown();
	EnergizeBridge_LevelShutdown();
	AkimboBridge_LevelShutdown();
	GlobalNonRewind_LevelShutdown();
	ScriptNetDataExt_LevelShutdown();
	SNDC_ExtensionLevelShutdown();
	Translocation_LevelShutdown();
	ServerScript_PlacementLevelShutdown();
	BreachTrace_LevelShutdown();
	PrecacheNativesDedi_LevelShutdown();
	ExtendedUse_LevelShutdown();
	ScriptRemoteC2S_LevelShutdown();
	// Clear dt_extend's NonRewind/GLOBAL captures + pending/applied
	// m_pServerClass swap queues so map 2+ re-captures fresh and stale
	// entity pointers can't edict-collide with new entities.
	DTExtend_LevelShutdown();
	// Drop snapshot_diag's per-entity settings-applied hash set + one-shot
	// forensic dump guard so a new map's first entity isn't shadowed by
	// a stale map-1 ptr in the set.
	SnapshotDiag_LevelShutdown();
	BotCmd_LevelShutdown();
	CmdRecorder_LevelShutdown();
	MapEditPaks_LevelShutdown();
	// Clear the SkinNames inject guard; string tables are recreated per-map.
	SkinNamesInject_LevelShutdown();
	ScriptNetData_LevelShutdown();
	DeathField_LevelShutdown();
	HighlightContext_LevelShutdown();
	// Teleport sticky state (and other player script maps) must clear on dedi
	// changelevel too -- was incorrectly gated behind !DEDICATED.
	VScriptPlayer_LevelShutdown();
	Script_ClearRemoteFunctionRegistrations();
	ScriptRemote_ResetExtendedArgBuffer();

	s_bSdkLevelResetArmed.store(false, std::memory_order_release);

	Msg(eDLL_T::SERVER, "[s21-dedi] SDK level reset (%s, gen %u)\n",
		pszReason ? pszReason : "?",
		s_nSdkLevelGeneration.load(std::memory_order_acquire));
}

unsigned int ServerGameDLL_GetLevelGeneration(void)
{
	return s_nSdkLevelGeneration.load(std::memory_order_acquire);
}

unsigned int ServerGameDLL_OnLevelStringTablesCreated(void)
{
	// map / State_NewGame never call LevelShutdown; catch that path here
	// before any SDK code caches a table pointer for the new level.
	if (s_bSdkLevelResetArmed.exchange(false, std::memory_order_acq_rel))
		ServerGameDLL_RunSdkLevelReset("engine level-init");

	const unsigned int gen =
		s_nSdkLevelGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;

	s_bSdkLevelResetArmed.store(true, std::memory_order_release);

	// CreateNetworkStringTables runs during level bring-up before the BSP
	// entity lump is parsed, so the factory is live when trigger_updraft
	// brushes are created.
	UpdraftBridge_InstallEntityFactory();

	return gen;
}

//-----------------------------------------------------------------------------
// Called when a level is shutdown (including changing levels)
//-----------------------------------------------------------------------------
void CServerGameDLL::LevelShutdown(void)
{
	ServerGameDLL_RunSdkLevelReset("LevelShutdown");

	const static int index = 8;
	CallVFunc<void>(index, this);
}

//-----------------------------------------------------------------------------
// This is called when a game ends (server disconnect, death, restart, load)
// NOT on level transitions within a game
//-----------------------------------------------------------------------------
void CServerGameDLL::GameShutdown(void)
{
	// Game just calls a nullsub for GameShutdown lol.
	const static int index = 9;
	CallVFunc<void>(index, this);
}

//-----------------------------------------------------------------------------
// Purpose: Gets the simulation tick interval
// Output: float
//-----------------------------------------------------------------------------
float CServerGameDLL::GetTickInterval(void)
{
	const static int index = 11;
	return CallVFunc<float>(index, this);
}

//-----------------------------------------------------------------------------
// Purpose: get all server classes
// Output: ServerClass*
//-----------------------------------------------------------------------------
ServerClass* CServerGameDLL::GetAllServerClasses(void)
{
	const static int index = 12;
	return CallVFunc<ServerClass*>(index, this);
}

static ConVar chat_debug("chat_debug", "0", FCVAR_RELEASE, "Enables chat-related debug printing.");
static ConVar sv_chat_commands("sv_chat_commands", "1", FCVAR_RELEASE,
	"Treat chat lines starting with '!' as client commands (not broadcast).");

static bool Chat_CommandVerbDenied(const char* const pszVerb)
{
	if (!pszVerb || !pszVerb[0])
		return true;

	static const char* const kDeny[] = {
		"script", "script_client", "script_ui",
		"bind", "unbind", "unbindall",
		"exec", "alias", "quit",
		"_setClassVarClient",
		"net_setkey", "net_generatekey", "net_getkey", "sv_netkey",
		"net_tracePayload", "net_dumpWire", "net_useRandomKey",
		"spawnbots",
		"chat_announce",
		"launchplaylist",
		"language",
		"fs_guardLiveMapUnmount",
		"sdk_splitpacket_recv_clamp",
		"bridge_akimbo",
		"akimbo_weapon_can_zoom",
		"bridge_pose_param_ext",
		"bridge_pose_moveyaw",
	};

	for (size_t i = 0; i < SDK_ARRAYSIZE(kDeny); i++)
	{
		if (!_stricmp(pszVerb, kDeny[i]))
			return true;
	}

	if (pszVerb[0] == '+' || pszVerb[0] == '-')
		return true;

	return false;
}

// Returns true if the line was a '!' command and must not be broadcast.
static bool Chat_TryRunBangCommand(CClient* const pSenderClient, const char* text)
{
	if (!sv_chat_commands.GetBool() || !pSenderClient || !text || text[0] != '!')
		return false;

	const char* pszCmd = text + 1;
	while (*pszCmd == ' ' || *pszCmd == '\t')
		++pszCmd;
	if (!*pszCmd)
		return true;

	if (V_strlen(pszCmd) >= 512)
	{
		Warning(eDLL_T::SERVER, "[CHAT-CMD] drop overlong command from slot=%i\n",
			pSenderClient->GetUserID());
		return true;
	}

	CCommand args;
	if (!args.Tokenize(pszCmd, cmd_source_t::kCommandSrcNetClient) || args.ArgC() < 1)
	{
		Warning(eDLL_T::SERVER, "[CHAT-CMD] drop from slot=%i (tokenize)\n",
			pSenderClient->GetUserID());
		return true;
	}

	if (Chat_CommandVerbDenied(args.Arg(0)))
	{
		Warning(eDLL_T::SERVER, "[CHAT-CMD] drop '%s' from slot=%i\n",
			args.Arg(0), pSenderClient->GetUserID());
		return true;
	}

	// Engine ProcessStringCmd reads cmd at +0x20 and the 1024-byte buffer at +0x28.
	// Do not construct NET_StringCmd (INetMessage still has pure virtuals).
	unsigned char raw[sizeof(NET_StringCmd)];
	memset(raw, 0, sizeof(raw));
	char* const pszBuf = reinterpret_cast<char*>(raw + 0x28);
	V_strncpy(pszBuf, pszCmd, 512);
	*reinterpret_cast<const char**>(raw + 0x20) = pszBuf;

	CClient* const pShifted = reinterpret_cast<CClient*>(
		reinterpret_cast<char*>(pSenderClient) + sizeof(void*));
	CClient::VProcessStringCmd(pShifted, reinterpret_cast<NET_StringCmd*>(raw));
	return true;
}

static ConVar bridge_chat_bridge_render("bridge_chat_bridge_render", "1", FCVAR_RELEASE,
	"S21 bridge: deliver player chat as a bridge chat stream carrying the sender's name (1, default) "
	"instead of the native SayText usermessage. The native handler resolves the sender by edict and "
	"renders nothing when that entity is outside the viewer's realm, which silences every cross-realm "
	"line. 0 = native SayText (same-realm senders only).");
static ConVar bridge_chat_unreliable("bridge_chat_unreliable", "1", FCVAR_RELEASE,
	"S21 bridge: broadcast the SayText chat usermessage UNRELIABLY (1, default) instead of reliably. "
	"The S21<->S3 bridge only translates/relays the UNRELIABLE S2C message stream to the client; reliable "
	"subchannel usermessages (the default for chat) never reach the S21 hud chat. Localhost bridge = no loss.");
static ConVar sv_overrideTeamChatRestriction("sv_overrideTeamChatRestriction", "1", FCVAR_RELEASE,
	"When enabled, sv_forceChatToTeamOnly controls the chat restriction (overriding the client's "
	"per-message team flag). Defaults ON for the S21 bridge -- the bridge client emits SayText with "
	"isTeamChat=1, so without this every line would render as TEAM chat; with this + sv_forceChatToTeamOnly 0, "
	"chat is GLOBAL.",
	"0: honor the client's per-message team flag, 1: force the value from sv_forceChatToTeamOnly."
);
static ConVar sv_allowIconsInChat("sv_allowIconsInChat", "0", FCVAR_RELEASE, "Allow game icon characters in chat messages. 0 = Block icons, 1 = Allow icons");

// Function to check if a UTF-8 string contains blocked game icon characters
static bool SV_ContainsBlockedIcons(const char* text)
{
	if (!text)
		return false;

	const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
	int safety = 0;

	while (*p && safety++ < 2048)
	{
		if (p[0] == 0xF3 && p[1] == 0xB0)
			return true;
		if (p[0] == 0xEF && p[1] >= 0x80 && p[1] <= 0xBF)
			return true;

		const unsigned char c = p[0];
		if (c <= 0x7F)
		{
			++p;
			continue;
		}

		if (c <= 0xBF || c == 0xC0 || c == 0xC1 || c >= 0xF5)
		{
			++p;
			continue;
		}

		if (c <= 0xDF)
		{
			if (!p[1] || (p[1] & 0xC0) != 0x80)
			{
				++p;
				continue;
			}
			p += 2;
			continue;
		}

		if (c <= 0xEF)
		{
			unsigned char lo = 0x80;
			unsigned char hi = 0xBF;
			if (c == 0xE0)
				lo = 0xA0;
			else if (c == 0xED)
				hi = 0x9F;
			if (!p[1] || !p[2]
				|| p[1] < lo || p[1] > hi
				|| (p[2] & 0xC0) != 0x80)
			{
				++p;
				continue;
			}
			p += 3;
			continue;
		}

		{
			unsigned char lo = 0x80;
			unsigned char hi = 0xBF;
			if (c == 0xF0)
				lo = 0x90;
			else if (c == 0xF4)
				hi = 0x8F;
			if (!p[1] || !p[2] || !p[3]
				|| p[1] < lo || p[1] > hi
				|| (p[2] & 0xC0) != 0x80
				|| (p[3] & 0xC0) != 0x80)
			{
				++p;
				continue;
			}
			p += 4;
		}
	}

	return false;
}

void CServerGameDLL::OnReceivedSayTextMessage(CServerGameDLL* thisptr, int senderId, const char* text, bool isTeamChat)
{
	CPlayer* const pSenderPlayer = UTIL_PlayerByIndex(senderId);
	CClient* const pSenderClient = g_pServer->GetClient(senderId - 1);

	if (chat_debug.GetBool())
		Msg(eDLL_T::SERVER, "[BRIDGE-CHAT] OnReceivedSayText senderId=%d player=%p client=%p connected=%d team=%d text='%s'\n",
			senderId, reinterpret_cast<void*>(pSenderPlayer), reinterpret_cast<void*>(pSenderClient),
			(pSenderPlayer && pSenderPlayer->IsConnected()) ? 1 : 0, (int)isTeamChat, text ? text : "(null)");

	if (!pSenderPlayer || !pSenderClient ||  !pSenderPlayer->IsConnected())
	{
		if (chat_debug.GetBool())
			Msg(eDLL_T::SERVER, "[BRIDGE-CHAT] DROP: sender invalid/disconnected (player=%p client=%p)\n",
				reinterpret_cast<void*>(pSenderPlayer), reinterpret_cast<void*>(pSenderClient));
		return;
	}

	if (text && text[0] == 'b' && text[1] == 'r' && text[2] == 'c' && text[3] == ' ')
	{
		const char* const pszNum = text + 4;
		if (pszNum[0] >= '0' && pszNum[0] <= '9')
		{
			char* pszEnd = nullptr;
			const unsigned long nSeqUL = strtoul(pszNum, &pszEnd, 10);
			const ptrdiff_t nDigits = pszEnd ? (pszEnd - pszNum) : 0;
			if (pszEnd && nDigits > 0 && nDigits <= 10
				&& !(nDigits == 10 && strncmp(pszNum, "4294967295", 10) > 0)
				&& *pszEnd == ' ')
			{
				CClientExtended* const pExt = pSenderClient->GetClientExtended();
				if (pExt && !pExt->AcceptBridgeRelSeq(static_cast<uint32_t>(nSeqUL)))
				{
					if (chat_debug.GetBool())
						Msg(eDLL_T::SERVER, "[BRIDGE-CHAT] drop dup seq=%lu senderId=%d\n",
							nSeqUL, senderId);
					return;
				}
				text = pszEnd + 1;
			}
		}
	}

	const bool bIsTeamChat = sv_overrideTeamChatRestriction.GetBool() ? sv_forceChatToTeamOnly->GetBool()  : isTeamChat;

	// Validate chat message characters
	if (text)
	{
		// Always check if it's valid UTF-8
		if (!V_IsValidUTF8(text))
		{
			if (chat_debug.GetBool())
				Msg(eDLL_T::SERVER, "Dropping chat message from '%s' (%llu): invalid UTF-8 encoding\n",
					pSenderPlayer->GetNetName(), pSenderPlayer->GetPlatformUserId());
			return; // Drop invalid UTF-8
		}
		
		// Check for blocked game icon characters (unless allowed)
		if (!sv_allowIconsInChat.GetBool() && SV_ContainsBlockedIcons(text))
		{
			if (chat_debug.GetBool())
				Msg(eDLL_T::SERVER, "Dropping chat message from '%s' (%llu): contains blocked game icon characters\n",
					pSenderPlayer->GetNetName(), pSenderPlayer->GetPlatformUserId());
			return; // Drop message with blocked icons
		}
		
		// Always check for control characters but allow normal Unicode
		for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p)
		{
			// Allow printable ASCII (32-126)
			if (*p >= 32 && *p <= 126)
				continue;
				
			// Allow high-bit characters (UTF-8 sequences) - this includes Japanese, Chinese, etc.
			if (*p >= 128)
			{
				// This is part of a UTF-8 sequence, allow it (Japanese, Chinese, Korean, etc.)
				continue;
			}
			
			// Block control characters (0-31, 127) except common whitespace
			if (*p != '\t' && *p != '\n' && *p != '\r')
			{
				if (chat_debug.GetBool())
					Msg(eDLL_T::SERVER, "Dropping chat message from '%s' (%llu): contains control characters\n",
						pSenderPlayer->GetNetName(), pSenderPlayer->GetPlatformUserId());
				return; // Drop message with control characters
			}
		}
	}
	const int nMaxClients = gpGlobals->maxClients;
	const bool bShouldApplyGlobalCommsMutes = SV_ShouldApplyTextChatGlobalMutes();

	pSenderPlayer->UpdateLastActiveTime(gpGlobals->curTime);
	
	const bool bSenderIsCommsBanned = pSenderClient->GetClientExtended()->IsClientCommsBanned();

	if (bShouldApplyGlobalCommsMutes && bSenderIsCommsBanned)
	{
		if (chat_debug.GetBool())
			Msg(eDLL_T::SERVER, "Dropping chat message from '%s' (%llu) User is globally muted", 
				pSenderPlayer->GetNetName(), pSenderPlayer->GetPlatformUserId());

		CSingleUserRecipientFilter filter(pSenderPlayer);
		filter.MakeReliable();

		v_UserMessageBegin(&filter, "SayText", 2);

		MessageWriteByte(pSenderPlayer->GetEdict());
		MessageWriteString(pSenderClient->GetClientExtended()->GetCommsMuteDisplayMessage());
		MessageWriteBool(bIsTeamChat);

		MessageEnd();
		return;
	}

	for (auto& cb : !PluginSystem()->GetChatMessageCallbacks())
	{
		if (!cb.Function()(pSenderPlayer, text ? text : "", sv_forceChatToTeamOnly->GetBool()))
		{
			if (chat_debug.GetBool())
			{
				char moduleName[MAX_PATH] = {};

				V_UnicodeToUTF8(V_UnqualifiedFileName(cb.ModuleName()), moduleName, MAX_PATH);

				Msg(eDLL_T::SERVER, "[%s] Plugin blocked chat message from '%s' (%llu): \"%s\"\n", moduleName, pSenderPlayer->GetNetName(), pSenderPlayer->GetPlatformUserId(), text ? text : "");
			}

			return;
		}
	}

	if (Chat_TryRunBangCommand(pSenderClient, text))
		return;

	const bool bSenderDeadAndCanOnlyTalkToDead = hudchat_dead_can_only_talk_to_other_dead->GetBool() && pSenderPlayer->GetLifeState();

	for (int nRecipientIndex = 1; nRecipientIndex <= nMaxClients; nRecipientIndex++)
	{
		const CPlayer* const pRecipientPlayer = UTIL_PlayerByIndex(nRecipientIndex);
		CClient* const pRecipientClient = g_pServer->GetClient(nRecipientIndex - 1);

		//Are we all there
		if (!pRecipientPlayer || !pRecipientClient || !pRecipientPlayer->IsConnected())
			continue;

		//If our recipient is banned and the host doesnt want banned people to see others chat skip them
		if (bShouldApplyGlobalCommsMutes && pRecipientClient->GetClientExtended()->IsClientCommsBanned() && !sv_commsBannedClientsCanReceiveComms.GetBool())
			continue;

		//If we are only allowed to talk to the dead make sure the recipient is dead
		if (bSenderDeadAndCanOnlyTalkToDead && !pRecipientPlayer->GetLifeState())
			continue;

		//If we arent the recipient
		if (pRecipientPlayer != pSenderPlayer &&
			//If the chat is limited to one team we must check the sender and recipient are on the same team
			bIsTeamChat && pSenderPlayer->GetTeamNum() != pRecipientPlayer->GetTeamNum()
		)
			continue;

		if (chat_debug.GetBool())
			Msg(eDLL_T::SERVER, "[BRIDGE-CHAT] -> sending SayText to recipient idx=%d edict=%d (maxClients=%d, team=%d, unreliable=%d, bridgeRender=%d)\n",
				nRecipientIndex, pSenderPlayer->GetEdict(), nMaxClients, (int)bIsTeamChat,
				(int)bridge_chat_unreliable.GetBool(), (int)bridge_chat_bridge_render.GetBool());

		if (bridge_chat_bridge_render.GetBool())
		{
			ChatBuilder_SendPlayerChat(pRecipientClient,
				pSenderPlayer->GetEdict(), pSenderPlayer->GetNetName(), text ? text : "",
				bIsTeamChat, !bridge_chat_unreliable.GetBool());
			continue;
		}

		CSingleUserRecipientFilter filter(pRecipientPlayer);
		if (!bridge_chat_unreliable.GetBool())
			filter.MakeReliable();

		v_UserMessageBegin(&filter, "SayText", 2);

		MessageWriteByte(pSenderPlayer->GetEdict());
		MessageWriteString(text ? text : "");
		MessageWriteBool(bIsTeamChat);

		MessageEnd();
	}
}

static void DrawServerHitbox(int iEntity)
{
	const CEntInfo* const pInfo = g_serverEntityList->GetEntInfoPtrByIndex(iEntity);
	CBaseAnimating* const pAnimating = dynamic_cast<CBaseAnimating*>(pInfo->m_pEntity);

	if (pAnimating)
	{
		pAnimating->DrawServerHitboxes();
	}
}

static void DrawServerHitboxes()
{
	const int nVal = sv_showhitboxes->GetInt();
	Assert(nVal < NUM_ENT_ENTRIES);

	if (nVal == -1)
		return;

	if (nVal == 0)
	{
		for (int i = 0; i < NUM_ENT_ENTRIES; i++)
		{
			DrawServerHitbox(i);
		}
	}
	else // Lookup entity manually by index from 'sv_showhitboxes'.
	{
		DrawServerHitbox(nVal);
	}
}

static void DrawGeometryOverlays()
{
	const CEntInfo* pInfo = g_serverEntityList->FirstEntInfo();

	for (; pInfo; pInfo = pInfo->m_pNext)
	{
		CBaseEntity* const ent = (CBaseEntity*)pInfo->m_pEntity;

		if (ent->GetDebugOverlays() || ent->GetTimedOverlay())
		{
			ent->DrawDebugGeometryOverlays();
		}
	}
}

static void DrawAllDebugOverlays()
{
	if (!developer->GetBool())
		return;

	DrawServerHitboxes();
	DrawGeometryOverlays();
}

void CServerGameClients::_ProcessUserCmds(CServerGameClients* thisp, edict_t edict,
	bf_read* buf, int numCmds, int totalCmds, int droppedPackets, bool ignore, bool paused)
{
	int i;
	CUserCmd* from, * to;

	// We track last three command in case we drop some
	// packets but get them back.
	CUserCmd cmds[MAX_BACKUP_COMMANDS_PROCESS];
	CUserCmd cmdNull;  // For delta compression

	Assert(numCmds >= 0);
	Assert((totalCmds - numCmds) >= 0);

	// Too many commands?
	if (totalCmds < 0 || totalCmds >= (MAX_BACKUP_COMMANDS_PROCESS - 1) ||
		numCmds < 0 || numCmds > totalCmds)
	{
		const CClient* const pClient = g_pServer->GetClient(edict-1);

		Warning(eDLL_T::SERVER, "%s: Player '%s' sent too many cmds (%i)\n", __FUNCTION__, pClient->GetServerName(), totalCmds);
		buf->SetOverflowFlag();

		return;
	}

	from = &cmdNull;
	for (i = totalCmds - 1; i >= 0; i--)
	{
		to = &cmds[i];
		ReadUserCmd(buf, to, from);
		from = to;
	}

	if (buf->IsOverflowed())
	{
		Warning(eDLL_T::SERVER, "%s: overflowed usercmd stream (edict %d)\n", __FUNCTION__, edict);
		return;
	}

	// Server has gone inactive, just ignore.
	if (ignore)
	{
		return;
	}

	CPlayer* const pPlayer = UTIL_PlayerByIndex(edict);

	// Client not fully connected, just ignore.
	if (!pPlayer)
	{
		return;
	}

	// Time the engine's per-cmd dispatch; first usercmd can trip net_processTimeBudget.
	const double flUsrCmdStart = Plat_FloatTime();
	const double flUsrCmdStallBase = Plat_GetThreadStallTime();
	pPlayer->ProcessUserCmds(cmds, numCmds, totalCmds, droppedPackets, paused);
	const double flUsrCmdEnd = Plat_FloatTime();
	const float flUsrCmdMs = static_cast<float>((flUsrCmdEnd - flUsrCmdStart) * 1000.0);

	// The intake path logs from inside itself, so part of dur can be this thread
	// blocked in log emission rather than intake work. Report it separately.
	const float flUsrCmdStallMs = static_cast<float>(
		Clamp(Plat_GetThreadStallTime() - flUsrCmdStallBase, 0.0,
			flUsrCmdEnd - flUsrCmdStart) * 1000.0);

	// Always log if a real hitch (>5 ms) occurred -- hitch detection is
	// useful in any session.
	if (flUsrCmdMs > 5.0f)
	{
		Msg(eDLL_T::SERVER,
			"[USRCMD-TIME] slot=%d numCmds=%d totalCmds=%d dur=%.2fms logstall=%.2fms cmdNr=%u tick=%u\n",
			edict - 1, numCmds, totalCmds, flUsrCmdMs, flUsrCmdStallMs,
			(unsigned)cmds[0].command_number, (unsigned)cmds[0].tick_count);
	}
}

//---------------------------------------------------------------------------------
// Purpose: dispatches the server frame job, this calls ExecuteFrameServerJob,
// anything you add in this function will either be before, or after the
// server frame job has ran, so ThreadInServerFrameThread will always
// return false here. If you need to run code in the server frame thread,
// consider adding your code in ExecuteFrameServerJob.
// Input: flFrameTime - 
// bRunOverlays - 
// bUpdateFrame - 
//---------------------------------------------------------------------------------
static void DispatchFrameServerJob(double flFrameTime, bool bRunOverlays, bool bUniformUpdate)
{
	v_DispatchFrameServerJob(flFrameTime, bRunOverlays, bUniformUpdate);
}

//---------------------------------------------------------------------------------
// Purpose: executes the server frame job
// Input: flFrameTime - 
// bRunOverlays - 
// bUpdateFrame - 
//---------------------------------------------------------------------------------
static void ExecuteFrameServerJob(double flFrameTime, bool bRunOverlays, bool bUpdateFrame)
{
	v_ExecuteFrameServerJob(flFrameTime, bRunOverlays, bUpdateFrame);

	DrawAllDebugOverlays();
}

static bool s_bFreezeCurLatched = false;
static float s_flFreezeCurTime = 0.0f;

__int64 CServerGameDLL::GameFrame(void* thisptr, unsigned char simulating)
{
	float flSavedFrame = 0.0f;
	const float flScale = GameTimescale_WorldScale();
	const bool bFreeze = (gpGlobals != nullptr) && (flScale < 1.0f);
	if (bFreeze)
	{
		if (!s_bFreezeCurLatched)
		{
			s_flFreezeCurTime = gpGlobals->curTime;
			s_bFreezeCurLatched = true;
			Msg(eDLL_T::SERVER, "[FREEZE] hold curTime=%.4f\n",
				static_cast<double>(s_flFreezeCurTime));
		}
		gpGlobals->curTime = s_flFreezeCurTime;
		flSavedFrame = gpGlobals->frameTime;
		gpGlobals->frameTime *= flScale;
	}
	else
		s_bFreezeCurLatched = false;

	AgentLink_Think();
	const __int64 nRet = CServerGameDLL__GameFrame(thisptr, simulating);
	if (bFreeze)
	{
		gpGlobals->frameTime = flSavedFrame;
		gpGlobals->curTime = s_flFreezeCurTime;
	}
	return nRet;
}

void MessageEnd(void)
{
	Assert(*g_ppUsrMessageBuffer);

	g_pEngineServer->MessageEnd();

	(*g_ppUsrMessageBuffer) = nullptr;
}

void MessageWriteByte(int iValue)
{
	if (!*g_ppUsrMessageBuffer)
		Error(eDLL_T::ENGINE, EXIT_FAILURE, "WRITE_BYTE called with no active message\n");

	(*g_ppUsrMessageBuffer)->WriteByte(iValue);
}

void MessageWriteString(const char* pszString)
{
	if (!*g_ppUsrMessageBuffer)
		Error(eDLL_T::ENGINE, EXIT_FAILURE, "WriteString called with no active message\n");

	(*g_ppUsrMessageBuffer)->WriteString(pszString);
}

void MessageWriteBool(bool bValue)
{
	if (!*g_ppUsrMessageBuffer)
		Error(eDLL_T::ENGINE, EXIT_FAILURE, "WriteBool called with no active message\n");

	(*g_ppUsrMessageBuffer)->WriteOneBit(static_cast<int>(bValue));
}

void VServerGameDLL::Detour(const bool bAttach) const
{
	DetourSetup(&CServerGameDLL__DLLInit, &CServerGameDLL::DLLInit, bAttach);
	DetourSetup(&CServerGameDLL__OnReceivedSayTextMessage, &CServerGameDLL::OnReceivedSayTextMessage, bAttach);
	DetourSetup(&CServerGameClients__ProcessUserCmds, CServerGameClients::_ProcessUserCmds, bAttach);
	DetourSetup(&v_DispatchFrameServerJob, &DispatchFrameServerJob, bAttach);
	DetourSetup(&v_ExecuteFrameServerJob, &ExecuteFrameServerJob, bAttach);
	if (CServerGameDLL__GameFrame)
		DetourSetup(&CServerGameDLL__GameFrame, &CServerGameDLL::GameFrame, bAttach);
	else if (bAttach)
		Warning(eDLL_T::SERVER,
			"[FREEZE] CServerGameDLL::GameFrame pattern unresolved -- sim scale off\n");
}

CThreadMutex* g_serverFrameMutex;

CServerGameDLL* g_pServerGameDLL = nullptr;
CServerGameClients* g_pServerGameClients = nullptr;
CServerGameEnts* g_pServerGameEntities = nullptr;
CServerRandomStream* g_randomStream = nullptr;

// Holds global variables shared between engine and game.
CGlobalVars* gpGlobals = nullptr;
