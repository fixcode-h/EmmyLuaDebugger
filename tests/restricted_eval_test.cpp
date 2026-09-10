#include "emmy_debugger/proto/restricted_eval.h"

#include "nlohmann/json.hpp"

#include <cstdlib>
#include <iostream>

namespace {

void Require(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "restricted eval test failed: " << message << std::endl;
		std::exit(1);
	}
}

void Reject(const std::string& expression, const char* expectedCode) {
	std::string code;
	std::vector<RestrictedValuePathSegment> segments;
	Require(!ParseRestrictedValuePath(expression, segments, code), "unsafe path is rejected");
	Require(code == expectedCode, "path rejection has stable error code");
}

} // namespace

int main() {
	std::string code;
	std::vector<RestrictedValuePathSegment> segments;
	Require(ParseRestrictedValuePath(" self . State[ 2 ][\"name\"] ", segments, code),
		"valid path parses");
	Require(segments.size() == 4, "path segment count");
	Require(segments[0].kind == RestrictedValuePathSegmentKind::Identifier &&
		segments[0].text == "self", "root identifier");
	Require(segments[2].kind == RestrictedValuePathSegmentKind::Integer &&
		segments[2].text == "2", "numeric index");
	Require(segments[3].kind == RestrictedValuePathSegmentKind::String &&
		segments[3].text == "name", "quoted index");

	Reject("fn()", "EVALUATION_DENIED");
	Reject("value = 1", "EVALUATION_DENIED");
	Reject("require('os')", "EVALUATION_DENIED");
	Reject("value.__secret", "EVALUATION_DENIED");
	Reject("value[1 + 2]", "EVALUATION_DENIED");

	RestrictedEvalLimits shallow;
	shallow.maxDepth = 0;
	Require(ParseRestrictedValuePath("a.b.c", segments, code, shallow), "scalar capture does not limit path traversal");
	shallow.maxDepth = 3;
	shallow.maxSegments = 2;
	Require(!ParseRestrictedValuePath("a.b.c", segments, code, shallow), "segment count is bounded");
	Require(code == "EVALUATION_LIMIT_EXCEEDED", "segment error code");

	nlohmann::json payload = {
		{"policy", "VALUE_PATH"},
		{"expr", "self.State[1]"},
		{"maxDepth", 3},
		{"maxNodes", 10},
		{"maxBytes", 1024},
		{"cacheId", 0}
	};
	RestrictedEvalLimits parsed;
	Require(ValidateRestrictedEvalPayload(payload, code, &parsed), "valid payload");
	Require(parsed.maxDepth == 3 && parsed.maxNodes == 10 && parsed.maxBytes == 1024,
		"payload limits are parsed");
	payload["depth"] = 2;
	Require(!ValidateRestrictedEvalPayload(payload, code, nullptr) && code == "INVALID_ARGUMENT",
		"conflicting depth fields are rejected");
	payload.erase("depth");
	payload["setValue"] = true;
	Require(!ValidateRestrictedEvalPayload(payload, code, nullptr) && code == "EVALUATION_DENIED",
		"assignment mode is rejected");

	std::cout << "restricted eval tests passed" << std::endl;
	return 0;
}
