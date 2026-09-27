//=============================================================================//
//
// Purpose: client demo recorder controls
//
//=============================================================================//
#ifndef ENGINE_CLIENT_DEMO_RECORD_H
#define ENGINE_CLIENT_DEMO_RECORD_H

bool DemoRecord_Start(const char* pszName, const bool bFromConnect = false);
void DemoRecord_Stop(const char* pszReason);
bool DemoRecord_IsRecording(void);
void DemoRecord_SetMeta(const char* pszJson);
bool DemoRecord_AddEvent(const char* pszType, const char* pszAttacker, const char* pszVictim,
	const char* pszWeapon, const float flDamage);
void DemoRecord_SetForceFullUpdateFn(void (*pfn)(void));

bool DemoPlay_Stop(const char* pszReason);

#endif // ENGINE_CLIENT_DEMO_RECORD_H
