#pragma once

#include "emmy_debugger/vm/vm_registry.h"

#include <cstdint>

class HostVmRegistry {
public:
	HostVmRegistry();
	~HostVmRegistry();

	// Before the Agent handshake this stores a pending record and returns a
	// non-zero handle so the host can still report Ready/Close later.
	uint64_t RegisterBeforeAgent(lua_State* mainState, const VmMetadata& metadata);
	bool MarkReady(uint64_t registrationId);
	bool RejectAbi(uint64_t registrationId, const std::string& error);
	bool BeginClose(uint64_t registrationId, const std::string& reason);
	bool EndClose(uint64_t registrationId);
	bool ResetContext(uint64_t registrationId, const std::string& reason);
	bool Release(uint64_t registrationId);
	bool SetDisplayName(uint64_t registrationId, const std::string& displayName);
	std::shared_ptr<const VmRecord> Find(uint64_t registrationId) const;
	std::shared_ptr<const VmRecord> FindByState(lua_State* mainState) const;

	// Activates routing and reconciles all records that were created before the
	// Agent handshake. Repeated calls are idempotent for the same destination.
	bool ReconcileExistingVms(NativeVmRegistry& destination);
	bool DrainTo(NativeVmRegistry& destination) { return ReconcileExistingVms(destination); }
	bool IsActive() const;

private:
	uint64_t AllocatePending(lua_State* mainState, const VmMetadata& metadata);
	uint64_t ResolveNativeIdLocked(uint64_t registrationId) const;

	mutable std::mutex mutex_;
	std::map<uint64_t, std::shared_ptr<VmRecord>> records_;
	std::map<lua_State*, uint64_t> activeByState_;
	std::map<lua_State*, uint64_t> generationByState_;
	// Host registration IDs are the public handles.  In the defensive case
	// where a destination registry already owns that ID, keep an explicit
	// alias instead of making callers guess which identity is authoritative.
	std::map<uint64_t, uint64_t> nativeIds_;
	NativeVmRegistry* nativeRegistry_;
	bool active_;
	bool reconciling_;
	uint64_t mutationVersion_;
};
