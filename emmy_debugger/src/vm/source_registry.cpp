#include "emmy_debugger/vm/source_registry.h"

#include <algorithm>
#include <cctype>
#include <vector>

SourceRegistry::SourceRegistry(std::size_t maxEntries)
	: maxEntries_(maxEntries) {
}

bool SourceRegistry::Register(const HostSourceIdentity& input) {
	if (input.vmId == 0 || input.sourceEpoch == 0 || input.chunkName.empty() ||
		input.canonicalPath.empty() || input.chunkName.size() > kMaxFieldBytes ||
		input.canonicalPath.size() > kMaxFieldBytes || input.sha256.size() > kMaxFieldBytes ||
		!ValidSha256(input.sha256) || maxEntries_ == 0) {
		return false;
	}
	HostSourceIdentity identity = input;
	identity.chunkName = NormalizePath(input.chunkName);
	identity.canonicalPath = NormalizePath(input.canonicalPath);
	std::transform(identity.sha256.begin(), identity.sha256.end(), identity.sha256.begin(),
		[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
	if (identity.chunkName.empty() || identity.canonicalPath.empty()) {
		return false;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	const std::string key = Key(identity.vmId, identity.sourceEpoch, identity.chunkName);
	if (entries_.find(key) == entries_.end() && entries_.size() >= maxEntries_) return false;
	auto existing = entries_.find(key);
	if (existing != entries_.end() && existing->second.sha256 != identity.sha256) return false;
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
	std::string replaced;
	replaced.reserve(path.size());
	for (char value : path) {
		if (value == '\\') value = '/';
		if (value == '/' && !replaced.empty() && replaced.back() == '/') continue;
		replaced += value;
	}
	while (replaced.compare(0, 2, "./") == 0) replaced.erase(0, 2);
	if (!replaced.empty() && replaced.front() == '@') replaced.erase(0, 1);
	const bool absolute = (!replaced.empty() && replaced[0] == '/') ||
		(replaced.size() > 2 && replaced[1] == ':' && replaced[2] == '/');
	const std::string prefix = replaced.size() > 2 && replaced[1] == ':'
		? replaced.substr(0, 3) : (absolute ? "/" : "");
	const std::size_t start = prefix.empty() ? 0 : prefix.size();
	std::vector<std::string> parts;
	for (std::size_t pos = start; pos <= replaced.size();) {
		const std::size_t end = replaced.find('/', pos);
		const std::string part = replaced.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
		if (part.empty() || part == ".") {
		} else if (part == ".." && !parts.empty() && parts.back() != "..") {
			parts.pop_back();
		} else if (part != ".." || !absolute) {
			parts.push_back(part);
		}
		if (end == std::string::npos) break;
		pos = end + 1;
	}
	std::string normalized = prefix;
	for (std::size_t i = 0; i < parts.size(); ++i) {
		if (i > 0 || (!prefix.empty() && prefix.back() != '/')) {
			normalized += '/';
		}
		normalized += parts[i];
	}
#ifdef _WIN32
	std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char value) {
		return static_cast<char>(std::tolower(value));
	});
#endif
	return normalized;
}

std::string SourceRegistry::Key(uint64_t vmId, uint64_t sourceEpoch, const std::string& chunkName) {
	return std::to_string(vmId) + ":" + std::to_string(sourceEpoch) + ":" + NormalizePath(chunkName);
}

bool SourceRegistry::ValidSha256(const std::string& hash) {
	if (hash.size() != 64) return false;
	return std::all_of(hash.begin(), hash.end(), [](char value) {
		return std::isdigit(static_cast<unsigned char>(value)) ||
			(value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
	});
}
