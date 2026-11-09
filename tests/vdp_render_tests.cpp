#include <initializer_list>
#include <vector>

#include "md/emulator.hpp"
#include "md/vdp.hpp"
#include "test.hpp"
#include "vdp_helpers.hpp"

using md::Vdp;
using namespace vdp_test;

namespace {

constexpr u16 kPlaneA  = 0xC000;
constexpr u16 kPlaneB  = 0xE000;
constexpr u16 kWindow  = 0xB000;
constexpr u16 kHScroll = 0xFC00;

// Standard layout: H40, planes A/B at 0xC000/0xE000, 64x32 cells, display on.
// Tile 1: row 0 = colours 1..8, rows 1-7 = colour 1.
struct RenderFixture : Fixture {
    RenderFixture() {
        set_reg(vdp, 1, 0x44);   // display on, Mode 5
        set_reg(vdp, 2, 0x30);   // plane A at 0xC000
        set_reg(vdp, 3, 0x2C);   // window at 0xB000
        set_reg(vdp, 4, 0x07);   // plane B at 0xE000
        set_reg(vdp, 7, 0x00);   // backdrop = colour 0
        set_reg(vdp, 12, 0x81);  // H40
        set_reg(vdp, 13, 0x3F);  // hscroll table at 0xFC00
        set_reg(vdp, 16, 0x01);  // 64x32 cells

        write_words(vdp, kVramWrite, 0x0020, {
            0x1234, 0x5678,
            0x1111, 0x1111, 0x1111, 0x1111, 0x1111, 0x1111, 0x1111, 0x1111,
            0x1111, 0x1111, 0x1111, 0x1111, 0x1111, 0x1111,
        });

        // 64 distinct colours.
        command(vdp, make_command(kCramWrite, 0));
        for (u16 i = 0; i < 64; ++i) vdp.write(kData, static_cast<u16>((i * 0x0246) & 0x0EEE));
    }

    void set_cell(u16 nametable, int cx, int cy, int width_cells, u16 entry) {
        write_words(vdp, kVramWrite, static_cast<u16>(nametable + (cy * width_cells + cx) * 2), {entry});
    }

    [[nodiscard]] md::Framebuffer::Pixel color(int index) const {
        return Vdp::cram_to_argb(vdp.cram()[static_cast<std::size_t>(index)]);
    }
};

} // namespace

TEST(render_tile_pixels_from_plane_a) {
    RenderFixture f;
    f.set_cell(kPlaneA, 0, 0, 64, 0x0001);
    f.render(0);
    for (int x = 0; x < 8; ++x) CHECK(f.pixel(x, 0) == f.color(x + 1));
    CHECK(f.pixel(8, 0) == f.color(0));  // tile 0 is blank -> backdrop

    f.render(1);
    CHECK(f.pixel(7, 1) == f.color(1));
}

TEST(render_flips_and_palette) {
    RenderFixture f;
    f.set_cell(kPlaneA, 0, 0, 64, 0x0801);   // hflip
    f.set_cell(kPlaneA, 1, 0, 64, 0x1001);   // vflip
    f.set_cell(kPlaneA, 2, 0, 64, 0x6001);   // palette 3
    f.render(0);
    CHECK(f.pixel(0, 0) == f.color(8));      // hflip: rightmost pixel first
    CHECK(f.pixel(7, 0) == f.color(1));
    CHECK(f.pixel(15, 0) == f.color(1));     // vflip: line 0 shows row 7
    CHECK(f.pixel(16, 0) == f.color(48 + 1));
    f.render(7);
    CHECK(f.pixel(15, 7) == f.color(8));     // vflip: line 7 shows row 0
}

TEST(render_horizontal_and_vertical_scroll) {
    RenderFixture f;
    f.set_cell(kPlaneA, 0, 1, 64, 0x0001);         // tile in cell row 1
    write_words(f.vdp, kVramWrite, kHScroll, {0x0008});  // plane A: 8 px right
    write_words(f.vdp, kVsramWrite, 0, {0x0008});        // plane A: 8 px down
    f.render(0);
    CHECK(f.pixel(0, 0) == f.color(0));
    CHECK(f.pixel(8, 0) == f.color(1));
    CHECK(f.pixel(15, 0) == f.color(8));
}

TEST(render_per_line_hscroll) {
    RenderFixture f;
    set_reg(f.vdp, 11, 0x03);                     // per-line horizontal scroll
    f.set_cell(kPlaneA, 0, 0, 64, 0x0001);
    write_words(f.vdp, kVramWrite, static_cast<u16>(kHScroll + 1 * 4), {0x0010});  // line 1: +16
    f.render(0);
    CHECK(f.pixel(0, 0) == f.color(1));
    f.render(1);
    CHECK(f.pixel(0, 1) == f.color(0));
    CHECK(f.pixel(16, 1) == f.color(1));
}

TEST(render_plane_priority) {
    RenderFixture f;
    f.set_cell(kPlaneA, 0, 0, 64, 0x0001);         // A: low priority, palette 0
    f.set_cell(kPlaneB, 0, 0, 64, 0x2001);         // B: low priority, palette 1
    f.render(0);
    CHECK(f.pixel(0, 0) == f.color(1));            // A low beats B low

    f.set_cell(kPlaneB, 0, 0, 64, 0xA001);         // B: high priority
    f.render(1);
    CHECK(f.pixel(0, 1) == f.color(16 + 1));       // B high beats A low

    f.set_cell(kPlaneA, 0, 0, 64, 0x8001);         // A: high too
    f.render(2);
    CHECK(f.pixel(0, 2) == f.color(1));            // A high beats B high
}

TEST(render_transparent_plane_a_shows_plane_b) {
    RenderFixture f;
    f.set_cell(kPlaneB, 1, 0, 64, 0x2001);
    f.render(0);
    CHECK(f.pixel(8, 0) == f.color(16 + 1));
}

TEST(render_window_replaces_plane_a) {
    RenderFixture f;
    f.set_cell(kPlaneA, 0, 0, 64, 0x0001);
    f.set_cell(kPlaneA, 2, 0, 64, 0x0001);
    f.set_cell(kWindow, 0, 0, 64, 0x4001);         // palette 2
    set_reg(f.vdp, 17, 0x01);                      // window on the left 16 px
    f.render(0);
    CHECK(f.pixel(0, 0) == f.color(32 + 1));       // window
    CHECK(f.pixel(8, 0) == f.color(0));            // window cell 1 is empty (and hides A)
    CHECK(f.pixel(16, 0) == f.color(1));           // plane A again
}

TEST(render_display_disabled_shows_backdrop) {
    RenderFixture f;
    f.set_cell(kPlaneA, 0, 0, 64, 0x0001);
    set_reg(f.vdp, 1, 0x04);
    set_reg(f.vdp, 7, 0x05);
    f.render(0);
    CHECK(f.pixel(0, 0) == f.color(5));
}

TEST(render_left_column_blank) {
    RenderFixture f;
    f.set_cell(kPlaneA, 0, 0, 64, 0x0001);
    f.set_cell(kPlaneA, 1, 0, 64, 0x0001);
    set_reg(f.vdp, 0, 0x24);
    f.render(0);
    CHECK(f.pixel(0, 0) == f.color(0));
    CHECK(f.pixel(8, 0) == f.color(1));
}

TEST(render_full_h40_width) {
    RenderFixture f;
    f.set_cell(kPlaneA, 39, 0, 64, 0x0001);
    f.render(0);
    CHECK(f.pixel(319, 0) == f.color(8));
}

// --- Full system: a 68000 program uploads graphics with DMA ------------------

namespace {

class Asm {
public:
    Asm() : rom_(0x1000, 0) { put_long(0, 0x00FFFE00); put_long(4, 0x200); }

    void at(u32 address) { pc_ = address; }
    void word(u16 w) { put_word(pc_, w); pc_ += 2; }
    void words(std::initializer_list<u16> ws) { for (u16 w : ws) word(w); }

    // move.w #imm,($C00004).l
    void vdp_ctrl_w(u16 value) { words({0x33FC, value, 0x00C0, 0x0004}); }
    // move.l #imm,($C00004).l
    void vdp_ctrl_l(u32 value) {
        words({0x23FC, static_cast<u16>(value >> 16), static_cast<u16>(value), 0x00C0, 0x0004});
    }
    // move.w #imm,($C00000).l
    void vdp_data_w(u16 value) { words({0x33FC, value, 0x00C0, 0x0000}); }

    [[nodiscard]] const std::vector<u8>& rom() const { return rom_; }

private:
    void put_word(u32 a, u16 v) { rom_[a] = static_cast<u8>(v >> 8); rom_[a + 1] = static_cast<u8>(v); }
    void put_long(u32 a, u32 v) { put_word(a, static_cast<u16>(v >> 16)); put_word(a + 2, static_cast<u16>(v)); }

    std::vector<u8> rom_;
    u32 pc_ = 0;
};

} // namespace

TEST(system_dma_upload_renders_a_tile) {
    Asm a;

    // Data in ROM: a 16-colour palette at 0x800 and one tile at 0x900.
    a.at(0x800);
    for (u16 i = 0; i < 16; ++i) a.word(static_cast<u16>((i * 0x0124) & 0x0EEE));
    a.at(0x900);
    a.words({0x1234, 0x5678});
    for (int i = 0; i < 14; ++i) a.word(0x1111);

    a.at(0x200);
    a.vdp_ctrl_w(0x8F02);              // auto-increment 2
    a.vdp_ctrl_w(0x8154);              // display + DMA + Mode 5
    a.vdp_ctrl_w(0x8C81);              // H40
    a.vdp_ctrl_w(0x8230);              // plane A at 0xC000
    a.vdp_ctrl_w(0x8407);              // plane B at 0xE000
    a.vdp_ctrl_w(0x8D3F);              // hscroll at 0xFC00
    a.vdp_ctrl_w(0x9001);              // 64x32

    // DMA palette: 16 words from 0x800 to CRAM 0.
    a.vdp_ctrl_w(0x9310); a.vdp_ctrl_w(0x9400);
    a.vdp_ctrl_w(0x9500); a.vdp_ctrl_w(0x9604); a.vdp_ctrl_w(0x9700);
    a.vdp_ctrl_l(make_command(kCramWrite | kDma, 0x0000));

    // DMA tile: 16 words from 0x900 to VRAM 0x0020 (tile 1).
    a.vdp_ctrl_w(0x9310); a.vdp_ctrl_w(0x9400);
    a.vdp_ctrl_w(0x9580); a.vdp_ctrl_w(0x9604); a.vdp_ctrl_w(0x9700);
    a.vdp_ctrl_l(make_command(kVramWrite | kDma, 0x0020));

    // Plane A cell (0,0) = tile 1.
    a.vdp_ctrl_l(make_command(kVramWrite, kPlaneA));
    a.vdp_data_w(0x0001);
    a.word(0x60FE);                    // bra.s *

    md::Emulator emu{md::Region::NTSC};
    emu.load_rom(a.rom());
    emu.run_frame();                   // setup happens during this frame
    emu.run_frame();                   // fully rendered frame

    const auto& fb = emu.framebuffer();
    CHECK(fb.width() == 320);
    const auto expected = [&](int i) { return Vdp::cram_to_argb(static_cast<u16>((i * 0x0124) & 0x0EEE)); };
    for (int x = 0; x < 8; ++x) CHECK(fb.pixels()[static_cast<std::size_t>(x)] == expected(x + 1));
    CHECK(fb.pixels()[static_cast<std::size_t>(md::Framebuffer::kStride * 3)] == expected(1));
    CHECK(fb.pixels()[8] == expected(0));
}

// --- Sprites -----------------------------------------------------------------

namespace {

constexpr u16 kSpriteTable = 0xF000;

struct SpriteFixture : RenderFixture {
    SpriteFixture() {
        set_reg(vdp, 5, 0x78);  // sprite table at 0xF000
        // Solid tiles 10..13 in colours 2..5 (for multi-cell layout checks).
        for (u16 t = 0; t < 4; ++t) {
            const u16 nibbles = static_cast<u16>((t + 2) * 0x1111);
            command(vdp, make_command(kVramWrite, static_cast<u16>((10 + t) * 32)));
            for (int i = 0; i < 16; ++i) vdp.write(kData, nibbles);
        }
    }

    // Screen coordinates; the +128 offset is added here.
    void sprite(int index, int x, int y, int h_cells, int v_cells, int link, u16 entry) {
        write_words(vdp, kVramWrite, static_cast<u16>(kSpriteTable + index * 8), {
            static_cast<u16>(y + 128),
            static_cast<u16>(((h_cells - 1) << 10) | ((v_cells - 1) << 8) | link),
            entry,
            static_cast<u16>(x + 128),
        });
    }
};

} // namespace

TEST(sprite_single_cell) {
    SpriteFixture f;
    f.sprite(0, 10, 0, 1, 1, 0, 0x0001);
    f.render(0);
    CHECK(f.pixel(9, 0) == f.color(0));
    for (int i = 0; i < 8; ++i) CHECK(f.pixel(10 + i, 0) == f.color(i + 1));
    CHECK(f.pixel(18, 0) == f.color(0));
    f.render(8);
    CHECK(f.pixel(10, 8) == f.color(0));  // 1 cell tall: lines 0-7 only
}

TEST(sprite_multi_cell_is_column_major) {
    SpriteFixture f;
    f.sprite(0, 0, 0, 2, 2, 0, 10);       // 2x2 cells starting at tile 10
    f.render(0);
    CHECK(f.pixel(0, 0) == f.color(2));   // tile 10: column 0, row 0
    CHECK(f.pixel(8, 0) == f.color(4));   // tile 12: column 1, row 0
    f.render(8);
    CHECK(f.pixel(0, 8) == f.color(3));   // tile 11: column 0, row 1
    CHECK(f.pixel(8, 8) == f.color(5));   // tile 13: column 1, row 1
}

TEST(sprite_flips_swap_cells) {
    SpriteFixture f;
    f.sprite(0, 0, 0, 2, 2, 0, 0x1800 | 10);  // hflip + vflip
    f.render(0);
    CHECK(f.pixel(0, 0) == f.color(5));
    CHECK(f.pixel(8, 0) == f.color(3));
    f.render(8);
    CHECK(f.pixel(0, 8) == f.color(4));
    CHECK(f.pixel(8, 8) == f.color(2));

    SpriteFixture g;
    g.sprite(0, 0, 0, 1, 1, 0, 0x0801);       // hflip within a cell
    g.render(0);
    CHECK(g.pixel(0, 0) == g.color(8));
    CHECK(g.pixel(7, 0) == g.color(1));
}

TEST(sprite_palette_and_negative_position) {
    SpriteFixture f;
    f.sprite(0, -4, -2, 1, 1, 0, 0x2001);     // palette 1, partly off-screen
    f.render(0);
    CHECK(f.pixel(0, 0) == f.color(16 + 1));  // row 2 of tile 1 is all colour 1
    CHECK(f.pixel(3, 0) == f.color(16 + 1));
    CHECK(f.pixel(4, 0) == f.color(0));
}

TEST(sprite_link_list_order_and_collision) {
    SpriteFixture f;
    f.sprite(0, 0, 0, 1, 1, 1, 10);           // colour 2, links to 1
    f.sprite(1, 4, 0, 1, 1, 0, 11);           // colour 3, overlaps 4 pixels
    f.sprite(2, 40, 0, 1, 1, 0, 12);          // not linked: never drawn
    f.render(0);
    CHECK(f.pixel(4, 0) == f.color(2));       // earlier sprite wins
    CHECK(f.pixel(8, 0) == f.color(3));
    CHECK(f.pixel(40, 0) == f.color(0));
    CHECK((f.vdp.read(kControl) & Vdp::Status::SpriteCollision) != 0);
    CHECK((f.vdp.read(kControl) & Vdp::Status::SpriteCollision) == 0);  // cleared by read
}

TEST(sprite_priority_against_planes) {
    SpriteFixture f;
    f.set_cell(kPlaneA, 0, 0, 64, 0x8000 | 10);  // A high, colour 2
    f.set_cell(kPlaneA, 1, 0, 64, 10);           // A low,  colour 2
    f.sprite(0, 0, 0, 2, 1, 1, 11);              // sprite low,  colour 3
    f.sprite(1, 16, 0, 1, 1, 0, 0x8000 | 11);    // sprite high, colour 3
    f.set_cell(kPlaneA, 2, 0, 64, 0x8000 | 10);  // A high under the high sprite
    f.render(0);
    CHECK(f.pixel(0, 0)  == f.color(2));         // A high beats sprite low
    CHECK(f.pixel(8, 0)  == f.color(4));         // sprite low beats A low (2nd cell = tile 12)
    CHECK(f.pixel(16, 0) == f.color(3));         // sprite high beats A high
}

TEST(sprite_per_line_limit_h40) {
    SpriteFixture f;
    for (int i = 0; i < 21; ++i) {
        f.sprite(i, i * 8, 0, 1, 1, i < 20 ? i + 1 : 0, 10);
    }
    f.render(0);
    CHECK(f.pixel(19 * 8, 0) == f.color(2));     // 20th sprite drawn
    CHECK(f.pixel(20 * 8, 0) == f.color(0));     // 21st dropped
    CHECK((f.vdp.status() & Vdp::Status::SpriteOverflow) != 0);
}

TEST(sprite_dot_limit_h40) {
    SpriteFixture f;
    // 11 sprites, 4 cells wide = 352 pixels > 320: the last 4 cells are cut.
    for (int i = 0; i < 11; ++i) {
        f.sprite(i, (i % 2) * 32, 20, 4, 1, i < 10 ? i + 1 : 0, 10);
    }
    f.sprite(10, 200, 20, 4, 1, 0, 11);          // last one is a different colour
    f.render(20);
    CHECK(f.pixel(200, 20) == f.color(0));       // budget exhausted before it
    CHECK((f.vdp.status() & Vdp::Status::SpriteOverflow) != 0);
}

TEST(sprite_x0_masks_later_sprites) {
    SpriteFixture f;
    f.sprite(0, 50, 0, 1, 1, 1, 10);
    f.sprite(1, -128, 0, 1, 1, 2, 10);           // raw X = 0: mask
    f.sprite(2, 100, 0, 1, 1, 0, 11);
    f.render(0);
    CHECK(f.pixel(50, 0)  == f.color(2));
    CHECK(f.pixel(100, 0) == f.color(0));        // masked

    // A masking sprite that is first on the line does nothing.
    SpriteFixture g;
    g.sprite(0, -128, 0, 1, 1, 1, 10);
    g.sprite(1, 100, 0, 1, 1, 0, 11);
    g.render(0);
    CHECK(g.pixel(100, 0) == g.color(3));
}
