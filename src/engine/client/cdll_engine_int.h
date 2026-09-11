#pragma once
#ifndef DEDICATED // We should think about not including this file at all in dedicated tbh.
#include "public/globalvars_base.h"
#include "public/client_class.h"
#endif // !DEDICATED
#include "game/shared/usercmd.h"

enum class ClientFrameStage_t : int
{
	FRAME_UNDEFINED = -1, // (haven't run any frames yet)
	FRAME_START,

	// A network packet is being received
	FRAME_NET_UPDATE_START,
	// Data has been received and we're going to start calling PostDataUpdate
	FRAME_NET_UPDATE_POSTDATAUPDATE_START,
	// Data has been received and we've called PostDataUpdate on all data recipients
	FRAME_NET_UPDATE_POSTDATAUPDATE_END,
	// We've received all packets, we can now do interpolation, prediction, etc..
	FRAME_NET_UPDATE_END,

	// We're about to start rendering the scene
	FRAME_RENDER_START,
	// We've finished rendering the scene.
	FRAME_RENDER_END,

	FRAME_NET_FULL_FRAME_UPDATE_ON_REMOVE
};

class CHLClient
{
public:
	static int Init(CHLClient* thisptr, CreateInterfaceFn appSystemFactory, CGlobalVarsBase* pGlobals);
	static int PostInit(CHLClient* thisptr);
	static void FrameStageNotify(CHLClient* pHLClient, ClientFrameStage_t curStage);

#ifndef DEDICATED
	ClientClass* GetAllClasses();
#endif

	void CreateMove(int sequenceNumber, float inputSampleFrameTime, bool active)
	{
		const static int index = 27;
		CallVFunc<void>(index, this, sequenceNumber, inputSampleFrameTime, active);
	}

	CUserCmd* GetUserCmd(int sequenceNumber) // @ in R5pc_r5launch_N1094_CL456479_2019_10_30_05_20_PM
	{
		const static int index = 28;
		return CallVFunc<CUserCmd*>(index, this, sequenceNumber); /*48 83 EC 28 48 8B 05 ? ? ? ? 48 8D 0D ? ? ? ? 44 8B C2*/
	}

	bool DispatchUserMessage(int msgType, bf_read* msgData)
	{
		const static int index = 59;
		return CallVFunc<bool>(index, this, msgType, msgData);
	}

	void SetSoundState(const i8 bState)
	{
		const static int index = 125;
		CallVFunc<void>(index, this, bState);
	}
};

/* ==== CHLCLIENT ======================================================================================================================================================= */
#ifndef DEDICATED
inline int(*CHLClient__Init)(CHLClient* thisptr, CreateInterfaceFn appSystemFactory, CGlobalVarsBase* pGlobals);
inline int(*CHLClient__PostInit)(CHLClient* thisptr);
inline void*(*CHLClient__LevelShutdown)(CHLClient* thisptr);
inline void(*CHLClient__SetSoundState)(CHLClient* thisptr, bool bActive);
inline void(*CHLClient__FrameStageNotify)(CHLClient* thisptr, ClientFrameStage_t frameStage);
inline ClientClass*(*CHLClient__GetAllClasses)();
#endif // !DEDICATED

inline CHLClient* g_pHLClient = nullptr;

