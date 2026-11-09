// Mega-CD graphics ASIC: stamp rendering with affine trace vectors.

#include <functional>

#include "md/megacd/graphics_asic.hpp"
#include "md/megacd/megacd.hpp"
#include "test.hpp"

using md::u16;
using md::u32;
using md::u8;
using md::megacd::GraphicsAsic;
using md::megacd::MegaCd;

namespace {

// Word RAM layout used by the tests (byte addresses, 2M mode).
constexpr u32 kMap    = 0x10000;  // stamp map
constexpr u32 kImage  = 0x20000;  // image buffer
constexpr u32 kTrace  = 0x30000;  // trace vectors
constexpr int kDots   = 16;       // 16x16 output

// Reference layout (from the hardware documentation, written independently
// of the implementation): stamps and the image buffer are made of 8x8 4-bpp
// cells of 32 bytes, ordered column by column.
u32 cell_dot_address(u32 base, u32 cells_per_column, u32 x, u32 y) {
    return base + ((x / 8) * cells_per_column + y / 8) * 32 + (y % 8) * 4 + (x % 8) / 2;
}

struct Fixture {
    MegaCd cd;
    md::megacd::WordRam& wram = cd.word_ram();

    Fixture() {
        cd.write8(0xA12003, 0x02);                  // main gives Word RAM to the sub CPU (2M)
        reg(0x58, 0x0000);                          // 16x16 stamps, 256x256 map, no repeat
        reg(0x5A, kMap >> 2);
        reg(0x5C, 1);                               // 2 cells per column (16 lines)
        reg(0x5E, kImage >> 2);
        reg(0x60, 0);
        reg(0x62, kDots);
        reg(0x64, kDots);
    }

    void reg(u32 offset, u16 value) { cd.sub_bus().write_word(0xFF8000 + offset, value); }
    [[nodiscard]] u16 reg_read(u32 offset) { return cd.sub_bus().read_word(0xFF8000 + offset); }

    void set_dot(u32 base, u32 cells, u32 x, u32 y, u8 colour) {
        const u32 a = cell_dot_address(base, cells, x, y);
        const u8 b = wram.sub_read8(a);
        wram.sub_write8(a, (x & 1) ? static_cast<u8>((b & 0xF0) | colour) : static_cast<u8>((b & 0x0F) | (colour << 4)));
    }
    [[nodiscard]] u8 get_dot(u32 base, u32 cells, u32 x, u32 y) {
        const u8 b = wram.sub_read8(cell_dot_address(base, cells, x, y));
        return (x & 1) ? static_cast<u8>(b & 0x0F) : static_cast<u8>(b >> 4);
    }

    // Stamp `number` (16x16 or 32x32) filled by `colour(x, y)`.
    void make_stamp(u32 number, u32 size, const std::function<u8(u32, u32)>& colour) {
        for (u32 y = 0; y < size; ++y)
            for (u32 x = 0; x < size; ++x) set_dot(number * 128, size / 8, x, y, colour(x, y));
    }
    void map_entry(u32 sx, u32 sy, u16 entry, u32 stamps_per_side = 16) {
        wram.sub_write16(kMap + (sy * stamps_per_side + sx) * 2, entry);
    }

    // One trace vector per line: start (dots, 13.3 written as dots * 8) and
    // step (5.11 written as value * 2048).
    void vector(u32 line, double x, double y, double dx, double dy) {
        const u32 v = kTrace + line * 8;
        wram.sub_write16(v + 0, static_cast<u16>(x * 8));
        wram.sub_write16(v + 2, static_cast<u16>(y * 8));
        wram.sub_write16(v + 4, static_cast<u16>(static_cast<md::s16>(dx * 2048)));
        wram.sub_write16(v + 6, static_cast<u16>(static_cast<md::s16>(dy * 2048)));
    }
    void run() { reg(0x66, kTrace >> 2); }          // writing the trace address starts it

    [[nodiscard]] u8 out(u32 x, u32 y) { return get_dot(kImage, 2, x, y); }
};

// Source pattern: distinct non-zero colour per (x, y) within a 16x16 stamp.
u8 pattern(u32 x, u32 y) { return static_cast<u8>(((x + 5 * y) % 15) + 1); }

} // namespace

TEST(asic_registers_and_masks) {
    Fixture f;
    f.reg(0x58, 0xFFFF);
    CHECK(f.reg_read(0x58) == 0x0007);              // only STS/SMS/RPT writable
    f.reg(0x60, 0xFFFF);
    CHECK(f.reg_read(0x60) == 0x003F);
    f.reg(0x62, 0xFFFF);
    CHECK(f.reg_read(0x62) == 0x01FF);
    f.reg(0x64, 0xFFFF);
    CHECK(f.reg_read(0x64) == 0x00FF);
    CHECK(f.cd.graphics().image_base() == kImage);
}

TEST(asic_stamp_layout_is_column_major_cells) {
    Fixture f;
    f.make_stamp(1, 16, pattern);
    f.map_entry(0, 0, 1);
    // Dot (8, 0) of stamp 1 is in its 3rd cell (column 1, row 0): byte 128 + 2*32.
    CHECK((f.wram.sub_read8(128 + 64) >> 4) == pattern(8, 0));
    CHECK(f.cd.graphics().sample(8, 0) == pattern(8, 0));
    CHECK(f.cd.graphics().sample(3, 12) == pattern(3, 12));
}

TEST(asic_identity_copy) {
    Fixture f;
    f.make_stamp(1, 16, pattern);
    f.map_entry(0, 0, 1);
    for (u32 l = 0; l < kDots; ++l) f.vector(l, 0, l, 1.0, 0.0);
    f.run();
    bool same = true;
    for (u32 y = 0; y < kDots; ++y)
        for (u32 x = 0; x < kDots; ++x) same &= f.out(x, y) == pattern(x, y);
    CHECK(same);
}

TEST(asic_scaling_and_fractional_steps) {
    Fixture f;
    f.make_stamp(1, 16, pattern);
    f.map_entry(0, 0, 1);
    for (u32 l = 0; l < kDots; ++l) f.vector(l, 0, l / 2.0, 0.5, 0.0);   // 2x zoom
    f.run();
    bool zoomed = true;
    for (u32 y = 0; y < kDots; ++y)
        for (u32 x = 0; x < kDots; ++x) zoomed &= f.out(x, y) == pattern(x / 2, y / 2);
    CHECK(zoomed);

    for (u32 l = 0; l < kDots; ++l) f.vector(l, 0.5, l, 0.75, 0.0);       // 0.75 step, start 0.5
    f.run();
    bool fractional = true;
    for (u32 x = 0; x < kDots; ++x) fractional &= f.out(x, 3) == pattern((2 + 3 * x) / 4, 3);
    CHECK(fractional);  // nearest neighbour = floor(0.5 + 0.75 x)
}

TEST(asic_rotation_by_trace_vectors) {
    // Line l walks up column l of the source: a 90-degree rotation.
    Fixture f;
    f.make_stamp(1, 16, pattern);
    f.map_entry(0, 0, 1);
    for (u32 l = 0; l < kDots; ++l) f.vector(l, l, 15, 0.0, -1.0);
    f.run();
    bool rotated = true;
    for (u32 y = 0; y < kDots; ++y)
        for (u32 x = 0; x < kDots; ++x) rotated &= f.out(x, y) == pattern(y, 15 - x);
    CHECK(rotated);
}

TEST(asic_arbitrary_angle_rotation) {
    // 30 degrees about the stamp centre, computed per line as a game would.
    // Checked against the same nearest-neighbour maths done independently.
    Fixture f;
    f.make_stamp(1, 16, pattern);
    f.map_entry(0, 0, 1);
    const double c = 0.8660254, s = 0.5;
    for (u32 l = 0; l < kDots; ++l) {
        const double ox = -8.0, oy = static_cast<double>(l) - 8.0;      // output point, centred
        f.vector(l, 8 + c * ox - s * oy + 32, 8 + s * ox + c * oy + 32, c, s);  // +32: stay positive
    }
    // Put the stamp where those coordinates land (map cell (2,2) = dots 32-47).
    f.map_entry(0, 0, 0);
    f.map_entry(2, 2, 1);
    f.run();

    int matches = 0;
    for (u32 l = 0; l < kDots; ++l) {
        u32 x = static_cast<u32>((8 + c * -8.0 - s * (l - 8.0) + 32) * 8) << 8;
        u32 y = static_cast<u32>((8 + s * -8.0 + c * (l - 8.0) + 32) * 8) << 8;
        const auto dx = static_cast<u32>(static_cast<md::s32>(static_cast<md::s16>(c * 2048)));
        const auto dy = static_cast<u32>(static_cast<md::s32>(static_cast<md::s16>(s * 2048)));
        for (u32 d = 0; d < kDots; ++d, x += dx, y += dy) {
            const u32 sx = (x >> 11) & 0x1FFF, sy = (y >> 11) & 0x1FFF;
            const bool inside = sx >= 32 && sx < 48 && sy >= 32 && sy < 48;
            matches += f.out(d, l) == (inside ? pattern(sx - 32, sy - 32) : 0);
        }
    }
    CHECK(matches == kDots * kDots);
}

TEST(asic_stamp_attributes) {
    // The flip and 180 degrees are direction-independent.
    Fixture f;
    f.make_stamp(1, 16, pattern);
    auto& g = f.cd.graphics();
    f.map_entry(0, 0, 0x8001);                      // H flip
    CHECK(g.sample(0, 5) == pattern(15, 5));
    CHECK(g.sample(15, 9) == pattern(0, 9));
    f.map_entry(0, 0, 0x4001);                      // ROT = 2 (180)
    CHECK(g.sample(2, 3) == pattern(13, 12));

    // Properties of the 90-degree steps that hold whatever the direction:
    // two quarter turns make a half turn, and rot 3 undoes rot 1.
    bool ok = true;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            int ax, ay, bx, by, hx, hy, ix, iy;
            GraphicsAsic::stamp_transform(16, false, 1, x, y, ax, ay);
            GraphicsAsic::stamp_transform(16, false, 1, ax, ay, bx, by);
            GraphicsAsic::stamp_transform(16, false, 2, x, y, hx, hy);
            ok &= bx == hx && by == hy;
            GraphicsAsic::stamp_transform(16, false, 3, ax, ay, ix, iy);
            ok &= ix == x && iy == y;
        }
    CHECK(ok);
}

TEST(asic_transparent_stamp_and_repeat) {
    Fixture f;
    f.make_stamp(1, 16, pattern);
    f.map_entry(0, 0, 1);
    auto& g = f.cd.graphics();
    CHECK(g.sample(16, 0) == 0);                    // map entry (1,0) = stamp 0: transparent
    CHECK(g.sample(256 + 3, 2) == 0);               // outside the map, no repeat
    f.reg(0x58, 0x0001);                            // RPT
    CHECK(g.sample(256 + 3, 2) == pattern(3, 2));   // wraps to (3, 2)
    CHECK(g.sample(3, 512 + 2) == pattern(3, 2));
}

TEST(asic_32x32_stamps) {
    Fixture f;
    f.reg(0x58, 0x0002);                            // SMS: 32x32 stamps (8x8 of them in the map)
    f.make_stamp(4, 32, [](u32 x, u32 y) { return static_cast<u8>(((x / 8) + (y / 8) * 4) % 15 + 1); });
    f.map_entry(0, 0, 5, 8);                        // number 5 is read as 4 (multiple of 4)
    auto& g = f.cd.graphics();
    CHECK(g.sample(0, 0) == 1);
    CHECK(g.sample(8, 0) == 2);                     // cell column 1
    CHECK(g.sample(0, 24) == 13);                   // cell row 3
    // Dot (8, 0) lives in cell 4 (column 1 x 4 cells + row 0): byte 4*128 + 4*32.
    CHECK((f.wram.sub_read8(512 + 128) >> 4) == 2);
}

TEST(asic_image_offset_and_priority_modes) {
    Fixture f;
    f.make_stamp(1, 16, [](u32 x, u32) { return static_cast<u8>(x < 8 ? 0 : 9); });  // left half transparent
    f.map_entry(0, 0, 1);
    for (u32 l = 0; l < kDots; ++l) f.vector(l, 0, l, 1.0, 0.0);

    const auto fill_dest = [&](u8 c) {
        for (u32 y = 0; y < 16; ++y)
            for (u32 x = 0; x < 16; ++x) f.set_dot(kImage, 2, x, y, c);
    };

    // PM off: everything is copied, including transparent dots.
    fill_dest(7);
    f.run();
    CHECK(f.out(2, 2) == 0 && f.out(12, 2) == 9);

    // PM = overwrite: transparent source dots keep the destination.
    fill_dest(7);
    f.cd.sub_bus().write_byte(0xFF8003, 0x10);
    f.run();
    CHECK(f.out(2, 2) == 7 && f.out(12, 2) == 9);

    // PM = underwrite: only empty destination dots are written.
    fill_dest(7);
    f.set_dot(kImage, 2, 12, 3, 0);
    f.cd.sub_bus().write_byte(0xFF8003, 0x08);
    f.run();
    CHECK(f.out(12, 2) == 7 && f.out(12, 3) == 9);

    // Offset (x 3, y 2): output dot (0, 0) lands at buffer dot (3, 2).
    f.cd.sub_bus().write_byte(0xFF8003, 0x00);
    fill_dest(0);
    f.reg(0x62, 8);
    f.reg(0x64, 1);
    f.reg(0x60, (2 << 3) | 3);
    for (u32 l = 0; l < kDots; ++l) f.vector(l, 8, l, 1.0, 0.0);      // the coloured half
    f.run();
    CHECK(f.out(3, 2) == 9 && f.out(10, 2) == 9);
    CHECK(f.out(2, 2) == 0 && f.out(11, 2) == 0 && f.out(3, 3) == 0);
}

TEST(asic_busy_flag_and_int1) {
    Fixture f;
    f.cd.sub_bus().write_word(0xFF8032, 0x0002);    // enable INT1
    f.make_stamp(1, 16, pattern);
    f.map_entry(0, 0, 1);
    for (u32 l = 0; l < kDots; ++l) f.vector(l, 0, l, 1.0, 0.0);
    f.run();
    CHECK((f.reg_read(0x58) & 0x8000) != 0);        // GRON: operation in progress
    CHECK(f.cd.registers().sub_irq_level() == 0);

    // 16 x 16 dots x 4 cycles: done after 1024 sub-CPU cycles.
    f.cd.run_sub_until(1000);
    CHECK((f.reg_read(0x58) & 0x8000) != 0);
    f.cd.run_sub_until(1100);
    CHECK((f.reg_read(0x58) & 0x8000) == 0);
    CHECK(f.cd.registers().sub_irq_level() == 1);   // INT1
    f.cd.registers().acknowledge_sub_interrupt(1);
    CHECK(f.cd.registers().sub_irq_level() == 0);
}

TEST(asic_needs_2m_mode) {
    Fixture f;
    f.make_stamp(1, 16, pattern);
    f.map_entry(0, 0, 1);
    for (u32 l = 0; l < kDots; ++l) f.vector(l, 0, l, 1.0, 0.0);
    f.cd.sub_bus().write_byte(0xFF8003, 0x04);      // switch to 1M mode
    f.run();
    f.cd.sub_bus().write_byte(0xFF8003, 0x00);      // back to 2M: RET=0 keeps it with the sub CPU
    CHECK(f.out(5, 5) == 0);                        // nothing was drawn
}
