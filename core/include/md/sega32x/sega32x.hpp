#pragma once

#include <array>
#include <memory>
#include <span>
#include <vector>

#include "md/bus.hpp"
#include "md/sega32x/sdram.hpp"
#include "md/sega32x/system_registers.hpp"
#include "md/sega32x/vdp32x.hpp"
#include "md/sh2.hpp"
#include "md/types.hpp"

namespace md::sega32x {

// Sega 32X: two Hitachi SH-2s (master and slave, 23 MHz = master clock x 3/7,
// exactly 3x the 68000) sharing 256 KB of SDRAM, a frame-buffer VDP and PWM
// sound, plugged into the cartridge slot. The 68000 keeps running the Mega
// Drive side.
//
// 68000 map (BusDevice; Memory::attach_32x routes these addresses here):
//   0xA130EC-0xA130EF   "MARS": the 32X's ID, how software detects it
//   0xA15100-0xA1517F   system registers (SystemRegisters)
//   0xA15180-0xA1518F   32X VDP registers  } only while FM = 0
//   0xA15200-0xA153FF   palette            } (the 68000 owns the VDP)
//   with ADEN = 1 (32X mode):
//   0x840000-0x85FFFF   frame buffer (FM = 0)
//   0x860000-0x87FFFF   frame buffer, overwrite image (FM = 0)
//   0x880000-0x8FFFFF   cartridge ROM, first 512 KB
//   0x900000-0x9FFFFF   cartridge ROM, 1 MB bank BK (0xA15104)
//   0x000000-0x0000FF   the 32X's 68000 vector page (while RV = 0); its
//                       H interrupt vector (0x70) is writable
//   ADEN = 0: 0x840000-0x9FFFFF is open bus, as without a 32X.
//
// SH-2 map (Sh2Bus, one per CPU: what each SH-2 reaches on its external bus).
// Address bit 29 selects the cache mode: 0x0xxxxxxx is read through the SH-2
// cache, 0x2xxxxxxx bypasses it (cache-through); both reach the same memory.
// Shared data is accessed cache-through, e.g. SDRAM at 0x26000000.
//   0x00000000-0x00003FFF   boot ROM (master 2 KB, slave 1 KB), mirrored
//   0x00004000-0x0000403F   system registers (mirrored on address bits 6-17)
//   0x00004100-0x0000410F   32X VDP registers  } only while FM = 1
//   0x00004200-0x000043FF   palette            } (the SH-2s own the VDP)
//   0x02000000-0x03FFFFFF   cartridge ROM (mirrored)
//   0x04000000-0x0401FFFF   frame buffer (FM = 1), mirrored
//   0x04020000-0x0403FFFF   frame buffer, overwrite image
//   0x06000000-0x07FFFFFF   SDRAM, 256 KB mirrored
// Higher areas (cache control, cache data array, on-chip peripherals) are
// inside each SH-2 (md::Sh2), not on this bus.
//
// Master or slave: both CPUs share everything above except their boot ROM,
// whose reset vectors (PC at 0, SP at 4) start each one on its own code; and
// each CPU's BCR1 register reads MASTER = 0 on the master, 1 on the slave.
//
// Running: the SH-2s are held in reset until the 68000 sets RES (0xA15100);
// they then start from their boot ROM's vectors. Without boot ROMs, a
// built-in boot does the BIOS's work instead (hle_boot()): it loads the
// cartridge's SH-2 program as its header at 0x3C0 describes and starts both
// CPUs at their entry points. run_until() runs them in
// lockstep, master then slave, to the same SH-2 time.
class Sega32x final : public BusDevice {
public:
    static constexpr u32 kMasterBootRomSize = 2 * 1024;
    static constexpr u32 kSlaveBootRomSize  = 1 * 1024;
    static constexpr u32 kRomFixedBase  = 0x88'0000;   // first 512 KB
    static constexpr u32 kRomFixedSize  = 0x8'0000;
    static constexpr u32 kRomBankBase   = 0x90'0000;   // 1 MB bank
    static constexpr u32 kRomBankSize   = 0x10'0000;
    static constexpr u8  kOpenBusByte = 0xFF;
    static constexpr u16 kOpenBusWord = 0xFFFF;

    // SH-2 clock = master clock x 3 / 7 (23.01 MHz NTSC, 22.80 MHz PAL).
    static constexpr u64 kSh2ClockNumerator = 3;
    static constexpr u64 kSh2ClockDenominator = 7;

    // Which part of the system an SH-2 address reaches.
    enum class Sh2Region { BootRom, Registers, Vdp, Palette, CartridgeRom, FrameBuffer, Sdram, OnChip, Unmapped };
    [[nodiscard]] static Sh2Region decode_sh2(u32 address) noexcept;

    // An SH-2's external bus. The SH-2 bus is 32 bits wide; the 16-bit
    // devices (registers, ROM, frame buffer) see a 32-bit access as two
    // 16-bit ones.
    class Sh2Bus final : public Sh2MemoryBus {
    public:
        Sh2Bus(Sega32x& system, Sh2 cpu) : system_(system), cpu_(cpu) {}
        [[nodiscard]] u8   read8(u32 address) override;
        [[nodiscard]] u16  read16(u32 address) override;
        [[nodiscard]] u32  read32(u32 address) override;
        void write8(u32 address, u8 value) override;
        void write16(u32 address, u16 value) override;
        void write32(u32 address, u32 value) override;
        [[nodiscard]] Sh2 cpu() const noexcept { return cpu_; }

    private:
        [[nodiscard]] u16 read16_lanes(u32 address, u16 lanes);
        void write16_lanes(u32 address, u16 value, u16 lanes);

        Sega32x& system_;
        Sh2 cpu_;
    };

    Sega32x();

    Sega32x(const Sega32x&) = delete;
    Sega32x& operator=(const Sega32x&) = delete;

    // Clears memories and registers: back to Mega Drive mode, SH-2s in reset.
    void reset();

    // The cartridge, as both sides see it. The span must stay valid until the
    // next call (the Emulator passes Memory's ROM after each load).
    void set_cartridge(std::span<const u8> rom) noexcept;

    // SH-2 boot ROMs (the 32X BIOS: 2 KB master, 1 KB slave). Without them
    // the area reads as 0 and the built-in boot starts the cartridge.
    void load_boot_roms(std::span<const u8> master, std::span<const u8> slave);

    // Video standard, for the 32X VDP's PAL bit.
    void set_ntsc(bool ntsc) noexcept { vdp_.set_ntsc(ntsc); }

    // --- Scheduling ---------------------------------------------------------------
    // Runs both SH-2s up to `sh2_time` (SH-2 cycles since reset, absolute):
    // nothing while RES holds them, a reset from their vectors when RES is
    // released. Instruction overshoot is absorbed by the next call.
    void run_until(s64 sh2_time);
    [[nodiscard]] s64 sh2_time() const noexcept { return sh2_time_; }
    // Video timing from the Mega Drive VDP, which the 32X follows.
    //   VBlank start: flips the frame buffers, raises V on both SH-2s and
    //   reloads the H interrupt counter.
    //   HBlank start: counts a line (in VBlank only if HEN); every
    //   (H count + 1) counted lines, raises H on both SH-2s.
    // Both also drive the VBLK / HBLK status bits.
    void set_vblank(bool active);
    void set_hblank(bool active);
    // Draws the 32X picture's line over the Mega Drive's (32X mode only):
    // called by the Mega Drive VDP as it finishes each visible line.
    void compose_line(int line, std::span<Framebuffer::Pixel> row, std::span<const u8> md_backdrop) const noexcept {
        if (registers_.adapter_enabled()) vdp_.compose_line(line, row, md_backdrop);
    }

    // PWM audio at `sh2_time` (SH-2 cycles, as for run_until), for the mixer.
    void render_audio(s64 sh2_time, s32& left, s32& right) { registers_.pwm().output_at(sh2_time, left, right); }

    // --- 68000 side (BusDevice, full 24-bit addresses) ----------------------
    // True for the addresses Memory must route here.
    [[nodiscard]] bool claims(u32 address) const noexcept;
    u8   read8(u32 address) override;
    u16  read16(u32 address) override;
    // Without side effects, for debuggers: the vector page and ROM windows
    // (registers and frame buffer read as open bus).
    [[nodiscard]] u16 peek_word(u32 address) const noexcept;
    void write8(u32 address, u8 value) override;
    void write16(u32 address, u16 value) override;

    // --- Components -------------------------------------------------------------
    [[nodiscard]] Sh2Bus& sh2_bus(Sh2 cpu) noexcept { return cpu == Sh2::Master ? master_bus_ : slave_bus_; }
    [[nodiscard]] md::Sh2& sh2(Sh2 cpu) noexcept { return cpu == Sh2::Master ? *master_ : *slave_; }
    [[nodiscard]] SystemRegisters& registers() noexcept { return registers_; }
    [[nodiscard]] const SystemRegisters& registers() const noexcept { return registers_; }
    [[nodiscard]] Sdram& sdram() noexcept { return *sdram_; }
    [[nodiscard]] const Sdram& sdram() const noexcept { return *sdram_; }
    [[nodiscard]] Vdp32x& vdp() noexcept { return vdp_; }
    [[nodiscard]] const Vdp32x& vdp() const noexcept { return vdp_; }

private:
    [[nodiscard]] u16 main_read16(u32 address);
    void main_write16(u32 address, u16 value, u16 lanes);
    [[nodiscard]] u8 rom_byte(u32 offset) const noexcept;
    [[nodiscard]] u16 rom_word(u32 offset) const noexcept;
    // Cartridge ROM offset for a 68000 address in 0x880000-0x9FFFFF.
    [[nodiscard]] u32 main_rom_offset(u32 address) const noexcept;
    [[nodiscard]] bool vector_page_visible() const noexcept {
        return registers_.adapter_enabled() && !registers_.rom_at_zero();
    }
    void build_vector_page();
    // DREQ0: while the 68000's FIFO holds words, an SH-2 whose DMA channel 0
    // waits for requests takes them (normally from 0x20004012, the FIFO).
    void service_fifo_dreq();
    // DREQ1: one unit for whichever SH-2's channel 1 waits (PWM refill).
    void pwm_dreq();
    // What the boot ROMs would do, for a CPU whose boot ROM is missing.
    void hle_boot(Sh2 cpu);

    // Declaration order matters: the CPUs read their vectors through the
    // buses (and so the boot ROMs) when constructed.
    SystemRegisters registers_;
    std::unique_ptr<Sdram> sdram_ = std::make_unique<Sdram>();
    Vdp32x vdp_;
    std::span<const u8> rom_;
    std::vector<u8> master_boot_rom_;
    std::vector<u8> slave_boot_rom_;
    std::array<u8, 0x100> vector_page_{};
    Sh2Bus master_bus_{*this, Sh2::Master};
    Sh2Bus slave_bus_{*this, Sh2::Slave};
    std::unique_ptr<md::Sh2> master_ = std::make_unique<md::Sh2>(master_bus_, false);
    std::unique_ptr<md::Sh2> slave_ = std::make_unique<md::Sh2>(slave_bus_, true);

    s64  sh2_time_ = 0;
    int  h_counter_ = 0;          // lines until the next H interrupt
    bool sh2_in_reset_ = true;
    s64  master_alignment_ = 0;   // cpu.cycles() - sh2_time_ while running
    s64  slave_alignment_ = 0;
};

} // namespace md::sega32x
