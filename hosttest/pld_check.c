/*
 * Checks a lair.pld file with the firmware's streaming decoder: every picture
 * in order, then pictures reached by seeking, each compared with the CRC32 the
 * converter stored for it.
 *
 *   ./hosttest/pld_check <lair.pld> [seeks]
 *   ./hosttest/pld_check <lair.pld> --dump <picture> <out.ppm>
 *
 * Also prints the read volume per picture, to compare with what the SD card
 * of a board delivers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ldstream.h"
#include "miniz.h"

static int disc_read(void *ctx, uint32_t ofs, void *dst, uint32_t len)
{
    FILE *f = (FILE *)ctx;
    if ((ofs & 511) || (len & 511))
    {
        fprintf(stderr, "unaligned read %u+%u\n", ofs, len);
        exit(1);
    }
    if (fseek(f, ofs, SEEK_SET) != 0)
        return -1;
    return (int)fread(dst, 1, len, f);
}

static uint32_t *crcs;

static int check(ldstream_t *s, uint8_t *picture, uint32_t frame)
{
    const uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, picture, PLD_WIDTH * PLD_HEIGHT);
    const uint32_t want = crcs[frame - s->hdr.first_frame];
    if (crc != want)
    {
        printf("picture %u: CRC %08x, expected %08x\n", frame, crc, want);
        return 1;
    }
    return 0;
}

/* steps until the decoder idles; returns the number of steps */
static int run(ldstream_t *s, uint32_t display, bool playing)
{
    int n = 0;
    while (ldstream_step(s, display, playing, 65536))
        if (++n > 1000000)
        {
            printf("decoder does not settle\n");
            exit(1);
        }
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <lair.pld> [seeks]\n       %s <lair.pld> --dump <picture> <out.ppm>\n", argv[0],
                argv[0]);
        return 1;
    }
    const int dump = argc > 4 && !strcmp(argv[2], "--dump");
    const int seeks = dump ? 0 : argc > 2 ? atoi(argv[2]) : 2000;
    FILE *f = fopen(argv[1], "rb");
    if (!f)
    {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    const uint32_t size = (uint32_t)ftell(f);

    static uint8_t picture[PLD_WIDTH * PLD_HEIGHT];
    static ldstream_t s;
    const ldstream_io_t io = {disc_read, f, f};
    int err = ldstream_open(&s, &io, size, NULL, 32768, 8192, picture, 735);
    if (err)
    {
        printf("cannot open: error %d%s\n", err, err == 4 ? " (made by an older ld2pico.py: convert again)" : "");
        return 1;
    }
    const pld_header_t *h = &s.hdr;
    printf("%s: pictures %u-%u, %u keys, largest record %u bytes, source %s, %s\n", argv[1], h->first_frame,
           h->first_frame + h->frame_count - 1, h->key_count, h->max_record, h->source, h->tool);
    if (!h->crc_offset)
    {
        printf("no CRC table\n");
        return 1;
    }
    crcs = malloc(4 * h->frame_count);
    fseek(f, h->crc_offset, SEEK_SET);
    if (fread(crcs, 4, h->frame_count, f) != h->frame_count)
    {
        printf("cannot read the CRC table\n");
        return 1;
    }

    int bad = 0;
    clock_t t0 = clock();

    if (dump)
    {
        const uint32_t want = (uint32_t)strtoul(argv[3], NULL, 0);
        ldstream_seek(&s, want);
        run(&s, LDS_NONE, false);
        if (!ldstream_seek_done(&s, want))
        {
            printf("picture %u not found\n", want);
            return 1;
        }
        FILE *o = fopen(argv[4], "wb");
        if (!o)
            return 1;
        fprintf(o, "P6\n%d %d\n255\n", PLD_WIDTH, PLD_HEIGHT);
        for (int i = 0; i < PLD_WIDTH * PLD_HEIGHT; i++)
        {
            const uint16_t v = s.palette[picture[i]];
            const uint8_t rgb[3] = {(uint8_t)(((v >> 10) & 31) * 255 / 31), (uint8_t)(((v >> 5) & 31) * 255 / 31),
                                    (uint8_t)((v & 31) * 255 / 31)};
            fwrite(rgb, 1, 3, o);
        }
        fclose(o);
        return check(&s, picture, want);
    }

    /* every picture in order, as during play */
    const uint32_t first = h->first_frame, last = first + h->frame_count - 1;
    ldstream_play(&s, first);
    for (uint32_t fr = first; fr <= last; fr++)
    {
        run(&s, fr, true);
        if (!ldstream_picture_due(&s, fr) || s.buf_frame != fr)
        {
            printf("picture %u not decoded (buffer holds %u)\n", fr, s.buf_frame);
            bad++;
            break;
        }
        bad += check(&s, picture, fr);
        ldstream_picture_shown(&s);
    }
    const double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("in order: %u pictures, %.1f MB read (%.1f KB per picture), %u checksum and %u format errors, %.1f s\n",
           h->frame_count, s.bytes_read / 1048576.0, s.bytes_read / 1024.0 / h->frame_count, s.checksum_errors,
           s.format_errors, secs);

    /* searches, as the game makes them */
    srand(1);
    uint32_t before = s.bytes_read, max_bytes = 0;
    for (int i = 0; i < seeks; i++)
    {
        const uint32_t target = first + (uint32_t)rand() % h->frame_count;
        const uint32_t b0 = s.bytes_read;
        ldstream_seek(&s, target);
        run(&s, LDS_NONE, false);
        if (!ldstream_seek_done(&s, target))
        {
            printf("seek to %u did not finish\n", target);
            bad++;
            continue;
        }
        bad += check(&s, picture, target);
        if (s.bytes_read - b0 > max_bytes)
            max_bytes = s.bytes_read - b0;
        ldstream_picture_shown(&s);
        /* and play a few pictures on from there */
        ldstream_play(&s, target);
        for (uint32_t fr = target + 1; fr <= target + 5 && fr <= last; fr++)
        {
            run(&s, fr, true);
            bad += check(&s, picture, fr);
            ldstream_picture_shown(&s);
        }
    }
    if (seeks)
        printf("seeks: %d, %.1f KB read per seek on average, %.1f KB at most\n", seeks,
               (s.bytes_read - before) / 1024.0 / seeks, max_bytes / 1024.0);

    printf("%s\n", bad ? "FAILED" : "OK");
    ldstream_close(&s);
    fclose(f);
    return bad ? 1 : 0;
}
