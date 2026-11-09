#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "md/types.hpp"

namespace md {

// Result of loading a ROM image from a file or an archive.
struct RomImage {
    std::vector<u8> data;
    std::string name;   // file name, or "archive.zip:entry.bin" for archives
    std::string error;  // empty on success

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Largest ROM we accept (the 68000 address space; real carts are <= 10 MB).
inline constexpr std::size_t kMaxRomFileSize = 16 * 1024 * 1024;

// Smallest file we accept as a raw ROM: the vector table + cartridge header.
inline constexpr std::size_t kMinRomFileSize = 0x200;

// True if the buffer starts with a ZIP local file header ("PK\3\4").
[[nodiscard]] bool is_zip(std::span<const u8> bytes) noexcept;

// Extracts the ROM from a ZIP archive held in memory. Picks the first entry
// with a ROM extension (.bin .md .gen .smd), otherwise the largest file.
// Supports "stored" and "deflate" entries; verifies the CRC-32.
[[nodiscard]] RomImage extract_rom_from_zip(std::span<const u8> zip);

// Console region a cartridge (or Mega-CD BIOS) is made for.
struct ConsoleRegion {
    Region video = Region::NTSC;
    bool overseas = true;  // false = Japan
};

// Reads the region field of a Mega Drive header (0x1F0-0x1F2). Supports the
// old letter codes ("J", "U", "E" in any combination) and the newer single
// hex digit (bit 0 = Japan, bit 2 = USA, bit 3 = Europe). When several regions
// are allowed, prefers USA (60 Hz, overseas), then Japan, then Europe.
// Returns nullopt if the header has no usable code.
[[nodiscard]] std::optional<ConsoleRegion> detect_region(std::span<const u8> rom);

// Loads a ROM from disk: a raw image, or a ZIP archive containing one
// (detected by content, not by extension).
[[nodiscard]] RomImage load_rom_file(const std::string& path);

} // namespace md
