#include "emmy_debugger/transporter/socket_client_transporter.h"
#include "emmy_debugger/transporter/socket_server_transporter.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>

namespace {
void Require(bool value, const char* message) {
	if (!value) {
		std::cerr << "transporter concurrency test failed: " << message << std::endl;
		std::exit(1);
	}
}
}

int main() {
	// Stop before an event loop exists must be harmless and must not hang
	// destruction.
	{
		SocketClientTransporter neverStarted;
		Require(neverStarted.Stop() == 0, "stop before connect is idempotent");
		neverStarted.Send(1, "discarded", 9);
	}

	SocketServerTransporter server;
	std::string error;
	Require(server.Listen("127.0.0.1", 43199, error), "localhost server starts");
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	SocketClientTransporter client;
	if (!client.Connect("127.0.0.1", 43199, error)) {
		std::cerr << "client connect error: " << error << std::endl;
		std::exit(1);
	}

	std::atomic<bool> go(false);
	std::vector<std::thread> senders;
	for (int i = 0; i < 8; ++i) {
		senders.emplace_back([&client, &go, i] {
			while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
			for (int n = 0; n < 2000; ++n) client.Send(18, "{}", 2);
		});
	}
	go.store(true, std::memory_order_release);
	for (std::thread& sender : senders) sender.join();

	Require(client.Stop() == 0, "client stop succeeds after concurrent sends");
	Require(client.Stop() == 0, "client stop is idempotent");
	client.Send(18, "{}", 2);
	Require(server.Stop() == 0, "server stop succeeds after client close");
	Require(server.Stop() == 0, "server stop is idempotent");

	std::cout << "transporter concurrency tests passed" << std::endl;
	return 0;
}
