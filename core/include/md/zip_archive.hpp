#pragma once

#include <memory>
#include <string>
#include <vector>

#include "md/types.hpp"

namespace md {

// Read-only ZIP archive on disk, for archives too large to load in memory
// (CD images: hundreds of MB). Entries are decompressed by streaming to a
// file, with their CRC-32 checked. Supports "stored" and "deflate" entries
// (deflate needs zlib); not ZIP64 or encrypted archives.
class ZipArchive {
public:
    struct Entry {
        std::string name;
        u64 size = 0;             // uncompressed
        u64 compressed_size = 0;
        u32 crc = 0;
        u16 method = 0;           // 0 = stored, 8 = deflate
        u16 flags = 0;
        u64 local_header_offset = 0;
    };

    // Returns nullptr (and sets `error`) if the file isn't a readable ZIP.
    [[nodiscard]] static std::unique_ptr<ZipArchive> open(const std::string& path, std::string& error);

    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }

    // Decompresses one entry into `out_path` (overwritten). On failure the
    // partial output is removed.
    bool extract(const Entry& entry, const std::string& out_path, std::string& error) const;

private:
    std::string path_;
    std::vector<Entry> entries_;
};

// True if the file starts with a ZIP local header.
[[nodiscard]] bool file_is_zip(const std::string& path);

} // namespace md
