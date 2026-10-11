/*
 * Dragon's Lair ROM set loader.
 *
 * Finds the four program ROMs of MAME's `dlair` set (revision F2, the parent
 * set) in a directory, either inside a zip (merged, split or non-merged; the
 * clone subfolders of a merged set are simply not matched) or as loose files.
 * Files are identified by size and CRC32 only, so their names do not matter.
 *
 * The file system is reached through dl_io_t, so the same code runs on FatFs
 * on the board and on stdio in the host harness. Large buffers are taken from
 * the heap, never the stack: the inflate state alone is about 11 KB.
 */
#ifndef ROMLOAD_H
#define ROMLOAD_H

#include <stdint.h>

#include "dlair.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DL_ROMFILE_COUNT 4
#define DL_ROMS_ALL ((1u << DL_ROMFILE_COUNT) - 1)

typedef struct
{
    const char *name; /* MAME name, used in messages only */
    uint32_t crc;
    uint16_t size;
    uint16_t offset; /* in the Z80 address space */
} dl_romfile_t;

extern const dl_romfile_t dl_romfiles[DL_ROMFILE_COUNT];

typedef void (*dl_dir_cb)(void *arg, const char *name, uint32_t size, int is_dir);

typedef struct
{
    void *ctx;
    /* Calls cb for every entry of dir; returns 0 when dir cannot be opened. */
    int (*list_dir)(void *ctx, const char *dir, dl_dir_cb cb, void *arg);
    void *(*open)(void *ctx, const char *path);
    uint32_t (*size)(void *ctx, void *file);
    /* Returns the number of bytes read, or -1. */
    int (*read_at)(void *ctx, void *file, uint32_t offset, void *buf, uint32_t len);
    void (*close)(void *ctx, void *file);
} dl_io_t;

/* Searches dir: dlair.zip first, then any other .zip, then loose files.
 * `found` is the mask of files already loaded (bit i = dl_romfiles[i]); the
 * updated mask is returned, DL_ROMS_ALL when the set is complete. */
uint32_t dl_romload(dl_roms_t *roms, const dl_io_t *io, const char *dir, uint32_t found);

#ifdef __cplusplus
}
#endif

#endif
