//=============================================================================//
//
// Purpose: client demo player
//
// The bridge already fakes the server for the engine: it rewrites the dedi
// handshake, drives native CONNECTED and injects every datagram. Playback puts
// the demo file at the head of that path. A synthetic S2C_CHALLENGE and
// CONNACCEPT walk the handshake; SIGNON / RELIABLE chunks go through the same
// DataBlock hand-off the fragment reassembler uses; PACKET chunks are injected
// as received datagrams on a demo clock.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier1/cvar.h"
#include "tier1/convar.h"
#include "engine/cmd.h"
#include "engine/cmodel_bsp.h"
#include "engine/client/net_bridge_internal.h"
#include "engine/shared/demo_reader.h"
#include "engine/shared/demo_json.h"
#include "engine/client/demo_bridge.h"
#include "engine/client/demo_record.h"
#include "engine/client/demo_play.h"
#include "tier0/commandline.h"
#include "engine/sys_mainwind.h"
#include "windows/input.h"
#include "inputsystem/inputsystem.h"
#include "game/client/pred_authority.h"
#include "game/client/cliententitylist.h"
#if defined(SDK_WIP)
#include "game/client/cubemap_capture.h"
#endif // SDK_WIP
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include <algorithm>

static ConVar demo_timescale("demo_timescale", "1", FCVAR_RELEASE,
	"Demo playback speed; the world and the packet feed slow down together.", true, 0.05f, true, 4.f);
// A near-zero game timescale freezes the rendered view, so a paused replay
// keeps game time running and only stops the packet feed.
static ConVar demo_pause_timescale("demo_pause_timescale", "1", FCVAR_DEVELOPMENTONLY,
	"Game timescale while the replay is paused (the packet feed stops either way).", true, 0.001f, true, 1.f);
static ConVar demo_feed_max_per_frame("demo_feed_max_per_frame", "8", FCVAR_RELEASE,
	"Demo packets fed per frame during normal playback.", true, 1.f, true, 64.f);
// The engine keeps every snapshot newer than its interpolation base until the
// next frame transitions past it; past ~30 per frame its delta store overflows
// and a snapshot fails to parse.
static ConVar demo_ff_packets_per_frame("demo_ff_packets_per_frame", "24", FCVAR_RELEASE,
	"Most demo packets fed per frame while seeking.", true, 1.f, true, 32.f);
static ConVar demo_ff_budget_ms("demo_ff_budget_ms", "40", FCVAR_RELEASE,
	"Milliseconds of packet decoding per frame while seeking.", true, 4.f, true, 250.f);
static ConVar demo_seek_restart("demo_seek_restart", "0", FCVAR_RELEASE,
	"1 = rewind by reconnecting to the demo instead of restarting from the nearest full snapshot.");
static ConVar demo_freecam_speed("demo_freecam_speed", "400", FCVAR_RELEASE,
	"Free camera speed in units per second (Shift = x3, Ctrl = x0.3; +/- or right mouse + wheel change it).",
	true, 10.f, true, 5000.f);
static ConVar demo_freecam_accel("demo_freecam_accel", "6", FCVAR_RELEASE,
	"How quickly the free camera reaches and leaves its speed, per second; 0 = instant.", true, 0.f, true, 50.f);
static ConVar demo_freecam_follow_turn("demo_freecam_follow_turn", "0", FCVAR_RELEASE,
	"1 = a following free camera (T) orbits with the player's facing; 0 keeps its world direction.");
static ConVar demo_freecam_follow_smooth("demo_freecam_follow_smooth", "4", FCVAR_RELEASE,
	"How quickly a following free camera turns after the player's facing, per second; 0 = rigid.", true, 0.f, true, 50.f);
static ConVar demo_freecam_roll("demo_freecam_roll", "0", FCVAR_RELEASE,
	"Free camera roll in degrees. Mouse wheel while the free camera is on; middle click resets.", true, -180.f, true, 180.f);
static ConVar demo_freecam_fov("demo_freecam_fov", "0", FCVAR_RELEASE,
	"Free camera field of view in degrees, 0 = the game's. Ctrl + mouse wheel while the free camera is on; "
	"Ctrl + middle click resets.", true, 0.f, true, 150.f);
static ConVar demo_freecam_dof("demo_freecam_dof", "0", FCVAR_RELEASE,
	"Free camera depth of field: the distance in focus, 0 = off. Shift + mouse wheel while the free camera is on; "
	"Shift + middle click turns it off.", true, 0.f, true, 20000.f);
static ConVar demo_freecam_dof_range("demo_freecam_dof_range", "150", FCVAR_RELEASE,
	"Free camera depth of field: the depth that stays sharp around the focus distance; blur ramps in over the same depth.",
	true, 10.f, true, 10000.f);
static ConVar demo_freecam_wheel_step("demo_freecam_wheel_step", "2", FCVAR_RELEASE,
	"Degrees of roll or field of view per mouse wheel notch.", true, 0.1f, true, 45.f);
static ConVar demo_campath_speed("demo_campath_speed", "250", FCVAR_RELEASE,
	"Camera path flight speed in units per second; a segment also lasts at least the replay time between its keys.",
	true, 10.f, true, 5000.f);
static ConVar demo_campath_keys("demo_campath_keys", "1", FCVAR_RELEASE,
	"Camera path keys while the free camera is on: K adds or replaces the keyframe at the current time, "
	"J / L jump to the previous / next one, Delete removes the one at the current time, P plays the path.");
static ConVar demo_hud("demo_hud", "1", FCVAR_RELEASE,
	"Demo HUD: 0 = stock, 1 = coaching overlay, 2 = clean view (no HUD, outlines, hit markers or damage numbers).",
	true, 0.f, true, 2.f);
static ConVar demo_diag("demo_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Log every chunk the demo player feeds.");
static ConVar demo_fuzz_dir("demo_fuzz_dir", "", FCVAR_DEVELOPMENTONLY,
	"Directory of .r5dem files to play in turn for 5 s each (fuzz harness). Cleared when done.");
static ConVar demo_input_diag("demo_input_diag", "0", FCVAR_DEVELOPMENTONLY,
	"While a replay plays, log once a second the engine and free camera angles, the camera guard, the "
	"pointer layer, input blocking and focus; the overlay script also logs each replay key it handles.");
static ConVar demo_focus_diag("demo_focus_diag", "0", FCVAR_DEVELOPMENTONLY,
	"While a replay plays, log each change of the foreground window, the engine's active-app flag and "
	"the cursor, plus slow-frame summaries.");

static constexpr int      kSignonFull          = 8;
static constexpr uint32_t kDemoChallenge       = 0x00DEC0DE;
static constexpr int      kObsModeInEye        = 5;
static constexpr int      kObsModeChase        = 6;
static constexpr int      kObsModeRoaming      = 7;
// C_Player recv-prop offsets (DT_LocalPlayerExclusive, embedded at 0).
static constexpr ptrdiff_t kPlayerObserverMode   = 0x3534;
static constexpr ptrdiff_t kPlayerObserverTarget = 0x3540;
static constexpr ULONGLONG kConnectTimeoutMs   = 30000;
static constexpr ULONGLONG kFuzzSliceMs        = 5000;
static constexpr size_t    kCamMaxKeys         = 256;
static constexpr double    kCamPathMinSegMs    = 500.0;
static constexpr float     kCamMinFov          = 10.0f;
static constexpr float     kCamMaxFov          = 150.0f;

enum class DemoPlayState_t
{
	IDLE,
	WAIT_DISCONNECT,  // leaving the live server / previous demo before connecting
	CONNECTING,       // virtual handshake + signon
	PLAYING,
	STOPPING,         // disconnect issued, gate still up until the session ends
};

struct DemoUserCmdAt_s
{
	uint32_t      tick;
	R5DemUserCmd_s cmd;
};

struct DemoViewAt_s
{
	uint32_t    wallMs;
	R5DemView_s view;
};

// One camera path keyframe: a free camera pose and the replay time it was set at.
struct DemoCamKey_s
{
	double tMs;
	float  pos[3];
	float  ang[3];      // pitch, yaw, roll
	float  fov;
	float  dof;         // focus distance, 0 = off
};

struct DemoPlayer_s
{
	CDemoReader*    pReader = nullptr;
	DemoPlayState_t state = DemoPlayState_t::IDLE;
	char            szPath[260] = {};
	char            szMap[64] = {};
	char            szMode[64] = {};
	int             nPov = 0;

	std::vector<int> order;       // chunk indices fed for this pov, seq order
	size_t          nCursor = 0;
	int             nOobStage = 0;
	bool            bReachedConnected = false;
	bool            bEnded = false;

	bool            bClockStarted = false;
	uint32_t        nClockBaseWallMs = 0;
	uint32_t        nDurationMs = 0;
	double          flDemoMs = 0.0;
	int64_t         nSeekTargetMs = -1;
	float           flPendingSeekSec = -1.0f;
	bool            bPaused = false;
	int             nStepBudget = 0;
	int             nFrameBudget = 0;
	LARGE_INTEGER   lastFrameQpc = {};
	ULONGLONG       nStateSinceMs = 0;

	int             nViewPov = -1;
	bool            bViewChase = false;
	bool            bFreecam = false;
	bool            bViewDirty = false;   // override written; restore the saved props
	int             nSavedObsMode = 0;
	uint32_t        nSavedObsTarget = 0xFFFFFFFFu;
	bool            bCamInit = false;
	float           camOrigin[3] = {};
	float           camAngles[3] = {};    // pitch, yaw, roll
	float           camVel[3] = {};       // free camera velocity, units per second
	// Follow (T): the camera keeps its offset from the watched player, optionally
	// in the frame of the player's smoothed facing.
	bool            bCamFollow = false;
	bool            bFollowYawInit = false;
	float           flFollowYaw = 0.0f;
	float           camFollowOff[3] = {};
	float           flFollowRelYaw = 0.0f;
	float           followAnchor[3] = {};
	float           flFollowSnapTime = -1.0f;  // world time of the newest snapshot seen
	LARGE_INTEGER   followSnapQpc = {};       // when it was first seen
	LARGE_INTEGER   camQpc = {};
	// The next camera init keeps camOrigin / camAngles: a restart or pov switch
	// leaves the free camera where it was.
	bool            bCamCarry = false;
	float           flCamStockFov = 0.0f;  // the game's field of view, last seen
	// The engine's depth-of-field override convars before the free camera took them.
	bool            bDofOwned = false;
	float           flDofApplied = -1.0f;
	float           flDofRangeApplied = -1.0f;
	int             nSavedDofOverride = 0;
	float           flSavedDof[4] = {};
	bool            bPathPlaying = false;
	std::vector<DemoCamKey_s> camKeys;    // in path order
	std::vector<double> camPathMs;        // each key's time on the path clock
	double          flPathMs = 0.0;        // path clock while it plays
	double          flPathUntilMs = 0.0;   // replay time the playing shot ends
	int             nCamEditKey = -1;      // the key J/L/goto landed on; K replaces it there
	char            szCamKeysPath[260] = {}; // replay camKeys belong to
	int             nCamKeysPrev = 0;      // GetAsyncKeyState edges, one bit per key
	int             nCamMode = 0;          // DemoCamMode_t
	// thirdperson_override before the own-pov third person camera took it.
	bool            bThirdPerson = false;
	int             nSavedThirdPerson = -1;
	std::vector<float> bookmarks;          // seconds, from <file>.marks

	int             nSavedPredict = -1;
	int             nSavedDrawHud = -1;
	int             nSavedHighlight = -1;
	bool            bOverlayStarted = false;

	// A restart requested while stopping.
	bool            bRestart = false;
	int             nRestartPov = 0;
	float           flRestartSeek = -1.0f;

	std::vector<std::vector<DemoUserCmdAt_s>> userCmds;
	std::vector<DemoViewAt_s> views;      // this pov's recorded view angles
	ULONGLONG       nOverlayAskMs = 0;

	// Full snapshots too large for one datagram arrive as DataBlocks; which
	// ones they are is only known once the bridge has decoded them.
	size_t          nLastBlockOrder = SIZE_MAX;
	std::vector<int> blockKeys;           // order indices, ascending
	bool            bSoftRewind = false;
	// Order index where each level's signon starts; a map reload mid-recording
	// adds one. A full snapshot only rebuilds the level it was sent in.
	std::vector<size_t> levelStarts;

	uint32_t        nLastFedTick = 0;
	int64_t         nLastFedRelMs = 0;
	ULONGLONG       nClockProbeUntilMs = 0;

	bool            bMouseLayer = false;
	bool            bCursorOwned = false;
	int             nSavedFocusSleep = -1;
	float           flSavedVolume = -1.0f;
	LARGE_INTEGER   ffFrameStartQpc = {};
	ULONGLONG       nLastRecoverMs = 0;
};

static DemoPlayer_s s_demo;
static void DemoPlay_CursorPump(void);
static void DemoPlay_RecoverPump(void);
static void DemoPlay_MutePump(void);
static void DemoCam_Pump(void);
static volatile bool s_bDemoGate = false;
static volatile LONG s_nSnapDropped = 0;
static volatile LONG s_nCamWheel = 0;   // WM_MOUSEWHEEL delta since the last frame
static volatile LONG s_nCamMouseX = 0;  // raw mouse counts since the last frame
static volatile LONG s_nCamMouseY = 0;
static volatile LONG s_nCamMouseSeen = 0; // raw mouse counts for demo_input_diag
static LONG s_nCamViewCalls = 0;            // roaming view calls for demo_input_diag
static float s_flCamLastDt = 0.0f;
static std::vector<uint8_t> s_chunkScratch;

struct DemoFuzz_s
{
	bool bActive = false;
	std::vector<std::string> files;
	size_t nNext = 0;
	ULONGLONG nSliceStartMs = 0;
	int nPlayed = 0;
};
static DemoFuzz_s s_fuzz;

typedef void (__fastcall* PFN_PlayerRoamingView)(uintptr_t pPlayer, float* pEyeOrigin, float* pEyeAngles, float* pFov);
typedef void (__fastcall* PFN_EngineGetViewAngles)(void* pUnused, float* pAngles);
typedef uintptr_t (__fastcall* PFN_GetLocalPlayer)(int nSlot);

static PFN_PlayerRoamingView   v_Player_RoamingView = nullptr;
static PFN_PlayerRoamingView   v_Player_CalcView = nullptr;
static void (__fastcall* v_View_Build)(void* pView) = nullptr;
// Saved camera the view builder copies back over the player view: origin, a
// second origin, then pitch/yaw/roll.
static float* g_pBuiltCamOrigin = nullptr;
static PFN_EngineGetViewAngles v_Engine_GetViewAngles = nullptr;
static float* g_pEngineViewAngles = nullptr;              // split-screen slot 0; stride below
static void*  g_pSplitScreenMgr = nullptr;
static constexpr size_t kViewAnglesStride = 0x41358;
static void (*v_CL_ForceFullUpdate)(void) = nullptr;
static void (__fastcall* v_Script_RunThreadsFrame)(CSquirrelVM* s, float flCurTime) = nullptr;

bool Demo_IsPlaying(void)
{
	return s_bDemoGate;
}

bool DemoPlay_IsActive(void)
{
	return s_demo.state != DemoPlayState_t::IDLE;
}

static uintptr_t DemoPlay_LocalPlayer(void)
{
	static PFN_GetLocalPlayer s_pfn = nullptr;
	if (!s_pfn)
		s_pfn = reinterpret_cast<PFN_GetLocalPlayer>(NetObs_Sym(NetObsSym_t::GetLocalPlayer));
	if (!s_pfn)
		return 0;
	uintptr_t p = 0;
	__try { p = s_pfn(0); }
	__except (EXCEPTION_EXECUTE_HANDLER) { p = 0; }
	return p;
}

int DemoPlay_LocalObserverMode(void)
{
	const uintptr_t pPlayer = DemoPlay_LocalPlayer();
	if (!pPlayer)
		return -1;
	int nMode = -1;
	__try { nMode = *reinterpret_cast<const int*>(pPlayer + kPlayerObserverMode); }
	__except (EXCEPTION_EXECUTE_HANDLER) { nMode = -1; }
	return nMode;
}

static void DemoPlay_SetThirdPerson(const bool bOn);
static void DemoCam_LoadKeys(void);
static void DemoCam_ReleaseDof(void);
static void DemoCam_UpdateDof(void);

// Back to the recorded player's own eyes with no camera state kept.
static void DemoPlay_ResetCamera(void)
{
	s_demo.nViewPov = -1;
	s_demo.bFreecam = false;
	s_demo.nCamMode = DEMO_CAM_FIRST;
	s_demo.bCamInit = false;
	s_demo.bCamCarry = false;
	s_demo.bPathPlaying = false;
	s_demo.camKeys.clear();
	s_demo.camPathMs.clear();
	s_demo.nCamEditKey = -1;
	s_demo.szCamKeysPath[0] = '\0';
}

static void DemoPlay_SetState(const DemoPlayState_t state)
{
	s_demo.state = state;
	s_demo.nStateSinceMs = GetTickCount64();
}

//-----------------------------------------------------------------------------
// Clock helpers
//-----------------------------------------------------------------------------
static bool DemoPlay_IsClockPacket(const R5DemChunkHeader_s& hdr)
{
	return hdr.type == static_cast<uint8_t>(R5DemChunk_t::PACKET)
		&& !(hdr.kind & R5DEM_KIND_PRELUDE)
		&& (hdr.signon >= kSignonFull || (hdr.kind & R5DEM_KIND_FULL));
}

static int64_t DemoPlay_RelMs(const R5DemChunkHeader_s& hdr)
{
	return static_cast<int64_t>(hdr.wallMs) - static_cast<int64_t>(s_demo.nClockBaseWallMs);
}

static void DemoPlay_BuildOrder(void)
{
	s_demo.order.clear();
	s_demo.nClockBaseWallMs = 0;
	s_demo.nDurationMs = 0;

	s_demo.levelStarts.clear();
	s_demo.levelStarts.push_back(0);

	const std::vector<DemoChunkRef_s>& chunks = s_demo.pReader->GetChunks();
	bool bBase = false;
	uint32_t nLast = 0;
	int nPrevSignonBlock = -1;
	for (size_t i = 0; i < chunks.size(); ++i)
	{
		const R5DemChunkHeader_s& h = chunks[i].hdr;
		if (h.pov != s_demo.nPov)
			continue;
		const R5DemChunk_t type = static_cast<R5DemChunk_t>(h.type);
		if (type != R5DemChunk_t::SIGNON && type != R5DemChunk_t::RELIABLE && type != R5DemChunk_t::PACKET)
			continue;
		if (type == R5DemChunk_t::SIGNON)
		{
			// Signon blocks climb within one level; a drop is the next level's serverinfo.
			if (nPrevSignonBlock >= 0 && h.signon < nPrevSignonBlock)
				s_demo.levelStarts.push_back(s_demo.order.size());
			nPrevSignonBlock = h.signon;
		}
		s_demo.order.push_back(static_cast<int>(i));
		if (DemoPlay_IsClockPacket(h))
		{
			if (!bBase)
			{
				bBase = true;
				s_demo.nClockBaseWallMs = h.wallMs;
			}
			nLast = h.wallMs;
		}
	}
	s_demo.nDurationMs = (bBase && nLast > s_demo.nClockBaseWallMs) ? nLast - s_demo.nClockBaseWallMs : 0;
}

static void DemoPlay_BuildViews(void)
{
	s_demo.views.clear();
	std::vector<uint8_t> payload;
	for (const DemoChunkRef_s& c : s_demo.pReader->GetChunks())
	{
		if (c.hdr.type != static_cast<uint8_t>(R5DemChunk_t::VIEW) || c.hdr.pov != s_demo.nPov)
			continue;
		if (!s_demo.pReader->ReadPayload(c, payload) || payload.size() < sizeof(R5DemView_s))
			continue;
		DemoViewAt_s e;
		e.wallMs = c.hdr.wallMs;
		memcpy(&e.view, payload.data(), sizeof(R5DemView_s));
		// Angles go straight to the engine view; infinite or absurd values would also overflow the interpolation.
		if (!std::isfinite(e.view.pitch) || !std::isfinite(e.view.yaw) || !std::isfinite(e.view.roll)
			|| fabsf(e.view.pitch) > 1e6f || fabsf(e.view.yaw) > 1e6f || fabsf(e.view.roll) > 1e6f)
			continue;
		s_demo.views.push_back(e);
		if (s_demo.views.size() >= 4000000)
			break;
	}
}

static void DemoPlay_BuildUserCmds(void)
{
	s_demo.userCmds.clear();
	s_demo.userCmds.resize(s_demo.pReader->GetHeader().povCount);

	std::vector<uint8_t> payload;
	for (const DemoChunkRef_s& c : s_demo.pReader->GetChunks())
	{
		if (c.hdr.type != static_cast<uint8_t>(R5DemChunk_t::USERCMD) || c.hdr.pov >= s_demo.userCmds.size())
			continue;
		if (!s_demo.pReader->ReadPayload(c, payload) || payload.size() < sizeof(R5DemUserCmdPrefix_s))
			continue;
		R5DemUserCmdPrefix_s pre;
		memcpy(&pre, payload.data(), sizeof(pre));
		const size_t nAvail = (payload.size() - sizeof(pre)) / sizeof(R5DemUserCmd_s);
		const size_t n = pre.count < nAvail ? pre.count : nAvail;
		std::vector<DemoUserCmdAt_s>& dst = s_demo.userCmds[c.hdr.pov];
		for (size_t i = 0; i < n && dst.size() < 1000000; ++i)
		{
			DemoUserCmdAt_s e;
			memcpy(&e.cmd, payload.data() + sizeof(pre) + i * sizeof(R5DemUserCmd_s), sizeof(R5DemUserCmd_s));
			e.tick = e.cmd.tickCount;
			dst.push_back(e);
		}
	}
}

//-----------------------------------------------------------------------------
// Start / stop
//-----------------------------------------------------------------------------
static void DemoPlay_CloseReader(void)
{
	if (s_demo.pReader)
	{
		delete s_demo.pReader;
		s_demo.pReader = nullptr;
	}
	s_demo.order.clear();
	s_demo.userCmds.clear();
	s_demo.views.clear();
	s_demo.blockKeys.clear();
	s_demo.levelStarts.clear();
	s_demo.nLastBlockOrder = SIZE_MAX;
	s_demo.bSoftRewind = false;
}

static bool DemoPlay_MapInstalled(const char* pszMap)
{
	AUTO_LOCK(g_InstalledMapsMutex);
	if (g_InstalledMaps.IsEmpty())
		return true;
	FOR_EACH_VEC(g_InstalledMaps, i)
	{
		if (!V_stricmp(g_InstalledMaps[i].String(), pszMap))
			return true;
	}
	return false;
}

//-----------------------------------------------------------------------------
// Bookmarks and clips
//-----------------------------------------------------------------------------
static constexpr size_t kDemoMaxBookmarks = 500;

static bool DemoPlay_BookmarkPath(char* pszOut, const size_t nOutLen)
{
	return s_demo.szPath[0] && _snprintf_s(pszOut, nOutLen, _TRUNCATE, "%s.marks", s_demo.szPath) > 0;
}

static void DemoPlay_LoadBookmarks(void)
{
	s_demo.bookmarks.clear();
	char szPath[280];
	if (!DemoPlay_BookmarkPath(szPath, sizeof(szPath)))
		return;
	const HANDLE h = CreateFileA(szPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return;
	char buf[8192];
	DWORD n = 0;
	const BOOL bRead = ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr);
	CloseHandle(h);
	if (!bRead)
		return;
	buf[n] = '\0';
	char* pCtx = nullptr;
	for (char* pLine = strtok_s(buf, "\r\n", &pCtx); pLine && s_demo.bookmarks.size() < kDemoMaxBookmarks;
		pLine = strtok_s(nullptr, "\r\n", &pCtx))
	{
		char* pEnd = nullptr;
		const double t = strtod(pLine, &pEnd);
		if (pEnd != pLine && t >= 0.0 && t < 86400.0)
			s_demo.bookmarks.push_back(static_cast<float>(t));
	}
	std::sort(s_demo.bookmarks.begin(), s_demo.bookmarks.end());
}

bool DemoPlay_AddBookmark(void)
{
	if (s_demo.state != DemoPlayState_t::PLAYING || s_demo.bookmarks.size() >= kDemoMaxBookmarks)
		return false;
	const float t = DemoPlay_GetTime();
	for (const float b : s_demo.bookmarks)
	{
		if (fabsf(b - t) < 0.5f)
			return false;
	}

	char szPath[280];
	if (!DemoPlay_BookmarkPath(szPath, sizeof(szPath)))
		return false;
	const HANDLE h = CreateFileA(szPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	char szLine[32];
	const int n = snprintf(szLine, sizeof(szLine), "%.2f\r\n", t);
	DWORD nWritten = 0;
	const bool bOk = n > 0 && WriteFile(h, szLine, static_cast<DWORD>(n), &nWritten, nullptr) && nWritten == static_cast<DWORD>(n);
	CloseHandle(h);
	if (!bOk)
		return false;

	s_demo.bookmarks.push_back(t);
	std::sort(s_demo.bookmarks.begin(), s_demo.bookmarks.end());
	Msg(eDLL_T::ENGINE, "[DEMO] bookmark at %.2f s\n", t);
	return true;
}

static bool DemoPlay_CopyRange(const HANDLE hSrc, const HANDLE hDst, const uint64_t nOffset, uint64_t nBytes,
	std::vector<uint8_t>& scratch)
{
	scratch.resize(1024 * 1024);
	uint64_t nAt = nOffset;
	while (nBytes > 0)
	{
		const DWORD nChunk = static_cast<DWORD>(nBytes < scratch.size() ? nBytes : scratch.size());
		LARGE_INTEGER li;
		li.QuadPart = static_cast<LONGLONG>(nAt);
		DWORD nRead = 0, nWritten = 0;
		if (!SetFilePointerEx(hSrc, li, nullptr, FILE_BEGIN)
			|| !ReadFile(hSrc, scratch.data(), nChunk, &nRead, nullptr) || nRead != nChunk
			|| !WriteFile(hDst, scratch.data(), nChunk, &nWritten, nullptr) || nWritten != nChunk)
			return false;
		nAt += nChunk;
		nBytes -= nChunk;
	}
	return true;
}

// A clip is the replay up to flEndSec (the stream cannot start mid-way without
// the history before it) plus a META that makes it open at flStartSec.
bool DemoPlay_SaveClip(const float flStartSec, const float flEndSec, char* pszOutName, const size_t nOutLen)
{
	if (!pszOutName || !nOutLen)
		return false;
	pszOutName[0] = '\0';
	CDemoReader* const pReader = s_demo.pReader;
	if (s_demo.state != DemoPlayState_t::PLAYING || !pReader
		|| !(flStartSec >= 0.0f) || !(flEndSec > flStartSec) || flEndSec - flStartSec > 600.0f)
		return false;

	const R5DemHeader_s& srcHeader = pReader->GetHeader();
	const float flTick = srcHeader.tickIntervalUs ? srcHeader.tickIntervalUs / 1000000.0f : 0.05f;
	const uint32_t nFirst = pReader->GetFirstFullTick(0);
	if (nFirst == UINT32_MAX)
		return false;
	const uint32_t nEndTick = nFirst + static_cast<uint32_t>(flEndSec / flTick) + 1;

	// <stem>_c<mmss>[_<n>] beside the source.
	const char* const pszFile = strrchr(s_demo.szPath, '\\');
	const char* const pszStemStart = pszFile ? pszFile + 1 : s_demo.szPath;
	char szStem[72];
	strncpy_s(szStem, pszStemStart, _TRUNCATE);
	if (char* const pDot = strrchr(szStem, '.'))
		*pDot = '\0';
	{
		// A clip of a clip keeps the original stem.
		char* pPrev = nullptr;
		for (char* p = strstr(szStem, "_c"); p; p = strstr(p + 1, "_c"))
			pPrev = p;
		bool bDigits = pPrev && pPrev[2] != '\0';
		for (const char* p = pPrev ? pPrev + 2 : ""; *p && bDigits; ++p)
			bDigits = (*p >= '0' && *p <= '9') || *p == '_';
		if (bDigits)
			*pPrev = '\0';
	}
	szStem[52] = '\0';
	const int nStartS = static_cast<int>(flStartSec);

	char szDir[260];
	strncpy_s(szDir, s_demo.szPath, _TRUNCATE);
	if (char* const pSlash = strrchr(szDir, '\\'))
		*pSlash = '\0';
	else
		return false;

	char szName[72] = {};
	char szPath[300] = {};
	bool bFree = false;
	for (int i = 0; i < 100 && !bFree; ++i)
	{
		if (i == 0)
			_snprintf_s(szName, _TRUNCATE, "%s_c%02d%02d", szStem, (nStartS / 60) % 100, nStartS % 60);
		else
			_snprintf_s(szName, _TRUNCATE, "%s_c%02d%02d_%d", szStem, (nStartS / 60) % 100, nStartS % 60, i);
		if (!R5Dem_IsValidName(szName))
			return false;
		_snprintf_s(szPath, _TRUNCATE, "%s\\%s.r5dem", szDir, szName);
		bFree = GetFileAttributesA(szPath) == INVALID_FILE_ATTRIBUTES;
	}
	if (!bFree)
		return false;

	const HANDLE hSrc = CreateFileA(s_demo.szPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hSrc == INVALID_HANDLE_VALUE)
		return false;
	const HANDLE hDst = CreateFileA(szPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hDst == INVALID_HANDLE_VALUE)
	{
		CloseHandle(hSrc);
		return false;
	}

	R5DemHeader_s header = srcHeader;
	header.flags &= ~R5DEM_FLAG_INDEX;
	header.indexOffset = 0;
	DWORD nWritten = 0;
	bool bOk = WriteFile(hDst, &header, sizeof(header), &nWritten, nullptr) && nWritten == sizeof(header);

	std::vector<uint8_t> scratch;
	uint32_t nLastSeq = 0;
	for (const DemoChunkRef_s& ref : pReader->GetChunks())
	{
		if (!bOk)
			break;
		if (ref.hdr.type == static_cast<uint8_t>(R5DemChunk_t::PACKET) && ref.hdr.tick > nEndTick)
			break;
		if (ref.hdr.tick > nEndTick)
			continue;
		bOk = DemoPlay_CopyRange(hSrc, hDst, ref.offset, sizeof(R5DemChunkHeader_s) + ref.hdr.len, scratch);
		nLastSeq = ref.hdr.seq;
	}

	if (bOk)
	{
		std::string meta = pReader->GetMetaJson();
		// The clip fields are always the last members; drop a previous clip's.
		const size_t nOldClip = meta.rfind(",\"clipStart\":");
		if (nOldClip != std::string::npos)
			meta.erase(nOldClip, meta.find_last_of('}') - nOldClip);
		const size_t nClose = meta.find_last_of('}');
		char szClip[128];
		const int n = snprintf(szClip, sizeof(szClip), "\"clipStart\":%.2f,\"clipEnd\":%.2f", flStartSec, flEndSec);
		if (nClose == std::string::npos || n <= 0)
			meta = std::string("{") + szClip + "}";
		else
		{
			const bool bEmpty = meta.find_first_not_of(" \t\r\n{", 0) == nClose;
			meta.insert(nClose, (bEmpty ? std::string() : std::string(",")) + szClip);
		}

		R5DemChunkHeader_s hdr = {};
		hdr.type = static_cast<uint8_t>(R5DemChunk_t::META);
		hdr.pov = R5DEM_POV_ALL;
		hdr.len = static_cast<uint32_t>(meta.size());
		hdr.seq = nLastSeq + 1;
		hdr.crc32 = R5Dem_Crc32(meta.data(), meta.size());
		bOk = meta.size() <= R5DEM_MAX_JSON
			&& WriteFile(hDst, &hdr, sizeof(hdr), &nWritten, nullptr) && nWritten == sizeof(hdr)
			&& WriteFile(hDst, meta.data(), hdr.len, &nWritten, nullptr) && nWritten == hdr.len;
	}

	CloseHandle(hSrc);
	FlushFileBuffers(hDst);
	CloseHandle(hDst);
	if (!bOk)
	{
		DeleteFileA(szPath);
		Warning(eDLL_T::ENGINE, "[DEMO] clip %s could not be written\n", szName);
		return false;
	}

	strncpy_s(pszOutName, nOutLen, szName, _TRUNCATE);
	Msg(eDLL_T::ENGINE, "[DEMO] saved clip %s (%.1f-%.1f s)\n", szName, flStartSec, flEndSec);
	return true;
}

static bool DemoPlay_OpenPath(const char* pszPath, const int nPov, const float flSeekSec)
{
	CDemoReader* const pReader = new CDemoReader();
	char szErr[160] = {};
	if (!pReader->Open(pszPath, szErr, sizeof(szErr)))
	{
		Warning(eDLL_T::ENGINE, "[DEMO] %s: %s\n", pszPath, szErr);
		delete pReader;
		return false;
	}

	char szMap[64] = {};
	char szMode[64] = {};
	const std::string& meta = pReader->GetMetaJson();
	DemoJson::GetString(meta.c_str(), meta.size(), "map", szMap, sizeof(szMap));
	DemoJson::GetString(meta.c_str(), meta.size(), "mode", szMode, sizeof(szMode));
	if (!szMap[0])
		strncpy_s(szMap, pReader->GetHeader().map, _TRUNCATE);
	if (!szMode[0])
		strncpy_s(szMode, pReader->GetHeader().mode, _TRUNCATE);
	for (char* p = szMode; *p; ++p)
	{
		const char c = *p;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
			*p = '_';
	}

	if (!Bridge_IsBareMapName(szMap))
	{
		Warning(eDLL_T::ENGINE, "[DEMO] %s: bad map name in the demo\n", pszPath);
		delete pReader;
		return false;
	}
	if (!DemoPlay_MapInstalled(szMap))
	{
		Warning(eDLL_T::ENGINE, "[DEMO] map not installed: %s\n", szMap);
		delete pReader;
		return false;
	}
	if (nPov < 0 || nPov >= static_cast<int>(pReader->GetHeader().povCount))
	{
		Warning(eDLL_T::ENGINE, "[DEMO] pov %d out of range (demo has %u)\n", nPov, pReader->GetHeader().povCount);
		delete pReader;
		return false;
	}

	DemoPlay_CloseReader();
	s_demo.pReader = pReader;
	strncpy_s(s_demo.szPath, pszPath, _TRUNCATE);
	strncpy_s(s_demo.szMap, szMap, _TRUNCATE);
	strncpy_s(s_demo.szMode, szMode, _TRUNCATE);
	s_demo.nPov = nPov;
	DemoPlay_BuildOrder();
	DemoPlay_BuildUserCmds();
	DemoPlay_BuildViews();

	if (s_demo.order.empty())
	{
		Warning(eDLL_T::ENGINE, "[DEMO] %s: pov %d has nothing to play\n", pszPath, nPov);
		DemoPlay_CloseReader();
		return false;
	}

	s_demo.nCursor = 0;
	s_demo.nOobStage = 0;
	s_demo.bReachedConnected = false;
	s_demo.bEnded = false;
	s_demo.bClockStarted = false;
	s_demo.flDemoMs = 0.0;
	s_demo.nSeekTargetMs = -1;
	s_demo.flPendingSeekSec = flSeekSec;
	{
		// A clip opens at its own start unless the caller asked for a time.
		double clipStart = 0.0;
		if (flSeekSec < 0.0f && DemoJson::GetNumber(meta.c_str(), meta.size(), "clipStart", &clipStart)
			&& clipStart > 0.0 && clipStart < 86400.0)
			s_demo.flPendingSeekSec = static_cast<float>(clipStart);
	}
	DemoPlay_LoadBookmarks();
	DemoCam_LoadKeys();
	s_demo.nStepBudget = 0;
	s_demo.bCamInit = false;
	s_demo.bViewDirty = false;
	s_demo.bOverlayStarted = false;
	s_demo.nOverlayAskMs = 0;

	Msg(eDLL_T::ENGINE, "[DEMO] loaded '%s': map=%s povs=%u pov=%d duration=%.1f s%s\n",
		pszPath, szMap, pReader->GetHeader().povCount, nPov, s_demo.nDurationMs / 1000.0f,
		pReader->IsTruncated() ? " (unfinished tail)" : "");
	return true;
}

static void DemoPlay_BeginConnect(void)
{
	if (!Cbuf_AddText)
	{
		Warning(eDLL_T::ENGINE, "[DEMO] Cbuf_AddText unresolved -- cannot start playback\n");
		DemoPlay_CloseReader();
		DemoPlay_SetState(DemoPlayState_t::IDLE);
		return;
	}

	ConVar* const pPredict = g_pCVar ? g_pCVar->FindVar("cl_predict") : nullptr;
	if (pPredict && s_demo.nSavedPredict < 0)
	{
		s_demo.nSavedPredict = pPredict->GetInt();
		pPredict->SetValue(0);
	}

	// A replay keeps full speed while another window has focus.
	ConVar* const pFocusSleep = g_pCVar ? g_pCVar->FindVar("not_focus_sleep") : nullptr;
	if (pFocusSleep && s_demo.nSavedFocusSleep < 0)
	{
		s_demo.nSavedFocusSleep = pFocusSleep->GetInt();
		pFocusSleep->SetValue(0);
	}

	s_bDemoGate = true;
	DemoPlay_SetState(DemoPlayState_t::CONNECTING);

	char szLine[96];
	snprintf(szLine, sizeof(szLine), "connect \"127.0.0.1:%u\"\n", static_cast<unsigned>(S21Bridge_GamePort()));
	Cbuf_AddText(Cbuf_GetCurrentPlayer(), szLine, cmd_source_t::kCommandSrcCode);
}

static void DemoPlay_Finalize(void)
{
	s_bDemoGate = false;
	DemoPlay_SetThirdPerson(false);
	DemoCam_ReleaseDof();

	ConVar* const pPredict = g_pCVar ? g_pCVar->FindVar("cl_predict") : nullptr;
	if (pPredict && s_demo.nSavedPredict >= 0)
		pPredict->SetValue(s_demo.nSavedPredict);
	s_demo.nSavedPredict = -1;

	ConVar* const pDrawHud = g_pCVar ? g_pCVar->FindVar("cl_drawhud") : nullptr;
	if (pDrawHud && s_demo.nSavedDrawHud >= 0)
		pDrawHud->SetValue(s_demo.nSavedDrawHud);
	s_demo.nSavedDrawHud = -1;

	ConVar* const pHighlight = g_pCVar ? g_pCVar->FindVar("highlight_draw") : nullptr;
	if (pHighlight && s_demo.nSavedHighlight >= 0)
		pHighlight->SetValue(s_demo.nSavedHighlight);
	s_demo.nSavedHighlight = -1;

	ConVar* const pFocusSleep = g_pCVar ? g_pCVar->FindVar("not_focus_sleep") : nullptr;
	if (pFocusSleep && s_demo.nSavedFocusSleep >= 0)
		pFocusSleep->SetValue(s_demo.nSavedFocusSleep);
	s_demo.nSavedFocusSleep = -1;

	ConVar* const pVolume = g_pCVar ? g_pCVar->FindVar("sound_volume") : nullptr;
	if (pVolume && s_demo.flSavedVolume >= 0.0f)
		pVolume->SetValue(s_demo.flSavedVolume);
	s_demo.flSavedVolume = -1.0f;

	s_demo.bMouseLayer = false;
	DemoPlay_CursorPump();

	// A restart (hard rewind, pov switch) keeps the camera the viewer chose: the
	// free camera where it stood, or the own-pov third person camera.
	if (s_demo.bRestart)
	{
		if (!s_demo.bFreecam && s_demo.nViewPov >= 0)
			s_demo.nCamMode = DEMO_CAM_FIRST;
		s_demo.nViewPov = -1;
		// A path that was playing keeps playing: the reload is the rewind to
		// its first key, and the camera follows the replay once that lands.
	}
	else
		DemoPlay_ResetCamera();
	s_demo.bookmarks.clear();
	s_demo.bPaused = false;
	DemoPlay_CloseReader();
	DemoPlay_SetState(DemoPlayState_t::IDLE);
	Msg(eDLL_T::ENGINE, "[DEMO] playback ended\n");

	if (s_demo.bRestart)
	{
		s_demo.bRestart = false;
		char szPath[260];
		strncpy_s(szPath, s_demo.szPath, _TRUNCATE);
		if (DemoPlay_OpenPath(szPath, s_demo.nRestartPov, s_demo.flRestartSeek))
			DemoPlay_SetState(DemoPlayState_t::WAIT_DISCONNECT);
		else
			DemoPlay_ResetCamera();
	}
}

bool DemoPlay_Stop(const char* pszReason)
{
	if (s_demo.state == DemoPlayState_t::IDLE)
		return false;

	Msg(eDLL_T::ENGINE, "[DEMO] stopping playback (%s)\n", pszReason ? pszReason : "?");
	if (s_demo.state == DemoPlayState_t::WAIT_DISCONNECT)
	{
		s_demo.bRestart = false;
		DemoPlay_ResetCamera();
		DemoPlay_CloseReader();
		DemoPlay_SetState(DemoPlayState_t::IDLE);
		return true;
	}

	// Teardown runs with the gate up so nothing reaches a socket.
	DemoPlay_SetState(DemoPlayState_t::STOPPING);
	if (Cbuf_AddText)
		Cbuf_AddText(Cbuf_GetCurrentPlayer(), "disconnect\n", cmd_source_t::kCommandSrcCode);
	return true;
}

static bool DemoPlay_StartPath(const char* pszPath, const int nPov, const float flSeekSec)
{
	if (DemoRecord_IsRecording())
		DemoRecord_Stop("starting playback");
	if (s_demo.state != DemoPlayState_t::IDLE)
	{
		Warning(eDLL_T::ENGINE, "[DEMO] a demo is already playing -- demo_stop first\n");
		return false;
	}
	DemoPlay_ResetCamera();
	if (!DemoPlay_OpenPath(pszPath, nPov, flSeekSec))
		return false;

	DemoPlay_SetState(DemoPlayState_t::WAIT_DISCONNECT);
	if (S21Bridge_GetClientSignonState() > 0 && Cbuf_AddText)
		Cbuf_AddText(Cbuf_GetCurrentPlayer(), "disconnect\n", cmd_source_t::kCommandSrcCode);
	return true;
}

bool DemoPlay_Start(const char* pszName, const int nPov, const float flSeekSec)
{
	if (!R5Dem_IsValidName(pszName))
	{
		Warning(eDLL_T::ENGINE, "[DEMO] demo names are [a-zA-Z0-9_-], 1..64 characters\n");
		return false;
	}
	char szPath[260];
	if (!Demo_FindFile(DEMO_CLIENT_ROOT, pszName, szPath, sizeof(szPath)))
	{
		Warning(eDLL_T::ENGINE, "[DEMO] no demo named '%s' under %s\n", pszName, DEMO_CLIENT_ROOT);
		return false;
	}
	return DemoPlay_StartPath(szPath, nPov, flSeekSec);
}

// Reconnect to the same file (pov switch, hard rewind).
static void DemoPlay_Restart(const int nPov, const float flSeekSec)
{
	if (s_demo.bFreecam && s_demo.bCamInit)
		s_demo.bCamCarry = true;
	s_demo.bRestart = true;
	s_demo.nRestartPov = nPov;
	s_demo.flRestartSeek = flSeekSec;
	DemoPlay_SetState(DemoPlayState_t::STOPPING);
	if (Cbuf_AddText)
		Cbuf_AddText(Cbuf_GetCurrentPlayer(), "disconnect\n", cmd_source_t::kCommandSrcCode);
}

void DemoPlay_OnSessionEnded(void)
{
	if (!s_bDemoGate)
		return;
	// The engine drops to signon 0 while it sets up the virtual connect; only a
	// session that got connected (or one we are tearing down) ends playback.
	if (s_demo.state == DemoPlayState_t::STOPPING || s_demo.bReachedConnected)
		DemoPlay_Finalize();
}

bool DemoPlay_OnStartHandshake(void)
{
	if (!s_bDemoGate || s_demo.state != DemoPlayState_t::CONNECTING || !s_demo.pReader)
		return false;
	s_demo.nOobStage = 0;
	return true;
}

static constexpr size_t kDemoMaxBlockKeys = 4096;

void DemoPlay_OnFullSnapshotBlock(void)
{
	if (!s_bDemoGate || !s_demo.pReader || s_demo.nLastBlockOrder >= s_demo.order.size()
		|| s_demo.blockKeys.size() >= kDemoMaxBlockKeys)
		return;
	const int nKey = static_cast<int>(s_demo.nLastBlockOrder);
	const auto it = std::lower_bound(s_demo.blockKeys.begin(), s_demo.blockKeys.end(), nKey);
	if (it != s_demo.blockKeys.end() && *it == nKey)
		return;
	s_demo.blockKeys.insert(it, nKey);

	const DemoChunkRef_s& ref = s_demo.pReader->GetChunks()[s_demo.order[nKey]];
	const int64_t nRel = DemoPlay_RelMs(ref.hdr);
	Msg(eDLL_T::ENGINE, "[DEMO] full snapshot at %.2f s (chunk %d) -- rewinds start here\n",
		(nRel > 0 ? nRel : 0) / 1000.0, nKey);
}

bool DemoPlay_InSoftRewind(void)
{
	return s_bDemoGate && s_demo.bSoftRewind;
}

void DemoPlay_OnSnapshotDropped(void)
{
	InterlockedExchange(&s_nSnapDropped, 1);
}

void DemoPlay_SetMouseLayer(const bool bOn)
{
	s_demo.bMouseLayer = bOn;
}

bool DemoPlay_WantsCursor(void)
{
	return s_bDemoGate && s_demo.state == DemoPlayState_t::PLAYING && s_demo.bMouseLayer;
}

// While the controls are up the game must not hold the mouse: release the
// input system's capture, keep the pointer inside the window and visible.
static void DemoPlay_CursorPump(void)
{
	const bool bWant = DemoPlay_WantsCursor();
	const HWND hGame = g_pGame ? g_pGame->GetWindow() : nullptr;
	if (bWant && hGame && GetForegroundWindow() == hGame)
	{
		if (!s_demo.bCursorOwned)
		{
			s_demo.bCursorOwned = true;
			if (g_pInputSystem)
				g_pInputSystem->DisableMouseCapture();
			Input_ConfineCursorToWindow(hGame);
		}
		CURSORINFO ci = {};
		ci.cbSize = sizeof(ci);
		if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor)
		{
			Input_EnsureCursorVisible();
			SetCursor(LoadCursor(nullptr, IDC_ARROW));
		}
		return;
	}
	if (!bWant && s_demo.bCursorOwned)
	{
		s_demo.bCursorOwned = false;
		if (g_pInputSystem)
		{
			const PlatWindow_t hAttached = g_pInputSystem->GetAttachedWindow();
			if (hAttached)
				g_pInputSystem->EnableMouseCapture(hAttached);
		}
	}
}

//-----------------------------------------------------------------------------
// Datagram source
//-----------------------------------------------------------------------------
static int DemoPlay_BuildChallenge(char* pBuf, const int nCap)
{
	const size_t nMap = strlen(s_demo.szMap);
	const int nNeed = 4 + 1 + 4 + 1 + static_cast<int>(nMap) + 1;
	if (nNeed > nCap)
		return 0;
	uint8_t* p = reinterpret_cast<uint8_t*>(pBuf);
	p[0] = p[1] = p[2] = p[3] = 0xFF;
	p[4] = 0x49;
	memcpy(p + 5, &kDemoChallenge, 4);
	p[9] = 0;
	memcpy(p + 10, s_demo.szMap, nMap);
	p[10 + nMap] = 0;
	return nNeed;
}

static int DemoPlay_BuildConnAccept(char* pBuf, const int nCap)
{
	const size_t nMap = strlen(s_demo.szMap);
	const size_t nMode = strlen(s_demo.szMode);
	const int nNeed = 5 + static_cast<int>(nMap) + 1 + static_cast<int>(nMode) + 1;
	if (nNeed > nCap)
		return 0;
	uint8_t* p = reinterpret_cast<uint8_t*>(pBuf);
	p[0] = p[1] = p[2] = p[3] = 0xFF;
	p[4] = 0x4A;
	memcpy(p + 5, s_demo.szMap, nMap);
	p[5 + nMap] = 0;
	memcpy(p + 6 + nMap, s_demo.szMode, nMode);
	p[6 + nMap + nMode] = 0;
	return nNeed;
}

static void DemoPlay_BadChunk(const DemoChunkRef_s& ref)
{
	Warning(eDLL_T::ENGINE, "[DEMO] %s: bad chunk at offset %llu (type %u len %u) -- stopping\n",
		s_demo.szPath, static_cast<unsigned long long>(ref.offset), ref.hdr.type, ref.hdr.len);
	DemoPlay_Stop("bad chunk");
}

static void DemoPlay_OnEnd(void)
{
	if (s_demo.bEnded)
		return;
	s_demo.bEnded = true;
	s_demo.nSeekTargetMs = -1;
	s_demo.bSoftRewind = false;
	Msg(eDLL_T::ENGINE, "[DEMO] end of demo -- demo_seek to review, demo_stop to leave\n");

	if (s_fuzz.bActive)
		s_fuzz.nSliceStartMs = 0;
}

static bool DemoPlay_FfBudgetSpent(void)
{
	if (!s_demo.ffFrameStartQpc.QuadPart)
		return false;
	LARGE_INTEGER now, freq;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&freq);
	const double ms = static_cast<double>(now.QuadPart - s_demo.ffFrameStartQpc.QuadPart) * 1000.0
		/ static_cast<double>(freq.QuadPart ? freq.QuadPart : 1);
	return ms >= demo_ff_budget_ms.GetFloat();
}

bool DemoPlay_NextDatagram(char* pBuf, const int nCap, int* pLen)
{
	*pLen = 0;
	if (!s_bDemoGate || !s_demo.pReader)
		return false;
	if (s_demo.state != DemoPlayState_t::CONNECTING && s_demo.state != DemoPlayState_t::PLAYING)
		return false;

	if (s_demo.nOobStage == 0)
	{
		*pLen = DemoPlay_BuildChallenge(pBuf, nCap);
		s_demo.nOobStage = 1;
		return *pLen > 0;
	}
	if (s_demo.nOobStage == 1)
	{
		*pLen = DemoPlay_BuildConnAccept(pBuf, nCap);
		s_demo.nOobStage = 2;
		return *pLen > 0;
	}

	if (!S21Bridge_HasNativeChan() || S21Bridge_GetClientSignonState() < 2
		|| S21Bridge_NativeConnectPending() || S21Bridge_SignonHandoffBusy()
		|| (S21Bridge_GetClientSignonState() >= kSignonFull && S21Bridge_S2CHoldQueueBusy()))
		return false;

	if (!s_demo.bReachedConnected)
	{
		s_demo.bReachedConnected = true;
		DemoPlay_SetState(DemoPlayState_t::PLAYING);
	}

	const std::vector<DemoChunkRef_s>& chunks = s_demo.pReader->GetChunks();
	for (int nSafety = 0; nSafety < 64; ++nSafety)
	{
		if (s_demo.nCursor >= s_demo.order.size())
		{
			DemoPlay_OnEnd();
			return false;
		}

		const DemoChunkRef_s& ref = chunks[s_demo.order[s_demo.nCursor]];
		const R5DemChunk_t type = static_cast<R5DemChunk_t>(ref.hdr.type);

		if (type == R5DemChunk_t::SIGNON || type == R5DemChunk_t::RELIABLE)
		{
			if (!s_demo.pReader->ReadPayload(ref, s_chunkScratch) || s_chunkScratch.empty())
			{
				DemoPlay_BadChunk(ref);
				return false;
			}
			s_demo.nLastBlockOrder = s_demo.nCursor;
			++s_demo.nCursor;
			if (demo_diag.GetBool())
				Msg(eDLL_T::ENGINE, "[DEMO] feed %s %u bytes signon=%u\n",
					type == R5DemChunk_t::SIGNON ? "SIGNON" : "RELIABLE", ref.hdr.len, ref.hdr.signon);
			S21Bridge_DeliverDataBlock(s_chunkScratch.data(), static_cast<int>(s_chunkScratch.size()));
			if (S21Bridge_SignonHandoffBusy())
				return false;
			continue;
		}

		// PACKET
		const bool bClock = DemoPlay_IsClockPacket(ref.hdr);
		const bool bSeeking = s_demo.nSeekTargetMs >= 0;
		if (bClock)
		{
			if (!s_demo.bClockStarted)
			{
				s_demo.bClockStarted = true;
				s_demo.nClockProbeUntilMs = GetTickCount64() + 15000;
				s_demo.flDemoMs = 0.0;
				QueryPerformanceCounter(&s_demo.lastFrameQpc);
				if (s_demo.flPendingSeekSec > 0.0f)
					s_demo.nSeekTargetMs = static_cast<int64_t>(s_demo.flPendingSeekSec * 1000.0f);
				s_demo.flPendingSeekSec = -1.0f;
			}

			const int64_t nRel = DemoPlay_RelMs(ref.hdr);
			if (s_demo.nSeekTargetMs >= 0)
			{
				if (nRel > s_demo.nSeekTargetMs)
				{
					s_demo.flDemoMs = static_cast<double>(s_demo.nSeekTargetMs);
					s_demo.nSeekTargetMs = -1;
					s_demo.bSoftRewind = false;
					s_demo.nClockProbeUntilMs = GetTickCount64() + 15000;
					Msg(eDLL_T::ENGINE, "[DEMO] seek done at %.2f s\n", s_demo.flDemoMs / 1000.0);
					return false;
				}
				if (s_demo.nFrameBudget >= demo_ff_packets_per_frame.GetInt() || DemoPlay_FfBudgetSpent())
					return false;
			}
			else if (s_demo.nStepBudget > 0)
			{
				--s_demo.nStepBudget;
				s_demo.flDemoMs = static_cast<double>(nRel);
			}
			else
			{
				if (s_demo.bPaused || s_demo.bEnded || static_cast<double>(nRel) > s_demo.flDemoMs)
					return false;
				if (s_demo.nFrameBudget >= demo_feed_max_per_frame.GetInt())
					return false;
			}
		}
		else if (!bSeeking && s_demo.nFrameBudget >= demo_ff_packets_per_frame.GetInt())
		{
			return false;
		}

		if (!s_demo.pReader->ReadPayload(ref, s_chunkScratch) || s_chunkScratch.size() < 9
			|| static_cast<int>(s_chunkScratch.size()) > nCap)
		{
			DemoPlay_BadChunk(ref);
			return false;
		}

		// A PACKET is a netchan datagram; connectionless / split headers only
		// ever come from the player itself.
		uint32_t nHead = 0;
		memcpy(&nHead, s_chunkScratch.data(), 4);
		if (nHead == 0xFFFFFFFFu || nHead == 0xFFFFFFFEu)
		{
			DemoPlay_BadChunk(ref);
			return false;
		}

		memcpy(pBuf, s_chunkScratch.data(), s_chunkScratch.size());
		*pLen = static_cast<int>(s_chunkScratch.size());
		if (bClock)
		{
			s_demo.nLastFedTick = ref.hdr.tick;
			s_demo.nLastFedRelMs = DemoPlay_RelMs(ref.hdr);
		}
		++s_demo.nCursor;
		++s_demo.nFrameBudget;
		if (demo_diag.GetBool())
			Msg(eDLL_T::ENGINE, "[DEMO] feed PACKET %u bytes tick=%u kind=0x%02X rel=%lld\n",
				ref.hdr.len, ref.hdr.tick, ref.hdr.kind, static_cast<long long>(DemoPlay_RelMs(ref.hdr)));
		return true;
	}
	return false;
}

//-----------------------------------------------------------------------------
// Frame pump, timescale, view
//-----------------------------------------------------------------------------
bool DemoPlay_TimescaleOverride(float* pOut)
{
	if (!s_bDemoGate || !s_demo.bClockStarted || s_demo.state != DemoPlayState_t::PLAYING)
		return false;
	if (s_demo.nSeekTargetMs >= 0)
		*pOut = 1.0f;
	else if (s_demo.bPaused || s_demo.bEnded)
		*pOut = demo_pause_timescale.GetFloat();
	else
		*pOut = demo_timescale.GetFloat();
	return true;
}

static void DemoPlay_WriteObserver(const int nMode, const uint32_t nTarget)
{
	const uintptr_t pPlayer = DemoPlay_LocalPlayer();
	if (!pPlayer)
		return;
	__try
	{
		if (!s_demo.bViewDirty)
		{
			s_demo.nSavedObsMode = *reinterpret_cast<const int*>(pPlayer + kPlayerObserverMode);
			s_demo.nSavedObsTarget = *reinterpret_cast<const uint32_t*>(pPlayer + kPlayerObserverTarget);
		}
		*reinterpret_cast<int*>(pPlayer + kPlayerObserverMode) = nMode;
		*reinterpret_cast<uint32_t*>(pPlayer + kPlayerObserverTarget) = nTarget;
		s_demo.bViewDirty = true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void DemoPlay_RestoreObserver(void)
{
	const uintptr_t pPlayer = DemoPlay_LocalPlayer();
	s_demo.bViewDirty = false;
	if (!pPlayer)
		return;
	__try
	{
		*reinterpret_cast<int*>(pPlayer + kPlayerObserverMode) = s_demo.nSavedObsMode;
		*reinterpret_cast<uint32_t*>(pPlayer + kPlayerObserverTarget) = s_demo.nSavedObsTarget;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

static uint32_t DemoPlay_PovHandle(const int nPov)
{
	if (!s_demo.pReader)
		return 0xFFFFFFFFu;
	for (const DemoPovInfo_s& p : s_demo.pReader->GetPovs())
	{
		if (p.id == nPov)
			return p.eh;
	}
	return 0xFFFFFFFFu;
}

// Recorded view angles at the current demo time, yaw interpolated the short way.
static bool DemoPlay_RecordedView(float* pAngles)
{
	if (s_demo.views.empty() || !s_demo.bClockStarted)
		return false;
	const double t = static_cast<double>(s_demo.nClockBaseWallMs) + s_demo.flDemoMs;
	const std::vector<DemoViewAt_s>& v = s_demo.views;
	auto it = std::upper_bound(v.begin(), v.end(), t,
		[](const double x, const DemoViewAt_s& e) { return x < static_cast<double>(e.wallMs); });
	if (it == v.begin())
	{
		pAngles[0] = it->view.pitch; pAngles[1] = it->view.yaw; pAngles[2] = it->view.roll;
		return true;
	}
	const DemoViewAt_s& a = *(it - 1);
	if (it == v.end())
	{
		pAngles[0] = a.view.pitch; pAngles[1] = a.view.yaw; pAngles[2] = a.view.roll;
		return true;
	}
	const DemoViewAt_s& b = *it;
	const double span = static_cast<double>(b.wallMs) - static_cast<double>(a.wallMs);
	float f = span > 0.0 ? static_cast<float>((t - a.wallMs) / span) : 0.0f;
	if (f < 0.0f) f = 0.0f;
	if (f > 1.0f) f = 1.0f;
	// Angles come from the demo file unbounded; a subtract loop never ends on huge or infinite deltas.
	float dYaw = remainderf(b.view.yaw - a.view.yaw, 360.0f);
	if (!std::isfinite(dYaw))
		dYaw = 0.0f;
	pAngles[0] = a.view.pitch + (b.view.pitch - a.view.pitch) * f;
	pAngles[1] = a.view.yaw + dYaw * f;
	pAngles[2] = a.view.roll + (b.view.roll - a.view.roll) * f;
	return true;
}

static bool DemoPlay_UsesRoaming(void)
{
	return s_demo.bFreecam;
}

// Own-pov third person is the game's own shoulder camera: thirdperson_override
// makes the local view player think in third person, so the framing, collision
// and follow are exactly a live third-person match. It is a cheat var the engine
// may reset on connect, so it is asserted every time the view is applied.
static void DemoPlay_SetThirdPerson(const bool bOn)
{
	ConVar* const pThird = g_pCVar ? g_pCVar->FindVar("thirdperson_override") : nullptr;
	if (!pThird)
		return;
	if (bOn)
	{
		if (!s_demo.bThirdPerson)
		{
			s_demo.nSavedThirdPerson = pThird->GetInt();
			s_demo.bThirdPerson = true;
		}
		if (pThird->GetInt() != 1)
			pThird->SetValue(1);
		return;
	}
	if (s_demo.bThirdPerson)
	{
		pThird->SetValue(s_demo.nSavedThirdPerson);
		s_demo.bThirdPerson = false;
	}
}

// True while the camera should follow the recorded player's own aim.
static bool DemoPlay_ViewLocked(void)
{
	return s_bDemoGate && s_demo.state == DemoPlayState_t::PLAYING
		&& !s_demo.bFreecam && s_demo.nViewPov < 0 && !s_demo.views.empty();
}

static float* DemoPlay_ViewAnglesSlot(void)
{
	if (!g_pEngineViewAngles || !g_pSplitScreenMgr)
		return nullptr;
	int nSlot = 0;
	__try
	{
		void** const vtbl = *reinterpret_cast<void***>(g_pSplitScreenMgr);
		nSlot = reinterpret_cast<int(__fastcall*)(void*)>(vtbl[5])(g_pSplitScreenMgr);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { nSlot = 0; }
	if (nSlot < 0 || nSlot > 3)
		nSlot = 0;
	return reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(g_pEngineViewAngles) + nSlot * kViewAnglesStride);
}

// Overwrites the engine's view angles with the recorded ones so the mouse
// cannot turn the camera. Called before and after the engine frame.
void DemoPlay_LockView(void)
{
	if (!DemoPlay_ViewLocked())
		return;
	float ang[3];
	if (!DemoPlay_RecordedView(ang))
		return;
	float* const p = DemoPlay_ViewAnglesSlot();
	if (!p)
		return;
	p[0] = ang[0];
	p[1] = ang[1];
	p[2] = ang[2];
}

void DemoPlay_OverlayStarted(void)
{
	if (!s_demo.bOverlayStarted)
		Msg(eDLL_T::ENGINE, "[DEMO] replay overlay is up\n");
	s_demo.bOverlayStarted = true;
}

bool DemoPlay_GetEngineViewAngles(float* pAngles)
{
	if (!v_Engine_GetViewAngles || !pAngles)
		return false;
	v_Engine_GetViewAngles(nullptr, pAngles);
	return pAngles[0] == pAngles[0] && pAngles[1] == pAngles[1];
}

bool DemoPlay_SetEngineViewAngles(const float* pAngles)
{
	float* const p = pAngles ? DemoPlay_ViewAnglesSlot() : nullptr;
	if (!p)
		return false;
	p[0] = pAngles[0];
	p[1] = pAngles[1];
	p[2] = pAngles[2];
	return true;
}

static void __fastcall Hook_Engine_GetViewAngles(void* pUnused, float* pAngles)
{
	v_Engine_GetViewAngles(pUnused, pAngles);
	if (pAngles && DemoPlay_ViewLocked())
		DemoPlay_RecordedView(pAngles);
}

// Script waits are scheduled against the time fed here. A rewind moves the
// game clock back, which would leave every waiting client thread asleep until
// the clock caught up again, so the client VM gets a clock that never goes back.
static CSquirrelVM* s_pScriptClockVM = nullptr;
static float s_flScriptClockOffset = 0.0f;
static float s_flScriptClockLast = 0.0f;

static void __fastcall Hook_Script_RunThreadsFrame(CSquirrelVM* s, float flCurTime)
{
	if (s && s == g_pClientScript)
	{
		if (s != s_pScriptClockVM || !s_bDemoGate)
		{
			s_pScriptClockVM = s_bDemoGate ? s : nullptr;
			s_flScriptClockOffset = 0.0f;
			s_flScriptClockLast = flCurTime;
		}
		else
		{
			float flFed = flCurTime + s_flScriptClockOffset;
			if (flFed < s_flScriptClockLast)
			{
				s_flScriptClockOffset += s_flScriptClockLast - flFed;
				flFed = s_flScriptClockLast;
			}
			s_flScriptClockLast = flFed;
			flCurTime = flFed;
		}
	}
	v_Script_RunThreadsFrame(s, flCurTime);
}

static void DemoPlay_ApplyView(void)
{
	if (s_demo.state != DemoPlayState_t::PLAYING)
		return;

	DemoPlay_SetThirdPerson(!s_demo.bFreecam && s_demo.nViewPov < 0 && s_demo.nCamMode == DEMO_CAM_THIRD);

	// The free camera runs on the roaming view, which the roaming hook places.
	if (DemoPlay_UsesRoaming())
	{
		DemoPlay_WriteObserver(kObsModeRoaming, 0xFFFFFFFFu);
		return;
	}
	if (s_demo.nViewPov >= 0)
	{
		const uint32_t eh = (s_demo.nViewPov < DemoPlay_GetPovCount())
			? DemoPlay_PovHandle(s_demo.nViewPov) : static_cast<uint32_t>(s_demo.nViewPov);
		DemoPlay_WriteObserver(s_demo.bViewChase ? kObsModeChase : kObsModeInEye, eh);
		return;
	}
	if (s_demo.bViewDirty)
		DemoPlay_RestoreObserver();
}

void DemoPlay_OnPacketProcessed(void)
{
	// A full snapshot rewrites every prop, the observer override included.
	DemoPlay_ApplyView();
}

static void DemoPlay_FuzzPump(void)
{
	const char* const pszDir = demo_fuzz_dir.GetString();
	if (!s_fuzz.bActive)
	{
		if (!pszDir || !pszDir[0] || s_demo.state != DemoPlayState_t::IDLE)
			return;
		if (!Demo_IsSafeRoot(pszDir))
		{
			Warning(eDLL_T::ENGINE, "[DEMO-FUZZ] unsafe directory '%s'\n", pszDir);
			demo_fuzz_dir.SetValue("");
			return;
		}
		std::vector<DemoFileInfo_s> files;
		Demo_ListFiles(pszDir, files, 100000);
		s_fuzz.files.clear();
		for (const DemoFileInfo_s& f : files)
			s_fuzz.files.emplace_back(f.path);
		std::sort(s_fuzz.files.begin(), s_fuzz.files.end());
		s_fuzz.nNext = 0;
		s_fuzz.nPlayed = 0;
		s_fuzz.bActive = true;
		Msg(eDLL_T::ENGINE, "[DEMO-FUZZ] %zu files under '%s'\n", s_fuzz.files.size(), pszDir);
	}

	if (s_demo.state == DemoPlayState_t::PLAYING
		&& (s_fuzz.nSliceStartMs == 0 || GetTickCount64() - s_fuzz.nSliceStartMs >= kFuzzSliceMs))
	{
		DemoPlay_Stop("fuzz slice");
		return;
	}
	if (s_demo.state != DemoPlayState_t::IDLE)
		return;

	while (s_fuzz.nNext < s_fuzz.files.size())
	{
		const std::string path = s_fuzz.files[s_fuzz.nNext++];
		++s_fuzz.nPlayed;
		Msg(eDLL_T::ENGINE, "[DEMO-FUZZ] %d/%zu %s\n", s_fuzz.nPlayed, s_fuzz.files.size(), path.c_str());
		if (DemoPlay_StartPath(path.c_str(), 0, -1.0f))
		{
			s_fuzz.nSliceStartMs = GetTickCount64();
			return;
		}
	}

	Msg(eDLL_T::ENGINE, "[DEMO-FUZZ] done: %d files, client still alive\n", s_fuzz.nPlayed);
	s_fuzz.bActive = false;
	demo_fuzz_dir.SetValue("");
}

// -replay <name> [-replay_pov <n>]: play a replay straight from launch, once
// the main menu is up and no connection is open.
static void DemoPlay_LaunchReplay(void)
{
	static bool s_bDone = false;
	static ULONGLONG s_nReadySinceMs = 0;
	if (s_bDone)
		return;

	const char* const pszName = CommandLine()->ParmValue("-replay", "");
	if (!pszName || !pszName[0])
	{
		s_bDone = true;
		return;
	}
	if (!g_pUIScript || S21Bridge_GetClientSignonState() > 0 || S21Bridge_HasNativeChan()
		|| s_demo.state != DemoPlayState_t::IDLE)
	{
		static ULONGLONG s_nLastWhyMs = 0;
		const ULONGLONG nNow = GetTickCount64();
		if (nNow - s_nLastWhyMs >= 10000)
		{
			s_nLastWhyMs = nNow;
			Msg(eDLL_T::ENGINE, "[DEMO] -replay %s waiting: ui=%d signon=%d nativeChan=%d state=%d\n", pszName,
				g_pUIScript ? 1 : 0, S21Bridge_GetClientSignonState(), S21Bridge_HasNativeChan() ? 1 : 0,
				static_cast<int>(s_demo.state));
		}
		s_nReadySinceMs = 0;
		return;
	}
	if (!s_nReadySinceMs)
		s_nReadySinceMs = GetTickCount64();
	if (GetTickCount64() - s_nReadySinceMs < 1500)
		return;

	s_bDone = true;
	const int nPov = CommandLine()->ParmValue("-replay_pov", 0);
	const char* const pszSeek = CommandLine()->ParmValue("-replay_seek", "");
	float flSeek = (pszSeek && pszSeek[0]) ? static_cast<float>(atof(pszSeek)) : -1.0f;
	if (!(flSeek >= 0.0f && flSeek < 86400.0f))
		flSeek = -1.0f;
	Msg(eDLL_T::ENGINE, "[DEMO] -replay %s (pov %d, from %.1f s)\n", pszName, nPov, flSeek);
	if (!DemoPlay_Start(pszName, nPov >= 0 && nPov < R5DEM_MAX_POVS ? nPov : 0, flSeek))
		Warning(eDLL_T::ENGINE, "[DEMO] -replay %s: the replay could not be started\n", pszName);
}

// The engine hides the cursor and sleeps each frame while it is not the active
// app, so a replay that loses focus looks cursorless and choppy. Names the
// window that holds the foreground whenever the picture changes.
static void DemoPlay_FocusProbe(const double dtMs)
{
	static constexpr ptrdiff_t kGameActiveApp = 0x28; // written by the engine's app-activate handler
	static int s_nLines = 0;
	static uint32_t s_nLastKey = UINT32_MAX;
	static double s_flSumMs = 0.0, s_flMaxMs = 0.0;
	static int s_nFrames = 0;
	static ULONGLONG s_nWindowStartMs = 0;

	if (!demo_focus_diag.GetBool() || s_nLines >= 200)
		return;

	s_flSumMs += dtMs;
	s_flMaxMs = dtMs > s_flMaxMs ? dtMs : s_flMaxMs;
	++s_nFrames;

	const HWND hGame = g_pGame ? g_pGame->GetWindow() : nullptr;
	const HWND hFg = GetForegroundWindow();
	DWORD nFgPid = 0;
	if (hFg)
		GetWindowThreadProcessId(hFg, &nFgPid);
	int nActive = -1;
	if (g_pGame)
	{
		__try { nActive = *(reinterpret_cast<const uint8_t*>(g_pGame) + kGameActiveApp); }
		__except (EXCEPTION_EXECUTE_HANDLER) { nActive = -1; }
	}
	CURSORINFO ci = {};
	ci.cbSize = sizeof(ci);
	const bool bShowing = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);
	RECT clip = {};
	const bool bClipped = GetClipCursor(&clip)
		&& (clip.right - clip.left) < GetSystemMetrics(SM_CXVIRTUALSCREEN);

	const bool bFgGame = hFg && hFg == hGame;
	const bool bFgOurs = nFgPid == GetCurrentProcessId();
	const uint32_t nKey = (bFgGame ? 1u : 0u) | (bFgOurs ? 2u : 0u) | (static_cast<uint32_t>(nActive & 0xFF) << 2)
		| (bShowing ? 1u << 10 : 0u) | (bClipped ? 1u << 11 : 0u) | (s_demo.bPaused ? 1u << 12 : 0u);

	const ULONGLONG nNow = GetTickCount64();
	if (!s_nWindowStartMs)
		s_nWindowStartMs = nNow;

	if (nKey != s_nLastKey)
	{
		s_nLastKey = nKey;
		++s_nLines;
		char szClass[64] = {};
		char szTitle[64] = {};
		if (hFg)
		{
			GetClassNameA(hFg, szClass, sizeof(szClass));
			GetWindowTextA(hFg, szTitle, sizeof(szTitle));
		}
		Warning(eDLL_T::ENGINE, "[DEMO-FOCUS] fg=%p class='%s' title='%s' pid=%lu%s game=%d active=%d cursor=%d clip=%d paused=%d t=%.2f\n",
			static_cast<void*>(hFg), szClass, szTitle, nFgPid, bFgOurs ? " (ours)" : "", bFgGame ? 1 : 0, nActive,
			bShowing ? 1 : 0, bClipped ? 1 : 0, s_demo.bPaused ? 1 : 0, DemoPlay_GetTime());
	}

	if (nNow - s_nWindowStartMs >= 5000)
	{
		const double flAvg = s_nFrames ? s_flSumMs / s_nFrames : 0.0;
		if (flAvg > 30.0)
		{
			++s_nLines;
			Warning(eDLL_T::ENGINE, "[DEMO-FOCUS] slow frames: avg %.1f ms max %.1f ms over %d frames (game=%d active=%d)\n",
				flAvg, s_flMaxMs, s_nFrames, bFgGame ? 1 : 0, nActive);
		}
		s_nWindowStartMs = nNow;
		s_flSumMs = s_flMaxMs = 0.0;
		s_nFrames = 0;
	}
}

// Once a second for 15 s after the clock starts and after each seek, and any
// second in which the world lerp extrapolated or lost its future snapshot.
static void DemoPlay_ClockProbe(const double dtMs)
{
	static int s_nLines = 0;
	static int s_nFrames = 0, s_nExtrap = 0, s_nCollapsed = 0;
	static float s_flLerpMax = 0.0f;
	static double s_flFrameMs = 0.0;
	static ULONGLONG s_nWindowStartMs = 0;

	if (!demo_focus_diag.GetBool() || !s_demo.bClockStarted || s_nLines >= 600)
		return;

	float flLast = 0.0f, flCur = 0.0f, flFut = 0.0f, flLerp = 0.0f;
	if (!PredNative_SnapTimes(&flLast, &flCur, &flFut, &flLerp))
		return;
	++s_nFrames;
	s_flFrameMs += dtMs;
	if (flLerp > 1.0f)
		++s_nExtrap;
	if (flFut <= flCur)
		++s_nCollapsed;
	if (flLerp > s_flLerpMax)
		s_flLerpMax = flLerp;

	const ULONGLONG nNow = GetTickCount64();
	if (!s_nWindowStartMs)
		s_nWindowStartMs = nNow;
	if (nNow - s_nWindowStartMs < 1000)
		return;

	const bool bWindow = nNow < s_demo.nClockProbeUntilMs;
	const bool bBad = (s_nExtrap + s_nCollapsed) * 10 > s_nFrames;
	if ((bWindow || bBad) && !s_demo.bPaused)
	{
		++s_nLines;
		const ClockDriftView_t* const pDrift = NetObs_ClockDrift();
		int nSrv = -1, nCli = -1;
		float flAhead = -1.0f, flScale = -1.0f;
		if (pDrift)
		{
			__try
			{
				nSrv = pDrift->m_nServerTick;
				nCli = pDrift->m_nClientTick;
				flAhead = pDrift->m_aheadBy;
				flScale = pDrift->m_serverFrameTimeScaleAverage;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}
		const float flTick = S21Bridge_IntervalPerTick();
		Warning(eDLL_T::ENGINE, "[DEMO-CLOCK] t=%.2f x%.2f fed{tick=%u rel=%.2f} snap=%u curtime=%.3f pair{cur=%.3f fut=%.3f lerpMax=%.2f} "
			"drift{srv=%d cli=%d ahead=%.3f scale=%.3f} srvTime=%.3f frames=%d extrap=%d collapsed=%d avg=%.1fms%s\n",
			s_demo.flDemoMs / 1000.0, demo_timescale.GetFloat(), s_demo.nLastFedTick, s_demo.nLastFedRelMs / 1000.0, S21Bridge_LastSnapshotTick(),
			PredNative_CurTime(), flCur, flFut, s_flLerpMax, nSrv, nCli, flAhead, flScale, nSrv * flTick,
			s_nFrames, s_nExtrap, s_nCollapsed, s_nFrames ? s_flFrameMs / s_nFrames : 0.0,
			s_demo.nSeekTargetMs >= 0 ? " seeking" : "");
	}
	s_nWindowStartMs = nNow;
	s_nFrames = s_nExtrap = s_nCollapsed = 0;
	s_flLerpMax = 0.0f;
	s_flFrameMs = 0.0;
}

void DemoPlay_OnHostFrame(void)
{
	DemoPlay_FuzzPump();
	DemoPlay_LaunchReplay();

	switch (s_demo.state)
	{
	case DemoPlayState_t::IDLE:
		return;

	case DemoPlayState_t::WAIT_DISCONNECT:
		if (S21Bridge_GetClientSignonState() <= 0 && GetTickCount64() - s_demo.nStateSinceMs > 100)
			DemoPlay_BeginConnect();
		else if (GetTickCount64() - s_demo.nStateSinceMs > kConnectTimeoutMs)
		{
			Warning(eDLL_T::ENGINE, "[DEMO] the previous connection never closed -- playback cancelled\n");
			DemoPlay_ResetCamera();
			DemoPlay_CloseReader();
			DemoPlay_SetState(DemoPlayState_t::IDLE);
		}
		return;

	case DemoPlayState_t::CONNECTING:
		if (GetTickCount64() - s_demo.nStateSinceMs > kConnectTimeoutMs)
		{
			Warning(eDLL_T::ENGINE, "[DEMO] virtual connect did not reach CONNECTED -- stopping\n");
			DemoPlay_Stop("connect timeout");
		}
		return;

	case DemoPlayState_t::STOPPING:
		// Disconnect never reached signon 0 (it was not connected yet).
		if (GetTickCount64() - s_demo.nStateSinceMs > 3000)
			DemoPlay_Finalize();
		return;

	case DemoPlayState_t::PLAYING:
		break;
	}

	S21Bridge_StampChanAlive();

	LARGE_INTEGER now, freq;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&freq);
	double dtMs = 0.0;
	if (s_demo.lastFrameQpc.QuadPart && freq.QuadPart)
		dtMs = static_cast<double>(now.QuadPart - s_demo.lastFrameQpc.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
	s_demo.lastFrameQpc = now;
	DemoPlay_FocusProbe(dtMs);
	if (dtMs > 250.0)
		dtMs = 250.0;

	if (s_demo.bClockStarted && !s_demo.bPaused && !s_demo.bEnded && s_demo.nSeekTargetMs < 0)
		s_demo.flDemoMs += dtMs * demo_timescale.GetFloat();

	DemoPlay_RecoverPump();
	DemoPlay_MutePump();
	s_demo.nFrameBudget = 0;
	QueryPerformanceCounter(&s_demo.ffFrameStartQpc);
	DemoPlay_ClockProbe(dtMs);
	DemoPlay_CursorPump();
	DemoCam_Pump();
	DemoCam_UpdateDof();
	DemoPlay_ApplyView();

	// demo_hud 2 is the clean view: no HUD and no outlines (the scripts drop the
	// hit markers and damage numbers); 0 and 1 keep the stock HUD and the
	// overlay script reads demo_hud itself.
	ConVar* const pDrawHud = g_pCVar ? g_pCVar->FindVar("cl_drawhud") : nullptr;
	if (pDrawHud)
	{
		if (s_demo.nSavedDrawHud < 0)
			s_demo.nSavedDrawHud = pDrawHud->GetInt();
		const int nWant = demo_hud.GetInt() == 2 ? 0 : s_demo.nSavedDrawHud;
		if (pDrawHud->GetInt() != nWant)
			pDrawHud->SetValue(nWant);
	}
	ConVar* const pHighlight = g_pCVar ? g_pCVar->FindVar("highlight_draw") : nullptr;
	if (pHighlight)
	{
		if (s_demo.nSavedHighlight < 0)
			s_demo.nSavedHighlight = pHighlight->GetInt();
		const int nWant = demo_hud.GetInt() == 2 ? 0 : s_demo.nSavedHighlight;
		if (pHighlight->GetInt() != nWant)
			pHighlight->SetValue(nWant);
	}

	DemoPlay_LockView();

	if (!s_demo.bOverlayStarted && S21Bridge_GetClientSignonState() >= kSignonFull
		&& GetTickCount64() - s_demo.nOverlayAskMs >= 2000)
	{
		if (!s_demo.nOverlayAskMs)
			Msg(eDLL_T::ENGINE, "[DEMO] in game -- starting the replay overlay (%zu recorded view samples)\n", s_demo.views.size());
		s_demo.nOverlayAskMs = GetTickCount64();
		DemoNatives_StartOverlay();
	}
}

//-----------------------------------------------------------------------------
// Controls
//-----------------------------------------------------------------------------
void DemoPlay_SetPaused(const bool bPaused)
{
	if (s_demo.state != DemoPlayState_t::PLAYING)
		return;
	s_demo.bPaused = bPaused;
	Msg(eDLL_T::ENGINE, "[DEMO] %s at %.2f s\n", bPaused ? "paused" : "resumed", s_demo.flDemoMs / 1000.0);
}

bool DemoPlay_IsPaused(void)
{
	return s_demo.bPaused;
}

void DemoPlay_Step(void)
{
	if (s_demo.state != DemoPlayState_t::PLAYING)
		return;
	s_demo.bPaused = true;
	s_demo.nStepBudget = 1;
}

void DemoPlay_SetTimescale(const float flScale)
{
	demo_timescale.SetValue(flScale);
	// A speed change gets the same clock probe window as a seek (demo_focus_diag).
	s_demo.nClockProbeUntilMs = GetTickCount64() + 15000;
}

float DemoPlay_GetTimescale(void)
{
	return demo_timescale.GetFloat();
}

// Where playback is headed: a pending restart, rewind or fast-forward target
// counts as the current time, so relative seeks stack and the bar never
// snaps back to 0 while the file reconnects.
float DemoPlay_GetTime(void)
{
	if (s_demo.bRestart)
		return s_demo.flRestartSeek > 0.0f ? s_demo.flRestartSeek : 0.0f;
	if (!s_demo.bClockStarted)
		return s_demo.flPendingSeekSec > 0.0f ? s_demo.flPendingSeekSec : 0.0f;
	if (s_demo.nSeekTargetMs >= 0)
		return static_cast<float>(s_demo.nSeekTargetMs / 1000.0);
	return static_cast<float>(s_demo.flDemoMs / 1000.0);
}

bool DemoPlay_IsSeeking(void)
{
	return s_demo.state != DemoPlayState_t::IDLE
		&& (s_demo.bRestart || !s_demo.bClockStarted || s_demo.nSeekTargetMs >= 0);
}

float DemoPlay_GetDuration(void)
{
	return s_demo.nDurationMs / 1000.0f;
}

static int DemoPlay_LevelOf(const size_t nOrder)
{
	const auto it = std::upper_bound(s_demo.levelStarts.begin(), s_demo.levelStarts.end(), nOrder);
	return it == s_demo.levelStarts.begin() ? 0 : static_cast<int>(it - s_demo.levelStarts.begin()) - 1;
}

// Level the world is in once playback reaches nTargetMs.
static int DemoPlay_LevelAtMs(const int64_t nTargetMs)
{
	const std::vector<DemoChunkRef_s>& chunks = s_demo.pReader->GetChunks();
	size_t nLast = 0;
	for (size_t i = 0; i < s_demo.order.size(); ++i)
	{
		const R5DemChunkHeader_s& h = chunks[s_demo.order[i]].hdr;
		if (!DemoPlay_IsClockPacket(h))
			continue;
		if (DemoPlay_RelMs(h) > nTargetMs)
			break;
		nLast = i;
	}
	return DemoPlay_LevelOf(nLast);
}

// Index into the order list of the last full snapshot of nLevel at or before
// nTargetMs: a datagram flagged at record time, or a DataBlock seen decoding one.
static int DemoPlay_FindKeyframe(const int64_t nTargetMs, const int nLevel)
{
	const std::vector<DemoChunkRef_s>& chunks = s_demo.pReader->GetChunks();
	const size_t nBegin = static_cast<size_t>(nLevel) < s_demo.levelStarts.size() ? s_demo.levelStarts[nLevel] : 0;
	const size_t nEnd = static_cast<size_t>(nLevel) + 1 < s_demo.levelStarts.size()
		? s_demo.levelStarts[nLevel + 1] : s_demo.order.size();
	int best = -1;
	for (size_t i = nBegin; i < nEnd; ++i)
	{
		const R5DemChunkHeader_s& h = chunks[s_demo.order[i]].hdr;
		if (!DemoPlay_IsClockPacket(h) || !(h.kind & R5DEM_KIND_FULL))
			continue;
		if (DemoPlay_RelMs(h) > nTargetMs)
			break;
		best = static_cast<int>(i);
	}
	for (const int k : s_demo.blockKeys)
	{
		if (static_cast<size_t>(k) < nBegin || static_cast<size_t>(k) >= nEnd)
			continue;
		if (DemoPlay_RelMs(chunks[s_demo.order[k]].hdr) > nTargetMs)
			break;
		if (k > best)
			best = k;
	}
	for (size_t i = nBegin; i < nEnd; ++i)
	{
		const R5DemChunkHeader_s& h = chunks[s_demo.order[i]].hdr;
		const R5DemChunk_t type = static_cast<R5DemChunk_t>(h.type);
		if ((type != R5DemChunk_t::SIGNON && type != R5DemChunk_t::RELIABLE) || !(h.kind & R5DEM_KIND_FULL))
			continue;
		if (DemoPlay_RelMs(h) > nTargetMs)
			break;
		if (static_cast<int>(i) > best)
			best = static_cast<int>(i);
	}
	return best;
}

// bRebuild: the world is broken, replay from a full snapshot even when the
// target is ahead of the cursor.
static bool DemoPlay_SeekInternal(const float flSeconds, const bool bRebuild)
{
	if (!(flSeconds == flSeconds))
		return false;
	float flClamped = flSeconds < 0.0f ? 0.0f : flSeconds;
	if (flClamped > DemoPlay_GetDuration())
		flClamped = DemoPlay_GetDuration();

	// A restart or connect already in flight takes the new target; starting
	// another one would reload the map again.
	if (s_demo.bRestart)
	{
		s_demo.flRestartSeek = flClamped;
		return true;
	}
	if (!s_demo.pReader)
		return false;
	if (s_demo.state == DemoPlayState_t::WAIT_DISCONNECT || s_demo.state == DemoPlayState_t::CONNECTING
		|| (s_demo.state == DemoPlayState_t::PLAYING && !s_demo.bClockStarted))
	{
		s_demo.flPendingSeekSec = flClamped;
		return true;
	}
	if (s_demo.state != DemoPlayState_t::PLAYING)
		return false;

	const int64_t nTargetMs = static_cast<int64_t>(flClamped * 1000.0f);

	// The loaded world belongs to the level of the last chunk fed. A full
	// snapshot from another level decodes against the wrong tables: an earlier
	// level needs a reload, a later one is reached by playing through the reload.
	const int nCurLevel = DemoPlay_LevelOf(s_demo.nCursor ? s_demo.nCursor - 1 : 0);
	const int nTgtLevel = DemoPlay_LevelAtMs(nTargetMs);
	if (nTgtLevel < nCurLevel)
	{
		DemoPlay_Restart(s_demo.nPov, flClamped);
		return true;
	}

	s_demo.bEnded = false;
	const int nKey = nTgtLevel == nCurLevel ? DemoPlay_FindKeyframe(nTargetMs, nCurLevel) : -1;
	if (!bRebuild && static_cast<double>(nTargetMs) >= s_demo.flDemoMs)
	{
		// Jump to a full snapshot past the cursor instead of replaying up to it.
		const bool bJump = nKey >= 0 && static_cast<size_t>(nKey) > s_demo.nCursor && v_CL_ForceFullUpdate
			&& DemoPlay_RelMs(s_demo.pReader->GetChunks()[s_demo.order[nKey]].hdr) > static_cast<int64_t>(s_demo.flDemoMs) + 3000;
		if (!bJump)
		{
			s_demo.nSeekTargetMs = nTargetMs;
			return true;
		}
	}

	if (demo_seek_restart.GetBool() || nKey < 0 || !v_CL_ForceFullUpdate)
	{
		DemoPlay_Restart(s_demo.nPov, flClamped);
		return true;
	}

	// Soft rewind: drop the receive-side dedup state, ask the engine for a full
	// update, and replay from the nearest full snapshot.
	S21Bridge_ResetForDemoSeek();
	v_CL_ForceFullUpdate();
	s_demo.nCursor = static_cast<size_t>(nKey);
	const std::vector<DemoChunkRef_s>& chunks = s_demo.pReader->GetChunks();
	const int64_t nKeyMs = DemoPlay_RelMs(chunks[s_demo.order[nKey]].hdr);
	s_demo.flDemoMs = static_cast<double>(nKeyMs > 0 ? nKeyMs : 0);
	s_demo.nSeekTargetMs = nTargetMs;
	s_demo.bSoftRewind = true;
	Msg(eDLL_T::ENGINE, "[DEMO] rewinding to %.2f s from the full snapshot at %.2f s\n",
		flClamped, s_demo.flDemoMs / 1000.0);
	return true;
}

bool DemoPlay_Seek(const float flSeconds)
{
	return DemoPlay_SeekInternal(flSeconds, false);
}

// A snapshot that fails to parse leaves every later delta without its base:
// the clock runs on while the world stands still. Rebuild from a full snapshot.
static void DemoPlay_RecoverPump(void)
{
	if (!InterlockedExchange(&s_nSnapDropped, 0) || s_demo.state != DemoPlayState_t::PLAYING
		|| !s_demo.bClockStarted || s_demo.bRestart)
		return;
	const ULONGLONG nNow = GetTickCount64();
	if (nNow - s_demo.nLastRecoverMs < 3000)
		return;
	// A rebuild that fails again gets a clean reload of the file instead.
	const bool bRepeat = s_demo.nLastRecoverMs && nNow - s_demo.nLastRecoverMs < 30000;
	s_demo.nLastRecoverMs = nNow;
	const float flAt = DemoPlay_GetTime();
	Warning(eDLL_T::ENGINE, "[DEMO] a snapshot failed to decode at %.2f s -- %s\n", flAt,
		bRepeat ? "reloading the replay" : "rebuilding from the last full snapshot");
	if (bRepeat)
		DemoPlay_Restart(s_demo.nPov, flAt);
	else
		DemoPlay_SeekInternal(flAt, true);
}

// Fast-forwarded audio is noise; the replay is silent until a seek lands.
static void DemoPlay_MutePump(void)
{
	ConVar* const pVolume = g_pCVar ? g_pCVar->FindVar("sound_volume") : nullptr;
	if (!pVolume)
		return;
	const bool bMute = s_demo.state == DemoPlayState_t::PLAYING && DemoPlay_IsSeeking();
	if (bMute && s_demo.flSavedVolume < 0.0f)
	{
		s_demo.flSavedVolume = pVolume->GetFloat();
		pVolume->SetValue(0.0f);
	}
	else if (!bMute && s_demo.flSavedVolume >= 0.0f)
	{
		pVolume->SetValue(s_demo.flSavedVolume);
		s_demo.flSavedVolume = -1.0f;
	}
}

int DemoPlay_GetPov(void)
{
	return s_demo.nPov;
}

int DemoPlay_GetPovCount(void)
{
	return s_demo.pReader ? static_cast<int>(s_demo.pReader->GetHeader().povCount) : 0;
}

const char* DemoPlay_GetPovName(const int nPov)
{
	if (!s_demo.pReader)
		return "";
	for (const DemoPovInfo_s& p : s_demo.pReader->GetPovs())
	{
		if (p.id == nPov)
			return p.name;
	}
	return "";
}

bool DemoPlay_SetPov(const int nPov)
{
	if (s_demo.state != DemoPlayState_t::PLAYING || nPov < 0 || nPov >= DemoPlay_GetPovCount())
		return false;
	if (nPov == s_demo.nPov)
		return true;
	DemoPlay_Restart(nPov, DemoPlay_GetTime());
	return true;
}

bool DemoPlay_SetView(const int nPovId, const bool bChase)
{
	if (s_demo.state != DemoPlayState_t::PLAYING)
		return false;
	if (nPovId < 0 || nPovId == s_demo.nPov)
	{
		s_demo.nViewPov = -1;
	}
	else
	{
		if (nPovId < DemoPlay_GetPovCount() && DemoPlay_PovHandle(nPovId) == 0xFFFFFFFFu)
		{
			Warning(eDLL_T::ENGINE, "[DEMO] pov %d has no entity handle in this demo\n", nPovId);
			return false;
		}
		s_demo.nViewPov = nPovId;
	}
	s_demo.bViewChase = bChase;
	s_demo.bFreecam = false;
	s_demo.nCamMode = bChase ? DEMO_CAM_THIRD : DEMO_CAM_FIRST;
	DemoPlay_ApplyView();
	return true;
}

int DemoPlay_GetView(void)
{
	return s_demo.nViewPov;
}

void DemoPlay_SetFreecam(const bool bOn)
{
	if (s_demo.state != DemoPlayState_t::PLAYING)
		return;
	if (bOn && !v_Player_RoamingView)
	{
		Warning(eDLL_T::ENGINE, "[DEMO] free camera unavailable (roaming view unresolved) -- use demo_view\n");
		return;
	}
	// Already on: a mode refresh (after a seek, say) leaves the camera where it
	// is. Turning it on starts at the eyes being watched.
	if (bOn && s_demo.bFreecam)
		return;
	s_demo.bFreecam = bOn;
	s_demo.nCamMode = bOn ? DEMO_CAM_FREE : DEMO_CAM_FIRST;
	s_demo.bCamInit = false;
	s_demo.bCamFollow = false;
	s_demo.bCamCarry = false;
	s_demo.bPathPlaying = false;
	DemoPlay_ApplyView();
}

bool DemoPlay_IsFreecam(void)
{
	return s_demo.bFreecam;
}

bool DemoPlay_SetCamera(const int nMode)
{
	if (s_demo.state != DemoPlayState_t::PLAYING || nMode < DEMO_CAM_FIRST || nMode >= DEMO_CAM_COUNT)
		return false;
	if (nMode != DEMO_CAM_FIRST && nMode != DEMO_CAM_THIRD && !v_Player_RoamingView)
	{
		Warning(eDLL_T::ENGINE, "[DEMO] camera %d unavailable (roaming view unresolved)\n", nMode);
		return false;
	}

	switch (nMode)
	{
	case DEMO_CAM_FREE:
		DemoPlay_SetFreecam(true);
		return s_demo.bFreecam;
	case DEMO_CAM_THIRD:
		if (s_demo.nViewPov >= 0)
			return DemoPlay_SetView(s_demo.nViewPov, true);
		break;
	case DEMO_CAM_FIRST:
		if (s_demo.nViewPov >= 0)
			return DemoPlay_SetView(s_demo.nViewPov, false);
		break;
	default:
		break;
	}

	s_demo.bFreecam = false;
	s_demo.bViewChase = false;
	s_demo.nCamMode = nMode;
	s_demo.bCamInit = false;
	DemoPlay_ApplyView();
	return true;
}

int DemoPlay_GetCamera(void)
{
	return s_demo.bFreecam ? DEMO_CAM_FREE : s_demo.nCamMode;
}

std::string DemoPlay_GetMetaJson(void)
{
	return s_demo.pReader ? s_demo.pReader->GetMetaJson() : std::string();
}

void DemoPlay_GetEvents(std::vector<std::string>& outJson, std::vector<std::string>& outRecords)
{
	outJson.clear();
	outRecords.clear();
	if (!s_demo.pReader)
		return;
	// type|seconds|attackerPov|victimPov|weapon|damage|attackerName|victimName
	auto clean = [](const char* pszIn, char* pszOut, const size_t nOut)
	{
		size_t o = 0;
		for (size_t i = 0; pszIn[i] && o + 1 < nOut; ++i)
			pszOut[o++] = (pszIn[i] == '|' || pszIn[i] == '\n' || pszIn[i] == '\r') ? '/' : pszIn[i];
		pszOut[o] = '\0';
	};
	for (const DemoEvent_s& e : s_demo.pReader->GetEvents())
	{
		outJson.push_back(e.json);
		char szA[64], szV[64], szT[16], szW[48];
		clean(e.attackerName, szA, sizeof(szA));
		clean(e.victimName, szV, sizeof(szV));
		clean(e.type, szT, sizeof(szT));
		clean(e.weapon, szW, sizeof(szW));
		char sz[320];
		snprintf(sz, sizeof(sz), "%s|%.3f|%d|%d|%s|%.1f|%s|%s", szT, e.timeSec, e.attacker, e.victim, szW,
			e.damage, szA, szV);
		outRecords.emplace_back(sz);
	}
	for (const float t : s_demo.bookmarks)
	{
		char sz[64];
		snprintf(sz, sizeof(sz), "bookmark|%.3f|-1|-1||0.0||", t);
		outJson.emplace_back("{\"t\":\"bookmark\"}");
		outRecords.emplace_back(sz);
	}
}

bool DemoPlay_GetInputsAt(const int nPov, const float flSeconds, char* pszOut, const size_t nOutLen)
{
	if (!s_demo.pReader || nPov < 0 || nPov >= static_cast<int>(s_demo.userCmds.size()))
		return false;
	const std::vector<DemoUserCmdAt_s>& cmds = s_demo.userCmds[nPov];
	if (cmds.empty())
		return false;

	const uint32_t nFirst = s_demo.pReader->GetFirstFullTick(s_demo.nPov);
	const float flTick = s_demo.pReader->GetHeader().tickIntervalUs
		? s_demo.pReader->GetHeader().tickIntervalUs / 1000000.0f : 0.05f;
	if (nFirst == UINT32_MAX)
		return false;
	const uint32_t nTick = nFirst + static_cast<uint32_t>((flSeconds > 0.0f ? flSeconds : 0.0f) / flTick);

	auto it = std::upper_bound(cmds.begin(), cmds.end(), nTick,
		[](const uint32_t t, const DemoUserCmdAt_s& c) { return t < c.tick; });
	if (it != cmds.begin())
		--it;
	const R5DemUserCmd_s& c = it->cmd;
	snprintf(pszOut, nOutLen, "%u|%.2f|%.2f|%.2f|%.2f|%.2f", c.buttons, c.forwardmove, c.sidemove, c.upmove, c.pitch, c.yaw);
	return true;
}

//-----------------------------------------------------------------------------
// Free camera: the roaming observer view, driven by the local view angles and
// the movement keys while the game window has focus.
//-----------------------------------------------------------------------------
static bool DemoPlay_WindowFocused(void)
{
	const HWND hFg = GetForegroundWindow();
	if (!hFg)
		return false;
	DWORD pid = 0;
	GetWindowThreadProcessId(hFg, &pid);
	return pid == GetCurrentProcessId();
}

// The free camera's angles become the engine's, so leaving it keeps the facing.
static void DemoCam_WriteEngineAngles(const float* pAngles)
{
	if (s_demo.state != DemoPlayState_t::PLAYING)
		return;
	float* const p = DemoPlay_ViewAnglesSlot();
	if (!p)
		return;
	p[0] = pAngles[0];
	p[1] = pAngles[1];
}

static float DemoCam_ClampFov(const float flFov)
{
	return flFov < kCamMinFov ? kCamMinFov : (flFov > kCamMaxFov ? kCamMaxFov : flFov);
}

// The field of view the free camera shows: the override, else the game's.
static float DemoCam_CurrentFov(void)
{
	const float flFov = demo_freecam_fov.GetFloat();
	if (flFov >= kCamMinFov)
		return DemoCam_ClampFov(flFov);
	return s_demo.flCamStockFov;
}

// A keyframe's field of view as the override convar holds it: 0 when it is the game's.
static void DemoCam_SetFovConVar(const float flFov)
{
	const bool bStock = !(flFov >= kCamMinFov)
		|| (s_demo.flCamStockFov > 0.0f && fabsf(flFov - s_demo.flCamStockFov) < 0.5f);
	demo_freecam_fov.SetValue(bStock ? 0.0f : DemoCam_ClampFov(flFov));
}

// Each key's time on the path clock. A segment is the replay time between its
// keys, so the camera is on a point when the replay is. Keys placed in the
// same moment have no replay span; those still get a short flight.
static void DemoCam_SortKeys(void)
{
	std::stable_sort(s_demo.camKeys.begin(), s_demo.camKeys.end(),
		[](const DemoCamKey_s& a, const DemoCamKey_s& b) { return a.tMs < b.tMs; });
	s_demo.camPathMs.clear();
}

// Points more than half a minute apart are separate shots. Play the shot the
// replay is in, otherwise the one just passed, otherwise the first.
static bool DemoCam_PickShot(const double nowMs, double& t0, double& t1)
{
	const std::vector<DemoCamKey_s>& k = s_demo.camKeys;
	t0 = t1 = 0.0;
	if (k.size() < 2)
		return false;
	struct Shot { double a, b; };
	std::vector<Shot> shots;
	size_t begin = 0;
	for (size_t i = 1; i <= k.size(); ++i)
	{
		const bool bSplit = i == k.size() || (k[i].tMs - k[i - 1].tMs) > 30000.0;
		if (!bSplit)
			continue;
		if (i - 1 > begin)
			shots.push_back(Shot{ k[begin].tMs, k[i - 1].tMs });
		begin = i;
	}
	if (shots.empty())
	{
		t0 = k.front().tMs;
		t1 = k.back().tMs;
		return t1 > t0;
	}
	for (const Shot& shot : shots)
	{
		if (nowMs >= shot.a && nowMs <= shot.b)
		{
			t0 = shot.a;
			t1 = shot.b;
			return true;
		}
	}
	for (size_t i = shots.size(); i-- > 0; )
	{
		if (shots[i].b <= nowMs)
		{
			t0 = shots[i].a;
			t1 = shots[i].b;
			return true;
		}
	}
	t0 = shots.front().a;
	t1 = shots.front().b;
	return true;
}

static void DemoCam_BuildPathTimes(void)
{
	DemoCam_SortKeys();
	const std::vector<DemoCamKey_s>& k = s_demo.camKeys;
	s_demo.camPathMs.assign(k.size(), 0.0);
	const float flSpeed = fmaxf(10.0f, demo_campath_speed.GetFloat());
	for (size_t i = 1; i < k.size(); ++i)
	{
		const DemoCamKey_s& a = k[i - 1];
		const DemoCamKey_s& b = k[i];
		const double flGap = b.tMs - a.tMs;
		double flSegMs = flGap;
		if (!(flSegMs > 0.0))
		{
			const float dx = b.pos[0] - a.pos[0], dy = b.pos[1] - a.pos[1], dz = b.pos[2] - a.pos[2];
			const double flFlyMs = sqrtf(dx * dx + dy * dy + dz * dz) / flSpeed * 1000.0;
			flSegMs = fmax(kCamPathMinSegMs, flFlyMs);
		}
		s_demo.camPathMs[i] = s_demo.camPathMs[i - 1] + flSegMs;
	}
}

// True when every key is later in the replay than the one before it, so the
// path clock above is just the replay time minus the first key.
static bool DemoCam_PathUsesReplay(void)
{
	const std::vector<DemoCamKey_s>& k = s_demo.camKeys;
	if (k.size() < 2)
		return false;
	for (size_t i = 1; i < k.size(); ++i)
		if (!(k[i].tMs > k[i - 1].tMs))
			return false;
	return true;
}

// Where the spline is sampled: the replay's own time between the keys, or the
// path clock when the keys share a timestamp.
static double DemoCam_PathSampleMs(void)
{
	if (!DemoCam_PathUsesReplay())
		return s_demo.flPathMs;
	double t = static_cast<double>(DemoPlay_GetTime()) * 1000.0 - s_demo.camKeys.front().tMs;
	if (t < 0.0)
		t = 0.0;
	return t;
}

// The camera path's pose at a time on the path clock; holds the first and last
// keys outside it. False with fewer than two keys.
static bool DemoCam_EvalPath(const double tMs, float* pPos, float* pAng, float* pFov, float* pDof = nullptr)
{
	const std::vector<DemoCamKey_s>& k = s_demo.camKeys;
	if (k.size() < 2)
		return false;
	if (s_demo.camPathMs.size() != k.size())
		DemoCam_BuildPathTimes();
	const std::vector<double>& T = s_demo.camPathMs;

	const DemoCamKey_s* pHold = nullptr;
	if (tMs <= T.front())
		pHold = &k.front();
	else if (tMs >= T.back())
		pHold = &k.back();
	if (pHold)
	{
		for (int i = 0; i < 3; ++i)
		{
			pPos[i] = pHold->pos[i];
			pAng[i] = pHold->ang[i];
		}
		*pFov = pHold->fov;
		if (pDof)
			*pDof = pHold->dof;
		return true;
	}

	const size_t i2 = static_cast<size_t>(std::upper_bound(T.begin(), T.end(), tMs) - T.begin());
	const size_t i1 = i2 - 1;
	const DemoCamKey_s& b = k[i1];
	const DemoCamKey_s& c = k[i2];
	const double tb = T[i1], tc = T[i2];
	float u = static_cast<float>((tMs - tb) / (tc - tb));
	if (u < 0.0f) u = 0.0f;
	if (u > 1.0f) u = 1.0f;

	for (int i = 0; i < 3; ++i)
		pPos[i] = b.pos[i] + (c.pos[i] - b.pos[i]) * u;

	// Pitch, yaw and roll turn the short way, and they move the whole time
	// between the keys instead of sitting on the first one.
	for (int i = 0; i < 3; ++i)
	{
		const float d = remainderf(c.ang[i] - b.ang[i], 360.0f);
		const float y = remainderf(b.ang[i] + (std::isfinite(d) ? d : 0.0f) * u, 360.0f);
		pAng[i] = std::isfinite(y) ? y : b.ang[i];
	}
	if (pAng[0] > 89.0f) pAng[0] = 89.0f;
	if (pAng[0] < -89.0f) pAng[0] = -89.0f;

	*pFov = DemoCam_ClampFov(b.fov + (c.fov - b.fov) * u);
	if (pDof)
	{
		if (b.dof > 0.0f && c.dof > 0.0f)
			*pDof = b.dof + (c.dof - b.dof) * u;
		else
			*pDof = u < 1.0f ? b.dof : c.dof;
	}
	return true;
}

//-----------------------------------------------------------------------------
// Camera path keyframes, kept in <file>.campath beside the replay
//-----------------------------------------------------------------------------
static bool DemoCam_KeysFile(char* pszOut, const size_t nOutLen)
{
	return s_demo.szPath[0] && _snprintf_s(pszOut, nOutLen, _TRUNCATE, "%s.campath", s_demo.szPath) > 0;
}

static bool DemoCam_KeyValid(const DemoCamKey_s& k)
{
	for (int i = 0; i < 3; ++i)
	{
		if (!std::isfinite(k.pos[i]) || fabsf(k.pos[i]) > 65535.0f || !std::isfinite(k.ang[i]) || fabsf(k.ang[i]) > 720.0f)
			return false;
	}
	return std::isfinite(k.tMs) && k.tMs >= 0.0 && k.tMs < 86400000.0
		&& std::isfinite(k.fov) && k.fov >= kCamMinFov && k.fov <= kCamMaxFov && fabsf(remainderf(k.ang[0], 360.0f)) <= 90.0f
		&& std::isfinite(k.dof) && k.dof >= 0.0f && k.dof <= 20000.0f;
}

static void DemoCam_LoadKeys(void)
{
	// A restart of the same replay keeps the keys in memory.
	if (!_stricmp(s_demo.szCamKeysPath, s_demo.szPath))
		return;
	s_demo.camKeys.clear();
	s_demo.camPathMs.clear();
	s_demo.bPathPlaying = false;
	strncpy_s(s_demo.szCamKeysPath, s_demo.szPath, _TRUNCATE);

	char szPath[280];
	if (!DemoCam_KeysFile(szPath, sizeof(szPath)))
		return;
	const HANDLE h = CreateFileA(szPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return;
	std::vector<char> buf(64 * 1024);
	DWORD n = 0;
	const BOOL bRead = ReadFile(h, buf.data(), static_cast<DWORD>(buf.size() - 1), &n, nullptr);
	CloseHandle(h);
	if (!bRead)
		return;
	buf[n] = '\0';

	char* pCtx = nullptr;
	for (char* pLine = strtok_s(buf.data(), "\r\n", &pCtx); pLine && s_demo.camKeys.size() < kCamMaxKeys;
		pLine = strtok_s(nullptr, "\r\n", &pCtx))
	{
		DemoCamKey_s k = {};
		// The focus distance column is optional.
		const int nFields = sscanf_s(pLine, "%lf %f %f %f %f %f %f %f %f", &k.tMs, &k.pos[0], &k.pos[1], &k.pos[2],
			&k.ang[0], &k.ang[1], &k.ang[2], &k.fov, &k.dof);
		if (nFields < 8 || !DemoCam_KeyValid(k))
			continue;
		for (int i = 0; i < 3; ++i)
			k.ang[i] = remainderf(k.ang[i], 360.0f);
		s_demo.camKeys.push_back(k);
	}
	s_demo.camPathMs.clear();
	s_demo.nCamEditKey = -1;
	if (!s_demo.camKeys.empty())
		Msg(eDLL_T::ENGINE, "[DEMO] %zu camera keyframes from %s\n", s_demo.camKeys.size(), szPath);
}

static void DemoCam_SaveKeys(void)
{
	char szPath[280];
	if (!DemoCam_KeysFile(szPath, sizeof(szPath)))
		return;
	if (s_demo.camKeys.empty())
	{
		DeleteFileA(szPath);
		return;
	}

	std::string out = "# ms x y z pitch yaw roll fov dof\r\n";
	for (const DemoCamKey_s& k : s_demo.camKeys)
	{
		char szLine[192];
		const int n = snprintf(szLine, sizeof(szLine), "%.1f %.3f %.3f %.3f %.3f %.3f %.3f %.2f %.1f\r\n",
			k.tMs, k.pos[0], k.pos[1], k.pos[2], k.ang[0], k.ang[1], k.ang[2], k.fov, k.dof);
		if (n > 0 && n < static_cast<int>(sizeof(szLine)))
			out.append(szLine, static_cast<size_t>(n));
	}

	const HANDLE h = CreateFileA(szPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	DWORD nWritten = 0;
	const bool bOk = h != INVALID_HANDLE_VALUE
		&& WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &nWritten, nullptr) && nWritten == out.size();
	if (h != INVALID_HANDLE_VALUE)
		CloseHandle(h);
	if (!bOk)
		Warning(eDLL_T::ENGINE, "[DEMO] camera keyframes could not be saved to %s\n", szPath);
}

// Puts the free camera at a keyframe's pose.
static void DemoCam_SetPose(const DemoCamKey_s& k)
{
	for (int i = 0; i < 3; ++i)
	{
		s_demo.camOrigin[i] = k.pos[i];
		s_demo.camAngles[i] = k.ang[i];
	}
	demo_freecam_roll.SetValue(remainderf(k.ang[2], 360.0f));
	DemoCam_SetFovConVar(k.fov);
	demo_freecam_dof.SetValue(k.dof);
	QueryPerformanceCounter(&s_demo.camQpc);
	s_demo.bCamInit = true;
	s_demo.bCamCarry = false;
	DemoCam_WriteEngineAngles(s_demo.camAngles);
}

static bool DemoCam_Ready(void)
{
	return s_demo.state == DemoPlayState_t::PLAYING && s_demo.bFreecam && s_demo.bCamInit && s_demo.bClockStarted;
}

bool DemoPlay_CamKeyAdd(void)
{
	if (!DemoCam_Ready())
	{
		Warning(eDLL_T::ENGINE, "[DEMO] camera keyframes need the free camera\n");
		return false;
	}
	if (s_demo.bPathPlaying)
	{
		Warning(eDLL_T::ENGINE, "[DEMO] stop the camera path (P) before adding keyframes\n");
		return false;
	}
	DemoCamKey_s k = {};
	k.tMs = static_cast<double>(DemoPlay_GetTime()) * 1000.0;
	for (int i = 0; i < 3; ++i)
	{
		k.pos[i] = s_demo.camOrigin[i];
		k.ang[i] = s_demo.camAngles[i];
	}
	// The roll as set now, not as the last view drew it.
	if (!s_demo.bPathPlaying)
		k.ang[2] = demo_freecam_roll.GetFloat();
	for (int i = 0; i < 3; ++i)
		k.ang[i] = remainderf(k.ang[i], 360.0f);
	k.fov = DemoCam_CurrentFov();
	if (!(k.fov >= kCamMinFov))
		k.fov = 90.0f;
	k.fov = DemoCam_ClampFov(k.fov);
	k.dof = demo_freecam_dof.GetFloat();
	if (!DemoCam_KeyValid(k))
		return false;

	// K on a key that J/L/goto landed on adjusts it; anywhere else it adds the
	// next point of the path.
	std::vector<DemoCamKey_s>& keys = s_demo.camKeys;
	const int nEdit = s_demo.nCamEditKey;
	const bool bReplace = nEdit >= 0 && nEdit < static_cast<int>(keys.size())
		&& fabs(keys[nEdit].tMs - k.tMs) < 250.0;
	if (bReplace)
	{
		k.tMs = keys[nEdit].tMs;
		keys[nEdit] = k;
	}
	else
	{
		if (keys.size() >= kCamMaxKeys)
		{
			Warning(eDLL_T::ENGINE, "[DEMO] a camera path holds at most %zu keyframes\n", kCamMaxKeys);
			return false;
		}
		keys.push_back(k);
	}
	s_demo.nCamEditKey = -1;
	s_demo.camPathMs.clear();
	DemoCam_SaveKeys();
	Msg(eDLL_T::ENGINE, "[DEMO] camera keyframe %d %s at %.2f s (%zu in the path)\n",
		bReplace ? nEdit + 1 : static_cast<int>(keys.size()), bReplace ? "updated" : "added", k.tMs / 1000.0, keys.size());
	return true;
}

bool DemoPlay_CamKeyRemove(const int nIndex)
{
	if (nIndex < 0 || nIndex >= static_cast<int>(s_demo.camKeys.size()))
		return false;
	const double tMs = s_demo.camKeys[nIndex].tMs;
	s_demo.camKeys.erase(s_demo.camKeys.begin() + nIndex);
	s_demo.camPathMs.clear();
	s_demo.nCamEditKey = -1;
	if (s_demo.camKeys.size() < 2)
		s_demo.bPathPlaying = false;
	DemoCam_SaveKeys();
	Msg(eDLL_T::ENGINE, "[DEMO] camera keyframe at %.2f s removed (%zu left)\n", tMs / 1000.0, s_demo.camKeys.size());
	return true;
}

// The key J/L/goto landed on while the replay is still there, else the one at
// the current replay time give or take a quarter second; -1 if none.
static int DemoCam_KeyAtNow(void)
{
	const double tMs = static_cast<double>(DemoPlay_GetTime()) * 1000.0;
	const int nEdit = s_demo.nCamEditKey;
	if (nEdit >= 0 && nEdit < static_cast<int>(s_demo.camKeys.size()) && fabs(s_demo.camKeys[nEdit].tMs - tMs) < 250.0)
		return nEdit;
	int best = -1;
	double bestD = 250.0;
	for (size_t i = 0; i < s_demo.camKeys.size(); ++i)
	{
		const double d = fabs(s_demo.camKeys[i].tMs - tMs);
		if (d <= bestD)
		{
			bestD = d;
			best = static_cast<int>(i);
		}
	}
	return best;
}

void DemoPlay_CamKeyClear(void)
{
	s_demo.camKeys.clear();
	s_demo.camPathMs.clear();
	s_demo.nCamEditKey = -1;
	s_demo.bPathPlaying = false;
	DemoCam_SaveKeys();
	Msg(eDLL_T::ENGINE, "[DEMO] camera path cleared\n");
}

// Pauses on a keyframe with the camera at its pose, ready to be adjusted and
// stored again with K.
bool DemoPlay_CamKeyGoto(const int nIndex)
{
	if (s_demo.state != DemoPlayState_t::PLAYING || !s_demo.bFreecam
		|| nIndex < 0 || nIndex >= static_cast<int>(s_demo.camKeys.size()))
		return false;
	const DemoCamKey_s k = s_demo.camKeys[nIndex];
	s_demo.bPathPlaying = false;
	// The pose first: a seek that has to reconnect carries it.
	DemoCam_SetPose(k);
	DemoPlay_SetPaused(true);
	DemoPlay_Seek(static_cast<float>(k.tMs / 1000.0));
	s_demo.nCamEditKey = nIndex;
	Msg(eDLL_T::ENGINE, "[DEMO] camera keyframe %d of %zu at %.2f s\n", nIndex + 1, s_demo.camKeys.size(), k.tMs / 1000.0);
	return true;
}

// The previous (nDir < 0) or next keyframe in path order from the one last
// landed on; from none, J goes to the last key and L to the first.
bool DemoPlay_CamKeyStep(const int nDir)
{
	const int nCount = static_cast<int>(s_demo.camKeys.size());
	if (nCount == 0)
		return false;
	const int nCur = DemoCam_KeyAtNow();
	int nIndex;
	if (nCur < 0)
		nIndex = nDir < 0 ? nCount - 1 : 0;
	else
		nIndex = nCur + (nDir < 0 ? -1 : 1);
	return nIndex >= 0 && nIndex < nCount && DemoPlay_CamKeyGoto(nIndex);
}

void DemoPlay_GetCamKeyData(std::vector<float>& out)
{
	out.clear();
	for (const DemoCamKey_s& k : s_demo.camKeys)
	{
		out.push_back(static_cast<float>(k.tMs / 1000.0));
		out.push_back(k.ang[2]);
		out.push_back(k.fov);
		out.push_back(k.dof);
	}
}

int DemoPlay_CamKeyAtNow(void)
{
	return DemoCam_KeyAtNow();
}

void DemoPlay_GetCamKeyTimes(std::vector<float>& out)
{
	out.clear();
	for (const DemoCamKey_s& k : s_demo.camKeys)
		out.push_back(static_cast<float>(k.tMs / 1000.0));
}

// The free camera leaves the path where the path left it.
static void DemoCam_StopPath(void)
{
	if (!s_demo.bPathPlaying)
		return;
	s_demo.bPathPlaying = false;
	float pos[3], ang[3], flFov = 0.0f, flDof = 0.0f;
	if (DemoCam_EvalPath(DemoCam_PathSampleMs(), pos, ang, &flFov, &flDof))
	{
		demo_freecam_roll.SetValue(ang[2]);
		DemoCam_SetFovConVar(flFov);
		demo_freecam_dof.SetValue(flDof);
	}
	Msg(eDLL_T::ENGINE, "[DEMO] camera path stopped at %.2f s\n", s_demo.flDemoMs / 1000.0);
}

// Flies the free camera through the keys in order while the replay plays from
// the first key's time.
bool DemoPlay_SetCamPathPlaying(const bool bOn)
{
	if (!bOn)
	{
		DemoCam_StopPath();
		return true;
	}
	if (s_demo.state != DemoPlayState_t::PLAYING || !s_demo.bFreecam || s_demo.camKeys.size() < 2)
	{
		Warning(eDLL_T::ENGINE, "[DEMO] a camera path needs the free camera and two keyframes\n");
		return false;
	}
	DemoCam_BuildPathTimes();
	s_demo.flPathMs = 0.0;
	s_demo.bCamFollow = false;
	s_demo.nCamEditKey = -1;
	double t0 = s_demo.camKeys.front().tMs;
	double t1 = s_demo.camKeys.back().tMs;
	DemoCam_PickShot(static_cast<double>(DemoPlay_GetTime()) * 1000.0, t0, t1);
	s_demo.flPathUntilMs = t1;
	const double nowMs = static_cast<double>(DemoPlay_GetTime()) * 1000.0;
	if (fabs(nowMs - t0) > 50.0)
		DemoPlay_Seek(static_cast<float>(t0 / 1000.0));
	DemoPlay_SetPaused(false);
	s_demo.bPathPlaying = true;
	DemoCam_SaveKeys();
	Msg(eDLL_T::ENGINE, "[DEMO] camera path playing %zu keyframes from %.2f s to %.2f s\n",
		s_demo.camKeys.size(), t0 / 1000.0, t1 / 1000.0);
	return true;
}

bool DemoPlay_IsCamPathPlaying(void)
{
	return s_demo.bPathPlaying;
}

void DemoPlay_SetCamRoll(const float flDegrees)
{
	if (std::isfinite(flDegrees))
		demo_freecam_roll.SetValue(remainderf(flDegrees, 360.0f));
}

float DemoPlay_GetCamRoll(void)
{
	return demo_freecam_roll.GetFloat();
}

void DemoPlay_SetCamFov(const float flDegrees)
{
	if (std::isfinite(flDegrees))
		demo_freecam_fov.SetValue(flDegrees >= kCamMinFov ? DemoCam_ClampFov(flDegrees) : 0.0f);
}

float DemoPlay_GetCamFov(void)
{
	return DemoCam_CurrentFov();
}

void DemoPlay_SetCamDof(const float flFocus, const float flRange)
{
	if (std::isfinite(flFocus))
		demo_freecam_dof.SetValue(flFocus);
	if (std::isfinite(flRange) && flRange > 0.0f)
		demo_freecam_dof_range.SetValue(flRange);
}

float DemoPlay_GetCamDof(void)
{
	return demo_freecam_dof.GetFloat();
}

float DemoPlay_GetCamDofRange(void)
{
	return demo_freecam_dof_range.GetFloat();
}

void DemoPlay_OnMouseWheel(const int nDelta)
{
	if (s_bDemoGate && s_demo.bFreecam)
		InterlockedExchangeAdd(&s_nCamWheel, nDelta);
}

void DemoPlay_OnRawMouse(const int nDx, const int nDy)
{
	if (!s_bDemoGate || !s_demo.bFreecam)
		return;
	InterlockedExchangeAdd(&s_nCamMouseX, nDx);
	InterlockedExchangeAdd(&s_nCamMouseY, nDy);
	InterlockedExchangeAdd(&s_nCamMouseSeen, abs(nDx) + abs(nDy));
}

static float DemoCam_VarFloat(const char* pszName, const float flDefault)
{
	ConVar* const pVar = g_pCVar ? g_pCVar->FindVar(pszName) : nullptr;
	const float fl = pVar ? pVar->GetFloat() : flDefault;
	return std::isfinite(fl) ? fl : flDefault;
}

// Turns the free camera by the mouse motion since the last frame, scaled like
// the game's own mouse look. Motion while the pointer is up is dropped.
static void DemoCam_ApplyMouseLook(const bool bLook)
{
	const LONG nDx = InterlockedExchange(&s_nCamMouseX, 0);
	const LONG nDy = InterlockedExchange(&s_nCamMouseY, 0);
	if (!bLook || (!nDx && !nDy))
		return;

	const float flSens = fminf(fmaxf(DemoCam_VarFloat("mouse_sensitivity", 5.0f), 0.0f), 100.0f);
	const float flYaw = DemoCam_VarFloat("m_yaw", 0.022f);
	float flPitch = DemoCam_VarFloat("m_pitch", 0.022f);
	if (DemoCam_VarFloat("m_invert_pitch", 0.0f) != 0.0f)
		flPitch = -flPitch;

	s_demo.camAngles[1] = remainderf(s_demo.camAngles[1] - static_cast<float>(nDx) * flSens * flYaw, 360.0f);
	s_demo.camAngles[0] = fminf(89.0f, fmaxf(-89.0f, s_demo.camAngles[0] + static_cast<float>(nDy) * flSens * flPitch));
}

static const char* const s_pszDofVars[4] = { "dof_nearDepthStart", "dof_nearDepthEnd", "dof_farDepthStart", "dof_farDepthEnd" };

// Hands the engine's depth-of-field override back as it was.
static void DemoCam_ReleaseDof(void)
{
	if (!s_demo.bDofOwned)
		return;
	s_demo.bDofOwned = false;
	s_demo.flDofApplied = -1.0f;
	s_demo.flDofRangeApplied = -1.0f;
	if (!g_pCVar)
		return;
	if (ConVar* const pOverride = g_pCVar->FindVar("dof_overrideParams"))
		pOverride->SetValue(s_demo.nSavedDofOverride);
	for (int i = 0; i < 4; ++i)
		if (ConVar* const pVar = g_pCVar->FindVar(s_pszDofVars[i]))
			pVar->SetValue(s_demo.flSavedDof[i]);
}

// The free camera's focus runs the engine's depth-of-field override: sharp for
// the range around the focus distance, full blur one more range beyond it.
static void DemoCam_UpdateDof(void)
{
	float flFocus = 0.0f;
	if (s_demo.state == DemoPlayState_t::PLAYING && s_demo.bFreecam)
	{
		float pos[3], ang[3], flFov = 0.0f;
		if (!(s_demo.bPathPlaying && DemoCam_EvalPath(s_demo.flPathMs, pos, ang, &flFov, &flFocus)))
			flFocus = demo_freecam_dof.GetFloat();
	}
	if (!(flFocus >= 1.0f) || !g_pCVar)
	{
		DemoCam_ReleaseDof();
		return;
	}

	ConVar* const pOverride = g_pCVar->FindVar("dof_overrideParams");
	ConVar* pVars[4] = {};
	for (int i = 0; i < 4; ++i)
		pVars[i] = g_pCVar->FindVar(s_pszDofVars[i]);
	if (!pOverride || !pVars[0] || !pVars[1] || !pVars[2] || !pVars[3])
		return;

	if (!s_demo.bDofOwned)
	{
		s_demo.nSavedDofOverride = pOverride->GetInt();
		for (int i = 0; i < 4; ++i)
			s_demo.flSavedDof[i] = pVars[i]->GetFloat();
		s_demo.bDofOwned = true;
	}
	const float flRange = demo_freecam_dof_range.GetFloat();
	if (pOverride->GetInt() != 1)
		pOverride->SetValue(1);
	if (flFocus == s_demo.flDofApplied && flRange == s_demo.flDofRangeApplied)
		return;
	s_demo.flDofApplied = flFocus;
	s_demo.flDofRangeApplied = flRange;

	const float flHalf = flRange * 0.5f;
	// A near start past its end turns the near blur off, as the engine reads it.
	pVars[0]->SetValue(fmaxf(0.0f, flFocus - flHalf - flRange));
	pVars[1]->SetValue(fmaxf(0.0f, flFocus - flHalf));
	pVars[2]->SetValue(flFocus + flHalf);
	pVars[3]->SetValue(flFocus + flHalf + flRange);
}

// 25% faster per step up, back down the same way.
static void DemoCam_ScaleSpeed(const float flSteps)
{
	const float flSpeed = demo_freecam_speed.GetFloat() * powf(1.25f, flSteps);
	demo_freecam_speed.SetValue(fminf(5000.0f, fmaxf(10.0f, flSpeed)));
	Msg(eDLL_T::ENGINE, "[DEMO] free camera speed %.0f\n", demo_freecam_speed.GetFloat());
}

// S21 C_BaseEntity: the current and next networked snapshot (lerp data) and
// the abs origin. A snapshot holds world time at +0, origin at +4, angles at +0x10.
static constexpr ptrdiff_t kClientEntLerpCurrent = 0x110;
static constexpr ptrdiff_t kClientEntLerpFuture  = 0x118;
static constexpr ptrdiff_t kClientEntAbsOrigin   = 0x188;

struct DemoCamSnap_s
{
	float time;
	float origin[3];
	float angles[3];
};

static bool DemoCam_CoordsValid(const float* p, const int n)
{
	for (int i = 0; i < n; ++i)
		if (!std::isfinite(p[i]) || fabsf(p[i]) > 65535.0f)
			return false;
	return true;
}

// The player the camera follows: the one being watched, else the recorded one.
static uintptr_t DemoCam_FollowEntity(void)
{
	if (s_demo.nViewPov < 0)
		return DemoPlay_LocalPlayer();
	const uint32_t eh = s_demo.nViewPov < DemoPlay_GetPovCount()
		? DemoPlay_PovHandle(s_demo.nViewPov) : static_cast<uint32_t>(s_demo.nViewPov);
	if (eh == 0xFFFFFFFFu)
		return 0;
	return reinterpret_cast<uintptr_t>(ClientEntityList_EntityAt(static_cast<int>(eh & ENT_ENTRY_MASK),
		static_cast<int>(eh >> NUM_SERIAL_NUM_SHIFT_BITS)));
}

static bool DemoCam_ReadSnap(const uintptr_t pEnt, const ptrdiff_t nSlot, DemoCamSnap_s& out)
{
	__try
	{
		const uintptr_t pSnap = *reinterpret_cast<const uintptr_t*>(pEnt + nSlot);
		if (!pSnap)
			return false;
		const float* const p = reinterpret_cast<const float*>(pSnap);
		out.time = p[0];
		for (int i = 0; i < 3; ++i)
		{
			out.origin[i] = p[1 + i];
			out.angles[i] = p[4 + i];
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	return std::isfinite(out.time) && DemoCam_CoordsValid(out.origin, 3) && DemoCam_CoordsValid(out.angles, 3);
}

// Where the followed player is now: its networked snapshots, interpolated from
// the current one to the next on the wall clock. The abs origin is the fallback.
static bool DemoCam_FollowPose(const LARGE_INTEGER& now, const LARGE_INTEGER& freq, float* pOrigin, float* pYaw)
{
	const uintptr_t pEnt = DemoCam_FollowEntity();
	if (!pEnt)
		return false;
	DemoCamSnap_s cur, fut;
	const bool bCur = DemoCam_ReadSnap(pEnt, kClientEntLerpCurrent, cur);
	const bool bFut = DemoCam_ReadSnap(pEnt, kClientEntLerpFuture, fut);
	if (!bCur && !bFut)
	{
		__try
		{
			const float* const p = reinterpret_cast<const float*>(pEnt + kClientEntAbsOrigin);
			for (int i = 0; i < 3; ++i)
				pOrigin[i] = p[i];
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		*pYaw = 0.0f;
		return DemoCam_CoordsValid(pOrigin, 3);
	}
	if (!bFut)
		fut = cur;
	else if (!bCur)
		cur = fut;

	if (fut.time != s_demo.flFollowSnapTime)
	{
		s_demo.flFollowSnapTime = fut.time;
		s_demo.followSnapQpc = now;
	}
	const float flSpan = fut.time - cur.time;
	float f = 1.0f;
	if (flSpan > 0.0f && flSpan < 1.0f && freq.QuadPart && !DemoPlay_IsSeeking())
	{
		const float flSince = static_cast<float>(now.QuadPart - s_demo.followSnapQpc.QuadPart) / static_cast<float>(freq.QuadPart);
		f = fminf(1.0f, fmaxf(0.0f, flSince * demo_timescale.GetFloat() / flSpan));
	}
	for (int i = 0; i < 3; ++i)
		pOrigin[i] = cur.origin[i] + (fut.origin[i] - cur.origin[i]) * f;
	*pYaw = cur.angles[1] + remainderf(fut.angles[1] - cur.angles[1], 360.0f) * f;
	return true;
}

void DemoPlay_SetCamFollow(const bool bOn)
{
	if (bOn && (s_demo.state != DemoPlayState_t::PLAYING || !s_demo.bFreecam || !s_demo.bCamInit))
		return;
	// The offset is taken on the next view from the free camera, not the eyes.
	s_demo.bCamFollow = bOn;
	s_demo.bFollowYawInit = false;
	s_demo.flFollowSnapTime = -1.0f;
	Msg(eDLL_T::ENGINE, "[DEMO] free camera %s the player\n", bOn ? "follows" : "no longer follows");
}

bool DemoPlay_IsCamFollow(void)
{
	return s_demo.bFreecam && s_demo.bCamFollow;
}

// Once a frame while playing: the wheel, middle click and path keys.
static void DemoCam_Pump(void)
{
	static const int s_vk[] = { 'K', 'J', 'L', VK_DELETE, 'P', VK_MBUTTON, 'T', VK_OEM_PLUS, VK_ADD, VK_OEM_MINUS, VK_SUBTRACT };
	int nDown = 0;
	for (int i = 0; i < static_cast<int>(sizeof(s_vk) / sizeof(s_vk[0])); ++i)
		if (GetAsyncKeyState(s_vk[i]) & 0x8000)
			nDown |= 1 << i;
	const int nPressed = nDown & ~s_demo.nCamKeysPrev;
	s_demo.nCamKeysPrev = nDown;
	const LONG nWheel = InterlockedExchange(&s_nCamWheel, 0);

	static ULONGLONG s_nInputDiagMs = 0;
	if (demo_input_diag.GetBool() && GetTickCount64() - s_nInputDiagMs >= 1000)
	{
		s_nInputDiagMs = GetTickCount64();
		float eng[3] = {};
		if (v_Engine_GetViewAngles)
			v_Engine_GetViewAngles(nullptr, eng);
		Msg(eDLL_T::ENGINE, "[DEMO-INPUT] t=%.2f free=%d path=%d eng=(%.1f %.1f) cam=(%.1f %.1f) org=(%.0f %.0f %.0f) "
			"mouse=%ld views=%ld dt=%.4f w=%d mouseLayer=%d blockInput=%d focused=%d paused=%d hud=%d keys=0x%X\n",
			s_demo.flDemoMs / 1000.0, s_demo.bFreecam ? 1 : 0, s_demo.bPathPlaying ? 1 : 0, eng[0], eng[1],
			s_demo.camAngles[0], s_demo.camAngles[1], s_demo.camOrigin[0], s_demo.camOrigin[1], s_demo.camOrigin[2],
			InterlockedExchange(&s_nCamMouseSeen, 0), InterlockedExchange(&s_nCamViewCalls, 0), s_flCamLastDt, (GetAsyncKeyState('W') & 0x8000) ? 1 : 0, s_demo.bMouseLayer ? 1 : 0, g_bBlockInput ? 1 : 0,
			DemoPlay_WindowFocused() ? 1 : 0, s_demo.bPaused ? 1 : 0, demo_hud.GetInt(), nDown);
		const uintptr_t pEnt = DemoCam_FollowEntity();
		DemoCamSnap_s cur = {}, fut = {};
		float abs[3] = {};
		const bool bCur = pEnt && DemoCam_ReadSnap(pEnt, kClientEntLerpCurrent, cur);
		const bool bFut = pEnt && DemoCam_ReadSnap(pEnt, kClientEntLerpFuture, fut);
		if (pEnt)
		{
			__try
			{
				for (int i = 0; i < 3; ++i)
					abs[i] = reinterpret_cast<const float*>(pEnt + kClientEntAbsOrigin)[i];
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}
		Msg(eDLL_T::ENGINE, "[DEMO-INPUT] follow=%d ent=%p abs=(%.0f %.0f %.0f) snap=%d:%.2f(%.0f %.0f %.0f) next=%d:%.2f(%.0f %.0f %.0f) "
			"anchor=(%.0f %.0f %.0f) off=(%.0f %.0f %.0f) vel=(%.0f %.0f %.0f) speed=%.0f\n",
			s_demo.bCamFollow ? 1 : 0, reinterpret_cast<void*>(pEnt), abs[0], abs[1], abs[2],
			bCur ? 1 : 0, cur.time, cur.origin[0], cur.origin[1], cur.origin[2],
			bFut ? 1 : 0, fut.time, fut.origin[0], fut.origin[1], fut.origin[2],
			s_demo.followAnchor[0], s_demo.followAnchor[1], s_demo.followAnchor[2],
			s_demo.camFollowOff[0], s_demo.camFollowOff[1], s_demo.camFollowOff[2], s_demo.camVel[0],
			s_demo.camVel[1], s_demo.camVel[2], demo_freecam_speed.GetFloat());
	}

	if (!s_demo.bFreecam)
	{
		s_demo.bPathPlaying = false;
		return;
	}

	// The path ends on its last keyframe; the camera stays there. A seek back
	// to the first key is not the end of the flight.
	if (s_demo.bPathPlaying && !DemoPlay_IsSeeking() && !s_demo.camPathMs.empty())
	{
		const bool bDone = DemoCam_PathUsesReplay()
			? static_cast<double>(DemoPlay_GetTime()) * 1000.0 >= s_demo.flPathUntilMs
			: DemoCam_PathSampleMs() >= s_demo.camPathMs.back();
		if (bDone)
			DemoCam_StopPath();
	}

	// Keys typed into the console or a menu are not camera controls.
	if (!DemoPlay_WindowFocused() || g_bBlockInput)
		return;
	const bool bCtrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
	const bool bShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

	// The wheel and middle button belong to the replay controls while their pointer is up.
	if (!s_demo.bMouseLayer && !s_demo.bPathPlaying)
	{
		if (nWheel)
		{
			const float flNotches = static_cast<float>(nWheel) / static_cast<float>(WHEEL_DELTA);
			const float flStep = demo_freecam_wheel_step.GetFloat();
			if (GetAsyncKeyState(VK_RBUTTON) & 0x8000)
				DemoCam_ScaleSpeed(flNotches);
			else if (bShift)
			{
				// Wheel forward pulls the focus in, 10% a notch.
				const float flFocus = demo_freecam_dof.GetFloat() >= 1.0f ? demo_freecam_dof.GetFloat() : 300.0f;
				demo_freecam_dof.SetValue(fminf(20000.0f, fmaxf(10.0f, flFocus * powf(1.1f, -flNotches))));
			}
			else if (bCtrl)
			{
				float flFov = DemoCam_CurrentFov();
				if (!(flFov >= kCamMinFov))
					flFov = 90.0f;
				// Wheel forward zooms in.
				demo_freecam_fov.SetValue(DemoCam_ClampFov(flFov - flNotches * flStep));
			}
			else
				demo_freecam_roll.SetValue(remainderf(demo_freecam_roll.GetFloat() + flNotches * flStep, 360.0f));
		}
		if (nPressed & (1 << 5))
		{
			if (bShift)
				demo_freecam_dof.SetValue(0.0f);
			else if (bCtrl)
				demo_freecam_fov.SetValue(0.0f);
			else
				demo_freecam_roll.SetValue(0.0f);
		}
	}

	if (!demo_campath_keys.GetBool())
		return;
	if (nPressed & (1 << 0))
		DemoPlay_CamKeyAdd();
	if (nPressed & (1 << 1))
		DemoPlay_CamKeyStep(-1);
	if (nPressed & (1 << 2))
		DemoPlay_CamKeyStep(1);
	if (nPressed & (1 << 3))
	{
		const int nIndex = DemoCam_KeyAtNow();
		if (nIndex >= 0)
			DemoPlay_CamKeyRemove(nIndex);
	}
	if (nPressed & (1 << 4))
		DemoPlay_SetCamPathPlaying(!s_demo.bPathPlaying);
	if ((nPressed & (1 << 6)) && !s_demo.bPathPlaying)
		DemoPlay_SetCamFollow(!s_demo.bCamFollow);
	if (nPressed & ((1 << 7) | (1 << 8)))
		DemoCam_ScaleSpeed(1.0f);
	if (nPressed & ((1 << 9) | (1 << 10)))
		DemoCam_ScaleSpeed(-1.0f);
}

static void __fastcall Hook_Player_RoamingView(uintptr_t pPlayer, float* pEyeOrigin, float* pEyeAngles, float* pFov)
{
	// A playing path owns this view through the rewind. ForceFullUpdate makes
	// GetLocalPlayer miss, and the stock roaming view on that call is the
	// recorded camera, so it is not run -- the path pose is written below.
	const bool bLocal = pPlayer == DemoPlay_LocalPlayer();
	const bool bPathCam = s_bDemoGate && s_demo.bPathPlaying && pEyeOrigin && pEyeAngles;
	// Stock roaming view writes the player eyes. Follow keeps the free camera
	// where it sits, so that call does not run while follow is on.
	if (v_Player_RoamingView && pPlayer && (!bPathCam || bLocal)
		&& !(s_demo.bCamFollow && !s_demo.bPathPlaying))
		v_Player_RoamingView(pPlayer, pEyeOrigin, pEyeAngles, pFov);

	if (!s_bDemoGate || !pEyeOrigin || !pEyeAngles || (!bLocal && !bPathCam))
		return;

	LARGE_INTEGER now, freq;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&freq);

	// The path still has to write the eyes if freecam was cleared for this call.
	if (!s_demo.bFreecam && !s_demo.bPathPlaying)
		return;
	if (pFov && std::isfinite(*pFov) && *pFov > 1.0f && *pFov < 179.0f)
		s_demo.flCamStockFov = *pFov;

	float eng[3] = { pEyeAngles[0], pEyeAngles[1], 0.0f };
	if (v_Engine_GetViewAngles)
		v_Engine_GetViewAngles(nullptr, eng);

	if (!s_demo.bCamInit)
	{
		if (s_demo.bCamCarry)
		{
			// A reconnect: the camera stays where it was.
			Msg(eDLL_T::ENGINE, "[DEMO] free camera kept at (%.0f %.0f %.0f) facing (%.1f %.1f)\n",
				s_demo.camOrigin[0], s_demo.camOrigin[1], s_demo.camOrigin[2], s_demo.camAngles[0], s_demo.camAngles[1]);
		}
		else
		{
			s_demo.camOrigin[0] = pEyeOrigin[0];
			s_demo.camOrigin[1] = pEyeOrigin[1];
			s_demo.camOrigin[2] = pEyeOrigin[2];
			s_demo.camAngles[0] = std::isfinite(eng[0]) ? fminf(89.0f, fmaxf(-89.0f, eng[0])) : 0.0f;
			s_demo.camAngles[1] = std::isfinite(eng[1]) ? eng[1] : 0.0f;
		}
		InterlockedExchange(&s_nCamMouseX, 0);
		InterlockedExchange(&s_nCamMouseY, 0);
		s_demo.camVel[0] = s_demo.camVel[1] = s_demo.camVel[2] = 0.0f;
		s_demo.bCamCarry = false;
		s_demo.camQpc = now;
		s_demo.bCamInit = true;
	}
	float dt = freq.QuadPart ? static_cast<float>(now.QuadPart - s_demo.camQpc.QuadPart) / static_cast<float>(freq.QuadPart) : 0.0f;
	s_demo.camQpc = now;
	if (dt < 0.0f || dt > 0.1f)
		dt = 0.0f;
	InterlockedIncrement(&s_nCamViewCalls);
	s_flCamLastDt = dt;

	// Keys on the replay are sampled from the replay clock, which already waits
	// out seeks and pauses. Only a path whose keys share a timestamp keeps its
	// own clock, and that one waits too.
	if (s_demo.bPathPlaying && !s_demo.bPaused && !DemoPlay_IsSeeking() && !DemoCam_PathUsesReplay())
		s_demo.flPathMs += static_cast<double>(dt) * 1000.0 * demo_timescale.GetFloat();
	float flPathFov = 0.0f;
	const bool bPath = s_demo.bPathPlaying
		&& DemoCam_EvalPath(DemoCam_PathSampleMs(), s_demo.camOrigin, s_demo.camAngles, &flPathFov);
	static bool s_bPathViewLogged = false;
	if (!s_demo.bPathPlaying)
		s_bPathViewLogged = false;
	else if (bPath && !s_bPathViewLogged)
	{
		s_bPathViewLogged = true;
		Msg(eDLL_T::ENGINE, "[DEMO] camera path view at %.2f s, local player %s\n",
			DemoPlay_GetTime(), bLocal ? "present" : "missing");
	}
	const float flDeg2Rad = 3.14159265358979f / 180.0f;
	// Anchor is the player's networked origin, never the roaming eye. The offset
	// is the free camera minus that origin at the moment follow starts.
	float plEye[3] = {};
	float flSnapYaw = 0.0f;
	const bool bFollow = !bPath && s_demo.bCamFollow && DemoCam_FollowPose(now, freq, plEye, &flSnapYaw);
	if (bFollow)
	{
		for (int i = 0; i < 3; ++i)
			s_demo.followAnchor[i] = plEye[i];
	}

	if (bFollow)
	{
		// Yaw-rotate only when the offset was captured in the player's yaw.
		// The capture frame leaves the camera and its angles where they sit.
		const bool bTurn = demo_freecam_follow_turn.GetBool();
		float flTarget = 0.0f;
		if (bTurn)
		{
			float rec[3] = {};
			flTarget = (s_demo.nViewPov < 0 && DemoPlay_RecordedView(rec) && std::isfinite(rec[1])) ? rec[1] : flSnapYaw;
			if (!std::isfinite(flTarget))
				flTarget = 0.0f;
		}
		if (!s_demo.bFollowYawInit)
		{
			const float d[3] = { s_demo.camOrigin[0] - plEye[0], s_demo.camOrigin[1] - plEye[1], s_demo.camOrigin[2] - plEye[2] };
			if (bTurn)
			{
				const float sy = sinf(-flTarget * flDeg2Rad), cy = cosf(-flTarget * flDeg2Rad);
				s_demo.camFollowOff[0] = d[0] * cy - d[1] * sy;
				s_demo.camFollowOff[1] = d[0] * sy + d[1] * cy;
				s_demo.flFollowRelYaw = remainderf(s_demo.camAngles[1] - flTarget, 360.0f);
			}
			else
			{
				s_demo.camFollowOff[0] = d[0];
				s_demo.camFollowOff[1] = d[1];
			}
			s_demo.camFollowOff[2] = d[2];
			s_demo.flFollowYaw = flTarget;
			s_demo.bFollowYawInit = true;
			Msg(eDLL_T::ENGINE, "[DEMO] following from (%.0f %.0f %.0f), offset (%.0f %.0f %.0f)\n",
				s_demo.camOrigin[0], s_demo.camOrigin[1], s_demo.camOrigin[2],
				s_demo.camFollowOff[0], s_demo.camFollowOff[1], s_demo.camFollowOff[2]);
		}
		else if (bTurn)
		{
			const float k = demo_freecam_follow_smooth.GetFloat();
			const float a = k > 0.0f ? 1.0f - expf(-k * dt) : 1.0f;
			const float dYaw = remainderf(flTarget - s_demo.flFollowYaw, 360.0f);
			s_demo.flFollowYaw = remainderf(s_demo.flFollowYaw + (std::isfinite(dYaw) ? dYaw : 0.0f) * a, 360.0f);
			const float sy = sinf(s_demo.flFollowYaw * flDeg2Rad), cy = cosf(s_demo.flFollowYaw * flDeg2Rad);
			s_demo.camOrigin[0] = plEye[0] + s_demo.camFollowOff[0] * cy - s_demo.camFollowOff[1] * sy;
			s_demo.camOrigin[1] = plEye[1] + s_demo.camFollowOff[0] * sy + s_demo.camFollowOff[1] * cy;
			s_demo.camOrigin[2] = plEye[2] + s_demo.camFollowOff[2];
			s_demo.camAngles[1] = remainderf(s_demo.flFollowYaw + s_demo.flFollowRelYaw, 360.0f);
		}
		else
		{
			s_demo.camOrigin[0] = plEye[0] + s_demo.camFollowOff[0];
			s_demo.camOrigin[1] = plEye[1] + s_demo.camFollowOff[1];
			s_demo.camOrigin[2] = plEye[2] + s_demo.camFollowOff[2];
		}
	}

	// The free camera owns its angles and reads the raw mouse itself: playback
	// does not feed the mouse into the engine's view angles.
	DemoCam_ApplyMouseLook(!bPath && DemoPlay_WindowFocused() && !s_demo.bMouseLayer && !g_bBlockInput);
	DemoCam_WriteEngineAngles(s_demo.camAngles);
	if (!bPath)
	{
		s_demo.camAngles[2] = demo_freecam_roll.GetFloat();

		const float sp = sinf(s_demo.camAngles[0] * flDeg2Rad), cp = cosf(s_demo.camAngles[0] * flDeg2Rad);
		const float sy = sinf(s_demo.camAngles[1] * flDeg2Rad), cy = cosf(s_demo.camAngles[1] * flDeg2Rad);
		const float fwd[3] = { cp * cy, cp * sy, -sp };
		const float right[3] = { sy, -cy, 0.0f };

		float f = 0.0f, s = 0.0f, u = 0.0f;
		float speed = demo_freecam_speed.GetFloat();
		if (DemoPlay_WindowFocused() && !g_bBlockInput)
		{
			if (GetAsyncKeyState('W') & 0x8000) f += 1.0f;
			if (GetAsyncKeyState('S') & 0x8000) f -= 1.0f;
			if (GetAsyncKeyState('D') & 0x8000) s += 1.0f;
			if (GetAsyncKeyState('A') & 0x8000) s -= 1.0f;
			if (GetAsyncKeyState('E') & 0x8000) u += 1.0f;
			if (GetAsyncKeyState('Q') & 0x8000) u -= 1.0f;
			if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
				speed *= 3.0f;
			if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
				speed *= 0.3f;
		}

		// The camera eases toward the wished velocity instead of jumping to it.
		float wish[3];
		for (int i = 0; i < 3; ++i)
			wish[i] = fwd[i] * f + right[i] * s + (i == 2 ? u : 0.0f);
		const float flLen = sqrtf(wish[0] * wish[0] + wish[1] * wish[1] + wish[2] * wish[2]);
		const float flScale = flLen > 1.0f ? speed / flLen : speed;
		const float k = demo_freecam_accel.GetFloat();
		const float a = k > 0.0f ? 1.0f - expf(-k * dt) : 1.0f;
		for (int i = 0; i < 3; ++i)
		{
			float& vel = s_demo.camVel[i];
			vel += (wish[i] * flScale - vel) * a;
			if (!std::isfinite(vel) || fabsf(vel) < 0.01f)
				vel = 0.0f;
			float v = s_demo.camOrigin[i] + vel * dt;
			if (v > 65535.0f) v = 65535.0f;
			if (v < -65535.0f) v = -65535.0f;
			s_demo.camOrigin[i] = v;
		}

		// Look and movement change the framing. Keep that delta; never zero the offset.
		if (bFollow)
		{
			const float d[3] = { s_demo.camOrigin[0] - plEye[0], s_demo.camOrigin[1] - plEye[1], s_demo.camOrigin[2] - plEye[2] };
			if (demo_freecam_follow_turn.GetBool())
			{
				const float fy = sinf(-s_demo.flFollowYaw * flDeg2Rad), fc = cosf(-s_demo.flFollowYaw * flDeg2Rad);
				s_demo.camFollowOff[0] = d[0] * fc - d[1] * fy;
				s_demo.camFollowOff[1] = d[0] * fy + d[1] * fc;
				s_demo.flFollowRelYaw = remainderf(s_demo.camAngles[1] - s_demo.flFollowYaw, 360.0f);
			}
			else
			{
				s_demo.camFollowOff[0] = d[0];
				s_demo.camFollowOff[1] = d[1];
			}
			s_demo.camFollowOff[2] = d[2];
		}
	}
	else
		s_demo.camVel[0] = s_demo.camVel[1] = s_demo.camVel[2] = 0.0f;

	pEyeOrigin[0] = s_demo.camOrigin[0];
	pEyeOrigin[1] = s_demo.camOrigin[1];
	pEyeOrigin[2] = s_demo.camOrigin[2];
	pEyeAngles[0] = s_demo.camAngles[0];
	pEyeAngles[1] = s_demo.camAngles[1];
	pEyeAngles[2] = s_demo.camAngles[2];

	const float flFov = bPath ? flPathFov : demo_freecam_fov.GetFloat();
	if (pFov && flFov >= kCamMinFov)
		*pFov = DemoCam_ClampFov(flFov);
}

// The stock view lets a scripted camera (traversal and portal animations) win
// over the observer mode; the replay's roaming cameras must win instead.
static void __fastcall Hook_Player_CalcView(uintptr_t pPlayer, float* pEyeOrigin, float* pEyeAngles, float* pFov)
{
	// The path camera stays on screen while the world rewinds, even when
	// ForceFullUpdate has dropped the local player and this would otherwise
	// fall through to the recorded view.
	const bool bPathCam = s_bDemoGate && s_demo.state == DemoPlayState_t::PLAYING
		&& s_demo.bFreecam && s_demo.bPathPlaying && pEyeOrigin && pEyeAngles;
	if (bPathCam || (s_bDemoGate && s_demo.state == DemoPlayState_t::PLAYING && DemoPlay_UsesRoaming()
		&& v_Player_RoamingView && pPlayer && pPlayer == DemoPlay_LocalPlayer()))
	{
		Hook_Player_RoamingView(pPlayer, pEyeOrigin, pEyeAngles, pFov);
		return;
	}
	v_Player_CalcView(pPlayer, pEyeOrigin, pEyeAngles, pFov);
#if defined(SDK_WIP)
	if (pPlayer && pPlayer == DemoPlay_LocalPlayer())
		CubemapCapture_OverrideView(pEyeOrigin, pEyeAngles, pFov);
#endif // SDK_WIP
}

// The rendered view. After the player view it copies a saved camera back over
// the origin and angles, so path and follow poses written into the player view
// never reach the screen. Own the last write when either is active.
static constexpr ptrdiff_t kBuiltViewOrigin = 0xE0;
static constexpr ptrdiff_t kBuiltViewAngles = 0xEC;

static void __fastcall Hook_View_Build(void* pView)
{
	if (v_View_Build)
		v_View_Build(pView);

	float pos[3], ang[3];
#if defined(SDK_WIP)
	const bool bCapture = pView && CubemapCapture_CurrentView(pos, ang);
#else
	const bool bCapture = false;
#endif // SDK_WIP
	if (!pView || (!bCapture && (!s_bDemoGate || s_demo.state != DemoPlayState_t::PLAYING)))
		return;

	const bool bPath = !bCapture && s_demo.bPathPlaying;
	if (bPath)
	{
		float flFov = 0.0f;
		if (!DemoCam_EvalPath(DemoCam_PathSampleMs(), pos, ang, &flFov))
			return;
	}
	else if (!bCapture && s_demo.bFreecam && s_demo.bCamFollow && s_demo.bCamInit)
	{
		// Last write: the free camera where it sits, not the player eyes.
		for (int i = 0; i < 3; ++i)
		{
			pos[i] = s_demo.camOrigin[i];
			ang[i] = s_demo.camAngles[i];
		}
	}
	else if (!bCapture)
		return;

	__try
	{
		float* const pOrg = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(pView) + kBuiltViewOrigin);
		float* const pAng = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(pView) + kBuiltViewAngles);
		for (int i = 0; i < 3; ++i)
		{
			pOrg[i] = pos[i];
			pAng[i] = ang[i];
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return;
	}

	if (g_pBuiltCamOrigin)
	{
		for (int i = 0; i < 3; ++i)
		{
			g_pBuiltCamOrigin[i] = pos[i];
			g_pBuiltCamOrigin[3 + i] = pos[i];
			g_pBuiltCamOrigin[6 + i] = ang[i];
		}
	}
	// The capture only places the rendered camera; the player keeps its own angles.
	if (bCapture)
		return;

	if (bPath)
	{
		for (int i = 0; i < 3; ++i)
		{
			s_demo.camOrigin[i] = pos[i];
			s_demo.camAngles[i] = ang[i];
		}
	}
	DemoCam_WriteEngineAngles(ang);

	static bool s_bPathLogged = false;
	static bool s_bFollowLogged = false;
	if (bPath)
	{
		if (!s_bPathLogged)
		{
			s_bPathLogged = true;
			Msg(eDLL_T::ENGINE, "[DEMO] camera path owns the rendered view at %.2f s (%.0f %.0f %.0f)\n",
				DemoPlay_GetTime(), pos[0], pos[1], pos[2]);
		}
	}
	else if (!s_bFollowLogged)
	{
		s_bFollowLogged = true;
		Msg(eDLL_T::ENGINE, "[DEMO] follow owns the rendered view at %.2f s (%.0f %.0f %.0f)\n",
			DemoPlay_GetTime(), pos[0], pos[1], pos[2]);
	}
}

//-----------------------------------------------------------------------------
// Console
//-----------------------------------------------------------------------------
static void DemoPlay_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		Msg(eDLL_T::ENGINE, "Usage: demo_play <name> [pov] [seconds]\n");
		return;
	}
	const int nPov = args.ArgC() >= 3 ? atoi(args.Arg(2)) : 0;
	const float flSeek = args.ArgC() >= 4 ? static_cast<float>(atof(args.Arg(3))) : -1.0f;
	DemoPlay_Start(args.Arg(1), nPov, flSeek);
}
static ConCommand demo_play("demo_play", DemoPlay_f,
	"Play a demo from platform/demos. Usage: demo_play <name> [pov] [seconds]", FCVAR_RELEASE);

static void DemoPause_f(const CCommand& args)
{
	if (args.ArgC() >= 2)
		DemoPlay_SetPaused(atoi(args.Arg(1)) != 0);
	else
		DemoPlay_SetPaused(!DemoPlay_IsPaused());
}
static ConCommand demo_pause("demo_pause", DemoPause_f,
	"Toggle demo pause, or demo_pause 0|1.", FCVAR_RELEASE);

static void DemoStep_f(const CCommand& args)
{
	NOTE_UNUSED(args);
	DemoPlay_Step();
}
static ConCommand demo_step("demo_step", DemoStep_f,
	"Pause the demo and advance it by one packet.", FCVAR_RELEASE);

static void DemoSeek_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		Msg(eDLL_T::ENGINE, "Usage: demo_seek <seconds>  (now %.2f of %.2f)\n", DemoPlay_GetTime(), DemoPlay_GetDuration());
		return;
	}
	const char* const psz = args.Arg(1);
	float fl = static_cast<float>(atof(psz));
	if (psz[0] == '+' || psz[0] == '-')
		fl += DemoPlay_GetTime();
	DemoPlay_Seek(fl);
}
static ConCommand demo_seek("demo_seek", DemoSeek_f,
	"Seek the demo. Usage: demo_seek <seconds> | +n | -n", FCVAR_RELEASE);

static void DemoPov_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		Msg(eDLL_T::ENGINE, "pov %d of %d\n", DemoPlay_GetPov(), DemoPlay_GetPovCount());
		return;
	}
	DemoPlay_SetPov(atoi(args.Arg(1)));
}
static ConCommand demo_pov("demo_pov", DemoPov_f,
	"Replay the demo from another recorded pov at the current time.", FCVAR_RELEASE);

static void DemoView_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		Msg(eDLL_T::ENGINE, "Usage: demo_view <povId|ehandle> [chase]   demo_view -1 restores\n");
		return;
	}
	DemoPlay_SetView(atoi(args.Arg(1)), args.ArgC() >= 3 && atoi(args.Arg(2)) != 0);
}
static ConCommand demo_view("demo_view", DemoView_f,
	"View another player through the observer camera while a demo plays.", FCVAR_RELEASE);

static void DemoFreecam_f(const CCommand& args)
{
	DemoPlay_SetFreecam(args.ArgC() >= 2 ? atoi(args.Arg(1)) != 0 : !DemoPlay_IsFreecam());
}
static ConCommand demo_freecam("demo_freecam", DemoFreecam_f,
	"Toggle the free camera while a demo plays (WASD, E/Q up/down, Shift faster, wheel roll, Ctrl+wheel FOV).", FCVAR_RELEASE);

static void DemoCamPath_f(const CCommand& args)
{
	const char* const pszVerb = args.ArgC() >= 2 ? args.Arg(1) : "list";
	if (!V_stricmp(pszVerb, "add"))
		DemoPlay_CamKeyAdd();
	else if (!V_stricmp(pszVerb, "remove"))
	{
		const int nIndex = args.ArgC() >= 3 ? atoi(args.Arg(2)) - 1 : DemoCam_KeyAtNow();
		if (!DemoPlay_CamKeyRemove(nIndex))
			Warning(eDLL_T::ENGINE, "[DEMO] no camera keyframe there\n");
	}
	else if (!V_stricmp(pszVerb, "clear"))
		DemoPlay_CamKeyClear();
	else if (!V_stricmp(pszVerb, "goto") && args.ArgC() >= 3)
		DemoPlay_CamKeyGoto(atoi(args.Arg(2)) - 1);
	else if (!V_stricmp(pszVerb, "next"))
		DemoPlay_CamKeyStep(1);
	else if (!V_stricmp(pszVerb, "prev"))
		DemoPlay_CamKeyStep(-1);
	else if (!V_stricmp(pszVerb, "play"))
		DemoPlay_SetCamPathPlaying(true);
	else if (!V_stricmp(pszVerb, "stop"))
		DemoPlay_SetCamPathPlaying(false);
	else if (!V_stricmp(pszVerb, "list"))
	{
		for (size_t i = 0; i < s_demo.camKeys.size(); ++i)
		{
			const DemoCamKey_s& k = s_demo.camKeys[i];
			Msg(eDLL_T::ENGINE, "  %2zu  %8.2f s  pos (%.0f %.0f %.0f)  ang (%.1f %.1f %.1f)  fov %.1f  dof %.0f\n", i + 1,
				k.tMs / 1000.0, k.pos[0], k.pos[1], k.pos[2], k.ang[0], k.ang[1], k.ang[2], k.fov, k.dof);
		}
		Msg(eDLL_T::ENGINE, "%zu camera keyframe(s)%s\n", s_demo.camKeys.size(), s_demo.bPathPlaying ? ", path playing" : "");
	}
	else
		Msg(eDLL_T::ENGINE, "Usage: demo_campath add | remove [n] | clear | goto <n> | next | prev | play | stop | list\n");
}
static ConCommand demo_campath("demo_campath", DemoCamPath_f,
	"Free camera path keyframes: add | remove [n] | clear | goto <n> | next | prev | play | stop | list", FCVAR_RELEASE);

static void DemoList_f(const CCommand& args)
{
	NOTE_UNUSED(args);
	std::vector<DemoFileInfo_s> files;
	Demo_ListFiles(DEMO_CLIENT_ROOT, files, 256);
	for (const DemoFileInfo_s& f : files)
		Msg(eDLL_T::ENGINE, "  %-64s %8llu KB%s\n", f.name, static_cast<unsigned long long>(f.size / 1024),
			f.bPartial ? "  (unfinished)" : "");
	Msg(eDLL_T::ENGINE, "%zu demo(s) under %s\n", files.size(), DEMO_CLIENT_ROOT);
}
static ConCommand demo_list("demo_list", DemoList_f, "List demos under platform/demos.", FCVAR_RELEASE);

static void DemoInfo_f(const CCommand& args)
{
	if (s_demo.state == DemoPlayState_t::IDLE)
	{
		Msg(eDLL_T::ENGINE, "[DEMO] idle\n");
		NOTE_UNUSED(args);
		return;
	}
	Msg(eDLL_T::ENGINE, "[DEMO] %s pov %d/%d  %.2f / %.2f s  x%.2f%s%s view=%d%s  chunk %zu/%zu\n",
		s_demo.szPath, s_demo.nPov, DemoPlay_GetPovCount(), DemoPlay_GetTime(), DemoPlay_GetDuration(),
		demo_timescale.GetFloat(), s_demo.bPaused ? " paused" : "", s_demo.bEnded ? " ended" : "",
		s_demo.nViewPov, s_demo.bFreecam ? " freecam" : "", s_demo.nCursor, s_demo.order.size());
}
static ConCommand demo_info("demo_info", DemoInfo_f, "Print the demo playback state.", FCVAR_RELEASE);

//-----------------------------------------------------------------------------
// Detours
//-----------------------------------------------------------------------------
void VDemoPlayer::GetAdr(void) const
{
	LogFunAdr("Player_RoamingView", v_Player_RoamingView);
	LogFunAdr("Player_CalcView", v_Player_CalcView);
	LogFunAdr("View_Build", v_View_Build);
	LogFunAdr("Engine_GetViewAngles", v_Engine_GetViewAngles);
	LogVarAdr("EngineViewAngles", g_pEngineViewAngles);
	LogFunAdr("CL_ForceFullUpdate", v_CL_ForceFullUpdate);
	LogFunAdr("Script_RunThreadsFrame", v_Script_RunThreadsFrame);
}

void VDemoPlayer::GetFun(void) const
{
	// C_Player roaming observer view (observer modes 4 and 7); the 0x3584 store
	// zeroes the chase distance.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 30 "
		"49 8B E9 49 8B F8 4C 8B F2 48 8B F1 E8 ?? ?? ?? ?? 48 85 C0 C7 86 84 35 00 00 00 00 00 00")
		.GetPtr(v_Player_RoamingView);

	// C_Player view: a scripted view entity first, then the observer-mode switch.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 18 55 56 57 41 54 41 56 48 81 EC 90 00 00 00 0F 29 B4 24 ?? ?? ?? ?? 49 8B E9 "
		"44 0F 29 4C 24 ?? 49 8B F8 4C 8B F2 48 8B D9 E8 ?? ?? ?? ?? F3 44 0F 10 0D ?? ?? ?? ?? 4C 8D 25")
		.GetPtr(v_Player_CalcView);
	if (!v_Player_CalcView)
		Warning(eDLL_T::CLIENT, "[DEMO] player view pattern unresolved -- scripted cameras override the free camera\n");

	// Rendered view. rcx is the view; +0xE0 origin and +0xEC angles are what the
	// player view filled, and a flag then copies the saved camera back over them.
	// The first saved-origin store is movss [rip+disp], xmm0 at +0x51C.
	Module_FindPattern(g_GameDll,
		"48 8B C4 53 56 57 48 81 EC 00 01 00 00 0F 29 70 A8 48 8D 3D ?? ?? ?? ??")
		.GetPtr(v_View_Build);
	if (v_View_Build)
	{
		const CMemory build(reinterpret_cast<uintptr_t>(v_View_Build));
		const uint8_t* const pStore = build.Offset(0x51C).RCast<const uint8_t*>();
		if (pStore && pStore[0] == 0xF3 && pStore[1] == 0x0F && pStore[2] == 0x11 && pStore[3] == 0x05)
			g_pBuiltCamOrigin = build.Offset(0x51C).ResolveRelativeAddress(4, 8).RCast<float*>();
		else
			Warning(eDLL_T::CLIENT, "[DEMO] rendered-view origin store drifted -- path pose still written on the view\n");
	}
	else
		Warning(eDLL_T::CLIENT, "[DEMO] rendered view pattern unresolved -- the saved camera paints over the path\n");

	// Copies the active split-screen client state's view angles (stride 0x41358).
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48 8B DA FF 50 28 48 63 C8 "
		"48 8D 05 ?? ?? ?? ?? 4C 69 C1 ?? ?? ?? ?? 4C 03 C0 41 8B 00 89 03 41 8B 40 04 89 43 04")
		.GetPtr(v_Engine_GetViewAngles);

	// cl_fullupdate callback: resets the delta tick so the next snapshot is NoDelta.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 8D 3D ?? ?? ?? ?? 48 8B CF FF 50 28 "
		"48 63 C8 48 69 D9 ?? ?? ?? ?? 83 BC 3B ?? ?? ?? ?? FF 74 ?? E8 ?? ?? ?? ?? C6 84 3B")
		.GetPtr(v_CL_ForceFullUpdate);

	if (!v_Player_RoamingView)
		Warning(eDLL_T::CLIENT, "[DEMO] roaming view pattern unresolved -- demo_freecam disabled\n");
	if (v_Engine_GetViewAngles)
	{
		// lea rcx, <split-screen manager> at +0x0D; lea rax, <view angles> at +0x1D.
		const CMemory getter(reinterpret_cast<uintptr_t>(v_Engine_GetViewAngles));
		g_pSplitScreenMgr = getter.Offset(0x0D).ResolveRelativeAddress(3, 7).RCast<void*>();
		g_pEngineViewAngles = getter.Offset(0x1D).ResolveRelativeAddress(3, 7).RCast<float*>();
	}
	else
		Warning(eDLL_T::CLIENT, "[DEMO] view angle getter unresolved -- free camera follows the pov's angles, replays follow the mouse\n");
	if (!v_CL_ForceFullUpdate)
		Warning(eDLL_T::CLIENT, "[DEMO] full-update pattern unresolved -- rewinds reconnect, mid-match records wait\n");

	// Script thread scheduler: stores the VM's wait clock (+0x14) and wakes
	// timed waits against it.
	Module_FindPattern(g_GameDll,
		"48 8B C4 56 57 41 56 41 57 48 83 EC 48 48 89 68 10 48 8B F1 4C 89 60 18 4C 89 68 D8 4C 8B 69 30 "
		"0F 29 70 C8 0F 28 F1 49 8B 45 50 0F B6 90 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 8B BC D0 ?? ?? ?? ?? C6 47 18 01")
		.GetPtr(v_Script_RunThreadsFrame);
	if (!v_Script_RunThreadsFrame)
		Warning(eDLL_T::CLIENT, "[DEMO] script thread pattern unresolved -- client script waits stall after a rewind\n");

	DemoRecord_SetForceFullUpdateFn(v_CL_ForceFullUpdate);
}

void VDemoPlayer::Detour(const bool bAttach) const
{
	if (v_Player_RoamingView)
		DetourSetup(&v_Player_RoamingView, &Hook_Player_RoamingView, bAttach);
	if (v_Player_CalcView)
		DetourSetup(&v_Player_CalcView, &Hook_Player_CalcView, bAttach);
	if (v_View_Build)
		DetourSetup(&v_View_Build, &Hook_View_Build, bAttach);
	if (v_Engine_GetViewAngles)
		DetourSetup(&v_Engine_GetViewAngles, &Hook_Engine_GetViewAngles, bAttach);
	if (v_Script_RunThreadsFrame)
		DetourSetup(&v_Script_RunThreadsFrame, &Hook_Script_RunThreadsFrame, bAttach);
}
