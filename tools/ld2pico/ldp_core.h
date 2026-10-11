/*
 * ld2pico core: converts MAME's Dragon's Lair laserdisc image (dlair.chd)
 * into lair.pld, the laserdisc file picoDragonsLair plays from the SD card
 * (format described in dlair/pld.h).
 *
 * Each picture of the disc becomes 256 palette colours, coded in 4x4 blocks
 * against the previous one; the sound becomes one track of 8-bit u-law at
 * 22050 Hz. Pictures are grouped into segments that end at scene cuts; each
 * segment has its own palette, starts with a key record, and is coded on a
 * thread of its own. ld2pico.exe (ldp_gui.c) and the command line
 * (ldp_cli.c) are front ends to ldp_convert().
 */
#ifndef LDP_CORE_H
#define LDP_CORE_H

#include <stddef.h>

typedef struct
{
    double quality;     /* allowed squared error per colour channel of a block (60) */
    double denoise_lo;  /* temporal filter: changes below this are dropped (10) */
    double denoise_hi;  /* and above this are kept as they are (28); hi <= lo: off */
    int keyint;         /* the most pictures between key records (32) */
    double max_rate_kb; /* the most KB of pictures per second of play (420) */
    double audio_gain;  /* 0: bring the sound to full level */
    int threads;        /* 0: one per processor, up to 12 */
    int start, count;   /* a part of the disc for trials; start < 0: all of it */
} ldp_settings;

void ldp_default_settings(ldp_settings *s);

typedef struct
{
    void *ctx;
    void (*log)(void *ctx, const char *line);
    void (*progress)(void *ctx, int done, int total); /* pictures written */
    int (*cancelled)(void *ctx);
} ldp_hooks;

/* Converts chd into pld. The file is written as pld + ".part" and renamed at
 * the end, so a cancelled or failed conversion leaves no lair.pld behind.
 * Returns 0 on success, 1 on an error (logged), 2 when cancelled. */
int ldp_convert(const char *chd, const char *pld, const ldp_settings *s, const ldp_hooks *h);

/* Writes one picture of the disc as a 320x240 PPM, as the converter sees it.
 * Returns 0, or -1 with the reason in err. */
int ldp_dump_picture(const char *chd, int picture, const char *ppm, char *err, size_t errlen);

/* One line about the CHD: fields, picture range. Returns 0 or -1 (err). */
int ldp_describe(const char *chd, char *out, size_t outlen);

#endif
