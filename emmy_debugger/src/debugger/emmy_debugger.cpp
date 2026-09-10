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
#include "emmy_debugger/debugger/emmy_debugger.h"
#include <algorithm>
#include <cassert>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <set>
#include "emmy_debugger/emmy_facade.h"
#include "emmy_debugger/debugger/hook_state.h"
#include "emmy_debugger/debugger/hook_dispatcher.h"
#include "emmy_debugger/api/lua_version.h"
#include "emmy_debugger/util.h"

#define CACHE_TABLE_NAME "_emmy_cache_table_"
#define CACHE_QUERY_NAME "_emmy_query_table_"

void WaitConnectedHook(lua_State *L, lua_Debug *ar) {
	// EmmyFacade::Get()
	// std::lock_guard<std::mutex> lock()
}

Debugger::Debugger(lua_State *L, EmmyDebuggerManager *manager, uint64_t vmId)
	: currentL(L),
	  mainL(L),
	  manager(manager),
	  vmId(vmId),
	  pauseIdCounter(0),
	  activePauseId(0),
	  contextGeneration(1),
	  sourceEpoch(1),
	  pauseScope(PauseScope::Thread),
	  pauseConsistency("THREAD_ONLY"),
	  pausedL(nullptr),
	  pauseClaimed(false),
	  hookState(nullptr),
	  stateBreak(std::make_shared<HookStateBreak>()),
	  stateContinue(std::make_shared<HookStateContinue>()),
	  stateStepOver(std::make_shared<HookStateStepOver>()),
	  stateStepIn(std::make_shared<HookStateStepIn>()),
	  stateStepOut(std::make_shared<HookStateStepOut>()),
	  stateStop(std::make_shared<HookStateStop>()),
	  running(false),
	  skipHook(false),
	  blocking(false),
	  helperLoaded(false),
	  arenaRef(nullptr),
	  cacheIdCounter(1),
	  cacheGeneration(1),
	  displayCustomTypeInfo(false) {
}

Debugger::~Debugger() {
}

void Debugger::Start() {
	skipHook = false;
	blocking = false;
	running = true;
	helperLoaded = false;  // 重置标志位，允许重新加载 helperCode
	EMMY_LOCK_GUARD(luaThreadMtx);
	doStringList.clear();
}

void Debugger::Attach() {
	if (!running)
		return;
	
	// 防止重复加载
	if (helperLoaded)
		return;

	// 需要 emmyHelperPath 才能加载
	if (manager->emmyHelperPath.empty())
		return;

	helperLoaded = true;
	ExecuteOnLuaThread([this](lua_State *L) {
		const int t = lua_gettop(L);
		// 判断是不是主lua_state
		int ret = lua_pushthread(L);
		if (ret != 1) {
			lua_settop(L, t);
			return;
		}
		
		// 1. 获取 package.path
		lua_getglobal(L, "package");
		if (!lua_istable(L, -1)) {
			EmmyFacade::Get().SendLog(LogType::Error, "[EmmyHelper] package table not found");
			lua_settop(L, t);
			return;
		}
		lua_getfield(L, -1, "path");
		const char* oldPath = lua_tostring(L, -1);
		std::string newPath = oldPath ? oldPath : "";
		
		// 2. 添加 emmyHelperPath 到 package.path
		if (!newPath.empty()) newPath += ";";
		newPath += manager->emmyHelperPath + "/?.lua";
		
		// 3. 如果有 customHelperPath，也添加到 package.path（优先级更高）
		if (!manager->customHelperPath.empty()) {
			newPath = manager->customHelperPath + "/?.lua;" + newPath;
		}
		
		// 4. 设置新的 package.path
		lua_pop(L, 1);  // 弹出旧的 path
		lua_pushstring(L, newPath.c_str());
		lua_setfield(L, -2, "path");
		lua_pop(L, 1);  // 弹出 package 表
		
		EmmyFacade::Get().SendLog(LogType::Debug, "[EmmyHelper] package.path updated");
		
		// 5. require 主 helper 脚本
		std::string helperName = manager->emmyHelperName.empty() ? "emmyHelper" : manager->emmyHelperName;
		lua_getglobal(L, "require");
		lua_pushstring(L, helperName.c_str());
		if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
			std::string msg = lua_tostring(L, -1);
			EmmyFacade::Get().SendLog(LogType::Error, "[EmmyHelper] require '%s' failed: %s", helperName.c_str(), msg.c_str());
			lua_settop(L, t);
			return;
		}
		EmmyFacade::Get().SendLog(LogType::Info, "[EmmyHelper] Loaded: %s", helperName.c_str());
		lua_pop(L, 1);  // 弹出 require 返回值
		
		// 6. require 扩展脚本（如果指定了）
		std::string extName = manager->emmyHelperExtName.empty() ? "emmyHelper_ue" : manager->emmyHelperExtName;
		lua_getglobal(L, "require");
		lua_pushstring(L, extName.c_str());
		if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
			std::string msg = lua_tostring(L, -1);
			EmmyFacade::Get().SendLog(LogType::Error, "[EmmyHelper] Extension '%s' not found: %s", extName.c_str(), msg.c_str());
		} else {
			EmmyFacade::Get().SendLog(LogType::Info, "[EmmyHelper] Extension loaded: %s", extName.c_str());
		}
		
		lua_settop(L, t);
	});
}

void Debugger::Detach() {
	// states.clear();
}

void Debugger::ResetContext() {
	// This method only changes debugger-owned state and does not call into Lua.
	// The host may invoke it from its lifecycle boundary while the old state is
	// being torn down; namespacing cache keys makes invalidation safe even when
	// the registry table cannot be touched from this thread.
	const bool wasSkipping = skipHook.load(std::memory_order_acquire);
	skipHook.store(true, std::memory_order_release);
	ExitDebugMode();
	ClearPause();
	ClearVariableArenaRef();

	{
		EMMY_LOCK_GUARD(evalMtx);
		pendingAction = DebugAction::None;
		while (!evalQueue.empty()) {
			const std::shared_ptr<EvalContext>& context = evalQueue.front();
			if (context) {
				context->success = false;
				context->error = "STALE_CONTEXT";
			}
			evalQueue.pop();
		}
	}
	{
		EMMY_LOCK_GUARD(luaThreadMtx);
		doStringList.clear();
	}
	{
		EMMY_LOCK_GUARD(breakpointMtx);
		contributionHitCounts.clear();
	}

	helperLoaded = false;
	const uint64_t nextContext = contextGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
	const uint64_t nextSource = sourceEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
	cacheIdCounter.store(1, std::memory_order_release);
	const uint64_t nextCacheGeneration = cacheGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
	if (nextContext == 0 || nextSource == 0 || nextCacheGeneration == 0) {
		// A wrapped namespace could alias old values. Keep the debugger disabled
		// rather than silently reusing stale cache ids.
		running.store(false, std::memory_order_release);
	}
	skipHook.store(wasSkipping, std::memory_order_release);
}


void Debugger::SetCurrentState(lua_State *L) {
	EMMY_LOCK_GUARD(stateMtx);
	if (L != nullptr) {
		currentL = L;
	}
}

lua_State* Debugger::GetStateForExecution() const {
	// A thread-only pause owns the Lua state that produced the snapshot.  A
	// different coroutine may still generate hook callbacks while that state
	// is blocked, so currentL is only a routing hint once a pause is active.
	{
		EMMY_LOCK_GUARD(pauseMtx);
		if (activePauseId.load(std::memory_order_acquire) != 0 && pausedL != nullptr) {
			return pausedL;
		}
	}
	EMMY_LOCK_GUARD(stateMtx);
	return currentL;
}

uint64_t Debugger::GetVmId() const {
	return vmId;
}

void Debugger::SetVmId(uint64_t value) {
	vmId = value;
}

void Debugger::SetContextIdentity(uint64_t value, uint64_t epoch) {
	if (value != 0) contextGeneration.store(value, std::memory_order_release);
	if (epoch != 0) sourceEpoch.store(epoch, std::memory_order_release);
}

uint64_t Debugger::GetContextGeneration() const {
	return contextGeneration.load(std::memory_order_acquire);
}

uint64_t Debugger::GetSourceEpoch() const {
	return sourceEpoch.load(std::memory_order_acquire);
}

uint64_t Debugger::GetPauseId() const {
	return activePauseId.load(std::memory_order_acquire);
}

bool Debugger::IsPauseActive(uint64_t pauseId) const {
	const uint64_t active = GetPauseId();
	return active != 0 && (pauseId == 0 || active == pauseId);
}

bool Debugger::IsPauseThread(const std::string& threadId) const {
	if (threadId.empty()) return true;
	EMMY_LOCK_GUARD(pauseMtx);
	return activePauseId.load(std::memory_order_acquire) != 0 && pauseThreadId == threadId;
}

bool Debugger::IsFrameActive(const std::string& frameId) const {
	if (frameId.empty()) return true;
	EMMY_LOCK_GUARD(pauseMtx);
	return activePauseId.load(std::memory_order_acquire) != 0 &&
		frameId.compare(0, pauseFramePrefix.size(), pauseFramePrefix) == 0;
}

void Debugger::ClearPause() {
	EMMY_LOCK_GUARD(pauseMtx);
	activePauseId.store(0, std::memory_order_release);
	pauseClaimed = false;
	pausedL = nullptr;
	pauseThreadId.clear();
	pauseFramePrefix.clear();
	pauseScope = PauseScope::Thread;
	pauseConsistency = "THREAD_ONLY";
	pauseReason.clear();
	pauseReasons.clear();
}

bool Debugger::TryClaimPause(lua_State* breakState) {
	if (breakState == nullptr) return false;
	EMMY_LOCK_GUARD(pauseMtx);
	if (activePauseId.load(std::memory_order_acquire) != 0 || pauseClaimed) {
		return false;
	}
	pauseClaimed = true;
	pausedL = breakState;
	return true;
}

void Debugger::ReleasePauseClaim() {
	EMMY_LOCK_GUARD(pauseMtx);
	if (activePauseId.load(std::memory_order_acquire) != 0) return;
	pauseClaimed = false;
	pausedL = nullptr;
	pauseThreadId.clear();
	pauseFramePrefix.clear();
	pauseReason.clear();
	pauseReasons.clear();
}

bool Debugger::CommitClaimedPause(lua_State* breakState,
							  const std::vector<std::string>& reasons,
							  uint64_t* outPauseId) {
	if (breakState == nullptr) return false;
	EMMY_LOCK_GUARD(pauseMtx);
	if (activePauseId.load(std::memory_order_acquire) != 0) return false;
	if (!pauseClaimed) {
		pauseClaimed = true;
		pausedL = breakState;
	} else if (pausedL != breakState) {
		return false;
	}
	const uint64_t id = pauseIdCounter.fetch_add(1, std::memory_order_relaxed) + 1;
	pausedL = breakState;
	std::ostringstream thread;
	thread << "thread-" << std::hex << reinterpret_cast<uintptr_t>(breakState);
	pauseThreadId = thread.str();
	pauseScope = PauseScope::Thread;
	pauseConsistency = "THREAD_ONLY";
	pauseReasons = reasons;
	if (pauseReasons.empty()) pauseReasons.push_back("SYSTEM");
	std::ostringstream reason;
	for (std::vector<std::string>::const_iterator it = pauseReasons.begin();
			 it != pauseReasons.end(); ++it) {
		if (it != pauseReasons.begin()) reason << ",";
		reason << *it;
	}
	pauseReason = reason.str();
	pauseFramePrefix = "frame-" + std::to_string(id) + "-";
	pauseClaimed = false;
	// Arm before publishing the pause. A fast Continue/Eval can arrive before
	// EnterDebugMode; entering the loop must not overwrite that command.
	blocking.store(true, std::memory_order_release);
	activePauseId.store(id, std::memory_order_release);
	if (outPauseId != nullptr) *outPauseId = id;
	return true;
}

bool Debugger::TryBeginPause(lua_State* breakState,
						 const std::vector<std::string>& reasons,
						 uint64_t* outPauseId) {
	if (!TryClaimPause(breakState)) return false;
	if (CommitClaimedPause(breakState, reasons, outPauseId)) return true;
	ReleasePauseClaim();
	return false;
}

std::string Debugger::GetPauseThreadId() const {
	EMMY_LOCK_GUARD(pauseMtx);
	return pauseThreadId;
}

PauseScope Debugger::GetPauseScope() const {
	EMMY_LOCK_GUARD(pauseMtx);
	return pauseScope;
}

std::string Debugger::GetPauseConsistency() const {
	EMMY_LOCK_GUARD(pauseMtx);
	return pauseConsistency;
}

std::string Debugger::GetPauseReason() const {
	EMMY_LOCK_GUARD(pauseMtx);
	return pauseReason;
}

std::vector<std::string> Debugger::GetPauseReasons() const {
	EMMY_LOCK_GUARD(pauseMtx);
	return pauseReasons;
}

std::string Debugger::GetPauseFrameId(int stackLevel) const {
	EMMY_LOCK_GUARD(pauseMtx);
	if (activePauseId.load(std::memory_order_acquire) == 0 || stackLevel < 0) return std::string();
	return pauseFramePrefix + std::to_string(stackLevel);
}

std::shared_ptr<HookStateBreak> Debugger::GetStateBreak() const { return stateBreak; }
std::shared_ptr<HookStateContinue> Debugger::GetStateContinue() const { return stateContinue; }
std::shared_ptr<HookStateStepOver> Debugger::GetStateStepOver() const { return stateStepOver; }
std::shared_ptr<HookStateStepIn> Debugger::GetStateStepIn() const { return stateStepIn; }
std::shared_ptr<HookStateStepOut> Debugger::GetStateStepOut() const { return stateStepOut; }
std::shared_ptr<HookStateStop> Debugger::GetStateStop() const { return stateStop; }

void Debugger::Hook(lua_Debug *ar, lua_State *L) {
	if (skipHook) {
		return;
	}
	// 设置当前协程
	SetCurrentState(L);
	if (GetPauseId() == 0) {
		DebugAction action;
		{
			EMMY_LOCK_GUARD(evalMtx);
			action = pendingAction;
			pendingAction = DebugAction::None;
		}
		if (action != DebugAction::None) DoAction(action);
	}

	if (getDebugEvent(ar) == LUA_HOOKLINE) {
		std::vector<Executor> executors;
		{
			EMMY_LOCK_GUARD(luaThreadMtx);
			executors.swap(luaThreadExecutors);
		}
		// Helpers may schedule more owner-thread work; do not execute callbacks
		// under the non-recursive queue mutex.
		for (auto &executor: executors) ExecuteWithSkipHook(executor);
		auto bp = FindBreakPoint(ar);
		if (bp && ProcessBreakPoint(bp, L)) {
			HandleBreak(L);
			return;
		}
		// 加锁

		std::shared_ptr<HookState> state = nullptr;

		{
			EMMY_LOCK_GUARD(hookStateMtx);
			state = hookState;
		}

		if (state) {
			state->ProcessHook(shared_from_this(), L, ar);
		}
	}
}


void Debugger::Stop() {
	running = false;
	skipHook = true;
	ClearPause();

	// 停止main_state 的hook
	// 但不停止coroutine的hook因为没有办法知道这个lua state 指针是否有效
	// stop在交互线程，不应该设置hook
	// 而且清理对lua_state 的 hook没有必要
	// UpdateHook(0, mainL);

	// 清理hook 状态
	{
		EMMY_LOCK_GUARD(hookStateMtx);
		hookState = nullptr;
	}

	{
		// clear lua thread executors
		EMMY_LOCK_GUARD(luaThreadMtx);
		luaThreadExecutors.clear();
	}
	ExitDebugMode();
}

bool Debugger::IsRunning() const {
	return running;
}

bool Debugger::IsMainCoroutine(lua_State *L) const {
	return L == mainL;
}

bool Debugger::GetStacks(std::vector<Stack> &stacks, std::size_t maxFrames, bool* truncated, bool rawSnapshot) {
	stacks.clear();
	if (truncated != nullptr) *truncated = false;
	if (maxFrames == 0) return false;
	// Capture the complete pause identity as one unit.  Reading activePauseId
	// first and the remaining fields later could pair a new pause with the old
	// coroutine or frame prefix.
	lua_State* L = nullptr;
	lua_State* capturedState = nullptr;
	uint64_t capturedPauseId = 0;
	std::string capturedFramePrefix;
	{
		EMMY_LOCK_GUARD(pauseMtx);
		capturedPauseId = activePauseId.load(std::memory_order_acquire);
		L = pausedL;
		capturedState = pausedL;
		capturedFramePrefix = pauseFramePrefix;
	}
	if (capturedPauseId == 0 || L == nullptr || capturedFramePrefix.empty()) {
		return false;
	}

	int totalLevel = 0;
	RestrictedEvalLimits snapshotLimits;
	int snapshotNodes = 0;
	size_t snapshotBytes = 0;
	std::set<const void*> snapshotVisited;
	auto boundedText = [](const char* text, size_t maximum) {
		if (!text) return std::string();
		size_t length = 0;
		while (length < maximum && text[length] != '\0') ++length;
		return std::string(text, length);
	};
	auto collect = [&](Idx<Variable> variable) {
		if (rawSnapshot) {
			CacheValue(-1, variable, L);
			GetRestrictedVariable(L, variable, -1, 0, snapshotNodes, snapshotBytes, snapshotLimits, snapshotVisited);
		} else GetVariable(L, variable, -1, 1);
	};
	auto exhausted = [&] {
		const bool full = rawSnapshot && (snapshotNodes >= snapshotLimits.maxNodes ||
			snapshotBytes >= static_cast<size_t>(snapshotLimits.maxBytes));
		if (full && truncated) *truncated = true;
		return full;
	};
	std::set<lua_State*> visitedStates;
	while (true) {
		if (!visitedStates.insert(L).second) {
			if (truncated != nullptr) *truncated = true;
			break;
		}
		int level = 0;
		while (true) {
			if (stacks.size() >= maxFrames) {
				if (truncated != nullptr) *truncated = true;
				break;
			}
			lua_Debug ar{};
			if (!lua_getstack(L, level, &ar)) {
				break;
			}
			if (!lua_getinfo(L, "nSlu", &ar)) {
				++level;
				continue;
			}
			// C++ 17 only return T&
			stacks.emplace_back();
			auto &stack = stacks.back();
			stack.file = rawSnapshot ? boundedText(getDebugSource(&ar), 4096) : GetFile(&ar, L);
			if (rawSnapshot && !stack.file.empty() && stack.file.front() == '@') stack.file.erase(0, 1);
			stack.sourceEpoch = GetSourceEpoch();
			HostSourceIdentity loadedSource;
			if (EmmyFacade::Get().GetSourceRegistry().Resolve(vmId, stack.sourceEpoch,
				getDebugSource(&ar) == nullptr ? std::string() : getDebugSource(&ar), loadedSource)) {
				stack.file = loadedSource.canonicalPath;
				stack.sourceHash = loadedSource.sha256;
			}
			stack.functionName = boundedText(getDebugName(&ar), 256);
			stack.level = totalLevel++;
			stack.frameId = capturedFramePrefix + std::to_string(stack.level);
			stack.line = getDebugCurrentLine(&ar);

			// get variables
			{
				for (int i = 1;; i++) {
					if (exhausted()) break;
					const char *name = lua_getlocal(L, &ar, i);
					if (name == nullptr) {
						break;
					}
					if (name[0] == '(') {
						lua_pop(L, 1);
						continue;
					}

					// add local variable
					auto var = stack.variableArena->Alloc();
					var->name = boundedText(name, 256);
					snapshotBytes += var->name.size();
					SetVariableArena(stack.variableArena.get());
					collect(var);
					ClearVariableArenaRef();
					lua_pop(L, 1);
					stack.localVariables.push_back(var);
				}

				if (lua_getinfo(L, "f", &ar)) {
					const int fIdx = lua_gettop(L);
					for (int i = 1;; i++) {
						if (exhausted()) break;
						const char *name = lua_getupvalue(L, fIdx, i);
						if (!name) {
							break;
						}

						// add up variable
						auto var = stack.variableArena->Alloc();
						var->name = boundedText(name, 256);
						snapshotBytes += var->name.size();
						SetVariableArena(stack.variableArena.get());
						collect(var);
						ClearVariableArenaRef();
						lua_pop(L, 1);
						stack.upvalueVariables.push_back(var);
					}
					// pop function
					lua_pop(L, 1);
				}

				// Expose globals through one explicit raw root.  Keeping this as a
				// single bounded node avoids copying the entire global namespace into
				// every frame while still allowing the safe `_G.foo` path used by CLI
				// probes.  The value collector never invokes Lua functions here.
				if (stack.level == 0 && !exhausted()) {
					lua_pushglobaltable(L);
					auto global = stack.variableArena->Alloc();
					global->name = "_G";
					global->nameType = LUA_TSTRING;
					SetVariableArena(stack.variableArena.get());
					if (rawSnapshot) collect(global);
					else GetVariable(L, global, -1, 1, false);
					ClearVariableArenaRef();
					lua_pop(L, 1);
					stack.globalVariables.push_back(global);
				}
			}

			level++;
		}

		// Raw snapshots describe this Lua thread only. Resolving logical parent
		// threads through extension callbacks would execute application Lua.
		if (rawSnapshot) break;
		lua_State *PL = manager->extension.QueryParentThread(L);

		if (PL != nullptr) {
			L = PL;
		} else {
			break;
		}
	}

	// Continue/close can invalidate a pause while the Lua owner thread is
	// being inspected.  Never publish a partially mixed snapshot in that case.
	{
		EMMY_LOCK_GUARD(pauseMtx);
		if (activePauseId.load(std::memory_order_acquire) != capturedPauseId ||
			pausedL != capturedState || pauseFramePrefix != capturedFramePrefix) {
			stacks.clear();
			return false;
		}
	}
	return !stacks.empty();
}

bool CallMetaFunction(lua_State *L, int valueIndex, const char *method, int numResults, int &result) {
	if (lua_getmetatable(L, valueIndex)) {
		const int metaIndex = lua_gettop(L);
		if (!lua_isnil(L, metaIndex)) {
			lua_pushstring(L, method);
			lua_rawget(L, metaIndex);
			if (lua_isnil(L, -1)) {
				// The meta-method doesn't exist.
				lua_pop(L, 1);
				lua_remove(L, metaIndex);
				return false;
			}
			lua_pushvalue(L, valueIndex);
			result = lua_pcall(L, 1, numResults, 0);
		}
		lua_remove(L, metaIndex);
		return true;
	}
	return false;
}

std::string ToPointer(lua_State *L, int index) {
	const void *pointer = lua_topointer(L, index);
	std::stringstream ss;
	ss << lua_typename(L, lua_type(L, index)) << "(0x" << std::hex << pointer << ")";
	return ss.str();
}

#ifndef EMMY_USE_LUA_SOURCE
void DisplayFunction54(Idx<Variable> variable, lua_State *L, int index, lua_Debug_54 &ar) {
	if (ar.what == nullptr) {
		return;
	}
	std::string what = ar.what;
	if (what == "Lua") {
		auto paramNum = ar.nparams > 10 ? 10 : ar.nparams;
		std::string showValue = "function(";
		lua_pushvalue(L, index);
		for (auto i = 0; i != paramNum; i++) {
			auto paramIndex = i + 1;
			auto paramName = lua_getlocal(L, nullptr, paramIndex);
			if (paramName) {
				showValue.append(paramName);
				if (paramIndex != paramNum) {
					showValue.append(", ");
				}
			}
		}
		showValue.push_back(')');
		variable->value = showValue;
		variable->valueType = 9;
		variable->valueTypeName = "function";
		// ptr
		auto ptr = variable.GetArena()->Alloc();
		ptr->nameType = LUA_TSTRING;
		ptr->valueType = LUA_TFUNCTION;
		ptr->name = "pointer";
		ptr->value = ToPointer(L, index);

		// source
		if (ar.source) {
			std::string sourceText = ar.source;
			if (!sourceText.empty() && sourceText.front() == '@') {
				sourceText = sourceText.substr(1);
			}
			auto source = variable.GetArena()->Alloc();
			source->nameType = LUA_TSTRING;
			source->valueType = LUA_TSTRING;
			source->name = "source";
			source->value = sourceText.append(":").append(std::to_string(ar.linedefined));
		}
	} else if (what == "C") {
		variable->value = "C " + ToPointer(L, index);
	} else {
		variable->value = ToPointer(L, index);
		return;
	}
}

void DisplayFunction(Idx<Variable> variable, lua_State *L, int index) {
	lua_Debug ar{};
	lua_pushvalue(L, index);
	if (lua_getinfo(L, ">Snu", &ar) == 0) {
		variable->value = ToPointer(L, index);
	}
	else {
		switch (luaVersion) {
			case LuaVersion::LUA_54: {
				DisplayFunction54(variable, L, index, ar.u.ar54);
				break;
			}
			default: {
				variable->value = ToPointer(L, index);
				break;
			}
		}
    }
	lua_settop(L, index);
}
#endif
// algorithm optimization
void Debugger::GetVariable(lua_State *L, Idx<Variable> variable, int index, int depth, bool queryHelper) {
	if (!L) {
		L = GetStateForExecution();
	}

	if (!L) {
		return;
	}

	// 如果没有计算深度则不予计算
	if (depth <= 0) {
		return;
	}

	const int topIndex = lua_gettop(L);
	index = lua_absindex(L, index);
	CacheValue(index, variable, L);
	const int type = lua_type(L, index);
	const char *typeName = lua_typename(L, type);
	variable->valueTypeName = typeName;
	variable->valueType = type;

	if (queryHelper) {
		if (displayCustomTypeInfo && type >= 0 && type < registeredTypes.size() && registeredTypes.test(type)
			&& manager->extension.QueryVariableCustom(L, variable, typeName, index, depth)) {
			return;
		}
		else if ((type == LUA_TTABLE || type == LUA_TUSERDATA || type == LUA_TFUNCTION)
			&& manager->extension.QueryVariable(L, variable, typeName, index, depth)) {
			return;
		}
	}
	switch (type) {
		case LUA_TNIL: {
			variable->value = "nil";
			break;
		}
		case LUA_TNUMBER: {
			variable->value = lua_tostring(L, index);
			break;
		}
		case LUA_TBOOLEAN: {
			const bool v = lua_toboolean(L, index);
			variable->value = v ? "true" : "false";
			break;
		}
		case LUA_TSTRING: {
			variable->value = lua_tostring(L, index);
			break;
		}
		case LUA_TUSERDATA: {
			// Host integrations may provide a safe, copied representation for
			// engine userdata. The provider owns any GameThread dispatch; the
			// debugger never exposes a UObject/FProperty pointer.
			if (vmId != 0 && manager != nullptr && EmmyFacade::Get().GetHostValueProviderRegistry().HasProvider()) {
				HostValueRequest request;
				request.vmId = vmId;
				{
					EMMY_LOCK_GUARD(pauseMtx);
					request.threadId = pauseThreadId;
				}
				request.valueRef = ToPointer(L, index);
				request.limits.maxDepth = static_cast<uint32_t>(depth);
				HostValueResult hostValue = EmmyFacade::Get().GetHostValueProviderRegistry().Describe(request);
				if (hostValue.IsSuccess()) {
					variable->value = hostValue.display.empty() ? hostValue.serializedJson : hostValue.display;
					if (!hostValue.typeName.empty()) variable->valueTypeName = hostValue.typeName;
					return;
				}
			}
			auto *string = lua_tostring(L, index);
			if (string == nullptr) {
				int result;
				if (CallMetaFunction(L, topIndex, "__tostring", 1, result) && result == 0) {
					string = lua_tostring(L, -1);
					lua_pop(L, 1);
				}
				else {
					lua_settop(L, topIndex);
				}
			}
			if (string) {
				variable->value = string;
			} else {
				variable->value = ToPointer(L, index);
			}
			if (depth > 1) {
				if (lua_getmetatable(L, index)) {
					GetVariable(L, variable, -1, depth);
					lua_pop(L, 1);//pop meta
				}
			}
			break;
		}
		case LUA_TFUNCTION: {
#ifndef EMMY_USE_LUA_SOURCE
			DisplayFunction(variable, L, index);
#else
			variable->value = ToPointer(L, index);
#endif

			break;
		}
		case LUA_TLIGHTUSERDATA:
		case LUA_TTHREAD: {
			variable->value = ToPointer(L, index);
			break;
		}
		case LUA_TTABLE: {
			std::size_t tableSize = 0;
			const void *tableAddr = lua_topointer(L, index);
			lua_pushnil(L);
			while (lua_next(L, index)) {
				// k: -2, v: -1
				if (depth > 1) {
					//todo: use allocator
					auto v = variable.GetArena()->Alloc();
					const auto t = lua_type(L, -2);
					v->nameType = t;
                    switch (t) {
                        case LUA_TSTRING:
                        {
                            v->name = lua_tostring(L, -2);
                            break;
                        }
                        case LUA_TNUMBER:
                        {
                            auto number = lua_tonumber(L, -2);
                            if (static_cast<long long>(number) == number) {
                                v->name = std::to_string(static_cast<long long>(number));
                            } else {
                                v->name = std::to_string(number);
                            }
                            break;
                        }
                        case LUA_TBOOLEAN:
                        {
                            v->name = lua_toboolean(L, -2) ? "true" : "false";
                            break;
                        }
                        default: {
                            v->name = ToPointer(L, -2);
                            break;
                        }
                    }

					GetVariable(L, v, -1, depth - 1);
					variable->children.push_back(v);
				}
				lua_pop(L, 1);
				tableSize++;
			}


			if (lua_getmetatable(L, index)) {
				// metatable
				auto metatable = variable.GetArena()->Alloc();
				metatable->name = "(metatable)";
				metatable->nameType = lua_type(L, -1);

				GetVariable(L, metatable, -1, depth - 1);
				variable->children.push_back(metatable);

				//__index
				if (lua_istable(L, -1)) {
					// fix BUG 导致涉及到FGUI的框架崩溃
					lua_pushstring(L, "__index");
					lua_rawget(L, -2);
					if (!lua_isnil(L, -1)) {
						auto v = variable.GetArena()->Alloc();
						v->name = "(metatable.__index)";
						v->nameType = lua_type(L, -1);
						GetVariable(L, v, -1, depth - 1);
						variable->children.push_back(v);
					}
					lua_pop(L, 1);
				}

				// metatable
				lua_pop(L, 1);
			}

			std::stringstream ss;
			ss << "table(0x" << std::hex << tableAddr << std::dec << ", size = " << tableSize << ")";
			variable->value = ss.str();
			break;
		}
	}
	const int t2 = lua_gettop(L);
	assert(t2 == topIndex);
}

void Debugger::CacheValue(int valueIndex, Idx<Variable> variable, lua_State* state) const {
	lua_State* L = state != nullptr ? state : GetStateForExecution();
	if (!L) {
		return;
	}

	const int type = lua_type(L, valueIndex);
	if (type == LUA_TUSERDATA || type == LUA_TTABLE) {
		const int id = cacheIdCounter.fetch_add(1, std::memory_order_relaxed);
		variable->cacheId = id;
		const int top = lua_gettop(L);
		lua_getfield(L, LUA_REGISTRYINDEX, CACHE_TABLE_NAME);// 1: cacheTable|nil
		if (lua_isnil(L, -1)) {
			lua_pop(L, 1);                                       //
			lua_newtable(L);                                     // 1: {}
			lua_setfield(L, LUA_REGISTRYINDEX, CACHE_TABLE_NAME);//
			lua_getfield(L, LUA_REGISTRYINDEX, CACHE_TABLE_NAME);// 1: cacheTable
		}

		lua_pushvalue(L, valueIndex);// 1: cacheTable, 2: value

		// snprintf 性能不够，问题也很大，这里采用c++标准算法
		std::string key = CacheKey(id);
		lua_setfield(L, -2, key.c_str());// 1: cacheTable

		lua_settop(L, top);
	}
}

void Debugger::ClearCache(lua_State* state) const {
	lua_State* L = state != nullptr ? state : GetStateForExecution();
	if (!L) {
		return;
	}

	lua_getfield(L, LUA_REGISTRYINDEX, CACHE_TABLE_NAME);
	if (!lua_isnil(L, -1)) {
		lua_pushnil(L);
		lua_setfield(L, LUA_REGISTRYINDEX, CACHE_TABLE_NAME);
	}
	lua_pop(L, 1);
}

bool Debugger::RequestAction(DebugAction action, uint64_t pauseId) {
	{
		EMMY_LOCK_GUARD(evalMtx);
		if (!running || pendingAction != DebugAction::None) return false;
		if (action != DebugAction::Break && !IsPauseActive(pauseId)) return false;
		pendingAction = action;
	}
	EMMY_COND_NOTIFY_ALL(cvRun);
	return true;
}

void Debugger::DoAction(DebugAction action) {
	const uint64_t resumedPause = GetPauseId();
	const std::string resumedThread = GetPauseThreadId();
	const uint64_t resumedContext = GetContextGeneration();
	const uint64_t resumedSource = GetSourceEpoch();
	switch (action) {
		case DebugAction::Break:
			SetHookState(stateBreak);
			break;
		case DebugAction::Continue:
			SetHookState(stateContinue);
			break;
		case DebugAction::StepOver:
			SetHookState(stateStepOver);
			break;
		case DebugAction::StepIn:
			SetHookState(stateStepIn);
			break;
		case DebugAction::Stop:
			SetHookState(stateStop);
			break;
		case DebugAction::StepOut:
			SetHookState(stateStepOut);
			break;
		default:
			break;
	}
	if (action == DebugAction::Continue || action == DebugAction::StepOver ||
		action == DebugAction::StepIn || action == DebugAction::StepOut || action == DebugAction::Stop) {
		ClearPause();
		EmmyFacade::Get().OnResume(GetVmId(), resumedPause, resumedThread, resumedContext, resumedSource);
	}
}

std::string Debugger::CacheKey(int cacheId) const {
	return std::to_string(cacheGeneration.load(std::memory_order_acquire)) +
		":" + std::to_string(cacheId);
}

std::shared_ptr<HookState> Debugger::GetHookState() const {
	EMMY_LOCK_GUARD(hookStateMtx);
	return hookState;
}

void Debugger::UpdateHook(int mask, lua_State *L) {
	if (mask == 0)
		ClearDebuggerHook(L);
	else
		SetDebuggerHook(L, EmmyFacade::HookLua, mask, 0);
}


// _G.emmy.fixPath = function(path) return (newPath) end
int FixPath(lua_State *L) {
	const auto path = lua_tostring(L, 1);
	lua_getglobal(L, "emmy");
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "fixPath");
		if (lua_isfunction(L, -1)) {
			lua_pushstring(L, path);
			lua_call(L, 1, 1);
			return 1;
		}
	}
	return 0;
}


std::string Debugger::GetFile(lua_Debug *ar, lua_State* state) const {
	lua_State* L = state != nullptr ? state : GetStateForExecution();
	if (!L) {
		return "";
	}

	const char *file = getDebugSource(ar);
	if (getDebugCurrentLine(ar) < 0)
		return file;
	if (strlen(file) > 0 && file[0] == '@')
		file++;
	lua_pushcclosure(L, FixPath, 0);
	lua_pushstring(L, file);
	const int result = lua_pcall(L, 1, 1, 0);
	if (result == LUA_OK) {
		const auto p = lua_tostring(L, -1);
		lua_pop(L, 1);
		if (p) {
			return p;
		}
	}
	// todo: handle error
	return file;
}

void Debugger::HandleBreak(lua_State* breakState) {
	if (breakState == nullptr) {
		EMMY_LOCK_GUARD(stateMtx);
		breakState = currentL;
	}
	if (breakState == nullptr) return;

	std::vector<std::string> reasons;
	{
		EMMY_LOCK_GUARD(pauseMtx);
		// ProcessBreakPoint may already have claimed this slot while evaluating
		// conditions. A different coroutine must never steal that claim.
		if (activePauseId.load(std::memory_order_acquire) != 0) return;
		if (pauseClaimed) {
			if (pausedL != breakState) return;
		} else {
			pauseClaimed = true;
			pausedL = breakState;
		}
		reasons = pauseReasons;
	}
	uint64_t pauseId = 0;
	if (!CommitClaimedPause(breakState, reasons, &pauseId)) {
		ReleasePauseClaim();
		return;
	}

	// To be on the safe side, hook the actual triggering coroutine again.
	UpdateHook(LUA_MASKCALL | LUA_MASKLINE | LUA_MASKRET, breakState);

	if (EmmyFacade::Get().OnBreak(shared_from_this())) {
		EnterDebugMode();
	} else {
		// A break that cannot be published must not leave the Lua thread in a
		// permanently active pause with no controller able to resume it.
		ExitDebugMode();
		ClearPause();
	}
}

// host thread
void Debugger::EnterDebugMode() {
	lua_State* const ownerState = GetStateForExecution();
	while (true) {
		SRWUniqueLock lockEval(evalMtx);
		EMMY_COND_WAIT(cvRun, lockEval, [this] {
			return !blocking || pendingAction != DebugAction::None || !evalQueue.empty();
		});
		if (!blocking) break;
		if (pendingAction != DebugAction::None) {
			const DebugAction action = pendingAction;
			pendingAction = DebugAction::None;
			lockEval.unlock();
			DoAction(action);
			continue;
		}
		if (!evalQueue.empty()) {
			const auto evalContext = evalQueue.front();
			evalQueue.pop();
			lockEval.unlock();
			const bool skip = skipHook.load(std::memory_order_acquire);
			skipHook.store(true, std::memory_order_release);
			evalContext->success = EmmyFacade::Get().TryStartEvaluation(evalContext) && DoEval(evalContext);
			skipHook.store(skip, std::memory_order_release);
			EmmyFacade::Get().OnEvalResult(evalContext);
			continue;
		}
		break;
	}
	ClearCache(ownerState);
}

void Debugger::ExitDebugMode() {
	std::queue<std::shared_ptr<EvalContext>> cancelled;
	{
		EMMY_LOCK_GUARD(evalMtx);
		blocking = false;
		pendingAction = DebugAction::None;
		cancelled.swap(evalQueue);
	}
	EMMY_COND_NOTIFY_ALL(cvRun);
	// Complete dropped requests outside the queue lock. Resume/stop must not
	// leave request identities permanently in flight or evaluate them on a
	// later pause with a different Lua stack.
	while (!cancelled.empty()) {
		const auto context = cancelled.front();
		cancelled.pop();
		if (!context) continue;
		context->success = false;
		context->error = "STALE_PAUSE_REFERENCE";
		EmmyFacade::Get().OnEvalResult(context);
	}
}


int EnvIndexFunction(lua_State *L) {
	const int locals = lua_upvalueindex(1);
	const int upvalues = lua_upvalueindex(2);
	const char *name = lua_tostring(L, 2);
	// up value
	lua_getfield(L, upvalues, name);
	if (lua_isnil(L, -1) == 0) {
		return 1;
	}
	lua_pop(L, 1);
	// local value
	lua_getfield(L, locals, name);
	if (lua_isnil(L, -1) == 0) {
		return 1;
	}
	lua_pop(L, 1);
	// _ENV
	lua_getfield(L, upvalues, "_ENV");
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, name);// _ENV[name]
		if (lua_isnil(L, -1) == 0) {
			return 1;
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	// global
	lua_getglobal(L, name);
	if (lua_isnil(L, -1) == 0) {
		return 1;
	}
	lua_pop(L, 1);
	return 0;
}

bool Debugger::CreateEnv(lua_State *L, int stackLevel) {
	if (!L) {
		return false;
	}


	//assert(L);
	//const auto L = L;

	lua_Debug ar{};
	if (!lua_getstack(L, stackLevel, &ar)) {
		return false;
	}
	if (!lua_getinfo(L, "nSlu", &ar)) {
		return false;
	}

	lua_newtable(L);
	const int env = lua_gettop(L);
	lua_newtable(L);
	const int envMetatable = lua_gettop(L);
	lua_newtable(L);
	const int locals = lua_gettop(L);
	lua_newtable(L);
	const int upvalues = lua_gettop(L);

	int idx = 1;
	// local values
	while (true) {
		const char *name = lua_getlocal(L, &ar, idx++);
		if (name == nullptr)
			break;
		if (name[0] == '(') {
			lua_pop(L, 1);
			continue;
		}
		lua_setfield(L, locals, name);
	}
	// up values
	if (lua_getinfo(L, "f", &ar)) {
		const int fIdx = lua_gettop(L);
		idx = 1;
		while (true) {
			const char *name = lua_getupvalue(L, fIdx, idx++);
			if (name == nullptr)
				break;
			lua_setfield(L, upvalues, name);
		}
		lua_pop(L, 1);
	}
	int top = lua_gettop(L);
	assert(top == upvalues);

	// index function
	// up value: locals, upvalues
	lua_pushcclosure(L, EnvIndexFunction, 2);

	// envMetatable.__index = EnvIndexFunction
	lua_setfield(L, envMetatable, "__index");
	// setmetatable(env, envMetatable)
	lua_setmetatable(L, env);

	top = lua_gettop(L);
	assert(top == env);
	return true;
}

namespace {

std::string ContributionReason(const BreakPoint& breakpoint,
	const BreakPointContribution& contribution) {
	if (contribution.owner.compare(0, 4, "CLI:") == 0) {
		if (contribution.breakpointId.compare(0, 6, "probe:") == 0) {
			return "PROBE:" + contribution.breakpointId.substr(6);
		}
		return contribution.owner;
	}
	if (contribution.runToHere || breakpoint.runToHere) return "SYSTEM";
	if (!contribution.owner.empty() && contribution.owner != "LEGACY") return contribution.owner;
	return "USER";
}

std::string ContributionKey(const BreakPoint& breakpoint,
	const BreakPointContribution& contribution) {
	std::ostringstream stream;
	stream << breakpoint.vmId << "|" << breakpoint.file << "|" << breakpoint.line << "|"
		<< contribution.owner << "|" << contribution.breakpointId;
	return stream.str();
}

} // namespace

bool Debugger::ProcessBreakPoint(std::shared_ptr<BreakPoint> bp, lua_State* breakState) {
	if (!bp) return false;
	// Reserve the pause slot before any condition evaluation. Evaluation may
	// yield through the host and another coroutine must not replace the pause
	// identity or reason set while it is in progress.
	lua_State* evaluationState = breakState != nullptr ? breakState : GetStateForExecution();
	if (!TryClaimPause(evaluationState)) return false;
	std::vector<BreakPointContribution> contributions = bp->contributions;
	if (contributions.empty()) {
		BreakPointContribution contribution;
		contribution.owner = bp->owner.empty() ? "LEGACY" : bp->owner;
		contribution.breakpointId = bp->breakpointId;
		contribution.condition = bp->condition;
		contribution.logMessage = bp->logMessage;
		contribution.hitCondition = bp->hitCondition;
		contribution.hitCount = bp->hitCount;
		contribution.runToHere = bp->runToHere;
		contributions.push_back(contribution);
	}

	std::vector<std::string> hitReasons;
	for (std::vector<BreakPointContribution>::const_iterator it = contributions.begin();
		 it != contributions.end(); ++it) {
		const BreakPointContribution& contribution = *it;
		bool shouldPause = false;
		if (!contribution.condition.empty()) {
			auto ctx = std::make_shared<EvalContext>();
			ctx->expr = contribution.condition;
			ctx->depth = 1;
			const bool evaluated = DoEval(ctx);
			shouldPause = evaluated && ctx->result->valueType == LUA_TBOOLEAN &&
				ctx->result->value == "true";
		} else if (!contribution.logMessage.empty()) {
			std::shared_ptr<BreakPoint> logBreakpoint(new BreakPoint(*bp));
			logBreakpoint->logMessage = contribution.logMessage;
			logBreakpoint->condition.clear();
			logBreakpoint->hitCondition.clear();
			DoLogMessage(logBreakpoint);
		} else if (!contribution.hitCondition.empty()) {
			const std::string key = ContributionKey(*bp, contribution);
			BreakPoint counter = *bp;
			counter.hitCondition = contribution.hitCondition;
			counter.hitCount = 0;
			{
				EMMY_LOCK_GUARD(breakpointMtx);
				counter.hitCount = ++contributionHitCounts[key];
			}
			shouldPause = DoHitCondition(std::make_shared<BreakPoint>(counter));
		} else {
			shouldPause = true;
		}
		if (shouldPause) hitReasons.push_back(ContributionReason(*bp, contribution));
	}

	if (!hitReasons.empty()) {
		bool claimValid = false;
		{
			EMMY_LOCK_GUARD(pauseMtx);
			if (pauseClaimed && pausedL == evaluationState &&
				activePauseId.load(std::memory_order_acquire) == 0) {
				claimValid = true;
				pauseReasons = hitReasons;
				std::ostringstream reason;
				for (std::vector<std::string>::const_iterator it = hitReasons.begin();
						 it != hitReasons.end(); ++it) {
					if (it != hitReasons.begin()) reason << ",";
					reason << *it;
				}
				pauseReason = reason.str();
			}
		}
		if (claimValid) return true;
		ReleasePauseClaim();
		return false;
	}
	ReleasePauseClaim();
	return false;
}

void Debugger::SetHookState(std::shared_ptr<HookState> newState) {
	lua_State* L = GetStateForExecution();
	if (!L || !newState) {
		return;
	}

	{
		EMMY_LOCK_GUARD(hookStateMtx);
		hookState = nullptr;
	}
	// Start() may transition to another state (HookStateStop), so do not hold
	// hookStateMtx across the virtual call.
	if (newState->Start(shared_from_this(), L)) {
		EMMY_LOCK_GUARD(hookStateMtx);
		hookState = newState;
	}
}

EmmyDebuggerManager *Debugger::GetEmmyDebuggerManager() {
	return manager;
}

void Debugger::SetVariableArena(Arena<Variable> *arena) {
	arenaRef = arena;
}

Arena<Variable> * Debugger::GetVariableArena() {
	return arenaRef;
}

void Debugger::ClearVariableArenaRef() {
	arenaRef = nullptr;
}

int Debugger::GetStackLevel(bool skipC) const {
	lua_State* L = GetStateForExecution();
	if (!L) {
		return 0;
	}

	int level = 0, i = 0;
	lua_Debug ar{};
	while (lua_getstack(L, i, &ar)) {
		lua_getinfo(L, "l", &ar);
		if (getDebugCurrentLine(&ar) >= 0 || !skipC)
			level++;
		i++;
	}
	return level;
}

void Debugger::AsyncDoString(const std::string &code) {
	doStringList.emplace_back(code);
}

void Debugger::CheckDoString() {
	lua_State* L = GetStateForExecution();
	if (!L) {
		return;
	}


	if (!doStringList.empty()) {
		const bool skip = skipHook.load(std::memory_order_acquire);
		skipHook.store(true, std::memory_order_release);
		const int t = lua_gettop(L);
		for (const auto &code: doStringList) {
			const int r = luaL_loadstring(L, code.c_str());
			if (r == LUA_OK) {
				lua_pcall(L, 0, 0, 0);
			}
			lua_settop(L, t);
		}
		skipHook.store(skip, std::memory_order_release);
		assert(lua_gettop(L) == t);
		doStringList.clear();
	}
}

// message thread
bool Debugger::Eval(std::shared_ptr<EvalContext> evalContext, bool force) {
	if (force)
		return DoEval(evalContext);
	// 加锁
	{
		EMMY_LOCK_GUARD(evalMtx);
		if (!blocking || pendingAction != DebugAction::None || !evalContext || evalQueue.size() >= 256) return false;
		evalQueue.push(evalContext);
	}

	EMMY_COND_NOTIFY_ALL(cvRun);
	return true;
}

int LastLevel(lua_State *L) {
	int level = 0;

	lua_Debug ar;
	while (lua_getstack(L, level, &ar)) {
		level++;
	}

	return level;
}

// host thread
bool Debugger::DoEval(std::shared_ptr<EvalContext> evalContext) {
	lua_State* L = GetStateForExecution();
	if (!L || !evalContext) {
		if (evalContext) evalContext->error = "VM_NOT_READY";
		return false;
	}
	if (evalContext->vmId != 0 &&
		(evalContext->vmId != vmId || !IsPauseActive(evalContext->pauseId) ||
		 (!evalContext->threadId.empty() && !IsPauseThread(evalContext->threadId)) ||
		 evalContext->frameId != GetPauseFrameId(evalContext->stackLevel) ||
		 (evalContext->contextGeneration != 0 && evalContext->contextGeneration != GetContextGeneration()) ||
		 (evalContext->sourceEpoch != 0 && evalContext->sourceEpoch != GetSourceEpoch()))) {
		evalContext->error = "STALE_PAUSE_REFERENCE";
		return false;
	}
	if (evalContext->policy == "VALUE_PATH") {
		return DoRestrictedEval(evalContext);
	}

	int innerLevel = evalContext->stackLevel;

	while (L != nullptr) {
		int level = LastLevel(L);
		if (innerLevel > level) {
			innerLevel -= level;
			L = manager->extension.QueryParentThread(L);
		} else {
			break;
		}
	}

	if (L == nullptr) {
		return false;
	}

	//auto* const L = L;
	// From "cacheId"
	if (evalContext->cacheId > 0) {
		lua_getfield(L, LUA_REGISTRYINDEX, CACHE_TABLE_NAME);// 1: cacheTable|nil
		if (lua_type(L, -1) == LUA_TTABLE) {
			lua_getfield(L, -1, CacheKey(evalContext->cacheId).c_str());// 1: cacheTable, 2: value
			SetVariableArena(evalContext->result.GetArena());
			GetVariable(L, evalContext->result, -1, evalContext->depth);
			ClearVariableArenaRef();
			lua_pop(L, 2);
			return true;
		}
		lua_pop(L, 1);
	}
	// LOAD AS "return expr"
	std::string statement = "return ";
	if (evalContext->setValue) {
		statement = evalContext->expr + " = " + evalContext->value + " return " + evalContext->expr;
	} else {
		statement.append(evalContext->expr);
	}

	// 如果是 aaa:bbbb 则纠正为aaa.bbbb
	int r = luaL_loadstring(L, statement.c_str());
	if (r == LUA_ERRSYNTAX) {
		evalContext->error = "syntax err: ";
		evalContext->error.append(evalContext->expr);
		return false;
	}
	// call
	const int fIdx = lua_gettop(L);
	// create env
	if (!CreateEnv(L, innerLevel))
		return false;
	// setup env
#ifndef EMMY_USE_LUA_SOURCE
	lua_setfenv(L, fIdx);
#elif defined(EMMY_LUA_51) || defined(EMMY_LUA_JIT)
    lua_setfenv(L, fIdx);
#else //52 & 53
    lua_setupvalue(L, fIdx, 1);
#endif
	assert(lua_gettop(L) == fIdx);
	// call function() return expr end
	r = lua_pcall(L, 0, 1, 0);
	if (r == LUA_OK) {
		evalContext->result->name = evalContext->expr;
		SetVariableArena(evalContext->result.GetArena());
		GetVariable(L, evalContext->result, -1, evalContext->depth);
		ClearVariableArenaRef();
		lua_pop(L, 1);
		return true;
	}
	if (r == LUA_ERRRUN) {
		evalContext->error = lua_tostring(L, -1);
	}

	lua_pop(L, 1);
	return false;
}

namespace {

std::string RestrictedPointerText(lua_State* L, int index) {
	std::stringstream stream;
	stream << lua_typename(L, lua_type(L, index)) << "(0x" << std::hex
		<< lua_topointer(L, index) << ")";
	return stream.str();
}

std::string TruncateUtf8Bytes(const std::string& value, std::size_t maxBytes) {
	if (value.size() <= maxBytes) return value;
	std::size_t end = maxBytes;
	// Do not split a UTF-8 continuation byte. Invalid input is conservatively
	// truncated at the byte boundary and remains representable in JSON.
	while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xc0) == 0x80) --end;
	return value.substr(0, end);
}

bool AppendRestrictedText(Variable& variable, const std::string& text,
	std::size_t& bytes, const RestrictedEvalLimits& limits) {
	if (bytes >= static_cast<std::size_t>(limits.maxBytes)) {
		variable.truncated = true;
		return false;
	}
	const std::size_t remaining = static_cast<std::size_t>(limits.maxBytes) - bytes;
	if (text.size() > remaining) {
		variable.value = TruncateUtf8Bytes(text, remaining);
		bytes += variable.value.size();
		variable.truncated = true;
		return false;
	}
	variable.value = text;
	bytes += text.size();
	return true;
}

} // namespace

void Debugger::GetRestrictedVariable(lua_State* L, Idx<Variable> variable, int index,
	int depth, int& nodes, std::size_t& bytes,
	const RestrictedEvalLimits& limits, std::set<const void*>& visited) {
	if (!L || variable.GetArena() == nullptr) return;
	if (nodes >= limits.maxNodes) {
		variable->truncated = true;
		return;
	}
	++nodes;
	index = lua_absindex(L, index);
	const int type = lua_type(L, index);
	variable->valueType = type;
	const char* typeName = lua_typename(L, type);
	variable->valueTypeName = typeName == nullptr ? "unknown" : typeName;

	switch (type) {
		case LUA_TNIL:
			AppendRestrictedText(*variable, "nil", bytes, limits);
			return;
		case LUA_TBOOLEAN:
			AppendRestrictedText(*variable, lua_toboolean(L, index) ? "true" : "false", bytes, limits);
			return;
		case LUA_TNUMBER: {
			const char* value = lua_tostring(L, index);
			AppendRestrictedText(*variable, value == nullptr ? "number" : value, bytes, limits);
			return;
		}
		case LUA_TSTRING: {
			size_t length = 0;
			const char* value = lua_tolstring(L, index, &length);
			const size_t remaining = static_cast<size_t>(limits.maxBytes) - (std::min)(bytes, static_cast<size_t>(limits.maxBytes));
			AppendRestrictedText(*variable, value == nullptr ? "" :
				std::string(value, (std::min)(length, remaining + 1)), bytes, limits);
			if (length > remaining) variable->truncated = true;
			return;
		}
		case LUA_TTABLE: {
			const void* address = lua_topointer(L, index);
			std::stringstream summary;
			summary << "table(0x" << std::hex << address << ")";
			AppendRestrictedText(*variable, summary.str(), bytes, limits);
			if (depth <= 0 || address == nullptr) { variable->truncated = true; return; }
			if (!visited.insert(address).second) {
				variable->truncated = true;
				return;
			}
			const int top = lua_gettop(L);
			lua_pushnil(L);
			while (lua_next(L, index) != 0) {
				if (nodes >= limits.maxNodes || bytes >= static_cast<std::size_t>(limits.maxBytes)) {
					variable->truncated = true;
					lua_pop(L, 1);
					break;
				}
				Idx<Variable> child = variable.GetArena()->Alloc();
				const int keyType = lua_type(L, -2);
				child->nameType = keyType;
				if (keyType == LUA_TSTRING) {
					size_t length = 0;
					const char* key = lua_tolstring(L, -2, &length);
					const size_t remaining = static_cast<size_t>(limits.maxBytes) - bytes;
					child->name = key == nullptr ? "?" : TruncateUtf8Bytes(
						std::string(key, (std::min)(length, remaining + 1)), remaining);
					if (length > remaining) child->truncated = true;
				} else if (keyType == LUA_TNUMBER) {
					// lua_tolstring mutates a number in place. Convert a copy so
					// lua_next receives the original numeric key on the next turn.
					lua_pushvalue(L, -2);
					const char* key = lua_tostring(L, -1);
					child->name = key == nullptr ? "?" : key;
					lua_pop(L, 1);
				} else {
					child->name = RestrictedPointerText(L, -2);
				}
				const size_t remaining = static_cast<size_t>(limits.maxBytes) - bytes;
				if (child->name.size() > remaining) {
					child->name = TruncateUtf8Bytes(child->name, remaining);
					child->truncated = true;
				}
				bytes += child->name.size();
				GetRestrictedVariable(L, child, -1, depth - 1, nodes, bytes, limits, visited);
				if (child->truncated) variable->truncated = true;
				variable->children.push_back(child);
				lua_pop(L, 1); // keep the key for lua_next
			}
			lua_settop(L, top);
			return;
		}
		case LUA_TUSERDATA:
		case LUA_TLIGHTUSERDATA:
		case LUA_TTHREAD:
		case LUA_TFUNCTION:
		default:
			// Never call __tostring or an extension callback in VALUE_PATH mode.
			AppendRestrictedText(*variable, RestrictedPointerText(L, index), bytes, limits);
			return;
	}
}

bool Debugger::DoRestrictedEval(std::shared_ptr<EvalContext> evalContext) {
	if (!evalContext) return false;
	// Revalidate on the Lua owner thread: queued work may outlive its pause or
	// context even after protocol admission succeeded.
	if (!evalContext->requestId.empty() &&
		(!IsPauseActive(evalContext->pauseId) || !IsPauseThread(evalContext->threadId) ||
		 evalContext->stackLevel < 0 || evalContext->frameId != GetPauseFrameId(evalContext->stackLevel) ||
		 (evalContext->contextGeneration != 0 && evalContext->contextGeneration != GetContextGeneration()) ||
		 (evalContext->sourceEpoch != 0 && evalContext->sourceEpoch != GetSourceEpoch()))) {
		evalContext->error = "STALE_PAUSE_REFERENCE";
		return false;
	}
	RestrictedEvalLimits limits;
	limits.maxDepth = evalContext->depth;
	limits.maxNodes = evalContext->maxNodes;
	limits.maxBytes = evalContext->maxBytes;
	std::vector<RestrictedValuePathSegment> segments;
	std::string parseError;
	if (!ParseRestrictedValuePath(evalContext->expr, segments, parseError, limits)) {
		evalContext->error = parseError.empty() ? "EVALUATION_DENIED" : parseError;
		return false;
	}
	if (limits.maxNodes <= 0 || limits.maxBytes <= 0) {
		evalContext->error = "EVALUATION_LIMIT_EXCEEDED";
		return false;
	}
	lua_State* L = GetStateForExecution();
	if (!L) {
		evalContext->error = "VM_NOT_READY";
		return false;
	}
	int innerLevel = evalContext->stackLevel;
	// VALUE_PATH frames come from the raw THREAD_ONLY snapshot; never run the
	// legacy parent-thread extension while evaluating an external request.
	if (L == nullptr || innerLevel < 0) {
		evalContext->error = "STALE_PAUSE_REFERENCE";
		return false;
	}
	const int base = lua_gettop(L);
	auto fail = [&](const char* code) {
		lua_settop(L, base);
		evalContext->error = code;
		return false;
	};
	lua_Debug ar{};
	if (!lua_getstack(L, innerLevel, &ar)) return fail("STALE_PAUSE_REFERENCE");
	if (!evalContext->sourceCanonicalPath.empty() || !evalContext->sourceHash.empty()) {
		if (!lua_getinfo(L, "S", &ar)) return fail("SOURCE_IDENTITY_MISMATCH");
		const std::string chunk = getDebugSource(&ar) == nullptr ? std::string() : getDebugSource(&ar);
		HostSourceIdentity actual;
		const bool registered = EmmyFacade::Get().GetSourceRegistry().Resolve(vmId, GetSourceEpoch(), chunk, actual);
		const std::string path = registered ? actual.canonicalPath : chunk;
		if (!evalContext->sourceCanonicalPath.empty() &&
			SourceRegistry::NormalizePath(path) != SourceRegistry::NormalizePath(evalContext->sourceCanonicalPath)) {
			return fail("SOURCE_IDENTITY_MISMATCH");
		}
		if (!evalContext->sourceHash.empty()) {
			if (!registered) return fail("SOURCE_HASH_UNAVAILABLE");
			if (CompareIgnoreCase(actual.sha256, evalContext->sourceHash) != 0) return fail("SOURCE_IDENTITY_MISMATCH");
		}
	}
	const std::string& root = segments.front().text;
	const bool functionEnvironment = luaVersion == LuaVersion::LUA_51 || luaVersion == LuaVersion::LUA_JIT;
	bool found = false;
	int localEnvironmentIndex = 0;
	int localIndex = 1;
	while (true) {
		const char* name = lua_getlocal(L, &ar, localIndex++);
		if (name == nullptr) break;
		if (root == name) {
			if (found) lua_remove(L, -2); // the later local shadows the previous one
			found = true;
			continue; // keep searching for an inner local with the same name
		}
		if (!functionEnvironment && !found && std::strcmp(name, "_ENV") == 0) {
			if (localEnvironmentIndex != 0) {
				lua_insert(L, localEnvironmentIndex);
				lua_remove(L, localEnvironmentIndex + 1);
			}
			else localEnvironmentIndex = lua_gettop(L);
			continue;
		}
		lua_pop(L, 1);
	}
	if (!found && lua_getinfo(L, "f", &ar)) {
		const int functionIndex = lua_gettop(L);
		int environmentIndex = localEnvironmentIndex;
		int upvalueIndex = 1;
		while (true) {
			const char* name = lua_getupvalue(L, functionIndex, upvalueIndex++);
			if (name == nullptr) break;
			if (root == name) {
				// Discard the function and any saved _ENV, retaining the value.
				lua_insert(L, functionIndex);
				lua_remove(L, functionIndex + 1);
				lua_settop(L, functionIndex);
				found = true;
				break;
			}
			if (!functionEnvironment && environmentIndex == 0 && std::strcmp(name, "_ENV") == 0) {
				environmentIndex = lua_gettop(L);
				continue;
			}
			lua_pop(L, 1);
		}
		if (!found && functionEnvironment) {
#if !defined(EMMY_USE_LUA_SOURCE) || defined(EMMY_LUA_51) || defined(EMMY_LUA_JIT)
#ifndef EMMY_USE_LUA_SOURCE
			if (lua_getfenv == nullptr) return fail("UNSUPPORTED_CAPABILITY");
#endif
			// Lua 5.1/LuaJIT 的 _ENV 只是普通变量；全局名字从函数环境 raw 读取。
			lua_getfenv(L, functionIndex);
			environmentIndex = lua_gettop(L);
#endif
		}
		if (!found && environmentIndex != 0) {
			// nil/非表 _ENV 也必须遮蔽默认全局环境，不能回退泄露错误作用域的值。
			if (!lua_istable(L, environmentIndex)) return fail("VALUE_NOT_FOUND");
			lua_pushstring(L, root.c_str());
			lua_rawget(L, environmentIndex);
			lua_insert(L, functionIndex);
			lua_remove(L, functionIndex + 1);
			lua_settop(L, functionIndex);
			// Missing values in a custom environment must not fall through to _G.
			if (lua_isnil(L, -1)) return fail("VALUE_NOT_FOUND");
			found = true;
		}
		if (!found) lua_pop(L, 1); // function
	}
	if (!found) {
		lua_pushglobaltable(L);
		lua_pushstring(L, root.c_str());
		lua_rawget(L, -2);
		lua_insert(L, -2);
		lua_pop(L, 1); // global table
		found = !lua_isnil(L, -1);
	}
	if (!found) return fail("VALUE_NOT_FOUND");
	for (std::size_t i = 1; i < segments.size(); ++i) {
		if (lua_type(L, -1) != LUA_TTABLE) return fail("VALUE_NOT_FOUND");
		const RestrictedValuePathSegment& segment = segments[i];
		if (segment.kind == RestrictedValuePathSegmentKind::Integer) {
			char* end = nullptr;
			errno = 0;
			const unsigned long long number = std::strtoull(segment.text.c_str(), &end, 10);
			if (errno != 0 || end == segment.text.c_str() || *end != '\0' ||
				number > 9007199254740991ULL) return fail("EVALUATION_LIMIT_EXCEEDED");
			lua_pushnumber(L, static_cast<lua_Number>(number));
		} else {
			lua_pushstring(L, segment.text.c_str());
		}
		lua_rawget(L, -2);
		lua_insert(L, -2);
		lua_pop(L, 1); // previous table
		if (lua_isnil(L, -1)) return fail("VALUE_NOT_FOUND");
	}
	evalContext->result->name = evalContext->expr;
	SetVariableArena(evalContext->result.GetArena());
	int nodes = 0;
	std::size_t bytes = 0;
	std::set<const void*> visited;
	GetRestrictedVariable(L, evalContext->result, -1, limits.maxDepth,
		nodes, bytes, limits, visited);
	ClearVariableArenaRef();
	lua_settop(L, base);
	return true;
}

struct LogMessageReplaceExpress {
public:
	LogMessageReplaceExpress(std::string &&expr, std::size_t startIndex, std::size_t endIndex, bool needEval)
		: Expr(expr),
		  StartIndex(startIndex),
		  EndIndex(endIndex),
		  NeedEval(needEval) {
	}

	std::string Expr;
	std::size_t StartIndex;
	std::size_t EndIndex;
	bool NeedEval;
};

std::string BaseName(const std::string &filePath) {
	std::size_t sepIndex = filePath.find_last_of('/');
	if (sepIndex == std::string::npos) {
		sepIndex = filePath.find_last_of('\\');
		if (sepIndex != std::string::npos) {
			return filePath.substr(sepIndex + 1);
		}
		return filePath;
	} else {
		return filePath.substr(sepIndex + 1);
	}
}

void Debugger::DoLogMessage(std::shared_ptr<BreakPoint> bp) {
	std::string &logMessage = bp->logMessage;
	// 为什么不用regex?
	// 因为gcc 4.8 regex还是空实现
	// 而且后续版本的gcc中正则表达式行为似乎也不太正常
	enum class ParseState {
		Normal,
		LeftBrace,
		RightBrace
	} state = ParseState::Normal;

	std::vector<LogMessageReplaceExpress> replaceExpresses;


	std::size_t leftBraceBegin = 0;

	std::size_t rightBraceBegin = 0;

	// 如果在表达式中出现左大括号
	std::size_t exprLeftCount = 0;


	for (std::size_t index = 0; index != logMessage.size(); index++) {
		char ch = logMessage[index];

		switch (state) {
			case ParseState::Normal: {
				if (ch == '{') {
					state = ParseState::LeftBrace;
					leftBraceBegin = index;
					exprLeftCount = 0;
				} else if (ch == '}') {
					state = ParseState::RightBrace;
					rightBraceBegin = index;
				}
				break;
			}
			case ParseState::LeftBrace: {
				if (ch == '{') {
					// 认为是左双大括号转义为可见的'{'
					if (index == leftBraceBegin + 1) {
						replaceExpresses.emplace_back("{", leftBraceBegin, index, false);
						state = ParseState::Normal;
					} else {
						exprLeftCount++;
					}
				} else if (ch == '}') {
					// 认为是表达式内的大括号
					if (exprLeftCount > 0) {
						exprLeftCount--;
						continue;
					}

					replaceExpresses.emplace_back(logMessage.substr(leftBraceBegin + 1, index - leftBraceBegin - 1),
					                              leftBraceBegin, index, true);


					state = ParseState::Normal;
				}
				break;
			}
			case ParseState::RightBrace: {
				if (ch == '}' && (index == rightBraceBegin + 1)) {
					replaceExpresses.emplace_back("}", rightBraceBegin, index, false);
				} else {
					//认为左右大括号失配，之前的不做处理，退格一位回去重新判断
					index--;
				}
				state = ParseState::Normal;
				break;
			}
		}
	}

	std::stringstream message;

	if (replaceExpresses.empty()) {
		message << logMessage;
	} else {
		// 拼接字符串
		// 怎么replace 函数都没有啊

		std::size_t start = 0;
		for (std::size_t index = 0; index != replaceExpresses.size(); index++) {
			auto &replaceExpress = replaceExpresses[index];
			if (start < replaceExpress.StartIndex) {
				auto fragment = logMessage.substr(start, replaceExpress.StartIndex - start);
				message << fragment;
				start = replaceExpress.StartIndex;
			}

			if (replaceExpress.NeedEval) {
				auto ctx = std::make_shared<EvalContext>();
				ctx->expr = std::move(replaceExpress.Expr);
				ctx->depth = 1;
				bool succeed = DoEval(ctx);
				if (succeed) {
					message << ctx->result->value;
				} else {
					message << ctx->error;
				}
			} else {
				message << replaceExpress.Expr;
			}

			start = replaceExpress.EndIndex + 1;
		}

		if (start < logMessage.size()) {
			auto fragment = logMessage.substr(start, logMessage.size() - start);
			message << fragment;
		}
	}

	std::string baseName = BaseName(bp->file);

	EmmyFacade::Get().SendLog(LogType::Info, "[%s:%d] %s", baseName.c_str(), bp->line, message.str().c_str());
}

std::shared_ptr<BreakPoint> Debugger::FindBreakPoint(lua_Debug *ar) {
	lua_State* L = GetStateForExecution();
	if (!L) {
		return nullptr;
	}

	const int cl = getDebugCurrentLine(ar);
	auto lineSet = manager->GetLineSet();

	if (cl >= 0 && lineSet.find(cl) != lineSet.end()) {
		lua_getinfo(L, "S", ar);
		// Exact AI source checks must use the loaded chunk identity; executing
		// emmy.fixPath here can invoke application code inside a debug hook.
		const std::string chunkname = getDebugSource(ar) == nullptr ? std::string() : getDebugSource(ar);
		return FindBreakPoint(chunkname, cl);
	}
	return nullptr;
}

std::shared_ptr<BreakPoint> Debugger::FindBreakPoint(const std::string &chunkname, int line) {
	std::shared_ptr<BreakPoint> best;
	int maxMatchProcess = 0;

	auto breakpoints = manager->GetBreakpoints();
	for (const auto& bp: breakpoints) {
		if (!bp || bp->line != line || (bp->vmId != 0 && bp->vmId != vmId)) continue;
		if (bp->sourceEpoch != 0 && bp->sourceEpoch != GetSourceEpoch()) continue;
		if (bp->contextGeneration != 0 &&
			bp->contextGeneration != GetContextGeneration()) continue;
		// fuzz match: bp(x/a/b/c), file(a/b/c)
		const std::string& matchFile = bp->sourceCanonicalPath.empty()
			? bp->file : bp->sourceCanonicalPath;
		int matchProcess = 0;
		const bool strictSource = bp->owner != "IDEA" && (bp->owner.find("CLI:") == 0 ||
			bp->owner.find("PROBE:") == 0 || !bp->sourceHash.empty() || bp->sourceEpoch != 0);
		if (strictSource) {
			HostSourceIdentity actual;
			const bool registered = EmmyFacade::Get().GetSourceRegistry().Resolve(vmId, GetSourceEpoch(), chunkname, actual);
			if (!bp->sourceHash.empty() && (!registered || CompareIgnoreCase(actual.sha256, bp->sourceHash) != 0)) continue;
			const auto& actualPath = registered ? actual.canonicalPath : chunkname;
			if (SourceRegistry::NormalizePath(actualPath) != SourceRegistry::NormalizePath(matchFile)) continue;
			matchProcess = static_cast<int>(matchFile.size());
		} else {
			matchProcess = FuzzyMatchFileName(chunkname, matchFile);
		}
		if (matchProcess <= 0) continue;
		if (matchProcess > maxMatchProcess) {
			maxMatchProcess = matchProcess;
			best = std::shared_ptr<BreakPoint>(new BreakPoint(*bp));
			continue;
		}
		if (matchProcess != maxMatchProcess || !best) continue;

		// A global contribution and a VM-specific contribution can share one
		// source location. Merge them into a single logical hit so both causes
		// are evaluated instead of whichever vector entry happened to win.
		const std::string bestFile = best->sourceCanonicalPath.empty() ? best->file : best->sourceCanonicalPath;
		if (CompareIgnoreCase(bestFile, matchFile) != 0) continue;
		if (best->contributions.empty()) {
			BreakPointContribution fallback;
			fallback.owner = best->owner.empty() ? "LEGACY" : best->owner;
			fallback.breakpointId = best->breakpointId;
			fallback.condition = best->condition;
			fallback.logMessage = best->logMessage;
			fallback.hitCondition = best->hitCondition;
			fallback.hitCount = best->hitCount;
			fallback.runToHere = best->runToHere;
			best->contributions.push_back(fallback);
		}
		if (bp->contributions.empty()) {
			BreakPointContribution contribution;
			contribution.owner = bp->owner.empty() ? "LEGACY" : bp->owner;
			contribution.breakpointId = bp->breakpointId;
			contribution.condition = bp->condition;
			contribution.logMessage = bp->logMessage;
			contribution.hitCondition = bp->hitCondition;
			contribution.hitCount = bp->hitCount;
			contribution.runToHere = bp->runToHere;
			best->contributions.push_back(contribution);
		} else {
			best->contributions.insert(best->contributions.end(), bp->contributions.begin(), bp->contributions.end());
		}
		best->composite = true;
	}

	return best;
}

#undef min
bool Debugger::DoHitCondition(std::shared_ptr<BreakPoint> bp) {
	auto &hitCondition = bp->hitCondition;

	enum class ParseState {
		ExpectedOperator,
		// 大于
		Gt,
		// 小于
		Le,
		// 单等号 
		Eq,

		ExpectedHitTimes,

		ParseDigit,

		ParseFinish
	} state = ParseState::ExpectedOperator;

	enum class Operator {
		// 大于
		Gt,
		// 小于
		Le,
		// 小于等于
		LeEq,
		// 大于等于
		GtEq,
		// 双等号
		EqEq,
	} evalOperator = Operator::EqEq;

	unsigned long long hitTimes = 0;

	for (std::size_t index = 0; index != hitCondition.size(); index++) {
		char ch = hitCondition[index];

		switch (state) {
			case ParseState::ExpectedOperator: {
				if (ch == ' ') {
					continue;
				}

				if (ch == '=') {
					state = ParseState::Eq;
				} else if (ch == '<') {
					state = ParseState::Le;
				} else if (ch == '>') {
					state = ParseState::Gt;
				} else {
					return false;
				}

				break;
			}
			case ParseState::Eq: {
				if (ch == '=') {
					evalOperator = Operator::EqEq;
					state = ParseState::ExpectedHitTimes;
				} else {
					return false;
				}
				break;
			}
			case ParseState::Gt: {
				if (ch == '=') {
					evalOperator = Operator::GtEq;
					state = ParseState::ExpectedHitTimes;
				} else if (isdigit(ch)) {
					evalOperator = Operator::Gt;
					hitTimes = ch - '0';
					state = ParseState::ParseDigit;
				} else if (ch == ' ') {
					evalOperator = Operator::Gt;
					state = ParseState::ExpectedHitTimes;
				} else {
					return false;
				}
				break;
			}
			case ParseState::Le: {
				if (ch == '=') {
					evalOperator = Operator::LeEq;
					state = ParseState::ExpectedHitTimes;
				} else if (isdigit(ch)) {
					evalOperator = Operator::Le;
					hitTimes = ch - '0';
					state = ParseState::ParseDigit;
				} else if (ch == ' ') {
					evalOperator = Operator::Le;
					state = ParseState::ExpectedHitTimes;
				} else {
					return false;
				}
				break;
			}
			case ParseState::ExpectedHitTimes: {
				if (ch == ' ') {
					continue;
				} else if (isdigit(ch)) {
					hitTimes = ch - '0';
					state = ParseState::ParseDigit;
				} else {
					return false;
				}
				break;
			}
			case ParseState::ParseDigit: {
				if (isdigit(ch)) {
					hitTimes = hitTimes * 10 + (ch - '0');
				} else if (ch == ' ') {
					state = ParseState::ParseFinish;
				} else {
					return false;
				}

				break;
			}
			case ParseState::ParseFinish: {
				if (ch == ' ') {
					break;
				} else {
					return false;
				}
				break;
			}
		}
	}

	switch (evalOperator) {
		case Operator::EqEq: {
			return bp->hitCount == hitTimes;
		}
		case Operator::Gt: {
			return bp->hitCount > hitTimes;
		}
		case Operator::GtEq: {
			return bp->hitCount >= hitTimes;
		}
		case Operator::Le: {
			return bp->hitCount < hitTimes;
		}
		case Operator::LeEq: {
			return bp->hitCount <= hitTimes;
		}
	}


	return false;
}


// 重写模糊匹配算法
int Debugger::FuzzyMatchFileName(const std::string &chunkName, const std::string &fileName) const {
	auto chunkSize = chunkName.size();
	auto fileSize = fileName.size();


	std::size_t chunkExtSize = 0;
	std::size_t fileExtSize = 0;
	// trim 掉后缀
	for (const auto &ext: manager->extNames) {
		if (EndWith(chunkName, ext)) {
			if (ext.size() > chunkExtSize) {
				chunkExtSize = ext.size();
			}
		}

		if (EndWith(fileName, ext)) {
			if (ext.size() > fileExtSize) {
				fileExtSize = ext.size();
			}
		}
	}

	chunkSize -= chunkExtSize;
	fileSize -= fileExtSize;

	// 我们用chunkname去匹配filename
	int maxMatchSize = static_cast<int>(std::min(chunkSize, fileSize));
	if (maxMatchSize <= 1) {
		return 0;
	}

	int matchProcess = 1;

	for (int i = 1; i != maxMatchSize; i++) {
		char cChar = chunkName[chunkSize - i];
		char fChar = fileName[fileSize - i];

		if (cChar != fChar) {
			// 以下匹配顺序是有意义的，不要轻易改变

			if (::tolower(cChar) == ::tolower(fChar)) {
				continue;
			}

			// 认为来自编辑器的路径不会是相对路径
			// chunkname有可能是(./aaaa)也可能是(aaa/./bbb)
			// 并不匹配(../)的情况
			// 因为 ../的路径意义并不唯一
			if (cChar == '.') {
				std::size_t cLastindex = chunkSize - i + 1;

				if (cLastindex >= chunkSize) {
					matchProcess = 0;
					break;
				}

				char cLastChar = chunkName[cLastindex];
				if (cLastChar != '/' && cLastChar != '\\') {
					matchProcess = 0;
					break;
				}

				// 该值可能为负数
				int cNextIndex = static_cast<int>(chunkSize) - i - 1;
				if (cNextIndex < 0) {
					// 匹配已经完毕
					break;
				}

				char cNextChar = chunkName[cNextIndex];

				// 那chunkname 就是 aaa./bbbb 那就不匹配
				if (cNextChar != '/' && cNextChar != '\\') {
					matchProcess = 0;
					break;
				}

				// 这里会消耗掉 next的匹配
				i++;

				// 这里是指保持下一次循环时fChar不变
				fileSize += 2;
				continue;
			}

			if (cChar == '/' || cChar == '\\') {
				if (fChar == '/' || fChar == '\\') {
					matchProcess++;
					continue;
				}
				// 一些人会写出 require './aaaa' 这种代码
				// 导致lua程序 的chunkname 给的是 .\\/aaaa.lua


				//保持fChar 下次循环时不变
				fileSize++;
				continue;
			}

			// 那就是不匹配
			matchProcess = 0;
			break;
		}
	}

	return matchProcess;
}

void Debugger::ExecuteWithSkipHook(const Executor &exec) {
	const bool skip = skipHook.load(std::memory_order_acquire);
	skipHook.store(true, std::memory_order_release);
	exec(GetStateForExecution());
	skipHook.store(skip, std::memory_order_release);
}

void Debugger::ExecuteOnLuaThread(const Executor &exec) {
	EMMY_LOCK_GUARD(luaThreadMtx);
	luaThreadExecutors.push_back(exec);
}

int Debugger::GetTypeFromName(const char* typeName) {
	if (strcmp(typeName, "nil") == 0) return LUA_TNIL;
	if (strcmp(typeName, "boolean") == 0) return LUA_TBOOLEAN;
	if (strcmp(typeName, "lightuserdata") == 0) return LUA_TLIGHTUSERDATA;
	if (strcmp(typeName, "number") == 0) return LUA_TNUMBER;
	if (strcmp(typeName, "string") == 0) return LUA_TSTRING;
	if (strcmp(typeName, "table") == 0) return LUA_TTABLE;
	if (strcmp(typeName, "function") == 0) return LUA_TFUNCTION;
	if (strcmp(typeName, "userdata") == 0) return LUA_TUSERDATA;
	if (strcmp(typeName, "thread") == 0) return LUA_TTHREAD;
	return -1; // 未知类型
}

bool Debugger::RegisterTypeName(const std::string& typeName, std::string& err) {
	int type = GetTypeFromName(typeName.c_str());
	if (type == -1) {
		err = "Unknown type name: " + typeName;
		return false;
	}
	displayCustomTypeInfo = true;
	registeredTypes.set(type);
	return true;
}
