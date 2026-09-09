#include "emmy_debugger/proto/protocol_v2.h"
#include "emmy_debugger/proto/protocol_session.h"
#include "emmy_debugger/transporter/transporter.h"

#include <cstdlib>
#include <iostream>

namespace {

void Require(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "protocol v2 test failed: " << message << std::endl;
		std::exit(1);
	}
}

} // namespace

int main() {
	Require(static_cast<int>(MessageCMD::Unknown) == 0, "Unknown wire id");
	Require(static_cast<int>(MessageCMD::InitReq) == 1, "InitReq wire id");
	Require(static_cast<int>(MessageCMD::ReadyRsp) == 4, "ReadyRsp wire id");
	Require(static_cast<int>(MessageCMD::LogNotify) == 17, "LogNotify wire id");
	Require(static_cast<int>(MessageCMD::EnvelopeV2) == 18, "EnvelopeV2 wire id");

	ProtocolSession session;
	const std::string sessionId = session.AgentSessionId();
	Require(!sessionId.empty(), "session id is generated");
	Require(session.ConnectionEpoch() == 0, "epoch starts at zero");
	session.OnConnect(true);
	Require(session.ConnectionEpoch() == 1, "first connection increments epoch");
	session.MarkNegotiated();
	session.MarkReady();
	Require(session.IsReady(), "session becomes ready");
	session.OnDisconnect();
	Require(!session.IsReady(), "disconnect clears ready state");
	session.OnConnect(true);
	Require(session.ConnectionEpoch() == 2, "reconnect increments epoch");
	Require(session.AgentSessionId() == sessionId, "session id survives reconnect");

	const nlohmann::json ready = MakeV2Envelope(
		"response", "agent.ready", session.AgentSessionId(), session.ConnectionEpoch(),
		"", 3, nlohmann::json{{"snapshotEventSeq", 3}});
	Require(ready["cmd"] == 18, "ready envelope command");
	Require(ready["protocolVersion"] == 2, "ready protocol version");
	Require(ready["type"] == "agent.ready", "ready envelope type");
	Require(ready["connectionEpoch"] == 2, "ready connection epoch");
	Require(ready["payload"]["snapshotEventSeq"] == 3, "ready snapshot sequence");

	VmMetadata metadata;
	metadata.displayName = "PIE";
	metadata.luaVersionHint = "5.4.3";
	metadata.discovery = "HOST_API";
	VmRecord record;
	record.id = 7;
	record.generation = 2;
	record.mainState = reinterpret_cast<lua_State*>(0x1000);
	record.metadata = metadata;
	record.state = VmLifecycleState::Ready;
	record.eventSeq = 3;
	std::vector<std::shared_ptr<const VmRecord>> records;
	records.push_back(std::shared_ptr<const VmRecord>(new VmRecord(record)));
	const nlohmann::json snapshot = MakeVmSnapshotEnvelope(
		session.AgentSessionId(), session.ConnectionEpoch(), records, 3);
	Require(snapshot["type"] == "vm.snapshot", "snapshot envelope type");
	Require(snapshot["kind"] == "event", "unsolicited snapshot is an event");
	Require(snapshot["payload"]["vms"].size() == 1, "snapshot VM count");
	Require(snapshot["payload"]["vms"][0]["vmId"] == "vm-7", "opaque VM id");
	Require(snapshot["payload"]["vms"][0]["state"] == "READY", "snapshot VM state");

	const nlohmann::json lifecycle = MakeVmLifecycleEnvelope(
		session.AgentSessionId(), session.ConnectionEpoch(),
		VmLifecycleEvent{7, 2, VmLifecycleState::Created, VmLifecycleState::Ready, "host-ready", 4});
	Require(lifecycle["eventSeq"] == 4, "lifecycle event sequence");
	Require(lifecycle["target"]["vmId"] == "vm-7", "lifecycle target VM");
	Require(lifecycle["payload"]["current"] == "READY", "lifecycle current state");
	Require(lifecycle["payload"]["eventSeq"] == 4, "lifecycle payload sequence");

	std::cout << "protocol v2 tests passed" << std::endl;
	return 0;
}
