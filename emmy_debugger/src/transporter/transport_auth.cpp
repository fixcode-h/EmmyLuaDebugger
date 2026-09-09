#include "emmy_debugger/transporter/transport_auth.h"

#include <algorithm>
#include <cstdint>

TransportAuth::TransportAuth() = default;

void TransportAuth::SetExpectedToken(const std::string& token) {
	std::lock_guard<std::mutex> lock(mutex_);
	expectedToken_ = token;
}

bool TransportAuth::IsRequired() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return !expectedToken_.empty();
}

bool TransportAuth::Verify(const std::string& token) const {
	std::lock_guard<std::mutex> lock(mutex_);
	if (expectedToken_.empty()) {
		// Empty expected token explicitly means legacy/no-auth mode.
		return true;
	}

	const std::size_t maxLength = std::max(expectedToken_.size(), token.size());
	unsigned int difference = expectedToken_.size() == token.size() ? 0u : 1u;
	for (std::size_t i = 0; i < maxLength; ++i) {
		const unsigned char expected = i < expectedToken_.size()
			? static_cast<unsigned char>(expectedToken_[i]) : 0;
		const unsigned char actual = i < token.size()
			? static_cast<unsigned char>(token[i]) : 0;
		difference |= static_cast<unsigned int>(expected ^ actual);
	}
	return difference == 0;
}
