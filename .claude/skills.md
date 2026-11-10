# SEGA Mega Drive & Mega-CD Emulator Development Skills

## Core Expertise
- **Languages & Tools:** Modern C++ (C++20/C++23), CMake (3.20+), SDL2 or SDL3 for input, audio, and video rendering.
- **Architecture Emulation:** 
  - Motorola 68000 (Main CPU & Mega-CD Co-CPU) cycle-accurate or instruction-accurate emulation.
  - Zilog Z80 (Sound CPU) and its banking mechanism.
  - VDP (Video Display Processor): DMA transfers, CRAM/VSRAM/VRAM, layers (Scroll A, Scroll B, Window, Sprites).
  - Sound Chips: Yamaha YM2612 (FM synthesis) and Texas Instruments SN76489 (PSG).
  - Mega-CD Hardware: ASIC graphics (rotation/scaling), CD-ROM drive emulation (ISO/BIN/CUE), PCM sound chip.

## Development Principles
- **Performance First:** Avoid heavy abstractions in the main execution loops (CPU fetch-decode-execute, VDP scanline rendering). Use data-oriented design where applicable.
- **Strict Separation:** Keep Core Emulation logic completely independent of the Frontend (SDL/UI).
- **Endianness:** Handle big-endian (68k) to little-endian (host x86_64/ARM64) memory mapping correctly.
- **Modern C++:** Use `std::uint8_t`, `std::uint16_t`, `std::uint32_t` for exact-width types. Prefer `std::span` and `std::array` over raw pointers.

- **Mega-CD / Sega CD Hardware Emulation:**
  - Sub-CPU: Second Motorola 68000 running at 12.5 MHz.
  - Memory Gateways: Program RAM (512KB), Word RAM (256KB total, dual-mapped or single-mapped modes 0 and 2), PCM RAM (64KB).
  - CDC (Sega CD Data Controller) & CD-ROM Drive: Emulation of ISO/BIN/CUE parsing, CD-DA audio streaming, and subcode reading.
  - Graphics ASIC: Hardware-accelerated cell-to-bitmap rendering, rotation, scaling, and texture mapping.
  - Hardware Synchronization: Sub-CPU/Main-CPU communication registers, timers, and specialized interrupts (Level 2 for Sub-CPU communication, Level 1 for Graphics ASIC, etc.).

- **Sega 32X Hardware Emulation:**
  - Dual SH-2 CPUs: Master and Slave Hitachi SH-2 RISC processors @ 23 MHz (cycle-accurate or fast instruction-accurate), sharing a common bus.
  - 32X VDP: Framebuffers (two 128KB banks for ping-pong rendering), Palette RAM, Shift/Fill hardware, and video overlay logic merging 32X RGB frames with Mega Drive VDP layers.
  - Memory Mapping & Security: 32X Gate Array registers (0xA15100 area), SDRAM (256KB), Framebuffer access protection, and ROM banking/decoding.
  - PWM Sound: 32X PWM audio registers and stereo sound generation mixed with YM2612/PSG.
