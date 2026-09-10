/*
* Copyright (c) 2019. tangzx(love.tangzx@qq.com)
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include <memory>
#include "emmy_debugger/proto/proto.h"
#include "emmy_debugger/proto/protocol_v2.h"
#include "emmy_debugger/api/lua_api.h"

namespace {

uint64_t ParseVmId(const nlohmann::json& value) {
	if (value.is_number_unsigned() || value.is_number_integer()) {
		return value.get<uint64_t>();
	}
	if (!value.is_string()) return 0;
	std::string text = value.get<std::string>();
	if (text.compare(0, 3, "vm-") == 0) text = text.substr(3);
	if (text.empty()) return 0;
	try {
		return static_cast<uint64_t>(std::stoull(text, nullptr, 16));
	} catch (...) {
		return 0;
	}
}

} // namespace

JsonProtocol::~JsonProtocol() {
}

nlohmann::json JsonProtocol::Serialize() {
	return nlohmann::json::object();
}

void JsonProtocol::Deserialize(nlohmann::json json) {
}

nlohmann::json InitParams::Serialize() {
	return JsonProtocol::Serialize();
}

void InitParams::Deserialize(nlohmann::json json) {
	if (json["authToken"].is_string()) {
		authToken = json["authToken"];
	}
	if (json["emmyHelperPath"].is_string()) {
		emmyHelperPath = json["emmyHelperPath"];
	}
	
	if (json["customHelperPath"].is_string()) {
		customHelperPath = json["customHelperPath"];
	}
	
	if (json["emmyHelperName"].is_string()) {
		emmyHelperName = json["emmyHelperName"];
	}
	
	if (json["emmyHelperExtName"].is_string()) {
		emmyHelperExtName = json["emmyHelperExtName"];
	}

	if (json["ext"].is_array()) {
		for (auto &e: json["ext"]) {
			std::string ex(e);
			// C++ can not use this only
			ext.emplace_back(ex);
		}
	}
}

nlohmann::json BreakPointContribution::Serialize() const {
	nlohmann::json object = nlohmann::json::object();
	object["owner"] = owner;
	if (!breakpointId.empty()) object["breakpointId"] = breakpointId;
	if (!condition.empty()) object["condition"] = condition;
	if (!logMessage.empty()) object["logMessage"] = logMessage;
	if (!hitCondition.empty()) object["hitCondition"] = hitCondition;
	if (hitCount != 0) object["hitCount"] = hitCount;
	if (runToHere) object["runToHere"] = true;
	if (autoContinue) object["autoContinue"] = true;
	return object;
}

void BreakPointContribution::Deserialize(const nlohmann::json& json) {
	if (json["owner"].is_string()) owner = json["owner"].get<std::string>();
	if (json["breakpointId"].is_string()) breakpointId = json["breakpointId"].get<std::string>();
	if (json["condition"].is_string()) condition = json["condition"].get<std::string>();
	if (json["logMessage"].is_string()) logMessage = json["logMessage"].get<std::string>();
	if (json["hitCondition"].is_string()) hitCondition = json["hitCondition"].get<std::string>();
	if (json["hitCount"].is_number_integer()) hitCount = json["hitCount"].get<int>();
	if (json["runToHere"].is_boolean()) runToHere = json["runToHere"].get<bool>();
	if (json["autoContinue"].is_boolean()) autoContinue = json["autoContinue"].get<bool>();
}

nlohmann::json BreakPoint::Serialize() {
	nlohmann::json object = nlohmann::json::object();
	object["file"] = file;
	object["line"] = line;
	if (!condition.empty()) object["condition"] = condition;
	if (!logMessage.empty()) object["logMessage"] = logMessage;
	if (!hitCondition.empty()) object["hitCondition"] = hitCondition;
	if (hitCount != 0) object["hitCount"] = hitCount;
	if (runToHere) object["runToHere"] = true;
	if (!owner.empty()) object["owner"] = owner;
	if (!breakpointId.empty()) object["breakpointId"] = breakpointId;
	if (vmId != 0) object["vmId"] = VmProtocolId(vmId);
	if (!sourceCanonicalPath.empty() || !sourceUri.empty() || !sourceHash.empty() ||
		sourceEpoch != 0 || contextGeneration != 0 || sourceVerified) {
		nlohmann::json source = nlohmann::json::object();
		if (!sourceCanonicalPath.empty()) source["canonicalPath"] = sourceCanonicalPath;
		if (!sourceUri.empty()) source["uri"] = sourceUri;
		if (!sourceHash.empty()) source["sourceHash"] = sourceHash;
		if (sourceEpoch != 0) source["sourceEpoch"] = sourceEpoch;
		if (contextGeneration != 0) source["contextGeneration"] = contextGeneration;
		source["verified"] = sourceVerified;
		object["sourceIdentity"] = source;
	}
	if (composite) object["composite"] = true;
	if (!contributions.empty()) {
		nlohmann::json array = nlohmann::json::array();
		for (std::vector<BreakPointContribution>::const_iterator it = contributions.begin();
			 it != contributions.end(); ++it) {
			array.push_back(it->Serialize());
		}
		object["contributions"] = array;
	}
	return object;
}

void BreakPoint::Deserialize(nlohmann::json json) {
	if (json.count("file") != 0) {
		file = json["file"];
	}
	if (json.count("line") != 0) {
		line = json["line"];
	}
	if (json.count("condition") != 0) {
		condition = json["condition"];
	}
	if (json.count("hitCondition") != 0) {
		hitCondition = json["hitCondition"];
	}
	if (json.count("logMessage") != 0) {
		logMessage = json["logMessage"];
	}
	if (json["hitCount"].is_number_integer()) {
		hitCount = json["hitCount"].get<int>();
	}
	if (json["runToHere"].is_boolean()) {
		runToHere = json["runToHere"].get<bool>();
	}
	if (json["owner"].is_string()) {
		owner = json["owner"].get<std::string>();
	}
	if (json["breakpointId"].is_string()) {
		breakpointId = json["breakpointId"].get<std::string>();
	}
	vmId = ParseVmId(json["vmId"]);
	if (json["composite"].is_boolean()) {
		composite = json["composite"].get<bool>();
	}
	const nlohmann::json& source = json["sourceIdentity"];
	if (source.is_object()) {
		if (source["canonicalPath"].is_string()) sourceCanonicalPath = source["canonicalPath"].get<std::string>();
		if (source["uri"].is_string()) sourceUri = source["uri"].get<std::string>();
		if (source["sourceHash"].is_string()) sourceHash = source["sourceHash"].get<std::string>();
		if (source["sourceEpoch"].is_number_unsigned() || source["sourceEpoch"].is_number_integer()) {
			sourceEpoch = source["sourceEpoch"].get<uint64_t>();
		}
		if (source["contextGeneration"].is_number_unsigned() || source["contextGeneration"].is_number_integer()) {
			contextGeneration = source["contextGeneration"].get<uint64_t>();
		}
		if (source["verified"].is_boolean()) sourceVerified = source["verified"].get<bool>();
	}
	if (json["contributions"].is_array()) {
		for (nlohmann::json::const_iterator it = json["contributions"].begin();
			 it != json["contributions"].end(); ++it) {
			BreakPointContribution contribution;
			contribution.Deserialize(*it);
			if (!contribution.owner.empty()) contributions.push_back(contribution);
		}
	}
}

nlohmann::json AddBreakpointParams::Serialize() {
	return JsonProtocol::Serialize();
}

void AddBreakpointParams::Deserialize(nlohmann::json json) {
	if (json["clear"].is_boolean()) {
		clear = json["clear"];
	}
	if (json["replaceComposite"].is_boolean()) {
		replaceComposite = json["replaceComposite"].get<bool>();
	}

	if (json["breakPoints"].is_array()) {
		for (auto breakPoint: json["breakPoints"]) {
			auto bp = std::make_shared<BreakPoint>();
			bp->Deserialize(breakPoint);
			breakPoints.push_back(bp);
		}
	}
}

nlohmann::json RemoveBreakpointParams::Serialize() {
	return JsonProtocol::Serialize();
}

void RemoveBreakpointParams::Deserialize(nlohmann::json json) {
	if (json["breakPoints"].is_array()) {
		for (auto breakPoint: json["breakPoints"]) {
			auto bp = std::make_shared<BreakPoint>();
			bp->Deserialize(breakPoint);
			breakPoints.push_back(bp);
		}
	}
}

nlohmann::json ActionParams::Serialize() {
	return JsonProtocol::Serialize();
}

void ActionParams::Deserialize(nlohmann::json json) {
	if (json.count("action") != 0 && json["action"].is_number_integer()) {
		action = json["action"].get<DebugAction>();
	}
	vmId = ParseVmId(json["vmId"]);
	if (json["pauseId"].is_number_unsigned() || json["pauseId"].is_number_integer()) {
		pauseId = json["pauseId"].get<uint64_t>();
	}
	if (json["threadId"].is_string()) {
		threadId = json["threadId"].get<std::string>();
	}
}

Variable::Variable()
	: nameType(LUA_TSTRING),
	  valueType(0),
	  cacheId(0) {
}

nlohmann::json Variable::Serialize() {
	auto obj = nlohmann::json::object();
	obj["name"] = name;
	obj["nameType"] = nameType;
	obj["value"] = value;
	obj["valueType"] = valueType;
	obj["valueTypeName"] = valueTypeName;
	obj["cacheId"] = cacheId;
	if (truncated) obj["truncated"] = true;

	// children
	if (!children.empty()) {
		auto arr = nlohmann::json::array();
		for (auto idx: children) {
			arr.push_back(idx->Serialize());
		}
		obj["children"] = arr;
	}
	return obj;
}

void Variable::Deserialize(nlohmann::json json) {
	if (json["truncated"].is_boolean()) truncated = json["truncated"].get<bool>();
	JsonProtocol::Deserialize(json);
}

Stack::Stack()
	: level(0), line(0), variableArena(std::make_shared<Arena<Variable>>()) {
}

nlohmann::json Stack::Serialize() {

	auto stackJson = nlohmann::json::object();
	stackJson["file"] = file;
	stackJson["functionName"] = functionName;
	if (!frameId.empty()) stackJson["frameId"] = frameId;
	if (sourceEpoch != 0) {
		stackJson["sourceIdentity"] = {{"canonicalPath", file},
			{"uri", file.find("://") != std::string::npos ? file : "file:///" + file},
			{"sourceEpoch", sourceEpoch}, {"verified", true}};
		if (!sourceHash.empty()) stackJson["sourceIdentity"]["sourceHash"] = sourceHash;
	}
	stackJson["line"] = line;
	stackJson["level"] = level;
	{
		auto arr = nlohmann::json::array();
		for (auto idx: localVariables) {
			arr.push_back(idx->Serialize());
		}
		stackJson["localVariables"] = arr;
	}

	{
		auto arr = nlohmann::json::array();
		for (auto idx: upvalueVariables) {
			arr.push_back(idx->Serialize());
		}
		stackJson["upvalueVariables"] = arr;
	}

	if (!globalVariables.empty()) {
		auto arr = nlohmann::json::array();
		for (auto idx: globalVariables) {
			arr.push_back(idx->Serialize());
		}
		stackJson["globalVariables"] = arr;
	}

	return stackJson;
}

void Stack::Deserialize(nlohmann::json json) {
	if (json["file"].is_string()) file = json["file"].get<std::string>();
	if (json["functionName"].is_string()) functionName = json["functionName"].get<std::string>();
	if (json["frameId"].is_string()) frameId = json["frameId"].get<std::string>();
	if (json["line"].is_number_integer()) line = json["line"].get<int>();
	if (json["level"].is_number_integer()) level = json["level"].get<int>();
}

EvalContext::EvalContext() {
	result = _arena.Alloc();
}

nlohmann::json EvalContext::Serialize() {
	auto obj = nlohmann::json::object();
	obj["seq"] = seq;
	obj["success"] = success;
	if (!requestId.empty()) obj["requestId"] = requestId;
	if (vmId != 0) obj["vmId"] = vmId;
	if (pauseId != 0) obj["pauseId"] = pauseId;
	if (contextGeneration != 0) obj["contextGeneration"] = contextGeneration;
	if (sourceEpoch != 0) obj["sourceEpoch"] = sourceEpoch;
	if (connectionEpoch != 0) obj["connectionEpoch"] = connectionEpoch;
	if (!threadId.empty()) obj["threadId"] = threadId;
	if (!frameId.empty()) obj["frameId"] = frameId;
	if (!policy.empty()) obj["policy"] = policy;
	if (maxNodes != 100) obj["maxNodes"] = maxNodes;
	if (maxBytes != 64 * 1024) obj["maxBytes"] = maxBytes;

	if (success) {
		obj["value"] = result->Serialize();
	} else {
		obj["error"] = error;
	}
	return obj;
}

void EvalContext::Deserialize(nlohmann::json json) {
	if (json.count("seq") != 0 && json["seq"].is_number_integer()) {
		seq = json["seq"];
	}
	if (json.count("expr") != 0 && json["expr"].is_string()) {
		expr = json["expr"];
	}

	if (json.count("value") != 0 && json["value"].is_string()) {
		value = json["value"];
	}

	if (json.count("setValue") != 0 && json["setValue"].is_boolean()) {
		setValue = json["setValue"];
	}

	if (json.count("stackLevel") != 0 && json["stackLevel"].is_number_integer()) {
		stackLevel = json["stackLevel"];
	}
	if (json.count("depth") != 0 && json["depth"].is_number_integer()) {
		depth = json["depth"];
	}
	if (json.count("cacheId") != 0 && json["cacheId"].is_number_integer()) {
		cacheId = json["cacheId"];
	}
	if (json["requestId"].is_string()) {
		requestId = json["requestId"];
	}
	vmId = ParseVmId(json["vmId"]);
	if (json["pauseId"].is_number_unsigned() || json["pauseId"].is_number_integer()) {
		pauseId = json["pauseId"].get<uint64_t>();
	}
	if (json["contextGeneration"].is_number_unsigned() || json["contextGeneration"].is_number_integer()) {
		contextGeneration = json["contextGeneration"].get<uint64_t>();
	}
	if (json["sourceEpoch"].is_number_unsigned() || json["sourceEpoch"].is_number_integer()) {
		sourceEpoch = json["sourceEpoch"].get<uint64_t>();
	}
	if (json["connectionEpoch"].is_number_unsigned() || json["connectionEpoch"].is_number_integer()) {
		connectionEpoch = json["connectionEpoch"].get<uint64_t>();
	}
	if (json["threadId"].is_string()) {
		threadId = json["threadId"].get<std::string>();
	}
	if (json["frameId"].is_string()) {
		frameId = json["frameId"].get<std::string>();
	}
	if (json["policy"].is_string()) {
		policy = json["policy"].get<std::string>();
	}
	if (json["maxNodes"].is_number_integer()) {
		maxNodes = json["maxNodes"].get<int>();
	}
	if (json["maxBytes"].is_number_integer()) {
		maxBytes = json["maxBytes"].get<int>();
	}

}

nlohmann::json EvalParams::Serialize() {
	return JsonProtocol::Serialize();
}

void EvalParams::Deserialize(nlohmann::json json) {
	ctx = std::make_shared<EvalContext>();
	ctx->Deserialize(json);
}
