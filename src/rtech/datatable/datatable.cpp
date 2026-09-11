//=============================================================================//
//
// Purpose: Disk CSV override for rpak datatable/... assets
//
//=============================================================================//

#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "tier1/strtools.h"
#include "filesystem/filesystem.h"
#include "datatable.h"
#include <unordered_map>
#include <vector>
#include <string>

//-----------------------------------------------------------------------------
// Limits
//-----------------------------------------------------------------------------
static constexpr size_t DATATABLE_MAX_FILE_SIZE    = 4 * 1024 * 1024;   // 4 MiB
static constexpr int    DATATABLE_MAX_ROWS         = 20000;
static constexpr int    DATATABLE_MAX_COLUMNS      = 128;
static constexpr int    DATATABLE_MAX_ROW_STRIDE   = 4096;
static constexpr size_t DATATABLE_MAX_TOTAL_BLOCK  = 16 * 1024 * 1024;  // 16 MiB
static constexpr size_t DATATABLE_MAX_DISK_ENTRIES = 256;
static constexpr size_t DATATABLE_MAX_PATH_LEN     = 260;
static constexpr const char* s_datatableDiskBasePath = "platform\\datatable\\";

//-----------------------------------------------------------------------------
// Squirrel ABI offsets (product split, one block)
//-----------------------------------------------------------------------------
#if defined(CLIENT_DLL)
static constexpr ptrdiff_t s_offVmStackTop      = 0x48;
static constexpr uint32_t  s_tagUserData        = 0xA0000080;
static constexpr uint32_t  s_tagAsset           = 0x80000400u;
static constexpr ptrdiff_t s_offUserDataTypeTag = 0x48;
static constexpr ptrdiff_t s_offUserDataPayload = 0x50;
static constexpr ptrdiff_t s_offAssetString     = 0x30;
#else
static constexpr ptrdiff_t s_offVmStackTop      = 0x58;
static constexpr uint32_t  s_tagUserData        = 0x0A000080;
static constexpr uint32_t  s_tagAsset           = 0x08000400u;
static constexpr ptrdiff_t s_offUserDataTypeTag = 0x58;
static constexpr ptrdiff_t s_offUserDataPayload = 0x60;
static constexpr ptrdiff_t s_offAssetString     = 0x40;
#endif // CLIENT_DLL

// Datatable userdata typetag written by Script_GetDatatable
static constexpr uint64_t s_datatableUserDataTypeTag = 0xFFF7FFF700000004ull;

//-----------------------------------------------------------------------------
// Owned table block: one allocation [columns][rows][string pool]
//-----------------------------------------------------------------------------
struct DiskTableBlock
{
	DataTableHeader header;
	char*           memBlock;
	char            assetPath[DATATABLE_MAX_PATH_LEN];
};

static std::unordered_map<uint64_t, DiskTableBlock*> s_overrideMap;
static std::vector<DiskTableBlock*> s_retireList;

// One-entry memo (caches hits and misses). Cleared on publish / detach.
static uint64_t s_lastGuid = 0;
static const DataTableHeader* s_lastHeader = nullptr;
static bool s_lastGuidValid = false;

static bool s_patternsReady = false;
// Script_GetDatatable + sq_newuserdata/sq_settypetag for disk-only assets.
static bool s_forgeReady = false;
// Filesystem is not up at detour-attach time; scan on first script use.
static bool s_scanPending = true;

// Bounded always-on override diagnostics (fixed caps; no alloc on the hook path).
static ConVar sdk_datatable_diag("sdk_datatable_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Report disk-datatable override GUIDs and the GUIDs the scripts actually ask for.");

static uint32_t s_nDiagHits       = 0;
static uint32_t s_nDiagMissLookup = 0;
static uint32_t s_nDiagMissDecode = 0;
// Distinct miss guids only; fixed so the hot path never allocates or floods.
static constexpr uint32_t s_kDiagMissGuidCap = 64;
static uint64_t s_diagMissGuids[s_kDiagMissGuidCap];
static uint32_t s_nDiagMissGuids  = 0;
static bool     s_bDiagLoggedHit  = false;

//-----------------------------------------------------------------------------
// Column helpers
//-----------------------------------------------------------------------------
static int Datatable_GetColumnTypeSize(int type)
{
	switch (type)
	{
	case DTCOL_BOOL:             return 4;
	case DTCOL_INT:              return 4;
	case DTCOL_FLOAT:            return 4;
	case DTCOL_VECTOR:           return 12;
	case DTCOL_STRING:           return 8;
	case DTCOL_ASSET:            return 8;
	case DTCOL_ASSET_NOPRECACHE: return 8;
	default:                     return 0;
	}
}

static int Datatable_ParseColumnType(const char* typeName)
{
	if (!typeName)
		return -1;

	if (_stricmp(typeName, "bool") == 0)             return DTCOL_BOOL;
	if (_stricmp(typeName, "int") == 0)              return DTCOL_INT;
	if (_stricmp(typeName, "float") == 0)            return DTCOL_FLOAT;
	if (_stricmp(typeName, "vector") == 0)           return DTCOL_VECTOR;
	if (_stricmp(typeName, "string") == 0)           return DTCOL_STRING;
	if (_stricmp(typeName, "asset") == 0)            return DTCOL_ASSET;
	if (_stricmp(typeName, "asset_noprecache") == 0) return DTCOL_ASSET_NOPRECACHE;

	if (strlen(typeName) == 1 && typeName[0] >= '0' && typeName[0] <= '6')
		return typeName[0] - '0';

	return -1;
}

static std::vector<std::string> Datatable_SplitCSVLine(const char* line)
{
	std::vector<std::string> tokens;
	std::string current;
	bool inQuotes = false;

	while (*line)
	{
		if (*line == '\r' || *line == '\n')
		{
			line++;
			continue;
		}

		if (inQuotes)
		{
			if (*line == '"')
			{
				if (*(line + 1) == '"')
				{
					current += '"';
					line += 2;
				}
				else
				{
					inQuotes = false;
					line++;
				}
			}
			else
			{
				current += *line;
				line++;
			}
		}
		else
		{
			if (*line == '"')
			{
				inQuotes = true;
				line++;
			}
			else if (*line == ',')
			{
				tokens.push_back(current);
				current.clear();
				line++;
			}
			else
			{
				current += *line;
				line++;
			}
		}
	}

	tokens.push_back(current);
	return tokens;
}

static bool Datatable_ParseVector(const char* str, float* out)
{
	while (*str == '<' || *str == ' ' || *str == '\t')
		str++;

	if (sscanf(str, "%f %f %f", &out[0], &out[1], &out[2]) == 3)
		return true;
	if (sscanf(str, "%f , %f , %f", &out[0], &out[1], &out[2]) == 3)
		return true;
	if (sscanf(str, "%f,%f,%f", &out[0], &out[1], &out[2]) == 3)
		return true;

	return false;
}

static void Datatable_TrimInPlace(std::string& s)
{
	const size_t start = s.find_first_not_of(" \t\"");
	const size_t end = s.find_last_not_of(" \t\"");
	if (start == std::string::npos)
		s.clear();
	else
		s = s.substr(start, end - start + 1);
}

//-----------------------------------------------------------------------------
// Path component validation: reject .., separators, colon, leading dot
//-----------------------------------------------------------------------------
static bool Datatable_IsSafeComponent(const char* name)
{
	if (!name || !name[0])
		return false;
	if (name[0] == '.')
		return false;

	const size_t len = strlen(name);
	if (len >= DATATABLE_MAX_PATH_LEN)
		return false;

	for (size_t i = 0; i < len; i++)
	{
		const char c = name[i];
		if (c == '/' || c == '\\' || c == ':')
			return false;
		if (c == '.' && i + 1 < len && name[i + 1] == '.')
			return false;
	}
	return true;
}

static uint64_t Datatable_HashAssetPath(const char* assetPath)
{
	if ((reinterpret_cast<uintptr_t>(assetPath) & 3) == 0)
		return v_HashNameAligned(assetPath);
	return v_HashNameUnaligned(assetPath);
}

//-----------------------------------------------------------------------------
// CSV parse -> one DiskTableBlock
//-----------------------------------------------------------------------------
static DiskTableBlock* Datatable_ParseCSV(const char* csvPath, const char* assetPath)
{
	const HANDLE hFile = CreateFileA(csvPath, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
		return nullptr;

	const DWORD fileSize = GetFileSize(hFile, nullptr);
	if (fileSize == INVALID_FILE_SIZE || fileSize == 0 ||
		static_cast<size_t>(fileSize) > DATATABLE_MAX_FILE_SIZE)
	{
		CloseHandle(hFile);
		if (fileSize != INVALID_FILE_SIZE && fileSize != 0)
			Warning(eDLL_T::RTECH, "[DiskDatatable] File too large: %s (%lu bytes, max %zu)\n",
				csvPath, fileSize, DATATABLE_MAX_FILE_SIZE);
		return nullptr;
	}

	char* fileBuffer = new char[static_cast<size_t>(fileSize) + 1];
	DWORD bytesRead = 0;
	const BOOL readOk = ReadFile(hFile, fileBuffer, fileSize, &bytesRead, nullptr);
	CloseHandle(hFile);

	if (!readOk)
	{
		delete[] fileBuffer;
		Warning(eDLL_T::RTECH, "[DiskDatatable] Read failed: %s\n", csvPath);
		return nullptr;
	}
	fileBuffer[bytesRead] = '\0';

	std::vector<std::string> lines;
	{
		char* lineStart = fileBuffer;
		for (char* p = fileBuffer; ; p++)
		{
			if (*p == '\n' || *p == '\0')
			{
				std::string line(lineStart, p - lineStart);
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (!line.empty())
					lines.push_back(line);
				if (*p == '\0')
					break;
				lineStart = p + 1;
			}
		}
	}
	delete[] fileBuffer;

	// Whole-line comments only (// or # after optional leading whitespace).
	int commentSkipped = 0;
	{
		size_t out = 0;
		for (size_t i = 0; i < lines.size(); i++)
		{
			const char* p = lines[i].c_str();
			while (*p == ' ' || *p == '\t')
				p++;
			if (*p == '#' || (p[0] == '/' && p[1] == '/'))
			{
				commentSkipped++;
				continue;
			}
			if (out != i)
				lines[out] = std::move(lines[i]);
			++out;
		}
		lines.resize(out);
	}
	if (commentSkipped > 0 && sdk_datatable_diag.GetBool())
	{
		Msg(eDLL_T::RTECH, "[DiskDatatable] Skipped %d comment line(s) in %s\n",
			commentSkipped, csvPath);
	}

	if (lines.size() < 2)
	{
		Warning(eDLL_T::RTECH, "[DiskDatatable] CSV needs at least 2 lines: %s\n", csvPath);
		return nullptr;
	}

	std::vector<std::string> colNames = Datatable_SplitCSVLine(lines[0].c_str());
	const int columnCount = static_cast<int>(colNames.size());

	if (columnCount <= 0 || columnCount > DATATABLE_MAX_COLUMNS)
	{
		Warning(eDLL_T::RTECH, "[DiskDatatable] Invalid column count %d (max %d): %s\n",
			columnCount, DATATABLE_MAX_COLUMNS, csvPath);
		return nullptr;
	}

	std::vector<std::string> colTypeStrs;
	int dataStartLine = -1;
	int dataEndLine = -1;
	bool formatA = false;

	// Format A: types on line 2
	{
		std::vector<std::string> candidate = Datatable_SplitCSVLine(lines[1].c_str());
		bool allValid = (static_cast<int>(candidate.size()) == columnCount);

		if (allValid)
		{
			for (int i = 0; i < columnCount; i++)
			{
				std::string t = candidate[static_cast<size_t>(i)];
				Datatable_TrimInPlace(t);
				if (Datatable_ParseColumnType(t.c_str()) < 0)
				{
					allValid = false;
					break;
				}
			}
		}

		if (allValid)
		{
			colTypeStrs = std::move(candidate);
			dataStartLine = 2;
			dataEndLine = static_cast<int>(lines.size());
			formatA = true;
		}
	}

	// Format B: types on last line (only if A failed)
	if (colTypeStrs.empty())
	{
		const int lastIdx = static_cast<int>(lines.size()) - 1;
		std::vector<std::string> candidate = Datatable_SplitCSVLine(lines[static_cast<size_t>(lastIdx)].c_str());
		bool allValid = (static_cast<int>(candidate.size()) == columnCount);

		if (allValid)
		{
			for (int i = 0; i < columnCount; i++)
			{
				std::string t = candidate[static_cast<size_t>(i)];
				Datatable_TrimInPlace(t);
				if (Datatable_ParseColumnType(t.c_str()) < 0)
				{
					allValid = false;
					break;
				}
			}
		}

		if (allValid)
		{
			colTypeStrs = std::move(candidate);
			dataStartLine = 1;
			dataEndLine = lastIdx;
		}
	}

	if (colTypeStrs.empty())
	{
		Warning(eDLL_T::RTECH, "[DiskDatatable] No valid type line found: %s\n", csvPath);
		return nullptr;
	}

	if (sdk_datatable_diag.GetBool())
	{
		DevMsg(eDLL_T::RTECH, "[DiskDatatable] Using format %c for %s\n",
			formatA ? 'A' : 'B', csvPath);
	}

	std::vector<int> colTypes(static_cast<size_t>(columnCount));
	std::vector<int> colByteOffsets(static_cast<size_t>(columnCount));
	int rowSize = 0;

	for (int i = 0; i < columnCount; i++)
	{
		std::string typeStr = colTypeStrs[static_cast<size_t>(i)];
		Datatable_TrimInPlace(typeStr);

		colTypes[static_cast<size_t>(i)] = Datatable_ParseColumnType(typeStr.c_str());
		if (colTypes[static_cast<size_t>(i)] < 0)
		{
			Warning(eDLL_T::RTECH, "[DiskDatatable] Unknown type '%s' for column '%s': %s\n",
				typeStr.c_str(), colNames[static_cast<size_t>(i)].c_str(), csvPath);
			return nullptr;
		}

		colByteOffsets[static_cast<size_t>(i)] = rowSize;
		const int stride = Datatable_GetColumnTypeSize(colTypes[static_cast<size_t>(i)]);
		if (stride <= 0)
		{
			Warning(eDLL_T::RTECH, "[DiskDatatable] Bad type stride for column '%s': %s\n",
				colNames[static_cast<size_t>(i)].c_str(), csvPath);
			return nullptr;
		}
		if (rowSize > DATATABLE_MAX_ROW_STRIDE - stride)
		{
			Warning(eDLL_T::RTECH, "[DiskDatatable] Row stride exceeds cap %d: %s\n",
				DATATABLE_MAX_ROW_STRIDE, csvPath);
			return nullptr;
		}
		rowSize += stride;
	}

	if (rowSize <= 0 || rowSize > DATATABLE_MAX_ROW_STRIDE)
	{
		Warning(eDLL_T::RTECH, "[DiskDatatable] Invalid row stride %d (max %d): %s\n",
			rowSize, DATATABLE_MAX_ROW_STRIDE, csvPath);
		return nullptr;
	}

	const int rowCount = dataEndLine - dataStartLine;
	if (rowCount < 0 || rowCount > DATATABLE_MAX_ROWS)
	{
		Warning(eDLL_T::RTECH, "[DiskDatatable] Invalid row count %d (max %d): %s\n",
			rowCount, DATATABLE_MAX_ROWS, csvPath);
		return nullptr;
	}

	// String pool: one permanent empty + column names + every string/asset cell
	size_t stringPoolSize = 1; // permanent empty string

	for (int i = 0; i < columnCount; i++)
	{
		Datatable_TrimInPlace(colNames[static_cast<size_t>(i)]);
		stringPoolSize += colNames[static_cast<size_t>(i)].size() + 1;
	}

	std::vector<std::vector<std::string>> parsedRows;
	parsedRows.reserve(static_cast<size_t>(rowCount));

	for (int r = 0; r < rowCount; r++)
	{
		std::vector<std::string> cells = Datatable_SplitCSVLine(lines[static_cast<size_t>(dataStartLine + r)].c_str());
		parsedRows.push_back(cells);

		for (int c = 0; c < columnCount && c < static_cast<int>(cells.size()); c++)
		{
			const int t = colTypes[static_cast<size_t>(c)];
			if (t == DTCOL_STRING || t == DTCOL_ASSET || t == DTCOL_ASSET_NOPRECACHE)
			{
				std::string trimmed = cells[static_cast<size_t>(c)];
				const size_t s = trimmed.find_first_not_of(" \t");
				const size_t e = trimmed.find_last_not_of(" \t");
				if (s != std::string::npos)
					trimmed = trimmed.substr(s, e - s + 1);
				else
					trimmed.clear();
				stringPoolSize += trimmed.size() + 1;
			}
		}
	}

	// Overflow-safe sizing: check caps before multiplying
	if (static_cast<size_t>(columnCount) > SIZE_MAX / sizeof(DTColumnData))
	{
		Warning(eDLL_T::RTECH, "[DiskDatatable] Column data size overflow: %s\n", csvPath);
		return nullptr;
	}
	const size_t columnDataSize = static_cast<size_t>(columnCount) * sizeof(DTColumnData);

	if (rowCount > 0)
	{
		if (static_cast<size_t>(rowSize) > SIZE_MAX / static_cast<size_t>(rowCount))
		{
			Warning(eDLL_T::RTECH, "[DiskDatatable] Row data size overflow: %s\n", csvPath);
			return nullptr;
		}
	}
	const size_t rowDataSize = static_cast<size_t>(rowCount) * static_cast<size_t>(rowSize);

	if (columnDataSize > DATATABLE_MAX_TOTAL_BLOCK ||
		rowDataSize > DATATABLE_MAX_TOTAL_BLOCK ||
		stringPoolSize > DATATABLE_MAX_TOTAL_BLOCK)
	{
		Warning(eDLL_T::RTECH, "[DiskDatatable] Block component exceeds total cap: %s\n", csvPath);
		return nullptr;
	}

	if (columnDataSize > DATATABLE_MAX_TOTAL_BLOCK - rowDataSize)
	{
		Warning(eDLL_T::RTECH, "[DiskDatatable] Size overflow: %s\n", csvPath);
		return nullptr;
	}
	size_t totalSize = columnDataSize + rowDataSize;
	if (stringPoolSize > DATATABLE_MAX_TOTAL_BLOCK - totalSize)
	{
		Warning(eDLL_T::RTECH, "[DiskDatatable] Size overflow: %s\n", csvPath);
		return nullptr;
	}
	totalSize += stringPoolSize;

	char* memBlock = new char[totalSize];
	memset(memBlock, 0, totalSize);

	DTColumnData* columnData = reinterpret_cast<DTColumnData*>(memBlock);
	char* rowBuffer = memBlock + columnDataSize;
	char* stringPool = memBlock + columnDataSize + rowDataSize;
	char* stringCursor = stringPool;

	// Permanent empty string for missing string/asset cells
	const char* emptyStr = stringCursor;
	*stringCursor++ = '\0';

	for (int i = 0; i < columnCount; i++)
	{
		const std::string& name = colNames[static_cast<size_t>(i)];
		memcpy(stringCursor, name.c_str(), name.size() + 1);
		columnData[i].name = stringCursor;
		stringCursor += name.size() + 1;
		columnData[i].type = colTypes[static_cast<size_t>(i)];
		columnData[i].byteOffset = colByteOffsets[static_cast<size_t>(i)];
	}

	for (int r = 0; r < rowCount; r++)
	{
		const std::vector<std::string>& cells = parsedRows[static_cast<size_t>(r)];

		for (int c = 0; c < columnCount; c++)
		{
			char* cellPtr = rowBuffer + (static_cast<size_t>(r) * static_cast<size_t>(rowSize))
				+ static_cast<size_t>(colByteOffsets[static_cast<size_t>(c)]);

			std::string cellVal;
			const bool hasCell = (c < static_cast<int>(cells.size()));
			if (hasCell)
			{
				cellVal = cells[static_cast<size_t>(c)];
				const size_t s = cellVal.find_first_not_of(" \t");
				const size_t e = cellVal.find_last_not_of(" \t");
				if (s != std::string::npos)
					cellVal = cellVal.substr(s, e - s + 1);
				else
					cellVal.clear();
			}

			switch (colTypes[static_cast<size_t>(c)])
			{
			case DTCOL_BOOL:
			{
				int val = 0;
				if (hasCell && (_stricmp(cellVal.c_str(), "true") == 0 || cellVal == "1"))
					val = 1;
				*reinterpret_cast<int*>(cellPtr) = val;
				break;
			}
			case DTCOL_INT:
				*reinterpret_cast<int*>(cellPtr) = hasCell ? atoi(cellVal.c_str()) : 0;
				break;
			case DTCOL_FLOAT:
				*reinterpret_cast<float*>(cellPtr) = hasCell
					? static_cast<float>(atof(cellVal.c_str())) : 0.0f;
				break;
			case DTCOL_VECTOR:
			{
				float vec[3] = { 0.0f, 0.0f, 0.0f };
				if (hasCell)
					Datatable_ParseVector(cellVal.c_str(), vec);
				*reinterpret_cast<float*>(cellPtr + 0) = vec[0];
				*reinterpret_cast<float*>(cellPtr + 4) = vec[1];
				*reinterpret_cast<float*>(cellPtr + 8) = vec[2];
				break;
			}
			case DTCOL_STRING:
			case DTCOL_ASSET:
			case DTCOL_ASSET_NOPRECACHE:
				if (!hasCell || cellVal.empty())
				{
					*reinterpret_cast<const char**>(cellPtr) = emptyStr;
				}
				else
				{
					memcpy(stringCursor, cellVal.c_str(), cellVal.size() + 1);
					*reinterpret_cast<const char**>(cellPtr) = stringCursor;
					stringCursor += cellVal.size() + 1;
				}
				break;
			default:
				break;
			}
		}
	}

	DiskTableBlock* block = new DiskTableBlock;
	memset(block, 0, sizeof(*block));
	block->header.columnCount = columnCount;
	block->header.rowCount = rowCount;
	block->header.columnData = columnData;
	block->header.rows = rowBuffer;
	block->header.dataTableCRC = 0;
	block->header.rowSize = rowSize;
	block->memBlock = memBlock;
	block->assetPath[0] = '\0';
	if (assetPath)
	{
		V_strncpy(block->assetPath, assetPath, sizeof(block->assetPath));
		block->assetPath[sizeof(block->assetPath) - 1] = '\0';
	}

	return block;
}

//-----------------------------------------------------------------------------
// Lifetime: publish / retire
//-----------------------------------------------------------------------------
static void Datatable_ClearMemo(void)
{
	s_lastGuidValid = false;
	s_lastGuid = 0;
	s_lastHeader = nullptr;
}

static void Datatable_FreeBlock(DiskTableBlock* block)
{
	if (!block)
		return;
	delete[] block->memBlock;
	delete block;
}

static void Datatable_Publish(std::unordered_map<uint64_t, DiskTableBlock*>&& next)
{
	for (auto& pair : s_overrideMap)
		s_retireList.push_back(pair.second);
	s_overrideMap = std::move(next);
	Datatable_ClearMemo();

	s_nDiagHits = 0;
	s_nDiagMissLookup = 0;
	s_nDiagMissDecode = 0;
	s_nDiagMissGuids = 0;
	s_bDiagLoggedHit = false;
}

static void Datatable_FreeRetired(void)
{
	for (DiskTableBlock* block : s_retireList)
		Datatable_FreeBlock(block);
	s_retireList.clear();
}

static void Datatable_FreeAll(void)
{
	for (auto& pair : s_overrideMap)
		Datatable_FreeBlock(pair.second);
	s_overrideMap.clear();
	Datatable_FreeRetired();
	Datatable_ClearMemo();
}

//-----------------------------------------------------------------------------
// Disk scan
//-----------------------------------------------------------------------------
static int Datatable_LoadSingleFile(
	std::unordered_map<uint64_t, DiskTableBlock*>& next,
	const char* csvPath,
	const char* subdir,
	const char* filename)
{
	if (next.size() >= DATATABLE_MAX_DISK_ENTRIES)
	{
		Warning(eDLL_T::RTECH, "[DiskDatatable] Entry cap (%zu) reached, skipping %s\n",
			DATATABLE_MAX_DISK_ENTRIES, csvPath);
		return 0;
	}

	if (!Datatable_IsSafeComponent(filename))
	{
		Warning(eDLL_T::RTECH, "[DiskDatatable] Rejected unsafe filename: %s\n", filename);
		return 0;
	}

	char stem[DATATABLE_MAX_PATH_LEN];
	V_StripExtension(filename, stem, sizeof(stem));
	if (!Datatable_IsSafeComponent(stem))
	{
		Warning(eDLL_T::RTECH, "[DiskDatatable] Rejected unsafe stem: %s\n", filename);
		return 0;
	}

	if (subdir && subdir[0] && !Datatable_IsSafeComponent(subdir))
	{
		Warning(eDLL_T::RTECH, "[DiskDatatable] Rejected unsafe subdir: %s\n", subdir);
		return 0;
	}

	char assetPath[DATATABLE_MAX_PATH_LEN];
	if (subdir && subdir[0])
	{
		const int n = V_snprintf(assetPath, sizeof(assetPath), "datatable/%s/%s.rpak", subdir, stem);
		if (n <= 0 || static_cast<size_t>(n) >= sizeof(assetPath))
		{
			Warning(eDLL_T::RTECH, "[DiskDatatable] Asset path too long: %s\n", csvPath);
			return 0;
		}
	}
	else
	{
		const int n = V_snprintf(assetPath, sizeof(assetPath), "datatable/%s.rpak", stem);
		if (n <= 0 || static_cast<size_t>(n) >= sizeof(assetPath))
		{
			Warning(eDLL_T::RTECH, "[DiskDatatable] Asset path too long: %s\n", csvPath);
			return 0;
		}
	}

	if (strlen(csvPath) >= DATATABLE_MAX_PATH_LEN)
	{
		Warning(eDLL_T::RTECH, "[DiskDatatable] CSV path too long: %s\n", csvPath);
		return 0;
	}

	if (!v_HashNameAligned || !v_HashNameUnaligned)
		return 0;

	const uint64_t guid = Datatable_HashAssetPath(assetPath);

	if (next.find(guid) != next.end())
	{
		Warning(eDLL_T::RTECH, "[DiskDatatable] Duplicate guid for %s (asset %s), first wins\n",
			csvPath, assetPath);
		return 0;
	}

	DiskTableBlock* block = Datatable_ParseCSV(csvPath, assetPath);
	if (!block)
		return 0;

	next[guid] = block;
	if (sdk_datatable_diag.GetBool())
		Msg(eDLL_T::RTECH, "[DiskDatatable] Override %s -> %s\n", csvPath, assetPath);
	return 1;
}

static int Datatable_ScanDirectory(
	std::unordered_map<uint64_t, DiskTableBlock*>& next,
	const char* basePath,
	const char* subdir)
{
	int loaded = 0;
	char searchPath[DATATABLE_MAX_PATH_LEN * 2];

	if (subdir && subdir[0])
		V_snprintf(searchPath, sizeof(searchPath), "%s%s\\*.csv", basePath, subdir);
	else
		V_snprintf(searchPath, sizeof(searchPath), "%s*.csv", basePath);

	WIN32_FIND_DATAA fd;
	const HANDLE hFind = FindFirstFileA(searchPath, &fd);
	if (hFind == INVALID_HANDLE_VALUE)
		return 0;

	do
	{
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;

		char csvPath[DATATABLE_MAX_PATH_LEN * 2];
		if (subdir && subdir[0])
			V_snprintf(csvPath, sizeof(csvPath), "%s%s\\%s", basePath, subdir, fd.cFileName);
		else
			V_snprintf(csvPath, sizeof(csvPath), "%s%s", basePath, fd.cFileName);

		loaded += Datatable_LoadSingleFile(next, csvPath, subdir, fd.cFileName);
	}
	while (FindNextFileA(hFind, &fd));

	FindClose(hFind);
	return loaded;
}

static void Datatable_LoadAllDiskFiles(void)
{
	std::unordered_map<uint64_t, DiskTableBlock*> next;

	if (!s_patternsReady || !v_HashNameAligned || !v_HashNameUnaligned)
	{
		Datatable_Publish(std::move(next));
		return;
	}

	int loaded = 0;
	// Win32 against the process working directory, not IFileSystem: the S21
	// client's filesystem vtable does not match this header's layout, and
	// calling through it corrupts the caller's stack.
	const char* basePath = s_datatableDiskBasePath;

	loaded += Datatable_ScanDirectory(next, basePath, "");

	// One level of subdirectories only
	char searchPath[DATATABLE_MAX_PATH_LEN * 2];
	V_snprintf(searchPath, sizeof(searchPath), "%s*", basePath);

	WIN32_FIND_DATAA fd;
	const HANDLE hDir = FindFirstFileA(searchPath, &fd);
	if (hDir != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				continue;
			if (!Datatable_IsSafeComponent(fd.cFileName))
				continue;

			loaded += Datatable_ScanDirectory(next, basePath, fd.cFileName);
		}
		while (FindNextFileA(hDir, &fd));

		FindClose(hDir);
	}

	Datatable_Publish(std::move(next));
	Msg(eDLL_T::RTECH, "[DiskDatatable] Loaded %d override(s)\n", loaded);

	{
		WIN32_FIND_DATAA legacyFd;
		const HANDLE hLegacy = FindFirstFileA(
			"platform\\scripts\\datatable\\*.csv", &legacyFd);
		if (hLegacy != INVALID_HANDLE_VALUE)
		{
			Warning(eDLL_T::RTECH,
				"[DiskDatatable] Ignoring CSV(s) still in platform\\scripts\\datatable\\ -- the override directory moved to platform\\datatable\\\n");
			FindClose(hLegacy);
		}
	}

	if (sdk_datatable_diag.GetBool())
	{
		for (const auto& pair : s_overrideMap)
		{
			const DiskTableBlock* block = pair.second;
			if (!block)
				continue;
			Msg(eDLL_T::RTECH,
				"[DT-DIAG] override guid=0x%016llX rows=%d cols=%d asset='%s'\n",
				static_cast<unsigned long long>(pair.first),
				block->header.rowCount,
				block->header.columnCount,
				block->assetPath);
		}
	}
}

static void Datatable_EnsureScanned(void)
{
	if (!s_scanPending || !s_patternsReady)
		return;

	s_scanPending = false;
	Datatable_LoadAllDiskFiles();
}

//-----------------------------------------------------------------------------
// Stack ABI: read datatable guid from script userdata arg
//-----------------------------------------------------------------------------
static bool Datatable_TryReadGuid(void* sqvm, uint64_t* outGuid)
{
	if (!sqvm || !outGuid)
		return false;

	const uint64_t top = *reinterpret_cast<uint64_t*>(
		reinterpret_cast<char*>(sqvm) + s_offVmStackTop);
	if (!top)
		return false;

	const uint32_t objType = *reinterpret_cast<uint32_t*>(
		reinterpret_cast<char*>(top) + 0x10);
	if (objType != s_tagUserData)
		return false;

	const uint64_t obj = *reinterpret_cast<uint64_t*>(
		reinterpret_cast<char*>(top) + 0x18);
	if (!obj)
		return false;

	const uint64_t typeTag = *reinterpret_cast<uint64_t*>(
		reinterpret_cast<char*>(obj) + s_offUserDataTypeTag);
	if (typeTag != s_datatableUserDataTypeTag)
		return false;

	*outGuid = *reinterpret_cast<uint64_t*>(
		reinterpret_cast<char*>(obj) + s_offUserDataPayload);
	return true;
}

// Stack top is the asset arg to GetDataTable(...).
static const char* Datatable_TryReadAssetArg(void* sqvm)
{
	if (!sqvm)
		return nullptr;

	const uint64_t top = *reinterpret_cast<uint64_t*>(
		reinterpret_cast<char*>(sqvm) + s_offVmStackTop);
	if (!top)
		return nullptr;

	const uint32_t objType = *reinterpret_cast<uint32_t*>(
		reinterpret_cast<char*>(top) + 0x10);
	if (objType != s_tagAsset)
		return nullptr;

	const uint64_t obj = *reinterpret_cast<uint64_t*>(
		reinterpret_cast<char*>(top) + 0x18);
	if (!obj)
		return nullptr;

	return reinterpret_cast<const char*>(
		reinterpret_cast<char*>(obj) + s_offAssetString);
}

static const DataTableHeader* Datatable_LookupOverride(uint64_t guid)
{
	if (s_lastGuidValid && s_lastGuid == guid)
		return s_lastHeader;

	const auto it = s_overrideMap.find(guid);
	if (it != s_overrideMap.end())
	{
		s_lastGuid = guid;
		s_lastHeader = &it->second->header;
		s_lastGuidValid = true;
		return s_lastHeader;
	}

	s_lastGuid = guid;
	s_lastHeader = nullptr;
	s_lastGuidValid = true;
	return nullptr;
}

//-----------------------------------------------------------------------------
// Public resolve used by hooks and vscript_shared
//-----------------------------------------------------------------------------
const DataTableHeader* Datatable_ResolveFromScript(void* sqvm)
{
	if (!sqvm)
		return nullptr;

	Datatable_EnsureScanned();

	uint64_t guid = 0;
	if (Datatable_TryReadGuid(sqvm, &guid))
	{
		const DataTableHeader* overrideHdr = Datatable_LookupOverride(guid);
		if (overrideHdr)
			return overrideHdr;
	}

	if (g_nGetDataTableFromScriptCount > 0 && v_GetDataTableFromScript[0])
		return v_GetDataTableFromScript[0](sqvm);

	return nullptr;
}

//-----------------------------------------------------------------------------
// Script_GetDatatable: forge a handle when the CSV exists but no rpak asset does.
// Engine refuses at Pak_Find when the asset is missing; override alone never runs.
//-----------------------------------------------------------------------------
static int64_t Hook_Script_GetDatatable(void* sqvm)
{
	Datatable_EnsureScanned();

	if (s_forgeReady && !s_overrideMap.empty() && sqvm
		&& v_sq_newuserdata && v_sq_settypetag
		&& v_HashNameAligned && v_HashNameUnaligned)
	{
		const char* assetPath = Datatable_TryReadAssetArg(sqvm);
		if (assetPath && _strnicmp(assetPath, "datatable/", 10) == 0)
		{
			const uint64_t guid = Datatable_HashAssetPath(assetPath);
			if (Datatable_LookupOverride(guid))
			{
				uint64_t* const payload = v_sq_newuserdata(sqvm, 8);
				if (payload)
				{
					// Second arg unused on the userdata branch; tags stack top.
					v_sq_settypetag(sqvm, 0, s_datatableUserDataTypeTag);
					*payload = guid;
					if (sdk_datatable_diag.GetBool())
					{
						Msg(eDLL_T::RTECH,
							"[DT-DIAG] forge GetDataTable guid=0x%016llX asset='%s'\n",
							static_cast<unsigned long long>(guid), assetPath);
					}
					return 1;
				}
			}
		}
	}

	return v_Script_GetDatatable(sqvm);
}

//-----------------------------------------------------------------------------
// Hooks (no alloc, no I/O; diag logs are hard-capped)
//-----------------------------------------------------------------------------
static DataTableHeader* Hook_GetDataTableFromScript_Impl(void* sqvm, int which)
{
	Datatable_EnsureScanned();

	if (s_overrideMap.empty())
		return v_GetDataTableFromScript[which](sqvm);

	uint64_t guid = 0;
	if (!Datatable_TryReadGuid(sqvm, &guid))
	{
		++s_nDiagMissDecode;
		if (sdk_datatable_diag.GetBool() && s_nDiagMissDecode <= 4)
		{
			Msg(eDLL_T::RTECH,
				"[DT-DIAG] script arg not decodable as a datatable handle (miss #%u)\n",
				s_nDiagMissDecode);
		}
		return v_GetDataTableFromScript[which](sqvm);
	}

	const DataTableHeader* overrideHdr = Datatable_LookupOverride(guid);
	if (overrideHdr)
	{
		++s_nDiagHits;
		if (sdk_datatable_diag.GetBool() && !s_bDiagLoggedHit)
		{
			s_bDiagLoggedHit = true;
			const char* asset = "";
			const auto it = s_overrideMap.find(guid);
			if (it != s_overrideMap.end() && it->second)
				asset = it->second->assetPath;
			Msg(eDLL_T::RTECH,
				"[DT-DIAG] override HIT guid=0x%016llX asset='%s'\n",
				static_cast<unsigned long long>(guid), asset);
		}
		return const_cast<DataTableHeader*>(overrideHdr);
	}

	++s_nDiagMissLookup;
	if (sdk_datatable_diag.GetBool())
	{
		bool known = false;
		for (uint32_t i = 0; i < s_nDiagMissGuids; i++)
		{
			if (s_diagMissGuids[i] == guid)
			{
				known = true;
				break;
			}
		}
		if (!known && s_nDiagMissGuids < s_kDiagMissGuidCap)
		{
			s_diagMissGuids[s_nDiagMissGuids] = guid;
			++s_nDiagMissGuids;
			Msg(eDLL_T::RTECH,
				"[DT-DIAG] no override for guid=0x%016llX\n",
				static_cast<unsigned long long>(guid));
		}
	}
	return v_GetDataTableFromScript[which](sqvm);
}

static DataTableHeader* Hook_GetDataTableFromScript_0(void* sqvm)
{
	return Hook_GetDataTableFromScript_Impl(sqvm, 0);
}

static DataTableHeader* Hook_GetDataTableFromScript_1(void* sqvm)
{
	return Hook_GetDataTableFromScript_Impl(sqvm, 1);
}

//=============================================================================
// Commands
//=============================================================================
static void CC_DatatableReload(const CCommand& args)
{
	(void)args;
	if (!s_patternsReady)
	{
		Warning(eDLL_T::RTECH, "[DiskDatatable] Patterns not resolved; reload ignored\n");
		return;
	}

	Datatable_LoadAllDiskFiles();
	s_scanPending = false;
}

static ConCommand datatable_reload("datatable_reload", CC_DatatableReload,
	"Rescan platform/datatable/ and republish disk CSV overrides", FCVAR_RELEASE);

static void CC_DatatableDiagDump(const CCommand& args)
{
	(void)args;

	Msg(eDLL_T::RTECH,
		"[DT-DIAG] hits=%u miss_lookup=%u miss_decode=%u miss_guids=%u\n",
		s_nDiagHits, s_nDiagMissLookup, s_nDiagMissDecode, s_nDiagMissGuids);

	for (const auto& pair : s_overrideMap)
	{
		const DiskTableBlock* block = pair.second;
		if (!block)
			continue;
		Msg(eDLL_T::RTECH,
			"[DT-DIAG] override guid=0x%016llX rows=%d cols=%d asset='%s'\n",
			static_cast<unsigned long long>(pair.first),
			block->header.rowCount,
			block->header.columnCount,
			block->assetPath);
	}

	for (uint32_t i = 0; i < s_nDiagMissGuids && i < s_kDiagMissGuidCap; i++)
	{
		Msg(eDLL_T::RTECH,
			"[DT-DIAG] missed guid=0x%016llX\n",
			static_cast<unsigned long long>(s_diagMissGuids[i]));
	}
}

static ConCommand datatable_diag_dump("datatable_diag_dump", CC_DatatableDiagDump,
	"Dump disk-datatable override GUIDs, script miss GUIDs, and hit/miss counters",
	FCVAR_RELEASE);

//=============================================================================
// Detour class
//=============================================================================
void V_Datatable::GetFun(void) const
{
	g_nGetDataTableFromScriptCount = 0;
	v_GetDataTableFromScript[0] = nullptr;
	v_GetDataTableFromScript[1] = nullptr;
	v_Script_GetDatatable = nullptr;
	v_sq_newuserdata = nullptr;
	v_sq_settypetag = nullptr;
	v_HashNameAligned = nullptr;
	v_HashNameUnaligned = nullptr;
	s_patternsReady = false;
	s_forgeReady = false;

	// Shared: Script_GetDatatable prologue (1 hit on client and dedi).
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ? 56 48 83 EC ? 48 8B 41 ? 48 8D 1D")
		.GetPtr(v_Script_GetDatatable);
	if (!v_Script_GetDatatable)
		Warning(eDLL_T::RTECH, "[DiskDatatable] Script_GetDatatable pattern unresolved\n");

	// Success-path cluster: newuserdata(8) then settypetag(0xFFF7FFF700000004).
	// Unique on both products.
	CMemory forgeCluster = Module_FindPattern(g_GameDll,
		"BA 08 00 00 00 48 8B CE 48 8B F8 E8 ?? ?? ?? ?? "
		"49 B8 04 00 00 00 F7 FF F7 FF 48 8B CE 48 8B D8 E8 ?? ?? ?? ?? 48 89 3B");
	if (forgeCluster.IsValid())
	{
		forgeCluster.Offset(11).FollowNearCallSelf().GetPtr(v_sq_newuserdata);
		forgeCluster.Offset(32).FollowNearCallSelf().GetPtr(v_sq_settypetag);
	}
	if (!v_sq_newuserdata || !v_sq_settypetag)
		Warning(eDLL_T::RTECH,
			"[DiskDatatable] sq_newuserdata/sq_settypetag unresolved -- disk-only GetDataTable disabled\n");

#if defined(CLIENT_DLL)
	CMemory anchor = Module_FindPattern(g_GameDll,
		"48 8B 58 ? 48 B8 04 00 00 00 F7 FF F7 FF 48 39 43 ? 75");
	if (anchor.IsValid())
	{
		anchor.FindPatternSelf("40 53 48 83 EC", CMemory::Direction::UP)
			.GetPtr(v_GetDataTableFromScript[0]);
	}
	if (v_GetDataTableFromScript[0])
		g_nGetDataTableFromScriptCount = 1;
	else
		Warning(eDLL_T::RTECH, "[DiskDatatable] GetDataTableFromScript pattern unresolved\n");

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 41 56 45 33 C9")
		.GetPtr(v_HashNameAligned);
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 41 56 45 33 DB")
		.GetPtr(v_HashNameUnaligned);
#else
	// Dedi: two byte-identical GetDataTableFromScript copies (server + client/UI VMs).
	const CModule::ModuleSections_t* pText = g_GameDll.FindSectionByName(".text");
	if (pText && pText->IsSectionValid())
	{
		static const char* const kAnchor =
			"4C 8B 40 ? 48 B8 04 00 00 00 F7 FF F7 FF 49 39 40 ? 75";
		const CMemory base(pText->m_pSectionBase);
		const int scanLen = static_cast<int>(pText->m_nSectionSize);

		for (int occ = 1; occ <= 2; occ++)
		{
			CMemory gdtAnchor = base.FindPattern(kAnchor, CMemory::Direction::DOWN, scanLen, occ);
			if (!gdtAnchor.IsValid())
				break;

			CMemory entry = gdtAnchor.FindPattern("40 53 48 83 EC", CMemory::Direction::UP);
			if (!entry.IsValid())
			{
				Warning(eDLL_T::RTECH,
					"[DiskDatatable] GetDataTableFromScript prologue miss (occ %d)\n", occ);
				break;
			}

			entry.GetPtr(v_GetDataTableFromScript[g_nGetDataTableFromScriptCount]);
			g_nGetDataTableFromScriptCount++;
		}
	}

	if (g_nGetDataTableFromScriptCount == 0)
		Warning(eDLL_T::RTECH, "[DiskDatatable] GetDataTableFromScript pattern unresolved\n");
	else if (g_nGetDataTableFromScriptCount < 2)
		Warning(eDLL_T::RTECH,
			"[DiskDatatable] Expected 2 GetDataTableFromScript twins, found %d\n",
			g_nGetDataTableFromScriptCount);

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 41 56 41 57 45 33 C9")
		.GetPtr(v_HashNameAligned);
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 41 56 41 57 33 DB")
		.GetPtr(v_HashNameUnaligned);
#endif // CLIENT_DLL

	if (!v_HashNameAligned)
		Warning(eDLL_T::RTECH, "[DiskDatatable] HashNameAligned pattern unresolved\n");
	if (!v_HashNameUnaligned)
		Warning(eDLL_T::RTECH, "[DiskDatatable] HashNameUnaligned pattern unresolved\n");

	s_patternsReady = (g_nGetDataTableFromScriptCount > 0
		&& v_HashNameAligned && v_HashNameUnaligned);
	s_forgeReady = (v_Script_GetDatatable && v_sq_newuserdata && v_sq_settypetag
		&& v_HashNameAligned && v_HashNameUnaligned);

	if (!s_patternsReady)
		Warning(eDLL_T::RTECH, "[DiskDatatable] Feature disabled (pattern resolve failed)\n");
	else if (!s_forgeReady)
		Warning(eDLL_T::RTECH,
			"[DiskDatatable] Override ready; disk-only GetDataTable forge disabled\n");
}

void V_Datatable::Detour(const bool bAttach) const
{
	if (bAttach)
	{
		if (g_nGetDataTableFromScriptCount > 0 && v_GetDataTableFromScript[0])
			DetourSetup(&v_GetDataTableFromScript[0], &Hook_GetDataTableFromScript_0, bAttach);
		if (g_nGetDataTableFromScriptCount > 1 && v_GetDataTableFromScript[1])
			DetourSetup(&v_GetDataTableFromScript[1], &Hook_GetDataTableFromScript_1, bAttach);
		if (s_forgeReady && v_Script_GetDatatable)
			DetourSetup(&v_Script_GetDatatable, &Hook_Script_GetDatatable, bAttach);
	}
	else
	{
		if (g_nGetDataTableFromScriptCount > 0 && v_GetDataTableFromScript[0])
			DetourSetup(&v_GetDataTableFromScript[0], &Hook_GetDataTableFromScript_0, bAttach);
		if (g_nGetDataTableFromScriptCount > 1 && v_GetDataTableFromScript[1])
			DetourSetup(&v_GetDataTableFromScript[1], &Hook_GetDataTableFromScript_1, bAttach);
		if (v_Script_GetDatatable)
			DetourSetup(&v_Script_GetDatatable, &Hook_Script_GetDatatable, bAttach);

		Datatable_FreeAll();
		s_scanPending = true;
	}
}
