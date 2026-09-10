#pragma once

#include <cstdint>
#include <string>

// Description of the Lua ABI visible to an injected Agent.  The descriptor is
// intentionally data-only; it is safe to copy across the Host C ABI boundary.
struct LuaAbiDescriptor {
	uint32_t major = 0;
	uint32_t minor = 0;
	std::string release;
	std::string layoutHash;
	uint32_t luaIdSize = 0;
	uint32_t luaStateSize = 0;
	uint64_t globalStateOffset = 0;
	uint64_t callInfoOffset = 0;
	bool privateLayoutSupported = false;
	bool spHook = false;

	bool IsSpecified() const {
		return major != 0 || minor != 0 || !release.empty() || !layoutHash.empty() ||
			luaIdSize != 0 || luaStateSize != 0 || globalStateOffset != 0 ||
			callInfoOffset != 0 || privateLayoutSupported || spHook;
	}

	std::string Fingerprint() const;
};

// Returns the ABI information known by the generic Emmy loader.  Dynamic
// loading cannot prove private layout compatibility, so that bit is false
// unless the library was built against the exact Lua sources.
LuaAbiDescriptor DetectLuaAbiDescriptor();

// Reference descriptor used by the UnLua 5.4.3 integration contract. It is
// intentionally explicit: a generic dynamically-loaded Lua API must never
// infer these private-layout facts from a 5.4 version number alone.
LuaAbiDescriptor MakeUnLua54_3AbiDescriptor();

// Validates only claims that can be checked safely. Private layout fields are
// compared when the host explicitly enables privateLayoutSupported; otherwise
// they are ignored and public API compatibility remains available.
bool ValidateLuaAbiDescriptor(const LuaAbiDescriptor& expected,
							  const LuaAbiDescriptor& actual,
							  std::string& error);

// Registration may use the public Lua API even when the generic loader cannot
// prove the host's private layout. Private-layout consumers must still call
// ValidateLuaAbiDescriptor and reject an unknown actual layout.
bool ValidateLuaAbiDescriptorForPublicApi(const LuaAbiDescriptor& expected,
										  const LuaAbiDescriptor& actual,
										  std::string& error);

// Registration never touches Lua or guesses the not-yet-loaded API version.
bool ValidateLuaAbiDescriptorForRegistration(const LuaAbiDescriptor& descriptor,
	std::string& error);
