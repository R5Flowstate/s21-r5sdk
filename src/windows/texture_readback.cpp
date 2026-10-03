//=============================================================================//
//
// Purpose: CPU readback of engine render targets through our own D3D12 queue
// (see texture_readback.h). Nothing is recorded on an engine command list.
//
//=============================================================================//
#include "core/stdafx.h"
#include "windows/texture_readback.h"
#include "windows/dlss_sr.h"
#include <d3d12.h>
#include <cmath>
#include <cstring>

static ID3D12Device* s_pDevice = nullptr;
static ID3D12CommandQueue* s_pQueue = nullptr;
static ID3D12CommandAllocator* s_pAlloc = nullptr;
static ID3D12GraphicsCommandList* s_pList = nullptr;
static ID3D12Fence* s_pFence = nullptr;
static HANDLE s_hEvent = nullptr;
static UINT64 s_nFenceValue = 0;

static bool TextureReadback_Ensure(ID3D12Device* const pDevice)
{
	if (s_pDevice == pDevice && s_pList)
		return true;
	if (s_pDevice)
		return false;

	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	if (FAILED(pDevice->CreateCommandQueue(&qd, IID_PPV_ARGS(&s_pQueue)))
		|| FAILED(pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_pAlloc)))
		|| FAILED(pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_pAlloc, nullptr, IID_PPV_ARGS(&s_pList)))
		|| FAILED(pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_pFence))))
	{
		Warning(eDLL_T::CLIENT, "[READBACK] D3D12 queue creation failed\n");
		return false;
	}
	s_pList->Close();
	s_hEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
	pDevice->AddRef();
	s_pDevice = pDevice;
	return s_hEvent != nullptr;
}

static float TextureReadback_Half(const uint16_t h)
{
	const uint32_t sign = (h >> 15) & 1;
	const int32_t exp = (h >> 10) & 0x1F;
	const uint32_t man = h & 0x3FF;
	float v;
	if (exp == 0)
		v = std::ldexp(static_cast<float>(man), -24);
	else if (exp == 31)
		v = man ? 0.0f : 65504.0f;
	else
		v = std::ldexp(static_cast<float>(man | 0x400), exp - 25);
	return sign ? -v : v;
}

static float TextureReadback_Float11(const uint32_t bits, const int manBits)
{
	const uint32_t man = bits & ((1u << manBits) - 1);
	const int32_t exp = static_cast<int32_t>(bits >> manBits) & 0x1F;
	if (exp == 0)
		return std::ldexp(static_cast<float>(man), -14 - manBits);
	if (exp == 31)
		return 0.0f;
	return std::ldexp(static_cast<float>(man | (1u << manBits)), exp - 15 - manBits);
}

bool TextureReadback_ReadRgb(void* const pTexture, const int x, const int y, const int w, const int h, std::vector<float>& rgb)
{
	ID3D12Resource* const pRes = DlssSr_ResourceFromTexture(pTexture);
	if (!pRes)
	{
		Warning(eDLL_T::CLIENT, "[READBACK] texture %p has no D3D12 resource\n", pTexture);
		return false;
	}

	bool bOk = false;
	ID3D12Device* pDevice = nullptr;
	ID3D12Resource* pBuffer = nullptr;
	const D3D12_RESOURCE_DESC desc = pRes->GetDesc();
	const DXGI_FORMAT fmt = desc.Format;
	const int bpp = fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8
		: fmt == DXGI_FORMAT_R32G32B32A32_FLOAT ? 16
		: fmt == DXGI_FORMAT_R11G11B10_FLOAT ? 4 : 0;

	do
	{
		if (!bpp)
		{
			Warning(eDLL_T::CLIENT, "[READBACK] unsupported format %u\n", static_cast<unsigned>(fmt));
			break;
		}
		if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > static_cast<int>(desc.Width) || y + h > static_cast<int>(desc.Height))
		{
			Warning(eDLL_T::CLIENT, "[READBACK] rect %d,%d %dx%d outside %llux%u\n", x, y, w, h,
				static_cast<unsigned long long>(desc.Width), desc.Height);
			break;
		}
		if (FAILED(pRes->GetDevice(IID_PPV_ARGS(&pDevice))) || !TextureReadback_Ensure(pDevice))
			break;

		D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot = {};
		UINT rows = 0;
		UINT64 rowBytes = 0, total = 0;
		pDevice->GetCopyableFootprints(&desc, 0, 1, 0, &foot, &rows, &rowBytes, &total);

		D3D12_HEAP_PROPERTIES hp = {};
		hp.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = total;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(pDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&pBuffer))))
			break;

		// The RHI returns every resource to its registered state between batches.
		const D3D12_RESOURCE_STATES resting = static_cast<D3D12_RESOURCE_STATES>(
			DlssSr_ResourceRestingState(pRes, D3D12_RESOURCE_STATE_RENDER_TARGET));

		s_pAlloc->Reset();
		s_pList->Reset(s_pAlloc, nullptr);
		D3D12_RESOURCE_BARRIER bar = {};
		bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		bar.Transition.pResource = pRes;
		bar.Transition.Subresource = 0;
		bar.Transition.StateBefore = resting;
		bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		const bool bTransition = resting != D3D12_RESOURCE_STATE_COPY_SOURCE && resting != D3D12_RESOURCE_STATE_COMMON;
		if (bTransition)
			s_pList->ResourceBarrier(1, &bar);

		D3D12_TEXTURE_COPY_LOCATION dst = {};
		dst.pResource = pBuffer;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst.PlacedFootprint = foot;
		D3D12_TEXTURE_COPY_LOCATION src = {};
		src.pResource = pRes;
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src.SubresourceIndex = 0;
		s_pList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

		if (bTransition)
		{
			bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
			bar.Transition.StateAfter = resting;
			s_pList->ResourceBarrier(1, &bar);
		}
		s_pList->Close();
		ID3D12CommandList* lists[] = { s_pList };
		s_pQueue->ExecuteCommandLists(1, lists);
		s_pQueue->Signal(s_pFence, ++s_nFenceValue);
		if (s_pFence->GetCompletedValue() < s_nFenceValue)
		{
			s_pFence->SetEventOnCompletion(s_nFenceValue, s_hEvent);
			if (WaitForSingleObject(s_hEvent, 5000) != WAIT_OBJECT_0)
			{
				Warning(eDLL_T::CLIENT, "[READBACK] copy did not complete\n");
				break;
			}
		}

		void* pMapped = nullptr;
		D3D12_RANGE all = { 0, static_cast<SIZE_T>(total) };
		if (FAILED(pBuffer->Map(0, &all, &pMapped)) || !pMapped)
			break;
		rgb.assign(static_cast<size_t>(w) * h * 3, 0.0f);
		const uint8_t* const base = static_cast<const uint8_t*>(pMapped) + foot.Offset;
		for (int row = 0; row < h; row++)
		{
			const uint8_t* const line = base + static_cast<size_t>(y + row) * foot.Footprint.RowPitch;
			for (int col = 0; col < w; col++)
			{
				const uint8_t* const px = line + static_cast<size_t>(x + col) * bpp;
				float* const o = &rgb[(static_cast<size_t>(row) * w + col) * 3];
				if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT)
				{
					uint16_t hv[3];
					memcpy(hv, px, sizeof(hv));
					for (int c = 0; c < 3; c++)
						o[c] = TextureReadback_Half(hv[c]);
				}
				else if (fmt == DXGI_FORMAT_R32G32B32A32_FLOAT)
				{
					memcpy(o, px, 3 * sizeof(float));
				}
				else
				{
					uint32_t v;
					memcpy(&v, px, sizeof(v));
					o[0] = TextureReadback_Float11(v & 0x7FF, 6);
					o[1] = TextureReadback_Float11((v >> 11) & 0x7FF, 6);
					o[2] = TextureReadback_Float11((v >> 22) & 0x3FF, 5);
				}
			}
		}
		D3D12_RANGE none = { 0, 0 };
		pBuffer->Unmap(0, &none);
		bOk = true;
	} while (false);

	if (pBuffer)
		pBuffer->Release();
	if (pDevice)
		pDevice->Release();
	pRes->Release();
	return bOk;
}
