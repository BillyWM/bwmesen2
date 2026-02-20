#include "pch.h"

#include "TraceStreamerConnection.h"
#include "TraceStreamerProtocol.h"

#include "Shared/Emulator.h"
#include "Shared/RomInfo.h"

#include "NES/NesConsole.h"
#include "NES/BaseMapper.h"
#include "NES/NesCpu.h"
#include "NES/BaseNesPpu.h"
#include "NES/RomData.h"

#include "Utilities/Socket.h"

using namespace std;

namespace
{
	static uint16_t ReadU16LE(const uint8_t* p)
	{
		return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
	}

	struct InfoSnapshot
	{
		bool HasGame = false;
		string FileName;
		string Sha1;
		uint32_t Crc32 = 0;
		uint32_t PrgCrc32 = 0;
		uint32_t PrgChrCrc32 = 0;
		uint16_t MapperId = 0;
		uint8_t SubmapperId = 0;
		uint8_t Mirroring = 0;
		int32_t PrgRomSize = 0;
		int32_t ChrRomSize = 0;
		int32_t WorkRamSize = 0;
		int32_t SaveRamSize = 0;
		int32_t ChrRamSize = 0;
		int32_t SaveChrRamSize = 0;
	};

	struct SyncSnapshot
	{
		bool Valid = false;
		uint64_t CpuCycleCount = 0;
		int16_t Scanline = 0;
		uint16_t Dot = 0;
		uint16_t PC = 0;
		uint8_t A = 0;
		uint8_t X = 0;
		uint8_t Y = 0;
		uint8_t SP = 0;
		uint8_t PS = 0;
	};

	static InfoSnapshot GetInfoSnapshot(Emulator* emu)
	{
		InfoSnapshot snap;

		auto lock = emu->AcquireLock();
		RomInfo& ri = emu->GetRomInfo();
		shared_ptr<IConsole> console = emu->GetConsole();
		if(!console) {
			return snap;
		}

		if(emu->GetConsoleType() != ConsoleType::Nes || ri.Format != RomFormat::iNes) {
			return snap;
		}

		auto nesConsole = dynamic_pointer_cast<NesConsole>(console);
		if(!nesConsole) {
			return snap;
		}

		BaseMapper* mapper = nesConsole->GetMapper();
		if(!mapper) {
			return snap;
		}

		NesRomInfo ninfo = mapper->GetRomInfo();
		CartridgeState cs = mapper->GetState();

		snap.HasGame = true;
		snap.FileName = ri.RomFile.GetFileName();
		snap.Sha1 = ri.RomFile.GetSha1Hash();
		snap.Crc32 = ninfo.Hash.Crc32;
		snap.PrgCrc32 = ninfo.Hash.PrgCrc32;
		snap.PrgChrCrc32 = ninfo.Hash.PrgChrCrc32;
		snap.MapperId = ninfo.MapperID;
		snap.SubmapperId = ninfo.SubMapperID;
		snap.Mirroring = (uint8_t)ninfo.Mirroring;

		snap.PrgRomSize = (int32_t)cs.PrgRomSize;
		snap.ChrRomSize = (int32_t)cs.ChrRomSize;
		snap.WorkRamSize = (int32_t)mapper->GetEffectiveWorkRamSize();
		snap.SaveRamSize = (int32_t)mapper->GetEffectiveSaveRamSize();
		snap.ChrRamSize = (int32_t)mapper->GetEffectiveChrRamSize();
		snap.SaveChrRamSize = (int32_t)mapper->GetEffectiveSaveChrRamSize();

		return snap;
	}

	static SyncSnapshot GetSyncSnapshot(Emulator* emu)
	{
		SyncSnapshot snap;
		auto lock = emu->AcquireLock();

		shared_ptr<IConsole> console = emu->GetConsole();
		if(!console) {
			return snap;
		}
		if(emu->GetConsoleType() != ConsoleType::Nes) {
			return snap;
		}

		auto nesConsole = dynamic_pointer_cast<NesConsole>(console);
		if(!nesConsole) {
			return snap;
		}

		NesCpu* cpu = nesConsole->GetCpu();
		BaseNesPpu* ppu = nesConsole->GetPpu();
		if(!cpu || !ppu) {
			return snap;
		}

		NesCpuState state = cpu->GetState();
		snap.Valid = true;
		snap.CpuCycleCount = state.CycleCount;
		snap.Scanline = (int16_t)ppu->GetCurrentScanline();
		snap.Dot = (uint16_t)ppu->GetCurrentCycle();
		snap.PC = state.PC;
		snap.A = state.A;
		snap.X = state.X;
		snap.Y = state.Y;
		snap.SP = state.SP;
		snap.PS = state.PS;

		return snap;
	}
}

TraceStreamerConnection::TraceStreamerConnection(Emulator* emu, unique_ptr<Socket> socket)
{
	_emu = emu;
	_socket = std::move(socket);
	_rxBuf.reserve(16 * 1024);
}

bool TraceStreamerConnection::ConnectionError() const
{
	return !_socket || _socket->ConnectionError();
}

void TraceStreamerConnection::SendFrame(const vector<uint8_t>& frame)
{
	if(!_socket || _socket->ConnectionError()) {
		return;
	}
	_socket->Send((char*)frame.data(), (int)frame.size(), 0);
}

void TraceStreamerConnection::SendHelloAck(uint16_t major, uint16_t minor)
{
	vector<uint8_t> payload;
	payload.reserve(4);
	TraceStreamerProtocol::WriteU16LE(payload, major);
	TraceStreamerProtocol::WriteU16LE(payload, minor);
	SendFrame(TraceStreamerProtocol::MakeFrame(TraceStreamerProtocol::MsgType::HelloAck, payload));
}

void TraceStreamerConnection::SendGoodbyeAck(uint8_t reason)
{
	vector<uint8_t> payload;
	payload.reserve(1);
	TraceStreamerProtocol::WriteU8(payload, reason);
	SendFrame(TraceStreamerProtocol::MakeFrame(TraceStreamerProtocol::MsgType::GoodbyeAck, payload));
}

bool TraceStreamerConnection::SendInfo()
{
	InfoSnapshot snap = GetInfoSnapshot(_emu);

	vector<uint8_t> payload;
	payload.reserve(128);
	TraceStreamerProtocol::WriteU8(payload, snap.HasGame ? 1 : 0);

	if(snap.HasGame) {
		TraceStreamerProtocol::WriteLen16String(payload, snap.FileName);
		TraceStreamerProtocol::WriteLen16String(payload, snap.Sha1);
		TraceStreamerProtocol::WriteU32LE(payload, snap.Crc32);
		TraceStreamerProtocol::WriteU32LE(payload, snap.PrgCrc32);
		TraceStreamerProtocol::WriteU32LE(payload, snap.PrgChrCrc32);
		TraceStreamerProtocol::WriteU16LE(payload, snap.MapperId);
		TraceStreamerProtocol::WriteU8(payload, snap.SubmapperId);
		TraceStreamerProtocol::WriteU8(payload, snap.Mirroring);

		TraceStreamerProtocol::WriteI32LE(payload, snap.PrgRomSize);
		TraceStreamerProtocol::WriteI32LE(payload, snap.ChrRomSize);
		TraceStreamerProtocol::WriteI32LE(payload, snap.WorkRamSize);
		TraceStreamerProtocol::WriteI32LE(payload, snap.SaveRamSize);
		TraceStreamerProtocol::WriteI32LE(payload, snap.ChrRamSize);
		TraceStreamerProtocol::WriteI32LE(payload, snap.SaveChrRamSize);
	}

	SendFrame(TraceStreamerProtocol::MakeFrame(TraceStreamerProtocol::MsgType::Info, payload));
	return snap.HasGame;
}

void TraceStreamerConnection::SendInfoUpdate(bool sendSync, uint8_t syncReason)
{
	if(!_handshakeComplete) {
		return;
	}
	if(!_socket || _socket->ConnectionError()) {
		return;
	}

	bool hasGame = SendInfo();
	if(sendSync && hasGame) {
		SendSync(syncReason);
	}
}

void TraceStreamerConnection::SendSync(uint8_t reason)
{
	SyncSnapshot snap = GetSyncSnapshot(_emu);
	if(!snap.Valid) {
		return;
	}

	vector<uint8_t> payload;
	payload.reserve(32);
	TraceStreamerProtocol::WriteU8(payload, reason);
	TraceStreamerProtocol::WriteCpuCycle40LE(payload, snap.CpuCycleCount);
	TraceStreamerProtocol::WriteI16LE(payload, snap.Scanline);
	TraceStreamerProtocol::WriteU16LE(payload, snap.Dot);
	TraceStreamerProtocol::WriteU16LE(payload, snap.PC);
	TraceStreamerProtocol::WriteU8(payload, snap.A);
	TraceStreamerProtocol::WriteU8(payload, snap.X);
	TraceStreamerProtocol::WriteU8(payload, snap.Y);
	TraceStreamerProtocol::WriteU8(payload, snap.SP);
	TraceStreamerProtocol::WriteU8(payload, snap.PS);

	SendFrame(TraceStreamerProtocol::MakeFrame(TraceStreamerProtocol::MsgType::Sync, payload));
}

bool TraceStreamerConnection::TryReceive()
{
	if(!_socket || _socket->ConnectionError()) {
		return false;
	}

	uint8_t tmp[4096];
	int n = _socket->Recv((char*)tmp, (int)sizeof(tmp), 0);
	if(n > 0) {
		_rxBuf.insert(_rxBuf.end(), tmp, tmp + n);
		return true;
	}
	return false;
}

void TraceStreamerConnection::HandleHello(const uint8_t* payload, size_t len)
{
	if(len < 4) {
		// Protocol error
		_socket->Close();
		return;
	}

	uint16_t major = ReadU16LE(payload);
	uint16_t minor = ReadU16LE(payload + 2);

	if(major != 1) {
		// v1 rule: require major match
		_socket->Close();
		return;
	}

	// We speak 1.0 for now
	(void)minor;
	SendHelloAck(1, 0);
	_handshakeComplete = true;

	// Immediately send INFO and (if hasGame==1) SYNC(Initial)
	bool hasGame = SendInfo();
	if(hasGame) {
		SendSync((uint8_t)TraceStreamerProtocol::SyncReason::Initial);
	}
}

void TraceStreamerConnection::HandleGoodbye(const uint8_t* payload, size_t len)
{
	uint8_t reason = (uint8_t)TraceStreamerProtocol::GoodbyeReason::ClientRequest;
	if(len >= 1) {
		reason = payload[0];
	}

	SendGoodbyeAck(reason);
	_socket->Close();
}

void TraceStreamerConnection::ProcessFrames()
{
	while(true) {
		if(_rxBuf.size() < 3) {
			return;
		}

		uint8_t msgType = _rxBuf[0];
		uint16_t payloadLen = (uint16_t)_rxBuf[1] | ((uint16_t)_rxBuf[2] << 8);
		if(_rxBuf.size() < (size_t)(3 + payloadLen)) {
			return;
		}

		const uint8_t* payload = payloadLen ? (_rxBuf.data() + 3) : nullptr;

		switch((TraceStreamerProtocol::MsgType)msgType) {
			case TraceStreamerProtocol::MsgType::Hello:
				if(!_handshakeComplete) {
					HandleHello(payload, payloadLen);
				}
				break;

			case TraceStreamerProtocol::MsgType::Goodbye:
				HandleGoodbye(payload, payloadLen);
				break;

			default:
				// Unknown / unsupported message type in v1; ignore.
				break;
		}

		_rxBuf.erase(_rxBuf.begin(), _rxBuf.begin() + 3 + payloadLen);

		if(!_socket || _socket->ConnectionError()) {
			return;
		}
	}
}

void TraceStreamerConnection::Poll()
{
	if(!_socket || _socket->ConnectionError()) {
		return;
	}

	// Read all available data
	for(int i = 0; i < 32; i++) {
		bool gotData = TryReceive();
		if(!gotData) {
			break;
		}
		if(_socket->ConnectionError()) {
			return;
		}
	}

	if(_socket->ConnectionError()) {
		return;
	}

	ProcessFrames();
}
