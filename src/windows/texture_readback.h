#pragma once
#ifndef DEDICATED
#include <vector>

// Reads a rectangle of an engine render target back to the CPU as linear RGB
// floats, on a command queue of our own. The caller guarantees the GPU has
// finished writing the texture and nothing is writing it now.
bool TextureReadback_ReadRgb(void* pTexture, int x, int y, int w, int h, std::vector<float>& rgb);

#endif // !DEDICATED
