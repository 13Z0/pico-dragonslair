/*
 * Dragon's Lair arcade board. See dlair.h.
 */
#include "dlair.h"

#include <string.h>

#include "dl_port.h"

/* cycles from the start of a field (vertical sync) to the strobe edges,
 * Hypseus' ldv1000.cpp at 4 MHz: vsync 27.1 us, status strobe 500 us after it
 * for 26 us, the command strobe 54 us later for 25 us */
#define T_STATUS_ON 2108
#define T_STATUS_OFF 2212
#define T_COMMAND_ON 2428
#define T_COMMAND_OFF 2528

/* Q8 CPU cycles per field: 4 MHz / 59.94 Hz = 66733.4 */
#define FIELD_CYCLES_Q8 ((uint32_t)(((uint64_t)DL_CPU_HZ * 1000u << 8) / DL_FIELD_MHZ))

/* nothing is fitted at 0x8000-0x9fff on the F2 board */
static const uint8_t empty_page[256];

/* the CPU's position in the field, also in the middle of z80_run */
static inline uint32_t now(const dl_machine_t *m)
{
    int32_t p = (int32_t)m->seg_end - m->cpu.icount;
    return p < 0 ? 0 : (uint32_t)p;
}

/* Idle skipping. Most of the time the program waits: its main loop polls the
 * joystick port, and its laserdisc routines poll the strobes. When the CPU
 * reads the same port from the same instruction twice, and in between no
 * register (except R) and no byte of RAM or of the board has changed, it is
 * in a loop that repeats unchanged until something from outside changes: a
 * strobe edge, the IRQ, or the next field. Those happen only at the end of the
 * CPU's current run (see dl_run_field), so the rest of the run is skipped.
 * The R register is advanced by what the skipped turns of the loop would have
 * added (the game uses it as a source of chance), so the program behaves as
 * before; only the loop's position at the next event can differ.
 * DL_IDLE_SKIP=0 switches it off. */
#ifndef DL_IDLE_SKIP
#define DL_IDLE_SKIP 1
#endif

static inline void idle_snapshot(const z80_t *cpu, dl_idle_t *s)
{
    s->pc = cpu->pc.w;
    s->sp = cpu->sp.w;
    s->af = cpu->af.w;
    s->bc = cpu->bc.w;
    s->de = cpu->de.w;
    s->hl = cpu->hl.w;
    s->ix = cpu->ix.w;
    s->iy = cpu->iy.w;
    s->wz = cpu->wz.w;
    s->alt[0] = cpu->af2.w;
    s->alt[1] = cpu->bc2.w;
    s->alt[2] = cpu->de2.w;
    s->alt[3] = cpu->hl2.w;
    s->flags[0] = cpu->f.s_val;
    s->flags[1] = cpu->f.z_val;
    s->flags[2] = cpu->f.yx_val;
    s->flags[3] = cpu->f.h_val;
    s->flags[4] = cpu->f.pv_val;
    s->flags[5] = (uint8_t)((cpu->f.n ? 1 : 0) | (cpu->f.c ? 2 : 0));
    s->flags[6] = cpu->f.q;
    s->flags[7] = (uint8_t)(cpu->iff1 | (cpu->iff2 << 1) | (cpu->halt << 2) | (cpu->im << 3));
}

static inline void DL_HOT(idle_check)(dl_machine_t *m)
{
#if DL_IDLE_SKIP
    z80_t *cpu = &m->cpu;
    dl_idle_t now_state;
    idle_snapshot(cpu, &now_state);
    const uint32_t t = now(m);
    if (!m->dirty && cpu->prvpc.w == m->idle_pc && !memcmp(&now_state, &m->idle_state, sizeof(now_state)) &&
        t > m->idle_t && cpu->icount > 0)
    {
        /* one turn of the loop took t - idle_t cycles and advanced R by this */
        const uint32_t turn = t - m->idle_t;
        const uint8_t dr = (uint8_t)((cpu->r - m->idle_r) & 0x7f);
        const uint32_t turns = (uint32_t)cpu->icount / turn;
        cpu->r = (uint8_t)(cpu->r + turns * dr);
        m->idle_cycles += (uint32_t)cpu->icount;
        cpu->icount = 0; /* the run ends after this instruction */
    }
    m->idle_pc = cpu->prvpc.w;
    m->idle_state = now_state;
    m->idle_r = cpu->r;
    m->idle_t = t;
    m->dirty = 0;
#else
    (void)m;
#endif
}

static inline void trace(dl_machine_t *m, const char *what, uint32_t value)
{
    if (m->trace)
        m->trace(m->trace_ctx, what, value);
}

/* renders the AY up to the CPU's position, so register writes land in time */
static void DL_HOT(ay_catch_up)(dl_machine_t *m)
{
    if (!m->audio)
        return;
    uint64_t pos = now(m);
    if (pos > m->audio_field_cycles)
        pos = m->audio_field_cycles;
    int due = (int)((pos * (uint64_t)m->audio_n) / m->audio_field_cycles);
    if (due > m->audio_done)
    {
        ay_render(&m->ay, m->audio + m->audio_done, due - m->audio_done, m->ay_level);
        m->audio_done = due;
    }
}

static uint8_t DL_HOT(mem_read)(z80_t *cpu, uint16_t a)
{
    dl_machine_t *m = (dl_machine_t *)cpu->user;
    if (a < 0xc000 || a >= 0xe000)
        return 0x00;
    switch (a & 0x38)
    {
    case 0x00: /* AY-3-8910 data; registers 14 and 15 are the DIP switches */
        idle_check(m);
        return ay_read(&m->ay, m->ay_latch);
    case 0x08: /* P1: joystick and sword, active low */
        idle_check(m);
        return (uint8_t)(~m->inputs & 0x1f) | 0xe0;
    case 0x10: /* SYSTEM: start and coin active low, the two strobes */
        idle_check(m);
        return (uint8_t)((~(m->inputs >> 8) & 0x0f) | 0x30 | m->strobes);
    case 0x20: /* laserdisc status: reading it can change the player's state */
    {
        uint8_t v = ldv_read(&m->ldv);
        m->dirty = 1;
        trace(m, "ld_read", v);
        return v;
    }
    default:
        return 0xff;
    }
}

static void DL_HOT(mem_write)(z80_t *cpu, uint16_t a, uint8_t v)
{
    dl_machine_t *m = (dl_machine_t *)cpu->user;
    if (a >= 0xa000 && a < 0xc000)
    {
        /* RAM (mirrored), written here rather than through the page table so
         * that a change can be seen: see idle_check() */
        uint8_t *p = &m->ram[a & 0x7ff];
        if (*p != v)
        {
            *p = v;
            m->dirty = 1;
        }
        return;
    }
    if (a < 0xe000)
        return; /* ROM, or nothing */
    m->dirty = 1;
    switch (a & 0x38)
    {
    case 0x00: /* AY data */
        ay_catch_up(m);
        ay_write(&m->ay, m->ay_latch, v);
        break;
    case 0x08: /* misc: coin counters, laserdisc bus direction */
        break;
    case 0x10: /* AY register select */
        m->ay_latch = v & 0x0f;
        break;
    case 0x20: /* laserdisc command, sent straight to the player as in Hypseus */
        trace(m, "ld_write", v);
        ldv_write(&m->ldv, v);
        break;
    case 0x30: /* scoreboard digits 8-15 */
    case 0x38: /* scoreboard digits 0-7 */
    {
        if (v == 0xcc)
            break; /* Space Ace's skill lamps */
        const int d = ((a & 0x38) == 0x38 ? 0 : 8) + (a & 7);
        if (m->leds.digit[d] != (v & 0x0f))
        {
            m->leds.digit[d] = v & 0x0f;
            m->leds.serial++;
        }
        break;
    }
    default:
        break;
    }
}

static uint8_t io_read(z80_t *cpu, uint16_t port)
{
    (void)cpu;
    (void)port;
    return 0xff; /* no I/O space on the US board */
}

static void io_write(z80_t *cpu, uint16_t port, uint8_t v)
{
    (void)cpu;
    (void)port;
    (void)v;
}

static void ld_trace(void *ctx, const char *what, uint32_t value)
{
    dl_machine_t *m = (dl_machine_t *)ctx;
    trace(m, what, value);
}

void dl_init(dl_machine_t *m, dl_roms_t *roms, ldstream_t *disc, bool fastboot, uint32_t samplerate)
{
    void (*t)(void *, const char *, uint32_t) = m->trace;
    void *tctx = m->trace_ctx;
    memset(m, 0, sizeof(*m));
    m->trace = t;
    m->trace_ctx = tctx;

    m->rom = roms->maincpu;
    if (fastboot)
    {
        /* Hypseus' fast boot: two delays of about 15 s each at power-on,
         * while the disc plays its opening, become zero */
        m->rom[0x121b] = 0x00;
        m->rom[0x1235] = 0x00;
    }

    m->dsw1 = DL_DSW1_DEFAULT;
    m->dsw2 = DL_DSW2_DEFAULT;
    ay_init(&m->ay, DL_AY_HZ, samplerate);
    m->ay_level = 10000;

    m->ldp.trace = ld_trace;
    m->ldp.trace_ctx = m;
    /* without a disc file, a disc of the size of Dragon's Lair's */
    ldp_init(&m->ldp, disc, 1, 31794);
    ldv_init(&m->ldv, &m->ldp);

    z80_t *cpu = &m->cpu;
    z80_init(cpu);
    cpu->user = m;
    cpu->read = mem_read;
    cpu->write = mem_write;
    cpu->in = io_read;
    cpu->out = io_write;
    for (int p = 0; p < 0x80; p++)
        cpu->rd_page[p] = m->rom + (p << 8); /* 32 KB: dl_roms_t.maincpu */
    for (int p = 0x80; p < 0xa0; p++)
        cpu->rd_page[p] = empty_page;
    for (int p = 0xa0; p < 0xc0; p++)
    {
        cpu->rd_page[p] = m->ram + ((p & 0x07) << 8);
#if !DL_IDLE_SKIP
        cpu->wr_page[p] = m->ram + ((p & 0x07) << 8);
#endif
    }
    dl_reset(m);
}

void dl_reset(dl_machine_t *m)
{
    z80_reset(&m->cpu);
    m->cpu.icount = 0;
    memset(m->ram, 0, sizeof(m->ram));
    ay_reset(&m->ay);
    m->ay.port_in[0] = m->dsw1;
    m->ay.port_in[1] = m->dsw2;
    m->ay_latch = 0;
    m->strobes = 0;
    m->irq_wait = DL_IRQ_CYCLES;
    m->field_cycles_frac = 0;
    for (int i = 0; i < 16; i++)
        m->leds.digit[i] = DL_LED_BLANK;
    m->leds.serial++;

    ldp_init(&m->ldp, m->ldp.disc, m->ldp.first, m->ldp.last);
    ldv_reset(&m->ldv);
}

/* runs the CPU up to `to` cycles into the field */
static inline void DL_HOT(run_to)(dl_machine_t *m, uint32_t *pos, uint32_t to)
{
    if (to <= *pos)
        return;
    m->seg_end = to;
    m->dirty = 1; /* something may have changed at the event: no skip on the first read */
    z80_run(&m->cpu, (int)(to - *pos));
    *pos = to;
}

void DL_HOT(dl_run_field)(dl_machine_t *m, uint16_t inputs, int16_t *audio, int n)
{
    m->inputs = inputs;
    m->ay.port_in[0] = (uint8_t)(m->dsw1 | ((inputs & DL_IN_SERVICE) ? 0x80 : 0x00));
    m->ay.port_in[1] = m->dsw2;

    m->field_cycles_frac += FIELD_CYCLES_Q8;
    const uint32_t field = m->field_cycles_frac >> 8;
    m->field_cycles_frac &= 0xff;

    m->audio = audio;
    m->audio_n = n;
    m->audio_done = 0;
    m->audio_field_cycles = field;

    /* vertical sync: the disc moves on, the strobes are low */
    ldp_vsync(&m->ldp);
    m->strobes = 0;

    static const uint32_t edge_at[4] = {T_STATUS_ON, T_STATUS_OFF, T_COMMAND_ON, T_COMMAND_OFF};
    static const uint8_t edge_bits[4] = {0x40, 0x00, 0x80, 0x00};
    uint32_t pos = 0;
    uint32_t irq_at = m->irq_wait;
    int e = 0;
    while (pos < field)
    {
        uint32_t next = field;
        if (e < 4 && edge_at[e] < next)
            next = edge_at[e];
        if (irq_at < next)
            next = irq_at;
        run_to(m, &pos, next);
        if (e < 4 && pos == edge_at[e])
        {
            m->strobes = edge_bits[e];
            e++;
        }
        if (pos == irq_at)
        {
            z80_hold_irq(&m->cpu, 0xff);
            irq_at += DL_IRQ_CYCLES;
        }
    }
    m->irq_wait = irq_at - field; /* the CPU's overshoot carries over in its icount */

    if (audio)
    {
        ay_render(&m->ay, audio + m->audio_done, n - m->audio_done, m->ay_level);
        m->audio_done = n;
    }
    m->audio = NULL;
    m->fields++;
}
