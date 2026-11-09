#include "md/memory.hpp"

#include <algorithm>

namespace md {

namespace {

// Address decoding boundaries (inclusive starts, exclusive ends).
constexpr u32 kRomEnd       = 0x40'0000;
constexpr u32 kZ80AreaStart = 0xA0'0000;
constexpr u32 kZ80AreaEnd   = 0xA1'0000;
constexpr u32 kIoAreaEnd    = 0xA2'0000;
constexpr u32 kVdpAreaStart = 0xC0'0000;
constexpr u32 kRamAreaStart = 0xE0'0000;

constexpr u32 kRamMask    = Memory::kRamSize - 1;      // 64 KB mirror
constexpr u32 kZ80RamMask = Memory::kZ80RamSize - 1;   // 8 KB mirror

// Big-endian helpers: the high byte lives at the lower address.
constexpr u16 make_word(u8 hi, u8 lo) noexcept {
    return static_cast<u16>((u16{hi} << 8) | lo);
}
constexpr u8 hi_byte(u16 w) noexcept { return static_cast<u8>(w >> 8); }
constexpr u8 lo_byte(u16 w) noexcept { return static_cast<u8>(w & 0xFF); }

// Power-on I/O register state: data ports pulled high, ports set as inputs,
// serial TX buffers 0xFF.
constexpr std::array<u8, 16> kIoRegsReset = {
    0x00,              // version (set by set_version_register)
    0x7F, 0x7F, 0x7F,  // data A, B, EXP
    0x00, 0x00, 0x00,  // ctrl A, B, EXP
    0xFF, 0x00, 0x00,  // port A serial: TX data, RX data, S-ctrl
    0xFF, 0x00, 0x00,  // port B serial
    0xFF, 0x00, 0x00,  // port EXP serial
};

// VDP region 0xC00000-0xDFFFFF is only decoded where these bits match; other
// addresses lock up a real console. We treat them as open bus.
constexpr bool is_valid_vdp_address(u32 address) noexcept {
    return (address & 0xE7'00E0) == 0xC0'0000;
}

} // namespace

Memory::Memory() {
    io_regs_[0] = 0xA0;  // overseas, NTSC, no expansion unit, no TMSS
    reset();
}

void Memory::load_rom(std::span<const u8> image) {
    const auto size = std::min<std::size_t>(image.size(), kRomMaxSize);
    rom_.assign(image.begin(), image.begin() + static_cast<std::ptrdiff_t>(size));
}

void Memory::reset() {
    ram_.fill(0);
    z80_ram_.fill(0);
    const u8 version = io_regs_[0];
    io_regs_ = kIoRegsReset;
    io_regs_[0] = version;
    z80_busreq_ = false;
    z80_reset_  = true;
    z80_bank_   = 0;
}

// ===========================================================================
// Public access functions
// ===========================================================================

u8 Memory::read_byte(u32 address) {
    address &= kAddressMask;

    if (is_expansion(address))    return expansion_->read8(address);
    if (address < kRomEnd)        return rom_byte(address);
    if (address >= kRamAreaStart) return ram_[address & kRamMask];
    if (address >= kVdpAreaStart) return vdp_read_byte(address);
    if (address >= kZ80AreaStart && address < kZ80AreaEnd) return z80_area_read(address);
    if (address >= kZ80AreaEnd   && address < kIoAreaEnd)  return io_area_read_byte(address);

    return kOpenBusByte;
}

u16 Memory::read_word(u32 address) {
    address &= kAddressMask;

    if (is_expansion(address)) return expansion_->read16(address);
    if (address < kRomEnd) {
        return make_word(rom_byte(address), rom_byte(address + 1));
    }
    if (address >= kRamAreaStart) {
        return make_word(ram_[address & kRamMask], ram_[(address + 1) & kRamMask]);
    }
    if (address >= kVdpAreaStart) return vdp_read_word(address);
    if (address >= kZ80AreaStart && address < kZ80AreaEnd) {
        // The Z80 bus is 8 bits wide: a 68000 word read sees the byte on both halves.
        const u8 b = z80_area_read(address);
        return make_word(b, b);
    }
    if (address >= kZ80AreaEnd && address < kIoAreaEnd) return io_area_read_word(address);

    return kOpenBusWord;
}

u32 Memory::read_long(u32 address) {
    // The 68000 performs a long access as two word cycles, high word first.
    const u32 hi = read_word(address);
    const u32 lo = read_word(address + 2);
    return (hi << 16) | lo;
}

u16 Memory::peek_word(u32 address) const noexcept {
    address &= kAddressMask;
    if (address < kRomEnd) {
        return make_word(rom_byte(address), rom_byte(address + 1));
    }
    if (address >= kRamAreaStart) {
        return make_word(ram_[address & kRamMask], ram_[(address + 1) & kRamMask]);
    }
    return kOpenBusWord;
}

void Memory::write_byte(u32 address, u8 value) {
    address &= kAddressMask;

    if (is_expansion(address)) { expansion_->write8(address, value); return; }
    if (address < kRomEnd) return;  // ROM is read-only (SRAM/mappers: TODO)
    if (address >= kRamAreaStart) { ram_[address & kRamMask] = value; return; }
    if (address >= kVdpAreaStart) { vdp_write_byte(address, value); return; }
    if (address >= kZ80AreaStart && address < kZ80AreaEnd) { z80_area_write(address, value); return; }
    if (address >= kZ80AreaEnd   && address < kIoAreaEnd)  { io_area_write_byte(address, value); return; }
}

void Memory::write_word(u32 address, u16 value) {
    address &= kAddressMask;

    if (is_expansion(address)) { expansion_->write16(address, value); return; }
    if (address < kRomEnd) return;
    if (address >= kRamAreaStart) {
        ram_[address & kRamMask]       = hi_byte(value);
        ram_[(address + 1) & kRamMask] = lo_byte(value);
        return;
    }
    if (address >= kVdpAreaStart) { vdp_write_word(address, value); return; }
    if (address >= kZ80AreaStart && address < kZ80AreaEnd) {
        // Only the high byte reaches the 8-bit Z80 bus.
        z80_area_write(address, hi_byte(value));
        return;
    }
    if (address >= kZ80AreaEnd && address < kIoAreaEnd) { io_area_write_word(address, value); return; }
}

void Memory::write_long(u32 address, u32 value) {
    // High word first. (The CPU core handles the -(An) case, where the
    // 68000 writes the low word first, by issuing two write_word calls itself.)
    write_word(address,     static_cast<u16>(value >> 16));
    write_word(address + 2, static_cast<u16>(value & 0xFFFF));
}

// ===========================================================================
// ROM
// ===========================================================================

u8 Memory::rom_byte(u32 address) const noexcept {
    return address < rom_.size() ? rom_[address] : kOpenBusByte;
}

// ===========================================================================
// Z80 area: 0xA00000-0xA0FFFF
// ===========================================================================

u8 Memory::z80_area_read(u32 address) {
    // The 68000 can only reach the Z80 bus after requesting it via 0xA11100.
    if (!z80_busreq_) return kOpenBusByte;

    const u32 offset = address & 0xFFFF;
    if (offset < 0x4000) return z80_ram_[offset & kZ80RamMask];  // 8 KB, mirrored once
    if (offset < 0x6000) {
        // With no YM2612 attached, report an idle chip (status 0: not busy, no
        // timer overflow). Open bus (0xFF) would read as "busy" forever and
        // hang 68000 sound drivers that poll bit 7 before each write.
        return ym2612_ ? ym2612_->read(offset & 3) : 0x00;
    }

    // 0x6000 bank register (write-only), 0x7F00 VDP (locks up from 68000 side),
    // 0x8000+ banked 68000 window (inaccessible to the 68000).
    return kOpenBusByte;
}

void Memory::z80_area_write(u32 address, u8 value) {
    if (!z80_busreq_) return;

    const u32 offset = address & 0xFFFF;
    if (offset < 0x4000) { z80_ram_[offset & kZ80RamMask] = value; return; }
    if (offset < 0x6000) { if (ym2612_) ym2612_->write(offset & 3, value); return; }
    if (offset < 0x6100) z80_write(static_cast<u16>(offset), value);  // bank register
}

// ===========================================================================
// Z80 view
// ===========================================================================

u8 Memory::z80_read(u16 address) {
    if (address < 0x4000) return z80_ram_[address & kZ80RamMask];
    if (address < 0x6000) return ym2612_ ? ym2612_->read(address & 3) : 0x00;
    if (address >= 0x8000) {
        // Banked window onto the 68000 bus (usually ROM, for sample data).
        return read_byte(z80_bank_base() | (address & 0x7FFF));
    }
    if (address >= 0x7F00 && address < 0x7F20) return vdp_read_byte(0xC0'0000 | (address & 0x1F));
    return kOpenBusByte;
}

void Memory::z80_write(u16 address, u8 value) {
    if (address < 0x4000) { z80_ram_[address & kZ80RamMask] = value; return; }
    if (address < 0x6000) { if (ym2612_) ym2612_->write(address & 3, value); return; }
    if (address < 0x6100) {
        // Each write shifts bit 0 in at the top of the 9-bit bank register,
        // so a full bank number is set with 9 writes, LSB first.
        z80_bank_ = ((z80_bank_ >> 1) | (u32{static_cast<u32>(value & 1)} << 8)) & 0x1FF;
        return;
    }
    if (address >= 0x8000) { write_byte(z80_bank_base() | (address & 0x7FFF), value); return; }
    if (address >= 0x7F00 && address < 0x7F20) vdp_write_byte(0xC0'0000 | (address & 0x1F), value);
}

// ===========================================================================
// I/O area: 0xA10000-0xA1FFFF
// ===========================================================================

u8 Memory::io_area_read_byte(u32 address) {
    switch ((address >> 8) & 0xFF) {
    case 0x00:  // 0xA100xx: I/O registers (only 0x00-0x1F decoded)
        if ((address & 0xE0) == 0) return io_reg_read((address >> 1) & 0x0F);
        break;
    case 0x11:  // 0xA111xx: Z80 BUSREQ. Bit 0 of the even byte: 0 = bus granted.
        if ((address & 1) == 0) {
            const bool granted = z80_busreq_ && !z80_reset_;
            return static_cast<u8>((kOpenBusByte & 0xFE) | (granted ? 0 : 1));
        }
        break;
    default:
        break;
    }
    return kOpenBusByte;
}

u16 Memory::io_area_read_word(u32 address) {
    switch ((address >> 8) & 0xFF) {
    case 0x00:
        if ((address & 0xE0) == 0) {
            // Byte-wide registers on odd addresses; the value appears on both halves.
            const u8 v = io_reg_read((address >> 1) & 0x0F);
            return make_word(v, v);
        }
        break;
    case 0x11: {
        const bool granted = z80_busreq_ && !z80_reset_;
        return static_cast<u16>((kOpenBusWord & 0xFEFF) | (granted ? 0 : 0x0100));
    }
    default:
        break;
    }
    return kOpenBusWord;
}

void Memory::io_area_write_byte(u32 address, u8 value) {
    switch ((address >> 8) & 0xFF) {
    case 0x00:
        if ((address & 0xE0) == 0) io_reg_write((address >> 1) & 0x0F, value);
        break;
    case 0x11:  // BUSREQ: 1 = request bus
        if ((address & 1) == 0) z80_busreq_ = (value & 1) != 0;
        break;
    case 0x12:  // RESET: 0 = assert reset, 1 = release
        // TODO: releasing reset must also reset the Z80 CPU and YM2612.
        if ((address & 1) == 0) z80_reset_ = (value & 1) == 0;
        break;
    default:
        // 0xA130xx (SRAM/mapper), 0xA14000 (TMSS): not implemented yet.
        break;
    }
}

void Memory::io_area_write_word(u32 address, u16 value) {
    switch ((address >> 8) & 0xFF) {
    case 0x00:
        if ((address & 0xE0) == 0) io_reg_write((address >> 1) & 0x0F, lo_byte(value));
        break;
    case 0x11:
        z80_busreq_ = (value & 0x0100) != 0;
        break;
    case 0x12:
        z80_reset_ = (value & 0x0100) == 0;
        break;
    default:
        break;
    }
}

u8 Memory::io_reg_read(u32 index) const noexcept {
    if (index >= 1 && index <= 3) {
        // Data ports: pins configured as outputs (ctrl bit = 1) read back the
        // latched value; inputs read the peripheral. With nothing connected,
        // inputs are pulled high. Bit 7 is always the latched value.
        const u8 data = io_regs_[index];
        const u8 ctrl = io_regs_[index + 3];
        const Gamepad* pad = pads_[static_cast<std::size_t>(index - 1)];
        const u8 input = pad ? pad->read() : 0x7F;
        return static_cast<u8>((data & (ctrl | 0x80)) | (input & ~ctrl & 0x7F));
    }
    return io_regs_[index];
}

void Memory::io_reg_write(u32 index, u8 value) noexcept {
    if (index == 0) return;  // version register is read-only
    io_regs_[index] = value;

    // Data (1-3) or control (4-6) writes can change the TH line seen by the
    // device on that port: driven by data bit 6 when ctrl bit 6 makes it an
    // output, otherwise pulled high.
    const u32 port = (index >= 1 && index <= 3) ? index - 1
                   : (index >= 4 && index <= 6) ? index - 4 : 3;
    if (port < 3 && pads_[port]) {
        const u8 data = io_regs_[port + 1];
        const u8 ctrl = io_regs_[port + 4];
        pads_[port]->set_th((ctrl & 0x40) ? (data & 0x40) != 0 : true);
    }
}

// ===========================================================================
// VDP area: 0xC00000-0xDFFFFF (ports at 0x00-0x1F)
// ===========================================================================

u8 Memory::vdp_read_byte(u32 address) {
    if (!is_valid_vdp_address(address)) return kOpenBusByte;

    const u32 port = address & 0x1F;
    if (port < 0x10 && vdp_) {
        // Byte read of a 16-bit port: pick the half selected by A0.
        const u16 w = vdp_->read(port & 0x1E);
        return (address & 1) ? lo_byte(w) : hi_byte(w);
    }
    return kOpenBusByte;  // PSG is write-only; 0x18-0x1F unused/debug
}

u16 Memory::vdp_read_word(u32 address) {
    if (!is_valid_vdp_address(address)) return kOpenBusWord;

    const u32 port = address & 0x1F;
    if (port < 0x10 && vdp_) return vdp_->read(port & 0x1E);
    return kOpenBusWord;
}

void Memory::vdp_write_byte(u32 address, u8 value) {
    if (!is_valid_vdp_address(address)) return;

    const u32 port = address & 0x1F;
    if (port < 0x10) {
        // Byte writes to the VDP put the same byte on both halves of the data bus.
        if (vdp_) vdp_->write(port & 0x1E, make_word(value, value));
    } else if (port < 0x18 && (port & 1)) {
        // PSG at 0x11/0x13/0x15/0x17.
        if (psg_) psg_->write(0, value);
    }
    // 0x1C-0x1F: debug register, ignored.
}

void Memory::vdp_write_word(u32 address, u16 value) {
    if (!is_valid_vdp_address(address)) return;

    const u32 port = address & 0x1F;
    if (port < 0x10) {
        if (vdp_) vdp_->write(port & 0x1E, value);
    } else if (port < 0x18) {
        if (psg_) psg_->write(0, lo_byte(value));
    }
}

} // namespace md
