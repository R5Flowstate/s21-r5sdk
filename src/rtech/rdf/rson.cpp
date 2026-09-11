#if defined(CLIENT_DLL)
#include "core/stdafx.h"
#include <tier0/memstd.h>
#include "tier1/utlbuffer.h"
#include <filesystem/filesystem.h>
#include "rtech/rson.h"

//-----------------------------------------------------------------------------
// Purpose: loads an RSON from a buffer
// Input: *bufName - 
// *buf - 
// rootType - 
// Output: pointer to RSON object on success, nullptr otherwise
//-----------------------------------------------------------------------------
RSON::Node_t* RSON::LoadFromBuffer(const char* const bufName, char* const buf, RSON::eFieldType rootType)
{
	return RSON_LoadFromBuffer(bufName, buf, rootType, 0, NULL);
}

//-----------------------------------------------------------------------------
// Purpose: loads an RSON from a file
// Input: *filePath - 
// *pathID - 
// Output: pointer to RSON object on success, nullptr otherwise
//-----------------------------------------------------------------------------
RSON::Node_t* RSON::LoadFromFile(const char* const filePath, const char* const pathID, bool* const parseFailure)
{
	if (parseFailure)
		*parseFailure = false;

	// IFileSystem optimal-I/O vtable is unsafe on S21; FileSystem_ReadAll uses IBaseFileSystem::Read.
	char* const fileBuf = FileSystem_ReadAll(filePath, pathID, nullptr);
	if (!fileBuf)
		return NULL;

	RSON::Node_t* const node = RSON::LoadFromBuffer(filePath, fileBuf, eFieldType::RSON_OBJECT);

	if (!node && parseFailure)
		*parseFailure = true;

	delete[] fileBuf;
	return node;
}
#else // !CLIENT_DLL
#include "core/stdafx.h"
#include <tier0/memstd.h>
#include "tier1/utlbuffer.h"
#include <filesystem/filesystem.h>
#include "rtech/rson.h"

//-----------------------------------------------------------------------------
// Purpose: loads an RSON from a buffer
// Input: *bufName - 
// *buf - 
// rootType - 
// Output: pointer to RSON object on success, nullptr otherwise
//-----------------------------------------------------------------------------
RSON::Node_t* RSON::LoadFromBuffer(const char* const bufName, char* const buf, RSON::eFieldType rootType)
{
	return RSON_LoadFromBuffer(bufName, buf, rootType, 0, NULL);
}

//-----------------------------------------------------------------------------
// Purpose: loads an RSON from a file
// Input: *filePath - 
// *pathID - 
// Output: pointer to RSON object on success, nullptr otherwise
//-----------------------------------------------------------------------------
RSON::Node_t* RSON::LoadFromFile(const char* const filePath, const char* const pathID, bool* const parseFailure)
{
	if (parseFailure)
		*parseFailure = false;

	FileHandle_t file = FileSystem()->Open(filePath, "rb", pathID);

	if (!file)
		return NULL;

	const ssize_t fileSize = FileSystem()->Size(file);

	if (fileSize <= 0)
	{
		FileSystem()->Close(file);
		return NULL;
	}

	const u64 bufSize = FileSystem()->GetOptimalReadSize(file, fileSize+2);
	char* const fileBuf = (char*)FileSystem()->AllocOptimalReadBuffer(file, bufSize, 0);

	if (!fileBuf)
	{
		FileSystem()->Close(file);
		return NULL;
	}

	const ssize_t numRead = FileSystem()->ReadEx(fileBuf, bufSize, fileSize, file);
	FileSystem()->Close(file);

	RSON::Node_t* node = NULL;

	if (numRead > 0)
	{
		fileBuf[fileSize] = '\0'; // null terminate file as EOF
		fileBuf[fileSize+1] = '\0'; // double NULL terminating in case this is an unicode file

		node = RSON::LoadFromBuffer(filePath, fileBuf, eFieldType::RSON_OBJECT);
	}

	if (!node && parseFailure)
		*parseFailure = true;

	FileSystem()->FreeOptimalReadBuffer(fileBuf);
	return node;
}
#endif // CLIENT_DLL
