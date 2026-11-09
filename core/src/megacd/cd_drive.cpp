#include "md/megacd/cd_drive.hpp"

#include <algorithm>

namespace md::megacd {

namespace {

constexpr int kScanSpeed = 30;  // sectors per tick in fast forward / rewind

// Seek latency, in ticks: a fixed spin-up (the BIOS hangs without at least
// two "seeking" interrupts) plus up to ~1.5 s across the whole disc.
constexpr u32 kSeekBaseLatency = 12;
constexpr u32 kSeekFullLatency = 120;
constexpr u32 kSeekFullDistance = 270'000;

} // namespace

void CdDrive::reset() {
    status_regs_.fill(0);
    status_regs_[9] = 0x0F;          // checksum of an all-zero status
    command_regs_.fill(0x0F);
    hock_ = false;
    audio_playing_ = false;
    status_code_ = tray_open_ ? kOpen : (disc_ ? kToc : kNoDisc);
    pending_ = 0;
    latency_ = 0;
    lba_ = 0;
    index_ = 0;
    scan_offset_ = 0;
    clock_ = ClockConverter(kTicksPerSecond, kClockHz);
    fifo_read_ = 0;
    fifo_count_ = 0;
    priming_ = true;
    cdda_clock_ = ClockConverter(kCddaRate, kClockHz);
    render_time_ = 0;
    cdda_out_ = {};
    fader_reg_ = 0;
    volume_ = volume_target_ = kFullVolume;
}

void CdDrive::insert(std::shared_ptr<const Disc> disc) {
    tray_open_ = false;
    tray_disc_.reset();
    swap_ticks_ = 0;
    disc_ = std::move(disc);
    stop();
}

void CdDrive::eject() {
    disc_.reset();
    stop();
}

void CdDrive::open_tray() {
    if (tray_open_) return;
    tray_disc_ = std::move(disc_);
    disc_.reset();
    tray_open_ = true;
    stop();
}

void CdDrive::close_tray() {
    if (!tray_open_) return;
    disc_ = std::move(tray_disc_);
    tray_disc_.reset();
    tray_open_ = false;
    swap_ticks_ = 0;
    stop();
}

void CdDrive::swap_disc(std::shared_ptr<const Disc> disc) {
    open_tray();
    tray_disc_ = std::move(disc);
    swap_ticks_ = kSwapTicks;
}

void CdDrive::read(u32 lba) {
    if (!disc_) return;
    lba_ = static_cast<s32>(std::min(lba, lead_out()));
    index_ = track_index_of(lba_);
    status_code_ = kPlay;
    pending_ = 0;
    latency_ = 0;
}

void CdDrive::stop() noexcept {
    status_code_ = tray_open_ ? kOpen : (disc_ ? kToc : kNoDisc);
    pending_ = 0;
    latency_ = 0;
    lba_ = 0;
    index_ = 0;
    audio_playing_ = false;
}

// ---------------------------------------------------------------------------
// Disc geometry
// ---------------------------------------------------------------------------

u32 CdDrive::track_count() const noexcept {
    return disc_ ? static_cast<u32>(disc_->tracks().size()) : 0;
}

u32 CdDrive::lead_out() const noexcept { return disc_ ? disc_->sector_count() : 0; }

u32 CdDrive::track_index_of(s32 lba) const noexcept {
    const u32 count = track_count();
    for (u32 i = 0; i < count; ++i) {
        if (lba < static_cast<s32>(disc_->tracks()[i].end_lba)) return i;
    }
    return count;
}

bool CdDrive::data_track(u32 index) const noexcept {
    return index < track_count() && is_data(disc_->tracks()[index].type);
}

// Positions before 00:02:00 are valid (the pregap before sector 0): the BIOS
// seeks a few sectors early and checks where the head landed.
s32 CdDrive::command_msf_lba() const noexcept {
    const int minutes = command(2) * 10 + command(3);
    const int seconds = command(4) * 10 + command(5);
    const int frames  = command(6) * 10 + command(7);
    const int lba = (minutes * 60 + seconds) * 75 + frames - static_cast<int>(kLeadInSectors);
    return std::min(lba, static_cast<s32>(lead_out()));
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

u16 CdDrive::read_register(u32 offset) const noexcept {
    offset &= ~1u;
    if (offset == kRegFader) {
        return static_cast<u16>((fader_reg_ & 0x7FFE) | (volume_ != volume_target_ ? 0x8000 : 0));
    }
    if (offset == kRegControl) return static_cast<u16>((audio_playing_ ? 0 : 0x0100) | (hock_ ? 0x0004 : 0));
    const auto pair = [](const std::array<u8, 10>& r, u32 i) {
        return static_cast<u16>((r[i] << 8) | r[i + 1]);
    };
    if (offset >= kRegStatus && offset < kRegCommand) return pair(status_regs_, offset - kRegStatus);
    if (offset >= kRegCommand && offset < kRegEnd) return pair(command_regs_, offset - kRegCommand);
    return 0;
}

void CdDrive::write_register(u32 offset, u16 value, u16 lanes) {
    offset &= ~1u;
    if (offset == kRegFader) {
        fader_reg_ = static_cast<u16>((fader_reg_ & ~lanes) | (value & lanes));
        volume_target_ = (fader_reg_ >> 4) & 0x7FF;
        return;
    }
    if (offset == kRegControl) {
        if (lanes & 0x00FF) hock_ = (value & 0x0004) != 0;
        return;
    }
    if (offset < kRegCommand || offset >= kRegEnd) return;  // status is read-only
    const u32 i = offset - kRegCommand;
    if (lanes & 0xFF00) command_regs_[i] = static_cast<u8>((value >> 8) & 0x0F);
    if (lanes & 0x00FF) command_regs_[i + 1] = static_cast<u8>(value & 0x0F);
    // Writing the last nibble (the checksum) sends the command.
    if (offset == kRegEnd - 2 && (lanes & 0x00FF)) process_command();
}

// ---------------------------------------------------------------------------
// Status reports
// ---------------------------------------------------------------------------

void CdDrive::set_bcd(std::size_t at, u32 value) {
    status_regs_[at] = static_cast<u8>((value / 10) % 10);
    status_regs_[at + 1] = static_cast<u8>(value % 10);
}

void CdDrive::set_time(s32 msf_frames) {
    const auto t = static_cast<u32>(std::max(msf_frames, 0));
    set_bcd(2, (t / 75) / 60);
    set_bcd(4, (t / 75) % 60);
    set_bcd(6, t % 75);
}

void CdDrive::finish_status() {
    u32 sum = 0;
    for (std::size_t i = 0; i < 9; ++i) sum += status_regs_[i];
    status_regs_[9] = static_cast<u8>(~sum & 0x0F);
}

void CdDrive::report(u8 type) {
    status_regs_[1] = type;
    std::fill(status_regs_.begin() + 2, status_regs_.begin() + 9, u8{0});
    const u32 count = track_count();
    switch (type) {
    case 0x0:  // absolute time
        set_time(lba_ + static_cast<s32>(kLeadInSectors));
        status_regs_[8] = data_track(index_) ? 0x04 : 0x00;
        break;
    case 0x1: {  // time within the track
        const auto start = static_cast<s32>(index_ < count ? disc_->tracks()[index_].start_lba : lead_out());
        set_time(lba_ >= start ? lba_ - start : start - lba_);
        status_regs_[8] = data_track(index_) ? 0x04 : 0x00;
        break;
    }
    case 0x2:  // track number
        if (index_ < count) {
            set_bcd(2, index_ + 1);
        } else {
            status_regs_[2] = status_regs_[3] = 0x0A;
        }
        break;
    case 0x3:  // disc length (lead-out position)
        set_time(static_cast<s32>(lead_out() + kLeadInSectors));
        break;
    case 0x4:  // first and last track numbers
        set_bcd(2, 1);
        set_bcd(4, count);
        break;
    case 0x5: {  // start of a track
        const u32 track = command(4) * 10u + command(5);
        if (track >= 1 && track <= count) {
            set_time(static_cast<s32>(disc_->tracks()[track - 1].start_lba + kLeadInSectors));
            if (data_track(track - 1)) status_regs_[6] |= 0x08;
            status_regs_[8] = static_cast<u8>(track % 10);
        } else {
            status_regs_[1] = 0x0F;
        }
        break;
    }
    case 0x6:  // latest error: none
        break;
    default:
        break;
    }
}

void CdDrive::process_command() {
    const auto head = [this](u8 status) {
        status_regs_.fill(0);
        status_regs_[0] = status;
        status_regs_[1] = 0x0F;  // no valid position data yet
    };

    switch (command(0)) {
    case 0x0:  // get status: refresh the last report with the current position
        // A seek keeps reporting "seeking" until the head is almost there
        // (some games need a few "playing" reports before data arrives).
        if (latency_ <= 3) {
            status_regs_[0] = status_code_;
            if (status_code_ != kStop && status_code_ <= kPause) {
                const u8 type = status_regs_[1] == 0x0F ? u8{0} : status_regs_[1];
                if (type <= 0x2) report(type);
            }
        }
        break;

    case 0x1:  // stop
        stop();
        head(kStop);
        break;

    case 0xC:  // close tray
        close_tray();
        stop();
        head(kStop);
        break;

    case 0x2: {  // report
        const u8 type = command(3);
        if (type > 0x6) return;  // unknown: no answer
        status_regs_[0] = status_code_;
        report(type);
        break;
    }

    case 0x3:  // play
    case 0x4:  // seek (then pause)
        head(kSeek);
        pending_ = command(0) == 0x3 ? kPlay : kPause;
        pending_lba_ = command_msf_lba();
        break;

    case 0x6:  // pause
    case 0xA:  // track jump (not decoded: pauses where it is)
        audio_playing_ = false;
        status_code_ = status_regs_[0] = kPause;
        break;

    case 0x7:  // resume
        status_code_ = status_regs_[0] = kPlay;
        break;

    case 0x8:  // fast forward
    case 0x9:  // rewind
        scan_offset_ = command(0) == 0x8 ? kScanSpeed : -kScanSpeed;
        status_code_ = status_regs_[0] = kScan;
        break;

    case 0xD:  // open tray
        open_tray();
        head(kOpen);
        break;

    default:
        status_regs_[0] = status_code_;
        break;
    }
    finish_status();
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

void CdDrive::advance(s64 sub_cycles) {
    const s64 due = clock_.advance(sub_cycles);
    for (s64 i = 0; i < due; ++i) tick();
}

void CdDrive::tick() {
    if (swap_ticks_ > 0 && --swap_ticks_ == 0) close_tray();  // disc change done
    const u32 count = track_count();

    if (latency_ > 0) {
        --latency_;
    } else if (status_code_ == kPlay) {
        for (int n = 0; n < speed_; ++n) {
            if (index_ >= count) {
                status_code_ = kEnd;
                audio_playing_ = false;
                break;
            }
            const Track& track = disc_->tracks()[index_];
            if (is_data(track.type)) {
                if (lba_ < 0) {
                    // Pregap before sector 0: empty Mode 1 sectors.
                    sector_.fill(0);
                    std::fill(sector_.begin() + 1, sector_.begin() + 11, u8{0xFF});
                    write_data_header(sector_.data() + 12, static_cast<u32>(lba_), 1);  // wraps to lba + 150
                } else if (!disc_->read_sector(static_cast<u32>(lba_), sector_)) {
                    status_code_ = kEnd;  // I/O error: stop as at the end of the disc
                    break;
                }
                cdc_.decode_sector(sector_);
            } else {
                audio_playing_ = lba_ >= static_cast<s32>(track.start_lba);
                if (audio_playing_ && disc_->read_sector(static_cast<u32>(lba_), sector_)) push_audio(sector_);
            }
            if (++lba_ >= static_cast<s32>(track.end_lba)) {
                ++index_;
                audio_playing_ = false;
            }
        }
    } else if (status_code_ == kScan && count > 0) {
        const s64 target = static_cast<s64>(lba_) + scan_offset_;
        if (target >= static_cast<s64>(lead_out())) {
            lba_ = static_cast<s32>(lead_out());
            index_ = count;
            status_code_ = kEnd;
            audio_playing_ = false;
        } else {
            lba_ = static_cast<s32>(std::max<s64>(target, 0));
            index_ = track_index_of(lba_);
            audio_playing_ = !data_track(index_);
        }
    }

    // A play/seek command starts moving the head on the tick after it.
    if (pending_ && !disc_) pending_ = 0;  // no disc (or tray open): nothing to seek to
    if (pending_) {
        const s32 target = pending_lba_;
        if (latency_ == 0) latency_ = kSeekBaseLatency;
        const auto distance = static_cast<u32>(target > lba_ ? target - lba_ : lba_ - target);
        latency_ += static_cast<u32>(static_cast<u64>(distance) * kSeekFullLatency / kSeekFullDistance);
        lba_ = target;
        index_ = track_index_of(lba_);
        if (index_ < count && !data_track(index_)) {
            lba_ = std::max(lba_, static_cast<s32>(disc_->tracks()[index_].start_lba));  // audio starts at INDEX 01
        }
        audio_playing_ = false;
        status_code_ = pending_;
        pending_ = 0;
    }

    if (hock_ && irq_) irq_();
}

// ---------------------------------------------------------------------------
// CD-DA
// ---------------------------------------------------------------------------

void CdDrive::push_audio(const RawSector& sector) {
    for (u32 i = 0; i < kFramesPerSector; ++i) {
        if (fifo_count_ == kFifoFrames) break;  // nobody is listening: drop
        const u8* p = sector.data() + i * 4;    // little-endian 16-bit, left then right
        auto& frame = fifo_[(fifo_read_ + fifo_count_) % kFifoFrames];
        frame[0] = static_cast<s16>(p[0] | (p[1] << 8));
        frame[1] = static_cast<s16>(p[2] | (p[3] << 8));
        ++fifo_count_;
    }
}

void CdDrive::render_cdda(s64 sub_time, s32& left, s32& right) {
    if (sub_time > render_time_) {
        s64 due = cdda_clock_.advance(sub_time - render_time_);
        render_time_ = sub_time;
        if (priming_ && fifo_count_ >= kFramesPerSector * 3 / 2) priming_ = false;
        while (due-- > 0 && !priming_) {
            if (fifo_count_ == 0) {
                priming_ = true;  // ran dry (playback stopped or paused)
                cdda_out_ = {};
                break;
            }
            // The fader ramps one step per sample (as in Genesis Plus GX).
            if (volume_ < volume_target_) ++volume_;
            else if (volume_ > volume_target_) --volume_;
            const s32 mul = static_cast<s32>((volume_ & 0x7FC) ? (volume_ & 0x7FC) : (volume_ & 0x3));
            const auto& frame = fifo_[fifo_read_];
            cdda_out_ = {frame[0] * mul / 1024, frame[1] * mul / 1024};
            fifo_read_ = (fifo_read_ + 1) % kFifoFrames;
            --fifo_count_;
        }
    }
    left = cdda_out_[0];
    right = cdda_out_[1];
}

} // namespace md::megacd
