#pragma once

#include <array>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "md/types.hpp"

namespace md::megacd {

// One CD sector as read off the disc: 2352 bytes. For data sectors that is
// 12 sync bytes, a 4-byte header (minute, second, frame in BCD + mode),
// then user data (2048 bytes in Mode 1) and error correction.
inline constexpr std::size_t kRawSectorSize = 2352;
using RawSector = std::array<u8, kRawSectorSize>;

// Absolute disc time: LBA 0 is MSF 00:02:00 (the 2-second lead-in).
inline constexpr u32 kLeadInSectors = 150;

enum class TrackType : u8 {
    Mode1_2352,  // data, raw sectors in the image
    Mode1_2048,  // data, user data only (header/ECC rebuilt on read)
    Mode2_2352,  // data, raw sectors (XA)
    Audio,       // CD-DA, 2352 bytes of 16-bit stereo PCM per sector
};

[[nodiscard]] constexpr bool is_data(TrackType t) noexcept { return t != TrackType::Audio; }

struct Track {
    int number = 0;
    TrackType type = TrackType::Mode1_2352;
    u32 first_lba = 0;     // first sector of the track (start of its pregap)
    u32 start_lba = 0;     // INDEX 01: where the track's content begins
    u32 end_lba = 0;       // one past the last sector
    u32 sector_size = 2352;  // bytes per sector in the image file
    u32 silent_pregap = 0;   // PREGAP sectors not stored in the file (just before start_lba)
    std::size_t file = 0;    // index into the disc's files
    u64 file_offset = 0;     // byte offset of start_lba in that file
};

// A CD image: a .cue sheet with its .bin file(s), a .iso, or a lone raw .bin.
class Disc {
public:
    // Opens an image from disk: .cue sheet, .iso, raw .bin/.img, or a ZIP
    // archive containing one of those. A ZIP is unpacked once into
    // `cache_dir` (default: default_cache_dir()/<archive name>) and reused on
    // later runs. On failure returns nullptr and sets `error`.
    [[nodiscard]] static std::shared_ptr<Disc> open(const std::string& path, std::string& error,
                                                    const std::string& cache_dir = {});

    // Where unpacked ZIP disc images are kept: <system temp>/sega-emu-discs.
    [[nodiscard]] static std::string default_cache_dir();

    // Parses a cue sheet. `file_size` returns the size in bytes of a FILE
    // referenced by the sheet (resolved relative to `base_dir`), or nullopt if
    // it doesn't exist; it is a parameter so tests can avoid real files.
    using FileSizeFn = std::function<std::optional<u64>(const std::string& path)>;
    [[nodiscard]] static std::shared_ptr<Disc> parse_cue(std::string_view cue, const std::string& base_dir,
                                                         const FileSizeFn& file_size, std::string& error);

    [[nodiscard]] const std::vector<Track>& tracks() const noexcept { return tracks_; }
    [[nodiscard]] u32 sector_count() const noexcept { return tracks_.empty() ? 0 : tracks_.back().end_lba; }
    [[nodiscard]] const Track* track_at(u32 lba) const noexcept;

    // Reads one sector as raw 2352 bytes (header and sync rebuilt for 2048-byte
    // images and silent pregaps). Returns false past the end of the disc or on
    // an I/O error.
    bool read_sector(u32 lba, RawSector& out) const;

    // Paths of the image files, in cue order.
    [[nodiscard]] const std::vector<std::string>& files() const noexcept { return paths_; }

private:
    std::vector<Track> tracks_;
    std::vector<std::string> paths_;
    mutable std::vector<std::unique_ptr<std::ifstream>> streams_;  // opened lazily
};

// Helpers shared with the CDC.
[[nodiscard]] constexpr u8 to_bcd(u32 v) noexcept { return static_cast<u8>(((v / 10) << 4) | (v % 10)); }
void write_data_header(u8* header4, u32 lba, u8 mode) noexcept;  // MSF (BCD) of lba + 150, mode

} // namespace md::megacd
