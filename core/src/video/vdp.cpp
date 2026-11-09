#include "md/vdp.hpp"

#include <algorithm>

namespace md {

namespace {

constexpr int kNtscLines = 262;
constexpr int kPalLines  = 313;

// Bits 15-10 of the status word are not driven by the VDP; the 68000 sees
// the upper bits of the next opcode. 0x3400 is the common "NOP-ish" value.
constexpr u16 kStatusFixedBits = 0x3400;

} // namespace

Vdp::Vdp(Region region, Framebuffer& framebuffer)
    : region_(region), framebuffer_(framebuffer) {
    reset();
}

void Vdp::reset() {
    regs_.fill(0);
    vram_.fill(0);
    cram_.fill(0);
    vsram_.fill(0);
    address_ = 0;
    code_ = 0;
    write_pending_ = false;
    dma_fill_pending_ = false;
    sprite_overflow_ = false;
    sprite_collision_ = false;
    prev_line_dot_overflow_ = false;
    vint_pending_ = false;
    hint_pending_ = false;
    hint_counter_ = 0;
    line_ = 0;
    line_cycle_ = 0;
    frame_ = 0;
    framebuffer_.set_resolution(active_width(), active_height());
    update_irq();
}

// ===========================================================================
// CPU interface
// ===========================================================================

u16 Vdp::read(u32 port) {
    switch (port & 0x1E) {
    case 0x00: case 0x02: return read_data();
    case 0x04: case 0x06: return read_status();
    default:              return hv_counter();  // 0x08-0x0E
    }
}

void Vdp::write(u32 port, u16 value) {
    switch (port & 0x1E) {
    case 0x00: case 0x02: write_data(value);    break;
    case 0x04: case 0x06: write_control(value); break;
    default:              break;  // HV counter is read-only
    }
}

// Control port protocol:
//
//   Register write (single word):  100R RRRR DDDD DDDD
//   Command, first word:           CD1 CD0 A13 ... A0
//   Command, second word:          0000 0000 CD5 CD4 CD3 CD2 0 0 A15 A14
//
// A word with top bits 10 is only a register write when no command is
// half-written; otherwise it is taken as the second command word.
void Vdp::write_control(u16 value) {
    if (!write_pending_) {
        if ((value & 0xC000) == 0x8000) {
            write_register((value >> 8) & 0x1F, static_cast<u8>(value & 0xFF));
            return;
        }
        // First half: hardware updates the low address bits and CD1-CD0 immediately.
        address_ = static_cast<u16>((address_ & 0xC000) | (value & 0x3FFF));
        code_    = static_cast<u8>((code_ & 0x3C) | ((value >> 14) & 0x03));
        write_pending_ = true;
        return;
    }

    address_ = static_cast<u16>((address_ & 0x3FFF) | ((value & 0x0003) << 14));
    code_    = static_cast<u8>((code_ & 0x03) | ((value >> 2) & 0x3C));
    write_pending_ = false;

    // CD5 set + DMA enabled in reg 1: the command starts a DMA.
    if ((code_ & 0x20) && dma_enabled()) {
        switch (regs_[23] >> 6) {
        case 0b00:
        case 0b01: run_dma_transfer(); break;
        case 0b10: dma_fill_pending_ = true; break;  // waits for the fill value
        case 0b11: run_dma_copy(); break;
        }
    }
}

void Vdp::write_register(int index, u8 value) {
    if (index >= kNumRegisters) return;  // registers 24-31 don't exist
    regs_[static_cast<std::size_t>(index)] = value;

    // Enabling an interrupt while it is pending raises it immediately.
    if (index == 0 || index == 1) update_irq();
}

void Vdp::write_data(u16 value) {
    write_pending_ = false;
    write_target(value);

    if (dma_fill_pending_) {
        dma_fill_pending_ = false;
        run_dma_fill(value);
    }
}

void Vdp::write_target(u16 value) {
    switch (static_cast<Target>(code_ & 0x0F)) {
    case Target::VramWrite:
        // A word is stored big-endian at the even address; an odd address
        // swaps the two bytes (hardware quirk: A0 is XORed into the byte lane).
        vram_[address_]       = static_cast<u8>(value >> 8);
        vram_[address_ ^ 1u]  = static_cast<u8>(value & 0xFF);
        break;
    case Target::CramWrite:
        cram_[(address_ >> 1) & 0x3F] = static_cast<u16>(value & 0x0EEE);
        break;
    case Target::VsramWrite:
        if (const auto i = static_cast<std::size_t>((address_ >> 1) & 0x3F); i < vsram_.size()) {
            vsram_[i] = static_cast<u16>(value & 0x07FF);
        }
        break;
    default:
        // Data writes while the code register selects a read are ignored.
        break;
    }

    address_ = static_cast<u16>(address_ + auto_increment());
}

u16 Vdp::read_data() {
    write_pending_ = false;

    u16 value = 0;
    switch (static_cast<Target>(code_ & 0x0F)) {
    case Target::VramRead: {
        const auto a = static_cast<std::size_t>(address_ & 0xFFFE);
        value = static_cast<u16>((vram_[a] << 8) | vram_[a + 1]);
        break;
    }
    case Target::CramRead:
        value = cram_[(address_ >> 1) & 0x3F];
        break;
    case Target::VsramRead:
        if (const auto i = static_cast<std::size_t>((address_ >> 1) & 0x3F); i < vsram_.size()) {
            value = vsram_[i];
        }
        break;
    default:
        // Reading while a write target is selected locks up real hardware.
        return 0;
    }

    address_ = static_cast<u16>(address_ + auto_increment());
    return value;
}

u16 Vdp::read_status() {
    const u16 value = status();
    // Reading the status register cancels a half-written command and clears
    // the sprite overflow/collision flags.
    write_pending_ = false;
    sprite_overflow_ = false;
    sprite_collision_ = false;
    return value;
}

u16 Vdp::status() const noexcept {
    u16 s = kStatusFixedBits | Status::FifoEmpty;  // TODO: model the 4-entry write FIFO
    if (vint_pending_) s |= Status::VintPending;
    if (sprite_overflow_)  s |= Status::SpriteOverflow;
    if (sprite_collision_) s |= Status::SpriteCollision;
    // With the display disabled the VDP reports VBlank permanently.
    if (line_ >= active_height() || !display_enabled()) s |= Status::VBlank;
    if (line_cycle_ >= kHBlankStart) s |= Status::HBlank;
    if (region_ == Region::PAL) s |= Status::Pal;
    return s;
}

// ===========================================================================
// DMA
// ===========================================================================

u32 Vdp::dma_length() const noexcept {
    // Regs 19/20, in words for transfers and bytes for fill/copy. 0 means 64K.
    const u32 length = regs_[19] | (u32{regs_[20]} << 8);
    return length ? length : 0x10000;
}

void Vdp::finish_dma(u32 source_low) noexcept {
    // The hardware counts the length registers down to 0 and leaves the
    // source registers pointing past the last word read.
    regs_[19] = 0;
    regs_[20] = 0;
    regs_[21] = static_cast<u8>(source_low & 0xFF);
    regs_[22] = static_cast<u8>((source_low >> 8) & 0xFF);
}

// 68000 -> VRAM/CRAM/VSRAM. The destination is whatever the command's code
// selects, exactly as if the CPU had written each word to the data port.
void Vdp::run_dma_transfer() {
    // Source (word address) = reg 23 bits 6-0 : reg 22 : reg 21. Only the
    // low 16 bits (regs 21/22) count up, so a transfer wraps at 128 KB.
    const u32 source_high = u32{static_cast<u32>(regs_[23] & 0x7F)} << 17;
    u32 source_low = regs_[21] | (u32{regs_[22]} << 8);

    for (u32 n = dma_length(); n > 0; --n) {
        const u32 source = source_high | (source_low << 1);
        write_target(dma_read_ ? dma_read_(source) : 0);
        source_low = (source_low + 1) & 0xFFFF;
    }
    finish_dma(source_low);
    // TODO: the 68000 is frozen while the transfer runs (~18 words per active
    // line, ~205 bytes per VBlank line in H40). Report the stall to the scheduler.
}

// The data port write that triggers the fill is stored normally; then the
// high byte of that value is written `length` more times.
// Note: emulators disagree on the byte lane for VRAM fills; we follow the
// common convention of writing to address ^ 1. Games usually fill with
// identical high/low bytes (e.g. 0x0000), which makes the lane irrelevant.
void Vdp::run_dma_fill(u16 value) {
    const u8 fill = static_cast<u8>(value >> 8);
    for (u32 n = dma_length(); n > 0; --n) {
        switch (static_cast<Target>(code_ & 0x0F)) {
        case Target::VramWrite:  vram_[address_ ^ 1u] = fill; break;
        case Target::CramWrite:  cram_[(address_ >> 1) & 0x3F] = static_cast<u16>(value & 0x0EEE); break;
        case Target::VsramWrite:
            if (const auto i = static_cast<std::size_t>((address_ >> 1) & 0x3F); i < vsram_.size()) {
                vsram_[i] = static_cast<u16>(value & 0x07FF);
            }
            break;
        default: break;
        }
        address_ = static_cast<u16>(address_ + auto_increment());
    }
    finish_dma(regs_[21] | (u32{regs_[22]} << 8));
}

// VRAM -> VRAM, byte by byte. Source = regs 22:21 (a VRAM byte address).
void Vdp::run_dma_copy() {
    u16 source = static_cast<u16>(regs_[21] | (regs_[22] << 8));
    for (u32 n = dma_length(); n > 0; --n) {
        vram_[address_] = vram_[source];
        source = static_cast<u16>(source + 1);
        address_ = static_cast<u16>(address_ + auto_increment());
    }
    finish_dma(source);
}

// ===========================================================================
// HV counter
// ===========================================================================

u16 Vdp::hv_counter() const noexcept {
    // H counter (in 2-pixel units) skips a range in HBlank:
    //   H32: 0x00-0x93, then 0xE9-0xFF       H40: 0x00-0xB6, then 0xE4-0xFF
    const bool h40 = active_width() == 320;
    const int  h_steps   = h40 ? 211 : 171;
    const int  h_jump    = h40 ? 0xB6 : 0x93;
    const int  h_resume  = h40 ? 0xE4 : 0xE9;
    const int  h_index   = line_cycle_ * h_steps / kMasterCyclesPerLine;
    const int  h = h_index <= h_jump ? h_index : h_index - (h_jump + 1) + h_resume;

    // V counter skips a range in VBlank so it fits in 8/9 bits:
    //   NTSC V28: 0x00-0xEA, 0xE5-0xFF     PAL V28: 0x00-0x102, 0x1CA-0x1FF
    //   PAL V30:  0x00-0x10A, 0x1D2-0x1FF  NTSC V30 (invalid mode): wraps at 0xFF
    int v_jump = 0, v_resume = 0;
    if (region_ == Region::NTSC) {
        if (active_height() == 224) { v_jump = 0xEA; v_resume = 0xE5; }
        else                        { v_jump = 0xFF; v_resume = 0x00; }
    } else {
        if (active_height() == 224) { v_jump = 0x102; v_resume = 0x1CA; }
        else                        { v_jump = 0x10A; v_resume = 0x1D2; }
    }
    const int v = line_ <= v_jump ? line_ : line_ - (v_jump + 1) + v_resume;

    // TODO: interlace mode replaces V bit 0 with V bit 8; reg 0 bit 1 latches HV.
    return static_cast<u16>(((v & 0xFF) << 8) | (h & 0xFF));
}

// ===========================================================================
// Timing
// ===========================================================================

int Vdp::lines_per_frame() const noexcept {
    return region_ == Region::PAL ? kPalLines : kNtscLines;
}

int Vdp::active_width() const noexcept {
    // Reg 12 bits 7 (RS0) and 0 (RS1) select H40.
    return (regs_[12] & 0x81) ? 320 : 256;
}

int Vdp::active_height() const noexcept {
    // Reg 1 bit 3 (M2) selects V30, which only exists on PAL consoles.
    return (region_ == Region::PAL && (regs_[1] & 0x08)) ? 240 : 224;
}

int Vdp::cycles_to_next_event() const noexcept {
    return line_cycle_ < kHBlankStart ? kHBlankStart - line_cycle_
                                      : kMasterCyclesPerLine - line_cycle_;
}

void Vdp::advance(int master_cycles) {
    while (master_cycles > 0) {
        const int step = std::min(master_cycles, cycles_to_next_event());
        line_cycle_   += step;
        master_cycles -= step;

        if (line_cycle_ == kHBlankStart) {
            on_hblank_start();
        } else if (line_cycle_ == kMasterCyclesPerLine) {
            line_cycle_ = 0;
            if (++line_ == lines_per_frame()) {
                line_ = 0;
                ++frame_;
            }
            on_line_start();
        }
    }
}

void Vdp::on_line_start() {
    if (line_ == 0) {
        // Resolution changes take effect on the next frame.
        framebuffer_.set_resolution(active_width(), active_height());
    }
    if (line_ == active_height()) {
        vint_pending_ = true;  // sets the F flag regardless of the enable bit
        update_irq();
        if (z80_int_) z80_int_(true);
    } else if (line_ == active_height() + 1) {
        if (z80_int_) z80_int_(false);
    }
}

void Vdp::on_hblank_start() {
    if (line_ < active_height()) render_line();

    // HINT counter: decremented on every active line (plus the first VBlank
    // line); when it underflows, HINT fires and the counter reloads from
    // reg 10. During the rest of VBlank it is reloaded on every line.
    if (line_ <= active_height()) {
        if (--hint_counter_ < 0) {
            hint_counter_ = regs_[10];
            hint_pending_ = true;
            update_irq();
        }
    } else {
        hint_counter_ = regs_[10];
    }
}

// ===========================================================================
// Interrupts
// ===========================================================================

void Vdp::update_irq() {
    int level = 0;
    if (vint_pending_ && vint_enabled())      level = kVintLevel;
    else if (hint_pending_ && hint_enabled()) level = kHintLevel;

    if (level != irq_level_) {
        irq_level_ = level;
        if (irq_callback_) irq_callback_(level);
    }
}

void Vdp::acknowledge_interrupt(int level) {
    if (level == kVintLevel)      vint_pending_ = false;
    else if (level == kHintLevel) hint_pending_ = false;
    update_irq();
}

// ===========================================================================
// Rendering
// ===========================================================================

Framebuffer::Pixel Vdp::cram_to_argb(u16 color) noexcept {
    // 3 bits per channel -> 8 bits: v * 255 / 7.
    const auto expand = [](unsigned v) { return (v * 255u + 3u) / 7u; };
    const unsigned r = expand((color >> 1) & 7u);
    const unsigned g = expand((color >> 5) & 7u);
    const unsigned b = expand((color >> 9) & 7u);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

u16 Vdp::vram_word(u32 address) const noexcept {
    const auto a = static_cast<std::size_t>(address & 0xFFFE);
    return static_cast<u16>((vram_[a] << 8) | vram_[a + 1]);
}

// Nametable entry: P PP V H TTTTTTTTTTT (priority, palette, vflip, hflip, tile).
// Tiles are 8x8, 4 bits per pixel, 4 bytes per row, left pixel in the high nibble.
u8 Vdp::tile_pixel(u16 entry, int x, int y) const noexcept {
    if (entry & 0x0800) x = 7 - x;
    if (entry & 0x1000) y = 7 - y;
    const u32 tile = entry & 0x07FFu;
    const u8 byte  = vram_[(tile * 32 + static_cast<u32>(y * 4 + x / 2)) & 0xFFFF];
    const u8 color = (x & 1) ? (byte & 0x0F) : (byte >> 4);
    if (color == 0) return 0;  // transparent
    return static_cast<u8>(((entry >> 8) & 0x80) | ((entry >> 9) & 0x30) | color);
}

void Vdp::render_plane(Plane plane, LineBuffer& out) const {
    const int  index = static_cast<int>(plane);
    const u32  nametable = plane == Plane::A ? u32{static_cast<u32>(regs_[2] & 0x38)} << 10
                                             : u32{static_cast<u32>(regs_[4] & 0x07)} << 13;

    // Plane size (reg 16): 0 = 32 cells, 1 = 64, 3 = 128 (2 is invalid -> 32).
    constexpr std::array<int, 4> kCells = {32, 64, 32, 128};
    const int width_cells  = kCells[regs_[16] & 0x03];
    const int height_cells = kCells[(regs_[16] >> 4) & 0x03];
    const int width_mask   = width_cells * 8 - 1;
    const int height_mask  = height_cells * 8 - 1;

    // Horizontal scroll (reg 11 bits 1-0): full screen, per 8 lines, or per line.
    // The table (reg 13) holds an A word and a B word per entry.
    u32 hscroll_offset = 0;
    switch (regs_[11] & 0x03) {
    case 0b00: hscroll_offset = 0; break;
    case 0b01: hscroll_offset = static_cast<u32>(line_ & 7) * 4; break;    // undocumented: first 8 lines
    case 0b10: hscroll_offset = static_cast<u32>(line_ & ~7) * 4; break;
    case 0b11: hscroll_offset = static_cast<u32>(line_) * 4; break;
    }
    const u32 hscroll_table = u32{static_cast<u32>(regs_[13] & 0x3F)} << 10;
    const int hscroll = vram_word(hscroll_table + hscroll_offset + static_cast<u32>(index) * 2) & 0x3FF;

    // Vertical scroll (reg 11 bit 2): full screen, or per 16-pixel column.
    const bool column_vscroll = (regs_[11] & 0x04) != 0;

    const int width = active_width();
    for (int x = 0; x < width; ++x) {
        const std::size_t vs_index = column_vscroll
            ? static_cast<std::size_t>(((x >> 4) * 2 + index) % kVsramEntries)
            : static_cast<std::size_t>(index);
        const int vscroll = vsram_[vs_index] & 0x3FF;

        const int px = (x - hscroll) & width_mask;
        const int py = (line_ + vscroll) & height_mask;
        const u32 cell = static_cast<u32>((py >> 3) * width_cells + (px >> 3));
        const u16 entry = vram_word(nametable + cell * 2);
        out[static_cast<std::size_t>(x)] = tile_pixel(entry, px & 7, py & 7);
    }
}

// The window replaces plane A where it is active. Reg 17: bit 7 = right side,
// bits 4-0 = split point in 16-pixel units. Reg 18: bit 7 = bottom side,
// bits 4-0 = split point in 8-line units. A line inside the vertical window
// region is window across its full width.
bool Vdp::window_covers(int x) const noexcept {
    const int split_line = (regs_[18] & 0x1F) * 8;
    const bool vertical = (regs_[18] & 0x80) ? line_ >= split_line : line_ < split_line;
    if (vertical) return true;

    const int split_x = (regs_[17] & 0x1F) * 16;
    return (regs_[17] & 0x80) ? x >= split_x : x < split_x;
}

void Vdp::render_window(LineBuffer& out) const {
    const bool h40 = active_width() == 320;
    const u32 nametable  = u32{static_cast<u32>(regs_[3] & (h40 ? 0x3C : 0x3E))} << 10;
    const int width_cells = h40 ? 64 : 32;  // the window is never scrolled

    const int width = active_width();
    for (int x = 0; x < width; ++x) {
        if (!window_covers(x)) continue;
        const u32 cell = static_cast<u32>((line_ >> 3) * width_cells + (x >> 3));
        out[static_cast<std::size_t>(x)] = tile_pixel(vram_word(nametable + cell * 2), x & 7, line_ & 7);
    }
}

// Sprite attribute table (reg 5), 8 bytes per sprite:
//   +0  ------YY YYYYYYYY  Y position + 128
//   +2  ----HHVV -LLLLLLL  size in cells - 1 (H, V), link to next sprite
//   +4  PCCVHTTT TTTTTTTT  same layout as a nametable entry
//   +6  -------X XXXXXXXX  X position + 128
// Sprites are drawn by following the link list from sprite 0. Multi-cell
// sprites use consecutive tiles in column-major order.
void Vdp::render_sprites(LineBuffer& out) {
    const bool h40 = active_width() == 320;
    const u32  table = u32{static_cast<u32>(regs_[5] & (h40 ? 0x7E : 0x7F))} << 9;
    const int  max_sprites  = h40 ? 80 : 64;   // entries in the table
    const int  max_per_line = h40 ? 20 : 16;   // sprites per line
    const int  width = active_width();
    int dot_budget = width;                    // sprite pixels per line (320 / 256)

    int  on_line = 0;
    bool seen_nonzero_x = false;
    bool dot_overflow = false;
    int  index = 0;

    for (int visited = 0; visited < max_sprites; ++visited) {
        const u32 base = table + static_cast<u32>(index) * 8;
        const int y          = (vram_word(base) & 0x1FF) - 128;
        const u16 size_link  = vram_word(base + 2);
        const int h_cells    = ((size_link >> 10) & 3) + 1;
        const int v_cells    = ((size_link >> 8) & 3) + 1;
        const int link       = size_link & 0x7F;

        const int row = line_ - y;
        if (row >= 0 && row < v_cells * 8) {
            if (++on_line > max_per_line) {
                sprite_overflow_ = true;
                break;
            }

            const u16 entry = vram_word(base + 4);
            const int raw_x = vram_word(base + 6) & 0x1FF;

            // Masking: a sprite at X=0 hides all later sprites on this line,
            // but only once a sprite with X != 0 was found on the line (or the
            // previous line ran out of sprite dots).
            if (raw_x == 0) {
                if (seen_nonzero_x || prev_line_dot_overflow_) break;
            } else {
                seen_nonzero_x = true;
            }

            const bool hflip = (entry & 0x0800) != 0;
            const bool vflip = (entry & 0x1000) != 0;
            int cell_row = row >> 3;
            if (vflip) cell_row = v_cells - 1 - cell_row;

            const int x0 = raw_x - 128;
            for (int c = 0; c < h_cells; ++c) {
                if (dot_budget <= 0) {
                    sprite_overflow_ = true;
                    dot_overflow = true;
                    break;
                }
                dot_budget -= 8;

                const int cell_col = hflip ? h_cells - 1 - c : c;
                const u16 tile = static_cast<u16>((entry + cell_col * v_cells + cell_row) & 0x07FF);
                const u16 cell_entry = static_cast<u16>((entry & 0xF800) | tile);

                for (int px = 0; px < 8; ++px) {
                    const int sx = x0 + c * 8 + px;
                    if (sx < 0 || sx >= width) continue;
                    const u8 pixel = tile_pixel(cell_entry, px, row & 7);
                    if (pixel == 0) continue;
                    auto& dst = out[static_cast<std::size_t>(sx)];
                    if (dst & 0x0F) {
                        sprite_collision_ = true;  // earlier sprite keeps the pixel
                        continue;
                    }
                    dst = pixel;
                }
            }
            if (dot_overflow) break;
        }

        if (link == 0 || link >= max_sprites) break;
        index = link;
    }
    prev_line_dot_overflow_ = dot_overflow;
    // TODO: real hardware caches the first 4 bytes of each entry (Y, size,
    // link) internally, updated on VRAM writes; a few games rely on that.
}

void Vdp::render_line() {
    if (line_ >= kMaxScreenHeight) return;

    const int width = active_width();
    auto row = framebuffer_.row(line_);
    const auto backdrop = cram_to_argb(cram_[regs_[7] & 0x3F]);

    if (!display_enabled()) {
        std::fill_n(row.begin(), width, backdrop);
        return;
    }

    LineBuffer a{};
    LineBuffer b{};
    LineBuffer sprites{};
    render_plane(Plane::B, b);
    render_plane(Plane::A, a);
    render_window(a);
    render_sprites(sprites);

    // Layer order, back to front:
    //   backdrop, B low, A low, sprites low, B high, A high, sprites high
    // TODO: shadow/highlight (reg 12 bit 3).
    constexpr u8 kPriority = 0x80;
    const auto opaque = [](u8 p) { return (p & 0x0F) != 0; };
    for (int x = 0; x < width; ++x) {
        const u8 ps = sprites[static_cast<std::size_t>(x)];
        const u8 pa = a[static_cast<std::size_t>(x)];
        const u8 pb = b[static_cast<std::size_t>(x)];

        u8 pixel = 0;
        bool found = true;
        if      (opaque(ps) && (ps & kPriority)) pixel = ps;
        else if (opaque(pa) && (pa & kPriority)) pixel = pa;
        else if (opaque(pb) && (pb & kPriority)) pixel = pb;
        else if (opaque(ps))                     pixel = ps;
        else if (opaque(pa))                     pixel = pa;
        else if (opaque(pb))                     pixel = pb;
        else                                     found = false;

        row[static_cast<std::size_t>(x)] = found ? cram_to_argb(cram_[pixel & 0x3F]) : backdrop;
    }

    // Reg 0 bit 5: blank the leftmost 8 pixels (hides horizontal scroll seams).
    if (regs_[0] & 0x20) std::fill_n(row.begin(), std::min(8, width), backdrop);
}

} // namespace md
