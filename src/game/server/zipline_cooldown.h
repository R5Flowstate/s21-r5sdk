//=============================================================================//
//
// Purpose: S21 client zipline re-mount cooldown parity on the S3 dedicated
// server. See zipline_cooldown.cpp for the ladder and the gate.
//
//=============================================================================//
#ifndef ZIPLINE_COOLDOWN_H
#define ZIPLINE_COOLDOWN_H

// Mount gate is Hook_Zipline_Use; refusing means not calling the original.
bool ZiplineCooldown_ShouldRefuseMount(void* player);

// Steps the lockout ladder after the engine granted a mount.
void ZiplineCooldown_OnMountGranted(void* player);

// Ages the zipline re-mount lockout. Call once per movement command, only while
// the player is actually attached to a zipline. No-op unless the decay path is
// selected -- the shipping path never ages the lockout at all.
void ZiplineCooldown_OnRideCommand(void* player);

// Reports the player's ground state once per movement command. Landing without
// being on a zipline clears the lockout ladder.
void ZiplineCooldown_OnGroundState(void* player, const bool bOnGround, const bool bZiplining);

#endif // ZIPLINE_COOLDOWN_H
