// Host test for libs/simple-lib-app/src/font.c (the stb_truetype text path that replaced libfont).
// Compiles the real font.c against the shims in font-include/ and renders text with the real firmware
// fonts (-DFONT_SYSTEM_DIR points at a copy of /dev_flash/data/font). Writes a contact sheet to
// $FONT_SHEET (default /tmp/yo-font-sheet.png) so the output can be looked at, not just counted.
//
//   -DNO_SYSDIR  builds the "firmware fonts unreadable" variant: only the embedded Latin face exists,
//                and text in the Latin script must still render.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "font.h"
#include "gfx.h"
#include "vfs.h"

int hostVerbose = 0;

// ---- shims -------------------------------------------------------------------------------------------

static uint8_t *lastTex;
static int lastW, lastH;

uint32_t uploadGfxTexture(const void *rgba, int w, int h, int srcPitch)
{
   free(lastTex);
   lastTex = (uint8_t *)malloc((size_t)w * h * 4);
   for (int y = 0; y < h; y++) memcpy(lastTex + (size_t)y * w * 4, (const uint8_t *)rgba + (size_t)y * srcPitch, (size_t)w * 4);
   lastW = w; lastH = h;
   return 0x1000;
}
void retireGfxTexture(GfxTexture *tex) { (void)tex; }

int statPath(const char *path, VfsStat *st)
{
   FILE *f = fopen(path, "rb");
   if (!f) return -1;
   fseek(f, 0, SEEK_END);
   memset(st, 0, sizeof(*st));
   st->size = (uint64_t)ftell(f);
   fclose(f);
   return 0;
}
int openFs(const char *path, int flags, VfsFile *file)
{
   (void)flags;
   memset(file, 0, sizeof(*file));
   file->host = fopen(path, "rb");
   return file->host ? 0 : -1;
}
int64_t readFs(VfsFile *file, void *buffer, uint64_t length)
{
   return (int64_t)fread(buffer, 1, (size_t)length, (FILE *)file->host);
}
int closeFs(VfsFile *file) { if (file->host) fclose((FILE *)file->host); file->host = NULL; return 0; }

// ---- check helpers -----------------------------------------------------------------------------------

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static long inkOf(const uint8_t *tex, int w, int h)
{
   long sum = 0;
   for (int i = 0; i < w * h; i++) sum += tex[i * 4];   // byte 0 of each pixel is alpha
   return sum;
}

// ---- contact sheet (grayscale PNG, stored deflate) ---------------------------------------------------

static uint8_t *sheet; static int sheetW, sheetH, sheetY;
#define SHEET_MAX_H 2400

static void sheetAdd(const char *label, const uint8_t *tex, int w, int h)
{
   (void)label;
   if (!sheet) { sheetW = 1100; sheetH = SHEET_MAX_H; sheet = (uint8_t *)calloc((size_t)sheetW * sheetH, 1); }
   if (!tex || w <= 0 || h <= 0 || sheetY + h + 6 > sheetH) return;
   for (int y = 0; y < h; y++)
      for (int x = 0; x < w && x < sheetW; x++)
         sheet[(size_t)(sheetY + y) * sheetW + x] = tex[((size_t)y * w + x) * 4];
   sheetY += h + 6;
}

static uint32_t crcTable[256];
static uint32_t crc32b(uint32_t crc, const uint8_t *p, size_t n)
{
   if (!crcTable[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; crcTable[i] = c; }
   crc = ~crc;
   while (n--) crc = crcTable[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
   return ~crc;
}
static void put32(FILE *f, uint32_t v) { fputc(v >> 24, f); fputc(v >> 16, f); fputc(v >> 8, f); fputc(v, f); }
static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n)
{
   put32(f, n); fwrite(type, 1, 4, f); if (n) fwrite(data, 1, n, f);
   uint32_t c = crc32b(0, (const uint8_t *)type, 4); if (n) c = crc32b(c, data, n);
   put32(f, c);
}
static void writeSheet(const char *path)
{
   if (!sheet) return;
   int h = sheetY > 0 ? sheetY : 1;
   size_t raw = (size_t)(sheetW + 1) * h;
   uint8_t *rawbuf = (uint8_t *)malloc(raw);
   for (int y = 0; y < h; y++) { rawbuf[(size_t)y * (sheetW + 1)] = 0; memcpy(rawbuf + (size_t)y * (sheetW + 1) + 1, sheet + (size_t)y * sheetW, sheetW); }
   size_t nblocks = (raw + 65534) / 65535;
   uint8_t *z = (uint8_t *)malloc(raw + nblocks * 5 + 6);
   size_t zp = 0; z[zp++] = 0x78; z[zp++] = 0x01;
   uint32_t a = 1, b = 0;
   for (size_t i = 0; i < raw; i++) { a = (a + rawbuf[i]) % 65521; b = (b + a) % 65521; }
   for (size_t off = 0; off < raw; off += 65535) {
      size_t n = raw - off > 65535 ? 65535 : raw - off;
      z[zp++] = (off + n >= raw) ? 1 : 0; z[zp++] = n & 0xFF; z[zp++] = n >> 8; z[zp++] = ~n & 0xFF; z[zp++] = (~n >> 8) & 0xFF;
      memcpy(z + zp, rawbuf + off, n); zp += n;
   }
   uint32_t ad = (b << 16) | a; z[zp++] = ad >> 24; z[zp++] = ad >> 16; z[zp++] = ad >> 8; z[zp++] = ad;
   FILE *f = fopen(path, "wb");
   if (f) {
      static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
      fwrite(sig, 1, 8, f);
      uint8_t ihdr[13] = { (uint8_t)(sheetW >> 24), (uint8_t)(sheetW >> 16), (uint8_t)(sheetW >> 8), (uint8_t)sheetW,
                           (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 0, 0, 0, 0 };
      chunk(f, "IHDR", ihdr, 13); chunk(f, "IDAT", z, (uint32_t)zp); chunk(f, "IEND", NULL, 0);
      fclose(f);
   }
   free(rawbuf); free(z);
}

// ---- tests -------------------------------------------------------------------------------------------

static TextTexture tt;

// renders and returns the ink total; adds the result to the sheet
static long draw(Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap)
{
   renderFont(&tt, f, size, text, color, maxWidth, wrap);
   if (tt.tex.w <= 0) return 0;
   long ink = inkOf(lastTex, lastW, lastH);
   sheetAdd(text, lastTex, lastW, lastH);
   return ink;
}

static void testLatin(Font *f)
{
   long ink = draw(f, 28, "Hello, World - the quick brown fox 0123456789", 0xFFFFFFFF, 0, TEXT_NOWRAP);
   CHECK(ink > 20000, "latin ink %ld", ink);
   CHECK(tt.tex.h >= 28, "latin height %d", tt.tex.h);
   float m = measureFontText(f, 28, "Hello, World - the quick brown fox 0123456789");
   CHECK(fabsf(m - (float)tt.tex.w) <= 1.5f, "measure %.1f vs texture width %d", m, tt.tex.w);

   // a wider size makes wider text
   CHECK(measureFontText(f, 40, "Hello") > measureFontText(f, 20, "Hello") * 1.8f, "advance does not scale with size");
   CHECK(measureFontChar(f, 28, 'W') > measureFontChar(f, 28, 'i'), "W should be wider than i");
   CHECK(measureFontChar(f, 28, ' ') > 3.0f, "space has no advance");

   // empty / NULL clear the texture rather than crash
   renderFont(&tt, f, 28, "", 0xFFFFFFFF, 0, TEXT_NOWRAP);
   CHECK(tt.tex.w == 0, "empty text kept a texture");
   renderFont(&tt, f, 28, NULL, 0xFFFFFFFF, 0, TEXT_NOWRAP);
   CHECK(tt.tex.w == 0, "NULL text kept a texture");
   CHECK(measureFontText(f, 28, NULL) == 0.0f, "measure(NULL)");

   // styled runs
   long plain = draw(f, 28, "Bold and italic and red", 0xFFFFFFFF, 0, TEXT_NOWRAP);
   long styled = draw(f, 28, "{b}Bold{/b} and {i}italic{/i} and {color=#ff0000}red{/color}", 0xFFFFFFFF, 0, TEXT_NOWRAP);
   CHECK(styled > plain, "bold added no ink (%ld vs %ld)", styled, plain);
   // the {color} run is red: green/blue bytes of its pixels must be 0 somewhere with ink
   int red = 0;
   for (int i = 0; i < lastW * lastH; i++) { const uint8_t *p = lastTex + i * 4; if (p[0] > 128 && p[1] == 255 && p[2] == 0 && p[3] == 0) red++; }
   CHECK(red > 50, "no red pixels from {color=#ff0000} (%d)", red);

   // wrapping
   long wr = draw(f, 24, "A fairly long sentence that has to wrap onto several lines when the box is narrow.", 0xFFFFFFFF, 300, TEXT_WRAP);
   CHECK(wr > 0 && tt.tex.h > 24 * 2, "wrap produced %d px of height", tt.tex.h);
   CHECK(tt.tex.w <= 300, "wrapped text overflowed: %d", tt.tex.w);

   // ellipsis
   long el = draw(f, 24, "A title that is definitely much too long to fit in two hundred pixels", 0xFFFFFFFF, 200, TEXT_NOWRAP_ELLIPSIS);
   CHECK(el > 0 && tt.tex.w <= 200, "ellipsis width %d", tt.tex.w);

   // shadow + end report + reveal map + alignment
   TextShadow sh = { 2, 2, 0xFF000000 };
   TextEnd end; TextReveal rv;
   renderFontTyped(&tt, f, 28, "Shadow test", 0xFFFFFFFF, 0, TEXT_NOWRAP, &sh, &end, &rv);
   CHECK(end.valid && end.endX > 80 && end.lineHeight >= 28, "end report endX=%d lh=%d", end.endX, end.lineHeight);
   CHECK(rv.count == 11 && rv.endX[10] >= rv.endX[0], "reveal count %d", rv.count);
   sheetAdd("shadow", lastTex, lastW, lastH);
   renderFontAligned(&tt, f, 24, "short\na much longer line here", 0xFFFFFFFF, 400, TEXT_WRAP, NULL, NULL, TEXT_ALIGN_CENTER);
   CHECK(tt.tex.w > 0, "aligned render");
   sheetAdd("centered", lastTex, lastW, lastH);

   // degenerate sizes and hostile input must not crash
   renderFont(&tt, f, 0, "zero", 0xFFFFFFFF, 0, TEXT_NOWRAP);
   renderFont(&tt, f, 1, "one", 0xFFFFFFFF, 0, TEXT_NOWRAP);
   renderFont(&tt, f, 200, "Big", 0xFFFFFFFF, 0, TEXT_NOWRAP);
   renderFont(&tt, f, 24, "bad utf8 \xff\xfe\xc3 \xe3\x81 end", 0xFFFFFFFF, 0, TEXT_NOWRAP);
   renderFont(&tt, f, 24, "ctrl\x01\x02\x7f chars\t and zero\xe2\x80\x8bwidth\xef\xb8\x8f", 0xFFFFFFFF, 0, TEXT_NOWRAP);
   char *big = (char *)malloc(20001);
   memset(big, 'x', 20000); big[20000] = 0;
   for (int i = 7; i < 20000; i += 8) big[i] = ' ';
   renderFont(&tt, f, 24, big, 0xFFFFFFFF, 600, TEXT_WRAP);
   renderFont(&tt, f, 24, big, 0xFFFFFFFF, 0, TEXT_NOWRAP);
   free(big);
}

#ifndef NO_SYSDIR
static void testScripts(Font *f)
{
   static const struct { const char *label, *text; } cases[] = {
      { "accented latin", "Ünïcödé àéîõü çñ ß Ł ő" },
      { "cyrillic", "Привет мир, как дела" },
      { "greek", "Γειά σου Κόσμε" },
      { "japanese", "こんにちは世界 カタカナ 漢字" },
      { "chinese", "你好，世界 简体中文 繁體中文" },
      { "korean", "안녕하세요 한국어" },
      { "symbols", "★ ♪ → © ® € ™ … – —" },
      { "mixed title", "【公式】Top 10 Moments — 最高の瞬間 | Official Video" },
   };
   for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      long ink = draw(f, 28, cases[i].text, 0xFFFFFFFF, 0, TEXT_NOWRAP);
      CHECK(ink > 8000, "%s: ink %ld", cases[i].label, ink);
      float m = measureFontText(f, 28, cases[i].text);
      CHECK(fabsf(m - (float)tt.tex.w) <= 1.5f, "%s: measure %.1f vs width %d", cases[i].label, m, tt.tex.w);
   }
}

static void testFonts(void)
{
   // every system set opens and draws Latin
   for (int type = 0; type < 4; type++) {
      Font f = openSystemFont(type);
      CHECK(f.open, "openSystemFont(%d) failed", type);
      if (f.open) {
         long ink = draw(&f, 26, "System font set check 123", 0xFFFFFFFF, 0, TEXT_NOWRAP);
         CHECK(ink > 10000, "set %d ink %ld", type, ink);
         closeFont(&f);
      }
   }
   // opened and closed repeatedly (every screen does this); LeakSanitizer checks the faces are freed
   for (int i = 0; i < 40; i++) {
      Font a = openSystemFont(FONT_POP), b = openSystemFont(FONT_POP);
      draw(&a, 20, "日本語", 0xFFFFFFFF, 0, TEXT_NOWRAP);
      closeFont(&a);
      draw(&b, 20, "ok", 0xFFFFFFFF, 0, TEXT_NOWRAP);
      closeFont(&b);
   }
   // a file and a memory font
   Font ff = openFontFile(FONT_SYSTEM_DIR "SCE-PS3-SR-R-LATIN.TTF");
   CHECK(ff.open, "openFontFile");
   CHECK(draw(&ff, 24, "From a file", 0xFFFFFFFF, 0, TEXT_NOWRAP) > 5000, "file font ink");
   closeFont(&ff);
   Font nf = openFontFile("/nonexistent/none.ttf");
   CHECK(!nf.open, "missing file opened");
   renderFont(&tt, &nf, 24, "x", 0xFFFFFFFF, 0, TEXT_NOWRAP);
   CHECK(measureFontText(&nf, 24, "x") == 0.0f, "closed font measured");
   FILE *fp = fopen(FONT_SYSTEM_DIR "SCE-PS3-RD-R-LATIN.TTF", "rb");
   static uint8_t mem[200000];
   size_t n = fp ? fread(mem, 1, sizeof(mem), fp) : 0;
   if (fp) fclose(fp);
   Font mf = openFontMemory(mem, (uint32_t)n);
   CHECK(mf.open, "openFontMemory");
   CHECK(draw(&mf, 24, "From memory", 0xFFFFFFFF, 0, TEXT_NOWRAP) > 5000, "memory font ink");
   uint8_t junk[64] = { 1, 2, 3 };
   Font jf = openFontMemory(junk, sizeof(junk));
   CHECK(!jf.open, "junk bytes opened as a font");
   closeFont(&mf);

   // single-glyph path used by the icon font
   Font pf = openSystemFont(FONT_POP);
   GfxTexture gt; int ox = -1, oy = -1;
   CHECK(renderGlyphTexture(&pf, 32, 'A', &gt, &ox, &oy) == 0 && gt.w > 10 && gt.h > 10, "renderGlyphTexture A");
   CHECK(renderGlyphTexture(&pf, 32, ' ', &gt, &ox, &oy) != 0, "blank glyph should report failure");
   closeFont(&pf);
}
#endif

int main(int argc, char **argv)
{
   if (argc > 1 && strcmp(argv[1], "-v") == 0) hostVerbose = 1;

   initFont();
   Font pop = openSystemFont(FONT_POP);
   CHECK(pop.open, "FONT_POP did not open");
   if (pop.open) {
      testLatin(&pop);
#ifndef NO_SYSDIR
      testScripts(&pop);
#else
      // firmware fonts unreadable: Latin still renders, and a CJK title does not crash
      CHECK(draw(&pop, 28, "Latin only still works", 0xFFFFFFFF, 0, TEXT_NOWRAP) > 10000, "embedded-only latin ink");
      draw(&pop, 28, "日本語 only boxes", 0xFFFFFFFF, 0, TEXT_NOWRAP);
#endif
      closeFont(&pop);
   }
#ifndef NO_SYSDIR
   testFonts();
#endif
   freeTextTexture(&tt);
   free(lastTex);

   const char *out = getenv("FONT_SHEET");
   writeSheet(out ? out : "/tmp/yo-font-sheet.png");
   free(sheet);

   printf("%d checks, %d failures\n", checks, failures);
   return failures ? 1 : 0;
}
