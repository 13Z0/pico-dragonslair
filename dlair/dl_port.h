/*
 * Build glue shared by the Dragon's Lair core. The core is plain C with no Pico
 * dependencies, so the same sources build for the board and for the host
 * harness in hosttest/. On the board the hot paths go to SRAM.
 */
#ifndef DL_PORT_H
#define DL_PORT_H

#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#include "pico.h"
#define DL_HOT(f) __not_in_flash_func(f)
#else
#define DL_HOT(f) f
#endif

#endif
