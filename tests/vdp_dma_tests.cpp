#include <vector>

#include "md/vdp.hpp"
#include "test.hpp"
#include "vdp_helpers.hpp"

using md::Vdp;
using namespace vdp_test;

namespace {

// Fake 68000 bus for DMA: returns (address >> 1) & 0xFFFF and records reads.
struct DmaFixture : Fixture {
    std::vector<u32> reads;

    DmaFixture() {
        vdp.set_dma_read_callback([this](u32 address) {
            reads.push_back(address);
            return static_cast<u16>(0xA000 | ((address >> 1) & 0x0FFF));
        });
        set_reg(vdp, 1, 0x54);  // display + DMA enable + Mode 5
    }

    void set_length(u16 length) {
        set_reg(vdp, 19, static_cast<u8>(length & 0xFF));
        set_reg(vdp, 20, static_cast<u8>(length >> 8));
    }

    void vram_fill_marker() {
        write_words(vdp, kVramWrite, 0x1000, {0x5A5A, 0x5A5A, 0x5A5A, 0x5A5A, 0x5A5A, 0x5A5A, 0x5A5A, 0x5A5A});
    }

    // 68000 -> VDP transfer from a byte address.
    void set_source(u32 address) {
        const u32 words = address >> 1;
        set_reg(vdp, 21, static_cast<u8>(words & 0xFF));
        set_reg(vdp, 22, static_cast<u8>((words >> 8) & 0xFF));
        set_reg(vdp, 23, static_cast<u8>((words >> 16) & 0x7F));
    }
};

} // namespace

TEST(dma_68k_to_vram) {
    DmaFixture f;
    f.set_length(4);
    f.set_source(0xFF0000);
    command(f.vdp, make_command(kVramWrite | kDma, 0x1000));

    CHECK(f.reads == (std::vector<u32>{0xFF0000, 0xFF0002, 0xFF0004, 0xFF0006}));
    CHECK(f.vdp.vram()[0x1000] == 0xA0);  // 0xFF0000 >> 1 = 0x7F8000 -> 0xA000 | 0x000
    CHECK(f.vdp.vram()[0x1001] == 0x00);
    CHECK(f.vdp.vram()[0x1006] == 0xA0);
    CHECK(f.vdp.vram()[0x1007] == 0x03);
    CHECK(f.vdp.address() == 0x1008);

    // Length counted down to 0, source advanced past the last word.
    CHECK(f.vdp.reg(19) == 0 && f.vdp.reg(20) == 0);
    CHECK(f.vdp.reg(21) == 0x04 && f.vdp.reg(22) == 0x80 && f.vdp.reg(23) == 0x7F);
}

TEST(dma_68k_to_cram_and_vsram) {
    DmaFixture f;
    f.set_length(2);
    f.set_source(0x000100);
    command(f.vdp, make_command(kCramWrite | kDma, 0x0000));
    CHECK(f.vdp.cram()[0] == ((0xA000 | 0x080) & 0x0EEE));
    CHECK(f.vdp.cram()[1] == ((0xA000 | 0x081) & 0x0EEE));

    f.set_length(1);
    f.set_source(0x000200);
    command(f.vdp, make_command(kVsramWrite | kDma, 0x0002));
    CHECK(f.vdp.vsram()[1] == ((0xA000 | 0x100) & 0x07FF));
}

TEST(dma_ignored_when_disabled_in_reg1) {
    DmaFixture f;
    set_reg(f.vdp, 1, 0x44);  // DMA enable off
    f.set_length(4);
    f.set_source(0xFF0000);
    command(f.vdp, make_command(kVramWrite | kDma, 0x1000));
    CHECK(f.reads.empty());
    CHECK(f.vdp.reg(19) == 4);
}

TEST(dma_source_wraps_at_128k) {
    DmaFixture f;
    f.set_length(2);
    f.set_source(0x01FFFE);
    command(f.vdp, make_command(kVramWrite | kDma, 0x0000));
    CHECK(f.reads == (std::vector<u32>{0x01FFFE, 0x000000}));
}

TEST(dma_length_zero_means_64k) {
    DmaFixture f;
    f.set_length(0);
    f.set_source(0xFF0000);
    command(f.vdp, make_command(kVramWrite | kDma, 0x0000));
    CHECK(f.reads.size() == 0x10000);
}

TEST(dma_vram_fill) {
    DmaFixture f;
    set_reg(f.vdp, 15, 1);     // fills normally use auto-increment 1
    f.set_length(6);
    set_reg(f.vdp, 23, 0x80);  // mode: VRAM fill
    command(f.vdp, make_command(kVramWrite | kDma, 0x2000));
    CHECK(f.vdp.vram()[0x2000] == 0x00);   // nothing happens until the data write

    f.vdp.write(kData, 0x5555);
    for (u32 a = 0x2000; a <= 0x2005; ++a) CHECK(f.vdp.vram()[a] == 0x55);
    CHECK(f.vdp.vram()[0x2006] == 0x00);
    CHECK(f.vdp.reg(19) == 0);
    CHECK(f.reads.empty());                 // fill never touches the 68000 bus

    // The fill is one-shot: the next data write is a plain write.
    command(f.vdp, make_command(kVramWrite, 0x3000));
    f.vdp.write(kData, 0x1234);
    CHECK(f.vdp.vram()[0x3000] == 0x12 && f.vdp.vram()[0x3001] == 0x34);
    CHECK(f.vdp.vram()[0x3002] == 0x00);
}

TEST(dma_vram_copy) {
    DmaFixture f;
    write_words(f.vdp, kVramWrite, 0x3000, {0x1122, 0x3344});

    set_reg(f.vdp, 15, 1);
    f.set_length(4);
    set_reg(f.vdp, 21, 0x00);  // copy source = VRAM 0x3000 (byte address)
    set_reg(f.vdp, 22, 0x30);
    set_reg(f.vdp, 23, 0xC0);  // mode: VRAM copy
    command(f.vdp, make_command(kVramCopy, 0x4000));

    CHECK(f.vdp.vram()[0x4000] == 0x11);
    CHECK(f.vdp.vram()[0x4001] == 0x22);
    CHECK(f.vdp.vram()[0x4002] == 0x33);
    CHECK(f.vdp.vram()[0x4003] == 0x44);
    CHECK(f.vdp.reg(21) == 0x04 && f.vdp.reg(22) == 0x30);
    CHECK(f.reads.empty());
}

TEST(dma_from_delayed_source_lands_one_word_late) {
    // Mega-CD Word RAM: the VDP gets each word one transfer late, so the
    // first destination word is skipped and one word fewer is read.
    DmaFixture f;
    f.vdp.set_dma_delay_predicate([](u32 address) { return (address & 0xE00000) == 0x200000; });
    f.vram_fill_marker();
    f.set_length(4);
    f.set_source(0x200002);                       // games add 2 to compensate
    command(f.vdp, make_command(kVramWrite | kDma, 0x1000));

    CHECK(f.reads == (std::vector<u32>{0x200002, 0x200004, 0x200006}));
    CHECK(f.vdp.vram()[0x1000] == 0x5A && f.vdp.vram()[0x1001] == 0x5A);  // untouched
    CHECK(f.vdp.vram()[0x1002] == 0xA0 && f.vdp.vram()[0x1003] == 0x01);  // word read from 0x200002
    CHECK(f.vdp.address() == 0x1008);

    // Other sources are unaffected.
    f.reads.clear();
    f.set_length(2);
    f.set_source(0xFF0000);
    command(f.vdp, make_command(kVramWrite | kDma, 0x2000));
    CHECK(f.reads.size() == 2);
}
