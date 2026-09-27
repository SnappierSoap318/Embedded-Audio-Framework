#pragma once
#include <eaf/eaf_types.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Portable TAS5805M core. Register writes go through the board's bus; a nonzero
   return fails the transaction. fault_asserted reports the active-low FAULT
   input and may be NULL when the carrier does not wire it. */

typedef struct {
    int (*write)(uint8_t reg, const uint8_t *data, size_t length, void *ctx);
    bool (*fault_asserted)(void *ctx);
    void *ctx;
} tas5805m_io_t;

/* Load the basic play register map (0 dB, BD modulation, 175 kHz loop, FAULT
   pin) and re-assert Play. Returns EAF_OK, EAF_IO or EAF_INVALID. */
int tas5805m_bringup(const tas5805m_io_t *io);
/* Re-assert Play after I2S clocks start; safe to call per stream. */
int tas5805m_play(const tas5805m_io_t *io);
/* True if the amplifier acknowledged during bring-up. */
bool tas5805m_present(void);
/* Last FAULT input level (true = asserted). */
bool tas5805m_fault(const tas5805m_io_t *io);
/* Board backend: configures PDN/FAULT, probes the I2C address and returns the
   register I/O binding, or NULL when the amplifier is absent. */
const tas5805m_io_t *tas5805m_board_io(void);
