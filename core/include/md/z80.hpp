#pragma once

#include <memory>

#include "md/types.hpp"

namespace md {

class Memory;

// Zilog Z80 sound CPU (3.58 MHz = master clock / 15).
//
// Thin wrapper around the superzazu/z80 core (hidden behind a pimpl). All
// memory accesses go through Memory::z80_read / z80_write, which implement
// the Z80's view of the system (its RAM, YM2612, bank register, VDP/PSG and
// the banked window onto the 68000 bus).
class Z80 {
public:
    static constexpr int kClockDivider = 15;

    explicit Z80(Memory& memory);
    ~Z80();

    Z80(const Z80&) = delete;
    Z80& operator=(const Z80&) = delete;

    void reset();

    // Executes one instruction (plus any interrupt it accepts). Returns T-states.
    int step();

    // Level of the /INT line. The VDP holds it for about one scanline at the
    // start of VBlank; mode 1 then jumps to 0x0038.
    void set_int_line(bool asserted);

    [[nodiscard]] u16 pc() const noexcept;
    [[nodiscard]] u64 cycles() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace md
