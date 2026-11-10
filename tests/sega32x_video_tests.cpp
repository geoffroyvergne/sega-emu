// 32X VDP picture: line table, the packed-pixel, direct-colour and run-length
// modes, and mixing with the Mega Drive picture.

#include <array>
#include <vector>

#include "md/emulator.hpp"
#include "md/sega32x/vdp32x.hpp"
#include "test.hpp"

using md::u16;
using md::u32;
using md::u8;
using md::Framebuffer;
using md::sega32x::Vdp32x;

namespace {

constexpr u16 kRed = 0x001F, kGreen = 0x03E0, kBlue = 0x7C00, kP = Vdp32x::kPriorityBit;

// Writes into the frame buffer the CPUs see (FS = 0: buffer 0).
void fb_word(Vdp32x& v, u32 word_address, u16 value) { v.write_frame_buffer(word_address * 2, value, 0xFFFF); }
void fb_byte(Vdp32x& v, u32 byte_address, u8 value) {
    v.write_frame_buffer(byte_address, (byte_address & 1) ? value : static_cast<u16>(value << 8), (byte_address & 1) ? 0x00FF : 0xFF00);
}

// Shows buffer 0: FS = 1 at the next VBlank.
void show_buffer0(Vdp32x& v) {
    v.write_register(0x0A, 1, 0xFFFF);
    v.set_vblank(true);
    v.set_vblank(false);
}

void set_mode(Vdp32x& v, Vdp32x::Mode mode, bool pri = false) {
    v.write_register(0x00, static_cast<u16>(static_cast<u16>(mode) | (pri ? Vdp32x::kPri : 0)), 0xFFFF);
}

} // namespace

TEST(vdp32x_colour_conversion) {
    CHECK(Vdp32x::to_argb(0x7FFF) == 0xFFFFFFFF);
    CHECK(Vdp32x::to_argb(kRed) == 0xFFFF0000);
    CHECK(Vdp32x::to_argb(kGreen) == 0xFF00FF00);
    CHECK(Vdp32x::to_argb(kBlue) == 0xFF0000FF);
    CHECK(Vdp32x::to_argb(kP) == 0xFF000000);                         // P isn't a colour
    CHECK(Vdp32x::to_argb(0x0010) == 0xFF840000);                     // 16 -> 0x84
}

TEST(vdp32x_direct_colour_and_line_table) {
    Vdp32x v;
    fb_word(v, 0, 0x0100);                                            // line 0 at word 0x100
    fb_word(v, 1, 0x0400);                                            // line 1 at word 0x400
    fb_word(v, 2, 0xFFFF);                                            // line 2 wraps the buffer
    for (u32 x = 0; x < 320; ++x) {
        fb_word(v, 0x100 + x, static_cast<u16>(x));
        fb_word(v, 0x400 + x, static_cast<u16>(0x7000 + x));
    }
    show_buffer0(v);
    set_mode(v, Vdp32x::Mode::DirectColor);

    std::array<u16, Vdp32x::kWidth> line{};
    CHECK(v.render_line(0, line));
    CHECK(line[0] == 0 && line[1] == 1 && line[319] == 319);
    CHECK(v.render_line(1, line));
    CHECK(line[0] == 0x7000 && line[319] == 0x7000 + 319);
    CHECK(v.render_line(2, line));
    // Word 0xFFFF (empty), then words 0 and 1 (the line table itself).
    CHECK(line[0] == 0 && line[1] == 0x0100 && line[2] == 0x0400);
}

TEST(vdp32x_packed_pixel_and_screen_shift) {
    Vdp32x v;
    v.write_palette(0 * 2, kRed, 0xFFFF);
    v.write_palette(1 * 2, kGreen, 0xFFFF);
    v.write_palette(255 * 2, kBlue | kP, 0xFFFF);
    fb_word(v, 0, 0x0100);
    for (u32 x = 0; x < 321; ++x) fb_byte(v, 0x200 + x, static_cast<u8>(x % 3 == 2 ? 255 : x % 3));
    show_buffer0(v);
    set_mode(v, Vdp32x::Mode::PackedPixel);

    std::array<u16, Vdp32x::kWidth> line{};
    CHECK(v.render_line(0, line));
    CHECK(line[0] == kRed && line[1] == kGreen && line[2] == (kBlue | kP) && line[3] == kRed);
    v.write_register(0x02, 1, 0xFFFF);                                // shift: one pixel left
    v.render_line(0, line);
    CHECK(line[0] == kGreen && line[1] == (kBlue | kP) && line[319] == (kBlue | kP));   // byte 320
}

TEST(vdp32x_run_length) {
    Vdp32x v;
    v.write_palette(1 * 2, kRed, 0xFFFF);
    v.write_palette(2 * 2, kGreen, 0xFFFF);
    v.write_palette(3 * 2, kBlue, 0xFFFF);
    fb_word(v, 0, 0x0100);
    fb_word(v, 0x100, (2 << 8) | 1);                                  // 3 x red
    fb_word(v, 0x101, (255 << 8) | 2);                                // 256 x green
    fb_word(v, 0x102, (255 << 8) | 3);                                // blue: only 61 fit
    show_buffer0(v);
    set_mode(v, Vdp32x::Mode::RunLength);

    std::array<u16, Vdp32x::kWidth> line{};
    CHECK(v.render_line(0, line));
    CHECK(line[0] == kRed && line[2] == kRed && line[3] == kGreen);
    CHECK(line[258] == kGreen && line[259] == kBlue && line[319] == kBlue);
}

TEST(vdp32x_blank_mode_draws_nothing) {
    Vdp32x v;
    std::array<u16, Vdp32x::kWidth> line{};
    CHECK(!v.render_line(0, line));
    std::array<Framebuffer::Pixel, 4> row{1, 2, 3, 4};
    const std::array<u8, 4> backdrop{1, 1, 1, 1};
    v.compose_line(0, row, backdrop);
    CHECK(row[0] == 1 && row[3] == 4);
}

TEST(vdp32x_mixing_with_the_mega_drive) {
    // Four 32X pixels: P = 0, 1, 0, 1, over Mega Drive pixels that are
    // backdrop for the first pair and a plane/sprite for the second.
    Vdp32x v;
    fb_word(v, 0, 0x0100);
    const std::array<u16, 4> pixels{kRed, kRed | kP, kGreen, kGreen | kP};
    for (u32 x = 0; x < 4; ++x) fb_word(v, 0x100 + x, pixels[x]);
    show_buffer0(v);
    constexpr Framebuffer::Pixel kMd = 0xFF123456;
    const std::array<u8, 4> backdrop{1, 1, 0, 0};
    const auto compose = [&](bool pri) {
        set_mode(v, Vdp32x::Mode::DirectColor, pri);
        std::array<Framebuffer::Pixel, 4> row;
        row.fill(kMd);
        v.compose_line(0, row, backdrop);
        return row;
    };
    const Framebuffer::Pixel red = Vdp32x::to_argb(kRed), green = Vdp32x::to_argb(kGreen);

    // PRI = 0: the Mega Drive is in front, except where P = 1.
    const auto a = compose(false);
    CHECK(a[0] == red && a[1] == red);                                // over the backdrop: always 32X
    CHECK(a[2] == kMd);
    CHECK(a[3] == green);
    // PRI = 1: the 32X is in front, except where P = 1.
    const auto b = compose(true);
    CHECK(b[0] == red && b[1] == red);
    CHECK(b[2] == green);
    CHECK(b[3] == kMd);
}

TEST(vdp32x_picture_reaches_the_emulator_framebuffer) {
    // A cartridge that just loops; the Mega Drive display stays off, so its
    // picture is all backdrop and the 32X picture shows everywhere.
    std::vector<u8> rom(0x1000, 0);
    rom[0] = 0x00; rom[1] = 0xFF;                                     // SSP 0xFF0000
    rom[6] = 0x02;                                                    // PC 0x200
    rom[0x200] = 0x60; rom[0x201] = 0xFE;                             // bra.s *
    md::Emulator emu;
    emu.load_rom(rom);
    emu.attach_32x();
    auto& mem = emu.memory();

    mem.write_word(0xA15100, md::sega32x::SystemRegisters::kAden);    // 32X mode, FM = 0
    for (u32 line = 0; line < 240; ++line) mem.write_word(0x840000 + line * 2, 0x0100);
    for (u32 x = 0; x < 320; ++x) mem.write_word(0x840200 + x * 2, x < 100 ? kBlue : kRed);
    mem.write_word(0xA15180, static_cast<u16>(Vdp32x::Mode::DirectColor));
    mem.write_word(0xA1518A, 1);                                      // show it from the next VBlank

    emu.run_frame();                                                  // the flip happens here
    emu.run_frame();                                                  // drawn
    const auto& fb = emu.framebuffer();
    const auto pixel = [&](int x, int y) { return fb.pixels()[static_cast<std::size_t>(y * Framebuffer::kStride + x)]; };
    CHECK(pixel(0, 0) == Vdp32x::to_argb(kBlue));
    CHECK(pixel(99, 100) == Vdp32x::to_argb(kBlue));
    CHECK(pixel(100, 100) == Vdp32x::to_argb(kRed));
    CHECK(pixel(fb.width() - 1, fb.height() - 1) == Vdp32x::to_argb(kRed));

    // Mega Drive mode again: the 32X layer is gone.
    mem.write_word(0xA15100, 0);
    emu.run_frame();
    CHECK(pixel(0, 0) != Vdp32x::to_argb(kBlue));
}
