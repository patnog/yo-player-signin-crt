#pragma once
// host stand-in for simple-lib-core/vfs.h: a one-file in-memory store, enough for account.txt.
#include <stdint.h>
int readFile(const char *path, char *buf, int cap);
int writeFile(const char *path, const char *data, uint64_t len);
int deleteFile(const char *path);
