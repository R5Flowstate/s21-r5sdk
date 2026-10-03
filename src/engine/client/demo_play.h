//=============================================================================//
//
// Purpose: client demo player. Plays a .r5dem through the bridge's own receive
//          path: a virtual connect, the demo as the only datagram source, and
//          every send swallowed at the bridge's transmit chokepoint.
//
//=============================================================================//
#ifndef ENGINE_CLIENT_DEMO_PLAY_H
#define ENGINE_CLIENT_DEMO_PLAY_H

#include "thirdparty/detours/include/idetour.h"
#include <string>
#include <vector>

constexpr const char* DEMO_CLIENT_ROOT = "platform\\demos";

bool  DemoPlay_Start(const char* pszName, const int nPov, const float flSeekSec);
bool  DemoPlay_Stop(const char* pszReason);
bool  DemoPlay_IsActive(void);

void  DemoPlay_SetPaused(const bool bPaused);
bool  DemoPlay_IsPaused(void);
void  DemoPlay_Step(void);
void  DemoPlay_SetTimescale(const float flScale);
float DemoPlay_GetTimescale(void);
bool  DemoPlay_Seek(const float flSeconds);
float DemoPlay_GetTime(void);
bool  DemoPlay_IsSeeking(void);
float DemoPlay_GetDuration(void);

// The overlay's clickable layer is up: the replay owns the pointer.
void  DemoPlay_SetMouseLayer(const bool bOn);
bool  DemoPlay_WantsCursor(void);

bool  DemoPlay_SetPov(const int nPov);
int   DemoPlay_GetPov(void);
int   DemoPlay_GetPovCount(void);
const char* DemoPlay_GetPovName(const int nPov);

// povId in [0, povCount) views that pov's eyes; -1 restores the own pov.
bool  DemoPlay_SetView(const int nPovId, const bool bChase);
int   DemoPlay_GetView(void);
void  DemoPlay_SetFreecam(const bool bOn);
bool  DemoPlay_IsFreecam(void);

enum DemoCamMode_t
{
	DEMO_CAM_FIRST = 0,
	DEMO_CAM_THIRD,
	DEMO_CAM_FREE,
	DEMO_CAM_COUNT
};
bool  DemoPlay_SetCamera(const int nMode);
int   DemoPlay_GetCamera(void);

// Free camera roll and field of view in degrees; a field of view under 10 is the game's.
void  DemoPlay_SetCamRoll(const float flDegrees);
float DemoPlay_GetCamRoll(void);
void  DemoPlay_SetCamFov(const float flDegrees);
float DemoPlay_GetCamFov(void);
// Free camera depth of field: focus distance (0 = off) and the sharp depth around it.
void  DemoPlay_SetCamDof(const float flFocus, const float flRange);
float DemoPlay_GetCamDof(void);
float DemoPlay_GetCamDofRange(void);
// WM_MOUSEWHEEL delta: roll, or field of view with Ctrl, while the free camera is on.
void  DemoPlay_OnMouseWheel(const int nDelta);
// Raw relative mouse motion in counts; the free camera looks with it, playing or paused.
void  DemoPlay_OnRawMouse(const int nDx, const int nDy);
// Free camera follow: keeps its offset from the watched player and turns with them.
void  DemoPlay_SetCamFollow(const bool bOn);
bool  DemoPlay_IsCamFollow(void);

// Camera path: free camera keyframes on the replay timeline, kept in
// <file>.campath beside the replay. Indices are 0-based and by time.
bool  DemoPlay_CamKeyAdd(void);
bool  DemoPlay_CamKeyRemove(const int nIndex);
void  DemoPlay_CamKeyClear(void);
bool  DemoPlay_CamKeyGoto(const int nIndex);
bool  DemoPlay_CamKeyStep(const int nDir);
void  DemoPlay_GetCamKeyTimes(std::vector<float>& out);
// Four floats per keyframe: seconds, roll, field of view, focus distance (0 = off).
void  DemoPlay_GetCamKeyData(std::vector<float>& out);
// The keyframe at the playhead, give or take a quarter second; -1 if none.
int   DemoPlay_CamKeyAtNow(void);
bool  DemoPlay_SetCamPathPlaying(const bool bOn);
bool  DemoPlay_IsCamPathPlaying(void);

// Bookmarks live in <file>.marks beside the replay; clips are new files named
// "<replay>_c<mmss>", cut at the end time, that open at their start time.
bool  DemoPlay_AddBookmark(void);
bool  DemoPlay_SaveClip(const float flStartSec, const float flEndSec, char* pszOutName, const size_t nOutLen);

std::string DemoPlay_GetMetaJson(void);
void  DemoPlay_GetEvents(std::vector<std::string>& outJson, std::vector<std::string>& outRecords);
// "buttons|forward|side|up|pitch|yaw" of the pov's command nearest flSeconds.
bool  DemoPlay_GetInputsAt(const int nPov, const float flSeconds, char* pszOut, const size_t nOutLen);

///////////////////////////////////////////////////////////////////////////////
class VDemoPlayer : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // ENGINE_CLIENT_DEMO_PLAY_H
