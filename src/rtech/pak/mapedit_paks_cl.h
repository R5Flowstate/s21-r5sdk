//=============================================================================//
//
// Purpose: map-editor extra map paks, client half.
//
//=============================================================================//
#ifndef RTECH_PAK_MAPEDIT_PAKS_CL_H
#define RTECH_PAK_MAPEDIT_PAKS_CL_H

class CSquirrelVM;

void MapEditPaks_RegisterClientFunctions(CSquirrelVM* s);
void MapEditPaks_UnloadAll(const char* reason);

#endif // RTECH_PAK_MAPEDIT_PAKS_CL_H
