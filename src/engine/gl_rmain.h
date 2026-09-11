#pragma once
#include "mathlib/vector.h"
#include "mathlib/vmatrix.h"
#include "view_shared.h"

#ifndef DEDICATED

bool ClipTransform(const VMatrix& w2sMatrix, const Vector3D& point, Vector2D* const pClip);
bool ScreenTransform(const CViewSetup& view, const VMatrix& w2sMatrix, const Vector3D& point, Vector2D* const pClip);
bool ScreenPosition(const CViewSetup& view, const float posX, const float posY, Vector2D* const pScreen);
bool ScreenPosition(const CViewSetup& view, const Vector2D& pos, Vector2D* const pScreen);

#endif
