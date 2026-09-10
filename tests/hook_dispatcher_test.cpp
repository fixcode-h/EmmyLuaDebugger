#include "emmy_debugger/debugger/hook_dispatcher.h"

#include <cstdlib>
#include <iostream>

namespace {
int hostHits = 0;
int debuggerHits = 0;

void HostHook(lua_State*, lua_Debug*) { ++hostHits; }
void DebuggerHook(lua_State*, lua_Debug*) { ++debuggerHits; }

void Require(bool value, const char* message) {
	if (!value) {
		std::cerr << message << std::endl;
		std::exit(1);
	}
}
}

int main() {
	lua_State* L = luaL_newstate();
	Require(L != nullptr, "lua state creation failed");
	HostHook(L, nullptr);
	lua_sethook(L, HostHook, LUA_MASKLINE, 0);
	Require(SetDebuggerHook(L, DebuggerHook, LUA_MASKLINE),
		"dispatcher installation failed");
	Require(lua_gethook(L) != HostHook && lua_gethookmask(L) == LUA_MASKLINE,
		"dispatcher did not replace the active hook");
	Require(luaL_loadstring(L, "local x = 1\nx = x + 1") == LUA_OK,
		"test chunk failed to load");
	Require(lua_pcall(L, 0, 0, 0) == LUA_OK, "test chunk failed to run");
	Require(hostHits > 0 && debuggerHits > 0, "both hooks did not receive line events");
	const int beforeCoroutine = debuggerHits;
	lua_State* coroutine = lua_newthread(L);
	Require(luaL_loadstring(coroutine, "local x=1\nx=x+1") == LUA_OK &&
		lua_pcall(coroutine, 0, 0, 0) == LUA_OK, "inherited coroutine hook runs");
	Require(debuggerHits > beforeCoroutine, "new coroutine inherits dispatcher behavior");
	Require(SetDebuggerHook(coroutine, DebuggerHook, LUA_MASKLINE), "explicit coroutine installation preserves inherited host");
	Require(ClearDebuggerHook(coroutine) && lua_gethook(coroutine) == HostHook, "coroutine restores inherited host hook");
	lua_pop(L, 1);

	// Main cleanup keeps host metadata available to inherited coroutines.
	lua_State* inherited = lua_newthread(L);
	const int debuggerBeforeMainClear = debuggerHits;
	const int hostBeforeClear = hostHits;
	Require(ClearDebuggerHook(L), "dispatcher clear failed");
	Require(lua_gethook(L) == HostHook && lua_gethookmask(L) == LUA_MASKLINE,
		"host hook was not restored");
	Require(luaL_loadstring(inherited, "local y=1\ny=y+1") == LUA_OK &&
		lua_pcall(inherited, 0, 0, 0) == LUA_OK, "inherited coroutine runs after main detach");
	Require(hostHits > hostBeforeClear && debuggerHits == debuggerBeforeMainClear,
		"main detach preserves inherited host events without debugger events");
	Require(lua_gethook(inherited) == HostHook, "inherited dispatcher restores host on owner thread");
	Require(SetDebuggerHook(inherited, DebuggerHook, LUA_MASKLINE), "inherited coroutine can re-install debugger hook");
	Require(ClearDebuggerHook(inherited) && lua_gethook(inherited) == HostHook,
		"inherited coroutine clear restores host after main clear");
	Require(luaL_loadstring(inherited, "local z=1\nz=z+1") == LUA_OK &&
		lua_pcall(inherited, 0, 0, 0) == LUA_OK, "cleared coroutine runs host hook");
	Require(debuggerHits == debuggerBeforeMainClear, "cleared main dispatcher does not receive debugger events");
	Require(ClearDebuggerHook(inherited), "inherited host-only dispatcher clears");
	ForgetDebuggerHooks(L);
	Require(!ClearDebuggerHook(inherited), "forgotten VM has no hook metadata");
	lua_pop(L, 1);
	lua_close(L);
	Require(hostHits >= hostBeforeClear, "host hook state was corrupted");
	std::cout << "hook dispatcher tests passed" << std::endl;
	return 0;
}
