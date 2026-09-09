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
	return RegisterWithId(AllocateVmRegistrationId(), 0, mainState, metadata);
}

uint64_t NativeVmRegistry::RegisterWithId(uint64_t registrationId,
										  uint64_t generation,
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
			return active->second;
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
		record->mainState = mainState;
		record->metadata = metadata;
		record->state = VmLifecycleState::Created;
		record->eventSeq = ++nextEventSeq_;
		records_[resultId] = record;
		activeByState_[mainState] = resultId;

		event.vmId = resultId;
		event.generation = record->generation;
		event.previous = VmLifecycleState::Unknown;
		event.current = VmLifecycleState::Created;
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
								 const VmMetadata& metadata) {
	return RegisterWithId(registrationId, generation, mainState, metadata);
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
		event.previous = record->state;
		event.current = state;
		event.reason = reason;
		event.eventSeq = ++nextEventSeq_;

		record->state = state;
		record->eventSeq = event.eventSeq;
		if (state == VmLifecycleState::Closed) {
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
	std::lock_guard<std::mutex> lock(mutex_);
	std::vector<std::shared_ptr<const VmRecord>> result;
	for (std::map<uint64_t, std::shared_ptr<VmRecord>>::const_iterator it = records_.begin();
		 it != records_.end(); ++it) {
		if (it->second->state != VmLifecycleState::Closed) {
			result.push_back(std::shared_ptr<const VmRecord>(new VmRecord(*it->second)));
		}
	}
	return result;
}

void NativeVmRegistry::SetEventSink(const VmEventSink& sink) {
	std::lock_guard<std::mutex> lock(mutex_);
	eventSink_ = sink;
}
