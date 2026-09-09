#pragma once
#include "uv.h"
#include "transporter.h"

class PipelineServerTransporter : public Transporter {
	uv_pipe_t uvServer;
	uv_pipe_t* uvClient;
	bool serverInitialized = false;
public:
	PipelineServerTransporter();
	~PipelineServerTransporter();

	bool pipe(const std::string& name, std::string& err);
	int Stop() override;
	void Send(int cmd, const char* data, size_t len) override;
	void OnPipeConnection(uv_stream_t* pipe, int status);
	void CloseClient();
	static void OnClientClosed(uv_handle_t* handle);
	static void OnServerClosed(uv_handle_t* handle);
};
