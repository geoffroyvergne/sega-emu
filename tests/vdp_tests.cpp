#include <vector>

#include "md/emulator.hpp"
#include "md/vdp.hpp"
#include "test.hpp"
#include "vdp_helpers.hpp"

using md::Vdp;

using namespace vdp_test;

TEST(vdp_memory_sizes) {
    md::Framebuffer fb;
    Vdp vdp{md::Region::NTSC, fb};
    CHECK(vdp.vram().size_bytes()  == 65536);
    CHECK(vdp.cram().size_bytes()  == 128);
    CHECK(vdp.vsram().size_bytes() == 80);
}

TEST(vdp_well_known_command_encodings) {
    // Cross-check our helper against constants every Mega Drive programmer knows.
    CHECK(make_command(kVramWrite, 0x0000)  == 0x40000000);
    CHECK(make_command(kVramWrite, 0xC000)  == 0x40000003);
    CHECK(make_command(kCramWrite, 0x0000)  == 0xC0000000);
    CHECK(make_command(kVsramWrite, 0x0000) == 0x40000010);
    CHECK(make_command(kCramRead, 0x0000)   == 0x00000020);
}

TEST(vdp_register_writes) {
    Fixture f;
    f.vdp.write(kControl, 0x8C81);  // reg 12: H40
    CHECK(f.vdp.reg(12) == 0x81);
    CHECK(f.vdp.active_width() == 320);
    f.vdp.write(kControl, 0x8C00);
    CHECK(f.vdp.active_width() == 256);

    f.vdp.write(kControl, 0x9F12);  // reg 31 doesn't exist: ignored, no crash
    CHECK(f.vdp.reg(15) == 0x02);
}

TEST(vdp_vram_write_and_read_with_autoincrement) {
    Fixture f;
    command(f.vdp, make_command(kVramWrite, 0xC000));
    f.vdp.write(kData, 0x1234);
    f.vdp.write(kData, 0x5678);
    CHECK(f.vdp.vram()[0xC000] == 0x12);
    CHECK(f.vdp.vram()[0xC001] == 0x34);
    CHECK(f.vdp.vram()[0xC002] == 0x56);
    CHECK(f.vdp.vram()[0xC003] == 0x78);

    command(f.vdp, make_command(kVramRead, 0xC000));
    CHECK(f.vdp.read(kData) == 0x1234);
    CHECK(f.vdp.read(kData) == 0x5678);
}

TEST(vdp_vram_odd_address_swaps_bytes) {
    Fixture f;
    command(f.vdp, make_command(kVramWrite, 0x0101));
    f.vdp.write(kData, 0xAABB);
    CHECK(f.vdp.vram()[0x0101] == 0xAA);
    CHECK(f.vdp.vram()[0x0100] == 0xBB);
}

TEST(vdp_cram_write_masks_to_9_bit_color) {
    Fixture f;
    command(f.vdp, make_command(kCramWrite, 0x0002));  // colour 1
    f.vdp.write(kData, 0xFFFF);
    CHECK(f.vdp.cram()[1] == 0x0EEE);

    command(f.vdp, make_command(kCramRead, 0x0002));
    CHECK(f.vdp.read(kData) == 0x0EEE);

    CHECK(Vdp::cram_to_argb(0x000E) == 0xFFFF0000);   // red
    CHECK(Vdp::cram_to_argb(0x00E0) == 0xFF00FF00);   // green
    CHECK(Vdp::cram_to_argb(0x0E00) == 0xFF0000FF);   // blue
}

TEST(vdp_vsram_write_and_bounds) {
    Fixture f;
    command(f.vdp, make_command(kVsramWrite, 0x0000));
    f.vdp.write(kData, 0x0123);
    CHECK(f.vdp.vsram()[0] == 0x0123);

    command(f.vdp, make_command(kVsramWrite, 0x0050));  // entry 40: out of range
    f.vdp.write(kData, 0x0456);                        // ignored, no overflow

    command(f.vdp, make_command(kVsramRead, 0x0000));
    CHECK(f.vdp.read(kData) == 0x0123);
}

TEST(vdp_status_read_cancels_pending_command) {
    Fixture f;
    f.vdp.write(kControl, 0x4000);   // first half of a command
    (void)f.vdp.read(kControl);      // status read clears the pending flag
    f.vdp.write(kControl, 0x8F04);   // must now be a register write
    CHECK(f.vdp.auto_increment() == 4);
}

TEST(vdp_second_command_word_is_not_a_register_write) {
    Fixture f;
    f.vdp.write(kControl, 0x4000);
    f.vdp.write(kControl, 0x8000);   // looks like "reg 0 = 0", but is word 2
    CHECK(f.vdp.reg(0) == 0);
    CHECK(f.vdp.reg(15) == 2);       // registers untouched
}

TEST(vdp_status_flags) {
    Fixture f;
    f.vdp.write(kControl, 0x8144);   // display on, VINT off, Mode 5
    CHECK((f.vdp.status() & Vdp::Status::FifoEmpty) != 0);
    CHECK((f.vdp.status() & Vdp::Status::VBlank) == 0);
    CHECK((f.vdp.status() & Vdp::Status::Pal) == 0);

    f.vdp.advance(Vdp::kHBlankStart);
    CHECK((f.vdp.status() & Vdp::Status::HBlank) != 0);

    f.advance_to_line(224);
    CHECK((f.vdp.status() & Vdp::Status::VBlank) != 0);
    CHECK((f.vdp.status() & Vdp::Status::VintPending) != 0);  // F set even with VINT disabled
    CHECK(f.irq_changes.empty());                             // ...but no IRQ

    f.vdp.write(kControl, 0x8104);   // display off -> VBlank always reported
    f.advance_to_line(10);
    CHECK((f.vdp.status() & Vdp::Status::VBlank) != 0);
}

TEST(vdp_vint_raises_level_6_and_ack_clears_it) {
    Fixture f;
    f.vdp.write(kControl, 0x8164);   // display on, VINT enabled
    f.advance_to_line(223);
    CHECK(f.vdp.irq_level() == 0);

    f.advance_to_line(224);
    CHECK(f.vdp.irq_level() == Vdp::kVintLevel);
    CHECK(f.irq_changes == std::vector<int>{6});

    f.vdp.acknowledge_interrupt(6);
    CHECK(f.vdp.irq_level() == 0);
    CHECK((f.vdp.status() & Vdp::Status::VintPending) == 0);
}

TEST(vdp_enabling_vint_while_pending_raises_immediately) {
    Fixture f;
    f.vdp.write(kControl, 0x8144);   // VINT disabled
    f.advance_to_line(224);
    CHECK(f.vdp.irq_level() == 0);
    f.vdp.write(kControl, 0x8164);   // enable while F is set
    CHECK(f.vdp.irq_level() == Vdp::kVintLevel);
}

TEST(vdp_hint_counter_period) {
    Fixture f;
    f.vdp.write(kControl, 0x8014);   // HINT enabled
    f.vdp.write(kControl, 0x8A0F);   // every 16 lines
    f.vdp.write(kControl, 0x8144);

    // Let one VBlank pass so the counter is loaded from reg 10, acknowledging
    // the HINTs raised along the way (the counter starts at 0 after reset).
    while (f.vdp.frame() == 0) {
        f.vdp.advance(f.vdp.cycles_to_next_event());
        f.vdp.acknowledge_interrupt(4);
    }

    int hints = 0;
    std::vector<int> lines;
    while (f.vdp.frame() == 1) {
        f.vdp.advance(f.vdp.cycles_to_next_event());
        if (f.vdp.irq_level() == Vdp::kHintLevel) {
            ++hints;
            lines.push_back(f.vdp.line());
            f.vdp.acknowledge_interrupt(4);
        }
    }
    // Underflow on lines 15, 31, ..., 223: 14 interrupts per frame.
    CHECK(hints == 14);
    CHECK(!lines.empty() && lines.front() == 15);
    CHECK(!lines.empty() && lines.back() == 223);
}

TEST(vdp_vint_has_priority_over_hint) {
    Fixture f;
    f.vdp.write(kControl, 0x8014);   // HINT on
    f.vdp.write(kControl, 0x8A00);   // every line
    f.vdp.write(kControl, 0x8164);   // VINT on
    f.advance_to_line(224);
    f.vdp.advance(Vdp::kHBlankStart); // HINT also pending now
    CHECK(f.vdp.irq_level() == Vdp::kVintLevel);
    f.vdp.acknowledge_interrupt(6);
    CHECK(f.vdp.irq_level() == Vdp::kHintLevel);
    f.vdp.acknowledge_interrupt(4);
    CHECK(f.vdp.irq_level() == 0);
}

TEST(vdp_hv_counter) {
    Fixture f;
    CHECK(f.vdp.read(kHv) == 0x0000);
    f.advance_to_line(100);
    CHECK((f.vdp.read(kHv) >> 8) == 100);
    f.advance_to_line(234);
    CHECK((f.vdp.read(kHv) >> 8) == 0xEA);
    f.advance_to_line(235);
    CHECK((f.vdp.read(kHv) >> 8) == 0xE5);  // NTSC V28 jump
    f.advance_to_line(261);
    CHECK((f.vdp.read(kHv) >> 8) == 0xFF);

    f.vdp.write(kControl, 0x8C81);          // H40
    f.vdp.advance(Vdp::kMasterCyclesPerLine - 1);
    CHECK((f.vdp.read(kHv) & 0xFF) == 0xFF); // end of line: last H value
}

TEST(vdp_frame_has_correct_line_count) {
    md::Framebuffer fb;
    Vdp ntsc{md::Region::NTSC, fb};
    Vdp pal{md::Region::PAL, fb};
    ntsc.advance(Vdp::kMasterCyclesPerLine * 262);
    pal.advance(Vdp::kMasterCyclesPerLine * 313);
    CHECK(ntsc.frame() == 1 && ntsc.line() == 0);
    CHECK(pal.frame() == 1 && pal.line() == 0);
    CHECK((pal.status() & Vdp::Status::Pal) != 0);
}

TEST(vdp_renders_backdrop_color) {
    Fixture f;
    command(f.vdp, make_command(kCramWrite, 0x0000));
    f.vdp.write(kData, 0x000E);      // colour 0 = red
    f.vdp.write(kControl, 0x8700);   // backdrop = palette 0, colour 0
    f.vdp.advance(Vdp::kHBlankStart);
    CHECK(f.fb.row(0)[0]   == 0xFFFF0000);
    CHECK(f.fb.row(0)[255] == 0xFFFF0000);
}

// --- Full system: 68000 program driven by VDP interrupts --------------------

namespace {

std::vector<u8> make_interrupt_rom() {
    std::vector<u8> rom(0x1000, 0);
    auto put_word = [&](u32 a, u16 v) { rom[a] = static_cast<u8>(v >> 8); rom[a + 1] = static_cast<u8>(v); };
    auto put_long = [&](u32 a, u32 v) { put_word(a, static_cast<u16>(v >> 16)); put_word(a + 2, static_cast<u16>(v)); };
    auto code = [&](u32 a, std::initializer_list<u16> words) { for (u16 w : words) { put_word(a, w); a += 2; } };

    put_long(0x00, 0x00FFFE00);  // SSP
    put_long(0x04, 0x00000200);  // PC
    put_long(28 * 4, 0x400);     // level 4 autovector: HINT
    put_long(30 * 4, 0x300);     // level 6 autovector: VINT

    code(0x200, {
        0x33FC, 0x8014, 0x00C0, 0x0004,  // move.w #$8014,($C00004).l  ; HINT on
        0x33FC, 0x8A1F, 0x00C0, 0x0004,  // move.w #$8A1F,($C00004).l  ; every 32 lines
        0x33FC, 0x8164, 0x00C0, 0x0004,  // move.w #$8164,($C00004).l  ; display + VINT on
        0x46FC, 0x2000,                  // move   #$2000,sr           ; unmask
        0x60FE,                          // bra.s  *
    });
    code(0x300, {
        0x5279, 0x00FF, 0x0000,          // addq.w #1,($FF0000).l      ; VINT count
        0x4E73,                          // rte
    });
    code(0x400, {
        0x5279, 0x00FF, 0x0002,          // addq.w #1,($FF0002).l      ; HINT count
        0x4E73,                          // rte
    });
    return rom;
}

} // namespace

TEST(system_vint_and_hint_reach_the_68000) {
    md::Emulator emu{md::Region::NTSC};
    emu.load_rom(make_interrupt_rom());

    emu.run_frame();                 // first frame: counter not yet loaded from reg 10
    const u16 hints_first = emu.memory().read_word(0xFF0002);
    emu.run_frame();
    emu.run_frame();

    CHECK(emu.memory().read_word(0xFF0000) == 3);              // one VINT per frame
    // Lines 31, 63, ..., 223 = 7 HINTs per full frame.
    CHECK(emu.memory().read_word(0xFF0002) - hints_first == 14);
    CHECK(emu.vdp().irq_level() == 0);                         // all acknowledged
}
