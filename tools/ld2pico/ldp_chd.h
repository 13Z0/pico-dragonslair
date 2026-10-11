/*
 * Reading MAME's laserdisc image (a CHD of version 5, codec "avhu") picture
 * by picture: for every picture number of the disc, one field of the
 * picture, cut to its 240 picture lines and scaled from 720 to 320 pixels by
 * area averaging, and the stereo sound of all the picture's fields.
 */
#ifndef LDP_CHD_H
#define LDP_CHD_H

#include <stddef.h>
#include <stdint.h>

#define LDP_W 320
#define LDP_H 240

/* sound sample pairs of one picture at most (three fields of 736) */
#define LDP_CHD_MAX_SAMPLES 4096

typedef struct ldp_chd ldp_chd;

/* NULL on failure, with the reason in err. */
ldp_chd *ldp_chd_open(const char *path, char *err, size_t errlen);
void ldp_chd_close(ldp_chd *d);

/* The range of picture numbers on the disc. */
int ldp_chd_first(const ldp_chd *d);
int ldp_chd_last(const ldp_chd *d);
void ldp_chd_info(const ldp_chd *d, char *out, size_t outlen);
uint32_t ldp_chd_crc_errors(const ldp_chd *d);

/* Picture p: rgb receives LDP_W x LDP_H x 3 bytes, audio up to
 * LDP_CHD_MAX_SAMPLES stereo pairs (*nsamples of them). Returns 0, 1 when the
 * picture is not on the disc, or -1 on an error (the reason in err). */
int ldp_chd_picture(ldp_chd *d, int p, uint8_t *rgb, int16_t *audio, int *nsamples, char *err, size_t errlen);

#endif
