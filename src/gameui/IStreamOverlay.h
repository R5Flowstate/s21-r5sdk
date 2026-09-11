#if defined(CLIENT_DLL)
#pragma once
#include "imgui_surface.h"

class CStreamOverlay : public CImguiSurface
{
public:
	CStreamOverlay(void);
	~CStreamOverlay(void);

	virtual bool Init(void);
	virtual void Shutdown(void);

	virtual void RunFrame(void);
	virtual bool DrawSurface(void);

	// Read-only display: never capture input even when auto-activated.
	virtual bool IsModal() const override { return false; }

	void UpdateWindowAvailability(void);

	bool ResizeScratchBuffer(const size_t newSize);
	void FreeScratchBuffer(void);

	void RenderToConsole(const char* const mode);

	// Command callbacks.
	static void DumpStreamInfo_f(const CCommand& args);

private:
	char* m_scratchBuffer;
	size_t m_scratchBufferSize;
	size_t m_currentTextLength;

	u8 m_numTicksSinceUpdate;
	bool m_lastAvailability;
};

extern CStreamOverlay g_streamOverlay;
#else // !CLIENT_DLL
#pragma once
#include "imgui_surface.h"

class CStreamOverlay : public CImguiSurface
{
public:
	CStreamOverlay(void);
	~CStreamOverlay(void);

	virtual bool Init(void);
	virtual void Shutdown(void);

	virtual void RunFrame(void);
	virtual bool DrawSurface(void);

	void UpdateWindowAvailability(void);

	bool ResizeScratchBuffer(const size_t newSize);
	void FreeScratchBuffer(void);

	void RenderToConsole(const char* const mode);

	// Command callbacks.
	static void DumpStreamInfo_f(const CCommand& args);

private:
	char* m_scratchBuffer;
	size_t m_scratchBufferSize;
	size_t m_currentTextLength;

	u8 m_numTicksSinceUpdate;
	bool m_lastAvailability;
};

extern CStreamOverlay g_streamOverlay;
#endif // CLIENT_DLL
