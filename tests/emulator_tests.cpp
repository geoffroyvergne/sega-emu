#include <cmath>

#include "md/emulator.hpp"
#include "test.hpp"

TEST(refresh_rates) {
    CHECK(std::abs(md::Emulator{md::Region::NTSC}.refresh_rate() - 59.92) < 0.01);
    CHECK(std::abs(md::Emulator{md::Region::PAL}.refresh_rate()  - 49.70) < 0.01);
}

TEST(frame_counter) {
    md::Emulator emu;
    emu.run_frame();
    emu.run_frame();
    CHECK(emu.frame_count() == 2);
    emu.reset();
    CHECK(emu.frame_count() == 0);
}

TEST(controller_state) {
    md::ControllerState pad;
    pad.set(md::Button::Start, true);
    pad.set(md::Button::A, true);
    CHECK(pad.is_pressed(md::Button::Start));
    CHECK(pad.is_pressed(md::Button::A));
    CHECK(!pad.is_pressed(md::Button::B));
    pad.set(md::Button::A, false);
    CHECK(!pad.is_pressed(md::Button::A));
}

TEST(framebuffer_resolution_follows_vdp) {
    // After reset the VDP registers are 0: H32 / V28.
    md::Emulator emu{md::Region::NTSC};
    CHECK(emu.framebuffer().width()  == 256);
    CHECK(emu.framebuffer().height() == 224);
}

TEST(emulator_version_register_matches_region) {
    md::Emulator ntsc{md::Region::NTSC};
    md::Emulator pal{md::Region::PAL};
    CHECK(ntsc.memory().read_byte(0xA10001) == 0xA0);
    CHECK(pal.memory().read_byte(0xA10001)  == 0xE0);
}
