#pragma once

#include <array>
#include <functional>

#include "md/sega32x/pwm.hpp"
#include "md/types.hpp"

namespace md::sega32x {

// The two SH-2s. Most system registers are shared, but each CPU has its own
// interrupt mask and pending interrupts.
enum class Sh2 { Master = 0, Slave = 1 };

// SH-2 interrupt sources, by their bit in the interrupt mask register
// (SH-2 0x4000 bits 3-0). VRES has no mask bit: it can't be disabled.
enum class Sh2Interrupt { Pwm = 0, Cmd = 1, H = 2, V = 3, Vres = 4 };

// 32X system registers: the link between the 68000 and the two SH-2s. All
// three CPUs see the same physical registers, at different addresses and with
// different rights:
//
//   68000: 0xA15100-0xA1517F         SH-2: 0x20004000-0x2000403F (each CPU)
//
//   68000 side
//   off  register           bits
//   ---  -----------------  ------------------------------------------------
//   00   adapter control    FM (15) | REN (7, R) | RES (1) | ADEN (0)
//          ADEN 1: 32X mode (the 32X's memory appears on the 68000 bus)
//          RES  0: SH-2s held in reset, 1: running (0->1 restarts them)
//          REN  1: the adapter is ready (always, here)
//          FM   0: the 68000 owns the 32X VDP, 1: the SH-2s do
//   02   interrupt control  INTS (1) | INTM (0): 1 = CMD interrupt to the
//                           slave / master SH-2, until that SH-2 clears it
//   04   bank set           BK (1-0): 1 MB of cartridge ROM at 0x900000
//   06   DREQ control       FULL (7, R) | 68S (2) | DMA (1) | RV (0)
//          68S 1: the 68000 is feeding the DREQ FIFO (0 empties it)
//          RV  1: cartridge ROM back at 0x000000 for VDP DMA (SH-2s locked out)
//   08   DREQ source        bits 7-0 = A23-A16   0A: A15-A1
//   0C   DREQ destination   bits 7-0 = A23-A16   0E: A15-A0
//   10   DREQ length        in words, multiple of 4
//   12   DREQ FIFO          (W) words to the SH-2 side (8-word FIFO)
//   1A   SEGA TV            bit 0
//   20   communication 0-7  (R/W) eight 16-bit mailboxes shared with the SH-2s
//   30   PWM                control (68000 writes bits 3-0), cycle, L, R, mono (Pwm)
//
//   SH-2 side
//   00   interrupt mask     FM (15) | ADEN (9, R) | CART (8, R, 0 = inserted)
//                           | HEN (7) | V (3) | H (2) | CMD (1) | PWM (0)
//          FM and HEN are shared; the four mask bits are per CPU.
//   02   stand-by change    (stored)
//   04   H count            (stored)
//   06   DREQ control       (R) EMPTY (14) | 68S (2) | DMA (1) | RV (0)
//   08-10                   DREQ source / destination / length (R)
//   12   DREQ FIFO          (R) next word written by the 68000
//   14   VRES clear  16 V clear  18 H clear  1A CMD clear  1C PWM clear
//          (W) clears the writing CPU's pending interrupt. CMD clear also
//          clears its INTM/INTS bit, which the 68000 sees.
//   20   communication 0-7  (R/W) the same eight mailboxes
//   30   PWM                (R/W, Pwm)
//
// Interrupts. Each source raises a level on the SH-2's IRL pins, and the SH-2
// takes it through an auto-vector (64 + level / 2) when the level is above
// its SR mask:
//
//   source  raised by                                level  vector
//   VRES    the reset button                          14     71
//   V       start of VBlank (Mega Drive VDP timing)   12     70
//   H       HBlank, every (H count + 1) lines         10     69
//           (in VBlank only if HEN is set)
//   CMD     the 68000 setting INTM / INTS (0xA15102)   8     68
//   PWM     the PWM timer, every TM samples            6     67
//
// V, H and PWM go to both CPUs; each CPU's mask (0x4000 bits 3-0) decides
// whether it takes them. A source stays asserted until the CPU writes its
// clear register (0x4014-0x401C), so a handler must clear it before RTE.
//
// Writes come with a lane mask (0xFF00 = high byte at the even address,
// 0x00FF = low byte, 0xFFFF = word), as byte and word writes differ.
class SystemRegisters {
public:
    static constexpr u32 kMainBase  = 0xA1'5100;
    static constexpr u32 kMainSize  = 0x80;    // 0xA15100-0xA1517F
    static constexpr u32 kSh2Base   = 0x4000;  // within the SH-2's boot area
    static constexpr u32 kSh2Size   = 0x40;    // 0x4000-0x403F
    static constexpr int kCommWords = 8;
    static constexpr std::size_t kFifoWords = 8;

    // Adapter control (68000 0x00).
    static constexpr u16 kFm   = 0x8000;
    static constexpr u16 kRen  = 0x0080;
    static constexpr u16 kRes  = 0x0002;
    static constexpr u16 kAden = 0x0001;
    // DREQ control (68000 0x06).
    static constexpr u16 kFull = 0x0080;
    static constexpr u16 k68s  = 0x0004;
    static constexpr u16 kDma  = 0x0002;
    static constexpr u16 kRv   = 0x0001;

    // The IRL level each source drives (see the table above).
    [[nodiscard]] static constexpr int level(Sh2Interrupt source) noexcept {
        switch (source) {
        case Sh2Interrupt::Vres: return 14;
        case Sh2Interrupt::V:    return 12;
        case Sh2Interrupt::H:    return 10;
        case Sh2Interrupt::Cmd:  return 8;
        case Sh2Interrupt::Pwm:  return 6;
        }
        return 0;
    }

    SystemRegisters();

    // Power-on state: 32X mode off, SH-2s in reset, mailboxes cleared.
    void reset();

    // CART bit (SH-2 0x4000 bit 8) reads 0 when a cartridge is inserted.
    void set_cartridge_present(bool present) noexcept { cartridge_present_ = present; }

    // `offset` is relative to the register block (68000: 0x00-0x7F, SH-2:
    // 0x00-0x3F).
    [[nodiscard]] u16 main_read(u32 offset) const noexcept;
    void main_write(u32 offset, u16 value, u16 lanes);

    // Not const: reading the FIFO (0x12) takes a word out.
    [[nodiscard]] u16 sh2_read(Sh2 cpu, u32 offset);
    void sh2_write(Sh2 cpu, u32 offset, u16 value, u16 lanes);

    // --- State the rest of the 32X follows ------------------------------------
    [[nodiscard]] bool adapter_enabled() const noexcept { return aden_; }
    [[nodiscard]] bool sh2_reset_asserted() const noexcept { return !res_; }
    [[nodiscard]] bool sh2_owns_vdp() const noexcept { return fm_; }
    [[nodiscard]] u8   rom_bank() const noexcept { return bank_; }
    [[nodiscard]] bool rom_at_zero() const noexcept { return rv_; }
    // H interrupt settings (SH-2 0x4004 and 0x4000 bit 7).
    [[nodiscard]] u8   h_count() const noexcept { return static_cast<u8>(hcount_); }
    [[nodiscard]] bool h_interrupt_in_vblank() const noexcept { return hen_; }

    [[nodiscard]] Pwm& pwm() noexcept { return pwm_; }
    [[nodiscard]] const Pwm& pwm() const noexcept { return pwm_; }

    // Called with true when the SH-2s enter reset, false when they leave it
    // (68000 RES 0->1: both CPUs restart from their boot ROM).
    void set_sh2_reset_callback(std::function<void(bool asserted)> cb) { sh2_reset_changed_ = std::move(cb); }

    // --- SH-2 interrupts --------------------------------------------------------
    // Event sources (V, H, PWM from the 32X VDP and PWM; VRES from the reset
    // button): pending until the SH-2 writes the matching clear register.
    // CMD comes from the 68000's INTM/INTS bits instead.
    void raise_interrupt(Sh2 cpu, Sh2Interrupt source);
    [[nodiscard]] bool interrupt_pending(Sh2 cpu, Sh2Interrupt source) const noexcept;
    // Highest enabled pending level for that CPU (0 = none).
    [[nodiscard]] int sh2_irq_level(Sh2 cpu) const noexcept;
    // Called whenever a CPU's level changes.
    void set_sh2_irq_callback(std::function<void(Sh2, int)> cb) { sh2_irq_changed_ = std::move(cb); }

    // --- Inspection -------------------------------------------------------------
    [[nodiscard]] u16 comm(int i) const noexcept { return comm_[static_cast<std::size_t>(i & 7)]; }
    [[nodiscard]] u8  interrupt_mask(Sh2 cpu) const noexcept { return mask_[index(cpu)]; }
    [[nodiscard]] u32 dreq_source() const noexcept { return dreq_source_; }
    [[nodiscard]] u32 dreq_destination() const noexcept { return dreq_destination_; }
    [[nodiscard]] u16 dreq_length() const noexcept { return dreq_length_; }
    [[nodiscard]] std::size_t fifo_size() const noexcept { return fifo_count_; }

private:
    static constexpr std::size_t index(Sh2 cpu) noexcept { return static_cast<std::size_t>(cpu); }
    static constexpr u16 merge(u16 old_value, u16 value, u16 lanes) noexcept {
        return static_cast<u16>((old_value & ~lanes) | (value & lanes));
    }

    [[nodiscard]] u16 dreq_control() const noexcept;
    void set_adapter_control(u16 value);
    void push_fifo(u16 value);
    void notify_irq();

    // Adapter control
    bool fm_   = false;
    bool res_  = false;
    bool aden_ = false;
    bool hen_  = false;

    // Interrupts: INTM (bit 0) / INTS (bit 1); per-CPU masks and events.
    u8 cmd_ = 0;
    std::array<u8, 2> mask_{};
    std::array<u8, 2> events_{};   // bit = Sh2Interrupt (except Cmd)
    std::array<int, 2> irq_level_{};

    u8 bank_ = 0;

    // DREQ
    bool s68_ = false;
    bool dma_ = false;
    bool rv_  = false;
    u32 dreq_source_ = 0;
    u32 dreq_destination_ = 0;
    u16 dreq_length_ = 0;
    std::array<u16, kFifoWords> fifo_{};
    std::size_t fifo_head_ = 0;
    std::size_t fifo_count_ = 0;
    u8 fifo_high_byte_ = 0;        // byte write to 0x12, completed by 0x13

    u16 tv_ = 0;
    std::array<u16, kCommWords> comm_{};
    Pwm pwm_;
    std::array<u16, 2> standby_{};
    u16 hcount_ = 0;

    bool cartridge_present_ = false;

    std::function<void(bool)> sh2_reset_changed_;
    std::function<void(Sh2, int)> sh2_irq_changed_;
};

} // namespace md::sega32x
