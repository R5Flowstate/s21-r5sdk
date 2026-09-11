#if defined(CLIENT_DLL)
#ifndef RTECH_RUI_H
#define RTECH_RUI_H

struct RuiInstance_s;

enum class RuiArgumentType_e : u8
{
	TYPE_STRING = 0x1,
	TYPE_ASSET = 0x2,
	TYPE_BOOL = 0x3,
	TYPE_INT = 0x4,
	TYPE_FLOAT = 0x5,
	TYPE_FLOAT2 = 0x6,
	TYPE_FLOAT3 = 0x7,
	TYPE_COLOR_ALPHA = 0x8,
	TYPE_GAMETIME = 0x9,
	TYPE_WALLTIME = 0xA,
	TYPE_UIHANDLE = 0xB,
	TYPE_IMAGE = 0xC,
	TYPE_FONT_FACE = 0xD,
	TYPE_FONT_HASH = 0xE,
	TYPE_ARRAY = 0xF,
};

struct RuiArg_s
{
	RuiArgumentType_e type;
	char unk1;
	i16 valueOffset;
	i16 nameOffset;
	i16 shortHash;
};

struct RuiArgCluster_s
{
	uint16_t argIndex;
	uint16_t argCount;
	char byte1;
	char byte2;
	i16 unk_6;
	i16 unk_8;
	i16 unk_A;
	char unk_C[4];
	i16 unk_10;
};

struct RuiFuncs_s
{
	void (*SetHidden)(RuiInstance_s* const rui);
	void (*SetNoRender)(RuiInstance_s* const rui);
	void (*SetNoRenderAndAssert)(RuiInstance_s* const rui, const char* const errorMsg);
	float (*GetTransformSize)(RuiInstance_s* const rui);
	void (*GetTextSize)(RuiInstance_s* const rui);
	void* unk1;
	void (*ExecuteTransform)(RuiInstance_s* const rui, const int index);
	void (*ExecuteTransformAndResize)(RuiInstance_s* const rui, const int index);
	const char* (*SNPrintF)(RuiInstance_s* const rui, const char* const fmt, ...);
	const char* (*Localize)(RuiInstance_s* const rui, const char* const key, __int64 a3, __int64 a4, __int64 a5, __int64 a6, __int64 a7);
	const char* (*ToUpper)(RuiInstance_s* const rui, char* const text);
	const char* (*FormatNumber)(RuiInstance_s* const rui, const char* fmt, const float number);
	void* unk2;
	void* unk3;
	void* unk4;
	void* unk5;
	void* unk6;
	void* unk7;
	u32(*FindAsset)(RuiInstance_s* const rui, const char* const assetName);
	void* unk8;
	void* unk9;
	void* unk10;
	void* unk11;
	void* unk12;
	void* unk13;
	u32(*StringToHash)(RuiInstance_s* const rui, const char* const assetName);
	void* (*GetActiveColorBlindPaletteColor)(RuiInstance_s* const rui, const int a2, const int a3);
	void* unk14;
};

struct RuiGlobalState_s
{
};

struct RuiHeader_s
{
	char* name;
	void* defaultValues;
	char* transformData;
	float elementWidth;
	float elementHeight;
	float elementWidthRatio;
	float elementHeightRatio;
	char* argNames;
	RuiArgCluster_s* argClusters;
	RuiArg_s* args;
	u16 argCount;
	u16 short42;
	u16 ruiDataStructSize;
	u16 defaultValuesSize;
	u16 styleDescriptorCount;
	u16 unk_4A;
	u16 renderJobCount;
	u16 argClusterCount;
	void* styleDescriptors;
	void* renderJobs;
	void* mappingData;
	void (*ruiFunc)(RuiFuncs_s* const api, RuiGlobalState_s* const state, RuiInstance_s* const rui, char* const values);
	void (*ruiHiddenFunc)(RuiFuncs_s* const api, RuiGlobalState_s* const state, RuiInstance_s* const rui, char* const values);
};

struct RuiInstance_s
{
	RuiHeader_s* header;
	float elementWidth;
	float elementHeight;
	float elementWidthRatio;
	float elementHeightRatio;
	void* str_v1;
	__m128 m128_20;
	_BYTE gap_30[8];
	i64 createTimeStamp;
	_QWORD qword_40;
	bool isHidden;
	bool hasError;
	_BYTE gap_4A[14];
	void* pointer_58;
	_BYTE data[1];
};

//-----------------------------------------------------------------------------
// Function pointers
//-----------------------------------------------------------------------------
inline bool(*v_Rui_Draw)(__int64* a1, __m128* a2, const __m128i* a3, __int64 a4, __m128* a5);
inline void* (*v_Rui_LoadAsset)(const char* szRuiAssetName);

inline __int64(*v_Rui_LookupAsset)(const char* assetPath);

inline int16_t(*v_Rui_GetFontFace)(void);

// Crashes with -1 index; patched in Detour when compat is active
inline __int64(*v_Rui_SetArgInt_Direct)(__int64 a1, int a2, int a3);
inline __int64(*v_Rui_SetArgFloat_Direct)(__int64 a1, unsigned int a2, __int64 a3);

// Native UIMG hash table lookup
// On miss this returns the "missing" texture index, not -1.
// Rui_AssetLookupCompat reimplements the lookup with proper miss detection.
inline __int64(*v_Rui_AssetLookup)(__int64 a1, const char* assetName);

inline void(*v_UIMG_BuildHashTable)(__int64 a1, __int64 a2, __int64 a3);

inline unsigned __int64(*v_Rui_EstimateTextWidth)(unsigned __int8* text, unsigned __int16 fontIndex, char uppercase);

// UIMG global hash table (populated at runtime by UIMG_BuildHashTable)
struct UIMGHashEntry
{
	u32 hash;
	u32 unused;
};

inline UIMGHashEntry** g_ppUIMGHashEntries = nullptr;  // hash entry array
inline i32**           g_ppUIMGHashBuckets = nullptr;   // bucket array
inline u32*            g_pnUIMGHashMask = nullptr;      // bucket mask (power_of_2 - 1)

inline u64 (*v_StringToGuid)(const char* str) = nullptr;

inline RuiFuncs_s* s_ruiApi = nullptr;


inline __int64 (*v_UI_DrawStageInstanceRange)(__int64 stage, __int64 bundles, __int64 beginIdx, __int64 endIdx) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VRuiDrawEnable : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("UI_DrawStageInstanceRange", v_UI_DrawStageInstanceRange);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

void Rui_BindDrawEnable(void);

#endif // RTECH_RUI_H
#else // !CLIENT_DLL
#ifndef RTECH_RUI_H
#define RTECH_RUI_H

struct RuiInstance_s;

enum class RuiArgumentType_e : u8
{
	TYPE_STRING = 0x1,
	TYPE_ASSET = 0x2,
	TYPE_BOOL = 0x3,
	TYPE_INT = 0x4,
	TYPE_FLOAT = 0x5,
	TYPE_FLOAT2 = 0x6,
	TYPE_FLOAT3 = 0x7,
	TYPE_COLOR_ALPHA = 0x8,
	TYPE_GAMETIME = 0x9,
	TYPE_WALLTIME = 0xA,
	TYPE_UIHANDLE = 0xB,
	TYPE_IMAGE = 0xC,
	TYPE_FONT_FACE = 0xD,
	TYPE_FONT_HASH = 0xE,
	TYPE_ARRAY = 0xF,
};

struct RuiArg_s
{
	RuiArgumentType_e type;
	char unk1;
	i16 valueOffset;
	i16 nameOffset;
	i16 shortHash;
};

struct RuiArgCluster_s
{
	uint16_t argIndex;
	uint16_t argCount;
	char byte1;
	char byte2;
	i16 unk_6;
	i16 unk_8;
	i16 unk_A;
	char unk_C[4];
	i16 unk_10;
};

struct RuiFuncs_s
{
	void (*SetHidden)(RuiInstance_s* const rui);
	void (*SetNoRender)(RuiInstance_s* const rui);
	void (*SetNoRenderAndAssert)(RuiInstance_s* const rui, const char* const errorMsg);
	float (*GetTransformSize)(RuiInstance_s* const rui);
	void (*GetTextSize)(RuiInstance_s* const rui);
	void* unk1;
	void (*ExecuteTransform)(RuiInstance_s* const rui, const int index);
	void (*ExecuteTransformAndResize)(RuiInstance_s* const rui, const int index);
	const char* (*SNPrintF)(RuiInstance_s* const rui, const char* const fmt, ...);
	const char* (*Localize)(RuiInstance_s* const rui, const char* const key, __int64 a3, __int64 a4, __int64 a5, __int64 a6, __int64 a7);
	const char* (*ToUpper)(RuiInstance_s* const rui, char* const text);
	const char* (*FormatNumber)(RuiInstance_s* const rui, const char* fmt, const float number);
	void* unk2;
	void* unk3;
	void* unk4;
	void* unk5;
	void* unk6;
	void* unk7;
	u32(*FindAsset)(RuiInstance_s* const rui, const char* const assetName);
	void* unk8;
	void* unk9;
	void* unk10;
	void* unk11;
	void* unk12;
	void* unk13;
	u32(*StringToHash)(RuiInstance_s* const rui, const char* const assetName);
	void* (*GetActiveColorBlindPaletteColor)(RuiInstance_s* const rui, const int a2, const int a3);
	void* unk14;
};

struct RuiGlobalState_s
{
};

struct RuiHeader_s
{
	char* name;
	void* defaultValues;
	char* transformData;
	float elementWidth;
	float elementHeight;
	float elementWidthRatio;
	float elementHeightRatio;
	char* argNames;
	RuiArgCluster_s* argClusters;
	RuiArg_s* args;
	u16 argCount;
	u16 short42;
	u16 ruiDataStructSize;
	u16 defaultValuesSize;
	u16 styleDescriptorCount;
	u16 unk_4A;
	u16 renderJobCount;
	u16 argClusterCount;
	void* styleDescriptors;
	void* renderJobs;
	void* mappingData;
	void (*ruiFunc)(RuiFuncs_s* const api, RuiGlobalState_s* const state, RuiInstance_s* const rui, char* const values);
	void (*ruiHiddenFunc)(RuiFuncs_s* const api, RuiGlobalState_s* const state, RuiInstance_s* const rui, char* const values);
};

struct RuiInstance_s
{
	RuiHeader_s* header;
	float elementWidth;
	float elementHeight;
	float elementWidthRatio;
	float elementHeightRatio;
	void* str_v1;
	__m128 m128_20;
	_BYTE gap_30[8];
	i64 createTimeStamp;
	_QWORD qword_40;
	bool isHidden;
	bool hasError;
	_BYTE gap_4A[14];
	void* pointer_58;
	_BYTE data[1];
};

//-----------------------------------------------------------------------------
// Function pointers
//-----------------------------------------------------------------------------
inline bool(*v_Rui_Draw)(__int64* a1, __m128* a2, const __m128i* a3, __int64 a4, __m128* a5);
inline void* (*v_Rui_LoadAsset)(const char* szRuiAssetName);

inline __int64(*v_Rui_LookupAsset)(const char* assetPath);

inline int16_t(*v_Rui_GetFontFace)(void);

// Crashes with -1 index; patched in Detour when compat is active
inline __int64(*v_Rui_SetArgInt_Direct)(__int64 a1, int a2, int a3);
inline __int64(*v_Rui_SetArgFloat_Direct)(__int64 a1, unsigned int a2, __int64 a3);

// Native UIMG hash table lookup
// On miss this returns the "missing" texture index, not -1.
// Rui_AssetLookupCompat reimplements the lookup with proper miss detection.
inline __int64(*v_Rui_AssetLookup)(__int64 a1, const char* assetName);

inline void(*v_UIMG_BuildHashTable)(__int64 a1, __int64 a2, __int64 a3);

inline unsigned __int64(*v_Rui_EstimateTextWidth)(unsigned __int8* text, unsigned __int16 fontIndex, char uppercase);

// UIMG global hash table (populated at runtime by UIMG_BuildHashTable)
struct UIMGHashEntry
{
	u32 hash;
	u32 unused;
};

inline UIMGHashEntry** g_ppUIMGHashEntries = nullptr;  // hash entry array
inline i32**           g_ppUIMGHashBuckets = nullptr;   // bucket array
inline u32*            g_pnUIMGHashMask = nullptr;      // bucket mask (power_of_2 - 1)

inline u64 (*v_StringToGuid)(const char* str) = nullptr;

inline RuiFuncs_s* s_ruiApi = nullptr;


#endif // RTECH_RUI_H
#endif // CLIENT_DLL
