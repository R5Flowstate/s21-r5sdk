#include "Shared/Include/SharedCommon.h"
#include "Recast/Include/Recast.h"
#include "DebugUtils/Include/RecastDebugDraw.h"
#include "DebugUtils/Include/DetourDebugDraw.h"
#include "NavEditor/Include/EditorInterfaces.h"
#include "NavEditor/Include/PerfTimer.h"

////////////////////////////////////////////////////////////////////////////////////////////////////

PFNGLGENBUFFERSPROC glGenBuffers = nullptr;
PFNGLDELETEBUFFERSPROC glDeleteBuffers = nullptr;
PFNGLBINDBUFFERPROC glBindBuffer = nullptr;
PFNGLBUFFERDATAPROC glBufferData = nullptr;

bool initGLExtensions()
{
	glGenBuffers = (PFNGLGENBUFFERSPROC)SDL_GL_GetProcAddress("glGenBuffers");
	glDeleteBuffers = (PFNGLDELETEBUFFERSPROC)SDL_GL_GetProcAddress("glDeleteBuffers");
	glBindBuffer = (PFNGLBINDBUFFERPROC)SDL_GL_GetProcAddress("glBindBuffer");
	glBufferData = (PFNGLBUFFERDATAPROC)SDL_GL_GetProcAddress("glBufferData");

	return glGenBuffers && glDeleteBuffers && glBindBuffer && glBufferData;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BuildContext::BuildContext() :
	m_messageCount(0),
	m_textPoolSize(0)
{
	memset(m_messages, 0, sizeof(char*) * MAX_MESSAGES);
	resetTimers();
}

// Virtual functions for custom implementations.
void BuildContext::doResetLog()
{
	m_messageCount = 0;
	m_textPoolSize = 0;
}

void BuildContext::doLog(const rcLogCategory category, const char* msg, const rdSizeType len)
{
	if (!len) return;
	if (m_messageCount >= MAX_MESSAGES)
		return;
	char* dst = &m_textPool[m_textPoolSize];
	const rdSizeType n = TEXT_POOL_SIZE - m_textPoolSize;
	if (n < 2)
		return;
	char* cat = dst;
	char* text = dst+1;
	const rdSizeType maxtext = n-1;
	// Store category
	*cat = (char)category;
	// Store message
	const rdSizeType count = rdMin(len+1, maxtext);
	memcpy(text, msg, count);
	text[count-1] = '\0';
	m_textPoolSize += 1 + count;
	m_messages[m_messageCount++] = dst;
}

void BuildContext::doResetTimers()
{
	for (rdSizeType i = 0; i < RC_MAX_TIMERS; ++i)
		m_accTime[i] = -1;
}

void BuildContext::doStartTimer(const rcTimerLabel label)
{
	m_startTime[label] = getPerfTime();
}

void BuildContext::doStopTimer(const rcTimerLabel label)
{
	const rdTimeType endTime = getPerfTime();
	const rdTimeType deltaTime = endTime - m_startTime[label];
	if (m_accTime[label] == -1)
		m_accTime[label] = deltaTime;
	else
		m_accTime[label] += deltaTime;
}

rdTimeType BuildContext::doGetAccumulatedTime(const rcTimerLabel label) const
{
	return getPerfTimeUsec(m_accTime[label]);
}

void BuildContext::dumpLog(const char* format, ...)
{
	// Print header.
	va_list ap;
	va_start(ap, format);
	vprintf(format, ap);
	va_end(ap);
	printf("\n");
	
	// Print messages
	const rdSizeType TAB_STOPS[4] = { 28, 36, 44, 52 };
	for (int i = 0; i < m_messageCount; ++i)
	{
		const char* msg = m_messages[i]+1;
		rdSizeType n = 0;
		while (*msg)
		{
			if (*msg == '\t')
			{
				rdSizeType count = 1;
				for (int j = 0; j < 4; ++j)
				{
					if (n < TAB_STOPS[j])
					{
						count = TAB_STOPS[j] - n;
						break;
					}
				}
				while (--count)
				{
					putchar(' ');
					n++;
				}
			}
			else
			{
				putchar(*msg);
				n++;
			}
			msg++;
		}
		putchar('\n');
	}
}

int BuildContext::getLogCount() const
{
	return m_messageCount;
}

const char* BuildContext::getLogText(const rdSizeType i) const
{
	return m_messages[i]+1;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

class GLCheckerTexture
{
	unsigned int m_texId;
public:
	GLCheckerTexture() : m_texId(0)
	{
	}
	
	~GLCheckerTexture()
	{
		if (m_texId != 0)
			glDeleteTextures(1, &m_texId);
	}
	void bind()
	{
		if (m_texId == 0)
		{
			// Create checker pattern.
			const unsigned int col0 = duRGBA(215,215,215,255);
			const unsigned int col1 = duRGBA(255,255,255,255);
			static const int TSIZE = 64;
			unsigned int data[TSIZE*TSIZE];
			
			glGenTextures(1, &m_texId);
			glBindTexture(GL_TEXTURE_2D, m_texId);

			int level = 0;
			int size = TSIZE;
			while (size > 0)
			{
				for (int y = 0; y < size; ++y)
					for (int x = 0; x < size; ++x)
						data[x+y*size] = (x==0 || y==0) ? col0 : col1;
				glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, size,size, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
				size /= 2;
				level++;
			}
			
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		}
		else
		{
			glBindTexture(GL_TEXTURE_2D, m_texId);
		}
	}
};
GLCheckerTexture g_checkerTexture;


void DebugDrawGL::depthMask(bool state)
{
	glDepthMask(state ? GL_TRUE : GL_FALSE);
}

void DebugDrawGL::texture(bool state)
{
	if (state)
	{
		glEnable(GL_TEXTURE_2D);
		g_checkerTexture.bind();
	}
	else
	{
		glDisable(GL_TEXTURE_2D);
	}
}

void DebugDrawGL::begin(const duDebugDrawPrimitives prim, const float size, const rdVec3D* offset)
{
	switch (prim)
	{
		case DU_DRAW_POINTS:
			glPointSize(size);
			glBegin(GL_POINTS);
			break;
		case DU_DRAW_LINES:
			glLineWidth(size);
			glBegin(GL_LINES);
			break;
		case DU_DRAW_TRIS:
			glBegin(GL_TRIANGLES);
			break;
		case DU_DRAW_QUADS:
			glBegin(GL_QUADS);
			break;

		default:
			rdAssert(0);
			break;
	};

	if (offset)
		rdVcopy(&m_drawOffset,offset);
}

void DebugDrawGL::vertex(const rdVec3D* pos, unsigned int color)
{
	glColor4ubv((GLubyte*)&color);

	rdVec3D opos;
	rdVadd(&opos,pos,&m_drawOffset);

	glVertex3fv((GLfloat*)&opos);
}

void DebugDrawGL::vertex(const float x, const float y, const float z, unsigned int color)
{
	glColor4ubv((GLubyte*)&color);

	rdVec3D opos(x,y,z);
	rdVadd(&opos,&opos,&m_drawOffset);

	glVertex3fv((GLfloat*)&opos);
}

void DebugDrawGL::vertex(const rdVec3D* pos, unsigned int color, const rdVec2D* uv)
{
	glColor4ubv((GLubyte*)&color);
	glTexCoord2fv((GLfloat*)uv);

	rdVec3D opos;
	rdVadd(&opos,pos,&m_drawOffset);

	glVertex3fv((GLfloat*)&opos);
}

void DebugDrawGL::vertex(const float x, const float y, const float z, unsigned int color, const float u, const float v)
{
	glColor4ubv((GLubyte*)&color);
	glTexCoord2f(u,v);

	rdVec3D opos(x,y,z);
	rdVadd(&opos,&opos,&m_drawOffset);

	glVertex3fv((GLfloat*)&opos);
}

void DebugDrawGL::end()
{
	glEnd();
	glLineWidth(1.0f);
	glPointSize(1.0f);

	m_drawOffset.init(0.0f,0.0f,0.0f);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

FileIO::FileIO() :
	m_fp(0),
	m_mode(-1)
{
}

FileIO::~FileIO()
{
	if (m_fp) fclose(m_fp);
}

bool FileIO::openForWrite(const char* path)
{
	if (m_fp) return false;
	m_fp = fopen(path, "wb");
	if (!m_fp) return false;
	m_mode = 1;
	return true;
}

bool FileIO::openForRead(const char* path)
{
	if (m_fp) return false;
	m_fp = fopen(path, "rb");
	if (!m_fp) return false;
	m_mode = 2;
	return true;
}

bool FileIO::isWriting() const
{
	return m_mode == 1;
}

bool FileIO::isReading() const
{
	return m_mode == 2;
}

bool FileIO::write(const void* ptr, const rdSizeType size)
{
	if (!m_fp || m_mode != 1) return false;
	fwrite(ptr, size, 1, m_fp);
	return true;
}

bool FileIO::read(void* ptr, const rdSizeType size)
{
	if (!m_fp || m_mode != 2) return false;
	size_t readLen = fread(ptr, size, 1, m_fp);
	return readLen == 1;
}
