# sega-emu

A SEGA Mega Drive / Genesis emulator written in modern C++ (C++20), built with CMake, using SDL3 for display and input.

## Purpose

The goal is a clean, readable, well-tested Mega Drive emulator, built one subsystem at a time. Priorities, in order:

1. **Correctness you can verify.** Each hardware behaviour gets a unit test, often a small hand-assembled 68000 program.
2. **Clean separation of concerns.** The emulation core is a plain C++ library with no dependency on SDL or any platform API. Frontends (SDL today, maybe a libretro core or a headless test runner later) sit on top of it.
3. **Reasonable accuracy.** Timing is driven by the master clock. Simplifications are allowed, but they are documented in the code where they occur (look for `TODO`).

The longer-term roadmap includes Mega-CD support (a second 68000, the graphics ASIC, and the communication registers at `0xA12000`). That work starts only once the base console runs simple games.

## Current status

**Early development.** A ROM boots, the 68000 and Z80 run it with interrupts, DMA uploads graphics, the VDP draws planes, window and sprites, the YM2612 and PSG produce sound, and the keyboard drives a 6-button pad. Simple games should be playable. Cartridge save RAM and large-ROM bank switching are still missing.

| Subsystem | Status |
|---|---|
| SDL3 frontend | ✅ Window, ~60 Hz frame pacing, keyboard → controller mapping, ROM loading from the command line |
| Memory map (68000 bus) | ✅ ROM, 64 KB RAM (mirrored), Z80 area + bus request/reset, I/O registers, VDP/PSG ports, open bus |
| 68000 CPU | ✅ [Moira](https://github.com/dirkwhoffmann/Moira) core, with address errors, autovectored interrupts and interrupt acknowledge |
| VDP: registers, ports, memories | ✅ 24 registers, 64 KB VRAM, CRAM, VSRAM, two-word commands, auto-increment, status register, HV counter |
| VDP: interrupts and timing | ✅ VINT (level 6) and HINT (level 4) from a cycle-based line counter, NTSC and PAL |
| VDP: DMA | ✅ 68000 → VRAM/CRAM/VSRAM (128 KB source wrap), VRAM fill, VRAM copy. Instant: the 68000 is not stalled |
| VDP: rendering | 🟡 Planes A/B with full/per-cell/per-line horizontal scroll and full/per-2-cell vertical scroll, window, tile flips, palettes, priority, left-column blank. Sprites: link list, 1–4×1–4 cells, flips, per-line sprite and pixel limits, X=0 masking, overflow/collision flags. No shadow/highlight, no interlace, no sprite attribute cache |
| Controllers | ✅ 3-button and 6-button pads on ports A and B (TH-line protocol, 6-button phase counter with ~1.5 ms timeout). 6-button by default; `--3button` for the few old games that misread 6-button pads. Keyboard for player 1, plus physical gamepads (SDL3) for players 1 and 2, with hot-plugging |
| Z80 CPU | ✅ [superzazu/z80](https://github.com/superzazu/z80) core. Full Z80 memory map: RAM, YM2612, bank register and 68000 window, VDP/PSG. VBlank interrupt; paused while the 68000 holds its bus; reset line |
| Sound | ✅ YM2612 via [ymfm](https://github.com/aaronsgiles/ymfm), including the DAC, timers and the model 1 "ladder effect". SN76489 PSG (own implementation). The chips are brought up to the current emulated time before every access, so DAC samples play at the right timing. DC-blocking output filter. Stereo, ~53.3 kHz |
| Cartridge extras | ❌ No battery-backed save RAM, no bank switching for ROMs over 4 MB, no TMSS |

When no ROM is given, the frontend shows scrolling colour bars (inverted while Start is held) so you can check video, timing and input.

## Architecture

```
┌─────────────────────────── frontend/ (megadrive executable) ───────────────────────────┐
│  SDL3: window, renderer, audio stream, keyboard → md::ControllerState, frame pacing     │
└──────────────┬─────────────────────────────────────────────────────────▲───────────────┘
               │ set_controller(), run_frame(), load_rom()               │ framebuffer(),
               │                                                         │ audio_samples()
┌──────────────▼────────────────────────── core/ (md::core) ─────────────┴───────────────┐
│  md::Emulator — owns every component and runs the scheduler                            │
│                                                                                        │
│  ┌──────────┐  bus hooks   ┌─────────────┐  WordDevice   ┌──────────┐                  │
│  │ md::M68k │─────────────▶│ md::Memory  │──────────────▶│ md::Vdp  │──▶ Framebuffer    │
│  │ (Moira)  │              │ 68000 view  │               └──┬────┬──┘                  │
│  └────▲─────┘              │ + Z80 view  │                  │    │                     │
│       │ IRQ 6/4, ack       └──▲───────┬──┘                  │    │ /INT at VBlank      │
│       └───────────────────────┼───────┼─────────────────────┘    │                     │
│  ┌──────────┐  z80_read/write │       │ ByteDevice               │                     │
│  │ md::Z80  │─────────────────┘       ▼                          │                     │
│  │(superzazu)◀────────────────────────┼──────────────────────────┘                     │
│  └──────────┘              ┌──────────┴───────────────┐                                │
│                            │ md::Sound                │                                │
│                            │ YM2612 (ymfm) + PSG      │──▶ stereo s16 samples          │
│                            └──────────────────────────┘                                │
└────────────────────────────────────────────────────────────────────────────────────────┘
```

### Core rules

- **No platform code in `core/`.** The core exposes plain types: `Framebuffer` (ARGB8888 pixels), `ControllerState` (a button bitmask) and interleaved stereo `s16` audio samples. `tests/` links only against `md::core`, so the build fails if SDL ever leaks into the core.
- **Components don't know about each other.** `Memory` talks to chips through two small interfaces: `WordDevice` for the VDP and `ByteDevice` for the sound chips. The VDP reports its IRQ level through a callback, and the CPU reports interrupt acknowledge through another. `Emulator` connects them.
- **Third-party cores are implementation details.** Only [`core/src/cpu/moira_bridge.hpp`](core/src/cpu/moira_bridge.hpp) includes Moira, only [`z80.cpp`](core/src/cpu/z80.cpp) includes the Z80 core, and only [`ym2612.cpp`](core/src/audio/ym2612.cpp) includes ymfm. The rest of the code uses `md::M68k`, `md::Z80` and `md::Ym2612`, each of which hides its library behind a pimpl.

### Timing and scheduling

Everything is measured against the master clock (53.69 MHz NTSC, 53.20 MHz PAL):

- One scanline is 3420 master cycles. A frame is 262 lines (NTSC) or 313 lines (PAL), about 59.92 Hz or 49.70 Hz.
- The 68000 and the YM2612 run at master clock / 7, and the Z80 and the PSG at master clock / 15. One audio sample is produced every 1008 master cycles (144 YM2612 clocks), about 53.3 kHz.
- The VDP tracks its beam position. `Emulator::run_frame()` cuts time into slices. A slice ends at the VDP's next event (HBlank start or end of line), or after one audio sample, whichever comes first. In each slice the Z80 runs first, then the 68000, then the VDP processes its event, which may raise or clear an interrupt before the next slice. CPU targets are absolute cycle counts, so when an instruction runs past a slice boundary the next slice absorbs it, and nothing drifts.
- Before any sound chip access, `Sound` generates the samples up to the accessing CPU's current cycle. A write therefore lands on the right sample, which matters for DAC playback, where the Z80 writes thousands of values per second.

### Byte order

The 68000 is big-endian. ROM, RAM and VRAM are kept in bus order, exactly as in a `.bin` ROM file. 16-bit and 32-bit values are built with shifts, never with `memcpy` or casts, so the code gives the same results on any host byte order.

## Project layout

```
sega-emu/
├── CMakeLists.txt                 # top level: options, C++20, warning flags
├── core/                          # md::core — static library, platform-independent
│   ├── CMakeLists.txt             #   also fetches Moira, ymfm and the Z80 core
│   ├── include/md/                #   public API
│   │   ├── emulator.hpp           #     the main Emulator class + scheduler
│   │   ├── memory.hpp             #     68000 memory map, WordDevice/ByteDevice interfaces
│   │   ├── m68k.hpp               #     CPU wrapper (hides Moira)
│   │   ├── vdp.hpp                #     Video Display Processor
│   │   ├── z80.hpp                #     Z80 wrapper (hides the Z80 core)
│   │   ├── sound.hpp              #     sound chips + mixer, timed by the scheduler
│   │   ├── ym2612.hpp, psg.hpp    #     FM chip (hides ymfm), SN76489 PSG
│   │   ├── framebuffer.hpp        #     ARGB8888 output buffer (max 320×240)
│   │   ├── input.hpp              #     6-button pad state
│   │   └── types.hpp              #     u8/u16/u32…, Region
│   ├── src/
│   │   ├── emulator.cpp
│   │   ├── memory.cpp
│   │   ├── cpu/m68k.cpp, cpu/moira_bridge.hpp, cpu/z80.cpp
│   │   ├── audio/sound.cpp, audio/ym2612.cpp, audio/psg.cpp
│   │   └── video/vdp.cpp
│   └── third_party/moira/MoiraConfig.h   # our Moira build settings (see below)
├── frontend/                      # megadrive — SDL3 executable (the only code that uses SDL)
│   └── src/main.cpp, input.cpp (keyboard + gamepads), pad_mapping.hpp
└── tests/                         # core_tests (no SDL) + frontend_tests (SDL input)
```

## Building

### Requirements

- CMake **3.25** or newer
- A C++20 compiler (developed with Apple Clang 21; GCC 12+ / MSVC 2022 should work but are untested)
- git and network access on the first configure (CMake downloads Moira, and SDL3 if it is not installed)
- Optional: SDL3 installed system-wide. Otherwise it is built from source.

On macOS: `brew install cmake sdl3`. On Debian/Ubuntu: `sudo apt install cmake libsdl3-dev`, if your release packages SDL3; otherwise let CMake build it.

### Compile

```sh
cmake -S . -B build                    # Debug by default
cmake --build build -j
```

Release build:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
```

### CMake options

| Option | Default | Effect |
|---|---|---|
| `MD_BUILD_FRONTEND` | `ON` | Build the SDL3 executable. Turn it off to build only the core and tests, with no SDL needed. |
| `MD_BUILD_TESTS` | `ON` | Build `core_tests` (and `frontend_tests` when the frontend is built), and register them with CTest |
| `MD_FETCH_SDL` | `OFF` | Always build SDL3 from source (pinned `release-3.2.10`) instead of using an installed copy |
| `MD_WARNINGS_AS_ERRORS` | `OFF` | Add `-Werror` / `/WX`. The code currently builds cleanly with it. |

### Dependencies

- **ymfm** (BSD-3-Clause, by Aaron Giles), for the YM2612. It is pinned to a commit in [`core/CMakeLists.txt`](core/CMakeLists.txt). Upstream has no CMake build, so we compile the three source files the OPN chips need.
- **superzazu/z80** (MIT license), for the Z80. Pinned the same way and compiled as a small C library.
- **zlib** (system library, optional), to decompress ZIP archives.
- **SDL3** (zlib license). [`frontend/CMakeLists.txt`](frontend/CMakeLists.txt) uses an installed SDL3 if it can find one, and otherwise fetches and builds a static copy.
- **Moira** (MIT license). It is pinned to a specific commit in [`core/CMakeLists.txt`](core/CMakeLists.txt). Moira reads its options from a `MoiraConfig.h` inside its own source folder, so CMake copies [our version](core/third_party/moira/MoiraConfig.h) over it at configure time. Our settings differ from upstream (marked `[sega-emu]` in that file):
  - address errors are on;
  - Musashi-compatibility mode is off;
  - function-code pins and the 68020 instruction cache are off.

## Running

```sh
./build/frontend/megadrive                 # no ROM: colour-bar test pattern
./build/frontend/megadrive path/to/game.bin
./build/frontend/megadrive path/to/game.zip              # ROM inside a ZIP archive
./build/frontend/megadrive --fullscreen path/to/game.bin
./build/frontend/megadrive --3button path/to/game.bin   # 3-button pad instead of 6-button
```

### Headless mode (screenshots and audio recording)

Runs a ROM for a number of frames without opening a window. It can save the last frame as a PPM image and/or all the audio as a WAV file. This is useful for debugging, or for attaching to a bug report:

```sh
./build/frontend/megadrive --frames 300 --screenshot frame.ppm --wav audio.wav path/to/game.bin
sips -s format png frame.ppm --out frame.png      # macOS; or: magick frame.ppm frame.png
```

### Sound

Audio goes to the default output device through an SDL3 audio stream, which resamples from ~53.3 kHz to the device rate. Picture and sound are paced by different clocks, so the frontend nudges the playback speed (by at most ±0.5%, which is inaudible) to keep about 60 ms of audio queued. Press **M** to mute.

### ROM formats

- **Raw images**: `.bin`, `.md`, `.gen`, unheadered. The interleaved `.smd` format is not supported.
- **ZIP archives**, detected by their content rather than the file name. The emulator loads the first entry with a ROM extension (`.bin`, `.md`, `.gen`, `.smd`), or else the largest file in the archive. Entries may be stored or deflate-compressed, which covers what every common ZIP tool produces, and their CRC-32 is checked. Encrypted and ZIP64 archives are refused with an error message. Decompression uses the system zlib; if CMake can't find zlib, only uncompressed entries can be loaded.

Files smaller than 512 bytes (the vector table plus the cartridge header) are rejected.

```
Loaded game.bin (524288 bytes): SSP=FFFE00 PC=000200  bra     $200
```

The window opens at 3× (960×672). Press **F2** to cycle the scaling mode, which is shown in the title bar:

| Mode | Behaviour |
|---|---|
| **Fit** (default) | As large as possible while keeping the game's aspect ratio. In a window, resizing keeps the window in that shape, so the image always fills it. In fullscreen there are bars at the sides. |
| **Stretch** | Fills the window or screen completely, distorting the image |
| **Integer** | Whole-number scale only. Sharpest, but may leave borders |

With SDL 3.4 or newer, non-integer scales use SDL's pixel-art filter, which keeps pixels sharp without uneven rows.

### Controls

Player 1 uses the keyboard and/or the first gamepad connected. Player 2 uses the second gamepad. Gamepads can be plugged in or unplugged while a game runs; the terminal shows which player each one is assigned to.

| Mega Drive | Keyboard | Gamepad (by position) | Xbox | PlayStation | Switch |
|---|---|---|---|---|---|
| D-pad | Arrow keys | D-pad or left stick | | | |
| A | A | Left face button | X | □ | Y |
| B | S | Bottom face button | A | ✕ | B |
| C | D | Right face button | B | ○ | A |
| X | Q | Left shoulder | LB | L1 | L |
| Y | W | Top face button | Y | △ | X |
| Z | E | Right shoulder | RB | R1 | R |
| Start | Enter | Start | Menu | Options | + |
| Mode | Right Shift | Back | View | Share | − |

Gamepad buttons are mapped by **position**, the same layout RetroArch uses for the Mega Drive: A, B and C follow the arc of the face buttons, whatever their labels. SDL3 recognises most USB and Bluetooth pads. For an unusual one, put a [`gamecontrollerdb.txt`](https://github.com/mdqinc/SDL_GameControllerDB) in the directory you run the emulator from.

| Emulator | Keyboard |
|---|---|
| Toggle fullscreen | F11, Alt/Option+Enter, or Cmd+F |
| Cycle scaling (Fit / Stretch / Integer) | F2 |
| Mute / unmute sound | M |
| Leave fullscreen / quit | Esc (leaves fullscreen first, quits from a window) |

## Testing

```sh
ctest --test-dir build --output-on-failure
# or, for a per-test listing:
./build/tests/core_tests
./build/tests/frontend_tests
```

There are 105 tests (99 core, 6 frontend), using a small built-in framework ([`tests/test.hpp`](tests/test.hpp), no external dependency):

- [`memory_tests.cpp`](tests/memory_tests.cpp): byte order, mirroring, 24-bit address wrap, Z80 bus arbitration, I/O registers, VDP/PSG port decoding.
- [`cpu_tests.cpp`](tests/cpu_tests.cpp): hand-assembled 68000 programs run through Moira → bridge → Memory. Covers reset vectors, interrupts with acknowledge, address errors (which also proves the Moira config is in place), and disassembly.
- [`vdp_tests.cpp`](tests/vdp_tests.cpp): command encoding, VRAM/CRAM/VSRAM access, status flags, VINT/HINT timing and priority, HV counter. Also a full-system test in which a 68000 program counts VINT and HINT interrupts over several frames.
- [`vdp_dma_tests.cpp`](tests/vdp_dma_tests.cpp): 68000 transfers to every target, source wrap, 64K length, fill, copy, and the DMA-enable bit.
- [`vdp_render_tests.cpp`](tests/vdp_render_tests.cpp): tile decoding, flips, palettes, scrolling modes, plane priority, transparency, window, display off. Sprites: multi-cell layout, flips, link order, collision, priority against planes, per-line sprite and pixel limits, X=0 masking. Also a full-system test in which a 68000 program uploads graphics by DMA and the frame is checked pixel by pixel.
- [`sound_tests.cpp`](tests/sound_tests.cpp): PSG tone frequency, silence and noise. YM2612 440 Hz FM tone and key-off release, timers A and B (including timer B's free-running prescaler), DAC. Z80 running from RAM, the bank window, PSG writes, the VBlank interrupt, pausing while the 68000 holds the bus. Also a full-system test in which a Z80 program plays a DAC square wave into the frame's audio.
- [`input_tests.cpp`](tests/input_tests.cpp): 3-button protocol, the full 6-button sequence and its timeout, TH as input, nothing connected, routing from `Emulator::set_controller`. Also a full-system test in which a 68000 program reads the pad.
- [`rom_loader_tests.cpp`](tests/rom_loader_tests.cpp): ZIP archives built in memory by the test. Covers stored and deflate entries, which entry is chosen, CRC checking, and corrupt, encrypted, truncated and empty archives; also files on disk, raw and zipped, plus bad names and sizes.
- [`frontend_input_tests.cpp`](tests/frontend_input_tests.cpp) (in `frontend_tests`): the gamepad-to-Mega-Drive mapping, stick dead zone, opposite directions cancelling, and merging keyboard and gamepad input. Also the real SDL path, using SDL3 **virtual gamepads**: hot-plugging, player assignment, buttons and stick, disconnecting and reconnecting. Real devices are hidden during the tests, so they don't depend on what's plugged in.
- [`emulator_tests.cpp`](tests/emulator_tests.cpp): refresh rates, frame stepping, region setup.

## Roadmap

Roughly in order:

1. **VDP completeness**: shadow/highlight, interlace, the sprite attribute cache, the write FIFO, and 68000 stalls during DMA.
2. **Sound polish**: a low-pass filter like the console's output stage, and 68000 stalls while the Z80 reads the 68000 bus.
3. **Cartridge features**: save RAM, bank switching for large ROMs, TMSS.
4. **Accuracy**: sub-slice VDP sync on port access, the TAS write-back quirk, Moira precise-timing mode.
5. **Mega-CD**: sub-CPU, graphics ASIC, PCM, CD-ROM.

Known simplifications are marked with `TODO` comments at the place in the code where they apply.
