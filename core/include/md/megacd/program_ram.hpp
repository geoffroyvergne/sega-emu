#pragma once

#include <array>

#include "md/types.hpp"

namespace md::megacd {

// 512 KB Program RAM (PRG-RAM): the sub CPU's main memory.
//
//   Sub CPU:   0x000000-0x07FFFF, all of it. Writes below the write-protect
//              boundary (WP x 0x200 bytes, set by the main CPU) are ignored,
//              which shields the BIOS's sub-CPU code and data from a game.
//   Main CPU:  a 128 KB window at expansion base + 0x020000, showing bank
//              BK (0-3). Only usable while the sub CPU is held (bus request
//              granted or reset); the gate array decides that, not this class.
class ProgramRam {
public:
    static constexpr u32 kSize     = 512 * 1024;
    static constexpr u32 kBankSize = 128 * 1024;
    static constexpr u32 kProtectUnit = 0x200;  // WP granularity in bytes

    void reset() {
        data_.fill(0);
        write_protect_ = 0;
        main_bank_ = 0;
    }

    // --- Sub CPU (offset 0x00000-0x7FFFF) ------------------------------------
    [[nodiscard]] u8 sub_read8(u32 offset) const noexcept { return data_[offset & (kSize - 1)]; }
    [[nodiscard]] u16 sub_read16(u32 offset) const noexcept {
        offset &= kSize - 2;
        return static_cast<u16>((data_[offset] << 8) | data_[offset + 1]);
    }
    void sub_write8(u32 offset, u8 value) noexcept {
        offset &= kSize - 1;
        if (!is_protected(offset)) data_[offset] = value;
    }
    void sub_write16(u32 offset, u16 value) noexcept {
        offset &= kSize - 2;
        if (is_protected(offset)) return;
        data_[offset]     = static_cast<u8>(value >> 8);
        data_[offset + 1] = static_cast<u8>(value);
    }

    // --- Main CPU window (offset 0x00000-0x1FFFF within the window) ----------
    // Not subject to write protection.
    [[nodiscard]] u8 main_read8(u32 offset) const noexcept { return data_[main_address(offset)]; }
    [[nodiscard]] u16 main_read16(u32 offset) const noexcept {
        const u32 a = main_address(offset & ~1u);
        return static_cast<u16>((data_[a] << 8) | data_[a + 1]);
    }
    void main_write8(u32 offset, u8 value) noexcept { data_[main_address(offset)] = value; }
    void main_write16(u32 offset, u16 value) noexcept {
        const u32 a = main_address(offset & ~1u);
        data_[a]     = static_cast<u8>(value >> 8);
        data_[a + 1] = static_cast<u8>(value);
    }

    // --- Configuration (driven by the gate array registers) -------------------
    void set_write_protect(u8 wp) noexcept { write_protect_ = wp; }
    [[nodiscard]] u8 write_protect() const noexcept { return write_protect_; }
    [[nodiscard]] u32 protected_bytes() const noexcept { return u32{write_protect_} * kProtectUnit; }

    void set_main_bank(u8 bank) noexcept { main_bank_ = bank & 3; }
    [[nodiscard]] u8 main_bank() const noexcept { return main_bank_; }

private:
    [[nodiscard]] bool is_protected(u32 offset) const noexcept { return offset < protected_bytes(); }
    [[nodiscard]] u32 main_address(u32 offset) const noexcept {
        return (u32{main_bank_} * kBankSize) | (offset & (kBankSize - 1));
    }

    std::array<u8, kSize> data_{};
    u8 write_protect_ = 0;
    u8 main_bank_ = 0;
};

} // namespace md::megacd
