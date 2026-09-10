#include "emmy_debugger/proto/restricted_eval.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <limits>

namespace {

bool IsIdentifierStart(unsigned char value) {
	return std::isalpha(value) != 0 || value == '_';
}

bool IsIdentifierChar(unsigned char value) {
	return std::isalnum(value) != 0 || value == '_';
}

bool IsForbiddenIdentifier(const std::string& value) {
	static const char* const forbidden[] = {
		"and", "or", "not", "nil", "true", "false", "function", "local",
		"return", "do", "end", "while", "for", "if", "then", "else",
		"elseif", "repeat", "until", "goto", "break", "require", "load",
		"loadfile", "dofile", "collectgarbage", "setmetatable", "getmetatable",
		"rawset", "rawget", "debug", "coroutine", "yield"
	};
	for (const char* candidate : forbidden) {
		if (value == candidate) return true;
	}
	return false;
}

bool IsIntegerInRange(const nlohmann::json& value, int minimum, int maximum, int& result) {
	if (!value.is_number_integer() && !value.is_number_unsigned()) return false;
	try {
		const int64_t number = value.get<int64_t>();
		if (number < minimum || number > maximum) return false;
		result = static_cast<int>(number);
		return true;
	} catch (...) {
		return false;
	}
}

bool IsForbiddenSegment(const std::string& value) {
	return (value.size() >= 2 && value[0] == '_' && value[1] == '_') ||
		IsForbiddenIdentifier(value);
}

bool ReadIdentifier(const std::string& text, std::size_t& cursor,
	std::string& value) {
	if (cursor >= text.size() ||
		!IsIdentifierStart(static_cast<unsigned char>(text[cursor]))) return false;
	const std::size_t start = cursor++;
	while (cursor < text.size() &&
		IsIdentifierChar(static_cast<unsigned char>(text[cursor]))) ++cursor;
	value = text.substr(start, cursor - start);
	return true;
}

void SkipSpace(const std::string& text, std::size_t& cursor) {
	while (cursor < text.size() &&
		std::isspace(static_cast<unsigned char>(text[cursor])) != 0) ++cursor;
}

} // namespace

bool ParseRestrictedValuePath(const std::string& expression,
	std::vector<RestrictedValuePathSegment>& segments,
	std::string& errorCode,
	const RestrictedEvalLimits& limits) {
	segments.clear();
	errorCode.clear();
	if (limits.maxExpressionBytes == 0 || expression.empty() ||
		expression.size() > limits.maxExpressionBytes) {
		errorCode = expression.empty() ? "EVALUATION_DENIED" : "EVALUATION_LIMIT_EXCEEDED";
		return false;
	}
	if (limits.maxSegments == 0 || limits.maxDepth < 0) {
		errorCode = "EVALUATION_LIMIT_EXCEEDED";
		return false;
	}
	const std::string text = expression;
	std::size_t cursor = 0;
	SkipSpace(text, cursor);
	std::string root;
	if (!ReadIdentifier(text, cursor, root) || IsForbiddenSegment(root)) {
		errorCode = "EVALUATION_DENIED";
		return false;
	}
	RestrictedValuePathSegment rootSegment;
	rootSegment.kind = RestrictedValuePathSegmentKind::Identifier;
	rootSegment.text = root;
	segments.push_back(rootSegment);
	while (true) {
		SkipSpace(text, cursor);
		if (cursor >= text.size()) break;
		if (text[cursor] == '.') {
			++cursor;
			SkipSpace(text, cursor);
			std::string identifier;
			if (!ReadIdentifier(text, cursor, identifier) || IsForbiddenSegment(identifier)) {
				errorCode = "EVALUATION_DENIED";
				return false;
			}
			RestrictedValuePathSegment segment;
			segment.kind = RestrictedValuePathSegmentKind::Identifier;
			segment.text = identifier;
			segments.push_back(segment);
		} else if (text[cursor] == '[') {
			++cursor;
			SkipSpace(text, cursor);
			if (cursor >= text.size()) {
				errorCode = "EVALUATION_DENIED";
				return false;
			}
			RestrictedValuePathSegment segment;
			if (text[cursor] == '\'' || text[cursor] == '"') {
				const char quote = text[cursor++];
				const std::size_t start = cursor;
				while (cursor < text.size() && text[cursor] != quote) {
					if (text[cursor] == '\\' || text[cursor] == '\n' || text[cursor] == '\r') {
						errorCode = "EVALUATION_DENIED";
						return false;
					}
					++cursor;
				}
				if (cursor >= text.size()) {
					errorCode = "EVALUATION_DENIED";
					return false;
				}
				segment.kind = RestrictedValuePathSegmentKind::String;
				segment.text = text.substr(start, cursor - start);
				++cursor;
			} else if (std::isdigit(static_cast<unsigned char>(text[cursor])) != 0) {
				const std::size_t start = cursor;
				while (cursor < text.size() &&
					std::isdigit(static_cast<unsigned char>(text[cursor])) != 0) ++cursor;
				segment.kind = RestrictedValuePathSegmentKind::Integer;
				segment.text = text.substr(start, cursor - start);
			} else {
				std::string identifier;
				if (!ReadIdentifier(text, cursor, identifier) || IsForbiddenSegment(identifier)) {
					errorCode = "EVALUATION_DENIED";
					return false;
				}
				segment.kind = RestrictedValuePathSegmentKind::String;
				segment.text = identifier;
			}
			SkipSpace(text, cursor);
			if (cursor >= text.size() || text[cursor] != ']') {
				errorCode = "EVALUATION_DENIED";
				return false;
			}
			++cursor;
			segments.push_back(segment);
		} else {
			errorCode = "EVALUATION_DENIED";
			return false;
		}
		// Path traversal has its own bound. maxDepth controls expansion of the
		// resulting value; zero still permits reading a scalar at self.State.
		if (segments.size() > limits.maxSegments) {
			errorCode = "EVALUATION_LIMIT_EXCEEDED";
			return false;
		}
	}
	return !segments.empty();
}

bool ValidateRestrictedValuePath(const std::string& expression,
	std::string& errorCode,
	const RestrictedEvalLimits& limits) {
	std::vector<RestrictedValuePathSegment> segments;
	return ParseRestrictedValuePath(expression, segments, errorCode, limits);
}

bool ValidateRestrictedEvalPayload(const nlohmann::json& payload,
	std::string& errorCode,
	RestrictedEvalLimits* limits) {
	errorCode.clear();
	if (!payload.is_object()) {
		errorCode = "INVALID_ARGUMENT";
		return false;
	}
	if (!payload.contains("policy") || !payload["policy"].is_string() || payload["policy"].get<std::string>() != "VALUE_PATH") {
		errorCode = "EVALUATION_DENIED";
		return false;
	}
	if (!payload.contains("expr") || !payload["expr"].is_string()) {
		errorCode = "EVALUATION_DENIED";
		return false;
	}
	RestrictedEvalLimits parsed;
	if (payload.contains("depth") && !IsIntegerInRange(payload["depth"], 0, 3, parsed.maxDepth)) {
		errorCode = "EVALUATION_LIMIT_EXCEEDED";
		return false;
	}
	if (payload.contains("depth") && payload.contains("maxDepth") &&
		payload["depth"] != payload["maxDepth"]) {
		errorCode = "INVALID_ARGUMENT";
		return false;
	}
	if (payload.contains("maxDepth") && !IsIntegerInRange(payload["maxDepth"], 0, 3, parsed.maxDepth)) {
		errorCode = "EVALUATION_LIMIT_EXCEEDED";
		return false;
	}
	if (payload.contains("maxNodes") && !IsIntegerInRange(payload["maxNodes"], 1, 100, parsed.maxNodes)) {
		errorCode = "EVALUATION_LIMIT_EXCEEDED";
		return false;
	}
	if (payload.contains("maxBytes") && !IsIntegerInRange(payload["maxBytes"], 1, 64 * 1024, parsed.maxBytes)) {
		errorCode = "EVALUATION_LIMIT_EXCEEDED";
		return false;
	}
	if (payload.contains("cacheId") && (!payload["cacheId"].is_number_integer() ||
		payload["cacheId"].get<int64_t>() != 0)) {
		errorCode = "EVALUATION_DENIED";
		return false;
	}
	if (payload.contains("setValue") && payload["setValue"].is_boolean() &&
		payload["setValue"].get<bool>()) {
		errorCode = "EVALUATION_DENIED";
		return false;
	}
	if (!ValidateRestrictedValuePath(payload["expr"].get<std::string>(), errorCode, parsed)) return false;
	if (limits != nullptr) *limits = parsed;
	return true;
}
