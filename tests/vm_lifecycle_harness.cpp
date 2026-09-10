#include "emmy_debugger/emmy_facade.h"
#include "emmy_debugger/debugger/emmy_debugger.h"
#include "emmy_debugger/debugger/hook_state.h"
#include "emmy_debugger/debugger/emmy_debugger_lib.h"
#include <cstdlib>
#include <iostream>
#include <thread>
#include <cstring>

namespace {
std::shared_ptr<Debugger> activeDebugger;
unsigned captures = 0;
const std::string sourceHash(64, 'a'); // Host-supplied identity fixture.

void Require(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "lifecycle harness: " << message << std::endl;
		std::exit(1);
	}
}

void OtherCoroutineStep(lua_State* state, lua_Debug* ar) {
	if (ar->event == LUA_HOOKLINE) {
		std::static_pointer_cast<HookState>(activeDebugger->GetStateStepIn())->ProcessHook(activeDebugger, state, ar);
	}
}

void Capture(lua_State* state, lua_Debug* ar) {
	if (ar->event != LUA_HOOKLINE || ar->currentline != 3) return;
	const auto debugger = activeDebugger;
	Require(debugger->TryBeginPause(state, {"PROBE:runtime"}), "pause must be claimed");
	const uint64_t pause = debugger->GetPauseId();
	auto eval = std::make_shared<EvalContext>();
	eval->requestId = "runtime-eval";
	eval->vmId = debugger->GetVmId();
	eval->pauseId = pause;
	eval->threadId = debugger->GetPauseThreadId();
	eval->frameId = debugger->GetPauseFrameId(0);
	eval->contextGeneration = debugger->GetContextGeneration();
	eval->sourceEpoch = debugger->GetSourceEpoch();
	eval->stackLevel = 0;
	eval->policy = "VALUE_PATH";
	eval->expr = "value.answer";
	eval->depth = 0;
	const int top = lua_gettop(state);
	std::vector<Stack> rawStacks;
	bool stacksTruncated = false;
	debugger->GetStacks(rawStacks, 128, &stacksTruncated, true);
	Require(!rawStacks.empty() && lua_gettop(state) == top, "raw stack snapshot preserves Lua stack");
	Require(rawStacks.front().sourceHash == sourceHash, "loaded source identity is propagated in stack snapshots");
	eval->sourceCanonicalPath = rawStacks.front().file;
	eval->sourceHash = sourceHash;
	const bool evaluated = debugger->Eval(eval, true);
	if (!evaluated) std::cerr << "restricted eval error: " << eval->error << std::endl;
	Require(evaluated, "restricted evaluation succeeds on Lua owner thread");
	Require(eval->result->value == "42", "local table value is captured");
	Require(lua_gettop(state) == top, "evaluation preserves Lua stack");
	eval->sourceHash = std::string(64, 'b');
	Require(!debugger->Eval(eval, true) && eval->error == "SOURCE_IDENTITY_MISMATCH", "mismatched host source hash is rejected on Lua owner");
	eval->sourceHash = sourceHash;
	eval->expr = "marker";
	Require(debugger->Eval(eval, true) && eval->result->value == "23", "global fallback uses the frame's custom _ENV");
	eval->expr = "string";
	Require(!debugger->Eval(eval, true) && eval->error == "VALUE_NOT_FOUND", "custom _ENV never leaks unrelated _G values");
	eval->expr = "shadow";
	Require(debugger->Eval(eval, true) && eval->result->value == "7", "inner local shadows outer local");
	eval->expr = "value";
	eval->depth = 2;
	Require(debugger->Eval(eval, true), "numeric table keys can be enumerated without mutating lua_next key");
	Require(eval->result->children.size() == 4, "table result retains numeric and string keys");
	eval->expr = "value.binary";
	Require(debugger->Eval(eval, true) && eval->result->value == std::string("a\0b", 3),
		"Lua string embedded NUL is preserved");
	eval->expr = "huge";
	eval->maxBytes = 16;
	Require(debugger->Eval(eval, true) && eval->result->value.size() <= 16 && eval->result->truncated,
		"large string is copied within byte budget");
	eval->maxBytes = 64 * 1024;
	eval->expr = "value.missing";
	debugger->Eval(eval, true);
	lua_getglobal(state, "metamethodCalls");
	Require(lua_tointeger(state, -1) == 0, "VALUE_PATH does not invoke __index");
	lua_pop(state, 1);
	eval->contextGeneration++;
	Require(!debugger->Eval(eval, true) && eval->error == "STALE_PAUSE_REFERENCE", "stale context fails on owner thread");
	// The message thread only queues the action. Lua is still inside this hook;
	// EnterDebugMode must consume an action received before it starts waiting.
	const DebugAction actions[] = {DebugAction::Continue, DebugAction::StepIn, DebugAction::StepOver, DebugAction::StepOut};
	const auto queued = std::make_shared<EvalContext>();
	queued->policy = "VALUE_PATH";
	queued->expr = "value.answer";
	Require(debugger->Eval(queued), "pending evaluation queues while paused");
	std::thread controller([&] {
		Require(debugger->RequestAction(actions[captures], pause), "early action is admitted");
		Require(!debugger->RequestAction(DebugAction::StepIn, pause), "only one action may consume a pause");
	});
	controller.join();
	debugger->EnterDebugMode();
	Require(debugger->GetPauseId() == 0, "continue invalidates pause");
	Require(!queued->success && queued->error == "STALE_PAUSE_REFERENCE", "resume drains queued evaluation");
	if (actions[captures] == DebugAction::StepIn) {
		lua_State* other = lua_newthread(state);
		lua_sethook(other, OtherCoroutineStep, LUA_MASKLINE, 0);
		Require(luaL_loadstring(other, "local other=1\nother=other+1") == 0 &&
			lua_pcall(other, 0, 0, 0) == 0, "another coroutine can emit line hooks during a step");
		lua_sethook(other, nullptr, 0, 0);
		lua_pop(state, 1);
		Require(debugger->TryBeginPause(state) && debugger->GetPauseId() == pause + 1,
			"step-in never creates a pause on another coroutine");
		debugger->ClearPause();
		debugger->ExitDebugMode();
	}
	++captures;
}

void Run(lua_State* state, const std::shared_ptr<Debugger>& debugger) {
	activeDebugger = debugger;
	lua_sethook(state, Capture, LUA_MASKLINE, 0);
	const char* script =
		"metamethodCalls = 0\n"
		"local value = setmetatable({11,22,answer=42,binary='a\\0b'}, {__index=function() metamethodCalls=metamethodCalls+1; return 99 end,__tostring=function() metamethodCalls=metamethodCalls+1; return 'forbidden' end}); local huge=string.rep('x',4096); local _ENV={marker=23}; local shadow=0; do local shadow=7\n"
		"result = value.answer\n"
		"end\n";
	EmmyLuaSourceIdentity source{};
	source.size = sizeof(source);
	source.version = EMMY_HOST_API_VERSION;
	source.sourceEpoch = debugger->GetSourceEpoch();
	source.chunkName = "@C:/harness/runtime.lua";
	source.canonicalPath = "C:/harness/runtime.lua";
	source.sha256 = sourceHash.c_str();
	Require(Emmy_RegisterLuaSource(debugger->GetVmId(), &source) != 0, "Host registers loaded source identity");
	Require(luaL_loadbuffer(state, script, std::strlen(script), source.chunkName) == 0, "runtime script compiles");
	Require(lua_pcall(state, 0, 0, 0) == 0, "runtime script completes after resume");
	lua_sethook(state, nullptr, 0, 0);
}
}

int main() {
	auto& facade = EmmyFacade::Get();
	lua_State* first = luaL_newstate();
	lua_State* second = luaL_newstate();
	Require(first && second, "two Lua VMs start");
	lua_State* rejected = luaL_newstate();
	EmmyLuaAbiDescriptor badAbi{};
	badAbi.size = sizeof(badAbi);
	badAbi.version = EMMY_HOST_API_VERSION;
	badAbi.major = 5;
	badAbi.minor = 3;
	EmmyHostVmMetadata badMetadata{};
	badMetadata.size = sizeof(badMetadata);
	badMetadata.version = EMMY_HOST_API_VERSION;
	badMetadata.abi = &badAbi;
	const auto rejectedId = Emmy_RegisterLuaVm(rejected, &badMetadata);
	Require(rejectedId != 0, "registration does not require a loaded Lua API");
	Require(!facade.ValidateLuaVmAccess(rejected), "actual incompatible Lua API rejects access");
	Require(!Emmy_NotifyLuaVmReady(rejectedId), "ABI rejection cannot become ready");
	Require(facade.GetHostVmRegistry().FindByState(rejected)->state == VmLifecycleState::Error,
		"ABI failure is visible before reconciliation");
	Require(Emmy_BeginLuaVmClose(rejectedId, "incompatible") != 0, "rejected VM remains closeable");
	lua_close(rejected);
	Require(Emmy_EndLuaVmClose(rejectedId) && Emmy_ReleaseLuaVmRegistration(rejectedId), "rejected VM releases safely");
	luaL_openlibs(first);
	luaL_openlibs(second);
	const uint64_t firstId = Emmy_RegisterLuaVm(first, nullptr);
	Require(firstId != 0 && Emmy_NotifyLuaVmReady(firstId), "host-before-agent registration is retained");
	Require(facade.ReconcileHostLuaVms(), "host registration reconciles");
	const uint64_t secondId = Emmy_RegisterLuaVm(second, nullptr);
	Require(secondId != 0 && secondId != firstId && Emmy_NotifyLuaVmReady(secondId), "agent-before-host second VM is distinct");
	auto& manager = facade.GetDebugManager();
	auto firstDebugger = manager.AddDebugger(first);
	auto secondDebugger = manager.AddDebugger(second);
	Require(manager.BindVmId(first, firstId) && manager.BindVmId(second, secondId), "VMs bind explicitly");
	facade.StartDebug();
	facade.SetReadyHook(first);
	facade.SetReadyHook(second);
	unsigned firstReadyCalls = 0, secondReadyCalls = 0;
	firstDebugger->ExecuteOnLuaThread([&](lua_State*) {
		++firstReadyCalls;
		firstDebugger->ExecuteOnLuaThread([&](lua_State*) { ++firstReadyCalls; });
	});
	secondDebugger->ExecuteOnLuaThread([&](lua_State*) { ++secondReadyCalls; });
	for (auto state : {first, second}) {
		Require(luaL_loadstring(state, "local ready=1\nready=ready+1\nready=ready+1") == 0 &&
			lua_pcall(state, 0, 0, 0) == 0, "each VM independently leaves its ready hook");
	}
	Require(firstReadyCalls == 2 && secondReadyCalls == 1,
		"first VM cannot consume second VM readiness; deferred callbacks can enqueue more work");
	Run(first, firstDebugger);
	Run(second, secondDebugger);
	// A coroutine shares the first VM but retains its own pause thread identity.
	lua_State* coroutine = lua_newthread(first);
	Run(coroutine, firstDebugger);
	lua_pop(first, 1);
	Require(firstDebugger->TryBeginPause(first), "pause before context reset");
	const uint64_t oldPause = firstDebugger->GetPauseId();
	Require(Emmy_ResetLuaVmContext(firstId, "hot-reload") != 0, "host reset succeeds");
	Require(!firstDebugger->IsPauseActive(oldPause), "reset invalidates pause");
	Require(facade.GetVmRegistry().Find(firstId)->contextGeneration == 2, "reset advances VM generation");
	Run(first, firstDebugger);
	Require(captures == 4, "both VMs, coroutine and reset were executed");
	Require(firstDebugger->TryBeginPause(first), "owner pauses before host shutdown");
	std::thread closer([&] { Require(Emmy_BeginLuaVmClose(firstId, "paused-close") != 0, "close wakes paused owner"); });
	firstDebugger->EnterDebugMode();
	closer.join();
	Require(!firstDebugger->IsRunning() && firstDebugger->GetPauseId() == 0, "close rejects further debugger work");
	for (const auto& vm : {std::make_pair(first, firstId), std::make_pair(second, secondId)}) {
		Require(Emmy_BeginLuaVmClose(vm.second, "harness") != 0, "close starts before lua_close");
		lua_close(vm.first);
		Require(Emmy_EndLuaVmClose(vm.second) != 0, "close completes without reading freed Lua state");
		Require(manager.GetDebuggerByVmId(vm.second) == nullptr, "closed VM has no debugger entry");
		Require(Emmy_ReleaseLuaVmRegistration(vm.second) != 0, "closed registration releases");
	}
	activeDebugger.reset();
	std::cout << "{\"ok\":true,\"luaRuntime\":true,\"captures\":4,\"hostBeforeAgent\":true,\"contextReset\":true}" << std::endl;
	return 0;
}
