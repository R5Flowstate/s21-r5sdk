//=============================================================================//
//
// Purpose: Halo vehicles on the dedicated server: registry, seats, driver
//          input and the 30 Hz simulation that drives each vehicle prop.
//
//=============================================================================//
#ifndef HALO_VEHICLE_H
#define HALO_VEHICLE_H

class CPlayer;
class CSquirrelVM;
class CUserCmd;

enum HaloSeat_t
{
	HALO_SEAT_DRIVER = 0,
	HALO_SEAT_GUNNER,
	HALO_SEAT_PASSENGER,
	HALO_SEAT_COUNT
};

void HaloVehicle_Frame(void);
// Captures a seated driver's input and keeps every seated player from walking.
void HaloVehicle_OnRunCommand(CPlayer* pPlayer, CUserCmd* pCmd);
void HaloVehicle_LevelShutdown(void);
void HaloVehicle_RegisterServerNatives(CSquirrelVM* s);

#endif // HALO_VEHICLE_H
