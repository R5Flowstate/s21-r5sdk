//=============================================================================//
//
// Purpose: Death field system shared declarations
//
//=============================================================================//
#if defined(CLIENT_DLL)
#ifndef DEATHFIELD_SYSTEM_H
#define DEATHFIELD_SYSTEM_H

class CSquirrelVM;

// No client-side deathfield registration. The S21 client already binds
// DeathField_IsActive / GetRadius* / PointDistanceFromFrontier against
// CWorld RecvTable arrays. Registering here would shadow those natives.
inline void DeathField_RegisterOnVM(CSquirrelVM*) {}

#endif // DEATHFIELD_SYSTEM_H
#else // !CLIENT_DLL
#ifndef DEATHFIELD_SYSTEM_H
#define DEATHFIELD_SYSTEM_H

#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "thirdparty/detours/include/idetour.h"

SQRESULT Script_DeathFieldIndex(HSQUIRRELVM v);
SQRESULT Script_SetDeathFieldIndex(HSQUIRRELVM v);
SQRESULT Script_DeathField_IsActive(HSQUIRRELVM v);
SQRESULT Script_DeathField_PointDistanceFromFrontier(HSQUIRRELVM v);
SQRESULT Script_DeathField_GetRadiusForNow(HSQUIRRELVM v);
SQRESULT Script_DeathField_GetRadiusForTime(HSQUIRRELVM v);
SQRESULT Script_DeathField_SetActive(HSQUIRRELVM v);
SQRESULT Script_DeathField_SetOrigin(HSQUIRRELVM v);
SQRESULT Script_DeathField_SetRadiusStartEnd(HSQUIRRELVM v);
SQRESULT Script_DeathField_SetTimeStartEnd(HSQUIRRELVM v);
SQRESULT Script_SetDeathFieldParams(HSQUIRRELVM v);

// Server-side deathfield index for one player, or 0 when unset. Feeds the
// DT_LocalPlayerExclusive.m_deathFieldIndex value proxy.
int DeathField_GetIndexForPlayer(void* pPlayer);

void DeathField_LevelShutdown();
void DeathField_RegisterOnVM(CSquirrelVM* s);
void DeathField_OnEntitySpawned(void* entity);

// Value proxy for one (field, ring) pair, or nullptr when either is out of range.
// The ring index is baked into the returned function, never derived from SP_OFFSET.
void* DeathField_GetRingProxy(int fieldId, int ring);

// True when fn is one of the per-(field, ring) value proxies. Those never
// dereference entity memory, so class-backing checks must not remediate them.
bool DeathField_IsRingProxy(const void* fn);

// Native-DT SoA: each m_deathField* parent is a type-10 DataTable of 64 children.
// SP_OFFSET = firstChild + k * stride; ring identity is the proxy, not the offset.
static constexpr int DF_CHILD_BASE = 4096; // well past DT_WORLD's native fields
enum DF_FieldId { DF_ISACTIVE = 0, DF_ORIGIN, DF_RADSTART, DF_RADEND, DF_TIMESTART, DF_TIMEEND, DF_FIELD_COUNT };
struct DF_FieldLayout { int firstChild; int stride; };
// firstChild = DF_CHILD_BASE + cumulative block; stride = element byte size
// (Int/Float = 4, Vector = 12). Blocks sized 64*stride so they never overlap.
static constexpr DF_FieldLayout DF_LAYOUT[DF_FIELD_COUNT] = {
	{ DF_CHILD_BASE +    0,  4 }, // m_deathFieldIsActive block [ 0, 256)
	{ DF_CHILD_BASE +  256, 12 }, // m_deathFieldOrigin block [ 256, 1024)
	{ DF_CHILD_BASE + 1024,  4 }, // m_deathFieldRadiusStart block [1024, 1280)
	{ DF_CHILD_BASE + 1280,  4 }, // m_deathFieldRadiusEnd block [1280, 1536)
	{ DF_CHILD_BASE + 1536,  4 }, // m_deathFieldTimeStart block [1536, 1792)
	{ DF_CHILD_BASE + 1792,  4 }, // m_deathFieldTimeEnd block [1792, 2048)
};

// Address of the engine CWorld pointer (mov r8, [rip+disp] in SetDeathFieldParams).
inline void** g_ppWorldEntity = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VDeathFieldSystem : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // DEATHFIELD_SYSTEM_H
#endif // CLIENT_DLL
