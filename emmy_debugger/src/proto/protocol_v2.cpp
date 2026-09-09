#include "emmy_debugger/proto/protocol_v2.h"

#include <iomanip>
#include <sstream>

std::string VmLifecycleStateName(VmLifecycleState state) {
	switch (state) {
		case VmLifecycleState::Created: return "CREATED";
		case VmLifecycleState::Ready: return "READY";
		case VmLifecycleState::Running: return "RUNNING";
		case VmLifecycleState::Paused: return "PAUSED";
		case VmLifecycleState::Closing: return "CLOSING";
		case VmLifecycleState::Closed: return "CLOSED";
		case VmLifecycleState::Lost: return "LOST";
		case VmLifecycleState::Error: return "ERROR";
		case VmLifecycleState::Unknown: return "UNKNOWN";
	}
	return "UNKNOWN";
}

std::string VmProtocolId(uint64_t registrationId) {
	std::ostringstream stream;
	stream << "vm-" << std::hex << registrationId;
	return stream.str();
}

nlohmann::json MakeV2Envelope(const std::string& kind,
							 const std::string& type,
							 const std::string& agentSessionId,
							 uint64_t connectionEpoch,
							 const std::string& requestId,
							 uint64_t eventSeq,
							 const nlohmann::json& payload,
							 bool ok,
							 const nlohmann::json& error) {
	nlohmann::json envelope = nlohmann::json::object();
	envelope["cmd"] = 18;
	envelope["protocolVersion"] = 2;
	envelope["kind"] = kind;
	envelope["type"] = type;
	if (!requestId.empty()) envelope["requestId"] = requestId;
	if (!agentSessionId.empty()) envelope["agentSessionId"] = agentSessionId;
	if (connectionEpoch != 0) envelope["connectionEpoch"] = connectionEpoch;
	if (eventSeq != 0) envelope["eventSeq"] = eventSeq;
	envelope["ok"] = ok;
	if (!payload.is_null() && !payload.empty()) envelope["payload"] = payload;
	if (!error.is_null() && !error.empty()) envelope["error"] = error;
	return envelope;
}

namespace {

nlohmann::json SerializeVm(const VmRecord& record) {
	nlohmann::json vm = nlohmann::json::object();
	vm["vmId"] = VmProtocolId(record.id);
	vm["generation"] = record.generation;
	vm["displayName"] = record.metadata.displayName;
	vm["state"] = VmLifecycleStateName(record.state);
	if (!record.metadata.luaVersionHint.empty()) {
		vm["luaVersion"] = record.metadata.luaVersionHint;
	}
	vm["discovery"] = record.metadata.discovery.empty() ? "HOST_API" : record.metadata.discovery;
	if (record.mainState != nullptr) {
		std::ostringstream address;
		address << "0x" << std::hex << reinterpret_cast<uintptr_t>(record.mainState);
		vm["diagnosticStateAddress"] = address.str();
	}
	return vm;
}

} // namespace

nlohmann::json MakeVmSnapshotEnvelope(
	const std::string& agentSessionId,
	uint64_t connectionEpoch,
	const std::vector<std::shared_ptr<const VmRecord>>& records,
	uint64_t snapshotEventSeq,
	const std::string& requestId) {
	nlohmann::json payload = nlohmann::json::object();
	payload["snapshotEventSeq"] = snapshotEventSeq;
	payload["vms"] = nlohmann::json::array();
	for (std::vector<std::shared_ptr<const VmRecord>>::const_iterator it = records.begin();
		 it != records.end(); ++it) {
		if (*it) payload["vms"].push_back(SerializeVm(**it));
	}
	return MakeV2Envelope(requestId.empty() ? "event" : "response", "vm.snapshot",
		agentSessionId, connectionEpoch,
		requestId, snapshotEventSeq, payload);
}

nlohmann::json MakeVmLifecycleEnvelope(const std::string& agentSessionId,
									  uint64_t connectionEpoch,
									  const VmLifecycleEvent& event) {
	nlohmann::json payload = nlohmann::json::object();
	payload["vmId"] = VmProtocolId(event.vmId);
	payload["generation"] = event.generation;
	if (event.previous != VmLifecycleState::Unknown) {
		payload["previous"] = VmLifecycleStateName(event.previous);
	}
	payload["current"] = VmLifecycleStateName(event.current);
	payload["eventSeq"] = event.eventSeq;
	if (!event.reason.empty()) payload["reason"] = event.reason;
	nlohmann::json envelope = MakeV2Envelope("event", "vm.lifecycle", agentSessionId, connectionEpoch,
		std::string(), event.eventSeq, payload);
	envelope["target"] = nlohmann::json{{"vmId", VmProtocolId(event.vmId)}};
	return envelope;
}
