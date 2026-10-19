// font - truetype font rendering via stb_truetype
//
// Text is rasterised in the app. The firmware font libraries (libfont / libfontFT) are deliberately not
// linked: a static import of those PRX libraries stops the process loader on a console, so the app never
// reaches main(). System fonts are plain files in /dev_flash/data/font, read like any other file.
//
// A Font is a chain of faces. A glyph comes from the first face that has it, so one Font covers Latin, kana,
// CJK and symbols the way the firmware's own font sets do. Only the first face is loaded when a Font is
// opened; the others load the first time a glyph at or above their minimum code point is needed, which keeps
// the multi-megabyte CJK files off the heap until a title actually uses them. Faces are shared between Fonts
// and reference counted, so reopening a system font on every screen costs nothing.
#include "font.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "gfx.h"
#include "colors.h"
#include "dbg.h"
#include "vfs.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "stb_truetype.h"
#pragma GCC diagnostic pop

#include "font-embedded.h"

#ifndef FONT_SYSTEM_DIR
#define FONT_SYSTEM_DIR "/dev_flash/data/font/"
#endif

static const int FONT_MAX_RENDER_W = 1024;
static const size_t FONT_MAX_SURFACE_BYTES = 16u * 1024u * 1024u;   // sanity cap for the rasterize() scratch buffer
static const uint64_t FONT_MAX_FILE_BYTES = 16u * 1024u * 1024u;    // the largest firmware face is ~8 MB

#define FONT_MAX_FACES 8
#define FACE_CACHE_MAX 24
#define FONT_ITALIC_SLANT 0.35f

typedef struct {
   char           path[96];   // cache key; "" for a face that belongs to one Font only (file / memory)
   uint8_t       *data;
   uint32_t       size;
   int            ownsData;   // free(data) when the last reference goes
   int            state;      // 0 = not loaded yet, 1 = ready, -1 = unavailable
   int            refs;
   stbtt_fontinfo info;
} SharedFace;

struct FontData {
   SharedFace *faces[FONT_MAX_FACES];
   uint32_t    minCode[FONT_MAX_FACES];   // the face is only consulted for code points >= this
   int         nFaces;
};

static SharedFace faceCache[FACE_CACHE_MAX];
static int        fontInited = 0;

int initFont(void)
{
   fontInited = 1;
   return 0;
}

void termFont(void)
{
   fontInited = 0;
}

// ---- face loading -----------------------------------------------------------------------------------

static uint8_t *loadWholeFile(const char *path, uint32_t *outSize)
{
   VfsStat st;
   if (statPath(path, &st) != 0 || st.size == 0 || st.size > FONT_MAX_FILE_BYTES) return NULL;
   uint32_t size = (uint32_t)st.size;

   VfsFile file;
   if (openFs(path, VFS_O_RDONLY, &file) != 0) return NULL;

   uint8_t *buf = (uint8_t *)malloc(size);
   if (!buf) { closeFs(&file); return NULL; }

   uint32_t got = 0;
   while (got < size) {
      int64_t n = readFs(&file, buf + got, size - got);
      if (n <= 0) break;
      got += (uint32_t)n;
   }
   closeFs(&file);
   if (got != size) { free(buf); return NULL; }

   *outSize = size;
   return buf;
}

// Initialises the stb font over the face's bytes. 1 on success.
static int initFaceInfo(SharedFace *fc)
{
   int off = stbtt_GetFontOffsetForIndex(fc->data, 0);
   if (off < 0 || !stbtt_InitFont(&fc->info, fc->data, off)) {
      fc->state = -1;
      return 0;
   }
   fc->state = 1;
   return 1;
}

static SharedFace *newFaceSlot(void)
{
   for (int i = 0; i < FACE_CACHE_MAX; i++)
      if (faceCache[i].refs == 0) { memset(&faceCache[i], 0, sizeof(faceCache[i])); return &faceCache[i]; }
   return NULL;
}

// A reference to the face stored at `path` (loaded on first use), shared with every Font that names it.
static SharedFace *acquireFace(const char *path)
{
   for (int i = 0; i < FACE_CACHE_MAX; i++)
      if (faceCache[i].refs > 0 && strcmp(faceCache[i].path, path) == 0) { faceCache[i].refs++; return &faceCache[i]; }

   SharedFace *fc = newFaceSlot();
   if (!fc) return NULL;
   strncpy(fc->path, path, sizeof(fc->path) - 1);
   fc->refs = 1;
   return fc;
}

// The built-in Latin face: always present, needs no file.
static SharedFace *acquireEmbeddedLatin(void)
{
   SharedFace *fc = acquireFace("embedded:latin");
   if (fc && fc->state == 0) {
      fc->data = (uint8_t *)embeddedLatinFont;
      fc->size = embeddedLatinFontSize;
      fc->ownsData = 0;
      initFaceInfo(fc);
   }
   return fc;
}

static void releaseFace(SharedFace *fc)
{
   if (!fc || fc->refs <= 0) return;
   if (--fc->refs > 0) return;
   if (fc->ownsData) free(fc->data);
   memset(fc, 0, sizeof(*fc));
}

// Loads a face on first use. 1 when it can be used.
static int faceReady(SharedFace *fc)
{
   if (fc->state > 0) return 1;
   if (fc->state < 0 || fc->path[0] == '\0') return 0;
   uint32_t size = 0;
   uint8_t *data = loadWholeFile(fc->path, &size);
   if (!data) { fc->state = -1; logInfo("[font] cannot read %s\n", fc->path); return 0; }
   fc->data = data;
   fc->size = size;
   fc->ownsData = 1;
   logInfo("[font] loaded %s (%u bytes)\n", fc->path, size);
   return initFaceInfo(fc);
}

static int fontAddFace(struct FontData *d, SharedFace *fc, uint32_t minCode)
{
   if (!fc) return 0;
   if (d->nFaces >= FONT_MAX_FACES) { releaseFace(fc); return 0; }
   d->faces[d->nFaces] = fc;
   d->minCode[d->nFaces] = minCode;
   d->nFaces++;
   return 1;
}

static int fontAddSystemFace(struct FontData *d, const char *name, uint32_t minCode)
{
   char path[96];
   if (strlen(FONT_SYSTEM_DIR) + strlen(name) + 1 > sizeof(path)) return 0;
   strcpy(path, FONT_SYSTEM_DIR);
   strcat(path, name);
   return fontAddFace(d, acquireFace(path), minCode);
}

// ---- SFNT metrics -----------------------------------------------------------------------------------

// Parse the SFNT (TrueType/OpenType) header for the hhea ascender/descender + head unitsPerEm, which is
// what FreeType (and hence Ren'Py, ftfont.pyx) uses for the line height (ascender - descender). SFNT is
// big-endian. Fills f->unitsPerEm/hheaAscent/hheaDescent; unitsPerEm stays 0 if the tables aren't found.
// useWin takes the OS/2 usWinAscent/usWinDescent instead of hhea: the firmware's own font layout is built
// on those, and the system fonts' hhea box is much tighter (RD-R-LATIN: 0.78/-0.19 em against 1.03/-0.20),
// so hhea would clip the tops of accents and shift every line relative to the UI laid out around it.
static uint16_t sfntU16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static int16_t  sfntS16(const uint8_t *p) { return (int16_t)((p[0] << 8) | p[1]); }
static uint32_t sfntU32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void parseSfntMetrics(Font *f, const uint8_t *buf, uint32_t size, int useWin)
{
   f->unitsPerEm = 0; f->hheaAscent = 0; f->hheaDescent = 0;
   if (!buf || size < 12) return;
   uint16_t numTables = sfntU16(buf + 4);     // offset table: u32 tag, u16 numTables, ...
   uint32_t headOff = 0, hheaOff = 0, os2Off = 0;
   for (uint16_t i = 0; i < numTables; i++)
   {
      uint32_t rec = 12u + (uint32_t)i * 16u;   // each table record: tag(4) checksum(4) offset(4) length(4)
      if (rec + 16u > size) break;
      const uint8_t *tag = buf + rec;
      uint32_t off = sfntU32(buf + rec + 8);
      if (off > size) continue;   // reject bogus offsets before they can overflow the checks below
      if (tag[0]=='h'&&tag[1]=='e'&&tag[2]=='a'&&tag[3]=='d') headOff = off;
      else if (tag[0]=='h'&&tag[1]=='h'&&tag[2]=='e'&&tag[3]=='a') hheaOff = off;
      else if (tag[0]=='O'&&tag[1]=='S'&&tag[2]=='/'&&tag[3]=='2') os2Off = off;
   }
   int upm = (headOff && headOff + 20u <= size) ? sfntU16(buf + headOff + 18) : 0;   // head.unitsPerEm @ +18
   if (hheaOff && hheaOff + 10u <= size && upm > 0)
   {
      f->hheaAscent  = sfntS16(buf + hheaOff + 4);   // hhea.ascender  @ +4
      f->hheaDescent = sfntS16(buf + hheaOff + 6);   // hhea.descender @ +6
      if (f->hheaAscent != 0 || f->hheaDescent != 0) f->unitsPerEm = upm;
      if (useWin && f->unitsPerEm > 0 && os2Off && os2Off + 78u <= size) {
         int winAscent = sfntU16(buf + os2Off + 74), winDescent = sfntU16(buf + os2Off + 76);   // usWinAscent/Descent
         if (winAscent + winDescent > 0) { f->hheaAscent = winAscent; f->hheaDescent = -winDescent; }
      }
   }
}

// ---- opening ----------------------------------------------------------------------------------------

static Font openFromFirstFace(struct FontData *d, int useWinMetrics)
{
   Font f;
   memset(&f, 0, sizeof(f));
   if (!d || d->nFaces == 0 || !faceReady(d->faces[0])) {
      if (d) {
         for (int i = 0; i < d->nFaces; i++) releaseFace(d->faces[i]);
         free(d);
      }
      return f;
   }
   f.data = d;
   f.open = 1;
   parseSfntMetrics(&f, d->faces[0]->data, d->faces[0]->size, useWinMetrics);
   return f;
}

Font openSystemFont(int type)
{
   if (!fontInited) initFont();

   struct FontData *d = (struct FontData *)calloc(1, sizeof(*d));
   if (!d) { Font none; memset(&none, 0, sizeof(none)); return none; }

   // The first face is the one whose metrics set the line height. The rest are glyph fallbacks, listed with
   // the lowest code point they are asked about (ASCII text never touches a file).
   switch (type) {
      case FONT_GOTHIC_JP:
         fontAddSystemFace(d, "SCE-PS3-NR-R-JPN.TTF", 0);
         fontAddFace(d, acquireEmbeddedLatin(), 0);
         fontAddSystemFace(d, "SCE-PS3-RD-R-LATIN2.TTF", 0x80);
         fontAddSystemFace(d, "SCE-PS3-NR-R-EXT.TTF", 0x80);
         fontAddSystemFace(d, "SCE-PS3-CP-R-KANA.TTF", 0x3000);
         fontAddSystemFace(d, "SCE-PS3-DH-R-CGB.TTF", 0x2E80);
         break;
      case FONT_SANS:
      case FONT_SERIF:
         fontAddSystemFace(d, "SCE-PS3-SR-R-LATIN.TTF", 0);
         fontAddSystemFace(d, "SCE-PS3-SR-R-LATIN2.TTF", 0x80);
         fontAddSystemFace(d, "SCE-PS3-SR-R-EXT.TTF", 0x80);
         fontAddFace(d, acquireEmbeddedLatin(), 0x80);
         fontAddSystemFace(d, "SCE-PS3-SR-R-JPN.TTF", 0x0370);
         fontAddSystemFace(d, "SCE-PS3-DH-R-CGB.TTF", 0x2E80);
         break;
      case FONT_POP:
      default:
         fontAddFace(d, acquireEmbeddedLatin(), 0);
         fontAddSystemFace(d, "SCE-PS3-RD-R-LATIN2.TTF", 0x80);
         fontAddSystemFace(d, "SCE-PS3-NR-R-EXT.TTF", 0x80);
         fontAddSystemFace(d, "SCE-PS3-NR-R-JPN.TTF", 0x0370);
         fontAddSystemFace(d, "SCE-PS3-CP-R-KANA.TTF", 0x3000);
         fontAddSystemFace(d, "SCE-PS3-DH-R-CGB.TTF", 0x2E80);
         break;
   }
   return openFromFirstFace(d, 1);
}

// A font that owns its bytes: `data` is freed with the last reference.
static Font openOwnedFont(uint8_t *data, uint32_t size)
{
   Font none;
   memset(&none, 0, sizeof(none));

   struct FontData *d = (struct FontData *)calloc(1, sizeof(*d));
   SharedFace *fc = d ? newFaceSlot() : NULL;
   if (!d || !fc) { free(d); free(data); return none; }

   fc->refs = 1;
   fc->data = data;
   fc->size = size;
   fc->ownsData = 1;
   initFaceInfo(fc);
   d->faces[0] = fc;
   d->nFaces = 1;
   return openFromFirstFace(d, 0);
}

Font openFontFile(const char *path)
{
   Font none;
   memset(&none, 0, sizeof(none));
   if (!fontInited) initFont();

   uint32_t size = 0;
   uint8_t *buf = loadWholeFile(path, &size);
   if (!buf) return none;
   return openOwnedFont(buf, size);
}

Font openFontMemory(const void *buf, uint32_t size)
{
   Font none;
   memset(&none, 0, sizeof(none));
   if (!fontInited) initFont();
   if (!buf || size == 0) return none;

   // the face keeps reading the bytes for the font's lifetime, so own a private copy.
   uint8_t *own = (uint8_t *)malloc(size);
   if (!own) return none;
   memcpy(own, buf, size);
   Font f = openOwnedFont(own, size);
   if (!f.open) logError("[font] openFontMemory failed (size=%u)\n", size);
   return f;
}

void closeFont(Font *f)
{
   if (f->data) {
      for (int i = 0; i < f->data->nFaces; i++) releaseFace(f->data->faces[i]);
      free(f->data);
      f->data = NULL;
   }
   f->open = 0;
}

void setFontMetrics(Font *f, FontMetricsMode mode, float grid)
{
   f->metricsMode = (int)mode;
   f->metricsGrid = grid > 0.0f ? grid : 0.0f;
}

// ---- glyph lookup + metrics -------------------------------------------------------------------------

typedef struct { SharedFace *face; int glyph; } GlyphRef;

// Characters that take no space and draw nothing: controls, zero-width joiners/marks, variation selectors.
static int isInvisible(uint32_t c)
{
   return c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0) || c == 0xAD ||
          (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) || (c >= 0x2060 && c <= 0x2064) ||
          (c >= 0xFE00 && c <= 0xFE0F) || c == 0xFEFF;
}

// The face + glyph index for a code point; face is NULL only when no face could be read at all.
static GlyphRef lookupGlyph(const Font *f, uint32_t code)
{
   GlyphRef r = { NULL, 0 };
   struct FontData *d = f->data;
   if (!d) return r;
   for (int i = 0; i < d->nFaces; i++) {
      if (code < d->minCode[i]) continue;
      SharedFace *fc = d->faces[i];
      if (!faceReady(fc)) continue;
      int g = stbtt_FindGlyphIndex(&fc->info, (int)code);
      if (g) { r.face = fc; r.glyph = g; return r; }
   }
   // in no face: the first face's .notdef box
   if (d->nFaces > 0 && faceReady(d->faces[0])) { r.face = d->faces[0]; r.glyph = 0; }
   return r;
}

static float faceScale(const SharedFace *fc, float px)
{
   return stbtt_ScaleForMappingEmToPixels(&fc->info, px);
}

// Applies the font's advance model to a raw advance (see FontMetricsMode). The small
// epsilon keeps an exact multiple from ceiling up a step due to float error.
static float quantAdvance(const Font *f, float adv)
{
   if (f->metricsMode != FONT_METRICS_GRID_CEIL || f->metricsGrid <= 0.0f) return adv;
   float cells = ceilf(adv / f->metricsGrid - 0.001f);
   return cells * f->metricsGrid;
}

// the unquantised advance of one code point at `px`, in output pixels
static float rawAdvance(const Font *f, uint32_t code, float px)
{
   if (isInvisible(code)) return 0.0f;
   GlyphRef r = lookupGlyph(f, code);
   if (!r.face) return px;
   int adv = 0, lsb = 0;
   stbtt_GetGlyphHMetrics(&r.face->info, r.glyph, &adv, &lsb);
   return (float)adv * faceScale(r.face, px);
}

// Kerning between two glyphs at the current scale, per the font's metrics mode.
// NATIVE mode reports 0 (fractional-advance stacks don't kern); GRID_CEIL floors the
// offset to the grid, matching classic stacks' integer `delta.x >> 6`.
static float fontKern(Font *f, uint32_t prev, uint32_t code, float px)
{
   if (f->metricsMode != FONT_METRICS_GRID_CEIL || prev == 0) return 0.0f;
   GlyphRef a = lookupGlyph(f, prev), b = lookupGlyph(f, code);
   if (!a.face || a.face != b.face) return 0.0f;
   int k = stbtt_GetGlyphKernAdvance(&a.face->info, a.glyph, b.glyph);
   if (k == 0) return 0.0f;
   float off = (float)k * faceScale(a.face, px);
   float g = f->metricsGrid > 0.0f ? f->metricsGrid : 1.0f;
   return floorf(off / g + 0.001f) * g;
}

// decodes one utf-8 codepoint, advances *pp past it. returns codepoint.
static uint32_t decodeUtf8(const uint8_t **pp)
{
   const uint8_t *p = *pp;
   uint32_t c = *p;
   if (c < 0x80) { (*pp)++; return c; }
   if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
      *pp += 2;
      return ((c & 0x1F) << 6) | (p[1] & 0x3F);
   }
   if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
      *pp += 3;
      return ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
   }
   if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
      *pp += 4;
      return ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
   }
   (*pp)++;
   return c;
}

float measureFontText(Font *f, int size, const char *text)
{
   if (!f->open || !text) return 0.0f;
   float fsize = (float)size;

   float w = 0.0f;
   uint32_t prev = 0;
   const uint8_t *p = (const uint8_t *)text;
   while (*p) {
      uint32_t code = decodeUtf8(&p);
      w += quantAdvance(f, rawAdvance(f, code, fsize)) + fontKern(f, prev, code, fsize);
      prev = code;
   }
   return w;
}

float measureFontChar(Font *f, int size, uint32_t code)
{
   if (!f->open) return (float)size;
   return quantAdvance(f, rawAdvance(f, code, (float)size));
}

// ---- inline style tags: {i} {b} {color=#rrggbb} (+ {{ and [[ escapes) ------------------------------
// Lightweight inline markup for styled runs inside one string. Text is pre-parsed into
// styled glyph items so tag handling stays separate from the (already fiddly)
// wrapping/layout logic, which then just walks the item array.
typedef struct { uint32_t code; uint8_t italic; uint8_t bold; uint32_t color; } GlyphItem;

// Parses text into items[] (<= maxItems). baseColor is the default colour. Nested {i}/{b}
// are counted; {color} uses a small stack. Unknown tags ({w},{p},{size=..},...) are skipped.
// rawMode disables all of the above - every byte (including '{'/'[') is literal content, for
// text that comes from outside the app (file contents, filenames, ...) rather than authored UI copy.
static int parseStyledText(const char *text, uint32_t baseColor, GlyphItem *items, int maxItems, int rawMode)
{
   int italic = 0, bold = 0;
   uint32_t colorStack[8];
   int colorTop = 0;
   colorStack[0] = baseColor;

   int n = 0;
   const uint8_t *p = (const uint8_t *)text;
   while (*p && n < maxItems) {
      if (!rawMode && p[0] == '{' && p[1] == '{') {   // literal brace
         items[n].code = '{'; items[n].italic = (uint8_t)(italic>0); items[n].bold = (uint8_t)(bold>0); items[n].color = colorStack[colorTop]; n++; p += 2; continue;
      }
      if (!rawMode && p[0] == '[' && p[1] == '[') {   // literal bracket
         items[n].code = '['; items[n].italic = (uint8_t)(italic>0); items[n].bold = (uint8_t)(bold>0); items[n].color = colorStack[colorTop]; n++; p += 2; continue;
      }
      if (!rawMode && p[0] == '{') {                  // a tag
         const uint8_t *q = p + 1;
         while (*q && *q != '}') q++;
         int len = (int)(q - (p + 1));
         const char *t = (const char *)(p + 1);
         if      (len == 1 && t[0] == 'i') italic++;
         else if (len == 2 && t[0] == '/' && t[1] == 'i') { if (italic > 0) italic--; }
         else if (len == 1 && t[0] == 'b') bold++;
         else if (len == 2 && t[0] == '/' && t[1] == 'b') { if (bold > 0) bold--; }
         else if (len >= 7 && strncmp(t, "color=#", 7) == 0) {
            // colour literal parsing lives in colors.h (shared with manifest parsing etc.)
            if (colorTop < 7) { colorTop++; colorStack[colorTop] = parseColorSpan(t + 7, len - 7, colorStack[colorTop - 1]); }
         }
         else if (len == 6 && strncmp(t, "/color", 6) == 0) { if (colorTop > 0) colorTop--; }
         // other tags carry no glyph: skip
         p = (*q == '}') ? q + 1 : q;
         continue;
      }
      if (p[0] == '\n') {
         items[n].code = '\n'; items[n].italic = (uint8_t)(italic>0); items[n].bold = (uint8_t)(bold>0); items[n].color = colorStack[colorTop]; n++; p++; continue;
      }
      uint32_t code = decodeUtf8(&p);
      items[n].code = code; items[n].italic = (uint8_t)(italic > 0); items[n].bold = (uint8_t)(bold > 0); items[n].color = colorStack[colorTop];
      n++;
   }
   return n;
}

static float getGlyphAdvance(Font *f, uint32_t code, float fsize)
{
   return quantAdvance(f, rawAdvance(f, code, fsize));
}

// ---- glyph rasterising ------------------------------------------------------------------------------

typedef struct {
   uint8_t *bitmap;     // coverage, w*h bytes (malloc'd; NULL for a blank glyph such as a space)
   int      w, h;
   int      x, y;       // bitmap top-left relative to (ox, baseline), y down
   int      ox;         // integer pen x the glyph was positioned at
   float    advance;    // unquantised horizontal advance, output px
   float    bearingY;   // how far the ink top sits above the baseline
} GlyphBitmap;

// Shears the bitmap about the baseline (row distance above it * slant), resampling horizontally with
// linear interpolation so the slanted strokes stay smooth. Replaces gb->bitmap/w/x.
static void shearGlyph(GlyphBitmap *gb, float slant)
{
   if (!gb->bitmap || gb->w <= 0 || gb->h <= 0) return;

   float shiftTop = slant * (-(float)gb->y - 0.5f);
   float shiftBot = slant * (-(float)(gb->y + gb->h - 1) - 0.5f);
   float lo = shiftTop < shiftBot ? shiftTop : shiftBot;
   float hi = shiftTop < shiftBot ? shiftBot : shiftTop;
   int base = (int)floorf(lo);
   int nw = gb->w + (int)ceilf(hi) - base + 2;

   uint8_t *out = (uint8_t *)calloc((size_t)nw * (size_t)gb->h, 1);
   if (!out) return;

   for (int r = 0; r < gb->h; r++) {
      float shift = slant * (-(float)(gb->y + r) - 0.5f);
      float pos = shift - (float)base;
      int ip = (int)floorf(pos);
      float fr = pos - (float)ip;
      const uint8_t *src = gb->bitmap + (size_t)r * gb->w;
      uint8_t *dst = out + (size_t)r * nw;
      for (int c = 0; c < gb->w; c++) {
         int v = src[c];
         if (v == 0) continue;
         int a1 = (int)((float)v * fr + 0.5f);
         int a0 = v - a1;
         int i0 = ip + c, i1 = ip + c + 1;
         if (i0 >= 0 && i0 < nw) { int t = dst[i0] + a0; dst[i0] = (uint8_t)(t > 255 ? 255 : t); }
         if (i1 >= 0 && i1 < nw) { int t = dst[i1] + a1; dst[i1] = (uint8_t)(t > 255 ? 255 : t); }
      }
   }
   free(gb->bitmap);
   gb->bitmap = out;
   gb->w = nw;
   gb->x += base;
}

// Rasterises one code point at `px` with the pen at fractional x `penX`. 0 on success (a blank glyph
// succeeds with bitmap == NULL), -1 when no face could be read. Free gb->bitmap afterwards.
static int rasterGlyph(Font *f, uint32_t code, float px, int italic, float penX, GlyphBitmap *gb)
{
   memset(gb, 0, sizeof(*gb));
   gb->ox = (int)floorf(penX);
   if (isInvisible(code)) return 0;

   GlyphRef r = lookupGlyph(f, code);
   if (!r.face) return -1;

   float s = faceScale(r.face, px);
   int adv = 0, lsb = 0;
   stbtt_GetGlyphHMetrics(&r.face->info, r.glyph, &adv, &lsb);
   gb->advance = (float)adv * s;

   float shiftX = penX - floorf(penX);
   int x0, y0, x1, y1;
   stbtt_GetGlyphBitmapBoxSubpixel(&r.face->info, r.glyph, s, s, shiftX, 0.0f, &x0, &y0, &x1, &y1);
   int w = x1 - x0, h = y1 - y0;
   if (w <= 0 || h <= 0 || (size_t)w * (size_t)h > (size_t)(4u * 1024u * 1024u)) return 0;

   gb->bitmap = (uint8_t *)calloc((size_t)w * (size_t)h, 1);
   if (!gb->bitmap) return -1;
   stbtt_MakeGlyphBitmapSubpixel(&r.face->info, gb->bitmap, w, h, w, s, s, shiftX, 0.0f, r.glyph);
   gb->w = w; gb->h = h; gb->x = x0; gb->y = y0;
   gb->bearingY = (float)(-y0);
   if (italic) shearGlyph(gb, FONT_ITALIC_SLANT);
   return 0;
}

// Source-over composite of one glyph pixel (coverage a, colour cr/cg/cb) onto the surface
// pixel d = [a, r, g, b]. A plain write when the destination is empty, so the common case
// (text over a cleared surface) costs one branch.
static inline void composePixel(uint8_t *d, int a, uint8_t cr, uint8_t cg, uint8_t cb)
{
   if (a <= 0) return;
   int da = d[0];
   if (da == 0 || a >= 255) { d[0] = (uint8_t)a; d[1] = cr; d[2] = cg; d[3] = cb; return; }
   int outA = a + da * (255 - a) / 255;
   if (outA <= 0) return;
   d[1] = (uint8_t)((cr * a + d[1] * da * (255 - a) / 255) / outA);
   d[2] = (uint8_t)((cg * a + d[2] * da * (255 - a) / 255) / outA);
   d[3] = (uint8_t)((cb * a + d[3] * da * (255 - a) / 255) / outA);
   d[0] = (uint8_t)outA;
}

// Blits a rendered glyph's alpha bitmap into the surface, src-over composited.
//   gx/gy     where the bitmap's top-left lands in the surface
//   dx/dy     offset from the glyph's nominal position (used by the shadow pass)
//   argb      colour; its ALPHA scales the glyph coverage (semi-transparent shadows/tags)
//   overhang  synthetic-bold strength in px (0 = regular). Faithful to FreeType's
//             FT_Bitmap_Embolden (src/base/ftbitmap.c, 8-bit gray path): each output pixel
//             is the SATURATING SUM of itself plus the `overhang` ORIGINAL pixels to its
//             left (a max would leave antialiased stroke interiors semi-transparent and
//             render thin, uneven bold), and the output is `overhang` columns wider so the
//             rightmost stroke thickens too (FreeType widens the bitmap the same way).
//             Reading from the glyph's own untouched bitmap matches FreeType's
//             right-to-left in-place pass. The caller widens the pen advance separately.
// Clipped against the surface bounds.
static void blitGlyph(const GlyphBitmap *gb, int gx, int gy, uint8_t *surfBase, int surfW, int surfH,
                      int dx, int dy, uint32_t argb, int overhang)
{
   if (!gb->bitmap) return;
   const uint8_t *img = gb->bitmap;
   int imgW = gb->w, imgH = gb->h;
   int surfBW = surfW * 4;
   uint8_t cr = (uint8_t)((argb >> 16) & 0xFF), cg = (uint8_t)((argb >> 8) & 0xFF), cb = (uint8_t)(argb & 0xFF);
   int alpha = (int)((argb >> 24) & 0xFF);
   int outW = imgW + overhang;
   for (int iy = 0; iy < imgH; iy++) {
      int ty = gy + iy + dy;
      if (ty < 0 || ty >= surfH) continue;
      const uint8_t *srow = img + iy * imgW;
      uint8_t *drow = surfBase + ty * surfBW;
      for (int ix = 0; ix < outW; ix++) {
         int tx = gx + ix + dx;
         if (tx < 0 || tx >= surfW) continue;
         int a = (ix < imgW) ? srow[ix] : 0;
         for (int k = 1; k <= overhang && a < 255; k++) {
            int sx = ix - k;
            if (sx < 0) break;
            if (sx < imgW) a += srow[sx];
         }
         if (a > 255) a = 255;
         if (alpha < 255) a = a * alpha / 255;
         composePixel(drow + tx * 4, a, cr, cg, cb);
      }
   }
}

// The line box for `size`: baseline offset from the line top and the line pitch. Ren'Py does it from the
// FreeType (hhea) ascender/descender (renpy/text/ftfont.pyx): ascent = ceil(ascender*size/upm), descent =
// floor(descender*size/upm), line height = ascent - descent (descent negative). A face without usable hhea
// metrics falls back to stb_truetype's vertical metrics.
static void getLineMetrics(Font *f, float fsize, float *lineH, float *baseY)
{
   float asc, dsc;
   if (f->unitsPerEm > 0) {
      asc = ceilf((float)f->hheaAscent  * fsize / (float)f->unitsPerEm);
      dsc = floorf((float)f->hheaDescent * fsize / (float)f->unitsPerEm);   // descender is negative
   } else {
      SharedFace *fc = f->data->faces[0];
      int a = 0, d = 0, gap = 0;
      stbtt_GetFontVMetrics(&fc->info, &a, &d, &gap);
      float s = faceScale(fc, fsize);
      asc = ceilf((float)a * s);
      dsc = floorf((float)d * s);
   }
   *baseY = asc;
   *lineH = asc - dsc;
}

// width of the word starting at items[j0] (up to the next space/newline), including
// kerning inside the word and for the (prevCode -> first glyph) pair. Used by the wrap
// decision: a line breaks when "line + space + word" would exceed the target width.
static float getWordWidth(Font *f, const GlyphItem *items, int nItems, int j0, uint32_t prevCode, float fsize)
{
   float w = 0.0f;
   uint32_t prev = prevCode;
   for (int j = j0; j < nItems && items[j].code != ' ' && items[j].code != '\n'; j++) {
      w += getGlyphAdvance(f, items[j].code, fsize) + fontKern(f, prev, items[j].code, fsize);
      prev = items[j].code;
   }
   return w;
}

// records where laid-out item `index` ends (pen x + which wrapped line it's on) for a typewriter
// reveal -- taken from the real render pass, so the map can never desync from the texture.
static void recordRevealItem(TextReveal *reveal, int index, float penX, int line)
{
   if (index < TEXT_REVEAL_MAX) { reveal->endX[index] = (int)(penX + 0.5f); reveal->line[index] = line; }
}

// The x offset of a laid-out line within the text block for a given alignment: 0 for LEFT,
// (blockW - lineW)/2 for CENTER, (blockW - lineW) for RIGHT. blockW = the longest line, so
// the longest line gets offset 0 and the block crops to it.
static float getLineStartOffset(int align, const float *lineW, float maxLineW, int lineCount, int li)
{
   if (!lineW || align == TEXT_ALIGN_LEFT || li < 0 || li >= lineCount) return 0.0f;
   float d = maxLineW - lineW[li]; if (d < 0.0f) d = 0.0f;
   return (align == TEXT_ALIGN_CENTER) ? d * 0.5f : d;
}

// rasterizes text into a CPU buffer. returns the actual content dimensions
// via outW/outH. surfW is the buffer row width (may be larger than content).
// caller must free the returned buffer. `end` (optional) receives where the
// final glyph landed (texture coords). `align` aligns each line within the block.
static uint8_t *rasterize(Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap,
                          const TextShadow *shadow, TextEnd *end, TextReveal *reveal, int align, int rawMode,
                          int *outW, int *outH, int *outSurfW)
{
   *outW = *outH = *outSurfW = 0;

   int sdx = 0, sdy = 0, hasShadow = 0;
   uint32_t shadowArgb = 0;
   if (shadow && (shadow->dx != 0 || shadow->dy != 0)) {
      sdx = shadow->dx; sdy = shadow->dy; shadowArgb = shadow->color; hasShadow = 1;
   }

   float fsize = (float)size;

   // Parse inline style tags into glyph items; layout/render walk this array.
   int cap = (int)strlen(text) + 1;
   GlyphItem *items = (GlyphItem *)malloc(sizeof(GlyphItem) * cap);
   if (!items) return NULL;
   int nItems = parseStyledText(text, color, items, cap, rawMode);

   float lineH, baseY;
   getLineMetrics(f, fsize, &lineH, &baseY);

   // count lines for height calculation (must mirror the wrap logic in the render loop;
   // kerning resets per line, since each laid-out line is measured independently)
   int lineCount = 1;
   if (wrap == TEXT_WRAP && maxWidth > 0) {
      float px = 0.0f;
      uint32_t prev = 0;
      for (int i = 0; i < nItems; i++) {
         uint32_t code = items[i].code;
         if (code == '\n') { lineCount++; px = 0.0f; prev = 0; continue; }
         float adv = getGlyphAdvance(f, code, fsize) + fontKern(f, prev, code, fsize);
         if (code == ' ') {
            float wordW = getWordWidth(f, items, nItems, i + 1, ' ', fsize);
            if (px + adv + wordW > (float)maxWidth && px > 0.0f) { lineCount++; px = 0.0f; prev = 0; continue; }
         }
         px += adv;
         prev = code;
      }
   }

   if (reveal) { reveal->lineHeight = (int)(lineH + 0.5f); reveal->count = 0; }

   // For CENTER/RIGHT alignment, measure each line's width (mirroring the wrap logic above
   // and the render pass exactly -- getGlyphAdvance == the rendered pen step) so each line can be
   // offset within the block at draw time. The wrap DECISIONS still use the line-relative pen,
   // so breaks are identical; only the draw x is shifted.
   float *lineW = NULL; float maxLineW = 0.0f;
   if (align != TEXT_ALIGN_LEFT && wrap == TEXT_WRAP && maxWidth > 0 && lineCount > 0) {
      lineW = (float *)malloc(sizeof(float) * lineCount);
      if (lineW) {
         int li = 0; float px = 0.0f; uint32_t prev = 0;
         for (int i = 0; i < nItems && li < lineCount; i++) {
            uint32_t code = items[i].code;
            if (code == '\n') { lineW[li++] = px; px = 0.0f; prev = 0; continue; }
            float adv = getGlyphAdvance(f, code, fsize) + fontKern(f, prev, code, fsize);
            if (code == ' ') {
               float wordW = getWordWidth(f, items, nItems, i + 1, ' ', fsize);
               if (px + adv + wordW > (float)maxWidth && px > 0.0f) { lineW[li++] = px; px = 0.0f; prev = 0; continue; }
            }
            px += adv; prev = code;
         }
         if (li < lineCount) lineW[li] = px;
         for (int k = 0; k < lineCount; k++) if (lineW[k] > maxLineW) maxLineW = lineW[k];
      }
   }

   int surfW = (maxWidth > 0) ? maxWidth : FONT_MAX_RENDER_W;
   int surfH = (int)(lineH * lineCount + fsize) + 4;
   if (hasShadow && sdy > 0) surfH += sdy;   // room for the shadow below the last line

   size_t bufSize64 = (size_t)surfW * (size_t)surfH * 4u;
   if (bufSize64 == 0 || bufSize64 > FONT_MAX_SURFACE_BYTES) { free(items); free(lineW); return NULL; }
   int bufSize = (int)bufSize64;
   uint8_t *buf = (uint8_t *)malloc(bufSize);
   if (!buf) { free(items); free(lineW); return NULL; }
   memset(buf, 0, bufSize);

   // Italic is a shear of the glyph bitmap (see shearGlyph). Bold is synthetic emboldening at blit time
   // (see blitGlyph), so tag-free text renders exactly like the default path.
   //
   // The text is drawn in up to two passes: an optional SHADOW pass (every glyph in the
   // shadow colour, offset by sdx/sdy) and then the text pass composited over it -- the
   // classic order, so a later glyph's shadow never darkens an earlier glyph.
   int maxX = 0;
   int inkMinY = surfH, inkMaxY = 0;   // union ink box over all (non-shadow) glyphs

   float ellipsisW = 0.0f;
   if (wrap == TEXT_NOWRAP_ELLIPSIS && maxWidth > 0) {
      for (int i = 0; i < 3; i++) ellipsisW += measureFontChar(f, size, '.');
   }

   for (int pass = hasShadow ? 0 : 1; pass < 2; pass++) {
      int shadowPass = (pass == 0);
      int bdx = shadowPass ? sdx : 0;
      int bdy = shadowPass ? sdy : 0;
      int lineIndex = 0;   // which wrapped line the pen is on (for the reveal map)
      float penX = 0.0f;   // line-relative pen (wrap/kern/advance all use this; offset added at draw)
      float lineOffX = getLineStartOffset(align, lineW, maxLineW, lineCount, 0);   // this line's draw offset
      float penY = baseY;   // top of the current line cell; the baseline sits baseY below it
      uint32_t prev = 0;   // previous glyph on this line (kerning context; resets per line)

      for (int i = 0; i < nItems; i++) {
         uint32_t code = items[i].code;

         if (code == '\n') {
            if (wrap == TEXT_WRAP) { penX = 0.0f; penY += lineH; prev = 0; lineIndex++; lineOffX = getLineStartOffset(align, lineW, maxLineW, lineCount, lineIndex); if (penY + lineH > (float)surfH) break; }
            if (!shadowPass && reveal) recordRevealItem(reveal, i, penX, lineIndex);
            continue;
         }

         float kern = fontKern(f, prev, code, fsize);
         float advance = getGlyphAdvance(f, code, fsize) + kern;

         if (maxWidth > 0) {
            if (wrap == TEXT_WRAP) {
               if (code == ' ') {
                  float wordW = getWordWidth(f, items, nItems, i + 1, ' ', fsize);
                  if (penX + advance + wordW > (float)maxWidth && penX > 0.0f) {
                     penX = 0.0f; penY += lineH; prev = 0; lineIndex++;   // the space wraps to the next line
                     lineOffX = getLineStartOffset(align, lineW, maxLineW, lineCount, lineIndex);
                     if (!shadowPass && reveal) recordRevealItem(reveal, i, penX, lineIndex);
                     if (penY + lineH > (float)surfH) break;
                     continue;
                  }
               }
            } else if (wrap == TEXT_NOWRAP_ELLIPSIS) {
               if (penX + advance > (float)maxWidth - ellipsisW && i + 1 < nItems) {
                  int baseline = (int)floorf(penY + baseY + 0.5f);
                  for (int d = 0; d < 3; d++) {
                     GlyphBitmap gb;
                     if (rasterGlyph(f, '.', fsize, 0, penX + lineOffX, &gb) == 0) {
                        blitGlyph(&gb, gb.ox + gb.x, baseline + gb.y, buf, surfW, surfH, bdx, bdy, shadowPass ? shadowArgb : color, 0);
                        penX += gb.advance;
                        free(gb.bitmap);
                     }
                  }
                  int endX = (int)(penX + lineOffX + 0.5f); if (endX > maxX) maxX = endX;
                  break;
               }
            } else {
               if (penX + advance > (float)maxWidth) break;
            }
         }

         penX += kern;   // kerning positions THIS glyph (already counted in `advance`)

         GlyphBitmap gb;
         int ret = rasterGlyph(f, code, fsize, items[i].italic, penX + lineOffX, &gb);
         if (ret == 0) {
            // synthetic-bold strength = pixel size / 10 (the FreeType convention; 0 at
            // tiny sizes = no embolden), and the pen advance widens by the same amount.
            int overhang = items[i].bold ? ((int)(fsize + 0.5f)) / 10 : 0;
            int gx = gb.ox + gb.x;
            int gy = (int)floorf(penY + baseY + 0.5f) + gb.y;
            blitGlyph(&gb, gx, gy, buf, surfW, surfH, bdx, bdy,
                      shadowPass ? shadowArgb : items[i].color, overhang);
            penX += quantAdvance(f, gb.advance) + (float)overhang;
            int endX = (int)(penX + lineOffX + 0.5f); if (endX > maxX) maxX = endX;
            if (!shadowPass) {
               if (gb.bitmap) {
                  if (gy < inkMinY) inkMinY = gy;                                   // union ink box
                  if (gy + gb.h > inkMaxY) inkMaxY = gy + gb.h;
               }
               if (end) {
                  end->endX = endX;
                  end->endTop = gy;
                  end->endBottom = gy + gb.h;
                  // Line cell top = baseline - ascent. The glyph's ink top gy sits
                  // bearingY above the baseline, so baseline = gy + bearingY, and the
                  // cell top (the top edge of the line box, where a caller top-aligns an
                  // inline decoration) = gy + bearingY - ascent. Same units as gy.
                  end->lineTop = gb.bitmap ? gy + (int)(gb.bearingY + 0.5f) - (int)baseY
                                           : (int)floorf(penY + 0.5f);
                  end->valid = 1;
               }
            }
            free(gb.bitmap);
         } else {
            penX += fsize;
         }
         if (!shadowPass && reveal) recordRevealItem(reveal, i, penX, lineIndex);
         prev = code;
      }
   }

   if (reveal) reveal->count = nItems < TEXT_REVEAL_MAX ? nItems : TEXT_REVEAL_MAX;

   // crop the blank rows above the first line's glyphs
   int skip = (int)baseY + 4;
   if (end && end->valid) {
      end->endTop -= skip;    if (end->endTop < 0) end->endTop = 0;
      end->endBottom -= skip; if (end->endBottom < 0) end->endBottom = 0;
      end->lineTop -= skip;   // NOT clamped: the cell top can legitimately be above row 0
      end->lineHeight = (int)(lineH + 0.5f);
      end->inkTop    = (inkMinY <= inkMaxY) ? inkMinY - skip : 0;  if (end->inkTop < 0) end->inkTop = 0;
      end->inkBottom = (inkMinY <= inkMaxY) ? inkMaxY - skip : 0;  if (end->inkBottom < 0) end->inkBottom = 0;
   }
   *outW = maxX + (hasShadow && sdx > 0 ? sdx : 0);
   if (*outW > surfW) *outW = surfW;
   *outH = surfH - skip;
   *outSurfW = surfW;

   free(items);
   free(lineW);

   if (*outW <= 0 || *outH <= 0) { free(buf); return NULL; }

   uint8_t *cropped = buf + skip * surfW * 4;
   memmove(buf, cropped, (*outH) * surfW * 4);

   return buf;
}

void freeTextTexture(TextTexture *tt)
{
   if (tt->valid) {
      retireGfxTexture(&tt->tex);   // given back once the frame that may name it has been shown
      tt->valid = 0;
   }
   tt->slotW = 0;
   tt->slotH = 0;
}

static void renderImpl(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap, const TextShadow *shadow, TextEnd *end, TextReveal *reveal, int align, int rawMode);

void renderFont(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap)
{
   renderFontEx(tt, f, size, text, color, maxWidth, wrap, NULL, NULL);
}

void renderFontEx(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap, const TextShadow *shadow, TextEnd *end)
{
   renderFontTyped(tt, f, size, text, color, maxWidth, wrap, shadow, end, NULL);
}

void renderFontAligned(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap, const TextShadow *shadow, TextEnd *end, TextAlign align)
{
   renderImpl(tt, f, size, text, color, maxWidth, wrap, shadow, end, NULL, align, 0);
}

void renderFontTyped(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap, const TextShadow *shadow, TextEnd *end, TextReveal *reveal)
{
   renderImpl(tt, f, size, text, color, maxWidth, wrap, shadow, end, reveal, TEXT_ALIGN_LEFT, 0);
}

void renderFontRaw(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap)
{
   renderImpl(tt, f, size, text, color, maxWidth, wrap, NULL, NULL, NULL, TEXT_ALIGN_LEFT, 1);
}

// renders ONE glyph at `size` px into its own tightly-cropped white texture (each pixel [A,R,G,B] with
// RGB = 255 and A = coverage). Unlike renderFont this uses no line box and no top crop, so tall glyphs
// (icon fonts) are never clipped - the texture IS the glyph's own bitmap. *offsetX/*offsetY (may be NULL)
// receive the glyph ink's position relative to the em-box top-left, so glyphs of the same size drawn at a
// common anchor line up exactly as the font intends (no scaling/fitting needed). 0 + fills the outputs on
// success, -1 on failure; free *out with freeGfxTexture. Rasterise once at init (never per frame).
int renderGlyphTexture(Font *f, int size, uint32_t code, GfxTexture *out, int *offsetX, int *offsetY)
{
   if (!f->open || size <= 0) return -1;

   GlyphBitmap gb;
   if (rasterGlyph(f, code, (float)size, 0, 0.0f, &gb) != 0) return -1;
   int w = gb.w, h = gb.h;
   if (!gb.bitmap || w <= 0 || h <= 0) { free(gb.bitmap); return -1; }

   uint32_t *argb = (uint32_t *)malloc((size_t)w * h * 4);
   if (!argb) { free(gb.bitmap); return -1; }
   for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++)
         argb[y * w + x] = ((uint32_t)gb.bitmap[y * w + x] << 24) | 0x00FFFFFFu;
   free(gb.bitmap);

   uint32_t offset = uploadGfxTexture(argb, w, h, w * 4);
   free(argb);
   if (!offset) return -1;

   out->offset = offset;
   out->w = w;
   out->h = h;
   out->pitch = (int)(((uint32_t)(w * 4) + 63u) & ~63u);

   // where the ink sits inside the em box: ink top-left = (bearingX, ascent - bearingY) from the em top.
   int ascentPx = (f->unitsPerEm > 0) ? (int)ceilf((float)f->hheaAscent * (float)size / (float)f->unitsPerEm) : size;
   if (offsetX) *offsetX = gb.x;
   if (offsetY) *offsetY = ascentPx - (int)(gb.bearingY + 0.5f);
   return 0;
}

static void renderImpl(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap, const TextShadow *shadow, TextEnd *end, TextReveal *reveal, int align, int rawMode)
{
   if (end) memset(end, 0, sizeof *end);
   if (reveal) reveal->count = 0;
   if (!f->open) return;

   // clear case: hide the texture but keep the slot for reuse (a permanently
   // empty label is released at teardown via freeTextTexture, not here, so we
   // don't stall the RSX clearing rows during scrolling).
   if (!text || !text[0]) {
      tt->tex.w = 0;
      tt->tex.h = 0;
      return;
   }

   int drawW, drawH, surfW;
   uint8_t *buf = rasterize(f, size, text, color, maxWidth, wrap, shadow, end, reveal, align, rawMode, &drawW, &drawH, &surfW);
   if (!buf) {
      // nothing rasterized (e.g. all-whitespace after wrapping): keep any
      // existing slot but show nothing.
      tt->tex.w = 0;
      tt->tex.h = 0;
      return;
   }

   // A slot the GPU may still be sampling cannot be overwritten, and waiting for it to finish shows
   // as a stutter and as torn text when a screen changes a lot of labels at once. The new picture
   // goes somewhere else and the old slot is handed back once this frame has been shown.
   freeTextTexture(tt);

   uint32_t offset = uploadGfxTexture(buf, drawW, drawH, surfW * 4);
   free(buf);
   if (offset == 0) {
      // upload failed (out of VRAM): leave the slot empty.
      tt->tex.w = 0;
      tt->tex.h = 0;
      return;
   }

   tt->tex.offset = offset;
   tt->tex.w = drawW;
   tt->tex.h = drawH;
   tt->tex.pitch = (drawW * 4 + 63) & ~63;
   tt->slotW = drawW;
   tt->slotH = drawH;
   tt->valid = 1;
}
