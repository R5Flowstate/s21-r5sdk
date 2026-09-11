//=============================================================================//
// 
// Purpose: Dear ImGui engine implementation
// 
//=============================================================================//

#include "imgui/misc/imgui_snapshot.h"
#include "engine/sys_mainwind.h"
#include "windows/id3dx.h"

#include "tier1/keyvalues.h"
#include "filesystem/filesystem.h"
#include "public/vstdlib/ikeyvaluessystem.h"
#include "imgui_system.h"

//-----------------------------------------------------------------------------
// Constructors/Destructors.
//-----------------------------------------------------------------------------
CImguiSystem::CImguiSystem()
	: m_enabled(false)
	, m_initialized(false)
	, m_hasNewFrame(false)
	, m_repeatFrame(false)
{
}

//-----------------------------------------------------------------------------
// Overlay window: DX12 uses the swapchain output, which does not exist at DirectX_Init.
//-----------------------------------------------------------------------------
extern void SDK_Log(const char* fmt, ...);

bool CImguiSystem::Init(const HWND hWnd)
{
	if (!hWnd)
		return false;

	IMGUI_CHECKVERSION();
	ImGuiContext* const context = ImGui::CreateContext();

	if (!context)
	{
		SDK_Log("ImguiSystem::Init: CreateContext FAILED\n");
		m_enabled = false;
		return false;
	}

	AUTO_LOCK(m_snapshotBufferMutex);
	AUTO_LOCK(m_inputEventQueueMutex);

	context->ConfigNavWindowingKeyNext = 0;
	context->ConfigNavWindowingKeyPrev = 0;

	ImGuiViewport* const vp = ImGui::GetMainViewport();
	vp->PlatformHandleRaw = hWnd;

	SetupIO();

	if (!ImGui_ImplWin32_Init(hWnd))
	{
		SDK_Log("ImguiSystem::Init: Win32 init FAILED\n");
		ImGui::DestroyContext();
		m_enabled = false;
		return false;
	}

	// DX12 builds its renderer backend from the game swapchain on the first
	// frame that reaches the overlay, see Dx12_InitRenderState.
	if (!DirectX_IsDx12Mode() && !ImGui_ImplDX11_Init(D3D11Device(), D3D11DeviceContext()))
	{
		SDK_Log("ImguiSystem::Init: DX11 init FAILED\n");
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
		m_enabled = false;
		return false;
	}

	m_initialized = true;
	m_hasNewFrame = false;

	SDK_Log("ImguiSystem::Init: SUCCESS (hwnd=%p)\n", (void*)hWnd);
	return true;
}

//-----------------------------------------------------------------------------
// Shuts the imgui system down, frees all allocated buffers.
//-----------------------------------------------------------------------------
void CImguiSystem::Shutdown()
{
	Assert(ThreadInMainThread(), "CImguiSystem::Shutdown() should only be called from the main thread!");
	Assert(IsInitialized(), "CImguiSystem::Shutdown() called recursively?");

	Assert(IsEnabled(), "CImguiSystem::Shutdown() called while system was disabled!");

	AUTO_LOCK(m_snapshotBufferMutex);
	AUTO_LOCK(m_inputEventQueueMutex);

	if (!DirectX_IsDx12Mode())
		ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();

	ImGui::DestroyContext();
	m_snapshotData.Clear();

	m_initialized = false;
	m_hasNewFrame = false;

	m_surfaceList.Purge();
}

//-----------------------------------------------------------------------------
// Sets the imgui system IO up.
//-----------------------------------------------------------------------------
void CImguiSystem::SetupIO() const
{
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_IsSRGB;
	SetupFonts();
}

//-----------------------------------------------------------------------------
// Retrieves glyph range by name
//-----------------------------------------------------------------------------
static const ImWchar* ImguiSystem_GetGlyphRangeForName(const char* const range, ImGuiIO& io)
{
	if (V_strcmp(range, "latin") == 0)
		return io.Fonts->GetGlyphRangesDefault();
	if (V_strcmp(range, "greek") == 0)
		return io.Fonts->GetGlyphRangesGreek();
	if (V_strcmp(range, "korean") == 0)
		return io.Fonts->GetGlyphRangesKorean();
	if (V_strcmp(range, "japanese") == 0)
		return io.Fonts->GetGlyphRangesJapanese();
	if (V_strcmp(range, "tchinese") == 0)
		return io.Fonts->GetGlyphRangesChineseFull();
	if (V_strcmp(range, "schinese") == 0)
		return io.Fonts->GetGlyphRangesChineseSimplifiedCommon();
	if (V_strcmp(range, "cyrillic") == 0)
		return io.Fonts->GetGlyphRangesCyrillic();
	if (V_strcmp(range, "thai") == 0)
		return io.Fonts->GetGlyphRangesThai();
	if (V_strcmp(range, "vietnamese") == 0)
		return io.Fonts->GetGlyphRangesVietnamese();

	return nullptr;
}

//-----------------------------------------------------------------------------
// Parses the font configuration and sets it up for the imgui system
//-----------------------------------------------------------------------------
void CImguiSystem::SetupFonts() const
{
	// Use BaseFileSystem (+8 secondary base). Implicit CFileSystem_Stdio* cast is the wrong thunk.

	if (!BaseFileSystem() || !KeyValuesSystem())
	{
		SDK_Log("  SetupFonts: missing globals, using default font\n");
		return;
	}

	static const char* const configFilePath = "resource/imgui_fonts.txt";
	KeyValues configKV("ImguiFonts");

	// Load via IBaseFileSystem Open/Read/Size/Close. LoadFromFile uses IFileSystem slots that do not match.
	IBaseFileSystem* const pFS = BaseFileSystem();
	FileHandle_t fh = pFS->Open(configFilePath, "rb", "GAME");
	if (!fh)
	{
		SDK_Log("  SetupFonts: config not found, using default font\n");
		return;
	}

	const ssize_t fileSize = pFS->Size(fh);
	if (fileSize <= 0)
	{
		pFS->Close(fh);
		SDK_Log("  SetupFonts: config empty, using default font\n");
		return;
	}

	char* pBuf = new char[fileSize + 2];
	pFS->Read(pBuf, fileSize, fh);
	pFS->Close(fh);
	pBuf[fileSize] = '\0';
	pBuf[fileSize + 1] = '\0';

	if (!configKV.LoadFromBuffer(configFilePath, pBuf, nullptr, nullptr))
	{
		delete[] pBuf;
		SDK_Log("  SetupFonts: parse failed, using default font\n");
		return;
	}
	delete[] pBuf;

	ImVector<ImWchar> rangesArray[IMGUI_SYSTEM_MAX_FONTS];
	ImGuiIO& io = ImGui::GetIO();

	int i = 0;
	bool ranFirst = false;

	for (KeyValues* pSubKey = configKV.GetFirstSubKey(); pSubKey != nullptr; pSubKey = pSubKey->GetNextKey())
	{
		if (i >= IMGUI_SYSTEM_MAX_FONTS)
			break;

		const char* const fontFileName = pSubKey->GetName();
		const float fontSizePixels = pSubKey->GetFloat("size", 13);

		ImVector<ImWchar>& ranges = rangesArray[i++]; bool hasRange = false;
		KeyValues* const rangesKV = pSubKey->FindKey("ranges");

		if (rangesKV)
		{
			ImFontGlyphRangesBuilder builder;

			for (KeyValues* pSubRange = rangesKV->GetFirstSubKey(); pSubRange != nullptr; pSubRange = pSubRange->GetNextKey())
			{
				const char* const rangeName = pSubRange->GetString();
				const ImWchar* const range = ImguiSystem_GetGlyphRangeForName(rangeName, io);

				if (range)
				{
					builder.AddRanges(range);
					hasRange = true;
				}
			}

			if (hasRange)
				builder.BuildRanges(&ranges);
		}

		LoadFont(fontFileName, ranFirst, fontSizePixels, hasRange ? ranges.Data : nullptr);
		ranFirst = true;
	}

	io.Fonts->Build();
}

//-----------------------------------------------------------------------------
// Loads the font to be used for the imgui system.
//-----------------------------------------------------------------------------
void CImguiSystem::LoadFont(const char* const fontPath, const bool mergeMode, const float sizePixels, ImWchar* const ranges) const
{
	// S21: Use only IBaseFileSystem methods (Open/Read/Size/Close) -- the IFileSystem
	// vtable indices differ between S3 and S21, so any IFileSystem-specific calls crash.
	IBaseFileSystem* const pFS = BaseFileSystem();
	if (!pFS)
		return;

	FileHandle_t fontFile = pFS->Open(fontPath, "rb", "GAME");
	if (!fontFile)
	{
		SDK_Log("  LoadFont: '%s' not found\n", fontPath);
		return;
	}

	const ssize_t fontSize = pFS->Size(fontFile);
	if (fontSize <= 100)
	{
		pFS->Close(fontFile);
		SDK_Log("  LoadFont: '%s' too small (%zd bytes)\n", fontPath, fontSize);
		return;
	}

	// NOTE: shouldn't be deleted! Dear ImGui needs it internally.
	u8* const fontBuf = new u8[fontSize];
	pFS->Read(fontBuf, fontSize, fontFile);
	pFS->Close(fontFile);
	SDK_Log("  LoadFont: '%s' loaded (%zd bytes)\n", fontPath, fontSize);

	ImFontConfig config;
	config.MergeMode = mergeMode;

	ImGuiIO& io = ImGui::GetIO();
	io.Fonts->AddFontFromMemoryTTF(fontBuf, (int)fontSize, sizePixels, &config, ranges);
}

//-----------------------------------------------------------------------------
// Add an imgui surface.
//-----------------------------------------------------------------------------
void CImguiSystem::AddSurface(CImguiSurface* const surface)
{
	Assert(IsInitialized());

	// A duplicate entry draws the window twice per frame, which trips
	// ImGui's "visible items with conflicting ID" error tooltip.
	if (m_surfaceList.Find(surface) != -1)
	{
		Warning(eDLL_T::MS, "[IMGUI] surface %p added twice -- ignoring\n",
			reinterpret_cast<void*>(surface));
		return;
	}

	m_surfaceList.AddToTail(surface);
}

//-----------------------------------------------------------------------------
// Remove an imgui surface.
//-----------------------------------------------------------------------------
void CImguiSystem::RemoveSurface(CImguiSurface* const surface)
{
	Assert(!IsInitialized());
	m_surfaceList.FindAndRemove(surface);
}

//-----------------------------------------------------------------------------
// Draws the ImGui panels and applies all queued input events.
//-----------------------------------------------------------------------------
void CImguiSystem::SampleFrame()
{
	Assert(ThreadInMainThread(), "CImguiSystem::SampleFrame() should only be called from the main thread!");
	Assert(IsInitialized());

	AUTO_LOCK(m_inputEventQueueMutex);

	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();

	ImGui::NewFrame();

	FOR_EACH_VEC(m_surfaceList, i)
	{
		CImguiSurface* const surface = m_surfaceList[i];
		surface->RunFrame();
	}

	ImGui::EndFrame();
	ImGui::Render();
}

//-----------------------------------------------------------------------------
// DrawSurfaces: SEH per surface so one crash cannot tear the overlay down.
//-----------------------------------------------------------------------------
static volatile unsigned int s_crashedSurfaceMask = 0;

// SEH helper; split out because __try cannot coexist with AUTO_LOCK (C2712).
static bool DrawOneSurfaceGuarded(CImguiSurface* const surface, const int index)
{
	__try
	{
		surface->RunFrame();
		return true;
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
		extern void SDK_Log(const char* fmt, ...);
		SDK_Log("[IMGUI-RT] surface[%d] RunFrame CRASHED (0x%08X) - permanently disabled\n",
			index, GetExceptionCode());
		return false;
	}
}

void CImguiSystem::DrawSurfaces()
{
	// Explicit Lock: DrawOneSurfaceGuarded uses SEH (no AUTO_LOCK, C2712).
	m_inputEventQueueMutex.Lock();

	bool deactivatedAny = false;

	FOR_EACH_VEC(m_surfaceList, i)
	{
		// Skip surfaces we have already seen crash - repeatedly crashing
		// them every frame would flood the log and keep triggering the
		// engine VEH/apex_crash.txt writer even though our SEH recovers.
		const unsigned int bit = 1u << (i & 31);
		if (s_crashedSurfaceMask & bit)
			continue;

		if (!DrawOneSurfaceGuarded(m_surfaceList[i], i))
		{
			s_crashedSurfaceMask |= bit;
			// Half-dead: draw is permanently skipped but m_activated would
			// still latch IsSurfaceActive / g_bBlockInput (invisible modal
			// that unclips multi-mon cursor and blocks game input).
			if (m_surfaceList[i]->IsActivated())
			{
				m_surfaceList[i]->SetActive(false);
				deactivatedAny = true;
			}
		}
	}

	m_inputEventQueueMutex.Unlock();

	if (deactivatedAny)
		ResetInput();
}

//-----------------------------------------------------------------------------
// Purpose: clear modal activation after Present/SEH kills the overlay.
//-----------------------------------------------------------------------------
void CImguiSystem::ForceDeactivateModals()
{
	bool any = false;
	FOR_EACH_VEC(m_surfaceList, i)
	{
		CImguiSurface* const surface = m_surfaceList[i];
		if (!surface)
			continue;
		if (surface->IsModal() && surface->IsActivated())
		{
			surface->SetActive(false);
			any = true;
		}
	}
	if (any)
		ResetInput();
}

//-----------------------------------------------------------------------------
// Copies currently drawn data into the snapshot buffer which is queued to be
// rendered in the render thread. This should only be called from the same
// thread SampleFrame is being called from.
//-----------------------------------------------------------------------------
void CImguiSystem::SwapBuffers()
{
	Assert(ThreadInMainThread(), "CImguiSystem::SwapBuffers() should only be called from the main thread!");
	Assert(IsInitialized());

	ImDrawData* const drawData = ImGui::GetDrawData();
	Assert(drawData);

	// Nothing has been drawn, nothing to swap.
	if (!drawData->CmdListsCount)
		return;

	AUTO_LOCK(m_snapshotBufferMutex);

	m_snapshotData.SnapUsingSwap(drawData, ImGui::GetTime());

	m_hasNewFrame = true;
	m_repeatFrame = true;
}

//-----------------------------------------------------------------------------
// Renders the drawn frame out which has been swapped to the snapshot buffer.
//-----------------------------------------------------------------------------
void CImguiSystem::RenderFrame()
{
	Assert(IsInitialized());

	if (!m_hasNewFrame.exchange(false) && !m_repeatFrame.exchange(false))
		return;

	AUTO_LOCK(m_snapshotBufferMutex);
	ImGui_ImplDX11_RenderDrawData(&m_snapshotData.DrawData);
}

//-----------------------------------------------------------------------------
// Checks whether we have an active surface.
//-----------------------------------------------------------------------------
bool CImguiSystem::IsSurfaceActive() const
{
	// Only modal surfaces (Console/Browser/DevMenu/TopBar) should block game
	// input. Non-modal HUD overlays (ParticleOverlay/StreamOverlay) auto-
	// activate from ConVars and would otherwise silently route WASD to imgui.
	FOR_EACH_VEC(m_surfaceList, i)
	{
		if (m_surfaceList[i]->IsActivated() && m_surfaceList[i]->IsModal())
			return true;
	}

	return false;
}

//-----------------------------------------------------------------------------
// Window procedure handler.
//-----------------------------------------------------------------------------
LRESULT CImguiSystem::MessageHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (!ImguiSystem()->IsInitialized())
		return NULL;

	AUTO_LOCK(ImguiSystem()->m_inputEventQueueMutex);

	extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
}

static CImguiSystem s_imguiSystem;

CImguiSystem* ImguiSystem()
{
	return &s_imguiSystem;
}
