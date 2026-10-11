/*
 * The scoreboard overlay. The US cabinet shows score, lives and credits on a
 * separate LED panel; here they are drawn in a dark strip along the top of the
 * picture:
 *
 *   1UP 000000  L5    CREDITS 00    2UP 000000  L5
 *
 * The strip is opaque, so it can be redrawn at any time without the picture
 * under it. It has its own 5x7 font, so the host harness draws the same thing.
 */
#ifndef SCOREBOARD_H
#define SCOREBOARD_H

#include <stdint.h>

#include "dlair.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SB_HEIGHT 12 /* lines covered at the top of the screen */

enum
{
    SB_RGB555 = 0, /* HSTX */
    SB_RGB444 = 1, /* PicoDVI */
};

/* Draws the strip into a 320-pixel wide 16-bit framebuffer. */
void sb_draw(const dl_leds_t *leds, uint16_t *fb, int stride, int format);

#ifdef __cplusplus
}
#endif

#endif
