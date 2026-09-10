#include "shared/shme.h"

#include <cstdlib>
#include <iostream>

namespace {
void Require(bool condition, const char* message) {
	if (!condition) {
		std::cerr << message << std::endl;
		std::exit(1);
	}
}
}

int main() {
	const DWORD processId = GetCurrentProcessId();
	const DWORD missingProcessId = 0xfffffffeu;
	TSharedData ignored;
	Require(!ReadSharedData(missingProcessId, ignored), "missing mapping must not be created by a read");
	Require(OpenFileMappingA(FILE_MAP_READ, FALSE, SharedMemoryName(missingProcessId).c_str()) == nullptr,
		"read failure leaves no mapping object behind");
	Require(SharedMemoryName(processId) != SharedMemoryName(missingProcessId), "mapping names must be PID scoped");

	SharedFile file = {};
	Require(CreateMemFile(&file, processId), "failed to create test mapping");
	TSharedData data;
	data.processId = processId;
	data.hModule = 0x1000;
	data.dwOffset = 0x200;
	data.lpInit = 0x1200;
	Require(WriteSharedData(file.hMapFile, file.lpMemFile, data), "failed to write test mapping");

	TSharedData loaded;
	Require(ReadSharedData(processId, loaded), "failed to read PID-scoped mapping");
	Require(loaded.processId == processId && ValidateSharedData(loaded, processId),
		"valid mapping schema was rejected");
	SharedFile second = {};
	const DWORD secondId = processId | 0x80000000u;
	Require(CreateMemFile(&second, secondId), "second target mapping is independent");
	TSharedData secondData = data;
	secondData.processId = secondId;
	secondData.hModule = 0x2000;
	secondData.lpInit = 0x2200;
	Require(WriteSharedData(second.hMapFile, second.lpMemFile, secondData), "second mapping writes");
	Require(ReadSharedData(processId, loaded) && loaded.hModule == 0x1000,
		"another target cannot overwrite the first target's entry");
	TSharedData secondLoaded;
	Require(ReadSharedData(secondId, secondLoaded) && secondLoaded.hModule == 0x2000,
		"second target reads its own entry");
	CloseMemFile(&second);
	loaded.version = 0;
	Require(!ValidateSharedData(loaded, processId), "unknown mapping schema is rejected");
	loaded.version = 1;
	loaded.processId = missingProcessId;
	Require(!ValidateSharedData(loaded, processId), "wrong PID must be rejected");
	loaded.processId = processId;
	loaded.lpInit = 0x1300;
	Require(!ValidateSharedData(loaded, processId), "inconsistent entry offset must be rejected");
	CloseMemFile(&file);

	std::cout << "injection shared memory tests passed" << std::endl;
	return 0;
}
