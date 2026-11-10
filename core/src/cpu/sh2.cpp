#include "md/sh2.hpp"

#include <cstdio>
#include <limits>

namespace md {

namespace {

constexpr u32 sext8(u32 v) noexcept { return static_cast<u32>(static_cast<s32>(static_cast<s8>(static_cast<u8>(v)))); }
constexpr u32 sext16(u32 v) noexcept { return static_cast<u32>(static_cast<s32>(static_cast<s16>(static_cast<u16>(v)))); }
constexpr u32 sext12(u32 v) noexcept { return (v & 0x800) ? (v | 0xFFFF'F000) : (v & 0x0FFF); }
constexpr s32 as_signed(u32 v) noexcept { return static_cast<s32>(v); }

// On-chip register file offsets.
constexpr u32 onchip_index(u32 address) noexcept { return address - Sh2::kOnChipBase; }

constexpr u32 kFtcsr = 0xFFFF'FE11;
constexpr u8  kFtcsrOvf = 0x02;
constexpr u32 kCcrPurge = 0x10;          // CP: self-clearing
constexpr u32 kDvcrOvf = 0x01;
constexpr u32 kBscKey = 0xA55A;           // BSC registers need it in the high word

// BSC register defaults (BCR1, BCR2, WCR, MCR, RTCSR, RTCNT, RTCOR).
constexpr u32 kBcr1Default = 0x03F0;
constexpr u32 kBcr2Default = 0x00FC;
constexpr u32 kWcrDefault  = 0xAAFF;

// The division unit's registers are mirrored: 0xFFFFFF00-1F at +0x20, and
// DVDNTUH/UL (0x18/0x1C) are the same as DVDNTH/DVDNTL.
constexpr u32 divu_register(u32 address) noexcept {
    u32 a = Sh2::kDvsr + ((address - Sh2::kDvsr) & 0x1F);
    if (a == 0xFFFF'FF18) a = Sh2::kDvdnth;
    if (a == 0xFFFF'FF1C) a = Sh2::kDvdntl;
    return a;
}
constexpr bool is_divu(u32 address) noexcept { return address >= Sh2::kDvsr && address < 0xFFFF'FF40; }
constexpr bool is_bsc(u32 address) noexcept { return address >= Sh2::kBcr1; }
constexpr bool is_dmac(u32 address) noexcept { return address >= Sh2::kSar0 && address < Sh2::kDmaor + 4; }
constexpr u32 dma_reg(int channel, u32 reg0) noexcept { return reg0 + static_cast<u32>(channel) * Sh2::kDmaChannelStride; }

} // namespace

Sh2::Sh2(Sh2MemoryBus& bus, bool slave) : bus_(bus), slave_(slave) { reset(); }

void Sh2::reset() {
    regs_ = {};
    reset_onchip();
    data_array_.fill(0);
    regs_.sr = kImask;   // all interrupts masked
    regs_.vbr = 0;
    regs_.pc = read32(0);
    regs_.r[15] = read32(4);
    sleeping_ = false;
    in_delay_slot_ = false;
    cycles_ += 10;
}

void Sh2::reset_onchip() {
    onchip_.fill(0);
    frt_prescale_ = 0;
    wdt_prescale_ = 0;
    onchip_[onchip_index(kWtcsr)] = 0x18;   // reserved bits read 1
    set_onchip_reg32(kBcr1, kBcr1Default | (slave_ ? kBcr1Slave : 0));
    set_onchip_reg32(0xFFFF'FFE4, kBcr2Default);
    set_onchip_reg32(0xFFFF'FFE8, kWcrDefault);
    onchip_[onchip_index(kTier)] = 0x01;
    onchip_[onchip_index(0xFFFF'FE14)] = 0xFF;   // OCRA/B
    onchip_[onchip_index(0xFFFF'FE15)] = 0xFF;
    onchip_[onchip_index(0xFFFF'FE17)] = 0xE0;   // TOCR
}

// ===========================================================================
// Memory
// ===========================================================================

u8 Sh2::read8(u32 address) {
    switch (address >> 29) {
    case 0: case 1: return bus_.read8(address);
    case 6: return data_array_[address & (kDataArraySize - 1)];
    case 7: return address >= kOnChipBase ? onchip_read8(address) : 0;
    default: return 0;
    }
}

u16 Sh2::read16(u32 address) {
    address &= ~1u;
    switch (address >> 29) {
    case 0: case 1: return bus_.read16(address);
    case 6: {
        const u32 a = address & (kDataArraySize - 1);
        return static_cast<u16>((data_array_[a] << 8) | data_array_[a + 1]);
    }
    case 7: return address >= kOnChipBase ? onchip_read16(address) : 0;
    default: return 0;
    }
}

u32 Sh2::read32(u32 address) {
    address &= ~3u;
    switch (address >> 29) {
    case 0: case 1: return bus_.read32(address);
    case 6: {
        const u32 a = address & (kDataArraySize - 1);
        return (u32{data_array_[a]} << 24) | (u32{data_array_[a + 1]} << 16) |
               (u32{data_array_[a + 2]} << 8) | data_array_[a + 3];
    }
    case 7: return address >= kOnChipBase ? onchip_read32(address) : 0;
    default: return 0;
    }
}

void Sh2::write8(u32 address, u8 value) {
    switch (address >> 29) {
    case 0: case 1: bus_.write8(address, value); break;
    case 6: data_array_[address & (kDataArraySize - 1)] = value; break;
    case 7: if (address >= kOnChipBase) onchip_write8(address, value); break;
    default: break;   // purge / address array
    }
}

void Sh2::write16(u32 address, u16 value) {
    address &= ~1u;
    switch (address >> 29) {
    case 0: case 1: bus_.write16(address, value); break;
    case 6: {
        const u32 a = address & (kDataArraySize - 1);
        data_array_[a] = static_cast<u8>(value >> 8);
        data_array_[a + 1] = static_cast<u8>(value);
        break;
    }
    case 7: if (address >= kOnChipBase) onchip_write16(address, value); break;
    default: break;
    }
}

void Sh2::write32(u32 address, u32 value) {
    address &= ~3u;
    switch (address >> 29) {
    case 0: case 1: bus_.write32(address, value); break;
    case 6: {
        const u32 a = address & (kDataArraySize - 1);
        data_array_[a] = static_cast<u8>(value >> 24);
        data_array_[a + 1] = static_cast<u8>(value >> 16);
        data_array_[a + 2] = static_cast<u8>(value >> 8);
        data_array_[a + 3] = static_cast<u8>(value);
        break;
    }
    case 7: if (address >= kOnChipBase) onchip_write32(address, value); break;
    default: break;
    }
}

// ===========================================================================
// On-chip peripherals
// ===========================================================================

u32 Sh2::onchip_reg32(u32 address) const noexcept {
    const u32 i = onchip_index(address & ~3u);
    return (u32{onchip_[i]} << 24) | (u32{onchip_[i + 1]} << 16) | (u32{onchip_[i + 2]} << 8) | onchip_[i + 3];
}

void Sh2::set_onchip_reg32(u32 address, u32 value) noexcept {
    const u32 i = onchip_index(address & ~3u);
    onchip_[i] = static_cast<u8>(value >> 24);
    onchip_[i + 1] = static_cast<u8>(value >> 16);
    onchip_[i + 2] = static_cast<u8>(value >> 8);
    onchip_[i + 3] = static_cast<u8>(value);
}

u8 Sh2::onchip_read8(u32 address) {
    if (address >= kDvsr) {
        const u32 word = onchip_read32(address & ~3u);
        return static_cast<u8>(word >> (8 * (3 - (address & 3))));
    }
    return onchip_[onchip_index(address)];
}

u16 Sh2::onchip_read16(u32 address) {
    if (address >= kDvsr) {
        const u32 word = onchip_read32(address & ~3u);
        return static_cast<u16>((address & 2) ? word : word >> 16);
    }
    const u32 i = onchip_index(address);
    return static_cast<u16>((onchip_[i] << 8) | onchip_[i + 1]);
}

u32 Sh2::onchip_read32(u32 address) {
    if (address < kDvsr) {
        return (u32{onchip_read16(address)} << 16) | onchip_read16(address + 2);
    }
    if (is_divu(address)) return onchip_reg32(divu_register(address));
    if (is_bsc(address)) return onchip_reg32(address) & 0xFFFF;   // 16-bit registers
    return onchip_reg32(address);
}

void Sh2::onchip_write8(u32 address, u8 value) {
    if (address >= kDvsr) return;   // 32-bit registers: byte writes ignored
    if (address >= kWtcsr && address < kWtcsr + 4) return;   // WDT: keyed word writes only
    u8 v = value;
    if (address == kCcr) v = static_cast<u8>(v & ~kCcrPurge);   // the purge happens at once
    if (address == kFtcsr) {
        // Flags can only be cleared (write 0 after reading 1).
        v = static_cast<u8>((onchip_[onchip_index(address)] & value) | (value & 0x01));
    }
    onchip_[onchip_index(address)] = v;
}

void Sh2::onchip_write16(u32 address, u16 value) {
    if (address == kWtcsr) {
        const u8 low = static_cast<u8>(value);
        if ((value >> 8) == 0xA5) {
            // WTCSR: OVF only clears; bits 4-3 read 1.
            const u8 old = onchip_[onchip_index(kWtcsr)];
            const u8 ovf = static_cast<u8>(old & low & kWtcsrOvf);
            onchip_[onchip_index(kWtcsr)] = static_cast<u8>((low & 0x67) | 0x18 | ovf);
            if (!(low & kWtcsrTme)) wdt_prescale_ = 0;
        } else if ((value >> 8) == 0x5A) {
            onchip_[onchip_index(kWtcnt)] = low;
        }
        return;
    }
    if (address == kWtcsr + 2) {
        // RSTCSR: (0xA5: clear WOVF, 0x5A: RSTE/RSTS) stored as written.
        if ((value >> 8) == 0x5A) onchip_[onchip_index(kWtcsr + 3)] = static_cast<u8>(value & 0x60);
        if ((value >> 8) == 0xA5 && !(value & 0x80)) onchip_[onchip_index(kWtcsr + 3)] &= 0x7F;
        return;
    }
    if (address >= kDvsr) {
        // 16-bit access to a 32-bit register: stores that half (no division
        // is started). The BSC registers only accept keyed 32-bit writes.
        const u32 aligned = address & ~3u;
        u32 word = onchip_reg32(aligned);
        word = (address & 2) ? ((word & 0xFFFF'0000) | value) : ((word & 0xFFFF) | (u32{value} << 16));
        if (is_divu(address)) {
            set_onchip_reg32(divu_register(aligned), word);
        } else if (is_dmac(aligned)) {
            dmac_store(aligned, word);
        } else if (!is_bsc(address)) {
            set_onchip_reg32(aligned, word);
        }
        return;
    }
    onchip_write8(address, static_cast<u8>(value >> 8));
    onchip_write8(address + 1, static_cast<u8>(value));
}

void Sh2::onchip_write32(u32 address, u32 value) {
    if (address < kDvsr) {
        onchip_write16(address, static_cast<u16>(value >> 16));
        onchip_write16(address + 2, static_cast<u16>(value));
        return;
    }
    if (is_divu(address)) {
        const u32 reg = divu_register(address);
        set_onchip_reg32(reg, value);
        if (reg == kDvdnt) {
            divide32();
        } else if (reg == kDvdntl) {
            divide64();
        }
        return;
    }
    if (is_bsc(address)) {
        if ((value >> 16) != kBscKey) return;   // unlocked writes only
        u32 v = value & 0xFFFF;
        if (address == kBcr1) v = (v & ~kBcr1Slave) | (slave_ ? kBcr1Slave : 0);   // MASTER is read-only
        set_onchip_reg32(address, v);
        return;
    }
    if (is_dmac(address)) {
        dmac_store(address, value);
        return;
    }
    set_onchip_reg32(address, value);   // vectors: stored
}

// ===========================================================================
// DMA controller
// ===========================================================================

void Sh2::dmac_store(u32 address, u32 value) {
    const u32 old = onchip_reg32(address);
    const bool chcr = address == kChcr0 || address == dma_reg(1, kChcr0);
    if (chcr) {
        // TE is only cleared (by writing 0 after reading 1), never set.
        value = (value & 0xFFFF & ~kChcrTe) | (old & value & kChcrTe);
    } else if (address == kDmaor) {
        value = (value & 0x000F & ~(kDmaorNmif | kDmaorAe)) | (old & value & (kDmaorNmif | kDmaorAe));
    } else if (address == kTcr0 || address == dma_reg(1, kTcr0)) {
        value &= 0x00FF'FFFF;
    }
    set_onchip_reg32(address, value);
    dma_auto_requests();
}

bool Sh2::dma_enabled(int channel) const noexcept {
    const u32 dmaor = onchip_reg32(kDmaor);
    const u32 chcr = onchip_reg32(dma_reg(channel, kChcr0));
    return (dmaor & (kDmaorDme | kDmaorNmif | kDmaorAe)) == kDmaorDme &&
           (chcr & (kChcrDe | kChcrTe)) == kChcrDe;
}

bool Sh2::dma_waiting_for_request(int channel) const noexcept {
    return dma_enabled(channel) && !(onchip_reg32(dma_reg(channel, kChcr0)) & kChcrAr);
}

void Sh2::dma_auto_requests() {
    for (int ch = 0; ch < 2; ++ch) {
        if (!dma_enabled(ch) || !(onchip_reg32(dma_reg(ch, kChcr0)) & kChcrAr)) continue;
        while (dma_enabled(ch)) dma_unit(ch);
    }
}

bool Sh2::dma_request(int channel) {
    if (!dma_waiting_for_request(channel)) return false;
    dma_unit(channel);
    return true;
}

void Sh2::dma_unit(int channel) {
    const u32 chcr = onchip_reg32(dma_reg(channel, kChcr0));
    const u32 ts = (chcr >> 10) & 3;
    const u32 size = ts == 3 ? 16 : (1u << ts);
    u32 src = onchip_reg32(dma_reg(channel, kSar0));
    u32 dst = onchip_reg32(dma_reg(channel, kDar0));
    u32 count = onchip_reg32(dma_reg(channel, kTcr0));
    if (count == 0) count = 0x0100'0000;   // 0 = 2^24

    if (ts == 3) {
        // 16 bytes as four longs; source and destination advance through
        // them, then apply their mode for the whole unit.
        for (u32 i = 0; i < 16; i += 4) write32(dst + i, read32(src + i));
    } else if (size == 4) {
        write32(dst, read32(src));
    } else if (size == 2) {
        write16(dst, read16(src));
    } else {
        write8(dst, read8(src));
    }
    const auto step = [size](u32 address, u32 mode) {
        if (mode == 1) return address + size;
        if (mode == 2) return address - size;
        return address;   // fixed (3 is reserved)
    };
    src = step(src, (chcr >> 12) & 3);
    dst = step(dst, (chcr >> 14) & 3);
    // TCR counts units, longs in 16-byte mode.
    const u32 used = ts == 3 ? 4u : 1u;
    count = count > used ? count - used : 0;

    set_onchip_reg32(dma_reg(channel, kSar0), src);
    set_onchip_reg32(dma_reg(channel, kDar0), dst);
    set_onchip_reg32(dma_reg(channel, kTcr0), count & 0x00FF'FFFF);
    if (count == 0) {
        set_onchip_reg32(dma_reg(channel, kChcr0), chcr | kChcrTe);
    }
}

Sh2::OnChipIrq Sh2::onchip_irq() const noexcept {
    OnChipIrq best;
    // DMAC: IPRA bits 11-8; channel 0 first.
    const int dma_level = onchip_[onchip_index(kIpra)] & 0x0F;
    if (dma_level > 0) {
        for (int ch = 0; ch < 2; ++ch) {
            const u32 chcr = onchip_reg32(dma_reg(ch, kChcr0));
            if ((chcr & (kChcrTe | kChcrIe)) == (kChcrTe | kChcrIe)) {
                best = {dma_level, onchip_reg32(ch == 0 ? kVcrDma0 : kVcrDma1) & 0x7F};
                break;
            }
        }
    }
    // WDT interval timer: IPRA bits 7-4, VCRWDT bits 14-8.
    const int wdt_level = onchip_[onchip_index(kIpra) + 1] >> 4;
    const u8 csr = onchip_[onchip_index(kWtcsr)];
    if (wdt_level > best.level && (csr & kWtcsrOvf) && !(csr & kWtcsrWt)) {
        best = {wdt_level, u32{onchip_[onchip_index(kVcrWdt)]} & 0x7F};
    }
    return best;
}

void Sh2::divide32() {
    // DVDNT / DVSR, signed: quotient to DVDNT and DVDNTL, remainder to DVDNTH.
    const s32 dividend = as_signed(onchip_reg32(kDvdnt));
    const s32 divisor = as_signed(onchip_reg32(kDvsr));
    if (divisor == 0 || (dividend == std::numeric_limits<s32>::min() && divisor == -1)) {
        set_onchip_reg32(kDvcr, onchip_reg32(kDvcr) | kDvcrOvf);
        const u32 saturated = dividend < 0 ? 0x8000'0000u : 0x7FFF'FFFFu;
        set_onchip_reg32(kDvdnt, saturated);
        set_onchip_reg32(kDvdntl, saturated);
        return;
    }
    const u32 q = static_cast<u32>(dividend / divisor);
    set_onchip_reg32(kDvdnt, q);
    set_onchip_reg32(kDvdntl, q);
    set_onchip_reg32(kDvdnth, static_cast<u32>(dividend % divisor));
}

void Sh2::divide64() {
    // DVDNTH:DVDNTL / DVSR, signed: quotient to DVDNTL (and DVDNT),
    // remainder to DVDNTH. A quotient that doesn't fit in 32 bits sets OVF.
    const s64 dividend = static_cast<s64>((u64{onchip_reg32(kDvdnth)} << 32) | onchip_reg32(kDvdntl));
    const s64 divisor = as_signed(onchip_reg32(kDvsr));
    bool overflow = divisor == 0;
    s64 q = 0, r = 0;
    if (!overflow) {
        q = dividend / divisor;
        r = dividend % divisor;
        overflow = q > std::numeric_limits<s32>::max() || q < std::numeric_limits<s32>::min();
    }
    if (overflow) {
        set_onchip_reg32(kDvcr, onchip_reg32(kDvcr) | kDvcrOvf);
        const bool negative = (dividend < 0) != (divisor < 0);
        const u32 saturated = negative ? 0x8000'0000u : 0x7FFF'FFFFu;
        set_onchip_reg32(kDvdntl, saturated);
        set_onchip_reg32(kDvdnt, saturated);
        return;
    }
    set_onchip_reg32(kDvdntl, static_cast<u32>(q));
    set_onchip_reg32(kDvdnt, static_cast<u32>(q));
    set_onchip_reg32(kDvdnth, static_cast<u32>(r));
}

void Sh2::advance_frt(int cycles) {
    // FRC counts at the clock / 8, 32 or 128 (TCR bits 1-0; 3 = external
    // clock, not connected on the 32X).
    const u8 cks = onchip_[onchip_index(kTcr)] & 3;
    if (cks == 3) return;
    const u32 divider = 8u << (2 * cks);
    frt_prescale_ += static_cast<u32>(cycles);
    const u32 ticks = frt_prescale_ / divider;
    if (ticks == 0) return;
    frt_prescale_ %= divider;
    const u32 i = onchip_index(kFrcH);
    const u32 frc = ((u32{onchip_[i]} << 8) | onchip_[i + 1]) + ticks;
    if (frc > 0xFFFF) onchip_[onchip_index(kFtcsr)] |= kFtcsrOvf;
    onchip_[i] = static_cast<u8>(frc >> 8);
    onchip_[i + 1] = static_cast<u8>(frc);
}

// ===========================================================================
// Execution
// ===========================================================================

void Sh2::advance_wdt(int cycles) {
    u8& csr = onchip_[onchip_index(kWtcsr)];
    if (!(csr & kWtcsrTme)) return;
    static constexpr u32 kDividers[8] = {2, 64, 128, 256, 512, 1024, 4096, 8192};
    const u32 divider = kDividers[csr & 7];
    wdt_prescale_ += static_cast<u32>(cycles);
    const u32 ticks = wdt_prescale_ / divider;
    if (ticks == 0) return;
    wdt_prescale_ %= divider;
    u8& cnt = onchip_[onchip_index(kWtcnt)];
    const u32 next = u32{cnt} + ticks;
    if (next > 0xFF) csr |= kWtcsrOvf;   // interval mode: an interrupt request
    cnt = static_cast<u8>(next);
}

void Sh2::run_until(s64 target) {
    while (cycles_ < target) {
        if (sleeping_ && irl_ <= imask()) {
            // Idle until an interrupt: the timers keep counting, and one of
            // them may wake the CPU.
            advance_peripherals(static_cast<int>(target - cycles_));
            cycles_ = target;
            if (onchip_irq().level > imask()) cycles_ += step();   // woken: take it now
            return;
        }
        const int used = step();
        cycles_ += used;
        advance_peripherals(used);
    }
}

void Sh2::exception(u32 vector, u32 return_pc) {
    regs_.r[15] -= 4;
    write32(regs_.r[15], regs_.sr);
    regs_.r[15] -= 4;
    write32(regs_.r[15], return_pc);
    regs_.pc = read32(regs_.vbr + vector * 4);
}

int Sh2::step() {
    const OnChipIrq onchip = onchip_irq();
    if (irl_ > imask() && irl_ >= onchip.level) {
        // IRL interrupt (wins ties with on-chip sources): the mask rises to
        // the accepted level.
        sleeping_ = false;
        exception(irl_vector(irl_), regs_.pc);
        regs_.sr = (regs_.sr & ~kImask) | (static_cast<u32>(irl_) << 4);
        return 13;
    }
    if (onchip.level > imask()) {
        sleeping_ = false;
        exception(onchip.vector, regs_.pc);
        regs_.sr = (regs_.sr & ~kImask) | (static_cast<u32>(onchip.level) << 4);
        return 13;
    }
    if (sleeping_) return 1;
    extra_cycles_ = 0;
    const u16 op = read16(regs_.pc);
    regs_.pc += 2;
    execute(op);
    return 1 + extra_cycles_;
}

void Sh2::branch_delayed(u32 target) {
    // The slot instruction runs with PC already at the target: a PC-relative
    // load there (MOV.W/MOV.L @(disp,PC), MOVA) addresses from target + 2,
    // not from its own address. Games rely on it (After Burner Complete
    // loads constants placed after the branch target this way).
    const u32 slot = regs_.pc;
    const u16 op = read16(slot);
    regs_.pc = target;
    slot_address_ = slot;
    in_delay_slot_ = true;
    slot_exception_ = false;
    execute(op);
    in_delay_slot_ = false;
    if (!slot_exception_) regs_.pc = target;   // the slot can't branch
    extra_cycles_ += 2;
}

void Sh2::illegal(u16 op) {
    (void)op;
    // The pushed PC is the faulting instruction's address; in a delay slot,
    // the delayed branch's.
    if (in_delay_slot_) {
        slot_exception_ = true;
        exception(kVectorSlotIllegal, slot_address_ - 2);
    } else {
        exception(kVectorIllegal, regs_.pc - 2);
    }
    extra_cycles_ += 7;
}

void Sh2::div1(u32 n, u32 m) {
    u32& rn = regs_.r[n];
    const u32 rm = regs_.r[m];
    const bool old_q = regs_.sr & kQ;
    const bool mbit = regs_.sr & kM;
    bool q = (rn & 0x8000'0000u) != 0;
    rn = (rn << 1) | (t() ? 1u : 0u);
    const u32 before = rn;
    bool carry;
    if (old_q == mbit) {
        rn -= rm;
        carry = rn > before;    // borrow
    } else {
        rn += rm;
        carry = rn < before;    // carry
    }
    // The manual's eight-case table reduces to: Q = Rn's old bit 31 ^ M ^
    // carry/borrow out of the add/subtract.
    q = q != (carry != mbit);
    regs_.sr = q ? (regs_.sr | kQ) : (regs_.sr & ~kQ);
    set_t(q == mbit);
}

void Sh2::mac_w(u32 n, u32 m) {
    const s32 a = static_cast<s16>(read16(regs_.r[n]));
    regs_.r[n] += 2;
    const s32 b = static_cast<s16>(read16(regs_.r[m]));
    regs_.r[m] += 2;
    const s64 product = static_cast<s64>(a) * b;
    if (regs_.sr & kS) {
        // 32-bit saturating accumulate in MACL; MACH bit 0 flags overflow.
        const s64 sum = static_cast<s64>(as_signed(regs_.macl)) + product;
        if (sum > std::numeric_limits<s32>::max()) {
            regs_.macl = 0x7FFF'FFFF;
            regs_.mach |= 1;
        } else if (sum < std::numeric_limits<s32>::min()) {
            regs_.macl = 0x8000'0000;
            regs_.mach |= 1;
        } else {
            regs_.macl = static_cast<u32>(sum);
        }
    } else {
        const u64 mac = ((u64{regs_.mach} << 32) | regs_.macl) + static_cast<u64>(product);
        regs_.mach = static_cast<u32>(mac >> 32);
        regs_.macl = static_cast<u32>(mac);
    }
    extra_cycles_ += 2;
}

void Sh2::mac_l(u32 n, u32 m) {
    const s32 a = as_signed(read32(regs_.r[n]));
    regs_.r[n] += 4;
    const s32 b = as_signed(read32(regs_.r[m]));
    regs_.r[m] += 4;
    const s64 product = static_cast<s64>(a) * b;
    s64 mac = static_cast<s64>((u64{regs_.mach} << 32) | regs_.macl);
    mac = static_cast<s64>(static_cast<u64>(mac) + static_cast<u64>(product));
    if (regs_.sr & kS) {
        // Saturate to 48 bits.
        constexpr s64 kMax = (s64{1} << 47) - 1;
        constexpr s64 kMin = -(s64{1} << 47);
        if (mac > kMax) mac = kMax;
        if (mac < kMin) mac = kMin;
    }
    regs_.mach = static_cast<u32>(static_cast<u64>(mac) >> 32);
    regs_.macl = static_cast<u32>(mac);
    extra_cycles_ += 2;
}

void Sh2::execute(u16 op) {
    auto& r = regs_.r;
    const u32 n = (op >> 8) & 0xF;
    const u32 m = (op >> 4) & 0xF;
    const u32 imm = op & 0xFF;
    const u32 d4 = op & 0xF;
    const u32 pc4 = regs_.pc + 2;   // "PC" in the manual: this instruction + 4

    switch (op >> 12) {
    case 0x0:
        switch (op & 0xF) {
        case 0x2:
            switch (m) {
            case 0: r[n] = regs_.sr; return;
            case 1: r[n] = regs_.gbr; return;
            case 2: r[n] = regs_.vbr; return;
            default: break;
            }
            break;
        case 0x3:
            switch (m) {
            case 0: regs_.pr = pc4; branch_delayed(pc4 + r[n]); return;   // BSRF
            case 2: branch_delayed(pc4 + r[n]); return;                    // BRAF
            default: break;
            }
            break;
        case 0x4: write8(r[0] + r[n], static_cast<u8>(r[m])); return;
        case 0x5: write16(r[0] + r[n], static_cast<u16>(r[m])); return;
        case 0x6: write32(r[0] + r[n], r[m]); return;
        case 0x7: regs_.macl = r[n] * r[m]; extra_cycles_ += 1; return;   // MUL.L
        case 0x8:
            if (n != 0) break;
            switch (m) {
            case 0: set_t(false); return;                                  // CLRT
            case 1: set_t(true); return;                                   // SETT
            case 2: regs_.mach = regs_.macl = 0; return;                   // CLRMAC
            default: break;
            }
            break;
        case 0x9:
            switch (m) {
            case 0: if (n == 0) return; break;                             // NOP
            case 1:                                                        // DIV0U
                if (n == 0) { regs_.sr &= ~(kM | kQ | kT); return; }
                break;
            case 2: r[n] = t() ? 1 : 0; return;                            // MOVT
            default: break;
            }
            break;
        case 0xA:
            switch (m) {
            case 0: r[n] = regs_.mach; return;
            case 1: r[n] = regs_.macl; return;
            case 2: r[n] = regs_.pr; return;
            default: break;
            }
            break;
        case 0xB:
            if (n != 0) break;
            switch (m) {
            case 0: branch_delayed(regs_.pr); return;                      // RTS
            case 1: sleeping_ = true; extra_cycles_ += 2; return;          // SLEEP
            case 2: {                                                      // RTE
                const u32 target = read32(r[15]);
                r[15] += 4;
                regs_.sr = read32(r[15]) & kSrMask;
                r[15] += 4;
                extra_cycles_ += 2;
                branch_delayed(target);
                return;
            }
            default: break;
            }
            break;
        case 0xC: r[n] = sext8(read8(r[0] + r[m])); return;
        case 0xD: r[n] = sext16(read16(r[0] + r[m])); return;
        case 0xE: r[n] = read32(r[0] + r[m]); return;
        case 0xF: mac_l(n, m); return;
        default: break;
        }
        break;

    case 0x1: write32(r[n] + d4 * 4, r[m]); return;

    case 0x2:
        switch (op & 0xF) {
        case 0x0: write8(r[n], static_cast<u8>(r[m])); return;
        case 0x1: write16(r[n], static_cast<u16>(r[m])); return;
        case 0x2: write32(r[n], r[m]); return;
        case 0x4: { const u32 v = r[m]; r[n] -= 1; write8(r[n], static_cast<u8>(v)); return; }
        case 0x5: { const u32 v = r[m]; r[n] -= 2; write16(r[n], static_cast<u16>(v)); return; }
        case 0x6: { const u32 v = r[m]; r[n] -= 4; write32(r[n], v); return; }
        case 0x7: {                                                        // DIV0S
            const bool q = r[n] >> 31;
            const bool mm = r[m] >> 31;
            regs_.sr = (regs_.sr & ~(kQ | kM)) | (q ? kQ : 0) | (mm ? kM : 0);
            set_t(q != mm);
            return;
        }
        case 0x8: set_t((r[n] & r[m]) == 0); return;                       // TST
        case 0x9: r[n] &= r[m]; return;
        case 0xA: r[n] ^= r[m]; return;
        case 0xB: r[n] |= r[m]; return;
        case 0xC: {                                                        // CMP/STR
            const u32 x = r[n] ^ r[m];
            set_t((x & 0xFF00'0000) == 0 || (x & 0x00FF'0000) == 0 || (x & 0x0000'FF00) == 0 || (x & 0xFF) == 0);
            return;
        }
        case 0xD: r[n] = (r[m] << 16) | (r[n] >> 16); return;              // XTRCT
        case 0xE: regs_.macl = (r[n] & 0xFFFF) * (r[m] & 0xFFFF); return;  // MULU.W
        case 0xF:                                                          // MULS.W
            regs_.macl = static_cast<u32>(static_cast<s32>(static_cast<s16>(r[n])) * static_cast<s16>(r[m]));
            return;
        default: break;
        }
        break;

    case 0x3:
        switch (op & 0xF) {
        case 0x0: set_t(r[n] == r[m]); return;                             // CMP/EQ
        case 0x2: set_t(r[n] >= r[m]); return;                             // CMP/HS
        case 0x3: set_t(as_signed(r[n]) >= as_signed(r[m])); return;       // CMP/GE
        case 0x4: div1(n, m); return;
        case 0x5: {                                                        // DMULU.L
            const u64 p = u64{r[n]} * r[m];
            regs_.mach = static_cast<u32>(p >> 32);
            regs_.macl = static_cast<u32>(p);
            extra_cycles_ += 1;
            return;
        }
        case 0x6: set_t(r[n] > r[m]); return;                              // CMP/HI
        case 0x7: set_t(as_signed(r[n]) > as_signed(r[m])); return;        // CMP/GT
        case 0x8: r[n] -= r[m]; return;                                    // SUB
        case 0xA: {                                                        // SUBC
            const u32 a = r[n];
            const u32 d = a - r[m];
            r[n] = d - (t() ? 1u : 0u);
            set_t(a < d || d < r[n]);
            return;
        }
        case 0xB: {                                                        // SUBV
            const s64 d = static_cast<s64>(as_signed(r[n])) - as_signed(r[m]);
            r[n] = static_cast<u32>(d);
            set_t(d > std::numeric_limits<s32>::max() || d < std::numeric_limits<s32>::min());
            return;
        }
        case 0xC: r[n] += r[m]; return;                                    // ADD
        case 0xD: {                                                        // DMULS.L
            const s64 p = static_cast<s64>(as_signed(r[n])) * as_signed(r[m]);
            regs_.mach = static_cast<u32>(static_cast<u64>(p) >> 32);
            regs_.macl = static_cast<u32>(p);
            extra_cycles_ += 1;
            return;
        }
        case 0xE: {                                                        // ADDC
            const u32 a = r[n];
            const u32 s = a + r[m];
            r[n] = s + (t() ? 1u : 0u);
            set_t(a > s || s > r[n]);
            return;
        }
        case 0xF: {                                                        // ADDV
            const s64 s = static_cast<s64>(as_signed(r[n])) + as_signed(r[m]);
            r[n] = static_cast<u32>(s);
            set_t(s > std::numeric_limits<s32>::max() || s < std::numeric_limits<s32>::min());
            return;
        }
        default: break;
        }
        break;

    case 0x4:
        switch (op & 0xFF) {
        case 0x00: case 0x20: set_t(r[n] >> 31); r[n] <<= 1; return;       // SHLL, SHAL
        case 0x10: r[n] -= 1; set_t(r[n] == 0); return;                    // DT
        case 0x01: set_t(r[n] & 1); r[n] >>= 1; return;                    // SHLR
        case 0x11: set_t(as_signed(r[n]) >= 0); return;                    // CMP/PZ
        case 0x21: set_t(r[n] & 1); r[n] = static_cast<u32>(as_signed(r[n]) >> 1); return;  // SHAR
        case 0x02: r[n] -= 4; write32(r[n], regs_.mach); return;
        case 0x12: r[n] -= 4; write32(r[n], regs_.macl); return;
        case 0x22: r[n] -= 4; write32(r[n], regs_.pr); return;
        case 0x03: r[n] -= 4; write32(r[n], regs_.sr); extra_cycles_ += 1; return;
        case 0x13: r[n] -= 4; write32(r[n], regs_.gbr); extra_cycles_ += 1; return;
        case 0x23: r[n] -= 4; write32(r[n], regs_.vbr); extra_cycles_ += 1; return;
        case 0x04: set_t(r[n] >> 31); r[n] = (r[n] << 1) | (r[n] >> 31); return;  // ROTL
        case 0x24: {                                                       // ROTCL
            const bool out = r[n] >> 31;
            r[n] = (r[n] << 1) | (t() ? 1u : 0u);
            set_t(out);
            return;
        }
        case 0x05: set_t(r[n] & 1); r[n] = (r[n] >> 1) | (r[n] << 31); return;    // ROTR
        case 0x15: set_t(as_signed(r[n]) > 0); return;                     // CMP/PL
        case 0x25: {                                                       // ROTCR
            const bool out = r[n] & 1;
            r[n] = (r[n] >> 1) | (t() ? 0x8000'0000u : 0u);
            set_t(out);
            return;
        }
        case 0x06: regs_.mach = read32(r[n]); r[n] += 4; return;
        case 0x16: regs_.macl = read32(r[n]); r[n] += 4; return;
        case 0x26: regs_.pr = read32(r[n]); r[n] += 4; return;
        case 0x07: regs_.sr = read32(r[n]) & kSrMask; r[n] += 4; extra_cycles_ += 2; return;
        case 0x17: regs_.gbr = read32(r[n]); r[n] += 4; extra_cycles_ += 2; return;
        case 0x27: regs_.vbr = read32(r[n]); r[n] += 4; extra_cycles_ += 2; return;
        case 0x08: r[n] <<= 2; return;
        case 0x18: r[n] <<= 8; return;
        case 0x28: r[n] <<= 16; return;
        case 0x09: r[n] >>= 2; return;
        case 0x19: r[n] >>= 8; return;
        case 0x29: r[n] >>= 16; return;
        case 0x0A: regs_.mach = r[n]; return;
        case 0x1A: regs_.macl = r[n]; return;
        case 0x2A: regs_.pr = r[n]; return;
        case 0x0B: {                                                       // JSR
            const u32 target = r[n];
            regs_.pr = pc4;
            branch_delayed(target);
            return;
        }
        case 0x1B: {                                                       // TAS.B
            const u8 v = read8(r[n]);
            set_t(v == 0);
            write8(r[n], static_cast<u8>(v | 0x80));
            extra_cycles_ += 3;
            return;
        }
        case 0x2B: branch_delayed(r[n]); return;                           // JMP
        case 0x0E: regs_.sr = r[n] & kSrMask; return;
        case 0x1E: regs_.gbr = r[n]; return;
        case 0x2E: regs_.vbr = r[n]; return;
        default:
            if ((op & 0xF) == 0xF) { mac_w(n, m); return; }
            break;
        }
        break;

    case 0x5: r[n] = read32(r[m] + d4 * 4); return;

    case 0x6:
        switch (op & 0xF) {
        case 0x0: r[n] = sext8(read8(r[m])); return;
        case 0x1: r[n] = sext16(read16(r[m])); return;
        case 0x2: r[n] = read32(r[m]); return;
        case 0x3: r[n] = r[m]; return;
        case 0x4: { const u32 v = sext8(read8(r[m])); if (n != m) r[m] += 1; r[n] = v; return; }
        case 0x5: { const u32 v = sext16(read16(r[m])); if (n != m) r[m] += 2; r[n] = v; return; }
        case 0x6: { const u32 v = read32(r[m]); if (n != m) r[m] += 4; r[n] = v; return; }
        case 0x7: r[n] = ~r[m]; return;
        case 0x8: r[n] = (r[m] & 0xFFFF'0000) | ((r[m] & 0xFF) << 8) | ((r[m] >> 8) & 0xFF); return;  // SWAP.B
        case 0x9: r[n] = (r[m] << 16) | (r[m] >> 16); return;              // SWAP.W
        case 0xA: {                                                        // NEGC
            const u32 neg = 0u - r[m];
            r[n] = neg - (t() ? 1u : 0u);
            set_t(neg != 0 || r[n] > neg);
            return;
        }
        case 0xB: r[n] = 0u - r[m]; return;
        case 0xC: r[n] = r[m] & 0xFF; return;
        case 0xD: r[n] = r[m] & 0xFFFF; return;
        case 0xE: r[n] = sext8(r[m]); return;
        case 0xF: r[n] = sext16(r[m]); return;
        default: break;
        }
        break;

    case 0x7: r[n] += sext8(imm); return;                                  // ADD #imm

    case 0x8:
        switch (n) {
        case 0x0: write8(r[m] + d4, static_cast<u8>(r[0])); return;
        case 0x1: write16(r[m] + d4 * 2, static_cast<u16>(r[0])); return;
        case 0x4: r[0] = sext8(read8(r[m] + d4)); return;
        case 0x5: r[0] = sext16(read16(r[m] + d4 * 2)); return;
        case 0x8: set_t(r[0] == sext8(imm)); return;                       // CMP/EQ #imm
        case 0x9: if (t()) { regs_.pc = pc4 + sext8(imm) * 2; extra_cycles_ += 2; } return;   // BT
        case 0xB: if (!t()) { regs_.pc = pc4 + sext8(imm) * 2; extra_cycles_ += 2; } return;  // BF
        case 0xD: if (t()) branch_delayed(pc4 + sext8(imm) * 2); return;   // BT/S
        case 0xF: if (!t()) branch_delayed(pc4 + sext8(imm) * 2); return;  // BF/S
        default: break;
        }
        break;

    case 0x9: r[n] = sext16(read16(pc4 + imm * 2)); return;                // MOV.W @(disp,PC)

    case 0xA: branch_delayed(pc4 + sext12(op & 0xFFF) * 2); return;        // BRA

    case 0xB: {                                                            // BSR
        regs_.pr = pc4;
        branch_delayed(pc4 + sext12(op & 0xFFF) * 2);
        return;
    }

    case 0xC:
        switch (n) {
        case 0x0: write8(regs_.gbr + imm, static_cast<u8>(r[0])); return;
        case 0x1: write16(regs_.gbr + imm * 2, static_cast<u16>(r[0])); return;
        case 0x2: write32(regs_.gbr + imm * 4, r[0]); return;
        case 0x3:                                                          // TRAPA
            exception(imm, regs_.pc);
            extra_cycles_ += 7;
            return;
        case 0x4: r[0] = sext8(read8(regs_.gbr + imm)); return;
        case 0x5: r[0] = sext16(read16(regs_.gbr + imm * 2)); return;
        case 0x6: r[0] = read32(regs_.gbr + imm * 4); return;
        case 0x7: r[0] = (pc4 & ~3u) + imm * 4; return;                    // MOVA
        case 0x8: set_t((r[0] & imm) == 0); return;
        case 0x9: r[0] &= imm; return;
        case 0xA: r[0] ^= imm; return;
        case 0xB: r[0] |= imm; return;
        case 0xC: set_t((read8(regs_.gbr + r[0]) & imm) == 0); extra_cycles_ += 2; return;
        case 0xD: { const u32 a = regs_.gbr + r[0]; write8(a, static_cast<u8>(read8(a) & imm)); extra_cycles_ += 2; return; }
        case 0xE: { const u32 a = regs_.gbr + r[0]; write8(a, static_cast<u8>(read8(a) ^ imm)); extra_cycles_ += 2; return; }
        case 0xF: { const u32 a = regs_.gbr + r[0]; write8(a, static_cast<u8>(read8(a) | imm)); extra_cycles_ += 2; return; }
        default: break;
        }
        break;

    case 0xD: r[n] = read32((pc4 & ~3u) + imm * 4); return;                // MOV.L @(disp,PC)

    case 0xE: r[n] = sext8(imm); return;                                   // MOV #imm

    default: break;
    }
    illegal(op);
}

// ===========================================================================
// Disassembler
// ===========================================================================

std::string Sh2::disassemble(u16 op) {
    struct Entry { u16 mask, match; const char* format; };
    // Operand tokens: N = Rn, M = Rm, I = signed imm8, U = unsigned imm8,
    // D = disp4, E = disp8, F = disp12.
    static constexpr Entry kTable[] = {
        {0xFFFF, 0x0009, "nop"}, {0xFFFF, 0x000B, "rts"}, {0xFFFF, 0x002B, "rte"},
        {0xFFFF, 0x001B, "sleep"}, {0xFFFF, 0x0008, "clrt"}, {0xFFFF, 0x0018, "sett"},
        {0xFFFF, 0x0028, "clrmac"}, {0xFFFF, 0x0019, "div0u"},
        {0xF0FF, 0x0002, "stc sr,N"}, {0xF0FF, 0x0012, "stc gbr,N"}, {0xF0FF, 0x0022, "stc vbr,N"},
        {0xF0FF, 0x0003, "bsrf N"}, {0xF0FF, 0x0023, "braf N"}, {0xF0FF, 0x0029, "movt N"},
        {0xF0FF, 0x000A, "sts mach,N"}, {0xF0FF, 0x001A, "sts macl,N"}, {0xF0FF, 0x002A, "sts pr,N"},
        {0xF00F, 0x0004, "mov.b M,@(r0,N)"}, {0xF00F, 0x0005, "mov.w M,@(r0,N)"}, {0xF00F, 0x0006, "mov.l M,@(r0,N)"},
        {0xF00F, 0x0007, "mul.l M,N"}, {0xF00F, 0x000C, "mov.b @(r0,M),N"}, {0xF00F, 0x000D, "mov.w @(r0,M),N"},
        {0xF00F, 0x000E, "mov.l @(r0,M),N"}, {0xF00F, 0x000F, "mac.l @M+,@N+"},
        {0xF000, 0x1000, "mov.l M,@(D,N)"},
        {0xF00F, 0x2000, "mov.b M,@N"}, {0xF00F, 0x2001, "mov.w M,@N"}, {0xF00F, 0x2002, "mov.l M,@N"},
        {0xF00F, 0x2004, "mov.b M,@-N"}, {0xF00F, 0x2005, "mov.w M,@-N"}, {0xF00F, 0x2006, "mov.l M,@-N"},
        {0xF00F, 0x2007, "div0s M,N"}, {0xF00F, 0x2008, "tst M,N"}, {0xF00F, 0x2009, "and M,N"},
        {0xF00F, 0x200A, "xor M,N"}, {0xF00F, 0x200B, "or M,N"}, {0xF00F, 0x200C, "cmp/str M,N"},
        {0xF00F, 0x200D, "xtrct M,N"}, {0xF00F, 0x200E, "mulu.w M,N"}, {0xF00F, 0x200F, "muls.w M,N"},
        {0xF00F, 0x3000, "cmp/eq M,N"}, {0xF00F, 0x3002, "cmp/hs M,N"}, {0xF00F, 0x3003, "cmp/ge M,N"},
        {0xF00F, 0x3004, "div1 M,N"}, {0xF00F, 0x3005, "dmulu.l M,N"}, {0xF00F, 0x3006, "cmp/hi M,N"},
        {0xF00F, 0x3007, "cmp/gt M,N"}, {0xF00F, 0x3008, "sub M,N"}, {0xF00F, 0x300A, "subc M,N"},
        {0xF00F, 0x300B, "subv M,N"}, {0xF00F, 0x300C, "add M,N"}, {0xF00F, 0x300D, "dmuls.l M,N"},
        {0xF00F, 0x300E, "addc M,N"}, {0xF00F, 0x300F, "addv M,N"},
        {0xF0FF, 0x4000, "shll N"}, {0xF0FF, 0x4010, "dt N"}, {0xF0FF, 0x4020, "shal N"},
        {0xF0FF, 0x4001, "shlr N"}, {0xF0FF, 0x4011, "cmp/pz N"}, {0xF0FF, 0x4021, "shar N"},
        {0xF0FF, 0x4002, "sts.l mach,@-N"}, {0xF0FF, 0x4012, "sts.l macl,@-N"}, {0xF0FF, 0x4022, "sts.l pr,@-N"},
        {0xF0FF, 0x4003, "stc.l sr,@-N"}, {0xF0FF, 0x4013, "stc.l gbr,@-N"}, {0xF0FF, 0x4023, "stc.l vbr,@-N"},
        {0xF0FF, 0x4004, "rotl N"}, {0xF0FF, 0x4024, "rotcl N"}, {0xF0FF, 0x4005, "rotr N"},
        {0xF0FF, 0x4015, "cmp/pl N"}, {0xF0FF, 0x4025, "rotcr N"},
        {0xF0FF, 0x4006, "lds.l @N+,mach"}, {0xF0FF, 0x4016, "lds.l @N+,macl"}, {0xF0FF, 0x4026, "lds.l @N+,pr"},
        {0xF0FF, 0x4007, "ldc.l @N+,sr"}, {0xF0FF, 0x4017, "ldc.l @N+,gbr"}, {0xF0FF, 0x4027, "ldc.l @N+,vbr"},
        {0xF0FF, 0x4008, "shll2 N"}, {0xF0FF, 0x4018, "shll8 N"}, {0xF0FF, 0x4028, "shll16 N"},
        {0xF0FF, 0x4009, "shlr2 N"}, {0xF0FF, 0x4019, "shlr8 N"}, {0xF0FF, 0x4029, "shlr16 N"},
        {0xF0FF, 0x400A, "lds N,mach"}, {0xF0FF, 0x401A, "lds N,macl"}, {0xF0FF, 0x402A, "lds N,pr"},
        {0xF0FF, 0x400B, "jsr @N"}, {0xF0FF, 0x401B, "tas.b @N"}, {0xF0FF, 0x402B, "jmp @N"},
        {0xF0FF, 0x400E, "ldc N,sr"}, {0xF0FF, 0x401E, "ldc N,gbr"}, {0xF0FF, 0x402E, "ldc N,vbr"},
        {0xF00F, 0x400F, "mac.w @M+,@N+"},
        {0xF000, 0x5000, "mov.l @(D,M),N"},
        {0xF00F, 0x6000, "mov.b @M,N"}, {0xF00F, 0x6001, "mov.w @M,N"}, {0xF00F, 0x6002, "mov.l @M,N"},
        {0xF00F, 0x6003, "mov M,N"}, {0xF00F, 0x6004, "mov.b @M+,N"}, {0xF00F, 0x6005, "mov.w @M+,N"},
        {0xF00F, 0x6006, "mov.l @M+,N"}, {0xF00F, 0x6007, "not M,N"}, {0xF00F, 0x6008, "swap.b M,N"},
        {0xF00F, 0x6009, "swap.w M,N"}, {0xF00F, 0x600A, "negc M,N"}, {0xF00F, 0x600B, "neg M,N"},
        {0xF00F, 0x600C, "extu.b M,N"}, {0xF00F, 0x600D, "extu.w M,N"}, {0xF00F, 0x600E, "exts.b M,N"},
        {0xF00F, 0x600F, "exts.w M,N"},
        {0xF000, 0x7000, "add I,N"},
        {0xFF00, 0x8000, "mov.b r0,@(D,M)"}, {0xFF00, 0x8100, "mov.w r0,@(D,M)"},
        {0xFF00, 0x8400, "mov.b @(D,M),r0"}, {0xFF00, 0x8500, "mov.w @(D,M),r0"},
        {0xFF00, 0x8800, "cmp/eq I,r0"}, {0xFF00, 0x8900, "bt E"}, {0xFF00, 0x8B00, "bf E"},
        {0xFF00, 0x8D00, "bt/s E"}, {0xFF00, 0x8F00, "bf/s E"},
        {0xF000, 0x9000, "mov.w @(E,pc),N"}, {0xF000, 0xA000, "bra F"}, {0xF000, 0xB000, "bsr F"},
        {0xFF00, 0xC000, "mov.b r0,@(E,gbr)"}, {0xFF00, 0xC100, "mov.w r0,@(E,gbr)"},
        {0xFF00, 0xC200, "mov.l r0,@(E,gbr)"}, {0xFF00, 0xC300, "trapa U"},
        {0xFF00, 0xC400, "mov.b @(E,gbr),r0"}, {0xFF00, 0xC500, "mov.w @(E,gbr),r0"},
        {0xFF00, 0xC600, "mov.l @(E,gbr),r0"}, {0xFF00, 0xC700, "mova @(E,pc),r0"},
        {0xFF00, 0xC800, "tst U,r0"}, {0xFF00, 0xC900, "and U,r0"}, {0xFF00, 0xCA00, "xor U,r0"},
        {0xFF00, 0xCB00, "or U,r0"}, {0xFF00, 0xCC00, "tst.b U,@(r0,gbr)"}, {0xFF00, 0xCD00, "and.b U,@(r0,gbr)"},
        {0xFF00, 0xCE00, "xor.b U,@(r0,gbr)"}, {0xFF00, 0xCF00, "or.b U,@(r0,gbr)"},
        {0xF000, 0xD000, "mov.l @(E,pc),N"}, {0xF000, 0xE000, "mov I,N"},
    };
    for (const auto& e : kTable) {
        if ((op & e.mask) != e.match) continue;
        std::string out;
        char buf[16];
        for (const char* p = e.format; *p; ++p) {
            switch (*p) {
            case 'N': std::snprintf(buf, sizeof buf, "r%u", (op >> 8) & 0xFu); out += buf; break;
            case 'M': std::snprintf(buf, sizeof buf, "r%u", (op >> 4) & 0xFu); out += buf; break;
            case 'I': std::snprintf(buf, sizeof buf, "#%d", static_cast<int>(static_cast<s8>(op & 0xFF))); out += buf; break;
            case 'U': std::snprintf(buf, sizeof buf, "#$%02x", op & 0xFFu); out += buf; break;
            case 'D': std::snprintf(buf, sizeof buf, "$%x", op & 0xFu); out += buf; break;
            case 'E': std::snprintf(buf, sizeof buf, "$%02x", op & 0xFFu); out += buf; break;
            case 'F': std::snprintf(buf, sizeof buf, "$%03x", op & 0xFFFu); out += buf; break;
            default: out += *p; break;
            }
        }
        return out;
    }
    char buf[24];
    std::snprintf(buf, sizeof buf, ".word $%04x", op);
    return buf;
}

} // namespace md
