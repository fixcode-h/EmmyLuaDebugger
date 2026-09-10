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
#include "emmy_debugger/transporter/transporter.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include "emmy_debugger/emmy_facade.h"
#include "nlohmann/json.hpp"

Transporter::Transporter(bool server):
	receiveSize(0),
	readHead(true),
	running(false),
	connected(false),
	serverMode(server),
	maxFrameSize(kDefaultMaxFrameSize),
	disconnectNotified(false),
	protocolFailed(false)
{
	loop = uv_loop_new();
	asyncInitialized.store(false);
	stopRequested.store(false);
	bufSize = 10 * 1024;
	buf = static_cast<char*>(malloc(bufSize));
}

Transporter::~Transporter()
{
	Stop();
	JoinEventLoop();
	if (buf)
	{
		free(buf);
	}
	{
		std::lock_guard<std::mutex> lock(sendMutex);
		for (std::deque<PendingWrite>::iterator it = sendQueue.begin(); it != sendQueue.end(); ++it)
			free(it->data);
		sendQueue.clear();
	}
	if (loop != nullptr) {
		uv_loop_close(loop);
		delete loop;
		loop = nullptr;
	}
}

void Transporter::Send(int cmd, const nlohmann::json document)
{
	std::string documentText = document.dump(-1, ' ', false, nlohmann::detail::error_handler_t::ignore);
	if (documentText.size() > maxFrameSize) {
		ProtocolError("outgoing_frame_too_large");
		return;
	}
	Send(cmd, documentText.data(), documentText.size());
}

void Transporter::OnAfterRead(uv_stream_t* handle, ssize_t nread, const uv_buf_t* buf)
{
	if (buf == nullptr) {
		return;
	}
	if (nread < 0)
	{
		/* Error or EOF */
		free(buf->base);
		if (handle != nullptr) {
			uv_read_stop(handle);
			uv_handle_t* uvHandle = reinterpret_cast<uv_handle_t*>(handle);
			if (!uv_is_closing(uvHandle)) {
				uv_close(uvHandle, nullptr);
			}
		}

		// on disconnect
		OnDisconnect();
		return;
	}

	if (nread == 0)
	{
		/* Everything OK, but nothing read. */
		free(buf->base);
		return;
	}

	Receive(buf->base, nread);
	free(buf->base);
}

void Transporter::Receive(const char* data, size_t len)
{
	if (data == nullptr || len == 0 || protocolFailed.load(std::memory_order_acquire)) {
		return;
	}
	const size_t maxBufferedBytes = maxFrameSize + 64;
	size_t offset = 0;
	while (offset < len && !protocolFailed.load(std::memory_order_acquire)) {
		if (receiveSize >= maxBufferedBytes) {
			if (!ProcessBufferedData() || receiveSize >= maxBufferedBytes) {
				ProtocolError("frame_buffer_limit_exceeded");
				return;
			}
		}

		size_t available = maxBufferedBytes - receiveSize;
		size_t chunk = len - offset;
		if (chunk > available) chunk = available;
		const size_t required = receiveSize + chunk;
		if (required > bufSize) {
			size_t newSize = bufSize == 0 ? 1024 : bufSize;
			while (newSize < required && newSize < maxBufferedBytes) {
				const size_t doubled = newSize * 2;
				newSize = doubled > newSize ? doubled : required;
			}
			if (newSize < required || newSize > maxBufferedBytes) {
				ProtocolError("frame_buffer_allocation_failed");
				return;
			}
			char* resized = static_cast<char*>(realloc(buf, newSize));
			if (resized == nullptr) {
				ProtocolError("frame_buffer_allocation_failed");
				return;
			}
			buf = resized;
			bufSize = newSize;
		}
		memcpy(buf + receiveSize, data + offset, chunk);
		receiveSize = required;
		offset += chunk;
		if (!ProcessBufferedData()) return;
	}
}

bool Transporter::ProcessBufferedData()
{
	if (protocolFailed.load(std::memory_order_acquire)) return false;
	size_t consumed = 0;
	while (consumed < receiveSize) {
		size_t newline = consumed;
		while (newline < receiveSize && buf[newline] != '\n') {
			++newline;
		}
		if (newline == receiveSize) {
			if (receiveSize - consumed > maxFrameSize) {
				ProtocolError("frame_line_too_large");
				return false;
			}
			break;
		}

		const size_t lineLength = newline - consumed;
		if (lineLength > maxFrameSize) {
			ProtocolError("frame_line_too_large");
			return false;
		}

		std::string line(buf + consumed, lineLength);
		consumed = newline + 1;
		if (readHead) {
			if (line.empty()) {
				ProtocolError("empty_command_line");
				return false;
			}
			char* end = nullptr;
			errno = 0;
			const long command = std::strtol(line.c_str(), &end, 10);
			if (errno != 0 || end == line.c_str() || *end != '\0' ||
				command < 0 || command > 2147483647L) {
				ProtocolError("invalid_command_line");
				return false;
			}
			readHead = false;
		} else {
			try {
				auto document = nlohmann::json::parse(line);
				OnReceiveMessage(document);
			} catch (const std::exception&) {
				ProtocolError("invalid_json");
				return false;
			}
			readHead = true;
		}
	}

	if (consumed > 0) {
		const size_t remaining = receiveSize - consumed;
		if (remaining > 0) {
			memmove(buf, buf + consumed, remaining);
		}
		receiveSize = remaining;
	}
	return !protocolFailed.load(std::memory_order_acquire);
}

void Transporter::OnReceiveMessage(const nlohmann::json document)
{
	EmmyFacade::Get().OnReceiveMessage(document);
}

void Transporter::OnDisconnect()
{
	if (disconnectNotified.exchange(true, std::memory_order_acq_rel)) {
		return;
	}
	connected = false;
	readHead = true;
	receiveSize = 0;
	EmmyFacade::Get().OnDisconnect();
}

void Transporter::OnConnect(bool suc)
{
	connected = suc;
	disconnectNotified.store(false, std::memory_order_release);
	protocolFailed.store(false, std::memory_order_release);
	readHead = true;
	receiveSize = 0;

	EmmyFacade::Get().OnConnect(suc);
}

void Transporter::SetMaxFrameSize(size_t size)
{
	if (size == 0) {
		return;
	}
	maxFrameSize = size;
}

size_t Transporter::GetMaxFrameSize() const
{
	return maxFrameSize;
}

void Transporter::OnProtocolError(const std::string& reason)
{
	std::fprintf(stderr, "[Emmy] transport protocol error: %s\n", reason.c_str());
	EmmyFacade::Get().OnTransportProtocolError(reason);
}

void Transporter::ProtocolError(const char* reason)
{
	if (protocolFailed.exchange(true, std::memory_order_acq_rel)) {
		return;
	}
	OnProtocolError(reason == nullptr ? "protocol_error" : reason);
	Stop();
	OnDisconnect();
}

bool Transporter::IsConnected() const
{
	return connected;
}

bool Transporter::IsServerMode() const
{
	return serverMode;
}

////////////////////////////////////////////////////////////////////////////////
// send data

typedef struct { uv_write_t req; uv_buf_t buf; Transporter* owner; } write_req_t;

static void after_write(uv_write_t* req, int status)
{
	const auto* writeReq = reinterpret_cast<write_req_t*>(req);
	if (writeReq->owner != nullptr) writeReq->owner->OnWriteComplete(writeReq->buf.len);
	free(writeReq->buf.base);
	delete writeReq;
}

void Transporter::Send(uv_stream_t* handler, int cmd, const char* data, size_t len)
{
	if (len > maxFrameSize) {
		ProtocolError("outgoing_frame_too_large");
		return;
	}
	if (!IsConnected() || handler == nullptr || data == nullptr)
	{
		return;
	}
	char cmdValue[100];
	const int l1 = sprintf(cmdValue, "%d\n", cmd);
	const size_t newLen = len + l1 + 1;
	char* newData = static_cast<char*>(malloc(newLen));
	if (newData == nullptr) return;
	// line1
	memcpy(newData, cmdValue, l1);
	// line2
	memcpy(newData + l1, data, len);
	newData[newLen - 1] = '\n';
	{
		std::lock_guard<std::mutex> lock(sendMutex);
		if (stopRequested.load(std::memory_order_acquire) || sendQueue.size() >= 256 ||
			outstandingBytes + newLen > maxFrameSize * 4) { free(newData); return; }
		outstandingBytes += newLen;
		sendQueue.push_back(PendingWrite{handler, newData, newLen});
	}
	std::lock_guard<std::mutex> asyncLock(asyncMutex);
	if (asyncInitialized.load(std::memory_order_acquire)) uv_async_send(&sendAsync);
}

void Transporter::Send(uv_stream_t* handler, const char* data, size_t len)
{
	if (len > maxFrameSize) {
		ProtocolError("outgoing_frame_too_large");
		return;
	}
	if (!IsConnected() || handler == nullptr || data == nullptr)
	{
		return;
	}
	char* newData = static_cast<char*>(malloc(len));
	if (newData == nullptr) return;

	memcpy(newData, data, len);
	{
		std::lock_guard<std::mutex> lock(sendMutex);
		if (stopRequested.load(std::memory_order_acquire) || sendQueue.size() >= 256 ||
			outstandingBytes + len > maxFrameSize * 4) { free(newData); return; }
		outstandingBytes += len;
		sendQueue.push_back(PendingWrite{handler, newData, len});
	}
	std::lock_guard<std::mutex> asyncLock(asyncMutex);
	if (asyncInitialized.load(std::memory_order_acquire)) uv_async_send(&sendAsync);
}

void Transporter::StartEventLoop()
{
	thread = std::thread(std::bind(&Transporter::Run, this));
}

void Transporter::JoinEventLoop()
{
	if (thread.joinable() && thread.get_id() != std::this_thread::get_id()) {
		thread.join();
	}
}

void Transporter::Run()
{
	running.store(!stopRequested.load(std::memory_order_acquire), std::memory_order_release);
	if (uv_async_init(loop, &sendAsync, OnSendAsync) != 0) return;
	sendAsync.data = this;
	asyncInitialized.store(true, std::memory_order_release);
	if (!running.load(std::memory_order_acquire)) uv_async_send(&sendAsync);
	while (running.load(std::memory_order_acquire) || uv_loop_alive(loop))
		uv_run(loop, UV_RUN_DEFAULT);
	DrainSendQueue();
	asyncInitialized.store(false, std::memory_order_release);
	if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&sendAsync)))
		uv_close(reinterpret_cast<uv_handle_t*>(&sendAsync), nullptr);
	uv_run(loop, UV_RUN_DEFAULT);
}

int Transporter::Stop()
{
	running.store(false, std::memory_order_release);
	stopRequested.store(true, std::memory_order_release);
	std::lock_guard<std::mutex> asyncLock(asyncMutex);
	if (asyncInitialized.load(std::memory_order_acquire)) uv_async_send(&sendAsync);
	return 0;
}

void Transporter::OnSendAsync(uv_async_t* handle) {
	if (handle == nullptr || handle->data == nullptr) return;
	Transporter* transporter = static_cast<Transporter*>(handle->data);
	transporter->DrainSendQueue();
	if (!transporter->running.load(std::memory_order_acquire)) transporter->OnLoopStop();
	if (!transporter->running.load(std::memory_order_acquire) &&
		!uv_is_closing(reinterpret_cast<uv_handle_t*>(handle))) {
		std::lock_guard<std::mutex> asyncLock(transporter->asyncMutex);
		transporter->asyncInitialized.store(false, std::memory_order_release);
		uv_close(reinterpret_cast<uv_handle_t*>(handle), nullptr);
	}
}

void Transporter::DrainSendQueue() {
	std::deque<PendingWrite> pending;
	{
		std::lock_guard<std::mutex> lock(sendMutex);
		pending.swap(sendQueue);
	}
	for (std::deque<PendingWrite>::iterator it = pending.begin(); it != pending.end(); ++it) {
		if (it->handler == nullptr || uv_is_closing(reinterpret_cast<uv_handle_t*>(it->handler))) {
			free(it->data);
			OnWriteComplete(it->len);
			continue;
		}
		write_req_t* request = new write_req_t();
		request->owner = this;
		request->buf = uv_buf_init(it->data, static_cast<unsigned int>(it->len));
		if (uv_write(&request->req, it->handler, &request->buf, 1, after_write) < 0) {
			free(it->data);
			delete request;
		}
	}
}

void Transporter::OnLoopStop() {}

void Transporter::OnWriteComplete(size_t len) {
	std::lock_guard<std::mutex> lock(sendMutex);
	if (outstandingBytes >= len) outstandingBytes -= len;
}

bool Transporter::ParseSocketAddress(const std::string &host, int port, sockaddr_storage *addr, std::string &err) 
{
	auto const loop = uv_default_loop();
	uv_getaddrinfo_t resolver;
	int res = uv_getaddrinfo(loop, &resolver, nullptr, host.c_str(),
	                         std::to_string(port).c_str(), nullptr);
	if (res != 0) {
		err = "Invalid host: ";
		err += uv_strerror(res);
		return false;
	}
	memcpy(addr, resolver.addrinfo->ai_addr, resolver.addrinfo->ai_addrlen);
	uv_freeaddrinfo(resolver.addrinfo);
	return true;
}
