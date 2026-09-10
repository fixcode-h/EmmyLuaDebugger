#include "emmy_debugger/vm/lua_abi_descriptor.h"

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
	const LuaAbiDescriptor host = MakeUnLua54_3AbiDescriptor();
	LuaAbiDescriptor dynamic;
	dynamic.major = 5;
	dynamic.minor = 4;
	std::string error;
	Require(ValidateLuaAbiDescriptorForRegistration(host, error),
		"host may register before a Lua API version is known");
	Require(!ValidateLuaAbiDescriptorForPublicApi(host, LuaAbiDescriptor(), error),
		"unknown API cannot authorize the first Lua access");

	Require(ValidateLuaAbiDescriptorForPublicApi(host, dynamic, error),
		"known host descriptor may use the public API with unknown private layout");
	Require(!ValidateLuaAbiDescriptor(host, dynamic, error) &&
		error == "LUA_PRIVATE_LAYOUT_UNAVAILABLE",
		"private layout access remains strict");

	LuaAbiDescriptor oversized = host;
	oversized.luaIdSize = 1025;
	Require(!ValidateLuaAbiDescriptorForPublicApi(oversized, dynamic, error) &&
		error == "LUA_IDSIZE_UNSAFE", "oversized LUA_IDSIZE is rejected");

	LuaAbiDescriptor wrongVersion = dynamic;
	wrongVersion.minor = 3;
	Require(!ValidateLuaAbiDescriptorForPublicApi(host, wrongVersion, error) &&
		error == "LUA_ABI_MINOR_MISMATCH", "mixed public Lua versions are rejected");
	LuaAbiDescriptor source = dynamic;
	source.luaIdSize = 60;
	Require(!ValidateLuaAbiDescriptorForPublicApi(host, source, error) && error == "LUA_IDSIZE_MISMATCH",
		"source header lua_Debug must match host LUA_IDSIZE");

	std::cout << "host ABI compatibility tests passed" << std::endl;
	return 0;
}
