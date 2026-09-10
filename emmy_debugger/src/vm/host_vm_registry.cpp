#include "emmy_debugger/vm/host_vm_registry.h"

#include <set>
#include <vector>

HostVmRegistry::HostVmRegistry()
	: nativeRegistry_(nullptr),
	  active_(false),
	  reconciling_(false),
	  mutationVersion_(0) {
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
	record->state = metadata.abiCompatible ? VmLifecycleState::Created : VmLifecycleState::Error;
	record->contextGeneration = 1;
	record->sourceEpoch = 1;
	auto generationIt = generationByState_.find(mainState);
	record->generation = generationIt == generationByState_.end() ? 1 : generationIt->second + 1;
	generationByState_[mainState] = record->generation;
	records_[registrationId] = record;
	activeByState_[mainState] = registrationId;
	nativeIds_.erase(registrationId);
	++mutationVersion_;
	return registrationId;
}

uint64_t HostVmRegistry::ResolveNativeIdLocked(uint64_t registrationId) const {
	auto alias = nativeIds_.find(registrationId);
	return alias == nativeIds_.end() ? registrationId : alias->second;
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
	record->contextGeneration = nativeRecord ? nativeRecord->contextGeneration : 1;
	record->sourceEpoch = nativeRecord ? nativeRecord->sourceEpoch : 1;
	records_[registrationId] = record;
	activeByState_[mainState] = registrationId;
	generationByState_[mainState] = record->generation;
	nativeIds_[registrationId] = registrationId;
	++mutationVersion_;
	return registrationId;
}

bool HostVmRegistry::MarkReady(uint64_t registrationId) {
	NativeVmRegistry* native = nullptr;
	uint64_t nativeId = 0;
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
			++mutationVersion_;
			return true;
		}
		native = nativeRegistry_;
		nativeId = ResolveNativeIdLocked(registrationId);
	}
	const bool result = native != nullptr && native->NotifyReady(nativeId);
	if (result) {
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = records_.find(registrationId);
		if (it != records_.end() && it->second->state == VmLifecycleState::Created) {
			it->second->state = VmLifecycleState::Ready;
			++mutationVersion_;
		}
	}
	return result;
}

bool HostVmRegistry::RejectAbi(uint64_t registrationId, const std::string& error) {
	NativeVmRegistry* native = nullptr;
	uint64_t nativeId = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = records_.find(registrationId);
		if (it == records_.end() || it->second->state == VmLifecycleState::Closed) return false;
		it->second->metadata.abiCompatible = false;
		it->second->metadata.abiError = error;
		it->second->state = VmLifecycleState::Error;
		++mutationVersion_;
		if (!active_) return true;
		native = nativeRegistry_;
		nativeId = ResolveNativeIdLocked(registrationId);
	}
	return native != nullptr && native->RejectAbi(nativeId, error);
}

bool HostVmRegistry::BeginClose(uint64_t registrationId, const std::string& reason) {
	NativeVmRegistry* native = nullptr;
	uint64_t nativeId = 0;
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
			++mutationVersion_;
			return true;
		}
		native = nativeRegistry_;
		nativeId = ResolveNativeIdLocked(registrationId);
	}
	const bool result = native != nullptr && native->BeginClose(nativeId, reason);
	if (result) {
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = records_.find(registrationId);
		if (it != records_.end() && it->second->state != VmLifecycleState::Closing) {
			it->second->state = VmLifecycleState::Closing;
			++mutationVersion_;
		}
	}
	return result;
}

bool HostVmRegistry::EndClose(uint64_t registrationId) {
	NativeVmRegistry* native = nullptr;
	uint64_t nativeId = 0;
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
			++mutationVersion_;
			return true;
		}
		native = nativeRegistry_;
		nativeId = ResolveNativeIdLocked(registrationId);
	}
	const bool result = native != nullptr && native->EndClose(nativeId);
	if (result) {
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = records_.find(registrationId);
		if (it != records_.end()) {
			it->second->state = VmLifecycleState::Closed;
			activeByState_.erase(it->second->mainState);
			++mutationVersion_;
		}
	}
	return result;
}

bool HostVmRegistry::ResetContext(uint64_t registrationId, const std::string& reason) {
	NativeVmRegistry* native = nullptr;
	uint64_t nativeId = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = records_.find(registrationId);
		if (it == records_.end()) return false;
		if (!active_) {
			if (it->second->state == VmLifecycleState::Closed ||
				it->second->state == VmLifecycleState::Closing ||
				it->second->state == VmLifecycleState::Lost) return false;
			if (it->second->contextGeneration == UINT64_MAX ||
				it->second->sourceEpoch == UINT64_MAX) return false;
			++it->second->contextGeneration;
			++it->second->sourceEpoch;
			if (it->second->state == VmLifecycleState::Paused) {
				it->second->state = VmLifecycleState::Running;
			}
			++mutationVersion_;
			return true;
		}
		native = nativeRegistry_;
		nativeId = ResolveNativeIdLocked(registrationId);
	}
	const bool result = native != nullptr && native->ResetContext(nativeId, reason);
	if (result) {
		std::shared_ptr<const VmRecord> nativeRecord = native->Find(nativeId);
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = records_.find(registrationId);
		if (it != records_.end() && nativeRecord) {
			it->second->contextGeneration = nativeRecord->contextGeneration;
			it->second->sourceEpoch = nativeRecord->sourceEpoch;
			it->second->state = nativeRecord->state;
			++mutationVersion_;
		}
	}
	return result;
}

bool HostVmRegistry::Release(uint64_t registrationId) {
	NativeVmRegistry* native = nullptr;
	uint64_t nativeId = 0;
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
			nativeIds_.erase(registrationId);
			++mutationVersion_;
			return true;
		}
		native = nativeRegistry_;
		nativeId = ResolveNativeIdLocked(registrationId);
	}
	const bool released = native != nullptr && native->Release(nativeId);
	if (released) {
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = records_.find(registrationId);
		if (it != records_.end()) {
			activeByState_.erase(it->second->mainState);
			records_.erase(it);
			nativeIds_.erase(registrationId);
			++mutationVersion_;
		}
	}
	return released;
}

bool HostVmRegistry::SetDisplayName(uint64_t registrationId, const std::string& displayName) {
	NativeVmRegistry* native = nullptr;
	uint64_t nativeId = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = records_.find(registrationId);
		if (it == records_.end()) {
			return false;
		}
		it->second->metadata.displayName = displayName;
		++mutationVersion_;
		if (!active_) {
			return true;
		}
		native = nativeRegistry_;
		nativeId = ResolveNativeIdLocked(registrationId);
	}
	return native != nullptr && native->SetDisplayName(nativeId, displayName);
}

std::shared_ptr<const VmRecord> HostVmRegistry::Find(uint64_t registrationId) const {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = records_.find(registrationId);
	if (it == records_.end() || !it->second) {
		return std::shared_ptr<const VmRecord>();
	}
	return std::shared_ptr<const VmRecord>(new VmRecord(*it->second));
}

std::shared_ptr<const VmRecord> HostVmRegistry::FindByState(lua_State* mainState) const {
	NativeVmRegistry* native = nullptr;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (active_ && nativeRegistry_ != nullptr) {
			native = nativeRegistry_;
		} else {
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
	}
	// Never call into the destination while holding the Host Registry lock;
	// NativeVmRegistry event sinks are allowed to query this mirror reentrantly.
	return native->FindByState(mainState);
}

bool HostVmRegistry::ReconcileExistingVms(NativeVmRegistry& destination) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (active_) return nativeRegistry_ == &destination;
		// A reentrant call from the destination event sink must return instead
		// of waiting on the outer reconciliation. The outer pass will observe
		// any host mutation made by the sink and adopt it on the next version.
		if (reconciling_) return false;
		reconciling_ = true;
		++mutationVersion_;
	}

	std::map<uint64_t, uint64_t> adopted;
	std::set<uint64_t> createdByThisPass;
	bool success = false;

	auto cleanupNative = [&destination](uint64_t nativeId) {
		if (nativeId == 0) return;
		std::shared_ptr<const VmRecord> record = destination.Find(nativeId);
		if (!record) return;
		if (record->state != VmLifecycleState::Closing &&
			record->state != VmLifecycleState::Closed &&
			record->state != VmLifecycleState::Lost) {
			destination.BeginClose(nativeId, "host-reconcile-cleanup");
		}
		record = destination.Find(nativeId);
		if (record && record->state == VmLifecycleState::Closing) {
			destination.EndClose(nativeId);
		}
		destination.Release(nativeId);
	};

	auto syncState = [&destination](uint64_t nativeId, const VmRecord& record) {
		if (!record.metadata.displayName.empty()) {
			destination.SetDisplayName(nativeId, record.metadata.displayName);
		}
		switch (record.state) {
			case VmLifecycleState::Ready:
			case VmLifecycleState::Running:
			case VmLifecycleState::Paused:
			case VmLifecycleState::Closing:
			case VmLifecycleState::Lost:
			case VmLifecycleState::Error:
				destination.SetState(nativeId, record.state, "host-reconcile");
				break;
			default:
				break;
		}
	};

	// Host callbacks are allowed to arrive while Adopt invokes an event sink.
	// Repeat until a complete pass observes an unchanged mutation version.
	for (std::size_t pass = 0; pass < 64 && !success; ++pass) {
		std::vector<std::shared_ptr<VmRecord>> pending;
		uint64_t observedVersion = 0;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			observedVersion = mutationVersion_;
			for (std::map<uint64_t, std::shared_ptr<VmRecord>>::const_iterator it = records_.begin();
					it != records_.end(); ++it) {
				pending.push_back(std::shared_ptr<VmRecord>(new VmRecord(*it->second)));
			}
		}

		std::set<uint64_t> seen;
		// Retire mappings for records removed since the previous pass before
		// adopting a replacement at the same lua_State address. NativeVmRegistry
		// intentionally returns the currently active address mapping, so leaving
		// this cleanup until after Adopt would alias the new host ID to the old VM.
		for (std::map<uint64_t, uint64_t>::iterator it = adopted.begin(); it != adopted.end();) {
			bool keep = false;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				auto found = records_.find(it->first);
				keep = found != records_.end() && found->second->state != VmLifecycleState::Closed;
			}
			if (!keep) {
				cleanupNative(it->second);
				it = adopted.erase(it);
			} else {
				++it;
			}
		}
		for (std::vector<std::shared_ptr<VmRecord>>::const_iterator it = pending.begin();
				it != pending.end(); ++it) {
			const std::shared_ptr<VmRecord>& snapshot = *it;
			seen.insert(snapshot->id);

			std::shared_ptr<VmRecord> current;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				auto found = records_.find(snapshot->id);
				if (found != records_.end()) current.reset(new VmRecord(*found->second));
			}
			if (!current || current->state == VmLifecycleState::Closed) {
				auto adoptedIt = adopted.find(snapshot->id);
				if (adoptedIt != adopted.end()) {
					cleanupNative(adoptedIt->second);
					adopted.erase(adoptedIt);
				}
				continue;
			}

			uint64_t nativeId = 0;
			auto adoptedIt = adopted.find(snapshot->id);
			if (adoptedIt == adopted.end()) {
				const bool existedBeforeAdopt = static_cast<bool>(destination.Find(snapshot->id));
				nativeId = destination.Adopt(snapshot->id, current->generation,
									current->mainState, current->metadata,
									current->contextGeneration, current->sourceEpoch);
				if (nativeId == 0) break;
				adopted[snapshot->id] = nativeId;
				if (!existedBeforeAdopt) createdByThisPass.insert(nativeId);
				{
					std::lock_guard<std::mutex> lock(mutex_);
					nativeIds_[snapshot->id] = nativeId;
				}
			} else {
				nativeId = adoptedIt->second;
			}
			syncState(nativeId, *current);
		}

		// Anything removed during this pass must be retired from the native
		// registry before we can publish the active fence.
		for (std::map<uint64_t, uint64_t>::iterator it = adopted.begin(); it != adopted.end();) {
			if (seen.find(it->first) == seen.end()) {
				cleanupNative(it->second);
				it = adopted.erase(it);
			} else {
				++it;
			}
		}

		bool stable = false;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			stable = observedVersion == mutationVersion_;
			if (stable) {
				for (std::map<uint64_t, std::shared_ptr<VmRecord>>::const_iterator it = records_.begin();
						it != records_.end(); ++it) {
					if (it->second->state != VmLifecycleState::Closed &&
						adopted.find(it->first) == adopted.end()) {
						stable = false;
						break;
					}
				}
			}
			if (stable) {
				nativeRegistry_ = &destination;
				active_ = true;
				reconciling_ = false;
				success = true;
			}
		}
	}

	if (!success) {
		for (std::map<uint64_t, uint64_t>::reverse_iterator it = adopted.rbegin();
				it != adopted.rend(); ++it) {
			if (createdByThisPass.find(it->second) != createdByThisPass.end()) {
				cleanupNative(it->second);
			}
		}
		std::lock_guard<std::mutex> lock(mutex_);
		reconciling_ = false;
	}
	return success;
}

bool HostVmRegistry::IsActive() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return active_;
}
