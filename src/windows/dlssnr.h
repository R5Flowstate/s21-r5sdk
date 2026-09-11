#pragma once
#ifndef DEDICATED

void DlssNr_OnEnginePresent(void);
void DlssNr_Shutdown(void);

bool DlssNr_DllPresent(void);
bool DlssNr_IsNvidia(void);
bool DlssNr_VendorKnown(void);
// nvngx_dlss.dll next to the exe -- the SuperSampling runtime.
bool DlssNr_SrRuntimePresent(void);
// Probes the adapter if needed. True only where DLSS can actually run: an
// NVIDIA adapter with an NGX runtime beside the exe. Fails open while the
// adapter is unprobed so a first frame still installs on NVIDIA.
bool DlssNr_DlssStackAllowed(void);
bool DlssNr_IsHooked(void);
bool DlssNr_FeatureLive(void);
bool DlssNr_Latched(void);
bool DlssNr_Running(void);
const char* DlssNr_HookText(void);
unsigned DlssNr_Width(void);
unsigned DlssNr_Height(void);
int DlssNr_SsEvals(void);
int DlssNr_SsCreates(void);
int DlssNr_SsDestroys(void);
void DlssNr_SsFileLog(const char* action, unsigned renderW, unsigned renderH,
	unsigned displayW, unsigned displayH, int quality, unsigned flags);
int DlssNr_NrEvals(void);
int DlssNr_LastNrResult(void);
int DlssNr_NrProbeResult(void);
const char* DlssNr_LastNrResultName(void);
const char* DlssNr_NrStatusText(void);
const char* DlssNr_NrHostText(void);
void DlssNr_ResetLatch(void);
void DlssNr_DumpStatus(void);
void DlssNr_FileLog(const char* fmt, ...);

struct ID3D12Device;
struct ID3D12Resource;

bool DlssNr_NgxReady(void);
bool DlssNr_EnsureNgxSession(ID3D12Device* device);
bool DlssNr_SuperSamplingLive(void);
int DlssNr_CreateSuperSampling(void* cmdlist, unsigned renderW, unsigned renderH,
	unsigned displayW, unsigned displayH, unsigned flags, int quality);
int DlssNr_EvaluateSuperSampling(void* cmdlist, ID3D12Resource* color, ID3D12Resource* depth,
	ID3D12Resource* motionVectors, ID3D12Resource* output,
	float jitterX, float jitterY, float mvScaleX, float mvScaleY,
	unsigned renderW, unsigned renderH, bool reset);
void DlssNr_ReleaseSuperSampling(void);

bool DlssNr_NrReplaceEnabled(void);
int DlssNr_ReplaceTsaa(void* cmdlist, ID3D12Resource* color, ID3D12Resource* output,
	ID3D12Resource* depth, ID3D12Resource* mv, unsigned w, unsigned h, bool reset,
	float mvScaleX, float mvScaleY);

#endif // !DEDICATED
