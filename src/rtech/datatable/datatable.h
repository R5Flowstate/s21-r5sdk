#ifndef RTECH_DATATABLE_H
#define RTECH_DATATABLE_H
//=============================================================================//
//
// Purpose: RTech datatable system declarations
//
//=============================================================================//
#include "thirdparty/detours/include/idetour.h"

//-----------------------------------------------------------------------------
// Column types
//-----------------------------------------------------------------------------
enum DatatableColumnType_e : int
{
	DTCOL_BOOL = 0,
	DTCOL_INT = 1,
	DTCOL_FLOAT = 2,
	DTCOL_VECTOR = 3,
	DTCOL_STRING = 4,
	DTCOL_ASSET = 5,
	DTCOL_ASSET_NOPRECACHE = 6,
	DTCOL_COUNT
};

//-----------------------------------------------------------------------------
// Engine structures
//-----------------------------------------------------------------------------
struct DTColumnData
{
	const char* name;
	int         type;
	int         byteOffset;
};
static_assert(sizeof(DTColumnData) == 16, "DTColumnData size mismatch");

struct DataTableHeader
{
	int             columnCount;
	int             rowCount;
	DTColumnData*   columnData;
	char*           rows;
	uint64_t        dataTableCRC;
	int             rowSize;
	char            pad[4];
};
static_assert(sizeof(DataTableHeader) == 40, "DataTableHeader size mismatch");

//-----------------------------------------------------------------------------
// Engine functions (pattern-resolved; no hardcoded RVAs)
// GetDataTableFromScript is twinned on the dedi (server VM + client/UI VM).
// Script_GetDatatable is shared across VMs (one copy).
//-----------------------------------------------------------------------------
inline DataTableHeader* (*v_GetDataTableFromScript[2])(void* sqvm);
inline int g_nGetDataTableFromScriptCount = 0;
inline int64_t (*v_Script_GetDatatable)(void* sqvm) = nullptr;
inline uint64_t* (*v_sq_newuserdata)(void* sqvm, int size) = nullptr;
// Second arg is unused on the userdata branch; stack-top is tagged.
inline int64_t (*v_sq_settypetag)(void* sqvm, int64_t unused, uint64_t typetag) = nullptr;
inline uint64_t (*v_HashNameAligned)(const char* name);
inline uint64_t (*v_HashNameUnaligned)(const char* name);

//-----------------------------------------------------------------------------
// Returns the disk override for the datatable the script passed in, or the
// engine's own header when there is none. Null on a malformed argument.
//-----------------------------------------------------------------------------
const DataTableHeader* Datatable_ResolveFromScript(void* sqvm);

///////////////////////////////////////////////////////////////////////////////
class V_Datatable : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("GetDataTableFromScript", v_GetDataTableFromScript[0]);
		if (g_nGetDataTableFromScriptCount > 1)
			LogFunAdr("GetDataTableFromScript_1", v_GetDataTableFromScript[1]);
		LogFunAdr("Script_GetDatatable", v_Script_GetDatatable);
		LogFunAdr("sq_newuserdata", v_sq_newuserdata);
		LogFunAdr("sq_settypetag", v_sq_settypetag);
		LogFunAdr("HashNameAligned", v_HashNameAligned);
		LogFunAdr("HashNameUnaligned", v_HashNameUnaligned);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // RTECH_DATATABLE_H
