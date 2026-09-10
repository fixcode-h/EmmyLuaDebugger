#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

struct HostSourceIdentity {
	uint64_t vmId = 0;
	uint64_t sourceEpoch = 0;
	std::string chunkName;
	std::string canonicalPath;
	std::string sha256;
};

// Stores source identities supplied by the Lua host. It never reads files or
// Lua state; sha256 is the hash of the bytes loaded by the host.
class SourceRegistry {
public:
	explicit SourceRegistry(std::size_t maxEntries = 4096);

	bool Register(const HostSourceIdentity& identity);
	bool Resolve(uint64_t vmId, uint64_t sourceEpoch, const std::string& chunkName,
		HostSourceIdentity& result, bool legacyFuzzy = false) const;
	std::size_t Invalidate(uint64_t vmId, uint64_t sourceEpoch = 0);
	std::size_t Size() const;

private:
	static std::string NormalizePath(const std::string& path);
	static std::string Key(uint64_t vmId, uint64_t sourceEpoch, const std::string& chunkName);
	static bool ValidSha256(const std::string& hash);

	const std::size_t maxEntries_;
	mutable std::mutex mutex_;
	std::unordered_map<std::string, HostSourceIdentity> entries_;
};
