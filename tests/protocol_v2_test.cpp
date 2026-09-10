#include "emmy_debugger/proto/protocol_v2.h"
#include "emmy_debugger/proto/protocol_session.h"
#include "emmy_debugger/transporter/transporter.h"
#include "emmy_debugger/transporter/transport_auth.h"

#include <cstdlib>
#include <fstream>
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
	std::ifstream fixtures("tests/protocol_fuzz_cases.jsonl");
	Require(fixtures.good(), "malformed protocol fixture must exist");
	std::string fixtureLine;
	unsigned fixtureCount = 0;
	while (std::getline(fixtures, fixtureLine)) {
		const auto fixture = nlohmann::json::parse(fixtureLine);
		const auto stage = fixture.at("stage").get<std::string>();
		if (stage == "frame") continue; // Executed by transporter_frame_test.
		std::string error;
		bool accepted = true;
		if (stage == "identity") {
			accepted = ValidateV2RequestIdentity(fixture.at("input"), "fixture-agent", 2, error);
		} else {
			Require(stage == "target", "unknown fixture stage must fail the suite");
			V2DebugTarget target;
			accepted = ParseV2DebugTarget(fixture.at("input"), true, target, error);
		}
		Require(!accepted && error == fixture.at("expected").get<std::string>(),
			fixture.at("name").get<std::string>().c_str());
		++fixtureCount;
	}
	Require(fixtureCount >= 9, "identity/target fixtures are not silently skipped");
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
	record.contextGeneration = 3;
	record.sourceEpoch = 4;
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
	Require(snapshot["payload"]["vms"][0]["contextGeneration"] == 3,
		"snapshot context generation");
	Require(snapshot["payload"]["vms"][0]["sourceEpoch"] == 4,
		"snapshot source epoch");

	VmLifecycleEvent lifecycleEvent;
	lifecycleEvent.vmId = 7;
	lifecycleEvent.generation = 2;
	lifecycleEvent.previous = VmLifecycleState::Paused;
	lifecycleEvent.current = VmLifecycleState::Running;
	lifecycleEvent.reason = "pie-reset";
	lifecycleEvent.eventSeq = 4;
	lifecycleEvent.contextGeneration = 3;
	lifecycleEvent.sourceEpoch = 4;
	lifecycleEvent.contextReset = true;
	const nlohmann::json lifecycle = MakeVmLifecycleEnvelope(
		session.AgentSessionId(), session.ConnectionEpoch(),
		lifecycleEvent);
	Require(lifecycle["eventSeq"] == 4, "lifecycle event sequence");
	Require(lifecycle["target"]["vmId"] == "vm-7", "lifecycle target VM");
	Require(lifecycle["payload"]["current"] == "RUNNING", "lifecycle current state");
	Require(lifecycle["payload"]["eventSeq"] == 4, "lifecycle payload sequence");
	Require(lifecycle["payload"]["contextReset"] == true,
		"lifecycle identifies context reset explicitly");
	Require(lifecycle["payload"]["contextGeneration"] == 3 &&
		lifecycle["payload"]["sourceEpoch"] == 4,
		"lifecycle carries reset invalidation identities");

	const nlohmann::json identityBase = nlohmann::json{
		{"protocolVersion", 2}, {"kind", "request"}, {"type", "debug.unknown"},
		{"requestId", "unknown-1"}, {"agentSessionId", session.AgentSessionId()},
		{"connectionEpoch", 3}, {"payload", nlohmann::json{{"b", 2}, {"a", 1}}}
	};
	std::string identityError;
	Require(ValidateV2RequestIdentity(identityBase, session.AgentSessionId(), 3, identityError),
		"current v2 identity is accepted");
	Require(!ValidateV2RequestIdentity(identityBase, session.AgentSessionId(), 4, identityError) &&
		identityError == "STALE_CONNECTION_EPOCH", "old/future epoch is rejected");
	nlohmann::json missingSession = identityBase;
	missingSession.erase("agentSessionId");
	Require(!ValidateV2RequestIdentity(missingSession, session.AgentSessionId(), 3, identityError) &&
		identityError == "MISSING_AGENT_SESSION_ID", "missing session is rejected");
	nlohmann::json missingEpoch = identityBase;
	missingEpoch.erase("connectionEpoch");
	Require(!ValidateV2RequestIdentity(missingEpoch, session.AgentSessionId(), 3, identityError) &&
		identityError == "MISSING_CONNECTION_EPOCH", "missing epoch is rejected");
	nlohmann::json wrongSession = identityBase;
	wrongSession["agentSessionId"] = "agent-other";
	Require(!ValidateV2RequestIdentity(wrongSession, session.AgentSessionId(), 3, identityError) &&
		identityError == "STALE_AGENT_SESSION", "wrong session is rejected");
	nlohmann::json reordered = nlohmann::json::object();
	reordered["payload"] = nlohmann::json{{"a", 1}, {"b", 2}};
	reordered["connectionEpoch"] = 3;
	reordered["requestId"] = "unknown-1";
	reordered["type"] = "debug.unknown";
	reordered["kind"] = "request";
	reordered["protocolVersion"] = 2;
	reordered["agentSessionId"] = session.AgentSessionId();
	Require(CanonicalV2Json(identityBase) == CanonicalV2Json(reordered),
		"canonical JSON ignores object key order");

	const auto unknownFirst = session.BeginRequest("unknown-1", CanonicalV2Json(identityBase), 3);
	Require(unknownFirst == ProtocolSession::RequestDisposition::New, "unknown request can start");
	const auto unknownRetry = session.BeginRequest("unknown-1", CanonicalV2Json(reordered), 3);
	Require(unknownRetry == ProtocolSession::RequestDisposition::Duplicate,
		"canonical duplicate request is detected");

	V2DebugTarget parsedTarget;
	const nlohmann::json debugRequest = {
		{"contextGeneration", 3}, {"sourceEpoch", 4},
		{"target", {{"vmId", "vm-7"}, {"pauseId", 2}, {"threadId", "thread-1"}, {"frameId", "frame-2-0"}}}
	};
	Require(ParseV2DebugTarget(debugRequest, true, parsedTarget, identityError), "debug target parses");
	Require(parsedTarget.vmId == 7 && parsedTarget.pauseId == 2 &&
		parsedTarget.contextGeneration == 3 && parsedTarget.sourceEpoch == 4,
		"context identity survives protocol routing");
	for (const char* invalid : {"vm-7junk", "vm--1", "vm-+1", "vm- 1", "vm-0", "vm-10000000000000000"}) {
		auto malformed = debugRequest;
		malformed["target"]["vmId"] = invalid;
		Require(!ParseV2DebugTarget(malformed, true, parsedTarget, identityError), "invalid VM id rejected");
	}
	for (const auto& invalid : {nlohmann::json(-1), nlohmann::json(0), nlohmann::json(1.5), nlohmann::json("3")}) {
		auto malformed = debugRequest;
		malformed["contextGeneration"] = invalid;
		Require(!ParseV2DebugTarget(malformed, true, parsedTarget, identityError), "invalid generation rejected");
	}
	auto missingThread = debugRequest;
	missingThread["target"].erase("threadId");
	Require(!ParseV2DebugTarget(missingThread, true, parsedTarget, identityError), "eval requires thread identity");
	Require(ParseVmProtocolId("vm-7junk") == 0 && ParseVmProtocolId(-1) == 0,
		"legacy parser also rejects partial hex ids and negative ids");
	std::cout << "protocol v2 tests passed" << std::endl;
	return 0;
}
