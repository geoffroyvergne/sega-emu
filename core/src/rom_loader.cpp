#include "md/rom_loader.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>

#if MD_HAVE_ZLIB
#include <zlib.h>
#endif

namespace md {

namespace {

// ZIP structures are little-endian.
u16 le16(std::span<const u8> b, std::size_t at) {
    return static_cast<u16>(b[at] | (b[at + 1] << 8));
}
u32 le32(std::span<const u8> b, std::size_t at) {
    return static_cast<u32>(b[at]) | (static_cast<u32>(b[at + 1]) << 8) |
           (static_cast<u32>(b[at + 2]) << 16) | (static_cast<u32>(b[at + 3]) << 24);
}

constexpr u32 kLocalHeaderSig = 0x04034B50;
constexpr u32 kCentralDirSig  = 0x02014B50;
constexpr u32 kEndOfDirSig    = 0x06054B50;

struct Entry {
    std::string name;
    u16 flags = 0;
    u16 method = 0;
    u32 crc = 0;
    u32 compressed_size = 0;
    u32 size = 0;
    u32 local_header_offset = 0;
};

RomImage failure(std::string message) {
    RomImage r;
    r.error = std::move(message);
    return r;
}

bool has_rom_extension(const std::string& name) {
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == "bin" || ext == "md" || ext == "gen" || ext == "smd";
}

// Locates the End Of Central Directory record (it sits at the end of the
// file, followed by an optional comment of up to 64 KB).
std::optional<std::size_t> find_end_of_directory(std::span<const u8> zip) {
    if (zip.size() < 22) return std::nullopt;
    const std::size_t lowest = zip.size() > 22 + 0xFFFF ? zip.size() - 22 - 0xFFFF : 0;
    for (std::size_t at = zip.size() - 22 + 1; at-- > lowest;) {
        if (le32(zip, at) == kEndOfDirSig) return at;
    }
    return std::nullopt;
}

std::optional<std::vector<Entry>> read_central_directory(std::span<const u8> zip, std::string& error) {
    const auto eocd = find_end_of_directory(zip);
    if (!eocd) { error = "not a valid ZIP archive (no central directory)"; return std::nullopt; }

    const u16 count = le16(zip, *eocd + 10);
    const u32 dir_offset = le32(zip, *eocd + 16);
    if (dir_offset == 0xFFFFFFFF || count == 0xFFFF) {
        error = "ZIP64 archives are not supported";
        return std::nullopt;
    }

    std::vector<Entry> entries;
    std::size_t at = dir_offset;
    for (u16 i = 0; i < count; ++i) {
        if (at + 46 > zip.size() || le32(zip, at) != kCentralDirSig) {
            error = "corrupt ZIP central directory";
            return std::nullopt;
        }
        Entry e;
        e.flags               = le16(zip, at + 8);
        e.method              = le16(zip, at + 10);
        e.crc                 = le32(zip, at + 16);
        e.compressed_size     = le32(zip, at + 20);
        e.size                = le32(zip, at + 24);
        const u16 name_len    = le16(zip, at + 28);
        const u16 extra_len   = le16(zip, at + 30);
        const u16 comment_len = le16(zip, at + 32);
        e.local_header_offset = le32(zip, at + 42);
        if (at + 46 + name_len > zip.size()) {
            error = "corrupt ZIP central directory";
            return std::nullopt;
        }
        e.name.assign(reinterpret_cast<const char*>(zip.data() + at + 46), name_len);
        entries.push_back(std::move(e));
        at += 46u + name_len + extra_len + comment_len;
    }
    return entries;
}

const Entry* choose_rom_entry(const std::vector<Entry>& entries) {
    const auto is_file = [](const Entry& e) { return !e.name.empty() && e.name.back() != '/'; };
    for (const auto& e : entries) {
        if (is_file(e) && has_rom_extension(e.name)) return &e;
    }
    const Entry* largest = nullptr;
    for (const auto& e : entries) {
        if (is_file(e) && (!largest || e.size > largest->size)) largest = &e;
    }
    return largest;
}

} // namespace

bool is_zip(std::span<const u8> bytes) noexcept {
    return bytes.size() >= 4 && le32(bytes, 0) == kLocalHeaderSig;
}

RomImage extract_rom_from_zip(std::span<const u8> zip) {
    std::string error;
    const auto entries = read_central_directory(zip, error);
    if (!entries) return failure(error);

    // A cue sheet means a CD image (Mega-CD game), not a cartridge ROM.
    for (const auto& e : *entries) {
        const auto dot = e.name.find_last_of('.');
        std::string ext = dot == std::string::npos ? "" : e.name.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == "cue") {
            return failure("this archive is a Mega-CD disc image (" + e.name +
                           "), not a cartridge ROM. Unzip it and use --bios <megacd_bios> --disc <file.cue>");
        }
    }

    const Entry* entry = choose_rom_entry(*entries);
    if (!entry) return failure("ZIP archive contains no files");

    if (entry->flags & 0x0001) return failure("encrypted ZIP entries are not supported: " + entry->name);
    if (entry->size == 0xFFFFFFFF || entry->compressed_size == 0xFFFFFFFF) {
        return failure("ZIP64 archives are not supported");
    }
    if (entry->size > kMaxRomFileSize) return failure("ROM in archive is too large: " + entry->name);

    // The local header repeats name/extra fields, possibly with different
    // extra lengths than the central directory: use its own values.
    const std::size_t lh = entry->local_header_offset;
    if (lh + 30 > zip.size() || le32(zip, lh) != kLocalHeaderSig) {
        return failure("corrupt ZIP local header: " + entry->name);
    }
    const std::size_t data_at = lh + 30 + le16(zip, lh + 26) + le16(zip, lh + 28);
    if (data_at + entry->compressed_size > zip.size()) {
        return failure("truncated ZIP archive: " + entry->name);
    }
    const auto compressed = zip.subspan(data_at, entry->compressed_size);

    RomImage rom;
    rom.name = entry->name;

    switch (entry->method) {
    case 0:  // stored
        if (entry->compressed_size != entry->size) return failure("corrupt ZIP entry: " + entry->name);
        rom.data.assign(compressed.begin(), compressed.end());
        break;

    case 8: {  // deflate
#if MD_HAVE_ZLIB
        rom.data.resize(entry->size);
        z_stream zs{};
        // Negative window bits: raw deflate data, as stored in ZIP files.
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return failure("zlib initialisation failed");
        zs.next_in   = const_cast<Bytef*>(compressed.data());
        zs.avail_in  = static_cast<uInt>(compressed.size());
        zs.next_out  = rom.data.data();
        zs.avail_out = static_cast<uInt>(rom.data.size());
        const int status = inflate(&zs, Z_FINISH);
        const auto produced = zs.total_out;
        inflateEnd(&zs);
        if (status != Z_STREAM_END || produced != entry->size) {
            return failure("could not decompress " + entry->name + " (corrupt archive?)");
        }
        break;
#else
        return failure("this build has no zlib: compressed ZIP entries are not supported");
#endif
    }

    default:
        return failure("unsupported ZIP compression method " + std::to_string(entry->method) +
                       " for " + entry->name + " (only stored and deflate are supported)");
    }

#if MD_HAVE_ZLIB
    const auto crc = crc32(0L, rom.data.data(), static_cast<uInt>(rom.data.size()));
    if (crc != entry->crc) return failure("CRC mismatch in " + entry->name + " (corrupt archive?)");
#endif
    return rom;
}

bool is_32x_rom(std::span<const u8> rom) {
    const auto text_at = [&](std::size_t at, std::string_view text) {
        return rom.size() >= at + text.size() &&
               std::equal(text.begin(), text.end(), rom.begin() + static_cast<std::ptrdiff_t>(at),
                          [](char c, u8 b) { return static_cast<u8>(c) == b; });
    };
    if (text_at(0x3C0, "MARS CHECK MODE")) return true;
    // The standard 32X start-up code at 0x3F0, which every 32X cartridge
    // carries (homebrew may rename the header fields around it): it begins
    // by checking for the adapter, cmpi.l #"MARS",($30EC,A5).
    if (text_at(0x40C, std::string_view("\x0C\xAD" "MARS" "\x30\xEC", 8))) return true;
    if (rom.size() < 0x110) return false;
    const std::string_view console(reinterpret_cast<const char*>(rom.data() + 0x100), 16);
    return console.find("32X") != std::string_view::npos;
}

std::optional<ConsoleRegion> detect_region(std::span<const u8> rom) {
    if (rom.size() < 0x1F3) return std::nullopt;
    bool jp = false, us = false, eu = false;
    for (std::size_t i = 0x1F0; i < 0x1F3; ++i) {
        const char c = static_cast<char>(std::toupper(rom[i]));
        if (c == 'J') jp = true;
        else if (c == 'U') us = true;
        else if (c == 'E') eu = true;  // a lone "E" is read as the letter code (Europe)
    }
    if (!jp && !us && !eu) {
        // New style: one hex digit, bit 0 = Japan, bit 2 = USA, bit 3 = Europe.
        const char c = static_cast<char>(std::toupper(rom[0x1F0]));
        int v = -1;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        if (v < 0) return std::nullopt;
        jp = v & 1;
        us = v & 4;
        eu = v & 8;
    }
    if (us) return ConsoleRegion{Region::NTSC, true};
    if (jp) return ConsoleRegion{Region::NTSC, false};
    if (eu) return ConsoleRegion{Region::PAL, true};
    return std::nullopt;
}

RomImage load_rom_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return failure("cannot open " + path);

    std::vector<u8> bytes(std::istreambuf_iterator<char>(file), {});
    if (bytes.empty()) return failure(path + " is empty");

    if (is_zip(bytes)) {
        RomImage rom = extract_rom_from_zip(bytes);
        if (!rom.ok()) {
            rom.error = path + ": " + rom.error;
        } else {
            rom.name = path + ":" + rom.name;
        }
        return rom;
    }

    // Named .zip but not a ZIP: report it rather than running it as a ROM.
    if (path.size() >= 4) {
        std::string ext = path.substr(path.size() - 4);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".zip") return failure(path + " is not a valid ZIP archive");
    }
    if (bytes.size() < kMinRomFileSize) {
        return failure(path + " is too small to be a Mega Drive ROM (" +
                       std::to_string(bytes.size()) + " bytes)");
    }
    if (bytes.size() > kMaxRomFileSize) return failure(path + " is too large to be a ROM");
    RomImage rom;
    rom.data = std::move(bytes);
    rom.name = path;
    return rom;
}

} // namespace md
