#include "Misc/Pch/Pch.h"
#include "MakcuApi.h"
#include <algorithm>
#include <cstdlib>
#include <vector>
#include <cstring>

MakcuApi::~MakcuApi() { Disconnect(); }

bool MakcuApi::OpenAtBaud(const std::string& port, DWORD baud) {
	const std::string path = "\\\\.\\" + port;
	handle_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle_ == INVALID_HANDLE_VALUE) return false;
	DCB dcb{};
	dcb.DCBlength = sizeof(dcb);
	if (!GetCommState(handle_, &dcb)) { Disconnect(); return false; }
	dcb.BaudRate = baud;
	dcb.ByteSize = 8;
	dcb.Parity = NOPARITY;
	dcb.StopBits = ONESTOPBIT;
	dcb.fBinary = TRUE;
	dcb.fParity = FALSE;
	dcb.fOutxCtsFlow = FALSE;
	dcb.fOutxDsrFlow = FALSE;
	dcb.fDtrControl = DTR_CONTROL_ENABLE;
	dcb.fDsrSensitivity = FALSE;
	dcb.fTXContinueOnXoff = TRUE;
	dcb.fOutX = FALSE;
	dcb.fInX = FALSE;
	dcb.fRtsControl = RTS_CONTROL_ENABLE;
	dcb.fAbortOnError = FALSE;
	if (!SetCommState(handle_, &dcb)) { Disconnect(); return false; }
	COMMTIMEOUTS timeouts{};
	timeouts.ReadIntervalTimeout = 5;
	timeouts.ReadTotalTimeoutConstant = 15;
	timeouts.ReadTotalTimeoutMultiplier = 1;
	timeouts.WriteTotalTimeoutConstant = 30;
	timeouts.WriteTotalTimeoutMultiplier = 2;
	if (!SetCommTimeouts(handle_, &timeouts)) { Disconnect(); return false; }
	SetupComm(handle_, 4096, 4096);
	PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
	return true;
}

bool MakcuApi::WriteCommand(const std::string& command) {
	if (!IsConnected()) return false;
	DWORD written = 0;
	return WriteFile(handle_, command.data(), static_cast<DWORD>(command.size()), &written, nullptr) && written == command.size();
}

bool MakcuApi::ReadPrompt(std::string& response, DWORD timeoutMs) {
	response.clear();
	const ULONGLONG deadline = GetTickCount64() + timeoutMs;
	char ch = 0;
	while (GetTickCount64() < deadline && response.size() < 512) {
		DWORD read = 0;
		if (!ReadFile(handle_, &ch, 1, &read, nullptr)) return false;
		if (read == 0) continue;
		response.push_back(ch);
		std::string receivedByte(1, ch);
		ProcessButtonEvents(receivedByte);
		const size_t prompt = response.find(">>>");
		if (prompt != std::string::npos) {
			response.resize(prompt);
			return true;
		}
	}
	return false;
}

// In text mode the Makcu emits button changes as: "km." + one mask byte + CRLF.
// Strip those frames from command replies so asynchronous input never corrupts an ACK.
void MakcuApi::ProcessButtonEvents(std::string& bytes) {
	buttonRxPending_ += bytes;
	bytes.clear();
	for (;;) {
		const size_t marker = buttonRxPending_.find("km.");
		if (marker == std::string::npos) {
			// Preserve only a possible partial "km." marker across serial reads.
			if (buttonRxPending_.size() > 2)
				buttonRxPending_.erase(0, buttonRxPending_.size() - 2);
			return;
		}
		if (buttonRxPending_.size() < marker + 6) {
			if (marker > 0) buttonRxPending_.erase(0, marker);
			return;
		}
		if (buttonRxPending_[marker + 4] == '\r' && buttonRxPending_[marker + 5] == '\n') {
			buttonMask_ = static_cast<unsigned char>(buttonRxPending_[marker + 3]);
			buttonRxPending_.erase(marker, 6);
		} else {
			// This is ordinary text such as "km.version()", not a stream frame.
			const size_t next = buttonRxPending_.find("km.", marker + 3);
			if (next == std::string::npos) {
				// Keep just a possible trailing marker prefix; the response text is
				// already being collected separately by ReadPrompt.
				const size_t tail = buttonRxPending_.size();
				if (tail >= 2 && buttonRxPending_.compare(tail - 2, 2, "km") == 0)
					buttonRxPending_ = "km";
				else if (tail >= 1 && buttonRxPending_[tail - 1] == 'k')
					buttonRxPending_ = "k";
				else buttonRxPending_.clear();
				return;
			}
			buttonRxPending_.erase(marker, next - marker);
		}
	}
}

bool MakcuApi::ReadPendingBytes() {
	if (!IsConnected()) return false;
	COMSTAT status{};
	DWORD errors = 0;
	if (!ClearCommError(handle_, &errors, &status)) return false;
	while (status.cbInQue > 0) {
		char buffer[256]{};
		const DWORD wanted = (std::min<DWORD>)(status.cbInQue, sizeof(buffer));
		DWORD received = 0;
		if (!ReadFile(handle_, buffer, wanted, &received, nullptr)) return false;
		if (!received) break;
		std::string bytes(buffer, buffer + received);
		ProcessButtonEvents(bytes);
		if (!ClearCommError(handle_, &errors, &status)) return false;
	}
	return true;
}

bool MakcuApi::Connect(const std::string& port) {
	Disconnect();
	buttonRxPending_.clear();
	buttonMask_ = 0;
	// The official compatibility guide recommends probing these two speeds
	// separately; a successful km.version() response identifies a MAKCU device.
	for (DWORD baud : { 4000000u, 115200u }) {
		if (!OpenAtBaud(port, baud)) continue;
		// Query replies follow the device's echo setting. Enable them first; if
		// they were already disabled this setter can be silent, so its reply is
		// deliberately optional.
		if (!WriteCommand("km.echo(1)\r\n")) { Disconnect(); continue; }
		std::string echoReply;
		ReadPrompt(echoReply, 40);
		PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
		std::string response;
		if (WriteCommand("km.version()\r\n") && ReadPrompt(response, 100)) {
			if (response.find("MAKCU") != std::string::npos) {
				PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
				buttonMask_ = 0;
				// Subscribe once to raw physical button changes. In particular, bits 3
				// and 4 are the gaming PC's Mouse 4 and Mouse 5 side buttons.
				if (WriteCommand("km.buttons(1)\r\n")) {
					std::string streamAck;
					if (ReadPrompt(streamAck, 150) && streamAck.find("ERR") == std::string::npos) return true;
				}
			}
		}
		Disconnect();
	}
	return false;
}

bool MakcuApi::ConnectAuto(std::string& connectedPort) {
	connectedPort.clear();
	std::vector<std::string> ports;
	char names[32768]{};
	const DWORD count = QueryDosDeviceA(nullptr, names, static_cast<DWORD>(sizeof(names)));
	if (!count) return false;
	for (const char* name = names; *name; name += std::strlen(name) + 1) {
		if (_strnicmp(name, "COM", 3) != 0 || name[3] < '0' || name[3] > '9') continue;
		bool numeric = true;
		for (const char* digit = name + 3; *digit; ++digit)
			if (*digit < '0' || *digit > '9') { numeric = false; break; }
		if (numeric) ports.emplace_back(name);
	}
	std::sort(ports.begin(), ports.end(), [](const std::string& a, const std::string& b) {
		return std::strtoul(a.c_str() + 3, nullptr, 10) < std::strtoul(b.c_str() + 3, nullptr, 10);
	});
	for (const auto& port : ports) {
		if (Connect(port)) {
			connectedPort = port;
			return true;
		}
	}
	return false;
}

void MakcuApi::Disconnect() {
	if (handle_ != INVALID_HANDLE_VALUE) {
		CloseHandle(handle_);
		handle_ = INVALID_HANDLE_VALUE;
	}
	buttonRxPending_.clear();
	buttonMask_ = 0;
}

bool MakcuApi::IsConnected() const { return handle_ != INVALID_HANDLE_VALUE; }

bool MakcuApi::Move(int dx, int dy) {
	dx = (std::clamp)(dx, -32768, 32767);
	dy = (std::clamp)(dy, -32768, 32767);
	if (!WriteCommand("km.move(" + std::to_string(dx) + "," + std::to_string(dy) + ")\r\n")) return false;
	std::string ack;
	return ReadPrompt(ack, 100);
}

bool MakcuApi::ButtonDown(int mouseButton, bool& down) {
	if (mouseButton < 1 || mouseButton > 5 || !ReadPendingBytes()) return false;
	// Raw stream mask: bit 0 left, 1 right, 2 middle, 3 side1/Mouse 4, 4 side2/Mouse 5.
	down = (buttonMask_ & (1u << (mouseButton - 1))) != 0;
	return true;
}
