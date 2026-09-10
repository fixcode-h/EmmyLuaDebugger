#include "shared/shme.h"

std::string SharedMemoryName(DWORD processId) {
	return std::string(SHMEMNAME_PREFIX) + std::to_string(processId);
}

bool CreateMemFile(SharedFile* file, DWORD processId) {
	if (file == nullptr || processId == 0) return false;
	const std::string name = SharedMemoryName(processId);
	const auto mapFile = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, SHMEMSIZE, name.c_str());
	if (mapFile == nullptr) return false;
	const auto memFile = MapViewOfFile(mapFile, FILE_MAP_ALL_ACCESS, 0, 0, SHMEMSIZE);
	if (memFile == nullptr) {
		CloseHandle(mapFile);
		return false;
	}
	file->lpMemFile = memFile;
	file->hMapFile = mapFile;
	return true;
}

bool CloseMemFile(SharedFile* file) {
	if (file == nullptr) return false;
	if (file->lpMemFile != nullptr) UnmapViewOfFile(file->lpMemFile);
	if (file->hMapFile != nullptr) CloseHandle(file->hMapFile);
	file->lpMemFile = nullptr;
	file->hMapFile = nullptr;
	return true;
}

bool ValidateSharedData(const TSharedData& data, DWORD processId) {
	if (data.version != 1 || processId == 0 || data.processId != processId ||
		data.hModule == 0 || data.lpInit == 0 || data.dwOffset == 0) return false;
	if (data.lpInit < data.hModule || data.lpInit - data.hModule != data.dwOffset) return false;
	return true;
}

bool ReadSharedData(DWORD processId, TSharedData& data) {
	if (processId == 0) return false;
	const std::string name = SharedMemoryName(processId);
	SharedFile file = {};
	file.hMapFile = OpenFileMappingA(FILE_MAP_READ, FALSE, name.c_str());
	if (file.hMapFile == nullptr) return false;
	file.lpMemFile = MapViewOfFile(file.hMapFile, FILE_MAP_READ, 0, 0, SHMEMSIZE);
	if (file.lpMemFile == nullptr) {
		CloseHandle(file.hMapFile);
		return false;
	}
	memcpy(&data, file.lpMemFile, SHMEMSIZE);
	CloseMemFile(&file);
	return ValidateSharedData(data, processId);
}

bool WriteSharedData(HANDLE hMapFile, LPVOID lpMemFile, TSharedData& data) {
	if (hMapFile == nullptr || lpMemFile == nullptr) return false;
	memset(lpMemFile, 0, SHMEMSIZE);
	memcpy(lpMemFile, &data, sizeof(TSharedData));
	return true;
}
