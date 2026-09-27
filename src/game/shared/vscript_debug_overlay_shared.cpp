//=============================================================================//
// 
// Purpose: VScript debug overlay implementation
// 
//=============================================================================//
#include "engine/debugoverlay.h"
#include "vscript/vscript.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript_gamedll_defs.h"
#include "vscript_shared.h"
#include "vscript_debug_overlay_shared.h"

static bool Script_CheckDebugOverlay(HSQUIRRELVM v)
{
    if (g_pDebugOverlay)
        return true;
    v_SQVM_RaiseError(v, "debug overlay interface is not available");
    return false;
}

SQRESULT SharedScript_DebugDrawSolidBox(HSQUIRRELVM v)
{
    if (!Script_CheckDebugOverlay(v))
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    const SQVector3D* origin;
    const SQVector3D* mins;
    const SQVector3D* maxs;
    const SQVector3D* colorVec;
    SQFloat alpha;
    SQBool drawThroughWorld;
    SQFloat duration;

    if (SQ_FAILED(sq_getvector(v, 2, &origin))
        || SQ_FAILED(sq_getvector(v, 3, &mins))
        || SQ_FAILED(sq_getvector(v, 4, &maxs))
        || SQ_FAILED(sq_getvector(v, 5, &colorVec))
        || SQ_FAILED(sq_getfloat(v, 6, &alpha))
        || SQ_FAILED(sq_getbool(v, 7, &drawThroughWorld))
        || SQ_FAILED(sq_getfloat(v, 8, &duration)))
    {
        v_SQVM_RaiseError(v, "invalid arguments");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    const Color color = Script_VectorToColor(colorVec, alpha);
    g_pDebugOverlay->AddBoxOverlay(*(Vector3D*)origin, *(Vector3D*)mins, *(Vector3D*)maxs,
        color.r(), color.g(), color.b(), color.a(), drawThroughWorld, duration);

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT SharedScript_DebugDrawSweptBox(HSQUIRRELVM v)
{
    if (!Script_CheckDebugOverlay(v))
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    const SQVector3D* start;
    const SQVector3D* end;
    const SQVector3D* mins;
    const SQVector3D* maxs;
    const SQVector3D* angles;
    const SQVector3D* colorVec;
    SQFloat alpha;
    SQBool drawThroughWorld;
    SQFloat duration;

    if (SQ_FAILED(sq_getvector(v, 2, &start))
        || SQ_FAILED(sq_getvector(v, 3, &end))
        || SQ_FAILED(sq_getvector(v, 4, &mins))
        || SQ_FAILED(sq_getvector(v, 5, &maxs))
        || SQ_FAILED(sq_getvector(v, 6, &angles))
        || SQ_FAILED(sq_getvector(v, 7, &colorVec))
        || SQ_FAILED(sq_getfloat(v, 8, &alpha))
        || SQ_FAILED(sq_getbool(v, 9, &drawThroughWorld))
        || SQ_FAILED(sq_getfloat(v, 10, &duration)))
    {
        v_SQVM_RaiseError(v, "invalid arguments");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    const Color color = Script_VectorToColor(colorVec, alpha);
    g_pDebugOverlay->AddSweptBoxOverlay(*(Vector3D*)start, *(Vector3D*)end, *(Vector3D*)mins, *(Vector3D*)maxs,
        *(QAngle*)angles, color.r(), color.g(), color.b(), color.a(), drawThroughWorld, duration);

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT SharedScript_DebugDrawTriangle(HSQUIRRELVM v)
{
    if (!Script_CheckDebugOverlay(v))
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    const SQVector3D* p1;
    const SQVector3D* p2;
    const SQVector3D* p3;
    const SQVector3D* colorVec;
    SQFloat alpha;
    SQBool drawThroughWorld;
    SQFloat duration;

    if (SQ_FAILED(sq_getvector(v, 2, &p1))
        || SQ_FAILED(sq_getvector(v, 3, &p2))
        || SQ_FAILED(sq_getvector(v, 4, &p3))
        || SQ_FAILED(sq_getvector(v, 5, &colorVec))
        || SQ_FAILED(sq_getfloat(v, 6, &alpha))
        || SQ_FAILED(sq_getbool(v, 7, &drawThroughWorld))
        || SQ_FAILED(sq_getfloat(v, 8, &duration)))
    {
        v_SQVM_RaiseError(v, "invalid arguments");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    const Color color = Script_VectorToColor(colorVec, alpha);
    g_pDebugOverlay->AddTriangleOverlay(*(Vector3D*)p1, *(Vector3D*)p2, *(Vector3D*)p3,
        color.r(), color.g(), color.b(), color.a(), drawThroughWorld, duration);

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT SharedScript_DebugDrawSolidSphere(HSQUIRRELVM v)
{
    if (!Script_CheckDebugOverlay(v))
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    const SQVector3D* origin;
    SQFloat radius;
    SQInteger theta;
    SQInteger phi;
    const SQVector3D* colorVec;
    SQFloat alpha;
    SQBool drawThroughWorld;
    SQFloat duration;

    if (SQ_FAILED(sq_getvector(v, 2, &origin))
        || SQ_FAILED(sq_getfloat(v, 3, &radius))
        || SQ_FAILED(sq_getinteger(v, 4, &theta))
        || SQ_FAILED(sq_getinteger(v, 5, &phi))
        || SQ_FAILED(sq_getvector(v, 6, &colorVec))
        || SQ_FAILED(sq_getfloat(v, 7, &alpha))
        || SQ_FAILED(sq_getbool(v, 8, &drawThroughWorld))
        || SQ_FAILED(sq_getfloat(v, 9, &duration)))
    {
        v_SQVM_RaiseError(v, "invalid arguments");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    const Color color = Script_VectorToColor(colorVec, alpha);
    g_pDebugOverlay->AddSphereOverlay(*(Vector3D*)origin, radius, theta, phi,
        color.r(), color.g(), color.b(), color.a(), drawThroughWorld, duration);

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT SharedScript_DebugDrawCapsule(HSQUIRRELVM v)
{
    if (!Script_CheckDebugOverlay(v))
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    const SQVector3D* start;
    const SQVector3D* end;
    SQFloat radius;
    const SQVector3D* colorVec;
    SQFloat alpha;
    SQBool drawThroughWorld;
    SQFloat duration;

    if (SQ_FAILED(sq_getvector(v, 2, &start))
        || SQ_FAILED(sq_getvector(v, 3, &end))
        || SQ_FAILED(sq_getfloat(v, 4, &radius))
        || SQ_FAILED(sq_getvector(v, 5, &colorVec))
        || SQ_FAILED(sq_getfloat(v, 6, &alpha))
        || SQ_FAILED(sq_getbool(v, 7, &drawThroughWorld))
        || SQ_FAILED(sq_getfloat(v, 8, &duration)))
    {
        v_SQVM_RaiseError(v, "invalid arguments");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    const Color color = Script_VectorToColor(colorVec, alpha);
    g_pDebugOverlay->AddCapsuleOverlay(*(Vector3D*)start, *(Vector3D*)end, radius,
        color.r(), color.g(), color.b(), color.a(), drawThroughWorld, duration);

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: create a permanent box for map making
//-----------------------------------------------------------------------------

SQRESULT SharedScript_CreateBox(HSQUIRRELVM v)
{
    if (!Script_CheckDebugOverlay(v))
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    const SQVector3D* origin;
    const SQVector3D* angles;
    const SQVector3D* mins;
    const SQVector3D* maxs;
    const SQVector3D* colorVec;
    SQFloat alpha;

    if (SQ_FAILED(sq_getvector(v, 2, &origin))
        || SQ_FAILED(sq_getvector(v, 3, &angles))
        || SQ_FAILED(sq_getvector(v, 4, &mins))
        || SQ_FAILED(sq_getvector(v, 5, &maxs))
        || SQ_FAILED(sq_getvector(v, 6, &colorVec))
        || SQ_FAILED(sq_getfloat(v, 7, &alpha)))
    {
        v_SQVM_RaiseError(v, "invalid arguments");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    Vector3D vOrigin(origin->x, origin->y, origin->z);
    QAngle qAngles(angles->x, angles->y, angles->z);
    Vector3D vMins(mins->x, mins->y, mins->z);
    Vector3D vMaxs(maxs->x, maxs->y, maxs->z);
    Color color((int)(colorVec->x), (int)(colorVec->y), (int)(colorVec->z), (int)(alpha));

    matrix3x4_t transform;
    AngleMatrix(qAngles, vOrigin, transform);

    g_pDebugOverlay->AddTransformedBoxOverlay(transform, vMins, vMaxs,
        color.r(), color.g(), color.b(), color.a(),
        false, 999999999.0f);

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: clear all debug overlays and boxes
//-----------------------------------------------------------------------------

SQRESULT SharedScript_ClearBoxes(HSQUIRRELVM v)
{
    if (!Script_CheckDebugOverlay(v))
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    g_pDebugOverlay->ClearAllOverlays();
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}
