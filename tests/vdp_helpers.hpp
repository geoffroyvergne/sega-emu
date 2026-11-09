#pragma once

// Shared helpers for VDP tests.

#include <vector>

#include "md/framebuffer.hpp"
#include "md/vdp.hpp"

namespace vdp_test {

using md::u16;
using md::u32;
using md::u8;

constexpr u32 kData    = 0x00;
constexpr u32 kControl = 0x04;
constexpr u32 kHv      = 0x08;

// Command codes (CD5-CD0).
constexpr u8 kVramRead   = 0x00;
constexpr u8 kVramWrite  = 0x01;
constexpr u8 kCramWrite  = 0x03;
constexpr u8 kVsramRead  = 0x04;
constexpr u8 kVsramWrite = 0x05;
constexpr u8 kCramRead   = 0x08;
constexpr u8 kDma        = 0x20;  // CD5: OR into a write code to start a DMA
constexpr u8 kVramCopy   = 0x30;  // CD5|CD4: VRAM copy DMA

// Builds a command long from a code (CD5-CD0) and a 16-bit address.
constexpr u32 make_command(u8 code, u16 address) {
    return (static_cast<u32>(code & 0x03) << 30) | (static_cast<u32>(address & 0x3FFF) << 16)
         | (static_cast<u32>(code & 0x3C) << 2)  | (static_cast<u32>(address) >> 14);
}

// Writes a 32-bit command as two control port words.
inline void command(md::Vdp& vdp, u32 cmd) {
    vdp.write(kControl, static_cast<u16>(cmd >> 16));
    vdp.write(kControl, static_cast<u16>(cmd & 0xFFFF));
}

inline void set_reg(md::Vdp& vdp, int reg, u8 value) {
    vdp.write(kControl, static_cast<u16>(0x8000 | (reg << 8) | value));
}

inline void write_words(md::Vdp& vdp, u8 code, u16 address, std::initializer_list<u16> words) {
    command(vdp, make_command(code, address));
    for (u16 w : words) vdp.write(kData, w);
}

struct Fixture {
    md::Framebuffer fb;
    md::Vdp vdp{md::Region::NTSC, fb};
    std::vector<int> irq_changes;

    Fixture() {
        vdp.set_irq_callback([this](int level) { irq_changes.push_back(level); });
        set_reg(vdp, 15, 2);  // auto-increment 2
    }

    void advance_to_line(int line) {
        while (vdp.line() != line || vdp.line_cycle() != 0) {
            vdp.advance(vdp.cycles_to_next_event());
        }
    }

    // Renders `line` (the VDP draws each line at the start of its HBlank).
    void render(int line) {
        advance_to_line(line);
        vdp.advance(md::Vdp::kHBlankStart);
    }

    [[nodiscard]] md::Framebuffer::Pixel pixel(int x, int y) {
        return fb.row(y)[static_cast<std::size_t>(x)];
    }
};

} // namespace vdp_test
