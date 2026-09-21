#ifndef DODGE_GATE_H
#define DODGE_GATE_H

//=============================================================================//
//
// Purpose: usercmd dodge gate -- ground and once-per-airtime rules the S3
// movement code has no settings field for.
//
//=============================================================================//
class CPlayer;
class CUserCmd;

// Strips IN_DODGE from the cmd when the rules refuse it; call before RunCommand.
void DodgeGate_PreRun(CPlayer* player, CUserCmd* ucmd);

// Latches an accepted airborne dodge; call after RunCommand with the same cmd.
void DodgeGate_PostRun(CPlayer* player, CUserCmd* ucmd);

#endif // DODGE_GATE_H
