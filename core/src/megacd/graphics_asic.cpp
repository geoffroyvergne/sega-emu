#include "md/megacd/graphics_asic.hpp"

#include <algorithm>

namespace md::megacd {

void GraphicsAsic::reset() {
    stamp_size_reg_ = map_base_reg_ = v_cell_size_ = image_base_reg_ = 0;
    image_offset_ = h_dots_ = v_dots_ = trace_base_reg_ = 0;
    busy_cycles_ = 0;
}

// ===========================================================================
// Registers
// ===========================================================================

u16 GraphicsAsic::read(u32 offset) const noexcept {
    switch (offset & 0xFE) {
    case 0x58: return static_cast<u16>((busy() ? 0x8000 : 0) | (stamp_size_reg_ & 0x07));
    case 0x5A: return map_base_reg_;
    case 0x5C: return v_cell_size_;
    case 0x5E: return image_base_reg_;
    case 0x60: return image_offset_;
    case 0x62: return h_dots_;
    case 0x64: return v_dots_;
    case 0x66: return trace_base_reg_;
    default:   return 0;
    }
}

void GraphicsAsic::write(u32 offset, u16 value, u16 lanes) {
    const auto merge = [&](u16 old, u16 mask) {
        return static_cast<u16>(((old & ~lanes) | (value & lanes)) & mask);
    };
    switch (offset & 0xFE) {
    case 0x58: stamp_size_reg_ = merge(stamp_size_reg_, 0x0007); break;
    case 0x5A: map_base_reg_   = merge(map_base_reg_, 0xFFE0); break;
    case 0x5C: v_cell_size_    = merge(v_cell_size_, 0x001F); break;
    case 0x5E: image_base_reg_ = merge(image_base_reg_, 0xFFF8); break;
    case 0x60: image_offset_   = merge(image_offset_, 0x003F); break;
    case 0x62: h_dots_         = merge(h_dots_, 0x01FF); break;
    case 0x64: v_dots_         = merge(v_dots_, 0x00FF); break;
    case 0x66:
        trace_base_reg_ = merge(trace_base_reg_, 0xFFFE);
        render();
        busy_cycles_ = std::max<s64>(1, s64{h_dots_} * v_dots_ * kSubCyclesPerDot);
        break;
    default: break;
    }
}

void GraphicsAsic::advance(s64 sub_cycles) {
    if (busy_cycles_ <= 0) return;
    busy_cycles_ -= sub_cycles;
    if (busy_cycles_ <= 0) {
        busy_cycles_ = 0;
        if (done_) done_();  // GRON clears, INT1
    }
}

// ===========================================================================
// Pipeline
// ===========================================================================

u32 GraphicsAsic::map_base() const noexcept {
    // The map is aligned to its own size in Word RAM.
    const u32 stamps_per_side = static_cast<u32>(map_size_dots() / stamp_size());
    const u32 map_bytes = stamps_per_side * stamps_per_side * 2;
    return (u32{map_base_reg_} << 2) & ~(map_bytes - 1) & (WordRam::kSize - 1);
}

u16 GraphicsAsic::read_word(u32 address) const {
    return word_ram_.sub_read16(address & (WordRam::kSize - 2));
}

u8 GraphicsAsic::read_nibble(u32 address, bool high) const {
    const u8 byte = word_ram_.sub_read8(address & (WordRam::kSize - 1));
    return high ? static_cast<u8>(byte >> 4) : static_cast<u8>(byte & 0x0F);
}

void GraphicsAsic::stamp_transform(int size, bool hflip, int rot, int x, int y, int& sx, int& sy) noexcept {
    // Rotation first undone, then the flip: the displayed dot (x, y) comes
    // from the stored dot (sx, sy). Rotation direction for rot = 1/3 follows
    // the "counterclockwise" wording of the hardware manual with Y pointing
    // down; 0 and 2 (and the flip) are unambiguous.
    const int m = size - 1;
    switch (rot & 3) {
    case 0: sx = x;     sy = y;     break;
    case 1: sx = m - y; sy = x;     break;
    case 2: sx = m - x; sy = m - y; break;
    case 3: sx = y;     sy = m - x; break;
    }
    if (hflip) sx = m - sx;
}

u8 GraphicsAsic::sample(u32 x, u32 y) const {
    const auto map = static_cast<u32>(map_size_dots());
    if (repeat()) {
        x &= map - 1;
        y &= map - 1;
    } else if (x >= map || y >= map) {
        return 0;  // outside the map: transparent
    }

    // Map entry: H (15) | ROT (14-13) | stamp number (10-0).
    const auto size = static_cast<u32>(stamp_size());
    const u32 stamps_per_side = map / size;
    const u16 entry = read_word(map_base() + ((y / size) * stamps_per_side + x / size) * 2);
    u32 number = entry & 0x07FF;
    if (size == 32) number &= ~3u;  // 32x32 stamps occupy 4 numbers
    if (number == 0) return 0;      // stamp 0: transparent

    int sx = 0, sy = 0;
    stamp_transform(static_cast<int>(size), (entry & 0x8000) != 0, (entry >> 13) & 3,
                    static_cast<int>(x % size), static_cast<int>(y % size), sx, sy);

    // Stamp bitmap: 8x8 cells of 32 bytes, column by column (like sprites);
    // stamp N starts at N x 128 bytes.
    const u32 cells = size / 8;
    const u32 cell = static_cast<u32>(sx / 8) * cells + static_cast<u32>(sy / 8);
    const u32 address = number * 128 + cell * 32 + static_cast<u32>(sy % 8) * 4 + static_cast<u32>(sx % 8) / 2;
    return read_nibble(address, (sx & 1) == 0);
}

u32 GraphicsAsic::image_address(u32 x, u32 y) const noexcept {
    x += image_offset_ & 7;
    y += (image_offset_ >> 3) & 7;
    const u32 cells_per_column = u32{v_cell_size_} + 1;
    const u32 cell = (x / 8) * cells_per_column + (y / 8);
    return image_base() + cell * 32 + (y % 8) * 4 + (x % 8) / 2;
}

void GraphicsAsic::write_dot(u32 x, u32 y, u8 colour) {
    const u32 address = image_address(x, y) & (WordRam::kSize - 1);
    const bool high = (((image_offset_ & 7) + x) & 1) == 0;
    const u8 byte = word_ram_.sub_read8(address);
    const u8 old = high ? static_cast<u8>(byte >> 4) : static_cast<u8>(byte & 0x0F);

    switch (static_cast<Priority>(priority_mode_ ? (priority_mode_() & 3) : 0)) {
    case Priority::Underwrite: if (old != 0) return; break;     // only into empty dots
    case Priority::Overwrite:  if (colour == 0) return; break;  // transparent dots keep the old one
    default: break;
    }
    const u8 updated = high ? static_cast<u8>((byte & 0x0F) | (colour << 4))
                            : static_cast<u8>((byte & 0xF0) | colour);
    word_ram_.sub_write8(address, updated);
}

void GraphicsAsic::render() {
    if (word_ram_.mode() != WordRam::Mode::TwoMeg) return;  // the ASIC needs 2M mode

    for (u32 line = 0; line < v_dots_; ++line) {
        const u32 vector = trace_base() + line * 8;
        // 13.3 start point -> 21.11 accumulator, so the 5.11 steps add directly.
        u32 x = u32{read_word(vector + 0)} << 8;
        u32 y = u32{read_word(vector + 2)} << 8;
        const auto dx = static_cast<u32>(static_cast<s32>(static_cast<s16>(read_word(vector + 4))));
        const auto dy = static_cast<u32>(static_cast<s32>(static_cast<s16>(read_word(vector + 6))));

        for (u32 dot = 0; dot < h_dots_; ++dot) {
            // The integer part wraps at 13 bits (0-8191 dots).
            write_dot(dot, line, sample((x >> 11) & 0x1FFF, (y >> 11) & 0x1FFF));
            x += dx;
            y += dy;
        }
    }
}

} // namespace md::megacd
