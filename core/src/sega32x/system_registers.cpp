#include "md/sega32x/system_registers.hpp"

namespace md::sega32x {

namespace {

constexpr u8 bit(Sh2Interrupt source) noexcept { return static_cast<u8>(1u << static_cast<int>(source)); }

// SH-2 interrupt mask register (SH-2 0x00).
constexpr u16 kSh2Aden = 0x0200;
constexpr u16 kSh2Cart = 0x0100;   // 1 = no cartridge
constexpr u16 kSh2Hen  = 0x0080;
constexpr u8  kMaskBits = 0x0F;

// SH-2 DREQ control: FIFO empty.
constexpr u16 kSh2Empty = 0x4000;

constexpr bool is_comm(u32 offset) noexcept { return (offset & 0x30) == 0x20; }
constexpr std::size_t comm_index(u32 offset) noexcept { return (offset & 0x0E) >> 1; }
constexpr bool is_pwm(u32 offset) noexcept { return (offset & 0x30) == 0x30; }

} // namespace

SystemRegisters::SystemRegisters() {
    // The PWM timer interrupts both SH-2s.
    pwm_.set_interrupt_callback([this] {
        raise_interrupt(Sh2::Master, Sh2Interrupt::Pwm);
        raise_interrupt(Sh2::Slave, Sh2Interrupt::Pwm);
    });
    reset();
}

void SystemRegisters::reset() {
    fm_ = false;
    res_ = false;
    aden_ = false;
    hen_ = false;
    cmd_ = 0;
    mask_ = {};
    events_ = {};
    bank_ = 0;
    s68_ = dma_ = rv_ = false;
    dreq_source_ = dreq_destination_ = 0;
    dreq_length_ = 0;
    fifo_head_ = fifo_count_ = 0;
    fifo_high_byte_ = 0;
    tv_ = 0;
    comm_ = {};
    pwm_.reset();
    standby_ = {};
    hcount_ = 0;
    notify_irq();
}

u16 SystemRegisters::dreq_control() const noexcept {
    return static_cast<u16>((s68_ ? k68s : 0) | (dma_ ? kDma : 0) | (rv_ ? kRv : 0));
}

// ===========================================================================
// 68000 side
// ===========================================================================

u16 SystemRegisters::main_read(u32 offset) const noexcept {
    offset &= kMainSize - 2;
    if (is_comm(offset)) return comm_[comm_index(offset)];
    if (is_pwm(offset)) return pwm_.read(offset & 0x0E);
    switch (offset) {
    case 0x00: return static_cast<u16>((fm_ ? kFm : 0) | kRen | (res_ ? kRes : 0) | (aden_ ? kAden : 0));
    case 0x02: return cmd_;
    case 0x04: return bank_;
    case 0x06: return static_cast<u16>(dreq_control() | (fifo_count_ == kFifoWords ? kFull : 0));
    case 0x08: return static_cast<u16>(dreq_source_ >> 16);
    case 0x0A: return static_cast<u16>(dreq_source_);
    case 0x0C: return static_cast<u16>(dreq_destination_ >> 16);
    case 0x0E: return static_cast<u16>(dreq_destination_);
    case 0x10: return dreq_length_;
    case 0x1A: return tv_;
    default:   return 0;  // 0x12 (FIFO) is write-only; the rest is unused
    }
}

void SystemRegisters::main_write(u32 offset, u16 value, u16 lanes) {
    offset &= kMainSize - 2;
    if (is_comm(offset)) {
        auto& word = comm_[comm_index(offset)];
        word = merge(word, value, lanes);
        return;
    }
    if (is_pwm(offset)) {
        pwm_.write(offset & 0x0E, value, lanes, false);
        return;
    }
    const u16 v = merge(main_read(offset), value, lanes);
    switch (offset) {
    case 0x00: set_adapter_control(v); break;
    case 0x02:
        cmd_ = static_cast<u8>(v & 3);
        notify_irq();
        break;
    case 0x04: bank_ = static_cast<u8>(v & 3); break;
    case 0x06:
        s68_ = (v & k68s) != 0;
        dma_ = (v & kDma) != 0;
        rv_  = (v & kRv) != 0;
        if (!s68_) fifo_head_ = fifo_count_ = 0;   // stopping the transfer empties the FIFO
        break;
    case 0x08: dreq_source_ = (dreq_source_ & 0x00'FFFF) | (u32{v & 0xFFu} << 16); break;
    case 0x0A: dreq_source_ = (dreq_source_ & 0xFF'0000) | (v & 0xFFFEu); break;
    case 0x0C: dreq_destination_ = (dreq_destination_ & 0x00'FFFF) | (u32{v & 0xFFu} << 16); break;
    case 0x0E: dreq_destination_ = (dreq_destination_ & 0xFF'0000) | v; break;
    case 0x10: dreq_length_ = static_cast<u16>(v & 0xFFFC); break;
    case 0x12:
        // A word goes in whole; a byte pair completes on the low byte.
        if (lanes == 0xFF00) {
            fifo_high_byte_ = static_cast<u8>(value >> 8);
        } else if (lanes == 0x00FF) {
            push_fifo(static_cast<u16>((fifo_high_byte_ << 8) | (value & 0xFF)));
        } else {
            push_fifo(value);
        }
        break;
    case 0x1A: tv_ = static_cast<u16>(v & 0x0001); break;
    default: break;
    }
}

void SystemRegisters::set_adapter_control(u16 value) {
    fm_ = (value & kFm) != 0;
    aden_ = (value & kAden) != 0;
    const bool res = (value & kRes) != 0;
    if (res != res_) {
        res_ = res;
        if (sh2_reset_changed_) sh2_reset_changed_(!res_);
    }
}

void SystemRegisters::push_fifo(u16 value) {
    if (!s68_ || fifo_count_ == kFifoWords) return;  // not feeding, or full: lost
    fifo_[(fifo_head_ + fifo_count_) % kFifoWords] = value;
    ++fifo_count_;
    // The length counts the words still to send; the transfer ends at 0.
    if (dreq_length_ > 0 && --dreq_length_ == 0) s68_ = false;
}

// ===========================================================================
// SH-2 side
// ===========================================================================

u16 SystemRegisters::sh2_read(Sh2 cpu, u32 offset) {
    offset &= kSh2Size - 2;
    if (is_comm(offset)) return comm_[comm_index(offset)];
    if (is_pwm(offset)) return pwm_.read(offset & 0x0E);
    const auto c = index(cpu);
    switch (offset) {
    case 0x00:
        return static_cast<u16>((fm_ ? kFm : 0) | (aden_ ? kSh2Aden : 0) |
                                (cartridge_present_ ? 0 : kSh2Cart) | (hen_ ? kSh2Hen : 0) | mask_[c]);
    case 0x02: return standby_[c];
    case 0x04: return hcount_;
    case 0x06: return static_cast<u16>(dreq_control() | (fifo_count_ == 0 ? kSh2Empty : 0));
    case 0x08: return static_cast<u16>(dreq_source_ >> 16);
    case 0x0A: return static_cast<u16>(dreq_source_);
    case 0x0C: return static_cast<u16>(dreq_destination_ >> 16);
    case 0x0E: return static_cast<u16>(dreq_destination_);
    case 0x10: return dreq_length_;
    case 0x12: {
        if (fifo_count_ == 0) return 0;
        const u16 word = fifo_[fifo_head_];
        fifo_head_ = (fifo_head_ + 1) % kFifoWords;
        --fifo_count_;
        return word;
    }
    default: return 0;  // clear registers read as 0
    }
}

void SystemRegisters::sh2_write(Sh2 cpu, u32 offset, u16 value, u16 lanes) {
    offset &= kSh2Size - 2;
    if (is_comm(offset)) {
        auto& word = comm_[comm_index(offset)];
        word = merge(word, value, lanes);
        return;
    }
    if (is_pwm(offset)) {
        pwm_.write(offset & 0x0E, value, lanes, true);
        return;
    }
    const auto c = index(cpu);
    switch (offset) {
    case 0x00: {
        const u16 v = merge(sh2_read(cpu, offset), value, lanes);
        fm_ = (v & kFm) != 0;       // shared with the 68000's adapter control
        hen_ = (v & kSh2Hen) != 0;
        mask_[c] = static_cast<u8>(v & kMaskBits);
        notify_irq();
        break;
    }
    case 0x02: standby_[c] = merge(standby_[c], value, lanes); break;
    case 0x04: hcount_ = static_cast<u16>(merge(hcount_, value, lanes) & 0xFF); break;
    // Interrupt clear registers: any write clears the writing CPU's source.
    case 0x14: events_[c] &= static_cast<u8>(~bit(Sh2Interrupt::Vres)); notify_irq(); break;
    case 0x16: events_[c] &= static_cast<u8>(~bit(Sh2Interrupt::V));    notify_irq(); break;
    case 0x18: events_[c] &= static_cast<u8>(~bit(Sh2Interrupt::H));    notify_irq(); break;
    case 0x1A: cmd_ &= static_cast<u8>(~(1u << c));                     notify_irq(); break;
    case 0x1C: events_[c] &= static_cast<u8>(~bit(Sh2Interrupt::Pwm));  notify_irq(); break;
    default: break;  // DREQ registers are read-only for the SH-2s
    }
}

// ===========================================================================
// Interrupts
// ===========================================================================

void SystemRegisters::raise_interrupt(Sh2 cpu, Sh2Interrupt source) {
    if (source == Sh2Interrupt::Cmd) {
        cmd_ |= static_cast<u8>(1u << index(cpu));
    } else {
        events_[index(cpu)] |= bit(source);
    }
    notify_irq();
}

bool SystemRegisters::interrupt_pending(Sh2 cpu, Sh2Interrupt source) const noexcept {
    if (source == Sh2Interrupt::Cmd) return (cmd_ >> index(cpu)) & 1;
    return (events_[index(cpu)] & bit(source)) != 0;
}

int SystemRegisters::sh2_irq_level(Sh2 cpu) const noexcept {
    const u8 mask = mask_[index(cpu)];
    const auto enabled = [&](Sh2Interrupt s) { return interrupt_pending(cpu, s) && (mask & bit(s)); };
    // Highest first; VRES has no mask bit.
    if (interrupt_pending(cpu, Sh2Interrupt::Vres)) return level(Sh2Interrupt::Vres);
    for (const auto s : {Sh2Interrupt::V, Sh2Interrupt::H, Sh2Interrupt::Cmd, Sh2Interrupt::Pwm}) {
        if (enabled(s)) return level(s);
    }
    return 0;
}

void SystemRegisters::notify_irq() {
    for (const Sh2 cpu : {Sh2::Master, Sh2::Slave}) {
        const int level = sh2_irq_level(cpu);
        auto& last = irq_level_[index(cpu)];
        if (level == last) continue;
        last = level;
        if (sh2_irq_changed_) sh2_irq_changed_(cpu, level);
    }
}

} // namespace md::sega32x
