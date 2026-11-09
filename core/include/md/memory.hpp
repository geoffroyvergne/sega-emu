#pragma once

#include <array>
#include <span>
#include <vector>

#include "md/bus.hpp"
#include "md/gamepad.hpp"
#include "md/types.hpp"

namespace md {

// Interfaces for chips that sit on the 68000 bus. Memory forwards accesses to
// them so it never needs to know how the VDP or sound chips work internally.

// 16-bit device (the VDP). `port` is the register offset, always even:
// 0x00/0x02 data, 0x04/0x06 control, 0x08..0x0E HV counter.
class WordDevice {
public:
    virtual ~WordDevice() = default;
    virtual u16  read(u32 port) = 0;
    virtual void write(u32 port, u16 value) = 0;
};

// 8-bit device (YM2612, SN76489 PSG). `port` is the chip-relative register.
class ByteDevice {
public:
    virtual ~ByteDevice() = default;
    virtual u8   read(u32 port) = 0;
    virtual void write(u32 port, u8 value) = 0;
};

// The 68000's view of the Mega Drive address space (24-bit, 16 MB).
//
//   0x000000-0x3FFFFF  Cartridge ROM (up to 4 MB)
//   0x400000-0x7FFFFF  Expansion (Mega-CD) — forwarded to attach_expansion()
//   0x800000-0x9FFFFF  32X — unmapped, open bus
//   0xA00000-0xA0FFFF  Z80 address space (8 KB RAM, YM2612) — needs Z80 bus
//   0xA10000-0xA1001F  I/O: version register, controller ports, serial
//   0xA11100           Z80 bus request
//   0xA11200           Z80 reset
//   0xA12000-0xA120FF  Mega-CD gate array registers (expansion device)
//   0xC00000-0xC0001F  VDP ports + PSG (mirrored up to 0xDFFFFF)
//   0xE00000-0xFFFFFF  64 KB main RAM, mirrored every 64 KB (canonical 0xFF0000)
//
// Endianness: the 68000 is big-endian. All storage here is kept in bus order
// (byte at the lower address = most significant), and 16/32-bit values are
// composed with shifts, so the code is correct on any host byte order.
//
// Alignment: word/long accesses to odd addresses trigger an address error on
// the 68000. The CPU core must raise that exception before calling into Memory.
class Memory final : public Bus68k {
public:
    static constexpr u32 kAddressMask = 0xFF'FFFF;
    static constexpr u32 kRomMaxSize  = 0x40'0000;
    static constexpr u32 kRamSize     = 0x1'0000;
    static constexpr u32 kZ80RamSize  = 0x2000;

    // Value returned for reads from unmapped/undriven addresses. Real hardware
    // returns the last prefetched opcode word; a constant is close enough for now.
    static constexpr u8  kOpenBusByte = 0xFF;
    static constexpr u16 kOpenBusWord = 0xFFFF;

    Memory();

    // Copies the ROM image (raw .bin/.md format, big-endian). Images larger than
    // 4 MB are truncated; bank-switched carts need a mapper (not handled yet).
    void load_rom(std::span<const u8> image);

    // Clears RAM and resets bus/I/O registers. Keeps the loaded ROM.
    void reset();

    // Reads may have side effects (e.g. reading the VDP control port), so they are non-const.
    [[nodiscard]] u8  read_byte(u32 address) override;
    [[nodiscard]] u16 read_word(u32 address) override;
    [[nodiscard]] u32 read_long(u32 address) override;

    // Side-effect-free read for debuggers/disassemblers: ROM and RAM only,
    // everything else returns open bus without touching any device.
    [[nodiscard]] u16 peek_word(u32 address) const noexcept override;

    void write_byte(u32 address, u8  value) override;
    void write_word(u32 address, u16 value) override;
    void write_long(u32 address, u32 value) override;

    void attach_vdp(WordDevice* vdp) noexcept    { vdp_ = vdp; }
    void attach_ym2612(ByteDevice* ym) noexcept  { ym2612_ = ym; }
    void attach_psg(ByteDevice* psg) noexcept    { psg_ = psg; }

    // Controller ports: 0 = port A (0xA10003), 1 = port B (0xA10005),
    // 2 = expansion (0xA10007). nullptr = nothing plugged in.
    // Expansion connector (Mega-CD): receives accesses to 0x400000-0x7FFFFF
    // and to its registers at 0xA12000-0xA120FF. With cd_boot, the console
    // has no cartridge and boots from the expansion: 0x000000-0x3FFFFF is
    // routed to the device instead of the cartridge ROM.
    void attach_expansion(BusDevice* device, bool cd_boot = false) noexcept {
        expansion_ = device;
        cd_boot_ = cd_boot;
    }

    void attach_pad(int port, Gamepad* pad) noexcept {
        if (port >= 0 && port < 3) pads_[static_cast<std::size_t>(port)] = pad;
    }

    // Hardware version register (0xA10001):
    //   bit 7: 1 = overseas (US/EU), 0 = domestic (JP)
    //   bit 6: 1 = PAL (50 Hz),     0 = NTSC (60 Hz)
    //   bit 5: 1 = no expansion unit (Sega CD) attached
    //   bits 3-0: hardware revision (0 = no TMSS)
    void set_version_register(u8 value) noexcept { io_regs_[0] = value; }

    // --- Z80 view of the system (16-bit address space) -----------------------
    //   0x0000-0x3FFF  8 KB Z80 RAM (mirrored once)
    //   0x4000-0x5FFF  YM2612 (4 ports, mirrored)
    //   0x6000-0x60FF  bank register (write: shifts in one address bit)
    //   0x7F00-0x7F1F  VDP ports / PSG (as seen from 0xC00000 on the 68000)
    //   0x8000-0xFFFF  32 KB window onto the 68000 bus at (bank << 15)
    [[nodiscard]] u8 z80_read(u16 address);
    void z80_write(u16 address, u8 value);

    // 68000 address of the Z80's 0x8000 window.
    [[nodiscard]] u32 z80_bank_base() const noexcept { return z80_bank_ << 15; }

    // Shared with the Z80 core, which sees this RAM at its 0x0000-0x1FFF.
    [[nodiscard]] std::span<u8>       z80_ram() noexcept        { return z80_ram_; }
    [[nodiscard]] std::span<const u8> main_ram() const noexcept { return ram_; }
    [[nodiscard]] std::span<const u8> rom() const noexcept      { return rom_; }

    // Z80 control lines, for the scheduler: the Z80 must not run while the
    // 68000 holds its bus or while its reset line is asserted.
    [[nodiscard]] bool z80_bus_requested()  const noexcept { return z80_busreq_; }
    [[nodiscard]] bool z80_reset_asserted() const noexcept { return z80_reset_; }

private:
    [[nodiscard]] u8   rom_byte(u32 address) const noexcept;

    // Addresses claimed by the expansion device (Mega-CD), if one is attached.
    [[nodiscard]] bool is_expansion(u32 address) const noexcept {
        if (!expansion_) return false;
        if ((address & 0xFF'FF00) == 0xA1'2000) return true;               // gate array
        if (address >= 0x40'0000 && address < 0x80'0000) return true;      // expansion area
        return cd_boot_ && address < 0x40'0000;                           // CD boot: no cartridge
    }

    [[nodiscard]] u8   z80_area_read(u32 address);
    void               z80_area_write(u32 address, u8 value);

    [[nodiscard]] u8   io_area_read_byte(u32 address);
    [[nodiscard]] u16  io_area_read_word(u32 address);
    void               io_area_write_byte(u32 address, u8 value);
    void               io_area_write_word(u32 address, u16 value);
    [[nodiscard]] u8   io_reg_read(u32 index) const noexcept;
    void               io_reg_write(u32 index, u8 value) noexcept;

    [[nodiscard]] u8   vdp_read_byte(u32 address);
    [[nodiscard]] u16  vdp_read_word(u32 address);
    void               vdp_write_byte(u32 address, u8 value);
    void               vdp_write_word(u32 address, u16 value);

    std::vector<u8>                rom_;
    std::array<u8, kRamSize>       ram_{};
    std::array<u8, kZ80RamSize>    z80_ram_{};

    // 0xA10001-0xA1001F, one byte register per odd address:
    // [0] version, [1..3] data A/B/EXP, [4..6] ctrl A/B/EXP, [7..15] serial.
    std::array<u8, 16> io_regs_{};

    u32  z80_bank_   = 0;      // 9-bit bank register (68000 A23-A15)
    bool z80_busreq_ = false;
    bool z80_reset_  = true;   // Z80 is held in reset at power-on

    WordDevice* vdp_    = nullptr;
    ByteDevice* ym2612_ = nullptr;
    ByteDevice* psg_    = nullptr;
    std::array<Gamepad*, 3> pads_{};
    BusDevice* expansion_ = nullptr;
    bool cd_boot_ = false;
};

} // namespace md
