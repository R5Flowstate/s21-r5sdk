//=============================================================================//
//
// Purpose
//
// $NoKeywords: $
//=============================================================================//
#include "core/stdafx.h"
#include "public/studio.h"
#include "game/shared/animation.h"
#include "tier1/cvar.h"

static ConVar sdk_log_seq_lookup("sdk_log_seq_lookup", "0",
	FCVAR_RELEASE,
	"Log every server-side CBaseAnimating::LookupSequence: model, requested "
	"name, resolved index and label, virtual model presence, inline seq count.");

static ConVar sdk_log_seq_lookup_filter("sdk_log_seq_lookup_filter", "door",
	FCVAR_RELEASE,
	"Substring the model name must contain for [SEQ-LOOKUP] to log. Empty "
	"logs every lookup, which is very heavy.");

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
int CStudioHdr::LookupSequence(CStudioHdr* pStudio, const char* pszName)
{
	if (!pStudio->m_pMDLCache)
		return -1; // animations are unavailable for missing dynamic props! (mdl/error.rmdl).

	return CStudioHdr__LookupSequence(pStudio, pszName);
}

// Server entity: CStudioHdr* at +0xFD8. virtualmodel_t: seq wrapper array at
// +0x08 (stride 24, seqdesc* at +8), count at +0x20. seqdesc label index at +4.
static const char* SeqLabel(const CStudioHdr* pStudio, const int nSeq)
{
	if (!pStudio || nSeq < 0)
		return "-";

	const uintptr_t base = reinterpret_cast<uintptr_t>(pStudio);
	const studiohdr_t* const pHdr = *reinterpret_cast<studiohdr_t* const*>(base + 0x08);
	const uintptr_t vm = *reinterpret_cast<const uintptr_t*>(base + 0x10);
	uintptr_t seqdesc = 0;

	if (vm)
	{
		const int nCount = *reinterpret_cast<const int*>(vm + 0x20);
		const uintptr_t arr = *reinterpret_cast<const uintptr_t*>(vm + 0x08);
		if (nSeq >= nCount || !arr)
			return "?vm";
		seqdesc = *reinterpret_cast<const uintptr_t*>(arr + 24 * nSeq + 8);
	}
	else if (pHdr)
	{
		if (nSeq >= pHdr->numlocalseq)
			return "?local";
		seqdesc = reinterpret_cast<uintptr_t>(pHdr) + pHdr->localseqindex + 208 * nSeq;
	}

	if (!seqdesc)
		return "(null seqdesc)";

	return reinterpret_cast<const char*>(seqdesc + *reinterpret_cast<const int*>(seqdesc + 4));
}

int Hook_CBaseAnimating_LookupSequence(void* pEntity, const char* pszName)
{
	const int nResult = CBaseAnimating__LookupSequence(pEntity, pszName);

	if (!sdk_log_seq_lookup.GetBool() || !pEntity)
		return nResult;

	const CStudioHdr* const pStudio = *reinterpret_cast<CStudioHdr* const*>(
		reinterpret_cast<uintptr_t>(pEntity) + 0xFD8);
	const studiohdr_t* const pHdr = pStudio ? pStudio->GetRenderHdr() : nullptr;
	const char* const pszModel = pHdr ? pHdr->name : "?";
	const char* const pszFilter = sdk_log_seq_lookup_filter.GetString();

	if (pszFilter && pszFilter[0] && !V_strstr(pszModel, pszFilter))
		return nResult;

	const uintptr_t vm = pStudio
		? *reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(pStudio) + 0x10) : 0;

	Warning(eDLL_T::SERVER,
		"[SEQ-LOOKUP] ent=%p model='%s' name='%s' -> %d label='%s' virtual=%d vmseqs=%d numlocalseq=%d\n",
		pEntity, pszModel, pszName ? pszName : "(null)", nResult, SeqLabel(pStudio, nResult),
		vm ? 1 : 0, vm ? *reinterpret_cast<const int*>(vm + 0x20) : -1,
		pHdr ? pHdr->numlocalseq : -1);

	return nResult;
}

void VAnimation::Detour(const bool bAttach) const
{
	DetourSetup(&CStudioHdr__LookupSequence, &CStudioHdr::LookupSequence, bAttach);
	if (CBaseAnimating__LookupSequence)
		DetourSetup(&CBaseAnimating__LookupSequence, &Hook_CBaseAnimating_LookupSequence, bAttach);
}
