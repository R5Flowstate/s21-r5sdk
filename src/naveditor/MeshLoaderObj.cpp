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

#include "NavEditor/Include/MeshLoaderObj.h"

#ifdef _WIN32
#define ftell64 _ftelli64
#define fseeki64 _fseeki64
#else
#define ftell64 ftello
#define fseeki64 fseeko
#endif

rcMeshLoaderObj::rcMeshLoaderObj() :
	m_scale(1.0f),
	m_verts(0),
	m_tris(0),
	m_normals(0),
	m_vertCount(0),
	m_triCount(0)
{
}

rcMeshLoaderObj::~rcMeshLoaderObj()
{
	delete [] m_verts;
	delete [] m_normals;
	delete [] m_tris;
}

void rcMeshLoaderObj::addVertex(float x, float y, float z, int& cap)
{
	if (m_vertCount+1 > cap)
	{
		cap = !cap ? 8 : cap*2;
		rdVec3D* nv = new rdVec3D[cap];
		if (m_vertCount)
			memcpy(nv, m_verts, m_vertCount*sizeof(rdVec3D));
		delete [] m_verts;
		m_verts = nv;
	}
	rdVec3D* dst = &m_verts[m_vertCount];

	dst->x = x * m_scale;
	dst->y = y * m_scale;
	dst->z = z * m_scale;

	m_vertCount++;
}

void rcMeshLoaderObj::addTriangle(int a, int b, int c, int& cap)
{
	if (m_triCount+1 > cap)
	{
		cap = !cap ? 8 : cap*2;
		int* nv = new int[cap*3];
		if (m_triCount)
			memcpy(nv, m_tris, m_triCount*3*sizeof(int));
		delete [] m_tris;
		m_tris = nv;
	}
	int* dst = &m_tris[m_triCount*3];
	*dst++ = a;
	*dst++ = b;
	*dst++ = c;
	m_triCount++;
}

static inline const char* skipLine(const char* p, const char* end)
{
	while (p < end && *p != '\n')
		p++;
	if (p < end)
		p++;
	return p;
}

static inline const char* skipSpaces(const char* p, const char* end)
{
	while (p < end && (*p == ' ' || *p == '\t'))
		p++;
	return p;
}

static inline bool isVertexLine(const char* p, const char* end)
{
	return p + 1 < end && p[0] == 'v' && (p[1] == ' ' || p[1] == '\t');
}

static inline bool isFaceLine(const char* p, const char* end)
{
	return p + 1 < end && p[0] == 'f' && (p[1] == ' ' || p[1] == '\t');
}

// Fast float parser for OBJ data. Handles [-]digits[.digits][e[-]digits].
// No NaN/Inf — OBJ files don't use them.
static inline float fastParseFloat(const char*& p, const char* end)
{
	while (p < end && (*p == ' ' || *p == '\t'))
		p++;

	float sign = 1.0f;
	if (p < end && *p == '-')
	{
		sign = -1.0f;
		p++;
	}
	else if (p < end && *p == '+')
	{
		p++;
	}

	float val = 0.0f;
	while (p < end && *p >= '0' && *p <= '9')
	{
		val = val * 10.0f + (float)(*p - '0');
		p++;
	}

	if (p < end && *p == '.')
	{
		p++;
		float frac = 0.0f;
		float div = 1.0f;
		while (p < end && *p >= '0' && *p <= '9')
		{
			frac = frac * 10.0f + (float)(*p - '0');
			div *= 10.0f;
			p++;
		}
		val += frac / div;
	}

	if (p < end && (*p == 'e' || *p == 'E'))
	{
		p++;
		int expSign = 1;
		if (p < end && *p == '-')
		{
			expSign = -1;
			p++;
		}
		else if (p < end && *p == '+')
		{
			p++;
		}

		int exp = 0;
		while (p < end && *p >= '0' && *p <= '9')
		{
			exp = exp * 10 + (*p - '0');
			p++;
		}

		// Apply exponent via repeated multiply/divide (exponents in OBJ
		// are small, typically < 10, so this is faster than powf).
		float scale = 1.0f;
		for (int i = 0; i < exp; i++)
			scale *= 10.0f;

		val = (expSign > 0) ? val * scale : val / scale;
	}

	return sign * val;
}

static inline int fastParseInt(const char*& p, const char* end)
{
	while (p < end && (*p == ' ' || *p == '\t'))
		p++;

	int sign = 1;
	if (p < end && *p == '-')
	{
		sign = -1;
		p++;
	}

	int val = 0;
	while (p < end && *p >= '0' && *p <= '9')
	{
		val = val * 10 + (*p - '0');
		p++;
	}

	return sign * val;
}

bool rcMeshLoaderObj::load(const std::string& filename)
{
	char* buf = 0;
	FILE* fp = fopen(filename.c_str(), "rb");
	if (!fp)
		return false;
	if (fseeki64(fp, 0, SEEK_END) != 0)
	{
		fclose(fp);
		return false;
	}
	const ssize_t bufSize = ftell64(fp);
	if (bufSize < 0)
	{
		fclose(fp);
		return false;
	}
	if (fseeki64(fp, 0, SEEK_SET) != 0)
	{
		fclose(fp);
		return false;
	}
	buf = new char[bufSize];
	if (!buf)
	{
		fclose(fp);
		return false;
	}
	const size_t readLen = fread(buf, bufSize, 1, fp);
	fclose(fp);

	if (readLen != 1)
	{
		delete[] buf;
		return false;
	}

	const char* src = buf;
	const char* srcEnd = buf + bufSize;

	// Estimate capacity from file size to avoid reallocations.
	// Average OBJ line is ~30-40 bytes. Over-estimate to avoid realloc.
	const int estLines = (int)(bufSize / 28);
	const int estVerts = estLines / 2 + 1;
	const int estTris = estLines / 2 + 1;

	m_verts = new rdVec3D[estVerts];
	m_tris = new int[estTris * 3];

	int vertCap = estVerts;
	int triCap = estTris;

	// Single pass: parse vertices and faces.
	const char* p = src;
	while (p < srcEnd)
	{
		if (isVertexLine(p, srcEnd))
		{
			p += 2; // skip "v "
			float x = fastParseFloat(p, srcEnd);
			float y = fastParseFloat(p, srcEnd);
			float z = fastParseFloat(p, srcEnd);

			if (m_vertCount >= vertCap)
			{
				vertCap = vertCap * 2;
				rdVec3D* nv = new rdVec3D[vertCap];
				memcpy(nv, m_verts, m_vertCount * sizeof(rdVec3D));
				delete[] m_verts;
				m_verts = nv;
			}

			rdVec3D* dst = &m_verts[m_vertCount];
			dst->x = x * m_scale;
			dst->y = y * m_scale;
			dst->z = z * m_scale;
			m_vertCount++;
		}
		else if (isFaceLine(p, srcEnd))
		{
			p += 2; // skip "f "
			int face[32];
			int nv = 0;

			while (p < srcEnd && *p != '\n' && *p != '\r' && nv < 32)
			{
				p = skipSpaces(p, srcEnd);
				if (p >= srcEnd || *p == '\n' || *p == '\r')
					break;

				int vi = fastParseInt(p, srcEnd);

				// Skip texture/normal indices (e.g., "1/2/3").
				while (p < srcEnd && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
					p++;

				face[nv++] = vi < 0 ? vi + m_vertCount : vi - 1;
			}

			for (int i = 2; i < nv; ++i)
			{
				const int a = face[0];
				const int b = face[i-1];
				const int c = face[i];
				if (a < 0 || a >= m_vertCount || b < 0 || b >= m_vertCount || c < 0 || c >= m_vertCount)
					continue;

				if (m_triCount >= triCap)
				{
					triCap = triCap * 2;
					int* nt = new int[triCap * 3];
					memcpy(nt, m_tris, m_triCount * 3 * sizeof(int));
					delete[] m_tris;
					m_tris = nt;
				}

				int* dst = &m_tris[m_triCount * 3];
				dst[0] = a;
				dst[1] = b;
				dst[2] = c;
				m_triCount++;
			}
		}

		p = skipLine(p, srcEnd);
	}

	delete [] buf;

	// Calculate normals in parallel.
	m_normals = new rdVec3D[m_triCount];
	{
		const int numWorkers = rdMax(1, (int)std::thread::hardware_concurrency() - 1);
		std::atomic<int> nextChunk(0);
		const int chunkSize = 4096;
		const int triCount = m_triCount;

		auto worker = [&]()
		{
			for (;;)
			{
				const int start = nextChunk.fetch_add(chunkSize);
				if (start >= triCount)
					break;
				const int end = rdMin(start + chunkSize, triCount);
				for (int i = start; i < end; i++)
				{
					const rdVec3D* v0 = &m_verts[m_tris[i*3]];
					const rdVec3D* v1 = &m_verts[m_tris[i*3+1]];
					const rdVec3D* v2 = &m_verts[m_tris[i*3+2]];
					rdTriNormal(v0, v1, v2, &m_normals[i]);
				}
			}
		};

		std::vector<std::thread> workers;
		for (int i = 0; i < numWorkers; i++)
			workers.emplace_back(worker);
		for (auto& w : workers)
			w.join();
	}

	m_filename = filename;
	return true;
}
