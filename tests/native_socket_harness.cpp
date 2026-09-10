#include "emmy_debugger/emmy_facade.h"
#include "emmy_debugger/proto/protocol_v2.h"
#include "emmy_debugger/transporter/transporter.h"

#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
using Socket = SOCKET;
static const Socket kInvalidSocket = INVALID_SOCKET;
static void CloseSocket(Socket s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
static const Socket kInvalidSocket = -1;
static void CloseSocket(Socket s) { close(s); }
#endif

namespace {

void Require(bool condition, const std::string& message) {
	if (!condition) {
		std::cerr << "native socket harness: " << message << std::endl;
		std::exit(1);
	}
}

class Client {
public:
	~Client() { Close(); }

	void Connect(int port) {
#ifdef _WIN32
		// WSAStartup is performed before the libuv server is created.
#endif
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(static_cast<unsigned short>(port));
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		for (int attempt = 0; attempt != 20; ++attempt) {
			socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			Require(socket_ != kInvalidSocket, "client socket created");
			int connectResult;
			bool pending;
#ifdef _WIN32
			u_long nonBlocking = 1;
			Require(ioctlsocket(socket_, FIONBIO, &nonBlocking) == 0, "client nonblocking mode");
			connectResult = ::connect(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
			const int connectError = connectResult == 0 ? 0 : WSAGetLastError();
			pending = connectError == WSAEWOULDBLOCK || connectError == WSAEINPROGRESS;
#else
			const int flags = fcntl(socket_, F_GETFL, 0);
			Require(flags >= 0 && fcntl(socket_, F_SETFL, flags | O_NONBLOCK) == 0,
				"client nonblocking mode");
			connectResult = ::connect(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
			pending = connectResult != 0 && (errno == EINPROGRESS || errno == EWOULDBLOCK);
#endif
			if (connectResult == 0) {
#ifdef _WIN32
				nonBlocking = 0;
				ioctlsocket(socket_, FIONBIO, &nonBlocking);
#else
				fcntl(socket_, F_SETFL, flags);
#endif
				return;
			}
			if (!pending) {
				CloseSocket(socket_);
				socket_ = kInvalidSocket;
				continue;
			}
			fd_set writeSet;
			FD_ZERO(&writeSet);
			FD_SET(socket_, &writeSet);
			timeval timeout{};
			timeout.tv_usec = 150000;
			if (::select(static_cast<int>(socket_) + 1, nullptr, &writeSet, nullptr, &timeout) > 0) {
				int error = 0;
#ifdef _WIN32
				int errorSize = static_cast<int>(sizeof(error));
#else
				socklen_t errorSize = static_cast<socklen_t>(sizeof(error));
#endif
				getsockopt(socket_, SOL_SOCKET, SO_ERROR,
					reinterpret_cast<char*>(&error), &errorSize);
				if (error == 0) {
#ifdef _WIN32
					nonBlocking = 0;
					ioctlsocket(socket_, FIONBIO, &nonBlocking);
#else
					fcntl(socket_, F_SETFL, flags);
#endif
					return;
				}
			}
			CloseSocket(socket_);
			socket_ = kInvalidSocket;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		Require(false, "client connected after bounded retry");
	}

#ifdef _WIN32
	void ConnectPipe(const std::string& name) {
		const std::wstring path = L"\\\\.\\pipe\\emmylua-" + std::wstring(name.begin(), name.end());
		for (int attempt = 0; attempt != 60; ++attempt) {
			handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
				OPEN_EXISTING, 0, nullptr);
			if (handle_ != INVALID_HANDLE_VALUE) return;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		Require(false, "named pipe connected after bounded retry");
	}
#endif

	void Send(int command, const nlohmann::json& document) {
		const std::string frame = std::to_string(command) + "\n" + document.dump() + "\n";
#ifdef _WIN32
		if (handle_ != INVALID_HANDLE_VALUE) {
			DWORD written = 0;
			Require(WriteFile(handle_, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr) &&
				written == frame.size(), "pipe frame sent");
			return;
		}
#endif
		for (size_t offset = 0; offset < frame.size();) {
			const int written = ::send(socket_, frame.data() + offset,
				static_cast<int>(frame.size() - offset), 0);
			Require(written > 0, "frame sent");
			offset += static_cast<size_t>(written);
		}
	}

	nlohmann::json Receive(int timeoutMs = 10000) {
		const std::string command = ReadLine(timeoutMs);
		Require(!command.empty(), "response command line");
		return nlohmann::json::parse(ReadLine(timeoutMs));
	}

	void Close() {
#ifdef _WIN32
		if (handle_ != INVALID_HANDLE_VALUE) {
			CloseHandle(handle_);
			handle_ = INVALID_HANDLE_VALUE;
			return;
		}
#endif
		if (socket_ == kInvalidSocket) return;
		CloseSocket(socket_);
		socket_ = kInvalidSocket;
	}

private:
	std::string ReadLine(int timeoutMs) {
		std::string line;
		for (;;) {
#ifdef _WIN32
			if (handle_ != INVALID_HANDLE_VALUE) {
				DWORD available = 0;
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
				for (;;) {
					Require(PeekNamedPipe(handle_, nullptr, 0, nullptr, &available, nullptr), "pipe remains open");
					if (available > 0) break;
					Require(std::chrono::steady_clock::now() < deadline, "pipe response before timeout");
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
				char ch = 0;
				DWORD read = 0;
				Require(ReadFile(handle_, &ch, 1, &read, nullptr) && read == 1, "pipe response read");
				if (ch == '\n') return line;
				line.push_back(ch);
				Require(line.size() <= 1024 * 1024, "response line is bounded");
				continue;
			}
#endif
			char ch = 0;
			fd_set readSet;
			FD_ZERO(&readSet);
			FD_SET(socket_, &readSet);
			timeval timeout{};
			timeout.tv_sec = timeoutMs / 1000;
			timeout.tv_usec = (timeoutMs % 1000) * 1000;
			Require(::select(static_cast<int>(socket_) + 1, &readSet, nullptr, nullptr, &timeout) > 0,
				"response received before timeout");
			const int count = ::recv(socket_, &ch, 1, 0);
			Require(count == 1, "response socket remains open");
			if (ch == '\n') return line;
			line.push_back(ch);
			Require(line.size() <= 1024 * 1024, "response line is bounded");
		}
	}

	Socket socket_ = kInvalidSocket;
#ifdef _WIN32
	HANDLE handle_ = INVALID_HANDLE_VALUE;
#endif
};

nlohmann::json ReceiveType(Client& client, const std::string& type) {
	for (int attempt = 0; attempt != 40; ++attempt) {
		nlohmann::json document = client.Receive(1000);
		const auto typeIt = document.find("type");
		std::cerr << "received cmd=" << document.value("cmd", -1)
			<< " type=" << (typeIt != document.end() && typeIt->is_string() ? typeIt->get<std::string>() : "") << std::endl;
		if (typeIt != document.end() && *typeIt == type) return document;
	}
	Require(false, "expected protocol message: " + type);
	return nlohmann::json();
}

nlohmann::json Request(Client& client, const nlohmann::json& request, const std::string& type) {
	client.Send(static_cast<int>(MessageCMD::EnvelopeV2), request);
	return ReceiveType(client, type);
}

} // namespace

int main(int argc, char** argv) {
	const int port = 39547;
	const bool pipeMode = argc > 1 && std::string(argv[1]) == "--pipe";
#ifdef _WIN32
	WSADATA winsockData;
	Require(WSAStartup(MAKEWORD(2, 2), &winsockData) == 0, "winsock startup");
#endif
	lua_State* state = luaL_newstate();
	Require(state != nullptr, "Lua state created");
	luaL_openlibs(state);
	auto& facade = EmmyFacade::Get();
	facade.SetExpectedAuthToken("native-token");
	std::string listenError;
	std::cerr << "listen starting" << std::endl;
	const std::string pipeName = "emmy-native-harness-" +
		std::to_string(static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
	if (pipeMode) {
		Require(facade.PipeListen(state, pipeName, listenError), "named pipe listens: " + listenError);
	} else {
		Require(facade.TcpListen(state, "127.0.0.1", port, listenError), "TCP server listens: " + listenError);
	}
	std::cerr << "listen ready" << std::endl;

	Client client;
	if (pipeMode) {
#ifdef _WIN32
		client.ConnectPipe(pipeName);
#else
		Require(false, "named pipe mode requires Windows");
#endif
	} else {
		client.Connect(port);
	}
	const auto unauthorized = Request(client, {{"cmd", 18}, {"kind", "request"},
		{"type", "agent.describe"}, {"requestId", "unauthorized"}}, "agent.describe");
	Require(unauthorized.at("ok") == false && unauthorized.at("error").at("code") == "NOT_AUTHORIZED",
		"unauthenticated requests cannot read Agent details");
	client.Send(static_cast<int>(MessageCMD::InitReq), nlohmann::json{
		{"cmd", static_cast<int>(MessageCMD::InitReq)}, {"authToken", "native-token"}});
	std::cerr << "init sent" << std::endl;
	nlohmann::json init = client.Receive();
	std::cerr << "init received" << std::endl;
	Require(init["cmd"] == static_cast<int>(MessageCMD::InitRsp), "init response received");
	Require(init["protocolVersion"] == 2, "init negotiates v2");
	const std::string session = init["agentSessionId"].get<std::string>();
	const uint64_t epoch = init["connectionEpoch"].get<uint64_t>();
	client.Send(static_cast<int>(MessageCMD::ReadyReq), nlohmann::json{
		{"cmd", static_cast<int>(MessageCMD::ReadyReq)}});
	std::cerr << "ready sent" << std::endl;
	nlohmann::json ready = client.Receive();
	Require(ready["cmd"] == static_cast<int>(MessageCMD::ReadyRsp), "ready response received");
	nlohmann::json snapshot = ReceiveType(client, "vm.snapshot");
	Require(snapshot["payload"]["vms"].size() == 1, "snapshot contains Lua VM");
	const std::string vmId = snapshot["payload"]["vms"][0]["vmId"].get<std::string>();
	const uint64_t contextGeneration = snapshot["payload"]["vms"][0]["contextGeneration"].get<uint64_t>();
	const uint64_t sourceEpoch = snapshot["payload"]["vms"][0]["sourceEpoch"].get<uint64_t>();

	const auto envelope = [&](const std::string& type, const std::string& requestId,
		const nlohmann::json& payload) {
		return nlohmann::json{{"cmd", 18}, {"protocolVersion", 2}, {"kind", "request"},
			{"type", type}, {"requestId", requestId}, {"agentSessionId", session},
			{"connectionEpoch", epoch}, {"payload", payload}};
	};
	const auto invalidBreakpointSnapshot = Request(client,
		envelope("debug.breakpoints.replace", "bp-invalid", {{"revision", 1}}),
		"debug.breakpoints.replace");
	Require(invalidBreakpointSnapshot["ok"] == false &&
		invalidBreakpointSnapshot["error"]["code"] == "INVALID_BREAKPOINT_SNAPSHOT",
		"invalid breakpoint snapshot is rejected without disconnecting");
	Request(client, envelope("debug.breakpoints.replace", "bp-1", {
		{"revision", 1}, {"breakpoints", nlohmann::json::array({
			{{"file", "@native_socket_harness.lua"}, {"line", 3},
			 {"owner", "CLI:native"}, {"breakpointId", "bp-native"}, {"vmId", vmId},
			 {"composite", true},
			 {"contributions", {{{"owner", "CLI:native"}, {"breakpointId", "bp-native"},
				 {"autoContinue", false}}}},
			 {"sourceIdentity", {{"canonicalPath", "native_socket_harness.lua"},
				 {"sourceHash", ""}, {"verified", false}}}}
		})}}), "debug.breakpoints.replace");

	std::thread runner([&] {
		const char* script = "local value = {answer = 42}\nlocal result = value.answer\nresult = value.answer\n";
		Require(luaL_loadbuffer(state, script, std::strlen(script), "@native_socket_harness.lua") == 0,
			"Lua script compiles");
		Require(lua_pcall(state, 0, 0, 0) == 0, "Lua script resumes after continue");
	});
	nlohmann::json paused = ReceiveType(client, "debug.paused");
	Require(paused.contains("target") && paused["target"].is_object() &&
		paused["target"].contains("vmId") && paused["target"]["vmId"] == vmId,
		"pause targets VM");
	Require(paused["target"].contains("pauseId") && paused["target"].contains("threadId"),
		"pause target identity is complete");
	const uint64_t pauseId = paused["target"]["pauseId"].get<uint64_t>();
	const std::string threadId = paused["target"]["threadId"].get<std::string>();
	Require(paused.contains("payload") && paused["payload"].contains("stacks") &&
		paused["payload"]["stacks"].is_array() && !paused["payload"]["stacks"].empty(),
		"paused stack snapshot exists");
	const std::string frameId = paused["payload"]["stacks"][0].value("frameId", std::string());
	Require(!frameId.empty(), "paused stack includes frame identity");
	std::cerr << "pause target parsed" << std::endl;
	nlohmann::json target{{"vmId", vmId}, {"pauseId", pauseId}, {"threadId", threadId}, {"frameId", frameId}};
	const nlohmann::json eval = envelope("debug.eval", "eval-1", {
		{"expr", "value.answer"}, {"stackLevel", 0}, {"depth", 0}, {"maxDepth", 0},
		{"policy", "VALUE_PATH"},
		{"maxNodes", 100}, {"maxBytes", 4096},
		{"sourceIdentity", {{"canonicalPath", "native_socket_harness.lua"},
			{"sourceHash", ""}}}});
	nlohmann::json evalRequest = eval;
	evalRequest["target"] = target;
	evalRequest["contextGeneration"] = contextGeneration;
	evalRequest["sourceEpoch"] = sourceEpoch;
	std::cerr << "eval sending" << std::endl;
	auto missingPolicy = evalRequest;
	missingPolicy["requestId"] = "eval-missing-policy";
	missingPolicy["payload"].erase("policy");
	const auto denied = Request(client, missingPolicy, "debug.eval");
	Require(denied.at("ok") == false && denied.at("error").at("code") == "EVALUATION_DENIED",
		"missing evaluation policy is rejected without disconnecting");
	nlohmann::json evalResponse = Request(client, evalRequest, "debug.eval");
	Require(evalResponse["ok"] == true && evalResponse["payload"]["value"]["value"] == "42",
		"VALUE_PATH evaluates to 42");
	Require(Request(client, evalRequest, "debug.eval") == evalResponse,
		"retrying the same request replays the completed value");
	auto reused = evalRequest;
	reused["payload"]["expr"] = "result";
	const auto conflict = Request(client, reused, "debug.eval");
	Require(conflict.at("ok") == false && conflict.at("error").at("code") == "REQUEST_ID_REUSE",
		"request ID cannot be reused for another expression");

	nlohmann::json action = envelope("debug.action", "continue-1", {{"action", static_cast<int>(DebugAction::Continue)}});
	action["target"] = nlohmann::json{{"vmId", vmId}, {"pauseId", pauseId}, {"threadId", threadId}};
	action["contextGeneration"] = contextGeneration;
	action["sourceEpoch"] = sourceEpoch;
	client.Send(static_cast<int>(MessageCMD::EnvelopeV2), action);
	bool resumed = false;
	nlohmann::json actionResponse;
	for (int attempt = 0; attempt != 40 && (!resumed || actionResponse.is_null()); ++attempt) {
		nlohmann::json document = client.Receive(1000);
		if (document["type"] == "debug.resumed") {
			resumed = true;
			continue;
		}
		if (document["type"] == "debug.action") {
			actionResponse = document;
		}
	}
	Require(!actionResponse.is_null(), "continue response received");
	Require(actionResponse["ok"] == true, "continue accepted");
	Require(resumed, "resume event received");
	runner.join();
	evalRequest["requestId"] = "eval-stale-pause";
	const auto stale = Request(client, evalRequest, "debug.eval");
	Require(stale.at("ok") == false && stale.at("error").at("code") == "STALE_PAUSE_REFERENCE",
		"resumed frames cannot be evaluated");
	client.Close();
	if (pipeMode) {
#ifdef _WIN32
		client.ConnectPipe(pipeName);
#endif
	} else client.Connect(port);
	client.Send(static_cast<int>(MessageCMD::InitReq), {{"cmd", 1}, {"authToken", "native-token"}});
	const auto reconnected = client.Receive();
	Require(reconnected.at("agentSessionId") == session && reconnected.at("connectionEpoch").get<uint64_t>() > epoch,
		"reconnect retains Agent identity and advances connection epoch");
	client.Send(static_cast<int>(MessageCMD::ReadyReq), {{"cmd", 3}});
	Require(client.Receive().at("cmd") == 4, "reconnect requires a new Ready handshake");
	const auto reconciled = ReceiveType(client, "vm.snapshot");
	Require(reconciled.at("payload").at("vms").size() == 1 &&
		reconciled.at("payload").at("vms")[0].at("vmId") == vmId,
		"reconnect snapshots the same live VM");
	const auto oldEpoch = Request(client, envelope("agent.describe", "old-epoch", nlohmann::json::object()), "agent.describe");
	Require(oldEpoch.at("ok") == false && oldEpoch.at("error").at("code") == "STALE_CONNECTION_EPOCH",
		"old connection requests cannot cross reconnect");
	facade.Destroy();
	lua_close(state);
#ifdef _WIN32
	WSACleanup();
#endif
	std::cout << nlohmann::json{{"ok", true}, {"transport", pipeMode ? "named-pipe" : "tcp"},
		{"auth", true}, {"snapshot", true}, {"pauseEvalContinue", true}}.dump() << std::endl;
	return 0;
}
