#include "emmy_debugger/debugger/hook_dispatcher.h"

#include <unordered_map>

namespace {
struct HookEntry {
	lua_Hook hostHook = nullptr;
	int hostMask = 0;
	int hostCount = 0;
	lua_Hook debuggerHook = nullptr;
	int debuggerMask = 0;
	int debuggerCount = 0;
};

std::unordered_map<lua_State*, HookEntry> gHooks;

int EventMask(const lua_Debug* ar) {
	if (ar == nullptr) return 0;
		switch (getDebugEvent(const_cast<lua_Debug*>(ar))) {
		case LUA_HOOKCALL: return LUA_MASKCALL;
		case LUA_HOOKRET:
		#ifdef LUA_HOOKTAILRET
		case LUA_HOOKTAILRET: return LUA_MASKRET;
		#endif
		#ifdef LUA_HOOKTAILCALL
		case LUA_HOOKTAILCALL: return LUA_MASKRET;
		#endif
		case LUA_HOOKLINE: return LUA_MASKLINE;
		case LUA_HOOKCOUNT: return LUA_MASKCOUNT;
		default: return 0;
	}
}

void Dispatch(lua_State* L, lua_Debug* ar) {
	auto it = gHooks.find(L);
	if (it == gHooks.end()) return;
	const HookEntry entry = it->second;
	const int eventMask = EventMask(ar);
	if (entry.hostHook != nullptr && (entry.hostMask & eventMask) != 0 &&
		entry.hostHook != Dispatch) {
		entry.hostHook(L, ar);
	}
	if (entry.debuggerHook != nullptr && (entry.debuggerMask & eventMask) != 0 &&
		entry.debuggerHook != Dispatch) {
		entry.debuggerHook(L, ar);
	}
}
}

bool SetDebuggerHook(lua_State* L, lua_Hook debuggerHook, int debuggerMask,
	int debuggerCount) {
	if (L == nullptr || debuggerHook == nullptr || debuggerMask == 0) return false;
	HookEntry entry;
	entry.hostHook = lua_gethook(L);
	entry.hostMask = lua_gethookmask(L);
	entry.hostCount = lua_gethookcount(L);
	entry.debuggerHook = debuggerHook;
	entry.debuggerMask = debuggerMask;
	entry.debuggerCount = debuggerCount;
	gHooks[L] = entry;
	// Lua has one count interval for a state. Preserve the host interval when
	// present; debugger hooks normally use event masks and count == 0.
	int count = entry.hostCount > 0 ? entry.hostCount : debuggerCount;
	lua_sethook(L, Dispatch, entry.hostMask | debuggerMask, count);
	return true;
}

bool ClearDebuggerHook(lua_State* L) {
	if (L == nullptr) return false;
	auto it = gHooks.find(L);
	if (it == gHooks.end()) return false;
	const HookEntry entry = it->second;
	lua_sethook(L, entry.hostHook, entry.hostMask, entry.hostCount);
	gHooks.erase(it);
	return true;
}
