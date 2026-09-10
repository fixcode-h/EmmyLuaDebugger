#include "emmy_debugger/transporter/socket_client_transporter.h"
#include "emmy_debugger/transporter/socket_server_transporter.h"
#include "emmy_debugger/transporter/pipeline_client_transporter.h"
#include "emmy_debugger/transporter/pipeline_server_transporter.h"
#include "nlohmann/json.hpp"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <string>

class CountingClient : public SocketClientTransporter {
public:
	std::atomic<int> received{0};
protected:
	void OnReceiveMessage(const nlohmann::json document) override { ++received; }
	void OnConnect(bool suc) override { SetConnectionState(suc); }
	void OnDisconnect() override { SetConnectionState(false); }
};

class CountingServer : public SocketServerTransporter {
public:
	std::atomic<int> received{0};
protected:
	void OnReceiveMessage(const nlohmann::json document) override { ++received; }
	void OnConnect(bool suc) override { SetConnectionState(suc); }
	void OnDisconnect() override { SetConnectionState(false); }
};

class CountingPipeClient : public PipelineClientTransporter {
public:
	std::atomic<int> received{0};
protected:
	void OnReceiveMessage(const nlohmann::json document) override { ++received; }
	void OnConnect(bool suc) override { SetConnectionState(suc); }
	void OnDisconnect() override { SetConnectionState(false); }
};

class CountingPipeServer : public PipelineServerTransporter {
public:
	std::atomic<int> received{0};
protected:
	void OnReceiveMessage(const nlohmann::json document) override { ++received; }
	void OnConnect(bool suc) override { SetConnectionState(suc); }
	void OnDisconnect() override { SetConnectionState(false); }
};

namespace {
void Require(bool value, const char* message) {
	if (!value) {
		std::cerr << "transporter concurrency test failed: " << message << std::endl;
		std::exit(1);
	}
}
}

int main(int argc, char** argv) {
	// Stop before an event loop exists must be harmless and must not hang
	// destruction.
	{
		SocketClientTransporter neverStarted;
		Require(neverStarted.Stop() == 0, "stop before connect is idempotent");
		neverStarted.Send(1, "discarded", 9);
	}

	const bool usePipe = argc > 1 && std::string(argv[1]) == "--pipe";
	if (usePipe) {
		CountingPipeServer server;
		std::string error;
		const std::string name = "concurrency-" + std::to_string(
			static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
		Require(server.pipe(name, error), "pipe server starts");
		CountingPipeClient client;
		if (!client.Connect(name, error)) {
			std::cerr << "pipe client connect error: " << error << std::endl;
			std::exit(1);
		}
		std::atomic<bool> stopSenders(false);
		std::vector<std::thread> senders;
		for (int i = 0; i < 8; ++i) senders.emplace_back([&client, &stopSenders] {
			while (!stopSenders.load(std::memory_order_acquire)) client.Send(18, "{}", 2);
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		Require(client.Stop() == 0, "pipe client stop succeeds during sends");
		stopSenders.store(true, std::memory_order_release);
		for (std::thread& sender : senders) sender.join();
		Require(server.received.load() > 0, "pipe server receives messages");
		Require(client.Stop() == 0, "pipe client stop is idempotent");
		Require(server.Stop() == 0, "pipe server stop succeeds");
		Require(server.Stop() == 0, "pipe server stop is idempotent");
		return 0;
	}

	CountingServer server;
	std::string error;
	Require(server.Listen("127.0.0.1", 0, error), "localhost server starts");
	const int port = server.GetPort();
	Require(port != 0, "dynamic localhost port is available");
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	CountingClient client;
	if (!client.Connect("127.0.0.1", port, error)) {
		std::cerr << "client connect error: " << error << std::endl;
		std::exit(1);
	}

	std::atomic<bool> go(false);
	std::vector<std::thread> senders;
	std::atomic<bool> stopSenders(false);
	for (int i = 0; i < 8; ++i) {
		senders.emplace_back([&client, &go, &stopSenders] {
			while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
			while (!stopSenders.load(std::memory_order_acquire)) client.Send(18, "{}", 2);
		});
	}
	go.store(true, std::memory_order_release);
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	Require(client.Stop() == 0, "client stop succeeds during concurrent sends");
	stopSenders.store(true, std::memory_order_release);
	for (std::thread& sender : senders) sender.join();
	Require(client.Stop() == 0, "client stop is idempotent");
	client.Send(18, "{}", 2);
	Require(server.Stop() == 0, "server stop succeeds after client close");
	Require(server.Stop() == 0, "server stop is idempotent");
	Require(server.received.load() > 0, "server receives messages");

	std::cout << "transporter concurrency tests passed" << std::endl;
	return 0;
}
