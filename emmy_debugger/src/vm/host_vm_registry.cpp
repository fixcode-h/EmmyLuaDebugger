#include "emmy_debugger/vm/host_vm_registry.h"

#include <vector>

HostVmRegistry::HostVmRegistry()
	: nativeRegistry_(nullptr),
	  active_(false) {
}

HostVmRegistry::~HostVmRegistry() {
}

uint64_t HostVmRegistry::AllocatePending(lua_State* mainState, const VmMetadata& metadata) {
	if (mainState == nullptr) {
		return 0;
	}
	const uint64_t registrationId = AllocateVmRegistrationId();
	std::shared_ptr<VmRecord> record(new VmRecord());
	record->id = registrationId;
	record->mainState = mainState;
	record->metadata = metadata;
	record->state = VmLifecycleState::Created;
	auto generationIt = generationByState_.find(mainState);
	record->generation = generationIt == generationByState_.end() ? 1 : generationIt->second + 1;
	generationByState_[mainState] = record->generation;
	records_[registrationId] = record;
	activeByState_[mainState] = registrationId;
	return registrationId;
}

uint64_t HostVmRegistry::RegisterBeforeAgent(lua_State* mainState, const VmMetadata& metadata) {
	NativeVmRegistry* native = nullptr;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto existing = activeByState_.find(mainState);
		if (existing != activeByState_.end()) {
			return existing->second;
		}
		if (!active_) {
			return AllocatePending(mainState, metadata);
		}
		native = nativeRegistry_;
	}

	if (native == nullptr) {
		return 0;
	}
	const uint64_t registrationId = native->Register(mainState, metadata);
	if (registrationId == 0) {
		return 0;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	std::shared_ptr<VmRecord> record(new VmRecord());
	record->id = registrationId;
	record->mainState = mainState;
	record->metadata = metadata;
	record->state = VmLifecycleState::Created;
	auto nativeRecord = native->Find(registrationId);
	record->generation = nativeRecord ? nativeRecord->generation : 1;
	records_[registrationId] = record;
	activeByState_[mainState] = registrationId;
	generationByState_[mainState] = record->generation;
	return registrationId;
}

bool HostVmRegistry::MarkReady(uint64_t registrationId) {
	NativeVmRegistry* native = nullptr;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = records_.find(registrationId);
		if (it == records_.end()) {
			return false;
		}
		if (!active_) {
			if (it->second->state == VmLifecycleState::Ready) {
				return true;
			}
			if (it->second->state != VmLifecycleState::Created) {
				return false;
			}
			it->second->state = VmLifecycleState::Ready;
			return true;
		}
		native = nativeRegistry_;
	}
	const bool result = native != nullptr && native->NotifyReady(registrationId);
	if (result) {
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = records_.find(registrationId);
		if (it != records_.end()) it->second->state = VmLifecycleState::Ready;
	}
	return result;
}

bool HostVmRegistry::BeginClose(uint64_t registrationId, const std::string& reason) {
	NativeVmRegistry* native = nullptr;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = records_.find(registrationId);
		if (it == records_.end()) {
			return false;
		}
		if (!active_) {
			if (it->second->state == VmLifecycleState::Closing) {
				return true;
			}
			if (it->second->state == VmLifecycleState::Closed) {
				return false;
			}
			it->second->state = VmLifecycleState::Closing;
			return true;
		}
		native = nativeRegistry_;
	}
	const bool result = native != nullptr && native->BeginClose(registrationId, reason);
	if (result) {
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = records_.find(registrationId);
		if (it != records_.end()) it->second->state = VmLifecycleState::Closing;
	}
	return result;
}

bool HostVmRegistry::EndClose(uint64_t registrationId) {
	NativeVmRegistry* native = nullptr;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = records_.find(registrationId);
		if (it == records_.end()) {
			return true;
		}
		if (!active_) {
			if (it->second->state == VmLifecycleState::Closed) {
				return true;
			}
			if (it->second->state != VmLifecycleState::Closing) {
				return false;
			}
			it->second->state = VmLifecycleState::Closed;
			activeByState_.erase(it->second->mainState);
			return true;
		}
		native = nativeRegistry_;
	}
	const bool result = native != nullptr && native->EndClose(registrationId);
	if (result) {
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = records_.find(registrationId);
		if (it != records_.end()) {
			it->second->state = VmLifecycleState::Closed;
			activeByState_.erase(it->second->mainState);
		}
	}
	return result;
}

bool HostVmRegistry::Release(uint64_t registrationId) {
	NativeVmRegistry* native = nullptr;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = records_.find(registrationId);
		if (it == records_.end()) {
			return true;
		}
		if (!active_) {
			if (it->second->state != VmLifecycleState::Closed &&
				it->second->state != VmLifecycleState::Lost) {
				return false;
			}
			activeByState_.erase(it->second->mainState);
			records_.erase(it);
			return true;
		}
		native = nativeRegistry_;
	}
	const bool released = native != nullptr && native->Release(registrationId);
	if (released) {
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = records_.find(registrationId);
		if (it != records_.end()) {
			activeByState_.erase(it->second->mainState);
			records_.erase(it);
		}
	}
	return released;
}

bool HostVmRegistry::SetDisplayName(uint64_t registrationId, const std::string& displayName) {
	NativeVmRegistry* native = nullptr;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = records_.find(registrationId);
		if (it == records_.end()) {
			return false;
		}
		it->second->metadata.displayName = displayName;
		if (!active_) {
			return true;
		}
		native = nativeRegistry_;
	}
	return native != nullptr && native->SetDisplayName(registrationId, displayName);
}

std::shared_ptr<const VmRecord> HostVmRegistry::FindByState(lua_State* mainState) const {
	std::lock_guard<std::mutex> lock(mutex_);
	auto active = activeByState_.find(mainState);
	if (active == activeByState_.end()) {
		return std::shared_ptr<const VmRecord>();
	}
	auto record = records_.find(active->second);
	if (record == records_.end()) {
		return std::shared_ptr<const VmRecord>();
	}
	return std::shared_ptr<const VmRecord>(new VmRecord(*record->second));
}

bool HostVmRegistry::ReconcileExistingVms(NativeVmRegistry& destination) {
	std::vector<std::shared_ptr<VmRecord>> pending;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (active_) {
			return nativeRegistry_ == &destination;
		}
		for (std::map<uint64_t, std::shared_ptr<VmRecord>>::const_iterator it = records_.begin();
			 it != records_.end(); ++it) {
			pending.push_back(std::shared_ptr<VmRecord>(new VmRecord(*it->second)));
		}
		active_ = true;
		nativeRegistry_ = &destination;
	}

	for (std::vector<std::shared_ptr<VmRecord>>::const_iterator it = pending.begin();
		 it != pending.end(); ++it) {
		const std::shared_ptr<VmRecord>& record = *it;
		if (record->state == VmLifecycleState::Closed) {
			continue;
		}
		const uint64_t nativeId = destination.Adopt(
			record->id, record->generation, record->mainState, record->metadata);
		if (nativeId == 0) {
			return false;
		}
		if (nativeId != record->id) {
			std::lock_guard<std::mutex> lock(mutex_);
			auto found = records_.find(record->id);
			if (found != records_.end()) {
				std::shared_ptr<VmRecord> moved(new VmRecord(*found->second));
				moved->id = nativeId;
				records_.erase(found);
				records_[nativeId] = moved;
				activeByState_[moved->mainState] = nativeId;
			}
		}

		uint64_t effectiveId = nativeId;
		if (record->state == VmLifecycleState::Ready ||
			record->state == VmLifecycleState::Running ||
			record->state == VmLifecycleState::Paused ||
			record->state == VmLifecycleState::Closing ||
			record->state == VmLifecycleState::Lost ||
			record->state == VmLifecycleState::Error) {
			destination.SetState(effectiveId, record->state, "host-reconcile");
		}
		if (record->state == VmLifecycleState::Closing) {
			// The host has not completed lua_close yet; leave it Closing.
		}
	}
	return true;
}

bool HostVmRegistry::IsActive() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return active_;
}
