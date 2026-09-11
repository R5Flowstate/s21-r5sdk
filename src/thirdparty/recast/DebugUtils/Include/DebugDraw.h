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

#ifndef DEBUGDRAW_H
#define DEBUGDRAW_H
#include "Shared/Include/SharedCommon.h"

enum duDebugDrawPrimitives
{
	DU_DRAW_UNDEFINED,
	DU_DRAW_POINTS,
	DU_DRAW_LINES,
	DU_DRAW_TRIS,
	DU_DRAW_QUADS,
};

/// Abstract debug draw interface.
struct duDebugDraw
{
	virtual ~duDebugDraw() = 0;
	
	virtual void depthMask(bool state) = 0;

	virtual void texture(bool state) = 0;

	/// Begin drawing primitives.
	///  @param prim [in] primitive type to draw, one of rcDebugDrawPrimitives.
	///  @param size [in] size of a primitive, applies to point size and line width only.
	virtual void begin(const duDebugDrawPrimitives prim, const float size = 1.0f, const rdVec3D* offset = 0) = 0;

	/// Submit a vertex
	///  @param pos [in] position of the verts.
	///  @param color [in] color of the verts.
	virtual void vertex(const rdVec3D* pos, unsigned int color) = 0;

	/// Submit a vertex
	///  @param x,y,z [in] position of the verts.
	///  @param color [in] color of the verts.
	virtual void vertex(const float x, const float y, const float z, unsigned int color) = 0;

	/// Submit a vertex
	///  @param pos [in] position of the verts.
	///  @param color [in] color of the verts.
	virtual void vertex(const rdVec3D* pos, unsigned int color, const rdVec2D* uv) = 0;
	
	/// Submit a vertex
	///  @param x,y,z [in] position of the verts.
	///  @param color [in] color of the verts.
	virtual void vertex(const float x, const float y, const float z, unsigned int color, const float u, const float v) = 0;
	
	/// End drawing primitives.
	virtual void end() = 0;

	/// Compute a color for given area.
	virtual unsigned int areaToFaceCol(const unsigned int area) const;
	virtual unsigned int areaToEdgeCol(const unsigned int area) const;
};

inline unsigned int duRGBA(int r, int g, int b, int a)
{
	return ((unsigned int)r) | ((unsigned int)g << 8) | ((unsigned int)b << 16) | ((unsigned int)a << 24);
}

inline unsigned int duRGBAf(float fr, float fg, float fb, float fa)
{
	unsigned char r = (unsigned char)(fr*255.0f);
	unsigned char g = (unsigned char)(fg*255.0f);
	unsigned char b = (unsigned char)(fb*255.0f);
	unsigned char a = (unsigned char)(fa*255.0f);
	return duRGBA(r,g,b,a);
}

unsigned int duIntToCol(int i, int a);
void duIntToCol(int i, float* col);

inline unsigned int duMultCol(const unsigned int col, const unsigned int d)
{
	const unsigned int r = col & 0xff;
	const unsigned int g = (col >> 8) & 0xff;
	const unsigned int b = (col >> 16) & 0xff;
	const unsigned int a = (col >> 24) & 0xff;
	return duRGBA((r*d) >> 8, (g*d) >> 8, (b*d) >> 8, a);
}

inline unsigned int duDarkenCol(unsigned int col)
{
	return ((col >> 1) & 0x007f7f7f) | (col & 0xff000000);
}

inline unsigned int duLerpCol(unsigned int ca, unsigned int cb, unsigned int u)
{
	const unsigned int ra = ca & 0xff;
	const unsigned int ga = (ca >> 8) & 0xff;
	const unsigned int ba = (ca >> 16) & 0xff;
	const unsigned int aa = (ca >> 24) & 0xff;
	const unsigned int rb = cb & 0xff;
	const unsigned int gb = (cb >> 8) & 0xff;
	const unsigned int bb = (cb >> 16) & 0xff;
	const unsigned int ab = (cb >> 24) & 0xff;
	
	unsigned int r = (ra*(255-u) + rb*u)/255;
	unsigned int g = (ga*(255-u) + gb*u)/255;
	unsigned int b = (ba*(255-u) + bb*u)/255;
	unsigned int a = (aa*(255-u) + ab*u)/255;
	return duRGBA(r,g,b,a);
}

inline unsigned int duTransCol(unsigned int c, unsigned int a)
{
	return (a<<24) | (c & 0x00ffffff);
}

const unsigned char* duSetBoxVerts(float minx, float miny, float minz, float maxx,
								   float maxy, float maxz, rdVec3D* verts);

void duCalcBoxColors(unsigned int* colors, unsigned int colTop, unsigned int colSide);

void duDebugDrawCylinderWire(struct duDebugDraw* dd, float minx, float miny, float minz,
							 float maxx, float maxy, float maxz, unsigned int col,
							 const float lineWidth, const rdVec3D* offset);

void duDebugDrawBoxWire(struct duDebugDraw* dd, float minx, float miny, float minz,
						float maxx, float maxy, float maxz, unsigned int col,
						const float lineWidth, const rdVec3D* offset);

void duDebugDrawArc(struct duDebugDraw* dd, const float x0, const float y0, const float z0,
					const float x1, const float y1, const float z1, const float h,
					const float as0, const float as1, unsigned int col,
					const float lineWidth, const rdVec3D* offset);

void duDebugDrawArrow(struct duDebugDraw* dd, const float x0, const float y0, const float z0,
					  const float x1, const float y1, const float z1,
					  const float as0, const float as1, unsigned int col,
					  const float lineWidth, const rdVec3D* offset);

void duDebugDrawCircle(struct duDebugDraw* dd, const float x, const float y, const float z,
					   const float r, unsigned int col, const float lineWidth, const rdVec3D* offset);

void duDebugDrawCross(struct duDebugDraw* dd, const float x, const float y, const float z,
					  const float size, unsigned int col, const float lineWidth, const rdVec3D* offset);

void duDebugDrawBox(struct duDebugDraw* dd, float minx, float miny, float minz,
					float maxx, float maxy, float maxz, const unsigned int* fcol, const rdVec3D* offset);

void duDebugDrawCylinder(struct duDebugDraw* dd, float minx, float miny, float minz,
						 float maxx, float maxy, float maxz, unsigned int col, const rdVec3D* offset);

void duDebugDrawGridXZ(struct duDebugDraw* dd, const float ox, const float oy, const float oz,
					   const int w, const int h, const float size,
					   const unsigned int col, const float lineWidth, const rdVec3D* offset);
void duDebugDrawGridXY(struct duDebugDraw* dd, const float ox, const float oy, const float oz,
	const int w, const int h, const float size,
	const unsigned int col, const float lineWidth, const rdVec3D* offset);

// Versions without begin/end, can be used to draw multiple primitives.
void duAppendCylinderWire(struct duDebugDraw* dd, float minx, float miny, float minz,
						  float maxx, float maxy, float maxz, unsigned int col);

void duAppendBoxWire(struct duDebugDraw* dd, float minx, float miny, float minz,
					 float maxx, float maxy, float maxz, unsigned int col);

void duAppendBoxPoints(struct duDebugDraw* dd, float minx, float miny, float minz,
					   float maxx, float maxy, float maxz, unsigned int col);

void duAppendArc(struct duDebugDraw* dd, const float x0, const float y0, const float z0,
				 const float x1, const float y1, const float z1, const float h,
				 const float as0, const float as1, unsigned int col);

void duAppendArrow(struct duDebugDraw* dd, const float x0, const float y0, const float z0,
				   const float x1, const float y1, const float z1,
				   const float as0, const float as1, unsigned int col);

void duAppendCircle(struct duDebugDraw* dd, const float x, const float y, const float z,
					const float r, unsigned int col);

void duAppendCross(struct duDebugDraw* dd, const float x, const float y, const float z,
				   const float size, unsigned int col);

void duAppendBox(struct duDebugDraw* dd, float minx, float miny, float minz,
				 float maxx, float maxy, float maxz, const unsigned int* fcol);

void duAppendCylinder(struct duDebugDraw* dd, float minx, float miny, float minz,
					  float maxx, float maxy, float maxz, unsigned int col);


class duDisplayList : public duDebugDraw
{
public:
	struct Segment
	{
		duDebugDrawPrimitives prim;
		float primSize;
		int startIndex;
		int count;
		bool textured;
	};

private:
	rdVec3D* m_pos;
	unsigned int* m_color;
	rdVec2D* m_uv;

	int m_size;
	int m_cap;

	Segment m_curSeg;
	std::vector<Segment> m_segments;

	rdVec3D m_drawOffset;
	bool m_depthMask;
	bool m_textured;

	void resize(int cap);

public:
	duDisplayList(int cap = 512);
	~duDisplayList();
	virtual void depthMask(bool state);
	virtual void texture(bool state);
	virtual void begin(const duDebugDrawPrimitives prim, const float size = 1.0f, const rdVec3D* offset = 0);
	virtual void vertex(const float x, const float y, const float z, unsigned int color);
	virtual void vertex(const rdVec3D* pos, unsigned int color);
	virtual void vertex(const rdVec3D* pos, unsigned int color, const rdVec2D* uv);
	virtual void vertex(const float x, const float y, const float z, unsigned int color, const float u, const float v);
	virtual void end();
	void clear();
	bool empty() const { return m_size == 0; }
	int size() const { return m_size; }
	int segmentCount() const { return (int)m_segments.size(); }
	const Segment& getSegment(int i) const { return m_segments[i]; }
	const rdVec3D* getPositions() const { return m_pos; }
	const unsigned int* getColors() const { return m_color; }
	const rdVec2D* getUVs() const { return m_uv; }
	void draw(struct duDebugDraw* dd);
private:
	// Explicitly disabled copy constructor and copy assignment operator.
	duDisplayList(const duDisplayList&);
	duDisplayList& operator=(const duDisplayList&);
};


#endif // DEBUGDRAW_H
