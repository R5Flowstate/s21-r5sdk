#if defined(CLIENT_DLL)
#ifndef RTECH_PAKENCODE_H
#define RTECH_PAKENCODE_H
#include "rtech/ipakfile.h"

bool Pak_BufferToBufferEncode(const uint8_t* const inBuf, const uint64_t inLen,
	uint8_t* const outBuf, const uint64_t outLen, const int level);

bool Pak_EncodePakFile(const char* const inPakFile, const char* const outPakFile, const int level);

// S21 in-game OodleLZ_Compress (flags & 0x300 -> 0x200). No external oo2core.
bool Pak_BufferToBufferEncodeOodle(const uint8_t* const inBuf, const uint64_t inLen,
	uint8_t* const outBuf, const uint64_t outCap, uint64_t* const outCompressedSize,
	const int compressor, const int level);

bool Pak_EncodePakFileOodle(const char* const inPakFile, const char* const outPakFile,
	const int compressor, const int level);

#endif // RTECH_PAKENCODE_H
#else // !CLIENT_DLL
#ifndef RTECH_PAKENCODE_H
#define RTECH_PAKENCODE_H
#include "rtech/ipakfile.h"

bool Pak_BufferToBufferEncode(const uint8_t* const inBuf, const uint64_t inLen,
	uint8_t* const outBuf, const uint64_t outLen, const int level);

bool Pak_EncodePakFile(const char* const inPakFile, const char* const outPakFile, const int level);

#endif // RTECH_PAKENCODE_H
#endif // CLIENT_DLL
