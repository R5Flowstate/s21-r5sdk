#pragma once
#ifndef DEDICATED

struct IDXGISwapChain;

void FrameGen_OnEnginePresent(void* pFrameCtx, IDXGISwapChain** ppSwapChain);
void FrameGen_Shutdown(void);

#endif // !DEDICATED
