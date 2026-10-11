/*
 * Host harness for the Dragon's Lair core: loads the ROM set through the same
 * loader as the firmware, plays lair.pld through the same streaming decoder,
 * runs the machine one field at a time, and dumps the screen as PPM and the
 * sound as WAV.
 *
 *   ./hosttest/dl_host <romdir> <fields> <dump-every-N> [outdir] [options]
 *
 * Options:
 *   --disc FILE    the laserdisc file (default <romdir>/lair.pld when present;
 *                  without one the player only keeps frame numbers)
 *   --nodisc       run without the laserdisc file
 *   --wav FILE     write the sound (mono, 16-bit, 44.1 kHz)
 *   --coin F       insert the coins for one credit from field F
 *   --start F      press 1P start at field F
 *   --moves FILE   scripted joystick: lines "<field> <keys>", keys from
 *                  U D L R S (S = sword), each held for 8 fields
 *   --ldlog        print every laserdisc command and status change
 *   --nofastboot   keep the 30 s start-up delay of the original
 *   --sd-kbps N    read at most N KB per second of game time, as a slow SD
 *                  card would (0 = no limit, the default)
 *   --buffer KB    size of the picture read buffer (default 32, as on the
 *                  HSTX boards without PSRAM; 1024 on boards with PSRAM)
 *   --noscore      leave the scoreboard overlay out
 *   --pc N         print the CPU's PC every N fields
 *
 * Every dumped field is outdir/field_NNNNNN.ppm (320x240, what the board
 * shows). At the end a summary reports the decoder's statistics.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "dlair.h"
#include "ldstream.h"
#include "romload.h"
#include "scoreboard.h"

#define SAMPLERATE 44100
#define SAMPLES_PER_FIELD 735

/* the board's mix, see main.cpp */
#define LD_AUDIO_GAIN_Q8 160
#define AY_LEVEL 15000

/* --- stdio implementation of dl_io_t ------------------------------------- */

static int host_list_dir(void *ctx, const char *dir, dl_dir_cb cb, void *arg)
{
    (void)ctx;
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        if (e->d_name[0] == '.')
            continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0)
            continue;
        cb(arg, e->d_name, (uint32_t)st.st_size, S_ISDIR(st.st_mode));
    }
    closedir(d);
    return 1;
}

static void *host_open(void *ctx, const char *path)
{
    (void)ctx;
    return fopen(path, "rb");
}

static uint32_t host_size(void *ctx, void *f)
{
    (void)ctx;
    long cur = ftell((FILE *)f);
    fseek((FILE *)f, 0, SEEK_END);
    long n = ftell((FILE *)f);
    fseek((FILE *)f, cur, SEEK_SET);
    return (uint32_t)n;
}

static int host_read_at(void *ctx, void *f, uint32_t ofs, void *buf, uint32_t len)
{
    (void)ctx;
    if (fseek((FILE *)f, ofs, SEEK_SET) != 0)
        return -1;
    return (int)fread(buf, 1, len, (FILE *)f);
}

static void host_close(void *ctx, void *f)
{
    (void)ctx;
    fclose((FILE *)f);
}

/* the disc reader: sector-aligned reads, as on the board */
static int disc_read(void *ctx, uint32_t ofs, void *dst, uint32_t len)
{
    FILE *f = (FILE *)ctx;
    if ((ofs & 511) || (len & 511))
    {
        fprintf(stderr, "unaligned disc read %u+%u\n", ofs, len);
        exit(1);
    }
    if (fseek(f, ofs, SEEK_SET) != 0)
        return -1;
    return (int)fread(dst, 1, len, f);
}

/* --- output helpers ------------------------------------------------------ */

static void write_ppm(const char *path, const uint16_t *fb)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n320 240\n255\n");
    for (int i = 0; i < 320 * 240; i++)
    {
        uint16_t p = fb[i]; /* RGB555 */
        uint8_t rgb[3] = {(uint8_t)(((p >> 10) & 0x1f) * 255 / 31), (uint8_t)(((p >> 5) & 0x1f) * 255 / 31),
                          (uint8_t)((p & 0x1f) * 255 / 31)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void put16(FILE *f, uint16_t v)
{
    fputc(v & 0xff, f);
    fputc(v >> 8, f);
}

static void put32(FILE *f, uint32_t v)
{
    put16(f, v & 0xffff);
    put16(f, v >> 16);
}

static void wav_header(FILE *f, uint32_t samples)
{
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + samples * 2);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 1);
    put32(f, SAMPLERATE);
    put32(f, SAMPLERATE * 2);
    put16(f, 2);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, samples * 2);
}

/* --- tracing ------------------------------------------------------------- */

static uint32_t g_field;
static int g_ldlog;

static const char *ld_name(uint8_t v)
{
    switch (v)
    {
    case 0x3f: return "0";
    case 0x0f: return "1";
    case 0x8f: return "2";
    case 0x4f: return "3";
    case 0x2f: return "4";
    case 0xaf: return "5";
    case 0x6f: return "6";
    case 0x1f: return "7";
    case 0x9f: return "8";
    case 0x5f: return "9";
    case 0xf7: return "SEARCH";
    case 0xfd: return "PLAY";
    case 0xfb: return "STOP";
    case 0xa0: return "PAUSE";
    case 0xf4: return "AUDIO1";
    case 0xfc: return "AUDIO2";
    case 0xcd: return "DISPLAY-OFF";
    case 0xce: return "DISPLAY-ON";
    case 0xbf: return "CLEAR";
    case 0xc2: return "GET-FRAME";
    case 0xff: return NULL; /* NO ENTRY, between commands */
    default: return "?";
    }
}

static void trace(void *ctx, const char *what, uint32_t value)
{
    (void)ctx;
    if (!g_ldlog)
        return;
    if (!strcmp(what, "ld_write"))
    {
        const char *n = ld_name((uint8_t)value);
        if (n)
            printf("%7u  ld <- %02x %s\n", g_field, value, n);
    }
    else if (!strcmp(what, "ld_read"))
    {
        static uint32_t last = 0x100;
        if (value != last)
            printf("%7u  ld -> %02x\n", g_field, value);
        last = value;
    }
    else
    {
        printf("%7u  player %s %u\n", g_field, what, value);
    }
}

/* --- scripted input ------------------------------------------------------ */

typedef struct
{
    uint32_t field;
    uint16_t bits;
} move_t;

static move_t *moves;
static int nmoves;

static void load_moves(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
    {
        fprintf(stderr, "cannot open %s\n", path);
        exit(1);
    }
    char line[128];
    while (fgets(line, sizeof(line), f))
    {
        unsigned field;
        char keys[64];
        if (line[0] == '#' || sscanf(line, "%u %63s", &field, keys) != 2)
            continue;
        uint16_t bits = 0;
        for (char *k = keys; *k; k++)
        {
            switch (*k)
            {
            case 'U': bits |= DL_IN_UP; break;
            case 'D': bits |= DL_IN_DOWN; break;
            case 'L': bits |= DL_IN_LEFT; break;
            case 'R': bits |= DL_IN_RIGHT; break;
            case 'S': bits |= DL_IN_SWORD; break;
            default: break;
            }
        }
        moves = realloc(moves, sizeof(move_t) * (nmoves + 1));
        moves[nmoves].field = field;
        moves[nmoves].bits = bits;
        nmoves++;
    }
    fclose(f);
}

/* --- main ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: %s <romdir> <fields> <dump-every-N> [outdir] [options]\n", argv[0]);
        return 1;
    }
    const char *romdir = argv[1];
    const uint32_t fields = (uint32_t)strtoul(argv[2], NULL, 0);
    const uint32_t every = (uint32_t)strtoul(argv[3], NULL, 0);
    const char *outdir = "hosttest/out";
    const char *wavpath = NULL, *discpath = NULL;
    int nodisc = 0, fastboot = 1, score = 1;
    uint32_t coin_at = 0xffffffff, start_at = 0xffffffff, sd_kbps = 0, pc_every = 0, buffer_kb = 32;
    int a = 4;
    if (argc > 4 && argv[4][0] != '-')
        outdir = argv[a++];
    for (; a < argc; a++)
    {
        if (!strcmp(argv[a], "--disc") && a + 1 < argc)
            discpath = argv[++a];
        else if (!strcmp(argv[a], "--nodisc"))
            nodisc = 1;
        else if (!strcmp(argv[a], "--wav") && a + 1 < argc)
            wavpath = argv[++a];
        else if (!strcmp(argv[a], "--coin") && a + 1 < argc)
            coin_at = (uint32_t)strtoul(argv[++a], NULL, 0);
        else if (!strcmp(argv[a], "--start") && a + 1 < argc)
            start_at = (uint32_t)strtoul(argv[++a], NULL, 0);
        else if (!strcmp(argv[a], "--moves") && a + 1 < argc)
            load_moves(argv[++a]);
        else if (!strcmp(argv[a], "--ldlog"))
            g_ldlog = 1;
        else if (!strcmp(argv[a], "--nofastboot"))
            fastboot = 0;
        else if (!strcmp(argv[a], "--sd-kbps") && a + 1 < argc)
            sd_kbps = (uint32_t)strtoul(argv[++a], NULL, 0);
        else if (!strcmp(argv[a], "--noscore"))
            score = 0;
        else if (!strcmp(argv[a], "--buffer") && a + 1 < argc)
            buffer_kb = (uint32_t)strtoul(argv[++a], NULL, 0);
        else if (!strcmp(argv[a], "--pc") && a + 1 < argc)
            pc_every = (uint32_t)strtoul(argv[++a], NULL, 0);
        else
        {
            fprintf(stderr, "unknown option %s\n", argv[a]);
            return 1;
        }
    }
    mkdir(outdir, 0755);

    /* ROMs */
    static dl_roms_t roms;
    const dl_io_t io = {NULL, host_list_dir, host_open, host_size, host_read_at, host_close};
    uint32_t found = dl_romload(&roms, &io, romdir, 0);
    if (found != DL_ROMS_ALL)
    {
        fprintf(stderr, "ROM set incomplete (mask %x)\n", found);
        return 1;
    }

    /* laserdisc */
    char defpath[1024];
    static ldstream_t stream;
    static uint8_t picture[PLD_WIDTH * PLD_HEIGHT];
    ldstream_t *disc = NULL;
    FILE *df = NULL;
    if (!nodisc)
    {
        if (!discpath)
        {
            snprintf(defpath, sizeof(defpath), "%s/lair.pld", romdir);
            discpath = defpath;
        }
        df = fopen(discpath, "rb");
        if (df)
        {
            fseek(df, 0, SEEK_END);
            uint32_t size = (uint32_t)ftell(df);
            const ldstream_io_t dio = {disc_read, df, df};
            int err = ldstream_open(&stream, &dio, size, NULL, buffer_kb * 1024, 8192, picture, SAMPLES_PER_FIELD);
            if (err)
            {
                fprintf(stderr, "%s: cannot use (error %d%s)\n", discpath, err,
                        err == 4 ? ", made by an older ld2pico.py: convert again" : "");
                return 1;
            }
            disc = &stream;
            printf("disc %s: frames %u-%u, %u keys, %s\n", discpath, ldstream_first_frame(disc),
                   ldstream_last_frame(disc), stream.hdr.key_count, stream.hdr.source);
        }
        else if (discpath != defpath)
        {
            fprintf(stderr, "cannot open %s\n", discpath);
            return 1;
        }
    }
    if (!disc)
        printf("no laserdisc file: frame numbers only\n");

    static dl_machine_t m;
    m.trace = trace;
    dl_init(&m, &roms, disc, fastboot != 0, SAMPLERATE);
    m.ay_level = AY_LEVEL;

    FILE *wav = NULL;
    uint32_t wav_samples = 0;
    if (wavpath)
    {
        wav = fopen(wavpath, "wb");
        if (wav)
            wav_header(wav, 0);
    }

    static uint16_t screen[320 * 240];
    memset(screen, 0, sizeof(screen));
    uint16_t lut[256];
    uint32_t lut_serial = 0xffffffff;
    uint32_t shown_serial = 0xffffffff;
    uint32_t presents = 0, late = 0, max_late = 0;
    const double budget_per_field = sd_kbps ? sd_kbps * 1024.0 / 59.94 : 0;
    double budget = 0;

    for (g_field = 0; g_field < fields; g_field++)
    {
        /* 1. at vertical blank: the picture the player shows now */
        const uint32_t display = ldp_display_frame(&m.ldp);
        if (m.ldp.state == LDP_PARKED)
        {
            memset(screen, 0, sizeof(screen));
        }
        else if (disc && ldstream_picture_due(disc, display))
        {
            if (disc->pic_serial != shown_serial || lut_serial != disc->palette_serial)
            {
                if (lut_serial != disc->palette_serial)
                {
                    memcpy(lut, disc->palette, sizeof(lut));
                    lut_serial = disc->palette_serial;
                }
                for (int i = 0; i < 320 * 240; i++)
                    screen[i] = lut[picture[i]];
                shown_serial = disc->pic_serial;
            }
            if (display > disc->buf_frame)
            {
                late++;
                if (display - disc->buf_frame > max_late)
                    max_late = display - disc->buf_frame;
            }
            presents++;
            ldstream_picture_shown(disc);
        }
        if (score)
            sb_draw(&m.leds, screen, 320, SB_RGB555);

        /* 2. the inputs */
        uint16_t in = 0;
        /* two coins make a credit (DIP default): two pulses, 14 fields low and
         * 20 high each, as the IRQ code needs 3 samples of each level */
        if (g_field >= coin_at && g_field < coin_at + 68)
        {
            uint32_t t = (g_field - coin_at) % 34;
            if (t < 14)
                in |= DL_IN_COIN1;
        }
        if (g_field >= start_at && g_field < start_at + 10)
            in |= DL_IN_START1;
        for (int i = 0; i < nmoves; i++)
            if (g_field >= moves[i].field && g_field < moves[i].field + 8)
                in |= moves[i].bits;

        /* 3. the field */
        int16_t audio[SAMPLES_PER_FIELD];
        memset(audio, 0, sizeof(audio));
        dl_run_field(&m, in, audio, SAMPLES_PER_FIELD);
        if (disc && ldp_audio_on(&m.ldp))
            ldstream_audio_render(disc, audio, SAMPLES_PER_FIELD, LD_AUDIO_GAIN_Q8);
        if (wav)
        {
            fwrite(audio, 2, SAMPLES_PER_FIELD, wav);
            wav_samples += SAMPLES_PER_FIELD;
        }

        /* 4. the decoder, until the next field (or the SD budget) */
        if (disc)
        {
            budget += budget_per_field;
            const bool playing = m.ldp.state == LDP_PLAYING;
            for (int guard = 0; guard < 100000; guard++)
            {
                uint32_t allow = 65536;
                if (budget_per_field)
                    allow = budget > 0 ? (uint32_t)budget : 0;
                uint32_t before = disc->bytes_read;
                if (!ldstream_step(disc, ldp_display_frame(&m.ldp), playing, allow))
                    break;
                budget -= (double)(disc->bytes_read - before);
            }
            if (!budget_per_field)
                budget = 0;
            else if (budget > budget_per_field)
                budget = budget_per_field; /* unused time is gone */
        }

        if (pc_every && g_field % pc_every == 0)
            printf("%7u  pc %04x sp %04x iff1 %u im %u\n", g_field, m.cpu.pc.w, m.cpu.sp.w, m.cpu.iff1, m.cpu.im);

        if (every && g_field % every == 0)
        {
            char path[1100];
            snprintf(path, sizeof(path), "%s/field_%06u.ppm", outdir, g_field);
            write_ppm(path, screen);
        }
    }

    if (wav)
    {
        wav_header(wav, wav_samples);
        fclose(wav);
    }

    printf("fields %u, player frame %u, state %d\n", fields, m.ldp.frame, m.ldp.state);
    printf("LEDs:");
    for (int i = 0; i < 16; i++)
        printf(" %x", m.leds.digit[i]);
    printf("\n");
    if (disc)
    {
        printf("disc: %u seeks, %u frames decoded, %u reads, %.1f MB read, %u presents (%u late, max %u)\n",
               disc->seeks, disc->frames_decoded, disc->reads, disc->bytes_read / 1048576.0, presents, late,
               max_late);
        printf("disc: %u checksum errors, %u format errors, %u sound reads, %u sound gaps\n", disc->checksum_errors,
               disc->format_errors, disc->audio_reads, disc->audio_gaps);
        ldstream_close(disc);
        fclose(df);
    }
    return 0;
}
