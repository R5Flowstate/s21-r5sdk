#ifndef HUD_BASECHAT_H
#define HUD_BASECHAT_H

//=============================================================================//
//
// Purpose: HUD base chat panel
//
//=============================================================================//
#include "vgui_controls/EditablePanel.h"
#include "vgui_controls/TextEntry.h"
#include "vgui_controls/RichText.h"
#include "game/shared/chatbuilder.h"

class CBaseHudChatInputLine;
class CHudChatHistory;

class CBaseHudChat : public vgui::EditablePanel
{
public:
	void PrintSystemMsg(const char* const pszPrefixStr, const char* const pszMsgText, const bool bAdminMsg);
	void PrintSystemMsg(const char* const pszPrefixStr, const char* const pszMsgText, const int r, const int g, const int b);
	void PrintSystemMsg(const char* const pszPrefixStr, const char* const pszMsgText, const int r, const int g, const int b, const float flDuration, const float flFadeTime);

	// ChatBuilder API - renders a validated segment array
	void RenderChatBuilder(const ChatBuilderSeg_t* const pSegs, const int nCount, const bool bAdminMsg);

private:
	char m_gap02C8[8];
	unsigned int m_hChatFont;
	int16_t m_hRuiFont;
	int16_t m_hRuiAsianFont;
	int m_RuiFontHeight;
	int m_RuiMinFontHeight;
	char m_gap2D4[8];
	int m_dword2E8;
	char m_gap2EC[12];
	Color m_TeamColors[2];
	Color m_clrText;
	char m_gap308[20];
	CBaseHudChatInputLine* m_pInputLine;
	CHudChatHistory* m_pChatHistory;
	char m_gap328[16];
};

static_assert(sizeof(CBaseHudChat) == 0x340);

class CBaseHudChatEntry : public vgui::TextEntry
{
	char m_gap000[12];
	CBaseHudChat* m_pHudChat;
};

class CBaseHudChatInputLine : public vgui::Panel
{
private:
	CBaseHudChat* m_pHudChat;
	class Label* m_pPrompt;
	CBaseHudChatEntry* m_pInput;
};

static_assert(sizeof(CBaseHudChatInputLine) == 0x290);

class CHudChatHistory : public vgui::RichText
{
	char m_gap000[8];
};

static_assert(sizeof(CHudChatHistory) == 0x3D0);

inline CBaseHudChat** g_ppHudChat;
inline int64_t (*v_CBaseHudChat_PrintSystem)(const char*, const char*, int) = nullptr;

// Decodes an svc_ChatBuilder body off the bridge and renders it.
void ChatBuilder_ApplyS2CPayload(const uint8_t* const pData, const int nBytes);

// hud_setting_chat: 0=Full, 1=opt-out server msgs, 2=disconnect.
bool Chat_AllowsPlayerChat(void);

// Engine ceiling is 128 players; the slot arrives as one payload byte.
constexpr int kChatMaxSenderSlots = 256;

// Per-sender chat mute, keyed by the SayText payload's sender slot.
void Chat_SetSlotMuted(const int nSlot, const bool bMuted);
bool Chat_IsSlotMuted(const int nSlot);
void Chat_ClearMutedSlots(void);

class VHudChat : public IDetour
{
	virtual void GetAdr(void) const 
	{
		LogVarAdr("g_pHudChat", g_ppHudChat);
		LogFunAdr("CBaseHudChat_PrintSystem", v_CBaseHudChat_PrintSystem);
	}

	virtual void GetFun(void) const
	{
#if defined(CLIENT_DLL)
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 48 89 6C 24 ?? 56 57 41 56 48 81 EC ?? ?? ?? ?? 48 8B 1D")
			.GetPtr(v_CBaseHudChat_PrintSystem);
		if (!v_CBaseHudChat_PrintSystem)
			Warning(eDLL_T::CLIENT, "[CHATBUILDER] PrintSystem pattern unresolved\n");
#endif
	}
	virtual void GetVar(void) const 
	{
#if defined(CLIENT_DLL)
		// S21 ctor: insert this into the panel list, then store the head.
		CMemory found = Module_FindPattern(g_GameDll,
			"48 89 B0 A8 02 00 00 48 8B 05 ?? ?? ?? ?? 48 89 86 A0 02 00 00 48 89 35 ?? ?? ?? ??");
		if (!found)
			Warning(eDLL_T::CLIENT, "[CHATBUILDER] g_pHudChat pattern unresolved\n");
		else
			found.Offset(0x15).ResolveRelativeAddressSelf(0x3, 0x7).GetPtr(g_ppHudChat);
#else
		Module_FindPattern(g_GameDll, "48 8B 1D ?? ?? ?? ?? 8B 70").ResolveRelativeAddressSelf(0x3, 0x7).GetPtr(g_ppHudChat);
#endif
	}
	virtual void GetCon(void) const {}
	virtual void Detour(const bool bAttach) const {};
};

#endif HUD_BASECHAT_H
