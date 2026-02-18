#pragma once

#include "pch.h"

class Emulator;
class Socket;

class TraceStreamerConnection
{
private:
	Emulator* _emu = nullptr;
	unique_ptr<Socket> _socket;
	vector<uint8_t> _rxBuf;
	bool _handshakeComplete = false;

	bool TryReceive();
	void ProcessFrames();

	void HandleHello(const uint8_t* payload, size_t len);
	void HandleGoodbye(const uint8_t* payload, size_t len);

	void SendHelloAck(uint16_t major, uint16_t minor);
	bool SendInfo();
	void SendSync(uint8_t reason);
	void SendGoodbyeAck(uint8_t reason);
	void SendFrame(const vector<uint8_t>& frame);

public:
	TraceStreamerConnection(Emulator* emu, unique_ptr<Socket> socket);

	bool ConnectionError() const;
	bool HandshakeComplete() const { return _handshakeComplete; }

	// Pump socket IO + handle messages. Safe to call frequently.
	void Poll();
};
