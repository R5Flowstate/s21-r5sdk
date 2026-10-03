//=============================================================================//
//
// Purpose: Portal placement result codes and tuning constants, shared by the
// server placement path and (later) client prediction.
//
//=============================================================================//
#ifndef PORTAL_PLACEMENT_SHARED_H
#define PORTAL_PLACEMENT_SHARED_H

// Script const table ePortalPlaceResult. Values are fixed by the contract.
enum class ePortalPlaceResult_t : int
{
	PORTAL_PLACE_SUCCESS = 0,
	PORTAL_PLACE_BUMPED = 1,
	PORTAL_PLACE_CANT_FIT = 2,
	PORTAL_PLACE_OVERLAP_LINKED = 3,
	PORTAL_PLACE_NEAR = 4,
	PORTAL_PLACE_INVALID_VOLUME = 5,
	PORTAL_PLACE_INVALID_SURFACE = 6,
	PORTAL_PLACE_PASSTHROUGH_SURFACE = 7,
	PORTAL_PLACE_RATE_LIMITED = 8
};

constexpr float kPortalHalfWidthDefault = 36.0f;
constexpr float kPortalHalfHeightDefault = 64.0f;
constexpr float kPortalHalfDepth = 2.0f;
constexpr float kPortalBumpForgiveness = 2.0f;
constexpr float kPortalFireDelay = 0.5f;
constexpr float kPortalTraceLength = 4096.0f;
constexpr int kPortalMaxActive = 64;

#endif // PORTAL_PLACEMENT_SHARED_H
