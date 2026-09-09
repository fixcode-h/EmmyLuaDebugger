#include "emmy_debugger/debugger/emmy_debugger.h"
#include "emmy_debugger/debugger/emmy_debugger_manager.h"

#include <cstdlib>
#include <iostream>

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

	std::cout << "debugger isolation tests passed" << std::endl;
	// The full static debugger library owns process-wide Lua/transport singletons;
	// this pure state-isolation harness intentionally skips their shutdown path.
	std::_Exit(0);
}
