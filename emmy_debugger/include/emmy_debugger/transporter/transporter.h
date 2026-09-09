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

#include <atomic>
#include <cstddef>
#include <string>
#include <thread>
#include "uv.h"
#include "nlohmann/json_fwd.hpp"

class EmmyFacade;

enum class MessageCMD : int {
	Unknown = 0,

	InitReq = 1,
	InitRsp = 2,

	ReadyReq = 3,
	ReadyRsp = 4,

	AddBreakPointReq = 5,
	AddBreakPointRsp = 6,

	RemoveBreakPointReq = 7,
	RemoveBreakPointRsp = 8,

	ActionReq = 9,
	ActionRsp = 10,

	EvalReq = 11,
	EvalRsp = 12,

	// debugger -> ide
	BreakNotify = 13,
	AttachedNotify = 14,

	StartHookReq = 15,
	StartHookRsp = 16,

	// debugger -> ide
	LogNotify = 17,

	// Versioned protocol envelope.
	EnvelopeV2 = 18,
};

class Transporter {
	static const size_t kDefaultMaxFrameSize = 1024 * 1024;
	std::thread thread;
	char* buf;
	size_t bufSize;
	size_t receiveSize;
	bool readHead;
	bool running;
	bool connected;
	bool serverMode;
	size_t maxFrameSize;
	std::atomic<bool> disconnectNotified;
	std::atomic<bool> protocolFailed;
protected:
	uv_loop_t* loop;
public:
	Transporter(bool server);
	virtual ~Transporter();
	virtual int Stop();
	bool IsConnected() const;
	bool IsServerMode() const;
	void SetMaxFrameSize(size_t size);
	size_t GetMaxFrameSize() const;
	void Send(int cmd, const nlohmann::json document);
	// void SetHandler(std::shared_ptr<EmmyFacade> facade);
	void OnAfterRead(uv_stream_t* handle, ssize_t nread, const uv_buf_t* buf);
protected:
	virtual void Send(int cmd, const char* data, size_t len) = 0;
	void Send(uv_stream_t* handler, int cmd, const char* data, size_t len);
	// send raw data
	void Send(uv_stream_t* handler, const char* data, size_t len);
	void Receive(const char* data, size_t len);
	bool ProcessBufferedData();
	virtual void OnReceiveMessage(const nlohmann::json document);
	virtual void OnProtocolError(const std::string& reason);
	void ProtocolError(const char* reason);
	void StartEventLoop();
	void Run();
	virtual void OnDisconnect();
	virtual void OnConnect(bool suc);
    // helper for both client and server
	static bool ParseSocketAddress(const std::string &host, int port, sockaddr_storage *addr, std::string &err);
};
