#pragma once

#include <array>

#include "md/types.hpp"

namespace md::megacd {

// 256 KB Word RAM, shared between the main and sub CPUs, built from two
// 128 KB banks. The MODE bit (sub CPU, gate array reg 2) selects how it is
// shared:
//
//   2M mode (MODE = 0): the whole 256 KB belongs to ONE CPU at a time.
//     RET = 1: main CPU owns it.  Main writes DMNA = 1 to give it to the
//     sub CPU (RET -> 0, DMNA reads 1). The sub CPU writes RET = 1 to give it
//     back (DMNA -> 0). Consecutive words alternate between the two physical
//     banks (word 0 -> bank 0, word 1 -> bank 1, ...).
//       main: expansion base + 0x200000-0x23FFFF   sub: 0x080000-0x0BFFFF
//
//   1M mode (MODE = 1): each CPU owns one 128 KB bank, and the banks are
//     swapped ("ping-pong") so one CPU can fill a bank while the other uses
//     the other. RET selects the assignment (see main_bank()). The sub CPU
//     swaps the banks by writing RET, which clears DMNA. Quirk: a main write
//     with DMNA = 0 sets the visible DMNA bit, while DMNA = 1 only marks the
//     Word RAM to go to the sub CPU on the next switch to 2M mode.
//       main: expansion base + 0x200000-0x21FFFF   sub: 0x0C0000-0x0DFFFF
//
// 1M mode also offers two alternate views of each CPU's bank (layouts as in
// Genesis Plus GX):
//   - main CPU "cell image", expansion base + 0x220000-0x23FFFF: the bank seen
//     as 8x8 cells, so a bitmap drawn in "columns" can be DMA'd straight to
//     the VDP as tiles (see cell_offset()).
//   - sub CPU "dot image", 0x080000-0x0BFFFF: one 4-bit pixel per byte
//     address (even = high nibble, odd = low nibble of byte address / 2),
//     with the priority modes (PM) for writes: normal, underwrite (only fills
//     zero pixels), overwrite (zero pixels in the new data are transparent).
class WordRam {
public:
    static constexpr u32 kSize     = 256 * 1024;
    static constexpr u32 kBankSize = 128 * 1024;

    enum class Mode : u8 { TwoMeg = 0, OneMeg = 1 };

    // Value returned when a CPU accesses Word RAM it doesn't own.
    static constexpr u8 kNotOwned = 0xFF;

    void reset() noexcept {
        for (auto& bank : banks_) bank.fill(0);
        mode_ = Mode::TwoMeg;
        ret_  = true;   // power-on: main CPU owns Word RAM
        dmna_ = false;
        return_to_sub_ = false;
        priority_mode_ = 0;
    }

    // PM bits of the sub CPU's memory mode register (dot image writes).
    void set_priority_mode(u8 pm) noexcept { priority_mode_ = pm & 3; }

    // Cell image: byte offset within a bank for byte `offset` (0-0x1FFFF) of
    // the main CPU's cell view.
    [[nodiscard]] static u32 cell_offset(u32 offset) noexcept;

    // --- State (read through the gate array's memory mode register) ----------
    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    [[nodiscard]] bool ret()  const noexcept { return ret_; }
    [[nodiscard]] bool dmna() const noexcept { return dmna_; }

    // 2M mode: does the main CPU currently own Word RAM?
    [[nodiscard]] bool main_owns() const noexcept { return mode_ == Mode::TwoMeg && ret_; }
    [[nodiscard]] bool sub_owns()  const noexcept { return mode_ == Mode::TwoMeg && !ret_; }

    // 1M mode: bank assigned to the main CPU (the sub CPU gets the other).
    // RET = 1 -> main gets bank 1.
    // NOTE: emulator sources disagree in their wording here; this follows
    // Genesis Plus GX. If CD games show swapped graphics, flip this.
    [[nodiscard]] int main_bank() const noexcept { return ret_ ? 1 : 0; }
    [[nodiscard]] int sub_bank()  const noexcept { return 1 - main_bank(); }

    // --- Control (called by the gate array on register writes) ---------------

    // The handover rules follow Genesis Plus GX, including its 1M-mode
    // quirks (games depend on them: Road Avenger deadlocks otherwise).

    // Main CPU writes its memory mode register's low byte (DMNA = bit 1).
    void main_write_dmna(bool dmna) noexcept {
        if (mode_ == Mode::TwoMeg) {
            if (dmna) {
                return_to_sub_ = true;
                ret_  = false;   // Word RAM goes to the sub CPU
                dmna_ = true;
            }
            // DMNA = 0 does nothing in 2M mode.
        } else if (dmna) {
            // 1M: remembered; the bank goes to the sub CPU when it switches
            // back to 2M mode. The visible DMNA bit is unchanged.
            return_to_sub_ = true;
        } else {
            dmna_ = true;        // 1M: a swap request, completed by the sub CPU
        }
    }

    // Sub CPU writes the MODE and RET bits of its memory mode register.
    // Only a change of MODE or RET has an effect.
    void sub_write_mode(Mode mode, bool ret) noexcept {
        if (mode == mode_ && ret == ret_) return;
        if (mode == Mode::OneMeg) {
            // RET picks the bank assignment; this completes any swap.
            mode_ = mode;
            if (ret) return_to_sub_ = false;
            ret_  = ret;
            dmna_ = false;
            return;
        }
        if (mode_ == Mode::OneMeg) {
            // 1M -> 2M: Word RAM returns to the main CPU, unless the main CPU
            // asked for it to go to the sub CPU and the sub keeps RET = 0.
            mode_ = Mode::TwoMeg;
            if (!return_to_sub_) ret = true;
            if (!ret) {
                ret_  = false;
                dmna_ = true;
                return;
            }
        }
        // 2M: the sub CPU can only *return* Word RAM (RET = 1); RET = 0 does
        // nothing (only the main CPU's DMNA hands it over).
        if (ret) {
            return_to_sub_ = false;
            ret_  = true;
            dmna_ = false;
        }
    }

    // --- Main CPU (offset relative to expansion base + 0x200000) -------------
    [[nodiscard]] u8 main_read8(u32 offset) const noexcept {
        const auto* p = main_byte(offset);
        return p ? *p : kNotOwned;
    }
    [[nodiscard]] u16 main_read16(u32 offset) const noexcept {
        return word(main_byte(offset & ~1u), main_byte(offset | 1u));
    }
    void main_write8(u32 offset, u8 value) noexcept {
        if (auto* p = mutable_main_byte(offset)) *p = value;
    }
    void main_write16(u32 offset, u16 value) noexcept {
        store(mutable_main_byte(offset & ~1u), mutable_main_byte(offset | 1u), value);
    }

    // --- Sub CPU (offset relative to 0x080000) --------------------------------
    [[nodiscard]] u8 sub_read8(u32 offset) const noexcept {
        if (in_dot_image(offset)) {
            const u8 b = dot_byte(offset);
            return static_cast<u8>((offset & 1) ? (b & 0x0F) : (b >> 4));
        }
        const auto* p = sub_byte(offset);
        return p ? *p : kNotOwned;
    }
    [[nodiscard]] u16 sub_read16(u32 offset) const noexcept {
        if (in_dot_image(offset)) {
            // Both pixels of one byte: high nibble in bits 8-11, low in 0-3.
            const u8 b = dot_byte(offset);
            return static_cast<u16>(((b & 0xF0) << 4) | (b & 0x0F));
        }
        return word(sub_byte(offset & ~1u), sub_byte(offset | 1u));
    }
    void sub_write8(u32 offset, u8 value) noexcept {
        if (in_dot_image(offset)) {
            u8& b = mutable_dot_byte(offset);
            const u8 merged = (offset & 1) ? static_cast<u8>((b & 0xF0) | (value & 0x0F))
                                           : static_cast<u8>((b & 0x0F) | ((value & 0x0F) << 4));
            b = prioritize(b, merged);
            return;
        }
        if (auto* p = mutable_sub_byte(offset)) *p = value;
    }
    void sub_write16(u32 offset, u16 value) noexcept {
        if (in_dot_image(offset)) {
            u8& b = mutable_dot_byte(offset);
            b = prioritize(b, static_cast<u8>(((value >> 4) & 0xF0) | (value & 0x0F)));
            return;
        }
        store(mutable_sub_byte(offset & ~1u), mutable_sub_byte(offset | 1u), value);
    }

    // Raw bank access (tests, debugger).
    [[nodiscard]] u8 bank_byte(int bank, u32 offset) const noexcept {
        return banks_[static_cast<std::size_t>(bank & 1)][offset & (kBankSize - 1)];
    }

private:
    // 2M layout: word N of the 256 KB space lives in bank (N & 1) at word N/2.
    [[nodiscard]] const u8* two_meg_byte(u32 offset) const noexcept {
        offset &= kSize - 1;
        const auto bank = static_cast<std::size_t>((offset >> 1) & 1);
        const u32 index = ((offset >> 2) << 1) | (offset & 1);
        return &banks_[bank][index];
    }
    [[nodiscard]] const u8* one_meg_byte(int bank, u32 offset) const noexcept {
        return &banks_[static_cast<std::size_t>(bank)][offset & (kBankSize - 1)];
    }

    [[nodiscard]] const u8* main_byte(u32 offset) const noexcept {
        offset &= kSize - 1;
        if (mode_ == Mode::TwoMeg) return main_owns() ? two_meg_byte(offset) : nullptr;
        if (offset < kBankSize) return one_meg_byte(main_bank(), offset);
        return one_meg_byte(main_bank(), cell_offset(offset - kBankSize));
    }

    // Dot image: sub offsets 0x00000-0x3FFFF in 1M mode (0x080000-0x0BFFFF).
    [[nodiscard]] bool in_dot_image(u32 offset) const noexcept {
        return mode_ == Mode::OneMeg && offset < 0x40000;
    }
    [[nodiscard]] u8 dot_byte(u32 offset) const noexcept {
        return banks_[static_cast<std::size_t>(sub_bank())][(offset >> 1) & (kBankSize - 1)];
    }
    u8& mutable_dot_byte(u32 offset) noexcept {
        return banks_[static_cast<std::size_t>(sub_bank())][(offset >> 1) & (kBankSize - 1)];
    }
    // Priority mode, per pixel (nibble); 0 is a transparent/empty pixel.
    [[nodiscard]] u8 prioritize(u8 old_byte, u8 new_byte) const noexcept {
        const auto pick = [&](u8 mask) -> u8 {
            const u8 o = old_byte & mask, n = new_byte & mask;
            switch (priority_mode_) {
            case 1:  return o ? o : n;   // underwrite: only empty pixels change
            case 2:  return n ? n : o;   // overwrite: empty new pixels are transparent
            case 3:  return o;           // invalid: no effect
            default: return n;
            }
        };
        return static_cast<u8>(pick(0xF0) | pick(0x0F));
    }
    [[nodiscard]] const u8* sub_byte(u32 offset) const noexcept {
        if (mode_ == Mode::TwoMeg) {
            return (sub_owns() && offset < kSize) ? two_meg_byte(offset) : nullptr;
        }
        // 1M: 0x0C0000-0x0DFFFF (offset 0x40000-0x5FFFF) is the sub's bank.
        if (offset >= 0x40000 && offset < 0x60000) return one_meg_byte(sub_bank(), offset - 0x40000);
        return nullptr;
    }

    u8* mutable_main_byte(u32 offset) noexcept { return const_cast<u8*>(main_byte(offset)); }
    u8* mutable_sub_byte(u32 offset) noexcept  { return const_cast<u8*>(sub_byte(offset)); }

    static u16 word(const u8* hi, const u8* lo) noexcept {
        if (!hi || !lo) return static_cast<u16>((kNotOwned << 8) | kNotOwned);
        return static_cast<u16>((*hi << 8) | *lo);
    }
    static void store(u8* hi, u8* lo, u16 value) noexcept {
        if (!hi || !lo) return;
        *hi = static_cast<u8>(value >> 8);
        *lo = static_cast<u8>(value);
    }

    std::array<std::array<u8, kBankSize>, 2> banks_{};
    Mode mode_ = Mode::TwoMeg;
    bool ret_  = true;
    bool dmna_ = false;
    bool return_to_sub_ = false;  // main wrote DMNA = 1 (GPGX "scd.dmna")
    u8   priority_mode_ = 0;
};

namespace detail {

// Cell image lookup (Genesis Plus GX's gfx.lut_offset), indexed by bits 2-16
// of the cell-view offset. The view is split into zones of decreasing height
// (32, 16, 8, 4, 4 cells); within a zone, consecutive 4-byte groups walk down
// the 8 lines of a cell, then down the cells of a column, then across.
inline constexpr std::array<u16, 0x8000> make_cell_lut() {
    std::array<u16, 0x8000> lut{};
    for (u32 i = 0; i < 0x8000; ++i) {
        u32 x = 0, y = 0, base = 0;
        if (i < 0x4000)      { x = (i >> 8) & 0x3F; y = (i >> 3) & 0x1F; }
        else if (i < 0x6000) { x = (i >> 7) & 0x3F; y = (i >> 3) & 0x0F; }
        else if (i < 0x7000) { x = (i >> 6) & 0x3F; y = (i >> 3) & 0x07; base = 0x8000; }
        else if (i < 0x7800) { x = (i >> 5) & 0x3F; y = (i >> 3) & 0x03; base = 0xC000; }
        else                 { x = (i >> 5) & 0x3F; y = (i >> 3) & 0x03; base = 0xE000; }
        lut[i] = static_cast<u16>(base | ((i & 7) << 8) | (x << 2) | (y << 11));
    }
    return lut;
}
inline constexpr std::array<u16, 0x8000> kCellLut = make_cell_lut();

} // namespace detail

inline u32 WordRam::cell_offset(u32 offset) noexcept {
    return detail::kCellLut[(offset >> 2) & 0x7FFF] | (offset & 0x10003);
}

} // namespace md::megacd
