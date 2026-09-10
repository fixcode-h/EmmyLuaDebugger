#include "emmy_debugger/vm/source_registry.h"

#include <algorithm>
#include <cctype>

SourceRegistry::SourceRegistry(std::size_t maxEntries)
	: maxEntries_(maxEntries) {
}

bool SourceRegistry::Register(const HostSourceIdentity& input) {
	if (input.vmId == 0 || input.sourceEpoch == 0 || input.chunkName.empty() ||
		input.canonicalPath.empty() || !ValidSha256(input.sha256) || maxEntries_ == 0) {
		return false;
	}
	HostSourceIdentity identity = input;
	identity.canonicalPath = NormalizePath(input.canonicalPath);
	if (identity.canonicalPath.empty()) {
		return false;
	}
	for (std::size_t start = 0; start < identity.canonicalPath.size();) {
		const std::size_t end = identity.canonicalPath.find('/', start);
		const std::string segment = identity.canonicalPath.substr(start,
			end == std::string::npos ? std::string::npos : end - start);
		if (segment == "..") return false;
		if (end == std::string::npos) break;
		start = end + 1;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	const std::string key = Key(identity.vmId, identity.sourceEpoch, identity.chunkName);
	if (entries_.find(key) == entries_.end() && entries_.size() >= maxEntries_) return false;
	entries_[key] = identity;
	return true;
}

bool SourceRegistry::Resolve(uint64_t vmId, uint64_t sourceEpoch, const std::string& chunkName,
	HostSourceIdentity& result, bool legacyFuzzy) const {
	if (vmId == 0 || sourceEpoch == 0 || chunkName.empty()) return false;
	std::lock_guard<std::mutex> lock(mutex_);
	auto exact = entries_.find(Key(vmId, sourceEpoch, chunkName));
	if (exact != entries_.end()) {
		result = exact->second;
		return true;
	}
	if (!legacyFuzzy) return false;
	const std::string normalizedChunk = NormalizePath(chunkName);
	const std::string suffix = "/" + normalizedChunk;
	for (const auto& pair : entries_) {
		const HostSourceIdentity& candidate = pair.second;
		if (candidate.vmId == vmId && candidate.sourceEpoch == sourceEpoch &&
			(NormalizePath(candidate.chunkName) == normalizedChunk ||
			 (candidate.canonicalPath.size() >= suffix.size() &&
			  candidate.canonicalPath.compare(candidate.canonicalPath.size() - suffix.size(), suffix.size(), suffix) == 0))) {
			result = candidate;
			return true;
		}
	}
	return false;
}

std::size_t SourceRegistry::Invalidate(uint64_t vmId, uint64_t sourceEpoch) {
	std::lock_guard<std::mutex> lock(mutex_);
	std::size_t removed = 0;
	for (auto it = entries_.begin(); it != entries_.end();) {
		if (it->second.vmId == vmId && (sourceEpoch == 0 || it->second.sourceEpoch == sourceEpoch)) {
			it = entries_.erase(it);
			++removed;
		} else {
			++it;
		}
	}
	return removed;
}

std::size_t SourceRegistry::Size() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return entries_.size();
}

std::string SourceRegistry::NormalizePath(const std::string& path) {
	std::string normalized;
	normalized.reserve(path.size());
	for (char value : path) {
		if (value == '\\') value = '/';
		if (value == '/' && !normalized.empty() && normalized.back() == '/') continue;
		normalized += value;
	}
	while (normalized.size() > 1 && normalized.back() == '/') normalized.pop_back();
	return normalized;
}

std::string SourceRegistry::Key(uint64_t vmId, uint64_t sourceEpoch, const std::string& chunkName) {
	return std::to_string(vmId) + ":" + std::to_string(sourceEpoch) + ":" + chunkName;
}

bool SourceRegistry::ValidSha256(const std::string& hash) {
	if (hash.size() != 64) return false;
	return std::all_of(hash.begin(), hash.end(), [](char value) {
		return std::isdigit(static_cast<unsigned char>(value)) ||
			(value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
	});
}
