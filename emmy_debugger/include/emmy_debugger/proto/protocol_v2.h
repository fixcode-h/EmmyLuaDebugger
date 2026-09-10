#pragma once

#include "emmy_debugger/proto/proto.h"
#include "emmy_debugger/proto/protocol_session.h"
#include "emmy_debugger/vm/vm_registry.h"
#include "nlohmann/json.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

std::string VmLifecycleStateName(VmLifecycleState state);
std::string VmProtocolId(uint64_t registrationId);
uint64_t ParseVmProtocolId(const nlohmann::json& value);

struct V2DebugTarget {
	uint64_t vmId = 0;
	uint64_t pauseId = 0;
	uint64_t contextGeneration = 0;
	uint64_t sourceEpoch = 0;
	std::string threadId;
	std::string frameId;
};

// Optional generation fields are accepted only when they are positive integers.
// Malformed identity must never silently downgrade to legacy routing.
bool ParseV2DebugTarget(const nlohmann::json& document, bool evaluation,
	V2DebugTarget& target, std::string& errorCode);

// Validates the identity envelope before routing a v2 request. Keeping this
// pure makes the admission contract testable without a live transport.
bool ValidateV2RequestIdentity(const nlohmann::json& document,
							   const std::string& expectedAgentSessionId,
							   uint64_t currentConnectionEpoch,
							   std::string& errorCode);

// Stable key ordering prevents semantically identical JSON requests from
// producing different idempotency hashes.
std::string CanonicalV2Json(const nlohmann::json& document);

nlohmann::json MakeV2Envelope(const std::string& kind,
							 const std::string& type,
							 const std::string& agentSessionId,
							 uint64_t connectionEpoch,
							 const std::string& requestId,
							 uint64_t eventSeq,
							 const nlohmann::json& payload,
							 bool ok = true,
							 const nlohmann::json& error = nlohmann::json());

nlohmann::json MakeVmSnapshotEnvelope(
		const std::string& agentSessionId,
		uint64_t connectionEpoch,
		const std::vector<std::shared_ptr<const VmRecord>>& records,
		uint64_t snapshotEventSeq,
		const std::string& requestId = std::string());

nlohmann::json MakeVmLifecycleEnvelope(const std::string& agentSessionId,
									  uint64_t connectionEpoch,
									  const VmLifecycleEvent& event);
