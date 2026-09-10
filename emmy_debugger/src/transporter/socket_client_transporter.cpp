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
#include <condition_variable>
#include "emmy_debugger/transporter/socket_client_transporter.h"

void OnConnectCB(uv_connect_t* req, int status) {
	const auto thiz = (SocketClientTransporter*)req->data;
	thiz->OnConnection(req, status);
}

static void echo_alloc(uv_handle_t* handle,
                       size_t suggested_size,
                       uv_buf_t* buf) {
	buf->base = static_cast<char*>(malloc(suggested_size));
	buf->len = suggested_size;
}

static void after_read(uv_stream_t* handle,
                       ssize_t nread,
                       const uv_buf_t* buf) {
	auto p = static_cast<Transporter*>(handle->data);
	p->OnAfterRead(handle, nread, buf);
}

SocketClientTransporter::SocketClientTransporter():
	Transporter(false),
	uvClient({}),
	connect_req({}),
	connectionStatus(0) {
}

SocketClientTransporter::~SocketClientTransporter() {
	Stop();
	JoinEventLoop();
	if (loop != nullptr) uv_run(loop, UV_RUN_DEFAULT);
}

int SocketClientTransporter::Stop() {
	Transporter::Stop();
	EMMY_COND_NOTIFY_ALL(cv);
	return 0;
}

void SocketClientTransporter::OnLoopStop() {
	if (clientInitialized) {
		uv_read_stop((uv_stream_t*)&uvClient);
		if (!uv_is_closing((uv_handle_t*)&uvClient)) {
			uv_close((uv_handle_t*)&uvClient, OnClientClosed);
		}
	}
}

void SocketClientTransporter::OnDisconnect() {
	Transporter::OnDisconnect();
	if (clientInitialized && !uv_is_closing((uv_handle_t*)&uvClient))
		uv_close((uv_handle_t*)&uvClient, OnClientClosed);
}

bool SocketClientTransporter::Connect(const std::string& host, int port, std::string& err) {
	if (clientInitialized) {
		err = "socket client is already connected or closing";
		return false;
	}
	connectionNotified = false;
	uvClient.data = this;
	if (uv_tcp_init(loop, &uvClient) != 0) {
		err = "failed to initialize socket client";
		return false;
	}
	clientInitialized = true;
	struct sockaddr_storage addr;
	bool addr_suc = ParseSocketAddress(host, port, &addr, err);
	if (!addr_suc) {
		return false;
	}

	connect_req.data = this;
	const int r = uv_tcp_connect(&connect_req, &uvClient, reinterpret_cast<const struct sockaddr*>(&addr), OnConnectCB);
	if (r) {
		err = uv_strerror(r);
		return false;
	}
	StartEventLoop();
	SRWUniqueLock lock(mutex);
	EMMY_COND_WAIT(cv, lock, [this] { return connectionNotified; });
	if (this->connectionStatus < 0) {
		err = uv_strerror(this->connectionStatus);
	}
	return IsConnected();
}

void SocketClientTransporter::OnConnection(uv_connect_t* req, int status) {
	this->connectionStatus = status;
	if (status >= 0) {
		SetActiveHandler((uv_stream_t*)&uvClient);
		OnConnect(true);
		if (uv_read_start((uv_stream_t*)&uvClient, echo_alloc, after_read) != 0) {
			Stop();
			OnDisconnect();
		}
	}
	else {
		Stop();
		OnConnect(false);
	}
	connectionNotified = true;
	EMMY_COND_NOTIFY_ALL(cv);
}

void SocketClientTransporter::Send(int cmd, const char* data, size_t len) {
	SendActive(cmd, data, len);
}

void SocketClientTransporter::OnClientClosed(uv_handle_t* handle) {
	auto* self = static_cast<SocketClientTransporter*>(handle->data);
	if (self != nullptr) {
		self->InvalidateActiveHandler(reinterpret_cast<uv_stream_t*>(handle));
		self->DropPendingWrites(reinterpret_cast<uv_stream_t*>(handle));
	}
	if (self != nullptr) self->clientInitialized = false;
}
