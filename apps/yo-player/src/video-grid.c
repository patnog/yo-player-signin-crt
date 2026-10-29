// video-grid - the reusable windowed video-result grid (see video-grid.h).
//
// A background worker fetches result pages (via the screen-supplied GridFetchFn) and thumbnail jpegs; the
// jpegs are kept and only the on-screen band of tiles is "materialised" (thumbnail decoded to a texture +
// labels rasterised), freed again on scroll-away and re-decoded from the kept jpeg on the way back, so VRAM
// stays flat no matter how many results are loaded. The worker is stopped before playback so no per-call
// http fetch overlaps the streaming http client.

#include "video-grid.h"

#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "http.h"
#include "image-loader.h"
#include "dbg.h"
#include "string-utilities.h"   // strCopy
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define GRID_GAP    22
#define TILE_TEXT_H 54          // space under each thumbnail for the title + meta line
#define ROW_TITLE   21
#define META_SIZE   15
#define DUR_SIZE    19
#define WATCHLATER_BADGE_SIZE DUR_SIZE
#define MARGIN_ROWS 2           // rows kept materialised above and below the visible band
#define THUMB_LOOKAHEAD_ROWS 0  // fetch only visible thumbnails on PS3; extend as the user scrolls
#define DECODES_PER_FRAME 1
#define THUMB_CAP   (96 * 1024) // one mqdefault jpeg fits easily
#define THUMB_UA    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36"

#define WATCHED_ALPHA 70        // label alpha for already-watched tiles

// ---- thumbnail fetch (worker side) ----

static void thumbFetchThread(uint64_t arg)
{
   VideoGrid *grid = (VideoGrid *)(uintptr_t)arg;
   uint8_t *buffer = (uint8_t *)malloc(THUMB_CAP);
   if (buffer) {
      HttpHeader headers[] = { { "User-Agent", THUMB_UA }, { "Accept-Encoding", "identity" } };
      for (;;) {
         int i = __sync_fetch_and_add(&grid->thumbNext, 1);
         if (i >= grid->thumbEnd || grid->stopWorker) break;

         char url[128];
         snprintf(url, sizeof url, "https://i.ytimg.com/vi/%s/mqdefault.jpg", grid->results.items[i].videoId);
         int length = 0, status = 0;
         int rc = fetchHttp("GET", url, headers, 2, NULL, 0, (char *)buffer, THUMB_CAP, &length, &status);
         if (rc == 0 && status == 200 && length > 0) {
            uint8_t *copy = (uint8_t *)malloc(length);
            if (copy) {
               memcpy(copy, buffer, length);
               grid->thumbs[i].jpeg = copy; grid->thumbs[i].jpegLen = length;
               __sync_synchronize();
               grid->thumbs[i].state = THUMB_FETCHED;
               logInfo("[grid] thumbnail %s fetched (%d bytes)\n", grid->results.items[i].videoId, length);
            } else {
               grid->thumbs[i].state = THUMB_FAILED;
               logWarn("[grid] thumbnail %s skipped (out of memory)\n", grid->results.items[i].videoId);
            }
         } else {
            grid->thumbs[i].state = THUMB_FAILED;
            logWarn("[grid] thumbnail %s failed rc=%d status=%d bytes=%d\n", grid->results.items[i].videoId, rc, status, length);
         }
      }
      free(buffer);
   }
   exitThread();
}

// fetch thumbnails for results [from, to) in parallel; returns once every one has landed or failed.
static void fetchThumbnailsRange(VideoGrid *grid, int from, int to)
{
   if (to <= from) return;
   logInfo("[grid] fetching %d thumbnails from %d\n", to - from, from);
   grid->thumbNext = from;
   grid->thumbEnd = to;
   sys_ppu_thread_t threads[GRID_THREADS];
   int spawned = 0;
   for (int t = 0; t < GRID_THREADS; t++)
      if (spawnJoinableThread(&threads[spawned], thumbFetchThread, (uint64_t)(uintptr_t)grid,
                              THREAD_PRIORITY_LOW, 128 * 1024, "yt-thumb") == 0)
         spawned++;
   for (int t = 0; t < spawned; t++) joinThread(threads[t]);
   logInfo("[grid] thumbnail batch finished\n");
}

// append a fetched page's items, skipping videoIds we already have. sortTokens (channel chips) are kept
// from the first page - continuation pages don't carry them.
static void appendPage(VideoGrid *grid, const SearchResults *page)
{
   for (int i = 0; i < page->count && grid->results.count < MAX_SEARCH_RESULTS; i++) {
      const SearchResult *item = &page->items[i];
      int duplicate = 0;
      for (int j = 0; j < grid->results.count; j++)
         if (strcmp(grid->results.items[j].videoId, item->videoId) == 0) { duplicate = 1; break; }
      if (duplicate) continue;
      grid->results.items[grid->results.count] = *item;
      __sync_synchronize();
      grid->results.count++;
   }
   strCopy(grid->results.continuation, sizeof grid->results.continuation, page->continuation);
}

// how far down to fetch thumbnails: the visible band plus a lookahead, clamped to what's loaded. thumbnails
// are fetched lazily to this frontier instead of all at once, so returning to a category is cheap.
static int thumbFrontier(const VideoGrid *grid)
{
   int frontier = (grid->scrollRow + grid->visibleRows + THUMB_LOOKAHEAD_ROWS) * grid->columns;
   if (frontier > grid->results.count) frontier = grid->results.count;
   return frontier;
}

static void worker(uint64_t arg)
{
   VideoGrid *grid = (VideoGrid *)(uintptr_t)arg;
   switch (grid->job) {
   case JOB_LOAD: {
      int rc = grid->fetch(NULL, &grid->results, grid->fetchUser);
      grid->stage = (rc == 0 && grid->results.count > 0) ? GRID_READY : GRID_EMPTY;
      __sync_synchronize();
      grid->searchParsed = 1;
      if (grid->stage == GRID_READY) { int target = thumbFrontier(grid); fetchThumbnailsRange(grid, 0, target); grid->thumbFetched = target; }
      break;
   }
   case JOB_CACHE: {   // results already restored; fetch only the first window of thumbnails
      int target = thumbFrontier(grid);
      fetchThumbnailsRange(grid, 0, target);
      grid->thumbFetched = target;
      break;
   }
   case JOB_THUMBS: {  // extend thumbnails to the frontier the window has scrolled to
      int target = thumbFrontier(grid);
      fetchThumbnailsRange(grid, grid->thumbFetched, target);
      grid->thumbFetched = target;
      break;
   }
   case JOB_MORE: {    // append the next page; its thumbnails are picked up lazily by a later JOB_THUMBS
      SearchResults *page = (SearchResults *)malloc(sizeof *page);
      if (page && grid->fetch(grid->results.continuation, page, grid->fetchUser) == 0) {
         int base = grid->results.count;
         appendPage(grid, page);
         if (grid->results.count == base) grid->results.continuation[0] = 0;   // nothing new: stop paging
      }
      free(page);
      break;
   }
   }
   __sync_synchronize();
   grid->workerDone = 1;
   exitThread();
}

static int spawnWorker(VideoGrid *grid)
{
   grid->stopWorker = 0;
   grid->workerDone = 0;
   grid->threadActive = (spawnJoinableThread(&grid->workerTid, worker, (uint64_t)(uintptr_t)grid,
                         THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-feed") == 0);
   return grid->threadActive;
}

// ---- tile windowing (UI side) ----

static void cancelGridDecode(VideoGrid *grid)
{
   if (grid->decodeIndex < 0) return;
   int i = grid->decodeIndex;
   cancelImageAsync();
   if (i < grid->results.count && grid->thumbs[i].state == THUMB_DECODING)
      grid->thumbs[i].state = THUMB_FETCHED;
   grid->decodeIndex = -1;
}

static void freeThumbnails(VideoGrid *grid)
{
   cancelGridDecode(grid);
   finishGfx();
   for (int i = 0; i < MAX_SEARCH_RESULTS; i++) {
      if (grid->thumbs[i].jpeg) { free(grid->thumbs[i].jpeg); grid->thumbs[i].jpeg = NULL; }
      freeGfxTexture(&grid->thumbs[i].tex);
   }
   memset(grid->thumbs, 0, sizeof grid->thumbs);
}

static void resetFeed(VideoGrid *grid)
{
   freeThumbnails(grid);
   for (int i = 0; i < MAX_SEARCH_RESULTS; i++) { freeLabel(&grid->rows[i]); freeLabel(&grid->metas[i]); freeLabel(&grid->durations[i]); }
   memset(&grid->results, 0, sizeof grid->results);
   grid->stage = GRID_RUNNING;
   grid->searchParsed = grid->workerDone = 0;
   grid->thumbFetched = 0;
   grid->decodeIndex = -1;
   grid->selected = grid->scrollRow = grid->winStart = grid->winEnd = 0;
}

static void beginFeed(VideoGrid *grid)
{
   if (grid->job == JOB_CACHE) {   // results already in hand: show immediately, worker only fetches thumbnails
      grid->stage = GRID_READY;
      __sync_synchronize();
      grid->searchParsed = 1;
   }
   if (!spawnWorker(grid)) {
      grid->workerDone = 1;
      if (grid->job != JOB_CACHE) { grid->stage = GRID_EMPTY; grid->searchParsed = 1; }
   }
}

// (re)load the current source: restore pendingCached results instantly if set (worker then only refetches
// thumbnails), else fetch page 1 fresh. run either immediately (idle) or deferred after the worker is reaped.
static void applyDeferredSource(VideoGrid *grid)
{
   resetFeed(grid);
   if (grid->pendingCached) { grid->results = *grid->pendingCached; grid->job = JOB_CACHE; }
   else                       grid->job = JOB_LOAD;
   beginFeed(grid);
}

static void loadMore(VideoGrid *grid)
{
   grid->job = JOB_MORE;
   if (!spawnWorker(grid)) grid->workerDone = 1;
}

// fetch the next band of thumbnails the window has scrolled toward (lazy, non-blocking)
static void loadThumbnails(VideoGrid *grid)
{
   grid->job = JOB_THUMBS;
   if (!spawnWorker(grid)) grid->workerDone = 1;
}

// join the non-empty metadata parts with " - " into "author - views - age".
static void composeMeta(char *out, int cap, const SearchResult *item)
{
   const char *parts[3] = { item->author, item->views, item->published };
   int length = 0;
   out[0] = 0;
   for (int i = 0; i < 3; i++) {
      if (!parts[i][0]) continue;
      length += snprintf(out + length, cap - length, "%s%s", length ? " \xe2\x80\xa2 " : "", parts[i]);
      if (length >= cap) break;
   }
}

static void materializeTile(VideoGrid *grid, int i)
{
   char meta[128];
   const SearchResult *item = &grid->results.items[i];
   setLabelText(&grid->rows[i], item->title);
   composeMeta(meta, sizeof meta, item);
   setLabelText(&grid->metas[i], meta);
   setLabelText(&grid->durations[i], item->isLive ? "LIVE" : item->duration);
}

static void dematerializeTile(VideoGrid *grid, int i)
{
   freeLabel(&grid->rows[i]);
   freeLabel(&grid->metas[i]);
   freeLabel(&grid->durations[i]);
   if (grid->thumbs[i].tex.offset) { freeGfxTexture(&grid->thumbs[i].tex); memset(&grid->thumbs[i].tex, 0, sizeof grid->thumbs[i].tex); }
}

// keep exactly the visible band (+ margin) materialised. safe without an RSX flush: a tile leaving the
// MARGIN_ROWS-wide window wasn't drawn last frame (the grid draws only the inner visible rows).
static void reconcileWindow(VideoGrid *grid)
{
   int start = (grid->scrollRow - MARGIN_ROWS) * grid->columns;
   int end   = (grid->scrollRow + grid->visibleRows + MARGIN_ROWS) * grid->columns;
   if (start < 0) start = 0;
   if (end > grid->results.count) end = grid->results.count;
   if (start == grid->winStart && end == grid->winEnd) return;

   int lo = start < grid->winStart ? start : grid->winStart;
   int hi = end   > grid->winEnd   ? end   : grid->winEnd;
   for (int i = lo; i < hi; i++) {
      int wasIn = (i >= grid->winStart && i < grid->winEnd);
      int nowIn = (i >= start && i < end);
      if      (nowIn && !wasIn) materializeTile(grid, i);
      else if (wasIn && !nowIn) dematerializeTile(grid, i);
   }
   grid->winStart = start;
   grid->winEnd   = end;
}

static void decodeWindowThumbnails(VideoGrid *grid)
{
   if (grid->decodeIndex >= 0) {
      ImageBuffer decoded = { 0, 0, 0, 0 };
      int rc = pollImageAsync(&decoded);
      if (rc == 0) return;

      int i = grid->decodeIndex;
      grid->decodeIndex = -1;
      if (i >= 0 && i < grid->results.count && grid->thumbs[i].state == THUMB_DECODING) {
         int row = i / grid->columns;
         int visible = row >= grid->scrollRow && row < grid->scrollRow + grid->visibleRows;
         if (rc > 0 && visible) {
            grid->thumbs[i].tex = uploadImageBuffer(&decoded);
            if (grid->thumbs[i].tex.offset)
               logInfo("[grid] thumbnail %s decoded and uploaded\n", grid->results.items[i].videoId);
            else {
               grid->thumbs[i].state = THUMB_FAILED;
               logWarn("[grid] thumbnail %s texture upload failed\n", grid->results.items[i].videoId);
            }
         } else if (rc > 0) {
            // It scrolled away while decoding. Keep the JPEG and defer upload until visible again.
            grid->thumbs[i].state = THUMB_FETCHED;
         } else {
            grid->thumbs[i].state = THUMB_FAILED;
            logWarn("[grid] thumbnail %s JPEG decode failed\n", grid->results.items[i].videoId);
         }
      }
      if (rc > 0) freeImageBuffer(&decoded);
   }

   for (int i = grid->winStart; i < grid->winEnd; i++) {
      int row = i / grid->columns;
      if (row < grid->scrollRow || row >= grid->scrollRow + grid->visibleRows) continue;
      if (grid->thumbs[i].state != THUMB_FETCHED || !grid->thumbs[i].jpeg || grid->thumbs[i].tex.offset) continue;
      if (requestImageMemAsync(grid->thumbs[i].jpeg, (uint32_t)grid->thumbs[i].jpegLen) == 0) {
         grid->thumbs[i].state = THUMB_DECODING;
         grid->decodeIndex = i;
         logInfo("[grid] thumbnail %s decode queued\n", grid->results.items[i].videoId);
      } else {
         grid->thumbs[i].state = THUMB_FAILED;
         logWarn("[grid] thumbnail %s decode queue failed\n", grid->results.items[i].videoId);
      }
      break;  // one image decoder job at a time
   }
}

// left/right step one tile and wrap onto the previous/next row when they exist; up/down step a whole row.
static void moveSelection(VideoGrid *grid, int delta)
{
   int target = grid->selected + delta;
   if (target >= 0 && target < grid->results.count) grid->selected = target;
}

// ---- public interface ----

void initVideoGrid(VideoGrid *grid, Font *font, int x, int y, int width, int height)
{
   memset(grid, 0, sizeof *grid);
   grid->decodeIndex = -1;
   grid->font = font;
   grid->x = x; grid->y = y; grid->width = width; grid->height = height;
   int sdOutput = isGfxSdVideoOutput();
   grid->columns = sdOutput ? 3 : GRID_COLS_WIDE;
   grid->gap = sdOutput ? 10 : GRID_GAP;
   // Reserve enough text space for the biggest 2.0x preset. This keeps grid
   // coordinates and thumbnail size fixed when L2 changes font size.
   grid->titleSize = sdOutput ? 15 : ROW_TITLE;
   grid->metaSize = sdOutput ? 11 : META_SIZE;
   grid->tileTextH = sdOutput
      ? 6 + grid->titleSize * 2 + 6 + grid->metaSize * 2 + 4
      : TILE_TEXT_H;
   grid->metaOffset = sdOutput ? grid->titleSize * 2 + 6 : ROW_TITLE + 8;

   int maxTileW = (width - (grid->columns - 1) * grid->gap) / grid->columns;
   // In either 4:3 or 16:9 SD output, cap tile width so three complete rows fit.
   if (sdOutput) {
      int maxStep = height / 3;
      int maxThumbH = maxStep - grid->tileTextH - grid->gap;
      int widthFromHeight = maxThumbH > 0 ? maxThumbH * 16 / 9 : 1;
      if (maxTileW > widthFromHeight) maxTileW = widthFromHeight;
   }
   if (maxTileW < 1) maxTileW = 1;
   grid->tileW = maxTileW;
   grid->thumbH = grid->tileW * 9 / 16;
   grid->rowStep = grid->thumbH + grid->tileTextH + grid->gap;
   grid->visibleRows = height / grid->rowStep;
   if (grid->visibleRows < 1) grid->visibleRows = 1;
   int usedWidth = grid->columns * grid->tileW + (grid->columns - 1) * grid->gap;
   grid->x = x + (width - usedWidth) / 2;

   for (int i = 0; i < MAX_SEARCH_RESULTS; i++) {
      initLabel(&grid->rows[i],      font, 0, 0, grid->tileW, AUTO, grid->titleSize, activeTheme->textPrimary,   TEXT_NOWRAP_ELLIPSIS, "");
      initLabel(&grid->metas[i],     font, 0, 0, grid->tileW, AUTO, grid->metaSize, activeTheme->textSecondary, TEXT_NOWRAP_ELLIPSIS, "");
      initLabel(&grid->durations[i], font, 0, 0, AUTO,        AUTO, DUR_SIZE,       activeTheme->textPrimary,   TEXT_NOWRAP,          "");
   }
   initLabel(&grid->watchLaterBadge, font, 0, 0, AUTO, AUTO, WATCHLATER_BADGE_SIZE, activeTheme->textPrimary, TEXT_NOWRAP, "Watch Later");
}

void termVideoGrid(VideoGrid *grid)
{
   stopVideoGrid(grid);
   freeThumbnails(grid);
   for (int i = 0; i < MAX_SEARCH_RESULTS; i++) { freeLabel(&grid->rows[i]); freeLabel(&grid->metas[i]); freeLabel(&grid->durations[i]); }
   freeLabel(&grid->watchLaterBadge);
}

void setGridWatchedPredicate(VideoGrid *grid, int (*isWatched)(const char *videoId)) { grid->isWatched = isWatched; }
void setGridWatchLaterPredicate(VideoGrid *grid, int (*isWatchLater)(const char *videoId)) { grid->isWatchLater = isWatchLater; }

// both source-changes are non-blocking: if a load is in flight, ask it to stop and defer the swap until the
// reap in updateVideoGrid, so mode / category switches never freeze the frame on an in-flight fetch.
static void changeSource(VideoGrid *grid, GridFetchFn fetch, void *user, const SearchResults *cached)
{
   grid->fetch = fetch;
   grid->fetchUser = user;
   grid->pendingCached = cached;
   if (!grid->threadActive) { applyDeferredSource(grid); return; }
   grid->stopWorker = 1;
   grid->reloadPending = 1;
   grid->stage = GRID_RUNNING;
}

void setGridSource(VideoGrid *grid, GridFetchFn fetch, void *user) { changeSource(grid, fetch, user, NULL); }
void setGridCached(VideoGrid *grid, const SearchResults *cached, GridFetchFn fetch, void *user) { changeSource(grid, fetch, user, cached); }

void stopVideoGrid(VideoGrid *grid)
{
   if (grid->threadActive) { grid->stopWorker = 1; joinThread(grid->workerTid); grid->threadActive = 0; }
   grid->reloadPending = 0;
   cancelGridDecode(grid);
}

void updateVideoGrid(VideoGrid *grid)
{
   // reap a finished worker; a deferred source swap then resets and starts the new feed
   if (grid->workerDone && grid->threadActive) {
      joinThread(grid->workerTid); grid->threadActive = 0;
      if (grid->reloadPending) { grid->reloadPending = 0; applyDeferredSource(grid); }
   }
   if (grid->stage != GRID_READY) return;

   reconcileWindow(grid);
   decodeWindowThumbnails(grid);

   if      (isRepeatDue(&grid->horizontalRepeat, getPadButtonState(PAD_BTN_RIGHT))) moveSelection(grid, 1);
   else if (isRepeatDue(&grid->horizontalRepeat, getPadButtonState(PAD_BTN_LEFT)))  moveSelection(grid, -1);
   if      (isRepeatDue(&grid->verticalRepeat,   getPadButtonState(PAD_BTN_DOWN)))  moveSelection(grid, grid->columns);
   else if (isRepeatDue(&grid->verticalRepeat,   getPadButtonState(PAD_BTN_UP)))    moveSelection(grid, -grid->columns);

   int selRow = grid->selected / grid->columns;
   if (selRow < grid->scrollRow)                            grid->scrollRow = selRow;
   else if (selRow >= grid->scrollRow + grid->visibleRows)  grid->scrollRow = selRow - grid->visibleRows + 1;

   // idle background work: fetch thumbnails the window scrolled toward first, then the next page near the bottom
   if (!grid->threadActive && !grid->reloadPending) {
      if (grid->thumbFetched < thumbFrontier(grid)) {
         loadThumbnails(grid);
      } else if (grid->results.continuation[0] && grid->results.count < MAX_SEARCH_RESULTS) {
         int lastRow = (grid->results.count - 1) / grid->columns;
         if (selRow >= lastRow - 1) loadMore(grid);
      }
   }
}

static void drawTile(VideoGrid *grid, int i)
{
   int row = i / grid->columns, col = i % grid->columns;
   int tx = grid->x + col * (grid->tileW + grid->gap);
   int ty = grid->y + (row - grid->scrollRow) * grid->rowStep;
   const SearchResult *item = &grid->results.items[i];
   int watched = grid->isWatched && grid->isWatched(item->videoId);

   if (i == grid->selected) {
      int ring = activeTheme->focusThickness;
      fillGfxRectangle(tx - ring, ty - ring, grid->tileW + 2 * ring, grid->thumbH + 2 * ring, activeTheme->focusBorder);
   }

   if (grid->thumbs[i].tex.offset)
      drawGfxTextureFixedSize(tx, ty, grid->tileW, grid->thumbH, grid->thumbs[i].tex, 0, 0, 1, 1, watched ? activeTheme->watchedThumbTint : 0xFFFFFFFF, GFX_FILTER_LINEAR);
   else
      fillGfxRectangle(tx, ty, grid->tileW, grid->thumbH, activeTheme->surface);

   float uiScale = getGfxUiScaleFactor();
   int padX = (int)(6.0f * uiScale + 0.5f);
   int padY = (int)(4.0f * uiScale + 0.5f);
   int edgeGap = (int)(6.0f * uiScale + 0.5f);
   if (item->duration[0] || item->isLive) {
      int badgeW = grid->durations[i].tt.tex.w + 2 * padX;
      int badgeH = grid->durations[i].tt.tex.h + 2 * padY;
      int badgeX = tx + grid->tileW - badgeW - edgeGap, badgeY = ty + grid->thumbH - badgeH - edgeGap;
      fillGfxRectangle(badgeX, badgeY, badgeW, badgeH, item->isLive ? activeTheme->accent : activeTheme->badgeFill);
      drawLabelAt(&grid->durations[i], badgeX + padX, badgeY + padY);
   }

   if (grid->isWatchLater && grid->isWatchLater(item->videoId)) {
      int badgeW = grid->watchLaterBadge.tt.tex.w + 2 * padX;
      int badgeH = grid->watchLaterBadge.tt.tex.h + 2 * padY;
      int badgeX = tx + grid->tileW - badgeW - edgeGap, badgeY = ty + 6;
      fillGfxRectangle(badgeX, badgeY, badgeW, badgeH, activeTheme->badgeFill);
      drawLabelAt(&grid->watchLaterBadge, badgeX + padX, badgeY + padY);
   }

   int textY = ty + grid->thumbH + 6, alpha = watched ? WATCHED_ALPHA : 255;
   moveLabel(&grid->rows[i],  tx, textY);                    drawLabelAlpha(&grid->rows[i],  alpha);
   moveLabel(&grid->metas[i], tx, textY + grid->metaOffset); drawLabelAlpha(&grid->metas[i], alpha);
}

void drawVideoGrid(VideoGrid *grid)
{
   if (grid->stage != GRID_READY) return;
   for (int i = 0; i < grid->results.count; i++) {
      int row = i / grid->columns;
      if (row >= grid->scrollRow && row < grid->scrollRow + grid->visibleRows) drawTile(grid, i);
   }
}

GridStage            gridStage(const VideoGrid *grid)    { return grid->stage; }
int                  gridBusy(const VideoGrid *grid)     { return grid->threadActive && grid->job == JOB_MORE; }
int                  gridStopRequested(const VideoGrid *grid) { return grid->stopWorker; }
const SearchResults *gridResults(const VideoGrid *grid)  { return &grid->results; }
const SearchResult  *gridSelected(const VideoGrid *grid)
{
   return grid->results.count ? &grid->results.items[grid->selected] : NULL;
}
