#include "emmy_debugger/vm/host_vm_registry.h"
#include "emmy_debugger/vm/vm_registry.h"

#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

namespace {

lua_State* FakeState(uintptr_t address) {
	return reinterpret_cast<lua_State*>(address);
}

void Require(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "vm registry test failed: " << message << std::endl;
		std::exit(1);
	}
}

} // namespace

int main() {
	NativeVmRegistry registry;
	std::vector<VmLifecycleEvent> events;
	registry.SetEventSink([&](const VmLifecycleEvent& event) {
		events.push_back(event);
		// The sink must run outside the registry lock and be safe to re-enter.
		Require(registry.Find(event.vmId) != nullptr, "event sink cannot re-enter registry");
	});

	lua_State* state = FakeState(0x1000);
	VmMetadata metadata;
	metadata.displayName = "first";
	const uint64_t id = registry.Register(state, metadata);
	Require(id != 0, "register returns an id");
	Require(registry.Register(state, metadata) == id, "duplicate state keeps its id");
	std::shared_ptr<const VmRecord> record = registry.Find(id);
	Require(record != nullptr && record->generation == 1, "first generation is one");
	Require(record->state == VmLifecycleState::Created, "new VM starts in Created");

	Require(registry.NotifyReady(id), "Created VM can become Ready");
	Require(registry.NotifyReady(id), "Ready notification is idempotent");
	record = registry.Find(id);
	Require(record != nullptr && record->state == VmLifecycleState::Ready, "VM becomes Ready");
	Require(record->contextGeneration == 1 && record->sourceEpoch == 1,
		"new VM starts with the first context and source generation");
	Require(registry.ResetContext(id, "pie-reset"), "active VM context can reset");
	record = registry.Find(id);
	Require(record != nullptr && record->contextGeneration == 2 && record->sourceEpoch == 2,
		"context reset advances both invalidation identities");
	Require(record->state == VmLifecycleState::Ready,
		"context reset preserves a non-paused lifecycle state");
	Require(events.back().contextReset && events.back().contextGeneration == 2 &&
		events.back().sourceEpoch == 2, "context reset emits an explicit lifecycle event");
	Require(registry.SetState(id, VmLifecycleState::Paused, "breakpoint"),
		"Ready VM can enter Paused");
	const uint64_t pausedContext = registry.Find(id)->contextGeneration;
	Require(registry.ResetContext(id, "paused-pie-reset"),
		"paused VM context can reset");
	record = registry.Find(id);
	Require(record != nullptr && record->state == VmLifecycleState::Running,
		"paused context reset resumes the VM lifecycle");
	Require(record->contextGeneration == pausedContext + 1 &&
		events.back().previous == VmLifecycleState::Paused &&
		events.back().current == VmLifecycleState::Running,
		"paused reset emits the running transition and new context identity");

	Require(registry.BeginClose(id, "test"), "Ready VM can begin close");
	Require(registry.BeginClose(id, "duplicate"), "BeginClose is idempotent");
	Require(registry.EndClose(id), "Closing VM can end close");
	Require(registry.EndClose(id), "EndClose is idempotent");
	Require(registry.FindByState(state) == nullptr, "closed state leaves active index");
	Require(registry.Snapshot().empty(), "closed VM is absent from active snapshot");
	Require(registry.SnapshotWithEventSeq().eventSeq >= 4,
	        "empty snapshot keeps the lifecycle event fence");
	Require(registry.Release(id), "closed VM can be released");
	Require(registry.Release(id), "Release is idempotent");

	const uint64_t reusedId = registry.Register(state, metadata);
	Require(reusedId != 0 && reusedId != id, "address reuse gets a new id");
	record = registry.Find(reusedId);
	Require(record != nullptr && record->generation > 1, "address reuse increments generation");
	Require(registry.Release(reusedId) == false, "active VM cannot be released before close");
	Require(registry.BeginClose(reusedId, "cleanup"), "reused VM can close");
	Require(registry.EndClose(reusedId), "reused VM close completes");
	Require(registry.Release(reusedId), "reused VM can be released");

	// Lost removes the raw-state lookup immediately, and a later VM reusing
	// the address receives a fresh generation without being affected by the
	// old record's eventual Release.
	const uint64_t lostId = registry.Register(state, metadata);
	Require(registry.SetState(lostId, VmLifecycleState::Lost, "transport-lost"),
			"VM can enter Lost");
	Require(registry.FindByState(state) == nullptr, "Lost VM is absent from active state index");
	const uint64_t afterLostId = registry.Register(state, metadata);
	Require(afterLostId != lostId, "Lost address reuse gets a new registration id");
	Require(registry.Find(afterLostId)->generation > registry.Find(lostId)->generation,
			"Lost address reuse increments generation");
	Require(registry.Release(lostId), "Lost VM can be released after address reuse");
	Require(registry.FindByState(state) != nullptr &&
			registry.FindByState(state)->id == afterLostId,
			"releasing old Lost VM does not remove replacement index");
	Require(registry.BeginClose(afterLostId, "lost-replacement"), "replacement can close");
	Require(registry.EndClose(afterLostId), "replacement close completes");
	Require(registry.Release(afterLostId), "replacement can be released");

	HostVmRegistry hostRegistry;
	lua_State* pendingState = FakeState(0x2000);
	VmMetadata pendingMetadata;
	pendingMetadata.displayName = "pending";
	const uint64_t pendingId = hostRegistry.RegisterBeforeAgent(pendingState, pendingMetadata);
	Require(pendingId != 0, "pending host registration has a non-zero id");
	Require(hostRegistry.MarkReady(pendingId), "pending host VM can become Ready");
	Require(hostRegistry.ResetContext(pendingId, "pre-agent-reset"),
		"pending host VM records a context reset before Agent activation");
	Require(registry.Find(pendingId) == nullptr, "pending VM is not native before activation");
	Require(hostRegistry.ReconcileExistingVms(registry), "host registry reconciles to native registry");
	record = registry.Find(pendingId);
	Require(record != nullptr && record->state == VmLifecycleState::Ready,
	        "reconciled VM keeps id and Ready state");
	Require(record->contextGeneration == 2 && record->sourceEpoch == 2,
		"reconciliation preserves pending context reset identities");
	Require(record->metadata.displayName == "pending", "host metadata is copied");
	Require(hostRegistry.IsActive(), "host registry becomes active after reconciliation");
	Require(hostRegistry.BeginClose(pendingId, "after-activation"),
	        "active host registry routes close to native registry");
	Require(hostRegistry.EndClose(pendingId), "active host registry routes end close");
	Require(hostRegistry.Release(pendingId), "active host registry routes release");

	// Host lifecycle calls are serialized behind reconciliation. A registration
	// arriving while the Agent adopts the initial VM must be routed to native
	// after activation instead of being left in an unobserved pending map.
	HostVmRegistry concurrentHost;
	NativeVmRegistry slowNative;
	std::mutex gateMutex;
	std::condition_variable gateCv;
	bool adoptionEntered = false;
	bool releaseAdoption = false;
	slowNative.SetEventSink([&](const VmLifecycleEvent&) {
		std::unique_lock<std::mutex> gate(gateMutex);
		adoptionEntered = true;
		gateCv.notify_all();
		gateCv.wait(gate, [&] { return releaseAdoption; });
	});
	lua_State* initialState = FakeState(0x6000);
	lua_State* racingState = FakeState(0x7000);
	const uint64_t initialId = concurrentHost.RegisterBeforeAgent(initialState, metadata);
	std::thread reconcile([&] { Require(concurrentHost.ReconcileExistingVms(slowNative),
			"concurrent host reconciliation succeeds"); });
	{
		std::unique_lock<std::mutex> gate(gateMutex);
		Require(gateCv.wait_for(gate, std::chrono::seconds(2), [&] { return adoptionEntered; }),
				"reconciliation reaches native adoption");
	}
	std::atomic<bool> racingDone(false);
	uint64_t racingId = 0;
	std::thread racing([&] {
		racingId = concurrentHost.RegisterBeforeAgent(racingState, metadata);
		racingDone.store(true, std::memory_order_release);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	Require(racingDone.load(std::memory_order_acquire),
			"host registration is non-blocking during reconciliation");
	Require(slowNative.Find(racingId) == nullptr,
			"new host registration remains pending until the next reconcile pass");
	{
		std::lock_guard<std::mutex> gate(gateMutex);
		releaseAdoption = true;
	}
	gateCv.notify_all();
	reconcile.join();
	racing.join();
	Require(racingId != 0 && slowNative.Find(racingId) != nullptr,
			"post-reconcile registration is adopted by native registry");
	Require(slowNative.Find(initialId) != nullptr, "initial VM remains after reconciliation");
	Require(concurrentHost.FindByState(racingState) != nullptr,
			"host lookup uses authoritative native registry after activation");
	Require(concurrentHost.BeginClose(racingId, "cleanup-race"), "racing VM closes");
	Require(concurrentHost.EndClose(racingId), "racing VM end close");
	Require(concurrentHost.Release(racingId), "racing VM releases");

	// The destination event sink is allowed to call back into HostVmRegistry on
	// the reconciliation thread. This used to self-deadlock because every host
	// operation waited on reconciling_. The versioned pass must observe and
	// adopt the callback-created VM without blocking the sink.
	HostVmRegistry reentrantHost;
	NativeVmRegistry reentrantNative;
	lua_State* callbackState = FakeState(0x7100);
	uint64_t callbackId = 0;
	bool callbackEntered = false;
	reentrantNative.SetEventSink([&](const VmLifecycleEvent& event) {
		if (callbackEntered) return;
		callbackEntered = true;
		callbackId = reentrantHost.RegisterBeforeAgent(callbackState, metadata);
		Require(callbackId != 0, "reentrant host registration returns a pending id");
		Require(reentrantHost.MarkReady(callbackId), "reentrant pending VM accepts Ready");
		Require(!reentrantHost.ReconcileExistingVms(reentrantNative),
				"recursive reconcile does not claim the outer pass");
	});
	lua_State* reentrantInitialState = FakeState(0x7200);
	const uint64_t reentrantInitialId = reentrantHost.RegisterBeforeAgent(reentrantInitialState, metadata);
	Require(reentrantInitialId != 0, "reentrant initial VM has an id");
	Require(reentrantHost.ReconcileExistingVms(reentrantNative),
			"reentrant reconciliation completes");
	Require(callbackEntered && reentrantNative.Find(callbackId) != nullptr,
			"callback-created VM is adopted by a later versioned pass");
	Require(reentrantNative.Find(callbackId)->state == VmLifecycleState::Ready,
			"callback-created VM preserves its Ready state");

	// A VM can be closed and released while the initial adoption is blocked.
	// It must not be resurrected by a stale snapshot, and a replacement at the
	// same address must receive a newer generation.
	HostVmRegistry closeRaceHost;
	NativeVmRegistry closeRaceNative;
	std::mutex closeGateMutex;
	std::condition_variable closeGateCv;
	bool closeAdoptionEntered = false;
	bool releaseCloseAdoption = false;
	closeRaceNative.SetEventSink([&](const VmLifecycleEvent&) {
		std::unique_lock<std::mutex> gate(closeGateMutex);
		closeAdoptionEntered = true;
		closeGateCv.notify_all();
		closeGateCv.wait(gate, [&] { return releaseCloseAdoption; });
	});
	lua_State* reusedState = FakeState(0x7300);
	const uint64_t oldPendingId = closeRaceHost.RegisterBeforeAgent(reusedState, metadata);
	std::thread closeReconcile([&] {
		Require(closeRaceHost.ReconcileExistingVms(closeRaceNative),
				"close race reconciliation succeeds");
	});
	{
		std::unique_lock<std::mutex> gate(closeGateMutex);
		Require(closeGateCv.wait_for(gate, std::chrono::seconds(2),
				[&] { return closeAdoptionEntered; }),
				"close race reaches adoption");
	}
	Require(closeRaceHost.BeginClose(oldPendingId, "close-before-adopt"),
			"pending VM can begin close during reconcile");
	Require(closeRaceHost.EndClose(oldPendingId), "pending VM can end close during reconcile");
	Require(closeRaceHost.Release(oldPendingId), "pending VM can release during reconcile");
	const uint64_t replacementId = closeRaceHost.RegisterBeforeAgent(reusedState, metadata);
	Require(replacementId != 0 && replacementId != oldPendingId,
			"address reuse during reconcile gets a new pending id");
	{
		std::lock_guard<std::mutex> gate(closeGateMutex);
		releaseCloseAdoption = true;
	}
	closeGateCv.notify_all();
	closeReconcile.join();
	Require(closeRaceNative.Find(oldPendingId) == nullptr,
			"released pending VM is not resurrected");
	Require(closeRaceNative.Find(replacementId) != nullptr,
			"replacement VM is adopted");
	Require(closeRaceNative.Find(replacementId)->generation > 1,
			"replacement VM generation advances after address reuse");
	Require(closeRaceHost.BeginClose(replacementId, "cleanup-replacement"),
			"replacement closes after reconcile");
	Require(closeRaceHost.EndClose(replacementId), "replacement close completes");
	Require(closeRaceHost.Release(replacementId), "replacement releases");

	NativeVmRegistry abiRegistry;
	VmMetadata firstAbi;
	firstAbi.hasAbiDescriptor = true;
	firstAbi.abi.major = 5;
	firstAbi.abi.minor = 4;
	firstAbi.abi.release = "5.4.3";
	firstAbi.abi.layoutHash = "unlua-543-idsize256-sphook";
	firstAbi.abi.luaIdSize = 256;
	firstAbi.abi.privateLayoutSupported = true;
	firstAbi.abi.spHook = true;
	const uint64_t firstAbiId = abiRegistry.Register(FakeState(0x4000), firstAbi);
	Require(abiRegistry.Find(firstAbiId)->state == VmLifecycleState::Created,
		"first explicit ABI establishes the process fingerprint");
	VmMetadata secondAbi = firstAbi;
	secondAbi.abi.layoutHash = "stock-lua-546";
	const uint64_t secondAbiId = abiRegistry.Register(FakeState(0x5000), secondAbi);
	auto secondAbiRecord = abiRegistry.Find(secondAbiId);
	Require(secondAbiRecord != nullptr && secondAbiRecord->state == VmLifecycleState::Error,
		"mixed Lua ABI enters Error");
	Require(secondAbiRecord->metadata.abiError == "MIXED_LUA_ABI_UNSUPPORTED",
		"mixed Lua ABI has a stable error code");

	LuaAbiDescriptor publicExpected;
	publicExpected.major = 5;
	publicExpected.minor = 4;
	publicExpected.luaIdSize = 256;
	LuaAbiDescriptor publicActual;
	publicActual.major = 5;
	publicActual.minor = 4;
	publicActual.luaIdSize = 1024;
	std::string abiError;
	Require(ValidateLuaAbiDescriptor(publicExpected, publicActual, abiError),
		"public API mode ignores private layout differences");
	publicExpected.privateLayoutSupported = true;
	Require(!ValidateLuaAbiDescriptor(publicExpected, publicActual, abiError),
		"private layout claim requires an exact supported layout");
	Require(abiError == "LUA_PRIVATE_LAYOUT_UNAVAILABLE" ||
		abiError == "LUA_PRIVATE_LAYOUT_DESCRIPTOR_INCOMPLETE",
		"incomplete private layout has a stable refusal reason");

	// The complete UnLua fixture is strict: every private-layout fact must
	// match, and a generic 5.4 descriptor must not authorize private access.
	const LuaAbiDescriptor unlua = MakeUnLua54_3AbiDescriptor();
	Require(ValidateLuaAbiDescriptor(unlua, unlua, abiError),
		"exact UnLua ABI fixture is accepted");
	LuaAbiDescriptor altered = unlua;
	altered.release = "5.4.6";
	Require(!ValidateLuaAbiDescriptor(unlua, altered, abiError) &&
		abiError == "LUA_ABI_RELEASE_MISMATCH", "ABI release mismatch is rejected");
	altered = unlua;
	altered.layoutHash += "-changed";
	Require(!ValidateLuaAbiDescriptor(unlua, altered, abiError) &&
		abiError == "LUA_LAYOUT_HASH_MISMATCH", "layout hash mismatch is rejected");
	altered = unlua;
	altered.luaIdSize++;
	Require(!ValidateLuaAbiDescriptor(unlua, altered, abiError) &&
		abiError == "LUA_IDSIZE_MISMATCH", "LUA_IDSIZE mismatch is rejected");
	altered = unlua;
	altered.luaStateSize++;
	Require(!ValidateLuaAbiDescriptor(unlua, altered, abiError) &&
		abiError == "LUA_STATE_SIZE_MISMATCH", "lua_State size mismatch is rejected");
	altered = unlua;
	altered.globalStateOffset++;
	Require(!ValidateLuaAbiDescriptor(unlua, altered, abiError) &&
		abiError == "LUA_GLOBAL_STATE_OFFSET_MISMATCH",
		"global state offset mismatch is rejected");
	altered = unlua;
	altered.callInfoOffset++;
	Require(!ValidateLuaAbiDescriptor(unlua, altered, abiError) &&
		abiError == "LUA_CALL_INFO_OFFSET_MISMATCH",
		"call info offset mismatch is rejected");
	altered = unlua;
	altered.spHook = false;
	Require(!ValidateLuaAbiDescriptor(unlua, altered, abiError) &&
		abiError == "LUA_SP_HOOK_MISMATCH", "SP hook mismatch is rejected");

	HostVmRegistry closedBeforeActivation;
	lua_State* closedState = FakeState(0x3000);
	const uint64_t closedId = closedBeforeActivation.RegisterBeforeAgent(closedState, metadata);
	Require(closedId != 0, "closed pending VM has an id");
	Require(closedBeforeActivation.BeginClose(closedId, "before-agent"),
	        "pending VM can begin close");
	Require(closedBeforeActivation.EndClose(closedId), "pending VM can end close");
	Require(closedBeforeActivation.ReconcileExistingVms(registry), "closed pending VM reconciles");
	Require(registry.Find(closedId) == nullptr, "closed pending VM is not resurrected");

	Require(events.size() >= 8, "lifecycle events were emitted");
	for (std::size_t i = 1; i < events.size(); ++i) {
		Require(events[i - 1].eventSeq < events[i].eventSeq, "event sequence is monotonic");
	}

	std::cout << "vm registry tests passed" << std::endl;
	return 0;
}
