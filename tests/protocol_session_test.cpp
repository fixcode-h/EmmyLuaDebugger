#include "emmy_debugger/proto/protocol_session.h"

#include <cstdlib>
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <string>
#include <thread>

namespace {

void Require(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "protocol session test failed: " << message << std::endl;
		std::exit(1);
	}
}

} // namespace

int main() {
	ProtocolSession session;
	session.OnConnect(true);
	Require(session.ConnectionEpoch() == 1, "first connection has epoch one");

	Require(session.BeginRequest("request-1", "operation-a", 1) ==
			ProtocolSession::RequestDisposition::New,
			"first request is admitted");
	Require(session.BeginRequest("request-1", "operation-a", 1) ==
			ProtocolSession::RequestDisposition::Duplicate,
			"in-flight duplicate is recognized");
	std::string response;
	Require(!session.CachedResponse("request-1", "operation-a", response),
			"in-flight request has no replay response");
	Require(session.BeginRequest("request-1", "operation-b", 1) ==
			ProtocolSession::RequestDisposition::Conflict,
			"request id reuse with another operation is rejected");

	// A stale worker must not complete a request after a reconnect, even when
	// the request id and operation hash are otherwise identical.
	session.OnConnect(true);
	Require(session.ConnectionEpoch() == 2, "reconnect advances epoch");
	session.CompleteRequest("request-1", "operation-a", "old", 1);
	Require(!session.CachedResponse("request-1", "operation-a", response),
			"old epoch completion cannot populate new cache");
	Require(session.BeginRequest("request-1", "operation-a", 2) ==
			ProtocolSession::RequestDisposition::New,
			"same id is new after reconnect");
	session.CompleteRequest("request-1", "operation-a", "new", 2);
	Require(session.CachedResponse("request-1", "operation-a", response) && response == "new",
			"completed request can be replayed in current epoch");
	Require(session.BeginRequest("request-1", "operation-a", 2) ==
			ProtocolSession::RequestDisposition::Duplicate,
			"completed duplicate remains idempotent");

	session.OnDisconnect();
	Require(!session.CachedResponse("request-1", "operation-a", response),
			"disconnect drops completed response cache");
	Require(session.BeginRequest("request-1", "operation-a", 2) ==
			ProtocolSession::RequestDisposition::StaleEpoch,
			"request cannot be admitted while disconnected epoch is stale");

	// Admission and completion race a disconnect/reconnect cycle.  Each
	// operation may legally win the race, but no request admitted after the
	// disconnect may become replayable in the new epoch.
	for (int iteration = 0; iteration < 64; ++iteration) {
		session.OnConnect(true);
		const uint64_t epoch = session.ConnectionEpoch();
		std::atomic<bool> start(false);
		std::atomic<int> disposition(static_cast<int>(ProtocolSession::RequestDisposition::Invalid));
		std::thread worker([&] {
			while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
			disposition.store(static_cast<int>(session.BeginRequest(
				"race", "race-op", epoch)), std::memory_order_release);
			session.CompleteRequest("race", "race-op", "late", epoch);
		});
		start.store(true, std::memory_order_release);
		session.OnDisconnect();
		worker.join();
		std::string raceResponse;
		Require(!session.CachedResponse("race", "race-op", raceResponse),
				"disconnect closes the admission window and drops late completion");
		// The observed disposition is either stale (disconnect won) or new
		// (worker won before disconnect); neither may cross the next epoch.
		const int observed = disposition.load(std::memory_order_acquire);
		Require(observed == static_cast<int>(ProtocolSession::RequestDisposition::StaleEpoch) ||
				observed == static_cast<int>(ProtocolSession::RequestDisposition::New),
				"race returns a defined request disposition");
	}
	session.OnConnect(false);
	Require(!session.CachedResponse("race", "race-op", response),
			"failed connect clears any request records");

	session.OnConnect(true);
	const auto boundedEpoch = session.ConnectionEpoch();
	for (int index = 0; index < 256; ++index) {
		Require(session.BeginRequest("pending-" + std::to_string(index), "op", boundedEpoch) ==
			ProtocolSession::RequestDisposition::New, "bounded pending request admitted");
	}
	Require(session.BeginRequest("overflow", "op", boundedEpoch) ==
		ProtocolSession::RequestDisposition::Busy, "full pending table rejects new work");
	Require(session.BeginRequest("pending-0", "op", boundedEpoch) ==
		ProtocolSession::RequestDisposition::Duplicate, "oldest pending identity is retained");
	session.CompleteRequest("pending-1", "op", "first", boundedEpoch);
	session.CompleteRequest("pending-1", "op", "second", boundedEpoch);
	Require(session.CachedResponse("pending-1", "op", response) && response == "first",
		"first completion remains authoritative");
	Require(session.BeginRequest("overflow", "op", boundedEpoch) ==
		ProtocolSession::RequestDisposition::New, "completed response can free an admission slot");
	Require(session.BeginRequest("pending-0", "op", boundedEpoch) ==
		ProtocolSession::RequestDisposition::Duplicate, "admission never evicts an active request");

	std::cout << "protocol session tests passed" << std::endl;
	uint64_t now = 100;
	ProtocolSession timed([&] { return now; });
	timed.OnConnect(true);
	Require(timed.BeginRequest("queued", "eval", 1, 10) == ProtocolSession::RequestDisposition::New,
		"queued evaluation admitted with relative deadline");
	Require(timed.CancelRequest("queued", 1) == ProtocolSession::CancelDisposition::Cancelled,
		"queued evaluation can be cancelled");
	std::string error;
	Require(!timed.TryStartRequest("queued", 1, error) && error == "CANCELLED",
		"cancelled work never enters Lua");
	timed.BeginRequest("expired", "eval", 1, 10);
	now = 110;
	Require(!timed.TryStartRequest("expired", 1, error) && error == "DEADLINE_EXCEEDED",
		"expired queue entry never enters Lua");
	timed.BeginRequest("running", "eval", 1, 10);
	Require(timed.TryStartRequest("running", 1, error), "live request starts once");
	Require(timed.CancelRequest("running", 1) == ProtocolSession::CancelDisposition::Unsupported,
		"running Lua cannot be interrupted by protocol thread");
	now = 120;
	Require(timed.RequestError("running", 1) == "DEADLINE_EXCEEDED", "late result is expired");
	Require(timed.BeginRequest("invalid", "eval", 1, 30001) == ProtocolSession::RequestDisposition::Invalid,
		"unbounded deadline is rejected");
	return 0;
}
