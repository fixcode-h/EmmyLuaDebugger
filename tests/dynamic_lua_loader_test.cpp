#include "emmy_debugger/api/lua_api.h"
#include "emmy_debugger/api/lua_version.h"
#include <windows.h>
#include <cstdlib>
#include <iostream>

extern "C" bool SetupLuaAPI();

namespace {
void Require(bool value, const char* message) {
    if (!value) {
        std::cerr << "dynamic Lua loader: " << message << std::endl;
        std::exit(1);
    }
}
}

int main(int argc, char** argv) {
    Require(argc == 3, "expected Lua DLL path and version number");
    const auto module = LoadLibraryA(argv[1]);
    Require(module != nullptr, "Lua DLL loaded");
    Require(SetupLuaAPI(), "production dynamic API setup succeeds");
    const int expected = std::atoi(argv[2]);
    Require(static_cast<int>(luaVersion) == expected, "runtime version detected");
    Require((lua_getfenv != nullptr) == (expected == 51), "getfenv is optional on Lua 5.2+");
    const auto newState = reinterpret_cast<lua_State*(*)()>(GetProcAddress(module, "luaL_newstate"));
    const auto closeState = reinterpret_cast<void(*)(lua_State*)>(GetProcAddress(module, "lua_close"));
    Require(newState && closeState, "lifecycle API exported");
    auto* state = newState();
    Require(state != nullptr, "real Lua state created");
    lua_createtable(state, 0, 1);
    lua_pushstring(state, "answer");
    lua_pushnumber(state, 42);
    lua_rawset(state, -3);
    lua_pushstring(state, "answer");
    lua_rawget(state, -2);
    Require(lua_tonumber(state, -1) == 42 && lua_gettop(state) == 2, "dynamic raw value access");
    closeState(state);
    // API function pointers remain process-wide; keep the module loaded to process exit.
    std::cout << "{\"ok\":true,\"dynamicLuaVersion\":" << expected << "}" << std::endl;
}
