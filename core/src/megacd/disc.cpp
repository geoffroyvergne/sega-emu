#include "md/megacd/disc.hpp"

#include "md/zip_archive.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

namespace md::megacd {

namespace {

constexpr std::array<u8, 12> kSync = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

// "mm:ss:ff" -> frames (75 per second).
std::optional<u32> parse_msf(const std::string& text) {
    unsigned m = 0, s = 0, f = 0;
    char c1 = 0, c2 = 0;
    std::istringstream in(text);
    if (!(in >> m >> c1 >> s >> c2 >> f) || c1 != ':' || c2 != ':' || s >= 60 || f >= 75) return std::nullopt;
    return (m * 60 + s) * 75 + f;
}

// Reads the next token; a double-quoted string may contain spaces.
std::string next_token(std::istringstream& in) {
    in >> std::ws;
    if (in.peek() == '"') {
        in.get();
        std::string s;
        std::getline(in, s, '"');
        return s;
    }
    std::string s;
    in >> s;
    return s;
}

std::optional<std::pair<TrackType, u32>> parse_track_mode(const std::string& mode) {
    const std::string m = upper(mode);
    if (m == "MODE1/2352") return std::pair{TrackType::Mode1_2352, 2352u};
    if (m == "MODE1/2048") return std::pair{TrackType::Mode1_2048, 2048u};
    if (m == "MODE2/2352") return std::pair{TrackType::Mode2_2352, 2352u};
    if (m == "AUDIO")      return std::pair{TrackType::Audio, 2352u};
    return std::nullopt;
}

// Cue sheet as written, before absolute addresses are resolved.
struct CueTrack {
    int number = 0;
    TrackType type{};
    u32 sector_size = 2352;
    std::optional<u32> index0;
    std::optional<u32> index1;
    u32 pregap = 0;
};
struct CueFile {
    std::string path;
    std::vector<CueTrack> tracks;
};

} // namespace

void write_data_header(u8* header, u32 lba, u8 mode) noexcept {
    const u32 abs = lba + kLeadInSectors;
    header[0] = to_bcd(abs / (60 * 75));
    header[1] = to_bcd((abs / 75) % 60);
    header[2] = to_bcd(abs % 75);
    header[3] = mode;
}

std::shared_ptr<Disc> Disc::parse_cue(std::string_view cue, const std::string& base_dir,
                                      const FileSizeFn& file_size, std::string& error) {
    std::vector<CueFile> files;
    std::istringstream lines{std::string(cue)};
    std::string line;
    int line_no = 0;
    const auto fail = [&](const std::string& msg) {
        error = "cue line " + std::to_string(line_no) + ": " + msg;
        return nullptr;
    };

    while (std::getline(lines, line)) {
        ++line_no;
        std::istringstream in(line);
        const std::string keyword = upper(next_token(in));
        if (keyword.empty() || keyword == "REM" || keyword == "CATALOG" || keyword == "PERFORMER" ||
            keyword == "TITLE" || keyword == "SONGWRITER" || keyword == "FLAGS" || keyword == "ISRC" ||
            keyword == "CDTEXTFILE" || keyword == "POSTGAP") {
            continue;  // metadata; POSTGAP is rare and ignored
        }
        if (keyword == "FILE") {
            const std::string name = next_token(in);
            const std::string type = upper(next_token(in));
            if (name.empty()) return fail("FILE without a name");
            if (type != "BINARY" && type != "MOTOROLA") {
                return fail("unsupported FILE type '" + type + "' (only BINARY images are supported)");
            }
            files.push_back({(std::filesystem::path(base_dir) / name).string(), {}});
        } else if (keyword == "TRACK") {
            if (files.empty()) return fail("TRACK before FILE");
            CueTrack t;
            std::string mode;
            in >> t.number >> mode;
            const auto parsed = parse_track_mode(mode);
            if (!parsed) return fail("unsupported track mode '" + mode + "'");
            t.type = parsed->first;
            t.sector_size = parsed->second;
            files.back().tracks.push_back(t);
        } else if (keyword == "INDEX") {
            if (files.empty() || files.back().tracks.empty()) return fail("INDEX before TRACK");
            int number = -1;
            std::string msf;
            in >> number >> msf;
            const auto frames = parse_msf(msf);
            if (!frames) return fail("bad time '" + msf + "'");
            auto& t = files.back().tracks.back();
            if (number == 0) t.index0 = frames;
            else if (number == 1) t.index1 = frames;
            // INDEX 02+ (sub-indexes) don't affect layout.
        } else if (keyword == "PREGAP") {
            if (files.empty() || files.back().tracks.empty()) return fail("PREGAP before TRACK");
            std::string msf;
            in >> msf;
            const auto frames = parse_msf(msf);
            if (!frames) return fail("bad time '" + msf + "'");
            files.back().tracks.back().pregap = *frames;
        } else {
            return fail("unknown command '" + keyword + "'");
        }
    }
    if (files.empty()) { error = "cue sheet has no FILE"; return nullptr; }

    // Resolve absolute LBAs. Within a FILE, INDEX times are offsets from the
    // file's start; files follow each other; PREGAP sectors (not in any file)
    // shift everything after them.
    auto disc = std::make_shared<Disc>();
    u32 file_base = 0;      // LBA where the current file's first sector lands (before pregaps)
    u32 pregap_total = 0;
    for (std::size_t fi = 0; fi < files.size(); ++fi) {
        const auto& f = files[fi];
        if (f.tracks.empty()) { error = "FILE '" + f.path + "' has no TRACK"; return nullptr; }
        const auto size = file_size(f.path);
        if (!size) { error = "cannot find '" + f.path + "'"; return nullptr; }
        const u32 sector_size = f.tracks.front().sector_size;
        const auto file_sectors = static_cast<u32>(*size / sector_size);

        const std::size_t first_track = disc->tracks_.size();
        for (const auto& ct : f.tracks) {
            if (!ct.index1) { error = "track " + std::to_string(ct.number) + " has no INDEX 01"; return nullptr; }
            pregap_total += ct.pregap;
            Track t;
            t.number = ct.number;
            t.type = ct.type;
            t.sector_size = ct.sector_size;
            t.file = fi;
            t.silent_pregap = ct.pregap;
            t.start_lba = file_base + *ct.index1 + pregap_total;
            t.first_lba = ct.index0 ? file_base + *ct.index0 + pregap_total - ct.pregap
                                    : t.start_lba - ct.pregap;
            t.file_offset = u64{*ct.index1} * ct.sector_size;
            disc->tracks_.push_back(t);
        }
        // Track ends: the next track's first sector, or the end of the file.
        const u32 file_end = file_base + file_sectors + pregap_total;
        for (std::size_t i = first_track; i < disc->tracks_.size(); ++i) {
            disc->tracks_[i].end_lba = (i + 1 < disc->tracks_.size()) ? disc->tracks_[i + 1].first_lba : file_end;
            if (disc->tracks_[i].end_lba < disc->tracks_[i].start_lba) {
                error = "track " + std::to_string(disc->tracks_[i].number) + " extends past its file";
                return nullptr;
            }
        }
        file_base += file_sectors;
        disc->paths_.push_back(f.path);
    }
    disc->streams_.resize(disc->paths_.size());
    return disc;
}

std::string Disc::default_cache_dir() {
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec);
    if (ec) dir = ".";
    return (dir / "sega-emu-discs").string();
}

namespace {

// Rejects entry names that would escape the extraction folder ("zip slip").
bool safe_entry_name(const std::string& name) {
    const std::filesystem::path p(name);
    if (name.empty() || p.is_absolute() || name.front() == '/' || name.front() == '\\') return false;
    for (const auto& part : p) {
        if (part == "..") return false;
    }
    return name.find(':') == std::string::npos;
}

int disc_file_rank(const std::string& name) {
    const std::string ext = lower(std::filesystem::path(name).extension().string());
    if (ext == ".cue") return 3;
    if (ext == ".iso") return 2;
    if (ext == ".bin" || ext == ".img") return 1;
    return 0;
}

} // namespace

std::shared_ptr<Disc> Disc::open(const std::string& path, std::string& error, const std::string& cache_dir) {
    if (file_is_zip(path)) {
        // Unpack the archive (once) and open the image inside it.
        const auto archive = ZipArchive::open(path, error);
        if (!archive) return nullptr;

        const ZipArchive::Entry* image = nullptr;
        for (const auto& e : archive->entries()) {
            const int rank = disc_file_rank(e.name);
            if (rank == 0) continue;
            if (!image || rank > disc_file_rank(image->name) ||
                (rank == disc_file_rank(image->name) && e.size > image->size)) {
                image = &e;
            }
        }
        if (!image) { error = path + ": no .cue, .iso, .bin or .img in this archive"; return nullptr; }

        const std::filesystem::path dir = cache_dir.empty()
            ? std::filesystem::path(default_cache_dir()) / std::filesystem::path(path).stem()
            : std::filesystem::path(cache_dir);
        std::error_code e;
        std::filesystem::create_directories(dir, e);
        if (e) { error = "cannot create " + dir.string(); return nullptr; }

        for (const auto& entry : archive->entries()) {
            if (entry.name.empty() || entry.name.back() == '/') continue;  // directory
            if (!safe_entry_name(entry.name)) { error = path + ": unsafe file name in archive: " + entry.name; return nullptr; }
            const auto out = dir / entry.name;
            std::error_code se;
            if (std::filesystem::file_size(out, se) == entry.size && !se) continue;  // already unpacked
            std::filesystem::create_directories(out.parent_path(), se);
            if (!archive->extract(entry, out.string(), error)) { error = path + ": " + error; return nullptr; }
        }
        return open((dir / image->name).string(), error);
    }

    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) { error = "cannot open " + path; return nullptr; }

    const std::string ext = lower(std::filesystem::path(path).extension().string());
    if (ext == ".cue") {
        std::ifstream in(path);
        std::stringstream text;
        text << in.rdbuf();
        const auto base = std::filesystem::path(path).parent_path().string();
        auto disc = parse_cue(text.str(), base, [](const std::string& p) -> std::optional<u64> {
            std::error_code e;
            const auto s = std::filesystem::file_size(p, e);
            return e ? std::nullopt : std::optional<u64>(s);
        }, error);
        if (!disc) error = path + ": " + error;
        return disc;
    }

    // Single-track image: .iso = 2048-byte sectors; otherwise detect raw
    // 2352-byte sectors by the sync pattern.
    const std::string name = std::filesystem::path(path).filename().string();
    std::string cue;
    if (ext == ".iso") {
        cue = "FILE \"" + name + "\" BINARY\nTRACK 01 MODE1/2048\nINDEX 01 00:00:00\n";
    } else {
        std::ifstream in(path, std::ios::binary);
        std::array<u8, 12> head{};
        in.read(reinterpret_cast<char*>(head.data()), 12);
        const bool raw = in.gcount() == 12 && head == kSync;
        cue = std::string("FILE \"") + name + "\" BINARY\nTRACK 01 " + (raw ? "MODE1/2352" : "MODE1/2048") +
              "\nINDEX 01 00:00:00\n";
    }
    (void)size;
    const auto base = std::filesystem::path(path).parent_path().string();
    return parse_cue(cue, base, [](const std::string& p) -> std::optional<u64> {
        std::error_code e;
        const auto s = std::filesystem::file_size(p, e);
        return e ? std::nullopt : std::optional<u64>(s);
    }, error);
}

const Track* Disc::track_at(u32 lba) const noexcept {
    for (const auto& t : tracks_) {
        if (lba >= t.first_lba && lba < t.end_lba) return &t;
    }
    return nullptr;
}

bool Disc::read_sector(u32 lba, RawSector& out) const {
    const Track* t = track_at(lba);
    if (!t) return false;
    out.fill(0);

    const bool silent = lba < t->start_lba && lba >= t->start_lba - t->silent_pregap;
    if (!silent) {
        auto& stream = streams_[t->file];
        if (!stream) {
            stream = std::make_unique<std::ifstream>(paths_[t->file], std::ios::binary);
            if (!*stream) { stream.reset(); return false; }
        }
        const auto offset = static_cast<s64>(t->file_offset) +
                            (static_cast<s64>(lba) - static_cast<s64>(t->start_lba)) * t->sector_size;
        if (offset < 0) return false;
        stream->clear();
        stream->seekg(offset);
        u8* dst = t->type == TrackType::Mode1_2048 ? out.data() + 16 : out.data();
        stream->read(reinterpret_cast<char*>(dst), t->sector_size);
        if (stream->gcount() != static_cast<std::streamsize>(t->sector_size)) return false;
    }

    // Rebuild sync and header where the image doesn't store them.
    if (is_data(t->type) && (t->type == TrackType::Mode1_2048 || silent)) {
        std::copy(kSync.begin(), kSync.end(), out.begin());
        write_data_header(out.data() + 12, lba, t->type == TrackType::Mode2_2352 ? 2 : 1);
        // TODO: EDC/ECC are left zero; the emulated CDC doesn't verify them.
    }
    return true;
}

} // namespace md::megacd
