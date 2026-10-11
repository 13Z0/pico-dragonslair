/*
 * ld2pico core. See ldp_core.h, and dlair/pld.h for the file format.
 *
 * The steps, per segment of pictures between two scene cuts:
 *   - a temporal filter removes the film grain: a pixel that changes by less
 *     than denoise_lo keeps its value, one that changes by more than
 *     denoise_hi takes the new one, in between they are mixed;
 *   - a palette of 256 colours by median cut, refined by k-means;
 *   - every picture in 4x4 blocks against the previous one: a block is left
 *     unchanged, filled with one colour, or drawn with two or four colours,
 *     whichever is cheapest within the allowed error; otherwise its 16 pixels
 *     are stored as they are;
 *   - when a second of play would need more than max_rate_kb, the segment is
 *     coded again with a larger allowed error.
 * The sound of the whole disc is kept until the end, brought to a common
 * level and written as one track.
 */
#include "ldp_core.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ldp_chd.h"
#include "ldp_plat.h"

#define W LDP_W
#define H LDP_H
#define NPIX (W * H)
#define BX (W / 4)
#define BY (H / 4)

#define MAX_SEGMENT 240  /* pictures; also bounds the memory per segment */
#define CUT_THRESHOLD 24 /* mean grey difference of a scene cut */
#define MAX_THREADS 12

enum
{
    PLD_KEY = 0,
    PLD_DELTA = 1,
    PLD_REPEAT = 2,
    PLD_MISSING = 3,
};

enum
{
    M_SKIP,
    M_FILL,
    M_TWO,
    M_FOUR,
    M_RAW,
};

#define TOOL "ld2pico 3"

void ldp_default_settings(ldp_settings *s)
{
    s->quality = 60;
    s->denoise_lo = 10;
    s->denoise_hi = 28;
    s->keyint = 32;
    s->max_rate_kb = 420;
    s->audio_gain = 0;
    s->threads = 0;
    s->start = -1;
    s->count = 0;
}

/* ---------------------------------------------------------------------------
 * Checksums
 * ------------------------------------------------------------------------ */

static uint32_t crc_table[256];

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    while (n--)
        c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

static uint32_t adler32_of(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    while (n)
    {
        size_t k = n < 5552 ? n : 5552;
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

/* ---------------------------------------------------------------------------
 * Growing byte buffers
 * ------------------------------------------------------------------------ */

typedef struct
{
    uint8_t *p;
    size_t n, cap;
    int failed;
} bytes_t;

static void put(bytes_t *b, const void *data, size_t n)
{
    if (b->failed)
        return;
    if (b->n + n > b->cap)
    {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->n + n)
            cap *= 2;
        uint8_t *p = (uint8_t *)realloc(b->p, cap);
        if (!p)
        {
            b->failed = 1;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, data, n);
    b->n += n;
}

static void put8(bytes_t *b, uint8_t v)
{
    put(b, &v, 1);
}

static void put_le(uint8_t *p, uint32_t v, int n)
{
    for (int i = 0; i < n; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* ---------------------------------------------------------------------------
 * Palette
 * ------------------------------------------------------------------------ */

static inline uint16_t to555(const uint8_t *rgb)
{
    return (uint16_t)(((rgb[0] >> 3) << 10) | ((rgb[1] >> 3) << 5) | (rgb[2] >> 3));
}

static const float WEIGHT[3] = {0.30f * 3, 0.59f * 3, 0.11f * 3};

/* for every RGB555 value the closest palette index (weighted distance) */
static void nearest_lut(const float pal[256][3], uint8_t *lut)
{
    for (int v = 0; v < 32768; v++)
    {
        const float r = ((v >> 10) & 31) * (255.0f / 31.0f), g = ((v >> 5) & 31) * (255.0f / 31.0f),
                    b = (v & 31) * (255.0f / 31.0f);
        float best = 1e30f;
        int bi = 0;
        for (int i = 0; i < 256; i++)
        {
            const float dr = r - pal[i][0], dg = g - pal[i][1], db = b - pal[i][2];
            const float d = dr * dr * WEIGHT[0] + dg * dg * WEIGHT[1] + db * db * WEIGHT[2];
            if (d < best)
            {
                best = d;
                bi = i;
            }
        }
        lut[v] = (uint8_t)bi;
    }
}

static inline float clamp255(float v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

typedef struct
{
    uint32_t start, len;
} box_t;

/* sorts px[start, start+len) by channel ch (counting sort) */
static void sort_box(uint8_t *px, uint8_t *tmp, const box_t *b, int ch)
{
    uint32_t count[257] = {0};
    uint8_t *p = px + 3 * (size_t)b->start;
    for (uint32_t i = 0; i < b->len; i++)
        count[p[3 * i + ch] + 1]++;
    for (int i = 0; i < 256; i++)
        count[i + 1] += count[i];
    for (uint32_t i = 0; i < b->len; i++)
    {
        uint32_t at = count[p[3 * i + ch]]++;
        memcpy(tmp + 3 * at, p + 3 * i, 3);
    }
    memcpy(p, tmp, 3 * (size_t)b->len);
}

static void box_range(const uint8_t *px, const box_t *b, int rng[3])
{
    int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    const uint8_t *p = px + 3 * (size_t)b->start;
    for (uint32_t i = 0; i < b->len; i++)
        for (int c = 0; c < 3; c++)
        {
            if (p[3 * i + c] < lo[c])
                lo[c] = p[3 * i + c];
            if (p[3 * i + c] > hi[c])
                hi[c] = p[3 * i + c];
        }
    for (int c = 0; c < 3; c++)
        rng[c] = hi[c] - lo[c];
}

/* median cut into 256 boxes: split the box with the largest range (weighted
 * by the square root of its size) at the median of that channel */
static void median_cut(uint8_t *px, uint32_t n, float pal[256][3])
{
    box_t boxes[256];
    int nb = 1;
    boxes[0].start = 0;
    boxes[0].len = n;
    uint8_t *tmp = (uint8_t *)malloc(3 * (size_t)n + 3);
    while (nb < 256 && tmp)
    {
        int best = -1, best_ch = 0;
        double best_score = -1;
        for (int i = 0; i < nb; i++)
        {
            if (boxes[i].len < 2)
                continue;
            int rng[3];
            box_range(px, &boxes[i], rng);
            int ch = 0;
            for (int c = 1; c < 3; c++)
                if (rng[c] > rng[ch])
                    ch = c;
            const double score = rng[ch] * sqrt((double)boxes[i].len);
            if (score > best_score)
            {
                best = i;
                best_score = score;
                best_ch = ch;
            }
        }
        if (best < 0)
            break;
        box_t b = boxes[best];
        memmove(&boxes[best], &boxes[best + 1], sizeof(box_t) * (size_t)(nb - best - 1));
        nb--;
        sort_box(px, tmp, &b, best_ch);
        const uint32_t half = b.len / 2;
        boxes[nb].start = b.start;
        boxes[nb].len = half;
        boxes[nb + 1].start = b.start + half;
        boxes[nb + 1].len = b.len - half;
        nb += 2;
    }
    free(tmp);
    int k = 0;
    for (int i = 0; i < nb; i++)
    {
        if (!boxes[i].len)
            continue;
        double s[3] = {0, 0, 0};
        const uint8_t *p = px + 3 * (size_t)boxes[i].start;
        for (uint32_t j = 0; j < boxes[i].len; j++)
            for (int c = 0; c < 3; c++)
                s[c] += p[3 * j + c];
        for (int c = 0; c < 3; c++)
            pal[k][c] = (float)(s[c] / boxes[i].len);
        k++;
    }
    for (; k < 256; k++)
        pal[k][0] = pal[k][1] = pal[k][2] = 0;
}

#define PAL_SAMPLES 300000

/* 256 colours for the frames of a segment, snapped to RGB555 */
static int make_palette(uint8_t *const *frames, int nframes, uint8_t pal8[256][3])
{
    /* every 5th pixel of every frame, thinned out to PAL_SAMPLES */
    const uint64_t total = (uint64_t)nframes * ((NPIX + 4) / 5);
    const uint64_t stride = total > PAL_SAMPLES ? (total + PAL_SAMPLES - 1) / PAL_SAMPLES : 1;
    uint8_t *px = (uint8_t *)malloc(3 * (size_t)(total / stride + 2));
    if (!px)
        return -1;
    uint32_t n = 0;
    uint64_t k = 0;
    for (int f = 0; f < nframes; f++)
        for (int i = 0; i < NPIX; i += 5, k++)
            if (k % stride == 0)
            {
                memcpy(px + 3 * (size_t)n, frames[f] + 3 * (size_t)i, 3);
                n++;
            }

    float pal[256][3];
    uint8_t *seen = (uint8_t *)calloc(32768, 1);
    if (!seen)
    {
        free(px);
        return -1;
    }
    int nuniq = 0;
    for (uint32_t i = 0; i < n && nuniq <= 256; i++)
    {
        const uint16_t v = to555(px + 3 * (size_t)i);
        if (!seen[v])
        {
            seen[v] = 1;
            nuniq++;
        }
    }
    if (nuniq <= 256)
    {
        /* few colours (a fade, a black screen): take them as they are */
        int k2 = 0;
        for (int v = 0; v < 32768; v++)
            if (seen[v])
            {
                pal[k2][0] = ((v >> 10) & 31) * (255.0f / 31.0f);
                pal[k2][1] = ((v >> 5) & 31) * (255.0f / 31.0f);
                pal[k2][2] = (v & 31) * (255.0f / 31.0f);
                k2++;
            }
        for (; k2 < 256; k2++)
            pal[k2][0] = pal[k2][1] = pal[k2][2] = 0;
    }
    else
    {
        median_cut(px, n, pal);
        uint8_t *lut = seen; /* reused */
        for (int it = 0; it < 4; it++)
        {
            float rounded[256][3];
            for (int i = 0; i < 256; i++)
                for (int c = 0; c < 3; c++)
                    rounded[i][c] = clamp255(rintf(pal[i][c]));
            nearest_lut((const float(*)[3])rounded, lut);
            double sums[256][3];
            uint32_t cnt[256];
            memset(sums, 0, sizeof(sums));
            memset(cnt, 0, sizeof(cnt));
            for (uint32_t i = 0; i < n; i++)
            {
                const uint8_t *p = px + 3 * (size_t)i;
                const int idx = lut[to555(p)];
                cnt[idx]++;
                for (int c = 0; c < 3; c++)
                    sums[idx][c] += p[c];
            }
            for (int i = 0; i < 256; i++)
                if (cnt[i])
                    for (int c = 0; c < 3; c++)
                        pal[i][c] = (float)(sums[i][c] / cnt[i]);
        }
    }
    free(seen);
    free(px);
    /* what the board shows is RGB555: snap the palette to it */
    for (int i = 0; i < 256; i++)
        for (int c = 0; c < 3; c++)
        {
            const int v = (int)clamp255(rintf(pal[i][c]));
            pal8[i][c] = (uint8_t)(((v >> 3) * 255 + 15) / 31);
        }
    return 0;
}

/* ---------------------------------------------------------------------------
 * The block coder
 * ------------------------------------------------------------------------ */

typedef struct
{
    int32_t pal[256][3];
    float lum[256];
    uint8_t lut[32768];
    double limit;
} coder_t;

static void coder_init(coder_t *c, const uint8_t pal8[256][3], double quality)
{
    float palf[256][3];
    for (int i = 0; i < 256; i++)
    {
        for (int k = 0; k < 3; k++)
        {
            c->pal[i][k] = pal8[i][k];
            palf[i][k] = pal8[i][k];
        }
        c->lum[i] = pal8[i][0] * 0.30f + pal8[i][1] * 0.59f + pal8[i][2] * 0.11f;
    }
    nearest_lut((const float(*)[3])palf, c->lut);
    c->limit = 16.0 * 3.0 * quality;
}

static inline uint8_t nearest(const coder_t *c, const float rgb[3])
{
    uint8_t v[3];
    for (int k = 0; k < 3; k++)
        v[k] = (uint8_t)clamp255(rintf(rgb[k]));
    return c->lut[to555(v)];
}

/* the choice for one block */
typedef struct
{
    uint8_t mode;
    uint8_t fill;
    uint8_t two[2];
    uint8_t four[4];
    uint16_t mask2;
    uint32_t mask4;
    uint8_t raw[16];
} block_t;

static int64_t sq_err(const coder_t *c, const uint8_t *rec, const int32_t tgt[16][3])
{
    int64_t e = 0;
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 3; k++)
        {
            const int32_t d = c->pal[rec[i]][k] - tgt[i][k];
            e += d * d;
        }
    return e;
}

/* k-means with k = 2 or 4 colours, started from the pixels sorted by
 * brightness, two rounds, then snapped to the palette */
static void kmeans(const coder_t *c, int k, const float tsort[16][3], const float tf[16][3], uint8_t *out)
{
    float cen[4][3];
    const int per = 16 / k;
    for (int j = 0; j < k; j++)
        for (int ch = 0; ch < 3; ch++)
        {
            float s = 0;
            for (int i = j * per; i < (j + 1) * per; i++)
                s += tsort[i][ch];
            cen[j][ch] = s / per;
        }
    for (int it = 0; it < 2; it++)
    {
        float sums[4][3] = {{0}};
        int cnt[4] = {0};
        for (int i = 0; i < 16; i++)
        {
            int best = 0;
            float bd = 1e30f;
            for (int j = 0; j < k; j++)
            {
                const float dr = tf[i][0] - cen[j][0], dg = tf[i][1] - cen[j][1], db = tf[i][2] - cen[j][2];
                const float d = dr * dr + dg * dg + db * db;
                if (d < bd)
                {
                    bd = d;
                    best = j;
                }
            }
            cnt[best]++;
            for (int ch = 0; ch < 3; ch++)
                sums[best][ch] += tf[i][ch];
        }
        for (int j = 0; j < k; j++)
            if (cnt[j])
                for (int ch = 0; ch < 3; ch++)
                    cen[j][ch] = sums[j][ch] / cnt[j];
    }
    for (int j = 0; j < k; j++)
        out[j] = nearest(c, cen[j]);
}

/* every pixel to the nearest of k colours; sel and the reconstruction */
static void assign(const coder_t *c, const uint8_t *colours, int k, const int32_t tgt[16][3], uint8_t *sel,
                   uint8_t *rec)
{
    for (int i = 0; i < 16; i++)
    {
        int best = 0;
        int64_t bd = INT64_MAX;
        for (int j = 0; j < k; j++)
        {
            int64_t d = 0;
            for (int ch = 0; ch < 3; ch++)
            {
                const int32_t e = tgt[i][ch] - c->pal[colours[j]][ch];
                d += e * e;
            }
            if (d < bd)
            {
                bd = d;
                best = j;
            }
        }
        sel[i] = (uint8_t)best;
        rec[i] = colours[best];
    }
}

/* Codes one block: cur (16 indices) against prev (or NULL in a key record);
 * fills b and the 16 reconstructed indices. */
static void code_block(const coder_t *c, const uint8_t *cur, const uint8_t *prev, block_t *b, uint8_t *rec)
{
    int32_t tgt[16][3];
    float tf[16][3];
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 3; k++)
        {
            tgt[i][k] = c->pal[cur[i]][k];
            tf[i][k] = (float)tgt[i][k];
        }

    /* the distinct indices, ascending, padded with the first */
    uint8_t s[16];
    memcpy(s, cur, 16);
    for (int i = 1; i < 16; i++)
    {
        const uint8_t v = s[i];
        int j = i - 1;
        while (j >= 0 && s[j] > v)
        {
            s[j + 1] = s[j];
            j--;
        }
        s[j + 1] = v;
    }
    uint8_t uniq[4];
    int nuniq = 0;
    for (int i = 0; i < 16; i++)
        if (i == 0 || s[i] != s[i - 1])
        {
            if (nuniq < 4)
                uniq[nuniq] = s[i];
            nuniq++;
        }
    for (int i = nuniq; i < 4; i++)
        uniq[i] = uniq[0];

    /* the pixels sorted by brightness (stable) */
    int order[16];
    for (int i = 0; i < 16; i++)
    {
        int j = i - 1;
        const float l = c->lum[cur[i]];
        while (j >= 0 && c->lum[cur[order[j]]] > l)
        {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = i;
    }
    float tsort[16][3];
    float mean[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 3; k++)
        {
            tsort[i][k] = tf[order[i]][k];
            mean[k] += tf[i][k];
        }
    for (int k = 0; k < 3; k++)
        mean[k] /= 16;

    uint8_t fill = nearest(c, mean);
    uint8_t two[2], four[4];
    kmeans(c, 2, (const float(*)[3])tsort, (const float(*)[3])tf, two);
    kmeans(c, 4, (const float(*)[3])tsort, (const float(*)[3])tf, four);
    /* exact fits where the block has few enough colours */
    if (nuniq == 1)
        fill = uniq[0];
    if (nuniq <= 2)
        memcpy(two, uniq, 2);
    if (nuniq <= 4)
        memcpy(four, uniq, 4);

    uint8_t sel2[16], sel4[16], rec2[16], rec4[16], recf[16];
    assign(c, two, 2, (const int32_t(*)[3])tgt, sel2, rec2);
    assign(c, four, 4, (const int32_t(*)[3])tgt, sel4, rec4);
    memset(recf, fill, 16);

    /* the cheapest acceptable mode; an exact copy of the block before is
     * always left unchanged */
    int mode = M_RAW;
    if (prev && !memcmp(prev, cur, 16))
        mode = M_SKIP;
    else if (prev && sq_err(c, prev, (const int32_t(*)[3])tgt) <= c->limit)
        mode = M_SKIP;
    else if (sq_err(c, recf, (const int32_t(*)[3])tgt) <= c->limit)
        mode = M_FILL;
    else if (sq_err(c, rec2, (const int32_t(*)[3])tgt) <= c->limit)
        mode = M_TWO;
    else if (sq_err(c, rec4, (const int32_t(*)[3])tgt) <= c->limit)
        mode = M_FOUR;

    b->mode = (uint8_t)mode;
    b->fill = fill;
    memcpy(b->two, two, 2);
    memcpy(b->four, four, 4);
    memcpy(b->raw, cur, 16);
    b->mask2 = 0;
    b->mask4 = 0;
    for (int i = 0; i < 16; i++)
    {
        b->mask2 |= (uint16_t)((sel2[i] & 1) << i);
        b->mask4 |= (uint32_t)(sel4[i] & 3) << (2 * i);
    }
    switch (mode)
    {
    case M_SKIP: memcpy(rec, prev, 16); break;
    case M_FILL: memcpy(rec, recf, 16); break;
    case M_TWO: memcpy(rec, rec2, 16); break;
    case M_FOUR: memcpy(rec, rec4, 16); break;
    default: memcpy(rec, cur, 16); break;
    }
}

/* the byte opcodes of one block row (see dlair/pld.h) */
static void emit_row(bytes_t *out, const block_t *bl)
{
    int x = 0;
    while (x < BX)
    {
        const int m = bl[x].mode;
        int e = x + 1;
        while (e < BX && bl[e].mode == m)
            e++;
        if (m == M_SKIP)
        {
            while (x < e)
            {
                const int k = e - x < 64 ? e - x : 64;
                put8(out, (uint8_t)(k - 1));
                x += k;
            }
        }
        else if (m == M_FILL)
        {
            /* runs of one colour as RUN, the rest as FILL lists */
            while (x < e)
            {
                int r = x + 1;
                while (r < e && bl[r].fill == bl[x].fill && r - x < 64)
                    r++;
                if (r - x >= 3)
                {
                    put8(out, (uint8_t)(0x40 | (r - x - 1)));
                    put8(out, bl[x].fill);
                    x = r;
                    continue;
                }
                /* a FILL list up to the next run of 3 or more */
                const int s = x;
                while (x < e && x - s < 32)
                {
                    int r2 = x + 1;
                    while (r2 < e && bl[r2].fill == bl[x].fill && r2 - x < 3)
                        r2++;
                    if (r2 - x >= 3)
                        break;
                    x++;
                }
                put8(out, (uint8_t)(0x80 | (x - s - 1)));
                for (int i = s; i < x; i++)
                    put8(out, bl[i].fill);
            }
        }
        else
        {
            while (x < e)
            {
                const int k = e - x < 32 ? e - x : 32;
                if (m == M_TWO)
                {
                    put8(out, (uint8_t)(0xa0 | (k - 1)));
                    for (int i = x; i < x + k; i++)
                    {
                        uint8_t v[4] = {bl[i].two[0], bl[i].two[1], (uint8_t)bl[i].mask2, (uint8_t)(bl[i].mask2 >> 8)};
                        put(out, v, 4);
                    }
                }
                else if (m == M_FOUR)
                {
                    put8(out, (uint8_t)(0xc0 | (k - 1)));
                    for (int i = x; i < x + k; i++)
                    {
                        uint8_t v[8];
                        memcpy(v, bl[i].four, 4);
                        put_le(v + 4, bl[i].mask4, 4);
                        put(out, v, 8);
                    }
                }
                else
                {
                    put8(out, (uint8_t)(0xe0 | (k - 1)));
                    for (int i = x; i < x + k; i++)
                        put(out, bl[i].raw, 16);
                }
                x += k;
            }
        }
    }
}

/* picture (raster of indices) <-> blocks of 16 */
static void to_blocks(const uint8_t *raster, uint8_t *blocks)
{
    for (int by = 0; by < BY; by++)
        for (int bx = 0; bx < BX; bx++)
            for (int y = 0; y < 4; y++)
                memcpy(blocks + 16 * (by * BX + bx) + 4 * y, raster + (by * 4 + y) * W + bx * 4, 4);
}

static void from_blocks(const uint8_t *blocks, uint8_t *raster)
{
    for (int by = 0; by < BY; by++)
        for (int bx = 0; bx < BX; bx++)
            for (int y = 0; y < 4; y++)
                memcpy(raster + (by * 4 + y) * W + bx * 4, blocks + 16 * (by * BX + bx) + 4 * y, 4);
}

/* Codes a picture: cur against prev (NULL in a key record). Writes the
 * opcodes to out and the reconstruction to rec; returns whether every block
 * was left unchanged. */
static int encode_video(const coder_t *c, const uint8_t *cur, const uint8_t *prev, bytes_t *out, uint8_t *rec)
{
    block_t row[BX];
    int all_skip = 1;
    for (int by = 0; by < BY; by++)
    {
        for (int bx = 0; bx < BX; bx++)
        {
            const int i = by * BX + bx;
            code_block(c, cur + 16 * i, prev ? prev + 16 * i : NULL, &row[bx], rec + 16 * i);
            if (row[bx].mode != M_SKIP)
                all_skip = 0;
        }
        emit_row(out, row);
    }
    return prev && all_skip;
}

/* ---------------------------------------------------------------------------
 * Segments
 * ------------------------------------------------------------------------ */

typedef struct
{
    int number;
    uint8_t *rgb; /* NULL: not on the disc */
    int nsamples;
} pic_t;

typedef struct
{
    int number;
    uint8_t *rec;
    uint32_t len;
    int key;
    uint32_t crc;
} rec_t;

typedef struct job
{
    pic_t *pics;
    int n;
    rec_t *out;
    double q_used;
    int done;  /* 1 coded, -1 failed */
    int taken; /* a worker has it */
} job_t;

static void record(bytes_t *out, int type, const uint8_t *pal, int nsamples, const bytes_t *video)
{
    const size_t vlen = video ? video->n : 0;
    const size_t body = (pal ? 512 : 0) + vlen;
    const size_t total = (16 + body + 3) & ~(size_t)3;
    uint8_t *buf = (uint8_t *)calloc(1, total);
    if (!buf)
    {
        out->failed = 1;
        return;
    }
    size_t at = 16;
    if (pal)
    {
        memcpy(buf + at, pal, 512);
        at += 512;
    }
    if (vlen)
        memcpy(buf + at, video->p, vlen);
    buf[0] = (uint8_t)type;
    buf[1] = 0;
    put_le(buf + 2, (uint32_t)nsamples, 2);
    put_le(buf + 4, (uint32_t)vlen, 4);
    put_le(buf + 8, (uint32_t)total, 4);
    put_le(buf + 12, adler32_of(buf + 16, body), 4);
    out->p = buf;
    out->n = total;
}

static void denoise(pic_t *pics, int n, float lo, float hi, float *prev)
{
    if (hi <= lo)
        return;
    int have = 0;
    for (int p = 0; p < n; p++)
    {
        uint8_t *rgb = pics[p].rgb;
        if (!rgb)
            continue;
        for (int i = 0; i < NPIX; i++)
        {
            float f[3] = {rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2]};
            if (have)
            {
                float *q = prev + 3 * i;
                float d = 0;
                for (int k = 0; k < 3; k++)
                {
                    const float a = fabsf(f[k] - q[k]);
                    if (a > d)
                        d = a;
                }
                float w = (d - lo) / (hi - lo);
                w = w < 0 ? 0 : w > 1 ? 1 : w;
                for (int k = 0; k < 3; k++)
                    f[k] = q[k] + (f[k] - q[k]) * w;
            }
            for (int k = 0; k < 3; k++)
            {
                prev[3 * i + k] = f[k];
                rgb[3 * i + k] = (uint8_t)clamp255(rintf(f[k]));
            }
        }
        have = 1;
    }
}

typedef struct
{
    const ldp_settings *s;
    ldp_monitor *mon;
    job_t **jobs; /* in order of the pictures */
    int njobs;    /* submitted */
    int next;     /* the first job no worker has taken */
    int stop;
    volatile int cancel;
} pool_t;

static void free_out(job_t *j)
{
    if (!j->out)
        return;
    for (int i = 0; i < j->n; i++)
        free(j->out[i].rec);
    free(j->out);
    j->out = NULL;
}

/* Codes the pictures of a job with quality q. Returns 0, or -1 when out of
 * memory or cancelled. */
static int code_pictures(job_t *j, const coder_t *c, const uint8_t *pal_bytes, int keyint, const pool_t *pool)
{
    uint8_t *cur = (uint8_t *)malloc(NPIX), *prev = (uint8_t *)malloc(NPIX), *rec = (uint8_t *)malloc(NPIX),
            *raster = (uint8_t *)malloc(NPIX), *idx = (uint8_t *)malloc(NPIX);
    j->out = (rec_t *)calloc((size_t)j->n, sizeof(rec_t));
    int ok = cur && prev && rec && raster && idx && j->out;
    int have_prev = 0, since_key = 0;
    for (int p = 0; ok && p < j->n; p++)
    {
        if (pool->cancel)
        {
            ok = 0;
            break;
        }
        pic_t *pic = &j->pics[p];
        rec_t *r = &j->out[p];
        r->number = pic->number;
        bytes_t out = {0};
        if (!pic->rgb && have_prev)
        {
            record(&out, PLD_MISSING, NULL, pic->nsamples, NULL);
            from_blocks(prev, raster);
            r->crc = crc32_of(raster, NPIX);
        }
        else
        {
            for (int i = 0; i < NPIX; i++)
                idx[i] = pic->rgb ? c->lut[to555(pic->rgb + 3 * i)] : c->lut[0];
            to_blocks(idx, cur);
            const int key = !have_prev || since_key >= keyint;
            bytes_t video = {0};
            const int all_skip = encode_video(c, cur, key ? NULL : prev, &video, rec);
            if (key)
            {
                record(&out, PLD_KEY, pal_bytes, pic->nsamples, &video);
                since_key = 1;
            }
            else
            {
                record(&out, all_skip ? PLD_REPEAT : PLD_DELTA, NULL, pic->nsamples, all_skip ? NULL : &video);
                since_key++;
            }
            if (video.failed)
                out.failed = 1;
            free(video.p);
            memcpy(prev, rec, NPIX);
            have_prev = 1;
            r->key = key;
            from_blocks(prev, raster);
            r->crc = crc32_of(raster, NPIX);
        }
        if (out.failed)
        {
            free(out.p);
            ok = 0;
            break;
        }
        r->rec = out.p;
        r->len = (uint32_t)out.n;
    }
    free(cur);
    free(prev);
    free(rec);
    free(raster);
    free(idx);
    return ok ? 0 : -1;
}

/* no 24 consecutive records above max_rate bytes, none above a third of it */
static int within_rate(const job_t *j, double max_rate)
{
    double sum = 0, win = 0;
    for (int i = 0; i < j->n; i++)
    {
        const double len = j->out[i].len;
        if (len > max_rate / 3)
            return 0;
        sum += len;
        win += len;
        if (i >= 24)
            win -= j->out[i - 24].len;
        if (i >= 23 && win > max_rate)
            return 0;
    }
    return j->n >= 24 || sum * 24 / (j->n ? j->n : 1) <= max_rate * 1.5;
}

static int encode_segment(job_t *j, const pool_t *pool)
{
    const ldp_settings *s = pool->s;
    float *prevf = (float *)malloc(sizeof(float) * 3 * NPIX);
    uint8_t **frames = (uint8_t **)malloc(sizeof(uint8_t *) * (size_t)(j->n + 1));
    coder_t *c = (coder_t *)malloc(sizeof(coder_t));
    int ok = prevf && frames && c;
    if (ok)
    {
        denoise(j->pics, j->n, (float)s->denoise_lo, (float)s->denoise_hi, prevf);
        int nf = 0;
        for (int i = 0; i < j->n; i++)
            if (j->pics[i].rgb)
                frames[nf++] = j->pics[i].rgb;
        uint8_t pal8[256][3];
        uint8_t *blackframe = NULL;
        if (!nf)
        {
            /* nothing of the segment is on the disc: a black palette */
            blackframe = (uint8_t *)calloc(3, NPIX);
            if (blackframe)
                frames[nf++] = blackframe;
        }
        ok = nf && make_palette(frames, nf, pal8) == 0;
        free(blackframe);
        uint8_t pal_bytes[512];
        for (int i = 0; ok && i < 256; i++)
            put_le(pal_bytes + 2 * i, (uint32_t)(((pal8[i][0] >> 3) << 10) | ((pal8[i][1] >> 3) << 5) | (pal8[i][2] >> 3)),
                   2);
        double q = s->quality;
        for (int attempt = 0; ok && attempt < 6; attempt++)
        {
            coder_init(c, (const uint8_t(*)[3])pal8, q);
            if (code_pictures(j, c, pal_bytes, s->keyint, pool))
            {
                ok = 0;
                break;
            }
            j->q_used = q;
            if (s->max_rate_kb <= 0 || within_rate(j, s->max_rate_kb * 1024))
                break;
            if (attempt == 5)
                break;
            free_out(j);
            q *= 1.5;
        }
    }
    free(prevf);
    free(frames);
    free(c);
    return ok ? 0 : -1;
}

static void worker(void *arg)
{
    pool_t *pool = (pool_t *)arg;
    ldp_lock(pool->mon);
    for (;;)
    {
        while (!pool->stop && pool->next >= pool->njobs)
            ldp_wait(pool->mon);
        if (pool->next >= pool->njobs)
            break; /* stopped, and nothing left */
        job_t *j = pool->jobs[pool->next++];
        j->taken = 1;
        ldp_unlock(pool->mon);
        const int r = encode_segment(j, pool);
        ldp_lock(pool->mon);
        j->done = r ? -1 : 1;
        ldp_notify_all(pool->mon);
    }
    ldp_unlock(pool->mon);
}

/* ---------------------------------------------------------------------------
 * Sound
 * ------------------------------------------------------------------------ */

/* 2:1 decimation of a continuous 44.1 kHz stream, picture by picture, with a
 * 31-tap half-band low-pass (windowed sinc) */
typedef struct
{
    double fir[31];
    double hist[30];
    int phase;
} resampler_t;

static void resampler_init(resampler_t *r)
{
    const double pi = 3.14159265358979323846;
    double sum = 0;
    for (int i = 0; i < 31; i++)
    {
        const double t = (i - 15) / 2.0;
        const double sinc = t == 0 ? 1.0 : sin(pi * t) / (pi * t);
        const double hamming = 0.54 - 0.46 * cos(2 * pi * i / 30.0);
        r->fir[i] = sinc * hamming;
        sum += r->fir[i];
    }
    for (int i = 0; i < 31; i++)
        r->fir[i] /= sum;
    memset(r->hist, 0, sizeof(r->hist));
    r->phase = 0;
}

/* in: n mono samples; out: the decimated ones; returns their number */
static int resample(resampler_t *r, const double *in, int n, float *out)
{
    double *x = (double *)malloc(sizeof(double) * (size_t)(n + 30));
    if (!x)
        return -1;
    memcpy(x, r->hist, sizeof(r->hist));
    memcpy(x + 30, in, sizeof(double) * (size_t)n);
    int m = 0;
    for (int k = r->phase % 2; k < n; k += 2)
    {
        double y = 0;
        for (int j = 0; j < 31; j++)
            y += x[k + j] * r->fir[30 - j];
        out[m++] = (float)y;
    }
    r->phase = (r->phase + n) % 2;
    memcpy(r->hist, x + n, sizeof(r->hist));
    free(x);
    return m;
}

static uint8_t ulaw(double v)
{
    int x = (int)rint(v);
    if (x > 32767)
        x = 32767;
    else if (x < -32768)
        x = -32768;
    const int sign = x < 0 ? 0x80 : 0;
    int mag = x < 0 ? -x : x;
    if (mag > 32635)
        mag = 32635;
    mag += 0x84;
    int exp = 7;
    while (exp > 0 && !(mag & (0x80 << exp)))
        exp--;
    const int mant = (mag >> (exp + 3)) & 0x0f;
    return (uint8_t)(~(sign | (exp << 4) | mant) & 0xff);
}

/* ---------------------------------------------------------------------------
 * The conversion
 * ------------------------------------------------------------------------ */

static void logf_(const ldp_hooks *h, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (h && h->log)
        h->log(h->ctx, line);
}

static int cancelled(const ldp_hooks *h)
{
    return h && h->cancelled && h->cancelled(h->ctx);
}

typedef struct
{
    FILE *f;
    int64_t pos;
    int first, last, have_first;
    /* key table: frame, record offset, sound sample */
    uint32_t *keys;
    size_t nkeys, capkeys;
    uint32_t *crcs;
    uint32_t *sizes;
    size_t nrec, capcrc, capsize;
    uint64_t audio_pos; /* sum of the records' sound samples */
    uint32_t max_record;
    int types[4];
    int raised;
    double raised_q;
} writer_t;

static int grow(void **p, size_t *cap, size_t need, size_t size)
{
    if (need <= *cap)
        return 0;
    size_t c = *cap ? *cap * 2 : 1024;
    while (c < need)
        c *= 2;
    void *q = realloc(*p, c * size);
    if (!q)
        return -1;
    *p = q;
    *cap = c;
    return 0;
}

static int write_job(writer_t *w, job_t *j, const ldp_settings *s, const ldp_hooks *h)
{
    if (j->q_used > s->quality)
    {
        w->raised++;
        if (j->q_used > w->raised_q)
            w->raised_q = j->q_used;
    }
    for (int i = 0; i < j->n; i++)
    {
        rec_t *r = &j->out[i];
        if (w->have_first && r->number != w->last + 1)
        {
            logf_(h, "error: picture numbers are not consecutive (%d after %d)", r->number, w->last);
            return -1;
        }
        if (!w->have_first)
        {
            w->first = r->number;
            w->have_first = 1;
        }
        w->last = r->number;
        w->types[r->rec[0] & 3]++;
        if (r->key)
        {
            size_t capk = w->capkeys;
            if (grow((void **)&w->keys, &capk, 3 * (w->nkeys + 1), sizeof(uint32_t)))
                return -1;
            w->capkeys = capk;
            w->keys[3 * w->nkeys] = (uint32_t)r->number;
            w->keys[3 * w->nkeys + 1] = (uint32_t)w->pos;
            w->keys[3 * w->nkeys + 2] = (uint32_t)w->audio_pos;
            w->nkeys++;
        }
        if (grow((void **)&w->crcs, &w->capcrc, w->nrec + 1, sizeof(uint32_t)) ||
            grow((void **)&w->sizes, &w->capsize, w->nrec + 1, sizeof(uint32_t)))
            return -1;
        w->crcs[w->nrec] = r->crc;
        w->sizes[w->nrec] = r->len;
        w->nrec++;
        w->audio_pos += (uint32_t)(r->rec[2] | (r->rec[3] << 8));
        if (r->len > w->max_record)
            w->max_record = r->len;
        if (fwrite(r->rec, 1, r->len, w->f) != r->len)
        {
            logf_(h, "error: cannot write the output file (disk full?)");
            return -1;
        }
        w->pos += r->len;
        if (w->pos >= 0xffffffffLL)
        {
            logf_(h, "error: the output file would be larger than 4 GB");
            return -1;
        }
    }
    return 0;
}

static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    return b;
}

int ldp_convert(const char *chd_path, const char *pld_path, const ldp_settings *s, const ldp_hooks *h)
{
    crc_init();
    char err[256];
    const double t0 = ldp_seconds();
    ldp_chd *chd = ldp_chd_open(chd_path, err, sizeof(err));
    if (!chd)
    {
        logf_(h, "error: %s: %s", chd_path, err);
        return 1;
    }
    int first = ldp_chd_first(chd), last = ldp_chd_last(chd);
    if (s->start >= 0)
    {
        if (s->start > first)
            first = s->start;
        if (s->count > 0 && first + s->count - 1 < last)
            last = first + s->count - 1;
    }
    if (first > last)
    {
        logf_(h, "error: no pictures in the range asked for");
        ldp_chd_close(chd);
        return 1;
    }
    const int total = last - first + 1;
    char info[300];
    ldp_chd_info(chd, info, sizeof(info));
    logf_(h, "%s: %s", base_name(chd_path), info);

    int threads = s->threads > 0 ? s->threads : ldp_cpu_count();
    if (threads > MAX_THREADS)
        threads = MAX_THREADS;
    if (threads < 1)
        threads = 1;

    size_t part_len = strlen(pld_path) + 6;
    char *part = (char *)malloc(part_len);
    snprintf(part, part_len, "%s.part", pld_path);
    writer_t w;
    memset(&w, 0, sizeof(w));
    w.f = ldp_fopen(part, "wb");
    if (!w.f)
    {
        logf_(h, "error: cannot create %s", part);
        ldp_chd_close(chd);
        free(part);
        return 1;
    }
    uint8_t zero[512] = {0};
    fwrite(zero, 1, 512, w.f);
    w.pos = 512;

    pool_t pool;
    memset(&pool, 0, sizeof(pool));
    pool.s = s;
    pool.mon = ldp_monitor_new();
    pool.jobs = (job_t **)calloc((size_t)total + 1, sizeof(job_t *));
    ldp_thread **th = (ldp_thread **)calloc((size_t)threads, sizeof(ldp_thread *));
    for (int i = 0; i < threads; i++)
        th[i] = ldp_thread_start(worker, &pool);
    logf_(h, "converting %d pictures with %d threads", total, threads);

    resampler_t rs;
    resampler_init(&rs);
    float *pcm = NULL; /* the sound of all pictures */
    size_t npcm = 0, cappcm = 0;
    int16_t *audio = (int16_t *)malloc(sizeof(int16_t) * 2 * LDP_CHD_MAX_SAMPLES);
    double *mono = (double *)malloc(sizeof(double) * LDP_CHD_MAX_SAMPLES);
    float *grey = (float *)malloc(sizeof(float) * (H / 4) * (W / 4)), *last_grey = NULL;

    int status = 0, written = 0, oldest = 0;
    job_t *seg = NULL;
    const int in_flight = threads + 2;

    for (int p = first; p <= last + 1 && !status; p++)
    {
        pic_t pic = {p, NULL, 0};
        int cut = 0;
        if (p <= last)
        {
            if (cancelled(h))
            {
                status = 2;
                break;
            }
            pic.rgb = (uint8_t *)malloc(3 * NPIX);
            int nsamp = 0;
            const int r = pic.rgb ? ldp_chd_picture(chd, p, pic.rgb, audio, &nsamp, err, sizeof(err)) : -1;
            if (r < 0)
            {
                logf_(h, "error: %s", pic.rgb ? err : "out of memory");
                free(pic.rgb);
                status = 1;
                break;
            }
            if (r == 1)
            {
                free(pic.rgb);
                pic.rgb = NULL;
            }
            /* sound: both channels added (the game's sound is on one), then
             * decimated to 22050 Hz */
            for (int i = 0; i < nsamp; i++)
                mono[i] = (double)audio[2 * i] + audio[2 * i + 1];
            if (grow((void **)&pcm, &cappcm, npcm + (size_t)nsamp / 2 + 2, sizeof(float)))
            {
                logf_(h, "error: out of memory");
                status = 1;
                break;
            }
            const int got = nsamp ? resample(&rs, mono, nsamp, pcm + npcm) : 0;
            if (got < 0)
            {
                status = 1;
                break;
            }
            npcm += (size_t)got;
            pic.nsamples = got;
            /* a scene cut: the mean grey difference of a 80x60 sample */
            if (pic.rgb && grey)
            {
                double diff = 0;
                for (int y = 0; y < H / 4; y++)
                    for (int x = 0; x < W / 4; x++)
                    {
                        const uint8_t *q = pic.rgb + 3 * ((size_t)(y * 4) * W + x * 4);
                        const float g = q[0] * 0.30f + q[1] * 0.59f + q[2] * 0.11f;
                        if (last_grey)
                            diff += fabsf(g - last_grey[y * (W / 4) + x]);
                        grey[y * (W / 4) + x] = g;
                    }
                if (last_grey && diff / ((H / 4) * (W / 4)) > CUT_THRESHOLD)
                    cut = 1;
                if (!last_grey)
                    last_grey = (float *)malloc(sizeof(float) * (H / 4) * (W / 4));
                if (last_grey)
                    memcpy(last_grey, grey, sizeof(float) * (H / 4) * (W / 4));
            }
        }
        /* hand the segment to the workers at a cut, when it is full, or at
         * the end */
        if (seg && (p > last || cut || seg->n >= MAX_SEGMENT))
        {
            ldp_lock(pool.mon);
            pool.jobs[pool.njobs++] = seg;
            ldp_notify_all(pool.mon);
            ldp_unlock(pool.mon);
            seg = NULL;
        }
        if (p <= last)
        {
            if (!seg)
            {
                seg = (job_t *)calloc(1, sizeof(job_t));
                if (seg)
                    seg->pics = (pic_t *)malloc(sizeof(pic_t) * MAX_SEGMENT);
                if (!seg || !seg->pics)
                {
                    logf_(h, "error: out of memory");
                    free(pic.rgb);
                    status = 1;
                    break;
                }
            }
            seg->pics[seg->n++] = pic;
        }
        /* write what is done, in order; wait while too much is in flight */
        ldp_lock(pool.mon);
        for (;;)
        {
            while (oldest < pool.njobs && pool.jobs[oldest]->done)
            {
                job_t *j = pool.jobs[oldest];
                ldp_unlock(pool.mon);
                if (j->done < 0 || write_job(&w, j, s, h))
                {
                    if (j->done < 0 && !pool.cancel)
                        logf_(h, "error: out of memory");
                    status = 1;
                }
                written += j->n;
                for (int i = 0; i < j->n; i++)
                    free(j->pics[i].rgb);
                free(j->pics);
                free_out(j);
                free(j);
                pool.jobs[oldest] = NULL;
                oldest++;
                if (h && h->progress)
                    h->progress(h->ctx, written, total);
                ldp_lock(pool.mon);
                if (status)
                    break;
            }
            const int flight = pool.njobs - oldest;
            if (status)
                break;
            /* while reading: room for another segment; at the end: all done */
            if (p <= last ? flight < in_flight : flight == 0)
                break;
            if (cancelled(h))
            {
                status = 2;
                break;
            }
            ldp_wait_ms(pool.mon, 200);
        }
        ldp_unlock(pool.mon);
    }

    /* stop the workers */
    ldp_lock(pool.mon);
    if (status)
        pool.cancel = 1;
    pool.stop = 1;
    ldp_notify_all(pool.mon);
    ldp_unlock(pool.mon);
    for (int i = 0; i < threads; i++)
        ldp_thread_join(th[i]);
    for (int i = oldest; i < pool.njobs; i++)
    {
        job_t *j = pool.jobs[i];
        if (!j)
            continue;
        for (int k = 0; k < j->n; k++)
            free(j->pics[k].rgb);
        free(j->pics);
        free_out(j);
        free(j);
    }
    if (seg)
    {
        for (int k = 0; k < seg->n; k++)
            free(seg->pics[k].rgb);
        free(seg->pics);
        free(seg);
    }
    free(th);
    free(pool.jobs);
    ldp_monitor_free(pool.mon);
    free(audio);
    free(mono);
    free(grey);
    free(last_grey);
    const uint32_t crc_errors = ldp_chd_crc_errors(chd);
    ldp_chd_close(chd);

    uint32_t audio_offset = 0, audio_bytes = 0, key_offset = 0, crc_offset = 0;
    double gain = s->audio_gain;
    if (!status && npcm != w.audio_pos)
    {
        logf_(h, "error: sound and pictures disagree");
        status = 1;
    }
    if (!status)
    {
        /* the sound track, brought to a common level: the loudest 0.01% of
         * the samples just reach 90% of full scale */
        if (gain <= 0)
        {
            uint32_t *hist = (uint32_t *)calloc(65537, sizeof(uint32_t));
            if (hist)
            {
                for (size_t i = 0; i < npcm; i++)
                {
                    int a = (int)fabsf(pcm[i]);
                    hist[a > 65536 ? 65536 : a]++;
                }
                const uint64_t want = (uint64_t)(npcm * 0.9999);
                uint64_t acc = 0;
                int level = 0;
                for (int i = 0; i <= 65536; i++)
                {
                    acc += hist[i];
                    if (acc >= want)
                    {
                        level = i;
                        break;
                    }
                }
                free(hist);
                gain = level > 0 ? 29500.0 / level : 1.0;
            }
            else
                gain = 1.0;
        }
        audio_offset = (uint32_t)w.pos;
        uint8_t chunk[4096];
        size_t k = 0;
        for (size_t i = 0; i < npcm; i++)
        {
            chunk[k++] = ulaw(pcm[i] * gain);
            if (k == sizeof(chunk) || i + 1 == npcm)
            {
                if (fwrite(chunk, 1, k, w.f) != k)
                    status = 1;
                k = 0;
            }
        }
        audio_bytes = (uint32_t)npcm;
        w.pos += npcm;
        while (w.pos % 4)
        {
            fputc(0, w.f);
            w.pos++;
        }
        key_offset = (uint32_t)w.pos;
        for (size_t i = 0; i < w.nkeys; i++)
        {
            uint8_t e[12];
            put_le(e, w.keys[3 * i], 4);
            put_le(e + 4, w.keys[3 * i + 1], 4);
            put_le(e + 8, w.keys[3 * i + 2], 4);
            fwrite(e, 1, 12, w.f);
        }
        crc_offset = key_offset + 12 * (uint32_t)w.nkeys;
        for (size_t i = 0; i < w.nrec; i++)
        {
            uint8_t e[4];
            put_le(e, w.crcs[i], 4);
            fwrite(e, 1, 4, w.f);
        }
        /* the header */
        uint8_t hd[512] = {0};
        put_le(hd + 0, 0x32444c50u, 4); /* "PLD2" */
        put_le(hd + 4, 2, 2);
        put_le(hd + 6, 512, 2);
        put_le(hd + 8, W, 2);
        put_le(hd + 10, H, 2);
        put_le(hd + 12, 24000, 4);
        put_le(hd + 16, 1001, 4);
        put_le(hd + 20, 22050, 4);
        hd[24] = 1;
        hd[25] = 4;
        put_le(hd + 28, (uint32_t)w.first, 4);
        put_le(hd + 32, (uint32_t)(w.last - w.first + 1), 4);
        put_le(hd + 36, key_offset, 4);
        put_le(hd + 40, (uint32_t)w.nkeys, 4);
        put_le(hd + 44, crc_offset, 4);
        put_le(hd + 48, 512, 4);
        put_le(hd + 52, w.max_record, 4);
        put_le(hd + 56, audio_offset, 4);
        put_le(hd + 60, audio_bytes, 4);
        snprintf((char *)hd + 64, 64, "%s", base_name(chd_path));
        snprintf((char *)hd + 128, 32, "%s", TOOL);
        if (ldp_fseek64(w.f, 0) || fwrite(hd, 1, 512, w.f) != 512)
            status = 1;
    }
    if (fclose(w.f) != 0 && !status)
        status = 1;
    if (status == 1)
        logf_(h, "the conversion failed; nothing was written to %s", pld_path);
    if (status)
    {
        ldp_remove(part);
        if (status == 2)
            logf_(h, "cancelled; nothing was written to %s", pld_path);
    }
    else if (ldp_replace(part, pld_path) != 0)
    {
        logf_(h, "error: cannot rename %s to %s", part, pld_path);
        ldp_remove(part);
        status = 1;
    }
    free(part);

    if (!status)
    {
        const double el = ldp_seconds() - t0;
        uint64_t bytes = 0, best = 0, win = 0;
        for (size_t i = 0; i < w.nrec; i++)
        {
            bytes += w.sizes[i];
            win += w.sizes[i];
            if (i >= 24)
                win -= w.sizes[i - 24];
            if (win > best)
                best = win;
        }
        logf_(h, "done: %d pictures (%d-%d) in %.0f s", w.last - w.first + 1, w.first, w.last, el);
        logf_(h, "  records: %d key, %d delta, %d repeat, %d missing", w.types[0], w.types[1], w.types[2],
              w.types[3]);
        logf_(h, "  file: %.1f MB; pictures %.1f KB each on average, busiest second %.0f KB; sound %.1f MB, gain "
                 "%.2f",
              (double)(audio_offset + audio_bytes + 12.0 * w.nkeys + 4.0 * w.nrec) / 1048576.0,
              w.nrec ? bytes / 1024.0 / w.nrec : 0.0, best / 1024.0, audio_bytes / 1048576.0, gain);
        if (w.raised)
            logf_(h, "  %d segments coded at a lower quality (up to %.0f) to stay within %.0f KB per second",
                  w.raised, w.raised_q, s->max_rate_kb);
        if (crc_errors)
            logf_(h, "  warning: %u fields of the CHD failed their checksum", crc_errors);
    }
    free(pcm);
    free(w.keys);
    free(w.crcs);
    free(w.sizes);
    return status;
}

int ldp_describe(const char *chd_path, char *out, size_t outlen)
{
    ldp_chd *chd = ldp_chd_open(chd_path, out, outlen);
    if (!chd)
        return -1;
    ldp_chd_info(chd, out, outlen);
    ldp_chd_close(chd);
    return 0;
}

int ldp_dump_picture(const char *chd_path, int picture, const char *ppm, char *err, size_t errlen)
{
    ldp_chd *chd = ldp_chd_open(chd_path, err, errlen);
    if (!chd)
        return -1;
    uint8_t *rgb = (uint8_t *)malloc(3 * NPIX);
    int16_t *audio = (int16_t *)malloc(sizeof(int16_t) * 2 * LDP_CHD_MAX_SAMPLES);
    int n = 0, r = -1;
    if (rgb && audio)
    {
        r = ldp_chd_picture(chd, picture, rgb, audio, &n, err, errlen);
        if (r == 1)
        {
            snprintf(err, errlen, "picture %d is not on the disc", picture);
            r = -1;
        }
    }
    if (r == 0)
    {
        FILE *f = ldp_fopen(ppm, "wb");
        if (!f)
        {
            snprintf(err, errlen, "cannot create %s", ppm);
            r = -1;
        }
        else
        {
            fprintf(f, "P6\n%d %d\n255\n", W, H);
            fwrite(rgb, 3, NPIX, f);
            if (fclose(f))
                r = -1;
        }
    }
    free(rgb);
    free(audio);
    ldp_chd_close(chd);
    return r;
}
