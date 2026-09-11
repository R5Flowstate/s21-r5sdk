//===== Copyright � 1996-2005, Valve Corporation, All rights reserved. ======//
//
// Purpose: Base class for all animating characters and objects.
//
//===========================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "baseanimating.h"
#include "engine/modelinfo.h"
#include "engine/debugoverlay.h"

static const Vector3D s_HullColor[8] =
{
	Vector3D(1.0, 1.0, 1.0),
	Vector3D(1.0, 0.5, 0.5),
	Vector3D(0.5, 1.0, 0.5),
	Vector3D(1.0, 1.0, 0.5),
	Vector3D(0.5, 0.5, 1.0),
	Vector3D(1.0, 0.5, 1.0),
	Vector3D(0.5, 1.0, 1.0),
	Vector3D(1.0, 1.0, 1.0)
};

//-----------------------------------------------------------------------------
// Purpose: Send the current hitboxes for this model to the client ( to compare with
// r_drawentities 3 client side boxes ).
// WARNING: This uses a ton of bandwidth, only use on a listen server
//-----------------------------------------------------------------------------
void CBaseAnimating::DrawServerHitboxes(const float duration /*= 0.0f*/)
{
	const CStudioHdr* const pStudioHdr = GetModelPtr();
	if (!pStudioHdr)
		return;

    const mstudiohitboxset_t* const pSet = pStudioHdr->GetHitboxSet(m_nHitboxSet);
    if (!pSet)
        return;

	const bool bDepthTest = !debug_draw_box_depth_test || !debug_draw_box_depth_test->GetBool();

    for (int i = 0; i < pSet->numhitboxes; i++)
    {
        const mstudiobbox_t* const pBox = pSet->pHitbox(i);

		const int j = (pBox->group % 8);

		const int r = static_cast<int>(255.0f * s_HullColor[j][0]);
		const int g = static_cast<int>(255.0f * s_HullColor[j][1]);
		const int b = static_cast<int>(255.0f * s_HullColor[j][2]);

		matrix3x4_t transforms;
        HitboxToWorldTransforms(pBox->bone, &transforms);

        g_pDebugOverlay->AddTransformedBoxOverlay(transforms, pBox->bbmin, pBox->bbmax, r, g, b, 16, bDepthTest, duration);
    }
}

//-----------------------------------------------------------------------------
// Purpose: draws a slice of this model's hitboxes and reports how many it has
// Note: the S2C batch is capped per server frame, so callers walk the set a
// slice at a time instead of emitting a whole model and having the tail
// silently dropped.
//-----------------------------------------------------------------------------
int CBaseAnimating::DrawServerHitboxRange(const int firstBox, const int maxBoxes, const float duration)
{
	const CStudioHdr* const pStudioHdr = GetModelPtr();
	if (!pStudioHdr)
		return 0;

	const mstudiohitboxset_t* const pSet = pStudioHdr->GetHitboxSet(m_nHitboxSet);
	if (!pSet)
		return 0;

	const int numBoxes = pSet->numhitboxes;
	const bool bDepthTest = !debug_draw_box_depth_test || !debug_draw_box_depth_test->GetBool();

	for (int i = 0; i < maxBoxes; i++)
	{
		const int index = firstBox + i;
		if (index >= numBoxes)
			break;

		const mstudiobbox_t* const pBox = pSet->pHitbox(index);
		const int j = (pBox->group % 8);

		const int r = static_cast<int>(255.0f * s_HullColor[j][0]);
		const int g = static_cast<int>(255.0f * s_HullColor[j][1]);
		const int b = static_cast<int>(255.0f * s_HullColor[j][2]);

		matrix3x4_t transforms;
		HitboxToWorldTransforms(pBox->bone, &transforms);

		g_pDebugOverlay->AddTransformedBoxOverlay(transforms, pBox->bbmin, pBox->bbmax, r, g, b, 16, bDepthTest, duration);
	}

	return numBoxes;
}

void CBaseAnimating::HitboxToWorldTransforms(uint32_t iBone, matrix3x4_t* transforms)
{
    constexpr int index = 285;
    CallVFunc<void>(index, this, iBone, transforms);
}

//-----------------------------------------------------------------------------
// Purpose: sets studio pointer to an updated studiomdl cache
//-----------------------------------------------------------------------------
void CBaseAnimating::LockStudioHdr()
{
	// Populates the 'm_pStudioHdr' field.
	CBaseAnimating__LockStudioHdr(this);
}

CStudioHdr* CBaseAnimating::GetModelPtr(void)
{
	if (!m_pStudioHdr && GetModel())
	{
		LockStudioHdr();
	}
	return (m_pStudioHdr && m_pStudioHdr->IsValid()) ? m_pStudioHdr : nullptr;
}

//-----------------------------------------------------------------------------
// Purpose: flags this entity's animation as script-driven; networked to the
// client via the existing m_animNetworkFlags SendTable prop (DT_BaseAnimating),
// read there by C_BaseAnimating::ScriptIsScriptAnimModifierActive.
//-----------------------------------------------------------------------------
void CBaseAnimating::SetEnableScriptAnimModifier(bool bEnable)
{
	if (bEnable)
		m_animNetworkFlags |= ANF_SCRIPT_ANIM_MODIFIER;
	else
		m_animNetworkFlags &= ~ANF_SCRIPT_ANIM_MODIFIER;
}
