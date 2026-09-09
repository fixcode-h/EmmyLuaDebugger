#pragma once

#include "emmy_debugger/vm/vm_registry.h"

class HostVmRegistry {
public:
	HostVmRegistry();
	~HostVmRegistry();

	// Before the Agent handshake this stores a pending record and returns a
	// non-zero handle so the host can still report Ready/Close later.
	uint64_t RegisterBeforeAgent(lua_State* mainState, const VmMetadata& metadata);
	bool MarkReady(uint64_t registrationId);
	bool BeginClose(uint64_t registrationId, const std::string& reason);
	bool EndClose(uint64_t registrationId);
	bool Release(uint64_t registrationId);
	bool SetDisplayName(uint64_t registrationId, const std::string& displayName);

	// Activates routing and reconciles all records that were created before the
	// Agent handshake. Repeated calls are idempotent for the same destination.
	bool ReconcileExistingVms(NativeVmRegistry& destination);
	bool DrainTo(NativeVmRegistry& destination) { return ReconcileExistingVms(destination); }
	bool IsActive() const;

private:
	uint64_t AllocatePending(lua_State* mainState, const VmMetadata& metadata);

	mutable std::mutex mutex_;
	std::map<uint64_t, std::shared_ptr<VmRecord>> records_;
	std::map<lua_State*, uint64_t> activeByState_;
	std::map<lua_State*, uint64_t> generationByState_;
	NativeVmRegistry* nativeRegistry_;
	bool active_;
};
