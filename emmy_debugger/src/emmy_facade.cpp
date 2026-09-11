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
#include "emmy_debugger/debugger/hook_dispatcher.h"
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

void SetEnvelopeTarget(nlohmann::json& envelope, uint64_t vmId, uint64_t pauseId,
	const std::string& threadId = std::string(), const std::string& frameId = std::string()) {
	envelope["target"] = nlohmann::json::object();
	if (vmId != 0) envelope["target"]["vmId"] = VmProtocolId(vmId);
	if (pauseId != 0) envelope["target"]["pauseId"] = pauseId;
	if (!threadId.empty()) envelope["target"]["threadId"] = threadId;
	if (!frameId.empty()) envelope["target"]["frameId"] = frameId;
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
	// Readiness belongs to the connection. Each VM/coroutine replaces its own
	// ReadyLuaHook below; the first VM must not consume readiness for the rest.

	auto states = FindAllCoroutine(L);

	for (auto state: states) {
		SetDebuggerHook(state, HookLua, LUA_MASKCALL | LUA_MASKLINE | LUA_MASKRET, 0);
	}

	SetDebuggerHook(L, HookLua, LUA_MASKCALL | LUA_MASKLINE | LUA_MASKRET, 0);

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
	  _debugEventSeq(0),
	  _breakpointRevision(0),
	  _protoHandler(this) {
	_vmRegistry.SetEventSink([this](const VmLifecycleEvent& event) {
		OnVmLifecycleEvent(event);
	});
}

uint64_t EmmyFacade::NextDebugEventSeq() {
	return _debugEventSeq.fetch_add(1, std::memory_order_relaxed) + 1;
}

EmmyFacade::~EmmyFacade() {
}

#ifndef EMMY_USE_LUA_SOURCE
extern "C" bool SetupLuaAPI();

bool EmmyFacade::SetupLuaAPI() {
	if (isAPIReady.load(std::memory_order_acquire)) return true;
	std::lock_guard<std::mutex> lock(_apiSetupMutex);
	if (isAPIReady.load(std::memory_order_acquire)) return true;
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
	std::atomic_store(&transporter, std::static_pointer_cast<Transporter>(s));
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
	if (std::atomic_load(&transporter) == nullptr) {
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
	std::atomic_store(&transporter, std::static_pointer_cast<Transporter>(c));
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
	std::atomic_store(&transporter, std::static_pointer_cast<Transporter>(p));
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
	std::atomic_store(&transporter, std::static_pointer_cast<Transporter>(p));
	// p->SetHandler(shared_from_this());
	const auto suc = p->Connect(name, err);
	if (suc) {
		WaitIDE(true);
	}
	return suc;
}

void EmmyFacade::WaitIDE(bool force, int timeout) {
	const auto transporter = std::atomic_load(&this->transporter);
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
			EMMY_COND_WAIT(waitIDECV, lock, [this] { return isIDEReady.load(); });
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
		_breakpointRevision.store(0, std::memory_order_release);
	} else {
		_transportAuth.ClearAuthenticatedEpoch();
		_authenticated.store(false, std::memory_order_release);
	}
	return 0;
}

int EmmyFacade::OnDisconnect() {
	readyHook = false;
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
		// Retain the idempotent callback so a quiescence timeout can be retried
		// during a later bootstrap instead of losing ownership of live hooks.
	}

	const auto previousTransport = std::atomic_exchange(&transporter, std::shared_ptr<Transporter>());
	if (previousTransport) {
		previousTransport->Stop();
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
	// Answer the handshake BEFORE publishing VM lifecycle events. A client can
	// only form a valid v2 request identity after InitRsp, so lifecycle events
	// emitted first make the client request a snapshot without an identity; the
	// agent then rejects it with MISSING_AGENT_SESSION_ID (payload null) and the
	// client latches "snapshot outstanding", which permanently fences the VM out
	// and discards every later debug.paused for it.
	SendInitResponse();
	ReconcileHostLuaVms();
}

bool EmmyFacade::AuthenticateInit(const std::string& token) {
	const auto transporter = std::atomic_load(&this->transporter);
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
	{
		std::lock_guard<std::mutex> lock(_v2EventMutex);
		const VmRegistrySnapshot snapshot = _vmRegistry.SnapshotWithEventSeq();
		SendReadyResponse(snapshot.eventSeq);
		SendV2Document(MakeVmSnapshotEnvelope(
			_protocolSession.AgentSessionId(), _protocolSession.ConnectionEpoch(),
			snapshot.records, snapshot.eventSeq));
		for (const auto& pending : _pendingV2Events) {
			if (pending.event.eventSeq > snapshot.eventSeq)
				SendV2Document(MakeVmLifecycleEnvelope(_protocolSession.AgentSessionId(),
					_protocolSession.ConnectionEpoch(), pending.event));
		}
		_pendingV2Events.clear();
		// Publish readiness only after the snapshot fence has entered the same
		// transport queue. Lua callbacks can now publish pause snapshots safely.
		_protocolSession.MarkReady();
	}
	isIDEReady = true;
	EMMY_COND_NOTIFY_ALL(waitIDECV);
}

uint64_t EmmyFacade::RegisterLuaVm(lua_State* L, const VmMetadata& metadata) {
	return _hostVmRegistry.RegisterBeforeAgent(L, metadata);
}

bool EmmyFacade::NotifyLuaVmReady(uint64_t registrationId) {
	return _hostVmRegistry.MarkReady(registrationId);
}

bool EmmyFacade::BeginLuaVmClose(uint64_t registrationId, const std::string& reason) {
	std::lock_guard<std::mutex> lock(_sourceLifecycleMutex);
	if (!_hostVmRegistry.BeginClose(registrationId, reason)) return false;
	// Wake a paused Lua owner and reject queued work. The host remains
	// responsible for joining that owner before actually invoking lua_close.
	auto debugger = _emmyDebuggerManager.GetDebuggerByVmId(registrationId);
	if (debugger) debugger->Stop();
	_emmyDebuggerManager.ClearHitDebugger(registrationId);
	return true;
}

bool EmmyFacade::EndLuaVmClose(uint64_t registrationId) {
	std::lock_guard<std::mutex> lock(_sourceLifecycleMutex);
	const auto record = _hostVmRegistry.Find(registrationId);
	const bool closed = _hostVmRegistry.EndClose(registrationId);
	if (closed) _sourceRegistry.Invalidate(registrationId);
	if (closed && record) ForgetDebuggerHooks(record->mainState);
	if (closed) _emmyDebuggerManager.RemoveDebuggerByVmId(registrationId);
	return closed;
}

bool EmmyFacade::ResetLuaVmContext(uint64_t registrationId, const std::string& reason) {
	std::lock_guard<std::mutex> lock(_sourceLifecycleMutex);
	// Invalidate debugger-owned references before the registry emits the reset
	// event. This ordering prevents an IDE/CLI consumer from observing the new
	// source epoch while the old pause snapshot is still locally addressable.
	auto debugger = _emmyDebuggerManager.GetDebuggerByVmId(registrationId);
	if (debugger) {
		debugger->ResetContext();
		_emmyDebuggerManager.ClearHitDebugger(registrationId);
	}
	const bool reset = _hostVmRegistry.ResetContext(registrationId, reason);
	if (reset) _sourceRegistry.Invalidate(registrationId);
	if (reset && debugger) {
		auto record = _vmRegistry.Find(registrationId);
		if (record) debugger->SetContextIdentity(record->contextGeneration, record->sourceEpoch);
	}
	return reset;
}

bool EmmyFacade::ReleaseLuaVmRegistration(uint64_t registrationId) {
	return _hostVmRegistry.Release(registrationId);
}

bool EmmyFacade::RegisterLuaSource(uint64_t registrationId, HostSourceIdentity identity) {
	std::lock_guard<std::mutex> lock(_sourceLifecycleMutex);
	const auto record = _hostVmRegistry.Find(registrationId);
	if (!record || record->sourceEpoch != identity.sourceEpoch ||
		record->state == VmLifecycleState::Closing || record->state == VmLifecycleState::Closed ||
		record->state == VmLifecycleState::Lost || record->state == VmLifecycleState::Error) return false;
	identity.vmId = record->id;
	return _sourceRegistry.Register(identity);
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
		auto record = _hostVmRegistry.FindByState(mainState);
		auto debugger = _emmyDebuggerManager.GetDebuggerByVmId(registrationId);
		if (record && debugger) {
			debugger->SetContextIdentity(record->contextGeneration, record->sourceEpoch);
		}
		NotifyLuaVmReady(registrationId);
	}
	return registrationId;
}

bool EmmyFacade::ReconcileHostLuaVms() {
	return _hostVmRegistry.ReconcileExistingVms(_vmRegistry);
}

bool EmmyFacade::ValidateLuaVmAccess(lua_State* L) {
	if (L == nullptr) return false;
	auto validate = [this](const std::shared_ptr<const VmRecord>& record) {
		if (!record) return true;
		if (!record->metadata.abiCompatible || record->state == VmLifecycleState::Error ||
			record->state == VmLifecycleState::Closing || record->state == VmLifecycleState::Closed ||
			record->state == VmLifecycleState::Lost) return false;
		if (!record->metadata.hasAbiDescriptor) return true;
		std::string error;
		if (ValidateLuaAbiDescriptorForPublicApi(record->metadata.abi, DetectLuaAbiDescriptor(), error)) return true;
		_hostVmRegistry.RejectAbi(record->id, error);
		return false;
	};
	// A directly registered VM must be checked before even resolving its main
	// thread through Lua's public registry API.
	if (!validate(_hostVmRegistry.FindByState(L))) return false;
	auto mainState = GetMainState(L);
	return mainState == nullptr || mainState == L || validate(_hostVmRegistry.FindByState(mainState));
}

NativeVmRegistry& EmmyFacade::GetVmRegistry() {
	return _vmRegistry;
}

HostVmRegistry& EmmyFacade::GetHostVmRegistry() {
	return _hostVmRegistry;
}

HostValueProviderRegistry& EmmyFacade::GetHostValueProviderRegistry() {
	return _hostValueProviderRegistry;
}

void EmmyFacade::OnReceiveMessage(nlohmann::json document) {
	// Authentication is an admission check. Frames other than InitReq do not
	// enter ProtoHandler until the current transport epoch is authenticated.
	if (_transportAuth.IsRequired() && !_authenticated.load(std::memory_order_acquire)) {
		const bool isInit = document["cmd"].is_number_integer() &&
			document["cmd"].get<int>() == static_cast<int>(MessageCMD::InitReq);
		if (!isInit) {
			if (document["cmd"].is_number_integer() &&
				document["cmd"].get<int>() == static_cast<int>(MessageCMD::EnvelopeV2)) {
				nlohmann::json error = nlohmann::json::object();
				error["code"] = "NOT_AUTHORIZED";
				error["message"] = "Emmy Agent authentication is required before requests";
				error["retryable"] = false;
				const std::string type = document["type"].is_string()
					? document["type"].get<std::string>() : "unknown";
				const std::string requestId = document["requestId"].is_string()
					? document["requestId"].get<std::string>() : std::string();
				SendV2Document(MakeV2Envelope("response", type,
					_protocolSession.AgentSessionId(), _protocolSession.ConnectionEpoch(),
					requestId, 0, nlohmann::json(), false, error));
			}
			return;
		}
	}
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
	const std::string incomingSession = document["agentSessionId"].is_string()
		? document["agentSessionId"].get<std::string>() : std::string();
	if (kind == "request") {
		std::string identityError;
		if (!ValidateV2RequestIdentity(document, _protocolSession.AgentSessionId(),
			_protocolSession.ConnectionEpoch(), identityError)) {
			nlohmann::json error = nlohmann::json::object();
			error["code"] = identityError.empty() ? "INVALID_V2_IDENTITY" : identityError;
			error["message"] = "v2 request identity is invalid";
			error["retryable"] = identityError == "STALE_AGENT_SESSION" ||
				identityError == "STALE_CONNECTION_EPOCH";
			const nlohmann::json response = MakeV2Envelope(
				"response", type.empty() ? "unknown" : type,
				_protocolSession.AgentSessionId(), _protocolSession.ConnectionEpoch(),
				requestId, 0, nlohmann::json(), false, error);
			SendV2Document(response);
			return;
		}
	}
	if (incomingSession.empty() || incomingSession != _protocolSession.AgentSessionId()) {
		nlohmann::json error = nlohmann::json::object();
		error["code"] = "STALE_AGENT_SESSION";
		error["message"] = "request belongs to a different Agent session";
		error["retryable"] = true;
		SendV2Document(MakeV2Envelope("response", type.empty() ? "unknown" : type,
			_protocolSession.AgentSessionId(), _protocolSession.ConnectionEpoch(), requestId,
			0, nlohmann::json(), false, error));
		return;
	}
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
	V2DebugTarget debugTarget;
	if (kind == "request" && (type == "debug.action" || type == "debug.eval")) {
		std::string errorCode;
		if (!ParseV2DebugTarget(document, type == "debug.eval", debugTarget, errorCode)) {
			const nlohmann::json error = {{"code", errorCode},
				{"message", "VM, pause or context identity is invalid"}, {"retryable", false}};
			const auto response = MakeV2Envelope("response", type, _protocolSession.AgentSessionId(),
				incomingEpoch, requestId, 0, nlohmann::json(), false, error);
			CompleteV2Request(requestId, operationHash, response);
			SendV2Document(response);
			return;
		}
	}

	if (kind == "request" && type == "request.cancel") {
		const auto& payload = document["payload"];
		const std::string cancelledId = payload.is_object() && payload.contains("requestId") && payload["requestId"].is_string()
			? payload["requestId"].get<std::string>() : std::string();
		const auto cancelled = cancelledId != requestId
			? _protocolSession.CancelRequest(cancelledId, incomingEpoch)
			: ProtocolSession::CancelDisposition::NotFound;
		const bool ok = cancelled == ProtocolSession::CancelDisposition::Cancelled;
		nlohmann::json error;
		if (!ok) error = {{"code", cancelled == ProtocolSession::CancelDisposition::Unsupported
			? "CANCEL_UNSUPPORTED" : "REQUEST_NOT_FOUND"},
			{"message", "only queued evaluation can be cancelled; running Lua is never interrupted"}, {"retryable", false}};
		const auto response = MakeV2Envelope("response", type, _protocolSession.AgentSessionId(), incomingEpoch,
			requestId, 0, {{"cancelled", ok}}, ok, error);
		CompleteV2Request(requestId, operationHash, response);
		SendV2Document(response);
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

	if (kind == "request" && type == "debug.breakpoints.replace") {
		const nlohmann::json& payload = document["payload"];
		const uint64_t revision = payload.is_object() && payload.contains("revision") && payload["revision"].is_number_unsigned()
			? payload["revision"].get<uint64_t>() : 0;
		if (revision == 0 || !payload.is_object() || !payload.contains("breakpoints") ||
			!payload["breakpoints"].is_array() || payload["breakpoints"].size() > 4096) {
			nlohmann::json error = nlohmann::json::object();
			error["code"] = "INVALID_BREAKPOINT_SNAPSHOT";
			error["message"] = "revision and a bounded breakpoints array are required";
			error["retryable"] = false;
			const nlohmann::json response = MakeV2Envelope(
				"response", type, _protocolSession.AgentSessionId(),
				_protocolSession.ConnectionEpoch(), requestId, 0, nlohmann::json(), false, error);
			CompleteV2Request(requestId, operationHash, response);
			SendV2Document(response);
			return;
		}
		const uint64_t currentRevision = _breakpointRevision.load(std::memory_order_acquire);
		if (revision < currentRevision) {
			nlohmann::json error = nlohmann::json::object();
			error["code"] = "STALE_BREAKPOINT_REVISION";
			error["message"] = "breakpoint snapshot revision is older than the active revision";
			error["retryable"] = true;
			const nlohmann::json response = MakeV2Envelope(
				"response", type, _protocolSession.AgentSessionId(),
				_protocolSession.ConnectionEpoch(), requestId, 0, nlohmann::json(), false, error);
			CompleteV2Request(requestId, operationHash, response);
			SendV2Document(response);
			return;
		}

		std::vector<std::shared_ptr<BreakPoint>> snapshot;
		for (nlohmann::json::const_iterator it = payload["breakpoints"].begin();
				it != payload["breakpoints"].end(); ++it) {
			if (!it->is_object()) continue;
			std::shared_ptr<BreakPoint> breakpoint(new BreakPoint());
			breakpoint->Deserialize(*it);
			snapshot.push_back(breakpoint);
		}
		_emmyDebuggerManager.ReplaceBreakpoints(snapshot);
		_breakpointRevision.store(revision, std::memory_order_release);
		nlohmann::json acceptedPayload = nlohmann::json::object();
		acceptedPayload["accepted"] = true;
		acceptedPayload["revision"] = revision;
		acceptedPayload["count"] = snapshot.size();
		const nlohmann::json response = MakeV2Envelope(
			"response", type, _protocolSession.AgentSessionId(),
			_protocolSession.ConnectionEpoch(), requestId, 0, acceptedPayload);
		CompleteV2Request(requestId, operationHash, response);
		SendV2Document(response);
		return;
	}

	if (kind == "request" && type == "debug.action") {
		const uint64_t vmId = debugTarget.vmId;
		const uint64_t pauseId = debugTarget.pauseId;
		const std::string& threadId = debugTarget.threadId;
		const int actionValue = document["payload"]["action"].is_number_integer()
			? document["payload"]["action"].get<int>() : -1;
		const bool validAction = actionValue >= static_cast<int>(DebugAction::Break) &&
			actionValue <= static_cast<int>(DebugAction::Stop);
		const EmmyDebuggerManager::RouteResult route = validAction
			? _emmyDebuggerManager.RouteAction(vmId, static_cast<DebugAction>(actionValue), pauseId, threadId,
				debugTarget.contextGeneration, debugTarget.sourceEpoch)
			: EmmyDebuggerManager::RouteResult{false, "INVALID_ACTION"};
		const bool accepted = route.ok;
		if (accepted) {
			// This response acknowledges queue admission. Only the Lua owner
			// publishes RUNNING/debug.resumed after applying the action.
			nlohmann::json payload = nlohmann::json::object();
			payload["accepted"] = true;
			payload["vmId"] = VmProtocolId(vmId);
			if (pauseId != 0) payload["pauseId"] = pauseId;
			nlohmann::json response = MakeV2Envelope(
				"response", "debug.action", _protocolSession.AgentSessionId(),
				_protocolSession.ConnectionEpoch(), requestId, 0, payload);
			SetEnvelopeTarget(response, vmId, pauseId, threadId);
			CompleteV2Request(requestId, operationHash, response);
			SendV2Document(response);
		} else {
			nlohmann::json error = nlohmann::json::object();
			error["code"] = route.errorCode == nullptr ? "ACTION_REJECTED" : route.errorCode;
			error["message"] = "The requested VM action was rejected";
			error["retryable"] = false;
			nlohmann::json response = MakeV2Envelope(
				"response", "debug.action", _protocolSession.AgentSessionId(),
				_protocolSession.ConnectionEpoch(), requestId, 0, nlohmann::json(), false, error);
			SetEnvelopeTarget(response, vmId, pauseId, threadId);
			CompleteV2Request(requestId, operationHash, response);
			SendV2Document(response);
		}
		return;
	}

	if (kind == "request" && type == "debug.eval") {
		const uint64_t vmId = debugTarget.vmId;
		const uint64_t pauseId = debugTarget.pauseId;
		const nlohmann::json& payload = document["payload"];
		RestrictedEvalLimits evalLimits;
		std::string evalValidationError;
		if (!ValidateRestrictedEvalPayload(payload, evalValidationError, &evalLimits)) {
			nlohmann::json error = nlohmann::json::object();
			error["code"] = evalValidationError.empty() ? "EVALUATION_DENIED" : evalValidationError;
			error["message"] = "restricted VALUE_PATH evaluation was rejected";
			error["retryable"] = false;
			nlohmann::json response = MakeV2Envelope(
				"response", "debug.eval", _protocolSession.AgentSessionId(),
				_protocolSession.ConnectionEpoch(), requestId, 0, nlohmann::json(), false, error);
			SetEnvelopeTarget(response, vmId, pauseId,
				document["target"]["threadId"].is_string()
					? document["target"]["threadId"].get<std::string>() : std::string(),
				document["target"]["frameId"].is_string()
					? document["target"]["frameId"].get<std::string>() : std::string());
			CompleteV2Request(requestId, operationHash, response);
			SendV2Document(response);
			return;
		}
		std::shared_ptr<EvalContext> context(new EvalContext());
		context->requestId = requestId;
		context->operationHash = operationHash;
		context->vmId = vmId;
		context->pauseId = pauseId;
		context->connectionEpoch = incomingEpoch;
		context->contextGeneration = debugTarget.contextGeneration;
		context->sourceEpoch = debugTarget.sourceEpoch;
		if (document["target"]["threadId"].is_string()) {
			context->threadId = document["target"]["threadId"].get<std::string>();
		}
		if (document["target"]["frameId"].is_string()) {
			context->frameId = document["target"]["frameId"].get<std::string>();
		}
		context->policy = "VALUE_PATH";
		if (payload.contains("sourceIdentity") && payload["sourceIdentity"].is_object()) {
			const auto& source = payload["sourceIdentity"];
			if (source.contains("canonicalPath") && source["canonicalPath"].is_string()) context->sourceCanonicalPath = source["canonicalPath"].get<std::string>();
			if (source.contains("sourceHash") && source["sourceHash"].is_string()) context->sourceHash = source["sourceHash"].get<std::string>();
		}
		context->depth = evalLimits.maxDepth;
		context->maxNodes = evalLimits.maxNodes;
		context->maxBytes = evalLimits.maxBytes;
		if (payload["expr"].is_string()) context->expr = payload["expr"].get<std::string>();
		if (payload.contains("stackLevel") && payload["stackLevel"].is_number_integer()) context->stackLevel = payload["stackLevel"].get<int>();
		// Bounds and defaults come exclusively from the payload validator.

		const EmmyDebuggerManager::RouteResult route = _emmyDebuggerManager.RouteEval(vmId, context);
		const bool accepted = route.ok;
		if (!accepted) {
			nlohmann::json error = nlohmann::json::object();
			error["code"] = route.errorCode == nullptr ? "EVAL_REJECTED" : route.errorCode;
			error["message"] = "The requested evaluation target is not active";
			error["retryable"] = false;
			nlohmann::json response = MakeV2Envelope(
				"response", "debug.eval", _protocolSession.AgentSessionId(),
				_protocolSession.ConnectionEpoch(), requestId, 0, nlohmann::json(), false, error);
			SetEnvelopeTarget(response, vmId, pauseId, context->threadId, context->frameId);
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
			"vm.lifecycle", "vm.snapshot", "debug.paused", "debug.resumed",
			"pause.thread-only", "debug.breakpoints.replace", "debug.legacy-v1"
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
	const nlohmann::json response = MakeV2Envelope(
		"response", type.empty() ? "unknown" : type, _protocolSession.AgentSessionId(),
		_protocolSession.ConnectionEpoch(), requestId, 0, nlohmann::json(), false, error);
	CompleteV2Request(requestId, operationHash, response);
	SendV2Document(response);
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
	operationHash = CanonicalV2Json(document);
	uint64_t timeoutMillis = 5000;
	if (document.contains("deadlineMillis")) {
		if (!document["deadlineMillis"].is_number_unsigned() ||
			(timeoutMillis = document["deadlineMillis"].get<uint64_t>()) == 0 || timeoutMillis > 30000) {
			const nlohmann::json error = {{"code", "INVALID_DEADLINE"},
				{"message", "deadlineMillis must be a relative duration between 1 and 30000"}, {"retryable", false}};
			SendV2Document(MakeV2Envelope("response", "request", _protocolSession.AgentSessionId(),
				_protocolSession.ConnectionEpoch(), requestId, 0, nlohmann::json(), false, error));
			return false;
		}
	}
	const ProtocolSession::RequestDisposition disposition = _protocolSession.BeginRequest(
		requestId, operationHash, document["connectionEpoch"].is_number_unsigned()
			? document["connectionEpoch"].get<uint64_t>() : 0, timeoutMillis);
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
		? "REQUEST_ID_REUSE" : disposition == ProtocolSession::RequestDisposition::Busy
		? "RATE_LIMITED" : "STALE_CONNECTION_EPOCH";
	error["message"] = disposition == ProtocolSession::RequestDisposition::Conflict
		? "requestId was reused for a different operation" : "request is not valid for this connection";
	error["retryable"] = disposition == ProtocolSession::RequestDisposition::StaleEpoch ||
		disposition == ProtocolSession::RequestDisposition::Busy;
	SendV2Document(MakeV2Envelope("response", document["type"].is_string()
		? document["type"].get<std::string>() : "request",
		_protocolSession.AgentSessionId(), _protocolSession.ConnectionEpoch(), requestId,
		0, nlohmann::json(), false, error));
	return false;
}

void EmmyFacade::CompleteV2Request(const std::string& requestId,
	const std::string& operationHash,
	const nlohmann::json& response) {
	const uint64_t responseEpoch = response["connectionEpoch"].is_number_unsigned()
		? response["connectionEpoch"].get<uint64_t>() : 0;
	if (!_protocolSession.AcceptIncomingEpoch(responseEpoch, false)) return;
	_protocolSession.CompleteRequest(requestId, operationHash, response.dump(),
		responseEpoch);
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
	const auto transporter = std::atomic_load(&this->transporter);
	std::lock_guard<std::mutex> lock(_v2EventMutex);
	const nlohmann::json document = MakeVmLifecycleEnvelope(
		_protocolSession.AgentSessionId(), _protocolSession.ConnectionEpoch(), event);
	if (_protocolSession.IsReady() && transporter != nullptr && transporter->IsConnected()) {
		SendV2Document(document);
	} else {
		if (_pendingV2Events.size() >= 256) _pendingV2Events.pop_front();
		_pendingV2Events.push_back(PendingV2Event{event});
	}
}

void EmmyFacade::SendV2Document(const nlohmann::json& document) {
	const auto transporter = std::atomic_load(&this->transporter);
	if (document["cmd"].is_number_integer() &&
		document["cmd"].get<int>() == static_cast<int>(MessageCMD::EnvelopeV2)) {
		const std::string session = document["agentSessionId"].is_string()
			? document["agentSessionId"].get<std::string>() : std::string();
		const uint64_t epoch = document["connectionEpoch"].is_number_unsigned()
			? document["connectionEpoch"].get<uint64_t>() : 0;
		if (session != _protocolSession.AgentSessionId() ||
			!_protocolSession.AcceptIncomingEpoch(epoch, false)) return;
	}
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
	const auto transporter = std::atomic_load(&this->transporter);
	nlohmann::json response = nlohmann::json::object();
	response["cmd"] = static_cast<int>(MessageCMD::InitRsp);
	response["version"] = "2";
	response["protocolVersion"] = 2;
	response["agentSessionId"] = _protocolSession.AgentSessionId();
	response["connectionEpoch"] = _protocolSession.ConnectionEpoch();
	response["processId"] = CurrentProcessIdValue();
	response["capabilities"] = nlohmann::json::array({
		"vm.lifecycle", "vm.snapshot", "debug.paused", "debug.resumed",
		"pause.thread-only", "debug.breakpoints.replace", "debug.legacy-v1"
	});
	if (transporter != nullptr) {
		transporter->Send(static_cast<int>(MessageCMD::InitRsp), response);
	}
}

void EmmyFacade::SendReadyResponse(uint64_t snapshotEventSeq) {
	const auto transporter = std::atomic_load(&this->transporter);
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
	std::lock_guard<std::mutex> eventLock(_debugEventMutex);
	const auto transporter = std::atomic_load(&this->transporter);
	if (!debugger || !transporter || !transporter->IsConnected() || !_protocolSession.IsReady()) {
		return false;
	}
	std::vector<Stack> stacks;
	bool stacksTruncated = false;

	if (!debugger->GetStacks(stacks, 128, &stacksTruncated, true)) {
		SendLog(LogType::Warning, "暂停时无法读取 Lua 栈（VM=%llu）",
			static_cast<unsigned long long>(debugger->GetVmId()));
		return false;
	}
	_emmyDebuggerManager.SetHitDebugger(debugger);
	if (debugger->GetVmId() != 0) {
		// Publish PAUSED only after the snapshot is complete. A failed capture
		// must leave the VM controllable in its previous state.
		auto record = _vmRegistry.Find(debugger->GetVmId());
		if (record && (record->state == VmLifecycleState::Running ||
				record->state == VmLifecycleState::Ready)) {
			_vmRegistry.SetState(debugger->GetVmId(), VmLifecycleState::Paused, "breakpoint");
		}
	}

	auto obj = nlohmann::json::object();
	obj["cmd"] = static_cast<int>(MessageCMD::BreakNotify);
	obj["stacks"] = JsonProtocol::SerializeArray(stacks);
	if (stacksTruncated) obj["stacksTruncated"] = true;
	if (debugger->GetVmId() != 0) {
		obj["vmId"] = VmProtocolId(debugger->GetVmId());
	}
	if (debugger->GetPauseId() != 0) {
		obj["pauseId"] = debugger->GetPauseId();
	}
	obj["threadId"] = debugger->GetPauseThreadId();
	obj["pauseScope"] = debugger->GetPauseScope() == PauseScope::Thread ? "THREAD" : "VM";
	obj["consistency"] = debugger->GetPauseConsistency();
	obj["pauseReason"] = debugger->GetPauseReason();
	obj["reasons"] = debugger->GetPauseReasons();

	transporter->Send(int(MessageCMD::BreakNotify), obj);

	nlohmann::json payload = nlohmann::json::object();
	payload["pauseId"] = debugger->GetPauseId();
	payload["threadId"] = debugger->GetPauseThreadId();
	payload["pauseScope"] = debugger->GetPauseScope() == PauseScope::Thread ? "THREAD" : "VM";
	payload["consistency"] = debugger->GetPauseConsistency();
	payload["reason"] = debugger->GetPauseReason();
	payload["reasons"] = debugger->GetPauseReasons();
	payload["stacks"] = JsonProtocol::SerializeArray(stacks);
	nlohmann::json paused = MakeV2Envelope(
		"event", "debug.paused", _protocolSession.AgentSessionId(),
		_protocolSession.ConnectionEpoch(), std::string(), NextDebugEventSeq(), payload);
	paused["contextGeneration"] = debugger->GetContextGeneration();
	paused["sourceEpoch"] = debugger->GetSourceEpoch();
	paused["target"] = nlohmann::json::object();
	if (debugger->GetVmId() != 0) paused["target"]["vmId"] = VmProtocolId(debugger->GetVmId());
	paused["target"]["threadId"] = debugger->GetPauseThreadId();
	paused["target"]["pauseId"] = debugger->GetPauseId();
	SendV2Document(paused);

	return true;
}

void EmmyFacade::OnResume(uint64_t vmId, uint64_t pauseId, const std::string& threadId,
	uint64_t contextGeneration, uint64_t sourceEpoch) {
	if (pauseId == 0 || !_protocolSession.IsReady()) return;
	std::lock_guard<std::mutex> eventLock(_debugEventMutex);
	const auto record = _vmRegistry.Find(vmId);
	if (!record || record->contextGeneration != contextGeneration || record->sourceEpoch != sourceEpoch ||
		record->state != VmLifecycleState::Paused) return;
	_vmRegistry.SetState(vmId, VmLifecycleState::Running, "debug-action");
	auto resumed = MakeV2Envelope("event", "debug.resumed", _protocolSession.AgentSessionId(),
		_protocolSession.ConnectionEpoch(), std::string(), NextDebugEventSeq(), {{"pauseId", pauseId}});
	SetEnvelopeTarget(resumed, vmId, pauseId, threadId);
	resumed["contextGeneration"] = contextGeneration;
	resumed["sourceEpoch"] = sourceEpoch;
	SendV2Document(resumed);
}

bool EmmyFacade::TryStartEvaluation(const std::shared_ptr<EvalContext>& context) {
	if (!context) return false;
	if (context->requestId.empty()) return true;
	return _protocolSession.TryStartRequest(context->requestId, context->connectionEpoch, context->error);
}

void EmmyFacade::OnEvalResult(std::shared_ptr<EvalContext> context) {
	const auto transporter = std::atomic_load(&this->transporter);
	if (transporter) {
		if (context && !context->requestId.empty()) {
			if (!_protocolSession.AcceptIncomingEpoch(context->connectionEpoch, false)) return;
			const auto requestError = _protocolSession.RequestError(context->requestId, context->connectionEpoch);
			if (!requestError.empty()) { context->success = false; context->error = requestError; }
			nlohmann::json payload = context->Serialize();
			nlohmann::json error;
			if (!context->success) error = {{"code", context->error.empty() ? "EVALUATION_DENIED" : context->error},
				{"message", "restricted evaluation failed"}, {"retryable", false}};
			nlohmann::json envelope = MakeV2Envelope(
				"response", "debug.eval", _protocolSession.AgentSessionId(),
				context->connectionEpoch, context->requestId, 0, payload,
				context->success, error);
			SetEnvelopeTarget(envelope, context->vmId, context->pauseId,
				context->threadId, context->frameId);
			CompleteV2Request(context->requestId, context->operationHash, envelope);
			SendV2Document(envelope);
		} else if (context) {
			transporter->Send(int(MessageCMD::EvalRsp), context->Serialize());
		}
	}
}

void EmmyFacade::SendLog(LogType type, const char *fmt, ...) {
	const auto transporter = std::atomic_load(&this->transporter);
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
	ClearDebuggerHook(L);
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
	if (workMode == WorkMode::Attach && !_emmyDebuggerManager.IsRunning()) {
		ClearDebuggerHook(L);
		return;
	}
	auto debugger = GetDebugger(L);
	if (debugger) {
		if (!debugger->IsRunning()) {
			if (GetWorkMode() == WorkMode::Attach) ClearDebuggerHook(L);
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
			if (!install_emmy_debugger(L)) return;
			debugger = _emmyDebuggerManager.AddDebugger(L);

			RegisterFallbackLuaVm(L, "HOOK_FALLBACK");

			if (_emmyDebuggerManager.IsRunning()) {
				debugger->Start();
				debugger->Attach();
			}
			// send attached notify
			auto obj = nlohmann::json::object();
			obj["state"] = reinterpret_cast<int64_t>(L);

			const auto transporter = std::atomic_load(&this->transporter);
			if (transporter) {
				transporter->Send(int(MessageCMD::AttachedNotify), obj);
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
	SetDebuggerHook(L, ReadyLuaHook, LUA_MASKCALL | LUA_MASKLINE | LUA_MASKRET, 0);
}

void EmmyFacade::StartDebug() {
	_emmyDebuggerManager.SetRunning(true);
	readyHook = true;
}

bool EmmyFacade::StartupHookMode(int port) {
	// 只有在已经有 transporter 时才需要清理
	// 首次调用时不需要 Destroy()，避免不必要的 mutex 操作
	if (std::atomic_load(&transporter)) {
		// A re-attach replaces only this session and its transport. Tearing the
		// hooks down here is what broke every attach after the first: EasyHook
		// can refuse to uninstall a handle whose trampoline is still in use,
		// HookManager keeps the failed handle and then rejects every later
		// Enable, so the agent stays connected with no Lua hook at all. Keeping
		// the hooks is also what lets the next InitReq reuse them through
		// FindAndHook()'s already-enabled fast path.
		OnDisconnect();
		const auto previousTransport = std::atomic_exchange(
			&transporter, std::shared_ptr<Transporter>());
		if (previousTransport) {
			previousTransport->Stop();
		}
	}

	// 1024 - 65535
	while (port > 0xffff) port -= 0xffff;
	while (port < 0x400) port += 0x400;

	const auto s = std::make_shared<SocketServerTransporter>();
	std::string err;
	// Bind IPv4 loopback explicitly. "localhost" resolves to ::1 first on many
	// Windows hosts, and a firewall rule cannot express an IPv6 loopback address
	// condition, so a ::1-only listener can never be covered by the
	// "127.0.0.1 -> 127.0.0.1" inbound exemption that local debugging relies on.
	const auto suc = s->Listen("127.0.0.1", port, err);
	if (suc) {
		std::atomic_store(&transporter, std::static_pointer_cast<Transporter>(s));
		// transporter->SetHandler(shared_from_this());
	}
	return suc;
}

void EmmyFacade::Attach(lua_State *L) {
	const auto transporter = std::atomic_load(&this->transporter);
	if (!transporter || !transporter->IsConnected())
		return;

	// 这里存在一个问题就是 hook 的时机太早了，globalstate 都还没初始化完毕

#ifndef EMMY_USE_LUA_SOURCE
	if (!SetupLuaAPI()) return;
#endif
	if (!ValidateLuaVmAccess(L)) return;
	SetDebuggerHook(L, EmmyFacade::HookLua, LUA_MASKCALL | LUA_MASKLINE | LUA_MASKRET, 0);
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
