#include "emmy_debugger/vm/lua_abi_descriptor.h"

#include <iomanip>
#include <sstream>

#include "emmy_debugger/api/lua_version.h"

#ifdef EMMY_USE_LUA_SOURCE
extern "C" {
#include "lua.h"
}
#else
#include "emmy_debugger/api/lua_api_loader.h"
#endif

std::string LuaAbiDescriptor::Fingerprint() const {
	std::ostringstream stream;
	stream << major << "." << minor << "|" << release << "|" << layoutHash
		   << "|id=" << luaIdSize << "|state=" << luaStateSize
		   << "|g=" << globalStateOffset << "|ci=" << callInfoOffset
		   << "|private=" << (privateLayoutSupported ? 1 : 0)
		   << "|sp=" << (spHook ? 1 : 0);
	return stream.str();
}

LuaAbiDescriptor DetectLuaAbiDescriptor() {
	LuaAbiDescriptor descriptor;
	switch (luaVersion) {
		case LuaVersion::LUA_JIT:
			descriptor.major = 5;
			descriptor.minor = 0;
			descriptor.release = "LuaJIT";
			break;
		case LuaVersion::LUA_51:
			descriptor.major = 5;
			descriptor.minor = 1;
			break;
		case LuaVersion::LUA_52:
			descriptor.major = 5;
			descriptor.minor = 2;
			break;
		case LuaVersion::LUA_53:
			descriptor.major = 5;
			descriptor.minor = 3;
			break;
		case LuaVersion::LUA_54:
			descriptor.major = 5;
			descriptor.minor = 4;
			break;
		default:
			break;
	}
#ifdef EMMY_USE_LUA_SOURCE
	// Source builds know the exact public headers used for this binary. The
	// dynamic loader deliberately leaves release/layout/LUA_IDSIZE unknown,
	// because its compatibility header is not proof of the host's ABI.
	descriptor.luaIdSize = LUA_IDSIZE;
	#if defined(LUA_VERSION_RELEASE)
		descriptor.release = std::string("5.") +
			(luaVersion == LuaVersion::LUA_54 ? "4." :
			 luaVersion == LuaVersion::LUA_53 ? "3." :
			 luaVersion == LuaVersion::LUA_52 ? "2." :
			 luaVersion == LuaVersion::LUA_51 ? "1." : "0.") +
			LUA_VERSION_RELEASE;
	#endif
	descriptor.privateLayoutSupported = true;
	// The source build has no generated hash at this layer. Hosts that need
	// private access must provide an explicit layoutHash.
	descriptor.layoutHash = "source-build";
#endif
	return descriptor;
}

LuaAbiDescriptor MakeUnLua54_3AbiDescriptor() {
	LuaAbiDescriptor descriptor;
	descriptor.major = 5;
	descriptor.minor = 4;
	descriptor.release = "5.4.3";
	// Measured from UnLua's bundled lua-5.4.3 headers with the x64 ABI and
	// UnLua's CMake LUA_IDSIZE=256 definition: sizeof(lua_State)=208,
	// l_G=24, ci=32, sphook=168. The stock header default is 60 and is not a
	// runtime fingerprint.
	descriptor.layoutHash = "lua-5.4.3-x64-state208-lg24-ci32-sphook168-idsize256";
	descriptor.luaIdSize = 256;
	// x64 UnLua 5.4.3 (the SP-hook fork) layout contract. Version strings
	// alone are not sufficient to authorize private-structure access.
	descriptor.luaStateSize = 208;
	descriptor.globalStateOffset = 24;
	descriptor.callInfoOffset = 32;
	descriptor.privateLayoutSupported = true;
	descriptor.spHook = true;
	return descriptor;
}

bool ValidateLuaAbiDescriptor(const LuaAbiDescriptor& expected,
							  const LuaAbiDescriptor& actual,
							  std::string& error) {
	error.clear();
	if (!expected.IsSpecified()) return true;
	if (expected.major != 0 && (actual.major == 0 || expected.major != actual.major)) {
		error = "LUA_ABI_MAJOR_MISMATCH";
		return false;
	}
	if (expected.minor != 0 && (actual.minor == 0 || expected.minor != actual.minor)) {
		error = "LUA_ABI_MINOR_MISMATCH";
		return false;
	}
	if (!expected.release.empty() && !actual.release.empty() &&
			expected.release != actual.release) {
		error = "LUA_ABI_RELEASE_MISMATCH";
		return false;
	}
	if (!expected.privateLayoutSupported) {
		// A host that does not use private structures can safely run with a
		// different LUA_IDSIZE/layout as long as the public major/minor matches.
		return true;
	}
	if (!actual.privateLayoutSupported) {
		error = "LUA_PRIVATE_LAYOUT_UNAVAILABLE";
		return false;
	}
	if (expected.layoutHash.empty() || expected.luaIdSize == 0 ||
		expected.luaStateSize == 0 || expected.globalStateOffset == 0 ||
		expected.callInfoOffset == 0) {
		error = "LUA_PRIVATE_LAYOUT_DESCRIPTOR_INCOMPLETE";
		return false;
	}
	if (actual.layoutHash.empty() || actual.luaIdSize == 0 ||
		actual.luaStateSize == 0 || actual.globalStateOffset == 0 ||
		actual.callInfoOffset == 0) {
		error = "LUA_PRIVATE_LAYOUT_UNAVAILABLE";
		return false;
	}
	if (!expected.release.empty() && actual.release.empty()) {
		error = "LUA_ABI_RELEASE_MISSING";
		return false;
	}
	if (!expected.layoutHash.empty() && expected.layoutHash != actual.layoutHash) {
		error = "LUA_LAYOUT_HASH_MISMATCH";
		return false;
	}
	if (expected.luaIdSize != actual.luaIdSize) {
		error = "LUA_IDSIZE_MISMATCH";
		return false;
	}
	if (expected.luaStateSize != actual.luaStateSize) {
		error = "LUA_STATE_SIZE_MISMATCH";
		return false;
	}
	if (expected.globalStateOffset != actual.globalStateOffset) {
		error = "LUA_GLOBAL_STATE_OFFSET_MISMATCH";
		return false;
	}
	if (expected.callInfoOffset != actual.callInfoOffset) {
		error = "LUA_CALL_INFO_OFFSET_MISMATCH";
		return false;
	}
	if (expected.spHook && !actual.spHook) {
		error = "LUA_SP_HOOK_MISMATCH";
		return false;
	}
	return true;
}
