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

class ResultProvider final : public HostValueProvider {
public:
	HostValueResult DescribeUserdata(const HostValueRequest& request) override {
		if (request.fieldPath == "Destroyed") {
			HostValueResult result;
			result.status = HostValueStatus::ObjectInvalid;
			result.errorCode = "HOST_VALUE_OBJECT_INVALID";
			return result;
		}
		if (request.fieldPath == "Secret") {
			HostValueResult result;
			result.status = HostValueStatus::FieldDenied;
			result.errorCode = "HOST_VALUE_FIELD_DENIED";
			return result;
		}
		HostValueResult result;
		result.status = HostValueStatus::Ok;
		result.typeName = "FVector";
		result.display = "(1,2,3)";
		result.serializedJson = "{\"x\":1}";
		return result;
	}
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

	LuaAbiDescriptor unlua = MakeUnLua54_3AbiDescriptor();
	Require(unlua.major == 5 && unlua.minor == 4 && unlua.release == "5.4.3" &&
		unlua.luaIdSize == 256 && unlua.luaStateSize == 208 &&
		unlua.globalStateOffset == 24 && unlua.callInfoOffset == 32 &&
		unlua.privateLayoutSupported && unlua.spHook,
		"UnLua 5.4.3 fingerprint fixture is explicit");
	LuaAbiDescriptor compatible = unlua;
	Require(ValidateLuaAbiDescriptor(unlua, compatible, abiError),
		"matching UnLua fingerprint is accepted");
	compatible.major = 4;
	Require(!ValidateLuaAbiDescriptor(unlua, compatible, abiError) &&
		abiError == "LUA_ABI_MAJOR_MISMATCH", "major ABI mismatch is rejected");
	compatible = unlua;
	compatible.layoutHash = "other-layout";
	Require(!ValidateLuaAbiDescriptor(unlua, compatible, abiError) &&
		abiError == "LUA_LAYOUT_HASH_MISMATCH", "layout hash mismatch is rejected");
	compatible = unlua;
	compatible.luaIdSize = 60;
	Require(!ValidateLuaAbiDescriptor(unlua, compatible, abiError) &&
		abiError == "LUA_IDSIZE_MISMATCH", "LUA_IDSIZE mismatch is rejected");
	compatible = unlua;
	compatible.privateLayoutSupported = false;
	Require(!ValidateLuaAbiDescriptor(unlua, compatible, abiError) &&
		abiError == "LUA_PRIVATE_LAYOUT_UNAVAILABLE", "unknown private layout is rejected");
	compatible = unlua;
	compatible.luaStateSize = 0;
	Require(!ValidateLuaAbiDescriptor(unlua, compatible, abiError) &&
		abiError == "LUA_PRIVATE_LAYOUT_UNAVAILABLE", "missing lua_State size is rejected");
	compatible = unlua;
	compatible.globalStateOffset = 0;
	Require(!ValidateLuaAbiDescriptor(unlua, compatible, abiError) &&
		abiError == "LUA_PRIVATE_LAYOUT_UNAVAILABLE", "missing global-state offset is rejected");
	compatible = unlua;
	compatible.callInfoOffset = 0;
	Require(!ValidateLuaAbiDescriptor(unlua, compatible, abiError) &&
		abiError == "LUA_PRIVATE_LAYOUT_UNAVAILABLE", "missing CallInfo offset is rejected");
	compatible = unlua;
	compatible.spHook = false;
	Require(!ValidateLuaAbiDescriptor(unlua, compatible, abiError) &&
		abiError == "LUA_SP_HOOK_MISMATCH", "missing SP hook ABI is rejected");
	LuaAbiDescriptor incompleteExpected = unlua;
	incompleteExpected.layoutHash.clear();
	Require(!ValidateLuaAbiDescriptor(incompleteExpected, unlua, abiError) &&
		abiError == "LUA_PRIVATE_LAYOUT_DESCRIPTOR_INCOMPLETE",
		"host cannot request private access with an incomplete descriptor");

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

	registry.Set(std::make_shared<ResultProvider>());
	Require(IsSafeHostFieldPath("Actor.Location") && IsSafeHostFieldPath("Array[0]"),
		"reflection path syntax is accepted");
	Require(!IsSafeHostFieldPath("Actor..Location") && !IsSafeHostFieldPath("Actor;Secret") &&
		!IsSafeHostFieldPath("Array[]") && !IsSafeHostFieldPath("Array[foo.bar]") &&
		!IsSafeHostFieldPath("Array[\"unterminated]"),
		"malformed/reflection injection path is rejected");
	request = ValidRequest();
	result = registry.Describe(request);
	Require(result.status == HostValueStatus::Ok && result.typeName == "FVector" &&
		result.serializedJson == "{\"x\":1}", "host result is copied into a DTO");
	request.fieldPath = "Destroyed";
	result = registry.Describe(request);
	Require(result.status == HostValueStatus::ObjectInvalid &&
		result.errorCode == "HOST_VALUE_OBJECT_INVALID", "destroyed UObject is explicit");
	request.fieldPath = "Secret";
	result = registry.Describe(request);
	Require(result.status == HostValueStatus::FieldDenied &&
			result.errorCode == "HOST_VALUE_FIELD_DENIED", "non-whitelisted field is denied");
	// An oversized JSON copy is rejected intact, never cut into invalid JSON.
	class Utf8Provider final : public HostValueProvider {
	public:
		HostValueResult DescribeUserdata(const HostValueRequest&) override {
			HostValueResult value;
			value.status = HostValueStatus::Ok;
			value.serializedJson = "{\"text\":\"中文\"}";
			return value;
		}
	};
	auto utf8Provider = std::make_shared<Utf8Provider>();
	registry.Set(utf8Provider);
	request = ValidRequest();
	request.limits.maxBytes = 12;
	result = registry.Describe(request);
	Require(!result.IsSuccess() && result.errorCode == "HOST_VALUE_RESULT_TOO_LARGE",
			"oversized UTF-8 JSON fails without publishing a partial document");
	registry.Clear(provider);
	registry.Clear();
	Require(!registry.HasProvider(), "provider teardown clears registry");

	std::cout << "native contract tests passed" << std::endl;
	return 0;
}
