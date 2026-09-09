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
#include "emmy_debugger/debugger/emmy_debugger_lib.h"
#include <cstring>
#include <cstddef>
#include "emmy_debugger/debugger/emmy_debugger.h"
#include "emmy_debugger/emmy_facade.h"

namespace {

bool IsValidHostMetadata(const EmmyHostVmMetadata* metadata) {
	return metadata != nullptr &&
		metadata->size >= sizeof(uint32_t) * 2 &&
		metadata->version == EMMY_HOST_API_VERSION;
}

bool HasHostMetadataField(const EmmyHostVmMetadata* metadata, std::size_t offset) {
	return IsValidHostMetadata(metadata) &&
		metadata->size >= offset + sizeof(const char*);
}

const char* ReadHostMetadataField(const EmmyHostVmMetadata* metadata, std::size_t offset) {
	if (!HasHostMetadataField(metadata, offset)) {
		return nullptr;
	}
	const char* const* field = reinterpret_cast<const char* const*>(
		reinterpret_cast<const unsigned char*>(metadata) + offset);
	return *field;
}

} // namespace

// emmy.tcpListen(host: string, port: int): bool
int tcpListen(struct lua_State* L)
{
	luaL_checkstring(L, 1);
	std::string err;
	const auto host = lua_tostring(L, 1);
	luaL_checknumber(L, 2);
	const auto port = lua_tointeger(L, 2);
	const auto suc = EmmyFacade::Get().TcpListen(L, host, static_cast<int>(port), err);
	lua_pushboolean(L, suc);
	if (suc) return 1;
	lua_pushstring(L, err.c_str());
	return 2;
}

// emmy.tcpConnect(host: string, port: int): bool
int tcpConnect(lua_State* L)
{
	luaL_checkstring(L, 1);
	std::string err;
	const auto host = lua_tostring(L, 1);
	luaL_checknumber(L, 2);
	const auto port = lua_tointeger(L, 2);
	const auto suc = EmmyFacade::Get().TcpConnect(L, host, static_cast<int>(port), err);
	lua_pushboolean(L, suc);
	if (suc) return 1;
	lua_pushstring(L, err.c_str());
	return 2;
}

// emmy.pipeListen(pipeName: string): bool
int pipeListen(lua_State* L)
{
	luaL_checkstring(L, 1);
	std::string err;
	const auto pipeName = lua_tostring(L, 1);
	const auto suc = EmmyFacade::Get().PipeListen(L, pipeName, err);
	lua_pushboolean(L, suc);
	if (suc) return 1;
	lua_pushstring(L, err.c_str());
	return 2;
}

// emmy.pipeConnect(pipeName: string): bool
int pipeConnect(lua_State* L)
{
	luaL_checkstring(L, 1);
	std::string err;
	const auto pipeName = lua_tostring(L, 1);
	const auto suc = EmmyFacade::Get().PipeConnect(L, pipeName, err);
	lua_pushboolean(L, suc);
	if (suc) return 1;
	lua_pushstring(L, err.c_str());
	return 2;
}

// emmy.breakHere(): bool
int breakHere(lua_State* L)
{
	const bool suc = EmmyFacade::Get().BreakHere(L);
	lua_pushboolean(L, suc);
	return 1;
}

// emmy.waitIDE(timeout: number): void
int waitIDE(lua_State* L)
{
	int top = lua_gettop(L);
	int timeout = 0;
	if (top >= 1)
	{
		timeout = static_cast<int>(luaL_checknumber(L, 1));
	}
	EmmyFacade::Get().WaitIDE(false, timeout);
	return 0;
}

int tcpSharedListen(lua_State* L)
{
	luaL_checkstring(L, 1);
	std::string err;
	const auto host = lua_tostring(L, 1);
	luaL_checknumber(L, 2);
	const auto port = lua_tointeger(L, 2);
	const auto suc = EmmyFacade::Get().TcpSharedListen(L, host, static_cast<int>(port), err);
	lua_pushboolean(L, suc);
	if (suc) return 1;
	lua_pushstring(L, err.c_str());
	return 2;
}


// emmy.stop()
int stop(lua_State* L)
{
	EmmyFacade::Get().Destroy();
	return 0;
}

// emmy.registerTypeName(typeName: string): bool
int registerTypeName(lua_State* L)
{
	luaL_checkstring(L, 1);
	std::string err;
	const auto typeName = lua_tostring(L, 1);
	const auto suc = EmmyFacade::Get().RegisterTypeName(L, typeName, err);
	lua_pushboolean(L, suc);
	if (suc) return 1;
	lua_pushstring(L, err.c_str());
	return 2;
}

// emmy.sendLog(type: int, message: string): void
// type: 0=Debug, 1=Info, 2=Warning, 3=Error
int sendLog(lua_State* L)
{
	luaL_checknumber(L, 1);
	luaL_checkstring(L, 2);
	const auto type = static_cast<int>(lua_tointeger(L, 1));
	const auto message = lua_tostring(L, 2);
	
	LogType logType = LogType::Info;
	switch (type) {
		case 0: logType = LogType::Debug; break;
		case 1: logType = LogType::Info; break;
		case 2: logType = LogType::Warning; break;
		case 3: logType = LogType::Error; break;
		default: logType = LogType::Info; break;
	}
	
	EmmyFacade::Get().SendLog(logType, "%s", message);
	return 0;
}

int gc(lua_State* L)
{
	EmmyFacade::Get().OnLuaStateGC(L);
	return 0;
}

void handleStateClose(lua_State* L)
{
	lua_getfield(L, LUA_REGISTRYINDEX, "__EMMY__GC__");
	int isNil = lua_isnil(L, -1);
	lua_pop(L, 1);

	if (!isNil)
	{
		return;
	}

	lua_newtable(L);
	lua_pushcfunction(L, gc);
	lua_setfield(L, -2, "__gc");

	lua_newuserdata(L, 1);
	lua_pushvalue(L, -2);
	lua_setmetatable(L, -2);

	lua_setfield(L, LUA_REGISTRYINDEX, "__EMMY__GC__");

	lua_pop(L, 1);
}


bool install_emmy_debugger(struct lua_State* L)
{
#ifndef EMMY_USE_LUA_SOURCE
	if (!EmmyFacade::Get().SetupLuaAPI())
	{
		return false;
	}
#endif
	// register helper lib
	EmmyFacade::Get().GetDebugManager().extension.Initialize(L);
	handleStateClose(L);
	return true;
}

std::string prepareEvalExpr(const std::string& eval)
{
	if (eval.empty())
	{
		return eval;
	}

	int lastIndex = static_cast<int>(eval.size() - 1);

	for (int i = lastIndex; i >= 0; i--)
	{
		auto ch = eval[i];
		if (ch > 0)
		{
			if (!isalnum(ch) && ch != '_')
			{
				if (ch == ':')
				{
					auto newString = eval;
					newString[i] = '.';
					return eval;
				}
			}
		}
	}
	return eval;
}

extern "C" {

EMMY_HOST_API_EXPORT uint64_t EMMY_HOST_API_CALL Emmy_RegisterLuaVm(
	lua_State* L, const EmmyHostVmMetadata* metadata) {
	if (L == nullptr || (metadata != nullptr && !IsValidHostMetadata(metadata))) {
		return 0;
	}
	VmMetadata internal;
	internal.discovery = "HOST_API";
	if (metadata != nullptr) {
		const char* displayName = ReadHostMetadataField(metadata, offsetof(EmmyHostVmMetadata, displayName));
		const char* engineName = ReadHostMetadataField(metadata, offsetof(EmmyHostVmMetadata, engineName));
		const char* engineContext = ReadHostMetadataField(metadata, offsetof(EmmyHostVmMetadata, engineContext));
		const char* luaVersionHint = ReadHostMetadataField(metadata, offsetof(EmmyHostVmMetadata, luaVersionHint));
		const char* runtimeModule = ReadHostMetadataField(metadata, offsetof(EmmyHostVmMetadata, runtimeModule));
		if (displayName != nullptr) internal.displayName = displayName;
		if (engineName != nullptr) internal.engineName = engineName;
		if (engineContext != nullptr) internal.engineContext = engineContext;
		if (luaVersionHint != nullptr) internal.luaVersionHint = luaVersionHint;
		if (runtimeModule != nullptr) internal.runtimeModule = runtimeModule;
	}
	return EmmyFacade::Get().RegisterLuaVm(L, internal);
}

EMMY_HOST_API_EXPORT int EMMY_HOST_API_CALL Emmy_NotifyLuaVmReady(uint64_t registrationId) {
	return EmmyFacade::Get().NotifyLuaVmReady(registrationId) ? 1 : 0;
}

EMMY_HOST_API_EXPORT int EMMY_HOST_API_CALL Emmy_BeginLuaVmClose(
	uint64_t registrationId, const char* reason) {
	return EmmyFacade::Get().BeginLuaVmClose(
		registrationId, reason == nullptr ? std::string() : std::string(reason)) ? 1 : 0;
}

EMMY_HOST_API_EXPORT int EMMY_HOST_API_CALL Emmy_EndLuaVmClose(uint64_t registrationId) {
	return EmmyFacade::Get().EndLuaVmClose(registrationId) ? 1 : 0;
}

EMMY_HOST_API_EXPORT int EMMY_HOST_API_CALL Emmy_ReleaseLuaVmRegistration(uint64_t registrationId) {
	return EmmyFacade::Get().ReleaseLuaVmRegistration(registrationId) ? 1 : 0;
}

EMMY_HOST_API_EXPORT int EMMY_HOST_API_CALL Emmy_SetLuaVmDisplayName(
	uint64_t registrationId, const char* displayName) {
	return EmmyFacade::Get().SetLuaVmDisplayName(
		registrationId, displayName == nullptr ? std::string() : std::string(displayName)) ? 1 : 0;
}

} // extern "C"
