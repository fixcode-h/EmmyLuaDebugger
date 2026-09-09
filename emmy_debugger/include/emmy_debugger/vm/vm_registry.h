#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct lua_State;

enum class VmLifecycleState {
	Unknown,
	Created,
	Ready,
	Running,
	Paused,
	Closing,
	Closed,
	Lost,
	Error,
};

struct VmMetadata {
	std::string displayName;
	std::string engineName;
	std::string engineContext;
	std::string luaVersionHint;
	std::string runtimeModule;
	std::string discovery;
};

struct VmRecord {
	uint64_t id = 0;
	uint64_t generation = 0;
	lua_State* mainState = nullptr;
	VmMetadata metadata;
	VmLifecycleState state = VmLifecycleState::Unknown;
	uint64_t eventSeq = 0;
};

struct VmLifecycleEvent {
	uint64_t vmId = 0;
	uint64_t generation = 0;
	VmLifecycleState previous = VmLifecycleState::Unknown;
	VmLifecycleState current = VmLifecycleState::Unknown;
	std::string reason;
	uint64_t eventSeq = 0;
};

struct VmRegistrySnapshot {
	uint64_t eventSeq = 0;
	std::vector<std::shared_ptr<const VmRecord>> records;
};

typedef std::function<void(const VmLifecycleEvent&)> VmEventSink;

// IDs are process-local opaque registration handles. Zero is reserved for failure.
uint64_t AllocateVmRegistrationId();

class NativeVmRegistry {
public:
	NativeVmRegistry();
	~NativeVmRegistry();

	uint64_t Register(lua_State* mainState, const VmMetadata& metadata);

	// Adopt a Host registration without changing its opaque ID. A duplicate active
	// state returns the already active ID, which makes reconciliation idempotent.
	uint64_t Adopt(uint64_t registrationId,
				  uint64_t generation,
				  lua_State* mainState,
				  const VmMetadata& metadata);

	bool NotifyReady(uint64_t registrationId);
	bool BeginClose(uint64_t registrationId, const std::string& reason);
	bool EndClose(uint64_t registrationId);
	bool SetState(uint64_t registrationId, VmLifecycleState state, const std::string& reason);
	bool Release(uint64_t registrationId);
	bool SetDisplayName(uint64_t registrationId, const std::string& displayName);

	std::shared_ptr<const VmRecord> Find(uint64_t registrationId) const;
	std::shared_ptr<const VmRecord> FindByState(lua_State* mainState) const;
	std::vector<std::shared_ptr<const VmRecord>> Snapshot() const;
	VmRegistrySnapshot SnapshotWithEventSeq() const;

	void SetEventSink(const VmEventSink& sink);

private:
	uint64_t RegisterWithId(uint64_t registrationId,
						   uint64_t generation,
						   lua_State* mainState,
						   const VmMetadata& metadata);
	bool IsTransitionAllowed(VmLifecycleState from, VmLifecycleState to) const;

	mutable std::mutex mutex_;
	std::map<uint64_t, std::shared_ptr<VmRecord>> records_;
	std::map<lua_State*, uint64_t> activeByState_;
	std::map<lua_State*, uint64_t> generationByState_;
	VmEventSink eventSink_;
	uint64_t nextEventSeq_;
};
