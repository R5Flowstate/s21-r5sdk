//=====================================================================================//
//
// Purpose
//
// $NoKeywords: $
//=====================================================================================//
#include "core/stdafx.h"


#include "engine/gl_rmain.h"
#include "materialsystem/cmaterialsystem.h"

//-----------------------------------------------------------------------------
// Purpose: compute the scene coordinates of a point in 3D
// Input: &w2sMatrix -
// &point -
// *pClip -
// Output: false if the position if off-screen
//-----------------------------------------------------------------------------
bool ClipTransform(const VMatrix& w2sMatrix, const Vector3D& point, Vector2D* const pClip)
{
	pClip->x = w2sMatrix[0][0] * point.x + w2sMatrix[0][1] * point.y + w2sMatrix[0][2] * point.z + w2sMatrix[0][3];
	pClip->y = w2sMatrix[1][0] * point.x + w2sMatrix[1][1] * point.y + w2sMatrix[1][2] * point.z + w2sMatrix[1][3];

	const float w = w2sMatrix[3][0] * point.x + w2sMatrix[3][1] * point.y + w2sMatrix[3][2] * point.z + w2sMatrix[3][3];

	if (w < 0.001f)
	{
		// Clamp here.
		pClip->x *= 100000;
		pClip->y *= 100000;
		return true;
	}

	const float invw = 1.0f / w;
	pClip->x *= invw;
	pClip->y *= invw;

	return false;
}

//-----------------------------------------------------------------------------
// Purpose: translate point to screen position
// Input: &view -
// &w2sMatrix -
// &point -
// *pClip -
// Output: false if the position if off-screen
//-----------------------------------------------------------------------------
bool ScreenTransform(const CViewSetup& view, const VMatrix& w2sMatrix, const Vector3D& point, Vector2D* const pClip)
{
	const bool bIsOffscreen = ClipTransform(w2sMatrix, point, pClip);

	if (bIsOffscreen)
		return false;

	pClip->x = (view.width * 0.5f) + (pClip->x * view.width) * 0.5f + view.x;
	pClip->y = (view.height * 0.5f) - (pClip->y * view.height) * 0.5f + view.y;

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Given an xy screen pos (0-1), return the screen position 
// Input: view - 
// posX - 
// posY - 
// *pScreen - 
// Output: false if the position if off-screen
//-----------------------------------------------------------------------------
bool ScreenPosition(const CViewSetup& view, const float posX, const float posY, Vector2D* const pScreen)
{
	if (posX > 1.0 || posY > 1.0 || posX < 0.0 || posY < 0.0)
		return false; // Fail.

	pScreen->x = posX * view.width;
	pScreen->y = posY * view.height;

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Given an xy screen pos (0-1), return the screen position 
// Input: view - 
// &pos - 
// *pScreen - 
// Output: false if the position if off-screen
//-----------------------------------------------------------------------------
bool ScreenPosition(const CViewSetup& view, const Vector2D& pos, Vector2D* const pScreen)
{
	return ScreenPosition(view, pos.x, pos.y, pScreen);
}

