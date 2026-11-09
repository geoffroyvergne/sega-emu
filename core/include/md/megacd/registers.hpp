#pragma once

#include <array>
#include <functional>

#include "md/megacd/program_ram.hpp"
#include "md/megacd/word_ram.hpp"
#include "md/types.hpp"

namespace md::megacd {

// The Mega-CD gate array registers that link the two CPUs. Both CPUs see the
// same physical registers, at different addresses and with different rights:
//
//   main CPU: 0xA12000-0xA1202F        sub CPU: 0xFF8000-0xFF802F
//
//   off  main CPU (read / write)                   sub CPU (read / write)
//   ---  -----------------------------------------  ------------------------------------
//   00   IFL2 (bit 8) | SBRQ (bit 1) | SRES (bit 0) LED (bits 9-8) | RES0 (bit 0)
//          IFL2 = 1: interrupt the sub CPU (INT2)
//          SBRQ = 1: request the sub CPU's bus (halts it)
//          SRES = 0: hold the sub CPU in reset, 1: run
//   02   WP (8-15, W) | BK (7-6, W) | MODE (2, R)  WP (R) | PM (4-3) | MODE (2) | DMNA (1, R) | RET (0)
//          | DMNA (1, W) | RET (0, R)
//   04+  CDC, H-INT vector, stopwatch (stored, not interpreted yet)
//   0E   main flags (8-15, R/W) | sub flags (R)    main flags (R) | sub flags (0-7, R/W)
//   10   COMCMD0-7 (R/W)                           COMCMD0-7 (R)
//   20   COMSTAT0-7 (R)                            COMSTAT0-7 (R/W)
//   30+  -                                         timer, interrupt mask (0x33), CDD/... (stored)
//
// Writes come with a lane mask (0xFF00 = high byte at the even address,
// 0x00FF = low byte, 0xFFFF = word), because byte and word writes to the same
// register behave differently.
class MegaCdRegisters {
public:
    static constexpr u32 kMainCommArea = 0x30;   // main CPU sees offsets 0x00-0x2F
    static constexpr u32 kSubArea      = 0x200;  // sub CPU register block size
    static constexpr int kCommWords    = 8;

    // Sub CPU interrupt mask (sub register 0x32, low byte): bit N enables
    // interrupt level N. 1 = graphics, 2 = main CPU (IFL2), 3 = timer,
    // 4 = CD drive (CDD), 5 = CD data controller (CDC), 6 = subcode.
    static constexpr u8 kInt2Enable = 1u << 2;
    static constexpr u8 kInt5Enable = 1u << 5;

    MegaCdRegisters(ProgramRam& prg_ram, WordRam& word_ram)
        : prg_ram_(prg_ram), word_ram_(word_ram) {}

    void reset();

    // Accesses use the register offset (0x00-0xFF main, 0x000-0x1FF sub).
    [[nodiscard]] u16 main_read(u32 offset) const noexcept;
    void main_write(u32 offset, u16 value, u16 lanes);

    [[nodiscard]] u16 sub_read(u32 offset) const noexcept;
    void sub_write(u32 offset, u16 value, u16 lanes);

    // --- Sub CPU control lines ------------------------------------------------
    [[nodiscard]] bool sub_reset_asserted() const noexcept { return !sres_; }
    [[nodiscard]] bool sub_bus_requested() const noexcept  { return sbrq_; }
    // The sub CPU is stopped: the main CPU may access PRG-RAM.
    [[nodiscard]] bool sub_halted() const noexcept { return sub_reset_asserted() || sub_bus_requested(); }

    // --- INT2 (main -> sub "doorbell") ---------------------------------------
    [[nodiscard]] bool int2_pending() const noexcept { return ifl2_; }
    [[nodiscard]] u8 sub_interrupt_mask() const noexcept { return static_cast<u8>(sub_regs_[0x32 / 2] & 0xFF); }
    // Interrupt level the gate array asks of the sub CPU (0 = none): the
    // highest enabled level whose line is asserted.
    [[nodiscard]] int sub_irq_level() const noexcept;

    // Level-triggered interrupt sources (CDC = 5, ...): asserted until the
    // source itself is cleared (e.g. the CDC's DECI/DTEI flags).
    void set_interrupt_line(int level, bool asserted);

    // Event interrupts (graphics ASIC = 1, ...): pending until the sub CPU
    // acknowledges that level.
    void raise_interrupt(int level);
    void acknowledge_sub_interrupt(int level);
    // Called whenever sub_irq_level() may have changed.
    void set_sub_irq_callback(std::function<void(int)> cb) { sub_irq_changed_ = std::move(cb); }

    // --- Communication registers (inspection) ---------------------------------
    [[nodiscard]] u8 main_flags() const noexcept { return main_flags_; }
    [[nodiscard]] u8 sub_flags() const noexcept { return sub_flags_; }
    [[nodiscard]] u16 command(int i) const noexcept { return command_[static_cast<std::size_t>(i & 7)]; }
    [[nodiscard]] u16 status(int i) const noexcept { return status_[static_cast<std::size_t>(i & 7)]; }

    [[nodiscard]] u8 priority_mode() const noexcept { return pm_; }
    [[nodiscard]] u16 hint_vector() const noexcept { return hint_vector_; }

private:
    static u16 merge(u16 old_value, u16 value, u16 lanes) noexcept {
        return static_cast<u16>((old_value & ~lanes) | (value & lanes));
    }
    void notify_irq();

    ProgramRam& prg_ram_;
    WordRam&    word_ram_;

    // Reg 00
    bool ifl2_ = false;
    bool sbrq_ = false;
    bool sres_ = false;      // power-on: sub CPU held in reset
    u8   lines_ = 0;         // asserted level-triggered sources, bit N = level N
    u8   events_ = 0;        // pending event interrupts, bit N = level N
    u16  sub_led_res_ = 0;   // sub side of reg 00 (LEDs, RES0)

    // Reg 02 (WP and BK live in ProgramRam, MODE/DMNA/RET in WordRam)
    u8 pm_ = 0;

    // Reg 0E-2F
    u8 main_flags_ = 0;
    u8 sub_flags_  = 0;
    std::array<u16, kCommWords> command_{};
    std::array<u16, kCommWords> status_{};

    u16 hint_vector_ = 0;

    // Everything else is stored as written, for the CD/graphics/timer
    // hardware still to come.
    std::array<u16, 0x80> main_regs_{};     // main offsets 0x00-0xFF
    std::array<u16, kSubArea / 2> sub_regs_{};

    std::function<void(int)> sub_irq_changed_;
};

} // namespace md::megacd
