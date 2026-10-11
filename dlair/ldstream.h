/*
 * Streaming decoder for lair.pld: reads the picture records into a buffer,
 * decodes them into one 320x240 index buffer, and reads the sound track into
 * a second buffer of its own.
 *
 * Everything is done in small steps (ldstream_step), each either one read of
 * the file or the decoding of a few block rows, so the caller can interleave
 * the work with the emulation and stop when the next video frame is due. On
 * the board the reads go straight to the SD card (polled SPI), on the host to
 * stdio.
 *
 * Pictures: there is a single picture buffer, so the next picture is decoded
 * only after the current one has been shown (copied to the screen). While a
 * picture waits for its turn the decoder reads ahead, filling the read buffer
 * with the records that follow, so that a burst of large pictures finds most
 * of its data already there. Seeking looks up the key record at or before
 * the target in the key table on the card, and decodes the records from there
 * up to the target without showing them.
 *
 * Sound: the sound track is read through a second handle on the file, into
 * its own buffer, ahead of the sound cursor. It does not wait for the
 * pictures: when a picture is late, the sound plays on.
 */
#ifndef LDSTREAM_H
#define LDSTREAM_H

#include <stdbool.h>
#include <stdint.h>

#include "pld.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LDS_NONE 0xffffffffu

/* keys per entry of the key index kept in memory */
#define LDS_KEY_GROUP 16

typedef struct
{
    /* Reads len bytes at file offset ofs into dst. ofs and len are multiples
     * of 512; only the last read of the file may come back short. Returns the
     * number of bytes read, or -1. ctx is the handle for the pictures, actx a
     * second handle on the same file for the sound (it may be the same one
     * when seeking is free, as on the host). */
    int (*read)(void *ctx, uint32_t ofs, void *dst, uint32_t len);
    void *ctx;
    void *actx;
} ldstream_io_t;

typedef struct ldstream
{
    ldstream_io_t io;
    pld_header_t hdr;
    uint32_t file_size;
    uint32_t *key_index; /* picture number of every LDS_KEY_GROUP-th key */
    uint32_t key_groups;

    /* picture records, a ring: file bytes [ring_start, ring_end), the byte at
     * file offset x in buf[x % buf_size] */
    uint8_t *buf;
    uint32_t buf_size;
    bool buf_owned;
    uint32_t ring_start, ring_end;
    uint32_t read_chunk; /* the most bytes one step reads */
    uint8_t scratch[2048]; /* data that wraps around the ring's end; small reads */

    /* the picture */
    uint8_t *frame;         /* PLD_WIDTH x PLD_HEIGHT indices */
    uint16_t palette[256];  /* RGB555 */
    uint32_t palette_serial;
    uint32_t buf_frame;     /* picture complete in frame[], LDS_NONE when none or partial */
    uint32_t pic_serial;    /* changes whenever frame[] gets a new picture */
    bool pic_presentable;   /* buf_frame was decoded for display, not on the way to a target */
    bool pic_shown;

    /* the record being decoded */
    int state;
    uint32_t pos;       /* file offset of the next byte to parse */
    uint32_t rec_frame; /* picture number of the record at or after pos */
    uint32_t rec_audio; /* index of the first sound sample of rec_frame */
    uint32_t rec_start;
    pld_record_t rec;
    uint32_t video_left;
    int row;
    uint32_t sum;
    bool silent; /* the record is decoded on the way to a target, not shown */

    /* where decoding is heading */
    uint32_t target;  /* seek target, LDS_NONE while playing on */
    bool catchup;     /* the target was set to catch up with play, not by a search */
    uint32_t key_for; /* a key lookup is due for this frame (state ST_KEYS) */

    /* the sound start of the last decoded pictures */
    struct
    {
        uint32_t frame, audio;
    } hist[4];

    /* the sound track: file bytes [abuf_ofs, abuf_ofs + abuf_len) */
    uint8_t *abuf;
    uint32_t abuf_size;
    uint32_t abuf_ofs, abuf_len;
    int16_t ulaw[256];

    /* the sound cursor: sample index and Q16 fraction, LDS_NONE when stopped */
    uint32_t a_pos;
    uint32_t a_frac;
    uint32_t a_step;       /* Q16 sound samples per output sample */
    uint32_t a_wait_frame; /* the cursor starts when this picture's sound is known */
    uint32_t a_prep;       /* where the sound will start: read it in advance */

    /* statistics */
    uint32_t bytes_read;
    uint32_t reads;
    uint32_t audio_reads;
    uint32_t audio_gaps;
    uint32_t frames_decoded;
    uint32_t checksum_errors;
    uint32_t format_errors;
    uint32_t seeks;
} ldstream_t;

/* Opens the file and reads its key table into a small index. buf is the
 * picture read buffer of buf_size bytes (at least 16 KB, a multiple of 512;
 * allocated here when NULL), abuf_size the sound buffer (at least 4 KB), frame
 * the 76.8 KB picture buffer (4-byte aligned). The larger the read buffer,
 * the further ahead the pictures are read: in PSRAM it can hold seconds of
 * them.
 * samples_per_field sets the sound resampling: that many output samples are
 * rendered per emulated field of 1/59.94 s. Returns 0 on success, or an error
 * code: 1 read failed, 2 not a lair.pld file, 3 out of memory, 4 a lair.pld
 * of the first format (convert again). */
int ldstream_open(ldstream_t *s, const ldstream_io_t *io, uint32_t file_size, uint8_t *buf, uint32_t buf_size,
                  uint32_t abuf_size, uint8_t *frame, uint32_t samples_per_field);
void ldstream_close(ldstream_t *s);

uint32_t ldstream_first_frame(const ldstream_t *s);
uint32_t ldstream_last_frame(const ldstream_t *s);
bool ldstream_has_frame(const ldstream_t *s, uint32_t frame);

/* Head for `frame` and stop there (a search). Does not read: the work is
 * done by the following steps. */
void ldstream_seek(ldstream_t *s, uint32_t frame);
/* The searched frame is in the picture buffer. */
bool ldstream_seek_done(const ldstream_t *s, uint32_t frame);
/* Play on from `frame`: decode frame, frame+1, ... one ahead of the display. */
void ldstream_play(ldstream_t *s, uint32_t frame);

/* One unit of work. display_frame is the frame the player shows now, or
 * LDS_NONE; max_read the most bytes a read in this step may take (it is
 * rounded down to whole sectors; less than one sector means no read).
 * Returns 0 when there was nothing to do, 1 after a file read, 2 after
 * decoding. */
int ldstream_step(ldstream_t *s, uint32_t display_frame, bool playing, uint32_t max_read);

/* The picture buffer holds a frame to put on the screen for display_frame. */
bool ldstream_picture_due(const ldstream_t *s, uint32_t display_frame);
/* The caller copied the picture to the screen. */
void ldstream_picture_shown(ldstream_t *s);

/* Sound: start the cursor at the beginning of `frame` and render n output
 * samples (added to out with gain in Q8) per call; stop it. */
void ldstream_audio_start(ldstream_t *s, uint32_t frame);
void ldstream_audio_stop(ldstream_t *s);
void ldstream_audio_render(ldstream_t *s, int16_t *out, int n, int gain_q8);

#ifdef __cplusplus
}
#endif

#endif
