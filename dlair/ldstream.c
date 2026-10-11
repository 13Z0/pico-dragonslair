/*
 * Streaming decoder for lair.pld. See ldstream.h.
 */
#include "ldstream.h"

#include <stdlib.h>
#include <string.h>

#include "dl_port.h"

enum
{
    ST_IDLE,
    ST_KEYS, /* a key lookup is due: read the key table entries of its group */
    ST_HEADER,
    ST_PALETTE,
    ST_ROWS,
    ST_FINISH,
    ST_STALLED, /* a read failed: nothing happens until the next seek or play */
};

/* block rows decoded per step: a quarter of a frame */
#define ROWS_PER_STEP 15

/* while playing, a decoder that is this many frames behind the display jumps
 * ahead instead of decoding every frame in between */
#define LATE_LIMIT 12

/* a search this close ahead of the picture in the buffer decodes on to it
 * instead of looking for a key record */
#define CONTINUE_LIMIT 8

/* with less sound than this ahead of the cursor (about 0.19 s at 22050 Hz),
 * the sound is read before anything else */
#define AUDIO_LOW 4096
#define AUDIO_CHUNK 2048

static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint32_t last_frame(const ldstream_t *s)
{
    return s->hdr.first_frame + s->hdr.frame_count - 1;
}

uint32_t ldstream_first_frame(const ldstream_t *s)
{
    return s->hdr.first_frame;
}

uint32_t ldstream_last_frame(const ldstream_t *s)
{
    return last_frame(s);
}

bool ldstream_has_frame(const ldstream_t *s, uint32_t frame)
{
    return frame >= s->hdr.first_frame && frame <= last_frame(s);
}

int ldstream_open(ldstream_t *s, const ldstream_io_t *io, uint32_t file_size, uint8_t *buf, uint32_t buf_size,
                  uint32_t abuf_size, uint8_t *frame, uint32_t samples_per_field)
{
    memset(s, 0, sizeof(*s));
    s->io = *io;
    s->file_size = file_size;
    s->frame = frame;
    s->buf_size = buf_size & ~511u;
    s->abuf_size = abuf_size & ~511u;
    s->read_chunk = 4096;
    s->buf_frame = LDS_NONE;
    s->target = LDS_NONE;
    s->key_for = LDS_NONE;
    s->a_pos = s->a_wait_frame = s->a_prep = LDS_NONE;
    for (int i = 0; i < 4; i++)
        s->hist[i].frame = LDS_NONE;
    for (int i = 0; i < 256; i++)
        s->ulaw[i] = pld_ulaw_decode((uint8_t)i);

    s->buf = buf;
    if (!s->buf)
    {
        s->buf = (uint8_t *)malloc(s->buf_size);
        s->buf_owned = true;
    }
    s->abuf = (uint8_t *)malloc(s->abuf_size);
    if (!s->buf || !s->abuf)
        return 3;
    if (file_size < PLD_HEADER_SIZE || io->read(io->ctx, 0, s->scratch, PLD_HEADER_SIZE) != PLD_HEADER_SIZE)
        return 1;
    const int ok = pld_parse_header(&s->hdr, s->scratch);
    if (ok < 0)
        return 4;
    if (!ok)
        return 2;
    const pld_header_t *h = &s->hdr;
    if ((uint64_t)h->key_offset + (uint64_t)h->key_count * PLD_KEY_ENTRY > file_size)
        return 2;

    /* the key table, read once: keep the picture number of every
     * LDS_KEY_GROUP-th key, and check the order of all of them */
    s->key_groups = (h->key_count + LDS_KEY_GROUP - 1) / LDS_KEY_GROUP;
    s->key_index = (uint32_t *)malloc(s->key_groups * sizeof(uint32_t));
    if (!s->key_index)
        return 3;
    uint8_t e[PLD_KEY_ENTRY];
    int ei = 0;
    uint32_t idx = 0, prev = 0;
    uint32_t ofs = h->key_offset & ~511u;
    uint32_t skip = h->key_offset - ofs;
    while (idx < h->key_count)
    {
        int r = io->read(io->ctx, ofs, s->scratch, sizeof(s->scratch));
        if (r <= (int)skip)
            return 1;
        for (uint32_t i = skip; i < (uint32_t)r && idx < h->key_count; i++)
        {
            e[ei++] = s->scratch[i];
            if (ei < PLD_KEY_ENTRY)
                continue;
            ei = 0;
            pld_key_t k;
            pld_parse_key(&k, e);
            if (k.offset < h->data_offset || k.offset >= h->audio_offset || k.audio >= h->audio_bytes ||
                (idx && k.frame <= prev))
                return 2;
            if (idx == 0)
            {
                s->pos = k.offset;
                s->rec_frame = k.frame;
                s->rec_audio = k.audio;
            }
            if (idx % LDS_KEY_GROUP == 0)
                s->key_index[idx / LDS_KEY_GROUP] = k.frame;
            prev = k.frame;
            idx++;
        }
        ofs += (uint32_t)r;
        skip = 0;
    }

    s->ring_start = s->ring_end = s->pos & ~511u;
    s->state = ST_IDLE;

    /* sound samples per output sample, Q16 */
    s->a_step = (uint32_t)(((uint64_t)h->audio_rate * 1000u * 65536u) / (59940ull * samples_per_field));
    return 0;
}

void ldstream_close(ldstream_t *s)
{
    free(s->key_index);
    free(s->abuf);
    if (s->buf_owned)
        free(s->buf);
    s->key_index = NULL;
    s->abuf = NULL;
    s->buf = NULL;
}

/* ---------------------------------------------------------------------------
 * Reading
 * ------------------------------------------------------------------------ */

/* Reads more of the records after the ring's end. Returns 1 after a read, 0
 * when nothing can be read now (ring full, end of the records, no budget), -1
 * on a read error. */
static int DL_HOT(fill)(ldstream_t *s, uint32_t budget)
{
    if (s->pos < s->ring_start || s->pos > s->ring_end)
        s->ring_start = s->ring_end = s->pos & ~511u; /* a jump: start afresh */
    else
        s->ring_start = s->pos & ~511u; /* what has been parsed is no longer needed */
    if (s->ring_end >= s->hdr.audio_offset)
        return 0;
    const uint32_t phys = s->ring_end % s->buf_size;
    uint32_t len = s->buf_size - (s->ring_end - s->ring_start); /* free */
    if (len > s->buf_size - phys)
        len = s->buf_size - phys; /* up to the end of the buffer */
    if (len > budget)
        len = budget;
    if (len > s->read_chunk)
        len = s->read_chunk;
    len &= ~511u;
    if (len == 0)
        return 0;
    const int r = s->io.read(s->io.ctx, s->ring_end, s->buf + phys, len);
    if (r <= 0)
        return -1;
    s->ring_end += (uint32_t)r;
    s->bytes_read += (uint32_t)r;
    s->reads++;
    return 1;
}

/* Makes [pos, pos + n) available: 1 when it is, 0 after a read (call again),
 * 2 when it needs a read that the budget does not allow, -1 when it cannot be
 * read. */
static int DL_HOT(need)(ldstream_t *s, uint32_t n, uint32_t budget)
{
    if (s->pos >= s->ring_start && s->pos + n <= s->ring_end)
        return 1;
    if (budget < 512)
        return 2;
    const int r = fill(s, budget);
    if (r > 0)
        return 0;
    return -1; /* no room for n bytes, or the end of the records */
}

/* n bytes at pos, all available, in one piece: in the ring, or copied out of
 * it when they wrap around its end (n is at most the scratch size) */
static const uint8_t *DL_HOT(at)(ldstream_t *s, uint32_t n)
{
    const uint32_t phys = s->pos % s->buf_size;
    if (phys + n <= s->buf_size)
        return s->buf + phys;
    const uint32_t first = s->buf_size - phys;
    memcpy(s->scratch, s->buf + phys, first);
    memcpy(s->scratch + first, s->buf, n - first);
    return s->scratch;
}

/* the sample the sound buffer has to follow: the cursor, or where it is
 * going to start */
static inline uint32_t audio_from(const ldstream_t *s)
{
    return s->a_pos != LDS_NONE ? s->a_pos : s->a_prep;
}

static inline bool audio_low(const ldstream_t *s)
{
    const uint32_t from = audio_from(s);
    if (from == LDS_NONE || from >= s->hdr.audio_bytes)
        return false;
    const uint32_t f = s->hdr.audio_offset + from;
    if (f < s->abuf_ofs || f >= s->abuf_ofs + s->abuf_len)
        return true;
    const uint32_t end = s->hdr.audio_offset + s->hdr.audio_bytes;
    const uint32_t have = s->abuf_ofs + s->abuf_len;
    return have < end && have - f < AUDIO_LOW;
}

/* Reads more of the sound after the sound buffer's end: 1 after a read, 0
 * when there is nothing to read now. */
static int DL_HOT(audio_fill)(ldstream_t *s, uint32_t budget)
{
    const uint32_t from = audio_from(s);
    if (from == LDS_NONE || from >= s->hdr.audio_bytes)
        return 0;
    const uint32_t f = s->hdr.audio_offset + from;
    if (f < s->abuf_ofs || f > s->abuf_ofs + s->abuf_len)
    {
        s->abuf_ofs = f & ~511u;
        s->abuf_len = 0;
    }
    else
    {
        const uint32_t used = (f & ~511u) - s->abuf_ofs;
        if (used && (used >= s->abuf_size / 2 || s->abuf_size - s->abuf_len < 512))
        {
            memmove(s->abuf, s->abuf + used, s->abuf_len - used);
            s->abuf_len -= used;
            s->abuf_ofs += used;
        }
    }
    const uint32_t ofs = s->abuf_ofs + s->abuf_len;
    if (ofs >= s->hdr.audio_offset + s->hdr.audio_bytes || (s->abuf_len & 511))
        return 0;
    uint32_t len = (s->abuf_size - s->abuf_len) & ~511u;
    if (len > (budget & ~511u))
        len = budget & ~511u;
    if (len > AUDIO_CHUNK)
        len = AUDIO_CHUNK;
    if (len == 0)
        return 0;
    const int r = s->io.read(s->io.actx, ofs, s->abuf + s->abuf_len, len);
    if (r <= 0)
        return 0;
    s->abuf_len += (uint32_t)r;
    s->bytes_read += (uint32_t)r;
    s->audio_reads++;
    return 1;
}

/* ---------------------------------------------------------------------------
 * Seeking
 * ------------------------------------------------------------------------ */

static void begin_seek(ldstream_t *s, uint32_t frame)
{
    s->key_for = frame;
    s->state = ST_KEYS;
}

/* the sound start of a picture decoded lately, or LDS_NONE */
static uint32_t audio_of(const ldstream_t *s, uint32_t frame)
{
    for (int i = 0; i < 4; i++)
        if (s->hist[i].frame == frame)
            return s->hist[i].audio;
    if (frame == s->rec_frame && s->state != ST_KEYS)
        return s->rec_audio;
    return LDS_NONE;
}

/* ST_KEYS: reads the key table entries of the group holding the key at or
 * before key_for, and moves there. */
static int key_lookup(ldstream_t *s, uint32_t budget)
{
    const pld_header_t *h = &s->hdr;
    const uint32_t frame = s->key_for;
    uint32_t lo = 0, hi = s->key_groups; /* the group: in [lo, hi) */
    while (hi - lo > 1)
    {
        const uint32_t mid = (lo + hi) / 2;
        if (s->key_index[mid] <= frame)
            lo = mid;
        else
            hi = mid;
    }
    const uint32_t first = lo * LDS_KEY_GROUP;
    uint32_t n = h->key_count - first;
    if (n > LDS_KEY_GROUP)
        n = LDS_KEY_GROUP;
    const uint32_t start = h->key_offset + first * PLD_KEY_ENTRY;
    const uint32_t a0 = start & ~511u;
    const uint32_t len = (start + n * PLD_KEY_ENTRY - a0 + 511) & ~511u;
    if (budget < len || len > sizeof(s->scratch))
        return 0;
    const int r = s->io.read(s->io.ctx, a0, s->scratch, len);
    if (r < (int)(start + n * PLD_KEY_ENTRY - a0))
    {
        s->state = ST_STALLED;
        return 0;
    }
    s->bytes_read += (uint32_t)r;
    s->reads++;

    pld_key_t best, k;
    pld_parse_key(&best, s->scratch + (start - a0));
    for (uint32_t i = 1; i < n; i++)
    {
        pld_parse_key(&k, s->scratch + (start - a0) + i * PLD_KEY_ENTRY);
        if (k.frame > frame)
            break;
        best = k;
    }
    s->key_for = LDS_NONE;
    s->state = ST_IDLE;
    /* already decoding past that key toward the frame: carry on */
    if (s->buf_frame != LDS_NONE && s->rec_frame == s->buf_frame + 1 && best.frame <= s->buf_frame &&
        s->rec_frame <= frame)
        return 1;
    /* the key record may already be in the ring, read ahead */
    s->pos = best.offset;
    s->rec_frame = best.frame;
    s->rec_audio = best.audio;
    s->buf_frame = LDS_NONE;
    return 1;
}

void ldstream_seek(ldstream_t *s, uint32_t frame)
{
    s->seeks++;
    s->target = frame;
    s->catchup = false;
    s->a_pos = LDS_NONE;
    s->a_wait_frame = LDS_NONE;
    s->a_prep = LDS_NONE;
    if (s->state == ST_STALLED)
        s->state = ST_IDLE;
    if (s->state == ST_IDLE && s->buf_frame == frame)
    {
        s->pic_presentable = true;
        s->a_prep = audio_of(s, frame);
        return;
    }
    /* carry on from the picture in the buffer when the target is close ahead */
    if (s->state == ST_IDLE && s->buf_frame != LDS_NONE && s->rec_frame == s->buf_frame + 1 &&
        s->buf_frame < frame && frame - s->buf_frame <= CONTINUE_LIMIT)
        return;
    begin_seek(s, frame);
}

bool ldstream_seek_done(const ldstream_t *s, uint32_t frame)
{
    return s->state == ST_IDLE && s->buf_frame == frame;
}

void ldstream_play(ldstream_t *s, uint32_t frame)
{
    if (s->state == ST_STALLED)
        s->state = ST_IDLE;
    if (s->buf_frame == frame && s->state == ST_IDLE)
    {
        s->target = LDS_NONE;
        s->catchup = false;
        s->pic_presentable = true;
        return;
    }
    if (s->buf_frame != LDS_NONE && s->buf_frame + 1 == frame && s->rec_frame == frame &&
        (s->state == ST_IDLE || s->state == ST_HEADER))
    {
        s->target = LDS_NONE;
        s->catchup = false;
        return;
    }
    const uint32_t a_pos = s->a_pos;
    ldstream_seek(s, frame);
    s->a_pos = a_pos; /* the sound cursor is the player's business */
    s->catchup = true;
}

/* the picture in the buffer is lost: start again at the key record before
 * the frame being decoded, without showing anything up to it */
static void recover(ldstream_t *s)
{
    const uint32_t frame = s->rec_frame;
    if (s->target == LDS_NONE || s->target < frame)
    {
        s->target = frame;
        s->catchup = true;
    }
    s->buf_frame = LDS_NONE;
    begin_seek(s, frame);
}

/* ---------------------------------------------------------------------------
 * The step
 * ------------------------------------------------------------------------ */

/* the sound buffer has room for a whole read of AUDIO_CHUNK bytes */
static inline bool audio_room(const ldstream_t *s)
{
    const uint32_t from = audio_from(s);
    if (from == LDS_NONE || from >= s->hdr.audio_bytes)
        return false;
    const uint32_t f = s->hdr.audio_offset + from;
    if (f < s->abuf_ofs || f >= s->abuf_ofs + s->abuf_len)
        return true;
    return s->abuf_size - (s->abuf_ofs + s->abuf_len - (f & ~511u)) >= AUDIO_CHUNK;
}

/* Nothing to decode: top up the sound (in whole chunks: every read costs a
 * command on the card), then read ahead in the records. */
static int idle_work(ldstream_t *s, uint32_t budget)
{
    if (budget < 512)
        return 0;
    if (audio_room(s) && budget >= AUDIO_CHUNK && audio_fill(s, budget) > 0)
        return 1;
    return fill(s, budget) > 0 ? 1 : 0;
}

int DL_HOT(ldstream_step)(ldstream_t *s, uint32_t display, bool playing, uint32_t max_read)
{
    const uint32_t budget = max_read < 512 ? 0 : max_read;
    int r;

    /* the sound comes first when it runs low */
    if (budget && audio_low(s) && audio_fill(s, budget) > 0)
        return 1;

    switch (s->state)
    {
    case ST_IDLE:
    {
        const uint32_t next = s->rec_frame;
        if (s->target != LDS_NONE)
        {
            if (s->buf_frame == s->target)
            {
                if (s->catchup)
                {
                    s->target = LDS_NONE;
                    s->catchup = false;
                }
                return idle_work(s, budget);
            }
            if (next > s->target)
            {
                begin_seek(s, s->target);
                return 0;
            }
        }
        else
        {
            if (s->buf_frame != LDS_NONE && s->pic_presentable && !s->pic_shown)
                return idle_work(s, budget); /* the picture waits for its turn on the screen */
            if (display != LDS_NONE && next > display + 1)
                return idle_work(s, budget); /* one frame ahead is enough */
            if (display != LDS_NONE && playing && next + LATE_LIMIT < display)
            {
                s->target = display + 2;
                s->catchup = true;
                begin_seek(s, s->target);
                return 0;
            }
        }
        if (next > last_frame(s))
            return idle_work(s, budget);
        s->silent = (s->target != LDS_NONE && next < s->target);
        s->state = ST_HEADER;
    }
    /* fall through */
    case ST_HEADER:
        r = need(s, PLD_RECORD_HEADER, budget);
        if (r != 1)
            break;
        pld_parse_record(&s->rec, at(s, PLD_RECORD_HEADER));
        {
            const pld_record_t *rec = &s->rec;
            const uint32_t body = (rec->type == PLD_KEY ? 512u : 0u) + rec->video_bytes;
            const bool chained = s->buf_frame != LDS_NONE && s->buf_frame + 1 == s->rec_frame;
            if (rec->type > PLD_MISSING || rec->record_bytes < PLD_RECORD_HEADER + body ||
                (rec->type != PLD_KEY && !chained) ||
                ((rec->type == PLD_KEY || rec->type == PLD_DELTA) && rec->video_bytes == 0))
            {
                s->format_errors++;
                recover(s);
                return 2;
            }
        }
        s->rec_start = s->pos;
        s->pos += PLD_RECORD_HEADER;
        s->sum = PLD_ADLER_INIT;
        if (s->rec.type == PLD_KEY || s->rec.type == PLD_DELTA)
            s->buf_frame = LDS_NONE; /* the picture is about to change */
        s->video_left = s->rec.video_bytes;
        s->row = 0;
        if (s->rec.type == PLD_KEY)
            s->state = ST_PALETTE;
        else
            s->state = s->video_left ? ST_ROWS : ST_FINISH;
        return 2;

    case ST_PALETTE:
        r = need(s, 512, budget);
        if (r != 1)
            break;
        {
            const uint8_t *p = at(s, 512);
            for (int i = 0; i < 256; i++)
                s->palette[i] = (uint16_t)(p[2 * i] | (p[2 * i + 1] << 8));
            s->sum = pld_adler32(s->sum, p, 512);
        }
        s->palette_serial++;
        s->pos += 512;
        s->state = ST_ROWS;
        return 2;

    case ST_ROWS:
    {
        int done = 0;
        r = 1;
        for (int k = 0; k < ROWS_PER_STEP && s->row < PLD_BLOCKS_Y; k++)
        {
            const uint32_t want = s->video_left < PLD_MAX_ROW_BYTES ? s->video_left : PLD_MAX_ROW_BYTES;
            r = need(s, want, budget);
            if (r != 1)
                break;
            const uint8_t *p = at(s, want);
            const int used = pld_decode_row(s->frame, s->row, p, want);
            if (used < 0)
            {
                s->format_errors++;
                recover(s);
                return 2;
            }
            s->sum = pld_adler32(s->sum, p, (uint32_t)used);
            s->pos += (uint32_t)used;
            s->video_left -= (uint32_t)used;
            s->row++;
            done++;
        }
        if (s->row == PLD_BLOCKS_Y)
            s->state = ST_FINISH;
        if (done || r == 1)
            return 2;
        break;
    }

    case ST_FINISH:
        if (s->video_left != 0)
        {
            s->format_errors++;
            recover(s);
            return 2;
        }
        if (s->sum != s->rec.checksum)
        {
            s->checksum_errors++;
            recover(s);
            return 2;
        }
        s->pos = s->rec_start + s->rec.record_bytes;
        if (s->rec.type == PLD_KEY || s->rec.type == PLD_DELTA)
            s->pic_serial++;
        s->buf_frame = s->rec_frame;
        s->pic_presentable = !s->silent;
        s->pic_shown = false;
        s->hist[s->rec_frame & 3].frame = s->rec_frame;
        s->hist[s->rec_frame & 3].audio = s->rec_audio;
        if (!s->silent && s->a_wait_frame == s->rec_frame)
        {
            s->a_pos = s->rec_audio;
            s->a_frac = 0;
            s->a_wait_frame = LDS_NONE;
        }
        if (s->target == s->rec_frame && !s->catchup)
            s->a_prep = s->rec_audio; /* a search found its frame: read its sound */
        s->rec_audio += s->rec.audio_samples;
        s->rec_frame++;
        s->frames_decoded++;
        s->state = ST_IDLE;
        return 2;

    case ST_KEYS:
        return key_lookup(s, budget);

    case ST_STALLED:
    default:
        return 0;
    }

    /* need() did not deliver */
    if (r == 0)
        return 1; /* a read was made */
    if (r == 2)
        return 0; /* no budget for the read */
    s->state = ST_STALLED;
    return 0;
}

bool ldstream_picture_due(const ldstream_t *s, uint32_t display)
{
    return display != LDS_NONE && s->buf_frame != LDS_NONE && s->pic_presentable && !s->pic_shown &&
           s->buf_frame <= display;
}

void ldstream_picture_shown(ldstream_t *s)
{
    s->pic_shown = true;
}

/* ---------------------------------------------------------------------------
 * Sound
 * ------------------------------------------------------------------------ */

void ldstream_audio_start(ldstream_t *s, uint32_t frame)
{
    s->a_pos = audio_of(s, frame);
    s->a_frac = 0;
    s->a_wait_frame = s->a_pos == LDS_NONE ? frame : LDS_NONE;
    s->a_prep = LDS_NONE;
}

void ldstream_audio_stop(ldstream_t *s)
{
    s->a_pos = LDS_NONE;
    s->a_wait_frame = LDS_NONE;
}

void DL_HOT(ldstream_audio_render)(ldstream_t *s, int16_t *out, int n, int gain_q8)
{
    if (s->a_pos == LDS_NONE)
        return;
    const uint32_t base = s->hdr.audio_offset;
    const uint32_t total = s->hdr.audio_bytes;
    bool gap = false;
    for (int i = 0; i < n; i++)
    {
        int32_t v = 0;
        const uint32_t f = base + s->a_pos;
        if (f >= s->abuf_ofs && f + 1 < s->abuf_ofs + s->abuf_len)
        {
            const uint8_t *p = s->abuf + (f - s->abuf_ofs);
            const int32_t s0 = s->ulaw[p[0]], s1 = s->ulaw[p[1]];
            v = s0 + (((s1 - s0) * (int32_t)s->a_frac) >> 16);
        }
        else
        {
            gap = true;
        }
        int32_t o = out[i] + ((v * gain_q8) >> 8);
        if (o > 32767)
            o = 32767;
        else if (o < -32768)
            o = -32768;
        out[i] = (int16_t)o;

        s->a_frac += s->a_step;
        s->a_pos += s->a_frac >> 16;
        s->a_frac &= 0xffff;
        if (s->a_pos >= total)
        {
            s->a_pos = LDS_NONE;
            break;
        }
    }
    if (gap)
        s->audio_gaps++;
}
