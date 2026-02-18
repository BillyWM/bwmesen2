#pragma once

#include "pch.h"

namespace TraceStreamerProtocol
{
	enum class MsgType : uint8_t
	{
		Hello = 0x01,
		HelloAck = 0x02,
		Goodbye = 0x03,
		GoodbyeAck = 0x04,
		Info = 0x05,
		Sync = 0x06,
	};

	enum class GoodbyeReason : uint8_t
	{
		ClientRequest = 0,
		ServerShutdown = 1,
		ProtocolError = 2,
	};

	enum class SyncReason : uint8_t
	{
		Initial = 0,
		LoadState = 1,
		Reset = 2,
	};

	inline void WriteU8(vector<uint8_t>& out, uint8_t v)
	{
		out.push_back(v);
	}

	inline void WriteU16LE(vector<uint8_t>& out, uint16_t v)
	{
		out.push_back((uint8_t)(v & 0xFF));
		out.push_back((uint8_t)((v >> 8) & 0xFF));
	}

	inline void WriteU32LE(vector<uint8_t>& out, uint32_t v)
	{
		out.push_back((uint8_t)(v & 0xFF));
		out.push_back((uint8_t)((v >> 8) & 0xFF));
		out.push_back((uint8_t)((v >> 16) & 0xFF));
		out.push_back((uint8_t)((v >> 24) & 0xFF));
	}

	inline void WriteI16LE(vector<uint8_t>& out, int16_t v)
	{
		WriteU16LE(out, (uint16_t)v);
	}

	inline void WriteI32LE(vector<uint8_t>& out, int32_t v)
	{
		WriteU32LE(out, (uint32_t)v);
	}

	inline void WriteLen16Bytes(vector<uint8_t>& out, const uint8_t* data, size_t len)
	{
		if(len > 0xFFFF) {
			len = 0xFFFF;
		}
		WriteU16LE(out, (uint16_t)len);
		out.insert(out.end(), data, data + len);
	}

	inline void WriteLen16String(vector<uint8_t>& out, const string& s)
	{
		size_t len = s.size();
		if(len > 0xFFFF) {
			len = 0xFFFF;
		}
		WriteU16LE(out, (uint16_t)len);
		out.insert(out.end(), s.begin(), s.begin() + len);
	}

	inline void WriteCpuCycle40LE(vector<uint8_t>& out, uint64_t cycleCount)
	{
		uint64_t v = cycleCount & 0xFFFFFFFFFFull;
		for(int i = 0; i < 5; i++) {
			out.push_back((uint8_t)(v & 0xFF));
			v >>= 8;
		}
	}

	inline vector<uint8_t> MakeFrame(MsgType type, const vector<uint8_t>& payload)
	{
		vector<uint8_t> out;
		out.reserve(3 + payload.size());
		out.push_back((uint8_t)type);
		WriteU16LE(out, (uint16_t)payload.size());
		out.insert(out.end(), payload.begin(), payload.end());
		return out;
	}
}
