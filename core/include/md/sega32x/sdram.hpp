#pragma once

#include <array>
#include <span>

#include "md/types.hpp"

namespace md::sega32x {

// The 32X's 256 KB SDRAM: main memory for both SH-2s (program, data, stacks).
// The 68000 cannot see it; it reaches the SH-2s through the communication
// registers and the DREQ FIFO instead.
//
// SH-2 address map (both CPUs share the same physical memory):
//   0x06000000-0x0603FFFF   SDRAM, through the SH-2 cache
//   0x26000000-0x2603FFFF   the same, cache-through
//   mirrored every 256 KB up to 0x07FFFFFF (0x27FFFFFF)
//
// The SH-2 is big-endian, like the 68000: bytes are stored in bus order and
// 16/32-bit values are composed with shifts (host byte order independent).
// `offset` is relative to the start of SDRAM; Sega32x::Sh2Bus decodes SH-2
// addresses down to it.
class Sdram {
public:
    static constexpr u32 kSize    = 256 * 1024;
    static constexpr u32 kSh2Base = 0x0600'0000;
    static constexpr u32 kMask    = kSize - 1;

    void reset() noexcept { data_.fill(0); }

    [[nodiscard]] u8 read8(u32 offset) const noexcept { return data_[offset & kMask]; }
    [[nodiscard]] u16 read16(u32 offset) const noexcept {
        const u32 a = offset & kMask & ~1u;
        return static_cast<u16>((data_[a] << 8) | data_[a + 1]);
    }
    [[nodiscard]] u32 read32(u32 offset) const noexcept {
        const u32 a = offset & kMask & ~3u;
        return (u32{data_[a]} << 24) | (u32{data_[a + 1]} << 16) | (u32{data_[a + 2]} << 8) | data_[a + 3];
    }

    void write8(u32 offset, u8 value) noexcept { data_[offset & kMask] = value; }
    void write16(u32 offset, u16 value) noexcept {
        const u32 a = offset & kMask & ~1u;
        data_[a]     = static_cast<u8>(value >> 8);
        data_[a + 1] = static_cast<u8>(value);
    }
    void write32(u32 offset, u32 value) noexcept {
        const u32 a = offset & kMask & ~3u;
        data_[a]     = static_cast<u8>(value >> 24);
        data_[a + 1] = static_cast<u8>(value >> 16);
        data_[a + 2] = static_cast<u8>(value >> 8);
        data_[a + 3] = static_cast<u8>(value);
    }

    [[nodiscard]] std::span<const u8> data() const noexcept { return data_; }

private:
    std::array<u8, kSize> data_{};
};

} // namespace md::sega32x
