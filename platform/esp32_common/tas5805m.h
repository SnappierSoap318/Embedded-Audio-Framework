#pragma once
#include <eaf/eaf_types.h>

/* Minimal TAS5805M bring-up: releases PDN, probes the I2C address, loads a
   basic play configuration and reports faults. Register writes follow the
   TAS5805M datasheet (SLASEH5D) startup procedure; the full DSP coefficient
   profile is board-specific and may be added later. */

int tas5805m_bringup(void);
/* Re-assert Play after I2S clocks start; safe to call per stream. */
int tas5805m_play(void);
/* True if the amplifier acknowledged during bring-up. */
bool tas5805m_present(void);
/* Last FAULT input level (true = asserted). */
bool tas5805m_fault(void);
