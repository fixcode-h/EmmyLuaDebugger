#pragma once

#include <mutex>
#include <string>

class TransportAuth {
public:
	TransportAuth();

	void SetExpectedToken(const std::string& token);
	bool IsRequired() const;
	bool Verify(const std::string& token) const;

private:
	mutable std::mutex mutex_;
	std::string expectedToken_;
};
