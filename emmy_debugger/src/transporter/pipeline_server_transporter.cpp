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
#include "emmy_debugger/transporter/pipeline_server_transporter.h"
#include <cassert>

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

void onPipeConnectionCB(uv_stream_t* pipe, int status) {
	const auto t = (PipelineServerTransporter*)pipe->data;
	t->OnPipeConnection(pipe, status);
}

PipelineServerTransporter::PipelineServerTransporter():
	Transporter(true),
	uvClient(nullptr) {
}

PipelineServerTransporter::~PipelineServerTransporter() {
	Stop();
	JoinEventLoop();
	if (loop != nullptr) uv_run(loop, UV_RUN_DEFAULT);
}

bool PipelineServerTransporter::pipe(const std::string& name, std::string& err) {
	if (loop == nullptr) { err = "failed to initialize event loop"; return false; }
	std::string fullName;
#ifdef _WIN32
	{
		fullName = "\\\\.\\pipe\\emmylua-";
		fullName.append(name);
	}
#else
    {
		char tmp[2048];
		size_t len = sizeof(tmp);
		uv_os_tmpdir(tmp, &len);
		fullName = tmp;
		fullName.append("/");
		fullName.append(name);
        uv_fs_t req;
        uv_fs_unlink(nullptr, &req, fullName.c_str(), nullptr);
        uv_fs_req_cleanup(&req);
    }
#endif
	uvServer.data = this;
	if (serverInitialized) { err = "pipe server is already listening"; return false; }
	if (uv_pipe_init(loop, &uvServer, 0) != 0) { err = "failed to initialize pipe server"; return false; }
	serverInitialized = true;
	int r = uv_pipe_bind(&uvServer, fullName.c_str());
	if (r) {
		err = uv_err_name(r);
		return false;
	}
	r = uv_listen((uv_stream_t*)&uvServer, 128, onPipeConnectionCB);
	if (r) {
		err = uv_err_name(r);
		return false;
	}
	StartEventLoop();
	return true;
}

int PipelineServerTransporter::Stop() {
	Transporter::Stop();
	return 0;
}

void PipelineServerTransporter::OnLoopStop() {
	CloseClient();
	if (serverInitialized && !uv_is_closing((uv_handle_t*)&uvServer)) uv_close((uv_handle_t*)&uvServer, OnServerClosed);
}

void PipelineServerTransporter::OnDisconnect() {
	Transporter::OnDisconnect();
	CloseClient();
}

void PipelineServerTransporter::Send(int cmd, const char* data, size_t len) {
	SendActive(cmd, data, len);
}

void PipelineServerTransporter::OnPipeConnection(uv_stream_t* pipe, int status) {
	if (status < 0) {
		Stop();
	}
	else {
		CloseClient();
		if (IsConnected()) OnDisconnect();
		uvClient = (uv_pipe_t*)malloc(sizeof(uv_pipe_t));
		if (uvClient == nullptr) return;
		uv_pipe_init(loop, uvClient, 0);
		uvClient->data = this;
		const int r = uv_accept((uv_stream_t*)&uvServer, (uv_stream_t*)uvClient);
		if (r != 0) { uv_close((uv_handle_t*)uvClient, OnClientClosed); return; }
		SetActiveHandler((uv_stream_t*)uvClient);
		OnConnect(true);
		if (uv_read_start((uv_stream_t*)uvClient, echo_alloc, after_read) != 0) OnDisconnect();
	}
}

void PipelineServerTransporter::CloseClient() {
	if (uvClient == nullptr) return;
	uv_pipe_t* client = uvClient;
	uv_read_stop((uv_stream_t*)client);
	if (!uv_is_closing((uv_handle_t*)client)) uv_close((uv_handle_t*)client, OnClientClosed);
}

void PipelineServerTransporter::OnClientClosed(uv_handle_t* handle) {
	auto* self = static_cast<PipelineServerTransporter*>(handle->data);
	if (self != nullptr) {
		self->InvalidateActiveHandler(reinterpret_cast<uv_stream_t*>(handle));
		self->DropPendingWrites(reinterpret_cast<uv_stream_t*>(handle));
	}
	if (self != nullptr && self->uvClient == reinterpret_cast<uv_pipe_t*>(handle)) self->uvClient = nullptr;
	free(handle);
}

void PipelineServerTransporter::OnServerClosed(uv_handle_t* handle) {
	auto* self = static_cast<PipelineServerTransporter*>(handle->data);
	if (self != nullptr) self->serverInitialized = false;
}
