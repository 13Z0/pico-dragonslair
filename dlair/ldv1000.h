/*
 * Pioneer LD-V1000 laserdisc player, command level.
 *
 * The game writes one command byte per video field and reads a status byte
 * back. Digits collect a frame number, F7 searches to it, FD plays, F4/FC
 * switch the audio channels, and so on. The status byte carries a READY flag
 * in bit 7: a command is taken only while it is set, and 0xFF (NO ENTRY) sets
 * it again.
 *
 * The timing of the status and command strobes, which tell the game when to
 * read and write, is part of the machine (dl_machine.c).
 */
#ifndef LDV1000_H
#define LDV1000_H

#include <stdint.h>

#include "ldplayer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LDV1000_STACK_SIZE 10

typedef struct
{
    uint8_t output; /* status, bit 7 = READY */
    uint8_t stack[LDV1000_STACK_SIZE];
    int sp;
    char digits[6]; /* the last five digits entered, NUL when none */
    uint8_t search_pending;
    uint8_t temp_mute; /* Forward 1X plays without sound until Play */
    uint32_t autostop;
    ldplayer_t *ldp;
} ldv1000_t;

void ldv_init(ldv1000_t *l, ldplayer_t *ldp);
void ldv_reset(ldv1000_t *l);
uint8_t ldv_read(ldv1000_t *l);
void ldv_write(ldv1000_t *l, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif
