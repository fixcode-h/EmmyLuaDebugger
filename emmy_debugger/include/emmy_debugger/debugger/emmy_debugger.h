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

#include <cstdint>
#include <cstddef>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <memory>
#include <map>
#include <set>
#include <bitset>

#include "emmy_debugger/api/lua_api.h"
#include "emmy_debugger/platform/lock.h"
#include "hook_state.h"
#include "emmy_debugger/proto/proto.h"
#include "emmy_debugger/proto/restricted_eval.h"
#include "emmy_debugger/arena/arena.h"

using Executor = std::function<void(lua_State* L)>;
class EmmyDebuggerManager;

enum class PauseScope {
	Thread,
	Vm,
};

class Debugger: public std::enable_shared_from_this<Debugger>
{
public:
	Debugger(lua_State* L, EmmyDebuggerManager* manager, uint64_t vmId = 0);
	~Debugger();

	void Start();

	void Attach();
	/*
	 * 主lua state 销毁时，所做工作
	 */
	void Detach();
	// Clears all references that belong to the current Lua context without
	// destroying the debugger/VM registration. The host must call this at a
	// hot-reload or PIE reset boundary before exposing the new context.
	void ResetContext();

	void SetCurrentState(lua_State* L);
	uint64_t GetVmId() const;
	void SetVmId(uint64_t vmId);
	void SetContextIdentity(uint64_t contextGeneration, uint64_t sourceEpoch);
	uint64_t GetContextGeneration() const;
	uint64_t GetSourceEpoch() const;
	uint64_t GetPauseId() const;
	bool IsPauseActive(uint64_t pauseId = 0) const;
	bool IsPauseThread(const std::string& threadId) const;
	bool IsFrameActive(const std::string& frameId) const;
	void ClearPause();
	// Atomically creates a pause record for the supplied coroutine. This is
	// also the state-only seam used by lifecycle/concurrency fixtures; callers
	// that need to publish a break still go through HandleBreak().
	bool TryBeginPause(lua_State* breakState,
					   const std::vector<std::string>& reasons = std::vector<std::string>(),
					   uint64_t* pauseId = nullptr);
	std::string GetPauseThreadId() const;
	PauseScope GetPauseScope() const;
	std::string GetPauseConsistency() const;
	std::string GetPauseReason() const;
	std::vector<std::string> GetPauseReasons() const;
	std::string GetPauseFrameId(int stackLevel) const;
	std::shared_ptr<HookStateBreak> GetStateBreak() const;
	std::shared_ptr<HookStateContinue> GetStateContinue() const;
	std::shared_ptr<HookStateStepOver> GetStateStepOver() const;
	std::shared_ptr<HookStateStepIn> GetStateStepIn() const;
	std::shared_ptr<HookStateStepOut> GetStateStepOut() const;
	std::shared_ptr<HookStateStop> GetStateStop() const;
	/*
	 * hook时调用
	 */
	void Hook(lua_Debug* ar, lua_State* L);

	/*
	 * 停止调试时调用
	 */
	void Stop();
	/*
	 * 调试器是否运行
	 */
	bool IsRunning() const;
	/*
	 * 判断当前使用的lua_state 是否是main state
	 */
	bool IsMainCoroutine(lua_State* L) const;
    /*
	 * 推迟到lua线程执行
	 */
	void AsyncDoString(const std::string& code);
	bool Eval(std::shared_ptr<EvalContext> evalContext, bool force = false);
	bool GetStacks(std::vector<Stack>& stacks, std::size_t maxFrames = 1024,
				   bool* truncated = nullptr, bool rawSnapshot = false);
	void GetVariable(lua_State* L, Idx<Variable> variable, int index, int depth, bool queryHelper = true);
	void DoAction(DebugAction action);
	// Message-thread admission only; step initialization reads Lua on the owner thread.
	bool RequestAction(DebugAction action, uint64_t pauseId = 0);
	void EnterDebugMode();
	void ExitDebugMode();
	void ExecuteWithSkipHook(const Executor& exec);
	void ExecuteOnLuaThread(const Executor& exec);
	// L is the coroutine that actually triggered the break.  Keeping it
	// explicit avoids a racing coroutine replacing currentL before the pause
	// record is created.  The default keeps the legacy call sites source
	// compatible.
	void HandleBreak(lua_State* L = nullptr);
	int GetStackLevel(bool skipC) const;
	/*
	 * 更新hook
	 */
	void UpdateHook(int mask, lua_State* L);

	/*
	 * 设置当前状态机，他的锁由doAction负责
	 */
	void SetHookState(std::shared_ptr<HookState> newState);
	std::shared_ptr<HookState> GetHookState() const;
	EmmyDebuggerManager* GetEmmyDebuggerManager();

	void SetVariableArena(Arena<Variable> *arena);

	Arena<Variable> *GetVariableArena();

	void ClearVariableArenaRef();

	bool RegisterTypeName(const std::string& typeName, std::string& err);

private:
	bool TryClaimPause(lua_State* breakState);
	void ReleasePauseClaim();
	bool CommitClaimedPause(lua_State* breakState,
						 const std::vector<std::string>& reasons,
						 uint64_t* pauseId = nullptr);
	std::shared_ptr<BreakPoint> FindBreakPoint(lua_Debug* ar);
	std::shared_ptr<BreakPoint> FindBreakPoint(const std::string& file, int line);
	std::string GetFile(lua_Debug* ar, lua_State* state = nullptr) const;
	lua_State* GetStateForExecution() const;

	void CheckDoString();
	bool CreateEnv(lua_State* L, int stackLevel);
	bool ProcessBreakPoint(std::shared_ptr<BreakPoint> bp, lua_State* breakState);
	bool DoEval(std::shared_ptr<EvalContext> evalContext);
	bool DoRestrictedEval(std::shared_ptr<EvalContext> evalContext);
	void GetRestrictedVariable(lua_State* L, Idx<Variable> variable, int index,
		int depth, int& nodes, std::size_t& bytes,
		const RestrictedEvalLimits& limits, std::set<const void*>& visited);
	void DoLogMessage(std::shared_ptr<BreakPoint> bp);
	bool DoHitCondition(std::shared_ptr<BreakPoint> bp);
	// 模糊匹配算法会算出匹配度
	// 当多个文件路径都有可能命中应该采用匹配度最高的路径
	int FuzzyMatchFileName(const std::string& chunkName, const std::string& fileName) const;
	void CacheValue(int valueIndex, Idx<Variable> variable, lua_State* state = nullptr) const;
	// bool HasCacheValue(int valueIndex) const;
	void ClearCache(lua_State* state = nullptr) const;
	std::string CacheKey(int cacheId) const;

	int GetTypeFromName(const char* typeName);

	lua_State* currentL;
	lua_State* mainL;

	EmmyDebuggerManager* manager;
	std::atomic<uint64_t> vmId;
	std::atomic<uint64_t> pauseIdCounter;
	std::atomic<uint64_t> activePauseId;
	std::atomic<uint64_t> contextGeneration;
	std::atomic<uint64_t> sourceEpoch;
	std::string pauseThreadId;
	PauseScope pauseScope;
	std::string pauseConsistency;
	std::string pauseReason;
	std::vector<std::string> pauseReasons;
	std::string pauseFramePrefix;
	// State captured at the instant of the pause. A different coroutine may
	// continue in THREAD_ONLY mode, but cannot replace the current snapshot.
	lua_State* pausedL;
	// A breakpoint condition can execute Lua before the pause is committed.
	// Claiming the slot first prevents another coroutine from racing in and
	// replacing the eventual pause reasons or snapshot identity.
	bool pauseClaimed;
	mutable EmmyMutex pauseMtx = EMMY_MUTEX_INIT;
	mutable EmmyMutex stateMtx = EMMY_MUTEX_INIT;

	// 使用平台相关的锁类型
	mutable EmmyMutex hookStateMtx = EMMY_MUTEX_INIT;
	EmmyMutex runMtx = EMMY_MUTEX_INIT;
	EmmyMutex luaThreadMtx = EMMY_MUTEX_INIT;
	EmmyMutex evalMtx = EMMY_MUTEX_INIT;
	EmmyMutex breakpointMtx = EMMY_MUTEX_INIT;
	EmmyCondVar cvRun = EMMY_CONDVAR_INIT;

	std::shared_ptr<HookState> hookState;
	std::shared_ptr<HookStateBreak> stateBreak;
	std::shared_ptr<HookStateContinue> stateContinue;
	std::shared_ptr<HookStateStepOver> stateStepOver;
	std::shared_ptr<HookStateStepIn> stateStepIn;
	std::shared_ptr<HookStateStepOut> stateStepOut;
	std::shared_ptr<HookStateStop> stateStop;

	std::atomic<bool> running;
	std::atomic<bool> skipHook;
	std::atomic<bool> blocking;
	std::atomic<bool> helperLoaded;  // 标记 helperCode 是否已加载，防止重复执行

	std::vector<std::string> doStringList;

	std::vector<Executor> luaThreadExecutors;

	std::queue<std::shared_ptr<EvalContext>> evalQueue;
	DebugAction pendingAction = DebugAction::None; // protected by evalMtx
	// Hit counters are debugger/VM scoped even when the composite breakpoint
	// object is shared by several VMs.
	std::map<std::string, int> contributionHitCounts;

	Arena<Variable> *arenaRef;
	mutable std::atomic<int> cacheIdCounter;
	// Cache keys are namespaced by context so an old cache id can never alias
	// a newly created value after a hot reload/PIE reset.
	mutable std::atomic<uint64_t> cacheGeneration;

	bool displayCustomTypeInfo;
	std::bitset<LUA_NUMTAGS> registeredTypes;
};
