#pragma once
// host stand-in for simple-lib-app/gfx.h: just what font.c touches. uploadGfxTexture keeps the last
// ARGB buffer it was given so the test can look at the pixels.
#include <stdint.h>

typedef struct {
   uint32_t offset;
   int      w, h;
   int      pitch;
} GfxTexture;

uint32_t uploadGfxTexture(const void *rgba, int w, int h, int srcPitch);
void     retireGfxTexture(GfxTexture *tex);
