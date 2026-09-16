#if defined(CLIENT_DLL)
//=============================================================================//
//
// Purpose: buffered pak encoder
//
//=============================================================================//
#include "tier0/binstream.h"
#include "rtech/ipakfile.h"
#include "paktools.h"
#include "pakencode.h"
#include "engine/client/net_bridge_addrs.h"
#include <new>

//-----------------------------------------------------------------------------
// determines whether encoding had failed
//-----------------------------------------------------------------------------
static bool Pak_HasEncodeFailed(const size_t result)
{
	return static_cast<bool>(ZSTD_isError(result));
}

//-----------------------------------------------------------------------------
// returns the encode error as string
//-----------------------------------------------------------------------------
static const char* Pak_GetEncodeError(const size_t result)
{
	return ZSTD_getErrorName(result);
}

//-----------------------------------------------------------------------------
// encodes the pak file from buffer, we can't do streamed compression as we
// need to know the actual decompress size ahead of time, else the runtime will
// fail as we wouldn't be able to parse the decompressed size from the frame
// header
//-----------------------------------------------------------------------------
bool Pak_BufferToBufferEncode(const uint8_t* const inBuf, const uint64_t inLen,
	uint8_t* const outBuf, const uint64_t outLen, const int level)
{
	// offset to the actual pak data, the main file header shouldn't be
	// compressed
	const size_t dataOffset = sizeof(PakFileHeader_s);

	uint8_t* const dstBuf = outBuf + dataOffset;
	const size_t dstLen = outLen - dataOffset;

	const uint8_t* const srcBuf = inBuf + dataOffset;
	const size_t srcLen = inLen - dataOffset;

	size_t compressSize = NULL;

	compressSize = ZSTD_compress(dstBuf, dstLen, srcBuf, srcLen, level);

	if (Pak_HasEncodeFailed(compressSize))
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: compression failed! [%s]\n",
			__FUNCTION__, Pak_GetEncodeError(compressSize));

		return false;
	}

	PakFileHeader_s* const outHeader = reinterpret_cast<PakFileHeader_s* const>(outBuf);

	// the compressed size includes the entire buffer, even the data we didn't
	// compress like the file header
	outHeader->compressedSize = compressSize + dataOffset;

	// this flag is required for the game's runtime to decide whether or not to
	// decompress the pak, and how; see Pak_ProcessPakFile for more details
	outHeader->flags |= PAK_HEADER_FLAGS_ZSTD_ENCODED;

	return true;
}

//-----------------------------------------------------------------------------
// encodes the pak file from file name
//-----------------------------------------------------------------------------
bool Pak_EncodePakFile(const char* const inPakFile, const char* const outPakFile, const int level)
{
	if (!Pak_CreateWritePath())
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: failed to create output path for pak file '%s'!\n",
			__FUNCTION__, outPakFile);

		return false;
	}

	CIOStream inPakStream;

	if (!inPakStream.Open(inPakFile, CIOStream::Mode_e::Read))
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: failed to open pak file '%s' for read!\n",
			__FUNCTION__, inPakFile);

		return false;
	}

	CIOStream outPakStream;

	if (!outPakStream.Open(outPakFile, CIOStream::Mode_e::Write))
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: failed to open pak file '%s' for write!\n",
			__FUNCTION__, outPakFile);

		return false;
	}

	const size_t fileSize = inPakStream.GetSize();

	// file appears truncated
	if (fileSize <= sizeof(PakFileHeader_s) || fileSize > (2ull << 30))
	{
		Error(eDLL_T::RTECH, NO_ERROR,
			"%s: [PAK-ENCODE] pak '%s' size %zu rejected\n",
			__FUNCTION__, inPakFile, fileSize);
		return false;
	}

	std::unique_ptr<uint8_t[]> inPakBufContainer(new (std::nothrow) uint8_t[fileSize]);
	if (!inPakBufContainer)
	{
		Error(eDLL_T::RTECH, NO_ERROR,
			"%s: [PAK-ENCODE] pak '%s' input alloc failed (%zu)\n",
			__FUNCTION__, inPakFile, fileSize);
		return false;
	}
	uint8_t* const inPakBuf = inPakBufContainer.get();

	inPakStream.Read(inPakBuf, fileSize);
	inPakStream.Close();

	const PakFileHeader_s* const inHeader = reinterpret_cast<PakFileHeader_s* const>(inPakBuf);

	if (inHeader->magic != PAK_HEADER_MAGIC || inHeader->version != PAK_HEADER_VERSION)
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: pak '%s' has incompatible or invalid header!\n",
			__FUNCTION__, inPakFile);

		return false;
	}

	if (inHeader->GetCompressionMode() != PakDecodeMode_e::MODE_DISABLED)
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: pak '%s' is already compressed!\n",
			__FUNCTION__, inPakFile);

		return false;
	}

	if (inHeader->compressedSize != fileSize)
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: pak '%s' appears truncated or corrupt; compressed size: %zu, expected: %zu!\n",
			__FUNCTION__, inPakFile, fileSize, inHeader->compressedSize);

		return false;
	}

	// NOTE: if the paks this particular pak patches have different sizes than
	// current sizes in the patch header, the runtime will crash!
	if (inHeader->patchIndex && !Pak_UpdatePatchHeaders(inPakBuf, outPakFile))
	{
		Warning(eDLL_T::RTECH, "%s: pak '%s' is a patch pak, but the pak(s) it patches weren't found; patch headers not updated!\n",
			__FUNCTION__, inPakFile);
	}

	const size_t outBufSize = inHeader->decompressedSize;
	if (outBufSize <= sizeof(PakFileHeader_s) || outBufSize < fileSize || outBufSize > (2ull << 30))
	{
		Error(eDLL_T::RTECH, NO_ERROR,
			"%s: [PAK-ENCODE] pak '%s' decompressedSize %zu rejected (fileSize %zu)\n",
			__FUNCTION__, inPakFile, outBufSize, fileSize);
		return false;
	}

	std::unique_ptr<uint8_t[]> outPakBufContainer(new (std::nothrow) uint8_t[outBufSize]);
	if (!outPakBufContainer)
	{
		Error(eDLL_T::RTECH, NO_ERROR,
			"%s: [PAK-ENCODE] pak '%s' output alloc failed (%zu)\n",
			__FUNCTION__, inPakFile, outBufSize);
		return false;
	}
	uint8_t* const outPakBuf = outPakBufContainer.get();

	PakFileHeader_s* const outHeader = reinterpret_cast<PakFileHeader_s* const>(outPakBuf);

	// copy the header over
	*outHeader = *inHeader;

	// encoding failed
	if (!Pak_BufferToBufferEncode(inPakBuf, fileSize, outPakBuf, outBufSize, level))
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: failed to compress pak file '%s'!\n",
			__FUNCTION__, inPakFile);

		return false;
	}

	const PakFileHeader_s* const outPakHeader = reinterpret_cast<PakFileHeader_s* const>(outPakBuf);

	Pak_ShowHeaderDetails(outPakHeader);

	// this will be true if the entire buffer has been written
	outPakStream.Write(outPakBuf, outPakHeader->compressedSize);

	Msg(eDLL_T::RTECH, "Compressed pak file to: '%s'\n", outPakFile);
	return true;
}

//=============================================================================//
// S21 Oodle: flags&0x300 0x200. Decoder ~4MB ring; dst = GetCompressedBufferSizeNeeded.
//=============================================================================//

typedef int64_t(*OodleLZ_Compress_t)(int, const uint8_t*, int64_t, uint8_t*,
	uint32_t, const void*, uint64_t, const void*, const void*, const void*);

// OodleLZ_CompressOptions 0x58 bytes.
struct OodleLZ_CompressOptions_s
{
	uint32_t verbosity;               // +0x00
	int32_t  minMatchLen;             // +0x04
	int32_t  seekChunkReset;          // +0x08
	int32_t  seekChunkLen;            // +0x0C
	int32_t  profile;                 // +0x10
	int32_t  dictionarySize;          // +0x14
	int32_t  spaceSpeedTradeoffBytes; // +0x18
	int32_t  unused_maxHuffmans;      // +0x1C
	int32_t  sendQuantumCRCs;         // +0x20
	int32_t  maxLocalDictionarySize;  // +0x24
	int32_t  makeLongRangeMatcher;    // +0x28
	int32_t  matchTableSizeLog2;      // +0x2C
	uint8_t  reserved[0x58 - 0x30];   // +0x30..0x57 (zero in the engine default)
};
static_assert(sizeof(OodleLZ_CompressOptions_s) == 0x58, "OodleLZ_CompressOptions_s must be 0x58 bytes");

// worst-case Oodle output size == OodleLZ_GetCompressedBufferSizeNeeded
// raw + 274 bytes of framing per 256KB block (+ slack).
static uint64_t Pak_OodleCompressBound(const uint64_t rawLen)
{
	const uint64_t blocks = (rawLen + 0x3FFFF) / 0x40000;
	return rawLen + (274 * blocks) + 64;
}

//-----------------------------------------------------------------------------
// encodes the pak body with the in-game Oodle encoder; header stays uncompressed
//-----------------------------------------------------------------------------
bool Pak_BufferToBufferEncodeOodle(const uint8_t* const inBuf, const uint64_t inLen,
	uint8_t* const outBuf, const uint64_t outCap, uint64_t* const outCompressedSize,
	const int compressor, const int level)
{
	// the main file header (0x80) is never compressed; the runtime decoder writes
	// its decoded output starting at base + sizeof(PakFileHeader_s).
	const size_t dataOffset = sizeof(PakFileHeader_s);

	uint8_t* const dstBuf = outBuf + dataOffset;
	const uint64_t dstCap = outCap - dataOffset;

	const uint8_t* const srcBuf = inBuf + dataOffset;
	const uint64_t srcLen = inLen - dataOffset;

	const OodleLZ_Compress_t pCompress =
		reinterpret_cast<OodleLZ_Compress_t>(NetObs_Sym(NetObsSym_t::OodleLZCompress));

	if (!pCompress)
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: failed to resolve in-game OodleLZ_Compress!\n", __FUNCTION__);
		return false;
	}

	OodleLZ_CompressOptions_s opts;
	memset(&opts, 0, sizeof(opts));
	opts.seekChunkLen            = 0x40000;    // engine default (256KB)
	opts.spaceSpeedTradeoffBytes = 256;        // engine default
	opts.maxLocalDictionarySize  = 0x1000000;  // engine default (16MB)
	// --- the two overrides that keep every back-ref inside the ~4MB decode ring
	opts.dictionarySize          = 0x300000;   // 3MB hard match-offset cap (< 4MB ring)
	opts.makeLongRangeMatcher    = 0;          // no long-range (>ring) matches

	Msg(eDLL_T::RTECH, "[OODLE-PAK] encoding %llu body bytes (compressor %d, level %d, dict 0x%X, lrm %d)\n",
		(unsigned long long)srcLen, compressor, level, opts.dictionarySize, opts.makeLongRangeMatcher);

	const int64_t compressedSize = pCompress(compressor, srcBuf, static_cast<int64_t>(srcLen),
		dstBuf, static_cast<uint32_t>(level), &opts, 0, nullptr, nullptr, nullptr);

	if (compressedSize <= 0 || static_cast<uint64_t>(compressedSize) > dstCap)
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: oodle compression failed (ret %lld, cap %llu)!\n",
			__FUNCTION__, static_cast<long long>(compressedSize), (unsigned long long)dstCap);
		return false;
	}

	PakFileHeader_s* const outHeader = reinterpret_cast<PakFileHeader_s*>(outBuf);

	// compressedSize includes the entire file, even the uncompressed 0x80 header
	outHeader->compressedSize = static_cast<uint64_t>(compressedSize) + dataOffset;

	// route the pak through the runtime Oodle streaming decoder at load time
	outHeader->flags |= PAK_HEADER_FLAGS_OODLE_ENCODED;

	if (outCompressedSize)
		*outCompressedSize = outHeader->compressedSize;

	return true;
}

//-----------------------------------------------------------------------------
// oodle-encodes the pak file from file name (S21 native)
//-----------------------------------------------------------------------------
bool Pak_EncodePakFileOodle(const char* const inPakFile, const char* const outPakFile,
	const int compressor, const int level)
{
	if (!Pak_CreateWritePath())
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: failed to create output path for pak file '%s'!\n",
			__FUNCTION__, outPakFile);

		return false;
	}

	CIOStream inPakStream;

	if (!inPakStream.Open(inPakFile, CIOStream::Mode_e::Read))
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: failed to open pak file '%s' for read!\n",
			__FUNCTION__, inPakFile);

		return false;
	}

	CIOStream outPakStream;

	if (!outPakStream.Open(outPakFile, CIOStream::Mode_e::Write))
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: failed to open pak file '%s' for write!\n",
			__FUNCTION__, outPakFile);

		return false;
	}

	const size_t fileSize = inPakStream.GetSize();

	// file appears truncated
	if (fileSize <= sizeof(PakFileHeader_s) || fileSize > (2ull << 30))
	{
		Error(eDLL_T::RTECH, NO_ERROR,
			"%s: [PAK-ENCODE] pak '%s' size %zu rejected\n",
			__FUNCTION__, inPakFile, fileSize);
		return false;
	}

	std::unique_ptr<uint8_t[]> inPakBufContainer(new (std::nothrow) uint8_t[fileSize]);
	if (!inPakBufContainer)
	{
		Error(eDLL_T::RTECH, NO_ERROR,
			"%s: [PAK-ENCODE] pak '%s' input alloc failed (%zu)\n",
			__FUNCTION__, inPakFile, fileSize);
		return false;
	}
	uint8_t* const inPakBuf = inPakBufContainer.get();

	inPakStream.Read(inPakBuf, fileSize);
	inPakStream.Close();

	const PakFileHeader_s* const inHeader = reinterpret_cast<PakFileHeader_s* const>(inPakBuf);

	if (inHeader->magic != PAK_HEADER_MAGIC || inHeader->version != PAK_HEADER_VERSION)
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: pak '%s' has incompatible or invalid header!\n",
			__FUNCTION__, inPakFile);

		return false;
	}

	// already compressed? GetCompressionMode only checks RTech/ZSTD, so test the
	// Oodle bit explicitly as well.
	if (inHeader->GetCompressionMode() != PakDecodeMode_e::MODE_DISABLED ||
		(inHeader->flags & PAK_HEADER_FLAGS_OODLE_ENCODED) != 0)
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: pak '%s' is already compressed!\n",
			__FUNCTION__, inPakFile);

		return false;
	}

	if (inHeader->compressedSize != fileSize)
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: pak '%s' appears truncated or corrupt; size on disk: %zu, header: %zu!\n",
			__FUNCTION__, inPakFile, fileSize, inHeader->compressedSize);

		return false;
	}

	// NOTE: if the paks this particular pak patches have different sizes than
	// current sizes in the patch header, the runtime will crash!
	if (inHeader->patchIndex && !Pak_UpdatePatchHeaders(inPakBuf, outPakFile))
	{
		Warning(eDLL_T::RTECH, "%s: pak '%s' is a patch pak, but the pak(s) it patches weren't found; patch headers not updated!\n",
			__FUNCTION__, inPakFile);
	}

	const uint64_t bodyLen = fileSize - sizeof(PakFileHeader_s);
	const uint64_t outBufSize = sizeof(PakFileHeader_s) + Pak_OodleCompressBound(bodyLen);

	std::unique_ptr<uint8_t[]> outPakBufContainer(new (std::nothrow) uint8_t[outBufSize]);
	if (!outPakBufContainer)
	{
		Error(eDLL_T::RTECH, NO_ERROR,
			"%s: [PAK-ENCODE] pak '%s' output alloc failed (%zu)\n",
			__FUNCTION__, inPakFile, outBufSize);
		return false;
	}
	uint8_t* const outPakBuf = outPakBufContainer.get();

	PakFileHeader_s* const outHeader = reinterpret_cast<PakFileHeader_s* const>(outPakBuf);

	// copy the 0x80 header verbatim (patch/streaming/page fields preserved; the
	// body decompresses byte-identically so all in-body offsets stay valid)
	*outHeader = *inHeader;

	uint64_t finalSize = 0;

	// encoding failed
	if (!Pak_BufferToBufferEncodeOodle(inPakBuf, fileSize, outPakBuf, outBufSize, &finalSize, compressor, level))
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: failed to oodle-compress pak file '%s'!\n",
			__FUNCTION__, inPakFile);

		return false;
	}

	Pak_ShowHeaderDetails(reinterpret_cast<const PakFileHeader_s*>(outPakBuf));

	// this will be true if the entire buffer has been written
	outPakStream.Write(outPakBuf, finalSize);

	Msg(eDLL_T::RTECH, "Oodle-compressed pak '%s' -> '%s' (%zu -> %llu bytes, %.1f%% of original)\n",
		inPakFile, outPakFile, fileSize, (unsigned long long)finalSize,
		fileSize ? (100.0 * static_cast<double>(finalSize) / static_cast<double>(fileSize)) : 0.0);

	return true;
}
#else // !CLIENT_DLL
//=============================================================================//
//
// Purpose: buffered pak encoder
//
//=============================================================================//
#include "tier0/binstream.h"
#include "rtech/ipakfile.h"
#include "paktools.h"
#include "pakencode.h"
#include <new>

//-----------------------------------------------------------------------------
// determines whether encoding had failed
//-----------------------------------------------------------------------------
static bool Pak_HasEncodeFailed(const size_t result)
{
	return static_cast<bool>(ZSTD_isError(result));
}

//-----------------------------------------------------------------------------
// returns the encode error as string
//-----------------------------------------------------------------------------
static const char* Pak_GetEncodeError(const size_t result)
{
	return ZSTD_getErrorName(result);
}

//-----------------------------------------------------------------------------
// encodes the pak file from buffer, we can't do streamed compression as we
// need to know the actual decompress size ahead of time, else the runtime will
// fail as we wouldn't be able to parse the decompressed size from the frame
// header
//-----------------------------------------------------------------------------
bool Pak_BufferToBufferEncode(const uint8_t* const inBuf, const uint64_t inLen,
	uint8_t* const outBuf, const uint64_t outLen, const int level)
{
	// offset to the actual pak data, the main file header shouldn't be
	// compressed
	const size_t dataOffset = sizeof(PakFileHeader_s);

	uint8_t* const dstBuf = outBuf + dataOffset;
	const size_t dstLen = outLen - dataOffset;

	const uint8_t* const srcBuf = inBuf + dataOffset;
	const size_t srcLen = inLen - dataOffset;

	size_t compressSize = NULL;

	compressSize = ZSTD_compress(dstBuf, dstLen, srcBuf, srcLen, level);

	if (Pak_HasEncodeFailed(compressSize))
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: compression failed! [%s]\n",
			__FUNCTION__, Pak_GetEncodeError(compressSize));

		return false;
	}

	PakFileHeader_s* const outHeader = reinterpret_cast<PakFileHeader_s* const>(outBuf);

	// the compressed size includes the entire buffer, even the data we didn't
	// compress like the file header
	outHeader->compressedSize = compressSize + dataOffset;

	// this flag is required for the game's runtime to decide whether or not to
	// decompress the pak, and how; see Pak_ProcessPakFile for more details
	outHeader->flags |= PAK_HEADER_FLAGS_ZSTD_ENCODED;

	return true;
}

//-----------------------------------------------------------------------------
// encodes the pak file from file name
//-----------------------------------------------------------------------------
bool Pak_EncodePakFile(const char* const inPakFile, const char* const outPakFile, const int level)
{
	if (!Pak_CreateWritePath())
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: failed to create output path for pak file '%s'!\n",
			__FUNCTION__, outPakFile);

		return false;
	}

	CIOStream inPakStream;

	if (!inPakStream.Open(inPakFile, CIOStream::Mode_e::Read))
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: failed to open pak file '%s' for read!\n",
			__FUNCTION__, inPakFile);

		return false;
	}

	CIOStream outPakStream;

	if (!outPakStream.Open(outPakFile, CIOStream::Mode_e::Write))
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: failed to open pak file '%s' for write!\n",
			__FUNCTION__, outPakFile);

		return false;
	}

	const size_t fileSize = inPakStream.GetSize();

	// file appears truncated
	if (fileSize <= sizeof(PakFileHeader_s) || fileSize > (2ull << 30))
	{
		Error(eDLL_T::RTECH, NO_ERROR,
			"%s: [PAK-ENCODE] pak '%s' size %zu rejected\n",
			__FUNCTION__, inPakFile, fileSize);
		return false;
	}

	std::unique_ptr<uint8_t[]> inPakBufContainer(new (std::nothrow) uint8_t[fileSize]);
	if (!inPakBufContainer)
	{
		Error(eDLL_T::RTECH, NO_ERROR,
			"%s: [PAK-ENCODE] pak '%s' input alloc failed (%zu)\n",
			__FUNCTION__, inPakFile, fileSize);
		return false;
	}
	uint8_t* const inPakBuf = inPakBufContainer.get();

	inPakStream.Read(inPakBuf, fileSize);
	inPakStream.Close();

	const PakFileHeader_s* const inHeader = reinterpret_cast<PakFileHeader_s* const>(inPakBuf);

	if (inHeader->magic != PAK_HEADER_MAGIC || inHeader->version != PAK_HEADER_VERSION)
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: pak '%s' has incompatible or invalid header!\n",
			__FUNCTION__, inPakFile);

		return false;
	}

	if (inHeader->GetCompressionMode() != PakDecodeMode_e::MODE_DISABLED)
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: pak '%s' is already compressed!\n",
			__FUNCTION__, inPakFile);

		return false;
	}

	if (inHeader->compressedSize != fileSize)
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: pak '%s' appears truncated or corrupt; compressed size: %zu, expected: %zu!\n",
			__FUNCTION__, inPakFile, fileSize, inHeader->compressedSize);

		return false;
	}

	// NOTE: if the paks this particular pak patches have different sizes than
	// current sizes in the patch header, the runtime will crash!
	if (inHeader->patchIndex && !Pak_UpdatePatchHeaders(inPakBuf, outPakFile))
	{
		Warning(eDLL_T::RTECH, "%s: pak '%s' is a patch pak, but the pak(s) it patches weren't found; patch headers not updated!\n",
			__FUNCTION__, inPakFile);
	}

	const size_t outBufSize = inHeader->decompressedSize;
	if (outBufSize <= sizeof(PakFileHeader_s) || outBufSize < fileSize || outBufSize > (2ull << 30))
	{
		Error(eDLL_T::RTECH, NO_ERROR,
			"%s: [PAK-ENCODE] pak '%s' decompressedSize %zu rejected (fileSize %zu)\n",
			__FUNCTION__, inPakFile, outBufSize, fileSize);
		return false;
	}

	std::unique_ptr<uint8_t[]> outPakBufContainer(new (std::nothrow) uint8_t[outBufSize]);
	if (!outPakBufContainer)
	{
		Error(eDLL_T::RTECH, NO_ERROR,
			"%s: [PAK-ENCODE] pak '%s' output alloc failed (%zu)\n",
			__FUNCTION__, inPakFile, outBufSize);
		return false;
	}
	uint8_t* const outPakBuf = outPakBufContainer.get();

	PakFileHeader_s* const outHeader = reinterpret_cast<PakFileHeader_s* const>(outPakBuf);

	// copy the header over
	*outHeader = *inHeader;

	// encoding failed
	if (!Pak_BufferToBufferEncode(inPakBuf, fileSize, outPakBuf, outBufSize, level))
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s: failed to compress pak file '%s'!\n",
			__FUNCTION__, inPakFile);

		return false;
	}

	const PakFileHeader_s* const outPakHeader = reinterpret_cast<PakFileHeader_s* const>(outPakBuf);

	Pak_ShowHeaderDetails(outPakHeader);

	// this will be true if the entire buffer has been written
	outPakStream.Write(outPakBuf, outPakHeader->compressedSize);

	Msg(eDLL_T::RTECH, "Compressed pak file to: '%s'\n", outPakFile);
	return true;
}
#endif // CLIENT_DLL
