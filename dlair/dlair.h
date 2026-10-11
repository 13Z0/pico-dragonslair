/*
 * Dragon's Lair (Cinematronics, 1983), US revision F2: the arcade board.
 *
 * A Z80 at 4 MHz with 2 KB of RAM, an AY-3-8910 for the beeps and the DIP
 * switches, a 16-digit LED scoreboard, and a Pioneer LD-V1000 laserdisc player
 * for all pictures and sound. The memory map and the input bits follow MAME's
 * cinematronics/dlair.cpp (BSD-3-Clause, Aaron Giles); the laserdisc strobe
 * timing, the DIP switch defaults and the scoreboard layout follow Hypseus'
 * game/lair.cpp (GPL-2.0-or-later, Matt Ownby and others).
 *
 * The machine runs one video field (1/59.94 s) per call. At the start of each
 * field the laserdisc reports a vertical sync, and 27.1 + 500 us later the
 * player raises its status strobe for 26 us, then 54 us later its command
 * strobe for 25 us. The game reads one status byte and writes one command byte
 * per field, timed by these strobes. The IRQ comes every 32.768 ms from a
 * divider of the master clock, independently of the video.
 */
#ifndef DLAIR_H
#define DLAIR_H

#include <stdbool.h>
#include <stdint.h>

#include "ay8910.h"
#include "ldplayer.h"
#include "ldv1000.h"
#include "z80.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DL_CPU_HZ 4000000
#define DL_AY_HZ (DL_CPU_HZ / 2)
#define DL_FIELD_MHZ 59940 /* fields per 1000 s */
#define DL_IRQ_CYCLES 131072

/* inputs, active high here (the board's inputs are active low) */
#define DL_IN_UP 0x0001
#define DL_IN_DOWN 0x0002
#define DL_IN_LEFT 0x0004
#define DL_IN_RIGHT 0x0008
#define DL_IN_SWORD 0x0010
#define DL_IN_START1 0x0100
#define DL_IN_START2 0x0200
#define DL_IN_COIN1 0x0400
#define DL_IN_COIN2 0x0800
#define DL_IN_SERVICE 0x1000 /* DIP switch A:8, the diagnostics */

/* DIP switches as the CPU reads them (Hypseus' defaults) */
#define DL_DSW1_DEFAULT 0x22 /* 2 coins = 1 credit, 5 lives, difficulty mode 2 */
#define DL_DSW2_DEFAULT 0xd8 /* LD-V1000 protocol (bit 3), easy, demo sounds */

/* scoreboard digit values: 0-9, 0x0a-0x0e letters, 0x0f blank */
#define DL_LED_BLANK 0x0f

typedef struct
{
    uint8_t maincpu[0x8000]; /* four 8 KB ROMs at 0x0000-0x7fff */
} dl_roms_t;

typedef struct
{
    /* the 16 digits as written: [0..7] at 0xe038-0xe03f, [8..15] at
     * 0xe030-0xe037. Hypseus' layout: 0-5 player 1 score, 6 player 1 lives,
     * 7 player 2 lives, 8-13 player 2 score, 14-15 credits. */
    uint8_t digit[16];
    uint32_t serial; /* changes when a digit changes */
} dl_leds_t;

/* the CPU state compared by the idle skipping, see dl_machine.c */
typedef struct
{
    uint16_t pc, sp, af, bc, de, hl, ix, iy, wz, alt[4];
    uint8_t flags[8];
} dl_idle_t;

typedef struct dl_machine
{
    z80_t cpu;
    uint8_t *rom; /* the caller's dl_roms_t, patched in place for fast boot */
    uint8_t ram[0x800];

    ay8910_t ay;
    uint8_t ay_latch;
    uint8_t dsw1, dsw2;

    ldplayer_t ldp;
    ldv1000_t ldv;
    uint8_t strobes; /* bit 6 status strobe, bit 7 command strobe */

    uint16_t inputs;
    dl_leds_t leds;

    /* timing */
    uint32_t field_cycles_frac; /* Q8 cycles carried between fields */
    uint32_t irq_wait;          /* cycles until the next IRQ */
    uint32_t seg_end;           /* where the running z80_run stops, in cycles into the field */
    uint32_t fields;

    /* idle skipping */
    uint8_t dirty;       /* RAM or the board changed since the last port read */
    uint16_t idle_pc;    /* the instruction of the last port read */
    dl_idle_t idle_state;
    uint8_t idle_r;      /* R at that read */
    uint32_t idle_t;     /* and the cycle in the field */
    uint32_t idle_cycles; /* CPU cycles skipped */

    /* AY output of the current field */
    int16_t *audio;
    int audio_n, audio_done;
    uint32_t audio_field_cycles;
    int ay_level;

    /* trace hook for the host harness, may be NULL */
    void (*trace)(void *ctx, const char *what, uint32_t value);
    void *trace_ctx;
} dl_machine_t;

/* samplerate is the rate of the AY output rendered by dl_run_field. The
 * machine runs the program straight from roms, which must stay valid. */
void dl_init(dl_machine_t *m, dl_roms_t *roms, ldstream_t *disc, bool fastboot, uint32_t samplerate);
void dl_reset(dl_machine_t *m);

/* Runs one field. The AY sound of the field is added to audio[0..n). */
void dl_run_field(dl_machine_t *m, uint16_t inputs, int16_t *audio, int n);

#ifdef __cplusplus
}
#endif

#endif
