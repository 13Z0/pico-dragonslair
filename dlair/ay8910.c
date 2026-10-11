/*
 * AY-3-8910 sound generator. See ay8910.h.
 *
 * The generators run in ticks of clock / 8: a tone output toggles every
 * `period` ticks (clock / (16 * period) Hz), the noise shift register and the
 * envelope step every 2 * `period` ticks. Each output sample is the average of
 * the ticks it covers, then a DC blocker removes the offset of the chip's
 * unipolar output. The envelope follows MAME's ay8910.cpp (16-step chips).
 */
#include "ay8910.h"

#include <string.h>

#include "dl_port.h"

/* 3 dB per step, in Q15; level 0 is silent */
static const uint16_t vol_table[16] = {
    0, 182, 257, 364, 515, 728, 1029, 1456, 2059, 2912, 4118, 5793, 8192, 11585, 16384, 23170,
};

void ay_init(ay8910_t *ay, uint32_t clock, uint32_t samplerate)
{
    memset(ay, 0, sizeof(*ay));
    ay->tick_step = (uint32_t)((((uint64_t)clock / 8) << 16) / samplerate);
    ay->port_in[0] = ay->port_in[1] = 0xff;
    ay_reset(ay);
}

void ay_reset(ay8910_t *ay)
{
    uint8_t a = ay->port_in[0], b = ay->port_in[1];
    uint32_t step = ay->tick_step;
    memset(ay, 0, sizeof(*ay));
    ay->port_in[0] = a;
    ay->port_in[1] = b;
    ay->tick_step = step;
    ay->noise_lfsr = 1;
    ay->regs[7] = 0xff; /* everything off */
}

static const uint8_t reg_mask[16] = {
    0xff, 0x0f, 0xff, 0x0f, 0xff, 0x0f, 0x1f, 0xff, 0x1f, 0x1f, 0x1f, 0xff, 0xff, 0x0f, 0xff, 0xff,
};

static inline uint8_t env_shape_hold(const ay8910_t *ay)
{
    return (ay->regs[13] & 0x08) ? (ay->regs[13] & 0x01) : 1;
}
static inline uint8_t env_shape_alt(const ay8910_t *ay)
{
    return (ay->regs[13] & 0x08) ? (ay->regs[13] & 0x02) : ((ay->regs[13] & 0x04) != 0);
}

void ay_write(ay8910_t *ay, uint8_t reg, uint8_t value)
{
    reg &= 0x0f;
    ay->regs[reg] = value & reg_mask[reg];
    if (reg == 13)
    {
        ay->env_attack = (value & 0x04) ? 0x0f : 0x00;
        ay->env_holding = 0;
        ay->env_step = 0x0f;
        ay->env_count = 0;
        ay->env_level = (uint8_t)(ay->env_step ^ ay->env_attack);
    }
}

uint8_t ay_read(const ay8910_t *ay, uint8_t reg)
{
    reg &= 0x0f;
    if (reg == 14)
        return ay->port_in[0];
    if (reg == 15)
        return ay->port_in[1];
    return ay->regs[reg];
}

static inline uint16_t period(const ay8910_t *ay, int lo)
{
    uint16_t p = (uint16_t)(ay->regs[lo] | ((ay->regs[lo + 1] & 0x0f) << 8));
    return p ? p : 1;
}

static void DL_HOT(env_tick)(ay8910_t *ay)
{
    if (ay->env_holding)
        return;
    int8_t step = (int8_t)(ay->env_step - 1);
    if (step < 0)
    {
        if (env_shape_alt(ay))
            ay->env_attack ^= 0x0f;
        if (env_shape_hold(ay))
        {
            ay->env_holding = 1;
            step = 0;
        }
        else
        {
            step = 0x0f;
        }
    }
    ay->env_step = step;
    ay->env_level = (uint8_t)(step ^ ay->env_attack);
}

void DL_HOT(ay_render)(ay8910_t *ay, int16_t *out, int n, int level)
{
    const uint16_t tp[3] = {period(ay, 0), period(ay, 2), period(ay, 4)};
    const uint16_t np = (uint16_t)((ay->regs[6] & 0x1f) ? (ay->regs[6] & 0x1f) * 2 : 2);
    uint32_t ep = (uint32_t)(ay->regs[11] | (ay->regs[12] << 8));
    ep = ep ? ep * 2 : 2;
    const uint8_t mix = ay->regs[7];

    for (int i = 0; i < n; i++)
    {
        ay->tick_frac += ay->tick_step;
        int ticks = (int)(ay->tick_frac >> 16);
        ay->tick_frac &= 0xffff;
        int32_t acc = 0;
        for (int t = 0; t < ticks; t++)
        {
            for (int c = 0; c < 3; c++)
            {
                if (++ay->tone_count[c] >= tp[c])
                {
                    ay->tone_count[c] = 0;
                    ay->tone_out[c] ^= 1;
                }
            }
            if (++ay->noise_count >= np)
            {
                ay->noise_count = 0;
                uint32_t bit = (ay->noise_lfsr ^ (ay->noise_lfsr >> 3)) & 1;
                ay->noise_lfsr = (ay->noise_lfsr >> 1) | (bit << 16);
            }
            if (++ay->env_count >= ep)
            {
                ay->env_count = 0;
                env_tick(ay);
            }
            const uint8_t noise = (uint8_t)(ay->noise_lfsr & 1);
            for (int c = 0; c < 3; c++)
            {
                const int tone_on = ay->tone_out[c] | ((mix >> c) & 1);
                const int noise_on = noise | ((mix >> (c + 3)) & 1);
                if (tone_on && noise_on)
                {
                    const uint8_t v = ay->regs[8 + c];
                    acc += vol_table[(v & 0x10) ? ay->env_level : (v & 0x0f)];
                }
            }
        }
        int32_t x = ticks ? acc / ticks : 0; /* 0 .. 3 * 23170 */
        x = (x * level) >> 15;

        /* DC blocker, y = x - x1 + (1 - 1/256) * y1, with y kept in Q8 so that
         * it decays all the way to zero instead of stopping at a small offset */
        ay->dc_y += ((x - ay->dc_x) << 8) - (ay->dc_y >> 8);
        ay->dc_x = x;
        const int32_t y = ay->dc_y >> 8;

        int32_t s = out[i] + y;
        if (s > 32767)
            s = 32767;
        else if (s < -32768)
            s = -32768;
        out[i] = (int16_t)s;
    }
}
