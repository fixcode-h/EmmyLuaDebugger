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

	const int hostBeforeClear = hostHits;
	Require(ClearDebuggerHook(L), "dispatcher clear failed");
	Require(lua_gethook(L) == HostHook && lua_gethookmask(L) == LUA_MASKLINE,
		"host hook was not restored");
	lua_close(L);
	Require(hostHits >= hostBeforeClear, "host hook state was corrupted");
	std::cout << "hook dispatcher tests passed" << std::endl;
	return 0;
}
