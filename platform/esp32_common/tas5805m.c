#include "tas5805m.h"
#include <eaf/eaf_hal.h>

/* TAS5805M book/page selection must return to page zero before changing books. */
static bool present;

static int write_u8(const tas5805m_io_t *io, uint8_t reg, uint8_t value) {
    return io->write(reg, &value, 1u, io->ctx);
}

static void select_book_page(const tas5805m_io_t *io, uint8_t book, uint8_t page) {
    (void)write_u8(io, 0x00u, page);
    (void)write_u8(io, 0x7fu, book);
    (void)write_u8(io, 0x00u, page);
}

int tas5805m_bringup(const tas5805m_io_t *io) {
    if (!io || !io->write)
        return EAF_INVALID;
    select_book_page(io, 0x00u, 0x00u);
    if (write_u8(io, 0x02u, 0x00u)) {
        present = false;
        return EAF_IO;
    }
    /* BD modulation, standard 32-bit I2S, 175 kHz loop bandwidth, ADR/FAULT. */
    (void)write_u8(io, 0x33u, 0x03u);
    (void)write_u8(io, 0x53u, 0x60u);
    (void)write_u8(io, 0x61u, 0x0bu);
    select_book_page(io, 0x8cu, 0x2au);
    static const uint8_t volume_0db[4] = {0x00u, 0x80u, 0x00u, 0x00u};
    (void)io->write(0x24u, volume_0db, sizeof(volume_0db), io->ctx);
    select_book_page(io, 0x00u, 0x00u);
    (void)write_u8(io, 0x03u, 0x02u);
    hal_sleep_ms(5);
    present = true;
    return tas5805m_play(io);
}

int tas5805m_play(const tas5805m_io_t *io) {
    if (!present)
        return EAF_UNSUPPORTED;
    if (!io || !io->write)
        return EAF_INVALID;
    select_book_page(io, 0x00u, 0x00u);
    return write_u8(io, 0x03u, 0x03u) ? EAF_IO : EAF_OK;
}

bool tas5805m_present(void) {
    return present;
}

bool tas5805m_fault(const tas5805m_io_t *io) {
    if (!present || !io || !io->fault_asserted)
        return false;
    return io->fault_asserted(io->ctx);
}
