#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>

class ProtocolSession {
public:
	enum class RequestDisposition {
		New,
		Duplicate,
		Conflict,
		StaleEpoch,
		Busy,
		Invalid,
	};

	struct RequestRecord {
		std::string operationHash;
		std::string response;
		uint64_t connectionEpoch = 0;
		bool completed = false;
		bool started = false;
		bool cancelled = false;
		uint64_t deadline = 0;
	};

	enum class CancelDisposition { Cancelled, Unsupported, NotFound };
	using Clock = std::function<uint64_t()>;
	explicit ProtocolSession(Clock clock = Clock());

	void OnConnect(bool success);
	void OnDisconnect();
	void MarkNegotiated();
	void MarkReady();

	const std::string& AgentSessionId() const;
	uint64_t ConnectionEpoch() const;
	bool IsNegotiated() const;
	bool IsReady() const;
	std::string NextRequestId(const std::string& prefix);
	bool AcceptIncomingEpoch(uint64_t epoch, bool allowLegacyEpoch = true) const;
	RequestDisposition BeginRequest(const std::string& requestId,
									const std::string& operationHash,
									uint64_t epoch, uint64_t timeoutMillis = 5000);
	bool TryStartRequest(const std::string& requestId, uint64_t epoch, std::string& error);
	CancelDisposition CancelRequest(const std::string& requestId, uint64_t epoch);
	std::string RequestError(const std::string& requestId, uint64_t epoch) const;
	void CompleteRequest(const std::string& requestId,
								 const std::string& operationHash,
								 const std::string& response,
								 uint64_t epoch);
	bool CachedResponse(const std::string& requestId,
								const std::string& operationHash,
								std::string& response) const;

private:
	std::string agentSessionId_;
	std::atomic<uint64_t> connectionEpoch_;
	std::atomic<uint64_t> requestSequence_;
	std::atomic<bool> connected_;
	std::atomic<bool> negotiated_;
	std::atomic<bool> ready_;
	mutable std::mutex requestMutex_;
	std::map<std::string, RequestRecord> requests_;
	std::deque<std::string> requestOrder_;
	Clock clock_;
	static const std::size_t kMaxRememberedRequests = 256;
};
