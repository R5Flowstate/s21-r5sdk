//=============================================================================//
//
// Purpose: Highlight system context helpers
//
//=============================================================================//
#if defined(CLIENT_DLL)
#ifndef HIGHLIGHT_CONTEXT_H
#define HIGHLIGHT_CONTEXT_H

// No client-side highlight-context surface. The S21 client binds these natives
// itself and the SDK registration path that reached this half no longer exists.

#endif // HIGHLIGHT_CONTEXT_H

#else // !CLIENT_DLL
#ifndef HIGHLIGHT_CONTEXT_H
#define HIGHLIGHT_CONTEXT_H

#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "thirdparty/detours/include/idetour.h"
#include "game/shared/vscript_gamedll_defs.h"

void HighlightContext_LevelShutdown();
void HighlightContext_RegisterDrawFuncEnum(HSQUIRRELVM v);
void HighlightContext_RegisterEntityOverrides(ScriptClassDescriptor_t* entityStruct);

// Global function natives
SQRESULT Script_HighlightContext_GetId(HSQUIRRELVM v);
SQRESULT Script_HighlightContext_SetParam(HSQUIRRELVM v);
SQRESULT Script_HighlightContext_GetParam(HSQUIRRELVM v);
SQRESULT Script_HighlightContext_SetDrawFunc(HSQUIRRELVM v);
SQRESULT Script_HighlightContext_GetDrawFunc(HSQUIRRELVM v);
SQRESULT Script_HighlightContext_SetRadius(HSQUIRRELVM v);
SQRESULT Script_HighlightContext_GetOutlineRadius(HSQUIRRELVM v);
SQRESULT Script_HighlightContext_GetInsideFunction(HSQUIRRELVM v);
SQRESULT Script_HighlightContext_GetOutlineFunction(HSQUIRRELVM v);
SQRESULT Script_HighlightContext_SetFlags(HSQUIRRELVM v);
SQRESULT Script_HighlightContext_SetNearFadeDistance(HSQUIRRELVM v);
SQRESULT Script_HighlightContext_SetFarFadeDistance(HSQUIRRELVM v);
SQRESULT Script_HighlightContext_SetFocusedColor(HSQUIRRELVM v);
SQRESULT Script_HighlightContext_IsEntityVisible(HSQUIRRELVM v);
SQRESULT Script_HighlightContext_IsAfterPostProcess(HSQUIRRELVM v);

// Setter natives scripts use to override baked-in defaults at init or on the fly.
SQRESULT Script_HighlightContext_SetFill(HSQUIRRELVM v);
SQRESULT Script_HighlightContext_SetOutline(HSQUIRRELVM v);
SQRESULT Script_HighlightContext_SetFillFocused(HSQUIRRELVM v);
SQRESULT Script_HighlightContext_SetOutlineFocused(HSQUIRRELVM v);
SQRESULT Script_HighlightContext_SetADSFade(HSQUIRRELVM v);
SQRESULT Script_HighlightContext_SetAfterPostProcess(HSQUIRRELVM v);
SQRESULT Script_HighlightContext_SetEntityVisible(HSQUIRRELVM v);
SQRESULT Script_HighlightContext_SetDisableDeathFade(HSQUIRRELVM v);

// CLIENT C_BaseEntity highlight natives. Do NOT call with server entities —
// the server impls use a different field layout (+288 / +480 / +536) and
// silently corrupt memory.
inline void(*v_ClientEnt_Highlight_SetCurrentContext)(void* entity, int contextId) = nullptr;
inline void(*v_ClientEnt_Highlight_SetParam)(void* entity, int contextId, int paramIdx, float* vec3) = nullptr;
inline void(*v_ClientEnt_Highlight_SetFunctions)(void* entity, int contextId, int insideSlot,
	bool entityVisible, int outsideSlot, float outlineRadius, int highlightId, bool afterPostProcess) = nullptr;

// Registers into the highlight slab. Without it the +0x2C38 ptr stays NULL
// and Highlight_SetVisibilityType silently no-ops.
inline void(*v_ClientEnt_Highlight_RegisterWithSystem)(void* entity, uint8_t flag) = nullptr;

//: highlight-system singleton. Table at +120 holds the
// per-handle "dirty" bits server SetFunctions flips automatically; client
// SetFunctions doesn't, so we flip it ourselves after every write.
inline void** g_pHighlightSystem = nullptr;

// Client highlight entity slab; entity ptr at +0x2C38+16*slot; NULL skips SetVisibilityType.
inline uint8_t* g_pHighlightEntitySlab = nullptr;


#endif // HIGHLIGHT_CONTEXT_H
#endif // CLIENT_DLL
