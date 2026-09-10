#include "emmy_debugger/vm/vm_registry.h"

#include <algorithm>

namespace {

std::atomic<uint64_t> gNextRegistrationId(1);

bool IsTerminal(VmLifecycleState state) {
	return state == VmLifecycleState::Closed;
}

} // namespace

uint64_t AllocateVmRegistrationId() {
	uint64_t id = gNextRegistrationId.fetch_add(1, std::memory_order_relaxed);
	if (id == 0) {
		id = gNextRegistrationId.fetch_add(1, std::memory_order_relaxed);
	}
	return id;
}

NativeVmRegistry::NativeVmRegistry()
	: nextEventSeq_(0) {
}

NativeVmRegistry::~NativeVmRegistry() {
}

uint64_t NativeVmRegistry::Register(lua_State* mainState, const VmMetadata& metadata) {
	if (mainState == nullptr) {
		return 0;
	}
	return RegisterWithId(AllocateVmRegistrationId(), 0, 0, 0, mainState, metadata);
}

uint64_t NativeVmRegistry::RegisterWithId(uint64_t registrationId,
										  uint64_t generation,
										  uint64_t contextGeneration,
										  uint64_t sourceEpoch,
										  lua_State* mainState,
										  const VmMetadata& metadata) {
	if (registrationId == 0 || mainState == nullptr) {
		return 0;
	}

	VmLifecycleEvent event;
	VmEventSink sink;
	bool emit = false;
	uint64_t resultId = registrationId;
	{
		std::lock_guard<std::mutex> lock(mutex_);

		auto active = activeByState_.find(mainState);
		if (active != activeByState_.end()) {
			auto activeRecord = records_.find(active->second);
			if (activeRecord != records_.end() &&
				activeRecord->second->state != VmLifecycleState::Closed &&
				activeRecord->second->state != VmLifecycleState::Lost) {
				return active->second;
			}
			activeByState_.erase(active);
		}

		auto existing = records_.find(registrationId);
		if (existing != records_.end() && !IsTerminal(existing->second->state)) {
			// A caller cannot replace another live registration. Preserve the
			// supplied state identity and allocate a fresh opaque ID instead.
			resultId = AllocateVmRegistrationId();
		}

		uint64_t nextGeneration = generation;
		auto generationIt = generationByState_.find(mainState);
		if (nextGeneration == 0) {
			nextGeneration = generationIt == generationByState_.end()
				? 1
				: generationIt->second + 1;
		} else if (generationIt != generationByState_.end()) {
			nextGeneration = std::max(nextGeneration, generationIt->second + 1);
		}
		generationByState_[mainState] = nextGeneration;

		std::shared_ptr<VmRecord> record(new VmRecord());
		record->id = resultId;
		record->generation = nextGeneration;
		record->contextGeneration = contextGeneration == 0 ? 1 : contextGeneration;
		record->sourceEpoch = sourceEpoch == 0 ? 1 : sourceEpoch;
		record->mainState = mainState;
		record->metadata = metadata;
		record->state = VmLifecycleState::Created;
		if (metadata.hasAbiDescriptor) {
			if (processAbiFingerprint_.empty()) {
				processAbiFingerprint_ = metadata.abi.Fingerprint();
			} else if (processAbiFingerprint_ != metadata.abi.Fingerprint()) {
				record->metadata.abiCompatible = false;
				record->metadata.abiError = "MIXED_LUA_ABI_UNSUPPORTED";
			}
			if (!record->metadata.abiCompatible) {
				record->state = VmLifecycleState::Error;
			}
		}
		record->eventSeq = ++nextEventSeq_;
		records_[resultId] = record;
		activeByState_[mainState] = resultId;

		event.vmId = resultId;
		event.generation = record->generation;
		event.contextGeneration = record->contextGeneration;
		event.sourceEpoch = record->sourceEpoch;
		event.previous = VmLifecycleState::Unknown;
		event.current = record->state;
		if (record->state == VmLifecycleState::Error) {
			event.reason = record->metadata.abiError;
		}
		event.eventSeq = record->eventSeq;
		sink = eventSink_;
		emit = true;
	}

	if (emit && sink) {
		sink(event);
	}
	return resultId;
}

uint64_t NativeVmRegistry::Adopt(uint64_t registrationId,
								 uint64_t generation,
								 lua_State* mainState,
								 const VmMetadata& metadata,
								 uint64_t contextGeneration,
								 uint64_t sourceEpoch) {
	return RegisterWithId(registrationId, generation, contextGeneration, sourceEpoch,
									 mainState, metadata);
}

bool NativeVmRegistry::IsTransitionAllowed(VmLifecycleState from, VmLifecycleState to) const {
	if (from == to) {
		return true;
	}
	if (from == VmLifecycleState::Unknown || from == VmLifecycleState::Closed) {
		return false;
	}

	switch (from) {
		case VmLifecycleState::Created:
			return to == VmLifecycleState::Ready ||
				   to == VmLifecycleState::Running ||
				   to == VmLifecycleState::Closing ||
				   to == VmLifecycleState::Lost ||
				   to == VmLifecycleState::Error;
		case VmLifecycleState::Ready:
			return to == VmLifecycleState::Running ||
				   to == VmLifecycleState::Paused ||
				   to == VmLifecycleState::Closing ||
				   to == VmLifecycleState::Lost ||
				   to == VmLifecycleState::Error;
		case VmLifecycleState::Running:
			return to == VmLifecycleState::Paused ||
				   to == VmLifecycleState::Closing ||
				   to == VmLifecycleState::Lost ||
				   to == VmLifecycleState::Error;
		case VmLifecycleState::Paused:
			return to == VmLifecycleState::Running ||
				   to == VmLifecycleState::Closing ||
				   to == VmLifecycleState::Lost ||
				   to == VmLifecycleState::Error;
		case VmLifecycleState::Closing:
			return to == VmLifecycleState::Closed ||
				   to == VmLifecycleState::Lost ||
				   to == VmLifecycleState::Error;
		case VmLifecycleState::Lost:
			return to == VmLifecycleState::Closed;
		case VmLifecycleState::Error:
			return to == VmLifecycleState::Closing ||
				   to == VmLifecycleState::Closed ||
				   to == VmLifecycleState::Lost;
		case VmLifecycleState::Unknown:
		case VmLifecycleState::Closed:
			return false;
	}
	return false;
}

bool NativeVmRegistry::RejectAbi(uint64_t registrationId, const std::string& error) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = records_.find(registrationId);
		if (it == records_.end() || !IsTransitionAllowed(it->second->state, VmLifecycleState::Error)) return false;
		it->second->metadata.abiCompatible = false;
		it->second->metadata.abiError = error;
	}
	return SetState(registrationId, VmLifecycleState::Error, error);
}

bool NativeVmRegistry::SetState(uint64_t registrationId,
							VmLifecycleState state,
							const std::string& reason) {
	if (registrationId == 0 || state == VmLifecycleState::Unknown) {
		return false;
	}

	VmLifecycleEvent event;
	VmEventSink sink;
	bool emit = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = records_.find(registrationId);
		if (it == records_.end()) {
			return false;
		}
		const std::shared_ptr<VmRecord>& record = it->second;
		if (record->state == state) {
			return true;
		}
		if (!IsTransitionAllowed(record->state, state)) {
			return false;
		}

		event.vmId = record->id;
		event.generation = record->generation;
		event.contextGeneration = record->contextGeneration;
		event.sourceEpoch = record->sourceEpoch;
		event.previous = record->state;
		event.current = state;
		event.reason = reason;
		event.eventSeq = ++nextEventSeq_;

		record->state = state;
		record->eventSeq = event.eventSeq;
		// A Lost VM is no longer safe to address through its raw lua_State
		// pointer. Keep the record for lifecycle diagnostics, but remove the
		// address from the active index immediately.
		if (state == VmLifecycleState::Closed || state == VmLifecycleState::Lost) {
			activeByState_.erase(record->mainState);
		}
		sink = eventSink_;
		emit = true;
	}

	if (emit && sink) {
		sink(event);
	}
	return true;
}

bool NativeVmRegistry::NotifyReady(uint64_t registrationId) {
	return SetState(registrationId, VmLifecycleState::Ready, "host-ready");
}

bool NativeVmRegistry::BeginClose(uint64_t registrationId, const std::string& reason) {
	return SetState(registrationId, VmLifecycleState::Closing, reason);
}

bool NativeVmRegistry::EndClose(uint64_t registrationId) {
	return SetState(registrationId, VmLifecycleState::Closed, "host-closed");
}

bool NativeVmRegistry::ResetContext(uint64_t registrationId, const std::string& reason) {
	if (registrationId == 0) return false;

	VmLifecycleEvent event;
	VmEventSink sink;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = records_.find(registrationId);
		if (it == records_.end()) return false;
		const std::shared_ptr<VmRecord>& record = it->second;
		if (record->state == VmLifecycleState::Closed ||
			record->state == VmLifecycleState::Closing ||
			record->state == VmLifecycleState::Lost) {
			return false;
		}

		const uint64_t nextContext = record->contextGeneration + 1;
		const uint64_t nextSource = record->sourceEpoch + 1;
		// A wrapped epoch would make an old reference appear current. Refuse the
		// reset instead of reusing an identity.
		if (nextContext == 0 || nextSource == 0) return false;

		event.vmId = record->id;
		event.generation = record->generation;
		event.contextGeneration = nextContext;
		event.sourceEpoch = nextSource;
		event.previous = record->state;
		event.current = record->state == VmLifecycleState::Paused
			? VmLifecycleState::Running : record->state;
		event.reason = reason.empty() ? "context-reset" : reason;
		event.eventSeq = ++nextEventSeq_;
		event.contextReset = true;

		record->contextGeneration = nextContext;
		record->sourceEpoch = nextSource;
		record->state = event.current;
		record->eventSeq = event.eventSeq;
		sink = eventSink_;
	}

	if (sink) sink(event);
	return true;
}

bool NativeVmRegistry::Release(uint64_t registrationId) {
	if (registrationId == 0) {
		return true;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = records_.find(registrationId);
	if (it == records_.end()) {
		return true;
	}
	if (it->second->state != VmLifecycleState::Closed &&
		it->second->state != VmLifecycleState::Lost) {
		return false;
	}
	// Repair an index left by an older terminal transition and make Release
	// safe when an address has already been reused by another VM.
	const auto active = activeByState_.find(it->second->mainState);
	if (active != activeByState_.end() && active->second == registrationId) {
		activeByState_.erase(active);
	}
	records_.erase(it);
	return true;
}

bool NativeVmRegistry::SetDisplayName(uint64_t registrationId, const std::string& displayName) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = records_.find(registrationId);
	if (it == records_.end() || IsTerminal(it->second->state)) {
		return false;
	}
	it->second->metadata.displayName = displayName;
	return true;
}

std::shared_ptr<const VmRecord> NativeVmRegistry::Find(uint64_t registrationId) const {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = records_.find(registrationId);
	if (it == records_.end()) {
		return std::shared_ptr<const VmRecord>();
	}
	return std::shared_ptr<const VmRecord>(new VmRecord(*it->second));
}

std::shared_ptr<const VmRecord> NativeVmRegistry::FindByState(lua_State* mainState) const {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = activeByState_.find(mainState);
	if (it == activeByState_.end()) {
		return std::shared_ptr<const VmRecord>();
	}
	auto record = records_.find(it->second);
	if (record == records_.end()) {
		return std::shared_ptr<const VmRecord>();
	}
	return std::shared_ptr<const VmRecord>(new VmRecord(*record->second));
}

std::vector<std::shared_ptr<const VmRecord>> NativeVmRegistry::Snapshot() const {
	return SnapshotWithEventSeq().records;
}

VmRegistrySnapshot NativeVmRegistry::SnapshotWithEventSeq() const {
	std::lock_guard<std::mutex> lock(mutex_);
	VmRegistrySnapshot snapshot;
	snapshot.eventSeq = nextEventSeq_;
	for (std::map<uint64_t, std::shared_ptr<VmRecord>>::const_iterator it = records_.begin();
		 it != records_.end(); ++it) {
		if (it->second->state != VmLifecycleState::Closed) {
			snapshot.records.push_back(std::shared_ptr<const VmRecord>(new VmRecord(*it->second)));
		}
	}
	return snapshot;
}

void NativeVmRegistry::SetEventSink(const VmEventSink& sink) {
	std::lock_guard<std::mutex> lock(mutex_);
	eventSink_ = sink;
}
