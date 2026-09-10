#include "emmy_debugger/vm/host_value_provider.h"
#include "nlohmann/json.hpp"

#include <chrono>
#include <cctype>

namespace {
uint64_t NowUnixMillis() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count());
}

HostValueResult Error(HostValueStatus status, const char* code) {
	HostValueResult result;
	result.status = status;
	result.errorCode = code;
	return result;
}

bool WithinJsonLimits(const nlohmann::json& value, uint32_t depth, uint32_t maxDepth,
	uint32_t& nodes, uint32_t maxNodes) {
	if (++nodes > maxNodes || depth > maxDepth) return false;
	if (!value.is_array() && !value.is_object()) return true;
	for (const auto& child : value) {
		if (!WithinJsonLimits(child, depth + 1, maxDepth, nodes, maxNodes)) return false;
	}
	return true;
}

bool ValidateSerializedJson(const std::string& serialized, const HostValueLimits& limits) {
	if (serialized.empty()) return true;
	try {
		const nlohmann::json value = nlohmann::json::parse(serialized);
		uint32_t nodes = 0;
		return WithinJsonLimits(value, 0, limits.maxDepth, nodes, limits.maxNodes);
	} catch (...) {
		return false;
	}
}
}

bool IsSafeHostFieldPath(const std::string& fieldPath) {
	if (fieldPath.empty()) return true; // empty means the userdata root
	if (fieldPath.size() > 256) return false;
	std::size_t index = 0;
	bool needIdentifier = true;
	while (index < fieldPath.size()) {
		const unsigned char first = static_cast<unsigned char>(fieldPath[index]);
		if (needIdentifier) {
			if (!(std::isalpha(first) || first == '_')) return false;
			++index;
			while (index < fieldPath.size()) {
				const unsigned char ch = static_cast<unsigned char>(fieldPath[index]);
				if (!(std::isalnum(ch) || ch == '_')) break;
				++index;
			}
			needIdentifier = false;
			continue;
		}
		if (fieldPath[index] == '.') {
			++index;
			needIdentifier = true;
			continue;
		}
		if (fieldPath[index] == '[') {
			const std::size_t close = fieldPath.find(']', index + 1);
			if (close == std::string::npos || close == index + 1) return false;
			const std::string literal = fieldPath.substr(index + 1, close - index - 1);
			bool valid = true;
			if (literal.front() == '"' && literal.back() == '"') {
				valid = literal.size() >= 2 &&
					literal.find('\\', 1) == std::string::npos &&
					literal.find('"', 1) == literal.size() - 1;
			} else {
				std::size_t pos = 0;
				if (std::isdigit(static_cast<unsigned char>(literal[0]))) {
					while (pos < literal.size() &&
						   std::isdigit(static_cast<unsigned char>(literal[pos]))) ++pos;
					valid = pos == literal.size();
				} else if (std::isalpha(static_cast<unsigned char>(literal[0])) || literal[0] == '_') {
					++pos;
					while (pos < literal.size() &&
						   (std::isalnum(static_cast<unsigned char>(literal[pos])) || literal[pos] == '_')) ++pos;
					valid = pos == literal.size();
				} else {
					valid = false;
				}
			}
			if (!valid) return false;
			index = close + 1;
			continue;
		}
		return false;
	}
	return !needIdentifier;
}

void HostValueProviderRegistry::Set(const std::shared_ptr<HostValueProvider>& provider) {
	std::lock_guard<std::mutex> lock(mutex_);
	provider_ = provider;
}

void HostValueProviderRegistry::Clear(const std::shared_ptr<HostValueProvider>& provider) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!provider || provider_ == provider) provider_.reset();
}

HostValueResult HostValueProviderRegistry::Describe(const HostValueRequest& request) const {
	if (request.vmId == 0 || request.threadId.empty() || request.valueRef.empty()) {
		return Error(HostValueStatus::Unavailable, "HOST_VALUE_INVALID_REQUEST");
	}
	if (!IsSafeHostFieldPath(request.fieldPath)) {
		return Error(HostValueStatus::FieldDenied, "HOST_VALUE_FIELD_DENIED");
	}
	if (request.limits.maxDepth > 16 ||
		request.limits.maxNodes == 0 || request.limits.maxNodes > 10000 ||
		request.limits.maxBytes == 0 || request.limits.maxBytes > 1024 * 1024) {
		return Error(HostValueStatus::Unavailable, "HOST_VALUE_LIMIT_INVALID");
	}
	if (request.limits.deadlineUnixMillis != 0 &&
		request.limits.deadlineUnixMillis <= NowUnixMillis()) {
		return Error(HostValueStatus::Timeout, "HOST_VALUE_TIMEOUT");
	}
	std::shared_ptr<HostValueProvider> provider;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		provider = provider_;
	}
	if (!provider) return Error(HostValueStatus::Unavailable, "HOST_VALUE_UNAVAILABLE");
	HostValueResult result;
	try {
		result = provider->DescribeUserdata(request);
	} catch (...) {
		return Error(HostValueStatus::Unavailable, "HOST_VALUE_PROVIDER_EXCEPTION");
	}
	if (request.limits.deadlineUnixMillis != 0 &&
		request.limits.deadlineUnixMillis <= NowUnixMillis()) {
		return Error(HostValueStatus::Timeout, "HOST_VALUE_TIMEOUT");
	}
	if (result.status == HostValueStatus::NeedsGameThread) {
		if (request.limits.deadlineUnixMillis != 0 &&
			request.limits.deadlineUnixMillis <= NowUnixMillis()) {
			return Error(HostValueStatus::Timeout, "HOST_VALUE_TIMEOUT");
		}
		try {
			result = provider->DispatchToGameThread(request);
		} catch (...) {
			return Error(HostValueStatus::Unavailable, "HOST_VALUE_PROVIDER_EXCEPTION");
		}
		if (request.limits.deadlineUnixMillis != 0 &&
			request.limits.deadlineUnixMillis <= NowUnixMillis()) {
			return Error(HostValueStatus::Timeout, "HOST_VALUE_TIMEOUT");
		}
	}
	if (result.IsSuccess()) {
		const std::size_t totalBytes = result.typeName.size() + result.display.size() +
			result.serializedJson.size();
		if (!ValidateSerializedJson(result.serializedJson, request.limits)) {
			return Error(HostValueStatus::Unavailable, "HOST_VALUE_RESULT_INVALID");
		}
		if (totalBytes > request.limits.maxBytes) {
			HostValueResult bounded = Error(HostValueStatus::Unavailable, "HOST_VALUE_RESULT_TOO_LARGE");
			bounded.truncated = true;
			return bounded;
		}
	}
	if (!result.IsSuccess() && result.errorCode.empty()) {
		result.errorCode = "HOST_VALUE_UNAVAILABLE";
	}
	return result;
}

bool HostValueProviderRegistry::HasProvider() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return static_cast<bool>(provider_);
}
