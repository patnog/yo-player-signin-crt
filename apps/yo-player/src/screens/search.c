// search screen - a VideoGrid in one of two modes: a text search or a channel's videos (see search.h).
// Pushed on top of home. Triangle drills into a channel in place, Circle backs out (channel -> search
// results -> pop to home), SELECT cycles the sort, Start re-searches, X plays.

#include "screens/search.h"
#include "screens/play.h"
#include "video-grid.h"
#include "storage.h"
#include "downloads.h"
#include "ui/button-hints.h"
#include "ui/console-glyphs.h"

#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "font.h"
#include "ui/label.h"
#include "screen-manager.h"
#include "osk-input.h"
#include "string-utilities.h"
#include "dbg.h"
#include <string.h>
#include <stdio.h>

#define MARGIN_X      80
#define HEADER_Y      54
#define TITLE_SIZE    30
#define GRID_TOP      100
#define HINTS_BOTTOM  88
#define HINT_GLYPH_H  28
#define HINT_TEXT     22
#define SD_HINTS_BOTTOM 100
#define SD_HINT_ROW_GAP 8
#define SD_MAX_SCALE 1.5f
#define SD_SCALE_PATH "/dev_hdd0/tmp/yo-player/sd-ui-scale.txt"
#define SD_SCALE_COUNT 2
static const float SdScaleChoices[SD_SCALE_COUNT] = { 1.00f, 1.50f };
#define TRIANGLE_HINT 3         // Triangle's slot in the hint row (see initSearch): retitled per mode

static const char *SortNames[SORT_COUNT]                = { "Relevance", "Views" };
static const char *ChannelSortNames[CHANNEL_SORT_COUNT] = { "Latest", "Popular", "Oldest" };

// how the next push opens (set by openSearchQuery / openChannelFor before pushScreen).
static char         pendingQuery[128];
static SearchResult pendingChannel;
static int          pendingIsChannel;

static struct {
   VideoGrid grid;
   char      query[128];
   char      channelId[32];
   char      channelName[48];
   char      channelSortToken[MAX_SORT_TOKEN];   // first-page token for the chosen channel sort ("" = Latest)
   int       channelSort;
   int       sort;
   int       screenW, screenH;
} search;

static Font        uiFont;
static Label       titleLabel, sortLabel, statusLabel;
static ButtonHints hints, hintsSecondRow;
static int sdHintsTwoRows;
static int scaleHint = -1;

static int searchFetch(const char *token, SearchResults *out, void *user)
{
   int rc;
   (void)user;
   logInfo("[diag-search] fetch begin mode=%s queryLen=%d channelIdLen=%d token=%s\n",
      search.channelId[0] ? "channel" : "query", (int)strlen(search.query),
      (int)strlen(search.channelId), token ? "yes" : "no");
   if (search.channelId[0]) {
      const char *first = search.channelSortToken[0] ? search.channelSortToken : NULL;
      rc = getChannelVideos(search.channelId, token ? token : first, out);
   } else {
      rc = searchVideos(search.query, search.sort, token, out);
   }
   logInfo("[diag-search] fetch returned rc=%d count=%d continuation=%s\n",
      rc, out ? out->count : -1, out && out->continuation[0] ? "yes" : "no");
   return rc;
}

// in channel mode Triangle toggles the subscription (you're already in the channel); elsewhere it drills in.
static void refreshScaleHint(void)
{
   if (!sdHintsTwoRows || scaleHint < 0) return;
   char caption[32];
   snprintf(caption, sizeof caption, "Scale %.1fx", (double)getGfxUiScaleFactor());
   setButtonHintCaption(&hintsSecondRow, scaleHint, caption);
}

static void cycleSdScale(void)
{
   float current = getGfxUiScaleFactor();
   int index = 0;
   for (int i = 0; i < SD_SCALE_COUNT; i++) {
      float diff = current - SdScaleChoices[i];
      if (diff > -0.001f && diff < 0.001f) { index = i; break; }
   }
   index = (index + 1) % SD_SCALE_COUNT;
   setGfxSdUiScale(SdScaleChoices[index]);
   char buf[16];
   int n = snprintf(buf, sizeof buf, "%.2f\n", (double)SdScaleChoices[index]);
   if (n > 0) writeFile(SD_SCALE_PATH, buf, (uint64_t)n);
   refreshScaleHint();
}

static void updateTriangleHint(void)
{
   const char *caption = "Channel";
   if (search.channelId[0]) caption = isSubscribed(search.channelId) ? "Unsubscribe" : "Subscribe";
   setButtonHintCaption(&hints, TRIANGLE_HINT, caption);
}

// title (left) shows the mode; sortLabel (right) shows the current ordering.
static void setHeading(void)
{
   char heading[192];
   if (search.channelId[0]) { snprintf(heading, sizeof heading, "%s", search.channelName); setLabelText(&sortLabel, ChannelSortNames[search.channelSort]); }
   else                     { snprintf(heading, sizeof heading, "Search: %s", search.query); setLabelText(&sortLabel, SortNames[search.sort]); }
   setLabelText(&titleLabel, heading);
   updateTriangleHint();
}

static void reload(void)
{
   logInfo("[diag-search] reload begin mode=%s queryLen=%d channelIdLen=%d gridThreadActive=%d\n",
      search.channelId[0] ? "channel" : "query", (int)strlen(search.query),
      (int)strlen(search.channelId), search.grid.threadActive);
   setHeading();
   logInfo("[diag-search] reload heading set; setGridSource begin\n");
   setGridSource(&search.grid, searchFetch, NULL);
   logInfo("[diag-search] reload setGridSource returned stage=%d threadActive=%d\n",
      gridStage(&search.grid), search.grid.threadActive);
}

static void enterChannel(const SearchResult *item)
{
   logInfo("[diag-triangle] search enterChannel videoId=%s channelIdLen=%d authorLen=%d\n",
      item ? item->videoId : "NULL", item ? (int)strlen(item->channelId) : 0,
      item ? (int)strlen(item->author) : 0);
   if (!item || !item->channelId[0]) { logInfo("[diag-triangle] search enterChannel skipped: missing channelId\n"); return; }
   strCopy(search.channelId, sizeof search.channelId, item->channelId);
   strCopy(search.channelName, sizeof search.channelName, item->author[0] ? item->author : "Channel");
   search.channelSort = CHANNEL_SORT_LATEST;
   search.channelSortToken[0] = 0;
   reload();
}

static void cycleChannelSort(void)
{
   const SearchResults *results = gridResults(&search.grid);
   if (!results->sortTokens[CHANNEL_SORT_LATEST][0]) return;   // chips not loaded yet
   search.channelSort = (search.channelSort + 1) % CHANNEL_SORT_COUNT;
   strCopy(search.channelSortToken, sizeof search.channelSortToken, results->sortTokens[search.channelSort]);
   reload();
}

static void onQueryEntered(const char *text)
{
   logInfo("[diag-search] search callback entered text=%s len=%d\n", text ? "present" : "NULL", text ? (int)strlen(text) : 0);
   if (!text || !text[0]) { logInfo("[diag-search] search callback empty; return\n"); return; }
   if (strstr(text, "youtu")) { logInfo("[diag-search] search URL path: stop grid\n"); stopVideoGrid(&search.grid); logInfo("[diag-search] search URL path: playVideo\n"); playVideo(text); return; }
   search.channelId[0] = 0;   // a search exits channel mode
   strCopy(search.query, sizeof search.query, text);
   search.sort = SORT_RELEVANCE;
   logInfo("[diag-search] search query stored; reload begin\n");
   reload();
   logInfo("[diag-search] search callback returned\n");
}

static void initSearch(void)
{
   logInfo("[diag-search] initSearch entered pendingIsChannel=%d pendingQueryLen=%d pendingChannelIdLen=%d\n",
      pendingIsChannel, (int)strlen(pendingQuery), (int)strlen(pendingChannel.channelId));
   search.screenW = getGfxScreenWidth();
   search.screenH = getGfxScreenHeight();

   memset(search.channelId, 0, sizeof search.channelId);
   search.query[0] = 0;
   search.sort = SORT_RELEVANCE;
   if (pendingIsChannel) {
      strCopy(search.channelId, sizeof search.channelId, pendingChannel.channelId);
      strCopy(search.channelName, sizeof search.channelName, pendingChannel.author[0] ? pendingChannel.author : "Channel");
      search.channelSort = CHANNEL_SORT_LATEST;
      search.channelSortToken[0] = 0;
   } else {
      strCopy(search.query, sizeof search.query, pendingQuery);
   }

   logInfo("[diag-search] initSearch opening UI font\n");
   uiFont = openSystemFont(FONT_POP);
   logInfo("[diag-search] initSearch UI font opened\n");
   initLabel(&titleLabel,  &uiFont, MARGIN_X, HEADER_Y, search.screenW - 300, AUTO, TITLE_SIZE, activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
   setLabelScaleWithUi(&titleLabel, 1);
   initLabel(&sortLabel,   &uiFont, 0, HEADER_Y, AUTO, AUTO, TITLE_SIZE, activeTheme->textSecondary, TEXT_NOWRAP, "");
   initLabel(&statusLabel, &uiFont, MARGIN_X, GRID_TOP, search.screenW - 2 * MARGIN_X, AUTO, 21, activeTheme->textSecondary, TEXT_NOWRAP, "Loading...");

   sdHintsTwoRows = isGfxSdVideoOutput();
   if (sdHintsTwoRows) {
      int maxGlyphHeight = (int)(HINT_GLYPH_H * SD_MAX_SCALE + 0.5f);
      initButtonHints(&hints, &uiFont,
                      search.screenH - SD_HINTS_BOTTOM - maxGlyphHeight - SD_HINT_ROW_GAP,
                      HINT_GLYPH_H, HINT_TEXT, activeTheme->textSecondary);
      addButtonHint(&hints, getConsoleGlyph(GLYPH_CROSS),    "Play");
      addButtonHint(&hints, getConsoleGlyph(GLYPH_CIRCLE),   "Back");
      addButtonHint(&hints, getConsoleGlyph(GLYPH_SQUARE),   "Watch Later");
      addButtonHint(&hints, getConsoleGlyph(GLYPH_TRIANGLE), "Channel");
      setButtonHintsStableLayout(&hints, 1);

      initButtonHints(&hintsSecondRow, &uiFont, search.screenH - SD_HINTS_BOTTOM,
                      HINT_GLYPH_H, HINT_TEXT, activeTheme->textSecondary);
      addButtonHint(&hintsSecondRow, getConsoleGlyph(GLYPH_SELECT), "Sort");
      addButtonHint(&hintsSecondRow, getConsoleGlyph(GLYPH_R3),     "Download");
      addButtonHint(&hintsSecondRow, getConsoleGlyph(GLYPH_START),  "Search");
      scaleHint = addButtonHint(&hintsSecondRow, getConsoleGlyph(GLYPH_L2), "Scale 1.5x");
      setButtonHintsStableLayout(&hintsSecondRow, 1);
      refreshScaleHint();
   } else {
      initButtonHints(&hints, &uiFont, search.screenH - HINTS_BOTTOM, HINT_GLYPH_H, HINT_TEXT, activeTheme->textSecondary);
      addButtonHint(&hints, getConsoleGlyph(GLYPH_CROSS),    "Play");
      addButtonHint(&hints, getConsoleGlyph(GLYPH_CIRCLE),   "Back");
      addButtonHint(&hints, getConsoleGlyph(GLYPH_SQUARE),   "Watch Later");
      addButtonHint(&hints, getConsoleGlyph(GLYPH_TRIANGLE), "Channel");
      addButtonHint(&hints, getConsoleGlyph(GLYPH_SELECT),   "Sort");
      addButtonHint(&hints, getConsoleGlyph(GLYPH_R3),       "Download");
      addButtonHint(&hints, getConsoleGlyph(GLYPH_START),    "Search");
   }

   logInfo("[diag-search] initSearch initVideoGrid begin dimensions=%dx%d\n",
      search.screenW - 2 * MARGIN_X, search.screenH - GRID_TOP - HINTS_BOTTOM - HINT_GLYPH_H - 24);
   initVideoGrid(&search.grid, &uiFont, MARGIN_X, GRID_TOP, search.screenW - 2 * MARGIN_X, search.screenH - GRID_TOP - HINTS_BOTTOM - HINT_GLYPH_H - 24);
   logInfo("[diag-search] initSearch initVideoGrid returned\n");
   setGridWatchedPredicate(&search.grid, isWatched);
   setGridWatchLaterPredicate(&search.grid, isWatchLater);
   reload();
}

static void updateSearch(void)
{
   updateDownloadOverlay();
   if (oskInputActive()) return;

   if (search.channelId[0] && isPadButtonPressed(PAD_BTN_L2) && isGfxSdVideoOutput()) {
      cycleSdScale();
      return;
   }

   if (isPadButtonPressed(PAD_BTN_START)) { oskInputBegin("Search YouTube", search.query[0] ? search.query : "", onQueryEntered); }

   // Circle: channel-with-underlying-search -> back to results; otherwise pop to home
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) {
      if (search.channelId[0] && search.query[0]) { search.channelId[0] = 0; reload(); }
      else popScreen();
      return;
   }
   if (isPadButtonPressed(PAD_BTN_SELECT)) {
      if (search.channelId[0]) cycleChannelSort();
      else { search.sort = (search.sort + 1) % SORT_COUNT; reload(); }
      return;
   }

   updateVideoGrid(&search.grid);
   if (gridStage(&search.grid) != GRID_READY) return;

   const SearchResult *selected = gridSelected(&search.grid);
   if (!selected) return;
   if (isPadButtonPressed(PAD_BTN_SQUARE)) { toggleWatchLater(selected); return; }
   if (isPadButtonPressed(PAD_BTN_R3)) { if (!selected->isLive) enqueueDownload(selected); return; }   // live can't be downloaded
   if (isPadButtonPressed(PAD_BTN_TRIANGLE)) {
      logInfo("[diag-triangle] search page mode=%s selectedVideoId=%s selectedChannelIdLen=%d currentChannelIdLen=%d\n",
         search.channelId[0] ? "channel-listing" : "results", selected->videoId,
         (int)strlen(selected->channelId), (int)strlen(search.channelId));
      if (search.channelId[0]) {
         int wasSubscribed = isSubscribed(search.channelId);
         logInfo("[diag-triangle] channel listing subscription toggle begin currentlySubscribed=%d\n", wasSubscribed);
         setSubscribed(search.channelId, !wasSubscribed);
         updateTriangleHint();
         logInfo("[diag-triangle] channel listing subscription toggle returned\n");
      } else {
         enterChannel(selected);
         logInfo("[diag-triangle] search enterChannel returned\n");
      }
      return;
   }
   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      markWatched(selected->videoId);
      stopVideoGrid(&search.grid);
      playVideo(selected->videoId);
   }
}

static void drawSearch(void)
{
   refreshLabelScale(&titleLabel);
   fillGfxRectangle(0, 0, search.screenW, search.screenH, activeTheme->appBg);
   drawLabel(&titleLabel);
   moveLabel(&sortLabel, search.screenW - MARGIN_X - sortLabel.tt.tex.w, HEADER_Y);   // right-aligned
   drawLabel(&sortLabel);

   if (gridStage(&search.grid) == GRID_READY) {
      drawVideoGrid(&search.grid);
   } else {
      setLabelText(&statusLabel, gridStage(&search.grid) == GRID_EMPTY ? "No results" : "Loading...");
      drawLabel(&statusLabel);
   }

   drawButtonHints(&hints, search.screenW);
   if (sdHintsTwoRows) drawButtonHints(&hintsSecondRow, search.screenW);
   drawDownloadOverlay(search.screenW);
}

static void termSearch(void)
{
   termVideoGrid(&search.grid);
   termButtonHints(&hints);
   termButtonHints(&hintsSecondRow);
   freeLabel(&titleLabel);
   freeLabel(&sortLabel);
   freeLabel(&statusLabel);
   closeFont(&uiFont);
}

void openSearchQuery(const char *query)
{
   logInfo("[diag-search] openSearchQuery begin queryLen=%d\n", query ? (int)strlen(query) : 0);
   strCopy(pendingQuery, sizeof pendingQuery, query);
   pendingIsChannel = 0;
   logInfo("[diag-search] openSearchQuery pushScreen begin\n");
   pushScreen(&searchScreen);
   logInfo("[diag-search] openSearchQuery pushScreen returned\n");
}

void openChannelFor(const SearchResult *item)
{
   logInfo("[diag-triangle] openChannelFor begin item=%s channelIdLen=%d authorLen=%d\n",
      item ? item->videoId : "NULL", item ? (int)strlen(item->channelId) : 0,
      item ? (int)strlen(item->author) : 0);
   if (!item || !item->channelId[0]) { logInfo("[diag-triangle] openChannelFor rejected missing channelId\n"); return; }
   pendingChannel = *item;
   pendingIsChannel = 1;
   logInfo("[diag-triangle] openChannelFor pushScreen begin\n");
   pushScreen(&searchScreen);
   logInfo("[diag-triangle] openChannelFor pushScreen returned\n");
}

Screen searchScreen = { initSearch, NULL, updateSearch, drawSearch, NULL, termSearch, SCREEN_TERMINATED };
