#include "emmy_debugger/proto/protocol_v2.h"
#include "emmy_debugger/proto/protocol_session.h"
#include "emmy_debugger/transporter/transporter.h"
#include "emmy_debugger/transporter/transport_auth.h"

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
	Require(session.AcceptIncomingEpoch(0), "missing epoch is accepted for legacy-compatible requests");
	Require(!session.AcceptIncomingEpoch(0, false), "v2 must carry an explicit epoch");
	Require(session.AcceptIncomingEpoch(2), "current epoch is accepted");
	Require(!session.AcceptIncomingEpoch(1), "old epoch is rejected");
	const auto firstRequest = session.BeginRequest("r1", "hash-a", 2);
	Require(firstRequest == ProtocolSession::RequestDisposition::New, "first request is new");
	Require(session.BeginRequest("r1", "hash-a", 2) == ProtocolSession::RequestDisposition::Duplicate,
		"same request is idempotent");
	Require(session.BeginRequest("r1", "hash-b", 2) == ProtocolSession::RequestDisposition::Conflict,
		"request id reuse is rejected");
	session.CompleteRequest("r1", "hash-a", "{\"ok\":true}", 2);
	std::string cached;
	Require(session.CachedResponse("r1", "hash-a", cached) && cached == "{\"ok\":true}",
		"completed response can be replayed");
	Require(session.BeginRequest("old", "hash", 1) == ProtocolSession::RequestDisposition::StaleEpoch,
		"old epoch request is rejected");
	session.OnDisconnect();
	session.OnConnect(true);
	Require(session.ConnectionEpoch() == 3, "second reconnect increments epoch");
	Require(!session.CachedResponse("r1", "hash-a", cached), "old epoch cache is cleared");
	Require(session.BeginRequest("r1", "hash-a", 3) == ProtocolSession::RequestDisposition::New,
		"request id can be reused after reconnect");

	TransportAuth auth;
	Require(!auth.IsRequired(), "auth is optional before a token is configured");
	auth.SetExpectedToken("token-123");
	Require(auth.IsRequired(), "configured auth token is required");
	Require(auth.Verify("token-123"), "matching token is accepted");
	Require(auth.VerifyForEpoch("token-123", 2), "matching token authenticates epoch");
	Require(auth.IsAuthenticatedForEpoch(2), "epoch authentication is recorded");
	auth.BeginEpoch(3);
	Require(!auth.IsAuthenticatedForEpoch(3), "reconnect starts unauthenticated");
	Require(!auth.VerifyForEpoch("token-123", 0), "token auth rejects epoch zero");
	Require(auth.VerifyForEpoch("token-123", 3), "reconnect requires fresh token verification");
	Require(!auth.Verify("token-124"), "wrong token is rejected");
	Require(!auth.Verify("token-123-extra"), "length mismatch is rejected");

	const nlohmann::json ready = MakeV2Envelope(
		"response", "agent.ready", session.AgentSessionId(), session.ConnectionEpoch(),
		"", 3, nlohmann::json{{"snapshotEventSeq", 3}});
	Require(ready["cmd"] == 18, "ready envelope command");
	Require(ready["protocolVersion"] == 2, "ready protocol version");
	Require(ready["type"] == "agent.ready", "ready envelope type");
	Require(ready["connectionEpoch"] == 3, "ready connection epoch");
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
