#pragma once

#include <array>
#include <functional>
#include <span>

#include "md/types.hpp"

namespace md::megacd {

// Sanyo LC8951 CD data controller ("CDC"), as wired in the Mega-CD.
//
// The drive feeds it raw sectors; when its decoder is enabled it writes each
// sector into a 16 KB buffer RAM and raises the "decoded" interrupt (DECI).
// The sub CPU then transfers data out of the buffer, either by reading the
// host data register itself (or letting the main CPU do it), or by DMA into
// PRG-RAM, Word RAM or PCM RAM. The end of a transfer raises DTEI. Both
// interrupts go to the sub CPU as INT5 (when enabled in IFCTRL and in the
// gate array's interrupt mask).
//
// Gate array interface (sub CPU 0xFF8000 + offset, main CPU 0xA12000 + offset):
//   04  sub R/W: EDT (15) | DSR (14) | DD (10-8, destination) | CA (3-0, register address)
//       main R:  EDT | DSR | DD
//   06  sub R/W: low byte = CDC register at CA (CA then auto-increments)
//   08  host data: R (sub when DD = 3, main when DD = 2), one word per read
//   0A  sub R/W: DMA address (PRG-RAM / Word RAM: byte address / 8)
//
// CDC registers (CA):
//   write: 0 SBOUT  1 IFCTRL  2 DBCL  3 DBCH  4 DACL  5 DACH  6 DTTRG  7 DTACK
//          8 WAL    9 WAH    10 CTRL0 11 CTRL1 12 PTL  13 PTH  14 -     15 RESET
//   read:  0 COMIN  1 IFSTAT  2 DBCL  3 DBCH  4-7 HEAD0-3   8 PTL  9 PTH
//          10 WAL  11 WAH   12-15 STAT0-3
//
// Simplifications (TODO): DMA completes instantly; EDC/ECC are not checked
// (STAT0 always reports CRC OK); the command/status serial port (COMIN/SBOUT)
// is unused on the Mega-CD and not modelled.
class Cdc {
public:
    static constexpr u32 kBufferSize = 16 * 1024;

    enum class Destination : u8 {
        MainHost = 2,  // main CPU reads A12008
        SubHost  = 3,  // sub CPU reads FF8008
        Pcm      = 4,
        PrgRam   = 5,
        WordRam  = 7,
    };

    // IFCTRL bits.
    static constexpr u8 kDteiEnable = 0x40;  // transfer-end interrupt
    static constexpr u8 kDeciEnable = 0x20;  // sector-decoded interrupt
    static constexpr u8 kDoutEnable = 0x02;  // data output (transfers) enabled
    // IFSTAT bits (active low).
    static constexpr u8 kDtei  = 0x40;
    static constexpr u8 kDeci  = 0x20;
    static constexpr u8 kDtbsy = 0x08;
    static constexpr u8 kDten  = 0x02;
    // CTRL0 bits.
    static constexpr u8 kDecoderEnable = 0x80;
    static constexpr u8 kWriteRequest  = 0x04;  // store decoded sectors in the buffer

    // Writes one word of a DMA transfer to its destination memory.
    using DmaWrite = std::function<void(Destination, u32 address, u16 value)>;

    Cdc() { reset(); }

    void reset();  // power-on state (also register 15 RESET, minus the DMA address)

    void set_dma_write(DmaWrite fn) { dma_write_ = std::move(fn); }
    void set_irq_callback(std::function<void(bool)> fn) { irq_changed_ = std::move(fn); }

    // --- Gate array side ------------------------------------------------------
    [[nodiscard]] u16 mode_word() const noexcept;   // reg 04 as read by the sub CPU
    void set_destination(u8 dd);                    // reg 04 high byte
    void set_register_address(u8 ca) noexcept { ca_ = ca & 0x0F; }
    [[nodiscard]] u8 register_address() const noexcept { return ca_; }

    u8   read_register();                           // reg 06 (low byte), then CA++
    void write_register(u8 value);

    u16  read_host_data(bool main_cpu);             // reg 08

    [[nodiscard]] u16 dma_address() const noexcept { return dma_address_; }
    void set_dma_address(u16 value) noexcept { dma_address_ = value; }

    // --- Drive side -------------------------------------------------------------
    // A sector arrives from the drive (raw 2352 bytes, see Disc).
    void decode_sector(std::span<const u8> raw);

    // --- Inspection ---------------------------------------------------------------
    [[nodiscard]] std::span<const u8> buffer() const noexcept { return buffer_; }
    [[nodiscard]] u16 write_address() const noexcept { return wa_; }
    [[nodiscard]] u16 pointer() const noexcept { return pt_; }
    [[nodiscard]] u8  ifstat() const noexcept { return ifstat_; }
    [[nodiscard]] bool irq() const noexcept { return irq_; }
    [[nodiscard]] bool data_set_ready() const noexcept { return dsr_; }
    [[nodiscard]] bool end_of_transfer() const noexcept { return edt_; }
    [[nodiscard]] u64 sectors_decoded() const noexcept { return sectors_decoded_; }

private:
    void start_transfer();
    void finish_transfer();
    void update_irq();
    [[nodiscard]] u8 buffer_at(u32 address) const noexcept { return buffer_[address & (kBufferSize - 1)]; }

    std::array<u8, kBufferSize> buffer_{};

    // LC8951 registers.
    u8  ca_ = 0;        // register address (gate array)
    u8  ifctrl_ = 0;
    u8  ifstat_ = 0xFF;
    u16 dbc_ = 0;       // byte count - 1
    u16 dac_ = 0;       // buffer address of the next byte to transfer
    u16 wa_ = 0;        // buffer address where the next sector is written
    u16 pt_ = 0;        // buffer address of the last sector's header
    u8  ctrl0_ = 0, ctrl1_ = 0;
    std::array<u8, 4> head_{};
    std::array<u8, 4> stat_{};

    // Gate array transfer state.
    Destination dest_ = Destination::SubHost;
    bool edt_ = false;  // end of data transfer
    bool dsr_ = false;  // data set ready (host data can be read)
    s32  remaining_ = 0;
    u16  dma_address_ = 0;

    bool irq_ = false;
    u64  sectors_decoded_ = 0;
    DmaWrite dma_write_;
    std::function<void(bool)> irq_changed_;
};

} // namespace md::megacd
