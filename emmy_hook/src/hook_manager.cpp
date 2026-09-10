#include "hook_manager.h"

#include <utility>

HookManager::CallbackScope::CallbackScope(HookManager& manager)
	: manager_(&manager), entered_(manager.EnterCallback()) {
}

HookManager::CallbackScope::~CallbackScope() {
	if (entered_ && manager_ != nullptr) {
		manager_->LeaveCallback();
	}
}

HookManager::HookManager()
	: enabled_(false), teardownStarted_(false), inFlight_(0), unhookInProgress_(false) {
}

void HookManager::Enable() {
	std::lock_guard<std::mutex> lock(mutex_);
	// A manager can be reused only after the previous generation has fully
	// quiesced, released all handles, and finished its uninstall callbacks.
	if (enabled_.load(std::memory_order_acquire) || inFlight_ != 0 ||
		!hooks_.empty() || unhookInProgress_) return;
	teardownStarted_.store(false, std::memory_order_release);
	enabled_.store(true, std::memory_order_release);
}

bool HookManager::IsEnabled() const {
	return enabled_.load(std::memory_order_acquire);
}

bool HookManager::EnterCallback() {
	// The lock closes the race between the enabled check and incrementing the
	// count. DisableAndQuiesce can therefore establish a real zero-count fence.
	std::lock_guard<std::mutex> lock(mutex_);
	if (!enabled_.load(std::memory_order_acquire)) return false;
	++inFlight_;
	return true;
}

void HookManager::LeaveCallback() {
	std::lock_guard<std::mutex> lock(mutex_);
	if (inFlight_ == 0) return;
	--inFlight_;
	if (inFlight_ == 0) quiesced_.notify_all();
}

std::size_t HookManager::InFlightCount() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return inFlight_;
}

bool HookManager::DisableAndQuiesce(std::chrono::milliseconds timeout) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		teardownStarted_.store(true, std::memory_order_release);
		enabled_.store(false, std::memory_order_release);
	}
	std::unique_lock<std::mutex> lock(mutex_);
	if (inFlight_ == 0) return true;
	if (timeout.count() < 0) {
		quiesced_.wait(lock, [this] { return inFlight_ == 0; });
		return true;
	}
	return quiesced_.wait_for(lock, timeout, [this] { return inFlight_ == 0; });
}

bool HookManager::AddHook(HookHandle handle, const UnhookFunction& unhook,
						  const HookChainRecord& chain) {
	if (handle == nullptr || !unhook) return false;
	std::lock_guard<std::mutex> lock(mutex_);
	if (teardownStarted_.load(std::memory_order_acquire) ||
		!enabled_.load(std::memory_order_acquire)) return false;
	for (const OwnedHook& hook : hooks_) {
		if (hook.handle == handle) return true;
	}
	hooks_.push_back(OwnedHook{handle, unhook, chain});
	return true;
}

bool HookManager::UnhookIfSafe() {
	if (IsEnabled() || InFlightCount() != 0) return false;
	std::vector<OwnedHook> pending;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (inFlight_ != 0 || enabled_.load(std::memory_order_acquire) ||
			unhookInProgress_) return false;
		unhookInProgress_ = true;
		pending.swap(hooks_);
	}
	bool success = true;
	std::vector<OwnedHook> failed;
	for (std::vector<OwnedHook>::reverse_iterator it = pending.rbegin();
			it != pending.rend(); ++it) {
		bool unhooked = false;
		try {
			unhooked = it->unhook && it->unhook(it->handle);
		} catch (...) {
			unhooked = false;
		}
		if (!unhooked) {
			success = false;
			failed.push_back(*it);
		}
	}
	if (!success) {
		std::lock_guard<std::mutex> lock(mutex_);
		for (std::vector<OwnedHook>::const_iterator it = failed.begin();
				it != failed.end(); ++it) {
			// Keep only failed handles available for a later retry.
			hooks_.push_back(*it);
		}
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		unhookInProgress_ = false;
	}
	return success;
}

std::size_t HookManager::HookCount() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return hooks_.size();
}

std::vector<HookManager::HookChainRecord> HookManager::ChainSnapshot() const {
	std::vector<HookChainRecord> result;
	std::lock_guard<std::mutex> lock(mutex_);
	result.reserve(hooks_.size());
	for (const OwnedHook& hook : hooks_) result.push_back(hook.chain);
	return result;
}

bool HookManager::CanRestore(const HookChainRecord& record, void* currentHook) {
	return record.emmyHook != nullptr && currentHook == record.emmyHook;
}
