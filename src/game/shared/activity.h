#if defined(CLIENT_DLL)
//=============================================================================//
//
// Purpose: Activity registration system (ACT_*, ACT_VM_*, etc.)
//
//=============================================================================//
#ifndef ACTIVITY_SYSTEM_H
#define ACTIVITY_SYSTEM_H

#include "tier1/utlsymbol.h"

inline __int64 (*v_ActivityList_RegisterActivity)(const char* name, int activityId, char flag);
inline const char* (*v_ActivityList_GetActivityName)(int activityId);

inline __int64 (*v_ActivityList_RegisterActivity_Server)(const char* name, int activityId, char flag);
inline const char* (*v_ActivityList_GetActivityName_Server)(int activityId);

inline uintptr_t g_pActivityList;
inline uintptr_t g_pActivitySymbolTable;
inline int* g_pMaxActivityId;

inline uintptr_t g_pActivityList_Server;
inline uintptr_t g_pActivitySymbolTable_Server;
inline int* g_pMaxActivityId_Server;

int  RegisterCustomActivity(const char* activityName);
bool IsActivitySystemInitialized();
int  GetActivityCount();
void ListAllActivities();
int  FindActivityByName(const char* name);
void ClearCustomActivities();
int  LoadCustomActivitiesFromFile();

#endif // ACTIVITY_SYSTEM_H
#else // !CLIENT_DLL
//=============================================================================//
//
// Purpose: Activity registration system (ACT_*, ACT_VM_*, etc.)
//
//=============================================================================//
#ifndef ACTIVITY_SYSTEM_H
#define ACTIVITY_SYSTEM_H

#include "tier1/utlsymbol.h"

inline __int64 (*v_ActivityList_RegisterActivity)(const char* name, int activityId, char flag);
inline const char* (*v_ActivityList_GetActivityName)(int activityId);

inline __int64 (*v_ActivityList_RegisterActivity_Server)(const char* name, int activityId, char flag);
inline const char* (*v_ActivityList_GetActivityName_Server)(int activityId);

// Customs must land here: table complete, no model resolved yet.
inline void (*v_ActivityList_RegisterSharedActivities_Server)(void);

// True once the hook above registered scripts/activity_types.txt against the
// CURRENT activity table. The engine calls RegisterSharedActivities again after
// every ActivityList_Free, so a level change re-arms this on its own.
bool ActivityList_CustomsRegisteredAtListInit();

inline uintptr_t g_pActivityList;
inline uintptr_t g_pActivitySymbolTable;
inline int* g_pMaxActivityId;

inline uintptr_t g_pActivityList_Server;
inline uintptr_t g_pActivitySymbolTable_Server;
inline int* g_pMaxActivityId_Server;

// g_nActivityListVersion. studiohdr+0xC8 caches the version last resolved.
inline int* g_pActivityListVersion_Server;

int  RegisterCustomActivity(const char* activityName);
// Call once after a batch of RegisterCustomActivity. Does not rebuild the
// process-wide activity->sequence map -- registration order is the real constraint.
void InvalidateModelActivityBindings();
// True if CLIENT activity list is up (games.dll / client binary). On the S3
// dedicated binary the client-side patterns 0-hit -- use IsActivitySystemReady.
bool IsActivitySystemInitialized();
// True if either client OR server activity list is usable. Dedi load of
// scripts/activity_types.txt must gate on this, not the client-only check
// (host_state.cpp historically never loaded customs on dedi because of that).
bool IsActivitySystemReady();
int  GetActivityCount();
void ListAllActivities();
int  FindActivityByName(const char* name);
void ClearCustomActivities();
int  LoadCustomActivitiesFromFile();

///////////////////////////////////////////////////////////////////////////////
class VActivityList : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("ActivityList_RegisterActivity", v_ActivityList_RegisterActivity);
		LogFunAdr("ActivityList_GetActivityName", v_ActivityList_GetActivityName);
		LogVarAdr("g_pActivityList", reinterpret_cast<void*>(g_pActivityList));
		LogVarAdr("g_pActivitySymbolTable", reinterpret_cast<void*>(g_pActivitySymbolTable));
		LogVarAdr("g_pMaxActivityId", reinterpret_cast<void*>(g_pMaxActivityId));
		LogFunAdr("ActivityList_RegisterActivity_Server", v_ActivityList_RegisterActivity_Server);
		LogFunAdr("ActivityList_GetActivityName_Server", v_ActivityList_GetActivityName_Server);
		LogVarAdr("g_pActivityList_Server", reinterpret_cast<void*>(g_pActivityList_Server));
		LogVarAdr("g_pActivitySymbolTable_Server", reinterpret_cast<void*>(g_pActivitySymbolTable_Server));
		LogVarAdr("g_pMaxActivityId_Server", reinterpret_cast<void*>(g_pMaxActivityId_Server));
		LogVarAdr("g_nActivityListVersion", reinterpret_cast<void*>(g_pActivityListVersion_Server));
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // ACTIVITY_SYSTEM_H
#endif // CLIENT_DLL
