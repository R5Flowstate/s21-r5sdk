//
// Copyright (c) 2009-2010 Mikko Mononen memon@inside.org
//
// This software is provided 'as-is', without any express or implied
// warranty.  In no event will the authors be held liable for any damages
// arising from the use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.
//

#include "NavEditor/Include/MeshLoaderBsp.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// rBSP dedicated-server sidecar lump ids (lowercase hex in filenames).
static const unsigned int kLumpVertices = 0x0003;
static const unsigned int kLumpModels = 0x000e;
static const unsigned int kLumpBvhNodes = 0x0012;
static const unsigned int kLumpBvhLeafData = 0x0013;

// BVH node: 48 bytes quantised bounds + 4x uint32 packed child metadata.
static const size_t kBvhNodeSize = 64;
static const int kBvhChildCount = 4;

// packedMetaData[i] >> 8 -> child index into node array or leaf DWORD stream.
// child types 0=internal, 1=empty, 4..7=polygon leaf, 3=bundle, 8=hull, 9=prop.
static const unsigned int kChildTypeInternal = 0;
static const unsigned int kChildTypeEmpty = 1;
static const unsigned int kChildTypeBundle = 3;
static const unsigned int kChildTypePolyLeafMin = 4;
static const unsigned int kChildTypePolyLeafMax = 7;
static const unsigned int kChildTypeConvexHull = 8;
static const unsigned int kChildTypeStaticProp = 9;

// Polygon leaf header DWORD.
// polyCount = ((h >> 12) & 0xF) + 1; baseVertex = (h >> 16) << 10.
static const unsigned int kLeafPolyCountShift = 12;
static const unsigned int kLeafPolyCountMask = 0xF;
static const unsigned int kLeafBaseVertexShift = 16;
static const unsigned int kLeafBaseVertexAlign = 10;

// The leaf type nibble carries two independent bits on top of "this is a
// polygon leaf", and both change how the entries decode:
//   bit 0  vertices are int16, decoded through the BVH's own origin/scale
//   bit 1  entries are parallelograms, not triangles
static const unsigned int kLeafTypeBitInt16 = 1;
static const unsigned int kLeafTypeBitQuad = 2;

// Per-poly packed DWORD. A triangle entry spends 11 bits on the v0 delta and
// puts v1/v2 at 11/20; a parallelogram entry spends 10 and puts them at 10/19.
static const unsigned int kPolyTriV0DeltaMask = 0x7FF;
static const unsigned int kPolyTriV1Shift = 11;
static const unsigned int kPolyTriV2Shift = 20;
static const unsigned int kPolyQuadV0DeltaMask = 0x3FF;
static const unsigned int kPolyQuadV1Shift = 10;
static const unsigned int kPolyQuadV2Shift = 19;
static const unsigned int kPolyVMask = 0x1FF;

// Convex hull leaf (child type 8): u8 vertCount, u8 planeCount, u8 triLeafCount,
// u8 quadLeafCount, float3 origin, float scale, then int16[3] verts, then three
// u8 vertex indices per plane, padded to the next DWORD, then the polygon leaves.
// The plane triples define planes, not a triangulation -- the surface comes from
// the trailing leaves, which index the hull's own vertices.
static const unsigned int kHullHeaderSize = 20;
static const unsigned int kHullVertSize = 6;
static const unsigned int kHullPlaneSize = 3;

// Bundle leaf (child type 3): count N, N descriptor DWORDs, then N payloads.
// Descriptor: bits 0-7 contents-mask index (ignored), 8-15 leaf type, 16-31 size in DWORDs.
// Payload cursor advances for every entry -- never conditional on filter/type/decode.
static const unsigned int kBundleDescLeafTypeShift = 8;
static const unsigned int kBundleDescLeafTypeMask = 0xFF;
static const unsigned int kBundleDescSizeShift = 16;
static const int kBundleMaxDepth = 8;

static const float kAabbExpand = 1.0f;

#pragma pack(push, 1)
struct BvhNodeRaw
{
	int16_t minMax[3][2][4]; // quantised child bounds; not used for soup extract
	uint32_t packedMetaData[4];
};
#pragma pack(pop)

static_assert(sizeof(BvhNodeRaw) == kBvhNodeSize, "BvhNodeRaw must be 64 bytes");

// Interning key for positions the source data does not store: the fourth corner
// of a parallelogram, and every convex-hull vertex.
struct BspVertKey
{
	uint32_t bits[3];
	bool operator==(const BspVertKey& o) const
	{
		return bits[0] == o.bits[0] && bits[1] == o.bits[1] && bits[2] == o.bits[2];
	}
};

struct BspVertKeyHash
{
	size_t operator()(const BspVertKey& k) const
	{
		uint64_t h = 1469598103934665603ULL;
		for (int i = 0; i < 3; ++i)
		{
			h ^= k.bits[i];
			h *= 1099511628211ULL;
		}
		return static_cast<size_t>(h);
	}
};

static BspVertKey makeVertKey(const float* p)
{
	BspVertKey k;
	memcpy(k.bits, p, sizeof(k.bits));
	return k;
}

static bool readLumpBytes(const std::string& bspPath, unsigned int lumpId,
	std::vector<unsigned char>& out, const char* label)
{
	char lower[32];
	char upper[32];
	snprintf(lower, sizeof(lower), ".%04x.bsp_lump", lumpId);
	snprintf(upper, sizeof(upper), ".%04X.bsp_lump", lumpId);

	std::string path = bspPath + lower;
	FILE* fp = fopen(path.c_str(), "rb");
	if (!fp)
	{
		path = bspPath + upper;
		fp = fopen(path.c_str(), "rb");
	}
	if (!fp)
	{
		printf("rcMeshLoaderBsp: missing lump %s (%04x) beside '%s'\n",
			label, lumpId, bspPath.c_str());
		return false;
	}

	if (fseek(fp, 0, SEEK_END) != 0)
	{
		fclose(fp);
		printf("rcMeshLoaderBsp: seek failed for lump %s\n", label);
		return false;
	}
	const long sz = ftell(fp);
	if (sz < 0)
	{
		fclose(fp);
		printf("rcMeshLoaderBsp: size failed for lump %s\n", label);
		return false;
	}
	if (fseek(fp, 0, SEEK_SET) != 0)
	{
		fclose(fp);
		printf("rcMeshLoaderBsp: rewind failed for lump %s\n", label);
		return false;
	}

	out.resize(static_cast<size_t>(sz));
	if (sz > 0 && fread(out.data(), 1, static_cast<size_t>(sz), fp) != static_cast<size_t>(sz))
	{
		fclose(fp);
		printf("rcMeshLoaderBsp: read failed for lump %s\n", label);
		return false;
	}
	fclose(fp);

	printf("rcMeshLoaderBsp: loaded %s lump %04x (%ld bytes)\n", label, lumpId, sz);
	return true;
}

// ---------------------------------------------------------------------------
// Static props.
//
// A type-9 leaf names a prop instead of carrying geometry: its low 24 bits
// index the prop array of the `sprp` game lump, and the collision lives in the
// referenced model at studiohdr.bvhOffset -- byte for byte the same
// mstudiocollmodel_t a brush entity carries in its base64 *coll fields.

static const unsigned int kLumpGameLump = 0x0023;
static const int32_t kSprpId = 0x73707270; // 'sprp'
static const size_t kGameLumpDirEntrySize = 20;
static const size_t kPropDictNameSize = 128;
static const size_t kPropRecordSize = 64;
static const size_t kPropOriginOffset = 8;
static const size_t kPropAnglesOffset = 20;   // pitch, yaw, roll in degrees
static const size_t kPropScaleOffset = 32;
static const size_t kPropTypeOffset = 36;
static const unsigned int kPropIndexMask = 0xFFFFFF;

// studiohdr v16 and v17 share their layout up to and past this field.
static const size_t kStudioBvhOffsetField = 0xD6;
static const size_t kStudioHeaderMinSize = 0xE0;

static const size_t kCollModelSize = 16;
static const size_t kCollHeaderSizeV8 = 32;
static const size_t kCollHeaderSizeV121 = 40;

static std::string s_propModelDir;
static bool s_propDetail = false;

void rcMeshLoaderBsp::setPropModelDir(const std::string& dir) { s_propModelDir = dir; }
const std::string& rcMeshLoaderBsp::getPropModelDir() { return s_propModelDir; }
void rcMeshLoaderBsp::setPropDetail(bool detail) { s_propDetail = detail; }

struct PropPlacement
{
	float origin[3];
	float angles[3];
	float scale;
	unsigned short type;
};

// The engine stores an offset that may be scaled: an odd value means the real
// offset is the even part shifted up by four.
static int studioFixOffset(unsigned short v)
{
	return static_cast<int>((v & 0xFFFE) << (4 * (v & 1)));
}

static bool parseStaticProps(const std::vector<unsigned char>& lump,
	std::vector<std::string>& names, std::vector<PropPlacement>& props)
{
	if (lump.size() < 4)
		return false;

	int32_t subLumpCount = 0;
	memcpy(&subLumpCount, lump.data(), sizeof(subLumpCount));
	if (subLumpCount < 1)
		return false;

	size_t sprpOffset = 0;
	bool found = false;
	for (int i = 0; i < subLumpCount; ++i)
	{
		const size_t base = 4 + static_cast<size_t>(i) * kGameLumpDirEntrySize;
		if (base + kGameLumpDirEntrySize > lump.size())
			return false;
		int32_t id = 0;
		int32_t fileOfs = 0;
		memcpy(&id, lump.data() + base, sizeof(id));
		memcpy(&fileOfs, lump.data() + base + 8, sizeof(fileOfs));
		if (id == kSprpId)
		{
			sprpOffset = static_cast<size_t>(fileOfs);
			found = true;
		}
	}
	if (!found || sprpOffset + 4 > lump.size())
		return false;

	size_t o = sprpOffset;
	int32_t dictCount = 0;
	memcpy(&dictCount, lump.data() + o, sizeof(dictCount));
	o += 4;
	if (dictCount < 0 || o + static_cast<size_t>(dictCount) * kPropDictNameSize > lump.size())
		return false;

	names.reserve(static_cast<size_t>(dictCount));
	for (int i = 0; i < dictCount; ++i)
	{
		const char* raw = reinterpret_cast<const char*>(lump.data() + o);
		size_t len = 0;
		while (len < kPropDictNameSize && raw[len])
			++len;
		names.emplace_back(raw, len);
		o += kPropDictNameSize;
	}

	if (o + 4 > lump.size())
		return false;
	int32_t propCount = 0;
	memcpy(&propCount, lump.data() + o, sizeof(propCount));
	o += 4;
	if (propCount < 0 || o + static_cast<size_t>(propCount) * kPropRecordSize > lump.size())
		return false;

	props.resize(static_cast<size_t>(propCount));
	for (int i = 0; i < propCount; ++i)
	{
		const unsigned char* rec = lump.data() + o + static_cast<size_t>(i) * kPropRecordSize;
		PropPlacement& p = props[static_cast<size_t>(i)];
		memcpy(p.origin, rec + kPropOriginOffset, sizeof(p.origin));
		memcpy(p.angles, rec + kPropAnglesOffset, sizeof(p.angles));
		memcpy(&p.scale, rec + kPropScaleOffset, sizeof(p.scale));
		memcpy(&p.type, rec + kPropTypeOffset, sizeof(p.type));
	}
	return true;
}

// v8 headers are 32 bytes and v121 are 40. Both parse without error at the
// wrong size, so pick the one whose offsets land inside the blob.
static bool probeCollHeaderSize(const unsigned char* blob, size_t size,
	int headerCount, size_t& outSize)
{
	const size_t sizes[2] = { kCollHeaderSizeV8, kCollHeaderSizeV121 };
	for (int s = 0; s < 2; ++s)
	{
		const size_t hs = sizes[s];
		if (kCollModelSize + static_cast<size_t>(headerCount) * hs > size)
			continue;
		bool ok = true;
		for (int h = 0; h < headerCount && ok; ++h)
		{
			const size_t off = kCollModelSize + static_cast<size_t>(h) * hs;
			int32_t idx[4];
			memcpy(idx, blob + off, sizeof(idx));
			float scale = 0.0f;
			memcpy(&scale, blob + off + hs - 4, sizeof(scale));
			const int nodeIndex = idx[1];
			const int vertIndex = idx[2];
			const int leafIndex = idx[3];
			if (!(vertIndex > 0 && vertIndex <= leafIndex && leafIndex <= nodeIndex &&
				static_cast<size_t>(nodeIndex) < size))
				ok = false;
			else if ((size - static_cast<size_t>(nodeIndex)) % kBvhNodeSize != 0)
				ok = false;
			else if (!(scale > 0.0f && scale < 1.0f))
				ok = false;
		}
		if (ok)
		{
			outSize = hs;
			return true;
		}
	}
	return false;
}

// One collision header -> model-local triangles, 9 floats each.
static void decodeCollHeader(const unsigned char* blob, size_t size,
	int nodeIndex, int vertIndex, int leafIndex,
	const float origin[3], float scale, std::vector<float>& out)
{
	if (!(vertIndex > 0 && vertIndex <= leafIndex && leafIndex <= nodeIndex &&
		static_cast<size_t>(nodeIndex) < size))
		return;

	const uint32_t* leaf = reinterpret_cast<const uint32_t*>(blob + leafIndex);
	const int leafCount = static_cast<int>((nodeIndex - leafIndex) / sizeof(uint32_t));
	const unsigned char* nodeBase = blob + nodeIndex;
	const int nodeCount = static_cast<int>((size - static_cast<size_t>(nodeIndex)) / kBvhNodeSize);
	const unsigned char* vertBase = blob + vertIndex;
	const size_t vertBytes = static_cast<size_t>(leafIndex - vertIndex);
	const unsigned int packedVertCount = static_cast<unsigned int>(vertBytes / 6);
	const unsigned int floatVertCount = static_cast<unsigned int>(vertBytes / 12);
	if (leafCount <= 0 || nodeCount <= 0)
		return;

	// One vertex region, two readings: the leaf type's low bit picks int16
	// quantised through this header's origin/scale, or plain float32. Reading
	// every leaf as int16 scatters the float-leaf models far outside themselves.
	auto readVert = [&](unsigned int i, bool packed, float* p) -> bool
	{
		if (packed)
		{
			if (i >= packedVertCount)
				return false;
			int16_t q[3];
			memcpy(q, vertBase + static_cast<size_t>(i) * 6, sizeof(q));
			for (int k = 0; k < 3; ++k)
				p[k] = origin[k] + static_cast<float>(static_cast<int32_t>(q[k]) << 16) * scale;
			return true;
		}
		if (i >= floatVertCount)
			return false;
		memcpy(p, vertBase + static_cast<size_t>(i) * 12, sizeof(float) * 3);
		return true;
	};

	std::vector<float> hullVerts;

	auto emitPolyLeaf = [&](int o, unsigned int leafType, const std::vector<float>* local)
	{
		if (o < 0 || o >= leafCount)
			return;
		const bool isQuad = (leafType & kLeafTypeBitQuad) != 0;
		const bool packed = (leafType & kLeafTypeBitInt16) != 0;
		const unsigned int deltaMask = isQuad ? kPolyQuadV0DeltaMask : kPolyTriV0DeltaMask;
		const unsigned int v1Shift = isQuad ? kPolyQuadV1Shift : kPolyTriV1Shift;
		const unsigned int v2Shift = isQuad ? kPolyQuadV2Shift : kPolyTriV2Shift;

		const uint32_t h = leaf[o];
		const int polyCount = static_cast<int>(((h >> kLeafPolyCountShift) & kLeafPolyCountMask) + 1);
		unsigned int running = (h >> kLeafBaseVertexShift) << kLeafBaseVertexAlign;
		if (o + 1 + polyCount > leafCount)
			return;

		for (int p = 0; p < polyCount; ++p)
		{
			const uint32_t pk = leaf[o + 1 + p];
			running += pk & deltaMask;
			const unsigned int iv[3] = {
				running,
				running + ((pk >> v1Shift) & kPolyVMask) + 1,
				running + ((pk >> v2Shift) & kPolyVMask) + 1
			};

			float v[3][3];
			bool ok = true;
			for (int k = 0; k < 3 && ok; ++k)
			{
				if (local)
				{
					if (iv[k] * 3 + 2 >= local->size())
						ok = false;
					else
						memcpy(v[k], local->data() + iv[k] * 3, sizeof(float) * 3);
				}
				else
				{
					ok = readVert(iv[k], packed, v[k]);
				}
			}
			if (!ok)
				continue;

			for (int k = 0; k < 3; ++k)
				out.insert(out.end(), v[k], v[k] + 3);

			if (!isQuad)
				continue;

			// The stored corners are three of four; the fourth is implied.
			const float p3[3] = {
				v[1][0] + v[2][0] - v[0][0],
				v[1][1] + v[2][1] - v[0][1],
				v[1][2] + v[2][2] - v[0][2]
			};
			out.insert(out.end(), v[2], v[2] + 3);
			out.insert(out.end(), v[1], v[1] + 3);
			out.insert(out.end(), p3, p3 + 3);
		}
	};

	auto emitHull = [&](int dwordOffset)
	{
		const size_t byteOffset = static_cast<size_t>(dwordOffset) * sizeof(uint32_t);
		const size_t leafBytes = static_cast<size_t>(nodeIndex - leafIndex);
		if (dwordOffset < 0 || byteOffset + kHullHeaderSize > leafBytes)
			return;
		const unsigned char* hull = reinterpret_cast<const unsigned char*>(leaf) + byteOffset;
		const unsigned int vertCount = hull[0];
		const unsigned int planeCount = hull[1];
		const unsigned int triLeaves = hull[2];
		const unsigned int quadLeaves = hull[3];

		float ho[3];
		float hs;
		memcpy(ho, hull + 4, sizeof(ho));
		memcpy(&hs, hull + 16, sizeof(hs));

		const size_t vbase = byteOffset + kHullHeaderSize;
		if (vbase + kHullVertSize * vertCount > leafBytes)
			return;

		hullVerts.clear();
		hullVerts.reserve(vertCount * 3);
		for (unsigned int i = 0; i < vertCount; ++i)
		{
			int16_t q[3];
			memcpy(q, reinterpret_cast<const unsigned char*>(leaf) + vbase + kHullVertSize * i, sizeof(q));
			for (int k = 0; k < 3; ++k)
				hullVerts.push_back(ho[k] + static_cast<float>(static_cast<int32_t>(q[k]) << 16) * hs);
		}

		const size_t span = kHullHeaderSize + kHullVertSize * vertCount +
			kHullPlaneSize * planeCount;
		int o = dwordOffset + static_cast<int>((span + 3) / 4);
		const unsigned int total = triLeaves + quadLeaves;
		for (unsigned int k = 0; k < total; ++k)
		{
			if (o < 0 || o >= leafCount)
				break;
			emitPolyLeaf(o, k < triLeaves ? 4u : 6u, &hullVerts);
			o += 1 + static_cast<int>(((leaf[o] >> kLeafPolyCountShift) & kLeafPolyCountMask) + 1);
		}
	};

	std::vector<int> stack;
	std::vector<bool> visited(static_cast<size_t>(nodeCount), false);
	std::vector<std::pair<int, int> > bundleWork;
	stack.push_back(0);

	while (!stack.empty())
	{
		const int ni = stack.back();
		stack.pop_back();
		if (ni < 0 || ni >= nodeCount || visited[static_cast<size_t>(ni)])
			continue;
		visited[static_cast<size_t>(ni)] = true;

		uint32_t meta[4];
		memcpy(meta, nodeBase + static_cast<size_t>(ni) * kBvhNodeSize + 48, sizeof(meta));
		const unsigned int childTypes[kBvhChildCount] = {
			meta[2] & 0xF, (meta[2] >> 4) & 0xF, meta[3] & 0xF, (meta[3] >> 4) & 0xF
		};

		for (int c = 0; c < kBvhChildCount; ++c)
		{
			const unsigned int ci = meta[c] >> 8;
			const unsigned int ct = childTypes[c];

			if (ct == kChildTypeInternal)
			{
				if (ci != 0 && ci < static_cast<unsigned int>(nodeCount) && !visited[ci])
					stack.push_back(static_cast<int>(ci));
			}
			else if (ct >= kChildTypePolyLeafMin && ct <= kChildTypePolyLeafMax)
			{
				emitPolyLeaf(static_cast<int>(ci), ct, nullptr);
			}
			else if (ct == kChildTypeConvexHull)
			{
				emitHull(static_cast<int>(ci));
			}
			else if (ct == kChildTypeBundle)
			{
				bundleWork.clear();
				bundleWork.push_back(std::make_pair(static_cast<int>(ci), 0));
				while (!bundleWork.empty())
				{
					const int bo = bundleWork.back().first;
					const int depth = bundleWork.back().second;
					bundleWork.pop_back();
					if (depth > kBundleMaxDepth || bo < 0 || bo >= leafCount)
						continue;
					const unsigned int entries = leaf[bo];
					if (entries == 0 || bo + 1 + static_cast<int>(entries) > leafCount)
						continue;
					int payload = bo + 1 + static_cast<int>(entries);
					for (unsigned int k = 0; k < entries; ++k)
					{
						const uint32_t desc = leaf[bo + 1 + static_cast<int>(k)];
						const unsigned int lt =
							(desc >> kBundleDescLeafTypeShift) & kBundleDescLeafTypeMask;
						const unsigned int sz = desc >> kBundleDescSizeShift;
						if (payload >= leafCount)
							break;
						const int po = payload;
						payload += static_cast<int>(sz); // unconditional, as the engine does
						if (lt >= kChildTypePolyLeafMin && lt <= kChildTypePolyLeafMax)
							emitPolyLeaf(po, lt, nullptr);
						else if (lt == kChildTypeBundle)
							bundleWork.push_back(std::make_pair(po, depth + 1));
						else if (lt == kChildTypeConvexHull)
							emitHull(po);
					}
				}
			}
		}
	}
}

// Whole model -> local triangles.
static void decodeModelCollision(const std::vector<unsigned char>& file,
	std::vector<float>& out)
{
	if (file.size() < kStudioHeaderMinSize)
		return;
	unsigned short raw = 0;
	memcpy(&raw, file.data() + kStudioBvhOffsetField, sizeof(raw));
	const int bvhOffset = studioFixOffset(raw);
	if (bvhOffset <= 0 || static_cast<size_t>(bvhOffset) + kCollModelSize > file.size())
		return;

	const unsigned char* blob = file.data() + bvhOffset;
	const size_t blobSize = file.size() - static_cast<size_t>(bvhOffset);

	int32_t model[4];
	memcpy(model, blob, sizeof(model));
	const int headerCount = model[3];
	if (headerCount <= 0 || headerCount > 1024)
		return;

	size_t headerSize = 0;
	if (!probeCollHeaderSize(blob, blobSize, headerCount, headerSize))
		return;

	// A model with more than one collision header stores a detailed mesh first
	// and a coarse hull that encases it last. For obstruction the envelope is
	// what an agent hits, and it is far cheaper: across 292 two-header models
	// the first headers total 597,179 triangles against 11,616 for the last.
	const int first = (!s_propDetail && headerCount > 1) ? headerCount - 1 : 0;

	for (int h = first; h < headerCount; ++h)
	{
		const size_t off = kCollModelSize + static_cast<size_t>(h) * headerSize;
		int32_t idx[4];
		memcpy(idx, blob + off, sizeof(idx));
		float origin[3];
		float scale;
		memcpy(origin, blob + off + headerSize - 16, sizeof(origin));
		memcpy(&scale, blob + off + headerSize - 4, sizeof(scale));
		decodeCollHeader(blob, blobSize, idx[1], idx[2], idx[3], origin, scale, out);
	}
}

// An exported model tree does not mirror the asset path: RSX rewrites
// `mdl/pipes/airduct_s_128.rmdl` to `<dir>/mdl/airduct_s_128/airduct_s_128.rmdl`.
// Rather than encode one exporter's shape, index every `.rmdl` under the
// directory once and match on basename.
static std::unordered_map<std::string, std::string> s_propModelIndex;
static bool s_propModelIndexBuilt = false;

static std::string lowerAscii(const std::string& s)
{
	std::string out(s);
	for (size_t i = 0; i < out.size(); ++i)
	{
		if (out[i] >= 'A' && out[i] <= 'Z')
			out[i] = static_cast<char>(out[i] - 'A' + 'a');
	}
	return out;
}

static void buildPropModelIndex(const std::string& dir)
{
	s_propModelIndexBuilt = true;
	s_propModelIndex.clear();
	std::error_code ec;
	fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
	if (ec)
	{
		printf("rcMeshLoaderBsp: cannot walk prop model dir '%s': %s\n",
			dir.c_str(), ec.message().c_str());
		return;
	}
	const fs::recursive_directory_iterator end;
	for (; it != end; it.increment(ec))
	{
		if (ec)
			break;
		if (!it->is_regular_file(ec))
			continue;
		const fs::path& p = it->path();
		if (lowerAscii(p.extension().string()) != ".rmdl")
			continue;
		s_propModelIndex.emplace(lowerAscii(p.filename().string()), p.string());
	}
	printf("rcMeshLoaderBsp: indexed %zu model file(s) under '%s'\n",
		s_propModelIndex.size(), dir.c_str());
}

static bool readPropModelFile(const std::string& dir, const std::string& assetName,
	std::vector<unsigned char>& out)
{
	if (!s_propModelIndexBuilt)
		buildPropModelIndex(dir);

	const size_t slash = assetName.find_last_of("/\\");
	const std::string base = lowerAscii(
		slash == std::string::npos ? assetName : assetName.substr(slash + 1));
	if (base.empty())
		return false;

	auto found = s_propModelIndex.find(base);
	if (found == s_propModelIndex.end())
		return false;

	FILE* fp = fopen(found->second.c_str(), "rb");
	if (!fp)
		return false;
	fseek(fp, 0, SEEK_END);
	const long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (sz <= 0)
	{
		fclose(fp);
		return false;
	}
	out.resize(static_cast<size_t>(sz));
	const bool ok = fread(out.data(), 1, static_cast<size_t>(sz), fp) == static_cast<size_t>(sz);
	fclose(fp);
	return ok;
}

// Source AngleMatrix: Z(yaw) * Y(pitch) * X(roll), then uniform scale.
static void makePropTransform(const PropPlacement& p, float m[9])
{
	const float deg2rad = 3.14159265358979323846f / 180.0f;
	const float sp = sinf(p.angles[0] * deg2rad), cp = cosf(p.angles[0] * deg2rad);
	const float sy = sinf(p.angles[1] * deg2rad), cy = cosf(p.angles[1] * deg2rad);
	const float sr = sinf(p.angles[2] * deg2rad), cr = cosf(p.angles[2] * deg2rad);

	m[0] = cp * cy;                  m[1] = sr * sp * cy - cr * sy;  m[2] = cr * sp * cy + sr * sy;
	m[3] = cp * sy;                  m[4] = sr * sp * sy + cr * cy;  m[5] = cr * sp * sy - sr * cy;
	m[6] = -sp;                      m[7] = sr * cp;                 m[8] = cr * cp;
}

bool rcMeshLoaderBsp::load(const std::string& filename)
{
	m_verts.clear();
	m_tris.clear();
	m_normals.clear();
	m_vertCount = 0;
	m_triCount = 0;
	m_filename.clear();

	std::vector<unsigned char> vertBytes;
	std::vector<unsigned char> nodeBytes;
	std::vector<unsigned char> leafBytes;
	std::vector<unsigned char> modelBytes;

	if (!readLumpBytes(filename, kLumpVertices, vertBytes, "VERTICES"))
		return false;
	if (!readLumpBytes(filename, kLumpBvhNodes, nodeBytes, "BVH_NODES"))
		return false;
	if (!readLumpBytes(filename, kLumpBvhLeafData, leafBytes, "BVH_LEAF_DATA"))
		return false;
	if (!readLumpBytes(filename, kLumpModels, modelBytes, "MODELS"))
		return false;

	if (vertBytes.size() % (sizeof(float) * 3) != 0)
	{
		printf("rcMeshLoaderBsp: VERTICES size %zu not multiple of 12\n", vertBytes.size());
		return false;
	}
	if (nodeBytes.size() % kBvhNodeSize != 0)
	{
		printf("rcMeshLoaderBsp: BVH_NODES size %zu not multiple of %zu\n",
			nodeBytes.size(), kBvhNodeSize);
		return false;
	}
	if (leafBytes.size() % sizeof(uint32_t) != 0)
	{
		printf("rcMeshLoaderBsp: BVH_LEAF_DATA size %zu not multiple of 4\n", leafBytes.size());
		return false;
	}
	if (modelBytes.size() < sizeof(float) * 6)
	{
		printf("rcMeshLoaderBsp: MODELS too small (%zu bytes)\n", modelBytes.size());
		return false;
	}

	// The BVH's vertex pointer starts one element into the lump: slot 0 is a
	// padding record holding garbage (~1e29), and every leaf index is relative
	// to lump[1]. Reading from lump[0] shifts every triangle by one vertex --
	// which stays in range, so triangle counts, the soup AABB and a
	// loader-to-loader byte comparison all still pass.
	const int lumpVertexCount = static_cast<int>(vertBytes.size() / (sizeof(float) * 3));
	if (lumpVertexCount < 2)
	{
		printf("rcMeshLoaderBsp: VERTICES holds %d record(s), need at least 2\n", lumpVertexCount);
		return false;
	}
	const int vertexCount = lumpVertexCount - 1;
	const float* srcVerts = reinterpret_cast<const float*>(vertBytes.data()) + 3;

	const int nodeCount = static_cast<int>(nodeBytes.size() / kBvhNodeSize);
	const BvhNodeRaw* nodes = reinterpret_cast<const BvhNodeRaw*>(nodeBytes.data());

	const int leafDwordCount = static_cast<int>(leafBytes.size() / sizeof(uint32_t));
	const uint32_t* leaf = reinterpret_cast<const uint32_t*>(leafBytes.data());

	const float* modelFloats = reinterpret_cast<const float*>(modelBytes.data());
	const float modelMins[3] = { modelFloats[0], modelFloats[1], modelFloats[2] };
	const float modelMaxs[3] = { modelFloats[3], modelFloats[4], modelFloats[5] };
	const float aabbMins[3] = {
		modelMins[0] - kAabbExpand,
		modelMins[1] - kAabbExpand,
		modelMins[2] - kAabbExpand
	};
	const float aabbMaxs[3] = {
		modelMaxs[0] + kAabbExpand,
		modelMaxs[1] + kAabbExpand,
		modelMaxs[2] + kAabbExpand
	};

	int typeCounts[16] = {};
	int bundleInnerTypes[16] = {};
	int nodesVisited = 0;
	int trisKept = 0;
	int trisDropped = 0;
	int bundleEntries = 0;
	int bundleSizeOk = 0;
	int bundleSizeMismatch = 0;

	std::vector<int> srcTris;
	srcTris.reserve(256 * 1024 * 3);

	std::vector<bool> visited(static_cast<size_t>(nodeCount), false);
	std::vector<int> stack;
	stack.reserve(256);
	stack.push_back(0);

	// Bundle worklist: (leaf DWORD offset, nest depth). Reused across nodes.
	std::vector<std::pair<int, int> > bundleWork;
	bundleWork.reserve(64);

	// Positions appended past the vertex lump: parallelogram fourth corners and
	// convex-hull vertices. Interned so a hull sitting on world geometry welds.
	std::vector<float> extraVerts;
	std::unordered_map<BspVertKey, unsigned int, BspVertKeyHash> vertIntern;
	vertIntern.reserve(static_cast<size_t>(vertexCount) * 2);
	for (int i = 0; i < vertexCount; ++i)
		vertIntern.emplace(makeVertKey(srcVerts + i * 3), static_cast<unsigned int>(i));

	auto vertAt = [&](unsigned int i) -> const float*
	{
		return i < static_cast<unsigned int>(vertexCount)
			? srcVerts + i * 3
			: extraVerts.data() + (i - static_cast<unsigned int>(vertexCount)) * 3;
	};

	auto internVert = [&](const float* p) -> unsigned int
	{
		const BspVertKey key = makeVertKey(p);
		auto it = vertIntern.find(key);
		if (it != vertIntern.end())
			return it->second;
		const unsigned int idx = static_cast<unsigned int>(vertexCount) +
			static_cast<unsigned int>(extraVerts.size() / 3);
		extraVerts.push_back(p[0]);
		extraVerts.push_back(p[1]);
		extraVerts.push_back(p[2]);
		vertIntern.emplace(key, idx);
		return idx;
	};

	auto inBounds = [&](const float* p) -> bool
	{
		return p[0] >= aabbMins[0] && p[0] <= aabbMaxs[0] &&
			p[1] >= aabbMins[1] && p[1] <= aabbMaxs[1] &&
			p[2] >= aabbMins[2] && p[2] <= aabbMaxs[2];
	};

	int quadsExpanded = 0;
	int hullsDecoded = 0;
	int hullTris = 0;
	int int16Leaves = 0;

	// Static props: placements from the game lump, geometry from the models,
	// each model decoded once and instanced by every reference to it.
	std::vector<std::string> propNames;
	std::vector<PropPlacement> propPlacements;
	std::vector<std::vector<float> > propModelTris;
	std::vector<char> propModelTried;
	int propsPlaced = 0;
	int propTris = 0;
	int propsNoModel = 0;
	int propsNoCollision = 0;
	int propRefsSkipped = 0;
	std::vector<std::string> propMissingNames;

	const bool wantProps = !s_propModelDir.empty();
	if (wantProps)
	{
		std::vector<unsigned char> gameLump;
		if (readLumpBytes(filename, kLumpGameLump, gameLump, "GAME_LUMP") &&
			parseStaticProps(gameLump, propNames, propPlacements))
		{
			propModelTris.resize(propNames.size());
			propModelTried.assign(propNames.size(), 0);
			printf("rcMeshLoaderBsp: static props -- %zu model(s), %zu placement(s)\n",
				propNames.size(), propPlacements.size());
		}
		else
		{
			printf("rcMeshLoaderBsp: WARNING could not read static prop placements; "
				"props will be skipped\n");
			propPlacements.clear();
		}
	}

	auto emitStaticProp = [&](int dwordOffset)
	{
		if (dwordOffset < 0 || dwordOffset >= leafDwordCount)
			return;
		if (propPlacements.empty())
		{
			++propRefsSkipped;
			return;
		}

		const unsigned int propIndex = leaf[dwordOffset] & kPropIndexMask;
		if (propIndex >= propPlacements.size())
		{
			++propRefsSkipped;
			return;
		}
		const PropPlacement& prop = propPlacements[propIndex];
		if (prop.type >= propNames.size())
		{
			++propRefsSkipped;
			return;
		}

		std::vector<float>& localTris = propModelTris[prop.type];
		if (!propModelTried[prop.type])
		{
			propModelTried[prop.type] = 1;
			std::vector<unsigned char> file;
			if (!readPropModelFile(s_propModelDir, propNames[prop.type], file))
			{
				++propsNoModel;
				if (propMissingNames.size() < 8)
					propMissingNames.push_back(propNames[prop.type]);
			}
			else
			{
				decodeModelCollision(file, localTris);
				if (localTris.empty())
					++propsNoCollision;
			}
		}
		if (localTris.empty())
			return;

		float m[9];
		makePropTransform(prop, m);
		const float s = prop.scale;

		++propsPlaced;
		const size_t triCount = localTris.size() / 9;
		for (size_t t = 0; t < triCount; ++t)
		{
			const float* src = localTris.data() + t * 9;
			unsigned int idx[3];
			bool ok = true;
			for (int k = 0; k < 3 && ok; ++k)
			{
				const float x = src[k * 3 + 0] * s;
				const float y = src[k * 3 + 1] * s;
				const float z = src[k * 3 + 2] * s;
				const float w[3] = {
					m[0] * x + m[1] * y + m[2] * z + prop.origin[0],
					m[3] * x + m[4] * y + m[5] * z + prop.origin[1],
					m[6] * x + m[7] * y + m[8] * z + prop.origin[2]
				};
				if (!inBounds(w))
					ok = false;
				else
					idx[k] = internVert(w);
			}
			if (!ok)
			{
				++trisDropped;
				continue;
			}
			srcTris.push_back(static_cast<int>(idx[0]));
			srcTris.push_back(static_cast<int>(idx[1]));
			srcTris.push_back(static_cast<int>(idx[2]));
			++trisKept;
			++propTris;
		}
	};

	// `local` remaps a convex hull's leaf-local vertex indices; null for world leaves.
	auto emitPolyLeaf = [&](int o, unsigned int leafType, const std::vector<unsigned int>* local)
	{
		if (o < 0 || o >= leafDwordCount)
		{
			++trisDropped;
			return;
		}

		if (leafType & kLeafTypeBitInt16)
		{
			// No map in hand carries these, so the origin/scale they decode
			// through has never been checked against the engine. Say so loudly
			// rather than emit geometry at guessed positions.
			++int16Leaves;
			printf("rcMeshLoaderBsp: WARNING int16 poly leaf (type %u) at dword %d "
				"not decoded -- BVH origin/scale unverified\n", leafType, o);
			return;
		}

		const bool isQuad = (leafType & kLeafTypeBitQuad) != 0;
		const unsigned int deltaMask = isQuad ? kPolyQuadV0DeltaMask : kPolyTriV0DeltaMask;
		const unsigned int v1Shift = isQuad ? kPolyQuadV1Shift : kPolyTriV1Shift;
		const unsigned int v2Shift = isQuad ? kPolyQuadV2Shift : kPolyTriV2Shift;
		const unsigned int indexLimit = local
			? static_cast<unsigned int>(local->size())
			: static_cast<unsigned int>(vertexCount);

		const uint32_t h = leaf[o];
		const int polyCount = static_cast<int>(((h >> kLeafPolyCountShift) & kLeafPolyCountMask) + 1);
		unsigned int running = (h >> kLeafBaseVertexShift) << kLeafBaseVertexAlign;

		if (o + 1 + polyCount > leafDwordCount)
		{
			trisDropped += polyCount;
			return;
		}

		for (int p = 0; p < polyCount; ++p)
		{
			const uint32_t pk = leaf[o + 1 + p];
			running += pk & deltaMask;
			unsigned int v0 = running;
			unsigned int v1 = running + ((pk >> v1Shift) & kPolyVMask) + 1;
			unsigned int v2 = running + ((pk >> v2Shift) & kPolyVMask) + 1;

			if (v0 >= indexLimit || v1 >= indexLimit || v2 >= indexLimit)
			{
				++trisDropped;
				continue;
			}

			if (local)
			{
				v0 = (*local)[v0];
				v1 = (*local)[v1];
				v2 = (*local)[v2];
			}

			const float* a = vertAt(v0);
			const float* b = vertAt(v1);
			const float* c3 = vertAt(v2);

			if (!inBounds(a) || !inBounds(b) || !inBounds(c3))
			{
				++trisDropped;
				continue;
			}

			srcTris.push_back(static_cast<int>(v0));
			srcTris.push_back(static_cast<int>(v1));
			srcTris.push_back(static_cast<int>(v2));
			++trisKept;

			if (!isQuad)
				continue;

			// The stored corners are three of four; the fourth is implied.
			const float p3[3] = {
				b[0] + c3[0] - a[0],
				b[1] + c3[1] - a[1],
				b[2] + c3[2] - a[2]
			};
			if (!inBounds(p3))
			{
				// Not a parallelogram. Confined to the map's outer shell on
				// every map measured; rejecting keeps the voxel grid sane.
				++trisDropped;
				continue;
			}
			const unsigned int v3 = internVert(p3);
			srcTris.push_back(static_cast<int>(v2));
			srcTris.push_back(static_cast<int>(v1));
			srcTris.push_back(static_cast<int>(v3));
			++trisKept;
			++quadsExpanded;
		}
	};

	std::vector<unsigned int> hullLocal;

	auto emitConvexHull = [&](int dwordOffset)
	{
		if (dwordOffset < 0)
			return;
		const size_t byteOffset = static_cast<size_t>(dwordOffset) * sizeof(uint32_t);
		if (byteOffset + kHullHeaderSize > leafBytes.size())
			return;

		const unsigned char* hull = leafBytes.data() + byteOffset;
		const unsigned int vertCount = hull[0];
		const unsigned int planeCount = hull[1];
		const unsigned int triLeafCount = hull[2];
		const unsigned int quadLeafCount = hull[3];

		float origin[3];
		float scale;
		memcpy(origin, hull + 4, sizeof(origin));
		memcpy(&scale, hull + 16, sizeof(scale));

		const size_t vertBase = byteOffset + kHullHeaderSize;
		if (vertBase + kHullVertSize * vertCount > leafBytes.size())
			return;

		hullLocal.clear();
		hullLocal.reserve(vertCount);
		for (unsigned int i = 0; i < vertCount; ++i)
		{
			int16_t q[3];
			memcpy(q, leafBytes.data() + vertBase + kHullVertSize * i, sizeof(q));
			// The engine widens each int16 into the high half of an int32 before
			// scaling, so the effective step is scale * 65536.
			const float p[3] = {
				origin[0] + static_cast<float>(static_cast<int32_t>(q[0]) << 16) * scale,
				origin[1] + static_cast<float>(static_cast<int32_t>(q[1]) << 16) * scale,
				origin[2] + static_cast<float>(static_cast<int32_t>(q[2]) << 16) * scale
			};
			hullLocal.push_back(internVert(p));
		}

		++hullsDecoded;
		const int before = trisKept;

		const size_t spanBytes = kHullHeaderSize + kHullVertSize * vertCount +
			kHullPlaneSize * planeCount;
		int o = dwordOffset + static_cast<int>((spanBytes + 3) / 4);
		const unsigned int leafTotal = triLeafCount + quadLeafCount;
		for (unsigned int k = 0; k < leafTotal; ++k)
		{
			if (o < 0 || o >= leafDwordCount)
				break;
			emitPolyLeaf(o, k < triLeafCount ? 4u : 6u, &hullLocal);
			o += 1 + static_cast<int>(((leaf[o] >> kLeafPolyCountShift) & kLeafPolyCountMask) + 1);
		}
		hullTris += trisKept - before;
	};

	while (!stack.empty())
	{
		const int ni = stack.back();
		stack.pop_back();

		if (ni < 0 || ni >= nodeCount || visited[static_cast<size_t>(ni)])
			continue;
		visited[static_cast<size_t>(ni)] = true;
		++nodesVisited;

		const BvhNodeRaw& node = nodes[ni];
		const uint32_t meta2 = node.packedMetaData[2];
		const uint32_t meta3 = node.packedMetaData[3];
		const unsigned int childTypes[kBvhChildCount] = {
			meta2 & 0xF,
			(meta2 >> 4) & 0xF,
			meta3 & 0xF,
			(meta3 >> 4) & 0xF
		};

		for (int c = 0; c < kBvhChildCount; ++c)
		{
			const unsigned int childIndex = node.packedMetaData[c] >> 8;
			const unsigned int childType = childTypes[c];

			if (childType < 16)
				++typeCounts[childType];
			else
				++typeCounts[15]; // bucket unknowns into last slot for visibility

			if (childType == kChildTypeInternal)
			{
				if (childIndex == 0)
					continue;
				if (childIndex < static_cast<unsigned int>(nodeCount) &&
					!visited[childIndex])
					stack.push_back(static_cast<int>(childIndex));
				continue;
			}

			if (childType == kChildTypeEmpty)
				continue;

			if (childType >= kChildTypePolyLeafMin && childType <= kChildTypePolyLeafMax)
			{
				emitPolyLeaf(static_cast<int>(childIndex), childType, nullptr);
				continue;
			}

			if (childType == kChildTypeConvexHull)
			{
				emitConvexHull(static_cast<int>(childIndex));
				continue;
			}

			if (childType == kChildTypeStaticProp)
			{
				emitStaticProp(static_cast<int>(childIndex));
				continue;
			}

			if (childType == kChildTypeBundle)
			{
				bundleWork.clear();
				bundleWork.push_back(std::make_pair(static_cast<int>(childIndex), 0));

				while (!bundleWork.empty())
				{
					const int bo = bundleWork.back().first;
					const int depth = bundleWork.back().second;
					bundleWork.pop_back();

					if (depth > kBundleMaxDepth)
						continue;
					if (bo < 0 || bo >= leafDwordCount)
						continue;

					const unsigned int entryCount = leaf[bo];
					if (entryCount == 0)
						continue;
					if (bo + 1 + static_cast<int>(entryCount) > leafDwordCount)
						continue;

					// Payload starts after the descriptor array; advance every entry.
					int payload = bo + 1 + static_cast<int>(entryCount);
					for (unsigned int k = 0; k < entryCount; ++k)
					{
						const uint32_t desc = leaf[bo + 1 + static_cast<int>(k)];
						const unsigned int leafType =
							(desc >> kBundleDescLeafTypeShift) & kBundleDescLeafTypeMask;
						const unsigned int sizeDwords = desc >> kBundleDescSizeShift;

						if (payload >= leafDwordCount)
							break;

						const int po = payload;
						// Unconditional: same as engine -- filter/type never skip this.
						payload += static_cast<int>(sizeDwords);

						++bundleEntries;
						if (leafType < 16)
							++bundleInnerTypes[leafType];
						else
							++bundleInnerTypes[15];

						if (leafType >= kChildTypePolyLeafMin &&
							leafType <= kChildTypePolyLeafMax)
						{
							int selfSpan = 0;
							if (po < leafDwordCount)
							{
								selfSpan = 1 + static_cast<int>(
									((leaf[po] >> kLeafPolyCountShift) & kLeafPolyCountMask) + 1);
							}
							if (selfSpan == static_cast<int>(sizeDwords))
								++bundleSizeOk;
							else
								++bundleSizeMismatch;

							emitPolyLeaf(po, leafType, nullptr);
						}
						else if (leafType == kChildTypeBundle)
						{
							bundleWork.push_back(std::make_pair(po, depth + 1));
						}
						else if (leafType == kChildTypeConvexHull)
						{
							emitConvexHull(po);
						}
						else if (leafType == kChildTypeStaticProp)
						{
							emitStaticProp(po);
						}
					}
				}
				continue;
			}

			// convex hull / static prop / unknown: counted only
			(void)childIndex;
		}
	}

	if (trisKept == 0)
	{
		printf("rcMeshLoaderBsp: zero triangles kept from '%s'\n", filename.c_str());
		return false;
	}

	// Dense remap: only vertices referenced by surviving tris. The index space
	// runs past the vertex lump into the synthesised positions.
	const int totalVertCount = vertexCount + static_cast<int>(extraVerts.size() / 3);
	std::vector<int> remap(static_cast<size_t>(totalVertCount), -1);
	m_verts.reserve(static_cast<size_t>(trisKept));
	m_tris.resize(static_cast<size_t>(trisKept) * 3);

	float soupMins[3] = { 1e30f, 1e30f, 1e30f };
	float soupMaxs[3] = { -1e30f, -1e30f, -1e30f };

	for (int t = 0; t < trisKept; ++t)
	{
		for (int k = 0; k < 3; ++k)
		{
			const int src = srcTris[static_cast<size_t>(t * 3 + k)];
			if (remap[static_cast<size_t>(src)] < 0)
			{
				remap[static_cast<size_t>(src)] = static_cast<int>(m_verts.size());
				const float* v = vertAt(static_cast<unsigned int>(src));
				rdVec3D pt;
				pt.init(v[0], v[1], v[2]);
				m_verts.push_back(pt);
				for (int a = 0; a < 3; ++a)
				{
					if (v[a] < soupMins[a]) soupMins[a] = v[a];
					if (v[a] > soupMaxs[a]) soupMaxs[a] = v[a];
				}
			}
			m_tris[static_cast<size_t>(t * 3 + k)] = remap[static_cast<size_t>(src)];
		}
	}

	m_vertCount = static_cast<int>(m_verts.size());
	m_triCount = trisKept;

	m_normals.resize(static_cast<size_t>(m_triCount));
	for (int i = 0; i < m_triCount; ++i)
	{
		const rdVec3D* v0 = &m_verts[m_tris[static_cast<size_t>(i * 3)]];
		const rdVec3D* v1 = &m_verts[m_tris[static_cast<size_t>(i * 3 + 1)]];
		const rdVec3D* v2 = &m_verts[m_tris[static_cast<size_t>(i * 3 + 2)]];
		rdTriNormal(v0, v1, v2, &m_normals[static_cast<size_t>(i)]);
	}

	m_filename = filename;

	printf(
		"rcMeshLoaderBsp: nodes total %d  nodes visited %d  leaf DWORDs %d  source verts %d\n"
		"  child slots type0=%d type1=%d type3=%d type4=%d type5=%d type6=%d type7=%d type8=%d type9=%d\n",
		nodeCount, nodesVisited, leafDwordCount, vertexCount,
		typeCounts[0], typeCounts[1], typeCounts[3], typeCounts[4], typeCounts[5],
		typeCounts[6], typeCounts[7], typeCounts[8], typeCounts[9]);
	if (bundleEntries != 0)
	{
		printf(
			"  bundle entries %d  (type4=%d type6=%d type8=%d type9=%d)\n"
			"  bundle size check ok=%d mismatch=%d\n",
			bundleEntries,
			bundleInnerTypes[4], bundleInnerTypes[6],
			bundleInnerTypes[8], bundleInnerTypes[9],
			bundleSizeOk, bundleSizeMismatch);
		if (bundleSizeMismatch != 0)
		{
			printf("rcMeshLoaderBsp: WARNING bundle size check mismatch=%d "
				"(payload cursor desync -- triangles after it are suspect)\n",
				bundleSizeMismatch);
		}
	}
	printf(
		"  quads expanded %d  convex hulls %d (%d tris)  synthesised verts %d\n",
		quadsExpanded, hullsDecoded, hullTris, totalVertCount - vertexCount);
	if (wantProps)
	{
		printf("  static props %d placed (%d tris, %s collision)\n",
			propsPlaced, propTris, s_propDetail ? "full detail" : "coarse hull");
		if (propsNoModel != 0)
		{
			printf("rcMeshLoaderBsp: WARNING %d prop model(s) not found under '%s'\n",
				propsNoModel, s_propModelDir.c_str());
			for (size_t i = 0; i < propMissingNames.size(); ++i)
				printf("    missing: %s\n", propMissingNames[i].c_str());
		}
		if (propsNoCollision != 0)
			printf("  %d prop model(s) carry no collision (wires, decals, foliage)\n",
				propsNoCollision);
		if (propRefsSkipped != 0)
			printf("rcMeshLoaderBsp: WARNING %d prop reference(s) unresolved\n",
				propRefsSkipped);
	}
	else if (typeCounts[kChildTypeStaticProp] != 0 ||
		bundleInnerTypes[kChildTypeStaticProp] != 0)
	{
		printf("  static props %d reference(s) NOT decoded (pass --props <model-dir>)\n",
			typeCounts[kChildTypeStaticProp] + bundleInnerTypes[kChildTypeStaticProp]);
	}
	if (int16Leaves != 0)
	{
		printf("rcMeshLoaderBsp: WARNING %d int16 poly leaves skipped\n", int16Leaves);
	}
	printf(
		"  triangles kept %d  dropped %d  unique verts %d\n"
		"  soup AABB mins=(%.3f %.3f %.3f) maxs=(%.3f %.3f %.3f)\n"
		"  model[0] AABB mins=(%.3f %.3f %.3f) maxs=(%.3f %.3f %.3f)\n",
		trisKept, trisDropped, m_vertCount,
		soupMins[0], soupMins[1], soupMins[2],
		soupMaxs[0], soupMaxs[1], soupMaxs[2],
		modelMins[0], modelMins[1], modelMins[2],
		modelMaxs[0], modelMaxs[1], modelMaxs[2]);

	return true;
}
