// ROM loading: raw images and ZIP archives (built in memory by the tests).

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#if MD_HAVE_ZLIB
#include <zlib.h>
#endif

#include "md/rom_loader.hpp"
#include "test.hpp"

using md::u16;
using md::u32;
using md::u8;

namespace {

// Minimal ZIP writer for tests: stored (method 0) or deflate (method 8) entries.
class ZipBuilder {
public:
    struct File {
        std::string name;
        std::vector<u8> data;
        bool deflate = false;
        bool encrypted = false;
    };

    ZipBuilder& add(File f) { files_.push_back(std::move(f)); return *this; }

    [[nodiscard]] std::vector<u8> build() const {
        std::vector<u8> out;
        std::vector<u8> directory;
        for (const auto& f : files_) {
            const auto compressed = f.deflate ? deflate(f.data) : f.data;
            const u32 crc = crc32_of(f.data);
            const auto offset = static_cast<u32>(out.size());
            const u16 method = f.deflate ? 8 : 0;
            const u16 flags = f.encrypted ? 1 : 0;

            put32(out, 0x04034B50); put16(out, 20); put16(out, flags); put16(out, method);
            put16(out, 0); put16(out, 0);                       // time, date
            put32(out, crc); put32(out, size32(compressed)); put32(out, size32(f.data));
            put16(out, static_cast<u16>(f.name.size())); put16(out, 0);
            out.insert(out.end(), f.name.begin(), f.name.end());
            out.insert(out.end(), compressed.begin(), compressed.end());

            put32(directory, 0x02014B50); put16(directory, 20); put16(directory, 20);
            put16(directory, flags); put16(directory, method); put16(directory, 0); put16(directory, 0);
            put32(directory, crc); put32(directory, size32(compressed)); put32(directory, size32(f.data));
            put16(directory, static_cast<u16>(f.name.size())); put16(directory, 0); put16(directory, 0);
            put16(directory, 0); put16(directory, 0); put32(directory, 0); put32(directory, offset);
            directory.insert(directory.end(), f.name.begin(), f.name.end());
        }
        const auto dir_offset = static_cast<u32>(out.size());
        out.insert(out.end(), directory.begin(), directory.end());
        put32(out, 0x06054B50); put16(out, 0); put16(out, 0);
        put16(out, static_cast<u16>(files_.size())); put16(out, static_cast<u16>(files_.size()));
        put32(out, size32(directory)); put32(out, dir_offset); put16(out, 0);
        return out;
    }

private:
    static void put16(std::vector<u8>& v, u16 x) { v.push_back(u8(x)); v.push_back(u8(x >> 8)); }
    static void put32(std::vector<u8>& v, u32 x) { put16(v, u16(x)); put16(v, u16(x >> 16)); }
    static u32 size32(const std::vector<u8>& v) { return static_cast<u32>(v.size()); }

    static u32 crc32_of(const std::vector<u8>& data) {
#if MD_HAVE_ZLIB
        return static_cast<u32>(crc32(0L, data.data(), static_cast<uInt>(data.size())));
#else
        u32 crc = 0xFFFFFFFF;  // bitwise CRC-32, same polynomial as zlib
        for (u8 b : data) {
            crc ^= b;
            for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
#endif
    }

    static std::vector<u8> deflate(const std::vector<u8>& data) {
#if MD_HAVE_ZLIB
        z_stream zs{};
        deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        std::vector<u8> out(deflateBound(&zs, static_cast<uLong>(data.size())));
        zs.next_in = const_cast<Bytef*>(data.data());
        zs.avail_in = static_cast<uInt>(data.size());
        zs.next_out = out.data();
        zs.avail_out = static_cast<uInt>(out.size());
        deflate(&zs, Z_FINISH);
        out.resize(zs.total_out);
        deflateEnd(&zs);
        return out;
#else
        return data;
#endif
    }

    std::vector<File> files_;
};

std::vector<u8> fake_rom(std::size_t size) {
    std::vector<u8> rom(size);
    for (std::size_t i = 0; i < size; ++i) rom[i] = static_cast<u8>((i * 7) ^ (i >> 8));
    rom[0x100] = 'S'; rom[0x101] = 'E'; rom[0x102] = 'G'; rom[0x103] = 'A';
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
