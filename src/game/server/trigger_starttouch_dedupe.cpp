//=============================================================================//
//
// Purpose: CBaseTrigger::StartTouch new-toucher guard. See
// trigger_starttouch_dedupe.h for the mechanism.
//
//=============================================================================//
#include "core/stdafx.h"


#include "trigger_starttouch_dedupe.h"
#include "trigger_updraft.h"

//-----------------------------------------------------------------------------
// Raw layout -- read straight off CBaseTrigger::StartTouch / EndTouch
// (r5apex_ds RVA 0xE27610 / 0xE27840), which address the same three slots.
//-----------------------------------------------------------------------------
// CUtlVector<EHANDLE> m_hTouchingEntities: base ptr, alloc count, element count.
// StartTouch's own lookup reads the ptr at +3208 and the count at +3208+24.
static constexpr ptrdiff_t TRIG_OFF_TOUCH_ELEMS = 3208;
static constexpr ptrdiff_t TRIG_OFF_TOUCH_COUNT = 3232;

// CBaseEntity::m_RefEHandle -- the dword StartTouch/EndTouch feed to the lookup.
static constexpr ptrdiff_t ENT_OFF_REFEHANDLE   = 0x08;

// The list is a per-trigger occupancy set; anything past this is corrupt state,
// not a crowded trigger. Bounds the scan instead of trusting the count.
static constexpr int kTouchListSanityMax = 4096;

//-----------------------------------------------------------------------------
// Engine function pointers.
//-----------------------------------------------------------------------------
// CBaseTrigger::StartTouch(CBaseEntity* pOther)
static void(*v_CBaseTrigger__StartTouch)(void* self, void* pOther) = nullptr;
// CBaseTrigger::EndTouch(CBaseEntity* pOther)
static void(*v_CBaseTrigger__EndTouch)(void* self, void* pOther) = nullptr;

//-----------------------------------------------------------------------------
// ConVars.
//-----------------------------------------------------------------------------
static ConVar bridge_trigger_starttouch_dedupe(
	"bridge_trigger_starttouch_dedupe", "1", FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Skip CBaseTrigger::StartTouch for an entity already in m_hTouchingEntities "
	"(restores the guard EndTouch already has). Stops duplicate OnStartTouch "
	"outputs and duplicate script enter callbacks. 0 = stock S3 behaviour.");

static ConVar bridge_trigger_starttouch_diag(
	"bridge_trigger_starttouch_diag", "0", FCVAR_DEVELOPMENTONLY | FCVAR_GAMEDLL,
	"[TRIG-DEDUP] log suppressed StartTouch re-entries. 1 = periodic tally, "
	"2 = also one line per suppression (rate-limited). Default 0.");

// Sampling stride for level-1 tallies. These fire thousands of times;
// an unsampled log would stall.
static constexpr LONG kTallyInterval = 256;
static constexpr LONG kVerboseMax    = 64;

static volatile LONG s_nSuppressed = 0;
static volatile LONG s_nVerbose    = 0;
static volatile LONG s_bAnnounced  = 0;

//-----------------------------------------------------------------------------
// Purpose: is pOther already listed as touching pTrigger?
// Mirrors the engine's own lookup: it resolves both sides through the entity
// list and compares pointers, which for a live pOther is exactly equality on
// the EHANDLE dword (index + serial). Raw compare avoids resolving a second
// engine symbol and cannot false-positive on two dead handles.
//-----------------------------------------------------------------------------
static bool TriggerDedupe_IsAlreadyTouching(const void* const pTrigger, const void* const pOther)
{
	// Live entity/trigger from StartTouch -- null-checked by caller; plain
	// reads with a hard count cap (hygiene D2). Torn list fails closed to false.
	if (!pTrigger || !pOther)
		return false;

	const int nHandle = *reinterpret_cast<const int*>(
		reinterpret_cast<const uint8_t*>(pOther) + ENT_OFF_REFEHANDLE);

	if (nHandle == -1)
		return false;

	const uint32_t* const pElems = *reinterpret_cast<const uint32_t* const*>(
		reinterpret_cast<const uint8_t*>(pTrigger) + TRIG_OFF_TOUCH_ELEMS);
	const int nCount = *reinterpret_cast<const int*>(
		reinterpret_cast<const uint8_t*>(pTrigger) + TRIG_OFF_TOUCH_COUNT);

	if (!pElems || nCount <= 0 || nCount > kTouchListSanityMax)
		return false;

	for (int i = 0; i < nCount; i++)
	{
		if (static_cast<int>(pElems[i]) == nHandle)
			return true;
	}

	return false;
}

//-----------------------------------------------------------------------------
// Purpose: StartTouch -- suppress re-entry for an entity already touching.
// With the engine's new-toucher flag false, the original's only remaining work
// is the duplicate OnStartTouch output and the duplicate m_enterCallback; the
// first-toucher think is already gated on that flag, so nothing else is lost.
//-----------------------------------------------------------------------------
static void Hook_CBaseTrigger_StartTouch(void* self, void* pOther)
{
	if (bridge_trigger_starttouch_dedupe.GetBool() && self && pOther &&
		TriggerDedupe_IsAlreadyTouching(self, pOther))
	{
		const LONG n = InterlockedIncrement(&s_nSuppressed);

		if (InterlockedCompareExchange(&s_bAnnounced, 1, 0) == 0)
			Msg(eDLL_T::SERVER,
				"[TRIG-DEDUP] active -- suppressing duplicate StartTouch "
				"(OnStartTouch output + script enter callback) for entities "
				"already in m_hTouchingEntities.\n");

		const int nDiag = bridge_trigger_starttouch_diag.GetInt();

		if (nDiag >= 2 && InterlockedIncrement(&s_nVerbose) <= kVerboseMax)
			DevMsg(eDLL_T::SERVER, "[TRIG-DEDUP] re-entry #%ld trigger=%p other=%p\n",
				n, self, pOther);
		else if (nDiag >= 1 && (n % kTallyInterval) == 0)
			DevMsg(eDLL_T::SERVER, "[TRIG-DEDUP] %ld duplicate StartTouch calls suppressed\n", n);

		return;
	}

	v_CBaseTrigger__StartTouch(self, pOther);

	// Updraft enter bookkeeping + CodeCallback after the engine has accepted
	// the new touch. Suppressed re-entries never reach here.
	UpdraftBridge_OnTriggerStartTouch(self, pOther);
}

//-----------------------------------------------------------------------------
// Purpose: EndTouch -- pass through, then updraft leave bookkeeping.
// Leave fires unconditionally for trigger_updraft (even if the handle was
// not in the per-player array); that asymmetry matches the S21 client.
//-----------------------------------------------------------------------------
static void Hook_CBaseTrigger_EndTouch(void* self, void* pOther)
{
	if (v_CBaseTrigger__EndTouch)
		v_CBaseTrigger__EndTouch(self, pOther);

	UpdraftBridge_OnTriggerEndTouch(self, pOther);
}

void VTriggerStartTouchDedupe::GetAdr(void) const
{
	LogFunAdr("CBaseTrigger::StartTouch", v_CBaseTrigger__StartTouch);
	LogFunAdr("CBaseTrigger::EndTouch", v_CBaseTrigger__EndTouch);
}

void VTriggerStartTouchDedupe::GetFun(void) const
{
	// CBaseTrigger::StartTouch (r5apex_ds RVA 0xE27610). Anchored on the
	// PassesTriggerFilters virtual call (vtbl+0x8C8) that opens the body, so the
	// signature cannot slide onto a sibling trigger override; only the jz rel32
	// to the early-out is wildcarded. Unique in both the stock and
	// the deployed dedi.
	Module_FindPattern(g_GameDll,
		"40 56 57 48 83 EC 78 48 8B 01 48 8B F2 48 8B F9 "
		"FF 90 C8 08 00 00 84 C0 0F 84 ?? ?? ?? ??")
		.GetPtr(v_CBaseTrigger__StartTouch);

	// CBaseTrigger::EndTouch. lea r12,[rcx+0C88h] is m_hTouchingEntities;
	// unique.
	Module_FindPattern(g_GameDll,
		"40 55 57 41 54 48 81 EC 80 00 00 00 48 8B EA 48 8B F9 "
		"48 85 D2 74 05 8B 42 08 EB 05 B8 FF FF FF FF "
		"4C 8D A1 88 0C 00 00")
		.GetPtr(v_CBaseTrigger__EndTouch);

	if (!v_CBaseTrigger__StartTouch)
		Warning(eDLL_T::SERVER,
			"[TRIG-DEDUP] CBaseTrigger::StartTouch pattern unresolved -- duplicate "
			"enter callbacks stay live (script enter-callbacks will re-fire every frame per occupant)\n");
	if (!v_CBaseTrigger__EndTouch)
		Warning(eDLL_T::SERVER,
			"[TRIG-DEDUP] CBaseTrigger::EndTouch pattern unresolved -- "
			"updraft leave callbacks will not fire\n");
}

void VTriggerStartTouchDedupe::Detour(const bool bAttach) const
{
	if (v_CBaseTrigger__StartTouch)
		DetourSetup(&v_CBaseTrigger__StartTouch, &Hook_CBaseTrigger_StartTouch, bAttach);
	if (v_CBaseTrigger__EndTouch)
		DetourSetup(&v_CBaseTrigger__EndTouch, &Hook_CBaseTrigger_EndTouch, bAttach);
}

