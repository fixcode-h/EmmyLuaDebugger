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

#include "emmy_debugger/emmy_facade.h"
#include <cstdarg>
#include <cstdint>
#include "nlohmann/json.hpp"
#include "emmy_debugger/transporter/socket_server_transporter.h"
#include "emmy_debugger/transporter/socket_client_transporter.h"
#include "emmy_debugger/transporter/pipeline_server_transporter.h"
#include "emmy_debugger/transporter/pipeline_client_transporter.h"
#include "emmy_debugger/debugger/emmy_debugger.h"
#include "emmy_debugger/debugger/emmy_debugger_lib.h"
#include "emmy_debugger/transporter/transporter.h"
#include "emmy_debugger/api/lua_version.h"
#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace {

uint64_t CurrentProcessIdValue() {
#ifdef _WIN32
	return static_cast<uint64_t>(GetCurrentProcessId());
#else
	return static_cast<uint64_t>(getpid());
#endif
}

} // namespace

EmmyFacade &EmmyFacade::Get() {
	static EmmyFacade instance;
	return instance;
}

void EmmyFacade::HookLua(lua_State *L, lua_Debug *ar) {
	Get().Hook(L, ar);
}

void EmmyFacade::ReadyLuaHook(lua_State *L, lua_Debug *ar) {
	if (!Get().readyHook) {
		return;
	}
	Get().readyHook = false;

	auto states = FindAllCoroutine(L);

	for (auto state: states) {
		lua_sethook(state, HookLua, LUA_MASKCALL | LUA_MASKLINE | LUA_MASKRET, 0);
	}

	lua_sethook(L, HookLua, LUA_MASKCALL | LUA_MASKLINE | LUA_MASKRET, 0);

	auto debugger = Get().GetDebugger(L);
	if (debugger) {
		debugger->Attach();
	}

	Get().Hook(L, ar);
}

EmmyFacade::EmmyFacade()
	: transporter(nullptr),
	  isIDEReady(false),
	  isAPIReady(false),
	  StartHook(nullptr),
	  isWaitingForIDE(false),
	  workMode(WorkMode::EmmyCore),
	  readyHook(false),
	  StopHook(nullptr),
	  _authenticated(false),
	  _protoHandler(this) {
	_vmRegistry.SetEventSink([this](const VmLifecycleEvent& event) {
		OnVmLifecycleEvent(event);
	});
}

EmmyFacade::~EmmyFacade() {
}

#ifndef EMMY_USE_LUA_SOURCE
extern "C" bool SetupLuaAPI();

bool EmmyFacade::SetupLuaAPI() {
	isAPIReady = ::SetupLuaAPI();
	return isAPIReady;
}

#endif

int LuaError(lua_State *L) {
	std::string msg = lua_tostring(L, 1);
	msg = "[Emmy]" + msg;
	lua_getglobal(L, "error");
	lua_pushstring(L, msg.c_str());
	lua_call(L, 1, 0);
	return 0;
}

bool EmmyFacade::TcpListen(lua_State *L, const std::string &host, int port, std::string &err) {
	Destroy();

	_emmyDebuggerManager.AddDebugger(L);

	SetReadyHook(L);
	RegisterFallbackLuaVm(L, "EMMY_CORE");

	const auto s = std::make_shared<SocketServerTransporter>();
	transporter = s;
	// s->SetHandler(shared_from_this());
	const auto suc = s->Listen(host, port, err);
	if (!suc) {
		lua_pushcfunction(L, LuaError);
		lua_pushstring(L, err.c_str());
		lua_call(L, 1, 0);
	}
	return suc;
}

bool EmmyFacade::TcpSharedListen(lua_State *L, const std::string &host, int port, std::string &err) {
	if (transporter == nullptr) {
		return TcpListen(L, host, port, err);
	}
	if (_emmyDebuggerManager.GetDebugger(L) == nullptr) {
		_emmyDebuggerManager.AddDebugger(L);
		SetReadyHook(L);
		RegisterFallbackLuaVm(L, "EMMY_CORE");
	}
	return true;
}

bool EmmyFacade::TcpConnect(lua_State *L, const std::string &host, int port, std::string &err) {
	Destroy();

	_emmyDebuggerManager.AddDebugger(L);

	SetReadyHook(L);
	RegisterFallbackLuaVm(L, "EMMY_CORE");

	const auto c = std::make_shared<SocketClientTransporter>();
	transporter = c;
	// c->SetHandler(shared_from_this());
	const auto suc = c->Connect(host, port, err);
	if (suc) {
		WaitIDE(true);
	} else {
		lua_pushcfunction(L, LuaError);
		lua_pushstring(L, err.c_str());
		lua_call(L, 1, 0);
	}
	return suc;
}

bool EmmyFacade::PipeListen(lua_State *L, const std::string &name, std::string &err) {
	Destroy();

	_emmyDebuggerManager.AddDebugger(L);

	SetReadyHook(L);
	RegisterFallbackLuaVm(L, "EMMY_CORE");

	const auto p = std::make_shared<PipelineServerTransporter>();
	transporter = p;
	// p->SetHandler(shared_from_this());
	const auto suc = p->pipe(name, err);
	return suc;
}

bool EmmyFacade::PipeConnect(lua_State *L, const std::string &name, std::string &err) {
	Destroy();

	_emmyDebuggerManager.AddDebugger(L);

	SetReadyHook(L);
	RegisterFallbackLuaVm(L, "EMMY_CORE");

	const auto p = std::make_shared<PipelineClientTransporter>();
	transporter = p;
	// p->SetHandler(shared_from_this());
	const auto suc = p->Connect(name, err);
	if (suc) {
		WaitIDE(true);
	}
	return suc;
}

void EmmyFacade::WaitIDE(bool force, int timeout) {
	if (transporter != nullptr
	    && (transporter->IsServerMode() || force)
	    && !isWaitingForIDE
	    && !isIDEReady) {
		isWaitingForIDE = true;
		SRWUniqueLock lock(waitIDEMutex);
		if (timeout > 0) {
#ifdef _WIN32
			SleepConditionVariableSRW(&waitIDECV, lock.mutex(), timeout, 0);
#else
			waitIDECV.wait_for(lock, std::chrono::milliseconds(timeout));
#endif
		} else {
			EMMY_COND_WAIT(waitIDECV, lock, [this] { return isIDEReady; });
		}
		isWaitingForIDE = false;
	}
}

int EmmyFacade::BreakHere(lua_State *L) {
	if (!isIDEReady)
		return 0;

	_emmyDebuggerManager.HandleBreak(L);

	return 1;
}

int EmmyFacade::OnConnect(bool suc) {
	_protocolSession.OnConnect(suc);
	if (suc) {
		_transportAuth.BeginEpoch(_protocolSession.ConnectionEpoch());
		_authenticated.store(false, std::memory_order_release);
	} else {
		_transportAuth.ClearAuthenticatedEpoch();
		_authenticated.store(false, std::memory_order_release);
	}
	return 0;
}

int EmmyFacade::OnDisconnect() {
	isIDEReady = false;
	isWaitingForIDE = false;
	_protocolSession.OnDisconnect();
	_authenticated.store(false, std::memory_order_release);

	_emmyDebuggerManager.OnDisconnect();

	_emmyDebuggerManager.RemoveAllBreakpoints();

	if (workMode == WorkMode::Attach) {
		_emmyDebuggerManager.RemoveAllDebugger();
	}

	return 0;
}

void EmmyFacade::Destroy() {
	OnDisconnect();
	if (StopHook) {
		StopHook();
		StopHook = nullptr;
	}

	if (transporter) {
		transporter->Stop();
		transporter = nullptr;
	}
}

void EmmyFacade::SetWorkMode(WorkMode mode) {
	workMode = mode;
}

WorkMode EmmyFacade::GetWorkMode() {
	return workMode;
}


void EmmyFacade::InitReq(InitParams & params) {
	const bool alreadyNegotiated = _protocolSession.IsNegotiated();
	_protocolSession.MarkNegotiated();
	if (!alreadyNegotiated && StartHook) {
		StartHook();
	}
	if (alreadyNegotiated) {
		SendInitResponse();
		return;
	}

	_emmyDebuggerManager.emmyHelperPath = params.emmyHelperPath;
	_emmyDebuggerManager.customHelperPath = params.customHelperPath;
	_emmyDebuggerManager.emmyHelperName = params.emmyHelperName;
	_emmyDebuggerManager.emmyHelperExtName = params.emmyHelperExtName;
	_emmyDebuggerManager.extNames.clear();
	_emmyDebuggerManager.extNames = params.ext;

	// 这里有个线程安全问题，消息线程和lua 执行线程不是相同线程，但是没有一个锁能让我做同步
	// 所以我不能在这里访问lua state 指针的内部结构
	//
	// 方案：提前为主state 设置hook 利用hook 实现同步

	// fix 以上安全问题
	StartDebug();
	ReconcileHostLuaVms();
	SendInitResponse();
}

bool EmmyFacade::AuthenticateInit(const std::string& token) {
	if (!_transportAuth.VerifyForEpoch(token, _protocolSession.ConnectionEpoch())) {
		nlohmann::json error = nlohmann::json::object();
		error["code"] = "NOT_AUTHORIZED";
		error["message"] = "Emmy Agent authentication failed";
		error["retryable"] = false;
		SendV2Document(MakeV2Envelope(
			"error", "agent.auth", _protocolSession.AgentSessionId(),
			_protocolSession.ConnectionEpoch(), std::string(), 0, nlohmann::json(), false, error));
		if (transporter != nullptr) {
			transporter->Stop();
		}
		OnDisconnect();
		return false;
	}
	_authenticated.store(true, std::memory_order_release);
	return true;
}

void EmmyFacade::SetExpectedAuthToken(const std::string& token) {
	_transportAuth.SetExpectedToken(token);
	_authenticated.store(false, std::memory_order_release);
}

bool EmmyFacade::IsAuthenticated() const {
	return _authenticated.load(std::memory_order_acquire);
}

bool EmmyFacade::IsAuthenticationRequired() const {
	return _transportAuth.IsRequired();
}

void EmmyFacade::ReadyReq() {
	_protocolSession.MarkReady();
	isIDEReady = true;
	EMMY_COND_NOTIFY_ALL(waitIDECV);
	const VmRegistrySnapshot snapshot = _vmRegistry.SnapshotWithEventSeq();
	SendReadyResponse(snapshot.eventSeq);
	SendV2Document(MakeVmSnapshotEnvelope(
		_protocolSession.AgentSessionId(), _protocolSession.ConnectionEpoch(),
		snapshot.records, snapshot.eventSeq));
	FlushPendingV2Events(snapshot.eventSeq);
}

uint64_t EmmyFacade::RegisterLuaVm(lua_State* L, const VmMetadata& metadata) {
	return _hostVmRegistry.RegisterBeforeAgent(L, metadata);
}

bool EmmyFacade::NotifyLuaVmReady(uint64_t registrationId) {
	return _hostVmRegistry.MarkReady(registrationId);
}

bool EmmyFacade::BeginLuaVmClose(uint64_t registrationId, const std::string& reason) {
	return _hostVmRegistry.BeginClose(registrationId, reason);
}

bool EmmyFacade::EndLuaVmClose(uint64_t registrationId) {
	return _hostVmRegistry.EndClose(registrationId);
}

bool EmmyFacade::ReleaseLuaVmRegistration(uint64_t registrationId) {
	return _hostVmRegistry.Release(registrationId);
}

bool EmmyFacade::SetLuaVmDisplayName(uint64_t registrationId, const std::string& displayName) {
	return _hostVmRegistry.SetDisplayName(registrationId, displayName);
}

uint64_t EmmyFacade::RegisterFallbackLuaVm(lua_State* L, const std::string& discovery) {
	if (L == nullptr) {
		return 0;
	}
	VmMetadata metadata;
	metadata.displayName = "Lua VM";
	metadata.discovery = discovery;
	switch (luaVersion) {
		case LuaVersion::LUA_JIT: metadata.luaVersionHint = "LuaJIT"; break;
		case LuaVersion::LUA_51: metadata.luaVersionHint = "5.1"; break;
		case LuaVersion::LUA_52: metadata.luaVersionHint = "5.2"; break;
		case LuaVersion::LUA_53: metadata.luaVersionHint = "5.3"; break;
		case LuaVersion::LUA_54: metadata.luaVersionHint = "5.4"; break;
		default: metadata.luaVersionHint = "unknown"; break;
	}
	lua_State* mainState = GetMainState(L);
	if (mainState == nullptr) mainState = L;
	const uint64_t registrationId = RegisterLuaVm(mainState, metadata);
	if (registrationId != 0) {
		_emmyDebuggerManager.BindVmId(mainState, registrationId);
		NotifyLuaVmReady(registrationId);
	}
	return registrationId;
}

bool EmmyFacade::ReconcileHostLuaVms() {
	return _hostVmRegistry.ReconcileExistingVms(_vmRegistry);
}

NativeVmRegistry& EmmyFacade::GetVmRegistry() {
	return _vmRegistry;
}

HostVmRegistry& EmmyFacade::GetHostVmRegistry() {
	return _hostVmRegistry;
}

void EmmyFacade::OnReceiveMessage(nlohmann::json document) {
	_protoHandler.OnDispatch(document);
}

void EmmyFacade::OnTransportProtocolError(const std::string& reason) {
	nlohmann::json error = nlohmann::json::object();
	error["code"] = "PROTOCOL_ERROR";
	error["message"] = reason;
	error["retryable"] = false;
	SendV2Document(MakeV2Envelope(
		"error", "transport.error", _protocolSession.AgentSessionId(),
		_protocolSession.ConnectionEpoch(), std::string(), 0, nlohmann::json(), false, error));
}

void EmmyFacade::OnV2Envelope(nlohmann::json document) {
	if (!document["protocolVersion"].is_number_integer() ||
		document["protocolVersion"].get<int>() != 2) {
		return;
	}
	const std::string type = document["type"].is_string()
		? document["type"].get<std::string>() : std::string();
	const std::string kind = document["kind"].is_string()
		? document["kind"].get<std::string>() : std::string();
	const std::string requestId = document["requestId"].is_string()
		? document["requestId"].get<std::string>() : std::string();
	const uint64_t incomingEpoch = document["connectionEpoch"].is_number_unsigned()
		? document["connectionEpoch"].get<uint64_t>() : 0;
	if (_transportAuth.IsRequired() && !_authenticated.load(std::memory_order_acquire)) {
		nlohmann::json error = nlohmann::json::object();
		error["code"] = "NOT_AUTHORIZED";
		error["message"] = "Emmy Agent authentication is required before requests";
		error["retryable"] = false;
		SendV2Document(MakeV2Envelope("response", type.empty() ? "unknown" : type,
			_protocolSession.AgentSessionId(), _protocolSession.ConnectionEpoch(), requestId,
			0, nlohmann::json(), false, error));
		return;
	}
	if (!_protocolSession.AcceptIncomingEpoch(incomingEpoch, false)) {
		nlohmann::json error = nlohmann::json::object();
		error["code"] = "STALE_CONNECTION_EPOCH";
		error["message"] = "request belongs to an old connection epoch";
		error["retryable"] = true;
		SendV2Document(MakeV2Envelope("response", type.empty() ? "unknown" : type,
			_protocolSession.AgentSessionId(), _protocolSession.ConnectionEpoch(), requestId,
			0, nlohmann::json(), false, error));
		return;
	}
	std::string operationHash;
	if (kind == "request" && !BeginV2Request(document, requestId, operationHash)) {
		return;
	}

	if (kind == "request" && type == "vm.snapshot") {
		const VmRegistrySnapshot snapshot = _vmRegistry.SnapshotWithEventSeq();
		const nlohmann::json response = MakeVmSnapshotEnvelope(
			_protocolSession.AgentSessionId(), _protocolSession.ConnectionEpoch(),
			snapshot.records, snapshot.eventSeq, requestId);
		CompleteV2Request(requestId, operationHash, response);
		SendV2Document(response);
		return;
	}

	if (kind == "request" && type == "debug.action") {
		const uint64_t vmId = ParseVmProtocolId(document["target"]["vmId"]);
		const uint64_t pauseId = document["target"]["pauseId"].is_number_unsigned()
			? document["target"]["pauseId"].get<uint64_t>() : 0;
		const int actionValue = document["payload"]["action"].is_number_integer()
			? document["payload"]["action"].get<int>() : -1;
		const bool validAction = actionValue >= static_cast<int>(DebugAction::Break) &&
			actionValue <= static_cast<int>(DebugAction::Stop);
		const bool accepted = validAction && vmId != 0 &&
			_emmyDebuggerManager.DoActionForVm(vmId, static_cast<DebugAction>(actionValue), pauseId);
		if (accepted) {
			nlohmann::json payload = nlohmann::json::object();
			payload["accepted"] = true;
			payload["vmId"] = VmProtocolId(vmId);
			if (pauseId != 0) payload["pauseId"] = pauseId;
			const nlohmann::json response = MakeV2Envelope(
				"response", "debug.action", _protocolSession.AgentSessionId(),
				_protocolSession.ConnectionEpoch(), requestId, 0, payload);
			CompleteV2Request(requestId, operationHash, response);
			SendV2Document(response);
		} else {
			nlohmann::json error = nlohmann::json::object();
			error["code"] = vmId == 0 || !_emmyDebuggerManager.GetDebuggerByVmId(vmId)
				? "VM_NOT_FOUND" : "STALE_PAUSE_REFERENCE";
			error["message"] = "The requested VM action was rejected";
			error["retryable"] = false;
			const nlohmann::json response = MakeV2Envelope(
				"response", "debug.action", _protocolSession.AgentSessionId(),
				_protocolSession.ConnectionEpoch(), requestId, 0, nlohmann::json(), false, error);
			CompleteV2Request(requestId, operationHash, response);
			SendV2Document(response);
		}
		return;
	}

	if (kind == "request" && type == "debug.eval") {
		const uint64_t vmId = ParseVmProtocolId(document["target"]["vmId"]);
		const uint64_t pauseId = document["target"]["pauseId"].is_number_unsigned()
			? document["target"]["pauseId"].get<uint64_t>() : 0;
		std::shared_ptr<EvalContext> context(new EvalContext());
		context->requestId = requestId;
		context->operationHash = operationHash;
		context->vmId = vmId;
		context->pauseId = pauseId;
		const nlohmann::json& payload = document["payload"];
		if (payload["expr"].is_string()) context->expr = payload["expr"].get<std::string>();
		if (payload["stackLevel"].is_number_integer()) context->stackLevel = payload["stackLevel"].get<int>();
		if (payload["depth"].is_number_integer()) context->depth = payload["depth"].get<int>();
		if (payload["cacheId"].is_number_integer()) context->cacheId = payload["cacheId"].get<int>();

		const bool accepted = vmId != 0 && pauseId != 0 && _emmyDebuggerManager.EvalForVm(vmId, context);
		if (!accepted) {
			nlohmann::json error = nlohmann::json::object();
			error["code"] = vmId == 0 || !_emmyDebuggerManager.GetDebuggerByVmId(vmId)
				? "VM_NOT_FOUND" : "STALE_PAUSE_REFERENCE";
			error["message"] = "The requested evaluation target is not active";
			error["retryable"] = false;
			const nlohmann::json response = MakeV2Envelope(
				"response", "debug.eval", _protocolSession.AgentSessionId(),
				_protocolSession.ConnectionEpoch(), requestId, 0, nlohmann::json(), false, error);
			CompleteV2Request(requestId, operationHash, response);
			SendV2Document(response);
		}
		return;
	}

	if (kind == "request" && type == "agent.describe") {
		nlohmann::json payload = nlohmann::json::object();
		payload["agentSessionId"] = _protocolSession.AgentSessionId();
		payload["protocolVersion"] = 2;
		payload["processId"] = CurrentProcessIdValue();
		payload["capabilities"] = nlohmann::json::array({
			"vm.lifecycle", "vm.snapshot", "debug.legacy-v1"
		});
		const nlohmann::json response = MakeV2Envelope(
			"response", "agent.describe", _protocolSession.AgentSessionId(),
			_protocolSession.ConnectionEpoch(), requestId, 0, payload);
		CompleteV2Request(requestId, operationHash, response);
		SendV2Document(response);
		return;
	}
	if (kind != "request") {
		return;
	}

	nlohmann::json error = nlohmann::json::object();
	error["code"] = "CAPABILITY_UNSUPPORTED";
	error["message"] = "Unsupported Emmy v2 request";
	error["retryable"] = false;
	SendV2Document(MakeV2Envelope(
		"response", type.empty() ? "unknown" : type, _protocolSession.AgentSessionId(),
		_protocolSession.ConnectionEpoch(), requestId, 0, nlohmann::json(), false, error));
}

bool EmmyFacade::BeginV2Request(const nlohmann::json& document,
	const std::string& requestId,
	std::string& operationHash) {
	if (requestId.empty() || requestId.size() > 128) {
		nlohmann::json error = nlohmann::json::object();
		error["code"] = "INVALID_REQUEST_ID";
		error["message"] = "requestId must be non-empty and at most 128 bytes";
		error["retryable"] = false;
		SendV2Document(MakeV2Envelope("response", "request", _protocolSession.AgentSessionId(),
			_protocolSession.ConnectionEpoch(), requestId, 0, nlohmann::json(), false, error));
		return false;
	}
	operationHash = document.dump();
	const ProtocolSession::RequestDisposition disposition = _protocolSession.BeginRequest(
		requestId, operationHash, document["connectionEpoch"].is_number_unsigned()
			? document["connectionEpoch"].get<uint64_t>() : 0);
	if (disposition == ProtocolSession::RequestDisposition::New) {
		return true;
	}
	if (disposition == ProtocolSession::RequestDisposition::Duplicate) {
		if (ReplayV2Request(requestId, operationHash)) {
			return false;
		}
		nlohmann::json error = nlohmann::json::object();
		error["code"] = "REQUEST_IN_PROGRESS";
		error["message"] = "requestId is already being processed";
		error["retryable"] = true;
		SendV2Document(MakeV2Envelope("response", document["type"].is_string()
			? document["type"].get<std::string>() : "request",
			_protocolSession.AgentSessionId(), _protocolSession.ConnectionEpoch(), requestId,
			0, nlohmann::json(), false, error));
		return false;
	}
	nlohmann::json error = nlohmann::json::object();
	error["code"] = disposition == ProtocolSession::RequestDisposition::Conflict
		? "REQUEST_ID_REUSE" : "STALE_CONNECTION_EPOCH";
	error["message"] = disposition == ProtocolSession::RequestDisposition::Conflict
		? "requestId was reused for a different operation" : "request is not valid for this connection";
	error["retryable"] = disposition == ProtocolSession::RequestDisposition::StaleEpoch;
	SendV2Document(MakeV2Envelope("response", document["type"].is_string()
		? document["type"].get<std::string>() : "request",
		_protocolSession.AgentSessionId(), _protocolSession.ConnectionEpoch(), requestId,
		0, nlohmann::json(), false, error));
	return false;
}

void EmmyFacade::CompleteV2Request(const std::string& requestId,
	const std::string& operationHash,
	const nlohmann::json& response) {
	_protocolSession.CompleteRequest(requestId, operationHash, response.dump(),
		_protocolSession.ConnectionEpoch());
}

bool EmmyFacade::ReplayV2Request(const std::string& requestId,
	const std::string& operationHash) {
	std::string response;
	if (!_protocolSession.CachedResponse(requestId, operationHash, response)) {
		return false;
	}
	try {
		SendV2Document(nlohmann::json::parse(response));
		return true;
	} catch (...) {
		return false;
	}
}

void EmmyFacade::OnVmLifecycleEvent(const VmLifecycleEvent& event) {
	const nlohmann::json document = MakeVmLifecycleEnvelope(
		_protocolSession.AgentSessionId(), _protocolSession.ConnectionEpoch(), event);
	if (_protocolSession.IsReady() && transporter != nullptr && transporter->IsConnected()) {
		SendV2Document(document);
	} else {
		QueueV2Event(event);
	}
}

void EmmyFacade::SendV2Document(const nlohmann::json& document) {
	if (transporter != nullptr && transporter->IsConnected()) {
		transporter->Send(static_cast<int>(MessageCMD::EnvelopeV2), document);
	}
}

void EmmyFacade::QueueV2Event(const VmLifecycleEvent& event) {
	std::lock_guard<std::mutex> lock(_v2EventMutex);
	if (_pendingV2Events.size() >= 256) {
		_pendingV2Events.pop_front();
	}
	_pendingV2Events.push_back(PendingV2Event{event});
}

uint64_t EmmyFacade::BuildAndSendVmSnapshot(const std::string& requestId) {
	const VmRegistrySnapshot snapshot = _vmRegistry.SnapshotWithEventSeq();
	SendV2Document(MakeVmSnapshotEnvelope(
		_protocolSession.AgentSessionId(), _protocolSession.ConnectionEpoch(),
		snapshot.records, snapshot.eventSeq, requestId));
	return snapshot.eventSeq;
}

void EmmyFacade::FlushPendingV2Events(uint64_t snapshotEventSeq) {
	std::deque<PendingV2Event> pending;
	{
		std::lock_guard<std::mutex> lock(_v2EventMutex);
		pending.swap(_pendingV2Events);
	}
	for (std::deque<PendingV2Event>::const_iterator it = pending.begin(); it != pending.end(); ++it) {
		if (it->event.eventSeq <= snapshotEventSeq) {
			continue;
		}
		SendV2Document(MakeVmLifecycleEnvelope(
			_protocolSession.AgentSessionId(), _protocolSession.ConnectionEpoch(), it->event));
	}
}

void EmmyFacade::SendInitResponse() {
	nlohmann::json response = nlohmann::json::object();
	response["cmd"] = static_cast<int>(MessageCMD::InitRsp);
	response["version"] = "2";
	response["protocolVersion"] = 2;
	response["agentSessionId"] = _protocolSession.AgentSessionId();
	response["connectionEpoch"] = _protocolSession.ConnectionEpoch();
	response["processId"] = CurrentProcessIdValue();
	response["capabilities"] = nlohmann::json::array({
		"vm.lifecycle", "vm.snapshot", "debug.legacy-v1"
	});
	if (transporter != nullptr) {
		transporter->Send(static_cast<int>(MessageCMD::InitRsp), response);
	}
}

void EmmyFacade::SendReadyResponse(uint64_t snapshotEventSeq) {
	nlohmann::json response = nlohmann::json::object();
	response["cmd"] = static_cast<int>(MessageCMD::ReadyRsp);
	response["protocolVersion"] = 2;
	response["agentSessionId"] = _protocolSession.AgentSessionId();
	response["connectionEpoch"] = _protocolSession.ConnectionEpoch();
	response["snapshotEventSeq"] = snapshotEventSeq;
	if (transporter != nullptr) {
		transporter->Send(static_cast<int>(MessageCMD::ReadyRsp), response);
	}
}

bool EmmyFacade::OnBreak(std::shared_ptr<Debugger> debugger) {
	if (!debugger || !transporter || !transporter->IsConnected()) {
		return false;
	}
	std::vector<Stack> stacks;

	_emmyDebuggerManager.SetHitDebugger(debugger);

	debugger->GetStacks(stacks);

	auto obj = nlohmann::json::object();
	obj["cmd"] = static_cast<int>(MessageCMD::BreakNotify);
	obj["stacks"] = JsonProtocol::SerializeArray(stacks);
	if (debugger->GetVmId() != 0) {
		obj["vmId"] = VmProtocolId(debugger->GetVmId());
	}
	if (debugger->GetPauseId() != 0) {
		obj["pauseId"] = debugger->GetPauseId();
	}

	transporter->Send(int(MessageCMD::BreakNotify), obj);

	return true;
}

void EmmyFacade::OnEvalResult(std::shared_ptr<EvalContext> context) {
	if (transporter) {
		if (context && !context->requestId.empty()) {
			nlohmann::json payload = context->Serialize();
			nlohmann::json envelope = MakeV2Envelope(
				"response", "debug.eval", _protocolSession.AgentSessionId(),
				_protocolSession.ConnectionEpoch(), context->requestId, 0, payload,
				context->success);
			envelope["target"] = nlohmann::json::object();
			if (context->vmId != 0) envelope["target"]["vmId"] = VmProtocolId(context->vmId);
			if (context->pauseId != 0) envelope["target"]["pauseId"] = context->pauseId;
			CompleteV2Request(context->requestId, context->operationHash, envelope);
			SendV2Document(envelope);
		} else {
			transporter->Send(int(MessageCMD::EvalRsp), context->Serialize());
		}
	}
}

void EmmyFacade::SendLog(LogType type, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	char buff[1024] = {0};
	vsnprintf(buff, 1024, fmt, args);
	va_end(args);

	const std::string msg = buff;

	auto obj = nlohmann::json::object();
	obj["type"] = type;
	obj["message"] = msg;

	if (transporter) {
		transporter->Send(int(MessageCMD::LogNotify), obj);
	}
}

void EmmyFacade::OnLuaStateGC(lua_State *L) {
	auto vmRecord = _hostVmRegistry.FindByState(L);
	if (vmRecord) {
		BeginLuaVmClose(vmRecord->id, "lua-state-gc");
		EndLuaVmClose(vmRecord->id);
		ReleaseLuaVmRegistration(vmRecord->id);
	}

	auto debugger = _emmyDebuggerManager.RemoveDebugger(L);

	if (debugger) {
		debugger->Detach();
	}

	if (workMode == WorkMode::EmmyCore) {
		if (_emmyDebuggerManager.IsDebuggerEmpty()) {
			Destroy();
		}
	}
}

void EmmyFacade::Hook(lua_State *L, lua_Debug *ar) {
	auto debugger = GetDebugger(L);
	if (debugger) {
		if (!debugger->IsRunning()) {
			if (GetWorkMode() == WorkMode::EmmyCore) {
				if (luaVersion != LuaVersion::LUA_JIT) {
					if (debugger->IsMainCoroutine(L)) {
						SetReadyHook(L);
					}
				} else {
					SetReadyHook(L);
				}
			}
			return;
		}

		debugger->Hook(ar, L);
	} else {
		if (workMode == WorkMode::Attach) {
			debugger = _emmyDebuggerManager.AddDebugger(L);
			install_emmy_debugger(L);

			RegisterFallbackLuaVm(L, "HOOK_FALLBACK");

			if (_emmyDebuggerManager.IsRunning()) {
				debugger->Start();
				debugger->Attach();
			}
			// send attached notify
			auto obj = nlohmann::json::object();
			obj["state"] = reinterpret_cast<int64_t>(L);

			if (this->transporter) {
				this->transporter->Send(int(MessageCMD::AttachedNotify), obj);
			}

			debugger->Hook(ar, L);
		}
	}
}

EmmyDebuggerManager &EmmyFacade::GetDebugManager() {
	return _emmyDebuggerManager;
}

std::shared_ptr<Debugger> EmmyFacade::GetDebugger(lua_State *L) {
	return _emmyDebuggerManager.GetDebugger(L);
}

void EmmyFacade::SetReadyHook(lua_State *L) {
	lua_sethook(L, ReadyLuaHook, LUA_MASKCALL | LUA_MASKLINE | LUA_MASKRET, 0);
}

void EmmyFacade::StartDebug() {
	_emmyDebuggerManager.SetRunning(true);
	readyHook = true;
}

bool EmmyFacade::StartupHookMode(int port) {
	// 只有在已经有 transporter 时才需要清理
	// 首次调用时不需要 Destroy()，避免不必要的 mutex 操作
	if (transporter) {
		Destroy();
	}

	// 1024 - 65535
	while (port > 0xffff) port -= 0xffff;
	while (port < 0x400) port += 0x400;

	const auto s = std::make_shared<SocketServerTransporter>();
	std::string err;
	const auto suc = s->Listen("localhost", port, err);
	if (suc) {
		transporter = s;
		// transporter->SetHandler(shared_from_this());
	}
	return suc;
}

void EmmyFacade::Attach(lua_State *L) {
	if (!this->transporter || !this->transporter->IsConnected())
		return;

	// 这里存在一个问题就是 hook 的时机太早了，globalstate 都还没初始化完毕

	if (!isAPIReady) {
		// 考虑到emmy_hook use lua source
		isAPIReady = install_emmy_debugger(L);
	}

	lua_sethook(L, EmmyFacade::HookLua, LUA_MASKCALL | LUA_MASKLINE | LUA_MASKRET, 0);
}

bool EmmyFacade::RegisterTypeName(lua_State *L, const std::string &typeName, std::string &err) {
	auto debugger = GetDebugger(L);
	if (!debugger) {
		err = "Debugger does not exist";
		return false;
	}
	const auto suc = debugger->RegisterTypeName(typeName, err);
	return suc;
}
