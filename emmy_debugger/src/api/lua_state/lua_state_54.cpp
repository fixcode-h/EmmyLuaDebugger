#include "emmy_debugger/api/lua_state.h"
#ifdef EMMY_USE_LUA_SOURCE
#include "lstate.h"
#else
#include "lua-5.4.6/src/lstate.h"
#endif

lua_State* GetMainState_lua54(lua_State* L)
{
	if (L == nullptr) return nullptr;
#ifdef EMMY_USE_LUA_SOURCE
	return G(L)->mainthread;
#else
	// The loader's private lua_State layout is not proof of the host ABI.
	// Lua 5.4 publishes the main thread through the registry instead.
	if (lua_gettop == nullptr || lua_rawgeti == nullptr ||
		lua_tothread == nullptr || lua_settop == nullptr) return nullptr;
	const int top = (*lua_gettop)(L);
	(*lua_rawgeti)(L, LUA_REGISTRYINDEX, 1 /* LUA_RIDX_MAINTHREAD */);
	lua_State* main = (*lua_tothread)(L, -1);
	(*lua_settop)(L, top);
	return main;
#endif
}

std::vector<lua_State*> FindAllCoroutine_lua54(lua_State* L)
{
#ifndef EMMY_USE_LUA_SOURCE
	// Walking allgc requires an exact private ABI. In loader mode retain only
	// the coroutine observed by the caller; callers can safely track it without
	// treating the bundled 5.4.6 layout as the host's layout.
	if (L == nullptr) return std::vector<lua_State*>();
	return std::vector<lua_State*>(1, L);
#else
	std::vector<lua_State*> result;
	auto head = G(L)->allgc;

	while (head)
	{
		if (head->tt == LUA_TTHREAD)
		{
			result.push_back(reinterpret_cast<lua_State*>(head));
		}
		head = head->next;
	}

	return result;
#endif
}
