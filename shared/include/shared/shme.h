#ifndef _SHME_H_
#define _SHME_H_

#include <Windows.h>
#include <cstdint>
#include <string>

// Data struct to be shared between processes
struct TSharedData {
	uint32_t version = 1;
	DWORD processId = 0;
	uintptr_t dwOffset = 0;
	uintptr_t hModule = 0;
	uintptr_t lpInit = 0;
};

struct SharedFile {
	HANDLE hMapFile;
	LPVOID lpMemFile;
};

struct RemoteThreadParam
{
	BOOL bRedirect = FALSE;
	char authToken[128] = {};
};

// Size (in bytes) of data to be shared
#define SHMEMSIZE sizeof(TSharedData)
// Name of the shared file map (NOTE: Global namespaces must have the SeCreateGlobalPrivilege privilege)
#define SHMEMNAME_PREFIX "InjectedDllName_SHMEM_"

std::string SharedMemoryName(DWORD processId);

bool CreateMemFile(SharedFile* file, DWORD processId);

bool CloseMemFile(SharedFile* file);

bool ReadSharedData(DWORD processId, TSharedData& data);
bool ValidateSharedData(const TSharedData& data, DWORD processId);

bool WriteSharedData(HANDLE hMapFile, LPVOID lpMemFile, TSharedData& data);
#endif
