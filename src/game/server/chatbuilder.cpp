//=============================================================================//
//
// Purpose: dedicated-server side of the structured chat builder.
//
//=============================================================================//
#include "core/stdafx.h"
#include "common/netmessages.h"
#include "engine/server/server.h"
#include "engine/client/client.h"
#include "vscript/languages/squirrel_re/include/sqtable.h"
#include "vscript/languages/squirrel_re/include/sqarray.h"
#include "vscript/languages/squirrel_re/include/sqstring.h"
#include "game/server/chatbuilder.h"

//-----------------------------------------------------------------------------
// Master switch for the script-side announcement rotator. Live-settable over
// RCON. Text and timings live in platform/datatable/chat_announcements.csv.
//-----------------------------------------------------------------------------
static ConVar bridge_chat_announce("bridge_chat_announce", "0", FCVAR_RELEASE,
	"Broadcast the rotating server chat announcements.");

//-----------------------------------------------------------------------------
// Purpose: carries a bounded segment array. S21 has no matching slot, so the
//          body is decoded by the bridge and never reaches a native handler.
//-----------------------------------------------------------------------------
class SVC_ChatBuilder : public CNetMessage
{
public:
	SVC_ChatBuilder()
	{
		m_nGroup = NetMessageGroup::NoReplay;
		m_bReliable = true;
		m_nStreamFlags = 0;
		m_nCount = 0;
		m_nSenderSlot = 0;
	}

	virtual bool			ReadFromBuffer(bf_read* buffer) { return !buffer->IsOverflowed(); }
	virtual bool			WriteToBuffer(bf_write* buffer);
	virtual bool			Process(void) { return true; }

	virtual int				GetType(void) const { return kChatBuilderS2CType; }
	virtual const char*		GetName(void) const { return "svc_ChatBuilder"; }
	virtual const char*		ToString(void) const { return "svc_ChatBuilder"; }
	virtual size_t			GetSize(void) const { return sizeof(SVC_ChatBuilder); }

	uint8_t          m_nStreamFlags;
	uint8_t          m_nSenderSlot;
	int              m_nCount;
	ChatBuilderSeg_t m_Segs[kChatBuilderMaxSegments];
};

bool SVC_ChatBuilder::WriteToBuffer(bf_write* buffer)
{
	const int nCount = Clamp(m_nCount, 0, kChatBuilderMaxSegments);

	int nTextLen[kChatBuilderMaxSegments];
	int nPayloadBytes = kChatBuilderStreamHeaderBytes;
	for (int i = 0; i < nCount; i++)
	{
		nTextLen[i] = Clamp(static_cast<int>(m_Segs[i].textLen), 0, kChatBuilderMaxSegText);
		nPayloadBytes += kChatBuilderSegHeaderBytes + nTextLen[i];
	}

	buffer->WriteShort(nPayloadBytes);
	buffer->WriteByte(m_nStreamFlags);
	buffer->WriteByte(nCount);
	buffer->WriteByte(m_nSenderSlot);

	for (int i = 0; i < nCount; i++)
	{
		const ChatBuilderSeg_t& seg = m_Segs[i];

		buffer->WriteByte(seg.op);
		buffer->WriteByte(seg.flags);
		buffer->WriteByte(seg.r);
		buffer->WriteByte(seg.g);
		buffer->WriteByte(seg.b);
		buffer->WriteWord(seg.sustainMs);
		buffer->WriteWord(seg.fadeMs);
		buffer->WriteByte(static_cast<uint8_t>(nTextLen[i]));

		for (int c = 0; c < nTextLen[i]; c++)
			buffer->WriteByte(static_cast<unsigned char>(seg.text[c]));
	}

	return !buffer->IsOverflowed();
}

//-----------------------------------------------------------------------------
// Purpose: re-validates every field the caller supplied, so a bad authoring
//          site can only lose its own segment, never shape the wire.
//-----------------------------------------------------------------------------
static int ChatBuilder_Build(SVC_ChatBuilder& msg, const ChatBuilderSeg_t* const pSegs,
	const int nCount, const bool bAdminMsg)
{
	msg.m_nStreamFlags = bAdminMsg ? CHATBUILDER_SF_ADMIN : 0;
	msg.m_nCount = 0;
	memset(msg.m_Segs, 0, sizeof(msg.m_Segs));

	if (!pSegs || nCount <= 0)
		return 0;

	if (nCount > kChatBuilderMaxSegments)
		Warning(eDLL_T::SERVER, "[CHATBUILDER] %d segments requested, %d is the ceiling -- dropping the tail\n",
			nCount, kChatBuilderMaxSegments);

	const int nUse = Min(nCount, kChatBuilderMaxSegments);

	for (int i = 0; i < nUse; i++)
	{
		const ChatBuilderSeg_t& in = pSegs[i];
		ChatBuilderSeg_t& out = msg.m_Segs[msg.m_nCount];

		char szIn[kChatBuilderMaxSegText + 1];
		const int nInLen = Min(static_cast<int>(in.textLen), kChatBuilderMaxSegText);
		memcpy(szIn, in.text, nInLen);
		szIn[nInLen] = '\0';

		char szText[kChatBuilderMaxSegText];
		const int nLen = ChatBuilder_SanitizeText(szIn, szText, sizeof(szText));

		if (nLen <= 0)
			continue;

		out.op = (in.op < CHATBUILDER_OP_COUNT) ? in.op : CHATBUILDER_OP_TEXT;
		out.flags = in.flags & CHATBUILDER_F_NEWLINE;
		out.r = in.r;
		out.g = in.g;
		out.b = in.b;
		out.sustainMs = Min(in.sustainMs, kChatBuilderMaxSustainMs);
		out.fadeMs = Min(in.fadeMs, kChatBuilderMaxFadeMs);
		out.textLen = static_cast<uint8_t>(nLen);
		memcpy(out.text, szText, nLen);

		msg.m_nCount++;
	}

	return msg.m_nCount;
}

bool ChatBuilder_SendToClient(CClient* const pClient, const ChatBuilderSeg_t* const pSegs,
	const int nCount, const bool bAdminMsg)
{
	if (!pClient)
		return false;

	SVC_ChatBuilder msg;
	if (ChatBuilder_Build(msg, pSegs, nCount, bAdminMsg) <= 0)
		return false;

	return pClient->SendNetMsgEx(&msg, false, true, false);
}

//-----------------------------------------------------------------------------
// Purpose: one player's chat line, carried as text the recipient can render
// without owning an entity for the sender.
//-----------------------------------------------------------------------------
bool ChatBuilder_SendPlayerChat(CClient* const pClient, const int nSenderSlot,
	const char* const pszName, const char* const pszText, const bool bTeamChat,
	const bool bReliable)
{
	if (!pClient || !pszText)
		return false;

	ChatBuilderSeg_t segs[kChatBuilderMaxSegments];
	memset(segs, 0, sizeof(segs));

	char szName[kChatBuilderMaxSegText];
	const int nNameLen = ChatBuilder_SanitizeText(pszName ? pszName : "", szName, sizeof(szName) - 2);

	int nSegs = 0;
	ChatBuilderSeg_t& name = segs[nSegs++];
	name.op = CHATBUILDER_OP_TEXT;
	name.flags = CHATBUILDER_F_NEWLINE;
	name.r = bTeamChat ? 110 : 245;
	name.g = bTeamChat ? 190 : 200;
	name.b = bTeamChat ? 255 : 90;
	memcpy(name.text, szName, nNameLen);
	name.text[nNameLen] = ':';
	name.text[nNameLen + 1] = ' ';
	name.textLen = static_cast<uint8_t>(nNameLen + 2);

	char szText[kChatBuilderMaxSegText * (kChatBuilderMaxSegments - 1) + 1];
	const int nTextLen = ChatBuilder_SanitizeText(pszText, szText, sizeof(szText) - 1);
	if (nTextLen <= 0)
		return false;

	for (int nPos = 0; nPos < nTextLen && nSegs < kChatBuilderMaxSegments; nPos += kChatBuilderMaxSegText)
	{
		const int nChunk = Min(nTextLen - nPos, kChatBuilderMaxSegText);
		ChatBuilderSeg_t& body = segs[nSegs++];

		body.op = CHATBUILDER_OP_TEXT;
		body.flags = 0;
		body.r = 255;
		body.g = 255;
		body.b = 255;
		memcpy(body.text, &szText[nPos], nChunk);
		body.textLen = static_cast<uint8_t>(nChunk);
	}

	SVC_ChatBuilder msg;
	if (ChatBuilder_Build(msg, segs, nSegs, false) <= 0)
		return false;

	msg.m_nStreamFlags = CHATBUILDER_SF_PLAYERCHAT;
	msg.m_nSenderSlot = static_cast<uint8_t>(Clamp(nSenderSlot, 0, 255));
	msg.m_bReliable = bReliable;

	return pClient->SendNetMsgEx(&msg, false, bReliable, false);
}

void ChatBuilder_Broadcast(const ChatBuilderSeg_t* const pSegs, const int nCount,
	const bool bAdminMsg)
{
	if (!g_pServer)
		return;

	SVC_ChatBuilder msg;
	if (ChatBuilder_Build(msg, pSegs, nCount, bAdminMsg) <= 0)
		return;

	g_pServer->BroadcastMessage(&msg, true, true);
}

//-----------------------------------------------------------------------------
// Purpose: script-facing segment table reader.
//-----------------------------------------------------------------------------
static bool ChatBuilder_KeyIs(const SQObject& key, const char* const pszName)
{
	if (sq_type(key) != OT_STRING)
		return false;

	const SQString* const pStr = _string(key);
	return pStr && V_stricmp(pStr->_val, pszName) == 0;
}

static int ChatBuilder_ValueToInt(const SQObject& val, const int nFallback)
{
	switch (sq_type(val))
	{
	case OT_INTEGER:
	case OT_BOOL:
		return static_cast<int>(_integer(val));
	case OT_FLOAT:
	{
		const float fl = static_cast<float>(_float(val));
		if (fl != fl)
			return nFallback;
		if (fl >= 2147483647.0f)
			return 2147483647;
		if (fl <= -2147483648.0f)
			return (-2147483647 - 1);
		return static_cast<int>(fl);
	}
	default:
		return nFallback;
	}
}

static float ChatBuilder_ValueToFloat(const SQObject& val, const float flFallback)
{
	switch (sq_type(val))
	{
	case OT_FLOAT:   return static_cast<float>(_float(val));
	case OT_INTEGER:
	case OT_BOOL:    return static_cast<float>(_integer(val));
	default:         return flFallback;
	}
}

static bool ChatBuilder_ReadSeg(const SQTable* const pTable, ChatBuilderSeg_t& seg)
{
	const char* pszText = nullptr;
	int r = 255, g = 255, b = 255;
	float flSustain = 0.0f, flFade = 0.0f;
	bool bRainbow = false, bNewline = true;

	SQ_FOR_EACH_TABLE(pTable, i)
	{
		const SQTable::_HashNode& node = pTable->_nodes[i];

		if (sq_isnull(node.key))
			continue;

		if (ChatBuilder_KeyIs(node.key, "text"))
		{
			if (sq_type(node.val) == OT_STRING)
				pszText = _string(node.val)->_val;
		}
		else if (ChatBuilder_KeyIs(node.key, "r"))        r = ChatBuilder_ValueToInt(node.val, r);
		else if (ChatBuilder_KeyIs(node.key, "g"))        g = ChatBuilder_ValueToInt(node.val, g);
		else if (ChatBuilder_KeyIs(node.key, "b"))        b = ChatBuilder_ValueToInt(node.val, b);
		else if (ChatBuilder_KeyIs(node.key, "sustain"))  flSustain = ChatBuilder_ValueToFloat(node.val, flSustain);
		else if (ChatBuilder_KeyIs(node.key, "fade"))     flFade = ChatBuilder_ValueToFloat(node.val, flFade);
		else if (ChatBuilder_KeyIs(node.key, "rainbow"))  bRainbow = ChatBuilder_ValueToInt(node.val, 0) != 0;
		else if (ChatBuilder_KeyIs(node.key, "newline"))  bNewline = ChatBuilder_ValueToInt(node.val, 1) != 0;
	}

	const int nLen = ChatBuilder_SanitizeText(pszText, seg.text, kChatBuilderMaxSegText);

	if (nLen <= 0)
		return false;

	seg.op = bRainbow ? CHATBUILDER_OP_RAINBOW : CHATBUILDER_OP_TEXT;
	seg.flags = bNewline ? CHATBUILDER_F_NEWLINE : 0;
	seg.r = static_cast<uint8_t>(ChatBuilder_ClampColor(r));
	seg.g = static_cast<uint8_t>(ChatBuilder_ClampColor(g));
	seg.b = static_cast<uint8_t>(ChatBuilder_ClampColor(b));
	seg.sustainMs = ChatBuilder_ClampMs(flSustain, kChatBuilderMaxSustainMs);
	seg.fadeMs = ChatBuilder_ClampMs(flFade, kChatBuilderMaxFadeMs);
	seg.textLen = static_cast<uint8_t>(nLen);

	return true;
}

int ChatBuilder_ParseSegArray(HSQUIRRELVM v, const SQInteger nStackIdx,
	ChatBuilderSeg_t* const pOut, const int nOutMax)
{
	SQObject arrObj;
	if (SQ_FAILED(sq_getstackobj(v, nStackIdx, &arrObj)) || !sq_isarray(arrObj))
		return -1;

	const SQArray* const pArray = _array(arrObj);
	if (!pArray || !pOut || nOutMax <= 0)
		return -1;

	const SQInteger nSize = pArray->Size();

	memset(pOut, 0, sizeof(*pOut) * nOutMax);

	if (nSize > nOutMax)
		Warning(eDLL_T::SERVER, "[CHATBUILDER] %d segments passed, %d is the ceiling -- dropping the tail\n",
			static_cast<int>(nSize), nOutMax);

	const SQInteger nScan = (nSize < nOutMax) ? nSize : nOutMax;
	int nCount = 0;
	for (SQInteger i = 0; i < nScan; i++)
	{
		const SQObject& elem = pArray->_values[i];

		if (sq_type(elem) != OT_TABLE)
			continue;

		if (ChatBuilder_ReadSeg(_table(elem), pOut[nCount]))
			nCount++;
	}

	return nCount;
}

bool ChatBuilder_ReadOptionalBool(HSQUIRRELVM v, const SQInteger nStackIdx, const bool bDefault)
{
	if (!v || sq_gettop(v) < nStackIdx)
		return bDefault;

	SQObject obj;
	if (SQ_FAILED(sq_getstackobj(v, nStackIdx, &obj)))
		return bDefault;

	switch (sq_type(obj))
	{
	case OT_BOOL:
	case OT_INTEGER:
		return _integer(obj) != 0;
	default:
		return bDefault;
	}
}
