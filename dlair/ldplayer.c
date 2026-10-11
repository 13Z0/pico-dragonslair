/*
 * Laserdisc transport. See ldplayer.h.
 *
 * Search, play and pause follow Hypseus' ldp.cpp (GPL-2.0-or-later,
 * Copyright (C) 2001 Matt Ownby): a search while another one runs is refused,
 * a search to the frame already shown during a still completes at once, Play
 * while playing and Pause while not playing are ignored.
 */
#include "ldplayer.h"

#include <string.h>

#include "dl_port.h"

static inline void trace(ldplayer_t *p, const char *what, uint32_t value)
{
    if (p->trace)
        p->trace(p->trace_ctx, what, value);
}

void ldp_init(ldplayer_t *p, ldstream_t *disc, uint32_t first, uint32_t last)
{
    void (*t)(void *, const char *, uint32_t) = p->trace;
    void *tctx = p->trace_ctx;
    memset(p, 0, sizeof(*p));
    p->trace = t;
    p->trace_ctx = tctx;
    p->disc = disc;
    p->first = disc ? ldstream_first_frame(disc) : first;
    p->last = disc ? ldstream_last_frame(disc) : last;
    p->state = LDP_PARKED;
    p->frame = 0;
    p->audio1 = p->audio2 = true;
}

bool ldp_search(ldplayer_t *p, uint32_t frame)
{
    trace(p, "search", frame);
    if (p->state == LDP_SEARCHING)
        return false;
    if (frame < p->first || frame > p->last)
        return false;
    if (p->state == LDP_PAUSED && frame == p->frame)
        return true;
    p->target = frame;
    p->state = LDP_SEARCHING;
    if (p->disc)
        ldstream_seek(p->disc, frame);
    return true;
}

void ldp_play(ldplayer_t *p)
{
    trace(p, "play", p->frame);
    if (p->state == LDP_PLAYING || p->state == LDP_SEARCHING)
        return;
    if (p->frame < p->first)
        p->frame = p->first;
    p->play_start = p->frame;
    p->fields = 0;
    p->waiting_vsync = true;
    p->state = LDP_PLAYING;
    if (p->disc)
        ldstream_play(p->disc, p->frame);
}

void ldp_pause(ldplayer_t *p)
{
    trace(p, "pause", p->frame);
    if (p->state == LDP_PLAYING)
        p->state = LDP_PAUSED;
}

void ldp_skip(ldplayer_t *p, uint32_t frames)
{
    trace(p, "skip", frames);
    if (p->state != LDP_PLAYING)
        return;
    uint32_t to = p->frame + frames;
    if (to > p->last)
        to = p->last;
    p->frame = to;
    p->play_start = to;
    p->fields = 0;
    p->waiting_vsync = true;
    if (p->disc)
        ldstream_play(p->disc, to);
}

ldp_state_t ldp_state(ldplayer_t *p)
{
    if (p->state == LDP_SEARCHING && (!p->disc || ldstream_seek_done(p->disc, p->target)))
    {
        p->frame = p->target;
        p->state = LDP_PAUSED;
        trace(p, "found", p->frame);
    }
    return p->state;
}

void DL_HOT(ldp_vsync)(ldplayer_t *p)
{
    ldp_state(p);
    if (p->state != LDP_PLAYING)
        return;
    if (p->waiting_vsync)
    {
        /* frames and sound start counting now */
        p->waiting_vsync = false;
        p->fields = 0;
        if (p->disc)
            ldstream_audio_start(p->disc, p->play_start);
    }
    else
    {
        p->fields++;
    }
    uint32_t f = p->play_start + (2 * p->fields + 1) / 5;
    if (f > p->last)
    {
        /* the end of the disc: hold the last frame */
        f = p->last;
        p->state = LDP_PAUSED;
    }
    p->frame = f;
}

uint32_t ldp_display_frame(const ldplayer_t *p)
{
    switch (p->state)
    {
    case LDP_PLAYING:
    case LDP_PAUSED:
        return p->frame;
    default:
        return LDS_NONE;
    }
}

bool ldp_audio_on(const ldplayer_t *p)
{
    return p->state == LDP_PLAYING && !p->waiting_vsync && (p->audio1 || p->audio2);
}
