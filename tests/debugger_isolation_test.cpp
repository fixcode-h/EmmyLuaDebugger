#include "emmy_debugger/debugger/emmy_debugger.h"
#include "emmy_debugger/debugger/emmy_debugger_manager.h"

#include <cstdlib>
#include <iostream>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "debugger isolation test failed: " << message << std::endl;
		std::exit(1);
	}
}

} // namespace

int main() {
	EmmyDebuggerManager manager;
	lua_State* firstState = reinterpret_cast<lua_State*>(0x1000);
	lua_State* secondState = reinterpret_cast<lua_State*>(0x2000);
	auto first = std::make_shared<Debugger>(firstState, &manager, 101);
	auto second = std::make_shared<Debugger>(secondState, &manager, 202);

	Require(first->GetVmId() == 101, "first VM id is preserved");
	Require(second->GetVmId() == 202, "second VM id is preserved");
	Require(first->GetStateBreak().get() != second->GetStateBreak().get(),
		"break states are not shared");
	Require(first->GetStateContinue().get() != second->GetStateContinue().get(),
		"continue states are not shared");
	Require(first->GetStateStepOver().get() != second->GetStateStepOver().get(),
		"step-over states are not shared");
	Require(first->GetStateStepIn().get() != second->GetStateStepIn().get(),
		"step-in states are not shared");
	Require(first->GetStateStepOut().get() != second->GetStateStepOut().get(),
		"step-out states are not shared");
	Require(first->GetStateStop().get() != second->GetStateStop().get(),
		"stop states are not shared");

	first->DoAction(DebugAction::Break);
	second->DoAction(DebugAction::Break);
	Require(first->GetHookState().get() == first->GetStateBreak().get(),
		"first action stays on first debugger");
	Require(second->GetHookState().get() == second->GetStateBreak().get(),
		"second action stays on second debugger");

	// Composite breakpoint storage is independent from callers and preserves
	// global plus VM-specific contributions at the same source location.
	EmmyDebuggerManager breakpointManager;
	std::shared_ptr<BreakPoint> global(new BreakPoint());
	global->file = "C:/game/main.lua";
	global->line = 12;
	global->owner = "USER";
	global->breakpointId = "idea-1";
	global->composite = true;
	BreakPointContribution userContribution;
	userContribution.owner = "USER";
	userContribution.breakpointId = "idea-1";
	global->contributions.push_back(userContribution);
	breakpointManager.AddBreakpoint(global);
	std::shared_ptr<BreakPoint> vm(new BreakPoint(*global));
	vm->vmId = 202;
	vm->owner = "CLI:bot";
	vm->breakpointId = "probe-1";
	vm->contributions.clear();
	BreakPointContribution probeContribution;
	probeContribution.owner = "CLI:bot";
	probeContribution.breakpointId = "probe-1";
	probeContribution.condition = "ready";
	vm->contributions.push_back(probeContribution);
	breakpointManager.AddBreakpoint(vm);
	std::vector<std::shared_ptr<BreakPoint>> stored = breakpointManager.GetBreakpoints();
	Require(stored.size() == 2, "global and VM-specific locations coexist");
	stored[0]->file = "mutated";
	Require(breakpointManager.GetBreakpoints()[0]->file != "mutated",
		"GetBreakpoints returns defensive copies");

	std::shared_ptr<BreakPoint> replacement(new BreakPoint(*global));
	replacement->composite = true;
	replacement->contributions.push_back(userContribution);
	BreakPointContribution cliContribution;
	cliContribution.owner = "CLI:bot";
	cliContribution.breakpointId = "probe-2";
	replacement->contributions.push_back(cliContribution);
	breakpointManager.ReplaceBreakpoints(std::vector<std::shared_ptr<BreakPoint>>{replacement});
	stored = breakpointManager.GetBreakpoints();
	Require(stored.size() == 1 && stored[0]->contributions.size() == 2,
		"replacement deduplicates owner and breakpoint id");
	breakpointManager.RemoveBreakpoint("C:/game/main.lua", 12, "USER", "idea-1");
	stored = breakpointManager.GetBreakpoints();
	Require(stored.size() == 1 && stored[0]->contributions.size() == 1 &&
		stored[0]->contributions[0].owner == "CLI:bot",
		"owner-specific removal preserves other contributions");

	// A single VM may have several Lua coroutines. Only one can own a pause
	// generation; the losing coroutine must not replace its identity/reasons.
	Debugger concurrent(firstState, &manager, 303);
	std::atomic<int> ready(0);
	std::atomic<bool> go(false);
	std::atomic<int> winners(0);
	std::atomic<uint64_t> winnerPause(0);
	std::string winnerReason;
	std::mutex winnerMutex;
	std::vector<std::thread> workers;
	for (int i = 0; i < 8; ++i) {
		workers.emplace_back([&, i] {
			ready.fetch_add(1, std::memory_order_release);
			while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
			lua_State* state = (i % 2 == 0) ? firstState : secondState;
			const std::string reason = "COROUTINE-" + std::to_string(i);
			uint64_t pause = 0;
			if (concurrent.TryBeginPause(state, std::vector<std::string>{reason}, &pause)) {
				winners.fetch_add(1, std::memory_order_relaxed);
				winnerPause.store(pause, std::memory_order_release);
				std::lock_guard<std::mutex> lock(winnerMutex);
				winnerReason = reason;
			}
		});
	}
	while (ready.load(std::memory_order_acquire) != 8) std::this_thread::yield();
	go.store(true, std::memory_order_release);
	for (std::thread& worker : workers) worker.join();
	Require(winners.load(std::memory_order_acquire) == 1, "only one coroutine claims a pause");
	Require(concurrent.GetPauseId() == winnerPause.load(std::memory_order_acquire),
		"pause id belongs to the winning coroutine");
	Require(concurrent.GetPauseReasons().size() == 1 &&
		concurrent.GetPauseReasons()[0] == winnerReason,
		"pause reason is not overwritten by a losing coroutine");
	Require(!concurrent.GetPauseFrameId(0).empty(), "pause frame prefix is published");
	concurrent.ClearPause();
	std::vector<Stack> noStacks;
	Require(!concurrent.GetStacks(noStacks), "stack query without an active pause is rejected");
	Require(concurrent.TryBeginPause(secondState, std::vector<std::string>{"NEXT"}),
		"a new pause can be created after cleanup");
	const uint64_t oldPauseId = concurrent.GetPauseId();
	const uint64_t oldContextGeneration = concurrent.GetContextGeneration();
	const uint64_t oldSourceEpoch = concurrent.GetSourceEpoch();
	Require(concurrent.IsPauseActive(oldPauseId), "pause is active before context reset");
	concurrent.ResetContext();
	Require(!concurrent.IsPauseActive(oldPauseId) && concurrent.GetPauseId() == 0,
		"context reset invalidates the active pause reference");
	Require(concurrent.GetContextGeneration() == oldContextGeneration + 1 &&
		concurrent.GetSourceEpoch() == oldSourceEpoch + 1,
		"debugger context reset advances both identities");
	Require(!concurrent.IsFrameActive("frame-" + std::to_string(oldPauseId) + "-0"),
		"context reset invalidates old pause frame references");
	concurrent.ClearPause();

	std::cout << "debugger isolation tests passed" << std::endl;
	// The full static debugger library owns process-wide Lua/transport singletons;
	// this pure state-isolation harness intentionally skips their shutdown path.
	std::_Exit(0);
}
