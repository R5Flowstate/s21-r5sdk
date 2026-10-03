//=============================================================================//
//
// Purpose: In-engine cubemap capture (see cubemap_capture.h).
//
// Each face is the main view at the cubemap origin with a field of view wide
// enough to hold a 90 degree square. After the face has settled for a number
// of frames (TSAA history, texture streaming), the HDR scene colour is read
// back, the centre square is cropped to exactly 90 degrees, box-filtered to
// the output size and written as a PFM next to a manifest the compiler reads.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "game/client/cubemap_capture.h"
#include "game/client/sceneview.h"
#include "windows/texture_readback.h"
#include <cmath>
#include <cstdio>
#include <vector>

static constexpr int CUBEMAP_MAX_SAMPLES = 256;
static constexpr int CUBEMAP_FACES = 6;

// Face order and angles (pitch, yaw); roll is always 0. Negative pitch looks up.
static constexpr float s_faceAngles[CUBEMAP_FACES][2] =
{
	{ 0.0f, 0.0f }, { 0.0f, 180.0f }, { 0.0f, 90.0f }, { 0.0f, 270.0f }, { -90.0f, 0.0f }, { 90.0f, 0.0f }
};

// Render context: a 1x1 ReadPixels of the back buffer waits for the render
// thread and the GPU, so the last frame is complete and nothing writes it.
static constexpr ptrdiff_t MATSYS_GET_RENDER_CONTEXT = 0x390;
static constexpr ptrdiff_t CTX_RELEASE = 0x8;
static constexpr ptrdiff_t CTX_READ_PIXELS = 0x158;
static constexpr int IMAGE_FORMAT_RGBA8888 = 0;

static ConVar cubemap_capture_res("cubemap_capture_res", "256", FCVAR_DEVELOPMENTONLY,
	"Output face size of a cubemap capture.", true, 16.f, true, 1024.f);
static ConVar cubemap_capture_fov("cubemap_capture_fov", "110", FCVAR_DEVELOPMENTONLY,
	"Field of view a capture renders with; must cover 90 degrees vertically.", true, 90.f, true, 130.f);
static ConVar cubemap_capture_settle("cubemap_capture_settle", "24", FCVAR_DEVELOPMENTONLY,
	"Frames a face renders before it is read back.", true, 4.f, true, 600.f);
static ConVar cubemap_capture_settle_first("cubemap_capture_settle_first", "90", FCVAR_DEVELOPMENTONLY,
	"Frames the first face of each cubemap renders, so streaming catches up after the move.", true, 4.f, true, 1200.f);

struct CubemapCapture_t
{
	bool bActive;
	int nSample;
	int nLastSample;
	int nFace;
	int nFrame;
	int nWritten;
	int nViewModelWas;
};

static float s_samples[CUBEMAP_MAX_SAMPLES][3];
static int s_nSamples = 0;
static char s_szMap[128] = "";
static CubemapCapture_t s_cap = {};

//-----------------------------------------------------------------------------
// Samples
//-----------------------------------------------------------------------------
void CubemapCapture_SetSamples(const char* const pszLoadName, const void* const pSamples, const int nCount)
{
	s_nSamples = 0;
	s_szMap[0] = '\0';
	s_cap.bActive = false;
	if (!pSamples || nCount <= 0)
		return;

	const char* pszBase = pszLoadName ? pszLoadName : "";
	for (const char* p = pszBase; *p; p++)
	{
		if (*p == '/' || *p == '\\')
			pszBase = p + 1;
	}
	snprintf(s_szMap, sizeof(s_szMap), "%s", pszBase);

	const int n = nCount < CUBEMAP_MAX_SAMPLES ? nCount : CUBEMAP_MAX_SAMPLES;
	const uint8_t* const pBytes = static_cast<const uint8_t*>(pSamples);
	for (int i = 0; i < n; i++)
		memcpy(s_samples[i], pBytes + i * 3 * sizeof(float), 3 * sizeof(float));
	s_nSamples = n;
}

//-----------------------------------------------------------------------------
// Output
//-----------------------------------------------------------------------------
static void CubemapCapture_Dir(char* const pszOut, const size_t nOut)
{
	CreateDirectoryA("platform\\cubemaps", nullptr);
	snprintf(pszOut, nOut, "platform\\cubemaps\\%s", s_szMap);
	CreateDirectoryA(pszOut, nullptr);
}

static bool CubemapCapture_WritePfm(const char* const pszPath, const std::vector<float>& rgb, const int size)
{
	FILE* const f = fopen(pszPath, "wb");
	if (!f)
		return false;
	fprintf(f, "PF\n%d %d\n-1.0\n", size, size);
	// PFM rows run bottom to top.
	for (int y = size - 1; y >= 0; y--)
		fwrite(&rgb[static_cast<size_t>(y) * size * 3], sizeof(float), static_cast<size_t>(size) * 3, f);
	fclose(f);
	return true;
}

static void CubemapCapture_WriteManifest(void)
{
	char szDir[260];
	CubemapCapture_Dir(szDir, sizeof(szDir));
	char szPath[300];
	snprintf(szPath, sizeof(szPath), "%s\\manifest.json", szDir);
	FILE* const f = fopen(szPath, "w");
	if (!f)
		return;
	fprintf(f, "{\n\t\"map\": \"%s\",\n\t\"size\": %d,\n\t\"faces\": [", s_szMap, cubemap_capture_res.GetInt());
	for (int i = 0; i < CUBEMAP_FACES; i++)
		fprintf(f, "%s[%.1f, %.1f, 0.0]", i ? ", " : "", s_faceAngles[i][0], s_faceAngles[i][1]);
	fprintf(f, "],\n\t\"cubemaps\": [\n");
	for (int i = 0; i < s_nSamples; i++)
		fprintf(f, "\t\t[%.3f, %.3f, %.3f]%s\n", s_samples[i][0], s_samples[i][1], s_samples[i][2],
			i + 1 < s_nSamples ? "," : "");
	fprintf(f, "\t]\n}\n");
	fclose(f);
}

//-----------------------------------------------------------------------------
// Readback: centre square of the scene at exactly 90 degrees, box-filtered.
//-----------------------------------------------------------------------------
static bool CubemapCapture_ReadFace(void)
{
	float tanX = 0.0f, tanY = 0.0f;
	int rect[4] = {};
	if (!SceneView_GetMainProjection(&tanX, &tanY, rect) || rect[2] <= 0 || rect[3] <= 0)
	{
		Warning(eDLL_T::CLIENT, "[CUBEMAP-CAPTURE] no main view projection yet\n");
		return false;
	}
	if (tanY < 1.0f || tanX < 1.0f)
	{
		Warning(eDLL_T::CLIENT, "[CUBEMAP-CAPTURE] view covers %.1f x %.1f degrees; raise cubemap_capture_fov\n",
			2.0f * atanf(tanX) * 57.29578f, 2.0f * atanf(tanY) * 57.29578f);
		return false;
	}

	const int w = rect[2];
	const int h = rect[3];
	const int side = static_cast<int>(static_cast<float>(h) / tanY);
	const int x0 = rect[0] + (w - side) / 2;
	const int y0 = rect[1] + (h - side) / 2;
	if (side < 16)
		return false;

	const uintptr_t mat = SceneView_MaterialSystem();
	const uintptr_t color = SceneView_MainResolvedColor();
	if (!mat || !color)
	{
		Warning(eDLL_T::CLIENT, "[CUBEMAP-CAPTURE] no resolved main colour yet\n");
		return false;
	}
	const uintptr_t ctx = (*reinterpret_cast<uintptr_t(__fastcall* const*)(uintptr_t)>(
		*reinterpret_cast<const uintptr_t*>(mat) + MATSYS_GET_RENDER_CONTEXT))(mat);
	if (!ctx)
		return false;
	uint8_t sync[4] = {};
	const uintptr_t vtCtx = *reinterpret_cast<const uintptr_t*>(ctx);
	(*reinterpret_cast<void(__fastcall* const*)(uintptr_t, int, int, int, int, uint8_t*, int, bool)>(vtCtx + CTX_READ_PIXELS))(
		ctx, 0, 0, 1, 1, sync, IMAGE_FORMAT_RGBA8888, false);
	(*reinterpret_cast<int(__fastcall* const*)(uintptr_t)>(vtCtx + CTX_RELEASE))(ctx);

	std::vector<float> src;
	if (!TextureReadback_ReadRgb(reinterpret_cast<void*>(color), x0, y0, side, side, src))
		return false;

	const int res = cubemap_capture_res.GetInt();
	std::vector<float> dst(static_cast<size_t>(res) * res * 3, 0.0f);
	double peak = 0.0, sum = 0.0;
	for (int oy = 0; oy < res; oy++)
	{
		const int sy0 = oy * side / res;
		const int sy1 = ((oy + 1) * side / res) > sy0 ? (oy + 1) * side / res : sy0 + 1;
		for (int ox = 0; ox < res; ox++)
		{
			const int sx0 = ox * side / res;
			const int sx1 = ((ox + 1) * side / res) > sx0 ? (ox + 1) * side / res : sx0 + 1;
			float acc[3] = {};
			int n = 0;
			for (int sy = sy0; sy < sy1 && sy < side; sy++)
			{
				for (int sx = sx0; sx < sx1 && sx < side; sx++)
				{
					const float* const p = &src[(static_cast<size_t>(sy) * side + sx) * 3];
					for (int c = 0; c < 3; c++)
						acc[c] += std::isfinite(p[c]) && p[c] > 0.0f ? p[c] : 0.0f;
					n++;
				}
			}
			float* const o = &dst[(static_cast<size_t>(oy) * res + ox) * 3];
			for (int c = 0; c < 3; c++)
			{
				o[c] = n ? acc[c] / static_cast<float>(n) : 0.0f;
				sum += o[c];
				if (o[c] > peak)
					peak = o[c];
			}
		}
	}

	if (peak <= 0.0)
	{
		Warning(eDLL_T::CLIENT, "[CUBEMAP-CAPTURE] cubemap %d face %d read back black; the resolved colour held nothing\n",
			s_cap.nSample, s_cap.nFace);
		return false;
	}

	char szDir[260];
	CubemapCapture_Dir(szDir, sizeof(szDir));
	char szPath[300];
	snprintf(szPath, sizeof(szPath), "%s\\cubemap_%03d_%d.pfm", szDir, s_cap.nSample, s_cap.nFace);
	if (!CubemapCapture_WritePfm(szPath, dst, res))
	{
		Warning(eDLL_T::CLIENT, "[CUBEMAP-CAPTURE] cannot write %s\n", szPath);
		return false;
	}
	Msg(eDLL_T::CLIENT, "[CUBEMAP-CAPTURE] cubemap %d/%d face %d: %dpx crop of %dx%d, mean %.3f peak %.2f -> %s\n",
		s_cap.nSample + 1, s_nSamples, s_cap.nFace, side, w, h, sum / (3.0 * res * res), peak, szPath);
	return true;
}

//-----------------------------------------------------------------------------
// Capture state
//-----------------------------------------------------------------------------
static void CubemapCapture_SetViewModel(const int nValue)
{
	ConVar* const pVar = g_pCVar ? g_pCVar->FindVar("r_drawviewmodel") : nullptr;
	if (pVar)
		pVar->SetValue(nValue);
}

static void CubemapCapture_Finish(const bool bComplete)
{
	if (!s_cap.bActive)
		return;
	s_cap.bActive = false;
	CubemapCapture_SetViewModel(s_cap.nViewModelWas);
	if (bComplete)
		CubemapCapture_WriteManifest();
	Msg(eDLL_T::CLIENT, "[CUBEMAP-CAPTURE] %s: %d faces written for '%s'\n",
		bComplete ? "done" : "stopped", s_cap.nWritten, s_szMap);
}

bool CubemapCapture_OverrideView(float* const pOrigin, float* const pAngles, float* const pFov)
{
	if (!s_cap.bActive || !pOrigin || !pAngles)
		return false;

	const int settle = s_cap.nFace == 0 ? cubemap_capture_settle_first.GetInt() : cubemap_capture_settle.GetInt();
	if (++s_cap.nFrame > settle)
	{
		// The frame just finished rendered this face; read it before moving on.
		if (!CubemapCapture_ReadFace())
		{
			CubemapCapture_Finish(false);
			return false;
		}
		s_cap.nWritten++;
		s_cap.nFrame = 0;
		if (++s_cap.nFace >= CUBEMAP_FACES)
		{
			s_cap.nFace = 0;
			if (++s_cap.nSample > s_cap.nLastSample)
			{
				CubemapCapture_Finish(true);
				return false;
			}
		}
	}

	memcpy(pOrigin, s_samples[s_cap.nSample], 3 * sizeof(float));
	pAngles[0] = s_faceAngles[s_cap.nFace][0];
	pAngles[1] = s_faceAngles[s_cap.nFace][1];
	pAngles[2] = 0.0f;
	if (pFov)
		*pFov = cubemap_capture_fov.GetFloat();
	return true;
}

static void CC_CubemapCapture_f(const CCommand& args)
{
	if (!s_nSamples)
	{
		Warning(eDLL_T::CLIENT, "[CUBEMAP-CAPTURE] the loaded map has no cubemaps\n");
		return;
	}
	if (s_cap.bActive)
	{
		Warning(eDLL_T::CLIENT, "[CUBEMAP-CAPTURE] a capture is already running (cubemap_capture_stop)\n");
		return;
	}

	int nFirst = 0;
	int nLast = s_nSamples - 1;
	if (args.ArgC() >= 2)
	{
		const int nIndex = atoi(args.Arg(1));
		if (nIndex < 0 || nIndex >= s_nSamples)
		{
			Warning(eDLL_T::CLIENT, "[CUBEMAP-CAPTURE] index %d out of range 0..%d\n", nIndex, s_nSamples - 1);
			return;
		}
		nFirst = nLast = nIndex;
	}

	ConVar* const pViewModel = g_pCVar ? g_pCVar->FindVar("r_drawviewmodel") : nullptr;
	s_cap = {};
	s_cap.bActive = true;
	s_cap.nSample = nFirst;
	s_cap.nLastSample = nLast;
	s_cap.nViewModelWas = pViewModel ? pViewModel->GetInt() : 1;
	CubemapCapture_SetViewModel(0);
	Msg(eDLL_T::CLIENT, "[CUBEMAP-CAPTURE] capturing %d cubemap(s) of '%s' at %dpx; stand still until done\n",
		nLast - nFirst + 1, s_szMap, cubemap_capture_res.GetInt());
}

static void CC_CubemapCaptureStop_f(const CCommand& args)
{
	CubemapCapture_Finish(false);
}

static ConCommand cubemap_capture("cubemap_capture", CC_CubemapCapture_f,
	"Render every cubemap of the loaded map from its origin and write the faces to platform/cubemaps/<map>. "
	"Usage: cubemap_capture [index]", FCVAR_DEVELOPMENTONLY);
static ConCommand cubemap_capture_stop("cubemap_capture_stop", CC_CubemapCaptureStop_f,
	"Stop a running cubemap capture.", FCVAR_DEVELOPMENTONLY);
