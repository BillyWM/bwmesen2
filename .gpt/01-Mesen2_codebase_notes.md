# Mesen2 codebase notes (focused on Lua, rendering, state/mappers, and extension points)

This document is based on a direct read of the `Mesen2-master` source tree (from the provided zip), with emphasis on:
- where Lua scripts live + how they are executed/sandboxed
- how script drawing reaches the renderer
- how the UI talks to the core (interop boundary)
- where emulation “state” is gathered (including NES mapping + mapper state)
- where trace/log data comes from (useful for streaming to NesViz)

---

## 0) Repo top-level map

Top-level directories of interest:

- `Core/` — C++ emulation core (all systems), debugger, rendering pipeline, script runtime.
- `InteropDLL/` — C++ exported API boundary (DllExport wrappers) used by the .NET UI.
- `UI/` — Avalonia (.NET/C#) desktop app, debugger windows, views/viewmodels, script editor.
- `Lua/` — vendored Lua (and LuaSocket/mime) with **Mesen-specific patches** (sandbox + watchdog timer).

Other platform folders (`Windows/`, `Linux/`, `MacOS/`, etc.) hold platform glue / build support.

---

## 1) High-level architecture: native core + Avalonia UI

Mesen2 is structured as:
- **Native core (C++)** owning emulation, rendering, debugger, scripting.
- **UI app (.NET/Avalonia)** using P/Invoke to call into exported wrapper functions in `InteropDLL/`, plus a notification callback for push events.

Key files on the boundary:
- Native exports (examples):
  - `InteropDLL/EmuApiWrapper.cpp` (emulator lifecycle, rendering device registration, etc.)
  - `InteropDLL/DebugApiWrapper.cpp` (debugger data/control: scripts, breakpoints, trace rows, state queries…)
- UI-side P/Invoke declarations:
  - `UI/Interop/EmuApi.cs`
  - `UI/Interop/DebugApi.cs` (and peers: `HistoryApi.cs`, etc.)
- Push notifications from core → UI:
  - Native: `Core/Shared/NotificationManager.*`, `InteropDLL/InteropNotificationListener.h`, `InteropDLL/InteropNotificationListeners.h`
  - UI: `UI/Interop/NotificationListener.cs`

The pattern is:
1. UI creates the emulator via `EmuApi.InitializeEmu(...)` (native: `InteropDLL/EmuApiWrapper.cpp`).
2. UI registers a notification callback (native → managed) via `EmuApi.RegisterNotificationCallback(...)`.
3. Core sends notifications via `NotificationManager::SendNotification(ConsoleNotificationType, void*)`.

---

## 2) Rendering pipeline and how “script HUD” is composited

### 2.1 Core renderer objects

Core objects (owned by `Emulator`) relevant to rendering:
- `Core/Shared/Emulator.h`
  - `unique_ptr<VideoRenderer> _videoRenderer;`
  - `unique_ptr<VideoDecoder> _videoDecoder;`
  - `unique_ptr<DebugHud> _debugHud;` (regular/hardwired HUD: FPS, OSD, etc.)
  - `unique_ptr<DebugHud> _scriptHud;` (**script-controlled** HUD surface)
- `Core/Shared/Video/VideoRenderer.*` — spawns a render thread and calls `IRenderingDevice::Render(...)` with:
  - an “emu HUD” surface (system HUD + input HUD + debug HUD)
  - a “script HUD” surface (drawn from `Emulator::_scriptHud`)
- `Core/Shared/Video/DebugHud.*` — command buffer of primitives (strings, rectangles, pixels, etc.) with optional frame lifetimes.

**Important composition detail (from `VideoRenderer::RenderThread` and `VideoRenderer::DrawScriptHud`):**
- `VideoRenderer` runs its own thread and periodically renders even when paused (~30fps) so HUD can update.
- The script HUD surface is separate (`_scriptHudSurface`) and is *only redrawn on new frames*.
- Script HUD clearing/drawing is optimized:
  - if scripts queued commands for the frame, `GetScriptHud()->Draw(...)` runs
  - otherwise the surface can be cleared and remain blank (to avoid wasting CPU)

This implies: **a Lua script that wants a persistent overlay typically needs to issue draw commands every frame** (unless paused, where the last drawn frame remains visible).

### 2.2 Software renderer path in UI (useful for understanding layering)

The software renderer path makes the layering especially explicit:

- Native: `Core/Shared/Video/SoftwareRenderer.cpp`
  - builds a `SoftwareRendererFrame` containing:
    - base frame pixels
    - emu HUD surface (dirty flag)
    - script HUD surface (dirty flag)
  - sends `ConsoleNotificationType::RefreshSoftwareRenderer` with a pointer to the frame struct:
    - `_emu->GetNotificationManager()->SendNotification(ConsoleNotificationType::RefreshSoftwareRenderer, &frame);`

- UI: `UI/Windows/MainWindow.axaml.cs`
  - on `RefreshSoftwareRenderer`, marshals the struct and calls:
    - `_softwareRenderer.UpdateSoftwareRenderer(frame);`

- UI view control: `UI/Controls/SoftwareRendererView.axaml` + `.axaml.cs`
  - renders **three stacked images**: `Frame`, `EmuHud`, `ScriptHud`
  - `UpdateSoftwareRenderer(...)` copies the native buffers into `DynamicBitmap`s and invalidates visuals.

This stack is the clearest “layout” for script drawing today: scripts can only draw to the script HUD bitmap (or the console/debug HUD).

---

## 3) Lua scripting system (core side)

### 3.1 “Script host” stack

There are three main layers:

1. **`ScriptManager`** — owns all active scripts and routes events/memory operations to them  
   - `Core/Debugger/ScriptManager.*`
   - Key responsibilities:
     - Manage script list (`AddScript`, `RemoveScript`, `ClearScripts`)
     - Track “do we have callbacks?” flags:
       - `_hasCpuMemoryCallbacks`, `_hasPpuMemoryCallbacks`  
       - `UpdateMemoryCallbackFlags()` calls each `ScriptHost::RefreshMemoryCallbackFlags()`
     - Broadcast emulator events to scripts: `ProcessEvent(EventType, CpuType)`
     - Broadcast memory ops: `ProcessMemoryOperation(...)`

2. **`ScriptHost`** — per-script wrapper, owns a `ScriptingContext`  
   - `Core/Debugger/ScriptHost.*`
   - Mostly glue: load script, fetch log, call `CallMemoryCallback` / `ProcessEvent`.

3. **`ScriptingContext`** — the per-script Lua VM (`lua_State*`) + callback registry  
   - `Core/Debugger/ScriptingContext.*`
   - Stores:
     - `_lua` (VM)
     - event callbacks array sized by `EventType::LastValue`
     - memory callbacks arrays keyed by `MemoryType` for CPU + PPU
     - current script draw surface (`ScriptDrawSurface::ConsoleScreen` or `ScriptHud`)
     - a log buffer (string + lock)

### 3.2 Lua VM creation, sandboxing, and watchdog timeout

`ScriptingContext::LoadScript(...)` shows the full setup:
- Creates Lua state: `luaL_newstate()`
- Loads libraries via `ScriptingContext::LuaOpenLibs(L, allowIoOsAccess)`:
  - When sandboxed (`allowIoOsAccess == false`), it **skips** IO, OS, and the package/loadlib system.
- Uses a global `SANDBOX_ALLOW_LOADFILE` to prevent loading files:
  - defined in **patched** Lua: `Lua/lauxlib.c` and declared in `Lua/lauxlib.h`
- Optional networking support:
  - If IO/OS is allowed *and* `DebugConfig.ScriptAllowNetworkAccess` is true, it preloads LuaSocket core:
    - sets `package.preload["socket.core"] = luaopen_socket_core`
    - sets `package.preload["mime.core"] = luaopen_mime_core`

Timeout enforcement:
- Uses a patched Lua API `lua_setwatchdogtimer` (defined in `Lua/ldebug.c`, declared in `Lua/lua.h`).
- `ScriptingContext::ExecutionCountHook` checks elapsed wall-clock time vs `DebugConfig.ScriptTimeout` and throws a Lua error if exceeded.

Relevant settings live in:
- `Core/Shared/SettingTypes.h` → `DebugConfig`
  - `bool ScriptAllowIoOsAccess`
  - `bool ScriptAllowNetworkAccess`
  - `uint32_t ScriptTimeout` (seconds)

### 3.3 How Lua sees Mesen: the `emu` library

The core registers a Lua library named `emu` via:
- `luaL_requiref(_lua, "emu", LuaApi::GetLibrary, 1);`
- Library definition lives in:
  - `Core/Debugger/LuaApi.*`

`LuaApi::GetLibrary` registers a table of functions and enums (via `magic_enum`) such as:
- Memory read/write APIs (multiple `MemoryType`s)
- Callback registration APIs:
  - event callbacks (NMI/IRQ/frame/reset/etc.)
  - memory callbacks (CPU/PPU read/write/execute)
- Drawing APIs:
  - `DrawString`, `DrawRectangle`, `DrawLine`, `DrawPixel`, etc.
  - `SelectDrawSurface(surface, scale)`
- Emulator control:
  - reset/pause/resume/step/run
  - save/load state
- Input query/override
- ROM/system info helpers
- Debugger tools (CDL data, access counters, etc.)

Parameter marshaling is centralized in:
- `Core/Debugger/LuaCallHelper.*`

---


### 3.4 UI-side script editor/window (how scripts are loaded from C#)

On the UI side, the Lua workflow is primarily driven by the Script Window:

- Window + layout:
  - `UI/Debugger/Windows/ScriptWindow.axaml` (Avalonia window)
- ViewModel:
  - `UI/Debugger/ViewModels/ScriptWindowViewModel.cs`

The ViewModel:
- Loads/updates scripts by calling interop functions exported from `InteropDLL/DebugApiWrapper.cpp`:
  - `LoadScript(name, path, content, scriptId)`
  - `RemoveScript(scriptId)`
  - `GetScriptLog(scriptId, ...)`
- Implements “run / stop / reload” behavior and (optionally) watches the script file on disk to hot-reload when the file changes.

So the end-to-end path for “run script” is:

`ScriptWindowViewModel` (C#) → `DebugApi` P/Invoke → `InteropDLL/DebugApiWrapper.cpp` → `Core/Debugger/ScriptManager` → `ScriptHost` → `ScriptingContext` → Lua VM


## 4) Script drawing surfaces: ConsoleScreen vs ScriptHud

Lua chooses a draw target via:
- `LuaApi::SelectDrawSurface(lua)` (see `Core/Debugger/LuaApi.cpp`)

Two surfaces exist:
- `ScriptDrawSurface::ConsoleScreen`
  - routes to `Emulator::GetDebugHud()` (drawn with the rest of the normal HUD)
  - scale must be 1 (`LuaApi` enforces it)
- `ScriptDrawSurface::ScriptHud`
  - routes to `Emulator::GetScriptHud()` (separate surface in `VideoRenderer`)
  - scale can be 1–4; `LuaApi` calls `VideoRenderer::SetScriptHudScale(surfaceScale)`

Internally:
- `LuaApi::GetHud()` picks between `_emu->GetDebugHud()` and `_emu->GetScriptHud()`.

This is the main “UI-like” capability currently available to scripts: immediate-mode drawing of primitives/text onto a HUD surface.

---

## 5) How scripts receive events and memory callbacks

### 5.1 Event callbacks

Event types are defined in:
- `Core/Shared/EventType.h`:
  - `Nmi`, `Irq`, `StartFrame`, `EndFrame`, `Reset`, `ScriptEnded`, `InputPolled`, `StateLoaded`, `StateSaved`, `CodeBreak`

The debugger calls script events through `ScriptManager::ProcessEvent(...)`, which calls each `ScriptHost::ProcessEvent(...)`, which calls:
- `ScriptingContext::CallEventCallback(EventType, CpuType)`

`CallEventCallback` pushes the CPU type into the Lua callback; scripts can branch on which CPU fired the event.

### 5.2 Memory callbacks (CPU, PPU, execute)

The debugger is the main hook point:
- `Core/Debugger/Debugger.cpp`
  - `Debugger::ProcessInstruction<CpuType>()` checks `_scriptManager->HasCpuMemoryCallbacks()` and emits an `ExecOpCode` memory operation after an instruction, using `InstructionProgress.LastMemOperation`.
  - `Debugger::ProcessMemoryRead` / `Debugger::ProcessMemoryWrite` call into debugger-specific “ProcessRead/Write” implementations; there are helper templates:
    - `Debugger::ProcessScripts(...)` → `_scriptManager->ProcessMemoryOperation(...)`

`ScriptManager::ProcessMemoryOperation` decides which scripts to call based on:
- whether the callback is CPU vs PPU memory
- whether it’s read/write vs execute
- callback type (`CallbackType` / `MemoryOperationType`)

On the Lua side, the callbacks are stored by `ScriptingContext` in per-memory-type arrays and invoked with:
- `ScriptingContext::CallMemoryCallback(relAddr, value, callbackType, cpuType)`

(“relAddr” is an `AddressInfo` struct with `Address` + `MemoryType`.)

---

## 6) NES emulation status: ROM info, mapping, and mapper state

### 6.1 ROM loading and “database header” behavior

ROM loading entry point:
- `Core/NES/Loaders/RomLoader.cpp`

It:
- reads the raw file into `RomData.RawData`
- computes CRC32
- tries loaders based on signature:
  - iNES (`NES\x1a`)
  - FDS, NSF, NSFE, UNIF, etc.
- **If no known signature**:
  - calls `GameDatabase::GetiNesHeader(crc, header)` and, if found, treats it as a *headerless* ROM using DB-provided iNES header info.

Database logic:
- `Core/NES/GameDatabase.*`

This is relevant if you’re thinking about “effective” header vs literal file header: the core can synthesize an iNES header for headerless files, and it can also update `RomData` based on DB info via `GameDatabase::SetGameInfo(...)`.

### 6.2 NES “state snapshot” used by the debugger/UI

Console state structure:
- Native: `Core/NES/NesTypes.h` → `struct NesState { Cpu, Ppu, Cartridge, Apu, ClockRate }`
- UI marshal: `UI/Interop/ConsoleState/NesState.cs`

Population:
- `Core/NES/NesConsole.cpp` → `NesConsole::GetConsoleState(BaseState&, ConsoleType)`
  - `state.Cpu = _cpu->GetState()`
  - `_ppu->GetState(state.Ppu)`
  - `state.Cartridge = _mapper->GetState()`
  - `state.Apu = _apu->GetState()`

The key piece for NesViz-style tooling is **`CartridgeState`**, populated by:
- `Core/NES/BaseMapper.cpp` → `BaseMapper::GetState()`

### 6.3 CartridgeState: PRG/CHR mapping + per-mapper extra registers

`CartridgeState` (native in `Core/NES/NesTypes.h`, UI mirror in `UI/Interop/ConsoleState/NesState.cs`) contains:

**Mapping tables**
- PRG:
  - `PrgPageCount`, `PrgPageSize`
  - `PrgMemoryOffset[0x100]` (256 x 256-byte blocks covering CPU space)
  - `PrgType[0x100]` (PRG ROM / WRAM / SRAM / mapper RAM, etc.)
  - `PrgMemoryAccess[0x100]` (read/write flags)
- CHR/PPU:
  - `ChrPageCount`, `ChrPageSize`, `ChrRamPageSize`
  - `ChrMemoryOffset[0x40]` (64 x 256-byte blocks covering 16KB PPU space)
  - `ChrType[0x40]`
  - `ChrMemoryAccess[0x40]`

**Other cartridge properties**
- mirroring mode (`MirroringType`)
- battery flag
- WRAM/SRAM page sizes

**Custom per-mapper entries**
- `CustomEntries[200]` + `CustomEntryCount`
- filled from `BaseMapper::GetMapperStateEntries()` (virtual; per-mapper override)
- each entry is a `MapperStateEntry` with:
  - `Address`, `Name`, `Value`, `RawValue`, `Type`

This is the “official” place Mesen exposes mapper-specific internal registers to the debugger UI.

### 6.4 Where the UI consumes mapping + mapper state

Example: memory mapping window:
- `UI/Debugger/ViewModels/MemoryMappingViewModel.cs`
  - fetches `DebugApi.GetConsoleState<NesState>(ConsoleType.Nes).Cartridge`
  - computes CPU + PPU mapping blocks from `PrgMemoryOffset/Type/Access` and `ChrMemoryOffset/Type/Access`

Other windows (register viewer, etc.) also pull from `GetConsoleState`.

---

## 7) Trace logging: where execution traces are built and exported

Core trace infrastructure:
- `Core/Debugger/ITraceLogger.h` (interface)
- `Core/Debugger/BaseTraceLogger.h` (large generic implementation base)
- `Core/Debugger/TraceLogFileSaver.h` (simple buffered file writer)

Debugger entry point:
- `Core/Debugger/Debugger.cpp` → `Debugger::GetExecutionTrace(TraceRow output[], uint32_t startOffset, uint32_t maxLineCount)`
  - merges trace rows across all active CPU types by rowId ordering.

Interop exposure:
- `InteropDLL/DebugApiWrapper.cpp`
  - `DllExport uint32_t __stdcall GetExecutionTrace(TraceRow output[], uint32_t startOffset, uint32_t lineCount)`
  - plus `SetTraceOptions`, `ClearExecutionTrace`
  - plus `StartLogTraceToFile`, `StopLogTraceToFile`

This is already a strong “data tap” for NesViz if you’d rather stream trace rows than re-derive instruction flow from raw CPU state.

---

## 8) Where Lua is *already* close to what you want (and where it isn’t)

### 8.1 Socket / external comms
Out of the box:
- If `DebugConfig.ScriptAllowIoOsAccess` **and** `DebugConfig.ScriptAllowNetworkAccess` are enabled, scripts can `require("socket.core")` via the preloaded LuaSocket core (see `ScriptingContext::LoadScript`).
- This means a script can already open TCP/UDP sockets and push data out — but it’s Lua-level and gated by settings.

If you want higher-performance / lower-overhead streaming:
- A native-side socket sender would likely live either:
  - as an additional exported “tooling” API in `InteropDLL/DebugApiWrapper.cpp` (so NesViz can connect directly), or
  - as a new LuaApi binding (so Lua can call `emu.sendTrace(...)` cheaply).

### 8.2 Script-triggered UI controls (buttons/text inputs, “web-ish”)
Current capabilities:
- immediate-mode drawing primitives to HUD surfaces (`DebugHud`) via Lua (`emu.drawString`, etc.)
- OSD messages via `emu.displayMessage(...)` → `MessageManager` → `SystemHud` overlay

What’s missing for “real UI”:
- There is **no bridge** from Lua to the Avalonia UI tree. Lua runs inside the core debugger process/thread, and UI widgets live in C# on the UI thread.
- The existing push channel core → UI is `NotificationManager` + `RegisterNotificationCallback`, but currently it carries only a fixed enum `ConsoleNotificationType` and (rarely) a pointer payload.

The most “Mesen-native” extension point to add a script UI panel would be:
1. Add a new notification type (e.g. `ScriptUiCommand`) to `ConsoleNotificationType` in:
   - `Core/Shared/Interfaces/INotificationListener.h`
   - `UI/Interop/NotificationListener.cs` (mirror enum)
2. Define a POD payload struct for UI commands (create panel, add button, set text, etc.) and send it from core.
3. In the UI, implement a “Script UI” tool window/panel (Avalonia) that listens to these notifications and updates viewmodels/controls on the UI thread.

Threading note: the `RefreshSoftwareRenderer` path demonstrates that core can push a struct pointer and UI can copy it immediately. For a script UI system you’d probably want:
- either “copy immediately” payloads (small, fixed-size) or
- a shared queue in core with an interop “pull” API (UI polls and marshals strings safely), to avoid lifetime issues of passing pointers to temporary memory.

---

## 9) Quick reference: key files by topic

### Lua + scripting
- `Core/Debugger/ScriptingContext.*` — per-script Lua VM, sandbox, callback registry, log
- `Core/Debugger/LuaApi.*` — `emu` library implementation (draw, memory, events, control, etc.)
- `Core/Debugger/LuaCallHelper.*` — stack/param helper for Lua bindings
- `Core/Debugger/ScriptManager.*` — manages scripts, routes events/memory ops
- `Core/Debugger/ScriptHost.*` — per-script wrapper

### Rendering / HUD composition
- `Core/Shared/Emulator.h` — owns `_debugHud` and `_scriptHud`
- `Core/Shared/Video/VideoRenderer.*` — render thread; composes emu HUD + script HUD
- `Core/Shared/Video/DebugHud.*` — draw command queue + rasterization
- `Core/Shared/Video/SoftwareRenderer.cpp` — pushes `RefreshSoftwareRenderer` notifications
- UI layering (software path):
  - `UI/Controls/SoftwareRendererView.axaml` (+ `.cs`)
  - `UI/Windows/MainWindow.axaml.cs` (notification handling)

### NES mapping + mapper state
- `Core/NES/BaseMapper.*` — mapping tables, `GetState()`, `GetMapperStateEntries()`
- `Core/NES/NesTypes.h` — `CartridgeState`, `MapperStateEntry`, `NesState`
- `Core/NES/NesConsole.cpp` — `GetConsoleState(...)` fills `NesState`
- UI marshal:
  - `UI/Interop/ConsoleState/NesState.cs`
- UI mapping display:
  - `UI/Debugger/ViewModels/MemoryMappingViewModel.cs`

### Trace logging / execution history
- `Core/Debugger/ITraceLogger.h`, `BaseTraceLogger.h`
- `Core/Debugger/Debugger.cpp` → `GetExecutionTrace(...)`
- `InteropDLL/DebugApiWrapper.cpp` — exports trace APIs to UI/clients

### Core ↔ UI notifications
- Core:
  - `Core/Shared/NotificationManager.*`
  - `Core/Shared/Interfaces/INotificationListener.h` (enum + payload structs)
- Interop:
  - `InteropDLL/InteropNotificationListener.h`
  - `InteropDLL/InteropNotificationListeners.h`
- UI:
  - `UI/Interop/NotificationListener.cs`

---

## 10) What I would look at next (if you want a deeper pass)

If you want a follow-up doc tailored to the exact two features you listed (script-side UI + trace socket), the next “deep dive” targets would be:
- How `Debugger` schedules/calls instruction trace logging (trace row creation sites, not just retrieval).
- How the UI builds tool windows and docks/anchors panels (Avalonia layout strategy used in `MainWindow.axaml`).
- How much of the existing “tool window” framework (script window, memory mapping window, event viewer) can be reused for a persistent “Lua Tool Panel”.


## 11) Deep dive: where execution trace rows are created (not just retrieved)

This section answers the “where are trace rows *created*?” question and lays out the exact call chain.

### 11.1 The call chain from CPU bus access → debugger → trace logger

For NES (pattern is similar for other systems):

1) **CPU executes and performs bus accesses**
- `NesCpu::MemoryRead()` / `NesCpu::MemoryWrite()` call into `NesMemoryManager::Read(...)` / `Write(...)` with a `MemoryOperationType` such as:
  - `ExecOpCode` (opcode fetch)
  - `ExecOperand` (operand fetch)
  - `Read` / `Write` (data accesses)
  - plus a bunch of special types (`DmaRead`, `DmcRead`, `SpriteDmaRead`, etc.)
  - See: `Core/NES/NesCpu.cpp` and `Core/NES/NesMemoryManager.cpp`.

2) **Memory manager forwards each access into the shared debugger plumbing**
- `NesMemoryManager::Read/Write` call `Emulator::ProcessMemoryRead/ProcessMemoryWrite` right before doing the actual bus operation.
  - See: `Core/NES/NesMemoryManager.cpp`.

3) **`Emulator` routes the callback to `Debugger` (and scripts/breakpoints/etc.)**
- `Emulator::ProcessMemoryRead<CpuType::Nes>` calls into `Debugger::ProcessMemoryRead<CpuType::Nes>`.
- Likewise for writes.
  - See: `Core/Shared/Emulator.h` (templated methods).

4) **`Debugger` dispatches into the per-console/per-cpu debugger (`NesDebugger`)**
- `Debugger::ProcessMemoryRead<CpuType::Nes>` builds a `MemoryOperationInfo` and calls `GetDebugger<CpuType::Nes>()->ProcessRead(operation, state, prevOp, breakpointType)`.
- Same for `ProcessMemoryWrite` → `ProcessWrite(...)`.
  - See: `Core/Debugger/Debugger.cpp`.

5) **`NesDebugger` is where trace rows are *actually produced***
- `NesDebugger::ProcessRead(...)` contains the core logic for **trace logging**.
  - When the read is an **opcode fetch** (`operation.Type == MemoryOperationType::ExecOpCode`) and tracing is enabled:
    - it disassembles (`Disassemble(operation.Address, ...)`)
    - snapshots CPU state (`GetState()`) + ppu timing (`GetPpuState()`)
    - computes `AddressInfo` (rel/abs mapping)
    - then calls `traceLogger->Log(cpuState, ppuState, disassemblyInfo, operation, addressInfo)`
      - See: `Core/NES/Debugger/NesDebugger.cpp`.

- For **non-opcode** ops (operand fetch, data reads/writes, DMC reads, etc.), `NesDebugger` calls:
  - `traceLogger->LogNonExec(operation)`
  - This exists to support conditions that may match on *later* memory ops (details below).

### 11.2 How `BaseTraceLogger` turns “an opcode happened” into a stored row

`NesTraceLogger` is a thin derived type; almost all mechanics live in:
- `Core/Debugger/BaseTraceLogger.h`

Key moving parts:

- **Ring buffer** of fixed size (default 30,000):
  - `_rowIds[_bufferSize]`, `_cpuState[_bufferSize]`, `_ppuState[_bufferSize]`, `_byteCode[_bufferSize]`.
  - `_currentPos` is the circular write pointer.

- **Global row ordering** across CPUs:
  - `ITraceLogger::NextRowId` is an `atomic<uint32_t>`.
  - Each added row gets a unique increasing `rowId`, which is what `Debugger::GetExecutionTrace(...)` later uses to merge multiple CPU logs.
  - See: `Core/Debugger/ITraceLogger.h`.

- **Row formatting**:
  - Options include a format string that’s parsed into `_rowParts`.
  - `NesTraceLogger::GetTraceRow(...)` walks `_rowParts` and renders each tag (A/X/Y/SP/P, plus shared tags handled via `ProcessSharedTag(...)` in the base).
  - See: `Core/NES/Debugger/NesTraceLogger.cpp`.

- **Row creation happens in `AddRow(...)`**
  - Writes ring buffer fields.
  - Optionally calls `GetTraceRow(...)` and streams to file if `TraceLogFileSaver` is enabled.
  - See: `BaseTraceLogger::AddRow(...)` in `Core/Debugger/BaseTraceLogger.h`.

### 11.3 The “pending row” mechanism (important if you want to stream)

A subtle but important behavior:

- `BaseTraceLogger::Log(...)` evaluates a *condition* via `_conditionData.Condition->CheckCondition(...)`.
- If the condition **does not match** on the opcode fetch, the logger **does not discard immediately**. Instead it saves:
  - `_pendingLog = true`
  - `_lastState`, `_lastPpuState`, `_lastDisassemblyInfo`, `_lastOperation`, `_lastAddressInfo`, `_lastByteCode`.

- Later, when `NesDebugger` observes non-exec ops, it calls `LogNonExec(...)`.
  - `LogNonExec(...)` re-evaluates the condition against the non-exec memory op.
  - If it matches, it flushes by calling `AddRow(...)` with the stored “last opcode” state.

This effectively supports conditions like “log the *instruction* only if it performs a matching read/write later.”
If you stream trace rows over a socket, you need to decide whether you want this same semantics (i.e., row emission may occur *after* the opcode fetch) or you prefer “always emit at opcode fetch time.”

### 11.4 Where the UI “pulls” from (so you can hook your socket)

Trace rows are *stored* inside the per-cpu trace loggers, but the UI typically pulls them via:

- `Debugger::GetExecutionTrace(TraceRow output[], uint32_t startOffset, uint32_t maxRowCount)`
  - Requests data from each CPU’s `ITraceLogger::GetExecutionTrace(...)`
  - Merges rows by `rowId`
  - Writes a contiguous output slice
  - See: `Core/Debugger/Debugger.cpp`.

- The Avalonia trace log UI (`TraceLoggerWindow`) refreshes on notifications like `PpuFrameDone` or `CodeBreak` and calls `DebugApi.GetExecutionTrace(...)` indirectly.
  - See: `UI/Debugger/Windows/TraceLoggerWindow.axaml.cs`.

If your goal is “stream trace rows to NesViz”, the lowest-friction native hook points are:
- `BaseTraceLogger::AddRow(...)` (row is finalized and in-ring)
- or `NesDebugger::ProcessRead(...)` at `ExecOpCode` (before condition/pending semantics)

## 12) Deep dive: debugger docking & tool windows (Dock.Avalonia)

The “tool window docking” system isn’t in `MainWindow.axaml`; it’s primarily inside the **debugger window**.

### 12.1 MainWindow layout is simple DockPanel (no docking framework)

- `UI/Windows/MainWindow.axaml` uses:
  - a `DockPanel` with a `MesenMenu` at the top
  - an `AudioPlayerHud` at the bottom
  - a `MainRenderer` filling the rest

This is why “a persistent Lua panel docked to the emulator window” would be a *new* feature (not just reusing an existing dock host).

### 12.2 DebuggerWindow is the docking host

- `UI/Debugger/Windows/DebuggerWindow.axaml` contains:
  - top menu/toolbars
  - an optional bottom `MemoryMappings` strip
  - and then `<idc:DockControl Layout="{Binding DockLayout}" />`

The docking framework is **Dock.Avalonia** (`Dock.Avalonia`, `Dock.Model.Avalonia` namespaces).

### 12.3 How the dock layout is built + persisted

- `DebuggerWindowViewModel` creates:
  - `DockFactory = new DebuggerDockFactory(Config.SavedDockLayout);`
  - `DockLayout = DockFactory.GetLayout();`
  - and later `DockFactory.InitLayout(DockLayout);`
  - See: `UI/Debugger/ViewModels/DebuggerWindowViewModel.cs`.

- Layout persistence:
  - `DebuggerConfig.SavedDockLayout` stores a serializable tree of `DockEntryDefinition`.
  - On window close: `_model.Dispose()` eventually calls `DebuggerWindowViewModel.Dispose()` which saves the dock tree via `DockFactory.ToDockDefinition(DockLayout)`.
  - On next open: `DebuggerDockFactory` uses `FromDockDefinition(...)` to reconstruct the tree.
  - See:
    - `UI/Config/Debugger/DebuggerConfig.cs` (`SavedDockLayout`)
    - `UI/Debugger/DebuggerDockFactory.cs` (`ToDockDefinition`, `FromDockDefinition`)

### 12.4 What a “tool” is in this dock system

Mesen wraps each dockable tool as a **container** around the real view model:

- `ToolContainerViewModel` (dockable)
  - has:
    - `ToolTypeName` (stable string identifier for persistence)
    - `Model` (the underlying view model: `DisassemblyViewModel`, `WatchListViewModel`, etc.)
  - disables some dock features:
    - `CanFloat = false`, `CanPin = false` by default
  - See: `UI/Debugger/ViewModels/DebuggerDock/ToolContainerViewModel.cs`.

- `ToolContainerView` is the matching view.
  - It’s basically a `ContentControl` that displays the contained `Model`.
  - See: `UI/Debugger/Views/DebuggerDock/ToolContainerView.axaml`.

- `DebuggerDockFactory` defines all built-in tools:
  - `DisassemblyTool`, `WatchTool`, `BreakpointsTool`, `LabelsTool`, `CallStackTool`, `FunctionListTool`, etc.
  - It also defines the **default layout** as a tree of docks.
  - See: `UI/Debugger/DebuggerDockFactory.cs`.

### 12.5 How tools are opened/closed at runtime

`DebuggerWindowViewModel` contains logic that interacts with `DockFactory`:

- `ToggleTool(tool)` checks visibility, then calls:
  - `DockFactory.CloseDockable(tool)` or `OpenTool(tool)`.

- `OpenTool(tool)` tries to insert it into an existing visible `ToolDock`.
  - If none is suitable, it creates a new section by splitting the main dock.

This matters for a “Lua Tool Panel” because you’d get panel placement “for free” if you implement it as another `ToolContainerViewModel`.

## 13) Deep dive: what’s already reusable for a Lua-driven UI panel

You asked about “a whole window to the side that Lua scripts can open, with buttons, text inputs, etc, web-ish.”
Here’s what exists today that you can reuse, and where you’d need new glue.

### 13.1 Two existing UI patterns you can piggyback on

**Pattern A — Dedicated tool windows (`MesenWindow` subclasses):**
- Examples:
  - `ScriptWindow` (`UI/Debugger/Windows/ScriptWindow.*`)
  - `EventViewerWindow` (`UI/Debugger/Windows/EventViewerWindow.*`)
  - `TraceLoggerWindow` (`UI/Debugger/Windows/TraceLoggerWindow.*`)
- Common behavior:
  - Derived from `MesenWindow` for global font rendering mode + DataContext disposal.
  - Each window has a `Config` object that calls `LoadWindowSettings/SaveWindowSettings(this)`.
  - Debug windows are tracked by `DebugWindowManager`, which also gates debugger lifetime.

This is the simplest way to get “a whole window” that can be opened/closed reliably.

**Pattern B — Docked tools inside DebuggerWindow (Dock.Avalonia):**
- If you want “a panel docked to the side” and you’re OK with it living *inside the debugger window*, adding a new tool is mechanically straightforward:
  - define a new ViewModel + View
  - add a `ToolContainerViewModel` entry in `DebuggerDockFactory`
  - add it to the default layout and/or open-on-demand
  - ensure it has a stable `ToolTypeName` so it can be persisted

The main *policy* question: do you want this UI available only when the debugger workspace is open (current architecture), or always next to the emulator window?

### 13.2 What’s missing for “Lua defines the UI”

Avalonia itself will happily render buttons, text boxes, lists, etc. What Mesen does *not* currently have is:

- A native “UI API” exposed to Lua that can:
  - create controls
  - update control properties
  - receive events (button clicked, text changed)

Lua today mostly interacts with:
- emulator state (memory, CPU regs, PPU data)
- hooks/callbacks (memory read/write/exec, events)
- drawing overlays (pixel/rect/text on the emulator surface)

So you’d be adding a new bridge layer between:
- **Lua thread / emulation thread** (where callbacks originate)
- and the **Avalonia UI thread** (where controls must be created/updated)

The main architectural “seam” for that bridge is the existing notification + UI-thread dispatch patterns you already see in tool windows:
- `DebugWindowManager.ProcessNotification(...)` fans out notifications.
- Tool windows typically use `Dispatcher.UIThread.Post(...)` when receiving core notifications.

### 13.3 Practical reuse suggestion (based on existing patterns)

If you want maximum reuse of existing infrastructure:

- Build a new **debug tool window** (Pattern A) that hosts a “Lua UI panel” view.
  - You get window persistence + open/close plumbing for free.
  - Your Lua API can expose something like “ensure the Lua panel window is open” and “send UI update messages”.

If you specifically want “docked next to emulator window” (not debugger window):
- there is *no existing dock host* in `MainWindow` today, so you’d either:
  - modify `MainWindow.axaml` to add a dock host / side panel region
  - or create a separate always-on-top/always-adjacent window and manage its position relative to the main window

## 14) Deep dive: sockets already in the tree (useful for a trace→NesViz link)

There are two *existing* socket-related mechanisms worth knowing about.

### 14.1 Native C++ socket wrapper (used by Netplay)

- Mesen has a cross-platform TCP socket wrapper:
  - `Utilities/Socket.h` / `Utilities/Socket.cpp`
- It’s used heavily in:
  - `Core/Netplay/*` (client/server, message framing, etc.)

Key characteristics (relevant if you want to stream trace rows):
- sockets are set to **non-blocking**
- buffers are set to 256k
- Nagle is disabled (`TCP_NODELAY`) to reduce latency

### 14.2 LuaSocket is already vendored + optionally enabled for scripts

- The repo vendors LuaSocket + mime under `Lua/`.
- `ScriptingContext::LoadScript(...)` preloads `socket.core` and `mime.core` into `package.preload` *if*:
  - `ScriptAllowIoOsAccess` and `ScriptAllowNetworkAccess` are enabled
  - See: `Core/Debugger/ScriptingContext.cpp`.

So today, a Lua script *can already open sockets* and send data outward (subject to security settings).

For your “trace socket” feature, this gives you two distinct implementation styles:
- **Lua-level streaming**: easiest to prototype (NesViz connects to a script)
- **Native-level streaming**: better for performance and “always available” tooling (NesViz connects to Mesen itself)



---

## 15) Opening a *second* “Lua UI render surface” window when Script Window is opened

You asked for: when the user uses the menu item **Open Script Window**, Mesen should open **two windows**:
1) the existing `ScriptWindow` (editor/log)
2) a new second window that is **only** a rendering surface for Avalonia elements driven by your own DOM-ish markup layer.

### 15.1 Where the Script Window menu item is wired

The main menu’s “Script Window” action is defined in:
- `UI/ViewModels/MainMenuViewModel.cs`
  - `ActionType.OpenScriptWindow`
  - `OnClick = () => DebugWindowManager.OpenDebugWindow(() => new ScriptWindow(new ScriptWindowViewModel(null)))`

Key implication: the menu uses **`DebugWindowManager.OpenDebugWindow(...)`** (not `GetOrOpenDebugWindow`). That means:
- it intentionally supports **multiple ScriptWindows** (each click opens another)
- it also ensures debug-workspace lifetime + notification routing is enabled for the window

### 15.2 What `DebugWindowManager` buys you (and why you probably want it for the new window)

`UI/Debugger/Utilities/DebugWindowManager.cs` is the glue that makes “debug windows” behave consistently:

- **Debugger workspace lifetime gating**
  - first debug window opened → `DebugApi.LoadDebugWorkspace()`
  - last debug window closed → `DebugApi.SaveDebugWorkspace()` and `DebugApi.ReleaseDebugWorkspace()`

- **Centralized notification fan-out**
  - `MainWindow` receives core notifications via `NotificationListener`
    - `UI/Windows/MainWindow.axaml.cs` calls `DebugWindowManager.ProcessNotification(e)`
  - `DebugWindowManager` forwards notifications to any open windows that implement `INotificationHandler`

- **Bring-to-front / show behavior**
  - windows are shown immediately; if already visible it can request focus/raise

- **A real-world deadlock workaround**
  - `ProcessNotification(...)` uses locking plus a `Dispatcher.UIThread.RunJobs(...)` flush to avoid an observed deadlock between config-window close and notification processing.

If your Lua-driven “render surface window” needs to update based on core → UI messages (likely), creating it via `DebugWindowManager` keeps it inside the existing notification routing.

### 15.3 How to open the additional window *when* the menu item is used

The lowest-friction place is exactly the `OnClick` delegate in `MainMenuViewModel`.

Conceptually, you’d change the click handler to open two windows back-to-back:
- `DebugWindowManager.OpenDebugWindow(() => new ScriptWindow(...))`
- `DebugWindowManager.OpenDebugWindow(() => new <YourLuaUiSurfaceWindow>(...))`

Notes / choices:
- If you want **one render surface window total** (instead of a new one each click), you can call `GetOrOpenDebugWindow(...)` for the second window.
  - `GetOrOpenDebugWindow` checks window type equality (`w.GetType() == wnd.GetType()`), so it enforces a *single instance per Window class*.
- If you actually want one render surface **per** script window, keeping `OpenDebugWindow` for both matches today’s “multiple ScriptWindows allowed” policy.

### 15.4 Patterns to copy for the new window itself

If you want it to behave like the other tool windows:

- **Derive from `MesenWindow`**
  - example: `UI/Debugger/Windows/ScriptWindow.axaml.cs` and most other tool windows

- **Persist size/position via `BaseWindowConfig`**
  - tool windows typically keep config under `ConfigManager.Config.Debug.<WindowName>`
  - `BaseWindowConfig<T>.LoadWindowSettings(wnd)` + `.SaveWindowSettings(wnd)`

- **(If needed) receive notifications**
  - implement `INotificationHandler` and register the window through `DebugWindowManager` (which happens automatically if you open via `OpenDebugWindow`).

### 15.5 One subtlety: ScriptWindow sometimes bypasses `DebugWindowManager`

Inside `ScriptWindowViewModel`, the “New Script” action creates a new script window directly:
- `new ScriptWindow(new ScriptWindowViewModel(ScriptStartupBehavior.ShowBlankWindow)).Show();`

So today, **not every ScriptWindow instance is guaranteed to be tracked** in `_debugWindowCount` / `_openedWindows`.

Why it matters for your new UI surface:
- if you require strict “workspace stays loaded until every related window closes,” you’ll want to ensure your own window-open path consistently goes through `DebugWindowManager`.
- if you only ever open the render surface from the main menu (and don’t care about ScriptWindow’s internal “New Script” spawning), this may not matter.

---

## 16) Avalonia in Mesen2: what’s used, and concrete quirks/workarounds present in the code

This is a “map of what exists today” so you know what patterns you can reuse when building your own DOM-ish markup renderer.

### 16.1 Package stack (from `UI/UI.csproj`)

Mesen2’s UI is built on Avalonia **11.3.x** with these notable additions:
- `Avalonia` / `Avalonia.Desktop` / `Avalonia.Themes.Fluent`
- `Avalonia.ReactiveUI` + `ReactiveUI.Fody` (MVVM + generated reactive properties)
- `Avalonia.AvaloniaEdit` (text editor: used heavily in debugger/script editor)
- `Dock.Avalonia` + `Dock.Model.Mvvm` (debugger docking host/layout)
- `Avalonia.Controls.ColorPicker`

Also worth noting:
- `AvaloniaUseCompiledBindingsByDefault` is enabled (compiled bindings are the default across the project).

### 16.2 Global style/theme setup

Entry points:
- `UI/App.axaml` sets up the global theme and merges many style dictionaries.
- `UI/Styles/*` contains extensive overrides for Fluent theme defaults.

In particular, Mesen ships/merges dedicated style sets for:
- menus (`AvaloniaMenuItemStyles.xaml` / `AvaloniaContextMenuStyles.xaml`)
- scroll viewer, combobox, numeric up-down
- AvaloniaEdit styling (`AvaloniaEditStyles.xaml`)
- Dock.Avalonia styling (`DockStyles.xaml`)
- plus `MesenStyles.xaml` and `MesenStyles.Light.xaml` (house style)

If you build a “DOM-ish” renderer, these styles will affect your controls automatically (good) — but you’ll also want to be aware that some default Avalonia control styling is deliberately overridden.

### 16.3 Base window class: `MesenWindow` (two important behaviors)

`UI/MesenWindow.cs` is used as the base class for most app/debugger windows.

Two key behaviors:
1) **Popup font rendering mode propagation**
   - When `UseSubpixelFontRendering` changes, it finds the internal `PopupRoot` and applies `TextRenderingMode` so popups/tooltips match the configured font rendering.

2) **DataContext disposal to mitigate leaks**
   - On window close it sets `DataContext = null` with a comment that this is needed “to avoid memory leak.”

If your Lua UI surface is a new window, inheriting from `MesenWindow` keeps you consistent with the project’s leak mitigation + font-rendering behavior.

### 16.4 Window positioning and multi-monitor DPI quirks

Mesen has two layers of “window placement correctness” helpers:

- `UI/Config/BaseWindowConfig.cs`
  - Persists window size/position/maximized state
  - Validates that saved window position is on-screen
  - **Linux/KDE/X11 positioning workaround**:
    - it re-applies position after open, then performs a corrective adjustment
    - explicitly references an Avalonia issue: `github.com/AvaloniaUI/Avalonia/issues/8161`

- `UI/Utilities/WindowExtensions.cs`
  - `ShowCenteredDialog(...)` and `CenterWindow(...)` have special logic to:
    - avoid incorrect centering on multi-monitor + high DPI
    - clamp window bounds so it fits the working area
    - propagate `Topmost` to child windows

If you’re opening a second window next to ScriptWindow, you may want to reuse these helpers (especially if you decide to “snap” the UI surface next to the editor window).

### 16.5 Notifications arrive *off* the UI thread

`UI/Interop/NotificationListener.cs` directly invokes `OnNotification` from the native callback.
It does **not** marshal to the Avalonia UI thread.

Consequences:
- anything that touches Avalonia controls must dispatch via `Dispatcher.UIThread.Post(...)` (pattern used all over `MainWindow.axaml.cs` and debug windows).
- if you build a Lua→UI bridge via notifications, your bridge handler should be careful about thread hops.

### 16.6 A concrete control-level workaround: `MesenNumericTextBox`

`UI/Controls/MesenNumericTextBox.cs` contains a defensive workaround:
- on `ValueProperty` change it posts updates via `Dispatcher.UIThread.Post(...)`
- comment explains it avoids a potential infinite update loop/stack overflow when min/max coercion triggers recursive value updates.

This is a good example of the kind of “real-world Avalonia edge” the codebase already contains.

### 16.7 Debug window manager: “UI correctness” workarounds (locks + RunJobs)

`UI/Debugger/Utilities/DebugWindowManager.cs` is not just a registry — it contains pragmatic correctness fixes:
- it uses locks to coordinate window-open/close with notification processing
- it calls `Dispatcher.UIThread.RunJobs(...)` in `ProcessNotification(...)` to avoid an observed deadlock scenario

If your Lua UI surface window is driven by frequent notifications (layout diffs, event callbacks), it’s worth understanding this file’s threading assumptions.

