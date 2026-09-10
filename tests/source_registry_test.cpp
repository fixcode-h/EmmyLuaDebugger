#include "emmy_debugger/vm/source_registry.h"

#include <cstdlib>
#include <iostream>

namespace {
const char* Hash() {
	return "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
}

HostSourceIdentity Source(uint64_t epoch = 1) {
	HostSourceIdentity value;
	value.vmId = 7;
	value.sourceEpoch = epoch;
	value.chunkName = "@Game/Main.lua";
	value.canonicalPath = "C:\\Game\\Main.lua";
	value.sha256 = Hash();
	return value;
}

void Require(bool condition, const char* message) {
	if (!condition) {
		std::cerr << message << std::endl;
		std::exit(1);
	}
}
}

int main() {
	SourceRegistry registry(2);
	HostSourceIdentity result;
	Require(registry.Register(Source()), "source registration failed");
	Require(registry.Resolve(7, 1, "@Game/Main.lua", result) &&
		result.canonicalPath == "C:/Game/Main.lua" && result.sha256 == Hash(),
		"exact source resolution failed");
	Require(!registry.Resolve(7, 1, "Main.lua", result), "non-legacy fuzzy match must be rejected");
	Require(registry.Resolve(7, 1, "Main.lua", result, true), "legacy fuzzy match failed");
	Require(!registry.Register(Source(0)), "zero source epoch must be rejected");
	HostSourceIdentity second = Source(2);
	second.chunkName = "@Game/Other.lua";
	Require(registry.Register(second), "second source registration failed");
	HostSourceIdentity overflow = Source(3);
	overflow.chunkName = "@Game/Overflow.lua";
	Require(!registry.Register(overflow), "registry must enforce its bound");
	Require(registry.Invalidate(7, 1) == 1 && registry.Size() == 1,
		"epoch invalidation failed");
	Require(registry.Invalidate(7) == 1 && registry.Size() == 0,
		"VM invalidation failed");
	std::cout << "source registry tests passed" << std::endl;
	return 0;
}
