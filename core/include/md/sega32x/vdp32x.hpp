#pragma once

#include <array>
#include <functional>
#include <span>
#include <vector>

#include "md/framebuffer.hpp"
#include "md/types.hpp"

namespace md::sega32x {

// The 32X VDP: two 128 KB frame buffers (DRAM), the 512-byte palette (256
// entries), the registers that control them, and the bitmap renderer that
// draws the 32X picture over the Mega Drive's.
//
// Frame buffers: the CPUs draw into one while the VDP displays the other.
// FS (register 0x0A bit 0) selects the one the CPUs see: dram[FS] is drawn
// into, dram[FS ^ 1] is shown. A new FS takes effect at the next VBlank (at
// once if written during VBlank or while the 32X picture is blank, mode 0),
// and reading FS returns the current one, so software waits for the flip by
// polling it.
//
// Each frame buffer is visible twice: normally, and as the "overwrite image",
// where writes skip zero bytes (transparent pixels in packed-pixel mode).
//
// Registers (SH-2 0x4100+, 68000 0xA15180+):
//   00  bitmap mode   PAL (15, R: 1 = NTSC) | PRI (7) | 240 (6) | mode (1-0)
//   02  screen shift  bit 0
//   04  autofill length (7-0): length + 1 words
//   06  autofill start address (word address in the frame buffer)
//   08  autofill data: writing it fills (within the 256-word line)
//   0A  frame buffer control  VBLK (15, R) | HBLK (14, R) | PEN (13, R)
//                             | FEN (1, R) | FS (0)
//
// Picture: 320 pixels per line. Colours are 15 bits plus a priority bit,
// P (15) | B (14-10) | G (9-5) | R (4-0), both in the palette and in
// direct-colour pixels. P (the "through" bit) is used when mixing with the
// Mega Drive.
//
// Each displayed frame buffer starts with a line table: word n (byte 2n) is
// the word address, within that frame buffer, of line n's pixel data. Line
// data then depends on the mode (register 00 bits 1-0):
//   0  blank         no 32X picture
//   1  packed pixel  one byte per pixel, a palette index; screen shift = 1
//                    starts the line one byte later (one pixel left)
//   2  direct color  one word per pixel, the colour itself
//   3  run length    words of (run length - 1) << 8 | palette index, until
//                    the 320 pixels are filled
// Line data wraps around the 64K words of the frame buffer.
//
// Mixing with the Mega Drive picture, per pixel (as PicoDrive models it): where the Mega Drive shows its backdrop, the 32X
// pixel shows. Elsewhere, the 32X pixel shows if its P bit differs from the
// PRI bit (register 00 bit 7), otherwise the Mega Drive pixel stays. So with
// PRI = 0, the Mega Drive is in front except where P = 1; with PRI = 1, the
// 32X is in front except where P = 1.
class Vdp32x {
public:
    static constexpr u32 kFrameBufferSize = 128 * 1024;
    static constexpr int kPaletteEntries = 256;
    static constexpr u16 kVblk = 0x8000;
    static constexpr u16 kHblk = 0x4000;
    static constexpr u16 kPen  = 0x2000;
    static constexpr u16 kFs   = 0x0001;
    static constexpr int kWidth = 320;
    static constexpr u16 kPriorityBit = 0x8000;   // P, in a colour
    static constexpr u16 kPri = 0x0080;           // PRI, register 00

    enum class Mode { Blank = 0, PackedPixel = 1, DirectColor = 2, RunLength = 3 };

    Vdp32x() { reset(); }

    void reset();

    void set_ntsc(bool ntsc) noexcept { ntsc_ = ntsc; }

    // --- Registers (offset 0x00-0x0F) ---
    [[nodiscard]] u16 read_register(u32 offset) const noexcept;
    void write_register(u32 offset, u16 value, u16 lanes);

    // --- Palette (offset 0x000-0x1FF, one 16-bit entry per even offset) ---
    [[nodiscard]] u16 read_palette(u32 offset) const noexcept { return palette_[(offset >> 1) & 0xFF]; }
    void write_palette(u32 offset, u16 value, u16 lanes);

    // --- Frame buffer (offset 0x00000-0x3FFFF: bit 17 = overwrite image) ---
    [[nodiscard]] u16 read_frame_buffer(u32 offset) const noexcept;
    void write_frame_buffer(u32 offset, u16 value, u16 lanes);

    // --- Picture ---
    [[nodiscard]] Mode mode() const noexcept { return static_cast<Mode>(mode_ & 3); }
    [[nodiscard]] bool pri() const noexcept { return (mode_ & kPri) != 0; }

    // Line `line` of the displayed frame buffer as 15-bit colours (palette
    // already applied). Returns false in blank mode.
    bool render_line(int line, std::array<u16, kWidth>& out) const noexcept;

    // Draws line `line` over the Mega Drive's `row` (ARGB), following the
    // mixing rule above. `md_backdrop[x]` is 1 where the Mega Drive shows
    // its backdrop. Only the first row.size() pixels are drawn (H32 shows
    // the left 256 of the 32X's 320).
    void compose_line(int line, std::span<Framebuffer::Pixel> row, std::span<const u8> md_backdrop) const noexcept;

    // A 15-bit colour as ARGB8888 (5 bits -> 8 by replicating the top bits).
    [[nodiscard]] static constexpr Framebuffer::Pixel to_argb(u16 color) noexcept {
        const auto expand = [](u32 v) { return (v << 3) | (v >> 2); };
        return 0xFF00'0000u | (expand(color & 0x1Fu) << 16) | (expand((color >> 5) & 0x1Fu) << 8) | expand((color >> 10) & 0x1Fu);
    }

    // VBlank from the Mega Drive VDP's timing. Its start applies a pending FS
    // and calls the V interrupt callback.
    void set_vblank(bool active);
    void set_vint_callback(std::function<void()> cb) { vint_ = std::move(cb); }
    // HBlank, for the HBLK status bit.
    void set_hblank(bool active) noexcept { hblank_ = active; }

    // --- Inspection (renderer, tests) ---
    [[nodiscard]] u8 frame_buffer_select() const noexcept { return fs_; }
    [[nodiscard]] const std::vector<u8>& frame_buffer(int index) const noexcept { return dram_[static_cast<std::size_t>(index & 1)]; }
    [[nodiscard]] const std::vector<u8>& displayed_frame_buffer() const noexcept { return dram_[fs_ ^ 1u]; }
    [[nodiscard]] u16 palette(int index) const noexcept { return palette_[static_cast<std::size_t>(index & 0xFF)]; }
    [[nodiscard]] bool in_vblank() const noexcept { return vblank_; }

private:
    void autofill(u16 data);
    [[nodiscard]] u16 display_word(u32 word_address) const noexcept;
    [[nodiscard]] std::vector<u8>& cpu_dram() noexcept { return dram_[fs_]; }

    std::array<std::vector<u8>, 2> dram_;
    std::array<u16, kPaletteEntries> palette_{};
    u16 mode_ = 0;
    u16 shift_ = 0;
    u16 fill_length_ = 0;
    u16 fill_address_ = 0;
    u16 fill_data_ = 0;
    u8  fs_ = 0;
    u8  pending_fs_ = 0;
    bool fs_pending_ = false;
    bool vblank_ = false;
    bool hblank_ = false;
    bool ntsc_ = true;
    std::function<void()> vint_;
};

} // namespace md::sega32x
