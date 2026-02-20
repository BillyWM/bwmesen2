#pragma once

#include "pch.h"

class INotificationListener;
class TraceStreamerNotificationListener;
class Emulator;
class Socket;
class TraceStreamerConnection;

class TraceStreamer
{
private:
	friend class TraceStreamerNotificationListener;
	Emulator* _emu = nullptr;
	unique_ptr<Socket> _listener;
	unique_ptr<TraceStreamerConnection> _conn;
	unique_ptr<thread> _thread;
	atomic<bool> _stop = false;
	atomic<bool> _listening = false;
	atomic<bool> _pendingInfo = false;
	atomic<bool> _pendingSync = false;
	atomic<uint8_t> _pendingSyncReason = 0;
	uint16_t _port = 0;
	shared_ptr<INotificationListener> _notifListener;

	void RegisterNotificationListener();
	void RequestInfoAndMaybeSync(bool sendSync, uint8_t syncReason);

	void Exec();
	bool TryBindListener();
	void AcceptConnections();

public:
	TraceStreamer(Emulator* emu);
	~TraceStreamer();

	// Temporary v1 behavior: auto-start on app init.
	void StartAuto();
	void Stop();

	bool IsListening() const { return _listening.load(); }
	uint16_t GetPort() const { return _port; }
	bool IsConnected() const;
};
