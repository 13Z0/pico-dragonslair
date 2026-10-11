/*
 * Reading MAME's laserdisc CHD. See ldp_chd.h.
 *
 * A laserdisc CHD (version 5, codec "avhu") stores one video field per hunk:
 * 720 x 262 pixels of YUY2 (the VBI lines included), the field's stereo audio
 * at 44.1 kHz, and in its metadata the VBI data of every field, among them the
 * picture numbers of a CAV disc. A film-based disc such as Dragon's Lair spans
 * each picture over two or three fields (2:3 pulldown); the picture number is
 * in the first of them.
 *
 * The hunk format follows MAME's src/lib/util/avhuff.cpp and chd.cpp
 * (BSD-3-Clause, Aaron Giles); the Huffman, bitstream and FLAC decoders are
 * libchdr's (BSD-3-Clause), with dr_flac (public domain / MIT-0).
 */
#include "ldp_chd.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ldp_plat.h"
#include "libchdr/bitstream.h"
#include "libchdr/flac.h"
#include "libchdr/huffman.h"

/* lines of a field that hold the picture; the lines before are the VBI */
#define FIELD_FIRST_LINE 22
#define OUT_W LDP_W
#define OUT_H LDP_H

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t be24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be48(const uint8_t *p)
{
    return ((uint64_t)be16(p) << 32) | be32(p + 2);
}

static uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

/* CRC-16/CCITT as in chd.cpp */
static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xffff;
    while (n--)
    {
        crc ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* --- the CHD file -------------------------------------------------------- */

enum
{
    COMP_TYPE_0 = 0,
    COMP_TYPE_3 = 3,
    COMP_NONE = 4,
    COMP_SELF = 5,
    COMP_PARENT = 6,
    COMP_RLE_SMALL,
    COMP_RLE_LARGE,
    COMP_SELF_0,
    COMP_SELF_1,
    COMP_PARENT_SELF,
    COMP_PARENT_0,
    COMP_PARENT_1,
};

typedef struct
{
    uint8_t type;
    uint32_t length;
    uint64_t offset;
    uint16_t crc;
} map_entry_t;

typedef struct
{
    FILE *f;
    uint64_t file_size;
    uint32_t hunk_bytes;
    uint32_t unit_bytes;
    uint32_t hunk_count;
    uint32_t codec[4];
    map_entry_t *map;
    /* AVAV metadata */
    uint32_t width, height, channels, samplerate;
    double fps;
    /* AVLD metadata: VBI per field */
    uint8_t *vbi;
    uint32_t vbi_count;
    int has_parent;
    /* errors: fail() jumps back to the public call that set jb */
    jmp_buf jb;
    char err[200];
} chd_t;

static void fail(chd_t *c, const char *msg)
{
    snprintf(c->err, sizeof(c->err), "%s", msg);
    longjmp(c->jb, 1);
}

static void read_at(chd_t *c, uint64_t ofs, void *dst, size_t n)
{
    if (ldp_fseek64(c->f, (int64_t)ofs) != 0 || fread(dst, 1, n, c->f) != n)
        fail(c, "cannot read the CHD file");
}

static void read_map(chd_t *c, uint64_t map_offset)
{
    uint8_t raw[16];
    read_at(c, map_offset, raw, 16);
    const uint32_t mapbytes = be32(raw);
    const uint64_t firstoffs = be48(raw + 4);
    const uint16_t mapcrc = be16(raw + 10);
    const uint8_t lengthbits = raw[12], selfbits = raw[13], parentbits = raw[14];

    uint8_t *comp = malloc(mapbytes);
    uint8_t *rawmap = malloc((size_t)c->hunk_count * 12);
    c->map = calloc(c->hunk_count, sizeof(map_entry_t));
    if (!comp || !rawmap || !c->map)
        fail(c, "out of memory");
    read_at(c, map_offset + 16, comp, mapbytes);
    struct bitstream *bits = create_bitstream(comp, mapbytes);

    /* the compression types, Huffman- and RLE-coded */
    struct huffman_decoder *dec = create_huffman_decoder(16, 8);
    if (huffman_import_tree_rle(dec, bits) != HUFFERR_NONE)
        fail(c, "the CHD's hunk map is damaged");
    int repcount = 0;
    uint8_t lastcomp = 0;
    for (uint32_t h = 0; h < c->hunk_count; h++)
    {
        if (repcount > 0)
        {
            rawmap[h * 12] = lastcomp;
            repcount--;
            continue;
        }
        uint8_t val = (uint8_t)huffman_decode_one(dec, bits);
        if (val == COMP_RLE_SMALL)
        {
            rawmap[h * 12] = lastcomp;
            repcount = 2 + (int)huffman_decode_one(dec, bits);
        }
        else if (val == COMP_RLE_LARGE)
        {
            rawmap[h * 12] = lastcomp;
            repcount = 2 + 16 + ((int)huffman_decode_one(dec, bits) << 4);
            repcount += (int)huffman_decode_one(dec, bits);
        }
        else
        {
            rawmap[h * 12] = lastcomp = val;
        }
    }

    /* then lengths, offsets and CRCs */
    uint64_t cur = firstoffs;
    uint32_t last_self = 0;
    uint64_t last_parent = 0;
    for (uint32_t h = 0; h < c->hunk_count; h++)
    {
        uint8_t *e = rawmap + h * 12;
        uint64_t offset = cur;
        uint32_t length = 0;
        uint16_t crc = 0;
        switch (e[0])
        {
        case 0:
        case 1:
        case 2:
        case 3:
            cur += length = bitstream_read(bits, lengthbits);
            crc = (uint16_t)bitstream_read(bits, 16);
            break;
        case COMP_NONE:
            cur += length = c->hunk_bytes;
            crc = (uint16_t)bitstream_read(bits, 16);
            break;
        case COMP_SELF:
            last_self = (uint32_t)(offset = bitstream_read(bits, selfbits));
            break;
        case COMP_PARENT:
            offset = bitstream_read(bits, parentbits);
            last_parent = offset;
            break;
        case COMP_SELF_1:
            last_self++;
            /* fall through */
        case COMP_SELF_0:
            e[0] = COMP_SELF;
            offset = last_self;
            break;
        case COMP_PARENT_SELF:
            e[0] = COMP_PARENT;
            last_parent = offset = ((uint64_t)h * c->hunk_bytes) / c->unit_bytes;
            break;
        case COMP_PARENT_1:
            last_parent += c->hunk_bytes / c->unit_bytes;
            /* fall through */
        case COMP_PARENT_0:
            e[0] = COMP_PARENT;
            offset = last_parent;
            break;
        }
        e[1] = (uint8_t)(length >> 16);
        e[2] = (uint8_t)(length >> 8);
        e[3] = (uint8_t)length;
        for (int i = 0; i < 6; i++)
            e[4 + i] = (uint8_t)(offset >> (40 - 8 * i));
        e[10] = (uint8_t)(crc >> 8);
        e[11] = (uint8_t)crc;
        c->map[h].type = e[0];
        c->map[h].length = length;
        c->map[h].offset = offset;
        c->map[h].crc = crc;
    }
    if (crc16(rawmap, (size_t)c->hunk_count * 12) != mapcrc)
        fail(c, "the CHD's hunk map is damaged (CRC)");
    delete_huffman_decoder(dec);
    free(bits);
    free(comp);
    free(rawmap);
}

static void read_metadata(chd_t *c, uint64_t ofs)
{
    while (ofs)
    {
        uint8_t mh[16];
        read_at(c, ofs, mh, 16);
        const uint32_t len = be24(mh + 5);
        const uint64_t next = be64(mh + 8);
        if (!memcmp(mh, "AVAV", 4))
        {
            char s[256] = {0};
            read_at(c, ofs + 16, s, len < 255 ? len : 255);
            int interlaced = 0;
            if (sscanf(s, "FPS:%lf WIDTH:%u HEIGHT:%u INTERLACED:%d CHANNELS:%u SAMPLERATE:%u", &c->fps, &c->width,
                       &c->height, &interlaced, &c->channels, &c->samplerate) != 6)
                fail(c, "the CHD's laserdisc metadata is damaged");
        }
        else if (!memcmp(mh, "AVLD", 4))
        {
            c->vbi = malloc(len);
            if (!c->vbi)
                fail(c, "out of memory");
            read_at(c, ofs + 16, c->vbi, len);
            c->vbi_count = len / 16;
        }
        ofs = next;
    }
}

static void chd_open(chd_t *c, const char *path)
{
    c->f = ldp_fopen(path, "rb");
    if (!c->f)
        fail(c, "cannot open the file");
    c->file_size = (uint64_t)ldp_fsize64(c->f);
    uint8_t h[124];
    read_at(c, 0, h, sizeof(h));
    if (memcmp(h, "MComprHD", 8) || be32(h + 12) != 5)
        fail(c, "not a CHD file of version 5");
    for (int i = 0; i < 4; i++)
        c->codec[i] = be32(h + 16 + 4 * i);
    const uint64_t logical = be64(h + 32);
    const uint64_t map_offset = be64(h + 40);
    const uint64_t meta_offset = be64(h + 48);
    c->hunk_bytes = be32(h + 56);
    c->unit_bytes = be32(h + 60);
    c->hunk_count = (uint32_t)((logical + c->hunk_bytes - 1) / c->hunk_bytes);
    c->has_parent = memcmp(h + 104, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 20) != 0;
    if (c->codec[0] != 0x61766875 /* avhu */)
        fail(c, "not a laserdisc CHD (a laserdisc image is compressed with 'avhu')");
    read_map(c, map_offset);
    read_metadata(c, meta_offset);
    if (!c->width || !c->vbi)
        fail(c, "the CHD has no laserdisc metadata");
}

/* --- avhuff ------------------------------------------------------------- */

typedef struct
{
    struct huffman_decoder *dec;
    int rlecount;
    uint8_t prev;
} deltarle_t;

static int code_to_rlecount(int code)
{
    if (code == 0x00)
        return 1;
    if (code <= 0x107)
        return 8 + (code - 0x100);
    return 16 << (code - 0x108);
}

static inline uint8_t deltarle_decode(deltarle_t *d, struct bitstream *b)
{
    if (d->rlecount)
    {
        d->rlecount--;
        return d->prev;
    }
    int data = (int)huffman_decode_one(d->dec, b);
    if (data < 0x100)
    {
        d->prev = (uint8_t)(d->prev + data);
        return d->prev;
    }
    d->rlecount = code_to_rlecount(data) - 1;
    return d->prev;
}

typedef struct
{
    deltarle_t y, cb, cr;
    struct huffman_decoder *audio_hi, *audio_lo;
    flac_decoder flac;
} avhuff_t;

static int avhuff_init(avhuff_t *a)
{
    memset(a, 0, sizeof(*a));
    a->y.dec = create_huffman_decoder(256 + 16, 16);
    a->cb.dec = create_huffman_decoder(256 + 16, 16);
    a->cr.dec = create_huffman_decoder(256 + 16, 16);
    a->audio_hi = create_huffman_decoder(256, 16);
    a->audio_lo = create_huffman_decoder(256, 16);
    if (!a->y.dec || !a->cb.dec || !a->cr.dec || !a->audio_hi || !a->audio_lo)
        return -1;
    return flac_decoder_init(&a->flac) ? -1 : 0;
}

/* Decodes a hunk into MAME's raw "chav" layout (big-endian), so that the
 * map's CRC can be checked. want_video = 0 leaves the video out (and the CRC
 * unchecked). Returns 0 on success. */
static int avhuff_decode(avhuff_t *a, const uint8_t *src, uint32_t len, uint8_t *dest, int want_video)
{
    if (len < 8)
        return -1;
    const uint32_t metasize = src[0], channels = src[1];
    const uint32_t samples = be16(src + 2), width = be16(src + 4), height = be16(src + 6);
    if (channels > 16 || len < 10 + 2 * channels)
        return -1;
    const uint16_t treesize = be16(src + 8);
    uint32_t srcoffs = 10 + 2 * channels;

    memcpy(dest, "chav", 4);
    dest[4] = (uint8_t)metasize;
    dest[5] = (uint8_t)channels;
    dest[6] = (uint8_t)(samples >> 8);
    dest[7] = (uint8_t)samples;
    dest[8] = (uint8_t)(width >> 8);
    dest[9] = (uint8_t)width;
    dest[10] = (uint8_t)(height >> 8);
    dest[11] = (uint8_t)height;
    uint8_t *out = dest + 12;
    memcpy(out, src + srcoffs, metasize);
    out += metasize;
    srcoffs += metasize;

    /* audio */
    const uint8_t *asrc = src + srcoffs;
    if (treesize == 0xffff)
    {
        for (uint32_t ch = 0; ch < channels; ch++)
        {
            const uint16_t size = be16(src + 10 + 2 * ch);
            if (!flac_decoder_reset(&a->flac, 48000, 1, samples, asrc, size))
                return -1;
            /* big-endian into the chav buffer */
            if (!flac_decoder_decode_interleaved(&a->flac, (int16_t *)out, samples, flac_decoder_detect_native_endian() ? 1 : 0))
                return -1;
            flac_decoder_finish(&a->flac);
            out += 2 * samples;
            asrc += size;
        }
    }
    else
    {
        if (treesize)
        {
            struct bitstream *b = create_bitstream(asrc, treesize);
            if (huffman_import_tree_rle(a->audio_hi, b) != HUFFERR_NONE)
                return -1;
            bitstream_flush(b);
            if (huffman_import_tree_rle(a->audio_lo, b) != HUFFERR_NONE)
                return -1;
            free(b);
            asrc += treesize;
        }
        for (uint32_t ch = 0; ch < channels; ch++)
        {
            const uint16_t size = be16(src + 10 + 2 * ch);
            int16_t prev = 0;
            struct bitstream *b = treesize ? create_bitstream(asrc, size) : NULL;
            for (uint32_t s = 0; s < samples; s++)
            {
                int16_t delta;
                if (!treesize)
                    delta = (int16_t)be16(asrc + 2 * s);
                else
                    delta = (int16_t)((huffman_decode_one(a->audio_hi, b) << 8) | huffman_decode_one(a->audio_lo, b));
                prev = (int16_t)(prev + delta);
                out[0] = (uint8_t)((uint16_t)prev >> 8);
                out[1] = (uint8_t)prev;
                out += 2;
            }
            free(b);
            asrc += size;
        }
    }
    srcoffs = (uint32_t)(asrc - src);

    /* video */
    if (!want_video || !width || !height)
        return 0;
    const uint8_t *vsrc = src + srcoffs;
    const uint32_t vlen = len - srcoffs;
    if (!(vsrc[0] & 0x80))
        return -1; /* only the lossless mode exists */
    struct bitstream *b = create_bitstream(vsrc, vlen);
    bitstream_read(b, 8);
    if (huffman_import_tree_rle(a->y.dec, b) != HUFFERR_NONE)
        return -1;
    bitstream_flush(b);
    if (huffman_import_tree_rle(a->cb.dec, b) != HUFFERR_NONE)
        return -1;
    bitstream_flush(b);
    if (huffman_import_tree_rle(a->cr.dec, b) != HUFFERR_NONE)
        return -1;
    bitstream_flush(b);
    a->y.rlecount = a->cb.rlecount = a->cr.rlecount = 0;
    a->y.prev = a->cb.prev = a->cr.prev = 0;
    for (uint32_t y = 0; y < height; y++)
    {
        uint8_t *row = out + (size_t)y * width * 2;
        for (uint32_t x = 0; x < width / 2; x++)
        {
            row[0] = deltarle_decode(&a->y, b);
            row[1] = deltarle_decode(&a->cb, b);
            row[2] = deltarle_decode(&a->y, b);
            row[3] = deltarle_decode(&a->cr, b);
            row += 4;
        }
        a->y.rlecount = a->cb.rlecount = a->cr.rlecount = 0;
    }
    const int bad = bitstream_overflow(b) || bitstream_flush(b) != vlen;
    free(b);
    return bad ? -1 : 0;
}

/* --- hunks --------------------------------------------------------------- */

typedef struct
{
    uint8_t *comp;
    uint8_t *raw; /* chav */
    avhuff_t av;
    uint32_t crc_errors;
} reader_t;

/* Decodes hunk h into r->raw. Returns 0 on success. */
static int read_hunk(chd_t *c, reader_t *r, uint32_t h, int want_video)
{
    for (int depth = 0; depth < 4; depth++)
    {
        const map_entry_t *e = &c->map[h];
        switch (e->type)
        {
        case 0:
        case 1:
        case 2:
        case 3:
            if (e->length > c->hunk_bytes * 2u)
                return -1;
            read_at(c, e->offset, r->comp, e->length);
            memset(r->raw, 0, c->hunk_bytes); /* the CRC covers the zero padding as well */
            if (avhuff_decode(&r->av, r->comp, e->length, r->raw, want_video))
                return -1;
            if (want_video && crc16(r->raw, c->hunk_bytes) != e->crc)
                r->crc_errors++;
            return 0;
        case COMP_NONE:
            read_at(c, e->offset, r->raw, c->hunk_bytes);
            return 0;
        case COMP_SELF:
            h = (uint32_t)e->offset;
            continue;
        default:
            return -1; /* parent hunks */
        }
    }
    return -1;
}

/* chav layout: 12 header, meta, channels x samples x 2, then YUY2 video */
static void chav_parts(const uint8_t *raw, uint32_t *samples, uint32_t *channels, const uint8_t **audio,
                       const uint8_t **video, uint32_t *w, uint32_t *h)
{
    const uint32_t meta = raw[4];
    *channels = raw[5];
    *samples = be16(raw + 6);
    *w = be16(raw + 8);
    *h = be16(raw + 10);
    *audio = raw + 12 + meta;
    *video = *audio + 2 * *samples * *channels;
}

/* --- VBI ----------------------------------------------------------------- */

/* the picture number of a field, or -1 (vbiparse: line 17/18 code 0xF8xxxx) */
static int field_picture(const chd_t *c, uint32_t field)
{
    if (field >= c->vbi_count)
        return -1;
    const uint8_t *e = c->vbi + 16 * field;
    const uint32_t codes[3] = {be24(e + 13), be24(e + 7), be24(e + 10)}; /* line 17/18, 17, 18 */
    for (int i = 0; i < 3; i++)
    {
        const uint32_t v = codes[i];
        if ((v & 0xf00000) == 0xf00000)
        {
            const int d4 = (v >> 16) & 0x07, d3 = (v >> 12) & 0x0f, d2 = (v >> 8) & 0x0f, d1 = (v >> 4) & 0x0f,
                      d0 = v & 0x0f;
            if (d3 > 9 || d2 > 9 || d1 > 9 || d0 > 9)
                continue;
            return d4 * 10000 + d3 * 1000 + d2 * 100 + d1 * 10 + d0;
        }
    }
    return -1;
}

typedef struct
{
    int32_t first_field; /* -1: not on the disc */
    uint8_t nfields;
} picture_t;

struct ldp_chd
{
    chd_t c;
    reader_t r;
    picture_t *pictures;
    int pic_lo, pic_hi;
    int32_t line[3 * 1024];
};

static void index_pictures(ldp_chd *d)
{
    chd_t *c = &d->c;
    int pic_lo = 1 << 30, pic_hi = -1;
    for (uint32_t f = 0; f < c->vbi_count; f++)
    {
        int p = field_picture(c, f);
        if (p < 0)
            continue;
        if (p < pic_lo)
            pic_lo = p;
        if (p > pic_hi)
            pic_hi = p;
    }
    if (pic_hi < 0)
        fail(c, "the CHD has no picture numbers: not a CAV laserdisc");
    d->pic_lo = pic_lo;
    d->pic_hi = pic_hi;
    picture_t *pictures = malloc(sizeof(picture_t) * (size_t)(pic_hi - pic_lo + 1));
    if (!pictures)
        fail(c, "out of memory");
    d->pictures = pictures;
    for (int p = pic_lo; p <= pic_hi; p++)
        pictures[p - pic_lo].first_field = -1;
    for (uint32_t f = 0; f < c->vbi_count; f++)
    {
        int p = field_picture(c, f);
        if (p < 0)
            continue;
        picture_t *pi = &pictures[p - pic_lo];
        if (pi->first_field >= 0)
            continue; /* the first occurrence counts */
        uint32_t n = 1;
        while (f + n < c->vbi_count && n < 4 && field_picture(c, f + n) < 0)
            n++;
        pi->first_field = (int32_t)f;
        pi->nfields = (uint8_t)n;
    }
}

/* --- picture conversion --------------------------------------------------- */

static inline uint8_t clamp8(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* YUY2 field (BT.601, studio range) to 320x240 RGB, area-averaged */
static void field_to_rgb(int32_t *line, const uint8_t *yuy2, uint32_t w, uint32_t h, uint8_t *rgb)
{
    if (w > 1024)
        w = 1024;
    for (int oy = 0; oy < OUT_H; oy++)
    {
        const uint32_t sy = FIELD_FIRST_LINE + (uint32_t)oy;
        const uint8_t *s = yuy2 + (size_t)(sy < h ? sy : h - 1) * w * 2;
        for (uint32_t x = 0; x < w; x += 2)
        {
            const int y0 = s[0] - 16, u = s[1] - 128, y1 = s[2] - 16, v = s[3] - 128;
            s += 4;
            const int yy[2] = {y0, y1};
            for (int k = 0; k < 2; k++)
            {
                const int c = 298 * yy[k];
                line[3 * (x + k) + 0] = clamp8((c + 409 * v + 128) >> 8);
                line[3 * (x + k) + 1] = clamp8((c - 100 * u - 208 * v + 128) >> 8);
                line[3 * (x + k) + 2] = clamp8((c + 516 * u + 128) >> 8);
            }
        }
        /* each output pixel covers w/320 source pixels (2.25 for 720) */
        uint8_t *o = rgb + (size_t)oy * OUT_W * 3;
        for (int ox = 0; ox < OUT_W; ox++)
        {
            const double x0 = (double)ox * w / OUT_W, x1 = (double)(ox + 1) * w / OUT_W;
            double acc[3] = {0, 0, 0}, wsum = 0;
            for (int sx = (int)x0; sx < (int)x1 + 1 && sx < (int)w; sx++)
            {
                double a = (sx + 1 < x1 ? sx + 1 : x1) - (sx > x0 ? sx : x0);
                if (a <= 0)
                    continue;
                for (int ch = 0; ch < 3; ch++)
                    acc[ch] += a * line[3 * sx + ch];
                wsum += a;
            }
            for (int ch = 0; ch < 3; ch++)
                o[3 * ox + ch] = clamp8((int)(acc[ch] / wsum + 0.5));
        }
    }
}


/* --- the interface -------------------------------------------------------- */

ldp_chd *ldp_chd_open(const char *path, char *err, size_t errlen)
{
    ldp_chd *volatile d = (ldp_chd *)calloc(1, sizeof(ldp_chd));
    if (!d)
    {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    chd_t *c = &d->c;
    if (setjmp(c->jb))
    {
        snprintf(err, errlen, "%s", c->err);
        ldp_chd_close(d);
        return NULL;
    }
    chd_open(c, path);
    index_pictures(d);
    d->r.comp = malloc(c->hunk_bytes * 2u);
    d->r.raw = malloc(c->hunk_bytes);
    if (!d->r.comp || !d->r.raw || avhuff_init(&d->r.av))
        fail(c, "out of memory");
    return d;
}

void ldp_chd_close(ldp_chd *d)
{
    if (!d)
        return;
    if (d->c.f)
        fclose(d->c.f);
    free(d->c.map);
    free(d->c.vbi);
    free(d->pictures);
    free(d->r.comp);
    free(d->r.raw);
    if (d->r.av.y.dec)
    {
        delete_huffman_decoder(d->r.av.y.dec);
        delete_huffman_decoder(d->r.av.cb.dec);
        delete_huffman_decoder(d->r.av.cr.dec);
        delete_huffman_decoder(d->r.av.audio_hi);
        delete_huffman_decoder(d->r.av.audio_lo);
        flac_decoder_free(&d->r.av.flac);
    }
    free(d);
}

int ldp_chd_first(const ldp_chd *d)
{
    return d->pic_lo;
}

int ldp_chd_last(const ldp_chd *d)
{
    return d->pic_hi;
}

void ldp_chd_info(const ldp_chd *d, char *out, size_t outlen)
{
    const chd_t *c = &d->c;
    uint32_t missing = 0;
    for (int p = d->pic_lo; p <= d->pic_hi; p++)
        missing += d->pictures[p - d->pic_lo].first_field < 0;
    snprintf(out, outlen,
             "%u fields of %ux%u at %.3f per second, sound %u channels at %u Hz; pictures %d-%d, %u not on "
             "the disc%s",
             c->hunk_count, c->width, c->height, c->fps, c->channels, c->samplerate, d->pic_lo, d->pic_hi, missing,
             c->has_parent ? "; it names a parent CHD, which is not read" : "");
}

uint32_t ldp_chd_crc_errors(const ldp_chd *d)
{
    return d->r.crc_errors;
}

int ldp_chd_picture(ldp_chd *d, int p, uint8_t *rgb, int16_t *audio, int *nsamples, char *err, size_t errlen)
{
    chd_t *c = &d->c;
    *nsamples = 0;
    if (p < d->pic_lo || p > d->pic_hi)
        return 1;
    const picture_t *pi = &d->pictures[p - d->pic_lo];
    if (pi->first_field < 0)
        return 1;
    if (setjmp(c->jb))
    {
        snprintf(err, errlen, "%s", c->err);
        return -1;
    }
    /* the video: the field of the picture with even parity, so that
     * consecutive pictures do not bob up and down by a line */
    uint32_t vfield = (uint32_t)pi->first_field;
    if ((vfield & 1) && pi->nfields > 1)
        vfield++;
    int n = 0;
    for (uint32_t k = 0; k < pi->nfields; k++)
    {
        const uint32_t fld = (uint32_t)pi->first_field + k;
        if (read_hunk(c, &d->r, fld, fld == vfield))
        {
            snprintf(err, errlen, "cannot decode field %u of picture %d", fld, p);
            return -1;
        }
        uint32_t samples, channels, w, h;
        const uint8_t *a, *video;
        chav_parts(d->r.raw, &samples, &channels, &a, &video, &w, &h);
        for (uint32_t s = 0; s < samples && n < LDP_CHD_MAX_SAMPLES; s++, n++)
        {
            audio[2 * n] = (int16_t)be16(a + 2 * s);
            audio[2 * n + 1] = (int16_t)(channels > 1 ? be16(a + 2 * samples + 2 * s) : be16(a + 2 * s));
        }
        if (fld == vfield)
            field_to_rgb(d->line, video, w, h, rgb);
    }
    *nsamples = n;
    return 0;
}
