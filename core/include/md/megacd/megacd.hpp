#pragma once

#include <array>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "md/bus.hpp"
#include "md/m68k.hpp"
#include "md/megacd/cd_drive.hpp"
#include "md/megacd/cdc.hpp"
#include "md/megacd/disc.hpp"
#include "md/megacd/graphics_asic.hpp"
#include "md/megacd/pcm.hpp"
#include "md/megacd/program_ram.hpp"
#include "md/megacd/registers.hpp"
#include "md/megacd/word_ram.hpp"
#include "md/types.hpp"

namespace md::megacd {

// Mega-CD (Sega CD) subsystem: a second 68000 (the "sub CPU", 12.5 MHz) with
// its own memory, linked to the Mega Drive through the gate array.
//
// Main CPU map (relative to the expansion base: 0x400000 when a cartridge is
// inserted, 0x000000 when booting from CD; see Memory::attach_expansion):
//   +0x000000-0x01FFFF  BIOS ROM (128 KB, mirrored up to +0x1FFFFF)
//   +0x020000-0x03FFFF  PRG-RAM window (128 KB bank BK; sub CPU must be halted)
//   +0x200000-0x23FFFF  Word RAM (see WordRam for 2M / 1M layouts)
//   0xA12000-0xA1202F   gate array registers (MegaCdRegisters)
//
// Sub CPU map (SubBus):
//   0x000000-0x07FFFF   PRG-RAM (write-protected below WP x 0x200)
//   0x080000-0x0BFFFF   Word RAM, 2M mode (1M: dot image, TODO)
//   0x0C0000-0x0DFFFF   Word RAM, 1M mode: the sub CPU's bank
//   0xFE0000-0xFE3FFF   backup RAM (8 KB, odd bytes only)
//   0xFF0000-0xFF7FFF   PCM sound chip (Pcm)
//   0xFF8000-0xFF81FF   gate array registers (0x04-0x0B: CDC, 0x0C: stopwatch,
//                       0x30: timer, 0x36-0x4B: CD drive, 0x58-0x67: graphics ASIC)
//
// In place: memories, mapping, CPU-to-CPU communication, reset, bus request,
// the CD drive (CDD command interface, INT4) streaming sectors from a disc
// image into the CD data controller (CDC, INT5), the graphics ASIC (stamp
// rotation/scaling, INT1), the timer (INT3), the stopwatch, and sound (the
// PCM chip and CD-DA). Still to come: subcode (INT6).
class MegaCd final : public BusDevice {
public:
    static constexpr u32 kBiosSize = 128 * 1024;
    static constexpr u32 kBackupRamSize = 8 * 1024;
    static constexpr double kSubCpuClockHz = 12'500'000.0;

    MegaCd();

    MegaCd(const MegaCd&) = delete;
    MegaCd& operator=(const MegaCd&) = delete;

    // Clears RAM and registers; the sub CPU is held in reset (as at power-on).
    void reset();

    // The Mega-CD BIOS (boot ROM), required to boot a CD. Raw 128 KB image.
    void load_bios(std::span<const u8> image);

    // --- Backup RAM ------------------------------------------------------------
    // The internal 8 KB battery-backed save RAM. It survives reset(); a new
    // MegaCd starts with it freshly formatted (empty directory), as after the
    // BIOS's "format" option. load_backup_ram() restores a saved image (8 KB).
    [[nodiscard]] std::span<const u8> backup_ram() const noexcept { return backup_ram_; }
    void load_backup_ram(std::span<const u8> data);
    void format_backup_ram();

    // --- Main CPU side (BusDevice, full 24-bit addresses) --------------------
    u8   read8(u32 address) override;
    u16  read16(u32 address) override;
    void write8(u32 address, u8 value) override;
    void write16(u32 address, u16 value) override;

    // --- CD ------------------------------------------------------------------
    void insert_disc(std::shared_ptr<const Disc> disc) { drive_.insert(std::move(disc)); }
    // Changes the disc while running (multi-disc games): the tray opens,
    // the disc is swapped, and the tray closes one second later.
    void swap_disc(std::shared_ptr<const Disc> disc) { drive_.swap_disc(std::move(disc)); }
    [[nodiscard]] Cdc&     cdc() noexcept { return cdc_; }
    [[nodiscard]] CdDrive& drive() noexcept { return drive_; }
    [[nodiscard]] GraphicsAsic& graphics() noexcept { return asic_; }
    [[nodiscard]] Pcm& pcm() noexcept { return pcm_; }

    // --- Sound ---------------------------------------------------------------
    // The Mega-CD's audio output (PCM + CD-DA, mixed) at `sub_time` (sub-CPU
    // cycles, non-decreasing), to be added to the Mega Drive's own sound.
    void render_audio(s64 sub_time, s32& left, s32& right);

    // --- Sub CPU ------------------------------------------------------------
    // Advances the sub CPU's timeline (in sub-CPU cycles since reset) to
    // `target`. The CPU executes only while it is running: held in reset or
    // with its bus granted to the main CPU, it is paused and the time passes
    // without it (no catch-up burst when it resumes). Releasing reset
    // restarts it from the vectors at PRG-RAM 0x000000.
    void run_sub_until(s64 target);

    // --- Interrupts towards the main CPU ------------------------------------
    // The expansion port can drive the main 68000's IPL pins. Level requested
    // by the Mega-CD (0 = none); the callback fires whenever it changes.
    // TODO: no gate array source is modelled yet (see main_irq_level()).
    [[nodiscard]] int main_irq_level() const noexcept { return main_irq_level_; }
    void set_main_irq_callback(std::function<void(int)> cb) { main_irq_changed_ = std::move(cb); }
    void acknowledge_main_interrupt(int level);
    // Used by gate array logic (and tests) to request a main-CPU interrupt.
    void request_main_interrupt(int level);

    // Sub-CPU timeline position (what run_sub_until has advanced to).
    [[nodiscard]] s64 sub_time() const noexcept { return sub_time_; }

    [[nodiscard]] M68k&   sub_cpu() noexcept { return sub_cpu_; }
    [[nodiscard]] Bus68k& sub_bus() noexcept { return sub_bus_; }

    // --- Components (inspection, tests, debugger) ----------------------------
    [[nodiscard]] MegaCdRegisters& registers() noexcept { return registers_; }
    [[nodiscard]] ProgramRam& program_ram() noexcept { return *prg_ram_; }
    [[nodiscard]] WordRam&    word_ram() noexcept { return *word_ram_; }

private:
    // The sub CPU's view of the system.
    class SubBus final : public Bus68k {
    public:
        explicit SubBus(MegaCd& cd) : cd_(cd) {}
        u8   read_byte(u32 address) override;
        u16  read_word(u32 address) override;
        void write_byte(u32 address, u8 value) override;
        void write_word(u32 address, u16 value) override;
        [[nodiscard]] u16 peek_word(u32 address) const noexcept override;
    private:
        MegaCd& cd_;
    };

    // Gate array register access with byte lanes (byte reads of some CDC
    // registers have side effects, so they must know which half is read).
    [[nodiscard]] u16 main_register_read(u32 offset, u16 lanes);
    void main_register_write(u32 offset, u16 value, u16 lanes);
    [[nodiscard]] u16 sub_register_read(u32 offset, u16 lanes);
    void sub_register_write(u32 offset, u16 value, u16 lanes);
    void cdc_dma_write(Cdc::Destination destination, u32 address, u16 value);

    // Current sub-CPU time, including the instruction being executed.
    [[nodiscard]] s64 sub_now() const noexcept;
    [[nodiscard]] u16 stopwatch() const noexcept;
    void advance_timer(s64 sub_cycles);

    // Main CPU access helpers; `offset` is relative to the expansion base.
    [[nodiscard]] u16 main_read_memory16(u32 offset);
    void main_write_memory16(u32 offset, u16 value, u16 lanes);

    // Large memories live on the heap (~1 MB in total).
    std::vector<u8> bios_ = std::vector<u8>(kBiosSize, 0xFF);
    std::unique_ptr<ProgramRam> prg_ram_ = std::make_unique<ProgramRam>();
    std::unique_ptr<WordRam>    word_ram_ = std::make_unique<WordRam>();
    std::array<u8, kBackupRamSize> backup_ram_{};

    MegaCdRegisters registers_{*prg_ram_, *word_ram_};
    Cdc     cdc_;
    CdDrive drive_{cdc_};
    Pcm     pcm_;
    GraphicsAsic asic_{*word_ram_, [this] { return registers_.priority_mode(); }};
    SubBus sub_bus_{*this};
    M68k   sub_cpu_{sub_bus_};
    bool   sub_was_in_reset_ = true;
    bool   sub_running_ = false;     // inside sub_cpu_.run_until
    u8     timer_reload_ = 0;        // reg 0x30: INT3 every (n + 1) x 384 cycles, 0 = off
    s64    timer_count_ = 0;         // sub cycles until the next INT3
    s64    stopwatch_base_ = 0;      // sub time when the stopwatch was cleared
    s64    sub_time_ = 0;        // timeline, in sub-CPU cycles
    s64    sub_alignment_ = 0;   // sub_cpu_.cycles() - sub_time_ while running
    int    main_irq_level_ = 0;
    std::function<void(int)> main_irq_changed_;
};

} // namespace md::megacd
