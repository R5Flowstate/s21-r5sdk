#pragma once
#include "iviewrender.h"
#include "view_shared.h"

//-------------------------------------------------------------------------------------
// Forward declarations
//-------------------------------------------------------------------------------------
class CViewRender : public IViewRender
{
public:
	inline float GetZFar() const { return m_CurrentView.zFar; }
	inline float GetZNear() const { return m_CurrentView.zNear; }

	inline float GetFieldOfView() const { return m_CurrentView.fov; }
	inline float GetAspectRatio() const { return ((float)m_CurrentView.width / (float)m_CurrentView.height); }

	const CViewSetup* GetMainView() const { return &m_CurrentView; }

private:
	CViewSetup m_CurrentView;
	bool m_bAllowViewAccess;
};

///////////////////////////////////////////////////////////////////////////////
const Vector3D& MainViewOrigin();
const QAngle& MainViewAngles();

inline Vector3D* g_vecRenderOrigin = nullptr;
inline QAngle* g_vecRenderAngles = nullptr;

inline CViewRender* g_pViewRender = nullptr;

