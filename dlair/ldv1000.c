/*
 * Pioneer LD-V1000, command level. See ldv1000.h.
 *
 * Ported from Hypseus' src/ldp-in/ldv1000.cpp, part of DAPHNE:
 *   Copyright (C) 2001 Matt Ownby, with contributions by Mark Broadhead.
 *   Licensed under the GNU General Public License, version 2 or (at your
 *   option) any later version.
 * Searches use Hypseus' non-blocking path with instant seeking, as set up for
 * Dragon's Lair: the status reads 0x50 (busy) until the player has the frame,
 * then 0xD0, or 0x90 when the search failed. The game-specific commands of
 * Badlands are left out.
 */
#include "ldv1000.h"

#include <string.h>

void ldv_init(ldv1000_t *l, ldplayer_t *ldp)
{
    memset(l, 0, sizeof(*l));
    l->ldp = ldp;
    ldv_reset(l);
}

void ldv_reset(ldv1000_t *l)
{
    memset(l->digits, 0, sizeof(l->digits));
    memset(l->stack, 0, sizeof(l->stack));
    l->sp = 0;
    l->autostop = 0;
    l->search_pending = 0;
    l->temp_mute = 0;
    l->output = 0xfc; /* parked and ready */
}

static void clear_digits(ldv1000_t *l)
{
    memset(l->digits, 0, sizeof(l->digits));
}

static void add_digit(ldv1000_t *l, char d)
{
    for (int i = 0; i < 4; i++)
        l->digits[i] = l->digits[i + 1] ? l->digits[i + 1] : '0';
    l->digits[4] = d;
}

static uint32_t buffered_frame(const ldv1000_t *l)
{
    uint32_t v = 0;
    for (int i = 0; i < 5; i++)
        if (l->digits[i] >= '0' && l->digits[i] <= '9')
            v = v * 10 + (uint32_t)(l->digits[i] - '0');
    return v;
}

static void push(ldv1000_t *l, uint8_t v)
{
    if (l->sp < LDV1000_STACK_SIZE - 1)
        l->stack[l->sp++] = v;
}

/* Audio channel switch: toggles without a digit, else odd = on, even = off. */
static void audio_switch(ldv1000_t *l, bool *channel)
{
    if (!l->digits[4])
    {
        *channel = !*channel;
    }
    else
    {
        *channel = ((l->digits[4] - '0') & 1) != 0;
        clear_digits(l);
    }
}

uint8_t ldv_read(ldv1000_t *l)
{
    if (l->sp > 0)
        return l->stack[--l->sp];

    if (l->search_pending)
    {
        l->output = 0x50; /* busy searching */
        if (ldp_state(l->ldp) == LDP_PAUSED)
        {
            l->output = 0xd0;
            l->search_pending = 0;
        }
    }
    else if ((l->output & 0x7f) == 0x54)
    {
        /* auto stop */
        if (l->ldp->frame >= l->autostop)
        {
            ldp_pause(l->ldp);
            l->output = (uint8_t)((l->output & 0x80) | 0x65);
            l->autostop = 0;
        }
    }
    return l->output;
}

void ldv_write(ldv1000_t *l, uint8_t value)
{
    ldplayer_t *p = l->ldp;

    if (!(l->output & 0x80))
    {
        /* not ready: only NO ENTRY makes it ready again */
        if (value == 0xff)
            l->output |= 0x80;
        else
            l->output &= 0x7f;
        return;
    }

    l->output &= 0x7f;
    switch (value)
    {
    case 0xbf: /* clear */
        clear_digits(l);
        break;
    case 0x3f: add_digit(l, '0'); break;
    case 0x0f: add_digit(l, '1'); break;
    case 0x8f: add_digit(l, '2'); break;
    case 0x4f: add_digit(l, '3'); break;
    case 0x2f: add_digit(l, '4'); break;
    case 0xaf: add_digit(l, '5'); break;
    case 0x6f: add_digit(l, '6'); break;
    case 0x1f: add_digit(l, '7'); break;
    case 0x9f: add_digit(l, '8'); break;
    case 0x5f: add_digit(l, '9'); break;
    case 0xf4: /* audio 1 */
        audio_switch(l, &p->audio1);
        break;
    case 0xfc: /* audio 2 */
        audio_switch(l, &p->audio2);
        break;
    case 0xa0: /* play at 0X */
        ldp_pause(p);
        break;
    case 0xa1: /* play at 1/4X, 1/2X: not used by Dragon's Lair */
    case 0xa2:
        break;
    case 0xa3: /* forward 1X: from a stop it plays without sound until Play */
        if (ldp_state(p) != LDP_PLAYING)
        {
            l->temp_mute = 1;
            p->audio1 = p->audio2 = false;
            ldp_play(p);
            l->output = 0x2e;
        }
        break;
    case 0xa4: /* 2X .. 5X: not used by Dragon's Lair */
    case 0xa5:
    case 0xa6:
    case 0xa7:
        break;
    case 0xf9: /* reject: parked, not ready */
        l->output = 0x7c;
        break;
    case 0xf3: /* auto stop at the entered frame, and play */
        l->autostop = buffered_frame(l);
        clear_digits(l);
        ldp_play(p);
        l->output = 0x54;
        break;
    case 0xfd: /* play */
        ldp_play(p);
        if (l->temp_mute)
        {
            l->temp_mute = 0;
            p->audio1 = p->audio2 = true;
        }
        l->output = 0x64;
        break;
    case 0xfe: /* step reverse: a still */
    case 0xfb: /* stop: a still as well */
        ldp_pause(p);
        l->output = 0x65;
        break;
    case 0xf7: /* search */
        if (!ldp_search(p, buffered_frame(l)))
        {
            l->output = 0x90;
        }
        else
        {
            l->search_pending = 1;
            l->output = 0x50;
        }
        clear_digits(l);
        break;
    case 0xc2: /* current frame, five ASCII digits, high digit first */
    {
        uint32_t f = p->frame;
        char s[5];
        for (int i = 4; i >= 0; i--, f /= 10)
            s[i] = (char)('0' + f % 10);
        for (int i = 4; i >= 0; i--)
            push(l, (uint8_t)s[i]);
        break;
    }
    case 0xb1: /* skip forward 10 .. 100 (the LD-V1000 adds one) */
    case 0xb2:
    case 0xb3:
    case 0xb4:
    case 0xb5:
    case 0xb6:
    case 0xb7:
    case 0xb8:
    case 0xb9:
    case 0xba:
        ldp_skip(p, 10u * (value & 0x0f) + 1);
        break;
    case 0xcd: /* display disable: the frame number on screen, not emulated */
    case 0xce: /* display enable */
        break;
    case 0xff: /* no entry */
        l->output |= 0x80;
        break;
    default:
        break;
    }
}
