#include "md/megacd/cdc.hpp"

#include "md/megacd/disc.hpp"

namespace md::megacd {

void Cdc::reset() {
    ca_ = 0;
    ifctrl_ = 0;
    ifstat_ = 0xFF;          // all flags inactive (active low)
    dbc_ = dac_ = wa_ = pt_ = 0;
    ctrl0_ = ctrl1_ = 0;
    head_.fill(0);
    stat_ = {0x00, 0x00, 0x00, 0x80};  // STAT3 VALST = 1: no valid status yet
    edt_ = dsr_ = false;
    remaining_ = 0;
    dest_ = Destination::SubHost;
    dma_address_ = 0;
    update_irq();
}

// ===========================================================================
// Gate array side
// ===========================================================================

u16 Cdc::mode_word() const noexcept {
    return static_cast<u16>((edt_ ? 0x8000 : 0) | (dsr_ ? 0x4000 : 0) |
                            (static_cast<u16>(dest_) << 8) | ca_);
}

void Cdc::set_destination(u8 dd) {
    // Selecting a destination prepares a new transfer: EDT/DSR clear.
    dest_ = static_cast<Destination>(dd & 7);
    edt_ = false;
    dsr_ = false;
}

u8 Cdc::read_register() {
    u8 value = 0xFF;
    switch (ca_) {
    case 0:  value = 0xFF; break;                              // COMIN: no commands
    case 1:  value = ifstat_; break;
    case 2:  value = static_cast<u8>(dbc_); break;
    case 3:  value = static_cast<u8>((dbc_ >> 8) & 0x0F); break;
    case 4: case 5: case 6: case 7:
        value = head_[ca_ - 4u]; break;
    case 8:  value = static_cast<u8>(pt_); break;
    case 9:  value = static_cast<u8>(pt_ >> 8); break;
    case 10: value = static_cast<u8>(wa_); break;
    case 11: value = static_cast<u8>(wa_ >> 8); break;
    case 12: case 13: case 14:
        value = stat_[ca_ - 12u]; break;
    case 15:
        value = stat_[3];
        // Reading STAT3 acknowledges the decoder interrupt.
        ifstat_ |= kDeci;
        update_irq();
        break;
    default: break;
    }
    ca_ = (ca_ + 1) & 0x0F;
    return value;
}

void Cdc::write_register(u8 value) {
    switch (ca_) {
    case 0:  break;                                               // SBOUT
    case 1:
        ifctrl_ = value;
        if (!(value & kDoutEnable)) {
            // Data output disabled: any transfer stops.
            ifstat_ |= static_cast<u8>(kDtbsy | kDten);
            dsr_ = false;
        }
        update_irq();
        break;
    case 2:  dbc_ = static_cast<u16>((dbc_ & 0x0F00) | value); break;
    case 3:  dbc_ = static_cast<u16>((dbc_ & 0x00FF) | ((value & 0x0F) << 8)); break;
    case 4:  dac_ = static_cast<u16>((dac_ & 0xFF00) | value); break;
    case 5:  dac_ = static_cast<u16>((dac_ & 0x00FF) | (value << 8)); break;
    case 6:  start_transfer(); break;                             // DTTRG
    case 7:  ifstat_ |= kDtei; update_irq(); break;               // DTACK
    case 8:  wa_ = static_cast<u16>((wa_ & 0xFF00) | value); break;
    case 9:  wa_ = static_cast<u16>((wa_ & 0x00FF) | (value << 8)); break;
    case 10: ctrl0_ = value; break;
    case 11: ctrl1_ = value; break;
    case 12: pt_ = static_cast<u16>((pt_ & 0xFF00) | value); break;
    case 13: pt_ = static_cast<u16>((pt_ & 0x00FF) | (value << 8)); break;
    case 14: break;                                               // CTRL2 (unused)
    case 15: {                                                    // RESET
        const u16 dma = dma_address_;
        const auto dest = dest_;
        reset();
        dma_address_ = dma;
        dest_ = dest;
        return;  // RESET also clears the register address
    }
    default: break;
    }
    ca_ = (ca_ + 1) & 0x0F;
}

void Cdc::start_transfer() {
    if (!(ifctrl_ & kDoutEnable)) return;
    ifstat_ &= static_cast<u8>(~(kDtbsy | kDten));  // busy, transfer enabled
    edt_ = false;
    remaining_ = static_cast<s32>(dbc_) + 1;

    switch (dest_) {
    case Destination::MainHost:
    case Destination::SubHost:
        dsr_ = true;  // the CPU now pulls words through the host data register
        return;

    case Destination::PrgRam:
    case Destination::WordRam:
    case Destination::Pcm: {
        // DMA: done at once. PRG-RAM / Word RAM addresses are in units of
        // 8 bytes, PCM RAM addresses in units of 4 bytes.
        // TODO: real DMA takes time (and stalls the destination); games that
        // poll EDT right after DTTRG are fine, cycle-exact effects are not.
        u32 address = dest_ == Destination::Pcm ? u32{dma_address_} << 2 : u32{dma_address_} << 3;
        while (remaining_ > 0) {
            const u16 word = static_cast<u16>((buffer_at(dac_) << 8) | buffer_at(dac_ + 1u));
            if (dma_write_) dma_write_(dest_, address, word);
            address += 2;
            dac_ = static_cast<u16>(dac_ + 2);
            remaining_ -= 2;
        }
        dma_address_ = static_cast<u16>(dest_ == Destination::Pcm ? address >> 2 : address >> 3);
        finish_transfer();
        return;
    }
    }
    // Unused destination codes: nothing happens.
}

u16 Cdc::read_host_data(bool main_cpu) {
    const auto expected = main_cpu ? Destination::MainHost : Destination::SubHost;
    if (dest_ != expected || !dsr_) return 0x0000;

    const u16 word = static_cast<u16>((buffer_at(dac_) << 8) | buffer_at(dac_ + 1u));
    dac_ = static_cast<u16>(dac_ + 2);
    remaining_ -= 2;
    if (remaining_ <= 0) finish_transfer();
    return word;
}

void Cdc::finish_transfer() {
    edt_ = true;
    dsr_ = false;
    remaining_ = 0;
    dbc_ = 0x0FFF;                                       // counted down past zero
    ifstat_ |= static_cast<u8>(kDtbsy | kDten);          // idle
    ifstat_ &= static_cast<u8>(~kDtei);                  // transfer-end interrupt pending
    update_irq();
}

// ===========================================================================
// Drive side
// ===========================================================================

void Cdc::decode_sector(std::span<const u8> raw) {
    if (!(ctrl0_ & kDecoderEnable) || raw.size() < kRawSectorSize) return;

    // Header (MSF + mode) as found in the sector.
    for (std::size_t i = 0; i < 4; ++i) head_[i] = raw[12 + i];

    if (ctrl0_ & kWriteRequest) {
        // The whole sector goes to the buffer at WA; PT points to its header
        // (software then transfers from PT + 4, the start of user data).
        for (std::size_t i = 0; i < kRawSectorSize; ++i) {
            buffer_[(wa_ + i) & (kBufferSize - 1)] = raw[i];
        }
        pt_ = static_cast<u16>(wa_ + 12);
        wa_ = static_cast<u16>(wa_ + kRawSectorSize);
    }

    stat_[0] = 0x80;                       // CRCOK
    stat_[1] = 0x00;
    stat_[2] = static_cast<u8>(raw[15] == 2 ? 0x08 : 0x00);  // MODE bit for Mode 2
    stat_[3] = 0x00;                       // VALST = 0: status valid
    ifstat_ &= static_cast<u8>(~kDeci);    // decoder interrupt pending
    ++sectors_decoded_;
    update_irq();
}

void Cdc::update_irq() {
    const bool dtei = !(ifstat_ & kDtei) && (ifctrl_ & kDteiEnable);
    const bool deci = !(ifstat_ & kDeci) && (ifctrl_ & kDeciEnable);
    const bool irq = dtei || deci;
    if (irq != irq_) {
        irq_ = irq;
        if (irq_changed_) irq_changed_(irq_);
    }
}

} // namespace md::megacd
