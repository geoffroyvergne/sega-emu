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