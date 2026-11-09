# sega-emu

A SEGA Mega Drive / Genesis emulator written in modern C++ (C++20), built with CMake, using SDL3 for display and input.

## Purpose

The goal is a clean, readable, well-tested Mega Drive emulator, built one subsystem at a time. Priorities, in order:

1. **Correctness you can verify.** Each hardware behaviour gets a unit test, often a small hand-assembled 68000 program.
2. **Clean separation of concerns.** The emulation core is a plain C++ library with no dependency on SDL or any platform API. Frontends (SDL today, maybe a libretro core or a headless test runner later) sit on top of it.
3. **Reasonable accuracy.** Timing is driven by the master clock. Simplifications are allowed, but they are documented in the code where they occur (look for `TODO`).

Mega-CD support has started: the foundation (sub CPU, its memories and the communication registers at `0xA12000`) is in place and tested, but CD games can't run yet. See [Mega-CD (foundation)](#mega-cd-foundation).

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
| Mega-CD | 🟡 Foundation, now running in the scheduler with the sub CPU in lockstep (`--bios`): sub CPU, 512 KB PRG-RAM with write protect, 256 KB Word RAM in 2M and 1M modes (with the 1M cell and dot image views), gate array communication registers, reset/bus request, INT2. CD drive with its command interface (CDD, INT4) and the CD data controller (LC8951, DMA, INT5), reading `.cue`/`.bin`/`.iso` images (or ZIPs of them). Graphics ASIC (stamp rotation/scaling into Word RAM, INT1), timer (INT3) and stopwatch. Sound: the PCM chip (RF5C164) and CD-DA, mixed into the console output. Backup RAM, pre-formatted. **Boots real games with the real BIOS** (tested with the EU BIOS: Road Avenger, Sonic CD) |
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
- The 68000 and the YM2612 run at master clock / 7, and the Z80 and the PSG at master clock / 15. The Mega-CD sub CPU has its own 12.5 MHz clock, converted exactly (see [Mega-CD](#mega-cd-foundation)). One audio sample is produced every 1008 master cycles (144 YM2612 clocks), about 53.3 kHz.
- The VDP tracks its beam position. `Emulator::run_frame()` cuts time into slices. A slice ends at the VDP's next event (HBlank start or end of line), or after one audio sample, whichever comes first. In each slice the Z80 runs first, then the 68000, then the VDP processes its event, which may raise or clear an interrupt before the next slice. CPU targets are absolute cycle counts, so when an instruction runs past a slice boundary the next slice absorbs it, and nothing drifts.
- Before any sound chip access, `Sound` generates the samples up to the accessing CPU's current cycle. A write therefore lands on the right sample, which matters for DAC playback, where the Z80 writes thousands of values per second.

### Byte order

The 68000 is big-endian. ROM, RAM and VRAM are kept in bus order, exactly as in a `.bin` ROM file. 16-bit and 32-bit values are built with shifts, never with `memcpy` or casts, so the code gives the same results on any host byte order.

### Mega-CD (foundation)

The Mega-CD adds a second 68000 (the *sub CPU*, 12.5 MHz) with its own memory. It talks to the Mega Drive through the gate array registers. Code is in [`core/include/md/megacd/`](core/include/md/megacd/):

- **`MegaCd`** plugs into the main CPU's expansion connector (`Memory::attach_expansion`). It supports both layouts: *cartridge boot* (Mega-CD at `0x400000`) and *CD boot* (BIOS at `0x000000`, PRG-RAM window at `0x020000`, Word RAM at `0x200000`). It owns the sub CPU, which runs on its own bus (`SubBus`) through the `Bus68k` interface. That interface is shared with the main CPU's `Memory`, so one `M68k` wrapper serves both processors.
- **`MegaCdRegisters`**: the registers both CPUs see, main at `0xA12000`, sub at `0xFF8000`, each with its own access rights:
  - communication flags (`0x0E`): the high byte belongs to the main CPU, the low byte to the sub CPU;
  - 8 command words (main → sub) and 8 status words (sub → main);
  - sub CPU reset (`SRES`) and bus request (`SBRQ`);
  - the INT2 "doorbell" (`IFL2`), gated by the sub CPU's interrupt mask.
- **`ProgramRam`** (512 KB): the sub CPU's memory. The main CPU sees a 128 KB bank (`BK`), and only while the sub CPU is halted. Sub CPU writes below `WP × 0x200` are ignored.
- **`WordRam`** (256 KB, two 128 KB banks):
  - **2M mode**: all of it belongs to one CPU. The main CPU hands it over with `DMNA`, and the sub CPU returns it with `RET`. Consecutive words alternate between the two banks, as on hardware, so data survives a mode switch.
  - **1M mode**: one bank per CPU, swapped ping-pong style. The sub CPU swaps them by writing `RET`, which clears `DMNA`. The main CPU's `DMNA` writes follow Genesis Plus GX's quirks: writing 0 sets the visible `DMNA` bit, and writing 1 only marks Word RAM to go to the sub CPU at the next switch to 2M. Road Avenger deadlocks without this.
  - **1M alternate views** (layouts as in Genesis Plus GX):
    - **Cell image** (main CPU, base + `0x220000–0x23FFFF`): the main CPU's bank seen as 8×8 tiles, listed column by column over a 256-byte-wide bitmap, in zones 32, 16, 8, 4 and 4 tiles tall. Software draws a picture as a bitmap and the VDP DMAs it as tiles: this is how full-motion video reaches the screen.
    - **Dot image** (sub CPU, `0x080000–0x0BFFFF`): one 4-bit pixel per byte address (even = high nibble, odd = low). Writes follow the priority mode (PM): normal, *underwrite* (only empty pixels change) or *overwrite* (new empty pixels are transparent).
  - **VDP DMA from Word RAM** delivers each word one transfer late. As in Genesis Plus GX, the first destination word is skipped and one word fewer is read; games compensate by starting the source 2 bytes later.

**Scheduling.** `Emulator::attach_mega_cd(bios, cd_boot)` plugs the Mega-CD in; in the frontend that's `--bios file`. While it is attached:

- The two 68000s run in **lockstep slices of at most 32 main-CPU cycles**, main CPU first, then the sub CPU. The BIOS has both CPUs polling each other's flags, so each one's view of the other's writes is at most one slice old.
- The sub CPU's share of each slice comes from `ClockConverter`, an exact integer conversion of master cycles to 12.5 MHz cycles that carries the remainder over. The ratio is **1.6296** sub cycles per main cycle on NTSC (1.6446 on PAL), with no drift over any length of time. Measured over 10 frames: 1.62963.
- **Interrupts** are raised at the moment the gate array register is written. When the main CPU sets `IFL2`, the sub CPU takes INT2 at its next instruction in the same slice, and the main CPU sees the reply at its first poll, about 30 cycles later. The main CPU's interrupt pins combine the VDP (levels 4 and 6) with the expansion port, so a Mega-CD interrupt request and a VDP interrupt never overwrite each other.
- While the sub CPU is held in reset or by a bus request, its timeline keeps moving without it, so it doesn't "catch up" with a burst of cycles when released.

**CD path.** Disc image → drive (CDD) → CDC → sub CPU:

- **`Disc`** ([disc.hpp](core/include/md/megacd/disc.hpp)) reads `.cue` sheets: one or several `FILE`s, `MODE1/2352`, `MODE1/2048`, `MODE2/2352` and `AUDIO` tracks, `INDEX 00/01`, `PREGAP`. It also reads plain `.iso` files and lone raw `.bin` files (detected by their sync pattern). Every sector comes out as raw 2352 bytes; for 2048-byte images and silent pregaps the sync and header (MSF of LBA + 150, in BCD) are rebuilt.
- **`CdDrive`** ([cd_drive.hpp](core/include/md/megacd/cd_drive.hpp)) is the drive and its microcontroller (CDD). It ticks 75 times a second on the sub CPU's clock (exact integer conversion), and keeps spinning while the sub CPU is paused.
  - **Command interface** (`FF8036–FF804B`): the sub CPU sets `HOCK`, and the drive then raises **INT4** every tick. The BIOS answers each one: it reads the 10-nibble status and writes a 10-nibble command, whose last nibble is a checksum (`~sum & 0xF`). The word write to `FF804A` sends it.
  - **Commands:** get status, stop, play and seek to an MSF time, pause, resume, fast forward and rewind, open and close the tray. Report commands give the table of contents: absolute or track-relative time, track number, disc length, first/last track, a track's start time.
  - **Timing:** the drive's behaviour follows Genesis Plus GX's, because games depend on its details. A play or seek reports "seeking" for 12 ticks plus up to 1.5 s depending on distance. Data sectors then reach the CDC at one per tick (two at 2×). The head can sit in the 2-second pregap before sector 0, because the BIOS seeks to 00:01:70 and checks the position it lands on.
- **`Cdc`** ([cdc.hpp](core/include/md/megacd/cdc.hpp)), the Sanyo LC8951, reached through the gate array's address/data register pair (`FF8004`/`FF8006`, with auto-increment):
  - **Buffer:** a 16 KB sector buffer (WA write address, PT header pointer) and the full register file (IFCTRL/IFSTAT, DBC, DAC, HEAD0–3, STAT0–3, CTRL0/1, RESET).
  - **Transfers:** to the sub or main CPU through the host data register (`FF8008` / `A12008`), or by DMA into PRG-RAM (which respects write protection) or Word RAM (whichever part the sub CPU owns).
  - **Interrupts:** "sector decoded" (DECI, cleared by reading STAT3) and "transfer ended" (DTEI, cleared by DTACK) raise **INT5** on the sub CPU.
- In the frontend: `--bios bios.bin --disc game.cue`. Both may be ZIP archives. A zipped disc is unpacked once, streamed with its CRC checked, into `<system temp>/sega-emu-discs/<archive name>/`, and reused on later runs. The `.cue` inside is preferred, then an `.iso`, then the largest `.bin`/`.img`. Entry names that would escape that folder ("zip slip") are refused.
- **Changing discs** (multi-disc games): drop the next disc on the window. The frontend loads it on a worker thread, so unzipping doesn't freeze the game, then swaps it the way a player would.
  - **Sequence:** the tray opens (the drive reports "tray open" and has no disc), the disc is replaced, and the tray closes one second later. The BIOS or game then sees a new disc and reads its table of contents.
  - **Game-driven tray:** the CDD's open/close tray commands work too. Dropping a file without a Mega-CD attached, or while a disc is still loading, prints a message and is ignored.
- With the real European BIOS, **Road Avenger (EU) and Sonic CD (Europe) boot and play with sound**:
  - **Road Avenger:** BIOS intro, disc check, the Sega licence screen, then the game's full-motion video with its PCM soundtrack.
  - **Sonic CD:** the opening animation with its music (drawn through the cell image view), then the title screen.

**Sound** (mixed into the Mega Drive's output by `Sound::set_expansion_audio`, which asks the Mega-CD for its output at each sample's exact time):

- **PCM** ([pcm.hpp](core/include/md/megacd/pcm.hpp)), the Ricoh RF5C164. It has 8 channels playing 8-bit sign-magnitude samples from 64 KB of wave RAM, at 12.5 MHz / 384 = **32,552 Hz**.
  - **Per channel:** volume, left/right pan, a 5.11 fixed-point step, a start address, and a loop address that a `0xFF` byte jumps to.
  - **Sub-CPU access:** registers at `FF0001–FF0011`, live play addresses at `FF0021–FF003F` (games poll them to stream audio), and a 4 KB banked window onto wave RAM at `FF2001–FF3FFF`. The CDC can also DMA sectors straight into wave RAM.
  - **Timing:** the chip is brought up to date before every access, so the play addresses are exact, and its output is interpolated to the mixer's rate.
- **CD-DA:** while the drive plays an audio track, each tick's 588 stereo samples go into a FIFO, and the mixer drains it at exactly 44.1 kHz of sub-CPU time. Production and consumption run off the same clock, so the FIFO stays between 0.5 and 2 sectors.
  - **Fader:** `FF8034` (0x400 = full volume) ramps one step per sample, and bit 15 reads 1 while it is still ramping. The DM bit of `FF8036` says whether CD-DA is playing.
- **Sample timing:** a register write from the sub CPU runs the chip up to that moment, but the mixer reads the output later (at the end of the frame, unless the main side touches the FM chip first). So the chip keeps the samples it makes until they are read. Without that, the samples between a write and the read were lost and replaced by one held value. That was the harsh sound in the BIOS intro, whose main CPU never touches the FM chip.
- **Output filter:** on the console, an analog low-pass filter on the PCM output softens it (the chip holds each byte until its address moves on, so slowly played samples come out as a staircase). Its cutoff isn't documented, and Genesis Plus GX and BlastEm don't model it. Here it is a 2-pole Butterworth low-pass at **7 kHz** by default; `--pcm-lowpass <Hz>` changes it, and `0` turns it off. CD-DA isn't filtered.
- **Levels:** PCM and CD-DA are both mixed at half scale against FM. This is a judgement call, not a measured value.

**Backup RAM.** The 8 KB internal save RAM (sub CPU `FE0001–FE3FFF`, odd bytes) starts out formatted (an empty directory, as written by the BIOS's format option), and it survives resets. Games such as Sonic CD refuse to start with unformatted RAM. It is not yet saved to disk between runs.

**Timer and stopwatch.** The timer (`FF8030`) raises INT3 every (n + 1) × 384 sub-CPU cycles (30.72 µs units). The 12-bit stopwatch (`FF800C`, also `A1200C`) counts in the same units, and any write clears it.

Simplifications: DMA completes instantly; EDC/ECC aren't checked (status always says CRC OK).

**Graphics ASIC** ([graphics_asic.hpp](core/include/md/megacd/graphics_asic.hpp)): the Mega-CD's hardware rotation and scaling.

- **Source:** a *stamp map* in Word RAM, 256×256 or 4096×4096 dots. It is built from 16×16 or 32×32 *stamps*: 4-bpp bitmaps made of 8×8 cells, stored column by column. Each map entry can flip its stamp and rotate it in 90° steps. Stamp 0 is transparent. Outside the map, sampling either wraps (repeat) or returns transparent.
- **Transform:** for each output line, a *trace vector* gives the source start point (13.3 fixed point) and the source step per output dot (signed 5.11). The ASIC walks that line and takes the nearest source dot. Rotation, scaling and shearing, or a different transform per line for perspective effects, are all just the vectors the software computes.
- **Destination:** the *image buffer* in Word RAM, written as 8×8 cells column by column (the VDP's tile format), with a 0–7 dot offset. Writes follow the priority mode: plain copy, *overwrite* (transparent source dots keep the destination) or *underwrite* (only empty destination dots are written).
- **Control:** registers `FF8058–FF8066`. Writing the trace-vector address starts the operation. The GRON flag stays set while it runs, and INT1 is raised at the end. The ASIC works only in 2M mode, with Word RAM owned by the sub CPU.
- **Simplifications:** the image is drawn at once when the operation starts. The busy period is an estimate (4 sub-CPU cycles per output dot) because I don't have a hardware measurement. The *direction* of the 90° and 270° map-entry rotations follows the manual's "counterclockwise" wording and is isolated in `GraphicsAsic::stamp_transform` in case games show otherwise. The flip and 180° are unambiguous.

Still to come:
- saving the backup RAM to a file;
- subcode reading (INT6).

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
│   │   ├── bus.hpp                #     Bus68k / BusDevice interfaces
│   │   └── megacd/                #     Mega-CD: registers, PRG-RAM, Word RAM, sub CPU
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
./build/frontend/megadrive --bios megacd_bios.zip --disc game.zip   # Mega-CD (see its status below)
./build/frontend/megadrive --bios megacd_bios.zip --disc game.zip --pcm-lowpass 5000   # softer Mega-CD PCM (0 = unfiltered)
./build/frontend/megadrive --region pal path/to/game.bin   # force the console region
```

### Headless mode (screenshots and audio recording)

Runs a ROM for a number of frames without opening a window. It can save the last frame as a PPM image and/or all the audio as a WAV file. This is useful for debugging, or for attaching to a bug report:

```sh
./build/frontend/megadrive --frames 300 --screenshot frame.ppm --wav audio.wav path/to/game.bin
sips -s format png frame.ppm --out frame.png      # macOS; or: magick frame.ppm frame.png
```

### Sound

Audio goes to the default output device through an SDL3 audio stream, which resamples from ~53.3 kHz to the device rate. Picture and sound are paced by different clocks, so the frontend nudges the playback speed (by at most ±0.5%, which is inaudible) to keep about 60 ms of audio queued. Press **M** to mute.

### Console region

The emulated console's region matters: games and the Mega-CD BIOS check it and may refuse to run on the wrong one. By default it is read from the header of what boots: the cartridge, or the Mega-CD BIOS when booting a CD. The region field is at `0x1F0` and may use either the old letter codes (`J`, `U`, `E`) or the newer hex digit.

- **Europe:** PAL, 50 Hz.
- **USA:** NTSC, 60 Hz, overseas.
- **Japan:** NTSC, domestic.
- **Several regions:** USA is preferred.

The terminal shows the choice and where it came from. Override it with `--region pal|ntsc|jp`.

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
| Change the Mega-CD disc (multi-disc games) | Drag and drop the next disc (`.cue`, `.iso`, `.bin` or `.zip`) onto the window |

## Testing

```sh
ctest --test-dir build --output-on-failure
# or, for a per-test listing:
./build/tests/core_tests
./build/tests/frontend_tests
```

There are 184 tests (178 core, 6 frontend), using a small built-in framework ([`tests/test.hpp`](tests/test.hpp), no external dependency):

- [`memory_tests.cpp`](tests/memory_tests.cpp): byte order, mirroring, 24-bit address wrap, Z80 bus arbitration, I/O registers, VDP/PSG port decoding.
- [`cpu_tests.cpp`](tests/cpu_tests.cpp): hand-assembled 68000 programs run through Moira → bridge → Memory. Covers reset vectors, interrupts with acknowledge, address errors (which also proves the Moira config is in place), and disassembly.
- [`vdp_tests.cpp`](tests/vdp_tests.cpp): command encoding, VRAM/CRAM/VSRAM access, status flags, VINT/HINT timing and priority, HV counter. Also a full-system test in which a 68000 program counts VINT and HINT interrupts over several frames.
- [`vdp_dma_tests.cpp`](tests/vdp_dma_tests.cpp): 68000 transfers to every target, source wrap, 64K length, fill, copy, the DMA-enable bit, and the one-word delay of transfers from the Mega-CD's Word RAM.
- [`vdp_render_tests.cpp`](tests/vdp_render_tests.cpp): tile decoding, flips, palettes, scrolling modes, plane priority, transparency, window, display off. Sprites: multi-cell layout, flips, link order, collision, priority against planes, per-line sprite and pixel limits, X=0 masking. Also a full-system test in which a 68000 program uploads graphics by DMA and the frame is checked pixel by pixel.
- [`sound_tests.cpp`](tests/sound_tests.cpp): PSG tone frequency, silence and noise. YM2612 440 Hz FM tone and key-off release, timers A and B (including timer B's free-running prescaler), DAC. Z80 running from RAM, the bank window, PSG writes, the VBlank interrupt, pausing while the 68000 holds the bus. Also a full-system test in which a Z80 program plays a DAC square wave into the frame's audio.
- [`input_tests.cpp`](tests/input_tests.cpp): 3-button protocol, the full 6-button sequence and its timeout, TH as input, nothing connected, routing from `Emulator::set_controller`. Also a full-system test in which a 68000 program reads the pad.
- [`rom_loader_tests.cpp`](tests/rom_loader_tests.cpp): ZIP archives built in memory by the test. Covers stored and deflate entries, which entry is chosen, CRC checking, and corrupt, encrypted, truncated and empty archives; also files on disk, raw and zipped, plus bad names and sizes.
- [`frontend_input_tests.cpp`](tests/frontend_input_tests.cpp) (in `frontend_tests`): the gamepad-to-Mega-Drive mapping, stick dead zone, opposite directions cancelling, and merging keyboard and gamepad input. Also the real SDL path, using SDL3 **virtual gamepads**: hot-plugging, player assignment, buttons and stick, disconnecting and reconnecting. Real devices are hidden during the tests, so they don't depend on what's plugged in.
- [`megacd_tests.cpp`](tests/megacd_tests.cpp): who may read or write each communication register, reset and bus request, the PRG-RAM window rules and write protect, Word RAM 2M handover, bank interleave and mode switches, the 1M ping-pong swap, and INT2. The 1M views: the cell image layout, checked at every byte of the bank against an independent description (and shown to be a one-to-one mapping), cell view reads and writes, and dot image pixels with all four priority modes. Also two end-to-end tests: the sub CPU booting from PRG-RAM and taking INT2, and the main and sub 68000s exchanging a command and a reply.
- [`megacd_scheduler_tests.cpp`](tests/megacd_scheduler_tests.cpp): the exact clock conversion (one second gives 12,500,000 sub cycles); the main/sub ratio measured inside the emulator; a BIOS-style flag handshake (each CPU waits for the other, about 1,300 round trips per frame); the latency of INT2 from main to sub; and an expansion interrupt reaching the main CPU through the shared IPL pins. Set `MD_TEST_VERBOSE=1` to print the measured values.
- [`megacd_cd_tests.cpp`](tests/megacd_cd_tests.cpp):
  - **Cue parsing:** single-file and multi-file layouts, `INDEX 00`, `PREGAP` and errors.
  - **Sector reading:** from real temporary `.iso`, raw `.bin` and `.cue` files.
  - **CDC registers:** auto-increment, and byte reads without side effects.
  - **Transfers and interrupts:** decoding into the buffer, INT5 and its acknowledgement, the 16 KB wrap, host reads by both CPUs, DMA to PRG-RAM and Word RAM, the DOUTEN gate.
  - **Streaming:** exactly 75 and 150 sectors per second, and stopping at the end of the disc.
  - **Tray:** a disc swap (tray open with no disc and nothing to play, then the new disc ready with its own table of contents one second later), and the open/close tray commands.
  - **CDD:** status and table-of-contents reports with valid checksums, no disc, INT4 every tick only while HOCK is set, play with seek latency then sector delivery (and pause/resume), seeking into the pregap before sector 0.
  - **Timer and stopwatch:** periodic INT3 and stopping it, stopwatch counting, clearing and wrapping.
  - **End to end:** a sub-CPU program waits for a streamed sector and reports its header.
  - **Zipped discs:** unpacking, choosing the `.cue`, reusing the cache, and rejecting zip-slip names, archives without a disc, and corrupt data (CRC).
- [`megacd_sound_tests.cpp`](tests/megacd_sound_tests.cpp): PCM wave RAM banks and mirroring; a channel playing, looping on a `0xFF` marker and reporting its play address; the exact 32,552 Hz rate; panning; output off; the output filter (a 16 kHz tone cut, a 1 kHz tone kept, exact output when off). CD-DA from a real temporary audio-only `.cue`/`.bin`: the DM bit, the level, the fader ramp, silence once paused, and a 44.1 kHz rate that keeps the FIFO bounded. Also a full-system test: a PCM square wave through the emulator's mixer, checked for loudness and frequency (325 Hz). The CDC-to-PCM DMA test is in `megacd_cd_tests.cpp`.
- [`megacd_graphics_tests.cpp`](tests/megacd_graphics_tests.cpp): the graphics ASIC. It checks:
  - the register masks;
  - the stamp and image-buffer cell layouts, against a reference written independently of the implementation;
  - an identity copy, 2× zoom and fractional steps;
  - 90° rotation by vectors, and a 30° rotation checked dot by dot against an independent computation;
  - flips, 180° rotation, and the algebra of the 90° steps (which holds whatever the direction);
  - transparency and repeat, 32×32 stamps, the dot offset and the three priority modes;
  - the busy flag and INT1 timing, and the 2M-only rule.
- [`emulator_tests.cpp`](tests/emulator_tests.cpp): refresh rates, frame stepping, region setup.

## Roadmap

Roughly in order:

1. **VDP completeness**: shadow/highlight, interlace, the sprite attribute cache, the write FIFO, and 68000 stalls during DMA.
2. **Sound polish**: a low-pass filter like the console's output stage, and 68000 stalls while the Z80 reads the 68000 bus.
3. **Cartridge features**: save RAM, bank switching for large ROMs, TMSS.
4. **Accuracy**: sub-slice VDP sync on port access, the TAS write-back quirk, Moira precise-timing mode.
5. **Mega-CD**: saving the backup RAM, subcode (INT6), and testing more games.

Known simplifications are marked with `TODO` comments at the place in the code where they apply.
