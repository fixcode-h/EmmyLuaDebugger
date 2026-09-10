#include "hook_manager.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void Require(bool value, const char* message) {
	if (!value) {
		std::cerr << "hook manager test failed: " << message << std::endl;
		std::exit(1);
	}
}
}

int main() {
	HookManager manager;
	std::atomic<bool> release(false);
	std::atomic<bool> entered(false);
	manager.Enable();
	std::thread callback([&] {
		HookManager::CallbackScope scope(manager);
		Require(static_cast<bool>(scope), "callback enters while enabled");
		entered.store(true, std::memory_order_release);
		while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
	});
	while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
	Require(!manager.DisableAndQuiesce(std::chrono::milliseconds(1)),
		"short quiescence deadline reports in-flight callback");
	release.store(true, std::memory_order_release);
	callback.join();
	Require(manager.InFlightCount() == 0, "callback count reaches zero");
	Require(manager.DisableAndQuiesce(std::chrono::milliseconds(100)),
		"quiescence succeeds after callback exits");
	Require(!manager.EnterCallback(), "disabled callback takes fast path");
	Require(!manager.AddHook(reinterpret_cast<void*>(99), [](void*) { return true; }),
		"new hooks are rejected once teardown has begun");
	// A fully drained manager may be explicitly reused for a new hook
	// generation.
	manager.Enable();

	int unhookCount = 0;
	Require(manager.AddHook(reinterpret_cast<void*>(1), [&](void*) {
		++unhookCount;
		return true;
	}), "hook handle is owned");
	Require(manager.AddHook(reinterpret_cast<void*>(2), [&](void*) {
		++unhookCount;
		return true;
	}), "second hook handle is owned");
	std::atomic<bool> failUnhook(true);
	Require(manager.AddHook(reinterpret_cast<void*>(3), [&](void*) {
		if (failUnhook.load(std::memory_order_acquire)) throw std::runtime_error("unhook failure");
		++unhookCount;
		return true;
	}), "throwing unhook handle is owned");
	Require(manager.DisableAndQuiesce(std::chrono::milliseconds(100)),
		"installed hooks quiesce before unhook");
	Require(!manager.UnhookIfSafe(), "failed unhook is reported");
	Require(unhookCount == 2 && manager.HookCount() == 1, "failed handle is retained");
	failUnhook.store(false, std::memory_order_release);
	Require(manager.UnhookIfSafe(), "failed unhook can be retried");
	Require(unhookCount == 3 && manager.HookCount() == 0, "retry releases retained handle");
	Require(manager.UnhookIfSafe(), "repeated unhook is idempotent");

	// Enabling while an uninstall callback is still running must not reopen the
	// callback gate or permit a second uninstall pass.
	manager.Enable();
	std::atomic<bool> unhookStarted(false);
	std::atomic<bool> releaseUnhook(false);
	Require(manager.AddHook(reinterpret_cast<void*>(4), [&](void*) {
		unhookStarted.store(true, std::memory_order_release);
		while (!releaseUnhook.load(std::memory_order_acquire)) std::this_thread::yield();
		return true;
	}), "hook can be installed for concurrent uninstall test");
	Require(manager.DisableAndQuiesce(std::chrono::milliseconds(100)),
		"concurrent uninstall test quiesces");
	std::atomic<bool> unhookResult(false);
	std::thread unhookThread([&] { unhookResult.store(manager.UnhookIfSafe()); });
	while (!unhookStarted.load(std::memory_order_acquire)) std::this_thread::yield();
	manager.Enable();
	Require(!manager.IsEnabled(), "enable is blocked while uninstall is in progress");
	Require(!manager.EnterCallback(), "callback stays disabled during uninstall");
	Require(!manager.UnhookIfSafe(), "second uninstall pass is rejected while busy");
	releaseUnhook.store(true, std::memory_order_release);
	unhookThread.join();
	Require(unhookResult.load() && manager.HookCount() == 0,
		"concurrent uninstall completes exactly once");
	manager.Enable();
	Require(manager.EnterCallback(), "manager can be reused after uninstall completes");
	manager.LeaveCallback();

	HookManager::HookChainRecord chain;
	chain.emmyHook = reinterpret_cast<void*>(0x1234);
	Require(HookManager::CanRestore(chain, reinterpret_cast<void*>(0x1234)),
		"unchanged host hook can be restored");
	Require(!HookManager::CanRestore(chain, reinterpret_cast<void*>(0x5678)),
		"host replacement is never overwritten");

	std::cout << "hook manager tests passed" << std::endl;
	return 0;
}
