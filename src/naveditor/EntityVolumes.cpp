//=============================================================================//
//
// Purpose: decode a map's out-of-bounds trigger brushes into clip volumes.
//
// The blob behind an entity's `*coll0..N` fields is a mstudiocollmodel_t
// followed by mstudiocollheader_t[headerCount] and the same BVH4 node/leaf
// stream MeshLoaderBsp.cpp walks. Out-of-bounds triggers are brushes, so their
// geometry always arrives as convex-hull leaves (child type 8), which carry
// their own quantised vertices and plane list.
//
//=============================================================================//

#include "NavEditor/Include/EntityVolumes.h"

#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <algorithm>

// The class whose brushes bound play. Everything else in the partition is
// gameplay logic and must not shrink the mesh.
static const char* const kOutOfBoundsClass = "trigger_out_of_bounds";

static const size_t kCollModelSize = 16;
static const size_t kCollHeaderV8Size = 32;
static const size_t kCollHeaderV121Size = 40;

static const size_t kBvhNodeSize = 64;
static const int kBvhChildCount = 4;

static const unsigned int kChildTypeInternal = 0;
static const unsigned int kChildTypeBundle = 3;
static const unsigned int kChildTypeConvexHull = 8;

static const unsigned int kBundleDescLeafTypeShift = 8;
static const unsigned int kBundleDescLeafTypeMask = 0xFF;
static const unsigned int kBundleDescSizeShift = 16;
static const int kBundleMaxDepth = 8;

static const size_t kHullHeaderSize = 20;
static const size_t kHullVertSize = 6;

// Two hull corners that project onto the same spot in XY are the same polygon
// point. A brush quantised at 1/32 of a unit never lands closer than this.
static const float kXyWeldEpsilon = 0.5f;

//-----------------------------------------------------------------------------
// Purpose: RFC 4648 decode; the collision chunks carry no padding variance
//-----------------------------------------------------------------------------
static bool Base64Decode(const std::string& in, std::vector<unsigned char>& out)
{
	static const char* const kAlphabet =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	int val = 0;
	int bits = -8;
	for (const char c : in)
	{
		if (c == '=')
			break;
		const char* p = strchr(kAlphabet, c);
		if (!p || c == '\0')
		{
			if (isspace(static_cast<unsigned char>(c)))
				continue;
			return false;
		}
		val = (val << 6) + static_cast<int>(p - kAlphabet);
		bits += 6;
		if (bits >= 0)
		{
			out.push_back(static_cast<unsigned char>((val >> bits) & 0xFF));
			bits -= 8;
		}
	}
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: read a whole file into memory
//-----------------------------------------------------------------------------
static bool ReadWholeFile(const std::string& path, std::string& out)
{
	FILE* fp = fopen(path.c_str(), "rb");
	if (!fp)
		return false;

	if (fseek(fp, 0, SEEK_END) != 0)
	{
		fclose(fp);
		return false;
	}
	const long size = ftell(fp);
	if (size < 0 || fseek(fp, 0, SEEK_SET) != 0)
	{
		fclose(fp);
		return false;
	}

	out.resize(static_cast<size_t>(size));
	const size_t read = size > 0 ? fread(&out[0], 1, static_cast<size_t>(size), fp) : 0;
	fclose(fp);

	out.resize(read);
	return true;
}

struct EntityKeyValues
{
	std::vector<std::pair<std::string, std::string> > pairs;

	const std::string* Find(const char* key) const
	{
		for (size_t i = 0; i < pairs.size(); ++i)
		{
			if (pairs[i].first == key)
				return &pairs[i].second;
		}
		return nullptr;
	}
};

//-----------------------------------------------------------------------------
// Purpose: split a `{ "key" "value" }` partition into entity blocks
//-----------------------------------------------------------------------------
static void ParseEntityPartition(const std::string& text, std::vector<EntityKeyValues>& out)
{
	size_t pos = 0;
	while (true)
	{
		const size_t open = text.find('{', pos);
		if (open == std::string::npos)
			break;
		const size_t close = text.find('}', open);
		if (close == std::string::npos)
			break;

		EntityKeyValues ent;
		size_t it = open;
		while (it < close)
		{
			const size_t k0 = text.find('"', it);
			if (k0 == std::string::npos || k0 >= close) break;
			const size_t k1 = text.find('"', k0 + 1);
			if (k1 == std::string::npos || k1 >= close) break;
			const size_t v0 = text.find('"', k1 + 1);
			if (v0 == std::string::npos || v0 >= close) break;
			const size_t v1 = text.find('"', v0 + 1);
			if (v1 == std::string::npos || v1 > close) break;

			ent.pairs.push_back(std::make_pair(text.substr(k0 + 1, k1 - k0 - 1),
				text.substr(v0 + 1, v1 - v0 - 1)));
			it = v1 + 1;
		}

		out.push_back(ent);
		pos = close + 1;
	}
}

//-----------------------------------------------------------------------------
// Purpose: join `*coll0..N` in numeric order, then decode
//
// bspconv splits collision at a fixed raw-byte chunk size, which is not a
// base64 group boundary for the blob as a whole -- decoding a field on its own
// silently shifts everything after the first chunk.
//-----------------------------------------------------------------------------
static bool GatherCollisionBlob(const EntityKeyValues& ent, std::vector<unsigned char>& out)
{
	std::vector<std::pair<int, const std::string*> > chunks;
	for (size_t i = 0; i < ent.pairs.size(); ++i)
	{
		const std::string& key = ent.pairs[i].first;
		if (key.compare(0, 5, "*coll") != 0)
			continue;

		int index = 0;
		if (key.size() > 5)
			index = atoi(key.c_str() + 5);
		chunks.push_back(std::make_pair(index, &ent.pairs[i].second));
	}

	if (chunks.empty())
		return false;

	std::sort(chunks.begin(), chunks.end());
	for (size_t i = 0; i < chunks.size(); ++i)
	{
		if (!Base64Decode(*chunks[i].second, out))
			return false;
	}
	return true;
}

struct CollTransform
{
	float m[3][3];
	float origin[3];
	float scale;
};

//-----------------------------------------------------------------------------
// Purpose: build the entity's local -> world transform
//
// Source order: yaw about Z, then pitch about Y, then roll about X.
//-----------------------------------------------------------------------------
static void BuildTransform(const float* origin, const float* angles, const float scale, CollTransform& out)
{
	const float deg2rad = 3.14159265358979323846f / 180.0f;
	const float sp = sinf(angles[0] * deg2rad), cp = cosf(angles[0] * deg2rad);
	const float sy = sinf(angles[1] * deg2rad), cy = cosf(angles[1] * deg2rad);
	const float sr = sinf(angles[2] * deg2rad), cr = cosf(angles[2] * deg2rad);

	out.m[0][0] = cp * cy; out.m[0][1] = sr * sp * cy - cr * sy; out.m[0][2] = cr * sp * cy + sr * sy;
	out.m[1][0] = cp * sy; out.m[1][1] = sr * sp * sy + cr * cy; out.m[1][2] = cr * sp * sy - sr * cy;
	out.m[2][0] = -sp;     out.m[2][1] = sr * cp;                out.m[2][2] = cr * cp;

	out.origin[0] = origin[0];
	out.origin[1] = origin[1];
	out.origin[2] = origin[2];
	out.scale = scale;
}

static void TransformPoint(const CollTransform& xf, const float* in, float* out)
{
	const float x = in[0] * xf.scale;
	const float y = in[1] * xf.scale;
	const float z = in[2] * xf.scale;
	out[0] = xf.m[0][0] * x + xf.m[0][1] * y + xf.m[0][2] * z + xf.origin[0];
	out[1] = xf.m[1][0] * x + xf.m[1][1] * y + xf.m[1][2] * z + xf.origin[1];
	out[2] = xf.m[2][0] * x + xf.m[2][1] * y + xf.m[2][2] * z + xf.origin[2];
}

//-----------------------------------------------------------------------------
// Purpose: pick the collheader variant whose offsets land inside the blob
//
// v8 is 32 bytes and v121 is 40; both parse without error at the wrong size,
// so only the resulting offsets tell them apart.
//-----------------------------------------------------------------------------
static size_t ProbeCollHeaderSize(const std::vector<unsigned char>& blob, const int headerCount)
{
	const size_t candidates[2] = { kCollHeaderV8Size, kCollHeaderV121Size };
	for (int c = 0; c < 2; ++c)
	{
		const size_t size = candidates[c];
		if (kCollModelSize + static_cast<size_t>(headerCount) * size > blob.size())
			continue;

		bool ok = true;
		for (int h = 0; h < headerCount && ok; ++h)
		{
			const size_t o = kCollModelSize + static_cast<size_t>(h) * size;
			int32_t fields[4];
			memcpy(fields, blob.data() + o, sizeof(fields));
			float hullScale;
			memcpy(&hullScale, blob.data() + o + size - 4, sizeof(hullScale));

			const int32_t nodeIndex = fields[1];
			const int32_t vertIndex = fields[2];
			const int32_t leafIndex = fields[3];

			ok = vertIndex > 0 && vertIndex <= leafIndex && leafIndex <= nodeIndex &&
				static_cast<size_t>(nodeIndex) < blob.size() &&
				(blob.size() - static_cast<size_t>(nodeIndex)) % kBvhNodeSize == 0 &&
				hullScale > 0.0f && hullScale < 1.0f;
		}
		if (ok)
			return size;
	}
	return 0;
}

//-----------------------------------------------------------------------------
// Purpose: 2D convex hull (monotone chain) of the projected hull corners
//-----------------------------------------------------------------------------
static float Cross2D(const float* o, const float* a, const float* b)
{
	return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
}

static int BuildXyHull(std::vector<std::pair<float, float> >& pts, float outX[MAX_ENTITYVOL_PTS],
	float outY[MAX_ENTITYVOL_PTS])
{
	std::sort(pts.begin(), pts.end());
	pts.erase(std::unique(pts.begin(), pts.end(),
		[](const std::pair<float, float>& a, const std::pair<float, float>& b)
		{
			return fabsf(a.first - b.first) < kXyWeldEpsilon &&
				fabsf(a.second - b.second) < kXyWeldEpsilon;
		}), pts.end());

	const int n = static_cast<int>(pts.size());
	if (n < 3)
		return 0;

	std::vector<float> flat(static_cast<size_t>(n) * 2);
	for (int i = 0; i < n; ++i)
	{
		flat[static_cast<size_t>(i) * 2] = pts[static_cast<size_t>(i)].first;
		flat[static_cast<size_t>(i) * 2 + 1] = pts[static_cast<size_t>(i)].second;
	}

	std::vector<int> hull(static_cast<size_t>(n) * 2);
	int k = 0;
	for (int i = 0; i < n; ++i)
	{
		while (k >= 2 && Cross2D(&flat[static_cast<size_t>(hull[k - 2]) * 2],
			&flat[static_cast<size_t>(hull[k - 1]) * 2], &flat[static_cast<size_t>(i) * 2]) <= 0.0f)
			k--;
		hull[k++] = i;
	}
	for (int i = n - 2, t = k + 1; i >= 0; --i)
	{
		while (k >= t && Cross2D(&flat[static_cast<size_t>(hull[k - 2]) * 2],
			&flat[static_cast<size_t>(hull[k - 1]) * 2], &flat[static_cast<size_t>(i) * 2]) <= 0.0f)
			k--;
		hull[k++] = i;
	}
	k--; // last point repeats the first

	if (k < 3 || k > MAX_ENTITYVOL_PTS)
		return 0;

	for (int i = 0; i < k; ++i)
	{
		outX[i] = flat[static_cast<size_t>(hull[i]) * 2];
		outY[i] = flat[static_cast<size_t>(hull[i]) * 2 + 1];
	}
	return k;
}

struct VolumeCollector
{
	std::vector<EntityClipVolume>* out;
	const CollTransform* xf;
	int dropped = 0;
	int nonPrism = 0;
};

//-----------------------------------------------------------------------------
// Purpose: turn one convex-hull leaf into an extruded clip volume
//
// A trigger brush is nearly always a prism, so the XY hull plus the corner Z
// range reproduces it exactly. The few brushes with a sloped cap have three
// distinct Z levels; extruding those to the full range over-covers, which
// errs toward clipping and is counted so it stays visible.
//-----------------------------------------------------------------------------
static void CollectHullLeaf(const unsigned char* leafBytes, const size_t leafSize,
	const int dwordOffset, VolumeCollector& c)
{
	if (dwordOffset < 0)
		return;
	const size_t byteOffset = static_cast<size_t>(dwordOffset) * sizeof(uint32_t);
	if (byteOffset + kHullHeaderSize > leafSize)
		return;

	const unsigned char* hull = leafBytes + byteOffset;
	const unsigned int vertCount = hull[0];
	if (vertCount < 4)
		return;

	float origin[3];
	float scale;
	memcpy(origin, hull + 4, sizeof(origin));
	memcpy(&scale, hull + 16, sizeof(scale));

	const size_t vertBase = byteOffset + kHullHeaderSize;
	if (vertBase + kHullVertSize * vertCount > leafSize)
		return;

	std::vector<std::pair<float, float> > xy;
	xy.reserve(vertCount);
	float zmin = 1e30f;
	float zmax = -1e30f;
	std::vector<float> zs;
	zs.reserve(vertCount);

	for (unsigned int i = 0; i < vertCount; ++i)
	{
		int16_t q[3];
		memcpy(q, leafBytes + vertBase + kHullVertSize * i, sizeof(q));
		// The engine widens each int16 into the high half of an int32 before
		// scaling, so the effective step is scale * 65536.
		const float local[3] = {
			origin[0] + static_cast<float>(static_cast<int32_t>(q[0]) << 16) * scale,
			origin[1] + static_cast<float>(static_cast<int32_t>(q[1]) << 16) * scale,
			origin[2] + static_cast<float>(static_cast<int32_t>(q[2]) << 16) * scale
		};
		float world[3];
		TransformPoint(*c.xf, local, world);

		xy.push_back(std::make_pair(world[0], world[1]));
		zs.push_back(world[2]);
		if (world[2] < zmin) zmin = world[2];
		if (world[2] > zmax) zmax = world[2];
	}

	std::sort(zs.begin(), zs.end());
	if (std::unique(zs.begin(), zs.end(), [](float a, float b) { return fabsf(a - b) < 1.0f; }) - zs.begin() > 2)
		++c.nonPrism;

	EntityClipVolume vol;
	float hx[MAX_ENTITYVOL_PTS];
	float hy[MAX_ENTITYVOL_PTS];
	vol.nverts = BuildXyHull(xy, hx, hy);
	if (vol.nverts == 0)
	{
		++c.dropped;
		return;
	}

	for (int i = 0; i < vol.nverts; ++i)
		vol.verts[i].init(hx[i], hy[i], zmin);

	vol.hmin = zmin;
	vol.hmax = zmax;
	c.out->push_back(vol);
}

//-----------------------------------------------------------------------------
// Purpose: walk one collision header's BVH, collecting its hull leaves
//-----------------------------------------------------------------------------
static void CollectHeaderVolumes(const std::vector<unsigned char>& blob,
	const int32_t leafIndex, const int32_t nodeIndex, VolumeCollector& c)
{
	if (leafIndex < 0 || nodeIndex <= leafIndex || static_cast<size_t>(nodeIndex) > blob.size())
		return;

	const unsigned char* leafBytes = blob.data() + leafIndex;
	const size_t leafSize = static_cast<size_t>(nodeIndex - leafIndex);
	const int leafDwordCount = static_cast<int>(leafSize / sizeof(uint32_t));

	const size_t nodeBytes = blob.size() - static_cast<size_t>(nodeIndex);
	const int nodeCount = static_cast<int>(nodeBytes / kBvhNodeSize);
	if (nodeCount <= 0 || leafDwordCount <= 0)
		return;

	const unsigned char* nodes = blob.data() + nodeIndex;

	std::vector<bool> visited(static_cast<size_t>(nodeCount), false);
	std::vector<int> stack;
	stack.push_back(0);

	while (!stack.empty())
	{
		const int ni = stack.back();
		stack.pop_back();
		if (ni < 0 || ni >= nodeCount || visited[static_cast<size_t>(ni)])
			continue;
		visited[static_cast<size_t>(ni)] = true;

		uint32_t meta[4];
		memcpy(meta, nodes + static_cast<size_t>(ni) * kBvhNodeSize + 48, sizeof(meta));
		const unsigned int childTypes[kBvhChildCount] = {
			meta[2] & 0xF, (meta[2] >> 4) & 0xF, meta[3] & 0xF, (meta[3] >> 4) & 0xF
		};

		for (int ci = 0; ci < kBvhChildCount; ++ci)
		{
			const unsigned int childIndex = meta[ci] >> 8;
			const unsigned int childType = childTypes[ci];

			if (childType == kChildTypeInternal)
			{
				if (childIndex != 0 && childIndex < static_cast<unsigned int>(nodeCount) &&
					!visited[childIndex])
					stack.push_back(static_cast<int>(childIndex));
				continue;
			}
			if (childType == kChildTypeConvexHull)
			{
				CollectHullLeaf(leafBytes, leafSize, static_cast<int>(childIndex), c);
				continue;
			}
			if (childType != kChildTypeBundle)
				continue;

			std::vector<std::pair<int, int> > work;
			work.push_back(std::make_pair(static_cast<int>(childIndex), 0));
			while (!work.empty())
			{
				const int bo = work.back().first;
				const int depth = work.back().second;
				work.pop_back();

				if (depth > kBundleMaxDepth || bo < 0 || bo >= leafDwordCount)
					continue;

				uint32_t entryCount;
				memcpy(&entryCount, leafBytes + static_cast<size_t>(bo) * 4, sizeof(entryCount));
				if (entryCount == 0 || bo + 1 + static_cast<int>(entryCount) > leafDwordCount)
					continue;

				int payload = bo + 1 + static_cast<int>(entryCount);
				for (unsigned int k = 0; k < entryCount; ++k)
				{
					uint32_t desc;
					memcpy(&desc, leafBytes + static_cast<size_t>(bo + 1 + static_cast<int>(k)) * 4,
						sizeof(desc));
					const unsigned int leafType =
						(desc >> kBundleDescLeafTypeShift) & kBundleDescLeafTypeMask;
					const unsigned int sizeDwords = desc >> kBundleDescSizeShift;

					if (payload >= leafDwordCount)
						break;

					const int po = payload;
					// Unconditional: the engine advances for every entry.
					payload += static_cast<int>(sizeDwords);

					if (leafType == kChildTypeConvexHull)
						CollectHullLeaf(leafBytes, leafSize, po, c);
					else if (leafType == kChildTypeBundle)
						work.push_back(std::make_pair(po, depth + 1));
				}
			}
		}
	}
}

//-----------------------------------------------------------------------------
std::string rcFindScriptEntPath(const std::string& bspPath)
{
	const size_t dot = bspPath.find_last_of('.');
	if (dot == std::string::npos)
		return std::string();

	const std::string candidate = bspPath.substr(0, dot) + "_script.ent";
	FILE* fp = fopen(candidate.c_str(), "rb");
	if (!fp)
		return std::string();

	fclose(fp);
	return candidate;
}

//-----------------------------------------------------------------------------
bool rcLoadEntityClipVolumes(const std::string& entPath, std::vector<EntityClipVolume>& out)
{
	std::string text;
	if (!ReadWholeFile(entPath, text))
	{
		printf("rcLoadEntityClipVolumes: cannot read '%s'\n", entPath.c_str());
		return false;
	}

	std::vector<EntityKeyValues> ents;
	ParseEntityPartition(text, ents);

	int matched = 0;
	int decoded = 0;
	int probeFailed = 0;
	VolumeCollector collector;
	collector.out = &out;

	for (size_t i = 0; i < ents.size(); ++i)
	{
		const std::string* classname = ents[i].Find("classname");
		if (!classname || *classname != kOutOfBoundsClass)
			continue;
		++matched;

		std::vector<unsigned char> blob;
		if (!GatherCollisionBlob(ents[i], blob) || blob.size() < kCollModelSize)
			continue;

		int32_t model[4];
		memcpy(model, blob.data(), sizeof(model));
		const int headerCount = model[3];
		if (headerCount <= 0 || headerCount > 1024)
			continue;

		const size_t headerSize = ProbeCollHeaderSize(blob, headerCount);
		if (headerSize == 0)
		{
			++probeFailed;
			continue;
		}

		float origin[3] = { 0.0f, 0.0f, 0.0f };
		float angles[3] = { 0.0f, 0.0f, 0.0f };
		float entScale = 1.0f;
		if (const std::string* s = ents[i].Find("origin"))
			sscanf(s->c_str(), "%f %f %f", &origin[0], &origin[1], &origin[2]);
		if (const std::string* s = ents[i].Find("angles"))
			sscanf(s->c_str(), "%f %f %f", &angles[0], &angles[1], &angles[2]);
		if (const std::string* s = ents[i].Find("scale"))
			sscanf(s->c_str(), "%f", &entScale);

		CollTransform xf;
		BuildTransform(origin, angles, entScale, xf);
		collector.xf = &xf;

		for (int h = 0; h < headerCount; ++h)
		{
			const size_t o = kCollModelSize + static_cast<size_t>(h) * headerSize;
			int32_t fields[4];
			memcpy(fields, blob.data() + o, sizeof(fields));
			CollectHeaderVolumes(blob, fields[3], fields[1], collector);
		}
		++decoded;
	}

	printf("rcLoadEntityClipVolumes: '%s' -- %d %s entities, %d decoded, %zu volumes\n",
		entPath.c_str(), matched, kOutOfBoundsClass, decoded, out.size());
	if (collector.nonPrism != 0)
	{
		printf("  %d volume(s) are not flat-capped prisms; extruded to the full corner Z "
			"range, which over-covers slightly\n", collector.nonPrism);
	}
	if (collector.dropped != 0 || probeFailed != 0)
	{
		printf("rcLoadEntityClipVolumes: WARNING %d hull(s) dropped (too many points or "
			"degenerate), %d entity blob(s) failed the collheader probe\n",
			collector.dropped, probeFailed);
	}
	return true;
}
