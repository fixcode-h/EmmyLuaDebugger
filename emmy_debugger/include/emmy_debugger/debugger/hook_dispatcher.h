#pragma once

#include "emmy_debugger/api/lua_api.h"

// Installs a Lua-owner-thread dispatcher while preserving the hook that was
// already installed by the host. Set/Clear must be called on that same Lua
// owner thread; the dispatcher never reads lua_State from another thread.
bool SetDebuggerHook(lua_State* L, lua_Hook debuggerHook, int debuggerMask,
	int debuggerCount = 0);
bool ClearDebuggerHook(lua_State* L);
// Metadata-only cleanup after the host has closed the complete Lua VM.
void ForgetDebuggerHooks(lua_State* mainState);
