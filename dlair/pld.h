/*
 * The laserdisc file, lair.pld ("Pico LaserDisc").
 *
 * ld2pico (tools/ld2pico) converts MAME's laserdisc image (dlair.chd) once into this
 * format, which a microcontroller can play from an SD card that is read over
 * SPI: about 8 KB per film frame on average instead of the 155 KB per field
 * of the CHD. All multi-byte values are little-endian.
 *
 *   header      512 bytes, pld_header_t
 *   records     one per laserdisc frame (picture number), in frame order,
 *               each 4-byte aligned: the pictures
 *   sound       the sound of the whole disc, u-law, mono, header.audio_rate;
 *               frame by frame in the same order, audio_samples bytes each
 *   key table   12 bytes per key record, in frame order: picture number,
 *               file offset of the record, and the index of its first sound
 *               sample
 *   CRC table   CRC32 of each decoded 8bpp frame (host checks only)
 *
 * The sound is kept apart from the pictures so that the player can read it
 * on its own, ahead of time: a picture that is late does not hold up the
 * sound.
 *
 * Record:
 *   pld_record_t   16 bytes
 *   palette        256 x RGB555 (512 bytes), key records only
 *   video          video_bytes of block opcodes (key and delta records)
 *   padding        to a multiple of 4
 *
 * Video is 320x240 pixels of 8-bit palette indices, coded as 4x4 blocks: 80
 * blocks per block row, 60 block rows. A delta record codes each block
 * against the previous frame of the stream. Opcodes, each followed by its
 * operands, never run past the end of a block row, so a frame can be decoded
 * one block row at a time as the data arrives:
 *
 *   0x00-0x3f  SKIP  n = op+1 blocks unchanged
 *   0x40-0x7f  RUN   n = (op&0x3f)+1 blocks of one colour: 1 byte
 *   0x80-0x9f  FILL  n = (op&0x1f)+1 blocks, a colour each: n bytes
 *   0xa0-0xbf  TWO   n = (op&0x1f)+1 blocks: c0 c1 mask16 (bit i set -> c1)
 *   0xc0-0xdf  FOUR  n = (op&0x1f)+1 blocks: c0 c1 c2 c3 mask32 (2 bits/pixel)
 *   0xe0-0xff  RAW   n = (op&0x1f)+1 blocks: 16 bytes each
 *
 * Pixel i of a block is row i/4, column i%4. Key records use no SKIP.
 */
#ifndef PLD_H
#define PLD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLD_MAGIC 0x32444c50u /* "PLD2" */
#define PLD_MAGIC_V1 0x31444c50u /* "PLD1": the first format, sound inside the records */
#define PLD_VERSION 2
#define PLD_HEADER_SIZE 512
#define PLD_KEY_ENTRY 12

#define PLD_WIDTH 320
#define PLD_HEIGHT 240
#define PLD_BLOCKS_X (PLD_WIDTH / 4)
#define PLD_BLOCKS_Y (PLD_HEIGHT / 4)

/* the most bytes one block row can take (80 RAW blocks and their opcodes) */
#define PLD_MAX_ROW_BYTES (PLD_BLOCKS_X * 16 + 3)

enum
{
    PLD_KEY = 0,     /* palette + complete picture */
    PLD_DELTA = 1,   /* picture coded against the previous frame */
    PLD_REPEAT = 2,  /* same picture as the previous frame */
    PLD_MISSING = 3, /* not on the disc image: previous picture, no sound */
};

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint16_t width, height;
    uint32_t fps_num, fps_den; /* film frame rate, 24000/1001 */
    uint32_t audio_rate;       /* 22050 */
    uint8_t audio_format;      /* 1 = u-law mono */
    uint8_t block_size;        /* 4 */
    uint16_t flags;
    uint32_t first_frame;      /* picture number of the first record */
    uint32_t frame_count;
    uint32_t key_offset, key_count;
    uint32_t crc_offset;       /* 0 when there is no CRC table */
    uint32_t data_offset;      /* first record, = header_size */
    uint32_t max_record;       /* largest record in bytes */
    uint32_t audio_offset;     /* the sound: file offset, also the end of the records */
    uint32_t audio_bytes;      /* and length (= samples) */
    char source[64];
    char tool[32];
} pld_header_t;

typedef struct
{
    uint32_t frame;  /* picture number */
    uint32_t offset; /* file offset of its record */
    uint32_t audio;  /* index of its first sound sample */
} pld_key_t;

typedef struct
{
    uint8_t type;
    uint8_t flags;
    uint16_t audio_samples; /* of this frame in the sound */
    uint32_t video_bytes;
    uint32_t record_bytes; /* the whole record, header and padding included */
    uint32_t checksum;     /* Adler-32 of the palette and the video */
} pld_record_t;

#define PLD_RECORD_HEADER 16

/* Parses the header from 512 bytes: 1 when it is a usable file, 0 when it
 * is not, -1 when it is a lair.pld of the first format (convert again). */
int pld_parse_header(pld_header_t *h, const uint8_t *buf);
void pld_parse_record(pld_record_t *r, const uint8_t *buf);
void pld_parse_key(pld_key_t *k, const uint8_t *buf);

/* Adler-32 (as zlib.adler32), continued across calls */
#define PLD_ADLER_INIT 1u
uint32_t pld_adler32(uint32_t adler, const uint8_t *p, uint32_t n);

/* Decodes one block row into frame (stride PLD_WIDTH). Returns the number of
 * bytes used, or -1 when the data is malformed or runs past `avail`. */
int pld_decode_row(uint8_t *frame, int row, const uint8_t *src, uint32_t avail);

/* u-law byte to 16-bit linear sample (ITU-T G.711) */
int16_t pld_ulaw_decode(uint8_t u);

#ifdef __cplusplus
}
#endif

#endif
