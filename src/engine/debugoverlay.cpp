//============================================================================//
//
// Purpose: Debug interface functions
//
//============================================================================//

#include "core/stdafx.h"
#include <cstring>
#include "common/pseudodefs.h"
#include "tier0/memstd.h"
#include "tier0/basetypes.h"
#include "tier0/commandline.h"
#include "tier1/cvar.h"
#ifndef CLIENT_DLL
#include "common/netmessages.h"
#endif // !CLIENT_DLL
#include "tier2/renderutils.h"
#include "mathlib/mathlib.h"
#ifndef DEDICATED
#include "engine/client/clientstate.h"
#endif // !DEDICATED
#include "engine/host_cmd.h"
#include "engine/cmodel.h"
#include "engine/debugoverlay.h"
#ifndef DEDICATED
#include "materialsystem/cmaterialsystem.h"
#endif // !DEDICATED
#ifndef CLIENT_DLL
#include "engine/server/server.h"
#include "game/server/entitylist.h"
#include "game/server/baseentity.h"
#endif // !CLIENT_DLL
#ifndef DEDICATED
#include "game/client/c_baseentity.h"
#include "game/client/cliententitylist.h"
#include "engine/cmodel_bsp_debug.h"
#endif // !DEDICATED

ConVar enable_debug_text_overlays("enable_debug_text_overlays", "1", FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT | FCVAR_GAMEDLL, "Enable rendering of debug text overlays");
static ConVar debug_overlay_nodecay("debug_overlay_nodecay", "0", FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT, "Keeps all debug overlays alive regardless of their lifetime. Use command 'clear_debug_overlays' to clear everything");

#ifndef CLIENT_DLL
// CHEAT rather than DEVELOPMENTONLY: this is the master gate for the whole
// replicate, and a dedi without dev cvars hides DEVELOPMENTONLY names from the
// command dispatch, leaving no way to toggle it at runtime.
static ConVar bridge_debug_overlays("bridge_debug_overlays", "1",
    FCVAR_CHEAT | FCVAR_GAMEDLL,
    "Replicate server debug overlays to connected clients.");
#endif // !CLIENT_DLL

bool DebugOverlay_DevModeEnabled()
{
    if (!CommandLine())
        return false;
    return CommandLine()->CheckParm("-devsdk")
        || CommandLine()->CheckParm("-dev")
        || CommandLine()->CheckParm("-developer");
}

static void DebugOverlay_ApplyDevDefaults()
{
    static bool s_done = false;
    if (s_done || !DebugOverlay_DevModeEnabled())
        return;

#if defined(CLIENT_DLL)
    if (enable_debug_overlays)
    {
        const uintptr_t cvAddr = reinterpret_cast<uintptr_t>(enable_debug_overlays);
        *reinterpret_cast<float*>(cvAddr + 0x60) = 1.0f;
        *reinterpret_cast<int*>(cvAddr + 0x64) = 1;
    }
    enable_debug_text_overlays.SetValue(1);
    s_done = true;
#else
    if (enable_debug_overlays)
        enable_debug_overlays->SetValue(1);
    bridge_debug_overlays.SetValue(1);
    s_done = true;
#endif
}

#ifndef CLIENT_DLL
static void DebugOverlay_S2C_Enqueue(const uint8_t type, const Vector3D& p0, const Vector3D& p1, const Vector3D& p2,
    const int r, const int g, const int b, const int a, const bool noDepthTest, const float duration,
    const Vector3D& p3 = vec3_origin);
static void DebugOverlay_S2C_Flush();

static void (*v_CIVDebugOverlay_AddLineOverlay)(CIVDebugOverlay* const, const Vector3D&, const Vector3D&,
    const int, const int, const int, const bool, const float) = nullptr;
static void (*v_CIVDebugOverlay_AddBoxOverlay)(CIVDebugOverlay* const, const Vector3D&, const Vector3D&, const Vector3D&,
    const int, const int, const int, const int, const bool, const float) = nullptr;
static void (*v_CIVDebugOverlay_AddTransformedBoxOverlay)(CIVDebugOverlay* const, const matrix3x4_t&, const Vector3D&, const Vector3D&,
    const int, const int, const int, const int, const bool, const float) = nullptr;
static void (*v_CIVDebugOverlay_AddTriangleOverlay)(CIVDebugOverlay* const, const Vector3D&, const Vector3D&, const Vector3D&,
    const int, const int, const int, const int, const bool, const float) = nullptr;
static void (*v_CIVDebugOverlay_AddLineOverlayAlpha)(CIVDebugOverlay* const, const Vector3D&, const Vector3D&,
    const int, const int, const int, const int, const bool, const float) = nullptr;
#endif // !CLIENT_DLL

#if defined(CLIENT_DLL)
//------------------------------------------------------------------------------
// Purpose: the clock native DrawAllOverlays expires overlays against
// Output: negative when no clock resolved, so callers keep the overlay alive
//------------------------------------------------------------------------------
static float DebugOverlay_OverlayTime()
{
    if (s_pOverlayUseTickClock && *s_pOverlayUseTickClock)
    {
        if (s_pOverlayTickCount && s_pOverlayTickInterval)
            return static_cast<float>(*s_pOverlayTickCount) * (*s_pOverlayTickInterval);

        return -1.0f;
    }

    return s_pOverlayCurTime ? *s_pOverlayCurTime : -1.0f;
}
#endif // CLIENT_DLL

//------------------------------------------------------------------------------
// Purpose: returns whether the overlay can be added at this moment
//------------------------------------------------------------------------------
static bool DebugOverlay_CanApplyOverlay()
{
#ifndef DEDICATED
    // VClientState is not registered on the client product, so g_pClientState stays null.
    if (!g_pClientState || !g_pClientState->IsPaused())
        return true;
#endif // !DEDICATED

#ifndef CLIENT_DLL
    if (g_pServer->CanApplyOverlays())
        return true;
#endif // !CLIENT_DLL

    return false;
}

//-----------------------------------------------------------------------------
// Purpose: determines and sets the end time for the overlay
//-----------------------------------------------------------------------------
template <class OverlayBaseClass>
static void DebugOverlay_SetEndTime(OverlayBaseClass* const base, const float duration, const bool nonTextOverlay)
{
    if (duration == 0.0f)
    {
        // note(kawe): the server runs in its own thread, and
        // at a different pace relative to the render thread.
        // DrawAllDebugOverlays is the entry point, and the
        // only section where server debug overlays are being
        // added. This always runs in the server frame thread.
        // `g_nOverlayStage` has the correct pacing for server
        // overlays, this stage counter ensures the overlay
        // only draws for one frame, and does not render twice
        // in a frame causing the alpha to be multiplied.
        if (ThreadInServerFrameThread())
        {
            // note(kawe): this was originally 'n + 1' but this
            // makes debug text overlays to render for 2 frames
            // resulting in a trail effect when text is being
            // displayed on moving entities. Text is rendered
            // at a different point during the frame than the
            // non-text overlays causing this effect, so only
            // increment if we have a non-text overlay. For
            // non-text overlays we need the increment as it
            // ensures the server overlay runs for the entirety
            // of the client frame without rendering twice.
            if (g_nOverlayStage)
                base->m_nOverlayTick = (*g_nOverlayStage) + nonTextOverlay;	// stay alive for only one frame
        }
        else
        {
            // note(kawe): for client overlays, we must set the
            // start tick to the current render tick to ensure
            // it only renders once during its lifetime.
            // g_nOverlayStage paces server overlays only; pacing
            // client overlays from it double-renders them when
            // frame times are low (visible flicker). The render
            // tick gives client overlays their correct lifetime.
            if (g_nRenderTickCount)
                base->m_nCreationTick = *g_nRenderTickCount;
        }
    }
    else if (duration == (NDEBUG_PERSIST_TILL_NEXT_CLIENT))
    {
        if (g_nRenderTickCount)
            base->m_nCreationTick = (*g_nRenderTickCount) + 1;
    }
    else if (duration == NDEBUG_PERSIST_TILL_NEXT_SERVER)
    {
        base->m_flEndTime = NDEBUG_PERSIST_TILL_NEXT_SERVER;
    }
    else
    {
#ifndef DEDICATED
        const float now = DebugOverlay_OverlayTime();
        base->m_flEndTime = (now >= 0.0f) ? (now + duration) : NDEBUG_PERSIST_TILL_NEXT_SERVER;
#else
        base->m_flEndTime = g_pServer->GetTime();
#endif
    }
}

//-----------------------------------------------------------------------------
// Purpose: Hack to allow this code to run on a client that's not connected to a server
// (i.e., demo playback, or multiplayer game )
// Input: entNum - 
// origin - 
//-----------------------------------------------------------------------------
static bool DebugOverlay_GetEntityOriginClientOrServer(const int entNum, Vector3D& origin)
{
#ifndef CLIENT_DLL
    if (g_pServer->IsActive())
    {
        const CEntInfo* const entInfo = g_serverEntityList->GetEntInfoPtrByIndex(entNum);
        const CBaseEntity* const serverEntity = (CBaseEntity*)entInfo->m_pEntity;

        if (!entInfo->m_pEntity)
            return false;

        CM_WorldSpaceCenter(serverEntity->CollisionProp(), &origin);
        return true;
    }
#endif // CLIENT_DLL

#ifndef DEDICATED
    IClientEntity* const clientEntity = g_clientEntityList->GetClientEntity(entNum);

    if (!clientEntity)
        return false;

    CM_WorldSpaceCenter(clientEntity->GetCollideable(), &origin);
#endif // DEDICATED

    return true;
}

//-----------------------------------------------------------------------------
// Purpose: add new overlay sphere
//-----------------------------------------------------------------------------
void CIVDebugOverlay::AddSphereOverlayInternal(CIVDebugOverlay* const thisptr, const Vector3D& vOrigin, const float flRadius,
    const int nTheta, const int nPhi, const int r, const int g, const int b, const int a, const bool noDepthTest, const float flDuration)
{
    if (!DebugOverlay_CanApplyOverlay())
        return;

    AUTO_LOCK(*s_OverlayMutex);
    OverlaySphere_t* const newOverlay = new OverlaySphere_t;

    if (!newOverlay)
        return;

    newOverlay->vOrigin = vOrigin;
    newOverlay->flRadius = flRadius;
    newOverlay->nTheta = nTheta;
    newOverlay->nPhi = nPhi;
    newOverlay->r = r;
    newOverlay->g = g;
    newOverlay->b = b;
    newOverlay->a = a;
    newOverlay->noDepthTest = noDepthTest;

    newOverlay->SetEndTime(flDuration);

    newOverlay->m_pNextOverlay = *s_pOverlays;
    *s_pOverlays = newOverlay;

#ifndef CLIENT_DLL
    DebugOverlay_S2C_Enqueue(kS2CSphere, vOrigin, Vector3D(flRadius, (float)nTheta, (float)nPhi), Vector3D(0.f, 0.f, 0.f),
        r, g, b, a, noDepthTest, flDuration);
#endif // !CLIENT_DLL
}

//-----------------------------------------------------------------------------
// Purpose: add new overlay swept box
//-----------------------------------------------------------------------------
void CIVDebugOverlay::AddSweptBoxInternal(CIVDebugOverlay* const thisptr, const Vector3D& start, const Vector3D& end, const Vector3D& mins,
    const Vector3D& max, const QAngle& angles, const int r, const int g, const int b, const int a, const bool noDepthTest, const float flDuration)
{
    if (!DebugOverlay_CanApplyOverlay())
        return;

    AUTO_LOCK(*s_OverlayMutex);
    OverlaySweptBox_t* const newOverlay = new OverlaySweptBox_t;

    if (!newOverlay)
        return;

    newOverlay->start = start;
    newOverlay->end = end;
    newOverlay->mins = mins;
    newOverlay->maxs = max;
    newOverlay->angles = angles;
    newOverlay->r = r;
    newOverlay->g = g;
    newOverlay->b = b;
    newOverlay->a = a;
    newOverlay->noDepthTest = noDepthTest;

    newOverlay->SetEndTime(flDuration);

    newOverlay->m_pNextOverlay = *s_pOverlays;
    *s_pOverlays = newOverlay;

#ifndef CLIENT_DLL
    // No wire id carries five vectors, so this shape does not replicate. Say so
    // rather than leaving a silent hole in the set.
    static bool s_warnedSweptBox = false;

    if (!s_warnedSweptBox)
    {
        s_warnedSweptBox = true;
        Warning(eDLL_T::SERVER, "[DBGDRAW] swept box overlays do not replicate to clients\n");
    }
#endif // !CLIENT_DLL
}

//-----------------------------------------------------------------------------
// Purpose: add new overlay capsule
//-----------------------------------------------------------------------------
void CIVDebugOverlay::AddCapsuleOverlayInternal(CIVDebugOverlay* const thisptr, const Vector3D& vStart, const Vector3D& vEnd,
    const float flRadius, const int r, const int g, const int b, const int a, const bool noDepthTest, const float flDuration)
{
    if (!DebugOverlay_CanApplyOverlay())
        return;

    AUTO_LOCK(*s_OverlayMutex);
    OverlayCapsule_t* const newOverlay = new OverlayCapsule_t;

    if (!newOverlay)
        return;

    newOverlay->start = vStart;
    newOverlay->end = vEnd;
    newOverlay->radius = flRadius;
    newOverlay->r = r;
    newOverlay->g = g;
    newOverlay->b = b;
    newOverlay->a = a;
    newOverlay->noDepthTest = noDepthTest;

    newOverlay->SetEndTime(flDuration);

    newOverlay->m_pNextOverlay = *s_pOverlays;
    *s_pOverlays = newOverlay;

#ifndef CLIENT_DLL
    DebugOverlay_S2C_Enqueue(kS2CCapsule, vStart, vEnd, Vector3D(flRadius, 0.f, 0.f), r, g, b, a, noDepthTest, flDuration);
#endif // !CLIENT_DLL
}

#ifndef CLIENT_DLL
static constexpr int kDebugOverlayS2CType = 69;

struct OverlayS2CItem_t
{
    uint8_t type;
    Vector3D p0;
    Vector3D p1;
    Vector3D p2;
    Vector3D p3;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
    uint8_t flags;
    float duration;
};

static OverlayS2CItem_t s_s2cQueue[kDebugOverlayS2CMax];
static int s_s2cCount = 0;
static int s_s2cDropped = 0;

static void DebugOverlay_S2C_Enqueue(const uint8_t type, const Vector3D& p0, const Vector3D& p1, const Vector3D& p2,
    const int r, const int g, const int b, const int a, const bool noDepthTest, const float duration,
    const Vector3D& p3)
{
    if (!bridge_debug_overlays.GetBool())
        return;
    if (s_s2cCount >= kDebugOverlayS2CMax)
    {
        s_s2cDropped++;
        return;
    }

    OverlayS2CItem_t& item = s_s2cQueue[s_s2cCount++];
    item.type = type;
    item.p0 = p0;
    item.p1 = p1;
    item.p2 = p2;
    item.p3 = p3;
    item.r = static_cast<uint8_t>(Clamp(r, 0, 255));
    item.g = static_cast<uint8_t>(Clamp(g, 0, 255));
    item.b = static_cast<uint8_t>(Clamp(b, 0, 255));
    item.a = static_cast<uint8_t>(Clamp(a, 0, 255));
    item.flags = noDepthTest ? 1 : 0;
    item.duration = duration;
}

class SVC_DebugOverlay : public CNetMessage
{
public:
    SVC_DebugOverlay()
    {
        m_nGroup = NetMessageGroup::NoReplay;
        m_bReliable = false;
        m_nCount = 0;
    }

    virtual bool ReadFromBuffer(bf_read* buffer) { return !buffer->IsOverflowed(); }
    virtual bool WriteToBuffer(bf_write* buffer);
    virtual bool Process(void) { return true; }
    virtual int GetType(void) const { return kDebugOverlayS2CType; }
    virtual const char* GetName(void) const { return "svc_DebugOverlay"; }
    virtual const char* ToString(void) const { return "svc_DebugOverlay"; }
    virtual size_t GetSize(void) const { return sizeof(SVC_DebugOverlay); }

    int m_nCount;
    OverlayS2CItem_t m_Items[kDebugOverlayS2CMax];
};

bool SVC_DebugOverlay::WriteToBuffer(bf_write* buffer)
{
    const int count = Clamp(m_nCount, 0, kDebugOverlayS2CMax);
    const int payloadBytes = 1 + count * kDebugOverlayS2CItemBytes;
    buffer->WriteShort(static_cast<int>(payloadBytes));
    buffer->WriteByte(count);
    for (int i = 0; i < count; i++)
    {
        const OverlayS2CItem_t& item = m_Items[i];
        buffer->WriteByte(item.type);
        buffer->WriteFloat(item.p0.x);
        buffer->WriteFloat(item.p0.y);
        buffer->WriteFloat(item.p0.z);
        buffer->WriteFloat(item.p1.x);
        buffer->WriteFloat(item.p1.y);
        buffer->WriteFloat(item.p1.z);
        buffer->WriteFloat(item.p2.x);
        buffer->WriteFloat(item.p2.y);
        buffer->WriteFloat(item.p2.z);
        buffer->WriteFloat(item.p3.x);
        buffer->WriteFloat(item.p3.y);
        buffer->WriteFloat(item.p3.z);
        buffer->WriteByte(item.r);
        buffer->WriteByte(item.g);
        buffer->WriteByte(item.b);
        buffer->WriteByte(item.a);
        buffer->WriteByte(item.flags);
        buffer->WriteFloat(item.duration);
    }
    return !buffer->IsOverflowed();
}

static void DebugOverlay_S2C_Flush()
{
    DebugOverlay_ApplyDevDefaults();
    if (s_s2cCount <= 0 || !g_pServer || !bridge_debug_overlays.GetBool())
    {
        s_s2cCount = 0;
        s_s2cDropped = 0;
        return;
    }

    if (s_s2cDropped > 0)
    {
        // A frame that overflows shows a truncated set, which reads as missing
        // geometry rather than as a budget problem. Say so.
        static int s_dropLogBudget = 8;
        if (s_dropLogBudget > 0)
        {
            --s_dropLogBudget;
            Warning(eDLL_T::SERVER, "[DBGDRAW] S2C overlay budget exceeded, dropped %d item(s) this frame (cap %d)\n",
                s_s2cDropped, kDebugOverlayS2CMax);
        }
        s_s2cDropped = 0;
    }

    SVC_DebugOverlay msg;
    msg.m_nCount = s_s2cCount;
    memcpy(msg.m_Items, s_s2cQueue, sizeof(OverlayS2CItem_t) * s_s2cCount);
    s_s2cCount = 0;

    static bool s_logged = false;
    if (!s_logged)
    {
        s_logged = true;
        Msg(eDLL_T::SERVER, "[DBGDRAW] replicating server overlays to clients (count=%d)\n", msg.m_nCount);
    }

    g_pServer->BroadcastMessage(&msg, true, false);
}

static void Hook_AddLineOverlay(CIVDebugOverlay* const thisptr, const Vector3D& origin, const Vector3D& dest,
    const int r, const int g, const int b, const bool noDepthTest, const float flDuration)
{
    if (v_CIVDebugOverlay_AddLineOverlay)
        v_CIVDebugOverlay_AddLineOverlay(thisptr, origin, dest, r, g, b, noDepthTest, flDuration);
    DebugOverlay_S2C_Enqueue(kS2CLine, origin, dest, vec3_origin, r, g, b, 255, noDepthTest, flDuration);
}

static void Hook_AddBoxOverlay(CIVDebugOverlay* const thisptr, const Vector3D& origin, const Vector3D& mins, const Vector3D& maxs,
    const int r, const int g, const int b, const int a, const bool noDepthTest, const float flDuration)
{
    if (v_CIVDebugOverlay_AddBoxOverlay)
        v_CIVDebugOverlay_AddBoxOverlay(thisptr, origin, mins, maxs, r, g, b, a, noDepthTest, flDuration);
    DebugOverlay_S2C_Enqueue(kS2CBox, origin, mins, maxs, r, g, b, a, noDepthTest, flDuration);
}

static void Hook_AddTriangleOverlay(CIVDebugOverlay* const thisptr, const Vector3D& p1, const Vector3D& p2, const Vector3D& p3,
    const int r, const int g, const int b, const int a, const bool noDepthTest, const float flDuration)
{
    if (v_CIVDebugOverlay_AddTriangleOverlay)
        v_CIVDebugOverlay_AddTriangleOverlay(thisptr, p1, p2, p3, r, g, b, a, noDepthTest, flDuration);
    DebugOverlay_S2C_Enqueue(kS2CTriangle, p1, p2, p3, r, g, b, a, noDepthTest, flDuration);
}

//------------------------------------------------------------------------------
// Purpose: replicate an oriented box as its twelve world-space edges
// Note: the S2C box item carries no rotation, and the client rebuilds a box
//       transform from an identity angle, so a transformed box has to be
//       resolved to lines on this side or it arrives axis-aligned.
//------------------------------------------------------------------------------
static void Hook_AddTransformedBoxOverlay(CIVDebugOverlay* const thisptr, const matrix3x4_t& transforms,
    const Vector3D& mins, const Vector3D& maxs,
    const int r, const int g, const int b, const int a, const bool noDepthTest, const float flDuration)
{
    if (v_CIVDebugOverlay_AddTransformedBoxOverlay)
        v_CIVDebugOverlay_AddTransformedBoxOverlay(thisptr, transforms, mins, maxs, r, g, b, a, noDepthTest, flDuration);

    if (!bridge_debug_overlays.GetBool())
        return;

    // Sent as origin + angles rather than twelve edges: the client expands it
    // back to lines, and one item instead of twelve is what lets a continuous
    // hitbox stream fit in the frame budget. Bone transforms are rigid, so the
    // angle round trip is lossless.
    Vector3D origin;
    QAngle angles;
    MatrixPosition(transforms, origin);
    MatrixAngles(transforms, angles);

    DebugOverlay_S2C_Enqueue(kS2CTransformedBox, origin, Vector3D(angles.x, angles.y, angles.z), mins,
        r, g, b, a, noDepthTest, flDuration, maxs);
}

static void Hook_AddLineOverlayAlpha(CIVDebugOverlay* const thisptr, const Vector3D& origin, const Vector3D& dest,
    const int r, const int g, const int b, const int a, const bool noDepthTest, const float flDuration)
{
    if (v_CIVDebugOverlay_AddLineOverlayAlpha)
        v_CIVDebugOverlay_AddLineOverlayAlpha(thisptr, origin, dest, r, g, b, a, noDepthTest, flDuration);
    DebugOverlay_S2C_Enqueue(kS2CLine, origin, dest, vec3_origin, r, g, b, a, noDepthTest, flDuration);
}
#endif // !CLIENT_DLL

//------------------------------------------------------------------------------
// Purpose: checks if overlay should be decayed
// Output: true to decay, false otherwise
//------------------------------------------------------------------------------
bool OverlayBase_t::IsDead() const
{
    if (debug_overlay_nodecay.GetBool())
    {
        // Keep rendering the overlay if no-decay is set.
        return false;
    }

    if (m_nCreationTick != -1)
        return g_nRenderTickCount && m_nCreationTick < *g_nRenderTickCount;

    if (m_nOverlayTick != -1)
        return g_nOverlayTickCount && m_nOverlayTick < *g_nOverlayTickCount;

    if (!DebugOverlay_CanApplyOverlay())
    {
        // Keep rendering the overlay if the simulation is paused.
        return false;
    }

    if (m_flEndTime == NDEBUG_PERSIST_TILL_NEXT_SERVER)
    {
        return false;
    }

#ifndef DEDICATED
    // g_pClientState is never resolved on this product. Without the engine's
    // own clock every duration overlay would be immortal, and a replicated
    // stream would grow the list without bound.
    const float now = DebugOverlay_OverlayTime();
    return (now >= 0.0f) && (m_flEndTime < now);
#else
    return m_flEndTime < g_pServer->GetTime();
#endif
}

//------------------------------------------------------------------------------
// Purpose: sets the shape overlay end time
// Input: duration
//------------------------------------------------------------------------------
void OverlayBase_t::SetEndTime(const float duration)
{
    if (g_nNewOtherOverlays)
        (*g_nNewOtherOverlays)++;
    DebugOverlay_SetEndTime(this, duration, true);
}

//------------------------------------------------------------------------------
// Purpose: sets the text overlay end time
// Input: duration
//------------------------------------------------------------------------------
void OverlayText_t::SetEndTime(const float duration)
{
    if (g_nNewTextOverlays)
        (*g_nNewTextOverlays)++;
    DebugOverlay_SetEndTime(this, duration, false);
}

//------------------------------------------------------------------------------
// Purpose: detour proxy for setting the overlay's end time
//------------------------------------------------------------------------------
static void DebugOverlay_SetEndTime(OverlayBase_t* const pOverlay, const float flDuration)
{
    pOverlay->SetEndTime(flDuration);
}

//------------------------------------------------------------------------------
// Purpose: destroys the overlay
// Input: *pOverlay - 
//------------------------------------------------------------------------------
static void DebugOverlay_DestroyOverlay(OverlayBase_t* const pOverlay)
{
#if defined(CLIENT_DLL)
    if (v_DebugOverlay_DestroyOverlay)
    {
        v_DebugOverlay_DestroyOverlay(pOverlay);
        return;
    }
#endif // CLIENT_DLL
    AUTO_LOCK(*s_OverlayMutex);
    switch (pOverlay->m_Type)
    {
    case OverlayType_t::OVERLAY_BOX:
    case OverlayType_t::OVERLAY_SPHERE:
    case OverlayType_t::OVERLAY_LINE:
    case OverlayType_t::OVERLAY_CUSTOM_MESH:
    case OverlayType_t::OVERLAY_TRIANGLE:
    case OverlayType_t::OVERLAY_SWEPT_BOX:
    case OverlayType_t::OVERLAY_CAPSULE:
        pOverlay->m_Type = OverlayType_t::OVERLAY_DESTROYED;
        delete pOverlay;

        break;
        // Splines aren't allocated, they are stored in s_splineOverlays
        // which is a static array of 300 * OverlayLine_t. Just mark it
        // destroyed here so the spline item can be reused.
    case OverlayType_t::OVERLAY_SPLINE:
        pOverlay->m_Type = OverlayType_t::OVERLAY_DESTROYED;
        break;
    default:
        Assert(0); // Code bug; invalid overlay type.
        break;
    }
}

//------------------------------------------------------------------------------
// Purpose: draws a generic overlay
// Input: *pOverlay - 
//------------------------------------------------------------------------------
static void DebugOverlay_DrawLine(const Vector3D& origin, const Vector3D& dest, const Color color, const bool bZBuffer)
{
#if defined(CLIENT_DLL)
    if (v_RenderLine)
    {
        v_RenderLine(origin, dest, color, bZBuffer);
        return;
    }
#endif // CLIENT_DLL
    RenderLine(origin, dest, color, bZBuffer);
}

static void DebugOverlay_DrawOverlay(const OverlayBase_t* const pOverlay)
{
    switch (pOverlay->m_Type)
    {
    case OverlayType_t::OVERLAY_BOX:
    {
        const OverlayBox_t* const pBox = static_cast<const OverlayBox_t*>(pOverlay);

        if (pBox->a > 0)
        {
            RenderBox(pBox->transforms, pBox->mins, pBox->maxs, Color(pBox->r, pBox->g, pBox->b, pBox->a), !pBox->noDepthTest);
        }
        if (pBox->a < 255)
        {
            RenderWireframeBox(pBox->transforms, pBox->mins, pBox->maxs, Color(pBox->r, pBox->g, pBox->b, 255), !pBox->noDepthTest);
        }

        break;
    }
    case OverlayType_t::OVERLAY_SPHERE:
    {
        const OverlaySphere_t* const pSphere = static_cast<const OverlaySphere_t*>(pOverlay);

        if (pSphere->a > 0)
        {
            RenderSphere(pSphere->vOrigin, pSphere->flRadius, pSphere->nTheta, pSphere->nPhi,
                Color(pSphere->r, pSphere->g, pSphere->b, pSphere->a), !pSphere->noDepthTest);
        }
        if (pSphere->a < 255)
        {
            RenderWireframeSphere(pSphere->vOrigin, pSphere->flRadius, pSphere->nTheta, pSphere->nPhi,
                Color(pSphere->r, pSphere->g, pSphere->b, 255), !pSphere->noDepthTest);
        }

        break;
    }
    case OverlayType_t::OVERLAY_LINE:
    {
        const OverlayLine_t* const pLine = static_cast<const OverlayLine_t*>(pOverlay);
#if defined(CLIENT_DLL)
        static bool s_drewLine = false;
        if (!s_drewLine)
        {
            s_drewLine = true;
            Msg(eDLL_T::CLIENT, "[DBGDRAW] drawing line overlay\n");
        }
#endif // CLIENT_DLL
        DebugOverlay_DrawLine(pLine->origin, pLine->dest, Color(pLine->r, pLine->g, pLine->b, pLine->a), !pLine->noDepthTest);

        break;
    }
    case OverlayType_t::OVERLAY_CUSTOM_MESH:
    {
        // TODO: 128 * matrix3x4_t, figure out how to render this...
        // Nothing in the game is currently calling this overlay, so
        // implementing this isn't necessary.
        break;
    }
    case OverlayType_t::OVERLAY_SPLINE:
    {
        // This is used for the Smart Pistol laser.
        const OverlayLine_t* const pSpline = reinterpret_cast<const OverlayLine_t*>(pOverlay);
        DebugOverlay_DrawLine(pSpline->origin, pSpline->dest, Color(pSpline->r, pSpline->g, pSpline->b, pSpline->a), !pSpline->noDepthTest);

        break;
    }
    case OverlayType_t::OVERLAY_TRIANGLE:
    {
        const OverlayTriangle_t* const pTriangle = reinterpret_cast<const OverlayTriangle_t*>(pOverlay);
        RenderTriangle(pTriangle->p1, pTriangle->p2, pTriangle->p3, Color(pTriangle->r, pTriangle->g, pTriangle->b, pTriangle->a), !pTriangle->noDepthTest);

        break;
    }
    case OverlayType_t::OVERLAY_SWEPT_BOX:
    {
        const OverlaySweptBox_t* const pSweptBox = reinterpret_cast<const OverlaySweptBox_t*>(pOverlay);
        RenderWireframeSweptBox(pSweptBox->start, pSweptBox->end, pSweptBox->angles, pSweptBox->mins, pSweptBox->maxs,
            Color(pSweptBox->r, pSweptBox->g, pSweptBox->b, pSweptBox->a), !pSweptBox->noDepthTest);
        break;
    }
    case OverlayType_t::OVERLAY_CAPSULE:
    {
        const OverlayCapsule_t* const pCapsule = static_cast<const OverlayCapsule_t*>(pOverlay);
        RenderCapsule(pCapsule->start, pCapsule->end, pCapsule->radius, Color(pCapsule->r, pCapsule->g, pCapsule->b, pCapsule->a), !pCapsule->noDepthTest);

        break;
    }
    }
}

//------------------------------------------------------------------------------
// Purpose: overlay drawing and decaying entry point
// Input: bDraw - only runs the decaying logic if false
//------------------------------------------------------------------------------
#if defined(CLIENT_DLL)
// Overlay adds arrive from the server, so the list length is attacker-chosen
// unless it is bounded here. Sampled by the walk and enforced on apply.
static constexpr int kMaxLiveOverlays = 4096;

// Incremented by every replicated insert and resynced by the walk. The walk
// alone is not enough: its detour only attaches when both overlay-manager
// patterns resolve, and a counter that stops moving disarms the ceiling.
static int s_liveOverlayCount = 0;

static ConVar bridge_dbg_probe("bridge_dbg_probe", "0",
    FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT | FCVAR_CLIENTDLL,
    "Log the overlay walk: whether it ticks, what is in the list, and whether the draw gate passes.");

//------------------------------------------------------------------------------
// Probe: walk ticks, list contents, and whether the draw gate passes.
//------------------------------------------------------------------------------
static void DebugOverlay_ProbeWalk(const bool bDraw, const bool bOverlayEnabled, const OverlayBase_t* pHead)
{
    if (!bridge_dbg_probe.GetBool())
        return;

    static size_t s_walks = 0;
    const size_t walk = ++s_walks;

    // Loud on the first walk, then roughly once a second.
    if (walk != 1 && (walk % 60) != 0)
        return;

    int count = 0;

    for (const OverlayBase_t* pOverlay = pHead; pOverlay; pOverlay = pOverlay->m_pNextOverlay)
        count++;

    Msg(eDLL_T::CLIENT, "[DBGDRAW-TICK] walk#%zu draw=%d enabled=%d overlays=%d renderTick=%d overlayTick=%d\n",
        walk, (int)bDraw, (int)bOverlayEnabled, count,
        g_nRenderTickCount ? *g_nRenderTickCount : -1,
        g_nOverlayTickCount ? *g_nOverlayTickCount : -1);

    int i = 0;

    for (const OverlayBase_t* pOverlay = pHead; pOverlay && i < 8; pOverlay = pOverlay->m_pNextOverlay, i++)
    {
        Msg(eDLL_T::CLIENT, "[DBGDRAW-TICK]   [%d] type=%d creationTick=%d overlayTick=%d endTime=%f\n",
            i, (int)pOverlay->m_Type, pOverlay->m_nCreationTick,
            pOverlay->m_nOverlayTick, pOverlay->m_flEndTime);
    }
}
#endif // CLIENT_DLL

static void DebugOverlay_DrawAllOverlays(const bool bDraw)
{
    AUTO_LOCK(*s_OverlayMutex);

    DebugOverlay_ApplyDevDefaults();

#if defined(CLIENT_DLL)
    (void)bDraw;
    const bool bOverlayEnabled = !enable_debug_overlays || enable_debug_overlays->GetBool();
#else
    const bool bOverlayEnabled = (bDraw && enable_debug_overlays && enable_debug_overlays->GetBool());
#endif // CLIENT_DLL
    OverlayBase_t* pCurrOverlay = *s_pOverlays;

#if defined(CLIENT_DLL)
    DebugOverlay_ProbeWalk(bDraw, bOverlayEnabled, pCurrOverlay);

    {
        int overlayCount = 0;
        for (const OverlayBase_t* p = pCurrOverlay; p; p = p->m_pNextOverlay)
            overlayCount++;
        s_liveOverlayCount = overlayCount;
        static size_t s_walks = 0;
        static bool s_loggedLive = false;
        static bool s_warnedFlood = false;
        const size_t walk = ++s_walks;
        if (overlayCount == 0 && walk == 120)
            Msg(eDLL_T::CLIENT, "[DBGDRAW] overlay walk is ticking with an empty list\n");
        else if (overlayCount > 0 && !s_loggedLive)
        {
            s_loggedLive = true;
            Msg(eDLL_T::CLIENT, "[DBGDRAW] overlay list live count=%d type=%d\n",
                overlayCount, (int)pCurrOverlay->m_Type);
        }
        // Decay still runs. Drawing several thousand lines every frame is the
        // CPU spike; refuse the raster past this point until the list shrinks.
        if (overlayCount >= 1024)
        {
            if (!s_warnedFlood)
            {
                s_warnedFlood = true;
                Warning(eDLL_T::CLIENT, "[DBGDRAW] overlay list at %d entries; skipping draw until it decays below 512\n",
                    overlayCount);
            }
        }
        else if (s_warnedFlood && overlayCount < 512)
        {
            s_warnedFlood = false;
            Msg(eDLL_T::CLIENT, "[DBGDRAW] overlay list decayed to %d entries; drawing again\n",
                overlayCount);
        }
    }

    CMatRenderContext* overlayCtx = nullptr;
    if (bOverlayEnabled && s_liveOverlayCount < 1024 && (!s_engineMaterialSystemSlot || !*s_engineMaterialSystemSlot))
    {
        // Drawing without the pool push is the silent failure mode: the map
        // call returns null and every line is discarded with no error.
        static bool s_warnedSlot = false;
        if (!s_warnedSlot)
        {
            s_warnedSlot = true;
            Warning(eDLL_T::CLIENT, "[DBGDRAW] material system unresolved; overlays cannot push a geo pool and will not draw\n");
        }
    }
    if (bOverlayEnabled && s_liveOverlayCount < 1024 && s_engineMaterialSystemSlot && *s_engineMaterialSystemSlot)
    {
        overlayCtx = (*s_engineMaterialSystemSlot)->GetRenderContext();
        if (overlayCtx)
            overlayCtx->PushDynamicGeoPool(1);
        else
        {
            static bool s_warnedCtx = false;
            if (!s_warnedCtx)
            {
                s_warnedCtx = true;
                Warning(eDLL_T::CLIENT, "[DBGDRAW] GetRenderContext returned null; overlay walk will not draw\n");
            }
        }
    }
#endif // CLIENT_DLL

#if defined(CLIENT_DLL)
    const bool bSkipFloodDraw = s_liveOverlayCount >= 1024;
#else
    const bool bSkipFloodDraw = false;
#endif // CLIENT_DLL
    (void)bSkipFloodDraw;

    OverlayBase_t* pPrevOverlay = nullptr;
    OverlayBase_t* pNextOverlay = nullptr;

    while (pCurrOverlay)
    {
        // Is it time to kill this overlay?
        if (pCurrOverlay->IsDead())
        {
            if (pPrevOverlay)
            {
                // If I had a last overlay reset it's next pointer
                pPrevOverlay->m_pNextOverlay = pCurrOverlay->m_pNextOverlay;
            }
            else
            {
                // If the first line, reset the s_pOverlays pointer
                *s_pOverlays = pCurrOverlay->m_pNextOverlay;
            }

            pNextOverlay = pCurrOverlay->m_pNextOverlay;
            DebugOverlay_DestroyOverlay(pCurrOverlay);
            pCurrOverlay = pNextOverlay;
        }
        else
        {
            if (bOverlayEnabled)
            {
                bool bShouldDraw = false;

                if (pCurrOverlay->m_nCreationTick == -1)
                {
                    if (pCurrOverlay->m_nOverlayTick == *g_nOverlayTickCount ||
                        pCurrOverlay->m_nOverlayTick == -1)
                    {
                        bShouldDraw = true;
                    }
                }
                else
                {
                    bShouldDraw = pCurrOverlay->m_nCreationTick == *g_nRenderTickCount;
                }
                if (bShouldDraw)
                {
#if defined(CLIENT_DLL)
                    if (!bSkipFloodDraw)
#endif // CLIENT_DLL
                    DebugOverlay_DrawOverlay(pCurrOverlay);
                }
            }

            pPrevOverlay = pCurrOverlay;
            pCurrOverlay = pCurrOverlay->m_pNextOverlay;
        }
    }

#if defined(CLIENT_DLL)
    if (overlayCtx)
    {
        overlayCtx->PopDynamicGeoPool();
        overlayCtx->EndRenderer();
    }
#endif // CLIENT_DLL

    if (g_pDebugOverlay)
        g_pDebugOverlay->ClearDeadTextOverlays();


#ifndef DEDICATED
    // BSP collision debug rendering
    CBSPCollisionDebug::Render();
#endif // !DEDICATED
}

//------------------------------------------------------------------------------
// Purpose: clear dead overlays
//------------------------------------------------------------------------------
static void DebugOverlay_ClearDeadOverlays()
{
    AUTO_LOCK(*s_OverlayMutex);

    OverlayBase_t* pCurrOverlay = *s_pOverlays;
    OverlayBase_t* pPrevOverlay = nullptr;
    OverlayBase_t* pNextOverlay = nullptr;

    while (pCurrOverlay)
    {
        // Is it time to kill this overlay?
        if (pCurrOverlay->IsDead())
        {
            if (pPrevOverlay)
            {
                // If I had a last overlay reset it's next pointer
                pPrevOverlay->m_pNextOverlay = pCurrOverlay->m_pNextOverlay;
            }
            else
            {
                // If the first line, reset the s_pOverlays pointer
                *s_pOverlays = pCurrOverlay->m_pNextOverlay;
            }

            pNextOverlay = pCurrOverlay->m_pNextOverlay;
            DebugOverlay_DestroyOverlay(pCurrOverlay);
            pCurrOverlay = pNextOverlay;
        }
        else
        {
            pPrevOverlay = pCurrOverlay;
            pCurrOverlay = pCurrOverlay->m_pNextOverlay;
        }
    }
}

//-----------------------------------------------------------------------------
// Purpose: clears all overlays
//-----------------------------------------------------------------------------
static void DebugOverlay_ClearAllOverlays()
{
    AUTO_LOCK(*s_OverlayMutex);

    while (*s_pOverlays)
    {
        OverlayBase_t* pOldOverlay = *s_pOverlays;
        *s_pOverlays = (*s_pOverlays)->m_pNextOverlay;
        DebugOverlay_DestroyOverlay(pOldOverlay);
    }

    while (*s_pOverlayText)
    {
        OverlayText_t* cur_ol = *s_pOverlayText;
        *s_pOverlayText = (*s_pOverlayText)->nextOverlayText;
        delete cur_ol;
    }

    if (s_bDrawGrid)
        *s_bDrawGrid = false;
}

//------------------------------------------------------------------------------
// Purpose: clear all dead overlays; this is a separate version of the decaying
// logic found in DebugOverlay_DrawAllOverlays. The dedicated server
// needs to call this function as DebugOverlay_DrawAllOverlays won't
// be called as this is initiated from CViewRender, which is not on.
//------------------------------------------------------------------------------
void DebugOverlay_HandleDecayed()
{
    // These must always be called, even when the debug overlay is disabled
    // because the calls to the debug interface still take place. Its up to
    // the engine and SDK to deal with these calls. Not calling these will
    // cause overlays to stack up forever.
    DebugOverlay_ClearDeadOverlays();
    if (g_pDebugOverlay)
        g_pDebugOverlay->ClearDeadTextOverlays();
#ifndef CLIENT_DLL
    DebugOverlay_S2C_Flush();
#endif // !CLIENT_DLL
}

//-----------------------------------------------------------------------------
// Purpose: internal wrapper for adding new world positioned overlay text
//-----------------------------------------------------------------------------
static void DebugOverlay_AddTextOverlay(const Vector3D& pos, const int lineOffset, const float duration,
    const int r, const int g, const int b, const int a, const char* const text, const ssize_t textLen)
{
    OverlayText_t* const newOverlay = new OverlayText_t;

    if (!newOverlay)
        return;

    VectorCopy(pos, newOverlay->origin);

    newOverlay->textLen = textLen;
    newOverlay->textBuf = new char[textLen + 1];

    if (!newOverlay->textBuf)
    {
        delete newOverlay;
        return;
    }

    Q_strncpy(newOverlay->textBuf, text, textLen + 1);

    newOverlay->bUseOrigin = true;
    newOverlay->lineOffset = lineOffset;

    newOverlay->SetEndTime(duration);

    newOverlay->r = r;
    newOverlay->g = g;
    newOverlay->b = b;
    newOverlay->a = a;

    newOverlay->nextOverlayText = *s_pOverlayText;
    *s_pOverlayText = newOverlay;
}

//-----------------------------------------------------------------------------
// Purpose: add new entity positioned overlay text
//-----------------------------------------------------------------------------
void CIVDebugOverlay::AddEntityTextOverlay(CIVDebugOverlay* const thisptr, const int entIndex, const int lineOffset, const float duration, 
                                            const int r, const int g, const int b, const int a, const char* const format, ...)
{
    if (!enable_debug_text_overlays.GetBool() || !DebugOverlay_CanApplyOverlay())
        return;

    Vector3D pos;

    if (!DebugOverlay_GetEntityOriginClientOrServer(entIndex, pos))
        return;

    AUTO_LOCK(*s_OverlayMutex);

    va_start(thisptr->m_argptr, format);
    const int textLen = Q_vsnprintf(thisptr->m_text, sizeof(thisptr->m_text), format, thisptr->m_argptr);
    va_end(thisptr->m_argptr);

    if (textLen > 0)
        DebugOverlay_AddTextOverlay(pos, lineOffset, duration, r, g, b, a, thisptr->m_text, textLen);
}

//-----------------------------------------------------------------------------
// Purpose: add new world positioned overlay text
//-----------------------------------------------------------------------------
void CIVDebugOverlay::AddTextOverlay(CIVDebugOverlay* const thisptr, const Vector3D& origin, const float duration, const char* const format, ...)
{
    if (!enable_debug_text_overlays.GetBool() || !DebugOverlay_CanApplyOverlay())
        return;

    AUTO_LOCK(*s_OverlayMutex);

    va_start(thisptr->m_argptr, format);
    const int textLen = Q_vsnprintf(thisptr->m_text, sizeof(thisptr->m_text), format, thisptr->m_argptr);
    va_end(thisptr->m_argptr);

    if (textLen > 0)
        DebugOverlay_AddTextOverlay(origin, 0, duration, 255, 255, 255, 255, thisptr->m_text, textLen);
}

//-----------------------------------------------------------------------------
// Purpose: add new world positioned overlay text at line offset
//-----------------------------------------------------------------------------
void CIVDebugOverlay::AddTextOverlayAtOffset(CIVDebugOverlay* const thisptr, const Vector3D& origin, const int lineOffset, const float duration, const char* const format, ...)
{
    if (!enable_debug_text_overlays.GetBool() || !DebugOverlay_CanApplyOverlay())
        return;

    AUTO_LOCK(*s_OverlayMutex);

    va_start(thisptr->m_argptr, format);
    const int textLen = Q_vsnprintf(thisptr->m_text, sizeof(thisptr->m_text), format, thisptr->m_argptr);
    va_end(thisptr->m_argptr);

    if (textLen > 0)
        DebugOverlay_AddTextOverlay(origin, lineOffset, duration, 255, 255, 255, 255, thisptr->m_text, textLen);
}

//-----------------------------------------------------------------------------
// Purpose: add new world positioned overlay text using 32 bit color
//-----------------------------------------------------------------------------
void CIVDebugOverlay::AddTextOverlayRGBu32(CIVDebugOverlay* const thisptr, const Vector3D& origin, const int lineOffset, const float duration, 
    const int r, const int g, const int b, const int a, PRINTF_FORMAT_STRING const char* const format, ...) FMTFUNCTION(9, 10)
{
    if (!enable_debug_text_overlays.GetBool())
        return;

    AUTO_LOCK(*s_OverlayMutex);

    va_start(thisptr->m_argptr, format);
    const int textLen = Q_vsnprintf(thisptr->m_text, sizeof(thisptr->m_text), format, thisptr->m_argptr);
    va_end(thisptr->m_argptr);

    if (textLen > 0)
        DebugOverlay_AddTextOverlay(origin, lineOffset, duration, r, g, b, a, thisptr->m_text, textLen);
}

//-----------------------------------------------------------------------------
// Purpose: add new world positioned overlay text using float color
//-----------------------------------------------------------------------------
void CIVDebugOverlay::AddTextOverlayRGBf32(CIVDebugOverlay* const thisptr, const Vector3D& origin, const int lineOffset, const float duration,
    const float r, const float g, const float b, const float a, PRINTF_FORMAT_STRING const char* const format, ...) FMTFUNCTION(8, 9)
{
    if (!enable_debug_text_overlays.GetBool() || !DebugOverlay_CanApplyOverlay())
        return;

    AUTO_LOCK(*s_OverlayMutex);

    va_start(thisptr->m_argptr, format);
    const int textLen = Q_vsnprintf(thisptr->m_text, sizeof(thisptr->m_text), format, thisptr->m_argptr);
    va_end(thisptr->m_argptr);

    if (textLen > 0)
    {
        const int cr = (int)Clamp(r * 255.f, 0.f, 255.f);
        const int cg = (int)Clamp(g * 255.f, 0.f, 255.f);
        const int cb = (int)Clamp(b * 255.f, 0.f, 255.f);
        const int ca = (int)Clamp(a * 255.f, 0.f, 255.f);

        DebugOverlay_AddTextOverlay(origin, lineOffset, duration, cr, cg, cb, ca, thisptr->m_text, textLen);
    }
}

//-----------------------------------------------------------------------------
// Purpose: internal wrapper for adding new screen positioned overlay text
//-----------------------------------------------------------------------------
static void DebugOverlay_AddScreenTextOverlay(const float flXpos, const float flYpos, const int lineOffset,
    const float duration, const int r, const int g, const int b, const int a, const char* const text, const ssize_t textLen)
{
    OverlayText_t* const newOverlay = new OverlayText_t;

    if (!newOverlay)
        return;

    newOverlay->screenPos.Init(flXpos, flYpos);

    newOverlay->textLen = textLen;
    newOverlay->textBuf = new char[textLen + 1];

    if (!newOverlay->textBuf)
    {
        delete newOverlay;
        return;
    }

    Q_strncpy(newOverlay->textBuf, text, textLen + 1);

    newOverlay->bUseOrigin = false;
    newOverlay->lineOffset = lineOffset;

    newOverlay->SetEndTime(duration);

    newOverlay->r = r;
    newOverlay->g = g;
    newOverlay->b = b;
    newOverlay->a = a;

    newOverlay->nextOverlayText = *s_pOverlayText;
    *s_pOverlayText = newOverlay;
}

//-----------------------------------------------------------------------------
// Purpose: add new screen positioned overlay text at offset
//-----------------------------------------------------------------------------
void CIVDebugOverlay::AddScreenTextOverlayAtOffsetInternal(CIVDebugOverlay* const thisptr, const float flXPos, const float flYPos,
    const int lineOffset, const float flDuration, const int r, const int g, const int b, const int a, const char* const text)
{
    if (!enable_debug_text_overlays.GetBool() || !DebugOverlay_CanApplyOverlay())
        return;

    const ssize_t textLen = (ssize_t)strlen(text);

    if (textLen < 1)
        return; // Empty.

    AUTO_LOCK(*s_OverlayMutex);
    DebugOverlay_AddScreenTextOverlay(flXPos, flYPos, lineOffset, flDuration, r, g, b, a, text, textLen);
}

//-----------------------------------------------------------------------------
// Purpose: add new screen positioned overlay text
//-----------------------------------------------------------------------------
void CIVDebugOverlay::AddScreenTextOverlayInternal(CIVDebugOverlay* const thisptr, const float flXPos, const float flYPos, const float flDuration, const int r, const int g, const int b, const int a, const char* const text)
{
    if (!enable_debug_text_overlays.GetBool() || !DebugOverlay_CanApplyOverlay())
        return;

    const ssize_t textLen = (ssize_t)strlen(text);

    if (textLen < 1)
        return; // Empty.

    AUTO_LOCK(*s_OverlayMutex);
    DebugOverlay_AddScreenTextOverlay(flXPos, flYPos, 0, flDuration, r, g, b, a, text, textLen);
}

//-----------------------------------------------------------------------------
// These are the same as above, except we have to shift the 'this' pointer back
// with sizeof(void*) bytes because we call CIVDebugOverlay methods which uses
// its member variables, IVPhysicsDebugOverlay methods will have the thisptr
// shifted with 8 bytes forward due to compiler optimizations. Only functions
// using member variables or IVDebugOverlay methods have been duplicated here.
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Purpose: add new entity positioned overlay text
//-----------------------------------------------------------------------------
void CIVDebugOverlay::AddPhysicsEntityTextOverlay(CIVDebugOverlay* const thisptr, const int entIndex, const int lineOffset, const float duration, const int r, const int g, const int b, const int a, const char* const format, ...)
{
    if (!enable_debug_text_overlays.GetBool() || !DebugOverlay_CanApplyOverlay())
        return;

    Vector3D pos;

    if (!DebugOverlay_GetEntityOriginClientOrServer(entIndex, pos))
        return;

    AUTO_LOCK(*s_OverlayMutex);
    CIVDebugOverlay* const thisprAdj = (CIVDebugOverlay*)((intptr_t)(thisptr)-sizeof(void*));

    va_start(thisprAdj->m_argptr, format);
    const int textLen = Q_vsnprintf(thisprAdj->m_text, sizeof(thisprAdj->m_text), format, thisprAdj->m_argptr);
    va_end(thisprAdj->m_argptr);

    if (textLen > 0)
        DebugOverlay_AddTextOverlay(pos, lineOffset, duration, r, g, b, a, thisprAdj->m_text, textLen);
}

//-----------------------------------------------------------------------------
// Purpose: add new world positioned overlay text
//-----------------------------------------------------------------------------
void CIVDebugOverlay::AddPhysicsTextOverlay(CIVDebugOverlay* const thisptr, const Vector3D& origin, const float duration, const char* const format, ...)
{
    if (!enable_debug_text_overlays.GetBool() || !DebugOverlay_CanApplyOverlay())
        return;

    AUTO_LOCK(*s_OverlayMutex);
    CIVDebugOverlay* const thisprAdj = (CIVDebugOverlay*)((intptr_t)(thisptr)-sizeof(void*));

    va_start(thisprAdj->m_argptr, format);
    const int textLen = Q_vsnprintf(thisprAdj->m_text, sizeof(thisprAdj->m_text), format, thisprAdj->m_argptr);
    va_end(thisprAdj->m_argptr);

    if (textLen > 0)
        DebugOverlay_AddTextOverlay(origin, 0, duration, 255, 255, 255, 255, thisprAdj->m_text, textLen);
}

//-----------------------------------------------------------------------------
// Purpose: add new world positioned overlay text at line offset
//-----------------------------------------------------------------------------
void CIVDebugOverlay::AddPhysicsTextOverlayAtOffset(CIVDebugOverlay* const thisptr, const Vector3D& origin, const int lineOffset, const float duration, const char* const format, ...)
{
    if (!enable_debug_text_overlays.GetBool() || !DebugOverlay_CanApplyOverlay())
        return;

    AUTO_LOCK(*s_OverlayMutex);
    CIVDebugOverlay* const thisprAdj = (CIVDebugOverlay*)((intptr_t)(thisptr)-sizeof(void*));

    va_start(thisprAdj->m_argptr, format);
    const int textLen = Q_vsnprintf(thisprAdj->m_text, sizeof(thisprAdj->m_text), format, thisprAdj->m_argptr);
    va_end(thisprAdj->m_argptr);

    if (textLen > 0)
        DebugOverlay_AddTextOverlay(origin, lineOffset, duration, 255, 255, 255, 255, thisprAdj->m_text, textLen);
}

//-----------------------------------------------------------------------------
// Purpose: add new world positioned overlay text using float color
//-----------------------------------------------------------------------------
void CIVDebugOverlay::AddPhysicsTextOverlayRGBf32(CIVDebugOverlay* const thisptr, const Vector3D& origin, const int lineOffset, const float duration,
    const float r, const float g, const float b, const float a, PRINTF_FORMAT_STRING const char* const format, ...) FMTFUNCTION(8, 9)
{
    if (!enable_debug_text_overlays.GetBool() || !DebugOverlay_CanApplyOverlay())
        return;

    AUTO_LOCK(*s_OverlayMutex);
    CIVDebugOverlay* const thisprAdj = (CIVDebugOverlay*)((intptr_t)(thisptr)-sizeof(void*));

    va_start(thisprAdj->m_argptr, format);
    const int textLen = Q_vsnprintf(thisprAdj->m_text, sizeof(thisprAdj->m_text), format, thisprAdj->m_argptr);
    va_end(thisprAdj->m_argptr);

    if (textLen > 0)
    {
        const int cr = (int)Clamp(r * 255.f, 0.f, 255.f);
        const int cg = (int)Clamp(g * 255.f, 0.f, 255.f);
        const int cb = (int)Clamp(b * 255.f, 0.f, 255.f);
        const int ca = (int)Clamp(a * 255.f, 0.f, 255.f);

        DebugOverlay_AddTextOverlay(origin, lineOffset, duration, cr, cg, cb, ca, thisprAdj->m_text, textLen);
    }
}

#if defined(CLIENT_DLL)
//------------------------------------------------------------------------------
// Purpose: announce every engine-side line insert so the list can be reasoned
//          about without a script round trip
// Note: the S2C replicate path reaches the engine adder through the saved
//       trampoline, so it is counted by its own batch log instead of here.
//------------------------------------------------------------------------------
static void Hook_DebugOverlay_AddLineOverlay(const Vector3D* origin, const Vector3D* dest,
    int r, int g, int b, int a, bool noDepthTest, float duration)
{
    static size_t s_inserts = 0;
    const size_t n = ++s_inserts;

    if (n <= 8 || (n % 256) == 0)
    {
        Msg(eDLL_T::CLIENT, "[DBGDRAW] AddLineOverlay #%zu (%.1f %.1f %.1f)->(%.1f %.1f %.1f) rgba=%d,%d,%d,%d noDepth=%d dur=%.5f\n",
            n,
            origin ? origin->x : 0.f, origin ? origin->y : 0.f, origin ? origin->z : 0.f,
            dest ? dest->x : 0.f, dest ? dest->y : 0.f, dest ? dest->z : 0.f,
            r, g, b, a, (int)noDepthTest, duration);
    }

    v_DebugOverlay_AddLineOverlay(origin, dest, r, g, b, a, noDepthTest, duration);
}
#endif // CLIENT_DLL

///////////////////////////////////////////////////////////////////////////////
void VDebugOverlay::Detour(const bool bAttach) const
{
#if defined(CLIENT_DLL)
    if (bAttach)
        DebugOverlay_ApplyDevDefaults();
    if (v_DebugOverlay_DrawAllOverlays && v_DebugOverlay_ClearAllOverlays)
    {
        DetourSetup(&v_DebugOverlay_DrawAllOverlays, &DebugOverlay_DrawAllOverlays, bAttach);
        DetourSetup(&v_DebugOverlay_ClearAllOverlays, &DebugOverlay_ClearAllOverlays, bAttach);
    }
    if (v_DebugOverlay_AddLineOverlay)
        DetourSetup(&v_DebugOverlay_AddLineOverlay, &Hook_DebugOverlay_AddLineOverlay, bAttach);
#else
    DetourSetup(&v_DebugOverlay_DrawAllOverlays, &DebugOverlay_DrawAllOverlays, bAttach);
    DetourSetup(&v_DebugOverlay_ClearAllOverlays, &DebugOverlay_ClearAllOverlays, bAttach);
    DetourSetup(&v_DebugOverlay_SetEndTime, &DebugOverlay_SetEndTime, bAttach);

    if (bAttach)
    {
        void* null;

        // Replace the nulled functions in the IVPhysicsDebugOverlay implementation with ours.
        CMemory::HookVirtualMethod((uintptr_t)g_pIVPhysicsDebugOverlay_VFTable, CIVDebugOverlay::AddPhysicsEntityTextOverlay, 0, &null);
        CMemory::HookVirtualMethod((uintptr_t)g_pIVPhysicsDebugOverlay_VFTable, CIVDebugOverlay::AddPhysicsTextOverlay, 4, &null);
        CMemory::HookVirtualMethod((uintptr_t)g_pIVPhysicsDebugOverlay_VFTable, CIVDebugOverlay::AddPhysicsTextOverlayAtOffset, 5, &null);
        CMemory::HookVirtualMethod((uintptr_t)g_pIVPhysicsDebugOverlay_VFTable, CIVDebugOverlay::AddScreenTextOverlayInternal, 6, &null);
        CMemory::HookVirtualMethod((uintptr_t)g_pIVPhysicsDebugOverlay_VFTable, CIVDebugOverlay::AddSweptBoxInternal, 7, &null); // NEW: now supports setting depth testing.
        CMemory::HookVirtualMethod((uintptr_t)g_pIVPhysicsDebugOverlay_VFTable, CIVDebugOverlay::AddPhysicsTextOverlayRGBf32, 8, &null);

        // Replace the nulled functions in the IVDebugOverlay implementation with ours.
        CMemory::HookVirtualMethod((uintptr_t)g_pIVDebugOverlay_VFTable, CIVDebugOverlay::AddEntityTextOverlay, 0, &null);
        CMemory::HookVirtualMethod((uintptr_t)g_pIVDebugOverlay_VFTable, CIVDebugOverlay::AddSphereOverlayInternal, 3, &null); // NEW: now supports setting depth testing.
        CMemory::HookVirtualMethod((uintptr_t)g_pIVDebugOverlay_VFTable, CIVDebugOverlay::AddTextOverlayAtOffset, 8, &null);
        CMemory::HookVirtualMethod((uintptr_t)g_pIVDebugOverlay_VFTable, CIVDebugOverlay::AddTextOverlay, 9, &null);
        CMemory::HookVirtualMethod((uintptr_t)g_pIVDebugOverlay_VFTable, CIVDebugOverlay::AddScreenTextOverlayAtOffsetInternal , 10, &null);
        CMemory::HookVirtualMethod((uintptr_t)g_pIVDebugOverlay_VFTable, CIVDebugOverlay::AddScreenTextOverlayInternal, 11, &null);
        CMemory::HookVirtualMethod((uintptr_t)g_pIVDebugOverlay_VFTable, CIVDebugOverlay::AddSweptBoxInternal, 12, &null); // NEW: now supports setting depth testing.
        CMemory::HookVirtualMethod((uintptr_t)g_pIVDebugOverlay_VFTable, CIVDebugOverlay::AddTextOverlayRGBu32, 24, &null);
        CMemory::HookVirtualMethod((uintptr_t)g_pIVDebugOverlay_VFTable, CIVDebugOverlay::AddTextOverlayRGBf32, 25, &null);

        // The overlay adder at index 27 is unknown and never used, its renderer also doesn't
        // exist. Replaced with capsule renderer allowing us to add these through the interface.
        CMemory::HookVirtualMethod((uintptr_t)g_pIVDebugOverlay_VFTable, CIVDebugOverlay::AddCapsuleOverlayInternal, 27, &null);

        if (g_pIVDebugOverlay_VFTable)
        {
            CMemory::HookVirtualMethod((uintptr_t)g_pIVDebugOverlay_VFTable, reinterpret_cast<void*>(&Hook_AddTransformedBoxOverlay), 1,
                reinterpret_cast<void**>(&v_CIVDebugOverlay_AddTransformedBoxOverlay));
            CMemory::HookVirtualMethod((uintptr_t)g_pIVDebugOverlay_VFTable, reinterpret_cast<void*>(&Hook_AddBoxOverlay), 2,
                reinterpret_cast<void**>(&v_CIVDebugOverlay_AddBoxOverlay));
            CMemory::HookVirtualMethod((uintptr_t)g_pIVDebugOverlay_VFTable, reinterpret_cast<void*>(&Hook_AddTriangleOverlay), 4,
                reinterpret_cast<void**>(&v_CIVDebugOverlay_AddTriangleOverlay));
            CMemory::HookVirtualMethod((uintptr_t)g_pIVDebugOverlay_VFTable, reinterpret_cast<void*>(&Hook_AddLineOverlay), 5,
                reinterpret_cast<void**>(&v_CIVDebugOverlay_AddLineOverlay));
            CMemory::HookVirtualMethod((uintptr_t)g_pIVDebugOverlay_VFTable, reinterpret_cast<void*>(&Hook_AddLineOverlayAlpha), 26,
                reinterpret_cast<void**>(&v_CIVDebugOverlay_AddLineOverlayAlpha));
        }
    }
#endif // CLIENT_DLL
}

#if defined(CLIENT_DLL)
static ConVar bridge_debug_draw("bridge_debug_draw", "1",
    FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT | FCVAR_CLIENTDLL,
    "Master gate for the bridge_dbg_* debug drawing commands.");

static ConVar bridge_dbg_duration("bridge_dbg_duration", "5",
    FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT | FCVAR_CLIENTDLL,
    "Seconds a bridge_dbg_* shape persists. 0 renders it once, immediately.");

// A persisted shape goes through the overlay manager so DrawAllOverlays redraws
// it every frame; an immediate draw only ever reaches the current frame.
static bool BridgeDbg_PersistFor(float* const outDuration)
{
    const float duration = bridge_dbg_duration.GetFloat();

    if (duration <= 0.0f || !g_pDebugOverlay)
        return false;

    *outDuration = duration;
    return true;
}

static void CC_BridgeDbg_Line_f(const CCommand& args)
{
    if (!bridge_debug_draw.GetBool())
        return;

    if (args.ArgC() < 7)
        return;

    const Vector3D p1(float(atof(args[1])), float(atof(args[2])), float(atof(args[3])));
    const Vector3D p2(float(atof(args[4])), float(atof(args[5])), float(atof(args[6])));
    Color color(255, 255, 255, 255);
    if (args.ArgC() >= 11)
        color = Color(atoi(args[7]), atoi(args[8]), atoi(args[9]), atoi(args[10]));

    float duration;
    if (BridgeDbg_PersistFor(&duration))
        g_pDebugOverlay->AddLineOverlayWithAlpha(p1, p2, color.r(), color.g(), color.b(), color.a(), false, duration);
    else
        RenderLine(p1, p2, color, true);
}
static ConCommand bridge_dbg_line("bridge_dbg_line", CC_BridgeDbg_Line_f,
    "Draw a debug line. Usage: bridge_dbg_line x1 y1 z1 x2 y2 z2 [r g b a]",
    FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT | FCVAR_CLIENTDLL);

static void CC_BridgeDbg_Box_f(const CCommand& args)
{
    if (!bridge_debug_draw.GetBool())
        return;

    if (args.ArgC() < 7)
        return;

    const Vector3D origin(float(atof(args[1])), float(atof(args[2])), float(atof(args[3])));
    const Vector3D extents(float(atof(args[4])), float(atof(args[5])), float(atof(args[6])));
    const Vector3D mins(-extents.x, -extents.y, -extents.z);
    const Vector3D maxs(extents.x, extents.y, extents.z);
    Color color(255, 255, 255, 255);
    if (args.ArgC() >= 11)
        color = Color(atoi(args[7]), atoi(args[8]), atoi(args[9]), atoi(args[10]));

    float duration;
    if (BridgeDbg_PersistFor(&duration))
        g_pDebugOverlay->AddBoxOverlay(origin, mins, maxs, color.r(), color.g(), color.b(), color.a(), false, duration);
    else
        DebugDrawBox(origin, { 0.f, 0.f, 0.f }, mins, maxs, color, true);
}
static ConCommand bridge_dbg_box("bridge_dbg_box", CC_BridgeDbg_Box_f,
    "Draw a debug box. Usage: bridge_dbg_box x y z ex ey ez [r g b a]",
    FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT | FCVAR_CLIENTDLL);

static void CC_BridgeDbg_Sphere_f(const CCommand& args)
{
    if (!bridge_debug_draw.GetBool())
        return;

    if (args.ArgC() < 5)
        return;

    const Vector3D origin(float(atof(args[1])), float(atof(args[2])), float(atof(args[3])));
    const float radius = float(atof(args[4]));
    if (radius <= 0.f)
        return;
    Color color(255, 255, 255, 255);
    if (args.ArgC() >= 9)
        color = Color(atoi(args[5]), atoi(args[6]), atoi(args[7]), atoi(args[8]));

    float duration;
    if (BridgeDbg_PersistFor(&duration))
        g_pDebugOverlay->AddSphereOverlay(origin, radius, 16, 12, color.r(), color.g(), color.b(), color.a(), false, duration);
    else
        RenderWireframeSphere(origin, radius, 16, 12, color, true);
}
static ConCommand bridge_dbg_sphere("bridge_dbg_sphere", CC_BridgeDbg_Sphere_f,
    "Draw a debug wireframe sphere. Usage: bridge_dbg_sphere x y z r [r g b a]",
    FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT | FCVAR_CLIENTDLL);

static void CC_BridgeDbg_Circle_f(const CCommand& args)
{
    if (!bridge_debug_draw.GetBool())
        return;

    if (args.ArgC() < 5)
        return;

    const Vector3D origin(float(atof(args[1])), float(atof(args[2])), float(atof(args[3])));
    const float radius = float(atof(args[4]));
    int segments = args.ArgC() >= 6 ? atoi(args[5]) : 32;
    if (segments < 3)
        segments = 3;
    const Color color(255, 255, 255, 255);

    DebugDrawCircle(origin, { 90.f, 0.f, 0.f }, radius, color, segments, true);
}
static ConCommand bridge_dbg_circle("bridge_dbg_circle", CC_BridgeDbg_Circle_f,
    "Draw a debug circle. Usage: bridge_dbg_circle x y z r [segments]",
    FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT | FCVAR_CLIENTDLL);

static void CC_BridgeDbg_Capsule_f(const CCommand& args)
{
    if (!bridge_debug_draw.GetBool())
        return;

    if (args.ArgC() < 8)
        return;

    const Vector3D start(float(atof(args[1])), float(atof(args[2])), float(atof(args[3])));
    const Vector3D end(float(atof(args[4])), float(atof(args[5])), float(atof(args[6])));
    const float radius = float(atof(args[7]));
    const Color color(255, 255, 255, 255);

    float duration;
    if (BridgeDbg_PersistFor(&duration))
        g_pDebugOverlay->AddCapsuleOverlay(start, end, radius, color.r(), color.g(), color.b(), color.a(), false, duration);
    else
        RenderCapsule(start, end, radius, color, true);
}
static ConCommand bridge_dbg_capsule("bridge_dbg_capsule", CC_BridgeDbg_Capsule_f,
    "Draw a debug capsule. Usage: bridge_dbg_capsule x1 y1 z1 x2 y2 z2 radius",
    FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT | FCVAR_CLIENTDLL);

static void CC_BridgeDbg_Axis_f(const CCommand& args)
{
    if (!bridge_debug_draw.GetBool())
        return;

    if (args.ArgC() < 4)
        return;

    const Vector3D origin(float(atof(args[1])), float(atof(args[2])), float(atof(args[3])));
    const float scale = args.ArgC() >= 5 ? float(atof(args[4])) : 50.f;

    DebugDrawAxis(origin, { 0.f, 0.f, 0.f }, scale, true);
}
static ConCommand bridge_dbg_axis("bridge_dbg_axis", CC_BridgeDbg_Axis_f,
    "Draw a debug axis. Usage: bridge_dbg_axis x y z [scale]",
    FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT | FCVAR_CLIENTDLL);

static void CC_BridgeDbg_Mark_f(const CCommand& args)
{
    if (!bridge_debug_draw.GetBool())
        return;

    if (args.ArgC() < 5)
        return;

    const Vector3D origin(float(atof(args[1])), float(atof(args[2])), float(atof(args[3])));
    const float radius = float(atof(args[4]));
    if (radius <= 0.f)
        return;
    const Color color(255, 255, 255, 255);

    DebugDrawMark(origin, radius, color, true);
}
static ConCommand bridge_dbg_mark("bridge_dbg_mark", CC_BridgeDbg_Mark_f,
    "Draw a debug mark. Usage: bridge_dbg_mark x y z radius",
    FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT | FCVAR_CLIENTDLL);

//------------------------------------------------------------------------------
// Zero duration on a replicated shape: promote to next client frame (net apply can miss this tick's walk).
//-----------------------------------------------------------------------------
static float DebugOverlay_ReplicatedDuration(const float duration)
{
    if (!isfinite(duration) || duration < 0.0f)
        return NDEBUG_PERSIST_TILL_NEXT_CLIENT;
    if (duration > 5.0f)
        return 5.0f;
    return (duration == 0.0f) ? NDEBUG_PERSIST_TILL_NEXT_CLIENT : duration;
}

static int DebugOverlay_ClampSphereSegs(const int n)
{
    if (n < 8)
        return 8;
    if (n > 32)
        return 32;
    return n;
}

//------------------------------------------------------------------------------
static void CC_BridgeDbg_LineTest_f(const CCommand& args)
{
    if (!v_DebugOverlay_AddLineOverlay || !s_pOverlays)
    {
        Warning(eDLL_T::CLIENT, "[DBGDRAW] line test unavailable: addLine=0x%llX overlays=0x%llX\n",
            (unsigned long long)(uintptr_t)v_DebugOverlay_AddLineOverlay,
            (unsigned long long)(uintptr_t)s_pOverlays);
        return;
    }

    Msg(eDLL_T::CLIENT, "[DBGDRAW] line test: head=0x%llX enabled=%d renderTick=%d overlayTick=%d stage=%d\n",
        (unsigned long long)(uintptr_t)*s_pOverlays,
        (int)(!enable_debug_overlays || enable_debug_overlays->GetBool()),
        g_nRenderTickCount ? *g_nRenderTickCount : -1,
        g_nOverlayTickCount ? *g_nOverlayTickCount : -1,
        g_nOverlayStage ? *g_nOverlayStage : -1);

    // No args: grid across the playable volume (world origin is invisible in-match).
    if (args.ArgC() < 4)
    {
        const float spacing = Clamp((args.ArgC() >= 2) ? float(atof(args[1])) : 4096.f, 64.f, 65536.f);
        const float extent = Clamp((args.ArgC() >= 3) ? float(atof(args[2])) : 20480.f, 0.f, 131072.f);
        const float duration = 30.f;
        int drawn = 0;

        // The loop is quadratic in extent/spacing, so it needs a hard stop as
        // well as clamped inputs before it reaches the engine allocator.
        const int kMaxGridLines = 2048;

        if (spacing >= 1.f)
        {
            for (float x = -extent; x <= extent && drawn < kMaxGridLines; x += spacing)
            {
                for (float y = -extent; y <= extent && drawn < kMaxGridLines; y += spacing)
                {
                    const Vector3D from(x, y, -16384.f);
                    const Vector3D to(x, y, 16384.f);
                    v_DebugOverlay_AddLineOverlay(&from, &to, 255, 0, 0, 255, true, duration);
                    drawn++;
                }
            }
        }

        Msg(eDLL_T::CLIENT, "[DBGDRAW] line test: grid spacing=%.0f extent=%.0f lines=%d head=0x%llX\n",
            spacing, extent, drawn, (unsigned long long)(uintptr_t)*s_pOverlays);
        return;
    }

    const Vector3D origin(float(atof(args[1])), float(atof(args[2])), float(atof(args[3])));
    const float height = (args.ArgC() >= 5) ? float(atof(args[4])) : 256.f;
    const float duration = (args.ArgC() >= 6) ? float(atof(args[5])) : 30.f;
    const Vector3D dest(origin.x, origin.y, origin.z + height);

    v_DebugOverlay_AddLineOverlay(&origin, &dest, 255, 0, 0, 255, true, duration);

    const OverlayBase_t* const head = *s_pOverlays;
    if (!head)
    {
        Warning(eDLL_T::CLIENT, "[DBGDRAW] line test: engine adder refused, list still empty\n");
        return;
    }

    Msg(eDLL_T::CLIENT, "[DBGDRAW] line test: head=0x%llX type=%d creationTick=%d overlayTick=%d endTime=%f\n",
        (unsigned long long)(uintptr_t)head, (int)head->m_Type,
        head->m_nCreationTick, head->m_nOverlayTick, head->m_flEndTime);
}
static ConCommand bridge_dbg_line_test("bridge_dbg_line_test", CC_BridgeDbg_LineTest_f,
    "Insert engine line overlays and report the list head. Usage: bridge_dbg_line_test [spacing [extent]] | bridge_dbg_line_test x y z [height [duration]]",
    FCVAR_CHEAT | FCVAR_CLIENTDLL);

static void DebugOverlay_InsertLine(const Vector3D& origin, const Vector3D& dest,
    const int r, const int g, const int b, const int a, const bool noDepthTest, const float duration)
{
    if (v_DebugOverlay_AddLineOverlay)
    {
        const float persist = (duration <= 0.0f) ? NDEBUG_PERSIST_TILL_NEXT_CLIENT : duration;
        v_DebugOverlay_AddLineOverlay(&origin, &dest, r, g, b, a, noDepthTest, persist);
        s_liveOverlayCount++;
        return;
    }

    if (!s_pOverlays || !s_OverlayMutex)
        return;

    AUTO_LOCK(*s_OverlayMutex);
    OverlayLine_t* const item = new OverlayLine_t;
    if (!item)
        return;

    item->origin = origin;
    item->dest = dest;
    item->r = r;
    item->g = g;
    item->b = b;
    item->a = a;
    item->noDepthTest = noDepthTest;
    item->SetEndTime(DebugOverlay_ReplicatedDuration(duration));
    item->m_pNextOverlay = *s_pOverlays;
    *s_pOverlays = item;
    s_liveOverlayCount++;
}

//-----------------------------------------------------------------------------
// Purpose: rebuild an oriented box and insert it as its twelve edges
// Note: lines rather than an OverlayBox_t because the engine's own RenderLine
// is the one draw path proven on this build; the SDK box renderer goes through
// a different mesh path that has never been exercised here.
//-----------------------------------------------------------------------------
static void DebugOverlay_InsertTransformedBox(const Vector3D& origin, const QAngle& angles,
    const Vector3D& mins, const Vector3D& maxs,
    const int r, const int g, const int b, const int a, const bool noDepthTest, const float duration)
{
    matrix3x4_t transforms;
    AngleMatrix(angles, origin, transforms);

    Vector3D corner[8];

    for (int i = 0; i < 8; i++)
    {
        const Vector3D local((i & 1) ? maxs.x : mins.x, (i & 2) ? maxs.y : mins.y, (i & 4) ? maxs.z : mins.z);
        VectorTransform(local, transforms, corner[i]);
    }

    // Corner index bit n selects the max side of axis n, so a pair differing in
    // exactly one bit is one edge of the box.
    static const int edge[12][2] =
    {
        { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 },
        { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 },
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
    };

    for (int i = 0; i < 12; i++)
        DebugOverlay_InsertLine(corner[edge[i][0]], corner[edge[i][1]], r, g, b, a, noDepthTest, duration);
}

static void DebugOverlay_InsertBox(const Vector3D& origin, const Vector3D& mins, const Vector3D& maxs,
    const int r, const int g, const int b, const int a, const bool noDepthTest, const float duration)
{
    if (!s_pOverlays || !s_OverlayMutex)
        return;

    AUTO_LOCK(*s_OverlayMutex);
    OverlayBox_t* const item = new OverlayBox_t;
    if (!item)
        return;

    AngleMatrix(QAngle(0.f, 0.f, 0.f), origin, item->transforms);
    item->mins = mins;
    item->maxs = maxs;
    item->r = r;
    item->g = g;
    item->b = b;
    item->a = a;
    item->noDepthTest = noDepthTest;
    item->SetEndTime(DebugOverlay_ReplicatedDuration(duration));
    item->m_pNextOverlay = *s_pOverlays;
    *s_pOverlays = item;
    s_liveOverlayCount++;
}

static void DebugOverlay_InsertSphere(const Vector3D& origin, const float radius, const int theta, const int phi,
    const int r, const int g, const int b, const int a, const bool noDepthTest, const float duration)
{
    if (!s_pOverlays || !s_OverlayMutex)
        return;

    AUTO_LOCK(*s_OverlayMutex);
    OverlaySphere_t* const item = new OverlaySphere_t;
    if (!item)
        return;

    item->vOrigin = origin;
    item->flRadius = radius;
    item->nTheta = DebugOverlay_ClampSphereSegs(theta);
    item->nPhi = DebugOverlay_ClampSphereSegs(phi);
    item->r = r;
    item->g = g;
    item->b = b;
    item->a = a;
    item->noDepthTest = noDepthTest;
    item->SetEndTime(DebugOverlay_ReplicatedDuration(duration));
    item->m_pNextOverlay = *s_pOverlays;
    *s_pOverlays = item;
    s_liveOverlayCount++;
}

static void DebugOverlay_InsertCapsule(const Vector3D& start, const Vector3D& end, const float radius,
    const int r, const int g, const int b, const int a, const bool noDepthTest, const float duration)
{
    if (!s_pOverlays || !s_OverlayMutex)
        return;

    AUTO_LOCK(*s_OverlayMutex);
    OverlayCapsule_t* const item = new OverlayCapsule_t;
    if (!item)
        return;

    item->start = start;
    item->end = end;
    item->radius = radius;
    item->r = r;
    item->g = g;
    item->b = b;
    item->a = a;
    item->noDepthTest = noDepthTest;
    item->SetEndTime(DebugOverlay_ReplicatedDuration(duration));
    item->m_pNextOverlay = *s_pOverlays;
    *s_pOverlays = item;
    s_liveOverlayCount++;
}

static void DebugOverlay_InsertTriangle(const Vector3D& p1, const Vector3D& p2, const Vector3D& p3,
    const int r, const int g, const int b, const int a, const bool noDepthTest, const float duration)
{
    if (!s_pOverlays || !s_OverlayMutex)
        return;

    AUTO_LOCK(*s_OverlayMutex);
    OverlayTriangle_t* const item = new OverlayTriangle_t;
    if (!item)
        return;

    item->p1 = p1;
    item->p2 = p2;
    item->p3 = p3;
    item->r = r;
    item->g = g;
    item->b = b;
    item->a = a;
    item->noDepthTest = noDepthTest;
    item->SetEndTime(DebugOverlay_ReplicatedDuration(duration));
    item->m_pNextOverlay = *s_pOverlays;
    *s_pOverlays = item;
    s_liveOverlayCount++;
}

void DebugOverlay_ApplyS2CPayload(const uint8_t* data, int nBytes)
{
    if (!data || nBytes < 1 || !s_pOverlays)
        return;

    // Re-arms once the list has actually drained, so a sender that floods
    // faster than the list decays keeps saying so instead of warning once and
    // then looking identical to a list that recovered.
    static bool s_warnedFull = false;

    if (s_liveOverlayCount >= kMaxLiveOverlays)
    {
        if (!s_warnedFull)
        {
            s_warnedFull = true;
            Warning(eDLL_T::CLIENT, "[DBGDRAW] overlay list at %d entries; dropping replicated batches until it decays\n",
                s_liveOverlayCount);
        }
        return;
    }

    if (s_warnedFull && s_liveOverlayCount < (kMaxLiveOverlays / 2))
    {
        s_warnedFull = false;
        Msg(eDLL_T::CLIENT, "[DBGDRAW] overlay list decayed to %d entries; accepting replicated batches again\n",
            s_liveOverlayCount);
    }

    DebugOverlay_ApplyDevDefaults();

    int off = 0;
    const uint8_t count = data[off++];
    if (count > kDebugOverlayS2CMax)
        return;

    const int need = 1 + static_cast<int>(count) * kDebugOverlayS2CItemBytes;
    if (nBytes < need)
        return;

    static bool s_logged = false;
    if (!s_logged)
    {
        s_logged = true;
        Msg(eDLL_T::CLIENT, "[DBGDRAW] applied server overlay batch (count=%u)\n", count);
    }

    for (uint8_t i = 0; i < count; i++)
    {
        // Per item, not just per batch: an oriented box expands to twelve lines
        // after the entry check, so one accepted batch could overshoot the cap.
        if (s_liveOverlayCount >= kMaxLiveOverlays)
            break;

        const uint8_t type = data[off++];
        Vector3D p0, p1, p2, p3;
        memcpy(&p0.x, data + off, 12); off += 12;
        memcpy(&p1.x, data + off, 12); off += 12;
        memcpy(&p2.x, data + off, 12); off += 12;
        memcpy(&p3.x, data + off, 12); off += 12;
        const int r = data[off++];
        const int g = data[off++];
        const int b = data[off++];
        const int a = data[off++];
        const bool noDepth = data[off++] != 0;
        float duration = 0.f;
        memcpy(&duration, data + off, 4); off += 4;

        switch (type)
        {
        case kS2CLine:
            DebugOverlay_InsertLine(p0, p1, r, g, b, a, noDepth, duration);
            break;
        case kS2CBox:
            DebugOverlay_InsertBox(p0, p1, p2, r, g, b, a, noDepth, duration);
            break;
        case kS2CSphere:
            DebugOverlay_InsertSphere(p0, p1.x, static_cast<int>(p1.y), static_cast<int>(p1.z), r, g, b, a, noDepth, duration);
            break;
        case kS2CCapsule:
            DebugOverlay_InsertCapsule(p0, p1, p2.x, r, g, b, a, noDepth, duration);
            break;
        case kS2CTriangle:
            DebugOverlay_InsertTriangle(p0, p1, p2, r, g, b, a, noDepth, duration);
            break;
        case kS2CTransformedBox:
            DebugOverlay_InsertTransformedBox(p0, QAngle(p1.x, p1.y, p1.z), p2, p3, r, g, b, a, noDepth, duration);
            break;
        default:
            break;
        }
    }
}
#endif // CLIENT_DLL
