#if defined(CLIENT_DLL)
//=============================================================================//
//
// Purpose: pak runtime memory and management. S21 walks the slot table (stride 352).
//
//=============================================================================//
#include "tier0/fasttimer.h"
#include "tier1/fmtstr.h"
#include "common/completion.h"
#include "rtech/ipakfile.h"
#include "rtech/pak/rpak_observe.h"
#include "pakencode.h"
#include "pakdecode.h"
#include "paktools.h"
#include "pakstate.h"
#include "ui_image_skip.h"

// Default paks\Win64\; S3 left these NULL and Asserted on first use.
static const char* s_pakReadPath  = "paks\\Win64\\";
static const char* s_pakWritePath = "paks\\Win64\\";

void Pak_SetReadPath(const char* const path)
{
	Assert(path);
	s_pakReadPath = path;
}

const char* Pak_GetReadPath()
{
	return s_pakReadPath ? s_pakReadPath : "paks\\Win64\\";
}

void Pak_SetWritePath(const char* const path)
{
	Assert(path);
	s_pakWritePath = path;
}

const char* Pak_GetWritePath()
{
	return s_pakWritePath ? s_pakWritePath : "paks\\Win64\\";
}

//=============================================================================
// S21 slot-table helpers
//=============================================================================

static uintptr_t Pak_GetSlotPtr_S21(int slot)
{
	const uintptr_t base = Pak_GetSlotBase_S21();
	if (!base) return 0;
	return base + (uintptr_t)slot * kS21_PakSlotStride;
}

static int  Pak_GetSlotHandle_S21(uintptr_t slot) { return *reinterpret_cast<int*>(slot + kS21_PakSlot_Handle); }
static int  Pak_GetSlotStatus_S21(uintptr_t slot) { return *reinterpret_cast<int*>(slot + kS21_PakSlot_Status); }
static const char* Pak_GetSlotName_S21(uintptr_t slot)
{
	const char* name = nullptr;
	__try { name = *reinterpret_cast<const char**>(slot + kS21_PakSlot_Name); }
	__except (EXCEPTION_EXECUTE_HANDLER) { name = nullptr; }
	return name;
}

// Linear scan by pak name.
static int Pak_FindSlotByName_S21(const char* pakName)
{
	if (!pakName) return -1;
	for (int i = 0; i < (int)kS21_PakSlotCount; ++i)
	{
		const uintptr_t slot = Pak_GetSlotPtr_S21(i);
		if (!slot) return -1;
		if (Pak_GetSlotStatus_S21(slot) == 0 /*FREED*/) continue;
		const char* const name = Pak_GetSlotName_S21(slot);
		if (name && _stricmp(name, pakName) == 0)
			return i;
	}
	return -1;
}

/*
=====================
Pak_ListPaks_f
=====================
*/
static void Pak_ListPaks_f()
{
	Msg(eDLL_T::RTECH, "| id   | name                                                 | status                          |\n");
	Msg(eDLL_T::RTECH, "|------|------------------------------------------------------|---------------------------------|\n");

	uint32_t numLoaded = 0;

	for (int i = 0; i < (int)kS21_PakSlotCount; ++i)
	{
		const uintptr_t slot = Pak_GetSlotPtr_S21(i);
		if (!slot) break;

		const int status = Pak_GetSlotStatus_S21(slot);
		if (status == 0)
			continue;

		const int handle = Pak_GetSlotHandle_S21(slot);
		const char* const name = Pak_GetSlotName_S21(slot);

		Msg(eDLL_T::RTECH, "| %04X | %-52s | %-31s |\n",
			handle & 0xFFFFFF, name ? name : "(null)", Pak_StatusToString_S21(status));
		numLoaded++;
	}
	Msg(eDLL_T::RTECH, "|------|------------------------------------------------------|---------------------------------|\n");
	Msg(eDLL_T::RTECH, "| %u loaded paks.\n", numLoaded);
}

/*
=====================
Pak_ListTypes_f
=====================
*/
static void Pak_ListTypes_f()
{
	Msg(eDLL_T::RTECH,
		"pak_listtypes: not yet ported to S21 (assetBindings table RVA "
		"unmapped). pak_listpaks works.\n");
}

/*
=====================
Pak_RequestUnload_f
=====================
*/
static void Pak_RequestUnload_f(const CCommand& args)
{
	if (args.ArgC() < 2)
		return;

	// Low-level UnloadAsyncByHandle; skip the script-binding wrapper.
	if (!Pak_UnloadAsyncByHandle_S21Resolve())
	{
		Warning(eDLL_T::RTECH, "pak_requestunload: Pak_UnloadAsyncByHandle_S21 not resolved\n");
		return;
	}

	int handle = -1;
	const char* nameForLog = "(by handle)";

	if (args.HasOnlyDigits(-1))
	{
		handle = atoi(args.ArgS());
		const int slotIdx = handle & (int)(kS21_PakSlotCount - 1);
		const uintptr_t slot = Pak_GetSlotPtr_S21(slotIdx);
		if (!slot)
		{
			Warning(eDLL_T::RTECH, "pak_requestunload: slot table unavailable\n");
			return;
		}
		const int status = Pak_GetSlotStatus_S21(slot);
		if (status != 10)
		{
			Warning(eDLL_T::RTECH,
				"pak_requestunload: handle %d not loaded (status=%s); skipping\n",
				handle, Pak_StatusToString_S21(status));
			return;
		}
		nameForLog = Pak_GetSlotName_S21(slot);
	}
	else
	{
		const char* const pakName = args.ArgS();
		const int slotIdx = Pak_FindSlotByName_S21(pakName);
		if (slotIdx < 0)
		{
			Warning(eDLL_T::RTECH, "pak_requestunload: '%s' not loaded\n", pakName);
			return;
		}
		const uintptr_t slot = Pak_GetSlotPtr_S21(slotIdx);
		const int status = Pak_GetSlotStatus_S21(slot);
		if (status != 10)
		{
			Warning(eDLL_T::RTECH,
				"pak_requestunload: '%s' status=%s; only LOADED paks can be unloaded\n",
				pakName, Pak_StatusToString_S21(status));
			return;
		}
		handle = Pak_GetSlotHandle_S21(slot);
		nameForLog = pakName;
	}

	v_Pak_UnloadAsyncByHandle_S21((unsigned int)handle, 1);
	Msg(eDLL_T::RTECH, "unloading '%s' (handle 0x%X)\n",
		nameForLog ? nameForLog : "(null)", handle & 0xFFFFFF);
}

/*
=====================
Pak_RequestLoad_f
=====================
*/
static bool Pak_RequestLoadConsole(const char* const pakFile, const bool bNoUi)
{
	// Full 5-arg Pak_RequestLoadByName. The script wrapper's callback chain is null here.
	if (!Pak_RequestLoadByName_S21Resolve())
	{
		Warning(eDLL_T::RTECH, "pak_requestload: Pak_RequestLoadByName_S21 not resolved\n");
		return false;
	}

	const uintptr_t allocSlot = Pak_GetGlobalAllocatorSlot_S21();
	if (!allocSlot)
	{
		Warning(eDLL_T::RTECH, "pak_requestload: failed to resolve global allocator slot\n");
		return false;
	}

	if (!Pak_IsAllowedLoadName_S21(pakFile))
	{
		Warning(eDLL_T::RTECH, "pak_requestload: rejected '%s'\n", pakFile);
		return false;
	}
	if (bNoUi)
	{
		if (!UIImageSkip_MarkPak(pakFile))
		{
			Warning(eDLL_T::RTECH, "pak_requestload_noui: cannot mark '%s' (name too long or list full)\n", pakFile);
			return false;
		}
	}
	else
		UIImageSkip_UnmarkPak(pakFile);
	auto pfnFull = reinterpret_cast<PFN_Pak_RequestLoadByName_Full_S21>(
		v_Pak_RequestLoadByName_S21);
	const int handle = pfnFull(pakFile,
		1 /*priority*/,
		allocSlot,
		8 /*c4*/,
		0 /*trackFeature*/);
	if (handle == -1)
	{
		Warning(eDLL_T::RTECH, "pak_requestload: '%s' failed (engine returned -1)\n", pakFile);
		return false;
	}
	Msg(eDLL_T::RTECH, "loading '%s' (handle 0x%X)%s\n", pakFile, handle & 0xFFFFFF,
		bNoUi ? " with UI images stubbed" : "");
	return true;
}

bool Pak_RequestLoadNoUi(const char* const pakFile)
{
	if (!pakFile || !pakFile[0])
	{
		Warning(eDLL_T::RTECH, "pak_requestload_noui: rejected empty name\n");
		return false;
	}
	return Pak_RequestLoadConsole(pakFile, true);
}

int Pak_GetStatusByHandle_S21(int handle)
{
	const uintptr_t base = Pak_GetSlotBase_S21();
	if (!base)
		return -1;

	int status = -1;
	__try
	{
		const int slotIdx = handle & (int)(kS21_PakSlotCount - 1);
		const uintptr_t slot = Pak_GetSlotPtr_S21(slotIdx);
		if (slot && Pak_GetSlotHandle_S21(slot) == handle)
			status = Pak_GetSlotStatus_S21(slot);
		else
		{
			for (int i = 0; i < (int)kS21_PakSlotCount; ++i)
			{
				const uintptr_t s = Pak_GetSlotPtr_S21(i);
				if (!s)
					break;
				if (Pak_GetSlotHandle_S21(s) == handle)
				{
					status = Pak_GetSlotStatus_S21(s);
					break;
				}
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { status = -1; }
	return status;
}

int Pak_FindHandleByName_S21(const char* const pakName)
{
	const int slotIdx = Pak_FindSlotByName_S21(pakName);
	if (slotIdx < 0)
		return -1;
	return Pak_GetSlotHandle_S21(Pak_GetSlotPtr_S21(slotIdx));
}

bool Pak_UnloadByHandle_S21(int handle)
{
	if (!Pak_UnloadAsyncByHandle_S21Resolve())
	{
		Warning(eDLL_T::RTECH, "pak unload: Pak_UnloadAsyncByHandle_S21 not resolved\n");
		return false;
	}
	v_Pak_UnloadAsyncByHandle_S21((unsigned int)handle, 1);
	return true;
}

static void Pak_RequestLoad_f(const CCommand& args)
{
	if (args.ArgC() < 2)
		return;
	Pak_RequestLoadConsole(args.ArgS(), false);
}

static void Pak_RequestLoadNoUi_f(const CCommand& args)
{
	if (args.ArgC() < 2)
		return;
	Pak_RequestLoadNoUi(args.ArgS());
}

/*
=====================
Pak_RequestSwap_f (S21)

 Unload-then-reload. Captures the name before the unload call
 because the slot's name pointer can be freed during the unload
 state machine transitions.
=====================
*/
static void Pak_RequestSwap_f(const CCommand& args)
{
	if (args.ArgC() < 2)
		return;

	if (!Pak_UnloadAsyncByHandle_S21Resolve() || !Pak_RequestLoadByName_S21Resolve())
	{
		Warning(eDLL_T::RTECH, "pak_requestswap: API not fully resolved\n");
		return;
	}
	const uintptr_t allocSlot = Pak_GetGlobalAllocatorSlot_S21();
	if (!allocSlot)
	{
		Warning(eDLL_T::RTECH, "pak_requestswap: failed to resolve global allocator slot\n");
		return;
	}

	int handle = -1;
	char nameCopy[MAX_OSPATH];
	nameCopy[0] = '\0';

	if (args.HasOnlyDigits(-1))
	{
		handle = atoi(args.ArgS());
		const int slotIdx = handle & (int)(kS21_PakSlotCount - 1);
		const uintptr_t slot = Pak_GetSlotPtr_S21(slotIdx);
		if (!slot)
		{
			Warning(eDLL_T::RTECH, "pak_requestswap: slot table unavailable\n");
			return;
		}
		const int status = Pak_GetSlotStatus_S21(slot);
		if (status != 10)
		{
			Warning(eDLL_T::RTECH,
				"pak_requestswap: handle %d not loaded (status=%s)\n",
				handle, Pak_StatusToString_S21(status));
			return;
		}
		const char* const slotName = Pak_GetSlotName_S21(slot);
		if (!slotName)
		{
			Warning(eDLL_T::RTECH, "pak_requestswap: handle %d has no name\n", handle);
			return;
		}
		strncpy_s(nameCopy, sizeof(nameCopy), slotName, _TRUNCATE);
	}
	else
	{
		const char* const pakName = args.ArgS();
		const int slotIdx = Pak_FindSlotByName_S21(pakName);
		if (slotIdx < 0)
		{
			Warning(eDLL_T::RTECH, "pak_requestswap: '%s' not loaded\n", pakName);
			return;
		}
		const uintptr_t slot = Pak_GetSlotPtr_S21(slotIdx);
		handle = Pak_GetSlotHandle_S21(slot);
		strncpy_s(nameCopy, sizeof(nameCopy), pakName, _TRUNCATE);
	}

	CFastTimer timer;
	timer.Start();

	v_Pak_UnloadAsyncByHandle_S21((unsigned int)handle, 1 /*wait*/);
	if (!Pak_IsAllowedLoadName_S21(nameCopy))
	{
		Warning(eDLL_T::RTECH, "pak_requestswap: rejected '%s'\n", nameCopy);
		return;
	}
	auto pfnFull = reinterpret_cast<PFN_Pak_RequestLoadByName_Full_S21>(
		v_Pak_RequestLoadByName_S21);
	const int newHandle = pfnFull(nameCopy, 1, allocSlot, 8, 0);

	timer.End();

	if (newHandle == -1)
		Warning(eDLL_T::RTECH, "pak_requestswap: '%s' reload failed (engine returned -1)\n", nameCopy);
	else
		Msg(eDLL_T::RTECH, "swapped '%s' in %.2fs (handle 0x%X)\n",
			nameCopy, timer.GetDuration().GetSeconds(), newHandle & 0xFFFFFF);
}

/*
=====================
Pak_StringToGUID_f
=====================
*/
static void Pak_StringToGUID_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		return;
	}

	const PakGuid_t guid = Pak_StringToGuid(args.ArgS());

	Msg(eDLL_T::RTECH, "______________________________________________________________\n");
	Msg(eDLL_T::RTECH, "] RTECH_HASH ]------------------------------------------------\n");
	Msg(eDLL_T::RTECH, "] GUID: '0x%llX'\n", guid);
}

/*
=====================
Pak_Decompress_f

 Decompresses input RPak file and
 dumps results to override path
=====================
*/
static void Pak_Decompress_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		return;
	}

	const CFmtStr1024 inPakFile("%s%s", Pak_GetReadPath(), args.ArgS());
	const CFmtStr1024 outPakFile("%s%s", Pak_GetWritePath(), args.ArgS());

	if (!Pak_DecodePakFile(inPakFile.String(), outPakFile.String()))
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s - decompression failed for '%s'!\n",
			__FUNCTION__, inPakFile.String());
	}
}

static ConVar pak_compresslevel("pak_compresslevel", "6", FCVAR_RELEASE, "Determines the RPAK file compression level.",
	true, (float)-5, // See https://github.com/facebook/zstd/issues/3032
	true, (float)ZSTD_maxCLevel(), "int");

/*
=====================
Pak_Compress_f

 Compresses input RPak file and
 dumps results to base path
=====================
*/
static void Pak_Compress_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		return;
	}

	const CFmtStr1024 inPakFile("%s%s", Pak_GetReadPath(), args.ArgS());
	const CFmtStr1024 outPakFile("%s%s", Pak_GetWritePath(), args.ArgS());

	if (!Pak_EncodePakFile(inPakFile.String(), outPakFile.String(), pak_compresslevel.GetInt()))
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s - compression failed for '%s'!\n",
			__FUNCTION__, inPakFile.String());
	}
}

static ConVar oodle_pak_compressor("oodle_pak_compressor", "8", FCVAR_RELEASE,
	"Oodle codec for pak_oodle_compress (8 = Kraken, 13 = Leviathan).", true, (float)0, true, (float)13, "int");
static ConVar oodle_pak_level("oodle_pak_level", "4", FCVAR_RELEASE,
	"Oodle compression level for pak_oodle_compress (1 = SuperFast .. 9 = Optimal5).", true, (float)1, true, (float)9, "int");

/*
=====================
Pak_OodleCompress_f

 Compresses an input RPak with the in-game (S21 native) Oodle encoder and writes
 the result to a SEPARATE output file. Compressed paks decode into freshly
 aligned buffers, which retires the uncompressed-pak collision-tree load-base
 crash. Never in-place -- protects the known-good uncompressed source.
=====================
*/
static void Pak_OodleCompress_f(const CCommand& args)
{
	if (args.ArgC() < 3)
	{
		Msg(eDLL_T::RTECH, "usage: pak_oodle_compress <inPakName> <outPakName>\n");
		return;
	}

	const CFmtStr1024 inPakFile("%s%s", Pak_GetReadPath(), args.Arg(1));
	const CFmtStr1024 outPakFile("%s%s", Pak_GetWritePath(), args.Arg(2));

	if (!Pak_EncodePakFileOodle(inPakFile.String(), outPakFile.String(),
		oodle_pak_compressor.GetInt(), oodle_pak_level.GetInt()))
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s - oodle compression failed for '%s'!\n",
			__FUNCTION__, inPakFile.String());
	}
}

// FCVAR_RELEASE: S21 rejects DEVELOPMENTONLY before the callback fires.
static ConCommand pak_stringtoguid("pak_stringtoguid", Pak_StringToGUID_f, "Compute GUID from input text", FCVAR_RELEASE);

static ConCommand pak_compress("pak_compress", Pak_Compress_f, "Compresses specified RPAK file", FCVAR_RELEASE, RTech_PakCompress_f_CompletionFunc);
static ConCommand pak_oodle_compress("pak_oodle_compress", Pak_OodleCompress_f, "Compresses an RPAK with the in-game S21 Oodle encoder. Usage: pak_oodle_compress <inPakName> <outPakName>", FCVAR_RELEASE, RTech_PakCompress_f_CompletionFunc);
static ConCommand pak_decompress("pak_decompress", Pak_Decompress_f, "Decompresses specified RPAK file", FCVAR_RELEASE, RTech_PakDecompress_f_CompletionFunc);

static ConCommand pak_requestload("pak_requestload", Pak_RequestLoad_f, "Requests asynchronous load for specified RPAK file", FCVAR_RELEASE, RTech_PakLoad_f_CompletionFunc);
static ConCommand pak_requestload_noui("pak_requestload_noui", Pak_RequestLoadNoUi_f, "Requests asynchronous load for specified RPAK file with its UI images loaded as 1x1 stubs (no atlas cost)", FCVAR_RELEASE, RTech_PakLoad_f_CompletionFunc);
static ConCommand pak_requestunload("pak_requestunload", Pak_RequestUnload_f, "Requests asynchronous unload for specified RPAK file or ID", FCVAR_RELEASE, RTech_PakUnload_f_CompletionFunc);

static ConCommand pak_requestswap("pak_requestswap", Pak_RequestSwap_f, "Requests swap for specified RPAK file or ID", FCVAR_RELEASE, RTech_PakSwap_f_CompletionFunc);

static ConCommand pak_listpaks("pak_listpaks", Pak_ListPaks_f, "Display a list of loaded RPAK files", FCVAR_RELEASE);
static ConCommand pak_listtypes("pak_listtypes", Pak_ListTypes_f, "Display a list of registered asset types (S21: stub)", FCVAR_RELEASE);


// Symbols taken from R2 dll's.
PakLoadFuncs_s* g_pakLoadApi = nullptr;
#else // !CLIENT_DLL
//=============================================================================//
//
// Purpose: pak runtime memory and management
//
//=============================================================================//
#include "tier0/fasttimer.h"
#include "tier1/fmtstr.h"
#include "common/completion.h"
#include "rtech/ipakfile.h"
#include "pakencode.h"
#include "pakdecode.h"
#include "paktools.h"
#include "pakstate.h"

static const char* s_pakReadPath = nullptr;
static const char* s_pakWritePath = nullptr;

void Pak_SetReadPath(const char* const path)
{
	Assert(path);
	s_pakReadPath = path;
}

const char* Pak_GetReadPath()
{
	Assert(s_pakReadPath);
	return s_pakReadPath;
}

void Pak_SetWritePath(const char* const path)
{
	Assert(path);
	s_pakWritePath = path;
}

const char* Pak_GetWritePath()
{
	Assert(s_pakWritePath);
	return s_pakWritePath;
}

/*
=====================
Pak_ListPaks_f
=====================
*/
static void Pak_ListPaks_f()
{
	Msg(eDLL_T::RTECH, "| id   | name                                               | status                               | asset count |\n");
	Msg(eDLL_T::RTECH, "|------|----------------------------------------------------|--------------------------------------|-------------|\n");

	uint32_t numLoaded = 0;

	for (uint16_t i = 0, n = PAK_MAX_LOADED_PAKS; i < n; ++i)
	{
		const PakLoadedInfo_s& info = g_pakGlobals->loadedPaks[i];

		if (info.status == PakStatus_e::PAK_STATUS_FREED)
			continue;

		const char* const pakStatus = Pak_StatusToString(info.status);

		Msg(eDLL_T::RTECH, "| %04i | %-50s | %-36s | %11u |\n", info.handle, info.fileName, pakStatus, info.assetCount);
		numLoaded++;
	}
	Msg(eDLL_T::RTECH, "|------|----------------------------------------------------|--------------------------------------|-------------|\n");
	Msg(eDLL_T::RTECH, "| %18u loaded paks.                                                                                |\n", numLoaded);
	Msg(eDLL_T::RTECH, "|------|----------------------------------------------------|--------------------------------------|-------------|\n");
}

/*
=====================
Pak_ListTypes_f
=====================
*/
static void Pak_ListTypes_f()
{
	Msg(eDLL_T::RTECH, "| ext  | description               | version | alignment | header size | struct size |\n");
	Msg(eDLL_T::RTECH, "|------|---------------------------|---------|-----------|-------------|-------------|\n");

	uint32_t numRegistered = 0;

	for (uint8_t i = 0; i < PAK_MAX_TRACKED_TYPES; ++i)
	{
		const PakAssetBinding_s& type = g_pakGlobals->assetBindings[i];

		if (type.type == PakAssetBinding_s::EType::NONE || type.type == PakAssetBinding_s::EType::STUB)
			continue;

		FourCCString_t assetExtension;
		FourCCToString(assetExtension, type.extension);

		Msg(eDLL_T::RTECH, "| %-4s | %-25s | %7u | %9u | %11u | %11u |\n", 
			assetExtension, type.description, type.version, type.headerAlignment, type.headerSize, type.structSize);

		numRegistered++;
	}
	Msg(eDLL_T::RTECH, "|------|---------------------------|---------|-----------|-------------|-------------|\n");
	Msg(eDLL_T::RTECH, "| %18u registered types.                                               |\n", numRegistered);
	Msg(eDLL_T::RTECH, "|------|---------------------------|---------|-----------|-------------|-------------|\n");
}

/*
=====================
Pak_RequestUnload_f
=====================
*/
static void Pak_RequestUnload_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		return;
	}

	const PakLoadedInfo_s* pakInfo = nullptr;

	if (args.HasOnlyDigits(-1))
	{
		const PakHandle_t pakHandle = atoi(args.ArgS());
		pakInfo = Pak_GetPakInfo(pakHandle);

		if (pakInfo->status != PAK_STATUS_LOADED)
		{
			Warning(eDLL_T::RTECH, "Pak with handle %d is currently unavailable; status %s, cannot unload\n",
				pakHandle, Pak_StatusToString(pakInfo->status));
			return;
		}
	}
	else
	{
		const char* const pakName = args.ArgS();
		pakInfo = Pak_GetPakInfo(pakName);

		if (!pakInfo)
		{
			Warning(eDLL_T::RTECH, "Pak with name '%s' not loaded, cannot unload\n", pakName);
			return;
		}
		else if (pakInfo->status != PAK_STATUS_LOADED)
		{
			Warning(eDLL_T::RTECH, "Pak with name '%s' is currently unavailable; status %s, cannot unload\n",
				pakName, Pak_StatusToString(pakInfo->status));
			return;
		}
	}

	Msg(eDLL_T::RTECH, "Requested pak unload for file '%s' with handle %d\n", pakInfo->fileName, pakInfo->handle);
	g_pakLoadApi->UnloadAsync(pakInfo->handle);
}

/*
=====================
Pak_RequestLoad_f
=====================
*/
static void Pak_RequestLoad_f(const CCommand& args)
{
	const char* const pakFile = args.ArgS();

	Msg(eDLL_T::RTECH, "Requested pak load for file '%s'\n", pakFile);
	g_pakLoadApi->LoadAsync(pakFile, AlignedMemAlloc(), 1, 0);
}

/*
=====================
Pak_RequestSwap_f
=====================
*/
static void Pak_RequestSwap_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		return;
	}

	const PakLoadedInfo_s* pakInfo = nullptr;
	const char* pakName = nullptr;

	if (args.HasOnlyDigits(-1))
	{
		const PakHandle_t pakHandle = atoi(args.ArgS());
		pakInfo = Pak_GetPakInfo(pakHandle);

		if (pakInfo->status != PAK_STATUS_LOADED)
		{
			Warning(eDLL_T::RTECH, "Pak with handle %d is currently unavailable; status %s, cannot swap\n",
				pakHandle, Pak_StatusToString(pakInfo->status));
			return;
		}
	}
	else
	{
		pakName = args.ArgS();
		pakInfo = Pak_GetPakInfo(pakName);

		if (!pakInfo)
		{
			Warning(eDLL_T::RTECH, "Pak with name '%s' not loaded, cannot swap\n", pakName);
			return;
		}
		else if (pakInfo->status != PAK_STATUS_LOADED)
		{
			Warning(eDLL_T::RTECH, "Pak with name '%s' is currently unavailable; status %s, cannot swap\n",
				pakName, Pak_StatusToString(pakInfo->status));
			return;
		}
	}

	Msg(eDLL_T::RTECH, "Requested hot swap for pak file '%s' with handle %d\n", pakInfo->fileName, pakInfo->handle);

	CFastTimer timer;
	timer.Start();

	// Store these since they will be clobbered.
	const int logChannel = pakInfo->logChannel;
	const uint8_t unkAC = pakInfo->unkAC;

	char tempName[MAX_OSPATH];

	// Small optimization, the command argument persist through the unload, so
	// use that if available. Else copy the name from the pak info struct and
	// reuse that since this will be freed during the unload!
	if (!pakName)
	{
		strncpy(tempName, pakInfo->fileName, sizeof(tempName));
		pakName = tempName;
	}

	g_pakLoadApi->UnloadAsyncAndWait(pakInfo->handle); // Wait till this slot gets free'd.
	g_pakLoadApi->LoadAsync(pakName, AlignedMemAlloc(), logChannel, unkAC);

	timer.End();
	Msg(eDLL_T::RTECH, "Hot swap took %lf seconds\n", timer.GetDuration().GetSeconds());
}

/*
=====================
Pak_StringToGUID_f
=====================
*/
static void Pak_StringToGUID_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		return;
	}

	const PakGuid_t guid = Pak_StringToGuid(args.ArgS());

	Msg(eDLL_T::RTECH, "______________________________________________________________\n");
	Msg(eDLL_T::RTECH, "] RTECH_HASH ]------------------------------------------------\n");
	Msg(eDLL_T::RTECH, "] GUID: '0x%llX'\n", guid);
}

/*
=====================
Pak_Decompress_f

 Decompresses input RPak file and
 dumps results to override path
=====================
*/
static void Pak_Decompress_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		return;
	}

	const CFmtStr1024 inPakFile("%s%s", Pak_GetReadPath(), args.ArgS());
	const CFmtStr1024 outPakFile("%s%s", Pak_GetWritePath(), args.ArgS());

	if (!Pak_DecodePakFile(inPakFile.String(), outPakFile.String()))
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s - decompression failed for '%s'!\n",
			__FUNCTION__, inPakFile.String());
	}
}

static ConVar pak_compresslevel("pak_compresslevel", "6", FCVAR_DEVELOPMENTONLY, "Determines the RPAK file compression level.",
	true, (float)-5, // See https://github.com/facebook/zstd/issues/3032
	true, (float)ZSTD_maxCLevel(), "int");

/*
=====================
Pak_Compress_f

 Compresses input RPak file and
 dumps results to base path
=====================
*/
static void Pak_Compress_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		return;
	}

	const CFmtStr1024 inPakFile("%s%s", Pak_GetReadPath(), args.ArgS());
	const CFmtStr1024 outPakFile("%s%s", Pak_GetWritePath(), args.ArgS());

	if (!Pak_EncodePakFile(inPakFile.String(), outPakFile.String(), pak_compresslevel.GetInt()))
	{
		Error(eDLL_T::RTECH, NO_ERROR, "%s - compression failed for '%s'!\n",
			__FUNCTION__, inPakFile.String());
	}
}

static ConCommand pak_stringtoguid("pak_stringtoguid", Pak_StringToGUID_f, "Compute GUID from input text", FCVAR_DEVELOPMENTONLY);

static ConCommand pak_compress("pak_compress", Pak_Compress_f, "Compresses specified RPAK file", FCVAR_DEVELOPMENTONLY, RTech_PakCompress_f_CompletionFunc);
static ConCommand pak_decompress("pak_decompress", Pak_Decompress_f, "Decompresses specified RPAK file", FCVAR_DEVELOPMENTONLY, RTech_PakDecompress_f_CompletionFunc);

static ConCommand pak_requestload("pak_requestload", Pak_RequestLoad_f, "Requests asynchronous load for specified RPAK file", FCVAR_DEVELOPMENTONLY, RTech_PakLoad_f_CompletionFunc);
static ConCommand pak_requestunload("pak_requestunload", Pak_RequestUnload_f, "Requests asynchronous unload for specified RPAK file or ID", FCVAR_DEVELOPMENTONLY, RTech_PakUnload_f_CompletionFunc);

static ConCommand pak_requestswap("pak_requestswap", Pak_RequestSwap_f, "Requests swap for specified RPAK file or ID", FCVAR_DEVELOPMENTONLY, RTech_PakSwap_f_CompletionFunc);

static ConCommand pak_listpaks("pak_listpaks", Pak_ListPaks_f, "Display a list of loaded RPAK files", FCVAR_RELEASE);
static ConCommand pak_listtypes("pak_listtypes", Pak_ListTypes_f, "Display a list of registered asset types", FCVAR_RELEASE);


// Symbols taken from R2 dll's.
PakLoadFuncs_s* g_pakLoadApi = nullptr;
#endif // CLIENT_DLL
