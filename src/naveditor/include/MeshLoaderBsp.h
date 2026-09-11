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
#ifndef MESHLOADER_BSP
#define MESHLOADER_BSP

#include <string>
#include <vector>
#include <NavEditor/Include/MeshLoaderObj.h>

class rcMeshLoaderBsp:public IMeshLoader
{
public:
	rcMeshLoaderBsp() = default;
	// Explicitly disabled copy constructor and copy assignment operator.
	rcMeshLoaderBsp(const rcMeshLoaderBsp&) =delete;
	rcMeshLoaderBsp& operator=(const rcMeshLoaderBsp&) =delete;

	bool load(const std::string& fileName);

	/// Directory of extracted `.rmdl` models, laid out the way RSX writes them
	/// (`<dir>/<name>/<name>.rmdl`). Empty disables static prop collision, in
	/// which case a type-9 leaf is counted and skipped.
	static void setPropModelDir(const std::string& dir);
	static const std::string& getPropModelDir();

	/// Emit a prop's full detail collision instead of just the coarse hull that
	/// encases it. Off by default: the envelope is what obstructs an agent, and
	/// it is ~50x cheaper.
	static void setPropDetail(bool detail);

	const rdVec3D* getVerts() const { return m_verts.data(); }
	const rdVec3D* getNormals() const { return m_normals.data(); }
	const int* getTris() const { return m_tris.data(); }
	int getVertCount() const { return m_vertCount; }
	int getTriCount() const { return m_triCount; }
	const std::string& getFileName() const { return m_filename; }

private:
	std::string m_filename;
	float m_scale = 1.0;
	std::vector<rdVec3D> m_verts;
	std::vector<int>  m_tris;
	std::vector<rdVec3D> m_normals;
	int m_vertCount = 0;
	int m_triCount = 0;

	
};

#endif // MESHLOADER_BSP
