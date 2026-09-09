#include "emmy_debugger/transporter/transporter.h"
#include "nlohmann/json.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

class TestTransporter final : public Transporter {
public:
	TestTransporter() : Transporter(false), disconnects(0), protocolErrors(0) {
	}

	using Transporter::Receive;
	using Transporter::Send;

	std::vector<nlohmann::json> messages;
	int disconnects;
	int protocolErrors;
	std::string lastProtocolError;

protected:
	void Send(int, const char*, size_t) override {
	}

	void OnReceiveMessage(const nlohmann::json document) override {
		messages.push_back(document);
	}

	void OnProtocolError(const std::string& reason) override {
		++protocolErrors;
		lastProtocolError = reason;
	}

	void OnDisconnect() override {
		++disconnects;
	}
};

namespace {

void Require(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "transporter frame test failed: " << message << std::endl;
		std::exit(1);
	}
}

} // namespace

int main() {
	TestTransporter split;
	split.SetMaxFrameSize(64);
	split.Receive("13\n", 3);
	split.Receive("{\"cmd\":13}", 10);
	split.Receive("\n", 1);
	Require(split.messages.size() == 1, "split frame is reconstructed");
	Require(split.messages[0]["cmd"] == 13, "split frame JSON is delivered");

	TestTransporter aggregate;
	aggregate.SetMaxFrameSize(16);
	const std::string aggregateFrames = "13\n{\"a\":1}\n13\n{\"b\":2}\n";
	aggregate.Receive(aggregateFrames.data(), aggregateFrames.size());
	Require(aggregate.messages.size() == 2, "multiple frames in one read are accepted");

	TestTransporter malformed;
	malformed.SetMaxFrameSize(64);
	const std::string malformedFrame = "13\n{not-json}\n";
	malformed.Receive(malformedFrame.data(), malformedFrame.size());
	Require(malformed.protocolErrors == 1, "malformed JSON reports one protocol error");
	Require(malformed.lastProtocolError == "invalid_json", "malformed JSON reason");
	Require(malformed.disconnects == 1, "malformed JSON disconnects once");

	TestTransporter oversized;
	oversized.SetMaxFrameSize(16);
	const std::string payload = "13\n" + std::string(17, 'x') + "\n";
	oversized.Receive(payload.data(), payload.size());
	Require(oversized.protocolErrors == 1, "oversized payload reports an error");
	Require(oversized.lastProtocolError == "frame_line_too_large" ||
			oversized.lastProtocolError == "frame_too_large", "oversized reason");

	TestTransporter invalidCommand;
	invalidCommand.SetMaxFrameSize(64);
	invalidCommand.Receive("abc\n", 4);
	Require(invalidCommand.protocolErrors == 1, "invalid command reports an error");
	Require(invalidCommand.lastProtocolError == "invalid_command_line", "invalid command reason");

	TestTransporter unterminated;
	unterminated.SetMaxFrameSize(16);
	const std::string noNewline(17, 'x');
	unterminated.Receive(noNewline.data(), noNewline.size());
	Require(unterminated.protocolErrors == 1, "unterminated oversized line reports an error");

	TestTransporter outgoing;
	outgoing.SetMaxFrameSize(16);
	outgoing.Send(13, nlohmann::json{{"value", std::string(32, 'x')}});
	Require(outgoing.protocolErrors == 1, "oversized outgoing frame reports an error");
	Require(outgoing.lastProtocolError == "outgoing_frame_too_large", "outgoing reason");

	std::cout << "transporter frame tests passed" << std::endl;
	return 0;
}
