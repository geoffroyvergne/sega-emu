#include "md/megacd/megacd.hpp"

#include <algorithm>

namespace md::megacd {

namespace {

constexpr u32 kAddressMask   = 0xFF'FFFF;
constexpr u16 kOpenBus       = 0xFFFF;

// Main CPU: offsets within the 4 MB expansion window.
constexpr u32 kExpansionMask = 0x3F'FFFF;
constexpr u32 kPrgWindow     = 0x02'0000;  // +0x020000-0x03FFFF
constexpr u32 kWordRamBase   = 0x20'0000;  // +0x200000-0x23FFFF
constexpr u32 kWordRamEnd    = 0x24'0000;

// Sub CPU map.
constexpr u32 kSubPcmBase     = 0xFF'0000;
constexpr u32 kSubPcmEnd      = 0xFF'8000;
constexpr u32 kSubWordRamBase = 0x08'0000;
constexpr u32 kSubWordRamEnd  = 0x0E'0000;
constexpr u32 kSubBackupBase  = 0xFE'0000;
constexpr u32 kSubBackupEnd   = 0xFE'4000;
constexpr u32 kSubRegsBase    = 0xFF'8000;
constexpr u32 kSubRegsEnd     = 0xFF'8200;

// Mixing levels of the Mega-CD's sound against the Mega Drive's (whose FM
// full scale is +/-32767): CD-DA is full-scale 16-bit audio, and the PCM
// chip's eight channels sum to well beyond it, so both are halved.
constexpr s32 kPcmGainDiv = 2;
constexpr s32 kCddaGainDiv = 2;

// Timer and stopwatch: one count every 384 sub-CPU cycles (30.72 us).
constexpr s64 kTimerUnit = 384;
constexpr u32 kRegStopwatch = 0x0C;
constexpr u32 kRegTimer = 0x30;

constexpr bool is_gate_array(u32 address) { return (address & 0xFF'FF00) == 0xA1'2000; }

// A byte write uses the lane selected by A0 (even = high byte).
constexpr u16 byte_lanes(u32 address) { return (address & 1) ? 0x00FF : 0xFF00; }
constexpr u16 byte_to_lanes(u32 address, u8 value) {
    return (address & 1) ? u16{value} : static_cast<u16>(value << 8);
}
constexpr u8 lane_byte(u32 address, u16 word) {
    return static_cast<u8>((address & 1) ? (word & 0xFF) : (word >> 8));
}

} // namespace

MegaCd::MegaCd() {
    format_backup_ram();
    registers_.set_sub_irq_callback([this](int level) { sub_cpu_.set_irq_level(level); });
    cdc_.set_irq_callback([this](bool asserted) { registers_.set_interrupt_line(5, asserted); });
    asic_.set_done_callback([this] { registers_.raise_interrupt(1); });
    drive_.set_irq_callback([this] { registers_.raise_interrupt(4); });
    cdc_.set_dma_write([this](Cdc::Destination d, u32 address, u16 value) { cdc_dma_write(d, address, value); });
    sub_cpu_.set_interrupt_ack_handler([this](int level) { registers_.acknowledge_sub_interrupt(level); });
    reset();
}

void MegaCd::reset() {
    prg_ram_->reset();
    word_ram_->reset();
    registers_.reset();
    cdc_.reset();
    drive_.reset();  // the disc stays in the tray
    asic_.reset();
    pcm_.reset();
    sub_was_in_reset_ = true;
    sub_running_ = false;
    timer_reload_ = 0;
    timer_count_ = 0;
    stopwatch_base_ = 0;
    sub_time_ = 0;
    sub_alignment_ = sub_cpu_.cycles();
    request_main_interrupt(0);
}

void MegaCd::format_backup_ram() {
    // The directory block the BIOS writes when formatting the internal RAM:
    // volume name, free block count (8 KB / 64 - 3 = 0x7D) and the format IDs.
    static constexpr std::array<u8, 0x40> kFormat = {
        0x5F, 0x5F, 0x5F, 0x5F, 0x5F, 0x5F, 0x5F, 0x5F, 0x5F, 0x5F, 0x5F, 0x00, 0x00, 0x00, 0x00, 0x40,
        0x00, 0x7D, 0x00, 0x7D, 0x00, 0x7D, 0x00, 0x7D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        'S',  'E',  'G',  'A',  '_',  'C',  'D',  '_',  'R',  'O',  'M',  0x00, 0x01, 0x00, 0x00, 0x00,
        'R',  'A',  'M',  '_',  'C',  'A',  'R',  'T',  'R',  'I',  'D',  'G',  'E',  '_',  '_',  '_',
    };
    backup_ram_.fill(0);
    std::copy(kFormat.begin(), kFormat.end(), backup_ram_.end() - kFormat.size());
}

void MegaCd::load_backup_ram(std::span<const u8> data) {
    if (data.size() != kBackupRamSize) return;
    std::copy(data.begin(), data.end(), backup_ram_.begin());
}

void MegaCd::request_main_interrupt(int level) {
    if (level == main_irq_level_) return;
    main_irq_level_ = level;
    if (main_irq_changed_) main_irq_changed_(level);
}

void MegaCd::acknowledge_main_interrupt(int level) {
    if (level == main_irq_level_) request_main_interrupt(0);
}

void MegaCd::load_bios(std::span<const u8> image) {
    std::fill(bios_.begin(), bios_.end(), u8{0xFF});
    std::copy_n(image.begin(), std::min<std::size_t>(image.size(), bios_.size()), bios_.begin());
}

// ===========================================================================
// Main CPU side
// ===========================================================================

u16 MegaCd::main_read_memory16(u32 offset) {
    offset &= ~1u;
    if (offset < kWordRamBase) {
        if ((offset & 0x3'FFFF) < kPrgWindow) {
            const u32 a = offset & (kBiosSize - 2);  // BIOS, mirrored
            return static_cast<u16>((bios_[a] << 8) | bios_[a + 1]);
        }
        // PRG-RAM window: only while the sub CPU is stopped.
        if (!registers_.sub_halted()) return kOpenBus;
        return prg_ram_->main_read16(offset & (ProgramRam::kBankSize - 1));
    }
    if (offset < kWordRamEnd) return word_ram_->main_read16(offset - kWordRamBase);
    return kOpenBus;
}

void MegaCd::main_write_memory16(u32 offset, u16 value, u16 lanes) {
    if (offset < kWordRamBase) {
        if ((offset & 0x3'FFFF) < kPrgWindow) return;  // BIOS is ROM
        if (!registers_.sub_halted()) return;
        const u32 o = offset & (ProgramRam::kBankSize - 1);
        if (lanes == 0xFFFF)      prg_ram_->main_write16(o, value);
        else if (lanes == 0xFF00) prg_ram_->main_write8(o & ~1u, static_cast<u8>(value >> 8));
        else                      prg_ram_->main_write8(o | 1u, static_cast<u8>(value));
        return;
    }
    if (offset < kWordRamEnd) {
        const u32 o = offset - kWordRamBase;
        if (lanes == 0xFFFF)      word_ram_->main_write16(o, value);
        else if (lanes == 0xFF00) word_ram_->main_write8(o & ~1u, static_cast<u8>(value >> 8));
        else                      word_ram_->main_write8(o | 1u, static_cast<u8>(value));
    }
}

u16 MegaCd::main_register_read(u32 offset, u16 lanes) {
    offset &= 0xFE;
    switch (offset) {
    case 0x04: return static_cast<u16>(cdc_.mode_word() & 0xC700);  // EDT, DSR, DD
    case 0x08: return cdc_.read_host_data(true);                    // when DD = main
    case 0x0A: return 0x0000;
    case kRegStopwatch: return stopwatch();
    default: break;
    }
    (void)lanes;
    return offset < MegaCdRegisters::kMainCommArea ? registers_.main_read(offset) : kOpenBus;
}

void MegaCd::main_register_write(u32 offset, u16 value, u16 lanes) {
    offset &= 0xFE;
    if (offset == 0x04 || offset == 0x08 || offset == 0x0A) return;  // CDC: read-only for main
    if (offset < MegaCdRegisters::kMainCommArea) registers_.main_write(offset, value, lanes);
}

u16 MegaCd::read16(u32 address) {
    address &= kAddressMask;
    if (is_gate_array(address)) return main_register_read(address & 0xFF, 0xFFFF);
    return main_read_memory16(address & kExpansionMask);
}

u8 MegaCd::read8(u32 address) {
    address &= kAddressMask;
    if (is_gate_array(address)) {
        return lane_byte(address, main_register_read(address & 0xFE, byte_lanes(address)));
    }
    return lane_byte(address, main_read_memory16(address & kExpansionMask & ~1u));
}

void MegaCd::write16(u32 address, u16 value) {
    address &= kAddressMask;
    if (is_gate_array(address)) {
        main_register_write(address & 0xFF, value, 0xFFFF);
        return;
    }
    main_write_memory16(address & kExpansionMask, value, 0xFFFF);
}

void MegaCd::write8(u32 address, u8 value) {
    address &= kAddressMask;
    const u16 lanes = byte_lanes(address);
    const u16 word = byte_to_lanes(address, value);
    if (is_gate_array(address)) {
        main_register_write(address & 0xFE, word, lanes);
        return;
    }
    main_write_memory16(address & kExpansionMask & ~1u, word, lanes);
}

// ===========================================================================
// CDC wiring
// ===========================================================================

u16 MegaCd::sub_register_read(u32 offset, u16 lanes) {
    offset &= MegaCdRegisters::kSubArea - 2;
    switch (offset) {
    case 0x04: return cdc_.mode_word();
    case 0x06:
        // Only the low byte is the CDC register; reading it advances CA, so a
        // read of the high byte alone must not touch the CDC.
        return (lanes & 0x00FF) ? cdc_.read_register() : 0x0000;
    case 0x08: return cdc_.read_host_data(false);
    case 0x0A: return cdc_.dma_address();
    case kRegStopwatch: return stopwatch();
    case kRegTimer: return timer_reload_;
    default:
        if (CdDrive::owns(offset)) return drive_.read_register(offset);
        if (offset >= 0x58 && offset < 0x68) return asic_.read(offset);
        return registers_.sub_read(offset);
    }
}

void MegaCd::sub_register_write(u32 offset, u16 value, u16 lanes) {
    offset &= MegaCdRegisters::kSubArea - 2;
    switch (offset) {
    case 0x04:
        if (lanes & 0xFF00) cdc_.set_destination(static_cast<u8>((value >> 8) & 7));
        if (lanes & 0x00FF) cdc_.set_register_address(static_cast<u8>(value & 0x0F));
        return;
    case 0x06:
        if (lanes & 0x00FF) cdc_.write_register(static_cast<u8>(value));
        return;
    case 0x08:
        return;  // host data is read-only
    case 0x0A:
        cdc_.set_dma_address(static_cast<u16>((cdc_.dma_address() & ~lanes) | (value & lanes)));
        return;
    case kRegStopwatch:
        stopwatch_base_ = sub_now();  // any write clears it
        return;
    case kRegTimer:
        if (lanes & 0x00FF) {
            timer_reload_ = static_cast<u8>(value);
            timer_count_ = timer_reload_ ? (timer_reload_ + 1) * kTimerUnit : 0;
        }
        return;
    default:
        if (CdDrive::owns(offset)) {
            drive_.write_register(offset, value, lanes);
            return;
        }
        if (offset >= 0x58 && offset < 0x68) {
            asic_.write(offset, value, lanes);
            return;
        }
        registers_.sub_write(offset, value, lanes);
    }
}

void MegaCd::cdc_dma_write(Cdc::Destination destination, u32 address, u16 value) {
    switch (destination) {
    case Cdc::Destination::PrgRam:
        // Same rules as sub CPU writes, including write protection.
        prg_ram_->sub_write16(address & (ProgramRam::kSize - 1), value);
        break;
    case Cdc::Destination::WordRam:
        // Into the Word RAM the sub CPU currently has: all of it in 2M mode
        // (if it owns it), its own bank in 1M mode.
        if (word_ram_->mode() == WordRam::Mode::TwoMeg) {
            word_ram_->sub_write16(address & (WordRam::kSize - 1), value);
        } else {
            word_ram_->sub_write16(0x40000 + (address & (WordRam::kBankSize - 1)), value);
        }
        break;
    case Cdc::Destination::Pcm:
        pcm_.run_until(sub_now());
        pcm_.dma_write(address, value);
        break;
    default:
        break;
    }
}

// ===========================================================================
// Sound
// ===========================================================================

void MegaCd::render_audio(s64 sub_time, s32& left, s32& right) {
    s32 pl = 0, pr = 0, cl = 0, cr = 0;
    pcm_.output_at(sub_time, pl, pr);
    drive_.render_cdda(sub_time, cl, cr);
    left  = pl / kPcmGainDiv + cl / kCddaGainDiv;
    right = pr / kPcmGainDiv + cr / kCddaGainDiv;
}

// ===========================================================================
// Timer and stopwatch
// ===========================================================================

s64 MegaCd::sub_now() const noexcept {
    return sub_running_ ? sub_cpu_.cycles() - sub_alignment_ : sub_time_;
}

u16 MegaCd::stopwatch() const noexcept {
    return static_cast<u16>(((sub_now() - stopwatch_base_) / kTimerUnit) & 0x0FFF);
}

void MegaCd::advance_timer(s64 sub_cycles) {
    if (timer_count_ <= 0) return;
    timer_count_ -= sub_cycles;
    while (timer_count_ <= 0) {
        timer_count_ += (timer_reload_ + 1) * kTimerUnit;
        // Only latched while enabled in the interrupt mask.
        if (registers_.sub_interrupt_mask() & (1u << 3)) registers_.raise_interrupt(3);
    }
}

// ===========================================================================
// Sub CPU
// ===========================================================================

void MegaCd::run_sub_until(s64 target) {
    if (target <= sub_time_) return;

    // The drive spins on its own, whether or not the sub CPU is running.
    // Sectors due in this slice reach the CDC before the CPU runs it.
    drive_.advance(target - sub_time_);
    asic_.advance(target - sub_time_);
    advance_timer(target - sub_time_);

    if (registers_.sub_reset_asserted()) {
        sub_was_in_reset_ = true;
    } else {
        if (sub_was_in_reset_) {
            // Reset released: the sub CPU fetches its vectors from PRG-RAM.
            sub_was_in_reset_ = false;
            sub_cpu_.reset();
            sub_cpu_.set_irq_level(registers_.sub_irq_level());
            sub_alignment_ = sub_cpu_.cycles() - sub_time_;  // reset sequence counts as elapsed
        }
        if (!registers_.sub_bus_requested()) {
            // The CPU's counter tracks the timeline at a fixed offset, so an
            // instruction that overshoots one slice is absorbed by the next.
            sub_running_ = true;
            sub_cpu_.run_until(target + sub_alignment_);
            sub_running_ = false;
            sub_time_ = target;
            return;
        }
    }

    // Paused: time passes without the CPU. Re-anchor so that it resumes from
    // "now" instead of catching up.
    sub_time_ = target;
    sub_alignment_ = sub_cpu_.cycles() - sub_time_;
}

// --- SubBus --------------------------------------------------------------

u16 MegaCd::SubBus::read_word(u32 address) {
    address &= kAddressMask & ~1u;
    auto& cd = cd_;
    if (address < ProgramRam::kSize) return cd.prg_ram_->sub_read16(address);
    if (address >= kSubWordRamBase && address < kSubWordRamEnd) {
        return cd.word_ram_->sub_read16(address - kSubWordRamBase);
    }
    if (address >= kSubBackupBase && address < kSubBackupEnd) {
        // 8-bit RAM on the odd byte lane.
        return static_cast<u16>(0xFF00 | cd.backup_ram_[(address >> 1) & (kBackupRamSize - 1)]);
    }
    if (address >= kSubRegsBase && address < kSubRegsEnd) return cd.sub_register_read(address - kSubRegsBase, 0xFFFF);
    if (address >= kSubPcmBase && address < kSubPcmEnd) {
        cd.pcm_.run_until(cd.sub_now());  // channel addresses are live
        return cd.pcm_.read((address - kSubPcmBase) | 1u);  // the byte, on the low lane
    }
    return kOpenBus;
}

u8 MegaCd::SubBus::read_byte(u32 address) {
    address &= kAddressMask;
    if (address >= kSubRegsBase && address < kSubRegsEnd) {
        return lane_byte(address, cd_.sub_register_read((address - kSubRegsBase) & ~1u, byte_lanes(address)));
    }
    return lane_byte(address, read_word(address & ~1u));
}

void MegaCd::SubBus::write_word(u32 address, u16 value) {
    address &= kAddressMask & ~1u;
    auto& cd = cd_;
    if (address < ProgramRam::kSize) { cd.prg_ram_->sub_write16(address, value); return; }
    if (address >= kSubWordRamBase && address < kSubWordRamEnd) {
        cd.word_ram_->sub_write16(address - kSubWordRamBase, value);
        return;
    }
    if (address >= kSubBackupBase && address < kSubBackupEnd) {
        cd.backup_ram_[(address >> 1) & (kBackupRamSize - 1)] = static_cast<u8>(value);
        return;
    }
    if (address >= kSubRegsBase && address < kSubRegsEnd) {
        cd.sub_register_write(address - kSubRegsBase, value, 0xFFFF);
        return;
    }
    if (address >= kSubPcmBase && address < kSubPcmEnd) {
        cd.pcm_.run_until(cd.sub_now());
        cd.pcm_.write((address - kSubPcmBase) | 1u, static_cast<u8>(value));
    }
}

void MegaCd::SubBus::write_byte(u32 address, u8 value) {
    address &= kAddressMask;
    auto& cd = cd_;
    if (address < ProgramRam::kSize) { cd.prg_ram_->sub_write8(address, value); return; }
    if (address >= kSubWordRamBase && address < kSubWordRamEnd) {
        cd.word_ram_->sub_write8(address - kSubWordRamBase, value);
        return;
    }
    if (address >= kSubBackupBase && address < kSubBackupEnd) {
        if (address & 1) cd.backup_ram_[(address >> 1) & (kBackupRamSize - 1)] = value;
        return;
    }
    if (address >= kSubRegsBase && address < kSubRegsEnd) {
        cd.sub_register_write((address - kSubRegsBase) & ~1u, byte_to_lanes(address, value),
                              byte_lanes(address));
        return;
    }
    if (address >= kSubPcmBase && address < kSubPcmEnd) {
        cd.pcm_.run_until(cd.sub_now());
        cd.pcm_.write(address - kSubPcmBase, value);  // even addresses are ignored
    }
}

u16 MegaCd::SubBus::peek_word(u32 address) const noexcept {
    address &= kAddressMask & ~1u;
    if (address < ProgramRam::kSize) return cd_.prg_ram_->sub_read16(address);
    return kOpenBus;
}

} // namespace md::megacd
