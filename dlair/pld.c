/*
 * lair.pld parsing and the block decoder. See pld.h.
 */
#include "pld.h"

#include <string.h>

#include "dl_port.h"

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int pld_parse_header(pld_header_t *h, const uint8_t *b)
{
    memset(h, 0, sizeof(*h));
    h->magic = rd32(b + 0);
    h->version = rd16(b + 4);
    h->header_size = rd16(b + 6);
    h->width = rd16(b + 8);
    h->height = rd16(b + 10);
    h->fps_num = rd32(b + 12);
    h->fps_den = rd32(b + 16);
    h->audio_rate = rd32(b + 20);
    h->audio_format = b[24];
    h->block_size = b[25];
    h->flags = rd16(b + 26);
    h->first_frame = rd32(b + 28);
    h->frame_count = rd32(b + 32);
    h->key_offset = rd32(b + 36);
    h->key_count = rd32(b + 40);
    h->crc_offset = rd32(b + 44);
    h->data_offset = rd32(b + 48);
    h->max_record = rd32(b + 52);
    h->audio_offset = rd32(b + 56);
    h->audio_bytes = rd32(b + 60);
    memcpy(h->source, b + 64, sizeof(h->source));
    h->source[sizeof(h->source) - 1] = 0;
    memcpy(h->tool, b + 128, sizeof(h->tool));
    h->tool[sizeof(h->tool) - 1] = 0;

    if (h->magic == PLD_MAGIC_V1)
        return -1;
    return h->magic == PLD_MAGIC && h->version == PLD_VERSION && h->width == PLD_WIDTH &&
           h->height == PLD_HEIGHT && h->block_size == 4 && h->audio_format == 1 && h->frame_count > 0 &&
           h->key_count > 0 && h->data_offset >= PLD_HEADER_SIZE && h->audio_offset >= h->data_offset &&
           h->key_offset >= h->audio_offset + h->audio_bytes;
}

void pld_parse_key(pld_key_t *k, const uint8_t *b)
{
    k->frame = rd32(b);
    k->offset = rd32(b + 4);
    k->audio = rd32(b + 8);
}

void pld_parse_record(pld_record_t *r, const uint8_t *b)
{
    r->type = b[0];
    r->flags = b[1];
    r->audio_samples = rd16(b + 2);
    r->video_bytes = rd32(b + 4);
    r->record_bytes = rd32(b + 8);
    r->checksum = rd32(b + 12);
}

uint32_t DL_HOT(pld_adler32)(uint32_t adler, const uint8_t *p, uint32_t n)
{
    uint32_t a = adler & 0xffff, b = adler >> 16;
    while (n)
    {
        uint32_t k = n < 5552 ? n : 5552; /* largest run without overflow */
        n -= k;
        while (k--)
        {
            a += *p++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

int16_t pld_ulaw_decode(uint8_t u)
{
    u = (uint8_t)~u;
    int t = ((u & 0x0f) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    return (int16_t)((u & 0x80) ? (0x84 - t) : (t - 0x84));
}

/* 4 bits -> 4 bytes of 0x00/0xff, lowest bit in the lowest (leftmost) byte */
static const uint32_t expand4[16] = {
    0x00000000, 0x000000ff, 0x0000ff00, 0x0000ffff, 0x00ff0000, 0x00ff00ff, 0x00ffff00, 0x00ffffff,
    0xff000000, 0xff0000ff, 0xff00ff00, 0xff00ffff, 0xffff0000, 0xffff00ff, 0xffffff00, 0xffffffff,
};

#define STRIDE32 (PLD_WIDTH / 4)

int DL_HOT(pld_decode_row)(uint8_t *frame, int row, const uint8_t *src, uint32_t avail)
{
    uint32_t *base = (uint32_t *)(frame + row * 4 * PLD_WIDTH);
    const uint8_t *p = src;
    const uint8_t *end = src + avail;
    int bx = 0;

    while (bx < PLD_BLOCKS_X)
    {
        if (p >= end)
            return -1;
        const uint8_t op = *p++;
        int n;
        if (op < 0x40)
        {
            n = op + 1;
            if (bx + n > PLD_BLOCKS_X)
                return -1;
            bx += n;
            continue;
        }
        if (op < 0x80)
        {
            n = (op & 0x3f) + 1;
            if (bx + n > PLD_BLOCKS_X || p + 1 > end)
                return -1;
            const uint32_t v = *p++ * 0x01010101u;
            for (int i = 0; i < n; i++, bx++)
            {
                uint32_t *d = base + bx;
                d[0] = v;
                d[STRIDE32] = v;
                d[2 * STRIDE32] = v;
                d[3 * STRIDE32] = v;
            }
            continue;
        }

        n = (op & 0x1f) + 1;
        if (bx + n > PLD_BLOCKS_X)
            return -1;
        if (op < 0xa0)
        {
            if (p + n > end)
                return -1;
            for (int i = 0; i < n; i++, bx++)
            {
                const uint32_t v = *p++ * 0x01010101u;
                uint32_t *d = base + bx;
                d[0] = v;
                d[STRIDE32] = v;
                d[2 * STRIDE32] = v;
                d[3 * STRIDE32] = v;
            }
        }
        else if (op < 0xc0)
        {
            if (p + 4 * n > end)
                return -1;
            for (int i = 0; i < n; i++, bx++, p += 4)
            {
                const uint32_t v0 = p[0] * 0x01010101u, v1 = p[1] * 0x01010101u;
                const uint32_t m = (uint32_t)(p[2] | (p[3] << 8));
                uint32_t *d = base + bx;
                for (int y = 0; y < 4; y++)
                {
                    const uint32_t e = expand4[(m >> (4 * y)) & 15];
                    d[y * STRIDE32] = (v0 & ~e) | (v1 & e);
                }
            }
        }
        else if (op < 0xe0)
        {
            if (p + 8 * n > end)
                return -1;
            for (int i = 0; i < n; i++, bx++, p += 8)
            {
                const uint8_t *c = p;
                uint32_t m = rd32(p + 4);
                uint32_t *d = base + bx;
                for (int y = 0; y < 4; y++, m >>= 8)
                {
                    d[y * STRIDE32] = (uint32_t)c[m & 3] | ((uint32_t)c[(m >> 2) & 3] << 8) |
                                      ((uint32_t)c[(m >> 4) & 3] << 16) | ((uint32_t)c[(m >> 6) & 3] << 24);
                }
            }
        }
        else
        {
            if (p + 16 * n > end)
                return -1;
            for (int i = 0; i < n; i++, bx++, p += 16)
            {
                uint32_t *d = base + bx;
                d[0] = rd32(p);
                d[STRIDE32] = rd32(p + 4);
                d[2 * STRIDE32] = rd32(p + 8);
                d[3 * STRIDE32] = rd32(p + 12);
            }
        }
    }
    return (int)(p - src);
}
