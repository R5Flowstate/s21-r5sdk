//=============================================================================//
//
// Purpose: DEDI map editor model facts read from studio data: whether a model
// carries collision, and its bounds. Lets the model browser filter and size
// its rows without spawning anything.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/vscript.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "vscript/languages/squirrel_re/include/sqstring.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "engine/modelloader.h"
#include "engine/gl_model_private.h"
#include "datacache/mdlcache.h"
#include "vscript_server.h"
#include "mapedit_models.h"

//-----------------------------------------------------------------------------
// Purpose: resolves a precached studio model by name, loading it if needed
//-----------------------------------------------------------------------------
static model_t* MapEditModels_Find(HSQUIRRELVM v)
{
	const SQChar* pszModel = nullptr;
	SQObject obj;
	if (SQ_SUCCEEDED(sq_getstackobj(v, 2, &obj)) && (sq_isstring(obj) || obj._type == OT_ASSET))
		pszModel = _stringval(obj);

	if (!pszModel || !*pszModel || strlen(pszModel) >= MAX_OSPATH)
		return nullptr;
	if (!g_pModelLoader || !CModelLoader__FindModel)
		return nullptr;

	model_t* pModel = nullptr;
	__try
	{
		pModel = reinterpret_cast<model_t*>(CModelLoader__FindModel(g_pModelLoader, pszModel));
		if (pModel && CModelLoader__LoadModel && !(pModel->nLoadFlags & IModelLoader::FMODELLOADER_LOADED))
			CModelLoader__LoadModel(g_pModelLoader, pModel);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Warning(eDLL_T::SERVER, "[MAPEDIT-MDL] model load fault for '%s'\n", pszModel);
		return nullptr;
	}

	if (!pModel || pModel->type != mod_studio || pModel->studio == MDLHANDLE_INVALID)
		return nullptr;
	return pModel;
}

//-----------------------------------------------------------------------------
// Purpose: MapEdit_ModelCollision(asset model) : int
// 1 when the studio data has a physics collide or collision geometry (what a
// SOLID_VPHYSICS prop is built from), 0 when it has neither, -1 when unknown.
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_MapEdit_ModelCollision(HSQUIRRELVM v)
{
	model_t* const pModel = MapEditModels_Find(v);
	if (!pModel || !g_pMDLCache || !CMDLCache__GetVCollide || !CMDLCache__GetPhysicsGeometry)
	{
		sq_pushinteger(v, -1);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const vcollide_t* const pCollide = CMDLCache__GetVCollide(g_pMDLCache, pModel->studio);
	const void* const pGeometry = CMDLCache__GetPhysicsGeometry(g_pMDLCache, pModel->studio);
	const bool bSolid = (pCollide && pCollide->solidCount > 0) || pGeometry;

	sq_pushinteger(v, bSolid ? 1 : 0);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: MapEdit_ModelSize(asset model) : vector -- bounds size, zero when unknown
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_MapEdit_ModelSize(HSQUIRRELVM v)
{
	SQVector3D size = { 0.0f, 0.0f, 0.0f };
	const model_t* const pModel = MapEditModels_Find(v);
	if (pModel)
	{
		const Vector3D ext = pModel->maxs - pModel->mins;
		if (IsFinite(ext.x) && IsFinite(ext.y) && IsFinite(ext.z) && ext.x >= 0.0f && ext.y >= 0.0f && ext.z >= 0.0f)
			size = { ext.x, ext.y, ext.z };
	}

	sq_pushvector(v, &size);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: MapEdit_ModelCenter(asset model) : vector -- bounds centre in model
// space, zero when unknown
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_MapEdit_ModelCenter(HSQUIRRELVM v)
{
	SQVector3D center = { 0.0f, 0.0f, 0.0f };
	const model_t* const pModel = MapEditModels_Find(v);
	if (pModel)
	{
		const Vector3D mid = (pModel->mins + pModel->maxs) * 0.5f;
		if (IsFinite(mid.x) && IsFinite(mid.y) && IsFinite(mid.z))
			center = { mid.x, mid.y, mid.z };
	}

	sq_pushvector(v, &center);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void MapEditModels_RegisterServerFunctions(CSquirrelVM* pVM)
{
	DEFINE_SERVER_SCRIPTFUNC_NAMED(pVM, MapEdit_ModelCollision, "1 when the model's studio data has collision, 0 when it has none, -1 when unknown.", "int", "asset model", false);
	DEFINE_SERVER_SCRIPTFUNC_NAMED(pVM, MapEdit_ModelSize, "Studio model bounds size, zero when unknown.", "vector", "asset model", false);
	DEFINE_SERVER_SCRIPTFUNC_NAMED(pVM, MapEdit_ModelCenter, "Studio model bounds centre in model space, zero when unknown.", "vector", "asset model", false);
}
