#include "emmy_debugger/vm/host_vm_registry.h"
#include "emmy_debugger/vm/vm_registry.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
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

	HostVmRegistry hostRegistry;
	lua_State* pendingState = FakeState(0x2000);
	VmMetadata pendingMetadata;
	pendingMetadata.displayName = "pending";
	const uint64_t pendingId = hostRegistry.RegisterBeforeAgent(pendingState, pendingMetadata);
	Require(pendingId != 0, "pending host registration has a non-zero id");
	Require(hostRegistry.MarkReady(pendingId), "pending host VM can become Ready");
	Require(registry.Find(pendingId) == nullptr, "pending VM is not native before activation");
	Require(hostRegistry.ReconcileExistingVms(registry), "host registry reconciles to native registry");
	record = registry.Find(pendingId);
	Require(record != nullptr && record->state == VmLifecycleState::Ready,
	        "reconciled VM keeps id and Ready state");
	Require(record->metadata.displayName == "pending", "host metadata is copied");
	Require(hostRegistry.IsActive(), "host registry becomes active after reconciliation");
	Require(hostRegistry.BeginClose(pendingId, "after-activation"),
	        "active host registry routes close to native registry");
	Require(hostRegistry.EndClose(pendingId), "active host registry routes end close");
	Require(hostRegistry.Release(pendingId), "active host registry routes release");

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
