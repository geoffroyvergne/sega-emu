#pragma once

// In-memory ZIP writer for tests: stored (method 0) or deflate (method 8) entries.

#include <string>
#include <vector>

#if MD_HAVE_ZLIB
#include <zlib.h>
#endif

#include "md/types.hpp"

namespace test {

using md::u8;
using md::u16;
using md::u32;

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
#if MD_HAVE_ZLIB
            const bool compress = f.deflate;
#else
            const bool compress = false;  // no zlib: store instead (still a valid archive)
#endif
            const auto compressed = compress ? deflate(f.data) : f.data;
            const u32 crc = crc32_of(f.data);
            const auto offset = static_cast<u32>(out.size());
            const u16 method = compress ? 8 : 0;
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
        ::deflate(&zs, Z_FINISH);  // zlib's, not this class's deflate()
        out.resize(zs.total_out);
        deflateEnd(&zs);
        return out;
#else
        return data;
#endif
    }

    std::vector<File> files_;
};

} // namespace test
