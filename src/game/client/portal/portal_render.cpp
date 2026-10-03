//=============================================================================//
//
// Purpose: Portal surface renderer (see portal_render.h). Draws after opaque
// geometry by hooking the engine's transparent scene pass and recording our
// own D3D12 quads on the engine's command list before the original runs.
// Our root signature carries root constants only, so no engine descriptor
// heap is ever rebound; render targets stay exactly as opaque left them.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier1/cvar.h"
#include "game/client/portal/portal_render.h"
#include "game/client/portal/c_prop_portal.h"
#include "game/client/sceneview.h"
#include "engine/modelloader.h"
#include "rtech/pak/paktools.h"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <cmath>

extern bool SDK_IsDx12Exe();

// The transparent pass only queues render commands; the D3D12 list is recorded
// later on the render thread, so this hook never has one. Portals draw as the
// networked prop model; this overlay stays off until it runs as a queued command.
static ConVar portal_render_enable("portal_render_enable", "0", FCVAR_DEVELOPMENTONLY | FCVAR_ACCESSIBLE_FROM_THREADS,
	"Draw the portal overlay quads in the main scene (0 = off).");
static ConVar portal_surface_test("portal_surface_test", "0", FCVAR_DEVELOPMENTONLY | FCVAR_ACCESSIBLE_FROM_THREADS,
	"Draw a solid magenta billboard 128 units in front of the main camera (S2 spike).");
static ConVar portal_render_debug("portal_render_debug", "0", FCVAR_DEVELOPMENTONLY | FCVAR_ACCESSIBLE_FROM_THREADS,
	"Log [PORTAL-SURF] per-second state.");

// Transparent pass flags, read off the two call sites: the main scene pass
// ORs 0x40000 into a4, the secondary caller passes 0x8040.
static constexpr unsigned int TRANSPARENT_MAIN_VIEW_BIT = 0x40000u;

static constexpr int PORTALSURF_MAX_LIST = 64;
static constexpr int PORTALSURF_MAX_QUADS = 72;
static constexpr int PORTALSURF_VERTS_PER_QUAD = 6;
static constexpr size_t PORTALSURF_VB_SIZE = 8192;
static constexpr float PORTALSURF_TEST_DIST = 128.0f;
static constexpr float PORTALSURF_TEST_HALF = 24.0f;
static constexpr float PORTALSURF_CULL_DIST = 6000.0f;
// SceneView refuses tan > 4; closer than this the view crops instead of widening.
static constexpr float PORTALSURF_MAX_FIT_TAN = 3.9f;

// Engine RHI context holding the recording command list (same fields the
// DLSS path uses: TLS slot 0 + 8736, context bytes, list at +0x5090).
static constexpr size_t PORTALSURF_TLS_RHI = 8736;
static constexpr size_t PORTALSURF_RHI_LIST_OFF = 0x5090;

struct PortalSurfVert_t
{
	float x, y, z;
	float u, v;
};

struct PortalSurfAnim_t
{
	int entNum;
	float openT;
	float staticT;
	bool used;
};

static ID3D12Device* s_pDevice = nullptr;
static ID3D12RootSignature* s_pRootSig = nullptr;
static ID3D12PipelineState* s_pPSO = nullptr;
static ID3D12Resource* s_pVB = nullptr;
static uint8_t* s_pVBMap = nullptr;

static PortalSurfAnim_t s_anims[PORTALSURF_MAX_LIST] = {};
static double s_flLastT = 0.0;

static uint32_t s_nHookCalls = 0;
static uint32_t s_nDraws = 0;
static uint32_t s_nSkipDisabled = 0;
static uint32_t s_nSkipView = 0;
static uint32_t s_nSkipList = 0;
static uint32_t s_nSkipCam = 0;
static uint32_t s_nSkipPSO = 0;
static uint32_t s_nPortalsSeen = 0;
static uint32_t s_nRequests = 0;
static bool s_bLoggedPSO = false;
static bool s_bLoggedFirstDraw = false;
static bool s_bLoggedViewMismatch = false;

static const char s_pszPortalVS[] =
	"cbuffer PortalCB : register(b0)\n"
	"{\n"
	"	row_major float4x4 g_viewProj;\n"
	"	float4 g_base;\n"
	"	float4 g_rim;\n"
	"	float4 g_anim;\n"
	"};\n"
	"struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; };\n"
	"struct PSIn { float4 clip : SV_Position; float2 uv : TEXCOORD0; };\n"
	"PSIn VSMain(VSIn i)\n"
	"{\n"
	"	PSIn o;\n"
	"	o.clip = mul(float4(i.pos, 1.0f), g_viewProj);\n"
	"	o.uv = i.uv;\n"
	"	return o;\n"
	"}\n";

static const char s_pszPortalPS[] =
	"cbuffer PortalCB : register(b0)\n"
	"{\n"
	"	row_major float4x4 g_viewProj;\n"
	"	float4 g_base;\n"
	"	float4 g_rim;\n"
	"	float4 g_anim;\n"
	"};\n"
	"struct PSIn { float4 clip : SV_Position; float2 uv : TEXCOORD0; };\n"
	"float Portal_Hash(float2 p)\n"
	"{\n"
	"	float3 p3 = frac(float3(p.xyx) * 0.1031f);\n"
	"	p3 += dot(p3, p3.yzx + 33.33f);\n"
	"	return frac((p3.x + p3.y) * p3.z);\n"
	"}\n"
	"float4 PSMain(PSIn i) : SV_Target0\n"
	"{\n"
	"	float mode = g_base.w;\n"
	"	if (mode < 0.5f)\n"
	"		return float4(g_base.xyz, 1.0f);\n"
	"	float2 d = (i.uv - 0.5f) * 2.0f;\n"
	"	float openR = max(g_anim.x, 1e-3f);\n"
	"	float r = length(d / openR);\n"
	"	if (r > 1.0f)\n"
	"		discard;\n"
	"	float rim = smoothstep(0.72f, 1.0f, r);\n"
	"	float3 col = g_base.xyz * (1.0f - rim) + g_rim.xyz * rim;\n"
	"	float n = Portal_Hash(i.uv * float2(320.0f, 180.0f) + frac(g_anim.y) * 61.7f);\n"
	"	float amt = g_rim.w * (mode > 1.5f ? 1.0f : 0.35f);\n"
	"	col = lerp(col, float3(n, n, n), saturate(amt));\n"
	"	return float4(col, 1.0f);\n"
	"}\n";

//-----------------------------------------------------------------------------
// Engine command list + device
//-----------------------------------------------------------------------------
static ID3D12GraphicsCommandList* PortalSurf_EngineList(void)
{
	__try
	{
		uint8_t* const teb = reinterpret_cast<uint8_t*>(NtCurrentTeb());
		if (!teb)
			return nullptr;
		void** const slots = *reinterpret_cast<void***>(teb + 0x58);
		if (!slots)
			return nullptr;
		uint8_t* const tls = static_cast<uint8_t*>(slots[0]);
		if (!tls)
			return nullptr;
		void** const ctxpp = *reinterpret_cast<void***>(tls + PORTALSURF_TLS_RHI);
		if (!ctxpp || !*ctxpp)
			return nullptr;
		uint8_t* const ctx = static_cast<uint8_t*>(*ctxpp);
		void* const cand = *reinterpret_cast<void**>(ctx + PORTALSURF_RHI_LIST_OFF);
		if (!cand)
			return nullptr;
		ID3D12GraphicsCommandList* list = nullptr;
		if (FAILED(reinterpret_cast<IUnknown*>(cand)->QueryInterface(IID_PPV_ARGS(&list))) || !list)
			return nullptr;
		if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
		{
			list->Release();
			return nullptr;
		}
		return list;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
	return nullptr;
}

static bool PortalSurf_EnsureDevice(ID3D12GraphicsCommandList* const list)
{
	if (s_pDevice)
		return true;
	if (!list)
		return false;
	__try
	{
		if (FAILED(list->GetDevice(IID_PPV_ARGS(&s_pDevice))) || !s_pDevice)
		{
			s_pDevice = nullptr;
			return false;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		s_pDevice = nullptr;
		return false;
	}
	return true;
}

static bool PortalSurf_Compile(const char* const src, const char* const entry, const char* const target, ID3DBlob** const out)
{
	ID3DBlob* err = nullptr;
	ID3DBlob* blob = nullptr;
	const HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target,
		D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
	if (FAILED(hr) || !blob)
	{
		if (err)
		{
			Warning(eDLL_T::CLIENT, "[PORTAL-SURF] %s compile failed: %s\n", entry,
				static_cast<char*>(err->GetBufferPointer()));
			err->Release();
		}
		else
		{
			Warning(eDLL_T::CLIENT, "[PORTAL-SURF] %s compile failed (hr=0x%08X)\n", entry, hr);
		}
		return false;
	}
	if (err)
		err->Release();
	*out = blob;
	return true;
}

static bool PortalSurf_EnsurePSO(void)
{
	if (s_pPSO && s_pRootSig && s_pVB)
		return true;
	if (!s_pDevice)
		return false;

	if (!s_pRootSig)
	{
		D3D12_ROOT_PARAMETER param = {};
		param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		param.Constants.Num32BitValues = 28;
		param.Constants.ShaderRegister = 0;
		param.Constants.RegisterSpace = 0;
		D3D12_ROOT_SIGNATURE_DESC rs = {};
		rs.NumParameters = 1;
		rs.pParameters = &param;
		rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
		ID3DBlob* blob = nullptr;
		ID3DBlob* err = nullptr;
		if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
		{
			Warning(eDLL_T::CLIENT, "[PORTAL-SURF] root signature serialize failed\n");
			if (err)
				err->Release();
			return false;
		}
		if (FAILED(s_pDevice->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
			IID_PPV_ARGS(&s_pRootSig))) || !s_pRootSig)
		{
			Warning(eDLL_T::CLIENT, "[PORTAL-SURF] root signature create failed\n");
			blob->Release();
			s_pRootSig = nullptr;
			return false;
		}
		blob->Release();
	}

	if (!s_pPSO)
	{
		ID3DBlob* vs = nullptr;
		ID3DBlob* ps = nullptr;
		if (!PortalSurf_Compile(s_pszPortalVS, "VSMain", "vs_5_0", &vs)
			|| !PortalSurf_Compile(s_pszPortalPS, "PSMain", "ps_5_0", &ps))
		{
			if (vs)
				vs->Release();
			if (ps)
				ps->Release();
			return false;
		}
		D3D12_INPUT_ELEMENT_DESC layout[2] = {};
		layout[0].SemanticName = "POSITION";
		layout[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
		layout[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
		layout[1].SemanticName = "TEXCOORD";
		layout[1].Format = DXGI_FORMAT_R32G32_FLOAT;
		layout[1].AlignedByteOffset = 12;
		layout[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
		desc.pRootSignature = s_pRootSig;
		desc.VS.pShaderBytecode = vs->GetBufferPointer();
		desc.VS.BytecodeLength = vs->GetBufferSize();
		desc.PS.pShaderBytecode = ps->GetBufferPointer();
		desc.PS.BytecodeLength = ps->GetBufferSize();
		desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		desc.SampleMask = UINT_MAX;
		desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.RasterizerState.DepthClipEnable = TRUE;
		desc.DepthStencilState.DepthEnable = TRUE;
		desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		desc.InputLayout.pInputElementDescs = layout;
		desc.InputLayout.NumElements = 2;
		desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		desc.NumRenderTargets = 1;
		desc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
		desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		const HRESULT hr = s_pDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&s_pPSO));
		vs->Release();
		ps->Release();
		if (FAILED(hr) || !s_pPSO)
		{
			Warning(eDLL_T::CLIENT, "[PORTAL-SURF] graphics PSO create failed (hr=0x%08X)\n", hr);
			s_pPSO = nullptr;
			return false;
		}
	}

	if (!s_pVB)
	{
		D3D12_HEAP_PROPERTIES hp = {};
		hp.Type = D3D12_HEAP_TYPE_UPLOAD;
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = PORTALSURF_VB_SIZE;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(s_pDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&s_pVB))) || !s_pVB)
		{
			Warning(eDLL_T::CLIENT, "[PORTAL-SURF] vertex buffer create failed\n");
			s_pVB = nullptr;
			return false;
		}
		D3D12_RANGE range = {};
		if (FAILED(s_pVB->Map(0, &range, reinterpret_cast<void**>(&s_pVBMap))) || !s_pVBMap)
		{
			Warning(eDLL_T::CLIENT, "[PORTAL-SURF] vertex buffer map failed\n");
			s_pVBMap = nullptr;
			return false;
		}
	}

	if (!s_bLoggedPSO)
	{
		s_bLoggedPSO = true;
		Msg(eDLL_T::CLIENT, "[PORTAL-SURF] own PSO live (root constants only, depth test on, no heap rebind)\n");
	}
	return true;
}

//-----------------------------------------------------------------------------
// Small math helpers (row-major 4x4, row vectors: v' = v * M, origin at [12..14])
//-----------------------------------------------------------------------------
static void PortalSurf_AngleVectors(const float* const ang, float* const fwd, float* const right, float* const up)
{
	const float sy = sinf(ang[1] * (3.14159265f / 180.0f));
	const float cy = cosf(ang[1] * (3.14159265f / 180.0f));
	const float sp = sinf(ang[0] * (3.14159265f / 180.0f));
	const float cp = cosf(ang[0] * (3.14159265f / 180.0f));
	const float sr = sinf(ang[2] * (3.14159265f / 180.0f));
	const float cr = cosf(ang[2] * (3.14159265f / 180.0f));
	if (fwd)
	{
		fwd[0] = cp * cy; fwd[1] = cp * sy; fwd[2] = -sp;
	}
	if (right)
	{
		right[0] = -sr * sp * cy + -cr * -sy;
		right[1] = -sr * sp * sy + -cr * cy;
		right[2] = -sr * cp;
	}
	if (up)
	{
		up[0] = cr * sp * cy + sr * -sy;
		up[1] = cr * sp * sy + -sr * cy;
		up[2] = cr * cp;
	}
}

// Local frame rows: X = face normal, Y = side, Z = up; 180 flip about Z.
static void PortalSurf_FrameFromPose(const float* const org, const float* const fwd,
	const float* const right, const float* const up, float* const m)
{
	m[0] = fwd[0]; m[1] = fwd[1]; m[2] = fwd[2]; m[3] = 0.0f;
	m[4] = right[0]; m[5] = right[1]; m[6] = right[2]; m[7] = 0.0f;
	m[8] = up[0]; m[9] = up[1]; m[10] = up[2]; m[11] = 0.0f;
	m[12] = org[0]; m[13] = org[1]; m[14] = org[2]; m[15] = 1.0f;
}

static void PortalSurf_Mul4(const float* const a, const float* const b, float* const o)
{
	for (int i = 0; i < 4; i++)
	{
		for (int j = 0; j < 4; j++)
		{
			o[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j]
				+ a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
		}
	}
}

static void PortalSurf_InvertRigid(const float* const m, float* const o)
{
	o[0] = m[0]; o[1] = m[4]; o[2] = m[8]; o[3] = 0.0f;
	o[4] = m[1]; o[5] = m[5]; o[6] = m[9]; o[7] = 0.0f;
	o[8] = m[2]; o[9] = m[6]; o[10] = m[10]; o[11] = 0.0f;
	o[12] = -(m[12] * o[0] + m[13] * o[4] + m[14] * o[8]);
	o[13] = -(m[12] * o[1] + m[13] * o[5] + m[14] * o[9]);
	o[14] = -(m[12] * o[2] + m[13] * o[6] + m[14] * o[10]);
	o[15] = 1.0f;
}

static void PortalSurf_TransformDir(const float* const m, const float* const d, float* const o)
{
	o[0] = d[0] * m[0] + d[1] * m[4] + d[2] * m[8];
	o[1] = d[0] * m[1] + d[1] * m[5] + d[2] * m[9];
	o[2] = d[0] * m[2] + d[1] * m[6] + d[2] * m[10];
}

static void PortalSurf_TransformPoint(const float* const m, const float* const p, float* const o)
{
	o[0] = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
	o[1] = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
	o[2] = p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14];
}

// Row vectors (p * M): M = inverse(entry) * Rz(180) * exit.
static void PortalSurf_PairMatrix(const float* const entryOrg, const float* const entryAng,
	const float* const exitOrg, const float* const exitAng, float* const m)
{
	float ef[3], er[3], eu[3], xf[3], xr[3], xu[3];
	PortalSurf_AngleVectors(entryAng, ef, er, eu);
	PortalSurf_AngleVectors(exitAng, xf, xr, xu);
	float E[16], X[16], Einv[16], Z[16], tmp[16];
	PortalSurf_FrameFromPose(entryOrg, ef, er, eu, E);
	PortalSurf_FrameFromPose(exitOrg, xf, xr, xu, X);
	PortalSurf_InvertRigid(E, Einv);
	memset(Z, 0, sizeof(Z));
	Z[0] = -1.0f; Z[5] = -1.0f; Z[10] = 1.0f; Z[15] = 1.0f;
	PortalSurf_Mul4(Einv, Z, tmp);
	PortalSurf_Mul4(tmp, X, m);
}

static void PortalSurf_DirToAngles(const float* const fwd, float* const ang)
{
	float f = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1]);
	if (f < 1e-6f)
	{
		ang[0] = fwd[2] > 0.0f ? -90.0f : 90.0f;
		ang[1] = 0.0f;
	}
	else
	{
		ang[0] = -asinf(fwd[2] < -1.0f ? -1.0f : (fwd[2] > 1.0f ? 1.0f : fwd[2])) * (180.0f / 3.14159265f);
		ang[1] = atan2f(fwd[1], fwd[0]) * (180.0f / 3.14159265f);
	}
	ang[2] = 0.0f;
}

//-----------------------------------------------------------------------------
// Quad recording (vertex slots partition one upload buffer across the draws
// of a single hook call, so no draw reads memory a later draw rewrote)
//-----------------------------------------------------------------------------
static int s_nQuadSlot = 0;

static void PortalSurf_WriteQuad(const float (*corners)[3], const float (*uvs)[2])
{
	const int slot = s_nQuadSlot++;
	if ((slot + 1) * PORTALSURF_VERTS_PER_QUAD * sizeof(PortalSurfVert_t) > PORTALSURF_VB_SIZE)
		return;
	PortalSurfVert_t* const v = reinterpret_cast<PortalSurfVert_t*>(
		s_pVBMap + slot * PORTALSURF_VERTS_PER_QUAD * sizeof(PortalSurfVert_t));
	const int idx[6] = { 0, 1, 2, 0, 2, 3 };
	for (int i = 0; i < 6; i++)
	{
		v[i].x = corners[idx[i]][0];
		v[i].y = corners[idx[i]][1];
		v[i].z = corners[idx[i]][2];
		v[i].u = uvs[idx[i]][0];
		v[i].v = uvs[idx[i]][1];
	}
}

static void PortalSurf_DrawQuad(ID3D12GraphicsCommandList* const list, const int slot,
	const float* const viewProj, const float baseR, const float baseG, const float baseB, const float mode,
	const float rimR, const float rimG, const float rimB, const float staticAmt,
	const float openAmt, const float time)
{
	float c[28];
	memcpy(c, viewProj, 16 * sizeof(float));
	c[16] = baseR; c[17] = baseG; c[18] = baseB; c[19] = mode;
	c[20] = rimR; c[21] = rimG; c[22] = rimB; c[23] = staticAmt;
	c[24] = openAmt; c[25] = time; c[26] = 0.0f; c[27] = 0.0f;
	list->SetGraphicsRootSignature(s_pRootSig);
	list->SetPipelineState(s_pPSO);
	list->SetGraphicsRoot32BitConstants(0, 28, c, 0);
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	D3D12_VERTEX_BUFFER_VIEW vbv = {};
	vbv.BufferLocation = s_pVB->GetGPUVirtualAddress() + slot * PORTALSURF_VERTS_PER_QUAD * sizeof(PortalSurfVert_t);
	vbv.SizeInBytes = PORTALSURF_VERTS_PER_QUAD * sizeof(PortalSurfVert_t);
	vbv.StrideInBytes = sizeof(PortalSurfVert_t);
	list->IASetVertexBuffers(0, 1, &vbv);
	list->DrawInstanced(PORTALSURF_VERTS_PER_QUAD, 1, 0, 0);
	++s_nDraws;
	if (!s_bLoggedFirstDraw)
	{
		s_bLoggedFirstDraw = true;
		Msg(eDLL_T::CLIENT, "[PORTAL-SURF] first quad recorded (mode=%.0f)\n", (double)mode);
	}
}

static void PortalSurf_EntryCorners(const float* const org, const float* const right,
	const float* const up, const float halfW, const float halfH, float (*corners)[3])
{
	for (int i = 0; i < 4; i++)
	{
		const float sx = (i == 0 || i == 1) ? -1.0f : 1.0f;
		const float sy = (i == 0 || i == 3) ? -1.0f : 1.0f;
		corners[i][0] = org[0] + right[0] * sx * halfW + up[0] * sy * halfH;
		corners[i][1] = org[1] + right[1] * sx * halfW + up[1] * sy * halfH;
		corners[i][2] = org[2] + right[2] * sx * halfW + up[2] * sy * halfH;
	}
}

static const float s_quadUV[4][2] = { { 0.0f, 0.0f }, { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f } };

static PortalSurfAnim_t* PortalSurf_AnimFor(const int entNum, const double now, const double dt)
{
	PortalSurfAnim_t* free = nullptr;
	for (int i = 0; i < PORTALSURF_MAX_LIST; i++)
	{
		if (s_anims[i].used && s_anims[i].entNum == entNum)
		{
			s_anims[i].openT += (float)(2.0 * dt);
			if (s_anims[i].openT > 1.0f)
				s_anims[i].openT = 1.0f;
			s_anims[i].staticT -= (float)dt;
			if (s_anims[i].staticT < 0.0f)
				s_anims[i].staticT = 0.0f;
			return &s_anims[i];
		}
		if (!s_anims[i].used && !free)
			free = &s_anims[i];
	}
	if (!free)
		return nullptr;
	free->used = true;
	free->entNum = entNum;
	free->openT = 0.0f;
	free->staticT = 1.0f;
	(void)now;
	return free;
}

//-----------------------------------------------------------------------------
// Picks the linked portals worth a view this frame (largest on screen first)
// and requests a SceneView through each. order[] indexes infos; drawn[] says
// whether that slot rendered last frame.
//-----------------------------------------------------------------------------
static int PortalSurf_RequestViews(const PortalRenderInfo_t* const infos, const int nPortals,
	const float* const eye, const float* const fwd, const float tanX,
	int* const order, bool* const drawn)
{
	int cand[PORTALSURF_MAX_LIST];
	float score[PORTALSURF_MAX_LIST];
	int nCand = 0;
	for (int i = 0; i < nPortals && nCand < PORTALSURF_MAX_LIST; i++)
	{
		const PortalRenderInfo_t& pi = infos[i];
		if (!pi.bActive || !pi.link)
			continue;
		const float d[3] = { pi.origin[0] - eye[0], pi.origin[1] - eye[1], pi.origin[2] - eye[2] };
		const float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (dist > PORTALSURF_CULL_DIST || dist < 1.0f)
			continue;
		if ((d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2]) <= 0.0f)
			continue;
		cand[nCand] = i;
		score[nCand] = (pi.halfW * pi.halfH) / (dist * dist + 1.0f);
		++nCand;
	}
	for (int i = 0; i < nCand; i++)
	{
		for (int j = i + 1; j < nCand; j++)
		{
			if (score[j] > score[i])
			{
				const float ts = score[i]; score[i] = score[j]; score[j] = ts;
				const int ti = cand[i]; cand[i] = cand[j]; cand[j] = ti;
			}
		}
	}

	const int nTake = nCand > SCENEVIEW_MAX_ACTIVE ? SCENEVIEW_MAX_ACTIVE : nCand;
	for (int t = 0; t < nTake; t++)
	{
		const PortalRenderInfo_t& pi = infos[cand[t]];
		order[t] = cand[t];
		drawn[t] = false;

		const PortalRenderInfo_t* exit = nullptr;
		const int wantEnt = static_cast<int>(pi.link & 0x1FFF);
		for (int i = 0; i < nPortals; i++)
		{
			if (infos[i].entNum == wantEnt)
			{
				exit = &infos[i];
				break;
			}
		}
		if (!exit && nPortals == 2)
			exit = (&infos[0] == &pi) ? &infos[1] : &infos[0];
		if (!exit || !exit->bActive)
			continue;

		float xf[3], xr[3], xu[3];
		PortalSurf_AngleVectors(exit->angles, xf, xr, xu);
		float M[16];
		PortalSurf_PairMatrix(pi.origin, pi.angles, exit->origin, exit->angles, M);
		float vorg[3];
		PortalSurf_TransformPoint(M, eye, vorg);
		const float clip[4] = { xf[0], xf[1], xf[2],
			-(xf[0] * exit->origin[0] + xf[1] * exit->origin[1] + xf[2] * exit->origin[2]) };
		float rect[4][3];
		PortalSurf_EntryCorners(exit->origin, xr, xu, exit->halfW, exit->halfH, rect);

		// The view looks along the exit normal with the exit's up, so the exit
		// rect projects axis-aligned and the fitted image maps onto the face UVs.
		float tanXNeed = 0.0f, tanYNeed = 0.0f, minDepth = 1e9f;
		for (int c = 0; c < 4; c++)
		{
			const float rel[3] = { rect[c][0] - vorg[0], rect[c][1] - vorg[1], rect[c][2] - vorg[2] };
			const float depth = rel[0] * xf[0] + rel[1] * xf[1] + rel[2] * xf[2];
			const float side = rel[0] * xr[0] + rel[1] * xr[1] + rel[2] * xr[2];
			const float vert = rel[0] * xu[0] + rel[1] * xu[1] + rel[2] * xu[2];
			minDepth = depth < minDepth ? depth : minDepth;
			if (depth > 0.0f)
			{
				tanXNeed = fmaxf(tanXNeed, fabsf(side / depth));
				tanYNeed = fmaxf(tanYNeed, fabsf(vert / depth));
			}
		}
		if (minDepth < 1.0f)
			continue;
		float mainTanX = 1.0f, mainTanY = 1.0f;
		int mainRect[4] = {};
		const float aspect = (SceneView_GetMainProjection(&mainTanX, &mainTanY, mainRect) && mainTanY > 0.0f)
			? mainTanX / mainTanY : 16.0f / 9.0f;
		const float fitTanX = fminf(fmaxf(tanXNeed, tanYNeed * aspect) * 1.02f, PORTALSURF_MAX_FIT_TAN);

		const int slot = pi.bIsPortal2 ? 1 : 0;
		SceneView_Request(slot, vorg, exit->angles, fitTanX, clip, rect, true);
		++s_nRequests;
		drawn[t] = SceneView_SlotDrawn(slot);
		const float* const vang = exit->angles;

		if (portal_render_debug.GetBool())
		{
			static uint32_t s_nWarnLink = 0;
			if (s_nHookCalls - s_nWarnLink > 300)
			{
				s_nWarnLink = s_nHookCalls;
				Msg(eDLL_T::CLIENT, "[PORTAL-SURF] slot=%d ent=%d exit=%d eye=(%.0f %.0f %.0f) virt=(%.0f %.0f %.0f) ang=(%.1f %.1f) drawn=%d\n",
					t, pi.entNum, exit->entNum, eye[0], eye[1], eye[2], vorg[0], vorg[1], vorg[2],
					vang[0], vang[1], drawn[t] ? 1 : 0);
			}
		}
	}
	return nTake;
}

//-----------------------------------------------------------------------------
// Transparent-pass hook
//-----------------------------------------------------------------------------
static int64_t Hook_PortalSurf_Transparent(const int64_t a1, unsigned int* const a2,
	const int64_t a3, const unsigned int a4, const unsigned int a5, const int64_t a6)
{
	++s_nHookCalls;
	if (portal_render_enable.GetBool() && SDK_IsDx12Exe())
	{
		const uintptr_t mainLogView = SceneView_MainLogView();
		const bool bIsMain = mainLogView && (a3 - 512) == static_cast<int64_t>(mainLogView)
			&& (a4 & TRANSPARENT_MAIN_VIEW_BIT) != 0;
		const bool bIsOwnView = !bIsMain && mainLogView && SceneView_IsOwnView(static_cast<uintptr_t>(a3 - 512));
		if (bIsMain)
		{
			PortalRenderInfo_t census[PORTALSURF_MAX_LIST];
			s_nPortalsSeen = static_cast<uint32_t>(Portal_GetRenderList(census, PORTALSURF_MAX_LIST));
		}

		if (!bIsMain && !bIsOwnView)
		{
			++s_nSkipView;
			if (!s_bLoggedViewMismatch && portal_render_debug.GetBool())
			{
				s_bLoggedViewMismatch = true;
				Warning(eDLL_T::CLIENT, "[PORTAL-SURF] non-main transparent pass (a4=0x%X logView=0x%p main=0x%p) -- quads skipped there\n",
					a4, reinterpret_cast<void*>(a3 - 512), reinterpret_cast<void*>(mainLogView));
			}
		}
		else
		{
			float eye[3], fwd[3], right[3], up[3], w2p[16], tanX = 1.0f;
			if (!SceneView_GetMainCamera(eye, fwd, right, up, w2p, &tanX))
			{
				++s_nSkipCam;
			}
			else
			{
				ID3D12GraphicsCommandList* const list = PortalSurf_EngineList();
				if (!list)
				{
					++s_nSkipList;
				}
				else if (!PortalSurf_EnsureDevice(list) || !PortalSurf_EnsurePSO())
				{
					++s_nSkipPSO;
					list->Release();
				}
				else
				{
					const double now = Plat_FloatTime();
					double dt = (s_flLastT > 0.0) ? (now - s_flLastT) : 0.016;
					if (dt < 0.0 || dt > 0.1)
						dt = 0.016;
					s_flLastT = now;
					s_nQuadSlot = 0;
					const float time = static_cast<float>(now);

					if (bIsMain && portal_surface_test.GetBool())
					{
						float corners[4][3];
						for (int i = 0; i < 4; i++)
						{
							const float sx = (i == 0 || i == 1) ? -1.0f : 1.0f;
							const float sy = (i == 0 || i == 3) ? -1.0f : 1.0f;
							corners[i][0] = eye[0] + fwd[0] * PORTALSURF_TEST_DIST
								+ right[0] * sx * PORTALSURF_TEST_HALF + up[0] * sy * PORTALSURF_TEST_HALF;
							corners[i][1] = eye[1] + fwd[1] * PORTALSURF_TEST_DIST
								+ right[1] * sx * PORTALSURF_TEST_HALF + up[1] * sy * PORTALSURF_TEST_HALF;
							corners[i][2] = eye[2] + fwd[2] * PORTALSURF_TEST_DIST
								+ right[2] * sx * PORTALSURF_TEST_HALF + up[2] * sy * PORTALSURF_TEST_HALF;
						}
						const int slot = s_nQuadSlot;
						PortalSurf_WriteQuad(corners, s_quadUV);
						PortalSurf_DrawQuad(list, slot, w2p, 1.0f, 0.0f, 1.0f, 0.0f,
							1.0f, 0.0f, 1.0f, 0.0f, 1.0f, time);
					}

					PortalRenderInfo_t infos[PORTALSURF_MAX_LIST];
					const int nPortals = Portal_GetRenderList(infos, PORTALSURF_MAX_LIST);
					s_nPortalsSeen = static_cast<uint32_t>(nPortals);

					if (bIsOwnView)
					{
						for (int i = 0; i < nPortals; i++)
						{
							const PortalRenderInfo_t& pi = infos[i];
							if (!pi.bActive || s_nQuadSlot >= PORTALSURF_MAX_QUADS)
								continue;
							PortalSurfAnim_t* const anim = PortalSurf_AnimFor(pi.entNum, now, dt);
							float pf[3], pr[3], pu[3];
							PortalSurf_AngleVectors(pi.angles, pf, pr, pu);
							float corners[4][3];
							PortalSurf_EntryCorners(pi.origin, pr, pu, pi.halfW, pi.halfH, corners);
							const int slot = s_nQuadSlot;
							PortalSurf_WriteQuad(corners, s_quadUV);
							PortalSurf_DrawQuad(list, slot, w2p,
								0.02f, 0.02f, 0.03f, 2.0f,
								pi.bIsPortal2 ? 1.0f : 0.25f, 0.55f, pi.bIsPortal2 ? 0.15f : 1.0f,
								1.0f, anim ? anim->openT : 1.0f, time);
						}
					}
					else if (nPortals > 0)
					{
						int order[SCENEVIEW_MAX_ACTIVE];
						bool drawn[SCENEVIEW_MAX_ACTIVE];
						const int nTake = PortalSurf_RequestViews(infos, nPortals, eye, fwd, tanX, order, drawn);
						for (int t = 0; t < nTake; t++)
						{
							const PortalRenderInfo_t& pi = infos[order[t]];
							PortalSurfAnim_t* const anim = PortalSurf_AnimFor(pi.entNum, now, dt);
							const float openT = anim ? anim->openT : 1.0f;
							const float staticT = anim ? anim->staticT : 0.0f;
							const bool bViewDrawn = drawn[t];

							if (s_nQuadSlot < PORTALSURF_MAX_QUADS)
							{
								float pf[3], pr[3], pu[3];
								PortalSurf_AngleVectors(pi.angles, pf, pr, pu);
								float corners[4][3];
								PortalSurf_EntryCorners(pi.origin, pr, pu, pi.halfW, pi.halfH, corners);
								const int slot = s_nQuadSlot;
								PortalSurf_WriteQuad(corners, s_quadUV);
								if (pi.bIsPortal2)
								{
									PortalSurf_DrawQuad(list, slot, w2p,
										0.30f, 0.12f, 0.03f, bViewDrawn ? 1.0f : 2.0f,
										1.0f, 0.55f, 0.15f, bViewDrawn ? staticT : 1.0f, openT, time);
								}
								else
								{
									PortalSurf_DrawQuad(list, slot, w2p,
										0.03f, 0.10f, 0.30f, bViewDrawn ? 1.0f : 2.0f,
										0.25f, 0.55f, 1.0f, bViewDrawn ? staticT : 1.0f, openT, time);
								}
							}
						}

						for (int i = 0; i < nPortals; i++)
						{
							const PortalRenderInfo_t& pi = infos[i];
							if (!pi.bActive || pi.link || s_nQuadSlot >= PORTALSURF_MAX_QUADS)
								continue;
							bool bTaken = false;
							for (int t = 0; t < nTake; t++)
							{
								if (infos[order[t]].entNum == pi.entNum)
								{
									bTaken = true;
									break;
								}
							}
							if (bTaken)
								continue;
							PortalSurfAnim_t* const anim = PortalSurf_AnimFor(pi.entNum, now, dt);
							float pf[3], pr[3], pu[3];
							PortalSurf_AngleVectors(pi.angles, pf, pr, pu);
							float corners[4][3];
							PortalSurf_EntryCorners(pi.origin, pr, pu, pi.halfW, pi.halfH, corners);
							const int slot = s_nQuadSlot;
							PortalSurf_WriteQuad(corners, s_quadUV);
							PortalSurf_DrawQuad(list, slot, w2p,
								0.02f, 0.02f, 0.03f, 2.0f,
								pi.bIsPortal2 ? 1.0f : 0.25f, 0.55f, pi.bIsPortal2 ? 0.15f : 1.0f,
								1.0f, anim ? anim->openT : 1.0f, time);
						}
					}
					list->Release();
				}
			}
		}
	}
	else
	{
		++s_nSkipDisabled;
	}

	if (portal_render_debug.GetBool())
	{
		static double s_flNextLog = 0.0;
		const double now = Plat_FloatTime();
		if (now >= s_flNextLog)
		{
			s_flNextLog = now + 1.0;
			Msg(eDLL_T::CLIENT, "[PORTAL-SURF] calls=%u draws=%u skipDis=%u skipView=%u skipCam=%u skipList=%u skipPSO=%u portals=%u reqs=%u pso=%d\n",
				s_nHookCalls, s_nDraws, s_nSkipDisabled, s_nSkipView, s_nSkipCam, s_nSkipList, s_nSkipPSO,
				s_nPortalsSeen, s_nRequests, s_pPSO ? 1 : 0);
		}
	}

	return v_PortalSurf_Transparent(a1, a2, a3, a4, a5, a6);
}

//-----------------------------------------------------------------------------
// Resolution
//-----------------------------------------------------------------------------
void VPortalSurface::GetFun(void) const
{
	// Transparent scene pass: home-rsp args, seven pushes, the 0x70500 frame.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 4C 89 44 24 18 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 "
		"B8 00 05 07 00").GetPtr(v_PortalSurf_Transparent);

	if (!v_PortalSurf_Transparent)
		Warning(eDLL_T::CLIENT, "[PORTAL-SURF] transparent pass unresolved -- portal surfaces disabled\n");
}

//-----------------------------------------------------------------------------
// Face binding: each portal face material's emissive slot is pointed at the
// matching view's colour target, so the retail material pipeline draws the
// see-through image. Only the material's handle array is touched; the pak's
// own face texture asset stays intact for unload.
//-----------------------------------------------------------------------------
static ConVar portal_view_bind("portal_view_bind", "1", FCVAR_RELEASE,
	"Show the see-through view on portal faces (0 = keep the baked face texture).");

static constexpr int PORTALFACE_VIEW_TEXTURE_SLOT = 4; // _ilm
// The lookup returns the engine's CMaterialGlue pool slot: MaterialGlue_s starts at +0x10.
static constexpr ptrdiff_t MATERIALGLUE_BODY = 0x10;
static constexpr ptrdiff_t MATERIALGLUE_TEXTURE_HANDLES = 0x50;
static constexpr int ITEXTURE_GET_ASSET_SLOT = 28;

static const char* const s_portalFaceMaterials[2][2] = {
	{ "material/models/cafefps_portalgun/portalgun/portal_blue_rgdp.rpak",
	  "material/models/cafefps_portalgun/portalgun/portal_blue_colpass_rgdp.rpak" },
	{ "material/models/cafefps_portalgun/portalgun/portal_orange_rgdp.rpak",
	  "material/models/cafefps_portalgun/portalgun/portal_orange_colpass_rgdp.rpak" },
};

static void** PortalFace_MaterialHandles(const char* const pszMaterial)
{
	// The engine's own name hash, the one its pak lookups use.
	uint64_t (*const pfnHash)(const char*) = ((reinterpret_cast<uintptr_t>(pszMaterial) & 3) != 0 && v_HashNameUnaligned)
		? v_HashNameUnaligned : v_HashNameAligned;
	if (!v_Pak_FindAssetVoid || !pfnHash)
		return nullptr;
	const PakGuid_t guid = pfnHash(pszMaterial);
	uint8_t* const p = static_cast<uint8_t*>(v_Pak_FindAssetVoid(guid, nullptr));
	if (p && *reinterpret_cast<const PakGuid_t*>(p + MATERIALGLUE_BODY) == guid)
		return *reinterpret_cast<void***>(p + MATERIALGLUE_BODY + MATERIALGLUE_TEXTURE_HANDLES);
	static bool s_bLogged = false;
	if (!s_bLogged)
	{
		s_bLogged = true;
		Warning(eDLL_T::CLIENT, "[PORTAL-SURF] material lookup '%s' guid=%016llX -> %p head=%016llX %016llX\n",
			pszMaterial, static_cast<unsigned long long>(guid), p,
			p ? static_cast<unsigned long long>(*reinterpret_cast<const uint64_t*>(p)) : 0ull,
			p ? static_cast<unsigned long long>(*reinterpret_cast<const uint64_t*>(p + MATERIALGLUE_BODY)) : 0ull);
	}
	return nullptr;
}

static void* PortalFace_TextureAsset(const uintptr_t pTexture)
{
	if (!pTexture)
		return nullptr;
	typedef void* (__fastcall* GetAssetFn)(uintptr_t, unsigned int);
	const GetAssetFn pfn = reinterpret_cast<GetAssetFn>(
		(*reinterpret_cast<void* const* const*>(pTexture))[ITEXTURE_GET_ASSET_SLOT]);
	return pfn ? pfn(pTexture, 0) : nullptr;
}

// The material's own slot-4 texture, recorded per handle array on first
// sight; the view only ever replaces that one pointer.
struct PortalFaceBinding_t
{
	void** pHandles;
	void* pOriginal;
};
static PortalFaceBinding_t s_faceBindings[2][2] = {};

static void PortalFace_BindViews(void)
{
	if (!portal_view_bind.GetBool())
		return;
	for (int c = 0; c < 2; c++)
	{
		const uintptr_t pTexture = SceneView_SlotColorTexture(c);
		if (!pTexture)
			continue;
		void* const pView = PortalFace_TextureAsset(pTexture);
		if (!pView)
		{
			static bool s_bWarnedAsset = false;
			if (!s_bWarnedAsset)
			{
				s_bWarnedAsset = true;
				Warning(eDLL_T::CLIENT, "[PORTAL-SURF] view slot %d target has no texture asset -- not bound\n", c);
			}
			continue;
		}
		for (int m = 0; m < 2; m++)
		{
			void** const pHandles = PortalFace_MaterialHandles(s_portalFaceMaterials[c][m]);
			if (!pHandles)
			{
				static bool s_bWarnedMat = false;
				if (!s_bWarnedMat)
				{
					s_bWarnedMat = true;
					Warning(eDLL_T::CLIENT, "[PORTAL-SURF] face material '%s' not found -- view not bound\n",
						s_portalFaceMaterials[c][m]);
				}
				continue;
			}
			PortalFaceBinding_t& bind = s_faceBindings[c][m];
			if (bind.pHandles != pHandles)
			{
				bind.pHandles = pHandles;
				bind.pOriginal = pHandles[PORTALFACE_VIEW_TEXTURE_SLOT];
			}
			void* const pCur = pHandles[PORTALFACE_VIEW_TEXTURE_SLOT];
			if (pCur == pView || pCur != bind.pOriginal || !pCur)
				continue;
			pHandles[PORTALFACE_VIEW_TEXTURE_SLOT] = pView;
			Msg(eDLL_T::CLIENT, "[PORTAL-SURF] bound view slot %d to '%s'\n", c, s_portalFaceMaterials[c][m]);
		}
	}
}

// The see-through views render whether or not the overlay draws, so
// sceneview_show can display them.
static void PortalSurf_RequestFromSceneView(void)
{
	if (!SDK_IsDx12Exe() || portal_render_enable.GetBool())
		return;
	PortalRenderInfo_t infos[PORTALSURF_MAX_LIST];
	const int nPortals = Portal_GetRenderList(infos, PORTALSURF_MAX_LIST);
	s_nPortalsSeen = static_cast<uint32_t>(nPortals);
	float eye[3], fwd[3], right[3], up[3], w2p[16], tanX = 1.0f;
	if (nPortals <= 0 || !SceneView_GetMainCamera(eye, fwd, right, up, w2p, &tanX))
		return;
	int order[SCENEVIEW_MAX_ACTIVE];
	bool drawn[SCENEVIEW_MAX_ACTIVE];
	PortalSurf_RequestViews(infos, nPortals, eye, fwd, tanX, order, drawn);
	PortalFace_BindViews();
}

void VPortalSurface::Detour(const bool bAttach) const
{
	SceneView_SetRequestProvider(bAttach ? &PortalSurf_RequestFromSceneView : nullptr);
	if (!v_PortalSurf_Transparent)
		return;

	DetourSetup(&v_PortalSurf_Transparent, &Hook_PortalSurf_Transparent, bAttach);
}
