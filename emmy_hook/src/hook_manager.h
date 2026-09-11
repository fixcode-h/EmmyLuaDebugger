#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Owns the lifetime of injected hooks independently from EasyHook.  The class
// is deliberately small and platform-neutral so its concurrency contract can
// be tested without loading a target process.
class HookManager {
public:
	using HookHandle = void*;
	using UnhookFunction = std::function<bool(HookHandle)>;

	struct HookChainRecord {
		HookChainRecord() : previousHook(nullptr), previousMask(0), previousCount(0), emmyHook(nullptr) {}
		void* previousHook;
		int previousMask;
		int previousCount;
		void* emmyHook;
		std::string owner;
	};

	class CallbackScope {
	public:
		explicit CallbackScope(HookManager& manager);
		~CallbackScope();
		CallbackScope(const CallbackScope&) = delete;
		CallbackScope& operator=(const CallbackScope&) = delete;
		operator bool() const { return entered_; }

	private:
		HookManager* manager_;
		bool entered_;
	};

	HookManager();

	/**
	 * Returns false when a previous generation still owns handles (an EasyHook
	 * uninstall can fail while its trampoline is still in use), so callers must
	 * not assume the manager became usable.
	 */
	bool Enable();
	bool IsEnabled() const;

	// Returns false after disable has begun. A successful entry must always be
	// paired with LeaveCallback (CallbackScope does this automatically).
	bool EnterCallback();
	void LeaveCallback();
	std::size_t InFlightCount() const;

	bool DisableAndQuiesce(std::chrono::milliseconds timeout);
	bool UnhookIfSafe();

	bool AddHook(HookHandle handle, const UnhookFunction& unhook,
				const HookChainRecord& chain = HookChainRecord());
	std::size_t HookCount() const;
	std::vector<HookChainRecord> ChainSnapshot() const;

	// A chain may be restored only if the host still points at Emmy's hook. If
	// the host replaced it meanwhile, leave the replacement untouched.
	static bool CanRestore(const HookChainRecord& record, void* currentHook);

private:
	struct OwnedHook {
		HookHandle handle;
		UnhookFunction unhook;
		HookChainRecord chain;
	};

	mutable std::mutex mutex_;
	std::condition_variable quiesced_;
	std::atomic<bool> enabled_;
	std::atomic<bool> teardownStarted_;
	std::size_t inFlight_;
	bool unhookInProgress_;
	std::vector<OwnedHook> hooks_;
};
