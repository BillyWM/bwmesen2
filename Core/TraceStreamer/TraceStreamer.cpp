#include "pch.h"

#include "TraceStreamer.h"
#include "TraceStreamerConnection.h"

#include "Shared/Emulator.h"
#include "Utilities/Socket.h"

using namespace std;

namespace
{
	static constexpr uint16_t kTraceStreamerPortStart = 63783;
	static constexpr int kTraceStreamerPortAttempts = 10;
}

TraceStreamer::TraceStreamer(Emulator* emu)
{
	_emu = emu;
}

TraceStreamer::~TraceStreamer()
{
	Stop();
}

bool TraceStreamer::IsConnected() const
{
	return _conn && !_conn->ConnectionError();
}

void TraceStreamer::StartAuto()
{
	if(_thread) {
		return;
	}

	_stop = false;
	_thread.reset(new thread(&TraceStreamer::Exec, this));
}

void TraceStreamer::Stop()
{
	_stop = true;

	if(_thread) {
		_thread->join();
		_thread.reset();
	}

	_conn.reset();
	_listener.reset();
	_listening = false;
	_port = 0;
}

bool TraceStreamer::TryBindListener()
{
	for(int i = 0; i < kTraceStreamerPortAttempts; i++) {
		uint16_t port = (uint16_t)(kTraceStreamerPortStart + i);

		unique_ptr<Socket> s(new Socket());
		if(s->ConnectionError()) {
			continue;
		}

		s->BindLoopback(port);
		if(s->ConnectionError()) {
			continue;
		}

		s->Listen(10);
		if(s->ConnectionError()) {
			continue;
		}

		_listener = std::move(s);
		_port = port;
		_listening = true;
		std::cout << "TraceStreamer listening on 127.0.0.1:" << port << std::endl;
		return true;
	}

	std::cout << "TraceStreamer: failed to bind any port in range." << std::endl;
	return false;
}

void TraceStreamer::AcceptConnections()
{
	if(!_listener || _listener->ConnectionError()) {
		return;
	}

	while(true) {
		unique_ptr<Socket> socket = _listener->Accept();
		if(!socket || socket->ConnectionError()) {
			break;
		}

		if(!_conn || _conn->ConnectionError()) {
			_conn.reset(new TraceStreamerConnection(_emu, std::move(socket)));
		} else {
			// v1: single connection. Reject additional clients.
			socket->Close();
		}
	}

	// Match netplay server behavior (re-arm listen after accept loop)
	_listener->Listen(10);
}

void TraceStreamer::Exec()
{
	_listening = false;
	_port = 0;

	if(!TryBindListener()) {
		return;
	}

	while(!_stop) {
		AcceptConnections();

		if(_conn) {
			_conn->Poll();
			if(_conn->ConnectionError()) {
				_conn.reset();
			}
		}

		std::this_thread::sleep_for(std::chrono::duration<int, std::milli>(1));
	}

	if(_conn) {
		_conn.reset();
	}
	if(_listener) {
		_listener.reset();
	}
	_listening = false;
	_port = 0;
}
