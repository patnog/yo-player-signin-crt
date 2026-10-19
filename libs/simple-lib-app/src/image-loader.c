// image-loader - PNG/JPEG decode for the image viewer.
//
// Two layers:
//   * decode-to-buffer: reads a file into a heap ARGB8888 buffer. No RSX/VRAM
//     calls, so it is safe to run on a background worker thread.
//   * upload: turns a decoded buffer into a VRAM GfxTexture. Touches the RSX
//     bump allocator, so it must run on the main thread only.
//
// requestImageAsync()/pollImageAsync() drive the background worker so a large
// image never freezes the UI; the caller uploads the result on the main thread.
#include "image-loader.h"
#include "dir-playlist.h"       // listDirFiltered (shared folder scan + sort)
#include "vfs.h"               // getExtension
#include "string-utilities.h"   // strCmpICase, strCopy
#include "dbg.h"                // logError
#include "thread.h"             // spawnThread, lwmutex helpers, sleepMs

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>
#include <jpeglib.h>
#include <cell/codec/pngdec.h>

// The RSX cannot sample a 2D texture larger than 4096x4096 -- handing it an
// oversized texture corrupts the display and hangs the GPU. So we never produce
// a buffer bigger than this: JPEG is downscaled by the codec, PNG (no codec
// downscale) is rejected when it exceeds the limit.
#define MAX_TEX_DIM 4096
#define MAX_DECODE_PIXELS (4u * 1024u * 1024u)

// ---------------------------------------------------------------------------
// format check + directory listing
// ---------------------------------------------------------------------------

int isSupportedImageFormat(const char *filename)
{
   const char *ext = getExtension(filename);
   if (!ext) return 0;
   return strCmpICase(ext, "png")  == 0 ||
         strCmpICase(ext, "jpg")  == 0 ||
         strCmpICase(ext, "jpeg") == 0;
}

int listSupportedImages(const char *dir, char names[][IMAGE_NAME_MAX], int maxCount)
{
   // IMAGE_NAME_MAX and DIR_PLAYLIST_NAME_MAX are both 256, so the array types match.
   return listDirFiltered(dir, names, maxCount, isSupportedImageFormat);
}

// ---------------------------------------------------------------------------
// decode to heap buffer (no VRAM; safe off the main thread)
// ---------------------------------------------------------------------------

// codec control-memory callbacks (heap).
static void *imgMalloc(uint32_t size, void *arg) { (void)arg; return malloc(size); }
#ifdef PSL1GHT
static void imgFree(void *ptr, void *arg)        { (void)arg; free(ptr); }
#define IMAGE_DECODE_STATUS(value) ((value).decode_status)
#define PNG_OUTPUT_WIDTH_BYTES(value) ((value).width_byte)
#else
static int32_t imgFree(void *ptr, void *arg)     { (void)arg; free(ptr); return 0; }
#define IMAGE_DECODE_STATUS(value) ((value).status)
#define PNG_OUTPUT_WIDTH_BYTES(value) ((value).outputWidthByte)
#endif

// Fill a PNG source descriptor for an in-memory buffer. We always decode from
// memory (the file is read through the VFS first) so images on userland volumes
// like /exfat0 work -- the SDK codec's own file source can only see cellFs.
static void pngSrcMem(CellPngDecSrc *s, const void *buf, uint32_t size)
{
   s->srcSelect = CELL_PNGDEC_BUFFER; s->fileName = NULL;
   s->fileOffset = 0; s->fileSize = 0; s->streamPtr = (void *)buf; s->streamSize = size;
   s->spuThreadEnable = CELL_PNGDEC_SPU_THREAD_DISABLE;
}

// decodes a PNG from a prepared source into a freshly malloc'd ARGB8888 buffer. returns 0 ok.
static int decodePng(const CellPngDecSrc *src, ImageBuffer *out)
{
   CellPngDecMainHandle mainHandle;
   CellPngDecSubHandle  subHandle;
   CellPngDecThreadInParam  threadIn;
   CellPngDecThreadOutParam threadOut;
   CellPngDecOpnInfo    openInfo;
   CellPngDecInfo       info;
   CellPngDecInParam    inParam;
   CellPngDecOutParam   outParam;
   CellPngDecDataOutInfo dataInfo;
   CellPngDecDataCtrlParam ctrl;
   int ret;

   threadIn.spuThreadEnable   = CELL_PNGDEC_SPU_THREAD_DISABLE;
   threadIn.ppuThreadPriority = 512;
   threadIn.spuThreadPriority = 200;
   threadIn.cbCtrlMallocFunc  = imgMalloc;
   threadIn.cbCtrlMallocArg   = NULL;
   threadIn.cbCtrlFreeFunc    = imgFree;
   threadIn.cbCtrlFreeArg     = NULL;

   ret = cellPngDecCreate(&mainHandle, &threadIn, &threadOut);
   if (ret != CELL_OK) return -1;

   ret = cellPngDecOpen(mainHandle, &subHandle, src, &openInfo);
   if (ret != CELL_OK) { cellPngDecDestroy(mainHandle); return -1; }

   ret = cellPngDecReadHeader(mainHandle, subHandle, &info);
   if (ret != CELL_OK) { cellPngDecClose(mainHandle, subHandle); cellPngDecDestroy(mainHandle); return -1; }

   // The PNG codec has no downscale, so an oversized PNG can't be made into a
   // legal texture -- fail cleanly rather than hand the RSX something it hangs on.
   if (info.imageWidth > MAX_TEX_DIM || info.imageHeight > MAX_TEX_DIM) {
      logError("[image-loader] PNG too large for RSX: %ux%u (max %d)\n",
               info.imageWidth, info.imageHeight, MAX_TEX_DIM);
      cellPngDecClose(mainHandle, subHandle); cellPngDecDestroy(mainHandle);
      return -1;
   }

   int hasAlpha = (info.colorSpace == CELL_PNGDEC_RGBA || info.colorSpace == CELL_PNGDEC_GRAYSCALE_ALPHA);
   inParam.commandPtr       = NULL;
   inParam.outputMode       = CELL_PNGDEC_TOP_TO_BOTTOM;
   inParam.outputColorSpace = CELL_PNGDEC_ARGB;
   inParam.outputBitDepth   = 8;
   inParam.outputPackFlag   = CELL_PNGDEC_1BYTE_PER_1PIXEL;
   inParam.outputAlphaSelect = hasAlpha ? CELL_PNGDEC_STREAM_ALPHA : CELL_PNGDEC_FIX_ALPHA;
   inParam.outputColorAlpha = 0xff;

   ret = cellPngDecSetParameter(mainHandle, subHandle, &inParam, &outParam);
   if (ret != CELL_OK) { cellPngDecClose(mainHandle, subHandle); cellPngDecDestroy(mainHandle); return -1; }

   uint32_t w = outParam.outputWidth;
   uint32_t h = outParam.outputHeight;
   uint32_t stride = (uint32_t)PNG_OUTPUT_WIDTH_BYTES(outParam);

   void *buf = malloc(stride * h);
   if (!buf) { cellPngDecClose(mainHandle, subHandle); cellPngDecDestroy(mainHandle); return -1; }

   ctrl.outputBytesPerLine = stride;
   ret = cellPngDecDecodeData(mainHandle, subHandle, (uint8_t *)buf, &ctrl, &dataInfo);
   cellPngDecClose(mainHandle, subHandle);
   cellPngDecDestroy(mainHandle);

   if (ret != CELL_OK || IMAGE_DECODE_STATUS(dataInfo) != CELL_PNGDEC_DEC_STATUS_FINISH) { free(buf); return -1; }

   out->pixels = buf;
   out->w = (int)w;
   out->h = (int)h;
   out->pitch = (int)stride;
   return 0;
}

// libjpeg's fatal-error callback normally terminates the process. Convert those
// errors to a local failure so a corrupt remote thumbnail cannot kill the app.
typedef struct {
   struct jpeg_error_mgr base;
   jmp_buf jump;
} JpegErrorState;

typedef struct {
   struct jpeg_decompress_struct decoder;
   JpegErrorState error;
   uint8_t *pixels;
   JSAMPLE *row;
} JpegDecodeState;

static void jpegDecodeError(j_common_ptr common)
{
   JpegErrorState *error = (JpegErrorState *)common->err;
   longjmp(error->jump, 1);
}

// Decode JPEG in software. The Sony JPGDEC module's create call can block on
// this console when invoked from a worker, so JPEG thumbnails must not depend
// on it. This decoder owns only heap buffers and never touches RSX state.
static int decodeJpeg(const void *data, uint32_t size, ImageBuffer *out)
{
   JpegDecodeState *state;
   struct jpeg_decompress_struct *cinfo;
   volatile uint32_t scale = 1;
   uint64_t scaledW, scaledH, pixelCount, pitch;

   if (!out) return -1;
   out->pixels = NULL;
   out->w = out->h = out->pitch = 0;
   if (!data || size == 0) return -1;

   state = (JpegDecodeState *)calloc(1, sizeof(*state));
   if (!state) return -1;
   cinfo = &state->decoder;
   cinfo->err = jpeg_std_error(&state->error.base);
   state->error.base.error_exit = jpegDecodeError;

   if (setjmp(state->error.jump)) {
      if (cinfo->mem) jpeg_destroy_decompress(cinfo);
      free(state->row);
      free(state->pixels);
      free(state);
      logError("[image-loader] software JPEG decode failed\n");
      return -1;
   }

   jpeg_create_decompress(cinfo);
   jpeg_mem_src(cinfo, (unsigned char *)data, (unsigned long)size);
   if (jpeg_read_header(cinfo, TRUE) != JPEG_HEADER_OK) goto decode_failed;

   // Keep both the texture dimensions and memory use bounded. libjpeg supports
   // 1/2, 1/4 and 1/8 output scales, which avoids allocating a full-size camera
   // image merely to make a small on-screen thumbnail.
   for (;;) {
      scaledW = ((uint64_t)cinfo->image_width + scale - 1) / scale;
      scaledH = ((uint64_t)cinfo->image_height + scale - 1) / scale;
      pixelCount = scaledW * scaledH;
      if ((scaledW <= MAX_TEX_DIM && scaledH <= MAX_TEX_DIM &&
           pixelCount <= MAX_DECODE_PIXELS) || scale == 8)
         break;
      scale <<= 1;
   }
   if (scaledW > MAX_TEX_DIM || scaledH > MAX_TEX_DIM ||
       pixelCount > MAX_DECODE_PIXELS)
      goto decode_failed;

   cinfo->scale_num = 1;
   cinfo->scale_denom = scale;
   cinfo->out_color_space = JCS_RGB;
   if (!jpeg_start_decompress(cinfo) || cinfo->output_components != 3)
      goto decode_failed;

   // Use the decoder's actual dimensions (it may round scaled dimensions up).
   if (cinfo->output_width == 0 || cinfo->output_height == 0 ||
       cinfo->output_width > MAX_TEX_DIM || cinfo->output_height > MAX_TEX_DIM ||
       (uint64_t)cinfo->output_width * cinfo->output_height > MAX_DECODE_PIXELS)
      goto decode_failed;
   pitch = (uint64_t)cinfo->output_width * 4;
   if (pitch > 0x7fffffffU || pitch * cinfo->output_height > 0x7fffffffU)
      goto decode_failed;

   state->pixels = (uint8_t *)malloc((size_t)(pitch * cinfo->output_height));
   state->row = (JSAMPLE *)malloc((size_t)cinfo->output_width * 3);
   if (!state->pixels || !state->row) goto decode_failed;

   while (cinfo->output_scanline < cinfo->output_height) {
      JDIMENSION y = cinfo->output_scanline;
      JSAMPROW row = state->row;
      uint32_t x;
      if (jpeg_read_scanlines(cinfo, &row, 1) != 1) goto decode_failed;

      // The renderer consumes native A8R8G8B8 pixels. On the PS3's big-endian
      // PPU this packed word is stored as A, R, G, B bytes.
      uint32_t *dst = (uint32_t *)(state->pixels + (uint64_t)y * pitch);
      for (x = 0; x < cinfo->output_width; ++x) {
         uint32_t r = state->row[x * 3 + 0];
         uint32_t g = state->row[x * 3 + 1];
         uint32_t b = state->row[x * 3 + 2];
         dst[x] = 0xff000000u | (r << 16) | (g << 8) | b;
      }
   }
   if (!jpeg_finish_decompress(cinfo)) goto decode_failed;

   out->pixels = state->pixels;
   out->w = (int)cinfo->output_width;
   out->h = (int)cinfo->output_height;
   out->pitch = (int)pitch;
   state->pixels = NULL;
   jpeg_destroy_decompress(cinfo);
   free(state->row);
   free(state);
   return 0;

decode_failed:
   if (cinfo->mem) jpeg_destroy_decompress(cinfo);
   free(state->row);
   free(state->pixels);
   free(state);
   logError("[image-loader] unsupported or invalid JPEG image\n");
   return -1;
}

static int decodeImageMem(const void *data, uint32_t size, ImageBuffer *out);

// Reads a file through the VFS into a heap buffer and decodes it from memory.
// Going via the VFS (rather than the SDK codec's own cellFs file source) is what
// lets images on userland volumes such as /exfat0 decode -- the codec file source
// can only see kernel cellFs devices. Works for cellFs paths too. 0 / -1.
static int decodeImageToBuffer(const char *path, ImageBuffer *out)
{
   VfsStat st;
   if (statPath(path, &st) != 0) { logError("[image-loader] stat failed: %s\n", path); return -1; }
   uint32_t size = (uint32_t)st.size;
   if (size == 0) { logError("[image-loader] empty file: %s\n", path); return -1; }

   VfsFile file;
   if (openFs(path, VFS_O_RDONLY, &file) != 0) { logError("[image-loader] open failed: %s\n", path); return -1; }

   uint8_t *data = (uint8_t *)malloc(size);
   if (!data) { closeFs(&file); logError("[image-loader] out of memory (%u bytes): %s\n", size, path); return -1; }

   uint64_t got = 0;
   while (got < size) {
      int64_t r = readFs(&file, data + got, (uint64_t)size - got);
      if (r <= 0) break;
      got += (uint64_t)r;
   }
   closeFs(&file);
   if (got != size) { free(data); logError("[image-loader] short read %llu/%u: %s\n", (unsigned long long)got, size, path); return -1; }

   int rc = decodeImageMem(data, size, out);
   free(data);
   return rc;
}

// dispatches an in-memory image by sniffing its magic bytes (PNG \x89PNG, JPEG \xFF\xD8).
// returns 0 on success, -1 otherwise. No temp file / no filesystem touch.
static int decodeImageMem(const void *data, uint32_t size, ImageBuffer *out)
{
   const unsigned char *b = (const unsigned char *)data;
   if (size >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G')
      { CellPngDecSrc s; pngSrcMem(&s, data, size); return decodePng(&s, out); }
   if (size >= 2 && b[0] == 0xFF && b[1] == 0xD8)
      return decodeJpeg(data, size, out);

   logError("[image-loader] unrecognised image header in memory buffer (%u bytes)\n", size);
   return -1;
}

// ---------------------------------------------------------------------------
// VRAM upload (main thread only)
// ---------------------------------------------------------------------------

GfxTexture uploadImageBuffer(const ImageBuffer *buf)
{
   GfxTexture zero = { 0, 0, 0, 0 };
   if (!buf->pixels) return zero;

   // never hand the RSX a texture beyond its limit (decode should already cap,
   // this is the last line of defence against a GPU-hanging upload).
   if (buf->w <= 0 || buf->h <= 0 || buf->w > MAX_TEX_DIM || buf->h > MAX_TEX_DIM) {
      logError("[image-loader] refusing oversized texture %dx%d\n", buf->w, buf->h);
      return zero;
   }

   uint32_t offset = uploadGfxTexture(buf->pixels, buf->w, buf->h, buf->pitch);
   if (offset == 0) return zero;

   GfxTexture t;
   t.offset = offset;
   t.w = buf->w;
   t.h = buf->h;
   t.pitch = (int)(((uint32_t)(buf->w * 4) + 63) & ~63u);  // matches uploadGfxTexture
   return t;
}

void freeImageBuffer(ImageBuffer *buf)
{
   if (buf->pixels) { free(buf->pixels); buf->pixels = NULL; }
}

// Synchronous load of a PNG or JPEG file into a VRAM texture (main thread).
// Dispatches by extension via the shared decoder (replaces the old PNG-only loader).
GfxTexture loadGfxTexture(const char *path)
{
   ImageBuffer buf;
   GfxTexture zero = { 0, 0, 0, 0 };
   if (decodeImageToBuffer(path, &buf) != 0) return zero;
   GfxTexture t = uploadImageBuffer(&buf);
   freeImageBuffer(&buf);
   return t;
}

// Synchronous load of an in-memory PNG/JPEG (e.g. an .rpk entry) into a VRAM texture.
// Format is sniffed from the magic bytes -- no temp file, no filesystem touch.
GfxTexture loadGfxTextureMem(const void *data, uint32_t size)
{
   ImageBuffer buf;
   GfxTexture zero = { 0, 0, 0, 0 };
   if (decodeImageMem(data, size, &buf) != 0) return zero;
   GfxTexture t = uploadImageBuffer(&buf);
   freeImageBuffer(&buf);
   return t;
}

// ---------------------------------------------------------------------------
// background worker
// ---------------------------------------------------------------------------
// A single persistent worker decodes one request at a time. The main thread
// publishes a request (path + generation); the worker decodes and publishes the
// result under that generation. A new request bumps the generation, so a result
// for a superseded request is dropped. A blocking codec call can't be aborted
// mid-decode, so a cancel simply discards the finished result and the worker
// moves on to the newest request. All shared fields are guarded by imgLock.

static sys_lwmutex_t imgLock;
static int           imgInit;          // lock created + worker started
static volatile int  workerStarted;

static char     reqPath[MAX_PATH_LEN];
static uint8_t *reqData;               // owned copy for an in-memory decode request not yet taken by worker
static uint32_t reqDataSize;
static int      reqFromMemory;
static uint32_t reqGen;                // newest request id
static int      reqPending;            // a new request awaits the worker

static ImageBuffer resBuf;             // decoded pixels for resGen
static uint32_t    resGen;             // generation resBuf/resState belong to
static int         resState;           // 0 none, 1 ok, -1 failed

static void imgWorker(uint64_t arg)
{
   (void)arg;
   for (;;) {
      char     path[MAX_PATH_LEN];
      uint8_t *data = NULL;
      uint32_t dataSize = 0, gen = 0;
      int      fromMemory = 0, have = 0;

      lock(&imgLock);
      if (reqPending) {
         strCopy(path, sizeof path, reqPath);
         gen = reqGen;
         fromMemory = reqFromMemory;
         data = reqData;
         dataSize = reqDataSize;
         reqData = NULL;
         reqDataSize = 0;
         reqPending = 0;
         have = 1;
      }
      unlock(&imgLock);

      if (!have) { sleepMs(8); continue; }

      ImageBuffer b = { 0, 0, 0, 0 };
      int ok;
      if (fromMemory) {
         logInfo("[image-loader] memory decode start (%u bytes)\n", dataSize);
         int rc = data ? decodeImageMem(data, dataSize, &b) : -1;
         ok = rc == 0;
         logInfo("[image-loader] memory decode returned rc=%d size=%dx%d\n", rc, b.w, b.h);
         free(data);
      } else {
         ok = decodeImageToBuffer(path, &b) == 0;
      }

      lock(&imgLock);
      if (gen == reqGen) {
         // still the request the main thread is waiting on
         if (resState == 1) freeImageBuffer(&resBuf);  // drop an unconsumed prior result
         resBuf   = b;
         resGen   = gen;
         resState = ok ? 1 : -1;
      } else {
         // superseded while decoding: throw it away
         if (ok) freeImageBuffer(&b);
      }
      unlock(&imgLock);
   }
}

static void ensureWorker(void)
{
   if (!imgInit) {
      createLock(&imgLock);
      imgInit = 1;
   }
   if (!workerStarted) {
      sys_ppu_thread_t tid;
      if (spawnThread(&tid, imgWorker, 0, THREAD_PRIORITY_LOW, 128 * 1024, "img-decode") == 0)
         workerStarted = 1;
   }
}

static void supersedeImageRequest(void)
{
   reqGen++;
   reqPending = 0;
   if (reqData) { free(reqData); reqData = NULL; }
   reqDataSize = 0;
   reqFromMemory = 0;
   if (resState == 1) freeImageBuffer(&resBuf);
   resState = 0;
}

void requestImageAsync(const char *path)
{
   ensureWorker();
   if (!workerStarted) return;

   lock(&imgLock);
   supersedeImageRequest();
   strCopy(reqPath, sizeof reqPath, path);
   reqPending = 1;
   unlock(&imgLock);
}

int requestImageMemAsync(const void *data, uint32_t size)
{
   if (!data || size == 0) return -1;
   uint8_t *copy = (uint8_t *)malloc(size);
   if (!copy) return -1;
   memcpy(copy, data, size);

   ensureWorker();
   if (!workerStarted) { free(copy); return -1; }

   lock(&imgLock);
   supersedeImageRequest();
   reqPath[0] = '\0';
   reqData = copy;
   reqDataSize = size;
   reqFromMemory = 1;
   reqPending = 1;
   unlock(&imgLock);
   return 0;
}

void cancelImageAsync(void)
{
   if (!imgInit) return;
   lock(&imgLock);
   supersedeImageRequest();
   unlock(&imgLock);
}

int pollImageAsync(ImageBuffer *out)
{
   int r = 0;
   if (!imgInit) return 0;  // nothing has been requested yet
   lock(&imgLock);
   if (resState != 0 && resGen == reqGen) {
      if (resState == 1) {
         *out = resBuf;
         resBuf.pixels = NULL;
         r = 1;
      } else {
         r = -1;
      }
      resState = 0;
   }
   unlock(&imgLock);
   return r;
}
