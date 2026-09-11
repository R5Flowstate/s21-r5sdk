#include "tier1/keyvalues.h"
#include "engine/cmodel_bsp.h"
#include "materialsystem/texturestreaming.h"

//---------------------------------------------------------------------------------
// Purpose: loads and processes STBSP files
// (overrides level name if stbsp field has value in prerequisites file)
// Input: *pszLevelName - 
//---------------------------------------------------------------------------------
static void StreamDB_Init(const char* const pszLevelName)
{
	KeyValues* const pSettingsKV = Mod_GetLevelCoreSettings(pszLevelName);
	const char* targetStreamDB = pszLevelName;

	if (pSettingsKV)
	{
		KeyValues* const pStreamKV = pSettingsKV->FindKey("StreamDB");

		if (pStreamKV)
			targetStreamDB = pStreamKV->GetString();

		pSettingsKV->DeleteThis();
	}

	v_StreamDB_Init(targetStreamDB);

	// If the requested STBSP file doesn't exist, then enable the GPU driven
	// texture streaming system.
	const bool gpuDriven = s_textureStreamMgr->fileHandle == FS_ASYNC_FILE_INVALID;

	if (gpu_driven_tex_stream->GetBool() != gpuDriven)
		gpu_driven_tex_stream->SetValue(gpuDriven);

	if (!gpuDriven)
		Msg(eDLL_T::MS, "StreamDB_Init: Loaded STBSP file '%s.stbsp'\n", targetStreamDB);
}

//---------------------------------------------------------------------------------
// Purpose: shift and scale the texture's histogram to accommodate varying screen
// FOV, screen resolutions and texture resolutions.
// Input: *taskList - 
//---------------------------------------------------------------------------------
static void StreamDB_CreditWorldTextures(TextureStreamMgr_TaskList_s* const taskList)
{
	// If we use the GPU driven texture streaming system, do not credit the textures
	// based on the STBSP pages.
	if (gpu_driven_tex_stream->GetBool())
		return;

	v_StreamDB_CreditWorldTextures(taskList);
}

//---------------------------------------------------------------------------------
// Purpose: same as above, except for older (legacy) STBSP's (v8.0).
// Input: *taskList - 
//---------------------------------------------------------------------------------
static void StreamDB_CreditWorldTextures_Legacy(TextureStreamMgr_TaskList_s* const taskList)
{
	// If we use the GPU driven texture streaming system, do not credit the textures
	// based on the STBSP pages.
	if (gpu_driven_tex_stream->GetBool())
		return;

	v_StreamDB_CreditWorldTextures_Legacy(taskList);
}

// New streaming textures array, double the size of TextureStreamMgr_s::streamingTextures.
// There was a high demand for increasing the size of the static streaming textures array,
// as new content was being added to this game while the game (in its original state) was
// already close to the 16384 limit. NOTE that TextureAsset_s::streamedTextureIndex is of
// type s16; the game uses the value -1 to indicate that the texture has no streaming mip,
// so if you end up increasing this to 65536, be aware that the last slot cannot be used.
// Additional note: the increase from 16384 to 32768 did not incur a performance penalty.

