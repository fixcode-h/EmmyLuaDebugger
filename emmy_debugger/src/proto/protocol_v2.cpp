#include "emmy_debugger/proto/protocol_v2.h"

#include <iomanip>
#include <algorithm>
#include <sstream>
#include <limits>

namespace {

std::string CanonicalJsonValue(const nlohmann::json& value) {
	if (value.is_object()) {
		std::vector<std::string> keys;
		for (nlohmann::json::const_iterator it = value.begin(); it != value.end(); ++it) {
			keys.push_back(it.key());
		}
		std::sort(keys.begin(), keys.end());
		std::ostringstream stream;
		stream << "{";
		for (std::vector<std::string>::const_iterator it = keys.begin(); it != keys.end(); ++it) {
			if (it != keys.begin()) stream << ",";
			stream << nlohmann::json(*it).dump() << ":" << CanonicalJsonValue(value.at(*it));
		}
		stream << "}";
		return stream.str();
	}
	if (value.is_array()) {
		std::ostringstream stream;
		stream << "[";
		for (nlohmann::json::const_iterator it = value.begin(); it != value.end(); ++it) {
			if (it != value.begin()) stream << ",";
			stream << CanonicalJsonValue(*it);
		}
		stream << "]";
		return stream.str();
	}
	return value.dump();
}

} // namespace

std::string CanonicalV2Json(const nlohmann::json& document) {
	return CanonicalJsonValue(document);
}

bool ValidateV2RequestIdentity(const nlohmann::json& document,
							   const std::string& expectedAgentSessionId,
							   uint64_t currentConnectionEpoch,
							   std::string& errorCode) {
	errorCode.clear();
	if (!document.is_object()) {
		errorCode = "INVALID_PROTOCOL_VERSION";
		return false;
	}
	const auto protocolIt = document.find("protocolVersion");
	if (protocolIt == document.end() || !protocolIt->is_number_integer() ||
		protocolIt->get<int>() != 2) {
		errorCode = "INVALID_PROTOCOL_VERSION";
		return false;
	}
	const auto kindIt = document.find("kind");
	if (kindIt == document.end() || !kindIt->is_string() || kindIt->get<std::string>() != "request") {
		errorCode = "INVALID_ENVELOPE_KIND";
		return false;
	}
	const auto sessionIt = document.find("agentSessionId");
	if (sessionIt == document.end() || !sessionIt->is_string() ||
		sessionIt->get<std::string>().empty()) {
		errorCode = "MISSING_AGENT_SESSION_ID";
		return false;
	}
	if (sessionIt->get<std::string>() != expectedAgentSessionId) {
		errorCode = "STALE_AGENT_SESSION";
		return false;
	}
	const auto epochIt = document.find("connectionEpoch");
	if (epochIt == document.end() || (!epochIt->is_number_unsigned() &&
		!epochIt->is_number_integer())) {
		errorCode = "MISSING_CONNECTION_EPOCH";
		return false;
	}
	uint64_t epoch = 0;
	try {
		if (epochIt->is_number_unsigned()) {
			epoch = epochIt->get<uint64_t>();
		} else {
			const int64_t signedEpoch = epochIt->get<int64_t>();
			if (signedEpoch > 0) epoch = static_cast<uint64_t>(signedEpoch);
		}
	} catch (...) {
		epoch = 0;
	}
	if (epoch == 0) {
		errorCode = "MISSING_CONNECTION_EPOCH";
		return false;
	}
	if (currentConnectionEpoch == 0 || epoch != currentConnectionEpoch) {
		errorCode = "STALE_CONNECTION_EPOCH";
		return false;
	}
	const auto requestIdIt = document.find("requestId");
	if (requestIdIt == document.end() || !requestIdIt->is_string() || requestIdIt->get<std::string>().empty()) {
		errorCode = "INVALID_REQUEST_ID";
		return false;
	}
	const auto typeIt = document.find("type");
	if (typeIt == document.end() || !typeIt->is_string() || typeIt->get<std::string>().empty()) {
		errorCode = "INVALID_REQUEST_TYPE";
		return false;
	}
	return true;
}

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

uint64_t ParseVmProtocolId(const nlohmann::json& value) {
	if (value.is_number_unsigned() || value.is_number_integer()) {
		if (value.is_number_integer() && !value.is_number_unsigned() && value.get<int64_t>() <= 0) return 0;
		return value.get<uint64_t>();
	}
	if (!value.is_string()) return 0;
	std::string text = value.get<std::string>();
	if (text.compare(0, 3, "vm-") == 0) text = text.substr(3);
	if (text.empty() || text.size() > 16) return 0;
	uint64_t id = 0;
	for (char ch : text) {
		unsigned digit;
		if (ch >= '0' && ch <= '9') digit = ch - '0';
		else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
		else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
		else return 0;
		id = id * 16 + digit;
	}
	return id;
}

bool ParseV2DebugTarget(const nlohmann::json& document, bool evaluation,
	V2DebugTarget& target, std::string& errorCode) {
	target = V2DebugTarget();
	errorCode = "INVALID_TARGET";
	if (!document.is_object()) return false;
	const auto targetIt = document.find("target");
	if (targetIt == document.end() || !targetIt->is_object()) return false;
	const auto vm = targetIt->find("vmId");
	if (vm == targetIt->end() || !vm->is_string() ||
		vm->get<std::string>().compare(0, 3, "vm-") != 0 ||
		(target.vmId = ParseVmProtocolId(*vm)) == 0) return false;
	auto positive = [](const nlohmann::json& object, const char* key, uint64_t& output) {
		const auto value = object.find(key);
		if (value == object.end() || value->is_null()) return true;
		if (!value->is_number_integer()) return false;
		if (!value->is_number_unsigned() && value->get<int64_t>() <= 0) return false;
		output = value->get<uint64_t>();
		return output != 0;
	};
	if (!positive(*targetIt, "pauseId", target.pauseId) ||
		!positive(document, "contextGeneration", target.contextGeneration) ||
		!positive(document, "sourceEpoch", target.sourceEpoch)) return false;
	auto identifier = [&](const char* key, std::string& output) {
		const auto value = targetIt->find(key);
		if (value == targetIt->end() || value->is_null()) return true;
		if (!value->is_string()) return false;
		output = value->get<std::string>();
		return !output.empty() && output.size() <= 128;
	};
	if (!identifier("threadId", target.threadId) || !identifier("frameId", target.frameId)) return false;
	if (evaluation && (target.pauseId == 0 || target.threadId.empty() || target.frameId.empty())) {
		errorCode = "STALE_PAUSE_REFERENCE";
		return false;
	}
	errorCode.clear();
	return true;
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
	vm["contextGeneration"] = record.contextGeneration;
	vm["sourceEpoch"] = record.sourceEpoch;
	vm["displayName"] = record.metadata.displayName;
	vm["state"] = VmLifecycleStateName(record.state);
	if (!record.metadata.luaVersionHint.empty()) {
		vm["luaVersion"] = record.metadata.luaVersionHint;
	}
	vm["discovery"] = record.metadata.discovery.empty() ? "HOST_API" : record.metadata.discovery;
	if (record.metadata.hasAbiDescriptor) {
		vm["abiFingerprint"] = record.metadata.abi.Fingerprint();
		vm["abiCompatible"] = record.metadata.abiCompatible;
		if (!record.metadata.abiError.empty()) vm["abiError"] = record.metadata.abiError;
	}
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
	payload["contextGeneration"] = event.contextGeneration;
	payload["sourceEpoch"] = event.sourceEpoch;
	if (event.previous != VmLifecycleState::Unknown) {
		payload["previous"] = VmLifecycleStateName(event.previous);
	}
	payload["current"] = VmLifecycleStateName(event.current);
	payload["eventSeq"] = event.eventSeq;
	if (event.contextReset) payload["contextReset"] = true;
	if (!event.reason.empty()) payload["reason"] = event.reason;
	nlohmann::json envelope = MakeV2Envelope("event", "vm.lifecycle", agentSessionId, connectionEpoch,
		std::string(), event.eventSeq, payload);
	envelope["target"] = nlohmann::json{{"vmId", VmProtocolId(event.vmId)}};
	return envelope;
}
