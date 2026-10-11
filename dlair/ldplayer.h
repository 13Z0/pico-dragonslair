/*
 * The laserdisc transport: what the player is doing (parked, playing, still,
 * searching) and which frame is under the laser.
 *
 * The timing follows Hypseus' ldp.cpp for a 23.976 fps disc on a 59.94 Hz
 * field clock: counting starts at the first vertical sync after Play, and the
 * frame advances on a 2:3 field cadence, frame = start + (2 * fields + 1) / 5.
 * Dragon's Lair never asks the player for the frame number, so only the rate
 * and the starting point matter.
 *
 * The pictures come from an ldstream_t. Without one (disc == NULL) the player
 * still keeps the frame numbers and completes every search at once, which is
 * enough to run the game logic in the host harness.
 */
#ifndef LDPLAYER_H
#define LDPLAYER_H

#include <stdbool.h>
#include <stdint.h>

#include "ldstream.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    LDP_PARKED,
    LDP_PLAYING,
    LDP_PAUSED,
    LDP_SEARCHING,
} ldp_state_t;

typedef struct ldplayer
{
    ldp_state_t state;
    uint32_t frame;      /* the frame under the laser */
    uint32_t play_start; /* frame where play started */
    uint32_t fields;     /* fields since the first vertical sync after Play */
    bool waiting_vsync;
    uint32_t target;     /* search target */
    bool audio1, audio2; /* audio channel switches */

    ldstream_t *disc;
    uint32_t first, last; /* frame range of the disc */

    /* trace hook for the host harness, may be NULL */
    void (*trace)(void *ctx, const char *what, uint32_t value);
    void *trace_ctx;
} ldplayer_t;

void ldp_init(ldplayer_t *p, ldstream_t *disc, uint32_t first, uint32_t last);

/* Returns false when the search is refused (another one is running, or the
 * frame is not on the disc). */
bool ldp_search(ldplayer_t *p, uint32_t frame);
void ldp_play(ldplayer_t *p);
void ldp_pause(ldplayer_t *p);
void ldp_skip(ldplayer_t *p, uint32_t frames);

/* The current state; a search that has reached its frame becomes a still. */
ldp_state_t ldp_state(ldplayer_t *p);

/* Called at every vertical sync, before the CPU runs the field. */
void ldp_vsync(ldplayer_t *p);

/* The frame the screen shows now, or LDS_NONE while parked. During a search
 * the last picture stays on the screen, as in Hypseus. */
uint32_t ldp_display_frame(const ldplayer_t *p);

/* The disc sound is playing. */
bool ldp_audio_on(const ldplayer_t *p);

#ifdef __cplusplus
}
#endif

#endif
