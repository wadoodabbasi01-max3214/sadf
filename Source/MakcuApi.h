#pragma once
#include <Windows.h>
#include <string>

class MakcuApi {
public:
	MakcuApi() = default;
	~MakcuApi();
	MakcuApi(const MakcuApi&) = delete;
	MakcuApi& operator=(const MakcuApi&) = delete;

	bool Connect(const std::string& port);
	bool ConnectAuto(std::string& connectedPort);
	void Disconnect();
	bool IsConnected() const;
	bool Move(int dx, int dy);
	bool ButtonDown(int mouseButton, bool& down);

private:
	bool OpenAtBaud(const std::string& port, DWORD baud);
	bool WriteCommand(const std::string& command);
	bool ReadPrompt(std::string& response, DWORD timeoutMs);
	void ProcessButtonEvents(std::string& bytes);
	bool ReadPendingBytes();
	HANDLE handle_ = INVALID_HANDLE_VALUE;
	unsigned char buttonMask_ = 0;
	std::string buttonRxPending_;
};
