//=============================================================================//
//
// Purpose: [CHAIN] end-to-end ledger of the per-usercmd execution chain, from
// netchannel intake to the bridge ticks that hang off the executed command.
//
//=============================================================================//
#ifndef BRIDGE_CMD_CHAIN_H
#define BRIDGE_CMD_CHAIN_H

#include "thirdparty/detours/include/idetour.h"

//-----------------------------------------------------------------------------
// Stages in execution order. First counter that stays zero while the prior climbs is the break.
//-----------------------------------------------------------------------------
enum CmdChainStage_t
{
	CMDCHAIN_INTAKE_CALLS = 0,    // CPlayer::ProcessUserCmds entered
	CMDCHAIN_INTAKE_CMDS,         // commands offered by the netchannel
	CMDCHAIN_INTAKE_CLAMPED,      // commands whose frametime the intake guard changed
	CMDCHAIN_SIM_CALLS,           // CPlayer::PhysicsSimulate (SDK detour) entered
	CMDCHAIN_SIM_STOCK,           // player simulation for a real client
	CMDCHAIN_SIM_BOT,             // player simulation for a bot
	CMDCHAIN_SIM_EXECUTED,        // commands the engine body dequeued and ran
	CMDCHAIN_SIM_QUEUE_LEFT,      // commands still queued when it returned
	CMDCHAIN_WRAPPER_ENTERED,     // CPlayerMove::StaticRunCommand (our detour) entered
	CMDCHAIN_WRAPPER_BUDGET_DROP, // wrapper dropped the cmd on the movement-time budget
	CMDCHAIN_ENGINE_RETURNED,     // engine RunCommand body returned to the wrapper
	CMDCHAIN_STARTCMD_ENTERED,    // CPlayerMove::StartCommand -- witness that the RunCommand
	                              // BODY ran, true whether or not our wrapper detour fired
	CMDCHAIN_TICK_JETDRIVE,
	CMDCHAIN_TICK_ENERGIZE,
	CMDCHAIN_TICK_LASER,
	CMDCHAIN_STAMP_EXEC,
	CMDCHAIN_SIM_STARVED,       // simulate entered with an empty queue (native null command, a missed server tick)
	CMDCHAIN_COUNT
};

void     CmdChain_Bump(const CmdChainStage_t stage, const uint64_t nCount = 1);
uint64_t CmdChain_Get(const CmdChainStage_t stage);

// Last player simulation, for the gate columns (a starved gate and a dead hook look
// identical from downstream; the clock lead is what separates them).
void CmdChain_NoteGate(const int nQueued, const int nRun, const double dLead);

// Rate-limited one-line ledger + verdict. Call at the player-simulation tail.
void CmdChain_Report(void);

///////////////////////////////////////////////////////////////////////////////
class VBridgeCmdChain : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // BRIDGE_CMD_CHAIN_H
