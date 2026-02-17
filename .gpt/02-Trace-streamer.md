# Trace Streamer v1 (BwMesen ↔ NesViz) protocol

This document captures the **Trace Streamer v1** wire format and session behavior we landed on.

Scope of v1:
- **NES-only**
- **iNES-only** (unsupported formats behave like “no game loaded”)
- Connection lifecycle + ROM info + baseline sync (no event streaming yet)

---

## Roles + transport

- **BwMesen (emulator)** is the **TCP server**, listening on loopback only.
- **NesViz** is the **TCP client**, connecting to the emulator to request data.

### Address / ports

- Bind address: `127.0.0.1` only.
- Port probing:
  - Start at **63783** ("NES83")
  - If taken, try the next ports up to **10 attempts** total.
  - Server tries: `63783..63792`
  - Client should probe the same range.

---

## Endianness

All multi-byte integers are **little-endian**.

Strings are sent as raw bytes with a 16-bit length prefix. In practice they’re expected to be UTF-8/ASCII (e.g., filename, SHA1 hex string).

---

## Framing

TCP is a byte stream, so each message is length-framed.

### Frame header

Every message is:

1. `u8  msgType`
2. `u16 payloadLen`
3. `payload[payloadLen]`

No per-message flags field in v1.

---

## Versioning

Versioning is negotiated once via `HELLO` / `HELLO_ACK`.

- Protocol version: `major=1`, `minor=0`
- v1 rule: **server requires major match** (client major must be 1).

---

## Message enums (magic numbers)

These values are intended to be mirrored on the NesViz JS side.

### MsgType (u8)

| Name | Value |
|---|---:|
| `Hello` | `0x01` |
| `HelloAck` | `0x02` |
| `Goodbye` | `0x03` |
| `GoodbyeAck` | `0x04` |
| `Info` | `0x05` |
| `Sync` | `0x06` |

### GoodbyeReason (u8)

| Name | Value |
|---|---:|
| `ClientRequest` | `0` |
| `ServerShutdown` | `1` (reserved) |
| `ProtocolError` | `2` (reserved) |

### SyncReason (u8)

| Name | Value |
|---|---:|
| `Initial` | `0` |
| `LoadState` | `1` |
| `Reset` | `2` |

---

## Message payloads (exact field order)

### 0x01 — HELLO (client → server)

Payload:
1. `u16 major`
2. `u16 minor`

### 0x02 — HELLO_ACK (server → client)

Payload:
1. `u16 major`
2. `u16 minor`

### 0x03 — GOODBYE (client → server)

Payload:
1. `u8 reason` (typically `ClientRequest`)

### 0x04 — GOODBYE_ACK (server → client)

Payload:
1. `u8 reasonEcho`

### 0x05 — INFO (server → client)

Purpose: describe the currently loaded ROM (if supported).

v1 rule: **hasGame=1 only when a supported iNES NES ROM is loaded**. Otherwise `hasGame=0`.

Payload:
1. `u8 hasGame` (0/1)

If `hasGame == 1`, append:

2. `u16 fileNameLen`
3. `u8  fileNameBytes[fileNameLen]`

4. `u16 sha1Len` (0 if unavailable)
5. `u8  sha1Bytes[sha1Len]`

6. `u32 crc32`
7. `u32 prgCrc32`
8. `u32 prgChrCrc32`

9.  `u16 mapperId`
10. `u8  submapperId`
11. `u8  mirroring` (MirroringType enum numeric value as used by Mesen)

Sizes (all `i32`):
12. `i32 prgRomSize`
13. `i32 chrRomSize`
14. `i32 workRamSize`
15. `i32 saveRamSize`
16. `i32 chrRamSize`
17. `i32 saveChrRamSize`

Notes:
- INFO is populated from Mesen’s *effective* ROM info (i.e., whatever the emulator believes is correct after its own header/DB handling).

### 0x06 — SYNC (server → client)

Purpose: establish an absolute baseline that later delta-based event streaming will key off.

When sent (v1):
- Immediately after HELLO/HELLO_ACK (if `hasGame==1`)
- Again whenever emulation state “jumps” (savestate load, reset)

Payload:
1. `u8 reason` (`SyncReason`)

2. `cpuCycle40` as **5 bytes** little-endian (low 40 bits of CPU cycle count)

3. `i16 ppuScanline`
4. `u16 ppuDot` (PPU cycle/dot)

CPU registers:
5. `u16 pc`
6. `u8  a`
7. `u8  x`
8. `u8  y`
9. `u8  sp`
10. `u8 ps`

---

## Session behavior

### Typical connect sequence

1. NesViz connects to `127.0.0.1:<port>`.
2. NesViz sends `HELLO`.
3. BwMesen replies `HELLO_ACK`.
4. BwMesen immediately sends:
   - `INFO`
   - `SYNC` (only if `INFO.hasGame==1`)

If the client’s `HELLO.major != 1`, the server disconnects.

### Disconnect

- NesViz sends `GOODBYE(reason=ClientRequest)`.
- BwMesen replies `GOODBYE_ACK` and closes the connection.

### Game load / unload / state jump

While connected (after HELLO):
- On **GameLoaded**: server sends `INFO` and then `SYNC(reason=Initial)`.
- On **StateLoaded** (savestate): server sends `SYNC(reason=LoadState)`.
- On **GameReset**: server sends `SYNC(reason=Reset)`.
- On **BeforeGameUnload**: server sends `INFO(hasGame=0)`.

No periodic SYNC messages are sent in v1.

---

## Forward compatibility notes

- The outer frame’s `payloadLen` allows skipping unknown message types.
- Additional message types will be added for streaming events (instruction records, mapper events, mapping deltas, etc.).
- v1 intentionally keeps the per-message header minimal (no always-present flags).
