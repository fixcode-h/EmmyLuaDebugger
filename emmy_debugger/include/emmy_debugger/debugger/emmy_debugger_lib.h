#pragma once

#include "emmy_debugger/api/lua_api.h"
#include "emmy_debugger/proto/proto.h"
#include "emmy_debugger/vm/lua_abi_descriptor.h"

#include <cstdint>

#if defined(_WIN32)
#define EMMY_HOST_API_EXPORT __declspec(dllexport)
#define EMMY_HOST_API_CALL __cdecl
#else
#define EMMY_HOST_API_EXPORT
#define EMMY_HOST_API_CALL
#endif

#define EMMY_HOST_API_VERSION 1u

#define EMMY_LUA_ABI_PRIVATE_LAYOUT_SUPPORTED 0x1u
#define EMMY_LUA_ABI_SP_HOOK 0x2u

typedef struct EmmyLuaAbiDescriptor {
	uint32_t size;
	uint32_t version;
	uint32_t major;
	uint32_t minor;
	const char* release;
	const char* layoutHash;
	uint32_t luaIdSize;
	uint32_t luaStateSize;
	uint64_t globalStateOffset;
	uint64_t callInfoOffset;
	uint32_t flags;
} EmmyLuaAbiDescriptor;

// C ABI metadata. String pointers are borrowed for the duration of the call.
typedef struct EmmyHostVmMetadata {
	uint32_t size;
	uint32_t version;
	const char* displayName;
	const char* engineName;
	const char* engineContext;
	const char* luaVersionHint;
	const char* runtimeModule;
	const EmmyLuaAbiDescriptor* abi;
} EmmyHostVmMetadata;

// Source metadata supplied by the host. String pointers are borrowed for the
// duration of the call and are copied by the debugger.
typedef struct EmmyLuaSourceIdentity {
	uint32_t size;
	uint32_t version;
	uint64_t sourceEpoch;
	const char* chunkName;
	const char* canonicalPath;
	const char* sha256;
} EmmyLuaSourceIdentity;

enum EmmyHostValueStatus {
	EMMY_HOST_VALUE_OK = 0,
	EMMY_HOST_VALUE_UNAVAILABLE = 1,
	EMMY_HOST_VALUE_NEEDS_GAME_THREAD = 2,
	EMMY_HOST_VALUE_OBJECT_INVALID = 3,
	EMMY_HOST_VALUE_FIELD_DENIED = 4,
	EMMY_HOST_VALUE_TIMEOUT = 5,
};

typedef struct EmmyHostValueRequest {
	uint32_t size;
	uint32_t version;
	uint64_t vmId;
	const char* threadId;
	const char* valueRef;
	const char* fieldPath;
	uint32_t maxDepth;
	uint32_t maxNodes;
	uint32_t maxBytes;
	uint64_t deadlineUnixMillis;
} EmmyHostValueRequest;

typedef struct EmmyHostValueResult {
	uint32_t size;
	uint32_t version;
	int32_t status;
	const char* typeName;
	const char* display;
	const char* serializedJson;
	const char* errorCode;
	int32_t truncated;
} EmmyHostValueResult;

typedef int (EMMY_HOST_API_CALL *EmmyDescribeHostValueFn)(
	void* userData, const EmmyHostValueRequest* request, EmmyHostValueResult* result);

typedef struct EmmyHostValueProvider {
	uint32_t size;
	uint32_t version;
	void* userData;
	EmmyDescribeHostValueFn describeUserdata;
	EmmyDescribeHostValueFn dispatchToGameThread;
} EmmyHostValueProvider;

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
	// Invalidates pause/frame/cache/evaluation/source references for a VM that
	// remains alive (for example an Unreal PIE restart or hot reload).
	EMMY_HOST_API_EXPORT int EMMY_HOST_API_CALL Emmy_ResetLuaVmContext(
		uint64_t registrationId, const char* reason);
	EMMY_HOST_API_EXPORT int EMMY_HOST_API_CALL Emmy_ReleaseLuaVmRegistration(uint64_t registrationId);
	EMMY_HOST_API_EXPORT int EMMY_HOST_API_CALL Emmy_SetLuaVmDisplayName(
		uint64_t registrationId, const char* displayName);
	EMMY_HOST_API_EXPORT int EMMY_HOST_API_CALL Emmy_RegisterLuaSource(
		uint64_t registrationId, const EmmyLuaSourceIdentity* source);
	EMMY_HOST_API_EXPORT uint64_t EMMY_HOST_API_CALL Emmy_RegisterHostValueProvider(
		const EmmyHostValueProvider* provider);
	EMMY_HOST_API_EXPORT int EMMY_HOST_API_CALL Emmy_UnregisterHostValueProvider(uint64_t providerId);
}

