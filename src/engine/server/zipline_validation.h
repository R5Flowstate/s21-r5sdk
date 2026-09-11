//=============================================================================//
//
// Purpose: Dedicated-server zipline validation shim.
//
//=============================================================================//
#ifndef ZIPLINE_VALIDATION_DEDI_H
#define ZIPLINE_VALIDATION_DEDI_H

#include "thirdparty/detours/include/idetour.h"
#include <cstdint>

// True when 'ent' is a fully-applied S21 ziprail chain start.
bool ZiprailDedi_HasPath(uintptr_t ent);

// True only for an entity that was walked as a chain start this map. Separates a
// rail with a broken wire block from the class baseline, which has neither.
bool ZiprailDedi_IsKnownChainStart(uintptr_t ent);

// Per-map state reset: clears the chain registry + the KeyValue-observer meta
// table so the next map's chain walk doesn't see stale entity pointers.
void ZiprailDedi_LevelShutdown();

// Materialize recorded ziprail chain starts after map spawn (SV_ActivateServer).
// No-op while sdk_ziprail_enable is 0; only the null-endpoint validator stays attached.
void ZiprailDedi_MaterializeAllPending(const char* reason);

// Native-shape DT_Ziprail path block. SendProp proxies read this instead of
// entity memory. Computed once per chain at materialize.
static constexpr int kZiprailWireMaxNodes = 32; // ZIPRAIL_PATH_NODES_MAX

struct ZiprailWireBlock
{
	int   numNodes;                                // m_numZiprailPathNodes
	int   numSmooth[kZiprailWireMaxNodes];         // m_numSmoothPointsForPathNodes (-1 = client auto-count)
	int   tangentTypes[kZiprailWireMaxNodes];      // m_tangentTypesForPathNodes (0 central / 1 back / 2 fwd)
	float positions[kZiprailWireMaxNodes][3];      // m_positionsForPathNodes (world, train nodes when the walk has >=2)
	float smoothDistToNode[kZiprailWireMaxNodes];  // m_smoothDistanceToNode (cumulative Hermite arc length; [0]=0)
	float pathLen;                                 // m_ziprailPathLen (total smooth length)
	float extentsMins[3];                          // m_pathExtentsMins (AABB over sampled curve + nodes, unpadded)
	float extentsMaxs[3];                          // m_pathExtentsMaxs
	int   useAutoDetachSpeed;                      // m_ziprailUseAutoDetachSpeed (0/1; KV 300 still sends 1)
	int   preventManualDetach;                     // m_ziplinePreventManualDetach
	float autoDetachDistance;                      // m_ziplineAutoDetachDistance
	float speedScale;                              // m_ziplineSpeedScale
	float ropeColorModulation[3];                  // m_ropeColorModulation (1,1,1 -- S21-only prop, zero-stomped otherwise)
	float mountReverseDistance;                    // m_ziplineMountReverseDistance (near path[0] forces forward)
};

// Wire block for a promoted chain-start entity. nullptr when 'ent' has no
// materialized chain (baseline temp entity, plain zipline, foreign class) --
// the caller substitutes an all-zero block so the class baseline packs zeros.
const ZiprailWireBlock* ZiprailDedi_GetWireBlock(uintptr_t ent);

// Path direction at arc distance 'dist' along the rail: normalized lerp of the
// two bracketing baked Hermite derivatives. False when 'ent' has no baked path.
bool ZiprailDedi_GetPathDirectionAtArcDistance(uintptr_t ent, float dist, float outDir[3]);

// Path position at arc distance 'dist' along the rail: lerp of the two
// bracketing baked points. False when 'ent' has no baked path.
bool ZiprailDedi_GetPathPointAtArcDistance(uintptr_t ent, float dist, float outPos[3]);

// [ZIPRAIL-WHYNOT] reports the nearest materialized rail, its distance to the
// player, and every gate the engine's candidate collector tests on it. A
// distance above the 120-unit search radius is the whole answer.
void ZiprailDedi_ReportNearestRail(uintptr_t player, const char* reason);

// Nearest materialized rail whose baked path passes within flMaxDist of 'pos',
// or 0. outDist receives the distance regardless of the range test (-1 when no
// rail is materialized at all).
uintptr_t ZiprailDedi_FindNearestRail(const float pos[3], float flMaxDist, float* outDist);

// Closest point on the baked path to 'pos', plus the path direction there.
// False when 'ent' has no baked path.
bool ZiprailDedi_ClosestPointOnPath(uintptr_t ent, const float pos[3],
	float outClosest[3], float outDir[3], float* outArcDist);

class VZiplineValidationDedi : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // ZIPLINE_VALIDATION_DEDI_H
