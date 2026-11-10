// ROM loading: raw images and ZIP archives (built in memory by the tests).

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "md/rom_loader.hpp"
#include "test.hpp"
#include "zip_builder.hpp"

using md::u16;
using md::u32;
using md::u8;
using test::ZipBuilder;

namespace {


std::vector<u8> fake_rom(std::size_t size) {
    std::vector<u8> rom(size);
    for (std::size_t i = 0; i < size; ++i) rom[i] = static_cast<u8>((i * 7) ^ (i >> 8));
    if (size >= 0x104) {  // cartridge header marker, when the image is large enough
        rom[0x100] = 'S'; rom[0x101] = 'E'; rom[0x102] = 'G'; rom[0x103] = 'A';
    }
    return rom;
}

} // namespace

TEST(zip_detection) {
    CHECK(md::is_zip(ZipBuilder{}.add({"a.bin", {1, 2, 3}}).build()));
    CHECK(!md::is_zip(fake_rom(0x200)));
    CHECK(!md::is_zip(std::vector<u8>{0x50, 0x4B}));
}

TEST(zip_stored_entry) {
    const auto rom = fake_rom(0x4000);
    const auto r = md::extract_rom_from_zip(ZipBuilder{}.add({"game.bin", rom}).build());
    CHECK(r.ok());
    CHECK(r.name == "game.bin");
    CHECK(r.data == rom);
}

#if MD_HAVE_ZLIB
TEST(zip_deflate_entry) {
    const auto rom = fake_rom(0x20000);
    const auto zip = ZipBuilder{}.add({"Game (World).md", rom, true}).build();
    CHECK(zip.size() < rom.size());              // really compressed
    const auto r = md::extract_rom_from_zip(zip);
    CHECK(r.ok());
    CHECK(r.data == rom);
}

TEST(zip_crc_mismatch_is_rejected) {
    auto zip = ZipBuilder{}.add({"game.bin", fake_rom(0x1000)}).build();
    zip[30 + 8 + 0x50] ^= 0xFF;                  // corrupt one data byte
    const auto r = md::extract_rom_from_zip(zip);
    CHECK(!r.ok());
    CHECK(r.error.find("CRC") != std::string::npos);
}
#endif

TEST(zip_prefers_rom_extension_over_larger_files) {
    const auto rom = fake_rom(0x1000);
    const auto zip = ZipBuilder{}
        .add({"readme.txt", std::vector<u8>(0x8000, 'x')})
        .add({"roms/", {}})
        .add({"roms/Game.GEN", rom})
        .build();
    const auto r = md::extract_rom_from_zip(zip);
    CHECK(r.ok());
    CHECK(r.name == "roms/Game.GEN");
    CHECK(r.data == rom);
}

TEST(zip_falls_back_to_largest_file) {
    const auto rom = fake_rom(0x2000);
    const auto zip = ZipBuilder{}.add({"notes.txt", {1, 2}}).add({"game.rom", rom}).build();
    const auto r = md::extract_rom_from_zip(zip);
    CHECK(r.ok());
    CHECK(r.name == "game.rom");
}

TEST(zip_with_cue_sheet_is_reported_as_cd_image) {
    const auto zip = ZipBuilder{}
        .add({"Game (Track 01).bin", fake_rom(0x1000)})
        .add({"Game.cue", {'F', 'I', 'L', 'E'}})
        .build();
    const auto r = md::extract_rom_from_zip(zip);
    CHECK(!r.ok());
    CHECK(r.error.find("Mega-CD disc image") != std::string::npos);
    CHECK(r.error.find("--disc") != std::string::npos);
}

TEST(zip_errors) {
    CHECK(!md::extract_rom_from_zip(std::vector<u8>{0x50, 0x4B, 3, 4, 0, 0}).ok());
    CHECK(!md::extract_rom_from_zip(ZipBuilder{}.build()).ok());          // empty archive

    const auto encrypted = ZipBuilder{}.add({"game.bin", fake_rom(64), false, true}).build();
    const auto r = md::extract_rom_from_zip(encrypted);
    CHECK(!r.ok());
    CHECK(r.error.find("encrypted") != std::string::npos);

    auto truncated = ZipBuilder{}.add({"game.bin", fake_rom(0x1000)}).build();
    truncated.resize(truncated.size() / 2);
    CHECK(!md::extract_rom_from_zip(truncated).ok());
}

TEST(load_rom_file_raw_and_zip) {
    const auto rom = fake_rom(0x3000);
    const std::string raw_path = "rom_loader_test.bin";
    const std::string zip_path = "rom_loader_test.zip";
    {
        std::ofstream(raw_path, std::ios::binary)
            .write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
        const auto zip = ZipBuilder{}.add({"inner.bin", rom}).build();
        std::ofstream(zip_path, std::ios::binary)
            .write(reinterpret_cast<const char*>(zip.data()), static_cast<std::streamsize>(zip.size()));
    }

    const auto a = md::load_rom_file(raw_path);
    CHECK(a.ok() && a.data == rom && a.name == raw_path);
    const auto b = md::load_rom_file(zip_path);
    CHECK(b.ok() && b.data == rom && b.name == zip_path + ":inner.bin");
    const auto c = md::load_rom_file("does_not_exist.bin");
    CHECK(!c.ok());

    // A text file named .zip, and a file too small to be a ROM.
    const std::string fake_zip = "rom_loader_fake.zip";
    const std::string tiny = "rom_loader_tiny.bin";
    std::ofstream(fake_zip) << "not a rom";
    std::ofstream(tiny) << "tiny";
    const auto d = md::load_rom_file(fake_zip);
    CHECK(!d.ok() && d.error.find("not a valid ZIP") != std::string::npos);
    const auto e = md::load_rom_file(tiny);
    CHECK(!e.ok() && e.error.find("too small") != std::string::npos);

    std::remove(raw_path.c_str());
    std::remove(zip_path.c_str());
    std::remove(fake_zip.c_str());
    std::remove(tiny.c_str());
}

TEST(detect_32x_cartridges) {
    const auto with = [](u32 at, std::vector<u8> bytes) {
        std::vector<u8> rom(0x800, 0);
        std::copy(bytes.begin(), bytes.end(), rom.begin() + at);
        return rom;
    };
    const auto text = [](const char* s) { return std::vector<u8>(s, s + std::strlen(s)); };
    CHECK(md::is_32x_rom(with(0x3C0, text("MARS CHECK MODE"))));
    CHECK(md::is_32x_rom(with(0x100, text("SEGA 32X        "))));
    // Homebrew with renamed header fields: found by its start-up code.
    CHECK(md::is_32x_rom(with(0x40C, {0x0C, 0xAD, 'M', 'A', 'R', 'S', 0x30, 0xEC})));
    CHECK(!md::is_32x_rom(with(0x100, text("SEGA MEGA DRIVE "))));
    CHECK(!md::is_32x_rom(std::vector<u8>(0x40, 0)));                // too small
}

TEST(detect_region_from_header) {
    auto with_code = [](const char* code) {
        std::vector<u8> rom(0x200, ' ');
        for (std::size_t i = 0; code[i] && i < 3; ++i) rom[0x1F0 + i] = static_cast<u8>(code[i]);
        return md::detect_region(rom);
    };
    const auto eu = with_code("E");
    CHECK(eu && eu->video == md::Region::PAL && eu->overseas);
    const auto us = with_code("U");
    CHECK(us && us->video == md::Region::NTSC && us->overseas);
    const auto jp = with_code("J");
    CHECK(jp && jp->video == md::Region::NTSC && !jp->overseas);
    const auto world = with_code("JUE");                 // multi-region: USA preferred
    CHECK(world && world->video == md::Region::NTSC && world->overseas);
    const auto eu_jp = with_code("JE");                  // Japan before Europe
    CHECK(eu_jp && !eu_jp->overseas);
    const auto hex_eu = with_code("8");                  // new style: bit 3 = Europe
    CHECK(hex_eu && hex_eu->video == md::Region::PAL);
    const auto hex_all = with_code("F");
    CHECK(hex_all && hex_all->video == md::Region::NTSC && hex_all->overseas);
    CHECK(!with_code("   "));
    CHECK(!md::detect_region(std::vector<u8>(0x100, 0)));  // too small for a header
}
