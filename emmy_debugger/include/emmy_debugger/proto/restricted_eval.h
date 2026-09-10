#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "nlohmann/json_fwd.hpp"

struct RestrictedEvalLimits {
	std::size_t maxExpressionBytes = 4096;
	std::size_t maxSegments = 32;
	int maxDepth = 3;
	int maxNodes = 100;
	int maxBytes = 64 * 1024;
};

enum class RestrictedValuePathSegmentKind {
	Identifier,
	String,
	Integer,
};

struct RestrictedValuePathSegment {
	RestrictedValuePathSegmentKind kind = RestrictedValuePathSegmentKind::Identifier;
	std::string text;
};

// Parses the same deliberately small grammar used by the validator and by
// the Lua owner-thread evaluator.  The returned segments never contain Lua
// source; they are data-only lookup keys.
bool ParseRestrictedValuePath(const std::string& expression,
	std::vector<RestrictedValuePathSegment>& segments,
	std::string& errorCode,
	const RestrictedEvalLimits& limits = RestrictedEvalLimits());

// Validates the deliberately small path grammar used by AI/CLI evaluation.
// The error string is a stable protocol error code, not a parser diagnostic.
bool ValidateRestrictedValuePath(const std::string& expression,
	std::string& errorCode,
	const RestrictedEvalLimits& limits = RestrictedEvalLimits());

// Validates a v2 debug.eval payload before it reaches a Lua owner thread.
bool ValidateRestrictedEvalPayload(const nlohmann::json& payload,
	std::string& errorCode,
	RestrictedEvalLimits* limits = nullptr);
