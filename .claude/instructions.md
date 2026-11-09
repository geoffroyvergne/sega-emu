# Claude System Instructions for Mega Drive/CD Emulator Project

## Code Style & Architecture
- **CMake:** Use modern target-based CMake (`target_link_libraries`, `target_include_directories`).
- **SDL:** Use SDL for window management, texture rendering (streaming texture for the VDP framebuffer), audio queuing, and input polling.
- **Modularity:** Separate components into isolated classes or namespaces (`MegaDrive::M68k`, `MegaDrive::VDP`, `MegaDrive::Memory`).

## Response Guidelines
1. **Incremental Changes:** Never rewrite a 500-line file to change 5 lines. Provide clean diffs or specific function replacements.
2. **Explain the "Why":** When implementing hardware behavior (e.g., VDP status register flags, Sega CD bios memory mapping), explain the hardware quirk being addressed.
3. **No Placeholders:** Do not emit code with `// TODO: implement later` in core logic unless explicitly asked. Fill the logic or stub it with a deterministic logging macro.
4. **State Machine Integrity:** Always consider synchronization. The Mega Drive relies on the 68k and Z80 running in parallel, synchronized by scanlines/cycles.
