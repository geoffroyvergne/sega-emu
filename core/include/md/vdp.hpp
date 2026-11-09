#pragma once

#include <array>
#include <functional>
#include <span>

#include "md/framebuffer.hpp"
#include "md/memory.hpp"
#include "md/types.hpp"

namespace md {

// Sega 315-5313 Video Display Processor (Mode 5 only).
//
// CPU interface (via Memory, ports relative to 0xC00000):
//   0x00/0x02  data port       — VRAM/CRAM/VSRAM reads & writes at the address register
//   0x04/0x06  control port    — register writes, address/command setup; read = status
//   0x08-0x0E  HV counter
//
// Timing model: the VDP tracks its own position in the frame in master clock
// cycles (3420 per line, 262 NTSC / 313 PAL lines). The scheduler asks for the
// distance to the next timing event, runs the CPU that far, then calls advance().
// Interrupts are reported through a callback so the VDP never depends on the CPU.
//
// Simplified model, accurate enough for a first pass:
//   - Each line starts with active display; HBlank occupies the last
//     kMasterCyclesPerLine - kHBlankStart cycles of the line.
//   - VINT fires at the start of the first line after active display.
//   - HV counter values follow the hardware sequences, but positions within
//     a line are linearly interpolated.
//   - DMA completes instantly (no 68000 bus stall, no DMA busy period).
//   - Each line is rendered in one go at the start of its HBlank, so raster
//     effects work at line granularity.
class Vdp final : public WordDevice {
public:
    static constexpr int kNumRegisters  = 24;
    static constexpr int kVramSize      = 0x10000;  // 64 KB
    static constexpr int kCramEntries   = 64;       // 4 palettes x 16 colours, 9-bit BGR
    static constexpr int kVsramEntries  = 40;       // vertical scroll, 10-bit

    static constexpr int kMasterCyclesPerLine = 3420;
    static constexpr int kHBlankStart         = 2560;  // 320 px * 8 (H40) == 256 px * 10 (H32)

    static constexpr int kVintLevel = 6;
    static constexpr int kHintLevel = 4;

    // Status register bits (control port read).
    struct Status {
        static constexpr u16 FifoEmpty  = 1u << 9;
        static constexpr u16 FifoFull   = 1u << 8;
        static constexpr u16 VintPending = 1u << 7;  // "F" flag
        static constexpr u16 SpriteOverflow  = 1u << 6;
        static constexpr u16 SpriteCollision = 1u << 5;
        static constexpr u16 OddFrame   = 1u << 4;
        static constexpr u16 VBlank     = 1u << 3;
        static constexpr u16 HBlank     = 1u << 2;
        static constexpr u16 DmaBusy    = 1u << 1;
        static constexpr u16 Pal        = 1u << 0;
    };

    // Called whenever the interrupt level the VDP drives (0, 4 or 6) changes.
    using IrqCallback = std::function<void(int level)>;

    // Reads one word from the 68000 bus, for 68000 -> VDP DMA transfers.
    using DmaReadCallback = std::function<u16(u32 address)>;

    Vdp(Region region, Framebuffer& framebuffer);

    void reset();

    // --- CPU bus interface (WordDevice) --------------------------------------
    u16  read(u32 port) override;
    void write(u32 port, u16 value) override;

    // --- Timing --------------------------------------------------------------
    // Master cycles until the next event (HBlank start or end of line).
    [[nodiscard]] int cycles_to_next_event() const noexcept;

    // Advances the VDP by `master_cycles`, processing every event it crosses.
    void advance(int master_cycles);

    // --- Interrupts ----------------------------------------------------------
    void set_irq_callback(IrqCallback callback) { irq_callback_ = std::move(callback); }

    void set_dma_read_callback(DmaReadCallback callback) { dma_read_ = std::move(callback); }

    // The VDP also drives the Z80's /INT line: asserted at the start of
    // VBlank, released one line later.
    using Z80IntCallback = std::function<void(bool asserted)>;
    void set_z80_int_callback(Z80IntCallback callback) { z80_int_ = std::move(callback); }

    // Called by the CPU's interrupt-acknowledge cycle.
    void acknowledge_interrupt(int level);

    [[nodiscard]] int irq_level() const noexcept { return irq_level_; }

    // --- Inspection (debugger, tests, renderer) ------------------------------
    [[nodiscard]] u8 reg(int index) const noexcept { return regs_[static_cast<std::size_t>(index)]; }
    [[nodiscard]] std::span<const u8>  vram()  const noexcept { return vram_; }
    [[nodiscard]] std::span<const u16> cram()  const noexcept { return cram_; }
    [[nodiscard]] std::span<const u16> vsram() const noexcept { return vsram_; }

    [[nodiscard]] u16  status() const noexcept;
    [[nodiscard]] u16  hv_counter() const noexcept;
    [[nodiscard]] int  line() const noexcept       { return line_; }
    [[nodiscard]] int  line_cycle() const noexcept { return line_cycle_; }
    [[nodiscard]] u64  frame() const noexcept      { return frame_; }
    [[nodiscard]] int  lines_per_frame() const noexcept;

    [[nodiscard]] int  active_width()  const noexcept;   // 256 (H32) or 320 (H40)
    [[nodiscard]] int  active_height() const noexcept;   // 224 (V28) or 240 (V30, PAL)
    [[nodiscard]] bool display_enabled() const noexcept { return (regs_[1] & 0x40) != 0; }
    [[nodiscard]] bool vint_enabled()    const noexcept { return (regs_[1] & 0x20) != 0; }
    [[nodiscard]] bool hint_enabled()    const noexcept { return (regs_[0] & 0x10) != 0; }
    [[nodiscard]] bool dma_enabled()     const noexcept { return (regs_[1] & 0x10) != 0; }
    [[nodiscard]] u8   auto_increment()  const noexcept { return regs_[15]; }
    [[nodiscard]] u16  address() const noexcept { return address_; }

    // Converts a 9-bit CRAM colour (----BBB-GGG-RRR-) to ARGB8888.
    [[nodiscard]] static Framebuffer::Pixel cram_to_argb(u16 color) noexcept;

private:
    // Target of data port accesses, selected by CD3-CD0 of the code register.
    enum class Target : u8 {
        VramRead   = 0b0000,
        VramWrite  = 0b0001,
        CramWrite  = 0b0011,
        VsramRead  = 0b0100,
        VsramWrite = 0b0101,
        CramRead   = 0b1000,
    };

    enum class Plane : u8 { A = 0, B = 1 };

    void write_control(u16 value);
    void write_data(u16 value);
    void write_target(u16 value);  // store at address_ per code_, then auto-increment
    u16  read_data();
    u16  read_status();
    void write_register(int index, u8 value);

    // DMA (reg 23 bits 7-6: 0x = 68000 -> VDP, 10 = VRAM fill, 11 = VRAM copy).
    [[nodiscard]] u32 dma_length() const noexcept;
    void finish_dma(u32 source_low) noexcept;
    void run_dma_transfer();
    void run_dma_fill(u16 value);
    void run_dma_copy();

    void on_line_start();
    void on_hblank_start();
    void update_irq();

    // Rendering. Line buffers hold one byte per pixel:
    //   bit 7 = priority, bits 5-4 = palette, bits 3-0 = colour (0 = transparent).
    using LineBuffer = std::array<u8, kMaxScreenWidth>;
    void render_line();
    void render_plane(Plane plane, LineBuffer& out) const;
    void render_window(LineBuffer& out) const;
    void render_sprites(LineBuffer& out);
    [[nodiscard]] bool window_covers(int x) const noexcept;
    [[nodiscard]] u8   tile_pixel(u16 entry, int x, int y) const noexcept;
    [[nodiscard]] u16  vram_word(u32 address) const noexcept;

    Region region_;
    Framebuffer& framebuffer_;

    std::array<u8,  kNumRegisters> regs_{};
    std::array<u8,  kVramSize>     vram_{};
    std::array<u16, kCramEntries>  cram_{};
    std::array<u16, kVsramEntries> vsram_{};

    // Command state (set through the control port).
    u16  address_       = 0;      // 16-bit VRAM/CRAM/VSRAM address (A15-A0)
    u8   code_          = 0;      // CD5-CD0
    bool write_pending_ = false;  // first half of a 2-word command received
    bool dma_fill_pending_ = false;  // fill armed; starts on the next data port write
    DmaReadCallback dma_read_;

    // Interrupt state.
    bool vint_pending_  = false;
    bool hint_pending_  = false;
    int  hint_counter_  = 0;
    int  irq_level_     = 0;
    IrqCallback irq_callback_;
    Z80IntCallback z80_int_;

    // Sprite status flags (cleared by reading the status register) and the
    // dot-overflow state of the previous line (enables X=0 masking).
    bool sprite_overflow_  = false;
    bool sprite_collision_ = false;
    bool prev_line_dot_overflow_ = false;

    // Beam position.
    int  line_       = 0;
    int  line_cycle_ = 0;
    u64  frame_      = 0;
};

static_assert(sizeof(std::array<u8,  Vdp::kVramSize>)     == 64 * 1024);
static_assert(sizeof(std::array<u16, Vdp::kCramEntries>)  == 128);
static_assert(sizeof(std::array<u16, Vdp::kVsramEntries>) == 80);

} // namespace md
