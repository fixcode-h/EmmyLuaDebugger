#include "emmy_debugger/debugger/hook_dispatcher.h"

#include <unordered_map>
#include <mutex>
#include "emmy_debugger/api/lua_version.h"

namespace {
struct HookEntry {
	lua_State* mainState = nullptr;
	lua_Hook hostHook = nullptr;
	int hostMask = 0;
	int hostCount = 0;
	lua_Hook debuggerHook = nullptr;
	int debuggerMask = 0;
	int debuggerCount = 0;
};

std::unordered_map<lua_State*, HookEntry> gHooks;
std::mutex gHooksMutex;

int EventMask(const lua_Debug* ar) {
	if (ar == nullptr) return 0;
		switch (getDebugEvent(const_cast<lua_Debug*>(ar))) {
		case LUA_HOOKCALL: return LUA_MASKCALL;
		case LUA_HOOKRET:
			return LUA_MASKRET;
		case 4: return luaVersion == LuaVersion::LUA_51 || luaVersion == LuaVersion::LUA_JIT
			? LUA_MASKRET : LUA_MASKCALL;
		case LUA_HOOKLINE: return LUA_MASKLINE;
		case LUA_HOOKCOUNT: return LUA_MASKCOUNT;
		default: return 0;
	}
}

void Dispatch(lua_State* L, lua_Debug* ar) {
	HookEntry entry;
	// Newly created coroutines inherit the VM's native hook. Resolve their
	// inherited dispatcher through the public main-thread registry entry.
	auto mainState = GetMainState(L);
	{
		std::lock_guard<std::mutex> lock(gHooksMutex);
		auto it = gHooks.find(L);
		if (it == gHooks.end() && mainState != nullptr) it = gHooks.find(mainState);
		if (it == gHooks.end()) return;
		entry = it->second;
	}
	// The main state may already have detached while this coroutine still
	// carries the inherited dispatcher. Restore it on its own owner thread.
	if (entry.debuggerHook == nullptr && lua_gethook(L) == Dispatch) {
		lua_sethook(L, entry.hostHook, entry.hostMask, entry.hostCount);
	}
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
	const lua_Hook currentHook = lua_gethook(L);
	const int currentMask = lua_gethookmask(L);
	const int currentCount = lua_gethookcount(L);
	auto mainState = GetMainState(L);
	{
		std::lock_guard<std::mutex> lock(gHooksMutex);
		auto existing = gHooks.find(L);
		if (existing == gHooks.end() && currentHook == Dispatch && mainState != nullptr) {
			existing = gHooks.find(mainState);
		}
		if (existing != gHooks.end()) {
			entry = existing->second;
			if (currentHook != Dispatch) {
				entry.hostHook = currentHook;
				entry.hostMask = currentMask;
				entry.hostCount = currentCount;
			}
		}
	}
	if (entry.hostHook == nullptr && currentHook != Dispatch) {
		entry.hostHook = currentHook;
		entry.hostMask = currentMask;
		entry.hostCount = currentCount;
	}
	entry.debuggerHook = debuggerHook;
	entry.mainState = mainState == nullptr ? L : mainState;
	entry.debuggerMask = debuggerMask;
	entry.debuggerCount = debuggerCount;
	{
		std::lock_guard<std::mutex> lock(gHooksMutex);
		gHooks[L] = entry;
	}
	// Lua has one count interval for a state. Preserve the host interval when
	// present; debugger hooks normally use event masks and count == 0.
	int count = entry.hostCount > 0 ? entry.hostCount : debuggerCount;
	lua_sethook(L, Dispatch, entry.hostMask | debuggerMask, count);
	return true;
}

bool ClearDebuggerHook(lua_State* L) {
	if (L == nullptr) return false;
	HookEntry entry;
	auto mainState = GetMainState(L);
	{
		std::lock_guard<std::mutex> lock(gHooksMutex);
		auto it = gHooks.find(L);
		if (it != gHooks.end()) {
			entry = it->second;
			if (entry.mainState == L) {
				it->second.debuggerHook = nullptr;
				it->second.debuggerMask = 0;
				it->second.debuggerCount = 0;
			} else {
				gHooks.erase(it);
			}
		} else {
			auto mainIt = mainState == nullptr ? gHooks.end() : gHooks.find(mainState);
			if (mainIt == gHooks.end()) return false;
			entry = mainIt->second;
		}
	}
	if (lua_gethook(L) == Dispatch) {
		lua_sethook(L, entry.hostHook, entry.hostMask, entry.hostCount);
	}
	return true;
}

void ForgetDebuggerHooks(lua_State* mainState) {
	std::lock_guard<std::mutex> lock(gHooksMutex);
	for (auto it = gHooks.begin(); it != gHooks.end();) {
		if (it->first == mainState || it->second.mainState == mainState) it = gHooks.erase(it);
		else ++it;
	}
}
