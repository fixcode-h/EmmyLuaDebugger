#pragma once

#include "emmy_debugger/api/lua_api.h"
#include "emmy_debugger/proto/proto.h"

#include <cstdint>

#if defined(_WIN32)
#define EMMY_HOST_API_EXPORT __declspec(dllexport)
#define EMMY_HOST_API_CALL __cdecl
#else
#define EMMY_HOST_API_EXPORT
#define EMMY_HOST_API_CALL
#endif

#define EMMY_HOST_API_VERSION 1u

// C ABI metadata. String pointers are borrowed for the duration of the call.
typedef struct EmmyHostVmMetadata {
	uint32_t size;
	uint32_t version;
	const char* displayName;
	const char* engineName;
	const char* engineContext;
	const char* luaVersionHint;
	const char* runtimeModule;
} EmmyHostVmMetadata;

bool query_variable(lua_State* L, std::shared_ptr<Variable> variable, const char* typeName, int object, int depth);

// emmy.tcpListen(host: string, port: int): bool
int tcpListen(struct lua_State* L);

// emmy.tcpConnect(host: string, port: int): bool
int tcpConnect(lua_State* L);

// emmy.pipeListen(pipeName: string): bool
int pipeListen(lua_State* L);

// emmy.pipeConnect(pipeName: string): bool
int pipeConnect(lua_State* L);

// emmy.breakHere(): bool
int breakHere(lua_State* L);

// emmy.waitIDE(timeout: number): void
int waitIDE(lua_State* L);

int tcpSharedListen(lua_State* L);

// emmy.stop()
int stop(lua_State* L);

// emmy.registerTypeName(typeName: string): bool
int registerTypeName(lua_State* L);

// emmy.sendLog(type: int, message: string): void
int sendLog(lua_State* L);

bool install_emmy_debugger(struct lua_State* L);

std::string prepareEvalExpr(const std::string& eval);

extern "C" {
	EMMY_HOST_API_EXPORT uint64_t EMMY_HOST_API_CALL Emmy_RegisterLuaVm(
		lua_State* L, const EmmyHostVmMetadata* metadata);
	EMMY_HOST_API_EXPORT int EMMY_HOST_API_CALL Emmy_NotifyLuaVmReady(uint64_t registrationId);
	EMMY_HOST_API_EXPORT int EMMY_HOST_API_CALL Emmy_BeginLuaVmClose(
		uint64_t registrationId, const char* reason);
	EMMY_HOST_API_EXPORT int EMMY_HOST_API_CALL Emmy_EndLuaVmClose(uint64_t registrationId);
	EMMY_HOST_API_EXPORT int EMMY_HOST_API_CALL Emmy_ReleaseLuaVmRegistration(uint64_t registrationId);
	EMMY_HOST_API_EXPORT int EMMY_HOST_API_CALL Emmy_SetLuaVmDisplayName(
		uint64_t registrationId, const char* displayName);
}

