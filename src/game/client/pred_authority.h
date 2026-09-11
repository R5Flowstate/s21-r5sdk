//=============================================================================//
//
// Purpose: Per-field prediction-authority contract for the S3->S21 bridge.
// One table entry per networked+predicted field this layer touches.
//
//=============================================================================//
#ifndef CLIENT_PRED_AUTHORITY_H
#define CLIENT_PRED_AUTHORITY_H

#include "tier1/cvar.h"
#include "game/client/pred_authority_probes.h"

//-----------------------------------------------------------------------------
// S21 prediction-copy struct offsets (ErrorCheckFlatFields / PostNetworkDataReceived layout).
//-----------------------------------------------------------------------------
#define ENT_PREDICTED_FLAG     0x7F0  // C_BaseEntity::m_savedPredictionStates.predicted (bool)
#define ENT_SAVED_STATES       0x7F8  // C_BaseEntity::m_savedPredictionStates.states (ptr)
#define ENT_ENTINDEX           0x38   // C_BaseEntity::m_entIndex
#define STATE_SERIALIZED_DATA  0x08   // PredictedEntityState::serializedData
#define STATES_ORIGINALDATA    0x2EE0 // PredictedEntityStates::originalData (server baseline)

#define VIDX_GetPredDescMap    14     // C_BaseEntity vtable slot (+0x70)
#define DMAP_OPTIMIZED         0x38   // datamap_t::m_pOptimizedDataMap
#define OPT_INFO_STRIDE        72     // optimized_datamap_t per-type datamapinfo_t stride
#define INFO_FLAT_FIELDS       0      // flattenedoffsets_t::m_Flattened (CUtlVector ptr)
#define INFO_FLAT_COUNT        24     // flattenedoffsets_t::m_Flattened.m_Size
#define PC_NETWORKED_ONLY      1

#define TD_STRIDE              0x80   // typedescription_Client_t
#define TD_FIELDTYPE           0x00
#define TD_FIELDNAME           0x08
#define TD_FIELDSIZE           0x14   // array element count (u16)
#define TD_FLAGS               0x18
#define TD_TOLERANCE           0x6C
#define TD_FLATOFFSET1         0x74   // flatOffset[1] (errorcheck dest/src index = 1)
#define TD_FLATOFFSET0         0x70   // flatOffset[0] (live-member offset, accumulated by flattening; unpacked side of CPredictionCopy)

#define FTYPEDESC_SKIP         0x400  // field excluded from this copy
#define FTYPEDESC_GRAPPLE      0x4000 // only valid while grappling (skip to avoid noise)

//-----------------------------------------------------------------------------
// C_EntInfo slot. S21 stride 0x20 (shl rdx,5).
//-----------------------------------------------------------------------------
#define ENTINFO_STRIDE         0x20
#define ENTINFO_ENTITY         0x00   // C_EntInfo::m_pEntity
#define ENTINFO_SERIAL         0x08   // C_EntInfo::m_SerialNumber (compared as a dword)

// Native compare: skip 0x400; skip 0x4000 (grapple, skipped here); FIELD_TIME +/- half tick; EHANDLE via pointers.
enum PredNativeCmp_t
{
	PRED_CMP_SAME = 0,   // the engine sees no difference at all
	PRED_CMP_BELOW_TOL,  // differs, but inside the tolerance the engine honours
	PRED_CMP_ERROR,      // the engine counts this as a prediction error
};

// gpGlobals_Client->interval_per_tick, read from the very global the native
// compare reads. 0 when unresolved (callers then fall back to fieldTolerance).
float PredNative_TickInterval(void);

// gpGlobals_Client->curtime -- the clock CategorizePosition holds the knockback
// windows against. 0 when unresolved.
float PredNative_CurTime(void);
float PredNative_LatestPredictedTime(void);

// [TIME-DOMAIN] clientTb-serverTb used for the current command's convert.
float TimeDomain_LastDelta(void);

// Effective tolerance the native compare applies to this field type.
float PredNative_Tolerance(int nType, float flFieldTol);

// EHANDLE: low 16 = entry, high 16 = serial. Null when unresolved; two dead handles compare equal.
const void* PredNative_ResolveEHandle(unsigned int nHandle);
void* PredNative_EntityFromSlot(int slot);
bool PredNative_HasEntInfoArray(void);

// One field, compared exactly as the engine compares it.
PredNativeCmp_t PredNative_Compare(int nType, const uint8_t* pPred, const uint8_t* pSrv,
	int nCount, float flFieldTol);

template <typename T> static inline T EntField(void* pBase, int nOffset)
{
	return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(pBase) + nOffset);
}

//-----------------------------------------------------------------------------
// Per-entry class: mask/repair, compare-only, tolerance, clock transplant, skip, force-check.
//-----------------------------------------------------------------------------
enum class PredAuthClass_t
{
	SERVER_CONTENT,
	CLIENT_TIMING,
	CLIENT_TIMING_COMPARE_ONLY,
	TOLERANCE,
	DOMAIN_TRANSPLANT_ANCHOR,
	DOMAIN_TRANSPLANT,
	SIM_DIVERGENT,
	SATURATING,
	NO_ERRORCHECK,
	FORCE_ERRORCHECK,
};

//-----------------------------------------------------------------------------
// Scope: marker fields timeBase => PLAYER, m_ammoInClip => WEAPON, m_viewModelOwner => VIEWMODEL.
//-----------------------------------------------------------------------------
enum PredAuthScope_t
{
	AUTH_SCOPE_ANY = 0,
	AUTH_SCOPE_PLAYER,
	AUTH_SCOPE_WEAPON,
	AUTH_SCOPE_VIEWMODEL,
};

struct PredAuthEntry_t
{
	const char*      pszName;
	PredAuthClass_t  eClass;
	int              nScope;
	ConVar*          pTolCvar;
	ConVar*          pGateCvar;
	uint8_t          nByteMask;
	const char*      pszWhy;
};

extern ConVar sdk_pred_unfed_mask;
extern ConVar sdk_pred_baseline_repair;
extern ConVar sdk_pred_clock_rebase;
extern ConVar sdk_pred_stamp_reset_adopt;
extern ConVar sdk_pred_teleport_adopt;
extern ConVar sdk_pred_teleport_dist;
extern ConVar sdk_pred_forced0_neut;
extern ConVar sdk_pred_tol_origin;
extern ConVar sdk_pred_tol_velocity;
extern ConVar sdk_pred_tol_fallvelocity;
extern ConVar sdk_pred_saturate_mask;
extern ConVar sdk_pred_fallvel_cap;
extern ConVar sdk_pred_tol_time;
extern ConVar sdk_pred_tol_groundnormal;
extern ConVar sdk_pred_ent_census;
extern ConVar bridge_kick_row_tap;
extern ConVar bridge_time_domain_convert;

//-----------------------------------------------------------------------------
// Cache lookup only; PredAuth_Apply populates it earlier in the same PNR hook.
//-----------------------------------------------------------------------------
int PredAuth_GetDmapKind(void* pEntity);

// Flat typedescription offsets for one field name (flatOffset[0]/[1]).
bool PredAuth_ResolveFlatOff(uintptr_t dmap, const char* want, int* pOff0, int* pOff1);

//-----------------------------------------------------------------------------
// Copy wire-fed m_shotCount into m_shotIndexForSpread (S3 networks the pre-split counter).
//-----------------------------------------------------------------------------
void PredAuth_ShotIndexFanout(void* pEntity);

//-----------------------------------------------------------------------------
// S21 SaveData(this, name, cmd, slot); slot -1 = originalData.
//-----------------------------------------------------------------------------
typedef void(__fastcall* PFN_PredSaveData)(void*, const char*, int, int);
PFN_PredSaveData PredAuth_SaveData(void);

//-----------------------------------------------------------------------------
// S3 m_nDuckTransitionTimeMsecs and S21 m_duckTransitionRemainderMsec sit in different tables.
//-----------------------------------------------------------------------------
void PredAuth_OnDuckRemainderWire(int nEntIndex, int nMsec);
void PredAuth_ResetSession(void);

void PredAuth_Apply(void* pEntity, unsigned int nCmd);

// Latch nLatest/nAckCount from the PNR hook so kick residual class logs can
// join the per-snapshot ack count without widening the Apply signature.
// nAckCount is commands_acknowledged (1..N per snapshot), not an absolute cmd.
void PredAuth_NoteAckCount(int nLatest, int nAckCount);
int PredAuth_PnrAckCount(void);
int PredAuth_PnrLatest(void);

//-----------------------------------------------------------------------------
// [FORCED0-NEUT] per-dispatch entity-error tally.
//-----------------------------------------------------------------------------
void PredAuth_DispatchBegin(void);
void PredAuth_NoteEntityError(void);
bool PredAuth_DispatchActive(void);

struct PredAuthDispatchResult_s
{
	int errAfter;
	int entErrs;
	int neutered;
};

// Reads +212 (m_Split[0].m_bPreviousAckHadErrors). Neutered when
// sdk_pred_forced0_neut && a3==0 && a4 && errAfter!=0 && entErrs==0.
PredAuthDispatchResult_s PredAuth_DispatchEnd(__int64 a1, unsigned int a2, int a3, char a4);

int PredAuth_BoardAlarm(unsigned int nZoom, unsigned int nPunch, unsigned int nAnimevt);

#endif // CLIENT_PRED_AUTHORITY_H
