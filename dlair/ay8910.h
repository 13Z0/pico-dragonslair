/*
 * General Instrument AY-3-8910 programmable sound generator.
 *
 * Three square-wave tone channels, one noise generator, the envelope
 * generator and the two I/O ports. Dragon's Lair uses it for its few beeps
 * (coin, accepted move, wrong move) and reads its DIP switches through the
 * ports; everything else comes from the laserdisc.
 *
 * Rendering is driven by the caller: ay_render() produces mono samples at the
 * output rate, so register writes can be placed in time by rendering up to
 * the write first.
 */
#ifndef AY8910_H
#define AY8910_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint8_t regs[16];
    uint8_t port_in[2]; /* values read from port A / B */

    /* generator state, in ticks of clock / 8 */
    uint16_t tone_count[3];
    uint8_t tone_out[3];
    uint16_t noise_count;
    uint32_t noise_lfsr;
    uint32_t env_count;
    int8_t env_step;     /* 15 .. 0, counting down */
    uint8_t env_attack;  /* 0x0f while rising, 0 while falling */
    uint8_t env_holding; /* the envelope has stopped */
    uint8_t env_level;

    int32_t dc_x, dc_y; /* DC blocker state: last input, output in Q8 */

    uint32_t tick_frac;  /* Q16 ticks carried between samples */
    uint32_t tick_step;  /* Q16 ticks per output sample */
} ay8910_t;

/* clock in Hz (2 MHz on Dragon's Lair), samplerate of the output */
void ay_init(ay8910_t *ay, uint32_t clock, uint32_t samplerate);
void ay_reset(ay8910_t *ay);
void ay_write(ay8910_t *ay, uint8_t reg, uint8_t value);
uint8_t ay_read(const ay8910_t *ay, uint8_t reg);

/* Adds n samples to out (mono, int16, saturating); level is the peak of one
 * channel at full volume. */
void ay_render(ay8910_t *ay, int16_t *out, int n, int level);

#ifdef __cplusplus
}
#endif

#endif
