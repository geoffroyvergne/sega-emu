#include "md/zip_archive.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>

#if MD_HAVE_ZLIB
#include <zlib.h>
#endif

namespace md {

namespace {

constexpr u32 kLocalHeaderSig = 0x04034B50;
constexpr u32 kCentralDirSig  = 0x02014B50;
constexpr u32 kEndOfDirSig    = 0x06054B50;

u16 le16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
u32 le32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

bool read_at(std::ifstream& in, u64 offset, u8* dst, std::size_t n) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(n));
    return in.gcount() == static_cast<std::streamsize>(n);
}

} // namespace

bool file_is_zip(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::array<u8, 4> head{};
    in.read(reinterpret_cast<char*>(head.data()), 4);
    return in.gcount() == 4 && le32(head.data()) == kLocalHeaderSig;
}

std::unique_ptr<ZipArchive> ZipArchive::open(const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) { error = "cannot open " + path; return nullptr; }
    const auto file_size = static_cast<u64>(in.tellg());
    if (file_size < 22) { error = path + " is not a ZIP archive"; return nullptr; }

    // The end-of-central-directory record is in the last 22 + 64K bytes.
    const u64 tail_size = std::min<u64>(file_size, 22 + 0xFFFF);
    std::vector<u8> tail(tail_size);
    if (!read_at(in, file_size - tail_size, tail.data(), tail.size())) {
        error = "cannot read " + path;
        return nullptr;
    }
    std::size_t eocd = tail.size();
    for (std::size_t at = tail.size() - 22 + 1; at-- > 0;) {
        if (le32(&tail[at]) == kEndOfDirSig) { eocd = at; break; }
    }
    if (eocd == tail.size()) { error = path + " is not a valid ZIP archive"; return nullptr; }

    const u16 count = le16(&tail[eocd + 10]);
    const u32 dir_size = le32(&tail[eocd + 12]);
    const u32 dir_offset = le32(&tail[eocd + 16]);
    if (dir_offset == 0xFFFFFFFF || count == 0xFFFF) { error = "ZIP64 archives are not supported"; return nullptr; }

    std::vector<u8> dir(dir_size);
    if (!read_at(in, dir_offset, dir.data(), dir.size())) { error = "corrupt ZIP directory in " + path; return nullptr; }

    auto zip = std::make_unique<ZipArchive>();
    zip->path_ = path;
    std::size_t at = 0;
    for (u16 i = 0; i < count; ++i) {
        if (at + 46 > dir.size() || le32(&dir[at]) != kCentralDirSig) {
            error = "corrupt ZIP directory in " + path;
            return nullptr;
        }
        Entry e;
        e.flags = le16(&dir[at + 8]);
        e.method = le16(&dir[at + 10]);
        e.crc = le32(&dir[at + 16]);
        e.compressed_size = le32(&dir[at + 20]);
        e.size = le32(&dir[at + 24]);
        const u16 name_len = le16(&dir[at + 28]);
        const u16 extra_len = le16(&dir[at + 30]);
        const u16 comment_len = le16(&dir[at + 32]);
        e.local_header_offset = le32(&dir[at + 42]);
        if (at + 46 + name_len > dir.size()) { error = "corrupt ZIP directory in " + path; return nullptr; }
        e.name.assign(reinterpret_cast<const char*>(&dir[at + 46]), name_len);
        zip->entries_.push_back(std::move(e));
        at += 46u + name_len + extra_len + comment_len;
    }
    return zip;
}

bool ZipArchive::extract(const Entry& entry, const std::string& out_path, std::string& error) const {
    const auto fail = [&](const std::string& message) {
        std::remove(out_path.c_str());
        error = message;
        return false;
    };
    if (entry.flags & 0x0001) return fail("encrypted ZIP entries are not supported: " + entry.name);
    if (entry.size == 0xFFFFFFFF || entry.compressed_size == 0xFFFFFFFF) return fail("ZIP64 archives are not supported");

    std::ifstream in(path_, std::ios::binary);
    std::array<u8, 30> local{};
    if (!read_at(in, entry.local_header_offset, local.data(), local.size()) || le32(local.data()) != kLocalHeaderSig) {
        return fail("corrupt ZIP entry: " + entry.name);
    }
    const u64 data_offset = entry.local_header_offset + 30 + le16(&local[26]) + le16(&local[28]);
    in.clear();
    in.seekg(static_cast<std::streamoff>(data_offset));

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) return fail("cannot write " + out_path);

    constexpr std::size_t kChunk = 256 * 1024;
    std::vector<u8> input(kChunk);
    u64 remaining = entry.compressed_size;
    u64 written = 0;
#if MD_HAVE_ZLIB
    uLong crc = crc32(0L, Z_NULL, 0);
#endif

    if (entry.method == 0) {  // stored
        while (remaining > 0) {
            const auto n = static_cast<std::size_t>(std::min<u64>(remaining, kChunk));
            in.read(reinterpret_cast<char*>(input.data()), static_cast<std::streamsize>(n));
            if (in.gcount() != static_cast<std::streamsize>(n)) return fail("truncated ZIP entry: " + entry.name);
            out.write(reinterpret_cast<const char*>(input.data()), static_cast<std::streamsize>(n));
#if MD_HAVE_ZLIB
            crc = crc32(crc, input.data(), static_cast<uInt>(n));
#endif
            remaining -= n;
            written += n;
        }
    } else if (entry.method == 8) {  // deflate
#if MD_HAVE_ZLIB
        std::vector<u8> output(kChunk);
        z_stream zs{};
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return fail("zlib initialisation failed");
        int status = Z_OK;
        while (status != Z_STREAM_END) {
            if (zs.avail_in == 0 && remaining > 0) {
                const auto n = static_cast<std::size_t>(std::min<u64>(remaining, kChunk));
                in.read(reinterpret_cast<char*>(input.data()), static_cast<std::streamsize>(n));
                if (in.gcount() != static_cast<std::streamsize>(n)) break;  // truncated file
                zs.next_in = input.data();
                zs.avail_in = static_cast<uInt>(n);
                remaining -= n;
            }
            // Call inflate even with no new input: when the previous call
            // filled the output buffer, zlib may still hold output to deliver.
            zs.next_out = output.data();
            zs.avail_out = static_cast<uInt>(output.size());
            status = inflate(&zs, Z_NO_FLUSH);
            const auto produced = output.size() - zs.avail_out;
            out.write(reinterpret_cast<const char*>(output.data()), static_cast<std::streamsize>(produced));
            crc = crc32(crc, output.data(), static_cast<uInt>(produced));
            written += produced;

            if (status == Z_BUF_ERROR) {
                // No progress possible: fine if more input is coming, an
                // error (truncated stream) if the input is exhausted.
                if (produced == 0 && zs.avail_in == 0 && remaining == 0) break;
                status = Z_OK;
            } else if (status != Z_OK && status != Z_STREAM_END) {
                break;  // corrupt data
            }
        }
        inflateEnd(&zs);
        if (status != Z_STREAM_END) return fail("could not decompress " + entry.name + " (corrupt archive?)");
#else
        return fail("this build has no zlib: compressed ZIP entries are not supported");
#endif
    } else {
        return fail("unsupported ZIP compression method " + std::to_string(entry.method) + " for " + entry.name);
    }

    out.close();
    if (!out) return fail("error writing " + out_path + " (disk full?)");
    if (written != entry.size) return fail("size mismatch in " + entry.name + " (corrupt archive?)");
#if MD_HAVE_ZLIB
    if (static_cast<u32>(crc) != entry.crc) return fail("CRC mismatch in " + entry.name + " (corrupt archive?)");
#endif
    return true;
}

} // namespace md
