#ifndef ZIPLINE_DISCONNECT_H
#define ZIPLINE_DISCONNECT_H

bool ZipDisc_ShouldRefuseMount(void* player);
void ZipDisc_OnMountGranted(void* player);
void ZipDisc_OnCommand(void* player, const bool bOnGround, const bool bZiplining);
void ZipDisc_OnMantle(void* player);

#endif
