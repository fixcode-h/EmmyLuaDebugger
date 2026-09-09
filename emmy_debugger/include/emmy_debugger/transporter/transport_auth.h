#pragma once

#include <cstdint>
#include <mutex>
#include <string>

class TransportAuth {
public:
	TransportAuth();

	void SetExpectedToken(const std::string& token);
	bool IsRequired() const;
	bool Verify(const std::string& token) const;
	// Verifies the token and records the connection epoch that authenticated it.
	// The same attach token may be reused by a reconnect of the same Agent.
	bool VerifyForEpoch(const std::string& token, uint64_t connectionEpoch);
	void BeginEpoch(uint64_t connectionEpoch);
	bool IsAuthenticatedForEpoch(uint64_t connectionEpoch) const;
	void ClearAuthenticatedEpoch();

private:
	mutable std::mutex mutex_;
	std::string expectedToken_;
	uint64_t authenticatedEpoch_ = 0;
};
