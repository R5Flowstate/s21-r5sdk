//=============================================================================//
//
// Purpose: HUD base chat panel
//
//=============================================================================//
#include "core/stdafx.h"
#include "engine/client/vengineclient_impl.h"
#include "game/client/cliententitylist.h"
#include "hud_basechat.h"
#include "edict.h"
#include "engine/client/cl_main.h"
#include "localize/localize.h"

static ConVar hudchat_ignore_server_messages("hudchat_ignore_server_messages", "0", FCVAR_RELEASE | FCVAR_ARCHIVE, "Disables server messages appearing in the chat box");

static ConVar* s_pHudSettingChat = nullptr;

static int Chat_SettingMode(void)
{
	if (!s_pHudSettingChat && g_pCVar)
		s_pHudSettingChat = g_pCVar->FindVar("hud_setting_chat");
	return s_pHudSettingChat ? s_pHudSettingChat->GetInt() : 0;
}

bool Chat_AllowsPlayerChat(void)
{
	return Chat_SettingMode() < 2;
}

//-----------------------------------------------------------------------------
// Per-sender chat mute, keyed by the sender slot the SayText usermessage carries
// in its first payload byte. Slot-keyed rather than identity-keyed because the
// S3 dedicated server has no GetUserID/GetUnspoofedHardware, so the client can
// never build the uid triplet TogglePlayerVoiceAndTextMuteForUID needs for a
// player it holds no entity for -- which is every player outside its own realm.
//-----------------------------------------------------------------------------
static bool s_chatMutedSlots[kChatMaxSenderSlots];
static bool s_chatMutedAny = false;

void Chat_SetSlotMuted(const int nSlot, const bool bMuted)
{
	if (nSlot < 0 || nSlot >= kChatMaxSenderSlots)
		return;

	s_chatMutedSlots[nSlot] = bMuted;

	if (bMuted)
	{
		s_chatMutedAny = true;
		return;
	}

	s_chatMutedAny = false;
	for (int i = 0; i < kChatMaxSenderSlots; i++)
	{
		if (s_chatMutedSlots[i])
		{
			s_chatMutedAny = true;
			break;
		}
	}
}

bool Chat_IsSlotMuted(const int nSlot)
{
	if (nSlot < 0 || nSlot >= kChatMaxSenderSlots)
		return false;

	// Only speaks up while something is muted, so it costs nothing in normal
	// play and self-calibrates the slot convention the moment you test a mute.
	if (s_chatMutedAny)
		DevMsg(eDLL_T::CLIENT, "[CHAT-MUTE] sender slot=%d muted=%d\n",
			nSlot, s_chatMutedSlots[nSlot] ? 1 : 0);

	return s_chatMutedSlots[nSlot];
}

void Chat_ClearMutedSlots(void)
{
	memset(s_chatMutedSlots, 0, sizeof(s_chatMutedSlots));
	s_chatMutedAny = false;
}

static bool Chat_RecipientAllowsServerMsg(const bool bAdminMsg)
{
	const int nMode = Chat_SettingMode();
	if (nMode >= 2)
		return false;

	if (bAdminMsg)
		return true;

	if (nMode >= 1)
		return false;

	if (!hudchat_visibility && g_pCVar)
		hudchat_visibility = g_pCVar->FindVar("hudchat_visibility");

	// Null means FindVar ran before the engine registered it -- not "hidden".
	if (hudchat_visibility && !hudchat_visibility->GetBool())
		return false;
	if (hudchat_ignore_server_messages.GetBool())
		return false;
	return true;
}

static float Chat_DefaultShowDuration(void)
{
	return hudchat_new_message_shown_duration ? hudchat_new_message_shown_duration->GetFloat() : 0.0f;
}

static float Chat_DefaultFadeDuration(void)
{
	return hudchat_new_message_fade_duration ? hudchat_new_message_fade_duration->GetFloat() : 0.0f;
}

static constexpr ptrdiff_t kS21HudChatReady = 579;
static constexpr ptrdiff_t kS21HudChatHistory = 0x298;
static constexpr size_t kS21RichText_InsertChar = 2064 / sizeof(void*);
static constexpr size_t kS21RichText_InsertWide = 2072 / sizeof(void*);
static constexpr size_t kS21RichText_InsertColor = 2248 / sizeof(void*);
static constexpr size_t kS21RichText_InsertFade = 2264 / sizeof(void*);

static void Chat_S21InsertAscii(void* const pHist, const char* const psz, const int nLen)
{
	if (!pHist || !psz || nLen <= 0)
		return;

	wchar_t wsz[kChatBuilderMaxSegText + 1];
	const int nUse = Min(nLen, kChatBuilderMaxSegText);
	for (int i = 0; i < nUse; i++)
		wsz[i] = static_cast<unsigned char>(psz[i]);
	wsz[nUse] = L'\0';

	void** const vtbl = *reinterpret_cast<void***>(pHist);
	reinterpret_cast<void(*)(void*, const wchar_t*)>(vtbl[kS21RichText_InsertWide])(pHist, wsz);
}

static void Chat_S21InsertColor(void* const pHist, const unsigned int col)
{
	if (!pHist)
		return;
	void** const vtbl = *reinterpret_cast<void***>(pHist);
	reinterpret_cast<void(*)(void*, unsigned int)>(vtbl[kS21RichText_InsertColor])(pHist, col);
}

static void Chat_S21InsertFade(void* const pHist, const float flShow, const float flFade)
{
	if (!pHist)
		return;
	void** const vtbl = *reinterpret_cast<void***>(pHist);
	reinterpret_cast<void(*)(void*, float, float)>(vtbl[kS21RichText_InsertFade])(pHist, flShow, flFade);
}

static void Chat_S21InsertChar(void* const pHist, const int ch)
{
	if (!pHist)
		return;
	void** const vtbl = *reinterpret_cast<void***>(pHist);
	reinterpret_cast<void(*)(void*, int)>(vtbl[kS21RichText_InsertChar])(pHist, ch);
}

static void* Chat_S21GetHistory(CBaseHudChat* const pChat)
{
	char* const pHead = reinterpret_cast<char*>(pChat);
	pHead[kS21HudChatReady] = 1;

	void* const pHist = *reinterpret_cast<void**>(pHead + kS21HudChatHistory);
	if (!pHist)
	{
		Warning(eDLL_T::CLIENT, "[CHATBUILDER] history pointer is null -- dropping\n");
		return nullptr;
	}
	return pHist;
}

void CBaseHudChat::PrintSystemMsg(const char* const pszPrefixStr, const char* const pszMsgText, const bool bAdminMsg)
{
	if (!Chat_RecipientAllowsServerMsg(bAdminMsg))
		return;

	void* const pHist = Chat_S21GetHistory(this);
	if (!pHist)
		return;

	static Color adminColour(186, 13, 13, 255);
	const float flMessageShowDuration = Chat_DefaultShowDuration();
	const float flMessageFadeDuration = Chat_DefaultFadeDuration();

	Chat_S21InsertChar(pHist, '\n');
	Chat_S21InsertColor(pHist, static_cast<unsigned int>(
		(bAdminMsg ? adminColour : m_clrText).GetRawColor()));
	Chat_S21InsertFade(pHist, flMessageShowDuration, flMessageFadeDuration);
	Chat_S21InsertAscii(pHist, pszPrefixStr, pszPrefixStr ? V_strlen(pszPrefixStr) : 0);
	Chat_S21InsertAscii(pHist, ": ", 2);
	Chat_S21InsertColor(pHist, static_cast<unsigned int>(m_clrText.GetRawColor()));
	Chat_S21InsertAscii(pHist, pszMsgText, pszMsgText ? V_strlen(pszMsgText) : 0);
}

void CBaseHudChat::PrintSystemMsg(const char* const pszPrefixStr, const char* const pszMsgText, const int r, const int g, const int b)
{
	if (!Chat_RecipientAllowsServerMsg(false))
		return;

	void* const pHist = Chat_S21GetHistory(this);
	if (!pHist)
		return;

	const float flMessageShowDuration = Chat_DefaultShowDuration();
	const float flMessageFadeDuration = Chat_DefaultFadeDuration();
	const Color customColor(r, g, b, 255);

	Chat_S21InsertChar(pHist, '\n');
	Chat_S21InsertColor(pHist, static_cast<unsigned int>(customColor.GetRawColor()));
	Chat_S21InsertFade(pHist, flMessageShowDuration, flMessageFadeDuration);
	Chat_S21InsertAscii(pHist, pszPrefixStr, pszPrefixStr ? V_strlen(pszPrefixStr) : 0);
	Chat_S21InsertAscii(pHist, ": ", 2);
	Chat_S21InsertColor(pHist, static_cast<unsigned int>(m_clrText.GetRawColor()));
	Chat_S21InsertAscii(pHist, pszMsgText, pszMsgText ? V_strlen(pszMsgText) : 0);
}

void CBaseHudChat::PrintSystemMsg(const char* const pszPrefixStr, const char* const pszMsgText, const int r, const int g, const int b, const float flDuration, const float flFadeTime)
{
	if (!Chat_RecipientAllowsServerMsg(false))
		return;

	void* const pHist = Chat_S21GetHistory(this);
	if (!pHist)
		return;

	const Color customColor(r, g, b, 255);

	Chat_S21InsertChar(pHist, '\n');
	Chat_S21InsertColor(pHist, static_cast<unsigned int>(customColor.GetRawColor()));
	Chat_S21InsertFade(pHist, flDuration, flFadeTime);
	Chat_S21InsertAscii(pHist, pszPrefixStr, pszPrefixStr ? V_strlen(pszPrefixStr) : 0);

	if (pszMsgText && pszMsgText[0] != '\0')
	{
		Chat_S21InsertColor(pHist, static_cast<unsigned int>(m_clrText.GetRawColor()));
		Chat_S21InsertAscii(pHist, pszMsgText, V_strlen(pszMsgText));
	}
}

void CBaseHudChat::RenderChatBuilder(const ChatBuilderSeg_t* const pSegs, const int nCount, const bool bAdminMsg)
{
	if (!pSegs || nCount <= 0)
		return;

	if (!Chat_RecipientAllowsServerMsg(bAdminMsg))
		return;

	char* const pHead = reinterpret_cast<char*>(this);
	pHead[kS21HudChatReady] = 1;

	void* const pHist = *reinterpret_cast<void**>(pHead + kS21HudChatHistory);
	if (!pHist)
	{
		Warning(eDLL_T::CLIENT, "[CHATBUILDER] history pointer is null -- dropping\n");
		return;
	}

	void** const vtbl = *reinterpret_cast<void***>(pHist);
	using FnChar_t = void(*)(void*, int);
	using FnColor_t = void(*)(void*, unsigned int);
	using FnFade_t = void(*)(void*, float, float);
	const FnChar_t fnChar = reinterpret_cast<FnChar_t>(vtbl[kS21RichText_InsertChar]);
	const FnColor_t fnColor = reinterpret_cast<FnColor_t>(vtbl[kS21RichText_InsertColor]);
	const FnFade_t fnFade = reinterpret_cast<FnFade_t>(vtbl[kS21RichText_InsertFade]);

	static const Color rainbowColors[] = {
		Color(255, 0, 0, 255),
		Color(255, 127, 0, 255),
		Color(255, 255, 0, 255),
		Color(0, 255, 0, 255),
		Color(0, 0, 255, 255),
		Color(75, 0, 130, 255),
		Color(148, 0, 211, 255)
	};
	static const int numRainbowColors = V_ARRAYSIZE(rainbowColors);

	for (int i = 0; i < nCount && i < kChatBuilderMaxSegments; i++)
	{
		const ChatBuilderSeg_t& seg = pSegs[i];
		const int nLen = Min<int>(seg.textLen, kChatBuilderMaxSegText);
		if (nLen <= 0)
			continue;

		if (seg.flags & CHATBUILDER_F_NEWLINE)
			fnChar(pHist, '\n');

		const float flSustain = seg.sustainMs ? (seg.sustainMs / 1000.0f) : 8.0f;
		const float flFade = seg.fadeMs ? (seg.fadeMs / 1000.0f) : 2.0f;

		fnColor(pHist, static_cast<unsigned int>(Color(seg.r, seg.g, seg.b, 255).GetRawColor()));
		fnFade(pHist, flSustain, flFade);

		if (seg.op == CHATBUILDER_OP_RAINBOW)
		{
			for (int c = 0; c < nLen; c++)
			{
				fnColor(pHist, static_cast<unsigned int>(rainbowColors[c % numRainbowColors].GetRawColor()));
				Chat_S21InsertAscii(pHist, &seg.text[c], 1);
			}
		}
		else
		{
			Chat_S21InsertAscii(pHist, seg.text, nLen);
		}
	}

	static int s_nPrinted = 0;
	if (s_nPrinted < 8)
	{
		s_nPrinted++;
		Msg(eDLL_T::CLIENT, "[CHATBUILDER] colored %d segs r=%u g=%u b=%u\n",
			nCount, pSegs[0].r, pSegs[0].g, pSegs[0].b);
	}
}

//-----------------------------------------------------------------------------
// Purpose: decodes an svc_ChatBuilder body. Every field is re-validated here;
//          a malformed or truncated stream renders nothing rather than
//          rendering a partially-read segment.
//-----------------------------------------------------------------------------
void ChatBuilder_ApplyS2CPayload(const uint8_t* const pData, const int nBytes)
{
	if (!pData)
		return;

	if (!g_ppHudChat || !*g_ppHudChat)
	{
		Warning(eDLL_T::CLIENT, "[CHATBUILDER] g_pHudChat is null -- dropping %d bytes\n", nBytes);
		return;
	}

	if (nBytes < kChatBuilderStreamHeaderBytes || nBytes > kChatBuilderMaxWireBytes)
	{
		Warning(eDLL_T::CLIENT, "[CHATBUILDER] payload of %d bytes out of bounds -- dropping\n", nBytes);
		return;
	}

	const uint8_t nStreamFlags = pData[0];
	const int nCount = pData[1];
	const int nSenderSlot = pData[2];

	if (nCount <= 0 || nCount > kChatBuilderMaxSegments)
	{
		Warning(eDLL_T::CLIENT, "[CHATBUILDER] segment count %d out of bounds -- dropping\n", nCount);
		return;
	}

	ChatBuilderSeg_t segs[kChatBuilderMaxSegments] = {};
	int nRead = 0;
	int nPos = kChatBuilderStreamHeaderBytes;

	for (int i = 0; i < nCount; i++)
	{
		if (nPos + kChatBuilderSegHeaderBytes > nBytes)
		{
			Warning(eDLL_T::CLIENT, "[CHATBUILDER] truncated segment header at %d/%d -- dropping\n", nPos, nBytes);
			return;
		}

		ChatBuilderSeg_t& seg = segs[nRead];

		seg.op = pData[nPos + 0];
		seg.flags = pData[nPos + 1] & CHATBUILDER_F_NEWLINE;
		seg.r = pData[nPos + 2];
		seg.g = pData[nPos + 3];
		seg.b = pData[nPos + 4];
		seg.sustainMs = static_cast<uint16_t>(pData[nPos + 5] | (pData[nPos + 6] << 8));
		seg.fadeMs = static_cast<uint16_t>(pData[nPos + 7] | (pData[nPos + 8] << 8));

		const int nTextLen = pData[nPos + 9];
		nPos += kChatBuilderSegHeaderBytes;

		if (nTextLen > kChatBuilderMaxSegText || nPos + nTextLen > nBytes)
		{
			Warning(eDLL_T::CLIENT, "[CHATBUILDER] segment text length %d out of bounds -- dropping\n", nTextLen);
			return;
		}

		if (seg.op >= CHATBUILDER_OP_COUNT)
			seg.op = CHATBUILDER_OP_TEXT;

		seg.sustainMs = Min(seg.sustainMs, kChatBuilderMaxSustainMs);
		seg.fadeMs = Min(seg.fadeMs, kChatBuilderMaxFadeMs);

		// The sender already filtered this, but the sender is the network.
		char szRaw[kChatBuilderMaxSegText + 1];
		memcpy(szRaw, &pData[nPos], nTextLen);
		szRaw[nTextLen] = '\0';
		nPos += nTextLen;

		seg.textLen = static_cast<uint8_t>(
			ChatBuilder_SanitizeText(szRaw, seg.text, kChatBuilderMaxSegText));

		if (seg.textLen > 0)
			nRead++;
	}

	if (nStreamFlags & CHATBUILDER_SF_PLAYERCHAT)
	{
		if (!Chat_AllowsPlayerChat() || Chat_IsSlotMuted(nSenderSlot))
			return;

		// Gated above by the player-chat rules, so the render must not also
		// apply the server-message ones (hudchat_ignore_server_messages).
		(*g_ppHudChat)->RenderChatBuilder(segs, nRead, true);
		return;
	}

	const bool bAdminMsg = (nStreamFlags & CHATBUILDER_SF_ADMIN) != 0;
	(*g_ppHudChat)->RenderChatBuilder(segs, nRead, bAdminMsg);
}
