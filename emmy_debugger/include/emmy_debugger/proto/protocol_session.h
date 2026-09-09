#pragma once

#include <atomic>
#include <cstdint>
#include <string>

class ProtocolSession {
public:
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

private:
	std::string agentSessionId_;
	std::atomic<uint64_t> connectionEpoch_;
	std::atomic<uint64_t> requestSequence_;
	std::atomic<bool> negotiated_;
	std::atomic<bool> ready_;
};
