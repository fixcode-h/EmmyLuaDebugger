// Contract-only example. The host must resolve these functions from the
// injected emmy_hook.dll; it must not link a second EmmyFacade singleton.
#include <cstdint>
#include <string>

struct lua_State;

struct EmmyHostVmMetadata {
	uint32_t size;
	uint32_t version;
	const char* displayName;
	const char* engineName;
	const char* engineContext;
	const char* luaVersionHint;
	const char* runtimeModule;
};

using RegisterLuaVmFn = uint64_t (*)(lua_State*, const EmmyHostVmMetadata*);
using NotifyLuaVmReadyFn = int (*)(uint64_t);
using BeginLuaVmCloseFn = int (*)(uint64_t, const char*);
using EndLuaVmCloseFn = int (*)(uint64_t);
using ReleaseLuaVmRegistrationFn = int (*)(uint64_t);

// Pseudocode showing the required call order; GetProcAddress and error
// handling are intentionally left to the host integration layer.
void OnLuaEnvReady(lua_State* mainState,
	RegisterLuaVmFn registerVm,
	NotifyLuaVmReadyFn notifyReady,
	uint64_t& registrationId) {
	EmmyHostVmMetadata metadata{};
	metadata.size = sizeof(metadata);
	metadata.version = 1;
	metadata.displayName = "UnLua FLuaEnv";
	metadata.engineName = "Unreal/UnLua";
	metadata.luaVersionHint = "5.4.3";
	metadata.runtimeModule = "UnLua";
	registrationId = registerVm(mainState, &metadata);
	if (registrationId != 0) {
		notifyReady(registrationId);
	}
}

void OnLuaEnvDestroy(lua_State* state,
	uint64_t registrationId,
	BeginLuaVmCloseFn beginClose,
	EndLuaVmCloseFn endClose,
	ReleaseLuaVmRegistrationFn releaseRegistration,
	void (*luaClose)(lua_State*)) {
	if (registrationId == 0) return;
	beginClose(registrationId, "FLuaEnv destroyed");
	luaClose(state);
	endClose(registrationId);
	releaseRegistration(registrationId);
}
