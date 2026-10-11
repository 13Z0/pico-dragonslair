/*
 * ld2pico on the command line (Linux, and any other POSIX system):
 *
 *   ld2pico <dlair.chd> <lair.pld> [options]   convert the disc
 *   ld2pico <dlair.chd> --info                  describe the CHD
 *   ld2pico <dlair.chd> --frame P --ppm FILE    one picture, 320x240 PPM
 *
 * Options:
 *   --quality Q      allowed squared error per colour channel of a block that
 *                    is coded with fewer colours than it has, or left
 *                    unchanged (default 60; higher: smaller, softer)
 *   --max-rate KB    the most KB of pictures per second of play (default 420);
 *                    busier scenes are coded at a lower quality
 *   --denoise A,B    temporal filter against the film grain: changes below A
 *                    (of 255) are dropped, above B kept (default 10,28; 0,0
 *                    switches it off)
 *   --keyint N       the most pictures between key records (default 32)
 *   --audio-gain G   multiply the sound by G instead of bringing it to full
 *                    level
 *   --threads N      threads (default: one per processor, up to 12)
 *   --start P --count N   only a part of the disc, for trials
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ldp_core.h"

static void on_log(void *ctx, const char *line)
{
    (void)ctx;
    fprintf(stderr, "%s\n", line);
}

static void on_progress(void *ctx, int done, int total)
{
    int *last = (int *)ctx;
    const int pct = total ? (int)((long long)done * 100 / total) : 100;
    if (pct / 5 != *last / 5)
        fprintf(stderr, "  %3d%%  %d of %d pictures\n", pct, done, total);
    *last = pct;
}

static int usage(void)
{
    fprintf(stderr, "usage: ld2pico <dlair.chd> <lair.pld> [--quality Q] [--max-rate KB] [--denoise A,B]\n"
                    "               [--keyint N] [--audio-gain G] [--threads N] [--start P --count N]\n"
                    "       ld2pico <dlair.chd> --info\n"
                    "       ld2pico <dlair.chd> --frame P --ppm FILE\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return usage();
    const char *chd = argv[1];
    const char *pld = NULL, *ppm = NULL;
    int info = 0, frame = -1;
    ldp_settings s;
    ldp_default_settings(&s);
    for (int a = 2; a < argc; a++)
    {
        const char *o = argv[a];
        const char *v = a + 1 < argc ? argv[a + 1] : NULL;
        if (!strcmp(o, "--info"))
            info = 1;
        else if (!strcmp(o, "--frame") && v)
            frame = atoi(argv[++a]);
        else if (!strcmp(o, "--ppm") && v)
            ppm = argv[++a];
        else if (!strcmp(o, "--quality") && v)
            s.quality = atof(argv[++a]);
        else if (!strcmp(o, "--max-rate") && v)
            s.max_rate_kb = atof(argv[++a]);
        else if (!strcmp(o, "--denoise") && v)
        {
            if (sscanf(argv[++a], "%lf,%lf", &s.denoise_lo, &s.denoise_hi) != 2)
                return usage();
        }
        else if (!strcmp(o, "--keyint") && v)
            s.keyint = atoi(argv[++a]);
        else if (!strcmp(o, "--audio-gain") && v)
            s.audio_gain = atof(argv[++a]);
        else if (!strcmp(o, "--threads") && v)
            s.threads = atoi(argv[++a]);
        else if (!strcmp(o, "--start") && v)
            s.start = atoi(argv[++a]);
        else if (!strcmp(o, "--count") && v)
            s.count = atoi(argv[++a]);
        else if (o[0] != '-' && !pld)
            pld = o;
        else
            return usage();
    }

    char msg[512];
    if (info)
    {
        if (ldp_describe(chd, msg, sizeof(msg)))
        {
            fprintf(stderr, "ld2pico: %s: %s\n", chd, msg);
            return 1;
        }
        printf("%s\n", msg);
        return 0;
    }
    if (frame >= 0)
    {
        if (!ppm)
            return usage();
        if (ldp_dump_picture(chd, frame, ppm, msg, sizeof(msg)))
        {
            fprintf(stderr, "ld2pico: %s\n", msg);
            return 1;
        }
        return 0;
    }
    if (!pld)
        return usage();
    int last = -1;
    const ldp_hooks hooks = {&last, on_log, on_progress, NULL};
    return ldp_convert(chd, pld, &s, &hooks);
}
