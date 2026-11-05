// RomFS image from a folder of game files (runtime/romfs_build.c). The game reads its data through one raw RomFS
// image (romfs.bin, runtime/services/fs.c); players bring their own game files, and the start screen builds the image
// from their romfs/ folder.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Writes the RomFS image of the folder `dir` to `out`. progress (may be NULL): bytes of file data written so far and
// the total, now and then. false = failed (why: a short message).
bool romfs_build(const char *dir, const char *out, void (*progress)(uint64_t done, uint64_t total), char *why,
                 size_t why_size);

// Bytes of file data under dir (the image is about this plus its tables).
uint64_t romfs_dir_bytes(const char *dir);

// Copies the folder tree src to dst (hard links where the file system allows: no second copy of 6 GB). progress as
// above. false = failed.
bool romfs_copy_tree(const char *src, const char *dst, void (*progress)(uint64_t done, uint64_t total));

// The same image served in place from the folder, never written (services/fs.c: game files given as already
// extracted exefs/ + romfs/ folders, no romfs.bin needed). The tables are built in memory when it is opened; file
// data is read from the files on each read. NULL = failed (why).
typedef struct RomfsVirt RomfsVirt;
RomfsVirt *romfs_virt_open(const char *dir, char *why, size_t why_size);
uint64_t romfs_virt_size(const RomfsVirt *v);
// bytes [off, off + n) of the image; returns the count read (short only at the image's end), -1 on a read error
int64_t romfs_virt_pread(RomfsVirt *v, void *dst, uint64_t n, uint64_t off);
void romfs_virt_close(RomfsVirt *v);

#ifdef __cplusplus
}
#endif
