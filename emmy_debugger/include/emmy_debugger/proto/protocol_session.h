#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
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
		Invalid,
	};

	struct RequestRecord {
		std::string operationHash;
		std::string response;
		uint64_t connectionEpoch = 0;
	};

	ProtocolSession();

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
									uint64_t epoch);
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
	std::atomic<bool> negotiated_;
	std::atomic<bool> ready_;
	mutable std::mutex requestMutex_;
	std::map<std::string, RequestRecord> requests_;
	std::deque<std::string> requestOrder_;
	static const std::size_t kMaxRememberedRequests = 256;
};
