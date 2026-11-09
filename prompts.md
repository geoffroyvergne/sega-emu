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