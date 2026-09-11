//=============================================================================//
//
// Purpose: structured chat rendering payload, shared by the dedicated server's
//          encoder and the S21 client's renderer.
//
//=============================================================================//
#ifndef GAME_SHARED_CHATBUILDER_H
#define GAME_SHARED_CHATBUILDER_H

// S3 netmessage type. S21 has no equivalent slot, so the bridge decodes the
// body client-side the same way svc_DebugOverlay (69) is handled.
static constexpr int kChatBuilderS2CType = 70;

static constexpr int kChatBuilderMaxSegments = 8;
static constexpr int kChatBuilderMaxSegText = 64;

// op, flags, r, g, b, sustainMs, fadeMs, textLen
static constexpr int kChatBuilderSegHeaderBytes = 10;
static constexpr int kChatBuilderStreamHeaderBytes = 3; // streamFlags, segCount, senderSlot

static constexpr int kChatBuilderMaxWireBytes = kChatBuilderStreamHeaderBytes +
	kChatBuilderMaxSegments * (kChatBuilderSegHeaderBytes + kChatBuilderMaxSegText);

enum ChatBuilderOp_t : uint8_t
{
	CHATBUILDER_OP_TEXT = 0,
	CHATBUILDER_OP_RAINBOW = 1,
	CHATBUILDER_OP_COUNT
};

enum ChatBuilderSegFlags_t : uint8_t
{
	CHATBUILDER_F_NEWLINE = (1 << 0),
};

enum ChatBuilderStreamFlags_t : uint8_t
{
	CHATBUILDER_SF_ADMIN = (1 << 0), // bypasses the recipient's chat filters
	// One player talking, not the server: the recipient applies its player-chat
	// rules (hud_setting_chat, per-slot mute) instead of its server-message ones.
	CHATBUILDER_SF_PLAYERCHAT = (1 << 1),
};

// Fixed-shape segment. There is no in-band delimiter and no escape sequence:
// text length is explicit, so text can never be re-read as formatting.
struct ChatBuilderSeg_t
{
	uint8_t  op;
	uint8_t  flags;
	uint8_t  r;
	uint8_t  g;
	uint8_t  b;
	uint16_t sustainMs; // 0 = the recipient's hudchat default
	uint16_t fadeMs;    // 0 = the recipient's hudchat default
	uint8_t  textLen;
	char     text[kChatBuilderMaxSegText];
};

// Timings travel as unsigned milliseconds so no negative, NaN or infinite
// duration can ever reach the client's fade compare.
static constexpr uint16_t kChatBuilderMinMs = 100;
static constexpr uint16_t kChatBuilderMaxSustainMs = 60000;
static constexpr uint16_t kChatBuilderMaxFadeMs = 10000;

// Rendered verbatim, so both the authoring and the receiving side restrict it
// to printable ASCII. Returns the filtered length. The input walk is capped so
// a field that is not NUL-terminated cannot run away.
static constexpr int kChatBuilderMaxSanitizeWalk = 256;

inline int ChatBuilder_SanitizeText(const char* const pszIn, char* const pOut, const int nOutMax)
{
	if (!pszIn || !pOut || nOutMax <= 0)
		return 0;

	int nOut = 0;
	for (int i = 0; i < kChatBuilderMaxSanitizeWalk && nOut < nOutMax; i++)
	{
		const unsigned char c = static_cast<unsigned char>(pszIn[i]);
		if (c == '\0')
			break;
		if (c >= 0x20 && c < 0x7F)
			pOut[nOut++] = static_cast<char>(c);
	}
	return nOut;
}

// Returns 0 for "use the recipient's default", which is also what a negative,
// zero or NaN input collapses to.
inline uint16_t ChatBuilder_ClampMs(const float flSeconds, const uint16_t nMaxMs)
{
	if (!(flSeconds > 0.0f))
		return 0;

	const float flMs = flSeconds * 1000.0f;

	if (flMs < static_cast<float>(kChatBuilderMinMs))
		return kChatBuilderMinMs;
	if (flMs > static_cast<float>(nMaxMs))
		return nMaxMs;

	return static_cast<uint16_t>(flMs);
}

inline int ChatBuilder_ClampColor(const int nValue)
{
	if (nValue < 0)
		return 0;
	if (nValue > 255)
		return 255;
	return nValue;
}

#endif // GAME_SHARED_CHATBUILDER_H
