#include "emmy_debugger/vm/lua_abi_descriptor.h"

#include <cstdlib>
#include <iostream>

namespace {
void Require(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "lua ABI descriptor test failed: " << message << std::endl;
		std::exit(1);
	}
}
}

int main() {
	const LuaAbiDescriptor unlua = MakeUnLua54_3AbiDescriptor();
	std::string error;
	Require(unlua.luaIdSize == 256 && unlua.luaStateSize == 208 &&
		unlua.globalStateOffset == 24 && unlua.callInfoOffset == 32,
		"fixture records the measured UnLua x64 layout");
	Require(ValidateLuaAbiDescriptor(unlua, unlua, error),
		"exact fixture is accepted");

	LuaAbiDescriptor generic;
	generic.major = 5;
	generic.minor = 4;
	Require(!generic.privateLayoutSupported && generic.release.empty() &&
		generic.layoutHash.empty(), "generic descriptor does not claim 5.4.3");
	Require(ValidateLuaAbiDescriptor(generic, unlua, error) &&
		error.empty(), "generic public descriptor remains usable without private access");
	const LuaAbiDescriptor detected = DetectLuaAbiDescriptor();
	Require(!detected.privateLayoutSupported && detected.layoutHash.empty() &&
		detected.release != "5.4.3", "unknown detection cannot masquerade as UnLua 5.4.3");

	LuaAbiDescriptor altered = unlua;
	altered.luaStateSize++;
	Require(!ValidateLuaAbiDescriptor(unlua, altered, error) &&
		error == "LUA_STATE_SIZE_MISMATCH", "state size mismatch is rejected");
	altered = unlua;
	altered.globalStateOffset++;
	Require(!ValidateLuaAbiDescriptor(unlua, altered, error) &&
		error == "LUA_GLOBAL_STATE_OFFSET_MISMATCH", "global offset mismatch is rejected");
	altered = unlua;
	altered.callInfoOffset++;
	Require(!ValidateLuaAbiDescriptor(unlua, altered, error) &&
		error == "LUA_CALL_INFO_OFFSET_MISMATCH", "call info offset mismatch is rejected");
	altered = unlua;
	altered.luaIdSize++;
	Require(!ValidateLuaAbiDescriptor(unlua, altered, error) &&
		error == "LUA_IDSIZE_MISMATCH", "LUA_IDSIZE mismatch is rejected");
	altered = unlua;
	altered.layoutHash += "-altered";
	Require(!ValidateLuaAbiDescriptor(unlua, altered, error) &&
		error == "LUA_LAYOUT_HASH_MISMATCH", "layout hash mismatch is rejected");

	std::cout << "lua ABI descriptor tests passed" << std::endl;
	return 0;
}
