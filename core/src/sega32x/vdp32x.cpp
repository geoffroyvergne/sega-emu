#include "md/sega32x/vdp32x.hpp"

#include <algorithm>

namespace md::sega32x {

namespace {

constexpr u16 merge(u16 old_value, u16 value, u16 lanes) noexcept {
    return static_cast<u16>((old_value & ~lanes) | (value & lanes));
}

constexpr u32 kOverwriteBit = 0x2'0000;

} // namespace

void Vdp32x::reset() {
    for (auto& d : dram_) d.assign(kFrameBufferSize, 0);
    palette_.fill(0);
    mode_ = shift_ = 0;
    fill_length_ = fill_address_ = fill_data_ = 0;
    fs_ = pending_fs_ = 0;
    fs_pending_ = false;
    vblank_ = false;
    hblank_ = false;
}

u16 Vdp32x::read_register(u32 offset) const noexcept {
    switch (offset & 0x0E) {
    case 0x00: return static_cast<u16>(mode_ | (ntsc_ ? 0x8000 : 0));
    case 0x02: return shift_;
    case 0x04: return fill_length_;
    case 0x06: return fill_address_;
    case 0x08: return fill_data_;
    case 0x0A:
        // PEN: the palette is always accessible here (no access timing).
        return static_cast<u16>((vblank_ ? kVblk : 0) | (hblank_ ? kHblk : 0) | kPen | fs_);
    default:   return 0;
    }
}

void Vdp32x::write_register(u32 offset, u16 value, u16 lanes) {
    switch (offset & 0x0E) {
    case 0x00: mode_ = merge(mode_, value, lanes) & 0x00C3; break;
    case 0x02: shift_ = merge(shift_, value, lanes) & 0x0001; break;
    case 0x04: fill_length_ = merge(fill_length_, value, lanes) & 0x00FF; break;
    case 0x06: fill_address_ = merge(fill_address_, value, lanes); break;
    case 0x08:
        fill_data_ = merge(fill_data_, value, lanes);
        autofill(fill_data_);
        break;
    case 0x0A: {
        const u8 fs = static_cast<u8>(merge(fs_, value, lanes) & kFs);
        // Immediate while nothing is being displayed (VBlank, or the 32X
        // picture blanked): games rely on it to set up both buffers in a
        // row while the screen is off.
        if (vblank_ || mode() == Mode::Blank) {
            fs_ = fs;
            fs_pending_ = false;
        } else {
            pending_fs_ = fs;
            fs_pending_ = true;
        }
        break;
    }
    default: break;
    }
}

void Vdp32x::autofill(u16 data) {
    // Fills length + 1 words; the address wraps within its 256-word line.
    auto& dram = cpu_dram();
    u16 address = fill_address_;
    for (u32 i = 0; i <= fill_length_; ++i) {
        const u32 byte = (u32{address} * 2) & (kFrameBufferSize - 1);
        dram[byte] = static_cast<u8>(data >> 8);
        dram[byte + 1] = static_cast<u8>(data);
        address = static_cast<u16>((address & 0xFF00) | ((address + 1) & 0x00FF));
    }
    fill_address_ = address;
}

void Vdp32x::write_palette(u32 offset, u16 value, u16 lanes) {
    auto& entry = palette_[(offset >> 1) & 0xFF];
    entry = merge(entry, value, lanes);
}

u16 Vdp32x::read_frame_buffer(u32 offset) const noexcept {
    const auto& dram = dram_[fs_];
    const u32 a = offset & (kFrameBufferSize - 2);
    return static_cast<u16>((dram[a] << 8) | dram[a + 1]);
}

void Vdp32x::write_frame_buffer(u32 offset, u16 value, u16 lanes) {
    auto& dram = cpu_dram();
    const u32 a = offset & (kFrameBufferSize - 2);
    const bool overwrite = (offset & kOverwriteBit) != 0;
    const u8 hi = static_cast<u8>(value >> 8);
    const u8 lo = static_cast<u8>(value);
    if ((lanes & 0xFF00) && !(overwrite && hi == 0)) dram[a] = hi;
    if ((lanes & 0x00FF) && !(overwrite && lo == 0)) dram[a + 1] = lo;
}

// ===========================================================================
// Picture
// ===========================================================================

u16 Vdp32x::display_word(u32 word_address) const noexcept {
    const auto& dram = displayed_frame_buffer();
    const u32 a = (word_address & 0xFFFF) * 2;
    return static_cast<u16>((dram[a] << 8) | dram[a + 1]);
}

bool Vdp32x::render_line(int line, std::array<u16, kWidth>& out) const noexcept {
    const Mode m = mode();
    if (m == Mode::Blank) return false;
    const u32 start = display_word(static_cast<u32>(line) & 0xFF);   // line table entry
    switch (m) {
    case Mode::PackedPixel: {
        const auto& dram = displayed_frame_buffer();
        const u32 byte = start * 2 + (shift_ & 1);
        for (u32 x = 0; x < kWidth; ++x) out[x] = palette_[dram[(byte + x) & (kFrameBufferSize - 1)]];
        break;
    }
    case Mode::DirectColor:
        for (u32 x = 0; x < kWidth; ++x) out[x] = display_word(start + x);
        break;
    case Mode::RunLength: {
        u32 address = start;
        u32 x = 0;
        while (x < kWidth) {
            const u16 run = display_word(address++);
            const u16 color = palette_[run & 0xFF];
            for (u32 n = (run >> 8) + 1u; n > 0 && x < kWidth; --n) out[x++] = color;
        }
        break;
    }
    case Mode::Blank:
        break;
    }
    return true;
}

void Vdp32x::compose_line(int line, std::span<Framebuffer::Pixel> row, std::span<const u8> md_backdrop) const noexcept {
    std::array<u16, kWidth> pixels;
    if (!render_line(line, pixels)) return;
    const u16 front = pri() ? 0 : kPriorityBit;   // P value that puts the 32X in front
    const std::size_t width = std::min<std::size_t>(row.size(), kWidth);
    for (std::size_t x = 0; x < width; ++x) {
        const u16 c = pixels[x];
        const bool backdrop = x < md_backdrop.size() && md_backdrop[x];
        if (backdrop || (c & kPriorityBit) == front) row[x] = to_argb(c);
    }
}

void Vdp32x::set_vblank(bool active) {
    if (active == vblank_) return;
    vblank_ = active;
    if (!active) return;
    if (fs_pending_) {
        fs_ = pending_fs_;
        fs_pending_ = false;
    }
    if (vint_) vint_();
}

} // namespace md::sega32x
