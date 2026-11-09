// Mega-CD CD path: disc images (.cue/.bin/.iso), the LC8951 CDC, the drive.

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include "md/megacd/cdc.hpp"
#include "md/megacd/disc.hpp"
#include "md/megacd/megacd.hpp"
#include "test.hpp"
#include "zip_builder.hpp"

using md::u16;
using md::u32;
using md::u8;
using namespace md::megacd;

namespace {

// --- Disc helpers --------------------------------------------------------------

Disc::FileSizeFn sizes(std::map<std::string, md::u64> table) {
    return [table](const std::string& path) -> std::optional<md::u64> {
        for (const auto& [name, size] : table) {
            if (path.size() >= name.size() && path.compare(path.size() - name.size(), name.size(), name) == 0) {
                return size;
            }
        }
        return std::nullopt;
    };
}

// A temporary file, deleted at the end of the test.
struct TempFile {
    std::string path;
    TempFile(std::string p, const std::vector<u8>& data) : path(std::move(p)) {
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    TempFile(std::string p, const std::string& text) : path(std::move(p)) { std::ofstream(path) << text; }
    ~TempFile() { std::remove(path.c_str()); }
};

// 2048-byte sectors, sector i filled with (i & 0xFF).
std::vector<u8> iso_image(u32 sectors) {
    std::vector<u8> data(sectors * 2048u);
    for (u32 i = 0; i < data.size(); ++i) data[i] = static_cast<u8>(i / 2048);
    return data;
}

// A raw Mode 1 sector for `lba` with user data filled with `fill`.
RawSector raw_sector(u32 lba, u8 fill) {
    RawSector s{};
    s[0] = 0x00;
    for (int i = 1; i <= 10; ++i) s[static_cast<std::size_t>(i)] = 0xFF;
    s[11] = 0x00;
    write_data_header(s.data() + 12, lba, 1);
    for (std::size_t i = 16; i < 16 + 2048; ++i) s[i] = static_cast<u8>(fill + i);
    return s;
}

// --- CDC access through the gate array, as the sub CPU does it ----------------

constexpr u32 kSubCdcMode = 0xFF8004;
constexpr u32 kSubCdcData = 0xFF8006;
constexpr u32 kSubHostData = 0xFF8008;
constexpr u32 kSubDmaAddr = 0xFF800A;
constexpr u32 kSubIntMask = 0xFF8032;

void cdc_write(MegaCd& cd, u8 reg, u8 value) {
    cd.sub_bus().write_byte(kSubCdcMode + 1, reg);
    cd.sub_bus().write_byte(kSubCdcData + 1, value);
}
u8 cdc_read(MegaCd& cd, u8 reg) {
    cd.sub_bus().write_byte(kSubCdcMode + 1, reg);
    return cd.sub_bus().read_byte(kSubCdcData + 1);
}
void cdc_write16(MegaCd& cd, u8 reg_low, u16 value) {  // e.g. DBC, DAC, WA
    cdc_write(cd, reg_low, static_cast<u8>(value));
    cdc_write(cd, static_cast<u8>(reg_low + 1), static_cast<u8>(value >> 8));
}
void set_destination(MegaCd& cd, Cdc::Destination d) {
    cd.sub_bus().write_byte(kSubCdcMode, static_cast<u8>(d));
}

constexpr u8 kIfctrl = 1, kDbc = 2, kDac = 4, kDttrg = 6, kDtack = 7, kWa = 8, kCtrl0 = 10,
             kHead0 = 4, kIfstat = 1, kStat3 = 15;

// Decoder on + buffer writes; interrupts and data output as requested.
void enable_decoder(MegaCd& cd, u8 ifctrl) {
    cdc_write(cd, kCtrl0, Cdc::kDecoderEnable | Cdc::kWriteRequest);
    cdc_write(cd, kIfctrl, ifctrl);
}

} // namespace

// ==============================================================================
// Disc images
// ==============================================================================

TEST(cue_single_file_multi_track) {
    std::string error;
    const auto disc = Disc::parse_cue(
        "FILE \"Game (USA).bin\" BINARY\n"
        "  TRACK 01 MODE1/2352\n"
        "    INDEX 01 00:00:00\n"
        "  TRACK 02 AUDIO\n"
        "    INDEX 00 00:10:00\n"
        "    INDEX 01 00:12:00\n",
        "dir", sizes({{"Game (USA).bin", 2352ull * 2000}}), error);
    CHECK(disc != nullptr);
    if (!disc) return;
    const auto& t = disc->tracks();
    CHECK(t.size() == 2);
    CHECK(t[0].type == TrackType::Mode1_2352 && t[0].start_lba == 0 && t[0].end_lba == 750);
    CHECK(t[1].type == TrackType::Audio);
    CHECK(t[1].first_lba == 750 && t[1].start_lba == 900 && t[1].end_lba == 2000);
    CHECK(t[1].file_offset == 900ull * 2352);
    CHECK(disc->sector_count() == 2000);
    CHECK(disc->track_at(800)->number == 2);              // INDEX 00 belongs to track 2
    CHECK(disc->files()[0].find("Game (USA).bin") != std::string::npos);
}

TEST(cue_multi_file_with_pregap) {
    std::string error;
    const auto disc = Disc::parse_cue(
        "REM a comment\n"
        "FILE \"t1.bin\" BINARY\n"
        "  TRACK 01 MODE1/2048\n"
        "    INDEX 01 00:00:00\n"
        "FILE \"t2.bin\" BINARY\n"
        "  TRACK 02 AUDIO\n"
        "    PREGAP 00:02:00\n"
        "    INDEX 01 00:00:00\n",
        "", sizes({{"t1.bin", 2048ull * 300}, {"t2.bin", 2352ull * 500}}), error);
    CHECK(disc != nullptr);
    if (!disc) return;
    const auto& t = disc->tracks();
    CHECK(t[0].start_lba == 0 && t[0].end_lba == 300 && t[0].sector_size == 2048);
    CHECK(t[1].first_lba == 300);                          // 150 silent sectors...
    CHECK(t[1].start_lba == 450);                          // ...then the file's audio
    CHECK(t[1].end_lba == 950);
    CHECK(t[1].file == 1 && t[1].file_offset == 0);
}

TEST(cue_errors) {
    std::string error;
    CHECK(!Disc::parse_cue("TRACK 01 AUDIO\n", "", sizes({}), error));
    CHECK(error.find("TRACK before FILE") != std::string::npos);
    CHECK(!Disc::parse_cue("FILE \"a.bin\" BINARY\nTRACK 01 MODE2/2336\nINDEX 01 00:00:00\n", "",
                           sizes({{"a.bin", 2352}}), error));
    CHECK(error.find("unsupported track mode") != std::string::npos);
    CHECK(!Disc::parse_cue("FILE \"missing.bin\" BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n", "",
                           sizes({}), error));
    CHECK(error.find("cannot find") != std::string::npos);
    CHECK(!Disc::parse_cue("FILE \"a.bin\" BINARY\nTRACK 01 AUDIO\nINDEX 01 00:99:00\n", "",
                           sizes({{"a.bin", 2352}}), error));
    CHECK(error.find("bad time") != std::string::npos);
}

TEST(disc_iso_rebuilds_sector_headers) {
    TempFile iso{"cd_test.iso", iso_image(20)};
    std::string error;
    const auto disc = Disc::open(iso.path, error);
    CHECK(disc != nullptr);
    if (!disc) return;
    CHECK(disc->sector_count() == 20);

    RawSector s{};
    CHECK(disc->read_sector(5, s));
    CHECK(s[0] == 0x00 && s[1] == 0xFF && s[10] == 0xFF && s[11] == 0x00);  // sync
    CHECK(s[12] == 0x00 && s[13] == 0x02 && s[14] == 0x05 && s[15] == 0x01); // 00:02:05, mode 1
    CHECK(s[16] == 5 && s[16 + 2047] == 5);                                // user data
    CHECK(!disc->read_sector(20, s));                                      // past the end
}

TEST(disc_raw_bin_and_cue_files) {
    std::vector<u8> raw;
    for (u32 lba = 0; lba < 10; ++lba) {
        const auto s = raw_sector(lba, static_cast<u8>(lba * 3));
        raw.insert(raw.end(), s.begin(), s.end());
    }
    TempFile bin{"cd_test_raw.bin", raw};
    std::string error;
    const auto lone = Disc::open(bin.path, error);         // no cue: sync pattern detected
    CHECK(lone && lone->tracks()[0].type == TrackType::Mode1_2352);

    TempFile cue{"cd_test.cue", std::string("FILE \"cd_test_raw.bin\" BINARY\n TRACK 01 MODE1/2352\n  INDEX 01 00:00:00\n")};
    const auto disc = Disc::open(cue.path, error);
    CHECK(disc != nullptr);
    if (!disc) return;
    RawSector s{};
    CHECK(disc->read_sector(7, s));
    CHECK(s == raw_sector(7, 21));                         // raw sectors pass through unchanged
}

// ==============================================================================
// CDC
// ==============================================================================

TEST(cdc_register_address_auto_increments) {
    MegaCd cd;
    cd.sub_bus().write_byte(kSubCdcMode + 1, kWa);
    cd.sub_bus().write_byte(kSubCdcData + 1, 0x34);       // WAL, CA -> 9
    cd.sub_bus().write_byte(kSubCdcData + 1, 0x12);       // WAH, CA -> 10
    CHECK(cd.cdc().write_address() == 0x1234);
    CHECK((cd.sub_bus().read_word(kSubCdcMode) & 0x0F) == 10);

    cd.sub_bus().write_byte(kSubCdcMode + 1, 10);          // read side: 10/11 = WAL/WAH
    (void)cd.sub_bus().read_byte(kSubCdcData);             // high byte: no side effect
    CHECK(cd.cdc().register_address() == 10);
    CHECK(cd.sub_bus().read_byte(kSubCdcData + 1) == 0x34);
    CHECK(cd.sub_bus().read_byte(kSubCdcData + 1) == 0x12);
    CHECK(cdc_read(cd, kIfstat) == 0xFF);                  // reset state: all flags inactive
}

TEST(cdc_decodes_sector_into_buffer_and_raises_int5) {
    MegaCd cd;
    cd.sub_bus().write_word(kSubIntMask, 0x0020);          // enable INT5
    cd.cdc().decode_sector(raw_sector(0, 0));              // decoder off: ignored
    CHECK(cd.cdc().sectors_decoded() == 0);

    enable_decoder(cd, Cdc::kDeciEnable);
    const auto sector = raw_sector(1234, 7);
    cd.cdc().decode_sector(sector);

    CHECK(cd.cdc().sectors_decoded() == 1);
    CHECK(std::equal(sector.begin(), sector.end(), cd.cdc().buffer().begin()));
    CHECK(cd.cdc().pointer() == 12);                       // PT -> header
    CHECK(cd.cdc().write_address() == 2352);               // WA -> next sector
    CHECK(cdc_read(cd, kHead0) == sector[12]);
    CHECK(cdc_read(cd, kHead0 + 2) == sector[14]);
    CHECK((cdc_read(cd, kIfstat) & Cdc::kDeci) == 0);      // DECI pending (active low)
    CHECK(cd.registers().sub_irq_level() == 5);

    (void)cdc_read(cd, kStat3);                            // acknowledge
    CHECK((cdc_read(cd, kIfstat) & Cdc::kDeci) != 0);
    CHECK(cd.registers().sub_irq_level() == 0);

    cd.cdc().decode_sector(raw_sector(1235, 9));           // next sector follows in the buffer
    CHECK(cd.cdc().pointer() == 2352 + 12);
}

TEST(cdc_buffer_wraps_at_16k) {
    MegaCd cd;
    enable_decoder(cd, 0);
    for (u32 i = 0; i < 7; ++i) cd.cdc().decode_sector(raw_sector(i, static_cast<u8>(i)));
    CHECK(cd.cdc().write_address() == 7 * 2352);           // register keeps counting
    const u32 sixth = (6 * 2352) & (Cdc::kBufferSize - 1); // ...the RAM wraps
    CHECK(cd.cdc().buffer()[(sixth + 12) & (Cdc::kBufferSize - 1)] == raw_sector(6, 6)[12]);
}

TEST(cdc_host_read_by_sub_cpu) {
    MegaCd cd;
    cd.sub_bus().write_word(kSubIntMask, 0x0020);
    enable_decoder(cd, Cdc::kDteiEnable | Cdc::kDoutEnable);
    const auto sector = raw_sector(0, 0x40);
    cd.cdc().decode_sector(sector);

    set_destination(cd, Cdc::Destination::SubHost);
    cdc_write16(cd, kDbc, 7);                              // 8 bytes
    cdc_write16(cd, kDac, static_cast<u16>(cd.cdc().pointer() + 4));  // user data
    cdc_write(cd, kDttrg, 0);
    CHECK((cd.sub_bus().read_word(kSubCdcMode) & 0x4000) != 0);       // DSR

    for (std::size_t i = 0; i < 4; ++i) {
        const u16 expected = static_cast<u16>((sector[16 + 2 * i] << 8) | sector[17 + 2 * i]);
        CHECK(cd.sub_bus().read_word(kSubHostData) == expected);
    }
    const u16 mode = cd.sub_bus().read_word(kSubCdcMode);
    CHECK((mode & 0x8000) != 0 && (mode & 0x4000) == 0);   // EDT, no DSR
    CHECK((cdc_read(cd, kIfstat) & Cdc::kDtei) == 0);      // transfer-end interrupt
    CHECK(cd.registers().sub_irq_level() == 5);
    cdc_write(cd, kDtack, 0);
    CHECK(cd.registers().sub_irq_level() == 0);
}

TEST(cdc_host_read_by_main_cpu) {
    MegaCd cd;
    enable_decoder(cd, Cdc::kDoutEnable);
    const auto sector = raw_sector(3, 0x11);
    cd.cdc().decode_sector(sector);
    set_destination(cd, Cdc::Destination::MainHost);
    cdc_write16(cd, kDbc, 3);
    cdc_write16(cd, kDac, 16);
    cdc_write(cd, kDttrg, 0);

    CHECK((cd.read16(0xA12004) & 0x4000) != 0);            // main sees DSR
    CHECK(cd.sub_bus().read_word(kSubHostData) == 0);      // wrong CPU: nothing
    CHECK(cd.read16(0xA12008) == static_cast<u16>((sector[16] << 8) | sector[17]));
    CHECK(cd.read16(0xA12008) == static_cast<u16>((sector[18] << 8) | sector[19]));
    CHECK((cd.read16(0xA12004) & 0x8000) != 0);            // EDT
}

TEST(cdc_dma_to_prg_ram) {
    MegaCd cd;
    enable_decoder(cd, Cdc::kDoutEnable);
    const auto sector = raw_sector(42, 0x22);
    cd.cdc().decode_sector(sector);

    set_destination(cd, Cdc::Destination::PrgRam);
    cd.sub_bus().write_word(kSubDmaAddr, 0x8000 >> 3);
    cdc_write16(cd, kDbc, 2047);                           // one sector of user data
    cdc_write16(cd, kDac, 16);
    cdc_write(cd, kDttrg, 0);

    bool same = true;
    for (u32 i = 0; i < 2048; ++i) same &= cd.program_ram().sub_read8(0x8000 + i) == sector[16 + i];
    CHECK(same);
    CHECK(cd.sub_bus().read_word(kSubDmaAddr) == (0x8000 + 2048) >> 3);
    CHECK((cd.sub_bus().read_word(kSubCdcMode) & 0x8000) != 0);  // EDT
}

TEST(cdc_dma_to_pcm_ram) {
    MegaCd cd;
    enable_decoder(cd, Cdc::kDoutEnable);
    const auto sector = raw_sector(7, 0x40);
    cd.cdc().decode_sector(sector);

    cd.sub_bus().write_byte(0xFF000F, 0x02);               // PCM window on wave RAM bank 2
    set_destination(cd, Cdc::Destination::Pcm);
    cd.sub_bus().write_word(kSubDmaAddr, 0x100 >> 2);      // byte 0x100 of the window (units of 4)
    cdc_write16(cd, kDbc, 511);                            // 512 bytes
    cdc_write16(cd, kDac, 16);
    cdc_write(cd, kDttrg, 0);

    bool same = true;
    for (u32 i = 0; i < 512; ++i) same &= cd.pcm().ram(0x2000 + 0x100 + i) == sector[16 + i];
    CHECK(same);
    CHECK(cd.sub_bus().read_word(kSubDmaAddr) == (0x100 + 512) >> 2);
}

TEST(cdc_dma_to_word_ram) {
    MegaCd cd;
    cd.write8(0xA12003, 0x02);                             // main gives Word RAM to the sub CPU (2M)
    enable_decoder(cd, Cdc::kDoutEnable);
    const auto sector = raw_sector(1, 0x33);
    cd.cdc().decode_sector(sector);
    set_destination(cd, Cdc::Destination::WordRam);
    cd.sub_bus().write_word(kSubDmaAddr, 0x100 >> 3);
    cdc_write16(cd, kDbc, 15);
    cdc_write16(cd, kDac, 16);
    cdc_write(cd, kDttrg, 0);
    CHECK(cd.sub_bus().read_word(0x080100) == static_cast<u16>((sector[16] << 8) | sector[17]));
    CHECK(cd.sub_bus().read_word(0x08010E) == static_cast<u16>((sector[30] << 8) | sector[31]));
}

TEST(cdc_transfer_needs_data_output_enabled) {
    MegaCd cd;
    enable_decoder(cd, 0);                                 // DOUTEN off
    cd.cdc().decode_sector(raw_sector(0, 0));
    set_destination(cd, Cdc::Destination::SubHost);
    cdc_write16(cd, kDbc, 1);
    cdc_write(cd, kDttrg, 0);
    CHECK(!cd.cdc().data_set_ready());
}

// ==============================================================================
// Drive streaming
// ==============================================================================

TEST(drive_streams_at_1x_and_2x) {
    TempFile iso{"cd_test_speed.iso", iso_image(300)};
    std::string error;
    MegaCd cd;
    cd.insert_disc(Disc::open(iso.path, error));
    enable_decoder(cd, 0);

    cd.drive().read(0);
    cd.run_sub_until(12'500'000);                          // 1 second of sub-CPU time
    CHECK(cd.cdc().sectors_decoded() == 75);
    CHECK(cd.drive().lba() == 75);

    cd.drive().set_speed(2);
    cd.run_sub_until(25'000'000);
    CHECK(cd.cdc().sectors_decoded() == 75 + 150);

    cd.run_sub_until(37'500'000);                          // only 75 sectors left
    CHECK(cd.cdc().sectors_decoded() == 300);
    CHECK(!cd.drive().reading());                          // stopped at the end of the disc
}

TEST(drive_to_sub_cpu_end_to_end) {
    // Sub CPU program: enable the decoder, wait for DECI, report HEAD2 (the
    // sector's frame number) to the main CPU through COMSTAT0.
    TempFile iso{"cd_test_e2e.iso", iso_image(50)};
    std::string error;
    MegaCd cd;
    cd.insert_disc(Disc::open(iso.path, error));

    const std::vector<u16> program = {
        0x13FC, 0x000A, 0x00FF, 0x8005,   // move.b #10,($FF8005).l   CA = CTRL0
        0x13FC, 0x0084, 0x00FF, 0x8007,   // move.b #$84,($FF8007).l  decoder + buffer on
        // wait (0x210):
        0x13FC, 0x0001, 0x00FF, 0x8005,   // move.b #1,($FF8005).l    CA = IFSTAT
        0x0839, 0x0005, 0x00FF, 0x8007,   // btst #5,($FF8007).l      DECI (active low)
        0x66EE,                           // bne.s wait
        0x13FC, 0x0006, 0x00FF, 0x8005,   // move.b #6,($FF8005).l    CA = HEAD2
        0x1039, 0x00FF, 0x8007,           // move.b ($FF8007).l,d0
        0x33C0, 0x00FF, 0x8020,           // move.w d0,($FF8020).l    COMSTAT0
        0x60FE,                           // bra.s *
    };
    const std::vector<u16> vectors = {0x0007, 0xFFF0, 0x0000, 0x0200};
    for (std::size_t i = 0; i < vectors.size(); ++i) cd.sub_bus().write_word(static_cast<u32>(2 * i), vectors[i]);
    for (std::size_t i = 0; i < program.size(); ++i) cd.sub_bus().write_word(0x200 + static_cast<u32>(2 * i), program[i]);

    cd.write8(0xA12001, 0x01);            // main CPU releases the sub CPU
    cd.run_sub_until(1000);               // sub enables its decoder
    cd.drive().read(10);                  // drive starts at LBA 10 (MSF 00:02:10)
    cd.run_sub_until(1000 + 12'500'000 / 75 + 2000);

    CHECK(cd.cdc().sectors_decoded() >= 1);
    CHECK(cd.read16(0xA12020) == 0x0010);  // frame 10, in BCD
}

// ==============================================================================
// Zipped disc images
// ==============================================================================

namespace {

// A small raw disc (cue + .img, as in many archives) zipped in memory.
std::vector<u8> zipped_disc(const std::string& image_name, u32 sectors, bool compress) {
    std::vector<u8> img;
    for (u32 lba = 0; lba < sectors; ++lba) {
        const auto sector = raw_sector(lba, static_cast<u8>(lba));
        img.insert(img.end(), sector.begin(), sector.end());
    }
    const std::string cue = "FILE \"" + image_name + "\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n";
    return test::ZipBuilder{}
        .add({"Game.ccd", {'x'}})
        .add({"Game.cue", std::vector<u8>(cue.begin(), cue.end())})
        .add({image_name, img, compress})
        .build();
}

struct CacheDir {
    std::string path;
    explicit CacheDir(std::string p) : path(std::move(p)) { std::filesystem::remove_all(path); }
    ~CacheDir() { std::filesystem::remove_all(path); }
};

} // namespace

TEST(disc_from_zip_archive) {
    TempFile zip{"cd_test_disc.zip", zipped_disc("Game (EU).img", 12, true)};
    CacheDir cache{"cd_test_cache"};
    std::string error;
    const auto disc = Disc::open(zip.path, error, cache.path);
    CHECK(disc != nullptr);
    if (!disc) { std::printf("    error: %s\n", error.c_str()); return; }
    CHECK(disc->sector_count() == 12);                       // the .cue inside was chosen
    RawSector s{};
    CHECK(disc->read_sector(9, s) && s == raw_sector(9, 9));
    CHECK(std::filesystem::exists(cache.path + "/Game.ccd")); // everything unpacked

    // Second open: files already there, nothing to unpack.
    const auto mtime = std::filesystem::last_write_time(cache.path + "/Game (EU).img");
    const auto again = Disc::open(zip.path, error, cache.path);
    CHECK(again && again->sector_count() == 12);
    CHECK(std::filesystem::last_write_time(cache.path + "/Game (EU).img") == mtime);
}

TEST(disc_zip_errors) {
    CacheDir cache{"cd_test_cache_errors"};
    std::string error;

    // "Zip slip": a name escaping the extraction folder is refused.
    TempFile evil{"cd_test_evil.zip", test::ZipBuilder{}
        .add({"Game.cue", {'F'}}).add({"../escaped.bin", {1, 2, 3}}).build()};
    CHECK(!Disc::open(evil.path, error, cache.path));
    CHECK(error.find("unsafe") != std::string::npos);
    CHECK(!std::filesystem::exists("escaped.bin"));

    // No disc image inside.
    TempFile other{"cd_test_other.zip", test::ZipBuilder{}.add({"readme.txt", {'h', 'i'}}).build()};
    CHECK(!Disc::open(other.path, error, cache.path));
    CHECK(error.find("no .cue") != std::string::npos);

    // Corrupt data: CRC mismatch, partial file removed.
    auto bytes = zipped_disc("Bad.img", 4, false);
    bytes[bytes.size() / 2] ^= 0xFF;                         // inside the stored image
    TempFile corrupt{"cd_test_corrupt.zip", bytes};
    CHECK(!Disc::open(corrupt.path, error, cache.path));
#if MD_HAVE_ZLIB
    CHECK(error.find("CRC") != std::string::npos);
#endif
    CHECK(!std::filesystem::exists(cache.path + "/Bad.img"));

    // A ZIP that isn't a ZIP after all.
    TempFile fake{"cd_test_fake.zip", std::vector<u8>{'P', 'K', 3, 4, 0, 0}};
    CHECK(!Disc::open(fake.path, error, cache.path));
}

// ==============================================================================
// CDD command interface
// ==============================================================================

namespace {

constexpr u32 kSubCddControl = 0xFF8036;
constexpr u32 kSubCddStatus  = 0xFF8038;
constexpr u32 kSubCddCommand = 0xFF8042;
constexpr md::s64 kTick = 12'500'000 / 75 + 1;  // a bit more than 1/75 s of sub-CPU time

// Sends a command as the BIOS does: nibbles 0-8, then the checksum written
// with a word access to 0x4A, which triggers it.
void cdd_send(MegaCd& cd, std::initializer_list<u8> nibbles) {
    std::array<u8, 10> c{};
    std::size_t i = 0;
    for (u8 n : nibbles) c[i++] = n;
    u32 sum = 0;
    for (std::size_t k = 0; k < 9; ++k) sum += c[k];
    c[9] = static_cast<u8>(~sum & 0x0F);
    for (std::size_t k = 0; k < 10; k += 2) {
        cd.sub_bus().write_word(kSubCddCommand + static_cast<u32>(k), static_cast<u16>((c[k] << 8) | c[k + 1]));
    }
}

std::array<u8, 10> cdd_status(MegaCd& cd) {
    std::array<u8, 10> s{};
    for (u32 i = 0; i < 10; ++i) s[i] = cd.sub_bus().read_byte(kSubCddStatus + i);
    return s;
}

bool checksum_ok(const std::array<u8, 10>& s) {
    u32 sum = 0;
    for (std::size_t k = 0; k < 9; ++k) sum += s[k];
    return s[9] == (~sum & 0x0F);
}

// A drive with an ISO of `sectors` sectors; the sub CPU stays in reset (the
// tests talk to the drive registers directly).
struct DriveRig {
    TempFile iso;
    MegaCd cd;
    md::s64 now = 0;
    explicit DriveRig(const char* name, u32 sectors) : iso(name, iso_image(sectors)) {
        std::string error;
        cd.insert_disc(Disc::open(iso.path, error));
    }
    void ticks(int n) { now += n * kTick; cd.run_sub_until(now); }
};

} // namespace

TEST(cdd_reports_status_and_toc) {
    DriveRig rig{"cdd_toc.iso", 300};
    auto& cd = rig.cd;

    cdd_send(cd, {0x0});                                   // get status
    auto s = cdd_status(cd);
    CHECK(s[0] == CdDrive::kToc);                          // disc in, stopped
    CHECK(checksum_ok(s));

    cdd_send(cd, {0x2, 0, 0, 0x3});                        // disc length
    s = cdd_status(cd);
    CHECK(s[1] == 0x3);
    CHECK(s[2] == 0 && s[3] == 0 && s[4] == 0 && s[5] == 6 && s[6] == 0 && s[7] == 0);  // 00:06:00 = 300 + 150
    CHECK(checksum_ok(s));

    cdd_send(cd, {0x2, 0, 0, 0x4});                        // first / last track
    s = cdd_status(cd);
    CHECK(s[1] == 0x4 && s[2] == 0 && s[3] == 1 && s[4] == 0 && s[5] == 1);

    cdd_send(cd, {0x2, 0, 0, 0x5, 0, 1});                  // track 1 start
    s = cdd_status(cd);
    CHECK(s[1] == 0x5);
    CHECK(s[4] == 0 && s[5] == 2);                         // 00:02:00
    CHECK(s[6] & 0x08);                                    // data track
    CHECK(s[8] == 1);
    CHECK(checksum_ok(s));

    cdd_send(cd, {0x2, 0, 0, 0x5, 0, 7});                  // no such track
    CHECK(cdd_status(cd)[1] == 0xF);

    cdd_send(cd, {0x1});                                   // stop
    s = cdd_status(cd);
    CHECK(s[0] == CdDrive::kStop && s[1] == 0xF && checksum_ok(s));
}

TEST(cdd_no_disc) {
    MegaCd cd;
    cdd_send(cd, {0x0});
    CHECK(cdd_status(cd)[0] == CdDrive::kNoDisc);
}

TEST(cdd_int4_every_tick_while_hock_is_set) {
    DriveRig rig{"cdd_int4.iso", 10};
    auto& cd = rig.cd;
    cd.sub_bus().write_byte(kSubIntMask + 1, 0x10);        // enable INT4
    rig.ticks(3);
    CHECK(cd.registers().sub_irq_level() == 0);            // HOCK off: silent

    cd.sub_bus().write_byte(kSubCddControl + 1, 0x04);     // HOCK
    CHECK((cd.sub_bus().read_word(kSubCddControl) & 0x0104) == 0x0104);  // DM (no audio) | HOCK
    rig.ticks(1);
    CHECK(cd.registers().sub_irq_level() == 4);
    cd.registers().acknowledge_sub_interrupt(4);
    CHECK(cd.registers().sub_irq_level() == 0);
    rig.ticks(1);
    CHECK(cd.registers().sub_irq_level() == 4);            // again on the next tick
}

TEST(cdd_play_seeks_then_streams_sectors) {
    DriveRig rig{"cdd_play.iso", 200};
    auto& cd = rig.cd;
    enable_decoder(cd, 0);

    cdd_send(cd, {0x3, 0, 0, 0, 0, 2, 1, 0});              // play from 00:02:10 = LBA 10
    auto s = cdd_status(cd);
    CHECK(s[0] == CdDrive::kSeek && s[1] == 0xF && checksum_ok(s));

    rig.ticks(1);                                          // the head starts moving
    CHECK(cd.drive().status() == CdDrive::kPlay);
    CHECK(cd.drive().latency() > 0);
    CHECK(cd.cdc().sectors_decoded() == 0);

    const auto wait = static_cast<int>(cd.drive().latency());
    rig.ticks(wait);
    CHECK(cd.cdc().sectors_decoded() == 0);                // arrived, nothing read yet
    rig.ticks(5);
    CHECK(cd.cdc().sectors_decoded() == 5);
    CHECK(cd.drive().lba() == 15);
    CHECK(cdc_read(cd, kHead0 + 2) == 0x14);               // last header: 00:02:14

    cdd_send(cd, {0x0});                                   // get status: refreshed position
    s = cdd_status(cd);
    CHECK(s[0] == CdDrive::kPlay && s[1] == 0x0);
    CHECK(s[4] == 0 && s[5] == 2 && s[6] == 1 && s[7] == 5);  // 00:02:15
    CHECK(s[8] == 0x4);                                    // data track
    CHECK(checksum_ok(s));

    cdd_send(cd, {0x6});                                   // pause
    rig.ticks(3);
    CHECK(cd.cdc().sectors_decoded() == 5);
    cdd_send(cd, {0x7});                                   // resume
    rig.ticks(2);
    CHECK(cd.cdc().sectors_decoded() == 7);
}

TEST(cdd_seek_into_the_pregap) {
    // The BIOS seeks to 00:01:70, five sectors before the first data sector,
    // and checks the reported position.
    DriveRig rig{"cdd_pregap.iso", 50};
    auto& cd = rig.cd;
    enable_decoder(cd, 0);

    cdd_send(cd, {0x4, 0, 0, 0, 0, 1, 7, 0});              // seek 00:01:70
    rig.ticks(1);
    CHECK(cd.drive().lba() == -5);
    rig.ticks(static_cast<int>(cd.drive().latency()));
    cdd_send(cd, {0x0});
    auto s = cdd_status(cd);
    CHECK(s[0] == CdDrive::kPause);
    CHECK(s[4] == 0 && s[5] == 1 && s[6] == 7 && s[7] == 0);

    cdd_send(cd, {0x3, 0, 0, 0, 0, 1, 7, 3});              // play from 00:01:73
    rig.ticks(1);
    rig.ticks(static_cast<int>(cd.drive().latency()) + 3);
    CHECK(cd.cdc().sectors_decoded() == 3);                // 00:01:73, :74, 00:02:00
    CHECK(cdc_read(cd, kHead0 + 1) == 0x02 && cdc_read(cd, kHead0 + 2) == 0x00);
}

TEST(cdd_plays_to_the_end_of_the_disc) {
    DriveRig rig{"cdd_end.iso", 20};
    auto& cd = rig.cd;
    cd.drive().read(15);
    rig.ticks(10);
    CHECK(cd.drive().status() == CdDrive::kEnd);
    CHECK(!cd.drive().reading());
}

// ==============================================================================
// Timer and stopwatch
// ==============================================================================

TEST(megacd_timer_raises_int3) {
    MegaCd cd;
    cd.sub_bus().write_byte(kSubIntMask + 1, 0x08);        // enable INT3
    cd.sub_bus().write_byte(0xFF8031, 9);                  // every 10 x 384 cycles
    CHECK(cd.sub_bus().read_byte(0xFF8031) == 9);
    cd.run_sub_until(3839);
    CHECK(cd.registers().sub_irq_level() == 0);
    cd.run_sub_until(3840);
    CHECK(cd.registers().sub_irq_level() == 3);
    cd.registers().acknowledge_sub_interrupt(3);
    cd.run_sub_until(3840 * 2);
    CHECK(cd.registers().sub_irq_level() == 3);            // periodic

    cd.registers().acknowledge_sub_interrupt(3);
    cd.sub_bus().write_byte(0xFF8031, 0);                  // stop
    cd.run_sub_until(3840 * 5);
    CHECK(cd.registers().sub_irq_level() == 0);
}

TEST(megacd_stopwatch_counts_and_clears) {
    MegaCd cd;
    cd.run_sub_until(384 * 100);
    CHECK(cd.sub_bus().read_word(0xFF800C) == 100);
    CHECK(cd.read16(0xA1200C) == 100);                     // main CPU sees it too
    cd.sub_bus().write_word(0xFF800C, 0);                  // any write clears
    cd.run_sub_until(384 * 100 + 384 * 7);
    CHECK(cd.sub_bus().read_word(0xFF800C) == 7);
    cd.run_sub_until(384 * 100 + 384 * (7 + 0x1000));      // 12-bit counter wraps
    CHECK(cd.sub_bus().read_word(0xFF800C) == 7);
}

// ==============================================================================
// Tray and disc changes
// ==============================================================================

TEST(cdd_disc_swap_opens_then_closes_the_tray) {
    DriveRig rig{"cdd_swap_a.iso", 300};
    auto& cd = rig.cd;
    TempFile second{"cdd_swap_b.iso", iso_image(100)};
    std::string error;
    auto disc_b = Disc::open(second.path, error);

    cd.drive().read(10);
    rig.ticks(2);
    cd.swap_disc(disc_b);                                  // the player changes the disc
    CHECK(cd.drive().tray_open());
    CHECK(!cd.drive().reading());
    cdd_send(cd, {0x0});
    CHECK(cdd_status(cd)[0] == CdDrive::kOpen);

    cdd_send(cd, {0x3, 0, 0, 0, 0, 2, 0, 0});              // play: nothing to play while open
    rig.ticks(2);
    CHECK(cd.drive().status() == CdDrive::kOpen);

    rig.ticks(static_cast<int>(CdDrive::kSwapTicks));      // tray closes with the new disc
    CHECK(!cd.drive().tray_open());
    cdd_send(cd, {0x0});
    CHECK(cdd_status(cd)[0] == CdDrive::kToc);
    cdd_send(cd, {0x2, 0, 0, 0x3});                        // its length: 100 + 150 = 00:03:25
    const auto s = cdd_status(cd);
    CHECK(s[4] == 0 && s[5] == 3 && s[6] == 2 && s[7] == 5);
}

TEST(cdd_tray_commands) {
    DriveRig rig{"cdd_tray.iso", 50};
    auto& cd = rig.cd;
    cdd_send(cd, {0xD});                                   // open tray
    CHECK(cd.drive().tray_open() && !cd.drive().has_disc());
    CHECK(cdd_status(cd)[0] == CdDrive::kOpen);
    cdd_send(cd, {0x0});
    CHECK(cdd_status(cd)[0] == CdDrive::kOpen);
    cdd_send(cd, {0xC});                                   // close: the same disc is back
    CHECK(!cd.drive().tray_open() && cd.drive().has_disc());
    cdd_send(cd, {0x0});
    CHECK(cdd_status(cd)[0] == CdDrive::kToc);
}
