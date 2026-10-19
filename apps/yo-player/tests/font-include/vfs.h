#pragma once
// host stand-in for simple-lib-core/vfs.h: the four calls font.c uses to read a font file, over stdio.
#include <stdint.h>

typedef struct { uint64_t size; uint64_t mtime; int isDir; uint32_t mode; uint32_t attributes; } VfsStat;
typedef struct { const void *backend; int descriptor; int abandoned; void *host; } VfsFile;

#define VFS_O_RDONLY 0x0001

int     statPath(const char *path, VfsStat *outStat);
int     openFs(const char *path, int flags, VfsFile *file);
int64_t readFs(VfsFile *file, void *buffer, uint64_t length);
int     closeFs(VfsFile *file);
