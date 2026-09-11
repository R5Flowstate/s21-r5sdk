//=============================================================================//
//
// Purpose: server-authoritative sound broadcast bridge. See sound.h.
//
//=============================================================================//
#include "core/stdafx.h"
#include "common/netmessages.h"
#include "engine/server/server.h"
#include "game/server/baseentity.h"
#include "game/server/sound.h"

static ConVar bridge_sound_csomet_bypass("bridge_sound_csomet_bypass", "1", FCVAR_RELEASE,
	"Hand S21 a synthetic Miles event record when the dedicated server's permanently empty CSOMET cache misses, "
	"so every EmitSound* native reaches its CTESoundDispatch send (0 = S21 behaviour, total silence).");

static ConVar bridge_sound_class("bridge_sound_class", "1", FCVAR_RELEASE,
	"Sound class stored in the synthetic record. 0<v<5 = one dispatch to the whole recipient filter, "
	"5..12 = per-recipient dispatch, >=12 = entity important-sound slot.", true, 0.f, true, 32.f);

static ConVar bridge_sound_max_dist("bridge_sound_max_dist", "0", FCVAR_RELEASE,
	"Max audible distance in the synthetic record (0 = no distance culling, everyone in the filter hears it).",
	true, 0.f, true, 100000.f);

static ConVar bridge_sound_trace("bridge_sound_trace", "0", FCVAR_DEVELOPMENTONLY,
	"[SOUND-BRIDGE] log every synthesized Miles event lookup (name + hash).");

static ConVar bridge_sound_svc_broadcast("bridge_sound_svc_broadcast", "0", FCVAR_RELEASE,
	"Legacy EmitSoundOnEntity -> bridge-native SVC_Sounds broadcast. Superseded by the CSOMET bypass; "
	"running both plays entity sounds twice.");

static ConVar bridge_sound_multi_exclude_diag("bridge_sound_multi_exclude_diag", "0",
	FCVAR_DEVELOPMENTONLY,
	"[SND-EXCLUDE] Log multi-player sound excludes (recipients removed from filter).");

// Handed back on a cache miss. Nothing reads the hash or chain fields once the
// lookup has returned, so one shared instance is safe.
static SoundBridgeMilesRecord_t s_syntheticRecord = {};

// Pending recipient excludes for the next filter populate only. thread_local
// because sound + SetAbsOrigin take locks and filter populate is not main-thread-only.
static constexpr int kMaxRecipientExcludes = 8;
static thread_local int s_nPendingExcludeSlots[kMaxRecipientExcludes] = {};
static thread_local int s_nPendingExcludeCount = 0;

//-----------------------------------------------------------------------------
// Purpose: Miles hash + CSOMET probe. A synthetic record clears the empty-cache gate.
//-----------------------------------------------------------------------------
void* Hook_SoundBridge_MilesEventLookup(const char* pszName, uint64_t* pOutHash, int* pOutSoundClass)
{
	void* const pRecord = v_SoundBridge_MilesEventLookup(pszName, pOutHash, pOutSoundClass);

	if (pRecord || !bridge_sound_csomet_bypass.GetBool())
		return pRecord;

	s_syntheticRecord.m_flMaxAudibleDist = bridge_sound_max_dist.GetFloat();
	s_syntheticRecord.m_flSoundClass = bridge_sound_class.GetFloat();

	// On a miss the engine probe wrote -1.0f on the class float, which would take the per-recipient path.
	if (pOutSoundClass)
		*pOutSoundClass = *reinterpret_cast<const int*>(&s_syntheticRecord.m_flSoundClass);

	// Pin miles_server_useSoundIDTable on first emit: gamedll registers it
	// after detour bring-up. 0 wires the 64-bit hash (m_networkTableID -1).
	static bool s_bAnnounced = false;
	if (!s_bAnnounced)
	{
		s_bAnnounced = true;

		const ConVar* const pDisable = g_pCVar->FindVar("miles_server_disable_sounds");
		ConVar* const pIdTable = g_pCVar->FindVar("miles_server_useSoundIDTable");

		int nIdTable = -1;
		if (pIdTable)
		{
			nIdTable = pIdTable->GetInt();
			if (nIdTable != 0)
			{
				pIdTable->SetValue(0);
				Msg(eDLL_T::SERVER, "[SOUND-BRIDGE] miles_server_useSoundIDTable %d -> 0 "
					"(wire the full 64-bit hash)\n", nIdTable);
				nIdTable = 0;
			}
		}

		Msg(eDLL_T::SERVER, "[SOUND-BRIDGE] CSOMET bypass live (class %.2f, maxDist %.1f) -- "
			"miles_server_disable_sounds=%d miles_server_useSoundIDTable=%d\n",
			s_syntheticRecord.m_flSoundClass, s_syntheticRecord.m_flMaxAudibleDist,
			pDisable ? pDisable->GetInt() : -1, nIdTable);
	}

	if (bridge_sound_trace.GetBool())
		DevMsg(eDLL_T::SERVER, "[SOUND-BRIDGE] synth '%s' hash %016llX\n",
			pszName, pOutHash ? *pOutHash : 0ULL);

	return &s_syntheticRecord;
}

//-----------------------------------------------------------------------------
// Purpose: 64-bit FNV-1a Miles hash. Multiply-then-XOR. 'A'-'Z' lowercase, '.' -> '_'.
//-----------------------------------------------------------------------------
uint64_t SoundBridge_ComputeMilesHash(const char* pszName)
{
	uint64_t hash = 0xCBF29CE484222325ULL;
	static constexpr uint64_t FNV_PRIME = 0x100000001B3ULL;

	for (const unsigned char* p = reinterpret_cast<const unsigned char*>(pszName); *p; ++p)
	{
		const unsigned char c = *p;
		unsigned char normalized;

		if (c >= 'A' && c <= 'Z')
			normalized = c + 32;
		else if (c == '.')
			normalized = '_';
		else
			normalized = c;

		hash = normalized ^ (FNV_PRIME * hash);
	}

	return hash;
}

//-----------------------------------------------------------------------------
// Purpose: EmitSoundOnEntity. Call through first, then optional SVC_Sounds broadcast.
//-----------------------------------------------------------------------------
__m128 Hook_EmitSoundOnEntity(CBaseEntity* pEntity, const char* pszSoundName)
{
	const __m128 result = v_EmitSoundOnEntity(pEntity, pszSoundName);

	if (!bridge_sound_svc_broadcast.GetBool())
		return result;

	if (!pEntity || !VALID_CHARSTAR(pszSoundName))
		return result;

	if (!g_pServer)
		return result;

	const int entIndex = static_cast<int>(pEntity->GetEdict());

	// Wire field is 14 bits; guard so a larger entity budget cannot silently truncate the index.
	if (entIndex < 0 || entIndex > 0x3FFF)
	{
		Warning(eDLL_T::SERVER, "[SOUND-BRIDGE] entity index %d out of 14-bit range for '%s', dropping\n",
			entIndex, pszSoundName);
		return result;
	}

	const uint64_t hash = SoundBridge_ComputeMilesHash(pszSoundName);
	SVC_Sounds message(g_pServer->GetTick(), hash, entIndex);

	g_pServer->BroadcastMessage(&message, true, false);

	return result;
}

//-----------------------------------------------------------------------------
// Purpose: drop armed edict slots from the Miles recipient filter after it is built.
//-----------------------------------------------------------------------------
char Hook_SoundBridge_FilterPopulate(void* pFilter, const float* pOrigin, void* pRecord)
{
	const char result = v_SoundBridge_FilterPopulate
		? v_SoundBridge_FilterPopulate(pFilter, pOrigin, pRecord)
		: 0;

	const int nCount = s_nPendingExcludeCount;
	if (nCount <= 0)
		return result;

	int nRemoved = 0;
	if (v_SoundBridge_RemoveRecipient && pFilter)
	{
		for (int i = 0; i < nCount; ++i)
		{
			v_SoundBridge_RemoveRecipient(pFilter, s_nPendingExcludeSlots[i]);
			++nRemoved;
		}
	}

	// Consume regardless of RemoveRecipient -- an armed list must never leak.
	s_nPendingExcludeCount = 0;

	if (bridge_sound_multi_exclude_diag.GetBool())
	{
		Msg(eDLL_T::SERVER,
			"[SND-EXCLUDE] removed %d recipients from filter\n", nRemoved);
	}

	return result;
}

void SoundBridge_ArmRecipientExcludes(const int* pEdictSlots, int nCount)
{
	s_nPendingExcludeCount = 0;

	if (!pEdictSlots || nCount <= 0)
		return;

	int nCopy = nCount;
	if (nCopy > kMaxRecipientExcludes)
	{
		static bool s_bCapWarned = false;
		if (!s_bCapWarned)
		{
			s_bCapWarned = true;
			Warning(eDLL_T::SERVER,
				"[SND-EXCLUDE] exclude list capped at %d (got %d)\n",
				kMaxRecipientExcludes, nCount);
		}
		nCopy = kMaxRecipientExcludes;
	}

	for (int i = 0; i < nCopy; ++i)
		s_nPendingExcludeSlots[i] = pEdictSlots[i];
	s_nPendingExcludeCount = nCopy;
}

void SoundBridge_DisarmRecipientExcludes(void)
{
	s_nPendingExcludeCount = 0;
}
