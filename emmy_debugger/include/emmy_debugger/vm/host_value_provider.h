#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

enum class HostValueStatus {
	Ok,
	Unavailable,
	NeedsGameThread,
	ObjectInvalid,
	FieldDenied,
	Timeout,
};

struct HostValueLimits {
	uint32_t maxDepth = 3;
	uint32_t maxNodes = 100;
	uint32_t maxBytes = 64 * 1024;
	uint64_t deadlineUnixMillis = 0;
};

// No engine pointer crosses this boundary. valueRef is an opaque copied token
// created on the Lua owner thread and all result strings are copied immediately.
struct HostValueRequest {
	uint64_t vmId = 0;
	std::string threadId;
	std::string valueRef;
	std::string fieldPath;
	HostValueLimits limits;
};

struct HostValueResult {
	HostValueStatus status = HostValueStatus::Unavailable;
	std::string typeName;
	std::string display;
	std::string serializedJson;
	std::string errorCode;
	bool truncated = false;

	bool IsSuccess() const { return status == HostValueStatus::Ok; }
};

// Syntax gate for reflection paths crossing the host boundary. Providers must
// still apply their own per-type whitelist; this helper only rejects malformed
// or injection-prone path strings before dispatching to an engine thread.
bool IsSafeHostFieldPath(const std::string& fieldPath);

class HostValueProvider {
public:
	virtual ~HostValueProvider() = default;

	// Called first on the Lua owner thread. Return NeedsGameThread when engine
	// reflection must be performed by DispatchToGameThread.
	virtual HostValueResult DescribeUserdata(const HostValueRequest& request) = 0;
	virtual HostValueResult DispatchToGameThread(const HostValueRequest& request) {
		HostValueResult result;
		result.status = HostValueStatus::Unavailable;
		result.errorCode = "HOST_VALUE_UNAVAILABLE";
		return result;
	}
};

class HostValueProviderRegistry {
public:
	void Set(const std::shared_ptr<HostValueProvider>& provider);
	void Clear(const std::shared_ptr<HostValueProvider>& provider = std::shared_ptr<HostValueProvider>());
	HostValueResult Describe(const HostValueRequest& request) const;
	bool HasProvider() const;

private:
	mutable std::mutex mutex_;
	std::shared_ptr<HostValueProvider> provider_;
};
