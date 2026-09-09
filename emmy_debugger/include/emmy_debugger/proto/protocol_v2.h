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
