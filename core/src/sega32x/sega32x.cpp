#include "md/sega32x/sega32x.hpp"

#include <algorithm>

namespace md::sega32x {

namespace {

constexpr u32 kAddressMask = 0xFF'FFFF;
constexpr u32 kMarsId      = 0xA1'30EC;
constexpr u32 kVdpRegs     = 0xA1'5180;
constexpr u32 kPalette     = 0xA1'5200;
constexpr u32 kPaletteEnd  = 0xA1'5400;
constexpr u32 kFrameBuffer = 0x84'0000;
constexpr u32 kAreaEnd     = 0xA0'0000;
constexpr u32 kVectorPageSize = 0x100;
constexpr u32 kHintVector  = 0x70;     // 68000 level 4 (H interrupt)

// Cartridge header fields the boot reads (32X header, "MARS CHECK MODE").
constexpr u32 kHeaderIdlSource      = 0x3D4;   // SH-2 program in the ROM
constexpr u32 kHeaderIdlDestination = 0x3D8;   // ... its offset in SDRAM
constexpr u32 kHeaderIdlSize        = 0x3DC;
constexpr u32 kHeaderMasterStart    = 0x3E0;
constexpr u32 kHeaderSlaveStart     = 0x3E4;
constexpr u32 kHeaderMasterVbr      = 0x3E8;
constexpr u32 kHeaderSlaveVbr       = 0x3EC;
constexpr u32 kHeaderChecksum       = 0x18E;   // Mega Drive header checksum
constexpr u32 kSh2SystemRegisters   = 0x2000'4000;

constexpr u16 byte_lanes(u32 address) { return (address & 1) ? 0x00FF : 0xFF00; }
constexpr u16 byte_to_lanes(u32 address, u8 value) {
    return (address & 1) ? u16{value} : static_cast<u16>(value << 8);
}
constexpr u8 lane_byte(u32 address, u16 word) {
    return static_cast<u8>((address & 1) ? (word & 0xFF) : (word >> 8));
}

constexpr bool is_registers(u32 address) noexcept {
    return (address & 0xFF'FF80) == SystemRegisters::kMainBase;
}

// SH-2 physical address within the cached / cache-through areas.
constexpr u32 sh2_physical(u32 address) noexcept { return address & 0x1FFF'FFFF; }

} // namespace

Sega32x::Sega32x() {
    registers_.set_sh2_irq_callback([this](Sh2 cpu, int level) { sh2(cpu).set_irl(level); });
    registers_.pwm().set_dreq_callback([this] { pwm_dreq(); });
    vdp_.set_vint_callback([this] {
        registers_.raise_interrupt(Sh2::Master, Sh2Interrupt::V);
        registers_.raise_interrupt(Sh2::Slave, Sh2Interrupt::V);
    });
    reset();
}

void Sega32x::reset() {
    registers_.reset();
    build_vector_page();
    sdram_->reset();
    vdp_.reset();
    sh2_time_ = 0;
    h_counter_ = 0;
    sh2_in_reset_ = true;
}

void Sega32x::set_cartridge(std::span<const u8> rom) noexcept {
    rom_ = rom;
    registers_.set_cartridge_present(!rom.empty());
}

void Sega32x::load_boot_roms(std::span<const u8> master, std::span<const u8> slave) {
    master_boot_rom_.assign(master.begin(), master.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(master.size(), kMasterBootRomSize)));
    slave_boot_rom_.assign(slave.begin(), slave.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(slave.size(), kSlaveBootRomSize)));
}

// The 32X's 68000 vector page. Every exception vector points into the jump
// table that 32X cartridges keep at ROM 0x200 (6-byte JMPs, seen at
// 0x880200 in 32X mode); the initial SSP and PC are unused, the 68000 having
// started from the cartridge's own vectors before 32X mode was entered.
// 0xC0-0xFF holds the BIOS's helper routine for leaving RV mode.
void Sega32x::build_vector_page() {
    vector_page_.fill(0);
    const auto put16 = [&](u32 at, u16 v) {
        vector_page_[at] = static_cast<u8>(v >> 8);
        vector_page_[at + 1] = static_cast<u8>(v);
    };
    for (u32 vector = 1; vector < 0xC0 / 4; ++vector) {
        const u32 target = kRomFixedBase + 0x200 + (vector - 1) * 6;
        put16(vector * 4, static_cast<u16>(target >> 16));
        put16(vector * 4 + 2, static_cast<u16>(target));
    }
    for (u32 at = 0xC0; at < kVectorPageSize; at += 2) put16(at, 0x4E71);   // nop
    put16(0xC8, 0x1280);                                                    // move.b d0,(a1)
    put16(0xCA, 0x0239); put16(0xCC, 0x00FE);                               // andi.b #$FE,($A15107).l
    put16(0xCE, 0x00A1); put16(0xD0, 0x5107);                               //   (RV off)
    put16(0xD2, 0x4E75);                                                    // rts
    put16(0xFE, 0x4E75);                                                    // rts
}

void Sega32x::hle_boot(Sh2 cpu) {
    auto& sh2 = this->sh2(cpu);
    const auto header32 = [&](u32 at) {
        return (u32{rom_word(at)} << 16) | rom_word(at + 2);
    };
    const bool master = cpu == Sh2::Master;
    if (master && rom_.size() > 0x400) {
        // Initial data load: the SH-2 program, copied into SDRAM.
        const u32 source = header32(kHeaderIdlSource) & 0x0FFF'FFFF;
        const u32 destination = header32(kHeaderIdlDestination) & 0x0FFF'FFFF;
        const u32 size = header32(kHeaderIdlSize);
        if (size <= Sdram::kSize && destination <= Sdram::kSize - size && source < rom_.size()) {
            const u32 available = std::min<u32>(size, static_cast<u32>(rom_.size()) - source);
            for (u32 i = 0; i < available; ++i) sdram_->write8(destination + i, rom_[source + i]);
        }
        // The BIOS leaves the header checksum in COMM4 for the program.
        registers_.main_write(0x28, rom_word(kHeaderChecksum), 0xFFFF);
    }
    // Handshake: each BIOS reports "M_OK" (COMM0-1) / "S_OK" (COMM2-3); the
    // 68000 start-up code waits for both, finishes its own setup, then
    // clears them, which is what the SH-2 programs wait for before starting.
    const u32 ok_offset = master ? 0x20 : 0x24;
    registers_.main_write(ok_offset, master ? u16{('M' << 8) | '_'} : u16{('S' << 8) | '_'}, 0xFFFF);
    registers_.main_write(ok_offset + 2, u16{('O' << 8) | 'K'}, 0xFFFF);
    auto& r = sh2.registers();
    r.vbr = header32(master ? kHeaderMasterVbr : kHeaderSlaveVbr);
    r.gbr = kSh2SystemRegisters;
    r.pc = header32(master ? kHeaderMasterStart : kHeaderSlaveStart);
    r.r[15] = sh2.read32(r.vbr + 4);   // the program's own power-on SP vector
}

u8 Sega32x::rom_byte(u32 offset) const noexcept {
    return offset < rom_.size() ? rom_[offset] : kOpenBusByte;
}

u16 Sega32x::rom_word(u32 offset) const noexcept {
    offset &= ~1u;
    return static_cast<u16>((rom_byte(offset) << 8) | rom_byte(offset + 1));
}

// ===========================================================================
// Scheduling
// ===========================================================================

void Sega32x::set_vblank(bool active) {
    if (active && !vdp_.in_vblank()) h_counter_ = registers_.h_count();
    vdp_.set_vblank(active);
}

void Sega32x::set_hblank(bool active) {
    vdp_.set_hblank(active);
    if (!active) return;
    if (vdp_.in_vblank() && !registers_.h_interrupt_in_vblank()) return;
    if (h_counter_ > 0) {
        --h_counter_;
        return;
    }
    h_counter_ = registers_.h_count();
    registers_.raise_interrupt(Sh2::Master, Sh2Interrupt::H);
    registers_.raise_interrupt(Sh2::Slave, Sh2Interrupt::H);
}

void Sega32x::run_until(s64 sh2_time) {
    if (sh2_time <= sh2_time_) return;
    // PWM runs on the SH-2 clock whether or not the CPUs do.
    registers_.pwm().advance(sh2_time - sh2_time_);
    if (registers_.sh2_reset_asserted()) {
        sh2_in_reset_ = true;
        sh2_time_ = sh2_time;
        return;
    }
    if (sh2_in_reset_) {
        // RES released: both CPUs fetch their vectors from their boot ROMs.
        sh2_in_reset_ = false;
        master_->reset();
        slave_->reset();
        if (master_boot_rom_.empty()) hle_boot(Sh2::Master);
        if (slave_boot_rom_.empty()) hle_boot(Sh2::Slave);
        master_->set_irl(registers_.sh2_irq_level(Sh2::Master));
        slave_->set_irl(registers_.sh2_irq_level(Sh2::Slave));
        master_alignment_ = master_->cycles() - sh2_time_;
        slave_alignment_ = slave_->cycles() - sh2_time_;
    }
    // Each CPU's counter tracks the shared timeline at a fixed offset, so an
    // instruction that overshoots one slice is absorbed by the next.
    master_->run_until(sh2_time + master_alignment_);
    slave_->run_until(sh2_time + slave_alignment_);
    sh2_time_ = sh2_time;
    service_fifo_dreq();
}

void Sega32x::service_fifo_dreq() {
    if (sh2_in_reset_) return;
    // Bounded: a channel whose source isn't the FIFO would never drain it.
    for (std::size_t budget = SystemRegisters::kFifoWords * 2; budget > 0 && registers_.fifo_size() > 0; --budget) {
        if (!master_->dma_request(0) && !slave_->dma_request(0)) return;
    }
}

void Sega32x::pwm_dreq() {
    if (sh2_in_reset_ || registers_.sh2_reset_asserted()) return;
    if (!master_->dma_request(1)) slave_->dma_request(1);
}

// ===========================================================================
// 68000 side
// ===========================================================================

bool Sega32x::claims(u32 address) const noexcept {
    address &= kAddressMask;
    if (address < kVectorPageSize) return vector_page_visible();
    if ((address & 0xFF'FFFC) == kMarsId) return true;
    if (address >= SystemRegisters::kMainBase && address < kPaletteEnd) return true;  // registers, VDP, palette
    return address >= kFrameBuffer && address < kAreaEnd;
}

u32 Sega32x::main_rom_offset(u32 address) const noexcept {
    if (address < kRomBankBase) return address - kRomFixedBase;
    return u32{registers_.rom_bank()} * kRomBankSize + (address - kRomBankBase);
}

u16 Sega32x::main_read16(u32 address) {
    if (address < kVectorPageSize) return static_cast<u16>((vector_page_[address] << 8) | vector_page_[address + 1]);
    if ((address & 0xFF'FFFC) == kMarsId) return (address & 2) ? u16{('R' << 8) | 'S'} : u16{('M' << 8) | 'A'};
    if (is_registers(address)) return registers_.main_read(address - SystemRegisters::kMainBase);
    const bool vdp_access = !registers_.sh2_owns_vdp();   // FM = 0: the 68000's turn
    if (address >= kVdpRegs && address < kPalette) {
        return (vdp_access && address < kVdpRegs + 0x10) ? vdp_.read_register(address - kVdpRegs) : 0;
    }
    if (address >= kPalette && address < kPaletteEnd) return vdp_access ? vdp_.read_palette(address - kPalette) : 0;
    if (!registers_.adapter_enabled()) return kOpenBusWord;          // Mega Drive mode
    if (address >= kRomFixedBase) return rom_word(main_rom_offset(address));
    return vdp_access ? vdp_.read_frame_buffer(address - kFrameBuffer) : 0;
}

void Sega32x::main_write16(u32 address, u16 value, u16 lanes) {
    if (address < kVectorPageSize) {
        // Only the H interrupt vector is writable.
        if ((address & ~3u) == kHintVector) {
            if (lanes & 0xFF00) vector_page_[address] = static_cast<u8>(value >> 8);
            if (lanes & 0x00FF) vector_page_[address + 1] = static_cast<u8>(value);
        }
        return;
    }
    if (is_registers(address)) {
        registers_.main_write(address - SystemRegisters::kMainBase, value, lanes);
        if (address - SystemRegisters::kMainBase == 0x12) service_fifo_dreq();   // FIFO: DREQ0
        return;
    }
    if (registers_.sh2_owns_vdp()) return;                           // FM = 1: VDP is the SH-2s'
    if (address >= kVdpRegs && address < kVdpRegs + 0x10) {
        vdp_.write_register(address - kVdpRegs, value, lanes);
    } else if (address >= kPalette && address < kPaletteEnd) {
        vdp_.write_palette(address - kPalette, value, lanes);
    } else if (registers_.adapter_enabled() && address >= kFrameBuffer && address < kRomFixedBase) {
        vdp_.write_frame_buffer(address - kFrameBuffer, value, lanes);
    }
    // ROM is read-only.
}

u16 Sega32x::peek_word(u32 address) const noexcept {
    address &= kAddressMask & ~1u;
    if (address < kVectorPageSize) return static_cast<u16>((vector_page_[address] << 8) | vector_page_[address + 1]);
    if (registers_.adapter_enabled() && address >= kRomFixedBase && address < kAreaEnd) {
        return rom_word(main_rom_offset(address));
    }
    return kOpenBusWord;
}

u16 Sega32x::read16(u32 address) {
    return main_read16(address & kAddressMask & ~1u);
}

u8 Sega32x::read8(u32 address) {
    address &= kAddressMask;
    return lane_byte(address, main_read16(address & ~1u));
}

void Sega32x::write16(u32 address, u16 value) {
    main_write16(address & kAddressMask & ~1u, value, 0xFFFF);
}

void Sega32x::write8(u32 address, u8 value) {
    address &= kAddressMask;
    main_write16(address & ~1u, byte_to_lanes(address, value), byte_lanes(address));
}

// ===========================================================================
// SH-2 side
// ===========================================================================

Sega32x::Sh2Region Sega32x::decode_sh2(u32 address) noexcept {
    switch (address >> 29) {
    case 0:   // cached
    case 1:   // cache-through
        break;
    default:  // associative purge, address/data arrays, on-chip I/O
        return Sh2Region::OnChip;
    }
    const u32 p = sh2_physical(address);
    switch (p >> 25) {  // 32 MB chip-select areas
    case 0:
        if (p < 0x4000) return Sh2Region::BootRom;
        if ((p & 0x3'FFC0) == SystemRegisters::kSh2Base) return Sh2Region::Registers;
        if (p >= 0x4100 && p < 0x4200) return Sh2Region::Vdp;
        if (p >= 0x4200 && p < 0x4400) return Sh2Region::Palette;
        return Sh2Region::Unmapped;
    case 1: return Sh2Region::CartridgeRom;
    case 2: return Sh2Region::FrameBuffer;
    case 3: return Sh2Region::Sdram;
    default: return Sh2Region::Unmapped;
    }
}

u16 Sega32x::Sh2Bus::read16_lanes(u32 address, u16 lanes) {
    (void)lanes;
    address &= ~1u;
    const u32 p = sh2_physical(address);
    const bool vdp_access = system_.registers_.sh2_owns_vdp();  // FM = 1
    switch (decode_sh2(address)) {
    case Sh2Region::BootRom: {
        const auto& rom = cpu_ == Sh2::Master ? system_.master_boot_rom_ : system_.slave_boot_rom_;
        if (rom.empty()) return 0;
        const u32 a = p % static_cast<u32>(rom.size()) & ~1u;
        return static_cast<u16>((rom[a] << 8) | (a + 1 < rom.size() ? rom[a + 1] : 0));
    }
    case Sh2Region::Registers:
        return system_.registers_.sh2_read(cpu_, p & (SystemRegisters::kSh2Size - 1));
    case Sh2Region::Vdp:
        return vdp_access ? system_.vdp_.read_register(p & 0x0F) : 0;
    case Sh2Region::Palette:
        return vdp_access ? system_.vdp_.read_palette(p - 0x4200) : 0;
    case Sh2Region::CartridgeRom: {
        // Mirrored in power-of-two steps of the ROM size.
        u32 size = 0x8000;
        while (size < system_.rom_.size() && size < 0x40'0000) size *= 2;
        return system_.rom_word(p & (size - 1));
    }
    case Sh2Region::FrameBuffer:
        return vdp_access ? system_.vdp_.read_frame_buffer(p & 0x3'FFFF) : 0;
    case Sh2Region::Sdram:
        return system_.sdram_->read16(p);
    default:
        return 0;
    }
}

void Sega32x::Sh2Bus::write16_lanes(u32 address, u16 value, u16 lanes) {
    address &= ~1u;
    const u32 p = sh2_physical(address);
    const bool vdp_access = system_.registers_.sh2_owns_vdp();
    switch (decode_sh2(address)) {
    case Sh2Region::Registers:
        system_.registers_.sh2_write(cpu_, p & (SystemRegisters::kSh2Size - 1), value, lanes);
        break;
    case Sh2Region::Vdp:
        if (vdp_access) system_.vdp_.write_register(p & 0x0F, value, lanes);
        break;
    case Sh2Region::Palette:
        if (vdp_access) system_.vdp_.write_palette(p - 0x4200, value, lanes);
        break;
    case Sh2Region::FrameBuffer:
        if (vdp_access) system_.vdp_.write_frame_buffer(p & 0x3'FFFF, value, lanes);
        break;
    case Sh2Region::Sdram:
        if (lanes == 0xFFFF) {
            system_.sdram_->write16(p, value);
        } else if (lanes == 0xFF00) {
            system_.sdram_->write8(p, static_cast<u8>(value >> 8));
        } else {
            system_.sdram_->write8(p | 1u, static_cast<u8>(value));
        }
        break;
    default:
        break;  // ROMs are read-only
    }
}

u8 Sega32x::Sh2Bus::read8(u32 address) {
    return lane_byte(address, read16_lanes(address, byte_lanes(address)));
}

u16 Sega32x::Sh2Bus::read16(u32 address) { return read16_lanes(address, 0xFFFF); }

u32 Sega32x::Sh2Bus::read32(u32 address) {
    address &= ~3u;
    if (decode_sh2(address) == Sh2Region::Sdram) return system_.sdram_->read32(sh2_physical(address));
    const u32 hi = read16_lanes(address, 0xFFFF);
    return (hi << 16) | read16_lanes(address + 2, 0xFFFF);
}

void Sega32x::Sh2Bus::write8(u32 address, u8 value) {
    write16_lanes(address, byte_to_lanes(address, value), byte_lanes(address));
}

void Sega32x::Sh2Bus::write16(u32 address, u16 value) { write16_lanes(address, value, 0xFFFF); }

void Sega32x::Sh2Bus::write32(u32 address, u32 value) {
    address &= ~3u;
    if (decode_sh2(address) == Sh2Region::Sdram) {
        system_.sdram_->write32(sh2_physical(address), value);
        return;
    }
    write16_lanes(address, static_cast<u16>(value >> 16), 0xFFFF);
    write16_lanes(address + 2, static_cast<u16>(value), 0xFFFF);
}

} // namespace md::sega32x
