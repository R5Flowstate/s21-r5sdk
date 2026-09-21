//=============================================================================//
//
// Purpose: Records the user commands the server executes for a player so a
// fake player can replay the exact input stream later.
//
//=============================================================================//
#ifndef CMD_RECORDER_H
#define CMD_RECORDER_H

#include "game/shared/usercmd.h"

class CPlayer;
struct ScriptClassDescriptor_t;
class CSquirrelVM;

struct CmdRecording_s
{
	bool     m_bUsed;
	bool     m_bOpen;          // still capturing
	int      m_nOwnerSlot;     // edict - 1 of the recorder, -1 once closed
	int      m_nCount;
	int      m_nCapacity;
	CUserCmd* m_pCmds;
	float    m_flDuration;     // sum of frametime
	float    m_flStartTimeBase;
	Vector3D m_vecStartOrigin;
	QAngle   m_angStartAngles;
};

void CmdRecorder_OnRunCommand(CPlayer* pPlayer, const CUserCmd* pCmd);

int  CmdRecorder_Start(CPlayer* pPlayer);
int  CmdRecorder_Stop(CPlayer* pPlayer);
bool CmdRecorder_IsRecording(const CPlayer* pPlayer);
const CmdRecording_s* CmdRecorder_Get(int nId);
void CmdRecorder_Free(int nId);
void CmdRecorder_OnPlayerGone(int nSlot);
void CmdRecorder_LevelShutdown(void);

void CmdRecorder_RegisterPlayerFuncs(ScriptClassDescriptor_t* pPlayerStruct);
void CmdRecorder_RegisterGlobalFuncs(CSquirrelVM* pVM);

#endif // CMD_RECORDER_H
