//=============================================================================//
//
// Purpose: object-placement / weapon-class natives
//
//=============================================================================//

#include "core/stdafx.h"
#include "common/callback.h"
#include "game/shared/scriptnetdata_limits.h"
#include "engine/server/server.h"
#include "engine/server/sv_main.h"
#include "engine/host_state.h"
#include "engine/debugoverlay.h"
#include "pluginsystem/pluginsystem.h"
#include "vscript/vscript.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"

#include "game/shared/vscript_gamedll_defs.h"
#include "game/shared/globalnonrewind_vars.h"
#include "game/shared/weapon_heat.h"
#include "game/server/energize.h"
#include "game/shared/deathfield_system.h"
#include "game/shared/alliance_compat.h"
#include "game/shared/highlight_context.h"
#include "game/shared/dt_extend.h"
#include "game/shared/player_extend_sidecar.h"
#include "game/shared/edict_dirty.h"

#include "game/shared/vscript_shared.h"
#include "game/shared/vscript_debug_overlay_shared.h"

#include "vscript_server.h"
#include "vscript_server_natives.h"
#include "vscript_server_placement.h"
#include "player.h"
#include "util_server.h"
#include "entitylist.h"
#include "game/shared/sdk_entity_state.h"
#include "detour_impl.h"
#include "game/shared/weapon_script_vars.h"
#include "game/server/jetdrive.h"
#include "game/server/trigger_updraft.h"
#include "game/server/skydive.h"
#include "game/server/player_overheat.h"
#include "game/server/translocation.h"
#include "game/shared/status_effects_sdk.h"
#include "game/shared/util_shared.h"
#include "game/client/vscript_player.h"
#include "game/shared/vscript_remotefunctions_sdk.h"
#include "engine/enginetrace.h"
#include "engine/modelloader.h"
#include "engine/server/precache_natives.h"
#include "public/bspflags.h"
#include "tier1/keyvalues.h"
#include "tier1/convar.h"
#include "tier1/cvar.h"
#include "tier2/curlutils.h"
#include "ebisusdk/EbisuSDK.h"
#include "game/server/sound.h"
#include "vscript/languages/squirrel_re/include/sqarray.h"

#include <atomic>
#include <cfloat>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

using SQNativeFn = SQRESULT (*)(HSQUIRRELVM);
static SQNativeFn s_s3CreatePlayerDecoyNative = nullptr;

static SQRESULT ServerScript_CreatePlayerDecoyS21Shim(HSQUIRRELVM v)
{
    if (!s_s3CreatePlayerDecoyNative)
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    const SQInteger oldTop = sq_gettop(v);
    if (oldTop > 7)
        sq_settop(v, 7);

    const SQRESULT result = s_s3CreatePlayerDecoyNative(v);

    // The S3 native returns its entity on the VM stack. Do not restore the
    // removed S21-only args over that return value.
    return result;
}

void Script_UpdateDedicatedPlayerDecoySignature()
{
    if (!ServerScript_IsDedicatedRuntime() || !g_serverScriptPlayerStruct)
        return;

    const char* const s21Params =
        "asset settingsName, asset modelName, int skin, int camo, "
        "float stickPercentToRun, bool hasMovement, bool useEyeAngles, "
        "vector initialVelocity";

    bool hasS21Overload = false;
    ScriptFunctionBindingStorageType_t nativeFn = nullptr;
    for (int i = 0; i < g_serverScriptPlayerStruct->m_StrTypedFunctions.Count(); ++i)
    {
        ScriptFunctionBinding_t& binding = g_serverScriptPlayerStruct->m_StrTypedFunctions[i];
        const char* scriptName = binding.m_Descriptor.m_ScriptName;
        if (scriptName && strcmp(scriptName, "CreatePlayerDecoy") == 0)
        {
            nativeFn = binding.m_pFunction;
            const char* params = binding.m_Descriptor.m_Parameters;
            if (params && strcmp(params, s21Params) == 0)
                hasS21Overload = true;
        }
    }

    if (!nativeFn || hasS21Overload)
        return;

    s_s3CreatePlayerDecoyNative = reinterpret_cast<SQNativeFn>(nativeFn);

    g_serverScriptPlayerStruct->AddFunction(
        "CreatePlayerDecoy",
        "Script_CreatePlayerDecoyS21Shim",
        "Creates a decoy of this player",
        "entity",
        s21Params,
        false,
        reinterpret_cast<ScriptFunctionBindingStorageType_t>(
            &ServerScript_CreatePlayerDecoyS21Shim));
}

static constexpr int SERVER_WEAPON_OWNER_HANDLE_OFFSET = 0x11F0;
static constexpr int SERVER_WEAPON_NAME_OFFSET = 0x15B0;
static constexpr int SERVER_WEAPON_SCRIPT_ACTIVATED_OFFSET = 0x1585; // CWeaponX m_scriptActivated
static constexpr ptrdiff_t PLAYER_OFF_OFFHAND_WEAPONS = 0x16B4; // m_inventory.offhandWeapons[0]
static constexpr ptrdiff_t PLAYER_OFF_ACTIVE_WEAPONS = 0x16CC; // m_inventory.activeWeapons[0]
static constexpr int PLAYER_OFFHAND_WEAPON_COUNT = 6;
static constexpr int PLAYER_ACTIVE_WEAPON_COUNT = 3;
static constexpr ptrdiff_t WEAPON_OFF_WEAPSTATE = 0x1234;
static constexpr unsigned int WEAP_STATE_HOLSTERED = 2;

struct ServerObjectPlacementState
{
    bool valid = false;
    bool special = false;
    Vector3D origin;
    Vector3D angles;
    Vector3D specialOrigin;
    Vector3D specialAngles;
    void* parent = nullptr;
    void* specialParent = nullptr;
    int specialResult = 1;
};

struct ServerPlacementCache
{
    ServerObjectPlacementState state;
    float curTime = -1.0f;
    bool useSpecial = false;
    Vector3D eyeOrigin;
    QAngle eyeAngles;

    // Cached parent-relative pose reused when a fresh compute fails and the eye
    // has not moved/rotated much. Parent is a raw EHANDLE index, not a pointer.
    bool hasLastGood = false;
    Vector3D lastGoodLocalOrigin;
    Vector3D lastGoodLocalAngles;
    Vector3D lastGoodEyeOrigin;
    Vector3D lastGoodEyeDir;
    uint32_t lastGoodParentHandle = INVALID_EHANDLE_INDEX;
    float lastGoodParentYaw = 0.0f; // parent's abs yaw at cache-store time, for rotation composition on reuse.
    Vector3D lastGoodParentOrigin;
    Vector3D lastGoodParentAngles;

    // Special (exit) pose, independent of the entrance last-good above.
    Vector3D lastGoodLocalSpecialOrigin;
    Vector3D lastGoodLocalSpecialAngles;
    uint32_t lastGoodSpecialParentHandle = INVALID_EHANDLE_INDEX;
    float lastGoodSpecialParentYaw = 0.0f;
    Vector3D lastGoodSpecialParentOrigin;
    Vector3D lastGoodSpecialParentAngles;

    int cmdNumber = 0;
    bool hasValidSpot = false;
    bool isLastKnownGood = false;
};

struct ServerObjectPlacementSettings
{
    bool loaded = false;
    bool objectPlacer = false;
    std::string modelName;
    float distanceMax = 128.0f;
    // True once object_placement_distance_max was read from weapon.txt.
    // distanceMax alone cannot tell unset from an explicit default.
    bool distanceMaxSet = false;
    float hillAngleMax = 45.0f;
    float clearanceBehind = 0.0f;
    float dropToGroundOffsetMax = 0.0f;
    float distanceToGroundMax = 20.0f;
    float groundPenetrationMax = 10.0f;
    float percentOffLedgeMax = 0.25f;
    float topSidePercentPierceMax = 0.34f;
    float topDistancePierceMax = 7.5f;
    bool forceUpright = false;
    bool ignorePlayers = false;
    bool useTopTrace = false;
    bool traceThroughTurrets = false;
    bool modelRegistered = false;
    bool hasModelBounds = false;
    Vector3D modelMins = Vector3D(-5.0f, -5.0f, -5.0f);
    Vector3D modelMaxs = Vector3D(5.0f, 5.0f, 5.0f);

    // Mirrors ConVar object_placement_special_allow_on_movers (default 1).
    bool allowOnMovers = true;
    // Mirrors ConVar object_placement_special_mover_blocker_validation (default on).
    bool moverBlockerValidation = true;
    // lastGoodDistanceMax in units (0 = disabled). lastGoodAngleMax is a
    // cosine threshold, not degrees.
    float lastGoodDistanceMax = 0.0f;
    float lastGoodAngleMax = 1.0f;
};

static SDKEntityMap<ServerPlacementCache> s_serverPlacementCache(ESide::Server, "alter.placement");

static ConVar sv_alter_portal_pred_store("sv_alter_portal_pred_store", "1", FCVAR_RELEASE,
    "Store object placement each hold cmd and serve GetObjectPlacement* from that store. 0 = toss-time recompute.");
static ConVar sv_alter_placement_use_cmd_viewangles("sv_alter_placement_use_cmd_viewangles", "0", FCVAR_DEVELOPMENTONLY,
    "Seed placement from usercmd viewangles instead of EyeAngles.");
static ConVar sdk_alter_pred_cadence_diag("sdk_alter_pred_cadence_diag", "0", FCVAR_DEVELOPMENTONLY,
    "Log hold-store vs toss-get origin/angles/cmd (first 8 then every 32).");
static ConVar sv_alter_placement_eye_diag("sv_alter_placement_eye_diag", "0", FCVAR_DEVELOPMENTONLY,
    "Log cmd viewangles vs EyeAngles/EyePosition at store and calc.");
static ConVar sv_alter_toss_store_diag("sv_alter_toss_store_diag", "0", FCVAR_DEVELOPMENTONLY,
    "Log cmd, hasValidSpot, isLKG, origin, specialOrigin on store vs get.");

static void ServerScript_PlacementCallbackCacheReset(void);

void ServerScript_PlacementLevelShutdown(void)
{
	s_serverPlacementCache.Clear();
	ServerScript_PlacementCallbackCacheReset();
}
static std::unordered_map<std::string, ServerObjectPlacementSettings> s_serverPlacementSettings;
static bool s_serverPlacementPrecacheScanned = false;
static int s_serverPlacementScanCount = 0;
static int s_serverPlacementDiagBudget = 20000;
// Separate from the general diag budget so a shallow-exit Warning cannot be
// silenced by probe spam (or silence the probes itself).
static int s_serverPlacementShallowLogBudget = 200;

static void ServerScript_PlacementDiag(const char* stage, void* pWeapon, CPlayer* owner = nullptr)
{
    if (s_serverPlacementDiagBudget <= 0)
        return;

    --s_serverPlacementDiagBudget;
    const uint32_t ownerHandle = pWeapon
        ? *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(pWeapon) + SERVER_WEAPON_OWNER_HANDLE_OFFSET)
        : INVALID_EHANDLE_INDEX;
    const char* weaponName = pWeapon
        ? reinterpret_cast<const char*>(reinterpret_cast<uintptr_t>(pWeapon) + SERVER_WEAPON_NAME_OFFSET)
        : "<null>";
    Msg(eDLL_T::SERVER,
        "[OPL-SRV] %s weapon=%p ownerHandle=0x%08X owner=%p name='%s' dedicated=%d g_pEngineTraceServer=%p\n",
        stage,
        pWeapon,
        ownerHandle,
        owner,
        weaponName ? weaponName : "<null>",
        ServerScript_IsDedicatedRuntime() ? 1 : 0,
        g_pEngineTraceServer);
}

static void* ServerScript_LookupEntityFromRawHandle(const uint32_t rawHandle)
{
    if (rawHandle == INVALID_EHANDLE_INDEX || !g_serverEntityList)
        return nullptr;

    const CBaseHandle handle = CBaseHandle::UnsafeFromIndex(static_cast<int>(rawHandle));
    if (void* pEntity = g_serverEntityList->LookupEntity(handle))
        return pEntity;

    const int entIndex = static_cast<int>(rawHandle & ENT_ENTRY_MASK);
    if (entIndex >= 0 && entIndex < NUM_ENT_ENTRIES)
        return g_serverEntityList->LookupEntityByNetworkIndex(entIndex);

    return nullptr;
}

static Vector3D ServerScript_PlayerEyeOrigin(CPlayer* player)
{
    Vector3D eyeOrigin;
    player->EyePosition(&eyeOrigin);
    return eyeOrigin;
}

static QAngle ServerScript_PlayerPlacementEyeAngles(CPlayer* player)
{
    QAngle eyeAngles(0.0f, 0.0f, 0.0f);
    if (!player)
        return eyeAngles;

    // cmd viewangles are local; EyeAngles LocalToWorlds m_localViewAngles.
    player->EyeAngles(&eyeAngles);
    if (!isfinite(eyeAngles.x) || !isfinite(eyeAngles.y) || !isfinite(eyeAngles.z))
        eyeAngles = QAngle(0.0f, 0.0f, 0.0f);

    if (sv_alter_placement_use_cmd_viewangles.GetBool())
    {
        const CUserCmd* const cmd = player->GetPlacementUserCommand();
        if (cmd &&
            isfinite(cmd->viewangles.x) &&
            isfinite(cmd->viewangles.y) &&
            isfinite(cmd->viewangles.z))
        {
            return cmd->viewangles;
        }
    }

    return eyeAngles;
}

static CPlayer* ServerScript_GetWeaponOwnerPlayer(void* pWeapon)
{
    if (!pWeapon)
        return nullptr;

    const uint32_t rawHandle = *reinterpret_cast<uint32_t*>(
        reinterpret_cast<uintptr_t>(pWeapon) + SERVER_WEAPON_OWNER_HANDLE_OFFSET);
    return reinterpret_cast<CPlayer*>(ServerScript_LookupEntityFromRawHandle(rawHandle));
}

static bool ServerScript_IsFiniteVector(const Vector3D& value)
{
    return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}

static bool ServerScript_IsFiniteQAngle(const QAngle& value)
{
    return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}

static bool ServerScript_PlacementRateLimit(int& counter)
{
    ++counter;
    return counter <= 8 || (counter % 32) == 0;
}

static void ServerScript_PlacementEyeDiag(CPlayer* player, const Vector3D& eyeOrigin, const QAngle& eyeAngles)
{
    if (!player)
        return;

    static bool s_loggedEyeSeed = false;
    if (!s_loggedEyeSeed)
    {
        s_loggedEyeSeed = true;
        Warning(eDLL_T::SERVER,
            "[OPL-SRV] placement eye seed first call useCmdView=%d hasCurrent=%d hasLast=%d\n",
            sv_alter_placement_use_cmd_viewangles.GetBool() ? 1 : 0,
            player->HasCurrentUserCommand() ? 1 : 0,
            player->HasLastUserCommand() ? 1 : 0);
    }

    if (!sv_alter_placement_eye_diag.GetBool())
        return;

    static int s_nEyeDiag = 0;
    if (!ServerScript_PlacementRateLimit(s_nEyeDiag))
        return;

    const CUserCmd* const cmd = player->GetPlacementUserCommand();
    const int cmdNumber = cmd ? cmd->command_number : 0;
    QAngle cmdView(0.0f, 0.0f, 0.0f);
    if (cmd)
        cmdView = cmd->viewangles;

    const Vector3D absOrigin = player->Diag_AbsOrigin();
    Warning(eDLL_T::SERVER,
        "[OPL-EYE] hasCurrent=%d hasLast=%d cmd=%d cmdView=<%.2f %.2f %.2f> eyeAngles=<%.2f %.2f %.2f> eyeOrigin=<%.1f %.1f %.1f> dAng=<%.2f %.2f %.2f> dPos=<%.1f %.1f %.1f>\n",
        player->HasCurrentUserCommand() ? 1 : 0,
        player->HasLastUserCommand() ? 1 : 0,
        cmdNumber,
        cmdView.x, cmdView.y, cmdView.z,
        eyeAngles.x, eyeAngles.y, eyeAngles.z,
        eyeOrigin.x, eyeOrigin.y, eyeOrigin.z,
        cmdView.x - eyeAngles.x, cmdView.y - eyeAngles.y, cmdView.z - eyeAngles.z,
        eyeOrigin.x - absOrigin.x, eyeOrigin.y - absOrigin.y, eyeOrigin.z - absOrigin.z);
}

static void ServerScript_PlacementCadenceDiag(const char* tag, void* pWeapon, int cmdNumber, const ServerPlacementCache& cache)
{
    static int s_nCadenceDiag = 0;
    const bool cadence = sdk_alter_pred_cadence_diag.GetBool();
    const bool tossStore = sv_alter_toss_store_diag.GetBool();
    if (!cadence && !tossStore)
        return;
    if (!ServerScript_PlacementRateLimit(s_nCadenceDiag))
        return;

    if (cadence)
    {
        Warning(eDLL_T::SERVER,
            "[OPL-SRV] %s weapon=%p cmd=%d origin=<%.1f %.1f %.1f> angles=<%.1f %.1f %.1f> special=<%.1f %.1f %.1f> valid=%d\n",
            tag,
            pWeapon,
            cmdNumber,
            cache.state.origin.x, cache.state.origin.y, cache.state.origin.z,
            cache.state.angles.x, cache.state.angles.y, cache.state.angles.z,
            cache.state.specialOrigin.x, cache.state.specialOrigin.y, cache.state.specialOrigin.z,
            cache.state.valid ? 1 : 0);
    }

    if (tossStore)
    {
        Warning(eDLL_T::SERVER,
            "[OPL-SRV] %s cmd=%d hasValidSpot=%d isLKG=%d origin=<%.1f %.1f %.1f> specialOrigin=<%.1f %.1f %.1f>\n",
            tag,
            cmdNumber,
            cache.hasValidSpot ? 1 : 0,
            cache.isLastKnownGood ? 1 : 0,
            cache.state.origin.x, cache.state.origin.y, cache.state.origin.z,
            cache.state.specialOrigin.x, cache.state.specialOrigin.y, cache.state.specialOrigin.z);
    }
}

// KV is degrees; stored as cos(radians).
static float ServerScript_ConvertLastGoodAngleMaxDegreesToDot(const float angleDegrees)
{
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    return cosf(angleDegrees * kDegToRad);
}

static float ServerScript_Dot(const Vector3D& a, const Vector3D& b)
{
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

static Vector3D ServerScript_AddScaled(const Vector3D& base, const Vector3D& dir, const float scale)
{
    return Vector3D(base.x + dir.x * scale, base.y + dir.y * scale, base.z + dir.z * scale);
}

static Vector3D ServerScript_AddVector(const Vector3D& a, const Vector3D& b)
{
    return Vector3D(a.x + b.x, a.y + b.y, a.z + b.z);
}

static Vector3D ServerScript_SubVector(const Vector3D& a, const Vector3D& b)
{
    return Vector3D(a.x - b.x, a.y - b.y, a.z - b.z);
}

static Vector3D ServerScript_ScaleVector(const Vector3D& value, const float scale)
{
    return Vector3D(value.x * scale, value.y * scale, value.z * scale);
}

static Vector3D ServerScript_Normalized(Vector3D value)
{
    const float lenSqr = ServerScript_Dot(value, value);
    if (lenSqr <= 0.000001f)
        return Vector3D(0.0f, 0.0f, 1.0f);

    const float invLen = 1.0f / sqrtf(lenSqr);
    return Vector3D(value.x * invLen, value.y * invLen, value.z * invLen);
}

static void* ServerScript_PlacementParentFromTrace(const trace_t& tr)
{
    void* const parent = tr.hit_entity;
    if (!parent)
        return nullptr;

    if (g_serverEntityList &&
        parent == g_serverEntityList->LookupEntityByNetworkIndex(0))
    {
        return nullptr;
    }

    return parent;
}

static bool ServerScript_LoadPlacementModelBounds(ServerObjectPlacementSettings& settings)
{
    if (settings.modelName.empty() || !g_pModelLoader || !CModelLoader__FindModel)
        return false;

    model_t* model = nullptr;
    __try
    {
        model = reinterpret_cast<model_t*>(
            CModelLoader__FindModel(g_pModelLoader, settings.modelName.c_str()));
        if (model && CModelLoader__LoadModel &&
            !(model->nLoadFlags & IModelLoader::FMODELLOADER_LOADED))
        {
            CModelLoader__LoadModel(g_pModelLoader, model);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Warning(eDLL_T::SERVER,
            "[OPL-SRV] object placement model load fault for '%s'\n",
            settings.modelName.c_str());
        return false;
    }

    if (!model ||
        !ServerScript_IsFiniteVector(model->mins) ||
        !ServerScript_IsFiniteVector(model->maxs))
    {
        return false;
    }

    const Vector3D size = ServerScript_SubVector(model->maxs, model->mins);
    if (size.x <= 0.0f || size.y <= 0.0f || size.z <= 0.0f)
        return false;

    settings.modelMins = model->mins;
    settings.modelMaxs = model->maxs;
    settings.hasModelBounds = true;
    return true;
}

static KeyValues* ServerScript_FindKeyRecursive(KeyValues* kv, const char* keyName)
{
    if (!kv || !keyName || !*keyName)
        return nullptr;

    if (KeyValues* const direct = kv->FindKey(keyName, false))
        return direct;

    for (KeyValues* subKey = kv->GetFirstSubKey(); subKey; subKey = subKey->GetNextKey())
    {
        if (KeyValues* const found = ServerScript_FindKeyRecursive(subKey, keyName))
            return found;
    }

    return nullptr;
}

static KeyValues* ServerScript_FindWeaponDataBlock(KeyValues* kv)
{
    if (!kv)
        return nullptr;

    const char* const name = kv->GetName();
    if (name && _stricmp(name, "WeaponData") == 0)
        return kv;

    if (KeyValues* const direct = kv->FindKey("WeaponData", false))
        return direct;

    for (KeyValues* subKey = kv->GetFirstSubKey(); subKey; subKey = subKey->GetNextKey())
    {
        if (KeyValues* const found = ServerScript_FindWeaponDataBlock(subKey))
            return found;
    }

    return kv;
}

static bool ServerScript_ParseQuotedKeyValueLine(
    const std::string& line,
    std::string& key,
    std::string& value)
{
    const size_t keyOpen = line.find('"');
    if (keyOpen == std::string::npos)
        return false;

    const size_t keyClose = line.find('"', keyOpen + 1);
    if (keyClose == std::string::npos)
        return false;

    const size_t valueOpen = line.find('"', keyClose + 1);
    if (valueOpen == std::string::npos)
        return false;

    const size_t valueClose = line.find('"', valueOpen + 1);
    if (valueClose == std::string::npos)
        return false;

    key.assign(line, keyOpen + 1, keyClose - keyOpen - 1);
    value.assign(line, valueOpen + 1, valueClose - valueOpen - 1);
    return !key.empty();
}

static bool ServerScript_ApplyPlacementKeyValue(
    ServerObjectPlacementSettings& settings,
    const char* key,
    const char* value)
{
    if (!key || !value)
        return false;

    if (_stricmp(key, "object_placer") == 0)
    {
        settings.objectPlacer = atoi(value) != 0;
        return true;
    }

    if (_stricmp(key, "object_placement_model") == 0)
    {
        settings.modelName = value;
        return true;
    }

    if (_stricmp(key, "object_placement_distance_max") == 0)
    {
        settings.distanceMax = static_cast<float>(atof(value));
        settings.distanceMaxSet = true;
    }
    else if (_stricmp(key, "object_placement_hill_angle_max") == 0)
        settings.hillAngleMax = static_cast<float>(atof(value));
    else if (_stricmp(key, "object_placement_clearance_behind") == 0)
        settings.clearanceBehind = static_cast<float>(atof(value));
    else if (_stricmp(key, "object_placement_drop_to_ground_offset_max") == 0)
        settings.dropToGroundOffsetMax = static_cast<float>(atof(value));
    else if (_stricmp(key, "object_placement_distance_to_ground_max") == 0)
        settings.distanceToGroundMax = static_cast<float>(atof(value));
    else if (_stricmp(key, "object_placement_ground_penetration_max") == 0)
        settings.groundPenetrationMax = static_cast<float>(atof(value));
    else if (_stricmp(key, "object_placement_percent_off_ledge_max") == 0)
        settings.percentOffLedgeMax = static_cast<float>(atof(value));
    else if (_stricmp(key, "object_placement_top_side_percent_pierce_max") == 0)
        settings.topSidePercentPierceMax = static_cast<float>(atof(value));
    else if (_stricmp(key, "object_placement_top_distance_pierce_max") == 0)
        settings.topDistancePierceMax = static_cast<float>(atof(value));
    else if (_stricmp(key, "object_placement_force_upright") == 0)
        settings.forceUpright = atoi(value) != 0;
    else if (_stricmp(key, "object_placement_ignore_players") == 0)
        settings.ignorePlayers = atoi(value) != 0;
    else if (_stricmp(key, "object_placement_use_top_trace") == 0)
        settings.useTopTrace = atoi(value) != 0;
    else if (_stricmp(key, "object_placement_trace_through_turrets") == 0)
        settings.traceThroughTurrets = atoi(value) != 0;
    else if (_stricmp(key, "object_placement_special_allow_on_movers") == 0)
        settings.allowOnMovers = atoi(value) != 0;
    else if (_stricmp(key, "object_placement_special_mover_blocker_validation") == 0)
        settings.moverBlockerValidation = atoi(value) != 0;
    else if (_stricmp(key, "object_placement_last_good_distance_max") == 0)
        settings.lastGoodDistanceMax = static_cast<float>(atof(value));
    else if (_stricmp(key, "object_placement_last_good_angle_max") == 0)
    {
        // KV is degrees; stored as a cosine threshold.
        settings.lastGoodAngleMax = ServerScript_ConvertLastGoodAngleMaxDegreesToDot(
            static_cast<float>(atof(value)));
    }
    else
        return false;

    return true;
}

static bool ServerScript_ReadPlacementSettingsFromDisk(
    const char* weaponName,
    ServerObjectPlacementSettings& settings)
{
    char diskPath[MAX_PATH];
    V_snprintf(diskPath, sizeof(diskPath), "platform\\scripts\\weapons\\%s.txt", weaponName);

    std::ifstream file(diskPath);
    if (!file.is_open())
        return false;

    bool readAny = false;
    std::string line;
    std::string key;
    std::string value;
    while (std::getline(file, line))
    {
        if (!ServerScript_ParseQuotedKeyValueLine(line, key, value))
            continue;

        if (ServerScript_ApplyPlacementKeyValue(settings, key.c_str(), value.c_str()))
            readAny = true;
    }

    return readAny;
}

static bool ServerScript_ReadPlacementSettingsForWeaponName(
    const char* weaponName,
    ServerObjectPlacementSettings& settings)
{
    if (!weaponName || !*weaponName)
        return false;

    settings.loaded = true;
    bool readAny = false;

    char path[MAX_PATH];
    V_snprintf(path, sizeof(path), "scripts/weapons/%s.txt", weaponName);

    KeyValues* kv = new KeyValues("WeaponData");
    const bool loaded = kv->LoadFromFile(FileSystem(), path, "GAME");
    if (loaded)
    {
        KeyValues* const weaponData = ServerScript_FindWeaponDataBlock(kv);
        if (weaponData)
        {
            settings.objectPlacer = weaponData->GetBool("object_placer", settings.objectPlacer);
            settings.modelName = weaponData->GetString("object_placement_model", settings.modelName.c_str());
            settings.distanceMax = weaponData->GetFloat("object_placement_distance_max", settings.distanceMax);
            settings.hillAngleMax = weaponData->GetFloat("object_placement_hill_angle_max", settings.hillAngleMax);
            settings.clearanceBehind = weaponData->GetFloat("object_placement_clearance_behind", settings.clearanceBehind);
            settings.dropToGroundOffsetMax = weaponData->GetFloat("object_placement_drop_to_ground_offset_max", settings.dropToGroundOffsetMax);
            settings.distanceToGroundMax = weaponData->GetFloat("object_placement_distance_to_ground_max", settings.distanceToGroundMax);
            settings.groundPenetrationMax = weaponData->GetFloat("object_placement_ground_penetration_max", settings.groundPenetrationMax);
            settings.percentOffLedgeMax = weaponData->GetFloat("object_placement_percent_off_ledge_max", settings.percentOffLedgeMax);
            settings.topSidePercentPierceMax = weaponData->GetFloat("object_placement_top_side_percent_pierce_max", settings.topSidePercentPierceMax);
            settings.topDistancePierceMax = weaponData->GetFloat("object_placement_top_distance_pierce_max", settings.topDistancePierceMax);
            settings.forceUpright = weaponData->GetBool("object_placement_force_upright", settings.forceUpright);
            settings.ignorePlayers = weaponData->GetBool("object_placement_ignore_players", settings.ignorePlayers);
            settings.useTopTrace = weaponData->GetBool("object_placement_use_top_trace", settings.useTopTrace);
            settings.traceThroughTurrets = weaponData->GetBool("object_placement_trace_through_turrets", settings.traceThroughTurrets);
            settings.allowOnMovers = weaponData->GetBool("object_placement_special_allow_on_movers", settings.allowOnMovers);
            settings.moverBlockerValidation = weaponData->GetBool("object_placement_special_mover_blocker_validation", settings.moverBlockerValidation);
            settings.lastGoodDistanceMax = weaponData->GetFloat("object_placement_last_good_distance_max", settings.lastGoodDistanceMax);
            // KV is degrees. Convert only when the key is present; the default
            // 1.0 is already a cosine and must not be re-converted.
            if (weaponData->FindKey("object_placement_last_good_angle_max", false))
            {
                settings.lastGoodAngleMax = ServerScript_ConvertLastGoodAngleMaxDegreesToDot(
                    weaponData->GetFloat("object_placement_last_good_angle_max", 0.0f));
            }
            if (weaponData->FindKey("object_placement_distance_max", false))
                settings.distanceMaxSet = true;

            readAny = settings.objectPlacer || !settings.modelName.empty();
        }

        if (settings.modelName.empty())
        {
            if (KeyValues* const modelKey = ServerScript_FindKeyRecursive(kv, "object_placement_model"))
            {
                settings.modelName = modelKey->GetString();
                readAny = true;
            }
        }
    }

    kv->DeleteThis();

    if (!readAny || settings.modelName.empty())
        readAny = ServerScript_ReadPlacementSettingsFromDisk(weaponName, settings) || readAny;

    return readAny;
}

static bool ServerScript_FinalizePlacementModel(ServerObjectPlacementSettings& settings)
{
    if (settings.modelName.empty())
        return false;

    if (!settings.modelRegistered)
    {
        PrecacheModel_RegisterReplicated(settings.modelName.c_str());
        if (!PrecacheModel_IsModelPrecacheReady())
            return false;
        settings.modelRegistered = true;
    }

    if (!settings.hasModelBounds)
        ServerScript_LoadPlacementModelBounds(settings);

    return true;
}

static void ServerScript_PrecacheObjectPlacementModels()
{
    if (s_serverPlacementPrecacheScanned)
        return;

    s_serverPlacementPrecacheScanned = true;

    WIN32_FIND_DATAA findData;
    HANDLE findHandle = FindFirstFileA("platform\\scripts\\weapons\\*.txt", &findData);
    if (findHandle == INVALID_HANDLE_VALUE)
    {
        if (s_serverPlacementDiagBudget > 0)
        {
            Warning(eDLL_T::SERVER,
                "[OPL-SRV] object_placement_model scan skipped: platform/scripts/weapons/*.txt not found\n");
            --s_serverPlacementDiagBudget;
        }
        return;
    }

    int scanned = 0;
    int registered = 0;
    do
    {
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        std::string weaponName(findData.cFileName);
        constexpr size_t txtSuffixLen = 4;
        if (weaponName.size() <= txtSuffixLen ||
            _stricmp(weaponName.c_str() + weaponName.size() - txtSuffixLen, ".txt") != 0)
        {
            continue;
        }

        weaponName.resize(weaponName.size() - txtSuffixLen);
        ++scanned;

        if (s_serverPlacementSettings.find(weaponName) != s_serverPlacementSettings.end())
            continue;

        ServerObjectPlacementSettings settings;
        if (!ServerScript_ReadPlacementSettingsForWeaponName(weaponName.c_str(), settings) ||
            settings.modelName.empty())
        {
            continue;
        }

        ServerScript_FinalizePlacementModel(settings);
        s_serverPlacementSettings.emplace(weaponName, settings);
        ++registered;
    } while (FindNextFileA(findHandle, &findData));

    FindClose(findHandle);
    s_serverPlacementScanCount = scanned;

    if (s_serverPlacementDiagBudget > 0)
    {
        Msg(eDLL_T::SERVER,
            "[OPL-SRV] object_placement_model scan: %d weapon txts, %d placement model(s) registered\n",
            scanned, registered);
        --s_serverPlacementDiagBudget;
    }
}

static const ServerObjectPlacementSettings& ServerScript_GetPlacementSettings(void* pWeapon)
{
    static const ServerObjectPlacementSettings emptySettings;

    if (!pWeapon)
        return emptySettings;

    const char* const weaponName = reinterpret_cast<const char*>(
        reinterpret_cast<uintptr_t>(pWeapon) + SERVER_WEAPON_NAME_OFFSET);
    if (!weaponName || !*weaponName)
        return emptySettings;

    ServerScript_PrecacheObjectPlacementModels();

    auto it = s_serverPlacementSettings.find(weaponName);
    if (it != s_serverPlacementSettings.end())
    {
        ServerScript_FinalizePlacementModel(it->second);
        return it->second;
    }

    // The boot scan already attempted every weapon file on disk, so a miss
    // after it means "not a placement weapon" (first-pull bow lag: this retry
    // was file IO on the per-usercmd path for every such pickup). Cache the
    // negative result without touching disk.
    if (s_serverPlacementScanCount > 0)
    {
        auto inserted = s_serverPlacementSettings.emplace(weaponName, ServerObjectPlacementSettings());
        return inserted.first->second;
    }

    ServerObjectPlacementSettings settings;
    if (!ServerScript_ReadPlacementSettingsForWeaponName(weaponName, settings))
    {
        if (s_serverPlacementDiagBudget > 0)
        {
            char path[MAX_PATH];
            V_snprintf(path, sizeof(path), "scripts/weapons/%s.txt", weaponName);
            Warning(eDLL_T::SERVER,
                "[OPL-SRV] failed to load placement weapon data '%s'\n", path);
            --s_serverPlacementDiagBudget;
        }
    }
    else
    {
        ServerScript_FinalizePlacementModel(settings);
    }

    auto inserted = s_serverPlacementSettings.emplace(weaponName, settings);
    if (s_serverPlacementDiagBudget > 0 && !settings.modelName.empty())
    {
        Msg(eDLL_T::SERVER,
            "[OPL-SRV] weapon '%s' object_placement_model='%s' bounds=%d mins=<%.1f %.1f %.1f> maxs=<%.1f %.1f %.1f> dist=%.1f\n",
            weaponName,
            settings.modelName.c_str(),
            settings.hasModelBounds ? 1 : 0,
            settings.modelMins.x, settings.modelMins.y, settings.modelMins.z,
            settings.modelMaxs.x, settings.modelMaxs.y, settings.modelMaxs.z,
            settings.distanceMax);
        --s_serverPlacementDiagBudget;
    }
    return inserted.first->second;
}

static bool ServerScript_TraceLine(
    const Vector3D& start,
    const Vector3D& end,
    CPlayer* passPlayer,
    trace_t& outTrace,
    const bool sweptHull = true,
    const Vector3D* hullMins = nullptr,
    const Vector3D* hullMaxs = nullptr,
    const unsigned int contentsMask =
        CONTENTS_BLOCK_PING | CONTENTS_PLAYERCLIP | CONTENTS_MOVEABLE |
        CONTENTS_GRATE | CONTENTS_WINDOW | CONTENTS_SOLID,
    // Ray_t carries its own up vector; the placement chain's overhang sweep
    // orients its hull along the surface normal instead of world up.
    const Vector3D* upVector = nullptr)
{
    NOTE_UNUSED(passPlayer);

    if (!g_pEngineTraceServer)
        return false;

    Ray_t ray;
    if (sweptHull)
    {
        memset(&ray, 0, sizeof(ray));
        VectorSubtract(end, start, ray.m_Delta);
        ray.m_IsSwept = ray.m_Delta.LengthSqr() != 0.0f;
        ray.m_IsRay = false;
        if (hullMins && hullMaxs)
        {
            const Vector3D center = ServerScript_ScaleVector(
                ServerScript_AddVector(*hullMins, *hullMaxs), 0.5f);
            const Vector3D extents = ServerScript_ScaleVector(
                ServerScript_SubVector(*hullMaxs, *hullMins), 0.5f);
            const Vector3D rayStart = ServerScript_AddVector(start, center);
            const Vector3D startOffset = ServerScript_ScaleVector(center, -1.0f);
            VectorCopy(startOffset, ray.m_StartOffset);
            VectorCopy(rayStart, ray.m_Start);

            VectorAligned* const rayExtents = reinterpret_cast<VectorAligned*>(
                reinterpret_cast<uintptr_t>(&ray) + 0x30);
            // No 1.0 floor: the overhang sweep's hull is deliberately
            // zero-width on two axes.
            rayExtents->x = fmaxf(extents.x, 0.0f);
            rayExtents->y = fmaxf(extents.y, 0.0f);
            rayExtents->z = fmaxf(extents.z, 0.0f);
            //.w is a derived bound the collision walk reads (see Ray_t in
            // cmodel.h); leaving it at 0 runs the sweep on a bad bound.
            rayExtents->w = rayExtents->z - rayExtents->x;
        }
        else
        {
            VectorClear(ray.m_StartOffset);
            VectorCopy(start, ray.m_Start);

            VectorAligned* const extents = reinterpret_cast<VectorAligned*>(
                reinterpret_cast<uintptr_t>(&ray) + 0x30);
            extents->x = 5.0f;
            extents->y = 5.0f;
            extents->z = 5.0f;
            extents->w = 0.0f;
        }

        VectorAligned* const upDir = reinterpret_cast<VectorAligned*>(
            reinterpret_cast<uintptr_t>(&ray) + 0x40);
        upDir->x = upVector ? upVector->x : 0.0f;
        upDir->y = upVector ? upVector->y : 0.0f;
        upDir->z = upVector ? upVector->z : 1.0f;
        upDir->w = 0.0f;

        *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(&ray) + 0x58) = 0.0f;
        *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(&ray) + 0x60) = 0;
        ray.m_pWorldAxisTransform = nullptr;
    }
    else
    {
        ray.Init(start, end, 0x3f800000, 0);
    }

    memset(&outTrace, 0, sizeof(outTrace));
    outTrace.fraction = 1.0f;
    outTrace.endpos = end;
    unsigned int mask = contentsMask;
    if ((mask & CONTENTS_MONSTER) && (!passPlayer || !v_TraceFilter_ShouldHitEntity))
        mask &= ~CONTENTS_MONSTER;

    if ((mask & CONTENTS_MONSTER) && passPlayer && v_TraceFilter_ShouldHitEntity)
    {
        CTraceFilterSimple filter(
            reinterpret_cast<const IHandleEntity*>(passPlayer),
            0);
        g_pEngineTraceServer->TraceRayFiltered(ray, mask, &filter, &outTrace);
    }
    else
    {
        g_pEngineTraceServer->TraceRay(ray, mask, &outTrace);
    }
    return true;
}

// ObjectPlacementSpecial_PlacementResult enum values.
// Script switch cases are NOT this order.
enum ObjectPlacementSpecialResult_t : int
{
    OPSPR_SUCCESS = 0,
    OPSPR_TOO_FAR = 1,
    OPSPR_TOO_DEEP = 2,
    OPSPR_TOO_COMPLEX = 3,
    OPSPR_ENTRANCE_BLOCKED = 4,
    OPSPR_ENTRANCE_INVALID_SPACE = 5,
    OPSPR_ENTRANCE_INVALID_OBJECT = 6,
    OPSPR_ENTRANCE_UNSAFE = 7,
    OPSPR_EXIT_BLOCKED = 8,
    OPSPR_EXIT_INVALID_SPACE = 9,
    OPSPR_EXIT_INVALID_OBJECT = 10,
    OPSPR_EXIT_UNSAFE = 11,
    OPSPR_EXIT_NORMAL_ALIGNED = 12,
    OPSPR_OTHER = 13,
};

// Zero-extent LINE mask == TRACE_MASK_PLAYERSOLID (includes CONTENTS_MONSTER).
// Extra ShouldHit rejects player/npc/decoy so the MONSTER bit is safe.
static constexpr unsigned int kPlacementTraceMask = TRACE_MASK_PLAYERSOLID;
static bool s_bPlacementFilterIsInitialTrace = false;

// CGameTrace.actualQuery (+0x70) is a full CollQuery copy from TraceRayFiltered.
// hitBackFace sits at results.hit + 0x20 = trace + 0x130. Surf flags at +0x4A.
static constexpr ptrdiff_t TRACE_OFF_HIT_BACKFACE = 0x130;
static constexpr ptrdiff_t TRACE_OFF_SURF_FLAGS = 0x4A;

// Long-trace length and the 32u entrance pull-back are literals out of
// ObjectPlacementSpecial_FindExitFromEyes.
static constexpr float kPlacementLongTrace = 5905.5146f;
static constexpr float kPlacementPullBack = 32.0f;

// FSOLID_TRIGGER_SLIP. CCollisionProperty: outer*, mins, maxs, then m_usSolidFlags.
static constexpr int kFSolidTriggerSlip = 0x80000;
// CCollisionProperty is polymorphic, so m_pOuter starts after the vptr:
// vptr 0x00, m_pOuter 0x08, m_vecMins 0x10, m_vecMaxs 0x1C, m_usSolidFlags 0x28.
// net_bridge_frame.cpp and zipline_validation.cpp both pin the same 0x28.
static constexpr ptrdiff_t kCollOffSolidFlags = 0x28;

static const char* ServerScript_EntityClassname(CBaseEntity* ent);
static bool ServerScript_ClassnameContains(const char* classname, const char* needle);
static bool ServerScript_PlacementFilterShouldHitEntity(
    IHandleEntity* pHandleEntity, int contentsMask);
static bool ServerScript_PointEmbedsInEntity(
    const Vector3D& point, CBaseEntity* ent);

// Ray_t m_nDetailLevel: 0 NORMAL, 1 HIGH_AT_RAY_START, 2 HIGH.
enum { kTraceDetailLevel_Normal = 0, kTraceDetailLevel_HighAtRayStart = 1, kTraceDetailLevel_High = 2 };

static bool ServerScript_PlacementTraceLine(
    const Vector3D& start,
    const Vector3D& end,
    CPlayer* owner,
    trace_t& outTrace,
    bool isInitialTrace = false,
    int detailLevel = kTraceDetailLevel_Normal)
{
    if (!g_pEngineTraceServer)
        return false;

    Ray_t ray;
    ray.Init(start, end, 0x3f800000, 0);
    ray.m_nDetailLevel = detailLevel;

    memset(&outTrace, 0, sizeof(outTrace));
    outTrace.fraction = 1.0f;
    outTrace.endpos = end;

    CTraceFilterSimple filter(
        reinterpret_cast<const IHandleEntity*>(owner),
        9, // PLAYER_MOVEMENT -- projectiles (16-17) are not surfaces
        &ServerScript_PlacementFilterShouldHitEntity);
    const bool prevInitial = s_bPlacementFilterIsInitialTrace;
    s_bPlacementFilterIsInitialTrace = isInitialTrace;
    g_pEngineTraceServer->TraceRayFiltered(
        ray, kPlacementTraceMask, &filter, &outTrace);
    s_bPlacementFilterIsInitialTrace = prevInitial;
    return true;
}

static bool ServerScript_TraceHitBackFace(const trace_t& tr)
{
    const uint8_t byte =
        *(reinterpret_cast<const uint8_t*>(&tr) + TRACE_OFF_HIT_BACKFACE);
    return byte != 0;
}

// S3 world traces never write hitBackFace. A back-trace that ends on the
// entrance is the thin / one-sided case that byte marks on S21.
static bool ServerScript_TraceEndedNearPoint(
    const trace_t& tr, const Vector3D& point, float epsSqr)
{
    if (tr.fraction >= 1.0f)
        return false;
    const Vector3D delta = ServerScript_SubVector(tr.endpos, point);
    return ServerScript_Dot(delta, delta) < epsSqr;
}

static uint16_t ServerScript_TraceSurfFlags(const trace_t& tr)
{
    return *reinterpret_cast<const uint16_t*>(
        reinterpret_cast<const uint8_t*>(&tr) + TRACE_OFF_SURF_FLAGS);
}

static bool ServerScript_IsPhaseDoorWeapon(void* pWeapon)
{
    if (!pWeapon)
        return false;

    const char* const weaponName = reinterpret_cast<const char*>(
        reinterpret_cast<uintptr_t>(pWeapon) + SERVER_WEAPON_NAME_OFFSET);
    if (weaponName && strncmp(weaponName, "mp_ability_phase_door", 21) == 0)
        return true;

    if (!v_UTIL_GetEntityScriptInfo)
        return false;

    const char* const info = UTIL_GetEntityScriptInfo(reinterpret_cast<CBaseEntity*>(pWeapon));
    return info && strstr(info, "mp_ability_phase_door") != nullptr;
}

static float ServerScript_ObjectPlacementDistance(void* pWeapon, const bool special)
{
    // 1750 is the phase-door default when weapon.txt did not set
    // object_placement_distance_max. distanceMaxSet distinguishes unset from 128.
    const ServerObjectPlacementSettings& settings = ServerScript_GetPlacementSettings(pWeapon);
    if (special || ServerScript_IsPhaseDoorWeapon(pWeapon))
        return settings.distanceMaxSet ? settings.distanceMax : 1750.0f; // 1750 = observed default only.

    return settings.distanceMax > 0.0f ? settings.distanceMax : 128.0f;
}

static void ServerScript_SetPlacementFromTrace(
    ServerObjectPlacementState& state,
    const trace_t& tr)
{
    const Vector3D normal = ServerScript_Normalized(tr.plane.normal);
    state.origin = tr.endpos;
    // Special/portal placement wants FORWARD along the wall normal. Ground props re-orient in PlacementFindAngles.
    VectorAngles(normal, state.angles.AsQAngle());
    state.parent = ServerScript_PlacementParentFromTrace(tr);
}

static unsigned int ServerScript_PlacementTraceMask(const ServerObjectPlacementSettings& settings)
{
    unsigned int mask =
        CONTENTS_BLOCK_PING | CONTENTS_PLAYERCLIP | CONTENTS_MOVEABLE |
        CONTENTS_GRATE | CONTENTS_WINDOW | CONTENTS_SOLID;

    if (!settings.ignorePlayers)
        mask |= CONTENTS_MONSTER;

    return mask;
}


// classname / move-parent are protected. Same-layout derived class, no extra
// members or vtable slots -- zero-offset downcast.
class ServerScript_EntityFieldAccess : public CBaseEntity
{
public:
    using CBaseEntity::m_iClassname;
    using CBaseEntity::m_hMoveParent;
    using CBaseEntity::m_angAbsRotation;
    using CBaseEntity::m_RefEHandle;
};

static uint32_t ServerScript_EntityOwnHandle(CBaseEntity* ent)
{
    if (!ent)
        return INVALID_EHANDLE_INDEX;

    ServerScript_EntityFieldAccess* const accessor =
        static_cast<ServerScript_EntityFieldAccess*>(ent);
    return static_cast<uint32_t>(accessor->m_RefEHandle.ToInt());
}

static float ServerScript_EntityAbsYaw(CBaseEntity* ent)
{
    if (!ent)
        return 0.0f;

    ServerScript_EntityFieldAccess* const accessor =
        static_cast<ServerScript_EntityFieldAccess*>(ent);
    return accessor->m_angAbsRotation.y;
}

static Vector3D ServerScript_EntityAbsAngles(CBaseEntity* ent)
{
    if (!ent)
        return Vector3D(0.0f, 0.0f, 0.0f);

    ServerScript_EntityFieldAccess* const accessor =
        static_cast<ServerScript_EntityFieldAccess*>(ent);
    return accessor->m_angAbsRotation;
}

static const char* ServerScript_EntityClassname(CBaseEntity* ent)
{
    if (!ent)
        return nullptr;

    ServerScript_EntityFieldAccess* const accessor =
        static_cast<ServerScript_EntityFieldAccess*>(ent);
    if (!accessor->m_iClassname)
        return nullptr;

    return STRING(accessor->m_iClassname);
}

static CBaseEntity* ServerScript_EntityMoveParent(CBaseEntity* ent)
{
    if (!ent)
        return nullptr;

    ServerScript_EntityFieldAccess* const accessor =
        static_cast<ServerScript_EntityFieldAccess*>(ent);
    if (!accessor->m_hMoveParent.IsValid())
        return nullptr;

    // CBaseHandle::Get has no implementation here; resolve via raw-index lookup.
    return reinterpret_cast<CBaseEntity*>(
        ServerScript_LookupEntityFromRawHandle(static_cast<uint32_t>(accessor->m_hMoveParent.ToInt())));
}

static bool ServerScript_IsMoverOrChildOfMover(CBaseEntity* ent)
{
    // MOVETYPE_PUSH mover, or a child in its move-parent chain.
    int safety = 0;
    while (ent && safety++ < 64)
    {
        if (ent->Diag_MoveType() == MOVETYPE_PUSH)
            return true;

        ent = ServerScript_EntityMoveParent(ent);
    }

    return false;
}

SQRESULT Script_IsMoverOrChildOfMover(HSQUIRRELVM v)
{
    void* pEnt = nullptr;
    if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
        return SQ_ERROR;

    const bool isMover = ServerScript_IsMoverOrChildOfMover(reinterpret_cast<CBaseEntity*>(pEnt));
    sq_pushbool(v, isMover ? SQTrue : SQFalse);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_SetEnableScriptAnimModifier(HSQUIRRELVM v)
{
    void* pEnt = nullptr;
    if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
        return SQ_ERROR;

    SQBool bEnable = SQFalse;
    if (SQ_FAILED(sq_getbool(v, 2, &bEnable)))
        return SQ_ERROR;

    reinterpret_cast<CBaseAnimating*>(pEnt)->SetEnableScriptAnimModifier(bEnable != SQFalse);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// S21 fields live in the grown allocation tail. Resolve only on that class --
// any other entity is smaller, and script can call these from any entity.
static const char* s_extendSlotWarned[16] = {};
static int s_extendSlotWarnedCount = 0;


static void* ServerScript_ResolveExtendSlot(void* pEnt, const char* pszClassName,
    const char* tableName, const char* propName, const char* pszTag)
{
    if (!DTExtend_EntityIsS21Class(pEnt, pszClassName))
        return nullptr;

    const int offset = DTExtend_GetOffset(tableName, propName);
    if (offset > 0)
        return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pEnt) + offset);

    for (int i = 0; i < s_extendSlotWarnedCount; ++i)
        if (s_extendSlotWarned[i] == propName)
            return nullptr;

    if (s_extendSlotWarnedCount < 16)
        s_extendSlotWarned[s_extendSlotWarnedCount++] = propName;

    Warning(eDLL_T::SERVER,
        "[%s] %s.%s has no entity slot (offset %d) -- the extend prop never reached "
        "the wrapper, so this field cannot be networked.\n",
        pszTag, tableName, propName, offset);
    return nullptr;
}

// Setters raise instead of no-opping: on S21 these are methods of the class
// itself, so calling one on anything else is a script bug, not a runtime state.
static bool ServerScript_WriteExtendInt(HSQUIRRELVM v, void* pEnt, const char* pszClassName,
    const char* tableName, const char* propName, const int value, const char* pszTag)
{
    int* const pField = reinterpret_cast<int*>(
        ServerScript_ResolveExtendSlot(pEnt, pszClassName, tableName, propName, pszTag));
    if (!pField)
    {
        if (v_SQVM_RaiseError)
            v_SQVM_RaiseError(v, "%s is only valid on a %s entity\n", propName, pszClassName);
        return false;
    }

    if (*pField != value)
    {
        *pField = value;
        MarkEntityEdictDirty(pEnt);
    }

    (void)pszTag;
    return true;
}

static int ServerScript_ReadExtendInt(void* pEnt, const char* pszClassName,
    const char* tableName, const char* propName, const char* pszTag)
{
    const int* const pField = reinterpret_cast<const int*>(
        ServerScript_ResolveExtendSlot(pEnt, pszClassName, tableName, propName, pszTag));
    return pField ? *pField : 0;
}

static bool ServerScript_WriteExtendFloat(HSQUIRRELVM v, void* pEnt, const char* pszClassName,
    const char* tableName, const char* propName, const float value, const char* pszTag)
{
    float* const pField = reinterpret_cast<float*>(
        ServerScript_ResolveExtendSlot(pEnt, pszClassName, tableName, propName, pszTag));
    if (!pField)
    {
        if (v_SQVM_RaiseError)
            v_SQVM_RaiseError(v, "%s is only valid on a %s entity\n", propName, pszClassName);
        return false;
    }

    if (*pField != value)
    {
        *pField = value;
        MarkEntityEdictDirty(pEnt);
    }

    (void)pszTag;
    return true;
}

static float ServerScript_ReadExtendFloat(void* pEnt, const char* pszClassName,
    const char* tableName, const char* propName, const char* pszTag)
{
    const float* const pField = reinterpret_cast<const float*>(
        ServerScript_ResolveExtendSlot(pEnt, pszClassName, tableName, propName, pszTag));
    return pField ? *pField : 0.0f;
}

// Tiers run 0..5: sh_loot_rollers.nut colours a vault-key eye with
// COLORID_FX_LOOT_TIER0 + 5, which is the highest palette entry the FX path uses.
static constexpr int LOOT_ROLLER_MAX_TIER = 5;

static bool ServerScript_SetLootRollerField(HSQUIRRELVM v, void* pEnt, const char* propName, int value)
{
    return ServerScript_WriteExtendInt(v, pEnt, "CLootRoller", "DT_LootRoller",
        propName, value, "LOOTROLLER");
}

SQRESULT Script_SetTier(HSQUIRRELVM v)
{
    void* pEnt = nullptr;
    if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
        return SQ_ERROR;

    SQInteger tier = 0;
    if (SQ_FAILED(sq_getinteger(v, 2, &tier)))
        return SQ_ERROR;

    // The client colours the eye FX with COLORID_FX_LOOT_TIER0 + tier, so a value
    // past the tier range would pull an unrelated palette entry.
    if (tier < 0 || tier > LOOT_ROLLER_MAX_TIER)
    {
        Warning(eDLL_T::SERVER, "[LOOTROLLER] SetTier %d out of range (0-%d); clamped.\n",
            static_cast<int>(tier), LOOT_ROLLER_MAX_TIER);
        tier = tier < 0 ? 0 : LOOT_ROLLER_MAX_TIER;
    }

    if (!ServerScript_SetLootRollerField(v, pEnt, "m_tier", static_cast<int>(tier)))
        return SQ_ERROR;

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_SetHasVaultKey(HSQUIRRELVM v)
{
    void* pEnt = nullptr;
    if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
        return SQ_ERROR;

    SQBool hasVaultKey = SQFalse;
    if (SQ_FAILED(sq_getbool(v, 2, &hasVaultKey)))
        return SQ_ERROR;

    if (!ServerScript_SetLootRollerField(v, pEnt, "m_hasVaultKey", hasVaultKey != SQFalse ? 1 : 0))
        return SQ_ERROR;

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// m_lootGrabberType: 0 default, 1 vending machine, 2 linked box.
// Client reads: +5600 colorID, +5602 type, +5608 lootBeingGrabbed, +5612 grabDist.
enum class LootGrabberType_t : int
{
    LOOTGRABBER_DEFAULT = 0,
    LOOTGRABBER_VENDING_MACHINE = 1,
    LOOTGRABBER_LINKED_BOX = 2,
};

// Radius feeds GetSurvivalLootNearbyPos and a particle CP; shipping max is firing-range 50000.
static constexpr float LOOT_GRAB_DIST_MAX = 100000.0f;
// Engine colorID keyfield parser refuses >= 256
// ("please increase COLORPALETTE_COLORID_BITS").
static constexpr int IMPACT_EFFECT_COLORID_MAX = 255;
// Refcount of players with this grabber's ground-list open. Bounded so a
// mismatched Increment/Decrement pair cannot run away.
static constexpr int LOOT_GRABBER_MAX_GRABBERS = 128;

SQRESULT Script_SetLootGrabDist(HSQUIRRELVM v)
{
    void* pEnt = nullptr;
    if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
        return SQ_ERROR;

    SQFloat dist = 0.0f;
    if (SQ_FAILED(sq_getfloat(v, 2, &dist)))
        return SQ_ERROR;

    if (!(dist >= 0.0f) || dist > LOOT_GRAB_DIST_MAX)   // rejects NaN too
    {
        Warning(eDLL_T::SERVER, "[LOOTGRABBER] SetLootGrabDist %f out of range (0-%f); clamped.\n",
            dist, LOOT_GRAB_DIST_MAX);
        dist = (dist > 0.0f) ? LOOT_GRAB_DIST_MAX : 0.0f;
    }

    if (!ServerScript_WriteExtendFloat(v, pEnt, "CLootGrabber", "DT_LootGrabber",
            "m_lootGrabDist", static_cast<float>(dist), "LOOTGRABBER"))
        return SQ_ERROR;

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_GetLootGrabDist(HSQUIRRELVM v)
{
    void* pEnt = nullptr;
    if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
        return SQ_ERROR;

    sq_pushfloat(v, ServerScript_ReadExtendFloat(pEnt, "CLootGrabber", "DT_LootGrabber",
        "m_lootGrabDist", "LOOTGRABBER"));
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_SetIsVendingMachine(HSQUIRRELVM v)
{
    void* pEnt = nullptr;
    if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
        return SQ_ERROR;

    if (!ServerScript_WriteExtendInt(v, pEnt, "CLootGrabber", "DT_LootGrabber",
            "m_lootGrabberType",
            static_cast<int>(LootGrabberType_t::LOOTGRABBER_VENDING_MACHINE), "LOOTGRABBER"))
        return SQ_ERROR;

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_IsVendingMachine(HSQUIRRELVM v)
{
    void* pEnt = nullptr;
    if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
        return SQ_ERROR;

    const int type = ServerScript_ReadExtendInt(pEnt, "CLootGrabber", "DT_LootGrabber",
        "m_lootGrabberType", "LOOTGRABBER");
    sq_pushbool(v, type == static_cast<int>(LootGrabberType_t::LOOTGRABBER_VENDING_MACHINE));
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_IsLinkedBox(HSQUIRRELVM v)
{
    void* pEnt = nullptr;
    if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
        return SQ_ERROR;

    const int type = ServerScript_ReadExtendInt(pEnt, "CLootGrabber", "DT_LootGrabber",
        "m_lootGrabberType", "LOOTGRABBER");
    sq_pushbool(v, type == static_cast<int>(LootGrabberType_t::LOOTGRABBER_LINKED_BOX));
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// m_lootBeingGrabbed is a refcount; S21 script only has Increment/Decrement.
static SQRESULT Script_AdjustPlayersGrabbingLoot(HSQUIRRELVM v, const int delta)
{
    void* pEnt = nullptr;
    if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
        return SQ_ERROR;

    int count = ServerScript_ReadExtendInt(pEnt, "CLootGrabber", "DT_LootGrabber",
        "m_lootBeingGrabbed", "LOOTGRABBER") + delta;
    if (count < 0)
        count = 0;
    else if (count > LOOT_GRABBER_MAX_GRABBERS)
        count = LOOT_GRABBER_MAX_GRABBERS;

    if (!ServerScript_WriteExtendInt(v, pEnt, "CLootGrabber", "DT_LootGrabber",
            "m_lootBeingGrabbed", count, "LOOTGRABBER"))
        return SQ_ERROR;

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_IncrementPlayersGrabbingLoot(HSQUIRRELVM v)
{
    return Script_AdjustPlayersGrabbingLoot(v, 1);
}

SQRESULT Script_DecrementPlayersGrabbingLoot(HSQUIRRELVM v)
{
    return Script_AdjustPlayersGrabbingLoot(v, -1);
}

// Scripts set ent.kv.impacteffectcolorid, but that keyfield is CShieldProp, not prop_dynamic.
SQRESULT Script_SetImpactEffectColorID(HSQUIRRELVM v)
{
    void* pEnt = nullptr;
    if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
        return SQ_ERROR;

    SQInteger colorID = 0;
    if (SQ_FAILED(sq_getinteger(v, 2, &colorID)))
        return SQ_ERROR;

    if (colorID < 0 || colorID > IMPACT_EFFECT_COLORID_MAX)
    {
        Warning(eDLL_T::SERVER, "[LOOTGRABBER] SetImpactEffectColorID %d out of range (0-%d); clamped.\n",
            static_cast<int>(colorID), IMPACT_EFFECT_COLORID_MAX);
        colorID = colorID < 0 ? 0 : IMPACT_EFFECT_COLORID_MAX;
    }

    if (!ServerScript_WriteExtendInt(v, pEnt, "CLootGrabber", "DT_LootGrabber",
            "m_impactEffectColorID", static_cast<int>(colorID), "LOOTGRABBER"))
        return SQ_ERROR;

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Client reads m_lootIndex at +5600, m_contentsTaken at +5604. -1 = nothing to reveal.
static constexpr int CARE_PACKAGE_LOOT_INDEX_MAX = 65535;

SQRESULT Script_SetLootIndex(HSQUIRRELVM v)
{
    void* pEnt = nullptr;
    if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
        return SQ_ERROR;

    SQInteger lootIndex = 0;
    if (SQ_FAILED(sq_getinteger(v, 2, &lootIndex)))
        return SQ_ERROR;

    if (lootIndex < -1 || lootIndex > CARE_PACKAGE_LOOT_INDEX_MAX)
    {
        Warning(eDLL_T::SERVER, "[CAREPACKAGE] SetLootIndex %d out of range (-1..%d); ignored.\n",
            static_cast<int>(lootIndex), CARE_PACKAGE_LOOT_INDEX_MAX);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    if (!ServerScript_WriteExtendInt(v, pEnt, "CCarePackageInsightProp",
            "DT_CarePackageInsightProp", "m_lootIndex", static_cast<int>(lootIndex), "CAREPACKAGE"))
        return SQ_ERROR;

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_GetLootIndex(HSQUIRRELVM v)
{
    void* pEnt = nullptr;
    if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
        return SQ_ERROR;

    sq_pushinteger(v, ServerScript_ReadExtendInt(pEnt, "CCarePackageInsightProp",
        "DT_CarePackageInsightProp", "m_lootIndex", "CAREPACKAGE"));
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_SetAreContentsTaken(HSQUIRRELVM v)
{
    void* pEnt = nullptr;
    if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
        return SQ_ERROR;

    SQBool contentsTaken = SQFalse;
    if (SQ_FAILED(sq_getbool(v, 2, &contentsTaken)))
        return SQ_ERROR;

    if (!ServerScript_WriteExtendInt(v, pEnt, "CCarePackageInsightProp",
            "DT_CarePackageInsightProp", "m_contentsTaken",
            contentsTaken != SQFalse ? 1 : 0, "CAREPACKAGE"))
        return SQ_ERROR;

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_GetAreContentsTaken(HSQUIRRELVM v)
{
    void* pEnt = nullptr;
    if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
        return SQ_ERROR;

    sq_pushbool(v, ServerScript_ReadExtendInt(pEnt, "CCarePackageInsightProp",
        "DT_CarePackageInsightProp", "m_contentsTaken", "CAREPACKAGE") != 0);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static bool ServerScript_ClassnameContains(const char* classname, const char* needle)
{
    return classname && needle && strstr(classname, needle) != nullptr;
}

// Filter body (declared above PlacementTraceLine).
static bool ServerScript_PlacementFilterShouldHitEntity(
    IHandleEntity* pHandleEntity, int contentsMask)
{
    NOTE_UNUSED(contentsMask);
    if (!pHandleEntity)
        return true;

    CBaseEntity* const ent = reinterpret_cast<CBaseEntity*>(pHandleEntity);
    const char* const classname = ServerScript_EntityClassname(ent);
    if (!classname)
        return true;

    if (_stricmp(classname, "player") == 0 ||
        ServerScript_ClassnameContains(classname, "npc_") ||
        ServerScript_ClassnameContains(classname, "player_decoy"))
    {
        return false;
    }

    if (!s_bPlacementFilterIsInitialTrace)
    {
        if (ServerScript_ClassnameContains(classname, "prop_loot") ||
            ServerScript_ClassnameContains(classname, "prop_survival") ||
            ServerScript_ClassnameContains(classname, "prop_death") ||
            ServerScript_ClassnameContains(classname, "death_box") ||
            ServerScript_ClassnameContains(classname, "deathbox") ||
            ServerScript_ClassnameContains(classname, "prop_script_loot") ||
            ServerScript_ClassnameContains(classname, "grenade_") ||
            ServerScript_ClassnameContains(classname, "projectile"))
        {
            return false;
        }
    }

    return true;
}

// Collideable embed via ClipRayToCollideable.
static bool ServerScript_PointEmbedsInEntity(
    const Vector3D& point, CBaseEntity* ent)
{
    if (!ent || !g_pEngineTraceServer)
        return false;

    CCollisionProperty* const coll = ent->CollisionProp();
    if (!coll)
        return false;

    constexpr float kCushion = 0.03125f;
    const Vector3D end(point.x, point.y, point.z + kCushion);

    Ray_t ray;
    ray.Init(point, end, 0x3f800000, 0);

    trace_t tr;
    memset(&tr, 0, sizeof(tr));
    tr.fraction = 1.0f;
    tr.endpos = end;

    g_pEngineTraceServer->ClipRayToCollideable(
        ray, 0xBFFFFFFFu, coll, &tr);

    return tr.startsolid || tr.allsolid || tr.fraction < 1.0f;
}

static int ServerScript_EntitySolidFlags(CBaseEntity* ent)
{
    if (!ent || !ent->CollisionProp())
        return 0;
    return *reinterpret_cast<const int*>(
        reinterpret_cast<const uint8_t*>(ent->CollisionProp()) + kCollOffSolidFlags);
}

static Vector3D ServerScript_CrossVector(const Vector3D& a, const Vector3D& b)
{
    return Vector3D(
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x);
}

// Rodrigues: rotate v by radians around a unit axis.
static Vector3D ServerScript_RotateAroundAxis(const Vector3D& v, const Vector3D& axis, const float radians)
{
    const float c = cosf(radians);
    const float s = sinf(radians);
    const Vector3D cross = ServerScript_CrossVector(axis, v);
    const float dot = ServerScript_Dot(axis, v);
    return Vector3D(
        v.x * c + cross.x * s + axis.x * dot * (1.0f - c),
        v.y * c + cross.y * s + axis.y * dot * (1.0f - c),
        v.z * c + cross.z * s + axis.z * dot * (1.0f - c));
}

static bool ServerScript_BoxesOverlap(
    const Vector3D& aMins, const Vector3D& aMaxs,
    const Vector3D& bMins, const Vector3D& bMaxs)
{
    return aMins.x <= bMaxs.x && aMaxs.x >= bMins.x &&
        aMins.y <= bMaxs.y && aMaxs.y >= bMins.y &&
        aMins.z <= bMaxs.z && aMaxs.z >= bMins.z;
}

// World point to entity local: origin-relative, inverse-yaw. OBB mins/maxs
// are local; entOrigin + localMins is only correct for an unrotated entity.
static Vector3D ServerScript_WorldPointToEntityLocal(const Vector3D& worldPoint, CBaseEntity* const ent)
{
    const Vector3D entOrigin = ent->Diag_AbsOrigin();
    const float yawRadians = ServerScript_EntityAbsYaw(ent) * (3.14159265358979323846f / 180.0f);
    return ServerScript_RotateAroundAxis(
        ServerScript_SubVector(worldPoint, entOrigin), Vector3D(0.0f, 0.0f, 1.0f), -yawRadians);
}

// 0 = wall, 1 = floor, 2 = ceiling.
int ServerScript_ClassifyPortalDir(const Vector3D& normal)
{
    // Same threshold as ServerScript_CheckEntombmentAlongLine (below) --
    // verified constant already used by this file's orientation shim.
    constexpr float kUpDownDot = 0.70610678f;
    if (normal.z > kUpDownDot)
        return 1;
    if (normal.z < -kUpDownDot)
        return 2;
    return 0;
}

// Entombment / safe-exit. No C_SafePositionList on this SDK.
static bool ServerScript_CheckEntombmentAlongLine(
    const Vector3D& exitPos,
    const Vector3D& traceEndPoint,
    const Vector3D& traceDir,
    const Vector3D& traceRight,
    CPlayer* owner,
    bool& needsNavmeshCheck)
{
    needsNavmeshCheck = false;

    constexpr float kUpDownDot = 0.70610678f;
    const float upDot = ServerScript_Dot(traceDir, Vector3D(0.0f, 0.0f, 1.0f));
    const bool isUp = upDot > kUpDownDot;
    const bool isDown = upDot < -kUpDownDot;

    const Vector3D crossRightDir = ServerScript_Normalized(ServerScript_CrossVector(traceRight, traceDir));

    // Up to 6 probe directions; skip +/-up that matches the classification above.
    Vector3D probeDirs[6];
    int numProbeDirs = 0;
    if (!isUp)
        probeDirs[numProbeDirs++] = Vector3D(0.0f, 0.0f, 1.0f);
    if (!isDown)
        probeDirs[numProbeDirs++] = Vector3D(0.0f, 0.0f, -1.0f);
    probeDirs[numProbeDirs++] = traceRight;
    probeDirs[numProbeDirs++] = ServerScript_ScaleVector(traceRight, -1.0f);
    probeDirs[numProbeDirs++] = crossRightDir;
    probeDirs[numProbeDirs++] = ServerScript_ScaleVector(crossRightDir, -1.0f);

    const Vector3D wallSpan = ServerScript_SubVector(traceEndPoint, exitPos);
    const float wallDepth = sqrtf(ServerScript_Dot(wallSpan, wallSpan));

    // Up to 3 shells along traceDir: shell 0 radius 16, shells 1-2 = wallDepth/3.
    constexpr int kMaxShells = 3;
    for (int shell = 0; shell < kMaxShells; ++shell)
    {
        const float shellRadius = (shell == 0) ? 16.0f : (wallDepth / 3.0f);
        if (shellRadius <= 0.0f)
            continue;

        const Vector3D shellPoint = ServerScript_AddScaled(exitPos, traceDir, shellRadius);
        for (int p = 0; p < numProbeDirs; ++p)
        {
            const Vector3D probeEnd = ServerScript_AddScaled(shellPoint, probeDirs[p], shellRadius);
            trace_t probeTrace;
            if (!ServerScript_TraceLine(shellPoint, probeEnd, owner, probeTrace, false))
                continue;

            // Probe is clear only on a full-length miss with no startsolid/allsolid.
            const bool probeClear = probeTrace.fraction >= 1.0f && !probeTrace.startsolid && !probeTrace.allsolid;
            if (probeClear)
                return false; // escape found on this probe -- not entombed.
        }
    }

    if (s_serverPlacementDiagBudget > 0)
    {
        Msg(eDLL_T::SERVER,
            "[OPL-SRV] entombment: all shells/probes exhausted, no escape found wallDepth=%.1f exitPos=<%.1f %.1f %.1f>\n",
            wallDepth, exitPos.x, exitPos.y, exitPos.z);
        --s_serverPlacementDiagBudget;
    }
    return true; // all shells/probes exhausted with no escape.
}

// No-placement volume scan (NUM_ENT_ENTRIES). Containment is
// ClipRayToCollideable, not OBB, so hollow OOB brushes do not veto interiors.
static bool ServerScript_MayPlaceObjectAtPoint(const Vector3D& point, void* excludeParent)
{
    if (!g_serverEntityList)
        return true;

    // Real client's 2x2x2 box: point +/- 1.0 -- used only as a coarse cull
    // before the collideable embed test.
    constexpr float kPlacementBoxHalfExtent = 1.0f;

    for (int i = 0; i < NUM_ENT_ENTRIES; ++i)
    {
        CBaseEntity* const ent = reinterpret_cast<CBaseEntity*>(
            g_serverEntityList->LookupEntityByNetworkIndex(i));
        if (!ent || reinterpret_cast<void*>(ent) == excludeParent)
            continue;

        const char* const classname = ServerScript_EntityClassname(ent);
        if (!classname)
            continue;

        // GT blockers: DisallowsObjectPlacementSpecial / OOB / slip. func_brush / func_door / trigger_hurt are not.
        const bool isNoPlace =
            ServerScript_ClassnameContains(classname, "trigger_no_object_placement_special") ||
            ServerScript_ClassnameContains(classname, "trigger_networked_no_ops") ||
            ServerScript_ClassnameContains(classname, "trigger_networked_block_all_op");
        const bool isOob =
            ServerScript_ClassnameContains(classname, "trigger_networked_out_of_bounds") ||
            ServerScript_ClassnameContains(classname, "trigger_out_of_bounds");
        const bool isSlip =
            (ServerScript_EntitySolidFlags(ent) & kFSolidTriggerSlip) != 0;
        if (!isNoPlace && !isOob && !isSlip)
            continue;

        // Coarse local OBB cull (point +/- 1) before the exact embed test.
        const Vector3D localPoint = ServerScript_WorldPointToEntityLocal(point, ent);
        const Vector3D obbMins = ent->CollisionProp()->OBBMins();
        const Vector3D obbMaxs = ent->CollisionProp()->OBBMaxs();
        const Vector3D placementLocalMins(
            localPoint.x - kPlacementBoxHalfExtent,
            localPoint.y - kPlacementBoxHalfExtent,
            localPoint.z - kPlacementBoxHalfExtent);
        const Vector3D placementLocalMaxs(
            localPoint.x + kPlacementBoxHalfExtent,
            localPoint.y + kPlacementBoxHalfExtent,
            localPoint.z + kPlacementBoxHalfExtent);
        if (!ServerScript_BoxesOverlap(placementLocalMins, placementLocalMaxs, obbMins, obbMaxs))
            continue;

        // Exact: only veto when the collideable actually embeds the point.
        if (!ServerScript_PointEmbedsInEntity(point, ent))
            continue;

        if (s_serverPlacementDiagBudget > 0)
        {
            Msg(eDLL_T::SERVER,
                "[OPL-SRV] MayPlaceObjectAtPoint: vetoed by '%s' (embed) point=<%.1f %.1f %.1f> entOrigin=<%.1f %.1f %.1f> flags=0x%X ent=%p\n",
                classname, point.x, point.y, point.z,
                ent->Diag_AbsOrigin().x, ent->Diag_AbsOrigin().y, ent->Diag_AbsOrigin().z,
                ServerScript_EntitySolidFlags(ent),
                reinterpret_cast<void*>(ent));
            --s_serverPlacementDiagBudget;
        }
        return false;
    }

    return true;
}

// CTriggerHurt: m_flDamage @ 3300, m_damageModel @ 3320. Keep volume when damageModel==2 || damage>=5000.
static constexpr ptrdiff_t kTriggerHurtOffDamage = 3300;
static constexpr ptrdiff_t kTriggerHurtOffDamageModel = 3320;
static constexpr int kTriggerHurtDamageModelFall = 2;
static constexpr float kTriggerHurtLethalDamage = 5000.0f;

static bool ServerScript_TriggerHurtIsLethalFall(CBaseEntity* ent)
{
    if (!ent)
        return false;

    const int damageModel = *reinterpret_cast<const int*>(
        reinterpret_cast<const uint8_t*>(ent) + kTriggerHurtOffDamageModel);
    if (damageModel == kTriggerHurtDamageModelFall)
        return true;

    const float damage = *reinterpret_cast<const float*>(
        reinterpret_cast<const uint8_t*>(ent) + kTriggerHurtOffDamage);
    return damage >= kTriggerHurtLethalDamage;
}

// Fall-hazard: fan finds the worst landing; reject if it embeds in a lethal trigger_hurt.
static bool ServerScript_PostValidatePlacement(
    const Vector3D& portalPos,
    const Vector3D& portalNormal,
    const int portalDir, // 0=LEFT wall, 1=RIGHT floor, 2=UP ceiling
    CPlayer* owner)
{
    if (portalDir == 1) // floor-facing: no fall-hazard fan.
        return true;

    constexpr float kProbeOffset = 16.0f;
    constexpr float kFanRange = 6000.0f;
    constexpr float kCeilingConeHalfAngle = 0.2617994f;   // 15 degrees.
    constexpr float kWallAngleStep = 0.17453292f;         // 10 degrees.

    const Vector3D probeOrigin = ServerScript_AddScaled(portalPos, portalNormal, kProbeOffset);

    Vector3D fanDirs[4];
    if (portalDir == 2) // ceiling: cone around straight-down.
    {
        const Vector3D down(0.0f, 0.0f, -1.0f);
        const Vector3D rawFlatNormal(portalNormal.x, portalNormal.y, 0.0f);
        const bool flatNormalIsDegenerate = ServerScript_Dot(rawFlatNormal, rawFlatNormal) <= 0.000001f;
        const Vector3D rotAxis = flatNormalIsDegenerate
            ? Vector3D(1.0f, 0.0f, 0.0f)
            : ServerScript_Normalized(rawFlatNormal);
        for (int i = 0; i < 4; ++i)
        {
            const float angle = kCeilingConeHalfAngle * (static_cast<float>(i) / 3.0f * 2.0f - 1.0f);
            fanDirs[i] = ServerScript_RotateAroundAxis(down, rotAxis, angle);
        }
    }
    else // wall: fan steps off the surface normal.
    {
        for (int i = 0; i < 4; ++i)
        {
            const float angle = kWallAngleStep * static_cast<float>(i); // 0/10/20/30 degrees.
            fanDirs[i] = ServerScript_RotateAroundAxis(portalNormal, Vector3D(0.0f, 0.0f, 1.0f), angle);
        }
    }

    Vector3D worstLandingPoint = probeOrigin;
    float worstZ = probeOrigin.z;
    bool anyTrace = false;
    for (int i = 0; i < 4; ++i)
    {
        trace_t fanTrace;
        Vector3D fanEnd = ServerScript_AddScaled(probeOrigin, fanDirs[i], kFanRange);
        if (portalDir != 2)
            fanEnd.z = probeOrigin.z - kFanRange;
        if (!ServerScript_TraceLine(probeOrigin, fanEnd, owner, fanTrace, false))
            continue;

        anyTrace = true;
        if (fanTrace.endpos.z < worstZ)
        {
            worstZ = fanTrace.endpos.z;
            worstLandingPoint = fanTrace.endpos;
        }
    }

    if (!anyTrace || !g_serverEntityList)
        return true;

    // Sample a few points along the fall segment so a tall thin hurt volume
    // that only crosses mid-path is still caught (capsule substitute).
    constexpr int kSegSamples = 5;
    Vector3D segSamples[kSegSamples];
    for (int s = 0; s < kSegSamples; ++s)
    {
        const float t = static_cast<float>(s) / static_cast<float>(kSegSamples - 1);
        segSamples[s] = ServerScript_AddVector(
            ServerScript_ScaleVector(probeOrigin, 1.0f - t),
            ServerScript_ScaleVector(worstLandingPoint, t));
    }

    for (int i = 0; i < NUM_ENT_ENTRIES; ++i)
    {
        CBaseEntity* const ent = reinterpret_cast<CBaseEntity*>(
            g_serverEntityList->LookupEntityByNetworkIndex(i));
        if (!ent)
            continue;

        const char* const classname = ServerScript_EntityClassname(ent);
        if (!ServerScript_ClassnameContains(classname, "trigger_hurt"))
            continue;
        if (!ServerScript_TriggerHurtIsLethalFall(ent))
            continue;

        for (int s = 0; s < kSegSamples; ++s)
        {
            if (ServerScript_PointEmbedsInEntity(segSamples[s], ent))
            {
                if (s_serverPlacementDiagBudget > 0)
                {
                    Msg(eDLL_T::SERVER,
                        "[OPL-SRV] PostValidate: fall path embeds in '%s' sample=%d pt=<%.1f %.1f %.1f>\n",
                        classname, s,
                        segSamples[s].x, segSamples[s].y, segSamples[s].z);
                    --s_serverPlacementDiagBudget;
                }
                return false;
            }
        }
    }

    return true;
}

// PhaseDoor_CheckInvalidEnt: cache HSCRIPT once per VM. True means invalid.
// The handle copies the closure object and dies with the Squirrel VM at level
// change while the CSquirrelVM wrapper is reused, so key on the live
// HSQUIRRELVM.
static HSCRIPT s_hPhaseDoorCheckInvalidEnt = nullptr;
static HSQUIRRELVM s_hPhaseDoorCallbackVM = nullptr;
static bool s_bPhaseDoorCallbackMissingLatched = false;

static void ServerScript_PlacementCallbackCacheReset(void)
{
    s_hPhaseDoorCheckInvalidEnt = nullptr;
    s_hPhaseDoorCallbackVM = nullptr;
    s_bPhaseDoorCallbackMissingLatched = false;
}

static bool ServerScript_PhaseDoorCheckInvalidEnt(CBaseEntity* hitEnt)
{
    if (!hitEnt || !g_pServerScript)
        return false;

    const HSQUIRRELVM hVMNow = g_pServerScript->GetVM();
    if (!hVMNow)
        return false;

    if (s_hPhaseDoorCallbackVM != hVMNow)
    {
        s_hPhaseDoorCallbackVM = hVMNow;
        s_hPhaseDoorCheckInvalidEnt = nullptr;
        s_bPhaseDoorCallbackMissingLatched = false;
    }

    if (!s_hPhaseDoorCheckInvalidEnt)
    {
        s_hPhaseDoorCheckInvalidEnt = g_pServerScript->FindFunction(
            "PhaseDoor_CheckInvalidEnt", nullptr, nullptr);
        if (!s_hPhaseDoorCheckInvalidEnt)
        {
            if (!s_bPhaseDoorCallbackMissingLatched)
            {
                s_bPhaseDoorCallbackMissingLatched = true;
                Warning(eDLL_T::SERVER,
                    "[OPL-SRV] PhaseDoor_CheckInvalidEnt not found -- script entity veto idle\n");
            }
            return false;
        }
        Msg(eDLL_T::SERVER, "[OPL-SRV] PhaseDoor_CheckInvalidEnt resolved\n");
    }

    const HSCRIPT hInst = hitEnt->GetScriptInstance();
    if (!hInst)
        return false;

    return BreachTrace_CallBoolCallback(
        s_hPhaseDoorCheckInvalidEnt, ScriptVariant_t(hInst));
}

// Entity-type veto (doors/vehicles), mover-parent walk, then the script name table.
static bool ServerScript_PassesStandardEntityChecks(
    CBaseEntity* hitEnt,
    bool& outIsParentedToMover,
    const ServerObjectPlacementSettings& settings)
{
    outIsParentedToMover = false;
    if (!hitEnt)
        return true; // world/no entity.

    const char* const hitClassname = ServerScript_EntityClassname(hitEnt);
    if (ServerScript_ClassnameContains(hitClassname, "func_door") ||
        ServerScript_ClassnameContains(hitClassname, "prop_door") ||
        ServerScript_ClassnameContains(hitClassname, "prop_vehicle"))
    {
        if (s_serverPlacementDiagBudget > 0)
        {
            Msg(eDLL_T::SERVER,
                "[OPL-SRV] PassesStandardEntityChecks: vetoed hit entity classname='%s'\n",
                hitClassname ? hitClassname : "(null)");
            --s_serverPlacementDiagBudget;
        }
        return false;
    }

    CBaseEntity* cur = hitEnt;
    int safety = 0;
    while (cur && safety++ < 32)
    {
        const char* const curClassname = ServerScript_EntityClassname(cur);
        const bool isMover =
            ServerScript_ClassnameContains(curClassname, "func_mover") ||
            ServerScript_ClassnameContains(curClassname, "script_mover");
        if (isMover)
        {
            if (!settings.allowOnMovers)
            {
                if (s_serverPlacementDiagBudget > 0)
                {
                    Msg(eDLL_T::SERVER,
                        "[OPL-SRV] PassesStandardEntityChecks: vetoed, mover parent classname='%s' allowOnMovers=false\n",
                        curClassname ? curClassname : "(null)");
                    --s_serverPlacementDiagBudget;
                }
                return false;
            }
            outIsParentedToMover = true;
        }

        cur = ServerScript_EntityMoveParent(cur);
    }

    if (ServerScript_PhaseDoorCheckInvalidEnt(hitEnt))
    {
        if (s_serverPlacementDiagBudget > 0)
        {
            Msg(eDLL_T::SERVER,
                "[OPL-SRV] PassesStandardEntityChecks: PhaseDoor_CheckInvalidEnt veto classname='%s'\n",
                hitClassname ? hitClassname : "(null)");
            --s_serverPlacementDiagBudget;
        }
        return false;
    }

    return true;
}

//-----------------------------------------------------------------------------
// A strong sample consensus can return a normal 90 degrees off the coarse seed.
//-----------------------------------------------------------------------------
static Vector3D ServerScript_PickNormalFromSamples(
    const Vector3D& coarseNormal,
    const Vector3D& queryPos,
    const Vector3D* sampleNormals,
    const Vector3D* samplePoints,
    const int sampleCount)
{
    constexpr float kAgreeDot = 0.9f;

    // Pass 1: agreement among the fresh samples alone.
    if (sampleCount > 0)
    {
        bool foundPair = false;
        Vector3D pairAvg(0.0f, 0.0f, 0.0f);
        for (int i = 0; i + 1 < sampleCount; ++i)
        {
            for (int j = i + 1; j < sampleCount; ++j)
            {
                if (ServerScript_Dot(sampleNormals[i], sampleNormals[j]) <= kAgreeDot)
                    continue;

                if (foundPair)
                {
                    // Three-way agreement.
                    Vector3D sum = ServerScript_AddVector(
                        ServerScript_AddVector(sampleNormals[0], sampleNormals[1]), sampleNormals[2]);
                    return ServerScript_Normalized(ServerScript_ScaleVector(sum, 0.33333334f));
                }

                foundPair = true;
                pairAvg = ServerScript_ScaleVector(
                    ServerScript_AddVector(sampleNormals[i], sampleNormals[j]), 0.5f);
            }
        }
        if (foundPair)
            return ServerScript_Normalized(pairAvg);
    }

    // Pass 2: any sample that agrees with the coarse normal, blended with it.
    for (int k = 0; k < sampleCount; ++k)
    {
        if (ServerScript_Dot(coarseNormal, sampleNormals[k]) > kAgreeDot)
        {
            return ServerScript_Normalized(ServerScript_ScaleVector(
                ServerScript_AddVector(sampleNormals[k], coarseNormal), 0.5f));
        }
    }

    // Pass 3: cross of consecutive hit-point edges; returns the confirming vector, not the cross.
    for (int i = 0; i + 1 < sampleCount; ++i)
    {
        const Vector3D edgeCur = ServerScript_SubVector(samplePoints[i], queryPos);
        const Vector3D edgeNext = ServerScript_SubVector(samplePoints[i + 1], queryPos);
        const Vector3D edgeCross = ServerScript_CrossVector(edgeCur, edgeNext);

        const float widest = fmaxf(fmaxf(fabsf(edgeCross.x), fabsf(edgeCross.y)), fabsf(edgeCross.z));
        if (widest <= 0.01f)
            continue; // degenerate pair.

        const Vector3D candidate = ServerScript_Normalized(edgeCross);

        bool confirmed = false;
        for (int m = 0; m < sampleCount; ++m)
        {
            if (ServerScript_Dot(candidate, sampleNormals[m]) > kAgreeDot)
            {
                confirmed = true;
                return ServerScript_Normalized(sampleNormals[m]);
            }
        }
        NOTE_UNUSED(confirmed);

        if (ServerScript_Dot(candidate, coarseNormal) > kAgreeDot)
            return ServerScript_Normalized(coarseNormal);
    }

    // Pass 4: weighted average, unless every sample is perpendicular/antiparallel; then keep the coarse normal.
    bool useWeightedAverage = (sampleCount > 2);
    if (!useWeightedAverage)
    {
        bool allOrthoOrOpposite = true;
        for (int i = 0; i < sampleCount && !useWeightedAverage; ++i)
        {
            const float dc = ServerScript_Dot(sampleNormals[i], coarseNormal);
            if (fabsf(dc) > 0.1f && (dc + 1.0f) > FLT_EPSILON)
            {
                useWeightedAverage = true;
                break;
            }
            for (int j = i + 1; j < sampleCount; ++j)
            {
                if (fabsf(ServerScript_Dot(sampleNormals[i], sampleNormals[j])) > 0.1f)
                    allOrthoOrOpposite = false;
            }
        }
        if (!allOrthoOrOpposite)
            useWeightedAverage = true;

        if (!useWeightedAverage)
            return ServerScript_Normalized(coarseNormal);
    }

    const float weight = 1.0f / static_cast<float>(sampleCount + 1);
    Vector3D acc = ServerScript_ScaleVector(coarseNormal, weight);
    for (int i = 0; i < sampleCount; ++i)
        acc = ServerScript_AddVector(acc, ServerScript_ScaleVector(sampleNormals[i], weight));

    return ServerScript_Normalized(acc);
}

//-----------------------------------------------------------------------------
// Three probes from 32u back along the view. objectPos is read-only.
//-----------------------------------------------------------------------------
static Vector3D ServerScript_GetRefinedSurfaceNormal(
    const Vector3D& objectPos,
    const Vector3D& traceForward,
    const Vector3D& traceRight,
    const Vector3D& coarseNormal,
    CPlayer* owner)
{
    // 10, 130 and 250 degrees.
    static const float kProbeAngles[3] = { 0.174533f, 2.268928f, 4.363323f };
    constexpr float kRingRadius = 20.0f;
    constexpr float kForwardWeight = 32.0f;
    constexpr float kProbeLen = 48.0f;

    const Vector3D up = ServerScript_CrossVector(traceForward, traceRight);
    const Vector3D start = ServerScript_AddScaled(objectPos, traceForward, -kForwardWeight);

    Vector3D sampleNormals[3];
    Vector3D samplePoints[3];
    int sampleCount = 0;

    for (int i = 0; i < 3; ++i)
    {
        // Forward weight exceeds ring radius, so probes lean toward traceForward -- not a fan perpendicular to view.
        const float c = cosf(kProbeAngles[i]);
        const float s = sinf(kProbeAngles[i]);
        Vector3D raw = ServerScript_AddVector(
            ServerScript_ScaleVector(traceRight, kRingRadius * c),
            ServerScript_ScaleVector(up, kRingRadius * s));
        raw = ServerScript_AddScaled(raw, traceForward, kForwardWeight);

        const Vector3D end = ServerScript_AddScaled(
            objectPos, ServerScript_Normalized(raw), kProbeLen);

        trace_t probe;
        if (!ServerScript_PlacementTraceLine(start, end, owner, probe))
            continue;

        // fraction is the only acceptance test -- no startsolid/allsolid or
        // surface gate exists here.
        if (probe.fraction != 1.0f)
        {
            samplePoints[sampleCount] = probe.endpos;
            sampleNormals[sampleCount] = probe.plane.normal;
            ++sampleCount;
        }
    }

    const Vector3D result = ServerScript_PickNormalFromSamples(
        coarseNormal, objectPos, sampleNormals, samplePoints, sampleCount);

    // Committed only if it is genuinely unit-length; otherwise the caller's own
    // normal survives.
    if (fabsf(ServerScript_Dot(result, result) - 1.0f) < 0.01f)
        return result;

    return coarseNormal;
}

//-----------------------------------------------------------------------------
// WALL: 4 attempts with a single recovery. FLOOR/CEILING: 3.
//-----------------------------------------------------------------------------
static bool ServerScript_ExitValidation(
    Vector3D& portalExitPos,
    const Vector3D& exitNormal,
    const Vector3D& traceRight,
    const int portalDir, // 0=WALL, 1=FLOOR, else CEILING
    const Vector3D& traceDirection,
    const Vector3D& furthestValidPointFromExit,
    CPlayer* owner)
{
    // traceRight is passed by the real function but never dereferenced in it --
    // every reference is the compiler reusing the register as scratch.
    NOTE_UNUSED(traceRight);

    const Vector3D hullMins(-16.0f, -16.0f, -16.0f);
    const Vector3D hullMaxs(16.0f, 16.0f, 16.0f);
    constexpr float kBoxHeight = 48.0f;

    if (portalDir == 0)
    {
        bool recoveryUsed = false;
        int bumpCount = 0;

        for (;;)
        {
            const Vector3D downStart = ServerScript_AddScaled(portalExitPos, exitNormal, 16.0f);
            const Vector3D downEnd(downStart.x, downStart.y, downStart.z - kBoxHeight);

            trace_t downTrace;
            if (!ServerScript_TraceLine(downStart, downEnd, owner, downTrace, true,
                    &hullMins, &hullMaxs, kPlacementTraceMask))
            {
                return false;
            }

            if (downTrace.startsolid)
            {
                // One-shot recovery for the whole call, and only before any bump.
                if (recoveryUsed || bumpCount >= 1)
                {
                    if (s_serverPlacementDiagBudget > 0)
                    {
                        Msg(eDLL_T::SERVER,
                            "[OPL-SRV] exit-validation: wall down-trace startsolid, recovery spent (bump=%d)\n",
                            bumpCount);
                        --s_serverPlacementDiagBudget;
                    }
                    return false;
                }

                const Vector3D toFurthest = ServerScript_SubVector(furthestValidPointFromExit, portalExitPos);
                const float pullBack =
                    fminf(0.5f * sqrtf(ServerScript_Dot(toFurthest, toFurthest)), 150.0f) - 16.0f;

                trace_t findExitStartTrace;
                if (!ServerScript_TraceLine(
                        ServerScript_AddScaled(portalExitPos, traceDirection, pullBack),
                        portalExitPos, owner, findExitStartTrace, true,
                        &hullMins, &hullMaxs, kPlacementTraceMask))
                {
                    return false;
                }

                trace_t normalSpaceTrace;
                if (!ServerScript_PlacementTraceLine(portalExitPos,
                        ServerScript_AddScaled(portalExitPos, exitNormal, 166.0f),
                        owner, normalSpaceTrace))
                {
                    return false;
                }

                const float alongNormal = normalSpaceTrace.fraction * 166.0f - 16.0f;
                const bool tooShort = alongNormal < 0.0f;

                if (findExitStartTrace.startsolid && tooShort)
                    return false; // neither direction is viable.

                float candN;
                float candB;
                if (!tooShort)
                {
                    trace_t confirmTrace;
                    if (!ServerScript_TraceLine(
                            ServerScript_AddScaled(portalExitPos, exitNormal, alongNormal),
                            portalExitPos, owner, confirmTrace, true,
                            &hullMins, &hullMaxs, kPlacementTraceMask))
                    {
                        return false;
                    }
                    candN = (1.0f - confirmTrace.fraction) * alongNormal;
                    candB = findExitStartTrace.startsolid
                        ? 100000.0f
                        : (1.0f - findExitStartTrace.fraction) * pullBack;
                }
                else
                {
                    candN = 100000.0f;
                    candB = (1.0f - findExitStartTrace.fraction) * pullBack;
                }

                // Advance by whichever candidate is smaller, along its own axis.
                if (candB <= candN)
                    portalExitPos = ServerScript_AddScaled(portalExitPos, traceDirection, candB);
                else
                    portalExitPos = ServerScript_AddScaled(portalExitPos, exitNormal, candN);

                recoveryUsed = true;
                continue;
            }

            const float downFrac = downTrace.fraction;
            const float cappedDownFrac = fminf(downFrac, 0.5f);
            const Vector3D liftedBase(
                downStart.x, downStart.y, downStart.z - cappedDownFrac * kBoxHeight);

            trace_t upTrace;
            if (!ServerScript_TraceLine(liftedBase,
                    Vector3D(liftedBase.x, liftedBase.y, liftedBase.z + kBoxHeight),
                    owner, upTrace, true, &hullMins, &hullMaxs, kPlacementTraceMask))
            {
                return false;
            }

            if ((1.0f - downFrac) <= upTrace.fraction)
            {
                portalExitPos.z +=
                    upTrace.fraction * kBoxHeight - 24.0f - cappedDownFrac * kBoxHeight;
                return true;
            }

            if (bumpCount >= 3)
            {
                if (s_serverPlacementDiagBudget > 0)
                {
                    Msg(eDLL_T::SERVER,
                        "[OPL-SRV] exit-validation: wall clearance never met, downFrac=%.3f upFrac=%.3f\n",
                        downFrac, upTrace.fraction);
                    --s_serverPlacementDiagBudget;
                }
                return false;
            }

            ++bumpCount;
            portalExitPos = ServerScript_AddScaled(portalExitPos, exitNormal, 16.0f);
        }
    }

    // FLOOR (portalDir 1) and CEILING (anything else) share one branch, with the
    // probe direction taken from the sign.
    const float sign = (portalDir == 1) ? 1.0f : -1.0f;
    bool lateralComputed = false;
    Vector3D lateralAxis(0.0f, 0.0f, 0.0f);
    int nudgeCount = 0;

    trace_t boxTrace;
    for (;;)
    {
        if (!ServerScript_TraceLine(
                Vector3D(portalExitPos.x, portalExitPos.y, portalExitPos.z + sign * kBoxHeight),
                Vector3D(portalExitPos.x, portalExitPos.y, portalExitPos.z + sign * 16.0f),
                owner, boxTrace, true, &hullMins, &hullMaxs, kPlacementTraceMask))
        {
            return false;
        }

        if (!boxTrace.startsolid)
            break;

        ++nudgeCount;
        if (nudgeCount > 2)
        {
            if (s_serverPlacementDiagBudget > 0)
            {
                Msg(eDLL_T::SERVER,
                    "[OPL-SRV] exit-validation: floor/ceiling startsolid after %d nudges\n", nudgeCount);
                --s_serverPlacementDiagBudget;
            }
            return false;
        }

        if (!lateralComputed)
        {
            // Too close to flat for a meaningful lateral escape direction.
            if (exitNormal.z >= 0.97000003f)
                return false;

            lateralAxis = ServerScript_Normalized(Vector3D(exitNormal.x, exitNormal.y, 0.0f));
            lateralComputed = true;
        }

        portalExitPos = ServerScript_AddScaled(portalExitPos, lateralAxis, 16.0f);
    }

    if (boxTrace.fraction >= 1.0f)
        return true; // the box is already completely clear.

    const Vector3D savedOldPos = portalExitPos;
    const Vector3D firstHit = boxTrace.endpos;
    portalExitPos = firstHit;

    trace_t secondTrace;
    if (!ServerScript_TraceLine(
            Vector3D(portalExitPos.x, portalExitPos.y, portalExitPos.z + sign * 16.0f),
            Vector3D(portalExitPos.x, portalExitPos.y, portalExitPos.z + sign * kBoxHeight),
            owner, secondTrace, true, &hullMins, &hullMaxs, kPlacementTraceMask))
    {
        return false;
    }

    // Undo the snap's 16u bias.
    portalExitPos.z -= sign * 16.0f;

    if (secondTrace.fraction < 1.0f)
        return false; // must be a total miss.

    trace_t lineOfSight;
    if (!ServerScript_PlacementTraceLine(savedOldPos, firstHit, owner, lineOfSight))
        return false;

    if (lineOfSight.fraction < 1.0f || lineOfSight.allsolid || lineOfSight.startsolid)
        return false;

    return true;
}

// Entrance: fatal on fail. Exit: caller reverts. Phase-2 excess/accum is entrance-only.
// Slot order {+A,+B,-A,-B}; axisIdx and axisIdx+2 are opposites.
struct ServerEdgeDirectionInfo
{
    bool  clamped = false;
    float edgeDist = 0.0f;
    float overhang = 0.0f;
    float accum = 0.0f;
    float excess = 0.0f;
    Vector3D offsetVec;
};

static bool ServerScript_EdgeCorrection(
    Vector3D& objectPos,
    const Vector3D& surfaceNormal,
    const Vector3D& traceRight,
    const bool isExit,
    CPlayer* owner)
{
    // axisA is the negated cross in the real function; the {+/-axisA, +/-axisB} set is identical.
    const Vector3D axisA = ServerScript_ScaleVector(
        ServerScript_Normalized(ServerScript_CrossVector(surfaceNormal, traceRight)), -1.0f);
    const Vector3D axisB = ServerScript_Normalized(ServerScript_CrossVector(surfaceNormal, axisA));
    const Vector3D axes[2] = { axisA, axisB };

    // The surface offset every probe is lifted by, and the depth the phase 2
    // surface probe reaches back to.
    constexpr float kSurfaceLift = 15.0f;
    constexpr float kSurfaceProbeDepth = -20.0f;
    constexpr float kMaxCorrection = 34.0f;
    constexpr float kAnneal = 0.99000001f;
    // Minimum span across each axis for the portal to fit, phase 1.
    const float kMinAxisSpan[2] = { 40.0f, 32.0f };
    // Phase 2 gates, entrance only.
    const float kMinEdgeSpace[2] = { 23.0f, 23.0f };
    const float kMinAccumSpan[2] = { 25.0f, 25.0f };

    // Initial validation: straight out along the normal. Anything in the way,
    // or a solid start, and the surface is not usable at all.
    trace_t initialValidation;
    if (!ServerScript_PlacementTraceLine(
            objectPos, ServerScript_AddScaled(objectPos, surfaceNormal, kSurfaceLift),
            owner, initialValidation))
    {
        return false;
    }
    // The real test reads the 16-bit pair at CBaseTrace+0x38, i.e. allsolid
    // OR startsolid, not startsolid alone.
    if (initialValidation.fraction < 1.0f ||
        initialValidation.allsolid || initialValidation.startsolid)
    {
        if (s_serverPlacementDiagBudget > 0)
        {
            Msg(eDLL_T::SERVER,
                "[OPL-SRV] edge-correction: initial validation blocked frac=%.3f startsolid=%d pos=<%.1f %.1f %.1f> normal=<%.3f %.3f %.3f>\n",
                initialValidation.fraction, initialValidation.startsolid ? 1 : 0,
                objectPos.x, objectPos.y, objectPos.z,
                surfaceNormal.x, surfaceNormal.y, surfaceNormal.z);
            --s_serverPlacementDiagBudget;
        }
        return false;
    }

    ServerEdgeDirectionInfo dirs[4];
    for (int d = 0; d < 4; ++d)
    {
        const Vector3D& axis = axes[d % 2];
        const float sign = (d < 2) ? 1.0f : -1.0f;
        dirs[d].edgeDist = 24.0f * sign;
        dirs[d].offsetVec = ServerScript_ScaleVector(axis, 24.0f * sign);
    }

    Vector3D currentPos = objectPos;
    float adjust[2] = { 0.0f, 0.0f };
    Vector3D correction(0.0f, 0.0f, 0.0f);

    // Phase 1: per axis, probe both directions 48u. Reject if span cannot hold the portal.
    int loopCount = 0;
    bool anyPass = false;
    for (int axisIdx = 0; axisIdx < 2; ++axisIdx)
    {
        const Vector3D probeBase = ServerScript_AddVector(
            ServerScript_AddScaled(currentPos, surfaceNormal, kSurfaceLift), correction);

        int foundCount = 0;
        bool anyFound = false;
        bool multiFound = false;
        int lastFoundIdx = axisIdx;

        for (int d = axisIdx; d < 4; d += 2)
        {
            trace_t surfaceTrace;
            const Vector3D probeEnd = ServerScript_AddVector(
                probeBase, ServerScript_ScaleVector(dirs[d].offsetVec, 2.0f));
            if (!ServerScript_PlacementTraceLine(probeBase, probeEnd, owner, surfaceTrace))
                continue;

            if (surfaceTrace.fraction < 1.0f || surfaceTrace.startsolid)
                ++foundCount;

            dirs[d].overhang = 2.0f * surfaceTrace.fraction * dirs[d].edgeDist;

            if (surfaceTrace.fraction < 0.5f)
            {
                if (anyFound)
                    multiFound = true;
                anyFound = true;
                lastFoundIdx = d;
            }
        }

        const int opp = (axisIdx + 2) % 4;

        if (foundCount == 2 &&
            kMinAxisSpan[axisIdx] > (dirs[axisIdx].overhang - dirs[opp].overhang))
        {
            if (s_serverPlacementDiagBudget > 0)
            {
                Msg(eDLL_T::SERVER,
                    "[OPL-SRV] edge-correction: axis %d span %.1f below minimum %.1f (isExit=%d)\n",
                    axisIdx, dirs[axisIdx].overhang - dirs[opp].overhang,
                    kMinAxisSpan[axisIdx], isExit ? 1 : 0);
                --s_serverPlacementDiagBudget;
            }
            return false;
        }

        if (!anyFound)
        {
            if (!anyPass)
                continue;
            break;
        }

        ++loopCount;

        if ((foundCount == 2 &&
             (dirs[axisIdx].edgeDist + dirs[axisIdx].edgeDist) >
                 (dirs[axisIdx].overhang - dirs[opp].overhang)) ||
            multiFound)
        {
            adjust[axisIdx] = (dirs[axisIdx].overhang + dirs[opp].overhang) * 0.5f;
            dirs[axisIdx].edgeDist = dirs[axisIdx].overhang;
            dirs[axisIdx].clamped = true;
            dirs[opp].edgeDist = dirs[opp].overhang;
            dirs[opp].clamped = true;
        }
        else
        {
            adjust[lastFoundIdx % 2] =
                -(dirs[lastFoundIdx].edgeDist - dirs[lastFoundIdx].overhang);
        }

        correction = ServerScript_AddVector(
            ServerScript_ScaleVector(axes[0], adjust[0]),
            ServerScript_ScaleVector(axes[1], adjust[1]));

        if (ServerScript_Dot(correction, correction) > kMaxCorrection * kMaxCorrection)
        {
            if (s_serverPlacementDiagBudget > 0)
            {
                Msg(eDLL_T::SERVER,
                    "[OPL-SRV] edge-correction: phase 1 correction %.1f over cap %.1f\n",
                    sqrtf(ServerScript_Dot(correction, correction)), kMaxCorrection);
                --s_serverPlacementDiagBudget;
            }
            return false;
        }

        if (loopCount > 10)
        {
            Warning(eDLL_T::SERVER, "[OPL-SRV] edge-correction: phase 1 exceeded 10 iterations\n");
            return false;
        }

        anyPass = true;
        if (axisIdx == 1)
            break;
    }

    currentPos = ServerScript_AddVector(currentPos, correction);

    // Rebuild each direction's probe offset from its (possibly clamped) reach.
    for (int d = 0; d < 4; ++d)
    {
        const float a = adjust[d % 2];
        dirs[d].overhang -= a;
        if (dirs[d].clamped)
            dirs[d].edgeDist -= a;
        dirs[d].offsetVec = ServerScript_ScaleVector(axes[d % 2], dirs[d].edgeDist * kAnneal);
        dirs[d].accum = dirs[d].edgeDist;
    }

    // Phase 2: overhang. Probe 15u off the surface back 20u; a miss means overhang.
    Vector3D phase2Correction = correction;
    const Vector3D hullMins(0.0f, 0.0f, -35.0f);
    const Vector3D hullMaxs(0.0f, 0.0f, 0.0f);

    for (int axisIdx = 0; axisIdx < 2; ++axisIdx)
    {
        int foundCount = 0;
        bool anyFound = false;
        bool multiFound = false;
        int lastFoundIdx = axisIdx;

        for (int d = axisIdx; d < 4; d += 2)
        {
            dirs[d].accum = dirs[d].edgeDist;
            dirs[d].excess = 0.0f;

            const Vector3D lifted = ServerScript_AddScaled(
                ServerScript_AddVector(currentPos, dirs[d].offsetVec), surfaceNormal, kSurfaceLift);
            const Vector3D sunken = ServerScript_AddScaled(
                ServerScript_AddVector(currentPos, dirs[d].offsetVec), surfaceNormal, kSurfaceProbeDepth);

            trace_t surfaceTrace;
            if (!ServerScript_PlacementTraceLine(lifted, sunken, owner, surfaceTrace))
                continue;

            if (surfaceTrace.fraction != 1.0f)
                continue; // surface present under this probe -- no overhang here.

            const int opp = (d + 2) % 4;
            const Vector3D hullEnd = ServerScript_AddScaled(
                ServerScript_AddVector(currentPos, dirs[opp].offsetVec), surfaceNormal, kSurfaceLift);

            trace_t edgeTrace;
            if (!ServerScript_TraceLine(
                    lifted, hullEnd, owner, edgeTrace, true, &hullMins, &hullMaxs,
                    kPlacementTraceMask, &surfaceNormal))
            {
                continue;
            }

            const float reach = (dirs[opp].edgeDist - dirs[d].edgeDist) * edgeTrace.fraction;
            const float oppSlack = dirs[opp].overhang - dirs[opp].edgeDist;
            const float raw = reach - oppSlack;
            const bool firstSide = (d == axisIdx);

            dirs[d].excess = firstSide ? fminf(raw, 0.0f) : fmaxf(raw, 0.0f);

            if (!isExit && fabsf(dirs[d].excess) > kMinEdgeSpace[axisIdx])
            {
                if (s_serverPlacementDiagBudget > 0)
                {
                    Msg(eDLL_T::SERVER,
                        "[OPL-SRV] edge-correction: overhang excess %.1f over %.1f on dir %d\n",
                        dirs[d].excess, kMinEdgeSpace[axisIdx], d);
                    --s_serverPlacementDiagBudget;
                }
                return false;
            }

            const float step = firstSide ? fmaxf(reach, oppSlack) : fminf(reach, oppSlack);
            if (fabsf(step) < 0.0099999998f)
                continue;

            dirs[d].accum += step;
            ++foundCount;
            if (anyFound)
                multiFound = true;
            anyFound = true;
            lastFoundIdx = d;
        }

        const int opp = (axisIdx + 2) % 4;

        if (foundCount == 2 && !isExit &&
            kMinAccumSpan[axisIdx] > (dirs[axisIdx].accum - dirs[opp].accum))
        {
            if (s_serverPlacementDiagBudget > 0)
            {
                Msg(eDLL_T::SERVER,
                    "[OPL-SRV] edge-correction: axis %d corrected span %.1f below minimum %.1f\n",
                    axisIdx, dirs[axisIdx].accum - dirs[opp].accum, kMinAccumSpan[axisIdx]);
                --s_serverPlacementDiagBudget;
            }
            return false;
        }

        if (!anyFound)
            continue;

        if (multiFound)
            adjust[axisIdx] = (dirs[opp].accum + dirs[axisIdx].accum) * 0.5f;
        else
            adjust[lastFoundIdx % 2] =
                -(dirs[lastFoundIdx].edgeDist - dirs[lastFoundIdx].accum);

        phase2Correction = ServerScript_AddVector(
            correction,
            ServerScript_AddVector(
                ServerScript_ScaleVector(axes[0], adjust[0]),
                ServerScript_ScaleVector(axes[1], adjust[1])));

        if (ServerScript_Dot(phase2Correction, phase2Correction) > kMaxCorrection * kMaxCorrection)
        {
            if (s_serverPlacementDiagBudget > 0)
            {
                Msg(eDLL_T::SERVER,
                    "[OPL-SRV] edge-correction: phase 2 correction %.1f over cap %.1f\n",
                    sqrtf(ServerScript_Dot(phase2Correction, phase2Correction)), kMaxCorrection);
                --s_serverPlacementDiagBudget;
            }
            return false;
        }
    }

    objectPos = ServerScript_AddVector(objectPos, phase2Correction);
    return true;
}

// Mover-blocking hull trace at the exit (platform/door about to crush).
bool ServerScript_TraceForMoverBlocking(
    const Vector3D& portalExitPos,
    const Vector3D& surfaceNormal,
    CPlayer* owner)
{
    // Wall sweep: 48u tall, centred 16u off the surface. Floor/ceiling: 48u to 16u
    // along the normal's sign.
    constexpr float kUpDownDot = 0.70610678f;
    const float upDot = surfaceNormal.z;

    Vector3D probeStart;
    Vector3D probeEnd;
    if (fabsf(upDot) <= kUpDownDot)
    {
        const Vector3D base = ServerScript_AddScaled(portalExitPos, surfaceNormal, 16.0f);
        probeStart = Vector3D(base.x, base.y, base.z - 24.0f);
        probeEnd = Vector3D(base.x, base.y, base.z + 24.0f);
    }
    else
    {
        const float sign = (upDot > kUpDownDot) ? 1.0f : -1.0f;
        probeStart = Vector3D(portalExitPos.x, portalExitPos.y, portalExitPos.z + sign * 48.0f);
        probeEnd = Vector3D(portalExitPos.x, portalExitPos.y, portalExitPos.z + sign * 16.0f);
    }

    const Vector3D hullMins(-16.0f, -16.0f, -16.0f);
    const Vector3D hullMaxs(16.0f, 16.0f, 16.0f);

    trace_t moverTrace;
    if (!ServerScript_TraceLine(probeStart, probeEnd, owner, moverTrace, true, &hullMins, &hullMaxs,
            kPlacementTraceMask))
    {
        return true; // trace engine unavailable -- fail closed (treat as blocked).
    }

    if (!moverTrace.allsolid && !moverTrace.startsolid && moverTrace.fraction >= 0.98000002f)
        return false;

    // No player-blocking-mover filter here; classify the hit by classname.
    // No hit entity is worldspawn, which that filter skips.
    CBaseEntity* const hit = moverTrace.hit_entity;
    if (!hit)
        return false;

    const char* const hitClassname = ServerScript_EntityClassname(hit);
    return ServerScript_ClassnameContains(hitClassname, "func_mover") ||
           ServerScript_ClassnameContains(hitClassname, "script_mover") ||
           ServerScript_ClassnameContains(hitClassname, "func_door") ||
           ServerScript_ClassnameContains(hitClassname, "prop_door");
}

//-----------------------------------------------------------------------------
// startPoint is the eye hit pulled 32u back; re-trace 37u forward from outside.
//-----------------------------------------------------------------------------

// Phase-door placement levers (FindStartPoint, FindExit, CalcSpecial).
static ConVar sv_alter_portal_mid_reconfirm("sv_alter_portal_mid_reconfirm", "1", FCVAR_RELEASE,
    "When set, re-confirm mid-wall traces reject thin/near-side exit candidates.");
static ConVar sv_alter_portal_min_depth("sv_alter_portal_min_depth", "0", FCVAR_RELEASE,
    "Minimum entrance-to-exit distance for special placement; 0 disables.");
static ConVar sv_alter_portal_entrance_plane_guard("sv_alter_portal_entrance_plane_guard", "0", FCVAR_RELEASE,
    "Reject entrance if edge-correction pushes it behind its surface plane or into solid.");
static ConVar sv_alter_portal_exit_normal_hemisphere("sv_alter_portal_exit_normal_hemisphere", "0", FCVAR_RELEASE,
    "Keep the coarse exit normal when the refined sample flips hemisphere relative to it.");
static ConVar sv_alter_portal_reconfirm_diag("sv_alter_portal_reconfirm_diag", "0", FCVAR_DEVELOPMENTONLY,
    "Log mid-reconfirm probe raw back-face bytes and reject decisions.");

static int ServerScript_FindStartPointAndSurfaceNormal(
    const Vector3D& startPoint,
    const Vector3D& eyeDir,
    const Vector3D& eyeRight,
    CPlayer* owner,
    const ServerObjectPlacementSettings& settings,
    Vector3D& objectPos,
    Vector3D& surfaceNormal,
    void*& entityToAttachTo)
{
    constexpr float kSightTraceLen = 37.0f;

    trace_t sightTrace;
    if (!ServerScript_PlacementTraceLine(
            startPoint, ServerScript_AddScaled(startPoint, eyeDir, kSightTraceLen), owner, sightTrace, true))
    {
        ServerScript_PlacementDiag("special sight trace engine missing", nullptr, owner);
        return OPSPR_OTHER;
    }

    objectPos = sightTrace.endpos;
    if (sightTrace.fraction >= 1.0f)
        return OPSPR_OTHER;

    surfaceNormal = ServerScript_Normalized(sightTrace.plane.normal);

    // The real function records the entity-check result but still runs the
    // normal refine before returning it, so keep that ordering.
    int result = OPSPR_SUCCESS;
    bool isParentedToMover = false;
    if (!ServerScript_PassesStandardEntityChecks(sightTrace.hit_entity, isParentedToMover, settings))
        result = OPSPR_ENTRANCE_INVALID_OBJECT;
    else if (isParentedToMover)
        entityToAttachTo = ServerScript_PlacementParentFromTrace(sightTrace);

    // objectPos is read-only; only the normal is refined.
    surfaceNormal = ServerScript_GetRefinedSurfaceNormal(objectPos, eyeDir, eyeRight, surfaceNormal, owner);

    if (result != OPSPR_SUCCESS)
        return result;

    if (fabsf(ServerScript_Dot(surfaceNormal, surfaceNormal) - 1.0f) >= 0.01f)
        return OPSPR_OTHER;

    // Entrance-side edge correction is FATAL in the real chain (the exit-side
    // call is not -- it reverts and continues).
    const Vector3D savedObjectPos = objectPos;
    if (!ServerScript_EdgeCorrection(objectPos, surfaceNormal, eyeRight, false, owner))
    {
        if (s_serverPlacementDiagBudget > 0)
        {
            Msg(eDLL_T::SERVER,
                "[OPL-SRV] entrance edge-correction failed entrance=<%.1f %.1f %.1f> normal=<%.3f %.3f %.3f>\n",
                objectPos.x, objectPos.y, objectPos.z,
                surfaceNormal.x, surfaceNormal.y, surfaceNormal.z);
            --s_serverPlacementDiagBudget;
        }
        return OPSPR_ENTRANCE_BLOCKED;
    }

    const float alongNormal = ServerScript_Dot(
        ServerScript_SubVector(objectPos, savedObjectPos), surfaceNormal);
    if (s_serverPlacementDiagBudget > 0)
    {
        Msg(eDLL_T::SERVER,
            "[OPL-SRV] entrance edge-correction pre=<%.1f %.1f %.1f> post=<%.1f %.1f %.1f> N=<%.3f %.3f %.3f> along=%.3f\n",
            savedObjectPos.x, savedObjectPos.y, savedObjectPos.z,
            objectPos.x, objectPos.y, objectPos.z,
            surfaceNormal.x, surfaceNormal.y, surfaceNormal.z,
            alongNormal);
        --s_serverPlacementDiagBudget;
    }

    // A correction that sinks the entrance into the wall collapses the back-trace.
    // Fail closed rather than restore a pre-correction pose.
    if (sv_alter_portal_entrance_plane_guard.GetBool())
    {
        bool badEntrance = false;
        const char* reason = nullptr;
        if (alongNormal < -0.1f)
        {
            badEntrance = true;
            reason = "behind plane";
        }
        else
        {
            trace_t solidCheck;
            if (ServerScript_PlacementTraceLine(objectPos, objectPos, owner, solidCheck) &&
                (solidCheck.startsolid || solidCheck.allsolid))
            {
                badEntrance = true;
                reason = "in solid";
            }
        }

        if (badEntrance)
        {
            if (s_serverPlacementDiagBudget > 0)
            {
                Msg(eDLL_T::SERVER,
                    "[OPL-SRV] entrance edge-correction rejected (%s) pre=<%.1f %.1f %.1f> post=<%.1f %.1f %.1f> N=<%.3f %.3f %.3f> along=%.3f\n",
                    reason,
                    savedObjectPos.x, savedObjectPos.y, savedObjectPos.z,
                    objectPos.x, objectPos.y, objectPos.z,
                    surfaceNormal.x, surfaceNormal.y, surfaceNormal.z,
                    alongNormal);
                --s_serverPlacementDiagBudget;
            }
            return OPSPR_ENTRANCE_BLOCKED;
        }
    }

    return OPSPR_SUCCESS;
}

//-----------------------------------------------------------------------------
// Walk 42u steps: long-forward, then back to the entrance. Far-face hit only.
//-----------------------------------------------------------------------------
static int ServerScript_FindExit(
    const Vector3D& objectPos,
    const Vector3D& traceDirection,
    const Vector3D& traceRight,
    CPlayer* owner,
    const ServerObjectPlacementSettings& settings,
    Vector3D& exitPoint,
    Vector3D& exitNormal,
    Vector3D& furthestValidPointFromExit,
    void*& exitParent)
{
    constexpr float kStep = 42.0f;
    // 1181.0605^2 -- the real over-trace escape hatch behind
    // object_placement_special_allow_over_tracing_client has no server mirror.
    constexpr float kMaxWallThicknessSqr = 1395004.1f;
    constexpr float kMinWallThicknessSqr = 1764.0f; // 42^2, measured BETWEEN the two traces.
    constexpr int kMaxLoops = 30;                   // object_placement_special_find_exit_max_loops.

    bool overDistance = false;
    bool prevAllsolid = false;
    bool accepted = false;

    Vector3D cur = ServerScript_AddScaled(objectPos, traceDirection, kStep);
    const Vector3D endPt = ServerScript_AddScaled(cur, traceDirection, kPlacementLongTrace);

    int loop = 0;
    for (; loop < kMaxLoops; ++loop)
    {
        if (prevAllsolid)
            return OPSPR_OTHER;

        const Vector3D fromEntrance = ServerScript_SubVector(cur, objectPos);
        if (ServerScript_Dot(fromEntrance, fromEntrance) > kMaxWallThicknessSqr)
        {
            overDistance = true;
            break;
        }

        trace_t depthTrace;
        if (!ServerScript_PlacementTraceLine(cur, endPt, owner, depthTrace))
        {
            ServerScript_PlacementDiag("special exit trace engine missing", nullptr, owner);
            return OPSPR_OTHER;
        }

        // Advance on allsolid; remembered flag is surface.flags bit 2.
        // (startsolid is a different field -- do not use it here.)
        if (depthTrace.allsolid)
        {
            cur = ServerScript_AddScaled(depthTrace.endpos, traceDirection, kStep);
            continue;
        }

        prevAllsolid = (depthTrace.surface.flags & 4) != 0;

        trace_t backTrace;
        if (!ServerScript_PlacementTraceLine(depthTrace.endpos, objectPos, owner, backTrace))
        {
            ServerScript_PlacementDiag("special exit backtrace engine missing", nullptr, owner);
            return OPSPR_OTHER;
        }

        if (backTrace.fraction < 1.0f)
        {
            const Vector3D thickness = ServerScript_SubVector(backTrace.endpos, depthTrace.endpos);
            const float thicknessSqr = ServerScript_Dot(thickness, thickness);
            const Vector3D probeExit = ServerScript_AddScaled(backTrace.endpos, traceDirection, 1.0f);
            const Vector3D probeDepthVec = ServerScript_SubVector(probeExit, objectPos);
            const float probeDepth = sqrtf(ServerScript_Dot(probeDepthVec, probeDepthVec));

            if (s_serverPlacementDiagBudget > 0)
            {
                Msg(eDLL_T::SERVER,
                    "[OPL-SRV] exit probe loop=%d thickness=%.1f (need>=42.0) far=<%.1f %.1f %.1f> depth=%.1f\n",
                    loop, sqrtf(thicknessSqr),
                    backTrace.endpos.x, backTrace.endpos.y, backTrace.endpos.z,
                    probeDepth);
                --s_serverPlacementDiagBudget;
            }

            if (thicknessSqr >= kMinWallThicknessSqr)
            {
                bool isParentedToMover = false;
                if (!ServerScript_PassesStandardEntityChecks(backTrace.hit_entity, isParentedToMover, settings))
                    return OPSPR_EXIT_INVALID_OBJECT;

                bool reconfirmRejected = false;
                if (sv_alter_portal_mid_reconfirm.GetBool())
                {
                    // Mid-point between the near (depth) and far (back) hits.
                    const Vector3D mid = ServerScript_ScaleVector(
                        ServerScript_AddVector(backTrace.endpos, depthTrace.endpos), 0.5f);

                    const int depthBF = static_cast<int>(
                        *(reinterpret_cast<const uint8_t*>(&depthTrace) + TRACE_OFF_HIT_BACKFACE));
                    const int backBF = static_cast<int>(
                        *(reinterpret_cast<const uint8_t*>(&backTrace) + TRACE_OFF_HIT_BACKFACE));
                    int fwdBF = -1;
                    int fwdFlags = -1;
                    int backBF2 = -1;
                    int backFlags2 = -1;
                    constexpr float kEntranceCollapseSqr = 4.0f;
                    const bool backCollapsed = ServerScript_TraceEndedNearPoint(
                        backTrace, objectPos, kEntranceCollapseSqr);

                    // Mid-reconfirm probes are HIGH; the coarse pair stays NORMAL.
                    if (ServerScript_TraceHitBackFace(depthTrace))
                    {
                        trace_t hi;
                        if (ServerScript_PlacementTraceLine(
                                mid, endPt, owner, hi, false, kTraceDetailLevel_High))
                        {
                            fwdBF = static_cast<int>(
                                *(reinterpret_cast<const uint8_t*>(&hi) + TRACE_OFF_HIT_BACKFACE));
                            fwdFlags = static_cast<int>(ServerScript_TraceSurfFlags(hi));
                            if (ServerScript_TraceHitBackFace(hi) &&
                                (ServerScript_TraceSurfFlags(hi) & 8) == 0)
                            {
                                reconfirmRejected = true;
                            }
                        }
                    }

                    if (ServerScript_TraceHitBackFace(backTrace) || backCollapsed)
                    {
                        trace_t hi;
                        if (ServerScript_PlacementTraceLine(
                                mid, objectPos, owner, hi, false, kTraceDetailLevel_High))
                        {
                            backBF2 = static_cast<int>(
                                *(reinterpret_cast<const uint8_t*>(&hi) + TRACE_OFF_HIT_BACKFACE));
                            backFlags2 = static_cast<int>(ServerScript_TraceSurfFlags(hi));
                            const bool hiCollapsed = ServerScript_TraceEndedNearPoint(
                                hi, objectPos, kEntranceCollapseSqr);
                            if ((ServerScript_TraceHitBackFace(hi) || hiCollapsed) &&
                                (ServerScript_TraceSurfFlags(hi) & 8) == 0)
                            {
                                reconfirmRejected = true;
                            }
                        }
                    }

                    if (sv_alter_portal_reconfirm_diag.GetBool() && s_serverPlacementDiagBudget > 0)
                    {
                        Msg(eDLL_T::SERVER,
                            "[OPL-SRV] mid-reconfirm loop=%d depthBF=%d backBF=%d collapse=%d fwdBF=%d fwdFlags=0x%04X backBF2=%d backFlags2=0x%04X rejected=%d mid=<%.1f %.1f %.1f>\n",
                            loop, depthBF, backBF, backCollapsed ? 1 : 0,
                            fwdBF, static_cast<unsigned int>(fwdFlags),
                            backBF2, static_cast<unsigned int>(backFlags2),
                            reconfirmRejected ? 1 : 0,
                            mid.x, mid.y, mid.z);
                        --s_serverPlacementDiagBudget;
                    }
                }

                if (reconfirmRejected)
                {
                    cur = ServerScript_AddScaled(depthTrace.endpos, traceDirection, kStep);
                    continue;
                }

                // Deviation from the placement chain kept as a live A/B lever.
                const float minDepth = sv_alter_portal_min_depth.GetFloat();
                if (minDepth > 0.0f && probeDepth < minDepth)
                {
                    if (s_serverPlacementDiagBudget > 0)
                    {
                        Msg(eDLL_T::SERVER,
                            "[OPL-SRV] min-depth reject loop=%d depth=%.1f need=%.1f far=<%.1f %.1f %.1f>\n",
                            loop, probeDepth, minDepth,
                            backTrace.endpos.x, backTrace.endpos.y, backTrace.endpos.z);
                        --s_serverPlacementDiagBudget;
                    }
                    cur = ServerScript_AddScaled(depthTrace.endpos, traceDirection, kStep);
                    continue;
                }

                const Vector3D candidate = probeExit;
                exitParent = isParentedToMover ? ServerScript_PlacementParentFromTrace(backTrace) : nullptr;

                // CheckEntombmentAlongLine is true when entombed; invert for
                // the safe-exit accept. No budget.
                bool needsNavmeshCheck = false;
                if (!ServerScript_CheckEntombmentAlongLine(
                        candidate, depthTrace.endpos, traceDirection, traceRight, owner, needsNavmeshCheck))
                {
                    exitPoint = candidate;
                    exitNormal = ServerScript_Normalized(backTrace.plane.normal);
                    furthestValidPointFromExit = depthTrace.endpos;
                    accepted = true;

                    if (probeDepth < 8.0f && s_serverPlacementShallowLogBudget > 0)
                    {
                        Warning(eDLL_T::SERVER,
                            "[OPL-SRV] shallow exit accepted depth=%.1f thickness=%.1f entrance=<%.1f %.1f %.1f> exit=<%.1f %.1f %.1f>\n",
                            probeDepth, sqrtf(thicknessSqr),
                            objectPos.x, objectPos.y, objectPos.z,
                            candidate.x, candidate.y, candidate.z);
                        --s_serverPlacementShallowLogBudget;
                    }
                    break;
                }
            }
        }

        cur = ServerScript_AddScaled(depthTrace.endpos, traceDirection, kStep);
    }

    if (accepted)
        return overDistance ? OPSPR_TOO_DEEP : OPSPR_SUCCESS;
    if (loop >= kMaxLoops)
        return OPSPR_TOO_COMPLEX;
    return OPSPR_TOO_DEEP;
}

static bool ServerScript_CalcSpecialPlacement(
    void* pWeapon,
    CPlayer* owner,
    const Vector3D& eyeOrigin,
    const Vector3D& eyeDir,
    const Vector3D& eyeRight,
    ServerObjectPlacementState& state)
{
    const ServerObjectPlacementSettings& settings = ServerScript_GetPlacementSettings(pWeapon);
    const float distanceMax = ServerScript_ObjectPlacementDistance(pWeapon, true);

    trace_t eyeTrace;
    const Vector3D longEnd = ServerScript_AddScaled(eyeOrigin, eyeDir, kPlacementLongTrace);
    if (!ServerScript_PlacementTraceLine(eyeOrigin, longEnd, owner, eyeTrace, true))
    {
        ServerScript_PlacementDiag("special trace engine missing", pWeapon, owner);
        state.specialResult = OPSPR_OTHER;
        return false;
    }

    if (eyeTrace.fraction >= 1.0f)
    {
        state.specialResult = OPSPR_TOO_FAR;
        return false;
    }

    const Vector3D toHit = ServerScript_SubVector(eyeTrace.endpos, eyeOrigin);
    const float distSqr = ServerScript_Dot(toHit, toHit);
    if (distSqr > distanceMax * distanceMax)
    {
        // The real function still publishes the hit pose on this failure.
        ServerScript_SetPlacementFromTrace(state, eyeTrace);
        state.specialResult = OPSPR_TOO_FAR;
        return false;
    }

    const Vector3D adjustedStartPoint =
        ServerScript_AddScaled(eyeTrace.endpos, eyeDir, -kPlacementPullBack);

    if (distSqr < kPlacementPullBack * kPlacementPullBack)
    {
        // Closer than the pull-back distance: the pulled-back point is only
        // usable if nothing sits between it and the hit.
        trace_t clearance;
        if (!ServerScript_PlacementTraceLine(eyeTrace.endpos, adjustedStartPoint, owner, clearance) ||
            clearance.fraction < 1.0f || clearance.startsolid)
        {
            if (s_serverPlacementDiagBudget > 0)
            {
                Msg(eDLL_T::SERVER,
                    "[OPL-SRV] entrance too close and pull-back blocked dist=%.1f entrance=<%.1f %.1f %.1f>\n",
                    sqrtf(distSqr), eyeTrace.endpos.x, eyeTrace.endpos.y, eyeTrace.endpos.z);
                --s_serverPlacementDiagBudget;
            }
            state.origin = eyeTrace.endpos;
            state.specialResult = OPSPR_ENTRANCE_BLOCKED;
            return false;
        }
    }

    Vector3D entranceOrigin;
    Vector3D entranceNormal;
    void* entranceParent = nullptr;
    const int startResult = ServerScript_FindStartPointAndSurfaceNormal(
        adjustedStartPoint, eyeDir, eyeRight, owner, settings,
        entranceOrigin, entranceNormal, entranceParent);
    if (startResult != OPSPR_SUCCESS)
    {
        state.specialResult = startResult;
        return false;
    }

    state.origin = entranceOrigin;
    VectorAngles(entranceNormal, state.angles.AsQAngle());
    state.parent = entranceParent;

    const int entrancePortalDir = ServerScript_ClassifyPortalDir(entranceNormal);

    if (!ServerScript_PostValidatePlacement(entranceOrigin, entranceNormal, entrancePortalDir, owner))
    {
        state.specialResult = OPSPR_ENTRANCE_UNSAFE;
        return false;
    }

    if (!ServerScript_MayPlaceObjectAtPoint(entranceOrigin, nullptr))
    {
        state.specialResult = OPSPR_ENTRANCE_INVALID_SPACE;
        return false;
    }

    // Entrance: one unit off the surface; a hit rejects.
    if (ServerScript_TraceForMoverBlocking(
            ServerScript_AddScaled(entranceOrigin, entranceNormal, 1.0f), entranceNormal, owner))
    {
        if (s_serverPlacementDiagBudget > 0)
        {
            Msg(eDLL_T::SERVER,
                "[OPL-SRV] entrance mover-blocked entrance=<%.1f %.1f %.1f>\n",
                entranceOrigin.x, entranceOrigin.y, entranceOrigin.z);
            --s_serverPlacementDiagBudget;
        }
        state.specialResult = OPSPR_ENTRANCE_BLOCKED;
        return false;
    }

    Vector3D exitPoint;
    Vector3D exitNormal;
    Vector3D furthestValidPointFromExit;
    void* exitParent = nullptr;
    const int exitResult = ServerScript_FindExit(
        entranceOrigin, eyeDir, eyeRight, owner, settings,
        exitPoint, exitNormal, furthestValidPointFromExit, exitParent);
    if (exitResult != OPSPR_SUCCESS)
    {
        if (s_serverPlacementDiagBudget > 0)
        {
            Msg(eDLL_T::SERVER,
                "[OPL-SRV] exit search failed result=%d entrance=<%.1f %.1f %.1f>\n",
                exitResult, entranceOrigin.x, entranceOrigin.y, entranceOrigin.z);
            --s_serverPlacementDiagBudget;
        }
        state.specialResult = exitResult;
        return false;
    }

    const Vector3D savedExitPoint = exitPoint;

    // Exit samples from inside looking back out; both basis vectors are negated.
    const Vector3D negEyeDir = ServerScript_ScaleVector(eyeDir, -1.0f);
    const Vector3D negEyeRight = ServerScript_ScaleVector(eyeRight, -1.0f);
    const Vector3D coarseExitNormal = exitNormal;
    exitNormal = ServerScript_GetRefinedSurfaceNormal(exitPoint, negEyeDir, negEyeRight, exitNormal, owner);

    if (s_serverPlacementDiagBudget > 0)
    {
        Msg(eDLL_T::SERVER,
            "[OPL-SRV] exit normal coarse=<%.3f %.3f %.3f> refined=<%.3f %.3f %.3f> dotCoarse=%.3f entranceN=<%.3f %.3f %.3f>\n",
            coarseExitNormal.x, coarseExitNormal.y, coarseExitNormal.z,
            exitNormal.x, exitNormal.y, exitNormal.z,
            ServerScript_Dot(coarseExitNormal, exitNormal),
            entranceNormal.x, entranceNormal.y, entranceNormal.z);
        --s_serverPlacementDiagBudget;
    }

    // Keep the coarse far-face normal when sample consensus flips into the wall.
    if (sv_alter_portal_exit_normal_hemisphere.GetBool() &&
        ServerScript_Dot(coarseExitNormal, exitNormal) <= 0.0f)
    {
        if (s_serverPlacementDiagBudget > 0)
        {
            Msg(eDLL_T::SERVER,
                "[OPL-SRV] refined exit normal discarded (flipped hemisphere) coarse=<%.3f %.3f %.3f> refined=<%.3f %.3f %.3f>\n",
                coarseExitNormal.x, coarseExitNormal.y, coarseExitNormal.z,
                exitNormal.x, exitNormal.y, exitNormal.z);
            --s_serverPlacementDiagBudget;
        }
        exitNormal = coarseExitNormal;
    }

    if (ServerScript_Dot(exitNormal, entranceNormal) > 0.70710677f)
    {
        if (s_serverPlacementDiagBudget > 0)
        {
            Msg(eDLL_T::SERVER,
                "[OPL-SRV] exit normal aligned entrance=<%.3f %.3f %.3f> exit=<%.3f %.3f %.3f>\n",
                entranceNormal.x, entranceNormal.y, entranceNormal.z,
                exitNormal.x, exitNormal.y, exitNormal.z);
            --s_serverPlacementDiagBudget;
        }
        state.specialResult = OPSPR_EXIT_NORMAL_ALIGNED;
        return false;
    }

    // Exit: negated right vector; failure reverts rather than rejecting.
    if (!ServerScript_EdgeCorrection(exitPoint, exitNormal, negEyeRight, true, owner))
        exitPoint = savedExitPoint;

    const int exitPortalDir = ServerScript_ClassifyPortalDir(exitNormal);

    if (!ServerScript_ExitValidation(
            exitPoint, exitNormal, eyeRight, exitPortalDir, eyeDir, furthestValidPointFromExit, owner))
    {
        if (s_serverPlacementDiagBudget > 0)
        {
            Msg(eDLL_T::SERVER,
                "[OPL-SRV] exit hull validation failed exit=<%.1f %.1f %.1f> normal=<%.3f %.3f %.3f> portalDir=%d\n",
                exitPoint.x, exitPoint.y, exitPoint.z,
                exitNormal.x, exitNormal.y, exitNormal.z, exitPortalDir);
            --s_serverPlacementDiagBudget;
        }
        state.specialResult = OPSPR_EXIT_BLOCKED;
        return false;
    }

    // Exit mover-block only warns; it does not reject.
    if (settings.moverBlockerValidation &&
        ServerScript_TraceForMoverBlocking(exitPoint, exitNormal, owner))
    {
        Warning(eDLL_T::SERVER,
            "[OPL-SRV] exit is mover-blocked but placement continues exit=<%.1f %.1f %.1f>\n",
            exitPoint.x, exitPoint.y, exitPoint.z);
    }

    if (!ServerScript_MayPlaceObjectAtPoint(exitPoint, exitParent))
    {
        state.specialResult = OPSPR_EXIT_INVALID_SPACE;
        return false;
    }

    if (!ServerScript_PostValidatePlacement(exitPoint, exitNormal, exitPortalDir, owner))
    {
        state.specialResult = OPSPR_EXIT_UNSAFE;
        return false;
    }

    state.special = true;
    state.specialOrigin = exitPoint;
    VectorAngles(exitNormal, state.specialAngles.AsQAngle());
    state.specialParent = exitParent;
    state.specialResult = OPSPR_SUCCESS;

    if (s_serverPlacementDiagBudget > 0)
    {
        Msg(eDLL_T::SERVER,
            "[OPL-SRV] placement accepted entrance=<%.1f %.1f %.1f> N=<%.3f %.3f %.3f> exit=<%.1f %.1f %.1f> N=<%.3f %.3f %.3f> depth=%.1f preValidation=<%.1f %.1f %.1f> dir=%d\n",
            entranceOrigin.x, entranceOrigin.y, entranceOrigin.z,
            entranceNormal.x, entranceNormal.y, entranceNormal.z,
            exitPoint.x, exitPoint.y, exitPoint.z,
            exitNormal.x, exitNormal.y, exitNormal.z,
            sqrtf(ServerScript_Dot(
                ServerScript_SubVector(exitPoint, entranceOrigin),
                ServerScript_SubVector(exitPoint, entranceOrigin))),
            savedExitPoint.x, savedExitPoint.y, savedExitPoint.z, exitPortalDir);
        --s_serverPlacementDiagBudget;
    }
    return true;
}

//=============================================================================
// Every Depth/Width/Height parameter is a HALF extent of the model bounds.
//=============================================================================

// A surface the object may rest on: world, brushwork, or a mover it can parent
// to. Anything else is a blocker the pivot walks away from.
static bool ServerScript_PlacementSurfaceAccepts(void* hitEntity)
{
    if (!hitEntity)
        return true;

    if (g_serverEntityList && hitEntity == g_serverEntityList->LookupEntityByNetworkIndex(0))
        return true;

    const char* const classname =
        ServerScript_EntityClassname(reinterpret_cast<CBaseEntity*>(hitEntity));
    return ServerScript_ClassnameContains(classname, "func_brush") ||
        ServerScript_ClassnameContains(classname, "func_mover") ||
        ServerScript_ClassnameContains(classname, "script_mover");
}

static bool ServerScript_PlacementTraceHit(const trace_t& tr)
{
    return tr.fraction < 1.0f || tr.allsolid || tr.startsolid;
}

static bool ServerScript_PlacementLine(
    const Vector3D& start,
    const Vector3D& end,
    CPlayer* owner,
    const unsigned int mask,
    trace_t& outTrace)
{
    return ServerScript_TraceLine(start, end, owner, outTrace, false, nullptr, nullptr, mask);
}

// Swept hull oriented along 'up' rather than world Z.
static bool ServerScript_PlacementSweep(
    const Vector3D& start,
    const Vector3D& end,
    const Vector3D& hullMins,
    const Vector3D& hullMaxs,
    const Vector3D& up,
    CPlayer* owner,
    const unsigned int mask,
    trace_t& outTrace)
{
    return ServerScript_TraceLine(
        start, end, owner, outTrace, true, &hullMins, &hullMaxs, mask, &up);
}

static bool ServerScript_PlacementFindEdgeOfCliff(
    trace_t& trDown,
    Vector3D& forwardEndPos,
    Vector3D& origin,
    void*& hitEntity,
    const Vector3D& forwardStartPos,
    const Vector3D& eyeDir,
    const float dropToGroundOffsetMax,
    CPlayer* owner,
    const unsigned int mask)
{
    // Sweep back toward the eye dragging a zero-width feeler that hangs
    // dropToGroundOffsetMax below the ray -- this finds the lip of the drop.
    const Vector3D feelerMins(0.0f, 0.0f, -dropToGroundOffsetMax);
    const Vector3D feelerMaxs(0.0f, 0.0f, 0.0f);
    const Vector3D worldUp(0.0f, 0.0f, 1.0f);

    trace_t edgeTrace;
    if (!ServerScript_PlacementSweep(forwardEndPos, forwardStartPos,
        feelerMins, feelerMaxs, worldUp, owner, mask, edgeTrace))
    {
        return false;
    }

    if (!ServerScript_PlacementTraceHit(edgeTrace))
        return false;

    forwardEndPos = ServerScript_AddScaled(edgeTrace.endpos, eyeDir, -10.0f);

    const Vector3D dropEnd(forwardEndPos.x, forwardEndPos.y,
        forwardEndPos.z - dropToGroundOffsetMax);
    if (!ServerScript_PlacementLine(forwardEndPos, dropEnd, owner, mask, trDown))
        return false;

    if (!ServerScript_PlacementTraceHit(trDown))
        return false;

    origin = trDown.endpos;
    hitEntity = trDown.hit_entity;
    return true;
}

static bool ServerScript_PlacementMoveAwayFromBlockers(
    trace_t& trDown,
    Vector3D& origin,
    Vector3D& forwardEndPos,
    void*& hitEntity,
    const Vector3D& forwardStart,
    const float dropToGroundOffsetMax,
    CPlayer* owner,
    const unsigned int mask)
{
    const Vector3D feelerMins(0.0f, 0.0f, -dropToGroundOffsetMax);
    const Vector3D feelerMaxs(0.0f, 0.0f, 0.0f);
    const Vector3D worldUp(0.0f, 0.0f, 1.0f);

    for (int i = 0; !ServerScript_PlacementSurfaceAccepts(hitEntity); ++i)
    {
        if (i >= 3)
            return false;

        // Sweep can stop on a nearer surface and only backs the pivot off further, never past it.
        trace_t blockerTrace;
        if (!ServerScript_PlacementSweep(forwardStart, forwardEndPos,
            feelerMins, feelerMaxs, worldUp, owner, mask, blockerTrace))
        {
            return false;
        }

        const float backOff = blockerTrace.fraction - 0.01f;
        forwardEndPos = ServerScript_AddVector(forwardStart,
            ServerScript_ScaleVector(
                ServerScript_SubVector(forwardEndPos, forwardStart), backOff));

        const Vector3D dropEnd(forwardEndPos.x, forwardEndPos.y,
            forwardEndPos.z - dropToGroundOffsetMax);
        if (!ServerScript_PlacementLine(forwardEndPos, dropEnd, owner, mask, trDown))
            return false;

        origin = trDown.endpos;
        if (!ServerScript_PlacementTraceHit(trDown) || trDown.startsolid)
            return false;

        hitEntity = trDown.hit_entity;
    }

    return true;
}

static bool ServerScript_PlacementIsSectionOffCliff(
    const Vector3D& directionToSide,
    const float distanceToSide,
    const Vector3D& capsuleDir,
    const float capsuleLength,
    const Vector3D& normal,
    const Vector3D& origin,
    const float percentOffLedgeMax,
    const float groundPenetrationMax,
    const float maxHeightToGround,
    CPlayer* owner,
    const unsigned int mask)
{
    const float allowedOffLedge = (1.0f - percentOffLedgeMax) * distanceToSide;
    const float radius = (distanceToSide - allowedOffLedge) * 0.5f;

    const Vector3D base = ServerScript_AddScaled(origin, normal, groundPenetrationMax);
    const Vector3D probeStart = ServerScript_AddScaled(base, normal, radius);
    const Vector3D sideStart = ServerScript_AddScaled(
        probeStart, directionToSide, radius + allowedOffLedge);

    trace_t lateralTrace;
    if (!ServerScript_PlacementLine(probeStart, sideStart, owner, mask, lateralTrace))
        return false;

    if (ServerScript_PlacementTraceHit(lateralTrace))
        return false; // Blocked sideways -- there is geometry there, not a ledge.

    const Vector3D sideEnd = ServerScript_AddScaled(
        sideStart, normal, -(maxHeightToGround + groundPenetrationMax));
    const Vector3D hullMins(-radius, -radius, -capsuleLength);
    const Vector3D hullMaxs(radius, radius, capsuleLength);

    trace_t downTrace;
    if (!ServerScript_PlacementSweep(sideStart, sideEnd,
        hullMins, hullMaxs, capsuleDir, owner, mask, downTrace))
    {
        return false;
    }

    return !ServerScript_PlacementTraceHit(downTrace);
}

static bool ServerScript_PlacementIsStretchingOffCliff(
    const Vector3D& origin,
    const Vector3D& forward,
    const Vector3D& right,
    const Vector3D& normal,
    const float objectDepth,
    const float objectWidth,
    const float percentOffLedgeMax,
    const float groundPenetrationMax,
    const float maxHeightToGround,
    bool* outLeft,
    bool* outRight,
    bool* outFront,
    bool* outBack,
    CPlayer* owner,
    const unsigned int mask)
{
    const bool leftOff = ServerScript_PlacementIsSectionOffCliff(
        ServerScript_ScaleVector(right, -1.0f), objectWidth, forward, objectDepth,
        normal, origin, percentOffLedgeMax, groundPenetrationMax, maxHeightToGround,
        owner, mask);
    const bool rightOff = ServerScript_PlacementIsSectionOffCliff(
        right, objectWidth, forward, objectDepth,
        normal, origin, percentOffLedgeMax, groundPenetrationMax, maxHeightToGround,
        owner, mask);
    const bool frontOff = ServerScript_PlacementIsSectionOffCliff(
        forward, objectDepth, right, objectWidth,
        normal, origin, percentOffLedgeMax, groundPenetrationMax, maxHeightToGround,
        owner, mask);
    const bool backOff = ServerScript_PlacementIsSectionOffCliff(
        ServerScript_ScaleVector(forward, -1.0f), objectDepth, right, objectWidth,
        normal, origin, percentOffLedgeMax, groundPenetrationMax, maxHeightToGround,
        owner, mask);

    if (outLeft)
        *outLeft = leftOff;
    if (outRight)
        *outRight = rightOff;
    if (outFront)
        *outFront = frontOff;
    if (outBack)
        *outBack = backOff;

    return leftOff || rightOff || frontOff || backOff;
}

static bool ServerScript_PlacementMoveSectionAwayFromCliff(
    trace_t& trDown,
    Vector3D& origin,
    Vector3D& forwardEndPos,
    void*& hitEntity,
    const Vector3D& directionToSide,
    const float distanceToSide,
    const Vector3D& capsuleDir,
    const float capsuleLength,
    const Vector3D& normal,
    const float dropToGroundOffsetMax,
    const float percentOffLedgeMax,
    const float maxHeightToGround,
    CPlayer* owner,
    const unsigned int mask)
{
    const float allowedOffLedge = (1.0f - percentOffLedgeMax) * distanceToSide;
    const float radius = (distanceToSide - allowedOffLedge) * 0.5f;

    Vector3D start = ServerScript_AddScaled(
        origin, directionToSide, radius + allowedOffLedge);
    start = ServerScript_AddScaled(start, normal, radius - maxHeightToGround);
    const Vector3D end = ServerScript_AddScaled(start, directionToSide, -distanceToSide);

    const Vector3D hullMins(-radius, -radius, -capsuleLength);
    const Vector3D hullMaxs(radius, radius, capsuleLength);

    trace_t sideTrace;
    if (!ServerScript_PlacementSweep(start, end,
        hullMins, hullMaxs, capsuleDir, owner, mask, sideTrace))
    {
        return false;
    }

    const float pushBack = sideTrace.fraction * distanceToSide + 1.0f;
    origin = ServerScript_AddScaled(origin, directionToSide, -pushBack);

    const float halfDrop = dropToGroundOffsetMax * 0.5f;
    forwardEndPos = Vector3D(origin.x, origin.y, origin.z + halfDrop);
    const Vector3D bottom(origin.x, origin.y, origin.z - halfDrop);

    if (!ServerScript_PlacementLine(forwardEndPos, bottom, owner, mask, trDown))
        return false;

    origin = trDown.endpos;
    hitEntity = trDown.hit_entity;

    return ServerScript_PlacementTraceHit(trDown) &&
        ServerScript_PlacementSurfaceAccepts(hitEntity);
}

static bool ServerScript_PlacementMoveAwayFromCliff(
    trace_t& trDown,
    Vector3D& origin,
    Vector3D& forwardEndPos,
    void*& hitEntity,
    const Vector3D& eyeRight,
    const float objectDepth,
    const float objectWidth,
    const float dropToGroundOffsetMax,
    const ServerObjectPlacementSettings& settings,
    CPlayer* owner,
    const unsigned int mask)
{
    const Vector3D normal = trDown.plane.normal;
    const Vector3D forward = ServerScript_Normalized(
        ServerScript_CrossVector(normal, eyeRight));
    const Vector3D right = ServerScript_CrossVector(forward, normal);

    bool leftOff = false;
    bool rightOff = false;
    bool frontOff = false;
    bool backOff = false;
    if (!ServerScript_PlacementIsStretchingOffCliff(origin, forward, right, normal,
        objectDepth, objectWidth, settings.percentOffLedgeMax,
        settings.groundPenetrationMax, settings.distanceToGroundMax,
        &leftOff, &rightOff, &frontOff, &backOff, owner, mask))
    {
        return true;
    }

    if (leftOff && rightOff)
        return false;
    if (frontOff && backOff)
        return false;

    trace_t workTrace = trDown;
    Vector3D workOrigin = origin;
    Vector3D workForwardEnd = forwardEndPos;
    void* workHitEntity = hitEntity;

    if (leftOff || rightOff)
    {
        const Vector3D moveDir = leftOff
            ? ServerScript_ScaleVector(right, -1.0f)
            : right;
        if (!ServerScript_PlacementMoveSectionAwayFromCliff(workTrace, workOrigin,
            workForwardEnd, workHitEntity, moveDir, objectWidth, forward, objectDepth,
            normal, dropToGroundOffsetMax, settings.percentOffLedgeMax,
            settings.distanceToGroundMax, owner, mask))
        {
            return false;
        }
    }

    if (frontOff || backOff)
    {
        const Vector3D moveDir = frontOff
            ? forward
            : ServerScript_ScaleVector(forward, -1.0f);
        if (!ServerScript_PlacementMoveSectionAwayFromCliff(workTrace, workOrigin,
            workForwardEnd, workHitEntity, moveDir, objectDepth, right, objectWidth,
            normal, dropToGroundOffsetMax, settings.percentOffLedgeMax,
            settings.distanceToGroundMax, owner, mask))
        {
            return false;
        }
    }

    trDown = workTrace;
    origin = workOrigin;
    forwardEndPos = workForwardEnd;
    hitEntity = workHitEntity;
    return true;
}

static bool ServerScript_PlacementFindPivot(
    Vector3D& origin,
    Vector3D& outNormal,
    void*& outParent,
    const trace_t& trForward,
    const Vector3D& eyeDir,
    const Vector3D& eyeRight,
    const float objectDepth,
    const float objectWidth,
    const ServerObjectPlacementSettings& settings,
    CPlayer* owner,
    const unsigned int mask)
{
    const float dropToGroundOffsetMax = settings.dropToGroundOffsetMax;
    const Vector3D forwardStart = trForward.startpos;
    Vector3D forwardEndPos = trForward.endpos;

    const Vector3D dropEnd(forwardEndPos.x, forwardEndPos.y,
        forwardEndPos.z - dropToGroundOffsetMax);

    trace_t trDown;
    if (!ServerScript_PlacementLine(forwardEndPos, dropEnd, owner, mask, trDown))
        return false;

    origin = trDown.endpos;
    void* hitEntity = trDown.hit_entity;

    // A miss is not a rejection: the aim point sits past a ledge, so walk back
    // along the eye ray until the downward feeler catches the lip.
    if (!ServerScript_PlacementTraceHit(trDown) &&
        !ServerScript_PlacementFindEdgeOfCliff(trDown, forwardEndPos, origin, hitEntity,
            forwardStart, eyeDir, dropToGroundOffsetMax, owner, mask))
    {
        return false;
    }

    if (!ServerScript_PlacementMoveAwayFromBlockers(trDown, origin, forwardEndPos,
        hitEntity, forwardStart, dropToGroundOffsetMax, owner, mask))
    {
        return false;
    }

    if (!ServerScript_PlacementMoveAwayFromCliff(trDown, origin, forwardEndPos,
        hitEntity, eyeRight, objectDepth, objectWidth, dropToGroundOffsetMax,
        settings, owner, mask))
    {
        return false;
    }

    outNormal = trDown.plane.normal;
    outParent = nullptr;
    if (hitEntity && ServerScript_PlacementSurfaceAccepts(hitEntity) &&
        (!g_serverEntityList || hitEntity != g_serverEntityList->LookupEntityByNetworkIndex(0)))
    {
        outParent = hitEntity;
    }
    return true;
}

static bool ServerScript_PlacementFindAngles(
    Vector3D& outAngles,
    Vector3D& outForward,
    Vector3D& outRight,
    Vector3D& outNormal,
    const Vector3D& origin,
    const Vector3D& pivotNormal,
    const Vector3D& eyeRight,
    const float objectDepth,
    const float objectWidth,
    const float objectHeight,
    const float hillAngleCos,
    const ServerObjectPlacementSettings& settings,
    CPlayer* owner,
    const unsigned int mask)
{
    const Vector3D topCenter(origin.x, origin.y, origin.z + objectHeight);
    const float dropDistance =
        settings.distanceToGroundMax + objectHeight + objectWidth * hillAngleCos;

    Vector3D forward = ServerScript_Normalized(
        ServerScript_CrossVector(pivotNormal, eyeRight));
    Vector3D right = ServerScript_CrossVector(forward, pivotNormal);

    const float halfDepth = objectDepth * 0.5f;
    const float halfWidth = objectWidth * 0.5f;

    Vector3D corners[4];
    corners[0] = ServerScript_AddScaled(
        ServerScript_AddScaled(topCenter, forward, halfDepth), right, halfWidth);
    corners[1] = ServerScript_AddScaled(
        ServerScript_AddScaled(topCenter, forward, -halfDepth), right, halfWidth);
    corners[2] = ServerScript_AddScaled(
        ServerScript_AddScaled(topCenter, forward, -halfDepth), right, -halfWidth);
    corners[3] = ServerScript_AddScaled(
        ServerScript_AddScaled(topCenter, forward, halfDepth), right, -halfWidth);

    for (int i = 0; i < 4; ++i)
    {
        trace_t cornerTrace;
        if (!ServerScript_PlacementLine(topCenter, corners[i], owner, mask, cornerTrace))
            return false;
        corners[i] = cornerTrace.endpos;
    }

    Vector3D accumulated = pivotNormal;
    int sampleCount = 1;
    for (int i = 0; i < 4; ++i)
    {
        const Vector3D cornerDropEnd(corners[i].x, corners[i].y, corners[i].z - dropDistance);
        trace_t cornerDrop;
        if (!ServerScript_PlacementLine(corners[i], cornerDropEnd, owner, mask, cornerDrop))
            return false;

        // 0.259 == cos(75 deg): a corner facing away from the pivot surface
        // belongs to different geometry and must not bias the average.
        if (ServerScript_PlacementTraceHit(cornerDrop) &&
            ServerScript_Dot(cornerDrop.plane.normal, pivotNormal) > 0.259f)
        {
            accumulated = ServerScript_AddVector(accumulated, cornerDrop.plane.normal);
            ++sampleCount;
        }
    }

    Vector3D normal = ServerScript_Normalized(
        ServerScript_ScaleVector(accumulated, 1.0f / static_cast<float>(sampleCount)));
    forward = ServerScript_Normalized(ServerScript_CrossVector(normal, eyeRight));
    right = ServerScript_CrossVector(forward, normal);

    // The hill test reads the averaged surface normal, never the upright override.
    const float hillTestZ = normal.z;

    if (settings.forceUpright)
    {
        normal = Vector3D(0.0f, 0.0f, 1.0f);
        right = eyeRight;
        forward = ServerScript_Normalized(ServerScript_CrossVector(normal, eyeRight));
    }

    QAngle angles;
    VectorAngles(forward, normal, angles);
    outAngles = Vector3D(angles.x, angles.y, angles.z);
    outForward = forward;
    outRight = right;
    outNormal = normal;

    return hillAngleCos < hillTestZ;
}

static void ServerScript_PlacementFindTracePositions(
    const Vector3D& origin,
    Vector3D& outStart,
    Vector3D& outEnd,
    const Vector3D& traceDir,
    const Vector3D& normal,
    const float radius,
    const float traceDistance,
    const float traceHeight,
    const float clearanceBehind)
{
    const Vector3D base = ServerScript_AddScaled(origin, normal, traceHeight);
    outStart = ServerScript_AddScaled(
        base, traceDir, -traceDistance - clearanceBehind + radius);
    outEnd = ServerScript_AddScaled(base, traceDir, traceDistance - radius);
}

static bool ServerScript_PlacementTracePosition(
    Vector3D& origin,
    const Vector3D& capsuleDir,
    const Vector3D& traceDir,
    const Vector3D& normal,
    const float radius,
    const float capsuleLength,
    const float traceDistance,
    const float traceHeight,
    const float clearanceBehind,
    CPlayer* owner,
    const unsigned int mask)
{
    const Vector3D hullMins(-radius, -radius, -capsuleLength);
    const Vector3D hullMaxs(radius, radius, capsuleLength);

    Vector3D startPos;
    Vector3D endPos;
    ServerScript_PlacementFindTracePositions(origin, startPos, endPos, traceDir,
        normal, radius, traceDistance, traceHeight, clearanceBehind);

    trace_t sweep;
    if (!ServerScript_PlacementSweep(startPos, endPos,
        hullMins, hullMaxs, capsuleDir, owner, mask, sweep))
    {
        return false;
    }

    if (!ServerScript_PlacementTraceHit(sweep))
        return true;

    const float totalLength = traceDistance + traceDistance + clearanceBehind;
    const float span = totalLength - (radius + radius);
    if (span <= 0.0f)
        return false;

    const float midFraction = (totalLength * 0.5f - (radius + radius)) / span;
    float pushSign = -1.0f;

    if (sweep.startsolid || midFraction > sweep.fraction)
    {
        if (!ServerScript_PlacementSweep(endPos, startPos,
            hullMins, hullMaxs, capsuleDir, owner, mask, sweep))
        {
            return false;
        }

        if (sweep.startsolid || midFraction > sweep.fraction)
            return false;

        pushSign = 1.0f;
    }

    const float push = ((1.0f - sweep.fraction) * span + 0.1f) * pushSign;
    origin = ServerScript_AddScaled(origin, traceDir, push);

    ServerScript_PlacementFindTracePositions(origin, startPos, endPos, traceDir,
        normal, radius, traceDistance, traceHeight, clearanceBehind);

    // Deliberately reversed on the confirmation sweep.
    if (!ServerScript_PlacementSweep(endPos, startPos,
        hullMins, hullMaxs, capsuleDir, owner, mask, sweep))
    {
        return false;
    }

    return !ServerScript_PlacementTraceHit(sweep);
}

static bool ServerScript_PlacementIsTopIntersecting(
    const Vector3D& origin,
    const Vector3D& forward,
    const Vector3D& right,
    const Vector3D& normal,
    const float objectDepth,
    const float objectWidth,
    const float objectHeight,
    const ServerObjectPlacementSettings& settings,
    CPlayer* owner,
    const unsigned int mask)
{
    const float keep = 1.0f - settings.topSidePercentPierceMax;

    Vector3D capsuleDir;
    float capsuleLength;
    float radius;
    if (objectWidth <= objectDepth)
    {
        capsuleDir = forward;
        capsuleLength = objectWidth * keep;
        radius = objectDepth * keep;
    }
    else
    {
        capsuleDir = right;
        capsuleLength = objectDepth * keep;
        radius = objectWidth * keep;
    }

    const Vector3D start = ServerScript_AddScaled(origin, normal, objectHeight);
    const float endDistance =
        (objectHeight + objectHeight - capsuleLength) - settings.topDistancePierceMax;
    const Vector3D end = ServerScript_AddScaled(origin, normal, endDistance);

    const Vector3D hullMins(-capsuleLength, -capsuleLength, -radius);
    const Vector3D hullMaxs(capsuleLength, capsuleLength, radius);

    trace_t topTrace;
    if (!ServerScript_PlacementSweep(start, end,
        hullMins, hullMaxs, capsuleDir, owner, mask, topTrace))
    {
        return true;
    }

    return ServerScript_PlacementTraceHit(topTrace);
}

static bool ServerScript_PlacementFindPosition(
    Vector3D& origin,
    const Vector3D& forward,
    const Vector3D& right,
    const Vector3D& normal,
    const float objectDepth,
    const float objectWidth,
    const float objectHeight,
    const ServerObjectPlacementSettings& settings,
    CPlayer* owner,
    const unsigned int mask)
{
    const float radius = fminf(objectWidth, objectDepth) * 0.5f;
    const float traceHeight = settings.useTopTrace
        ? (objectHeight + objectHeight) - radius
        : radius + settings.groundPenetrationMax;

    Vector3D working = origin;

    // Return value deliberately discarded: this pass exists for the sideways
    // nudge it writes into the origin, not for its verdict.
    ServerScript_PlacementTracePosition(working, forward, right, normal, radius,
        objectDepth, objectWidth, traceHeight, 0.0f, owner, mask);

    if (!ServerScript_PlacementTracePosition(working, right, forward, normal, radius,
        objectWidth, objectDepth, traceHeight, settings.clearanceBehind, owner, mask))
    {
        working = ServerScript_AddScaled(working, forward, -(radius + radius));
        if (!ServerScript_PlacementTracePosition(working, right, forward, normal, radius,
            objectWidth, objectDepth, traceHeight, settings.clearanceBehind, owner, mask))
        {
            return false;
        }
    }

    if (ServerScript_PlacementIsTopIntersecting(working, forward, right, normal,
        objectDepth, objectWidth, objectHeight, settings, owner, mask))
    {
        return false;
    }

    if (ServerScript_PlacementIsStretchingOffCliff(working, forward, right, normal,
        objectDepth, objectWidth, settings.percentOffLedgeMax,
        settings.groundPenetrationMax, settings.distanceToGroundMax,
        nullptr, nullptr, nullptr, nullptr, owner, mask))
    {
        return false;
    }

    origin = working;
    return true;
}

static bool ServerScript_CalcNormalPlacement(
    void* pWeapon,
    CPlayer* owner,
    const Vector3D& eyeOrigin,
    const Vector3D& eyeDir,
    const QAngle& eyeAngles,
    ServerObjectPlacementState& state)
{
    const ServerObjectPlacementSettings& settings = ServerScript_GetPlacementSettings(pWeapon);
    const unsigned int mask = ServerScript_PlacementTraceMask(settings);
    const float distanceMax = ServerScript_ObjectPlacementDistance(pWeapon, false);

    Vector3D eyeForward;
    Vector3D eyeRight;
    AngleVectors(eyeAngles, &eyeForward, &eyeRight, nullptr);
    eyeRight = ServerScript_Normalized(eyeRight);

    // Half extents of the placement model's collision bounds.
    const Vector3D boundsSize = ServerScript_SubVector(settings.modelMaxs, settings.modelMins);
    const float objectDepth = fabsf(boundsSize.x) * 0.5f;
    const float objectWidth = fabsf(boundsSize.y) * 0.5f;
    const float objectHeight = fabsf(boundsSize.z) * 0.5f;

    // object_placement_hill_angle_max is authored in degrees; every consumer
    // below compares it against a unit surface normal, so it is a cosine here.
    const float hillAngleCos = cosf(
        settings.hillAngleMax * (3.14159265358979323846f / 180.0f));

    // 5-unit swept hull, not a ray: a ray threads gaps the placed object cannot.
    const Vector3D forwardMins(-5.0f, -5.0f, -5.0f);
    const Vector3D forwardMaxs(5.0f, 5.0f, 5.0f);
    const Vector3D worldUp(0.0f, 0.0f, 1.0f);
    const Vector3D forwardEnd = ServerScript_AddScaled(eyeOrigin, eyeDir, distanceMax);

    trace_t trForward;
    if (!ServerScript_PlacementSweep(eyeOrigin, forwardEnd,
        forwardMins, forwardMaxs, worldUp, owner, mask, trForward))
    {
        return false;
    }

    if (trForward.startsolid || trForward.allsolid)
    {
        if (s_serverPlacementDiagBudget > 0)
        {
            Msg(eDLL_T::SERVER,
                "[OPL-SRV] normal reject forward-solid frac=%.3f startsolid=%d allsolid=%d start=<%.1f %.1f %.1f> end=<%.1f %.1f %.1f>\n",
                trForward.fraction,
                trForward.startsolid ? 1 : 0,
                trForward.allsolid ? 1 : 0,
                eyeOrigin.x, eyeOrigin.y, eyeOrigin.z,
                forwardEnd.x, forwardEnd.y, forwardEnd.z);
            --s_serverPlacementDiagBudget;
        }
        return false;
    }

    trForward.startpos = eyeOrigin;

    Vector3D origin;
    Vector3D pivotNormal;
    void* parent = nullptr;
    if (!ServerScript_PlacementFindPivot(origin, pivotNormal, parent, trForward,
        eyeDir, eyeRight, objectDepth, objectWidth, settings, owner, mask))
    {
        if (s_serverPlacementDiagBudget > 0)
        {
            Msg(eDLL_T::SERVER,
                "[OPL-SRV] normal reject pivot aim=<%.1f %.1f %.1f> dropUp=%.1f forwardFrac=%.3f\n",
                trForward.endpos.x, trForward.endpos.y, trForward.endpos.z,
                settings.dropToGroundOffsetMax,
                trForward.fraction);
            --s_serverPlacementDiagBudget;
        }
        return false;
    }

    Vector3D angles;
    Vector3D forward;
    Vector3D right;
    Vector3D normal;
    if (!ServerScript_PlacementFindAngles(angles, forward, right, normal, origin,
        pivotNormal, eyeRight, objectDepth, objectWidth, objectHeight, hillAngleCos,
        settings, owner, mask))
    {
        if (s_serverPlacementDiagBudget > 0)
        {
            Msg(eDLL_T::SERVER,
                "[OPL-SRV] normal reject angles-hill normal=<%.3f %.3f %.3f> minZ=%.3f origin=<%.1f %.1f %.1f>\n",
                normal.x, normal.y, normal.z,
                hillAngleCos,
                origin.x, origin.y, origin.z);
            --s_serverPlacementDiagBudget;
        }
        return false;
    }

    if (!ServerScript_PlacementFindPosition(origin, forward, right, normal,
        objectDepth, objectWidth, objectHeight, settings, owner, mask))
    {
        if (s_serverPlacementDiagBudget > 0)
        {
            Msg(eDLL_T::SERVER,
                "[OPL-SRV] normal reject position origin=<%.1f %.1f %.1f> depth=%.1f width=%.1f height=%.1f\n",
                origin.x, origin.y, origin.z,
                objectDepth, objectWidth, objectHeight);
            --s_serverPlacementDiagBudget;
        }
        return false;
    }

    state.origin = origin;
    state.angles = angles;
    state.parent = parent;
    state.specialOrigin = state.origin;
    state.specialAngles = state.angles;
    state.valid = true;
    return true;
}

// MayPlace + zero-extent hull revalidation for a last-known-good pose point.
static bool ServerScript_ValidateLastKnownGoodPoint(
    const Vector3D& point,
    void* parent,
    CPlayer* owner)
{
    if (!ServerScript_MayPlaceObjectAtPoint(point, parent))
        return false;

    // Confirm the recomposed spot is not embedded in solid geometry.
    const Vector3D hullMins(-16.0f, -16.0f, -16.0f);
    const Vector3D hullMaxs(16.0f, 16.0f, 16.0f);
    trace_t revalidateTrace;
    if (!ServerScript_TraceLine(point, point, owner, revalidateTrace, true, &hullMins, &hullMaxs) ||
        revalidateTrace.startsolid)
    {
        if (s_serverPlacementDiagBudget > 0)
        {
            Msg(eDLL_T::SERVER,
                "[OPL-SRV] last-known-good rejected on hull revalidation, origin=<%.1f %.1f %.1f> (geometry moved since cached)\n",
                point.x, point.y, point.z);
            --s_serverPlacementDiagBudget;
        }
        return false;
    }

    return true;
}

static void ServerScript_CacheLastGoodPose(
    ServerPlacementCache& cache,
    const ServerObjectPlacementState& state,
    const Vector3D& eyeOrigin,
    const Vector3D& eyeDir)
{
    cache.hasLastGood = true;
    cache.lastGoodEyeOrigin = eyeOrigin;
    cache.lastGoodEyeDir = eyeDir;

    CBaseEntity* const parentEnt = reinterpret_cast<CBaseEntity*>(state.parent);
    cache.lastGoodParentHandle = ServerScript_EntityOwnHandle(parentEnt);

    Vector3D parentOrigin(0.0f, 0.0f, 0.0f);
    cache.lastGoodParentYaw = 0.0f;
    cache.lastGoodParentOrigin = Vector3D(0.0f, 0.0f, 0.0f);
    cache.lastGoodParentAngles = Vector3D(0.0f, 0.0f, 0.0f);
    if (parentEnt)
    {
        parentOrigin = parentEnt->Diag_AbsOrigin();
        cache.lastGoodParentOrigin = parentOrigin;
        cache.lastGoodParentAngles = ServerScript_EntityAbsAngles(parentEnt);
        cache.lastGoodParentYaw = ServerScript_EntityAbsYaw(parentEnt);
    }

    const float yawRadians = cache.lastGoodParentYaw * (3.14159265358979323846f / 180.0f);
    cache.lastGoodLocalOrigin = ServerScript_RotateAroundAxis(
        ServerScript_SubVector(state.origin, parentOrigin), Vector3D(0.0f, 0.0f, 1.0f), -yawRadians);
    cache.lastGoodLocalAngles = state.angles;

    CBaseEntity* const specialParentEnt = reinterpret_cast<CBaseEntity*>(state.specialParent);
    cache.lastGoodSpecialParentHandle = ServerScript_EntityOwnHandle(specialParentEnt);

    Vector3D specialParentOrigin(0.0f, 0.0f, 0.0f);
    cache.lastGoodSpecialParentYaw = 0.0f;
    cache.lastGoodSpecialParentOrigin = Vector3D(0.0f, 0.0f, 0.0f);
    cache.lastGoodSpecialParentAngles = Vector3D(0.0f, 0.0f, 0.0f);
    if (specialParentEnt)
    {
        specialParentOrigin = specialParentEnt->Diag_AbsOrigin();
        cache.lastGoodSpecialParentOrigin = specialParentOrigin;
        cache.lastGoodSpecialParentAngles = ServerScript_EntityAbsAngles(specialParentEnt);
        cache.lastGoodSpecialParentYaw = ServerScript_EntityAbsYaw(specialParentEnt);
    }

    const float specialYawRadians =
        cache.lastGoodSpecialParentYaw * (3.14159265358979323846f / 180.0f);
    cache.lastGoodLocalSpecialOrigin = ServerScript_RotateAroundAxis(
        ServerScript_SubVector(state.specialOrigin, specialParentOrigin),
        Vector3D(0.0f, 0.0f, 1.0f), -specialYawRadians);
    cache.lastGoodLocalSpecialAngles = state.specialAngles;
}

static bool ServerScript_ParentPoseUnchanged(
    uint32_t handle,
    const Vector3D& cachedOrigin,
    const Vector3D& cachedAngles)
{
    CBaseEntity* const ent = reinterpret_cast<CBaseEntity*>(
        ServerScript_LookupEntityFromRawHandle(handle));
    if (!ent)
        return true;

    return ent->Diag_AbsOrigin() == cachedOrigin &&
        ServerScript_EntityAbsAngles(ent) == cachedAngles;
}

// Keep the stored pose when this hold already had a valid spot and the eye /
// parent pose still match last-good. Do not recompute or recompose.
static bool ServerScript_CanKeepLastKnownGoodPose(
    const Vector3D& eyeOrigin,
    const Vector3D& eyeDir,
    const ServerObjectPlacementSettings& settings,
    const ServerPlacementCache& cache)
{
    if (!cache.hasValidSpot || !cache.hasLastGood || settings.lastGoodDistanceMax <= 0.0f)
        return false;

    const Vector3D eyeDelta = ServerScript_SubVector(eyeOrigin, cache.lastGoodEyeOrigin);
    if (ServerScript_Dot(eyeDelta, eyeDelta) > settings.lastGoodDistanceMax * settings.lastGoodDistanceMax)
        return false;

    if (ServerScript_Dot(eyeDir, cache.lastGoodEyeDir) < settings.lastGoodAngleMax)
        return false;

    if (!ServerScript_ParentPoseUnchanged(
        cache.lastGoodParentHandle, cache.lastGoodParentOrigin, cache.lastGoodParentAngles))
        return false;

    if (!ServerScript_ParentPoseUnchanged(
        cache.lastGoodSpecialParentHandle,
        cache.lastGoodSpecialParentOrigin,
        cache.lastGoodSpecialParentAngles))
        return false;

    return true;
}

// Reuse last successful parent-relative pose while the eye is still inside the last-good window.
static bool ServerScript_TryLastKnownGoodPlacement(
    const Vector3D& eyeOrigin,
    const Vector3D& eyeDir,
    const ServerObjectPlacementSettings& settings,
    ServerPlacementCache& cache,
    ServerObjectPlacementState& out,
    CPlayer* owner)
{
    if (!cache.hasValidSpot || !cache.hasLastGood || settings.lastGoodDistanceMax <= 0.0f)
        return false;

    const Vector3D eyeDelta = ServerScript_SubVector(eyeOrigin, cache.lastGoodEyeOrigin);
    if (ServerScript_Dot(eyeDelta, eyeDelta) > settings.lastGoodDistanceMax * settings.lastGoodDistanceMax)
        return false;

    if (ServerScript_Dot(eyeDir, cache.lastGoodEyeDir) < settings.lastGoodAngleMax)
        return false;

    // Resolve parent through serial-checked handle lookup; never a raw pointer cached across ticks.
    CBaseEntity* const parentEnt = reinterpret_cast<CBaseEntity*>(
        ServerScript_LookupEntityFromRawHandle(cache.lastGoodParentHandle));
    CBaseEntity* const specialParentEnt = reinterpret_cast<CBaseEntity*>(
        ServerScript_LookupEntityFromRawHandle(cache.lastGoodSpecialParentHandle));

    Vector3D parentOrigin(0.0f, 0.0f, 0.0f);
    float parentYawNow = 0.0f;
    if (cache.lastGoodParentHandle != INVALID_EHANDLE_INDEX)
    {
        if (!parentEnt)
            return false; // parent was destroyed since caching -- do not reuse a stale pose.
        parentOrigin = parentEnt->Diag_AbsOrigin();
        parentYawNow = ServerScript_EntityAbsYaw(parentEnt);
    }

    Vector3D specialParentOrigin(0.0f, 0.0f, 0.0f);
    float specialParentYawNow = 0.0f;
    if (cache.lastGoodSpecialParentHandle != INVALID_EHANDLE_INDEX)
    {
        if (!specialParentEnt)
            return false;
        specialParentOrigin = specialParentEnt->Diag_AbsOrigin();
        specialParentYawNow = ServerScript_EntityAbsYaw(specialParentEnt);
    }

    // Re-compose each cached parent-local delta with the parent's current yaw.
    const float yawRadians = parentYawNow * (3.14159265358979323846f / 180.0f);
    const Vector3D worldDelta = ServerScript_RotateAroundAxis(
        cache.lastGoodLocalOrigin, Vector3D(0.0f, 0.0f, 1.0f), yawRadians);

    const float specialYawRadians = specialParentYawNow * (3.14159265358979323846f / 180.0f);
    const Vector3D specialWorldDelta = ServerScript_RotateAroundAxis(
        cache.lastGoodLocalSpecialOrigin, Vector3D(0.0f, 0.0f, 1.0f), specialYawRadians);

    out.valid = true;
    out.special = cache.useSpecial;
    out.origin = ServerScript_AddVector(parentOrigin, worldDelta);
    out.angles = Vector3D(cache.lastGoodLocalAngles.x,
        cache.lastGoodLocalAngles.y + (parentYawNow - cache.lastGoodParentYaw),
        cache.lastGoodLocalAngles.z);
    out.specialOrigin = ServerScript_AddVector(specialParentOrigin, specialWorldDelta);
    out.specialAngles = Vector3D(cache.lastGoodLocalSpecialAngles.x,
        cache.lastGoodLocalSpecialAngles.y + (specialParentYawNow - cache.lastGoodSpecialParentYaw),
        cache.lastGoodLocalSpecialAngles.z);
    out.parent = parentEnt;
    out.specialParent = specialParentEnt;
    out.specialResult = 0;

    // Re-validate before reuse; a mover may have re-blocked the spot. Either side failing rejects.
    if (!ServerScript_ValidateLastKnownGoodPoint(out.origin, parentEnt, owner))
        return false;

    if (cache.useSpecial &&
        !ServerScript_ValidateLastKnownGoodPoint(out.specialOrigin, specialParentEnt, owner))
        return false;

    return true;
}

static bool ServerScript_CalcWeaponPlacement(
    void* pWeapon,
    const bool specialRequested,
    ServerObjectPlacementState& out)
{
    CPlayer* const owner = ServerScript_GetWeaponOwnerPlayer(pWeapon);
    if (!owner)
    {
        ServerScript_PlacementDiag("owner lookup failed", pWeapon);
        return false;
    }

    QAngle eyeAngles = ServerScript_PlayerPlacementEyeAngles(owner);
    const Vector3D eyeOrigin = ServerScript_PlayerEyeOrigin(owner);
    if (!ServerScript_IsFiniteVector(eyeOrigin) || !ServerScript_IsFiniteQAngle(eyeAngles))
    {
        ServerScript_PlacementDiag("eye origin invalid", pWeapon, owner);
        return false;
    }

    ServerScript_PlacementEyeDiag(owner, eyeOrigin, eyeAngles);

    const bool useSpecial = specialRequested || ServerScript_IsPhaseDoorWeapon(pWeapon);
    ServerPlacementCache& cache = s_serverPlacementCache[pWeapon];
    if (cache.curTime == gpGlobals->curTime &&
        cache.useSpecial == useSpecial &&
        cache.eyeOrigin == eyeOrigin &&
        cache.eyeAngles == eyeAngles)
    {
        out = cache.state;
        return out.valid;
    }

    Vector3D eyeDir;
    Vector3D eyeRight;
    AngleVectors(eyeAngles, &eyeDir, &eyeRight, nullptr);
    eyeDir = ServerScript_Normalized(eyeDir);

    ServerObjectPlacementState state;
    if (useSpecial)
        state.valid = ServerScript_CalcSpecialPlacement(pWeapon, owner, eyeOrigin, eyeDir, eyeRight, state);
    else
        state.valid = ServerScript_CalcNormalPlacement(pWeapon, owner, eyeOrigin, eyeDir, eyeAngles, state);

    const ServerObjectPlacementSettings& settings = ServerScript_GetPlacementSettings(pWeapon);
    if (!state.valid)
    {
        ServerObjectPlacementState lastGoodState;
        if (ServerScript_TryLastKnownGoodPlacement(eyeOrigin, eyeDir, settings, cache, lastGoodState, owner))
            state = lastGoodState;
    }
    else
    {
        ServerScript_CacheLastGoodPose(cache, state, eyeOrigin, eyeDir);
    }

    cache.curTime = gpGlobals->curTime;
    cache.useSpecial = useSpecial;
    cache.eyeOrigin = eyeOrigin;
    cache.eyeAngles = eyeAngles;
    cache.state = state;
    cache.hasValidSpot = state.valid;
    cache.isLastKnownGood = !state.valid ? false : cache.isLastKnownGood;

    out = state;
    return out.valid;
}

static bool ServerScript_StoreWeaponPlacement(void* pWeapon, CPlayer* owner, int commandNumber)
{
    if (!pWeapon || !owner)
        return false;

    static bool s_loggedStore = false;
    if (!s_loggedStore)
    {
        s_loggedStore = true;
        Warning(eDLL_T::SERVER,
            "[OPL-SRV] StoreWeaponPlacement first call weapon=%p cmd=%d\n",
            pWeapon, commandNumber);
    }

    ServerPlacementCache& cache = s_serverPlacementCache[pWeapon];
    if (commandNumber > 0 && cache.cmdNumber == commandNumber)
        return cache.state.valid;

    const QAngle eyeAngles = ServerScript_PlayerPlacementEyeAngles(owner);
    const Vector3D eyeOrigin = ServerScript_PlayerEyeOrigin(owner);
    if (!ServerScript_IsFiniteVector(eyeOrigin) || !ServerScript_IsFiniteQAngle(eyeAngles))
    {
        ServerScript_PlacementDiag("hold store eye invalid", pWeapon, owner);
        return false;
    }

    if (cache.curTime == gpGlobals->curTime &&
        cache.eyeOrigin == eyeOrigin &&
        cache.eyeAngles == eyeAngles)
    {
        cache.cmdNumber = commandNumber;
        return cache.state.valid;
    }

    ServerScript_PlacementEyeDiag(owner, eyeOrigin, eyeAngles);

    Vector3D eyeDir;
    Vector3D eyeRight;
    AngleVectors(eyeAngles, &eyeDir, &eyeRight, nullptr);
    eyeDir = ServerScript_Normalized(eyeDir);

    const bool useSpecial = ServerScript_IsPhaseDoorWeapon(pWeapon);
    ServerObjectPlacementState state;
    if (useSpecial)
        state.valid = ServerScript_CalcSpecialPlacement(pWeapon, owner, eyeOrigin, eyeDir, eyeRight, state);
    else
        state.valid = ServerScript_CalcNormalPlacement(pWeapon, owner, eyeOrigin, eyeDir, eyeAngles, state);

    const ServerObjectPlacementSettings& settings = ServerScript_GetPlacementSettings(pWeapon);
    if (state.valid)
    {
        cache.hasValidSpot = true;
        cache.isLastKnownGood = false;
        ServerScript_CacheLastGoodPose(cache, state, eyeOrigin, eyeDir);
        cache.state = state;
    }
    else if (ServerScript_CanKeepLastKnownGoodPose(eyeOrigin, eyeDir, settings, cache))
    {
        cache.isLastKnownGood = true;
    }
    else
    {
        cache.hasValidSpot = false;
        cache.isLastKnownGood = false;
        cache.state = state;
    }

    cache.curTime = gpGlobals->curTime;
    cache.useSpecial = useSpecial;
    cache.eyeOrigin = eyeOrigin;
    cache.eyeAngles = eyeAngles;
    cache.cmdNumber = commandNumber;

    ServerScript_PlacementCadenceDiag("hold-store", pWeapon, commandNumber, cache);
    return cache.state.valid;
}

static void ServerScript_InvalidateHoldPlacement(void* pWeapon)
{
    ServerPlacementCache* const cache = s_serverPlacementCache.Find(pWeapon);
    if (!cache)
        return;

    cache->hasValidSpot = false;
    cache->hasLastGood = false;
    cache->isLastKnownGood = false;
    cache->cmdNumber = 0;
    cache->curTime = -1.0f;
    cache->state = ServerObjectPlacementState();
}

static void ServerScript_ConsiderHeldWeapon(CPlayer* player, void* pWeapon, int commandNumber)
{
    if (!pWeapon)
        return;

    if (!ServerScript_IsPhaseDoorWeapon(pWeapon) &&
        !ServerScript_GetPlacementSettings(pWeapon).objectPlacer)
        return;

    const unsigned int weapState = *reinterpret_cast<const unsigned int*>(
        reinterpret_cast<uintptr_t>(pWeapon) + WEAPON_OFF_WEAPSTATE);
    if (weapState == WEAP_STATE_HOLSTERED)
    {
        ServerScript_InvalidateHoldPlacement(pWeapon);
        return;
    }

    ServerScript_StoreWeaponPlacement(pWeapon, player, commandNumber);
}

void ServerScript_UpdateHeldObjectPlacement(CPlayer* player, int commandNumber)
{
    static bool s_loggedHeldUpdate = false;
    if (!s_loggedHeldUpdate)
    {
        s_loggedHeldUpdate = true;
        Warning(eDLL_T::SERVER,
            "[OPL-SRV] UpdateHeldObjectPlacement first call player=%p cmd=%d store=%d\n",
            player, commandNumber, sv_alter_portal_pred_store.GetBool() ? 1 : 0);
    }

    if (!player || !sv_alter_portal_pred_store.GetBool())
        return;

    int cmdNumber = commandNumber;
    if (cmdNumber <= 0)
    {
        if (const CUserCmd* const cmd = player->GetPlacementUserCommand())
            cmdNumber = cmd->command_number;
    }

    const uint32_t* const offhand = reinterpret_cast<const uint32_t*>(
        reinterpret_cast<uintptr_t>(player) + PLAYER_OFF_OFFHAND_WEAPONS);
    for (int i = 0; i < PLAYER_OFFHAND_WEAPON_COUNT; ++i)
        ServerScript_ConsiderHeldWeapon(player, ServerScript_LookupEntityFromRawHandle(offhand[i]), cmdNumber);

    const uint32_t* const active = reinterpret_cast<const uint32_t*>(
        reinterpret_cast<uintptr_t>(player) + PLAYER_OFF_ACTIVE_WEAPONS);
    for (int i = 0; i < PLAYER_ACTIVE_WEAPON_COUNT; ++i)
        ServerScript_ConsiderHeldWeapon(player, ServerScript_LookupEntityFromRawHandle(active[i]), cmdNumber);
}

static bool ServerScript_GetWeaponPlacement(
    void* pWeapon,
    const bool specialRequested,
    ServerObjectPlacementState& out)
{
    if (!sv_alter_portal_pred_store.GetBool())
        return ServerScript_CalcWeaponPlacement(pWeapon, specialRequested, out);

    if (ServerPlacementCache* const cache = s_serverPlacementCache.Find(pWeapon))
    {
        if (cache->cmdNumber != 0 || cache->curTime >= 0.0f)
        {
            static bool s_loggedGet = false;
            if (!s_loggedGet)
            {
                s_loggedGet = true;
                Warning(eDLL_T::SERVER,
                    "[OPL-SRV] toss-get first call weapon=%p cmd=%d origin=<%.1f %.1f %.1f>\n",
                    pWeapon,
                    cache->cmdNumber,
                    cache->state.origin.x, cache->state.origin.y, cache->state.origin.z);
            }

            out = cache->state;
            ServerScript_PlacementCadenceDiag("toss-get", pWeapon, cache->cmdNumber, *cache);
            return out.valid;
        }
    }

    static bool s_loggedGetMiss = false;
    if (!s_loggedGetMiss)
    {
        s_loggedGetMiss = true;
        Warning(eDLL_T::SERVER,
            "[OPL-SRV] toss-get miss weapon=%p -- compute once\n",
            pWeapon);
    }

    return ServerScript_CalcWeaponPlacement(pWeapon, specialRequested, out);
}

static SQRESULT ServerScript_PushPlacementVector(HSQUIRRELVM v, const Vector3D& value)
{
    const SQVector3D result(value.x, value.y, value.z);
    sq_pushvector(v, &result);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_PushWeaponPlacementVector(HSQUIRRELVM v, const bool special)
{
    void* pWeapon = nullptr;
    ServerObjectPlacementState placement;
    const bool gotWeapon = v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon));
    if (gotWeapon &&
        pWeapon &&
        ServerScript_GetWeaponPlacement(pWeapon, special, placement) &&
        placement.valid)
    {
        if (special && placement.special)
            return ServerScript_PushPlacementVector(v, placement.specialOrigin);
        return ServerScript_PushPlacementVector(v, placement.origin);
    }

    if (!gotWeapon || !pWeapon)
        ServerScript_PlacementDiag("sq_getentity weapon failed", pWeapon);

    return ServerScript_PushPlacementVector(v, Vector3D(0.0f, 0.0f, 0.0f));
}

static SQRESULT ServerScript_GetPlacementParentShim(HSQUIRRELVM v)
{
    void* pWeapon = nullptr;
    if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)) || !pWeapon)
    {
        sq_pushnull(v);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    ServerObjectPlacementState placement;
    void* pParent = nullptr;
    if (ServerScript_GetWeaponPlacement(pWeapon, false, placement) && placement.valid)
        pParent = placement.parent;

    if (v_CSquirrelVM_PushEntity_Server)
        v_CSquirrelVM_PushEntity_Server(v, pParent);
    else
        sq_pushnull(v);

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_GetPlacementOriginShim(HSQUIRRELVM v)
{
    return ServerScript_PushWeaponPlacementVector(v, false);
}

static SQRESULT ServerScript_GetPlacementAnglesShim(HSQUIRRELVM v)
{
    void* pWeapon = nullptr;
    ServerObjectPlacementState placement;
    if (v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)) &&
        pWeapon &&
        ServerScript_GetWeaponPlacement(pWeapon, false, placement) &&
        placement.valid)
    {
        return ServerScript_PushPlacementVector(v, placement.angles);
    }

    return ServerScript_PushPlacementVector(v, Vector3D(0.0f, 0.0f, 0.0f));
}

static SQRESULT ServerScript_GetPlacementSpecialOriginShim(HSQUIRRELVM v)
{
    return ServerScript_PushWeaponPlacementVector(v, true);
}

static SQRESULT ServerScript_GetPlacementSpecialAnglesShim(HSQUIRRELVM v)
{
    void* pWeapon = nullptr;
    ServerObjectPlacementState placement;
    if (v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)) &&
        pWeapon &&
        ServerScript_GetWeaponPlacement(pWeapon, true, placement) &&
        placement.valid)
    {
        if (placement.special)
            return ServerScript_PushPlacementVector(v, placement.specialAngles);
        return ServerScript_PushPlacementVector(v, placement.angles);
    }

    return ServerScript_PushPlacementVector(v, Vector3D(0.0f, 0.0f, 0.0f));
}

static SQRESULT ServerScript_GetPlacementSpecialParentShim(HSQUIRRELVM v)
{
    void* pWeapon = nullptr;
    if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)) || !pWeapon)
    {
        sq_pushnull(v);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    ServerObjectPlacementState placement;
    void* pParent = nullptr;
    if (ServerScript_GetWeaponPlacement(pWeapon, true, placement) && placement.valid && placement.special)
        pParent = placement.specialParent;
    if (!pParent)
        pParent = placement.parent;

    if (v_CSquirrelVM_PushEntity_Server)
        v_CSquirrelVM_PushEntity_Server(v, pParent);
    else
        sq_pushnull(v);

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_PlacementHasValidSpotShim(HSQUIRRELVM v)
{
    void* pWeapon = nullptr;
    ServerObjectPlacementState placement;
    const bool valid = v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)) &&
        pWeapon &&
        ServerScript_GetWeaponPlacement(pWeapon, false, placement) &&
        placement.valid;
    sq_pushbool(v, valid);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_GetPlacementSpecialPlacementResultShim(HSQUIRRELVM v)
{
    void* pWeapon = nullptr;
    ServerObjectPlacementState placement;
    if (v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)) && pWeapon)
        ServerScript_GetWeaponPlacement(pWeapon, true, placement);
    sq_pushinteger(v, placement.specialResult);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static void ServerScript_UpsertWeaponFunction(
    const SQChar* scriptName,
    const SQChar* nativeName,
    const SQChar* helpString,
    const SQChar* returnType,
    const SQChar* parameters,
    const ScriptFunctionBindingStorageType_t function)
{
    bool replaced = false;
    for (int i = 0; i < g_serverScriptWeaponStruct->m_StrTypedFunctions.Count(); ++i)
    {
        ScriptFunctionBinding_t& binding = g_serverScriptWeaponStruct->m_StrTypedFunctions[i];
        if (binding.m_Descriptor.m_ScriptName && strcmp(binding.m_Descriptor.m_ScriptName, scriptName) == 0)
        {
            binding.Init(scriptName, nativeName, helpString, returnType, parameters, false, function);
            replaced = true;
        }
    }

    for (int i = 0; i < g_serverScriptWeaponStruct->m_NumTypedFunctions.Count(); ++i)
    {
        ScriptFunctionBinding_t& binding = g_serverScriptWeaponStruct->m_NumTypedFunctions[i];
        if (binding.m_Descriptor.m_ScriptName && strcmp(binding.m_Descriptor.m_ScriptName, scriptName) == 0)
        {
            binding.Init(scriptName, nativeName, helpString, returnType, parameters, false, function);
            replaced = true;
        }
    }

    if (!replaced)
    {
        g_serverScriptWeaponStruct->AddFunction(
            scriptName,
            nativeName,
            helpString,
            returnType,
            parameters,
            false,
            function);
    }
}

//---------------------------------------------------------------------------------
// Bounce budget exhausted before the next simulate step, so bounce_count 1 returns the first impact.
//---------------------------------------------------------------------------------

// One-shot latches -- diagnostics stay loud exactly once per session so a
// disabled/degraded path can't spam the log on every grenade-preview call.
static bool s_simGrenadeWarnedNoOwnerSentinel = false;
static bool s_simGrenadeWarnedDragUnsupported = false;
static bool s_simGrenadeWarnedNoBounceVelocity = false;

// One simulate call: end time/pos/vel, plus hit time/normal if a fine step hit.
struct SimGrenadeSimResult
{
    float endTime = 0.0f;
    Vector3D endPosition;
    Vector3D endVelocity;
    float hitTime = -1.0f;
    Vector3D hitNormal = Vector3D(0.0f, 0.0f, 1.0f);
};

// Same Ray_t layout as PlacementTraceLine but no 1.0f extent clamp.
// Owner present: collision group 16 (grenade-arc filter).
static bool ServerScript_SimGrenadeTrace(
    const Vector3D& start,
    const Vector3D& end,
    CPlayer* owner,
    const bool useHull,
    const Vector3D& hullMins,
    const Vector3D& hullMaxs,
    const unsigned int contentsMask,
    trace_t& outTrace)
{
    if (!g_pEngineTraceServer)
        return false;

    Ray_t ray;
    memset(&ray, 0, sizeof(ray));
    VectorSubtract(end, start, ray.m_Delta);
    ray.m_IsSwept = ray.m_Delta.LengthSqr() != 0.0f;
    ray.m_IsRay = !useHull;

    if (useHull)
    {
        const Vector3D center = ServerScript_ScaleVector(
            ServerScript_AddVector(hullMins, hullMaxs), 0.5f);
        const Vector3D extents = ServerScript_ScaleVector(
            ServerScript_SubVector(hullMaxs, hullMins), 0.5f);
        const Vector3D rayStart = ServerScript_AddVector(start, center);
        const Vector3D startOffset = ServerScript_ScaleVector(center, -1.0f);
        VectorCopy(startOffset, ray.m_StartOffset);
        VectorCopy(rayStart, ray.m_Start);

        // Ray_t extents at +0x30 (VectorAligned). Do not clamp; a zero hull must trace as given.
        VectorAligned* const rayExtents = reinterpret_cast<VectorAligned*>(
            reinterpret_cast<uintptr_t>(&ray) + 0x30);
        rayExtents->x = extents.x;
        rayExtents->y = extents.y;
        rayExtents->z = extents.z;
        rayExtents->w = 0.0f;
    }
    else
    {
        VectorClear(ray.m_StartOffset);
        VectorCopy(start, ray.m_Start);

        VectorAligned* const extents = reinterpret_cast<VectorAligned*>(
            reinterpret_cast<uintptr_t>(&ray) + 0x30);
        extents->x = 0.0f;
        extents->y = 0.0f;
        extents->z = 0.0f;
        extents->w = 0.0f;
    }

    VectorAligned* const upDir = reinterpret_cast<VectorAligned*>(
        reinterpret_cast<uintptr_t>(&ray) + 0x40);
    upDir->x = 0.0f;
    upDir->y = 0.0f;
    upDir->z = 1.0f;
    upDir->w = 0.0f;

    *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(&ray) + 0x58) = 0.0f;
    *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(&ray) + 0x60) = 0;
    ray.m_pWorldAxisTransform = nullptr;

    memset(&outTrace, 0, sizeof(outTrace));
    outTrace.fraction = 1.0f;
    outTrace.endpos = end;

    if (owner)
    {
        // Collision group 16 -- the S21 grenade-arc filter's group.
        CTraceFilterSimple filter(reinterpret_cast<const IHandleEntity*>(owner), 16);
        g_pEngineTraceServer->TraceRayFiltered(ray, contentsMask, &filter, &outTrace);
    }
    else
    {
        g_pEngineTraceServer->TraceRay(ray, contentsMask, &outTrace);
    }
    return true;
}

// Coarse 0.4s hull, fine 0.05s near a hit. Zero-drag, startTime 0.
static void ServerScript_SimGrenadeSimulate(
    const Vector3D& startPos,
    const Vector3D& startVel,
    const float gravity,
    const float duration,
    const Vector3D& hullMins,
    const Vector3D& hullMaxs,
    const bool customHull,
    CPlayer* owner,
    SimGrenadeSimResult& out)
{
    static constexpr float SIMGRENADE_FINE = 0.05f;          // fine step
    static constexpr float SIMGRENADE_COARSE = 0.4f;         // coarse step (S21 bakes traceskipcount=8 * 0.05)
    static constexpr float SIMGRENADE_MIN_SUBDIV = 0.005f;   // startsolid bisection floor
    static constexpr unsigned int SIMGRENADE_LINE_MASK = TRACE_MASK_GRENADE;              // 0x4640400B
    static constexpr unsigned int SIMGRENADE_HULL_MASK = TRACE_MASK_GRENADE & ~CONTENTS_HITBOX; // hull sweeps skip hitboxes
    static constexpr int SIMGRENADE_SAFETY_MAX_ITERATIONS = 512; // project loop-safety convention

    Vector3D mins = hullMins;
    Vector3D maxs = hullMaxs;
    if (!customHull)
    {
        mins = Vector3D(-3.0f, -3.0f, -3.0f);
        maxs = Vector3D(3.0f, 3.0f, 3.0f);
    }

    // Coarse-sweep sag pad so a hull sweep cannot tunnel a ceiling-height surface mid-step.
    Vector3D padMaxs = maxs;
    padMaxs.z += ((-gravity) * 0.125f * SIMGRENADE_COARSE) * SIMGRENADE_COARSE;

    const auto ParabolaPos = [&](const float t) -> Vector3D
    {
        Vector3D p = ServerScript_AddScaled(startPos, startVel, t);
        p.z += 0.5f * gravity * t * t;
        return p;
    };
    const auto ParabolaVz = [&](const float t) -> float
    {
        return startVel.z + gravity * t;
    };

    out.endTime = 0.0f;
    out.endPosition = ParabolaPos(0.0f);
    out.endVelocity = startVel;
    out.hitTime = -1.0f;
    out.hitNormal = Vector3D(0.0f, 0.0f, 1.0f);

    float t = 0.0f;
    int smallTraceCount = 0;
    int smallTraceBank = 2;

    for (int safety = 0; safety < SIMGRENADE_SAFETY_MAX_ITERATIONS && t < duration; ++safety)
    {
        if (smallTraceCount == 0)
        {
            // Coarse sweep.
            const float t1 = fminf(duration, t + SIMGRENADE_COARSE);
            trace_t trace;
            ServerScript_SimGrenadeTrace(ParabolaPos(t), ParabolaPos(t1), owner,
                /*useHull=*/true, mins, padMaxs, SIMGRENADE_HULL_MASK, trace);

            if (!trace.startsolid && trace.fraction == 1.0f)
            {
                t = t1;
                out.endTime = t;
                out.endPosition = ParabolaPos(t1);
                continue;
            }

            const int k = trace.startsolid ? 0 : static_cast<int>(((t1 - t) / SIMGRENADE_FINE) * trace.fraction);
            t = fminf(duration, t + static_cast<float>(k) * SIMGRENADE_FINE);
            out.endTime = t;
            out.endPosition = ParabolaPos(t);
            out.endVelocity = Vector3D(startVel.x, startVel.y, ParabolaVz(t)); // ONLY place endVelocity updates
            smallTraceCount = smallTraceBank;
            smallTraceBank += 2;
            continue;
        }

        // Fine step, with startsolid bisection.
        float t2 = fminf(duration, t + SIMGRENADE_FINE);
        for (int bisect = 0; bisect < SIMGRENADE_SAFETY_MAX_ITERATIONS; ++bisect)
        {
            trace_t trace;
            if (customHull)
            {
                ServerScript_SimGrenadeTrace(ParabolaPos(t), ParabolaPos(t2), owner,
                    /*useHull=*/true, mins, maxs /*REAL maxs, no pad*/, SIMGRENADE_HULL_MASK, trace);
            }
            else
            {
                ServerScript_SimGrenadeTrace(ParabolaPos(t), ParabolaPos(t2), owner,
                    /*useHull=*/false, mins, maxs, SIMGRENADE_LINE_MASK, trace);
            }

            if (trace.fraction >= 1.0f)
            {
                t = t2;
                out.endTime = t;
                out.endPosition = ParabolaPos(t2);
                --smallTraceCount;
                break; // back to outer loop
            }

            if (trace.fraction != 0.0f || t != 0.0f)
            {
                t = t + (t2 - t) * trace.fraction;
                out.endTime = t;
                out.endPosition = trace.endpos;
                out.hitTime = t;
                out.hitNormal = trace.plane.normal;
                return; // HIT
            }

            if (t2 < SIMGRENADE_MIN_SUBDIV)
            {
                out.hitTime = t;
                out.hitNormal = trace.plane.normal;
                out.endPosition = trace.endpos;
                return; // HIT (spawned in solid)
            }

            t2 *= 0.5f; // bisect and retry
        }
    }
    // Outer loop exhausted: no hit, hitTime stays -1. No skin offset; next segment starts at endpos.
}

static Vector3D ServerScript_SimulateGrenadeImpactPos(
    void* pWeapon,
    const Vector3D& initialPositionOverride,
    const Vector3D& velocityOverride,
    const float durationOverride,
    const int bounceCountOverride)
{
    static const Vector3D s_zero(0.0f, 0.0f, 0.0f);


    static constexpr ptrdiff_t WEAPON_OFF_MODVARS = 6112;              // float array passed to CalculateBounceVelocity
    static constexpr ptrdiff_t WEAPON_OFF_FUSE_TIME = 7172;            // modvars+1060 "grenade_fuse_time"
    static constexpr ptrdiff_t WEAPON_OFF_IGNORE_BASE_VEL = 7188;      // modvars+1076, byte
    static constexpr ptrdiff_t WEAPON_OFF_GRAVITY_SCALE = 7216;        // modvars+1104 "projectile_gravity_scale"
    static constexpr ptrdiff_t WEAPON_OFF_ARC_BOUNCE_COUNT = 7184;     // modvars+1072 "grenade_arc_indicator_bounce_count"
    static constexpr ptrdiff_t WEAPON_OFF_HULL_MINS = 7148;            // modvars+1036 "grenade_hull_mins"
    static constexpr ptrdiff_t WEAPON_OFF_HULL_MAXS = 7160;            // modvars+1048 "grenade_hull_maxs"
    static constexpr ptrdiff_t WEAPON_OFF_DRAG_COEFF = 7220;           // modvars+1108 "projectile_drag_coefficient", WARN-ONLY (unsupported)
    static constexpr ptrdiff_t PLAYER_OFF_BASE_VELOCITY = 972;         // CPlayer m_vecBaseVelocity (3 floats)

    const uintptr_t weaponAddr = reinterpret_cast<uintptr_t>(pWeapon);
    CPlayer* const owner = ServerScript_GetWeaponOwnerPlayer(pWeapon);

    // -- Phase 1: launch state (S3 ProjectilePath_Init + S21 override tail) --
    Vector3D pos = s_zero;
    Vector3D vel = s_zero;

    if (owner)
    {
        if (v_WeaponX_GetFiringPositionAndAimDirection)
        {
            Vector3D dir;
            v_WeaponX_GetFiringPositionAndAimDirection(pWeapon, owner, &pos, &dir);

            if (v_WeaponX_GrenadeVelocityFromDir)
            {
                Vector3D tmp;
                vel = *v_WeaponX_GrenadeVelocityFromDir(pWeapon, &tmp, &dir, /*fromNpcThrow=*/false);

                const bool ignoreBaseVelocity = weaponAddr &&
                    *reinterpret_cast<uint8_t*>(weaponAddr + WEAPON_OFF_IGNORE_BASE_VEL) != 0;
                if (ignoreBaseVelocity)
                {
                    const Vector3D& baseVel = *reinterpret_cast<const Vector3D*>(
                        reinterpret_cast<uintptr_t>(owner) + PLAYER_OFF_BASE_VELOCITY);
                    vel = ServerScript_SubVector(vel, baseVel);
                }
            }
        }
    }

    // No owner and no engine launch state -- overrides must supply pos/vel.
    const bool haveEngineOrigin = (owner && v_WeaponX_GetFiringPositionAndAimDirection);
    const bool haveEngineVelocity = (owner && v_WeaponX_GetFiringPositionAndAimDirection && v_WeaponX_GrenadeVelocityFromDir);

    if (initialPositionOverride != s_zero)
        pos = initialPositionOverride;
    else if (!haveEngineOrigin && !s_simGrenadeWarnedNoOwnerSentinel)
    {
        Warning(eDLL_T::SERVER,
            "[SIMGRENADE] no owner/engine-derived position and ZERO_VECTOR override -- "
            "returning origin\n");
        s_simGrenadeWarnedNoOwnerSentinel = true;
    }

    if (velocityOverride != s_zero)
        vel = velocityOverride;
    else if (!haveEngineVelocity && !s_simGrenadeWarnedNoOwnerSentinel)
    {
        Warning(eDLL_T::SERVER,
            "[SIMGRENADE] no owner/engine-derived velocity and ZERO_VECTOR override -- "
            "returning origin\n");
        s_simGrenadeWarnedNoOwnerSentinel = true;
    }

    if (initialPositionOverride == s_zero && velocityOverride == s_zero && !haveEngineOrigin && !haveEngineVelocity)
        return s_zero;

    const float fuse = weaponAddr ? *reinterpret_cast<float*>(weaponAddr + WEAPON_OFF_FUSE_TIME) : 0.0f;
    float duration = (fuse == 0.0f) ? 3.0f : fminf(fuse, 2.0f);

    ConVar* const gravityCvar = g_pCVar ? g_pCVar->FindVar("sv_gravity") : nullptr;
    const float sv_gravity = gravityCvar ? gravityCvar->GetFloat() : 800.0f;
    const float gravityScale = weaponAddr ? *reinterpret_cast<float*>(weaponAddr + WEAPON_OFF_GRAVITY_SCALE) : 1.0f;
    const float gravity = -(gravityScale * sv_gravity);

    // S21 Init tail semantics -- ZERO_VECTOR/<=0 sentinels.
    if (durationOverride > 0.0f)
        duration = durationOverride;

    const float dragCoeff = weaponAddr ? *reinterpret_cast<float*>(weaponAddr + WEAPON_OFF_DRAG_COEFF) : 0.0f;
    if (dragCoeff != 0.0f && !s_simGrenadeWarnedDragUnsupported)
    {
        Warning(eDLL_T::SERVER,
            "[SIMGRENADE] weapon has projectile_drag_coefficient=%f, drag unsupported, "
            "prediction will drift\n", dragCoeff);
        s_simGrenadeWarnedDragUnsupported = true;
    }

    // Bounce budget, capped at 3.
    int budget = (bounceCountOverride >= 0) ? bounceCountOverride
        : (weaponAddr ? *reinterpret_cast<int*>(weaponAddr + WEAPON_OFF_ARC_BOUNCE_COUNT) : 0);
    if (budget > 3)
        budget = 3;
    // No 6-bounce landing-position branch.

    // -- Hull setup (shared across every simulate call for this native call) --
    const Vector3D hullMins = weaponAddr
        ? *reinterpret_cast<const Vector3D*>(weaponAddr + WEAPON_OFF_HULL_MINS) : s_zero;
    const Vector3D hullMaxs = weaponAddr
        ? *reinterpret_cast<const Vector3D*>(weaponAddr + WEAPON_OFF_HULL_MAXS) : s_zero;
    const bool customHull = !(hullMins == s_zero && hullMaxs == s_zero);

    // do-while; break before simulating once the bounce budget is exhausted.
    Vector3D result = s_zero;
    int iter = 0;
    float remaining = 0.0f;
    float lastHitTime = -1.0f;

    if (duration > 0.0f)
    {
        static constexpr int SIMGRENADE_SAFETY_MAX_BOUNCE_ITERATIONS = 512; // project loop-safety convention

        do
        {
            if (iter >= budget || iter >= SIMGRENADE_SAFETY_MAX_BOUNCE_ITERATIONS)
                break;

            SimGrenadeSimResult out;
            ServerScript_SimGrenadeSimulate(pos, vel, gravity, duration,
                hullMins, hullMaxs, customHull, owner, out);

            remaining = duration - out.endTime;
            result = out.endPosition;
            lastHitTime = out.hitTime;

            if (remaining > 0.0f)
            {
                bool roll = false;
                if (v_CalculateBounceVelocity)
                {
                    v_CalculateBounceVelocity(&out.endVelocity, &out.hitNormal,
                        weaponAddr ? reinterpret_cast<const float*>(weaponAddr + WEAPON_OFF_MODVARS) : nullptr,
                        &roll, /*forceRollFrac=*/false);
                }
                else if (!s_simGrenadeWarnedNoBounceVelocity)
                {
                    Warning(eDLL_T::SERVER,
                        "[SIMGRENADE] CalculateBounceVelocity unresolved -- treating every hit "
                        "as final\n");
                    s_simGrenadeWarnedNoBounceVelocity = true;
                    remaining = 0.0f; // fall back: first hit is final rest position
                }
            }

            pos = out.endPosition;
            vel = out.endVelocity;
            duration = remaining;
            ++iter;
        } while (remaining > 0.0f);
    }

    return result;
}

static SQRESULT Script_SimulateGrenadeImpactPos(HSQUIRRELVM v)
{
    void* pWeapon = nullptr;
    if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)) || !pWeapon)
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    const SQVector3D* posOverrideSQ = nullptr;
    const SQVector3D* velOverrideSQ = nullptr;
    sq_getvector(v, 2, &posOverrideSQ);
    sq_getvector(v, 3, &velOverrideSQ);

    SQFloat durationOverride = -1.0f;
    SQInteger bounceCountOverride = -1;
    sq_getfloat(v, 4, &durationOverride);
    sq_getinteger(v, 5, &bounceCountOverride);

    const Vector3D posOverride = posOverrideSQ
        ? Vector3D(posOverrideSQ->x, posOverrideSQ->y, posOverrideSQ->z)
        : Vector3D(0.0f, 0.0f, 0.0f);
    const Vector3D velOverride = velOverrideSQ
        ? Vector3D(velOverrideSQ->x, velOverrideSQ->y, velOverrideSQ->z)
        : Vector3D(0.0f, 0.0f, 0.0f);

    const Vector3D result = ServerScript_SimulateGrenadeImpactPos(
        pWeapon, posOverride, velOverride,
        static_cast<float>(durationOverride), static_cast<int>(bounceCountOverride));

    const SQVector3D resultSQ(result.x, result.y, result.z);
    sq_pushvector(v, &resultSQ);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// m_scriptActivated: set on activate, cleared on deactivate.
//-----------------------------------------------------------------------------
static SQRESULT Script_IsWeaponActivated(HSQUIRRELVM v)
{
    void* pWeapon = nullptr;
    if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)) || !pWeapon)
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    sq_pushbool(v, *reinterpret_cast<const bool*>(
        reinterpret_cast<uintptr_t>(pWeapon) + SERVER_WEAPON_SCRIPT_ACTIVATED_OFFSET));
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}


void Script_RegisterDedicatedWeaponNatives(void)
{
	if (!g_serverScriptWeaponStruct)
		return;

    ServerScript_UpsertWeaponFunction(
        "GetObjectPlacementParent",
        "Script_GetPlacementParentShim",
        "Gets the placement parent entity",
        "entity",
        "",
        reinterpret_cast<ScriptFunctionBindingStorageType_t>(ServerScript_GetPlacementParentShim));
    ServerScript_UpsertWeaponFunction(
        "GetObjectPlacementOrigin",
        "Script_GetPlacementOriginShim",
        "Gets the placement origin",
        "vector",
        "",
        reinterpret_cast<ScriptFunctionBindingStorageType_t>(ServerScript_GetPlacementOriginShim));
    ServerScript_UpsertWeaponFunction(
        "GetObjectPlacementAngles",
        "Script_GetPlacementAnglesShim",
        "Gets the placement angles",
        "vector",
        "",
        reinterpret_cast<ScriptFunctionBindingStorageType_t>(ServerScript_GetPlacementAnglesShim));
    ServerScript_UpsertWeaponFunction(
        "GetObjectPlacementSpecialOrigin",
        "Script_GetPlacementSpecialOriginShim",
        "Gets the secondary placement origin",
        "vector",
        "",
        reinterpret_cast<ScriptFunctionBindingStorageType_t>(ServerScript_GetPlacementSpecialOriginShim));
    ServerScript_UpsertWeaponFunction(
        "GetObjectPlacementSpecialAngles",
        "Script_GetPlacementSpecialAnglesShim",
        "Gets the secondary placement angles",
        "vector",
        "",
        reinterpret_cast<ScriptFunctionBindingStorageType_t>(ServerScript_GetPlacementSpecialAnglesShim));
    ServerScript_UpsertWeaponFunction(
        "GetObjectPlacementSpecialParent",
        "Script_GetPlacementSpecialParentShim",
        "Gets the secondary placement parent entity",
        "entity",
        "",
        reinterpret_cast<ScriptFunctionBindingStorageType_t>(ServerScript_GetPlacementSpecialParentShim));
    ServerScript_UpsertWeaponFunction(
        "ObjectPlacementHasValidSpot",
        "Script_PlacementHasValidSpotShim",
        "Returns whether placement has a usable server spot",
        "bool",
        "",
        reinterpret_cast<ScriptFunctionBindingStorageType_t>(ServerScript_PlacementHasValidSpotShim));
    ServerScript_UpsertWeaponFunction(
        "GetObjectPlacementSpecialPlacementResult",
        "Script_GetPlacementSpecialPlacementResultShim",
        "Gets the special-placement result code (0=success, see enum for rejects)",
        "int",
        "",
        reinterpret_cast<ScriptFunctionBindingStorageType_t>(ServerScript_GetPlacementSpecialPlacementResultShim));

    ServerScript_PrecacheObjectPlacementModels();
    // Server VM only -- registering on the client would shadow the compiled native.
    g_serverScriptWeaponStruct->AddFunction(
        "SimulateGrenadeImpactPos",
        "Script_SimulateGrenadeImpactPos",
        "S3 native gap-fill: engine-backed ballistic step simulation returning the "
        "predicted impact position (S21-authentic 4-arg signature). ZERO_VECTOR "
        "position/velocity sentinels derive from the weapon's real firing position "
        "and aim direction plus the grenade's own KV settings (via the S3 engine's "
        "own GetFiringPositionAndAimDirection/GrenadeVelocityFromDir); duration<=0 "
        "uses the weapon's fuse-time default (capped at 2s); bounceCount<0 uses "
        "grenade_arc_indicator_bounce_count.",
        "vector",
        "vector initialPositionOverride, vector velocityOverride, float durationOverride, int bounceCountOverride",
        false,
        Script_SimulateGrenadeImpactPos);

    g_serverScriptWeaponStruct->AddFunction(
        "IsWeaponActivated",
        "Script_IsWeaponActivated",
        "S3 native gap-fill: returns whether this weapon is currently activated, "
        "i.e. between its activate and deactivate callbacks.",
        "bool",
        "",
        false,
        Script_IsWeaponActivated);
}
