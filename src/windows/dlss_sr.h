#pragma once
#ifndef DEDICATED

void DlssSr_TryInstall(void);
void DlssSr_Shutdown(void);
void DlssSr_PresentNrPass(void* swapChain);

bool DlssSr_Installed(void);
bool DlssSr_SawContext(void);
bool DlssSr_SawDispatch(void);
bool DlssSr_Live(void);
bool DlssSr_TsaaHooked(void);
unsigned long long DlssSr_TsaaCount(void);
unsigned long long DlssSr_EvalOkCount(void);
unsigned long long DlssSr_EvalFailCount(void);
unsigned long long DlssSr_SkippedViews(void);
const char* DlssSr_LastFail(void);
unsigned DlssSr_RenderWidth(void);
unsigned DlssSr_RenderHeight(void);
unsigned DlssSr_DisplayWidth(void);
unsigned DlssSr_DisplayHeight(void);
unsigned DlssSr_ContextFlags(void);
void DlssSr_DumpStatus(void);

#endif // !DEDICATED
