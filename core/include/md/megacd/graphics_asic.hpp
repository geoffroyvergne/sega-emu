#pragma once

#include <functional>

#include "md/megacd/word_ram.hpp"
#include "md/types.hpp"

namespace md::megacd {

// Mega-CD graphics ASIC: hardware rotation / scaling ("stamp" rendering).
//
// Source: a STAMP MAP in Word RAM, a square grid of 16-bit entries, each
// naming a STAMP (a 16x16 or 32x32 dot, 4 bpp bitmap, also in Word RAM),
// with a flip and a 90-degree-step rotation. The map covers 256x256 dots
// (1 screen) or 4096x4096 dots (16x16 screens); outside it, the map either
// repeats or reads as transparent.
//
// Transform: for each output line, a TRACE VECTOR (4 words in Word RAM) gives
// the source point of the line's first dot and the source step per dot:
//     X, Y  : unsigned 13.3 fixed point (dots)
//     DX, DY: signed 5.11 fixed point (dots per output dot)
// Any affine transform (rotation, scaling, shear, perspective-by-line) is
// expressed by the software choosing these per line; the ASIC only walks
// the line and samples one source dot per output dot (nearest neighbour).
//
// Destination: the IMAGE BUFFER in Word RAM, laid out in 8x8 cells, column
// by column ("V cell size" cells per column), i.e. directly usable as VDP
// tiles. A dot offset (0-7, 0-7) shifts the drawing inside the first cell.
// Writes obey the priority mode (PM) of the memory mode register.
//
// Registers (sub CPU 0xFF8000 + offset):
//   58  stamp data size: GRON (15, R: busy) | STS (2: map size) | SMS (1: stamp size) | RPT (0: repeat)
//   5A  stamp map base address    (byte address = value x 4, aligned to the map size)
//   5C  image buffer V cell size  (cells per column - 1, 0-31)
//   5E  image buffer start address (byte address = value x 4, cell aligned)
//   60  image buffer offset       (bits 5-3: Y dot offset, bits 2-0: X dot offset)
//   62  image buffer H dot size   (1-511)
//   64  image buffer V dot size   (1-255 lines)
//   66  trace vector base address (byte address = value x 4); WRITING STARTS the operation
//
// The ASIC works in 2M Word RAM mode with Word RAM assigned to the sub CPU.
// When the operation completes, GRON clears and INT1 is raised.
//
// Simplifications (TODO): the whole image is rendered when the operation
// starts and GRON stays set for an estimated duration (kSubCyclesPerDot per
// output dot); on hardware, Word RAM changes during the operation would be
// seen by it.
class GraphicsAsic {
public:
    // Estimated cost per output dot, in sub-CPU cycles (not measured on hardware).
    static constexpr s64 kSubCyclesPerDot = 4;

    // Priority modes (memory mode register, PM1-PM0).
    enum class Priority : u8 { Off = 0, Underwrite = 1, Overwrite = 2 };

    GraphicsAsic(WordRam& word_ram, std::function<u8()> priority_mode)
        : word_ram_(word_ram), priority_mode_(std::move(priority_mode)) {}

    void reset();

    // Register access (offsets 0x58-0x67; lanes as in MegaCdRegisters).
    [[nodiscard]] u16 read(u32 offset) const noexcept;
    void write(u32 offset, u16 value, u16 lanes);

    // Advances time (sub-CPU cycles): ends the operation when it's done.
    void advance(s64 sub_cycles);

    // Called once per completed operation (wired to INT1).
    void set_done_callback(std::function<void()> cb) { done_ = std::move(cb); }

    [[nodiscard]] bool busy() const noexcept { return busy_cycles_ > 0; }

    // --- The pipeline, exposed for tests ------------------------------------
    [[nodiscard]] int stamp_size() const noexcept { return (stamp_size_reg_ & 0x02) ? 32 : 16; }
    [[nodiscard]] int map_size_dots() const noexcept { return (stamp_size_reg_ & 0x04) ? 4096 : 256; }
    [[nodiscard]] bool repeat() const noexcept { return (stamp_size_reg_ & 0x01) != 0; }
    [[nodiscard]] u32 map_base() const noexcept;
    [[nodiscard]] u32 image_base() const noexcept { return (u32{image_base_reg_} << 2) & 0x3'FFE0; }
    [[nodiscard]] u32 trace_base() const noexcept { return (u32{trace_base_reg_} << 2) & 0x3'FFF8; }

    // Source dot at map coordinates (whole dots, any value; wrapped or
    // clipped per RPT). Returns the 4-bit colour (0 = transparent).
    [[nodiscard]] u8 sample(u32 x, u32 y) const;

    // Stamp attribute transform: dot (x, y) of a stamp as displayed maps to
    // dot (sx, sy) of its stored bitmap. hflip, then rotation in 90-degree
    // steps (rot = 0..3).
    static void stamp_transform(int size, bool hflip, int rot, int x, int y, int& sx, int& sy) noexcept;

    // Byte address (and nibble) of image-buffer dot (x, y), offset included.
    [[nodiscard]] u32 image_address(u32 x, u32 y) const noexcept;

    // Runs the whole operation now (also what a write to register 0x66 does,
    // before the busy period).
    void render();

private:
    [[nodiscard]] u16 read_word(u32 address) const;
    [[nodiscard]] u8 read_nibble(u32 address, bool high) const;
    void write_dot(u32 x, u32 y, u8 colour);

    WordRam& word_ram_;
    std::function<u8()> priority_mode_;
    std::function<void()> done_;

    u16 stamp_size_reg_ = 0;
    u16 map_base_reg_ = 0;
    u16 v_cell_size_ = 0;
    u16 image_base_reg_ = 0;
    u16 image_offset_ = 0;
    u16 h_dots_ = 0;
    u16 v_dots_ = 0;
    u16 trace_base_reg_ = 0;

    s64 busy_cycles_ = 0;
};

} // namespace md::megacd
