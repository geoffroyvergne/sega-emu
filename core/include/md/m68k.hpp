#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>

#include "md/types.hpp"

namespace md {

class Memory;

// Motorola 68000 main CPU (7.67 MHz NTSC / 7.60 MHz PAL = master clock / 7).
//
// Thin wrapper around the Moira core. Moira is an implementation detail: it
// never appears in this header, so the rest of the codebase only depends on
// this interface and the CPU backend can be swapped without touching callers.
class M68k {
public:
    struct Registers {
        std::array<u32, 8> d{};
        std::array<u32, 8> a{};   // a[7] = active stack pointer
        u32 pc  = 0;
        u16 sr  = 0;
        u32 usp = 0;
        u32 ssp = 0;
    };

    // Called during the interrupt-acknowledge cycle with the level being serviced.
    // The VDP uses this to clear its pending VINT/HINT flag.
    using InterruptAckHandler = std::function<void(int level)>;

    explicit M68k(Memory& memory);
    ~M68k();

    M68k(const M68k&) = delete;
    M68k& operator=(const M68k&) = delete;

    // Hard reset: loads SSP from 0x000000 and PC from 0x000004.
    void reset();

    // Executes one instruction (or exception). Returns the cycles it took.
    int step();

    // Executes whole instructions until cycles() >= target. May overshoot by
    // up to one instruction, since an instruction cannot be split.
    void run_until(s64 target_cycle);

    // Total CPU cycles since power-on.
    [[nodiscard]] s64 cycles() const noexcept;

    // Drives the IPL pins (0 = none, 1-7). The Mega Drive uses autovectors:
    // level 6 = VINT, level 4 = HINT, level 2 = external (controller port TH).
    void set_irq_level(int level);
    [[nodiscard]] int irq_level() const noexcept;

    void set_interrupt_ack_handler(InterruptAckHandler handler);

    [[nodiscard]] bool is_halted() const noexcept;

    [[nodiscard]] Registers registers() const;
    void set_pc(u32 pc);

    // Disassembles the instruction at `address` without bus side effects.
    // `length` (optional) receives the instruction size in bytes.
    [[nodiscard]] std::string disassemble(u32 address, int* length = nullptr) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace md
