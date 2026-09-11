#pragma once

#pragma pack(push, 1)
class CShaderGlue // Most of these found in CShaderGlue::SetupShader
{
public:
	void* m_pVTable; //0x0000
	char pad_0008[8]; //0x0008 Dispatcher Context, Some SEH try and catch thing.
	uint64_t m_nUnknown1; //0x0010
	uint16_t m_nCount1; //0x0018
	uint16_t m_nTextureInputCount; //0x001A
	uint16_t m_nNumSamplers; //0x001C [ PIXIE ]: Used by ID3D11DeviceContext::PSSetSamplers to set NumSamplers
	uint8_t m_nStartSlot; //0x001E [ PIXIE ]: Used by ID3D11DeviceContext::PSSetShaderResources to set StartSlot.
	uint8_t m_nNumViews; //0x001F [ PIXIE ]: Used by ID3D11DeviceContext::PSSetShaderResources to set NumViews.
	uint8_t m_nByte1; //0x0020 [ PIXIE ]: No clue tbh the only usage is an example I will show below. All of the usages are cmp also..?
	uint8_t pad_0021[15]; //0x0021
	void** m_ppVertexShader; //0x0030 [ PIXIE ]: Points to another structure which holds a double ptr to d3d11.dll
	void** m_ppPixelShader; //0x0038 [ PIXIE ]: Points to another structure which holds a double ptr to d3d11.dll
}; //Size: 0x0040
static_assert(sizeof(CShaderGlue) == 0x40); // [ PIXIE ]: All vars have proper datatype size, paddings are unknown.
#pragma pack(pop)

/*
  if ( *(_BYTE *)(v19 + a1 + 32) != ) // (v19 + a1 + 32) = m_nByte1
  {
	 = *(_BYTE *)(v19 + a1 + 32);
	 = -1i64;
  }
*/


