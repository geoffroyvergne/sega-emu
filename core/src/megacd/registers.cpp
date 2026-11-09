#include "md/megacd/registers.hpp"

namespace md::megacd {

namespace {

constexpr u32 kRegControl  = 0x00;
constexpr u32 kRegMemMode  = 0x02;
constexpr u32 kRegHint     = 0x06;
constexpr u32 kRegFlags    = 0x0E;
constexpr u32 kRegCommand  = 0x10;  // 0x10-0x1F
constexpr u32 kRegStatus   = 0x20;  // 0x20-0x2F
constexpr u32 kRegIntMask  = 0x32;

constexpr bool in_command(u32 offset) { return offset >= kRegCommand && offset < kRegStatus; }
constexpr bool in_status(u32 offset)  { return offset >= kRegStatus && offset < kRegStatus + 0x10; }
constexpr std::size_t comm_index(u32 offset) { return static_cast<std::size_t>((offset & 0x0F) >> 1); }

} // namespace

void MegaCdRegisters::reset() {
    ifl2_ = false;
    sbrq_ = false;
    sres_ = false;
    lines_ = 0;
    events_ = 0;
    sub_led_res_ = 0;
    pm_ = 0;
    main_flags_ = 0;
    sub_flags_ = 0;
    command_.fill(0);
    status_.fill(0);
    hint_vector_ = 0;
    main_regs_.fill(0);
    sub_regs_.fill(0);
    notify_irq();
}

// ===========================================================================
// Main CPU side (0xA12000 + offset)
// ===========================================================================

u16 MegaCdRegisters::main_read(u32 offset) const noexcept {
    offset &= 0xFE;
    switch (offset) {
    case kRegControl:
        return static_cast<u16>((ifl2_ ? 0x0100 : 0) | (sbrq_ ? 0x0002 : 0) | (sres_ ? 0x0001 : 0));
    case kRegMemMode:
        return static_cast<u16>((u16{prg_ram_.write_protect()} << 8) | (prg_ram_.main_bank() << 6) |
                                (word_ram_.mode() == WordRam::Mode::OneMeg ? 0x04 : 0) |
                                (word_ram_.dmna() ? 0x02 : 0) | (word_ram_.ret() ? 0x01 : 0));
    case kRegHint:
        return hint_vector_;
    case kRegFlags:
        return static_cast<u16>((main_flags_ << 8) | sub_flags_);
    default:
        break;
    }
    if (in_command(offset)) return command_[comm_index(offset)];
    if (in_status(offset))  return status_[comm_index(offset)];
    return main_regs_[offset >> 1];
}

void MegaCdRegisters::main_write(u32 offset, u16 value, u16 lanes) {
    offset &= 0xFE;
    switch (offset) {
    case kRegControl:
        if (lanes & 0xFF00) {
            // IFL2: writing 1 rings the sub CPU's INT2 doorbell. It stays set
            // until the sub CPU acknowledges the interrupt.
            if (value & 0x0100) {
                ifl2_ = true;
                notify_irq();
            }
        }
        if (lanes & 0x00FF) {
            sres_ = (value & 0x0001) != 0;
            sbrq_ = (value & 0x0002) != 0;
        }
        return;

    case kRegMemMode:
        if (lanes & 0xFF00) prg_ram_.set_write_protect(static_cast<u8>(value >> 8));
        if (lanes & 0x00FF) {
            prg_ram_.set_main_bank(static_cast<u8>((value >> 6) & 3));
            word_ram_.main_write_dmna((value & 0x0002) != 0);
            // MODE and RET are read-only for the main CPU.
        }
        return;

    case kRegHint:
        hint_vector_ = merge(hint_vector_, value, lanes);
        return;

    case kRegFlags:
        // The main CPU owns the high byte only; the sub CPU's flags are read-only.
        if (lanes & 0xFF00) main_flags_ = static_cast<u8>(value >> 8);
        return;

    default:
        break;
    }
    if (in_command(offset)) {
        auto& reg = command_[comm_index(offset)];
        reg = merge(reg, value, lanes);
        return;
    }
    if (in_status(offset)) return;  // status registers are read-only for the main CPU
    main_regs_[offset >> 1] = merge(main_regs_[offset >> 1], value, lanes);
}

// ===========================================================================
// Sub CPU side (0xFF8000 + offset)
// ===========================================================================

u16 MegaCdRegisters::sub_read(u32 offset) const noexcept {
    offset &= kSubArea - 2;
    switch (offset) {
    case kRegControl:
        // RES0 reads 1 once the CD hardware is out of reset (immediately, here).
        return static_cast<u16>((sub_led_res_ & 0x0300) | 0x0001);
    case kRegMemMode:
        return static_cast<u16>((u16{prg_ram_.write_protect()} << 8) | (pm_ << 3) |
                                (word_ram_.mode() == WordRam::Mode::OneMeg ? 0x04 : 0) |
                                (word_ram_.dmna() ? 0x02 : 0) | (word_ram_.ret() ? 0x01 : 0));
    case kRegFlags:
        return static_cast<u16>((main_flags_ << 8) | sub_flags_);
    default:
        break;
    }
    if (in_command(offset)) return command_[comm_index(offset)];
    if (in_status(offset))  return status_[comm_index(offset)];
    return sub_regs_[offset >> 1];
}

void MegaCdRegisters::sub_write(u32 offset, u16 value, u16 lanes) {
    offset &= kSubArea - 2;
    switch (offset) {
    case kRegControl:
        sub_led_res_ = merge(sub_led_res_, value, lanes);
        return;

    case kRegMemMode:
        // WP (high byte) and DMNA are read-only for the sub CPU.
        if (lanes & 0x00FF) {
            pm_ = static_cast<u8>((value >> 3) & 3);
            word_ram_.set_priority_mode(pm_);
            const auto mode = (value & 0x04) ? WordRam::Mode::OneMeg : WordRam::Mode::TwoMeg;
            word_ram_.sub_write_mode(mode, (value & 0x01) != 0);
        }
        return;

    case kRegFlags:
        // The sub CPU owns the low byte only.
        if (lanes & 0x00FF) sub_flags_ = static_cast<u8>(value & 0xFF);
        return;

    case kRegIntMask:
        sub_regs_[offset >> 1] = merge(sub_regs_[offset >> 1], value, lanes);
        notify_irq();  // enabling INT2 while IFL2 is set raises it now
        return;

    default:
        break;
    }
    if (in_command(offset)) return;  // command registers are read-only for the sub CPU
    if (in_status(offset)) {
        auto& reg = status_[comm_index(offset)];
        reg = merge(reg, value, lanes);
        return;
    }
    sub_regs_[offset >> 1] = merge(sub_regs_[offset >> 1], value, lanes);
}

// ===========================================================================
// Interrupts
// ===========================================================================

int MegaCdRegisters::sub_irq_level() const noexcept {
    // TODO: INT6 (subcode).
    const u8 mask = sub_interrupt_mask();
    for (int level = 6; level >= 1; --level) {
        const bool asserted = level == 2 ? ifl2_ : ((lines_ | events_) >> level) & 1;
        if (asserted && ((mask >> level) & 1)) return level;
    }
    return 0;
}

void MegaCdRegisters::set_interrupt_line(int level, bool asserted) {
    const auto bit = static_cast<u8>(1u << level);
    const u8 before = lines_;
    lines_ = asserted ? static_cast<u8>(lines_ | bit) : static_cast<u8>(lines_ & ~bit);
    if (lines_ != before) notify_irq();
}

void MegaCdRegisters::acknowledge_sub_interrupt(int level) {
    if (level == 2) ifl2_ = false;
    events_ = static_cast<u8>(events_ & ~(1u << level));
    notify_irq();
}

void MegaCdRegisters::raise_interrupt(int level) {
    events_ = static_cast<u8>(events_ | (1u << level));
    notify_irq();
}

void MegaCdRegisters::notify_irq() {
    if (sub_irq_changed_) sub_irq_changed_(sub_irq_level());
}

} // namespace md::megacd
