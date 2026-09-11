//=============================================================================//
//
// Purpose: Clamp OOB bodypart indices on C_BaseAnimating skin/body change so map-load cannot AV on a bad table slot.
// Hook the skin/body change path: clamp index args and stored previous/current indices against studiohdr bodypart count; SEH-wrap the original.
// ConVar sdk_body_skin_guard (default 1). Zero = pure pass-through.
//
//=============================================================================//
#include "core/stdafx.h"
#include "core/logdef.h"
#include "tier0/dbg.h"
#include "tier0/memaddr.h"
#include "tier0/module.h"
#include "tier1/cvar.h"
#include "game/client/body_skin.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Entity / studio offsets verified against S21.
static constexpr ptrdiff_t ENT_OFF_BODY_PREV   = 0xD68;  // +3424 -- previous body/skin index (u16)
static constexpr ptrdiff_t ENT_OFF_BODY_CUR    = 0xE44;  // +3652 -- current stored index (u16)
static constexpr ptrdiff_t ENT_OFF_STUDIO_DATA = 0x1000; // +4096 -- per-entity studio model data*

static ConVar sdk_body_skin_guard("sdk_body_skin_guard", "1", FCVAR_RELEASE,
	"Guard C_BaseAnimating skin/body change against OOB"
	"bodypart-table indices. Clamps bad indices to 0 and SEH-wraps the "
	"original so map load cannot hard-kill on corrupt studio body data. "
	"0 = pass-through.");

//-----------------------------------------------------------------------------
// studiohdr packed-name decode (same as staticpropmgr / GetStaticPropModelName)
// nameOffset u16 @ studiohdr+8; string at base + ((off & ~1) << (4*(off&1))).
// SEH-isolated: no C++ objects share a __try frame.
//-----------------------------------------------------------------------------
static bool TryDecodeStudioHdrName(const void* const hdr, char* const out, const size_t outSize)
{
	if (!hdr || !out || outSize == 0)
		return false;

	__try
	{
		const uintptr_t base = reinterpret_cast<uintptr_t>(hdr);
		const unsigned int off = *reinterpret_cast<const unsigned short*>(base + 8);
		if ((off & 0xFFFF) == 0)
		{
			out[0] = '\0';
			return false;
		}
		const char* const s = reinterpret_cast<const char*>(
			base + ((off & 0xFFFFFFFE) << (4 * (off & 1))));
		size_t i = 0;
		for (; i + 1 < outSize && s[i]; ++i)
			out[i] = s[i];
		out[i] = '\0';
		return i > 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		out[0] = '\0';
		return false;
	}
}

// Prefer studiohdr at studio+0x08 (base); fall back to overlay studiohdr at +0x10.
static void TryGetEntityModelName(const __int64 entity, char* const out, const size_t outSize)
{
	if (!out || outSize == 0)
		return;
	out[0] = '\0';
	if (!entity)
		return;

	__try
	{
		const __int64 studio = *reinterpret_cast<const __int64*>(entity + ENT_OFF_STUDIO_DATA);
		if (!studio)
			return;

		const void* baseHdr = *reinterpret_cast<void* const*>(studio + 0x08);
		if (baseHdr && TryDecodeStudioHdrName(baseHdr, out, outSize))
			return;

		const void* overlay = *reinterpret_cast<void* const*>(studio + 0x10);
		if (overlay)
			TryDecodeStudioHdrName(overlay, out, outSize);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		out[0] = '\0';
	}
}

//-----------------------------------------------------------------------------
// Read bodypart count the same way does. SEH-guarded: a bad
// studio pointer must not take us down before the real call.
//-----------------------------------------------------------------------------
static bool TryGetBodypartCount(const __int64 entity, unsigned __int16& outCount)
{
	outCount = 0;
	if (!entity)
		return false;

	__try
	{
		const __int64 studio = *reinterpret_cast<const __int64*>(entity + ENT_OFF_STUDIO_DATA);
		if (!studio)
			return false;

		const __int64 overlay = *reinterpret_cast<__int64*>(studio + 0x10);
		if (overlay)
		{
			outCount = *reinterpret_cast<const unsigned __int16*>(overlay + 2);
			return true;
		}

		const __int64 baseHdr = *reinterpret_cast<__int64*>(studio + 0x08);
		if (!baseHdr)
			return false;

		outCount = *reinterpret_cast<const unsigned __int16*>(baseHdr + 122);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static void ClampBodyIndex(unsigned __int16& idx, const unsigned __int16 count,
	const char* which, __int64 entity, const char* modelName, unsigned __int16& logBudget)
{
	// 0xFFFF is the engine's "no previous body" sentinel -- leave it alone.
	if (idx == 0xFFFF)
		return;
	if (count == 0)
		return;
	if (idx < count)
		return;

	const unsigned __int16 old = idx;
	idx = 0;

	if (logBudget > 0)
	{
		--logBudget;
		const char* const nm = (modelName && modelName[0]) ? modelName : "<unknown>";
		Warning(eDLL_T::ENGINE,
			"[BODY-SKIN-GUARD] clamped %s body/skin index %u -> 0 "
			"(bodyparts=%u entity=%p model='%s')\n",
			which, (unsigned)old, (unsigned)count, (void*)entity, nm);
	}
}

static double __fastcall CallOriginalSEH(__int64 entity, unsigned __int16 newIdx,
	const char* modelName)
{
	__try
	{
		return v_AnimBodySkinChange(entity, newIdx);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		static volatile LONG s_avLog = 0;
		const LONG n = InterlockedIncrement(&s_avLog);
		if (n <= 32)
		{
			const char* const nm = (modelName && modelName[0]) ? modelName : "<unknown>";
			Warning(eDLL_T::ENGINE,
				"[BODY-SKIN-GUARD] #%ld SEH caught AV in"
				"(entity=%p newIdx=%u model='%s') -- suppressed hard kill.\n",
				n, (void*)entity, (unsigned)newIdx, nm);
		}
		return 0.0;
	}
}

static double __fastcall Hook_AnimBodySkinChange(__int64 entity, unsigned __int16 newIdx)
{
	if (!sdk_body_skin_guard.GetBool() || !entity)
		return v_AnimBodySkinChange(entity, newIdx);

	char modelName[260];
	modelName[0] = '\0';
	TryGetEntityModelName(entity, modelName, sizeof(modelName));

	unsigned __int16 bodypartCount = 0;
	if (TryGetBodypartCount(entity, bodypartCount) && bodypartCount > 0)
	{
		// Rate-limit clamp spam (hot on large maps / SPAWN).
		static unsigned __int16 s_logBudget = 64;

		ClampBodyIndex(newIdx, bodypartCount, "arg", entity, modelName, s_logBudget);

		__try
		{
			unsigned __int16& prev =
				*reinterpret_cast<unsigned __int16*>(entity + ENT_OFF_BODY_PREV);
			unsigned __int16& cur =
				*reinterpret_cast<unsigned __int16*>(entity + ENT_OFF_BODY_CUR);
			ClampBodyIndex(prev, bodypartCount, "prev", entity, modelName, s_logBudget);
			ClampBodyIndex(cur, bodypartCount, "cur", entity, modelName, s_logBudget);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			// Entity unreadable -- still SEH-wrap the original below.
		}
	}

	return CallOriginalSEH(entity, newIdx, modelName);
}

void VBodySkinGuardS21::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 "
		"48 81 EC ?? ?? ?? ?? 0F B7 DA")
		.GetPtr(v_AnimBodySkinChange);

	if (!v_AnimBodySkinChange)
		Warning(eDLL_T::ENGINE,
			"[BODY-SKIN-GUARD] pattern UNRESOLVED -- guard NOT installed\n");
}

void VBodySkinGuardS21::Detour(const bool bAttach) const
{
	if (v_AnimBodySkinChange)
		DetourSetup(&v_AnimBodySkinChange, &Hook_AnimBodySkinChange, bAttach);
}
