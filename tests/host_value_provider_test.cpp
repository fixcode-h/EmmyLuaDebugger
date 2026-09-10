#include "emmy_debugger/vm/host_value_provider.h"

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {
void Require(bool condition, const char* message) {
	if (!condition) {
		std::cerr << message << std::endl;
		std::exit(1);
	}
}

class FixedProvider final : public HostValueProvider {
public:
	explicit FixedProvider(std::string json) : json_(std::move(json)) {}

	HostValueResult DescribeUserdata(const HostValueRequest&) override {
		HostValueResult result;
		result.status = HostValueStatus::Ok;
		result.typeName = "userdata";
		result.display = "display";
		result.serializedJson = json_;
		return result;
	}

private:
	std::string json_;
};

HostValueRequest Request() {
	HostValueRequest request;
	request.vmId = 1;
	request.threadId = "thread";
	request.valueRef = "value";
	return request;
}
}

int main() {
	HostValueProviderRegistry registry;
	registry.Set(std::make_shared<FixedProvider>(R"("中文\"quoted")"));

	HostValueRequest request = Request();
	request.limits.maxDepth = 0;
	request.limits.maxBytes = 1024;
	HostValueResult result = registry.Describe(request);
	Require(result.IsSuccess() && result.serializedJson.find("中文") != std::string::npos,
		"depth zero must allow a root scalar/object result and preserve UTF-8 JSON");

	request.limits.maxBytes = 8;
	result = registry.Describe(request);
	Require(result.status == HostValueStatus::Unavailable && result.truncated &&
		result.errorCode == "HOST_VALUE_RESULT_TOO_LARGE" && result.serializedJson.empty(),
		"oversized result must be explicitly rejected without invalid JSON");

	registry.Set(std::make_shared<FixedProvider>(R"({"a":{"b":1}})"));
	request = Request();
	request.limits.maxDepth = 1;
	result = registry.Describe(request);
	Require(result.errorCode == "HOST_VALUE_RESULT_INVALID", "nested result must honor maxDepth");

	request.limits.maxDepth = 2;
	result = registry.Describe(request);
	Require(result.IsSuccess(), "result at maxDepth must be accepted");

	registry.Set(std::make_shared<FixedProvider>("not-json"));
	result = registry.Describe(Request());
	Require(result.errorCode == "HOST_VALUE_RESULT_INVALID", "invalid provider JSON must be rejected");

	std::cout << "host value provider tests passed" << std::endl;
	return 0;
}
