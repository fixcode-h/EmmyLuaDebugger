#include "emmy_debugger/vm/host_value_provider.h"
#include "emmy_debugger/vm/lua_abi_descriptor.h"
#include "hook_manager.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

void Require(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "native contract test failed: " << message << std::endl;
		std::exit(1);
	}
}

uint64_t NowUnixMillis() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count());
}

HostValueRequest ValidRequest() {
	HostValueRequest request;
	request.vmId = 7;
	request.threadId = "thread-1";
	request.valueRef = "opaque-1";
	request.fieldPath = "Actor.Location";
	return request;
}

class DeadlineProvider final : public HostValueProvider {
public:
	HostValueResult DescribeUserdata(const HostValueRequest&) override {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		HostValueResult result;
		result.status = HostValueStatus::NeedsGameThread;
		return result;
	}

	HostValueResult DispatchToGameThread(const HostValueRequest&) override {
		++dispatchCalls;
		HostValueResult result;
		result.status = HostValueStatus::Ok;
		result.display = "should-not-run-after-deadline";
		return result;
	}

	int dispatchCalls = 0;
};

} // namespace

int main() {
	// A disable fence prevents new callback entries and waits for in-flight work.
	HookManager hooks;
	hooks.Enable();
	std::atomic<bool> release(false);
	std::atomic<bool> entered(false);
	std::thread callback([&] {
		HookManager::CallbackScope scope(hooks);
		Require(static_cast<bool>(scope), "callback enters while enabled");
		entered.store(true, std::memory_order_release);
		while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
	});
	while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
	Require(!hooks.DisableAndQuiesce(std::chrono::milliseconds(1)),
		"short teardown deadline reports in-flight callback");
	release.store(true, std::memory_order_release);
	callback.join();
	Require(hooks.DisableAndQuiesce(std::chrono::milliseconds(100)),
		"teardown reaches quiescence after callback exits");
	Require(!hooks.EnterCallback(), "disabled hook rejects new callback");

	LuaAbiDescriptor expected;
	expected.major = 5;
	expected.minor = 4;
	LuaAbiDescriptor actual = expected;
	actual.minor = 3;
	std::string abiError;
	Require(!ValidateLuaAbiDescriptor(expected, actual, abiError),
		"Lua minor ABI mismatch is rejected");
	Require(abiError == "LUA_ABI_MINOR_MISMATCH", "minor mismatch has stable error code");
	expected.privateLayoutSupported = true;
	actual = expected;
	actual.privateLayoutSupported = false;
	Require(!ValidateLuaAbiDescriptor(expected, actual, abiError),
		"private ABI without host layout is rejected");
	Require(abiError == "LUA_PRIVATE_LAYOUT_UNAVAILABLE",
		"private ABI mismatch has stable error code");

	HostValueProviderRegistry registry;
	HostValueRequest request = ValidRequest();
	HostValueResult result = registry.Describe(request);
	Require(result.status == HostValueStatus::Unavailable &&
			result.errorCode == "HOST_VALUE_UNAVAILABLE",
		"missing host provider is explicit");
	request.limits.deadlineUnixMillis = NowUnixMillis() - 1;
	result = registry.Describe(request);
	Require(result.status == HostValueStatus::Timeout &&
			result.errorCode == "HOST_VALUE_TIMEOUT",
		"expired host value deadline is rejected");

	auto provider = std::make_shared<DeadlineProvider>();
	registry.Set(provider);
	request = ValidRequest();
	request.limits.deadlineUnixMillis = NowUnixMillis() + 1;
	result = registry.Describe(request);
	Require(result.status == HostValueStatus::Timeout &&
			result.errorCode == "HOST_VALUE_TIMEOUT",
		"deadline is rechecked before game-thread dispatch");
	Require(provider->dispatchCalls == 0, "expired request is not dispatched");
	registry.Clear(provider);
	Require(!registry.HasProvider(), "provider teardown clears registry");

	std::cout << "native contract tests passed" << std::endl;
	return 0;
}
