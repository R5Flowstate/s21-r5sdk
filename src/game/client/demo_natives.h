//=============================================================================//
//
// Purpose: demo replay script natives (CLIENT and UI VMs)
//
//=============================================================================//
#ifndef GAME_CLIENT_DEMO_NATIVES_H
#define GAME_CLIENT_DEMO_NATIVES_H

class CSquirrelVM;

void Demo_RegisterClientFunctions(CSquirrelVM* s);
void Demo_RegisterUIFunctions(CSquirrelVM* s);

#endif // GAME_CLIENT_DEMO_NATIVES_H
