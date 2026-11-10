#pragma once

#include <array>
#include <string>

#include "md/types.hpp"

namespace md {

// What an SH-2 sees outside the chip: the external bus (address areas 0 and 1,
// 0x00000000-0x3FFFFFFF). Big-endian; the CPU only issues aligned accesses.
class Sh2MemoryBus {
public:
    virtual ~Sh2MemoryBus() = default;
    virtual u8   read8(u32 address) = 0;
    virtual u16  read16(u32 address) = 0;
    virtual u32  read32(u32 address) = 0;
    virtual void write8(u32 address, u8 value) = 0;
    virtual void write16(u32 address, u16 value) = 0;
    virtual void write32(u32 address, u32 value) = 0;
};

// Hitachi SH-2 (SH7604), an interpreter written for this project.
//
// The CPU: sixteen 32-bit registers, SR (T, S, interrupt mask, Q, M), GBR,
// VBR, MACH/MACL, PR, PC; fixed 16-bit instructions with delayed branches
// (the delay slot executes with its branch, so nothing can interrupt between
// them); exceptions, TRAPA, interrupts through the IRL pins (auto-vectored),
// SLEEP.
//
// Address space (bits 31-29 select the area):
//   0x00000000-0x1FFFFFFF  external bus, through the cache
//   0x20000000-0x3FFFFFFF  external bus, cache-through (same memory)
//   0x40000000-0x5FFFFFFF  associative purge: writes ignored
//   0x60000000-0x7FFFFFFF  cache address array: ignored
//   0xC0000000-0xC0000FFF  cache data array: 4 KB of RAM (usable as scratch
//                          RAM when the cache is off or in two-way mode)
//   0xFFFFFE00-0xFFFFFFFF  on-chip peripherals
// The cache itself isn't modelled: cached reads go to the bus every time.
// That only differs from hardware for software that relies on stale cache
// contents, which well-behaved code avoids by using cache-through addresses
// for shared data.
//
// On-chip peripherals: BCR1's MASTER bit identifies the CPU (it reflects the
// MD5 pin, which the 32X ties low on the master and high on the slave), the
// division unit (DIVU) computes, the free-running timer counts, and the DMA
// controller (DMAC) transfers, and the watchdog timer (WDT) counts and, in
// interval mode, interrupts. The serial port and the other on-chip interrupt
// sources are stored but not active yet.
//
// WDT (0xFFFFFE80): written as words with a key in the high byte, 0xA5 for
// WTCSR (OVF 7 | WT/IT 6 | TME 5 | CKS 2-0: clock / 2, 64, 128, 256, 512,
// 1024, 4096, 8192) and 0x5A for WTCNT; read as bytes (WTCSR at 0x80,
// WTCNT at 0x81). With TME, WTCNT counts up; its overflow sets OVF (cleared
// by writing 0 after reading 1) and, in interval mode (WT/IT = 0), requests
// an interrupt at the WDT level in IPRA (bits 7-4) through VCRWDT (bits
// 14-8). Watchdog mode (a reset on overflow) isn't modelled.
//
// DMAC: two channels (SAR, DAR, TCR, CHCR at 0xFFFFFF80 / 0xFFFFFF90),
// enabled by DMAOR.DME and CHCR.DE. CHCR: DM (15-14) / SM (13-12)
// destination / source fixed, increment or decrement; TS (11-10) unit of 1,
// 2, 4 or 16 bytes; AR (9) auto request; IE (2); TE (1) transfer end, which
// software clears by writing 0; DE (0). An auto-request transfer runs to its
// end at once (no bus timing). Otherwise each external request
// (dma_request(), the DREQ pins) moves one unit. At the end TE is set, and
// with IE the channel interrupts at the DMAC level in IPRA (bits 11-8)
// through its vector in VCRDMA0/1.
//
// Timing: instructions take their documented issue cycles (1 for most, more
// for taken branches, multiplies, MAC, RTE/TRAPA, exceptions); memory wait
// states aren't counted.
class Sh2 {
public:
    struct Registers {
        std::array<u32, 16> r{};
        u32 pc = 0, pr = 0, sr = 0, gbr = 0, vbr = 0, mach = 0, macl = 0;
    };

    // SR bits.
    static constexpr u32 kT = 1u << 0;
    static constexpr u32 kS = 1u << 1;
    static constexpr u32 kImask = 0xF0;
    static constexpr u32 kQ = 1u << 8;
    static constexpr u32 kM = 1u << 9;
    static constexpr u32 kSrMask = 0x3F3;

    // On-chip register addresses.
    static constexpr u32 kOnChipBase = 0xFFFF'FE00;
    static constexpr u32 kTier = 0xFFFF'FE10, kFrcH = 0xFFFF'FE12, kTcr = 0xFFFF'FE16;
    static constexpr u32 kCcr = 0xFFFF'FE92;
    static constexpr u32 kDvsr = 0xFFFF'FF00, kDvdnt = 0xFFFF'FF04, kDvcr = 0xFFFF'FF08;
    static constexpr u32 kDvdnth = 0xFFFF'FF10, kDvdntl = 0xFFFF'FF14;
    static constexpr u32 kBcr1 = 0xFFFF'FFE0;
    static constexpr u32 kSar0 = 0xFFFF'FF80, kDar0 = 0xFFFF'FF84, kTcr0 = 0xFFFF'FF88, kChcr0 = 0xFFFF'FF8C;
    static constexpr u32 kSar1 = 0xFFFF'FF90, kDar1 = 0xFFFF'FF94, kTcr1 = 0xFFFF'FF98, kChcr1 = 0xFFFF'FF9C;
    static constexpr u32 kDmaChannelStride = 0x10;
    static constexpr u32 kVcrDma0 = 0xFFFF'FFA0, kVcrDma1 = 0xFFFF'FFA8;
    static constexpr u32 kDmaor = 0xFFFF'FFB0;
    static constexpr u32 kIpra = 0xFFFF'FEE2;
    static constexpr u32 kWtcsr = 0xFFFF'FE80, kWtcnt = 0xFFFF'FE81;
    static constexpr u32 kVcrWdt = 0xFFFF'FEE4;
    static constexpr u8  kWtcsrOvf = 0x80, kWtcsrWt = 0x40, kWtcsrTme = 0x20;
    // CHCR / DMAOR bits.
    static constexpr u32 kChcrDe = 0x0001, kChcrTe = 0x0002, kChcrIe = 0x0004, kChcrAr = 0x0200;
    static constexpr u32 kDmaorDme = 0x0001, kDmaorNmif = 0x0002, kDmaorAe = 0x0004;
    static constexpr u32 kBcr1Slave = 0x8000;   // BCR1.MASTER: 1 = slave

    static constexpr u32 kDataArrayBase = 0xC000'0000;
    static constexpr u32 kDataArraySize = 4 * 1024;

    // Exception vectors.
    static constexpr u32 kVectorIllegal = 4;
    static constexpr u32 kVectorSlotIllegal = 6;
    // IRL interrupts are auto-vectored: level 15/14 -> 71 ... 1 -> 64.
    [[nodiscard]] static constexpr u32 irl_vector(int level) noexcept { return 64u + static_cast<u32>(level) / 2u; }

    Sh2(Sh2MemoryBus& bus, bool slave);

    // Power-on reset: PC and R15 from vectors 0 and 1 (at address 0, the
    // boot ROM), interrupts masked (level 15), VBR = 0, peripherals reset.
    void reset();

    // Executes one instruction (a delayed branch together with its slot), or
    // takes a pending interrupt. Returns the cycles used.
    int step();

    // Runs until the cycle counter reaches `target` (absolute; overshoot is
    // absorbed by the next call).
    void run_until(s64 target);

    // IRL pins: the interrupt level requested from outside (0 = none). Taken
    // at the next instruction boundary if above the SR mask.
    void set_irl(int level) noexcept { irl_ = level; }
    [[nodiscard]] int irl() const noexcept { return irl_; }

    // DREQ: an external request for one DMA unit on `channel` (0 or 1).
    // Moves it if the channel is enabled and waiting for requests (not
    // auto-request); returns whether it did.
    bool dma_request(int channel);
    [[nodiscard]] bool dma_waiting_for_request(int channel) const noexcept;

    [[nodiscard]] s64  cycles() const noexcept { return cycles_; }
    [[nodiscard]] bool is_slave() const noexcept { return slave_; }
    [[nodiscard]] bool sleeping() const noexcept { return sleeping_; }
    [[nodiscard]] const Registers& registers() const noexcept { return regs_; }
    [[nodiscard]] Registers& registers() noexcept { return regs_; }

    // Memory as this CPU sees it (on-chip areas included), for tests and
    // debuggers. Reads may have side effects on peripherals.
    [[nodiscard]] u8  read8(u32 address);
    [[nodiscard]] u16 read16(u32 address);
    [[nodiscard]] u32 read32(u32 address);
    void write8(u32 address, u8 value);
    void write16(u32 address, u16 value);
    void write32(u32 address, u32 value);

    // Mnemonic of the instruction word, for debugging.
    [[nodiscard]] static std::string disassemble(u16 opcode);

private:
    // --- Execution ---
    void execute(u16 op);
    void branch_delayed(u32 target);
    void exception(u32 vector, u32 return_pc);
    void illegal(u16 op);

    [[nodiscard]] bool t() const noexcept { return regs_.sr & kT; }
    void set_t(bool v) noexcept { regs_.sr = v ? (regs_.sr | kT) : (regs_.sr & ~kT); }
    [[nodiscard]] int imask() const noexcept { return static_cast<int>((regs_.sr >> 4) & 0xF); }

    void div1(u32 n, u32 m);
    void mac_w(u32 n, u32 m);
    void mac_l(u32 n, u32 m);

    // --- On-chip peripherals ---
    [[nodiscard]] u8   onchip_read8(u32 address);
    [[nodiscard]] u16  onchip_read16(u32 address);
    [[nodiscard]] u32  onchip_read32(u32 address);
    void onchip_write8(u32 address, u8 value);
    void onchip_write16(u32 address, u16 value);
    void onchip_write32(u32 address, u32 value);
    void reset_onchip();
    void advance_frt(int cycles);
    void advance_wdt(int cycles);
    void advance_peripherals(int cycles) { advance_frt(cycles); advance_wdt(cycles); }
    void divide32();
    void divide64();
    [[nodiscard]] u32 onchip_reg32(u32 address) const noexcept;
    // DMAC
    void dmac_store(u32 address, u32 value);
    [[nodiscard]] bool dma_enabled(int channel) const noexcept;
    void dma_unit(int channel);
    void dma_auto_requests();
    // Highest on-chip interrupt request (DMA end): level 0 = none.
    struct OnChipIrq { int level = 0; u32 vector = 0; };
    [[nodiscard]] OnChipIrq onchip_irq() const noexcept;
    void set_onchip_reg32(u32 address, u32 value) noexcept;

    Sh2MemoryBus& bus_;
    bool slave_;
    Registers regs_;
    s64 cycles_ = 0;
    int irl_ = 0;
    bool sleeping_ = false;
    bool in_delay_slot_ = false;
    bool slot_exception_ = false;   // the slot instruction raised an exception
    u32  slot_address_ = 0;
    int extra_cycles_ = 0;   // added by the instruction being executed

    std::array<u8, kDataArraySize> data_array_{};
    std::array<u8, 0x200> onchip_{};   // 0xFFFFFE00-0xFFFFFFFF, as stored
    u32 frt_prescale_ = 0;
    u32 wdt_prescale_ = 0;
};

} // namespace md
