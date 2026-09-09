#include "emmy_debugger/proto/protocol_session.h"

#include <chrono>
#include <iomanip>
#include <sstream>

namespace {

std::string MakeSessionId() {
	const uint64_t now = static_cast<uint64_t>(
		std::chrono::steady_clock::now().time_since_epoch().count());
	std::ostringstream stream;
	stream << "agent-" << std::hex << now << "-" << reinterpret_cast<uintptr_t>(&MakeSessionId);
	return stream.str();
}

} // namespace

ProtocolSession::ProtocolSession()
	: agentSessionId_(MakeSessionId()),
	  connectionEpoch_(0),
	  requestSequence_(0),
	  negotiated_(false),
	  ready_(false) {
}

void ProtocolSession::OnConnect(bool success) {
	if (!success) {
		return;
	}
	connectionEpoch_.fetch_add(1, std::memory_order_relaxed);
	negotiated_.store(false, std::memory_order_release);
	ready_.store(false, std::memory_order_release);
	std::lock_guard<std::mutex> lock(requestMutex_);
	requests_.clear();
	requestOrder_.clear();
}

void ProtocolSession::OnDisconnect() {
	negotiated_.store(false, std::memory_order_release);
	ready_.store(false, std::memory_order_release);
}

void ProtocolSession::MarkNegotiated() {
	negotiated_.store(true, std::memory_order_release);
}

void ProtocolSession::MarkReady() {
	negotiated_.store(true, std::memory_order_release);
	ready_.store(true, std::memory_order_release);
}

const std::string& ProtocolSession::AgentSessionId() const {
	return agentSessionId_;
}

uint64_t ProtocolSession::ConnectionEpoch() const {
	return connectionEpoch_.load(std::memory_order_acquire);
}

bool ProtocolSession::IsNegotiated() const {
	return negotiated_.load(std::memory_order_acquire);
}

bool ProtocolSession::IsReady() const {
	return ready_.load(std::memory_order_acquire);
}

std::string ProtocolSession::NextRequestId(const std::string& prefix) {
	std::ostringstream stream;
	stream << prefix << "-" << requestSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
	return stream.str();
}

bool ProtocolSession::AcceptIncomingEpoch(uint64_t epoch) const {
	return epoch == 0 || epoch == ConnectionEpoch();
}

ProtocolSession::RequestDisposition ProtocolSession::BeginRequest(
	const std::string& requestId,
	const std::string& operationHash,
	uint64_t epoch) {
	if (requestId.empty() || operationHash.empty()) {
		return RequestDisposition::Invalid;
	}
	if (!AcceptIncomingEpoch(epoch)) {
		return RequestDisposition::StaleEpoch;
	}
	std::lock_guard<std::mutex> lock(requestMutex_);
	const auto it = requests_.find(requestId);
	if (it == requests_.end()) {
		requests_.insert(std::make_pair(requestId, RequestRecord{operationHash, std::string(), epoch}));
		requestOrder_.push_back(requestId);
		while (requestOrder_.size() > kMaxRememberedRequests) {
			requests_.erase(requestOrder_.front());
			requestOrder_.pop_front();
		}
		return RequestDisposition::New;
	}
	return it->second.operationHash == operationHash
		? RequestDisposition::Duplicate
		: RequestDisposition::Conflict;
}

void ProtocolSession::CompleteRequest(const std::string& requestId,
	const std::string& operationHash,
	const std::string& response,
	uint64_t epoch) {
	if (requestId.empty() || operationHash.empty() || !AcceptIncomingEpoch(epoch)) {
		return;
	}
	std::lock_guard<std::mutex> lock(requestMutex_);
	const auto it = requests_.find(requestId);
	if (it == requests_.end()) {
		return;
	}
	if (it->second.operationHash == operationHash) {
		it->second.response = response;
		it->second.connectionEpoch = epoch;
	}
}

bool ProtocolSession::CachedResponse(const std::string& requestId,
	const std::string& operationHash,
	std::string& response) const {
	std::lock_guard<std::mutex> lock(requestMutex_);
	const auto it = requests_.find(requestId);
	if (it == requests_.end() || it->second.operationHash != operationHash ||
		it->second.response.empty()) {
		return false;
	}
	response = it->second.response;
	return true;
}
