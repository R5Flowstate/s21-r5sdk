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

#include "NavEditor/Include/MeshLoaderPly.h"

#ifdef _WIN32
#define ftell64 _ftelli64
#define fseeki64 _fseeki64
#else
#define ftell64 ftello
#define fseeki64 fseeko
#endif

bool rcMeshLoaderPly::load(const std::string& filename)
{
//we expect and only support!
/*
ply
format binary_little_endian 1.0
element vertex %d
property float x
property float y
property float z
element face %d
property list uchar int vertex_index
end_header
*/
	FILE* fp = fopen(filename.c_str(), "rb");
	if (!fp)
		return false;

	if (fseeki64(fp, 0, SEEK_END) != 0)
	{
		fclose(fp);
		return false;
	}
	const long long fileSize = ftell64(fp);
	if (fileSize <= 0)
	{
		fclose(fp);
		return false;
	}
	if (fseeki64(fp, 0, SEEK_SET) != 0)
	{
		fclose(fp);
		return false;
	}

	char* buf = new char[fileSize];
	if (!buf)
	{
		fclose(fp);
		return false;
	}
	if (fread(buf, fileSize, 1, fp) != 1)
	{
		delete[] buf;
		fclose(fp);
		return false;
	}
	fclose(fp);

	const char* p = buf;
	const char* end = buf + fileSize;

	// Helper to read a line from the buffer.
	auto readLine = [&](std::string& out) -> bool
	{
		out.clear();
		while (p < end && *p != '\n' && *p != '\r')
			out += *p++;
		if (p < end && *p == '\r') p++;
		if (p < end && *p == '\n') p++;
		return !out.empty() || p < end;
	};

	std::string line;

	// Parse header.
	readLine(line);
	if (line != "ply")
	{
		delete[] buf;
		return false;
	}
	readLine(line);
	if (line != "format binary_little_endian 1.0")
	{
		delete[] buf;
		return false;
	}

	while (readLine(line))
	{
		if (line == "end_header")
			break;

		if (line.compare(0, 15, "element vertex ") == 0)
		{
			char* endptr;
			const long long val = strtoll(line.c_str() + 15, &endptr, 10);
			if (endptr == line.c_str() + 15 || val <= 0 || val > INT_MAX)
			{
				delete[] buf;
				return false;
			}
			m_vertCount = (int)val;
		}
		else if (line.compare(0, 13, "element face ") == 0)
		{
			char* endptr;
			const long long val = strtoll(line.c_str() + 13, &endptr, 10);
			if (endptr == line.c_str() + 13 || val <= 0 || val > INT_MAX)
			{
				delete[] buf;
				return false;
			}
			m_triCount = (int)val;
		}
	}

	if (m_vertCount <= 0 || m_triCount <= 0 || m_triCount > INT_MAX / 3)
	{
		delete[] buf;
		return false;
	}

	// Validate claimed sizes fit in remaining buffer before allocating.
	const long long vertBytes = (long long)m_vertCount * 3LL * sizeof(float);
	const long long faceBytes = (long long)m_triCount * (1LL + 3LL * sizeof(int));
	if (vertBytes + faceBytes > end - p)
	{
		delete[] buf;
		return false;
	}

	m_verts.resize(m_vertCount);
	m_tris.resize((size_t)m_triCount * 3);

	if (p + vertBytes > end)
	{
		delete[] buf;
		return false;
	}
	memcpy(m_verts.data(), p, vertBytes);
	p += vertBytes;

	// Read faces — each is 1 byte count (must be 3) + 3 ints.
	for (int i = 0; i < m_triCount; i++)
	{
		if (p + 1 + 3 * sizeof(int) > end)
		{
			delete[] buf;
			return false;
		}

		const unsigned char count = (unsigned char)*p++;
		if (count != 3)
		{
			delete[] buf;
			return false;
		}

		memcpy(&m_tris[i * 3], p, 3 * sizeof(int));
		p += 3 * sizeof(int);

		if (m_tris[i * 3] < 0 || m_tris[i * 3] >= m_vertCount ||
			m_tris[i * 3 + 1] < 0 || m_tris[i * 3 + 1] >= m_vertCount ||
			m_tris[i * 3 + 2] < 0 || m_tris[i * 3 + 2] >= m_vertCount)
		{
			delete[] buf;
			return false;
		}
	}

	delete[] buf;

	// Calculate normals in parallel.
	m_normals.resize(m_triCount);
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
