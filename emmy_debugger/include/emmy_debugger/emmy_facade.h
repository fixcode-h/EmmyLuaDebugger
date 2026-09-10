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
#pragma once

#include <mutex>
#include <condition_variable>
#include <map>
#include <deque>
#include <string>
#include "nlohmann/json_fwd.hpp"
#include "emmy_debugger/transporter/transporter.h"
#include "emmy_debugger/api/lua_api.h"
#include "emmy_debugger/debugger/emmy_debugger_manager.h"
#include "emmy_debugger/platform/lock.h"
#include "emmy_debugger/vm/host_vm_registry.h"
#include "emmy_debugger/proto/protocol_session.h"
#include "emmy_debugger/proto/protocol_v2.h"
#include "emmy_debugger/transporter/transport_auth.h"
#include "emmy_debugger/vm/host_value_provider.h"
#include "emmy_debugger/vm/source_registry.h"
#include "proto/proto_handler.h"

enum class LogType
{
	Debug,    // 调试信息（调试器内部使用）
	Info,     // 普通信息
	Warning,  // 警告
	Error     // 错误
};

enum class WorkMode
{
	EmmyCore,
	Attach,
};

class EmmyFacade
{
public:
	static EmmyFacade& Get();

	static void HookLua(lua_State* L, lua_Debug* ar);

	static void ReadyLuaHook(lua_State* L, lua_Debug* ar);

	EmmyFacade();
	~EmmyFacade();
#ifndef EMMY_USE_LUA_SOURCE
	bool SetupLuaAPI();
#endif

	bool TcpListen(lua_State* L, const std::string& host, int port, std::string& err);
	bool TcpSharedListen(lua_State* L, const std::string& host, int port, std::string& err);
	bool TcpConnect(lua_State* L, const std::string& host, int port, std::string& err);
	bool PipeListen(lua_State* L, const std::string& name, std::string& err);
	bool PipeConnect(lua_State* L, const std::string& name, std::string& err);
	int BreakHere(lua_State* L);
	bool RegisterTypeName(lua_State *L, const std::string &typeName, std::string &err);
	
	int OnConnect(bool suc);
	int OnDisconnect();
	void WaitIDE(bool force = false, int timeout = 0);
	bool OnBreak(std::shared_ptr<Debugger> debugger);
	void OnResume(uint64_t vmId, uint64_t pauseId, const std::string& threadId,
		uint64_t contextGeneration, uint64_t sourceEpoch);
	void Destroy();
	void OnEvalResult(std::shared_ptr<EvalContext> context);
	bool TryStartEvaluation(const std::shared_ptr<EvalContext>& context);
	void SendLog(LogType type, const char* fmt, ...);
	void OnLuaStateGC(lua_State* L);
	void Hook(lua_State* L, lua_Debug* ar);
	EmmyDebuggerManager& GetDebugManager();

	std::shared_ptr<Debugger> GetDebugger(lua_State* L);

	void SetReadyHook(lua_State* L);

	void StartDebug();

	// for hook
	bool StartupHookMode(int port);
	void Attach(lua_State* L);

	void SetWorkMode(WorkMode mode);
	WorkMode GetWorkMode();

	void InitReq(InitParams &params);
	bool AuthenticateInit(const std::string& token);
	void SetExpectedAuthToken(const std::string& token);
	bool IsAuthenticated() const;
	bool IsAuthenticationRequired() const;

	void ReadyReq();

	void OnReceiveMessage(nlohmann::json document);
	void OnV2Envelope(nlohmann::json document);
	void OnTransportProtocolError(const std::string& reason);
	// Monotonic sequence for pause/resume events, independent from VM lifecycle events.
	uint64_t NextDebugEventSeq();

	uint64_t RegisterLuaVm(lua_State* L, const VmMetadata& metadata);
	bool NotifyLuaVmReady(uint64_t registrationId);
	bool BeginLuaVmClose(uint64_t registrationId, const std::string& reason);
	bool EndLuaVmClose(uint64_t registrationId);
	bool ResetLuaVmContext(uint64_t registrationId, const std::string& reason);
	bool ReleaseLuaVmRegistration(uint64_t registrationId);
	bool SetLuaVmDisplayName(uint64_t registrationId, const std::string& displayName);
	bool ReconcileHostLuaVms();
	// Called on the Lua owner thread, after API loading and before Lua access.
	bool ValidateLuaVmAccess(lua_State* L);
	NativeVmRegistry& GetVmRegistry();
	HostVmRegistry& GetHostVmRegistry();
	HostValueProviderRegistry& GetHostValueProviderRegistry();
	SourceRegistry& GetSourceRegistry() { return _sourceRegistry; }
	bool RegisterLuaSource(uint64_t registrationId, HostSourceIdentity identity);

	// Start hook 作为成员存在
	std::function<void()> StartHook;
	// Attach hook teardown callback, installed by emmy_hook outside DllMain.
	std::function<void()> StopHook;

private:
	void OnVmLifecycleEvent(const VmLifecycleEvent& event);
	void SendV2Document(const nlohmann::json& document);
	void QueueV2Event(const VmLifecycleEvent& event);
	uint64_t BuildAndSendVmSnapshot(const std::string& requestId);
	void FlushPendingV2Events(uint64_t snapshotEventSeq);
	void SendInitResponse();
	void SendReadyResponse(uint64_t snapshotEventSeq);
	uint64_t RegisterFallbackLuaVm(lua_State* L, const std::string& discovery);
	bool BeginV2Request(const nlohmann::json& document,
						const std::string& requestId,
						std::string& operationHash);
	void CompleteV2Request(const std::string& requestId,
						  const std::string& operationHash,
						  const nlohmann::json& response);
	bool ReplayV2Request(const std::string& requestId,
						 const std::string& operationHash);

	// 使用平台相关的锁类型
	EmmyMutex waitIDEMutex = EMMY_MUTEX_INIT;
	EmmyCondVar waitIDECV = EMMY_CONDVAR_INIT;
	
	std::shared_ptr<Transporter> transporter;
	
	std::atomic<bool> isIDEReady;
	std::atomic<bool> isAPIReady;
	std::mutex _apiSetupMutex;
	std::atomic<bool> isWaitingForIDE;
	WorkMode workMode;

	// 表示使用了tcplisten tcpConnect 的states
	std::set<lua_State*> mainStates;

	std::atomic<bool> readyHook;

	ProtoHandler _protoHandler;

	EmmyDebuggerManager _emmyDebuggerManager;
	NativeVmRegistry _vmRegistry;
	HostVmRegistry _hostVmRegistry;
	HostValueProviderRegistry _hostValueProviderRegistry;
	SourceRegistry _sourceRegistry;
	std::mutex _sourceLifecycleMutex;
	ProtocolSession _protocolSession;
	TransportAuth _transportAuth;
	std::atomic<bool> _authenticated;
	std::atomic<uint64_t> _debugEventSeq;
	std::mutex _debugEventMutex;
	std::atomic<uint64_t> _breakpointRevision;

	struct PendingV2Event {
		VmLifecycleEvent event;
	};
	std::mutex _v2EventMutex;
	std::deque<PendingV2Event> _pendingV2Events;
};




