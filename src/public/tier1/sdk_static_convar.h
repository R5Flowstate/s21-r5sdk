//=============================================================================//
//
// Purpose: S21 client-inject static ConVar/ConCommand walk helpers.
// Dual-body convar.h is on the shared PCH (no CLIENT_DLL); this header is not.
//
//=============================================================================//
#pragma once

#if defined(CLIENT_DLL)

class ConCommandBase;
class ConVar;
class ConCommand;

ConCommandBase* Sdk_FindStaticConCommandBase(const char* const name);
ConVar* Sdk_FindStaticConVar(const char* const name);
ConCommand* Sdk_FindStaticConCommand(const char* const name);
ConCommandBase* Sdk_GetStaticConCommandBaseHead();

#endif // CLIENT_DLL
