#include <array>
#include <vector>

#include "md/memory.hpp"
#include "test.hpp"

using md::Memory;
using md::u8;
using md::u16;
using md::u32;

namespace {

std::vector<u8> make_rom() {
    std::vector<u8> rom(0x1000, 0);
    // Big-endian bytes as they appear in a .bin file.
    rom[0x100] = 0x12; rom[0x101] = 0x34; rom[0x102] = 0x56; rom[0x103] = 0x78;
    return rom;
}

struct FakeVdp final : md::WordDevice {
    std::vector<std::pair<u32, u16>> writes;
    u16 status = 0x3488;
    u16 read(u32 port) override { return port == 0x04 || port == 0x06 ? status : 0xBEEF; }
    void write(u32 port, u16 value) override { writes.emplace_back(port, value); }
};

struct FakeByteDevice final : md::ByteDevice {
    std::vector<std::pair<u32, u8>> writes;
    u8 read(u32 port) override { return static_cast<u8>(0x80 | port); }
    void write(u32 port, u8 value) override { writes.emplace_back(port, value); }
};

} // namespace

TEST(rom_big_endian_reads) {
    Memory mem;
    mem.load_rom(make_rom());
    CHECK(mem.read_byte(0x100) == 0x12);
    CHECK(mem.read_byte(0x101) == 0x34);
    CHECK(mem.read_word(0x100) == 0x1234);
    CHECK(mem.read_word(0x102) == 0x5678);
    CHECK(mem.read_long(0x100) == 0x12345678);
}

TEST(rom_is_read_only_and_open_bus_past_end) {
    Memory mem;
    mem.load_rom(make_rom());
    mem.write_word(0x100, 0xFFFF);
    CHECK(mem.read_word(0x100) == 0x1234);
    CHECK(mem.read_byte(0x2000) == Memory::kOpenBusByte);
}

TEST(ram_big_endian_round_trip) {
    Memory mem;
    mem.write_long(0xFF0000, 0xDEADBEEF);
    CHECK(mem.read_byte(0xFF0000) == 0xDE);
    CHECK(mem.read_byte(0xFF0001) == 0xAD);
    CHECK(mem.read_byte(0xFF0002) == 0xBE);
    CHECK(mem.read_byte(0xFF0003) == 0xEF);
    CHECK(mem.read_word(0xFF0002) == 0xBEEF);
    CHECK(mem.read_long(0xFF0000) == 0xDEADBEEF);

    mem.write_byte(0xFF0011, 0x42);
    CHECK(mem.read_word(0xFF0010) == 0x0042);
}

TEST(ram_mirrors_every_64k_from_e00000) {
    Memory mem;
    mem.write_word(0xFF1234, 0xCAFE);
    CHECK(mem.read_word(0xE01234) == 0xCAFE);
    CHECK(mem.read_word(0xF51234) == 0xCAFE);
    CHECK(mem.main_ram()[0x1234] == 0xCA);
}

TEST(addresses_wrap_to_24_bits) {
    Memory mem;
    mem.write_word(0xFFFF0000, 0x1111);  // upper 8 bits ignored -> 0xFF0000
    CHECK(mem.read_word(0xFF0000) == 0x1111);
    CHECK(mem.read_word(0x00FF0000) == 0x1111);
}

TEST(long_access_spanning_end_of_ram_wraps) {
    Memory mem;
    mem.write_long(0xFFFFFE, 0xAABBCCDD);  // second word lands at 0x000000 (ROM)
    CHECK(mem.read_word(0xFFFFFE) == 0xAABB);
}

TEST(z80_ram_requires_bus_request) {
    Memory mem;
    mem.write_byte(0xA00010, 0x55);
    CHECK(mem.z80_ram()[0x10] == 0);                     // ignored: bus not held
    CHECK(mem.read_byte(0xA00010) == Memory::kOpenBusByte);

    mem.write_word(0xA11100, 0x0100);  // request bus
    mem.write_byte(0xA00010, 0x55);
    CHECK(mem.z80_ram()[0x10] == 0x55);
    CHECK(mem.read_byte(0xA00010) == 0x55);
    CHECK(mem.read_byte(0xA02010) == 0x55);              // 8 KB mirror
}

TEST(z80_word_access_is_8_bit) {
    Memory mem;
    mem.write_byte(0xA11100, 0x01);
    mem.write_word(0xA00020, 0xAB12);                    // only high byte reaches Z80 bus
    CHECK(mem.z80_ram()[0x20] == 0xAB);
    CHECK(mem.z80_ram()[0x21] == 0x00);
    CHECK(mem.read_word(0xA00020) == 0xABAB);            // byte mirrored on both halves
}

TEST(z80_busreq_and_reset_status) {
    Memory mem;
    CHECK(mem.z80_reset_asserted());
    CHECK((mem.read_byte(0xA11100) & 1) == 1);           // not granted

    mem.write_word(0xA11100, 0x0100);
    CHECK(mem.z80_bus_requested());
    CHECK((mem.read_byte(0xA11100) & 1) == 1);           // still in reset -> no ack

    mem.write_word(0xA11200, 0x0100);                    // release reset
    CHECK(!mem.z80_reset_asserted());
    CHECK((mem.read_byte(0xA11100) & 1) == 0);           // granted
    CHECK((mem.read_word(0xA11100) & 0x0100) == 0);

    mem.write_byte(0xA11100, 0x00);                      // give bus back
    CHECK(!mem.z80_bus_requested());
    CHECK((mem.read_word(0xA11100) & 0x0100) == 0x0100);
}

TEST(ym2612_through_z80_space) {
    Memory mem;
    FakeByteDevice ym;
    mem.attach_ym2612(&ym);
    mem.write_byte(0xA11100, 0x01);
    mem.write_byte(0xA04000, 0x22);
    mem.write_byte(0xA04001, 0x0F);
    CHECK(ym.writes.size() == 2);
    CHECK(ym.writes[0] == std::make_pair(u32{0}, u8{0x22}));
    CHECK(ym.writes[1] == std::make_pair(u32{1}, u8{0x0F}));
    CHECK(mem.read_byte(0xA04002) == 0x82);
}

TEST(absent_ym2612_reads_as_idle) {
    Memory mem;
    mem.write_byte(0xA11100, 0x01);
    CHECK(mem.read_byte(0xA04000) == 0x00);  // bit 7 clear: not busy
}

TEST(io_version_and_data_ports) {
    Memory mem;
    mem.set_version_register(0xA1);
    CHECK(mem.read_byte(0xA10001) == 0xA1);
    CHECK(mem.read_word(0xA10000) == 0xA1A1);
    mem.write_byte(0xA10001, 0x00);                      // read-only
    CHECK(mem.read_byte(0xA10001) == 0xA1);

    // Port A: nothing connected, all inputs -> pulled high.
    CHECK(mem.read_byte(0xA10003) == 0x7F);

    // Set TH (bit 6) as output, drive it low.
    mem.write_byte(0xA10009, 0x40);
    mem.write_byte(0xA10003, 0x00);
    CHECK(mem.read_byte(0xA10003) == 0x3F);
    CHECK(mem.read_byte(0xA10009) == 0x40);
}

TEST(vdp_ports_and_mirrors) {
    Memory mem;
    FakeVdp vdp;
    mem.attach_vdp(&vdp);

    mem.write_word(0xC00004, 0x8174);
    mem.write_long(0xC00000, 0x11223344);                // data port, two word writes
    CHECK(vdp.writes.size() == 3);
    CHECK(vdp.writes[0] == std::make_pair(u32{0x04}, u16{0x8174}));
    CHECK(vdp.writes[1] == std::make_pair(u32{0x00}, u16{0x1122}));
    CHECK(vdp.writes[2] == std::make_pair(u32{0x02}, u16{0x3344}));

    // Byte write duplicates the byte on both halves.
    mem.write_byte(0xC00001, 0x9A);
    CHECK(vdp.writes.back() == std::make_pair(u32{0x00}, u16{0x9A9A}));

    CHECK(mem.read_word(0xC00004) == 0x3488);
    CHECK(mem.read_byte(0xC00004) == 0x34);
    CHECK(mem.read_byte(0xC00005) == 0x88);
    CHECK(mem.read_word(0xC00006) == 0x3488);            // 0x06 mirrors control port

    // Decoded mirrors: A8-A15 and A19-A20 are ignored by the VDP.
    CHECK(mem.read_word(0xC80004) == 0x3488);
    CHECK(mem.read_word(0xC00104) == 0x3488);
    // Undecoded (A5-A7 or A16-A18 set) -> open bus.
    CHECK(mem.read_word(0xC00024) == Memory::kOpenBusWord);
    CHECK(mem.read_word(0xC10004) == Memory::kOpenBusWord);
}

TEST(psg_writes) {
    Memory mem;
    FakeByteDevice psg;
    mem.attach_psg(&psg);
    mem.write_byte(0xC00011, 0x9F);
    mem.write_byte(0xC00017, 0xBF);
    mem.write_byte(0xC00010, 0xFF);                      // even address: not the PSG
    CHECK(psg.writes.size() == 2);
    CHECK(psg.writes[0].second == 0x9F);
    CHECK(psg.writes[1].second == 0xBF);
}

TEST(unmapped_expansion_area_is_open_bus) {
    Memory mem;
    CHECK(mem.read_byte(0x400000) == Memory::kOpenBusByte);
    CHECK(mem.read_word(0x900000) == Memory::kOpenBusWord);
}
