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

ProtocolSession::ProtocolSession(Clock clock)
	: agentSessionId_(MakeSessionId()),
	  connectionEpoch_(0),
	  requestSequence_(0),
	  connected_(false),
	  negotiated_(false),
	  ready_(false), clock_(clock ? clock : [] {
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	  }) {
}

void ProtocolSession::OnConnect(bool success) {
	std::lock_guard<std::mutex> lock(requestMutex_);
	if (!success) {
		connected_.store(false, std::memory_order_release);
		negotiated_.store(false, std::memory_order_release);
		ready_.store(false, std::memory_order_release);
		requests_.clear();
		requestOrder_.clear();
		return;
	}
	// Advance the epoch while holding the same lock used by request admission
	// and completion. An old worker that wakes after reconnect can therefore
	// never populate the cache belonging to the new connection.
	connectionEpoch_.fetch_add(1, std::memory_order_relaxed);
	connected_.store(true, std::memory_order_release);
	negotiated_.store(false, std::memory_order_release);
	ready_.store(false, std::memory_order_release);
	requests_.clear();
	requestOrder_.clear();
}

void ProtocolSession::OnDisconnect() {
	// A response from a disconnected transport can never be replayed safely.
	// Clear both completed and in-flight records before a later epoch starts.
	std::lock_guard<std::mutex> lock(requestMutex_);
	connected_.store(false, std::memory_order_release);
	negotiated_.store(false, std::memory_order_release);
	ready_.store(false, std::memory_order_release);
	requests_.clear();
	requestOrder_.clear();
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

bool ProtocolSession::AcceptIncomingEpoch(uint64_t epoch, bool allowLegacyEpoch) const {
	std::lock_guard<std::mutex> lock(requestMutex_);
	if (!connected_.load(std::memory_order_acquire)) return false;
	const uint64_t current = connectionEpoch_.load(std::memory_order_acquire);
	return (allowLegacyEpoch && epoch == 0) || (epoch != 0 && epoch == current);
}

ProtocolSession::RequestDisposition ProtocolSession::BeginRequest(
	const std::string& requestId,
	const std::string& operationHash,
	uint64_t epoch, uint64_t timeoutMillis) {
	if (requestId.empty() || operationHash.empty() || timeoutMillis == 0 || timeoutMillis > 30000) {
		return RequestDisposition::Invalid;
	}
	std::lock_guard<std::mutex> lock(requestMutex_);
	// Admission and connection state are checked under the same mutex.  A
	// disconnect racing this call must not leave an in-flight request behind.
	if (!connected_.load(std::memory_order_acquire) ||
		epoch == 0 || epoch != connectionEpoch_.load(std::memory_order_acquire)) {
		return RequestDisposition::StaleEpoch;
	}
	const auto it = requests_.find(requestId);
	if (it == requests_.end()) {
		// Keep every admitted in-flight identity until completion. Evicting an
		// active request lets a retry execute the same action a second time.
		if (requests_.size() >= kMaxRememberedRequests) {
			auto oldestCompleted = requestOrder_.end();
			for (auto order = requestOrder_.begin(); order != requestOrder_.end(); ++order) {
				if (requests_.at(*order).completed) { oldestCompleted = order; break; }
			}
			if (oldestCompleted == requestOrder_.end()) return RequestDisposition::Busy;
			requests_.erase(*oldestCompleted);
			requestOrder_.erase(oldestCompleted);
		}
		RequestRecord record;
		record.operationHash = operationHash;
		record.connectionEpoch = epoch;
		record.completed = false;
		record.deadline = clock_() + timeoutMillis;
		requests_.insert(std::make_pair(requestId, record));
		requestOrder_.push_back(requestId);
		return RequestDisposition::New;
	}
	return it->second.operationHash == operationHash
		? RequestDisposition::Duplicate
		: RequestDisposition::Conflict;
}

bool ProtocolSession::TryStartRequest(const std::string& requestId, uint64_t epoch, std::string& error) {
	std::lock_guard<std::mutex> lock(requestMutex_);
	const auto it = requests_.find(requestId);
	if (!connected_ || epoch != connectionEpoch_ || it == requests_.end()) error = "STALE_CONNECTION_EPOCH";
	else if (it->second.completed || it->second.cancelled) error = "CANCELLED";
	else if (clock_() >= it->second.deadline) error = "DEADLINE_EXCEEDED";
	else if (it->second.started) error = "REQUEST_IN_PROGRESS";
	else { it->second.started = true; error.clear(); return true; }
	return false;
}

ProtocolSession::CancelDisposition ProtocolSession::CancelRequest(const std::string& requestId, uint64_t epoch) {
	std::lock_guard<std::mutex> lock(requestMutex_);
	const auto it = requests_.find(requestId);
	if (!connected_ || epoch != connectionEpoch_ || it == requests_.end()) return CancelDisposition::NotFound;
	if (it->second.started || it->second.completed) return CancelDisposition::Unsupported;
	it->second.cancelled = true;
	return CancelDisposition::Cancelled;
}

std::string ProtocolSession::RequestError(const std::string& requestId, uint64_t epoch) const {
	std::lock_guard<std::mutex> lock(requestMutex_);
	const auto it = requests_.find(requestId);
	if (!connected_ || epoch != connectionEpoch_ || it == requests_.end()) return "STALE_CONNECTION_EPOCH";
	if (it->second.cancelled) return "CANCELLED";
	if (clock_() >= it->second.deadline) return "DEADLINE_EXCEEDED";
	return std::string();
}

void ProtocolSession::CompleteRequest(const std::string& requestId,
	const std::string& operationHash,
	const std::string& response,
	uint64_t epoch) {
	if (requestId.empty() || operationHash.empty()) {
		return;
	}
	std::lock_guard<std::mutex> lock(requestMutex_);
	// Check connected_ while holding requestMutex_.  Without this check an old
	// worker could complete in the small interval after OnDisconnect changed
	// the connection flag but before a new connection acquired the lock.
	if (!connected_.load(std::memory_order_acquire) ||
		epoch == 0 || epoch != connectionEpoch_.load(std::memory_order_acquire)) {
		return;
	}
	const auto it = requests_.find(requestId);
	if (it == requests_.end()) {
		return;
	}
	if (it->second.operationHash == operationHash && !it->second.completed) {
		it->second.response = response;
		it->second.connectionEpoch = epoch;
		it->second.completed = true;
	}
}

bool ProtocolSession::CachedResponse(const std::string& requestId,
	const std::string& operationHash,
	std::string& response) const {
	std::lock_guard<std::mutex> lock(requestMutex_);
	const auto it = requests_.find(requestId);
	if (!connected_.load(std::memory_order_acquire) ||
		it == requests_.end() || it->second.operationHash != operationHash ||
		!it->second.completed || it->second.connectionEpoch != ConnectionEpoch() ||
		it->second.response.empty()) {
		return false;
	}
	response = it->second.response;
	return true;
}
