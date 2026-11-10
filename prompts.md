# Mega Drive

# Prompt 1: Project initialization (Architecture & CMake)

Context: I am starting a SEGA Mega Drive (Genesis) emulator in Modern C++ using CMake and SDL for input/display.
Task: Generate a clean, modular project directory structure. Provide a CMakeLists.txt that fetches or links SDL, and sets up targets for the core library and the executable. Also, provide a basic main.cpp that initializes an SDL window, creates a basic 60Hz game loop, and handles window close events. Keep emulation core logic strictly detached from SDL.

# Prompt 2: The Memory System (Memory Map)

Context: I need to implement the Mega Drive memory map. The Motorola 68000 has a 24-bit address space (16MB).
Task: Create a Memory class in C++ that maps the standard Mega Drive memory regions:
• ROM (0x000000 - 0x3FFFFF)
• Main RAM (0xFF0000 - 0xFFFFFF, mirrored)
• VDP ports (0xC00000 - 0xC0001F)
• Z80 space & I/O ports (0xA00000 - 0xA1FFFF)
Implement read_byte, read_word, read_long, and their corresponding write functions. Ensure correct big-endian to little-endian handling for 16-bit and 32-bit accesses.

# Prompt 3: CPU Integration (Using an existing library or skeleton)

Note: Writing a 68000 from scratch takes months. It is highly recommended to use an existing CPU implementation, such as Musashi or Moira. Task Claude with integrating it.

Context: I want to integrate an existing open-source 68000 emulator core (like Musashi or a clean C++ header-only 68k library) into our CMake build.
Task: Update our CMakeLists.txt to include the CPU core. Then, write the bridge code/glue logic connecting this CPU core's read/write hooks to our previously created Memory class.

# Prompt 4: The VDP (Base Rendering and Scanlines)

Context: I need to start implementing the Video Display Processor (VDP). The Mega Drive outputs a resolution of 320x224 or 256x224.
Task: Implement the basic structure of the VDP class. Include its internal registers (24 registers), VRAM (64KB), CRAM (128 bytes), and VSRAM (80 bytes). Write the code to handle VDP control port and data port writes from the CPU. Implement a dummy loop that triggers a Vertical Blanking (VBlank) interrupt (68k Interrupt Level 6) and Horizontal Blanking interrupt (Level 4) based on cycles executed.

# Prompt 5: Next Step - Introduction of the Mega-CD
Only tackle the Mega-CD once the Mega Drive is running simple games (homebrews or Sonic 1 without sound).

Context: Now that our base Mega Drive core is functional, I want to add Mega-CD architecture foundations. The Mega-CD adds a second M68k CPU (Sub-CPU) at 12.5MHz, a graphics ASIC, PCM sound, and CD-ROM hardware.
Task: Design the architectural layout to support the Sub-CPU and its communication gates (graphics/program RAM banks, communication registers at 0xA12000). Show how the Main CPU and Sub-CPU will ping-pong data via memory-mapped registers.

# Mega CD

# Prompt 1: Overall architecture and memory allocation (The Sega CD Gate Array)

Context: Our Mega Drive core is fully working. We are now adding Mega-CD capabilities. The Mega-CD adds a Sub-CPU (M68k @ 12.5MHz), 512KB Program RAM, 256KB Word RAM, and communication registers mapped to the Main-CPU area 0xA12000 - 0xA120FF.
Task: Create the foundational blueprint for a SegaCD subsystem. Implement the MegaCD_Registers structures handling the communication flags (Comms Flags/Registers 0xA1200E - 0xA1202F), Write Protect rules, and Reset/Bus requests. Provide the structural setup for the new 512KB Program RAM and Word RAM, including the memory mapping switches for Word RAM Mode 0 (2M mode, split allocation) and Mode 2 (1M mode, ping-pong banks).

# Prompt 2 : Boucle de synchronisation Dual-CPU (Main & Sub)

Context: The Mega-CD relies on the original Main-CPU (7.67MHz) and the new Sub-CPU (12.5MHz) running concurrently. The BIOS initializes by having both CPUs poll communication flags.
Task: Update our main execution/emulation loop to handle both CPUs. Since they run at different clock speeds, design a cycle-stepper mechanism where for every N cycles executed by the Main-CPU, the Sub-CPU executes the proportional amount of cycles (Ratio ~1.63). Ensure that when the Sub-CPU triggers a Main-CPU interrupt (or vice-versa via the Gate Array registers), the interrupt is asserted immediately in the next step.

# Prompt 3: CDC emulation (Sega CD Data Controller / LC8951)

Context: To boot the Sega CD BIOS and load games, we need to emulate the CD-ROM drive interface and the CDC (Sega CD Data Controller chip, LC8951).
Task: Implement the CDC class. It needs to handle the CDC registers, the host interface, data transfers to/from the Sub-CPU Program RAM or Word RAM via DMA, and the sector buffer management. Create a basic parser layout capable of reading .cue / .bin tracks so we can stream data sectors into the CDC buffer at standard 1X/2X CD-ROM speeds.

# Prompt 4: The ASIC Graphics Processor (Rotation and Scaling)

Context: The Mega-CD includes a custom Graphics ASIC that allows the Sub-CPU to perform hardware rotation, scaling, and stamping of graphics into the Word RAM, which the Main-CPU VDP can then display.
Task: Implement the SegaCD_ASIC rendering pipeline. It must process the ASIC registers (stamping size, map data pointer, vector table pointer). Write the core mathematical logic for the 2D affine transformation (rotation, scaling) that translates a source bitmap/tile into a destination buffer in Word RAM, respecting cell-based layouts.

# 32X

# Prompt 1: Overall Structure of the 32X and Communication Registers (Sys Regs)

Context: Our Mega Drive and Mega-CD cores are fully functional. We are now adding Sega 32X support. The 32X maps its system control registers to the Main-CPU space at 0xA15100 - 0xA1517F. It features two Hitachi SH-2 CPUs (Master and Slave) and 256KB of shared SDRAM.
Task: Create the foundation for a Sega32X subsystem. Implement the 32X_Registers structure handling the Adapter Control, Interrupt Control, and Bank Switching. Write the code for the Main-CPU (68k) to communication registers like the DMAC / Command Registers (0xA15120 - 0xA1512F) used to pass messages between the 68k and the dual SH-2s. Set up the memory mapping structure for the 256KB SDRAM.

# Prompt 2: Skeleton and Interfacing of SH-2 Processors

Note : Comme pour le 68000, vous utiliserez probablement un cœur SH-2 existant (comme une adaptation de MAME ou un interpréteur C++ propre).
Context: I am integrating a dual SH-2 CPU interpreter core into our 32X project. The Master and Slave SH-2s run at 23 MHz, while our Main-CPU 68k runs at 7.67 MHz.
Task: Update our emulation stepper/loop to include both SH-2 CPUs running alongside the 68k. Design the shared memory map for the SH-2 processors, handling cached (0x06000000) and uncached (0x26000000) access regions to SDRAM, Framebuffer, and Palette RAM. Ensure that the Master/Slave ID is correctly identified via the SYSCR (System Control Register) status or CPU initialization vectors.

# Prompt 3: The 32X VDP and Video Overlay

Context: The 32X VDP features two 128KB Framebuffers (used in a ping-pong fashion via a frame blank switch) and outputs in 15-bit direct color or indexed modes. This output must be mixed with the Mega Drive VDP layer.
Task: Implement the 32X_VDP class. Include structures for the two 128KB Framebuffer banks and the 512-byte Palette RAM (CRAM). Implement the Line Table and Bitmap drawing modes (Direct Color, Palette, and Run-Length Encoded). Finally, write the composition logic that takes the finalized 32X framebuffer and overlays it on top of the Mega Drive VDP frame buffer, respecting transparency pixels/priorities before passing the final texture to SDL.

# Prompt 4: Exception Handling and Timed Interrupts (PWM & Timers)

Context: The 32X relies on frequent interrupts to synchronize the 68k and the SH-2s, particularly H-Blank, V-Blank, and PWM timers.
Task: Implement the interrupt controller for the SH-2s. Write the logic that translates a 68k register write or a VDP event into an SH-2 interrupt vector (e.g., Level 14 for V-Blank, Level 12 for H-Blank, Level 6 for PWM). Include a basic skeleton for the PWM audio registers to prevent games from freezing while waiting for audio buffer clear flags.