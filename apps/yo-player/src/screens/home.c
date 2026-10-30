// home - the landing screen: trending categories in a VideoGrid (see home.h).

#include "screens/home.h"
#include "screens/search.h"
#include "screens/play.h"
#include "video-grid.h"
#include "storage.h"
#include "extractor.h"
#include "vfs.h"
#include "account.h"
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
#include "string-utilities.h"   // strCopy
#include "dbg.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define MARGIN_X      80
#define HEADER_Y      54
#define TITLE_SIZE    30
#define GRID_TOP      100
#define HINTS_BOTTOM     88       // standard single-row hint top offset from the screen bottom
#define SD_HINTS_BOTTOM 100       // lower hint-row top offset; keeps 2.0x glyphs inside TV safe area
#define SD_HINT_ROW_GAP   8
#define GRID_BOTTOM_GAP  24
#define SD_GRID_BOTTOM_GAP 20
#define SD_MAX_SCALE    1.5f
#define SD_HEADER_Y      54
#define SD_GRID_TOP     132
#define HINT_GLYPH_H  28
#define HINT_TEXT     22

static const char *TrendingNames[TREND_COUNT] = { "Gaming", "Sports", "Podcasts" };
static const char *AccountNames[ACCOUNT_FEED_COUNT] = { "Recommended", "Subscriptions", "History", "Watch Later" };

// home categories = the trending feeds, a Subscriptions feed and the Watch Later queue kept on this PS3,
// and - when signed in - the account's own feeds.
#define HOME_SUBS        TREND_COUNT
#define HOME_WATCHLATER  (TREND_COUNT + 1)
#define HOME_ACCOUNT     (TREND_COUNT + 2)                      // first account feed; + AccountFeed
#define HOME_CAT_COUNT   (HOME_ACCOUNT + ACCOUNT_FEED_COUNT)
#define SUBS_PER_CHANNEL 8      // latest videos pulled from each subscribed channel

// the L1/R1 cycle. signed out it is the original app's; signed in, the account feeds lead.
static const int SignedOutOrder[] = { HOME_SUBS, HOME_WATCHLATER, TREND_GAMING, TREND_SPORTS, TREND_PODCASTS };
static const int SignedInOrder[]  = {
   HOME_ACCOUNT + ACCOUNT_FEED_HOME, HOME_ACCOUNT + ACCOUNT_FEED_SUBSCRIPTIONS, HOME_ACCOUNT + ACCOUNT_FEED_HISTORY,
   HOME_ACCOUNT + ACCOUNT_FEED_WATCH_LATER, HOME_SUBS, HOME_WATCHLATER, TREND_GAMING, TREND_SPORTS, TREND_PODCASTS,
};

// each category's fully-loaded results, so returning to a tab skips the feed fetch (thumbnails still refetch).
static SearchResults categoryCache[HOME_CAT_COUNT];
static int           categoryCached[HOME_CAT_COUNT];
static int           subsRevisionSeen;         // subscriptions revision the cached HOME_SUBS feed was built at
static int           watchLaterRevisionSeen;   // watch-later revision the cached HOME_WATCHLATER feed was built at
static int           accountRevisionSeen;      // sign-in state the category order + account caches belong to

static struct {
   VideoGrid grid;
   int       category;
   int       screenW, screenH;
   int       accountOpen;   // the sign-in / account panel is showing
} home;

static Font        uiFont;
static Label       titleLabel, statusLabel;
static ButtonHints hints, hintsSecondRow;
static int         accountHint;   // index of the Select hint ("Sign in" / "Account")
static int         scaleHint = -1;
static int         sdHintsTwoRows;

// L2 cycles through these SD-only scales. The value is saved separately from
// settings.txt so the homebrew can update it without rewriting user settings.
#define SD_SCALE_PATH "/dev_hdd0/tmp/yo-player/sd-ui-scale.txt"
static const float SdScaleChoices[] = { 1.00f, 1.50f };
#define SD_SCALE_COUNT ((int)(sizeof SdScaleChoices / sizeof SdScaleChoices[0]))
static int sdScaleLoaded;
static int sdScaleIndex = 1; // default 1.5x on first launch

static void loadSdScale(void)
{
   if (sdScaleLoaded) return;
   sdScaleLoaded = 1;
   char buf[32];
   int n = readFile(SD_SCALE_PATH, buf, sizeof buf - 1);
   if (n > 0) {
      buf[n] = 0;
      float saved = 0.0f;
      if (sscanf(buf, "%f", &saved) == 1) {
         for (int i = 0; i < SD_SCALE_COUNT; i++) {
            float diff = saved - SdScaleChoices[i];
            if (diff > -0.001f && diff < 0.001f) { sdScaleIndex = i; break; }
         }
      }
   }
   setGfxSdUiScale(SdScaleChoices[sdScaleIndex]);
}

static void cycleSdScale(void)
{
   if (!isGfxSdVideoOutput()) return;
   // Keep this index synchronized when the channel screen changes the shared scale.
   float current = getGfxUiScaleFactor();
   for (int i = 0; i < SD_SCALE_COUNT; i++) {
      float diff = current - SdScaleChoices[i];
      if (diff > -0.001f && diff < 0.001f) { sdScaleIndex = i; break; }
   }
   sdScaleIndex = (sdScaleIndex + 1) % SD_SCALE_COUNT;
   setGfxSdUiScale(SdScaleChoices[sdScaleIndex]);
   char buf[32];
   int n = snprintf(buf, sizeof buf, "%.2f\n", (double)SdScaleChoices[sdScaleIndex]);
   if (n > 0) writeFile(SD_SCALE_PATH, buf, (uint64_t)n);
}

static void refreshScaleHint(void)
{
   if (!sdHintsTwoRows || scaleHint < 0) return;
   char caption[32];
   snprintf(caption, sizeof caption, "Scale %.1fx", (double)getGfxSdUiScale());
   setButtonHintCaption(&hintsSecondRow, scaleHint, caption);
}

static void setAccountHintCaption(const char *caption)
{
   setButtonHintCaption(sdHintsTwoRows ? &hintsSecondRow : &hints, accountHint, caption);
}


// the account panel
static Label       panelTitle, panelLine1, panelLine2, panelCode, panelFooter;
static ButtonHints panelHints;
static int         panelConfirmHint, panelBackHint;

// approximate seconds-ago from a "N unit ago" string. approximate is fine here (unlike a precise date sort):
// it only needs to group recent above old across channels. unknown/live sorts last.
#define AGE_UNKNOWN 2000000000L   // larger than any real age, still inside a 32-bit long

static long ageSeconds(const char *published)
{
   const char *digit = published;
   while (*digit && (*digit < '0' || *digit > '9')) digit++;
   if (!*digit) return AGE_UNKNOWN;
   long value = atol(digit);
   if      (strstr(published, "second")) return value;
   else if (strstr(published, "minute")) return value * 60;
   else if (strstr(published, "hour"))   return value * 3600;
   else if (strstr(published, "day"))    return value * 86400;
   else if (strstr(published, "week"))   return value * 604800;
   else if (strstr(published, "month"))  return value * 2592000;
   else if (strstr(published, "year"))   return value * 31536000;
   return AGE_UNKNOWN;
}

static int byRecency(const void *a, const void *b)
{
   long ageA = ageSeconds(((const SearchResult *)a)->published), ageB = ageSeconds(((const SearchResult *)b)->published);
   return (ageA > ageB) - (ageA < ageB);
}

// aggregate the latest videos from each subscribed channel into one feed, newest first. bails if the source
// was swapped away mid-fetch (each channel is a separate blocking request), so switching off stays snappy.
static int fetchSubscriptions(SearchResults *out)
{
   char channels[MAX_SUBSCRIPTIONS][CHANNEL_ID_LEN];
   int channelCount = getSubscriptions(channels, MAX_SUBSCRIPTIONS);
   memset(out, 0, sizeof *out);
   if (channelCount == 0) return -1;

   SearchResults *page = (SearchResults *)malloc(sizeof *page);
   if (!page) return -1;
   for (int c = 0; c < channelCount && out->count < MAX_SEARCH_RESULTS; c++) {
      if (gridStopRequested(&home.grid)) break;
      if (getChannelVideos(channels[c], NULL, page) != 0) continue;   // NULL token = the channel's latest page
      for (int i = 0, added = 0; i < page->count && added < SUBS_PER_CHANNEL && out->count < MAX_SEARCH_RESULTS; i++) {
         int duplicate = 0;
         for (int j = 0; j < out->count; j++)
            if (strcmp(out->items[j].videoId, page->items[i].videoId) == 0) { duplicate = 1; break; }
         if (duplicate) continue;
         // channel-page items omit their own channel id/name (the channel is implied) - stamp them so the
         // tile shows the channel and Triangle can open it.
         SearchResult *item = &out->items[out->count++];
         *item = page->items[i];
         strCopy(item->channelId, sizeof item->channelId, channels[c]);
         if (!item->author[0]) strCopy(item->author, sizeof item->author, page->channelName);
         added++;
      }
   }
   free(page);
   qsort(out->items, out->count, sizeof out->items[0], byRecency);   // mixed recency across all channels
   out->continuation[0] = 0;   // merged feed - no single continuation to page (for now)
   return out->count > 0 ? 0 : -1;
}

static int homeFetch(const char *token, SearchResults *out, void *user)
{
   (void)user;
   if (home.category >= HOME_ACCOUNT)    return getAccountFeed((AccountFeed)(home.category - HOME_ACCOUNT), token, out);
   if (home.category == HOME_SUBS)       return fetchSubscriptions(out);
   if (home.category == HOME_WATCHLATER) return getWatchLater(out) > 0 ? 0 : -1;
   return getTrending(home.category, token, out);
}

static void setHeading(void)
{
   // signed in, the account has its own Subscriptions and Watch Later, so name the PS3's copies apart
   const char *local = isSignedIn() ? " \xe2\x80\xa2 this PS3" : "";
   char heading[128];
   if      (home.category >= HOME_ACCOUNT)    snprintf(heading, sizeof heading, "%s", AccountNames[home.category - HOME_ACCOUNT]);
   else if (home.category == HOME_SUBS)       snprintf(heading, sizeof heading, "Subscriptions%s", local);
   else if (home.category == HOME_WATCHLATER) snprintf(heading, sizeof heading, "Watch Later%s", local);
   else                                       snprintf(heading, sizeof heading, "Trending \xe2\x80\xa2 %s", TrendingNames[home.category]);
   setLabelText(&titleLabel, heading);
}

static void loadCategory(void)
{
   setHeading();
   if (categoryCached[home.category]) setGridCached(&home.grid, &categoryCache[home.category], homeFetch, NULL);
   else                               setGridSource(&home.grid, homeFetch, NULL);
}

// cache the current category (only when it's fully loaded) so returning to it is instant.
static void cacheCurrent(void)
{
   if (gridStage(&home.grid) != GRID_READY || gridBusy(&home.grid)) return;   // don't copy mid-append
   categoryCache[home.category] = *gridResults(&home.grid);
   categoryCached[home.category] = 1;
}

static void switchCategory(int delta)
{
   cacheCurrent();
   const int *order = isSignedIn() ? SignedInOrder : SignedOutOrder;
   int count = isSignedIn() ? (int)(sizeof SignedInOrder / sizeof SignedInOrder[0])
                            : (int)(sizeof SignedOutOrder / sizeof SignedOutOrder[0]);
   int position = 0;
   while (position < count && order[position] != home.category) position++;
   if (position == count) position = 0;   // the category left the cycle (signed out): restart it
   home.category = order[(position + delta + count) % count];
   loadCategory();
}

// signed in or out (here, or a revoked token found by a feed request): the account caches belong to the
// old state, so drop them and land on the first tab of the new cycle.
static void applyAccountChange(void)
{
   accountRevisionSeen = getAccountRevision();
   for (int feed = 0; feed < ACCOUNT_FEED_COUNT; feed++) categoryCached[HOME_ACCOUNT + feed] = 0;
   setAccountHintCaption(isSignedIn() ? "Account" : "Sign in");
   home.category = isSignedIn() ? HOME_ACCOUNT + ACCOUNT_FEED_HOME : HOME_SUBS;
   loadCategory();
}

// ---- account panel (Select) ----

#define PANEL_W 820
#define PANEL_H 360

// setLabelText re-rasterises, so the countdown only pays for it once a second
static void setLabelIfChanged(Label *label, const char *text)
{
   if (strcmp(label->text, text) != 0) setLabelText(label, text);
}

static void openAccountPanel(void)
{
   home.accountOpen = 1;
   if (!isSignedIn()) beginSignIn();
}

static void closeAccountPanel(void)
{
   cancelSignIn();
   home.accountOpen = 0;
}

static void updateAccountPanel(void)
{
   SignInStatus status;
   getSignInStatus(&status);
   int retryable = status.stage == SIGNIN_DENIED || status.stage == SIGNIN_EXPIRED || status.stage == SIGNIN_FAILED;

   if (status.stage == SIGNIN_DONE) { home.accountOpen = 0; return; }   // applyAccountChange shows the new feeds
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) { closeAccountPanel(); return; }
   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      if (isSignedIn())   { signOut(); home.accountOpen = 0; }
      else if (retryable) beginSignIn();
   }
}

static void drawAccountPanel(void)
{
   SignInStatus status;
   getSignInStatus(&status);

   const char *title = "Sign in to YouTube", *line1 = "", *line2 = "", *code = "", *confirm = NULL;
   char url[96] = "", footer[64] = "";
   if (isSignedIn()) {
      title   = "Signed in to YouTube";
      line1   = "Recommended, Subscriptions, History and Watch Later";
      line2   = "come from your account. Videos still play signed out.";
      confirm = "Sign out";
   } else if (status.stage == SIGNIN_WAITING) {
      // "https://www.google.com/device" reads better without the scheme
      const char *shown = strstr(status.verificationUrl, "://");
      strCopy(url, sizeof url, shown ? shown + 3 : status.verificationUrl);
      line1 = "On your phone or computer, go to";
      line2 = url;
      code  = status.userCode;
      snprintf(footer, sizeof footer, "and enter this code. It expires in %d:%02d.", status.secondsLeft / 60, status.secondsLeft % 60);
   } else if (status.stage == SIGNIN_DENIED)  { line1 = "Sign-in was declined on the consent screen."; confirm = "Try again"; }
   else if (status.stage == SIGNIN_EXPIRED)   { line1 = "The code expired before it was entered.";    confirm = "Try again"; }
   else if (status.stage == SIGNIN_FAILED)    { line1 = "Couldn't reach Google. Check the connection."; confirm = "Try again"; }
   else                                       { line1 = "Getting a sign-in code..."; }

   setLabelIfChanged(&panelTitle, title);
   setLabelIfChanged(&panelLine1, line1);
   setLabelIfChanged(&panelLine2, line2);
   setLabelIfChanged(&panelCode, code);
   setLabelIfChanged(&panelFooter, footer);
   setButtonHintShown(&panelHints, panelConfirmHint, confirm != NULL);
   if (confirm && strcmp(panelHints.hints[panelConfirmHint].caption.text, confirm) != 0)
      setButtonHintCaption(&panelHints, panelConfirmHint, confirm);
   const char *back = (status.stage == SIGNIN_STARTING || status.stage == SIGNIN_WAITING) ? "Cancel" : "Close";
   if (strcmp(panelHints.hints[panelBackHint].caption.text, back) != 0) setButtonHintCaption(&panelHints, panelBackHint, back);

   int x = (home.screenW - PANEL_W) / 2, y = (home.screenH - PANEL_H) / 2, centerX = home.screenW / 2;
   fillGfxRectangle(0, 0, home.screenW, home.screenH, activeTheme->scrim);
   fillGfxRectangle(x, y, PANEL_W, PANEL_H, activeTheme->appBg);
   fillGfxRectangle(x, y, PANEL_W, 4, activeTheme->accent);
   drawLabelCentered(&panelTitle, centerX, y + 30);
   drawLabelCentered(&panelLine1, centerX, y + 92);
   if (line2[0])  drawLabelCentered(&panelLine2, centerX, y + 128);
   if (code[0])   drawLabelCentered(&panelCode, centerX, y + 178);
   if (footer[0]) drawLabelCentered(&panelFooter, centerX, y + 252);
   drawButtonHints(&panelHints, home.screenW);
}

// OSK confirmed: a link plays directly, anything else opens a search.
static void onQueryEntered(const char *text)
{
   logInfo("[diag-search] home callback entered text=%s len=%d\n", text ? "present" : "NULL", text ? (int)strlen(text) : 0);
   if (!text || !text[0]) { logInfo("[diag-search] home callback empty; return\n"); return; }
   if (strstr(text, "youtu")) { logInfo("[diag-search] home URL path: stop grid\n"); stopVideoGrid(&home.grid); logInfo("[diag-search] home URL path: playVideo\n"); playVideo(text); return; }
   logInfo("[diag-search] home query path: cacheCurrent begin\n");
   cacheCurrent();
   logInfo("[diag-search] home query path: cacheCurrent done; openSearchQuery begin\n");
   openSearchQuery(text);
   logInfo("[diag-search] home query path: openSearchQuery returned\n");
}

static void initHome(void)
{
   loadSdScale();
   home.screenW = getGfxScreenWidth();
   home.screenH = getGfxScreenHeight();
   sdHintsTwoRows = isGfxSdVideoOutput();
   int headerY = sdHintsTwoRows ? SD_HEADER_Y : HEADER_Y;
   int gridTop = sdHintsTwoRows ? SD_GRID_TOP : GRID_TOP;

   uiFont = openSystemFont(FONT_POP);
   initLabel(&titleLabel,  &uiFont, MARGIN_X, headerY, home.screenW - 2 * MARGIN_X, AUTO, TITLE_SIZE, activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&statusLabel, &uiFont, MARGIN_X, gridTop, home.screenW - 2 * MARGIN_X, AUTO, 21, activeTheme->textSecondary, TEXT_NOWRAP, "Loading...");
   // Home heading/status grow by rerasterising their font, while retaining their
   // original anchors instead of being pushed off the 4:3 safe area.
   setLabelScaleWithUi(&titleLabel, 1);
   setLabelScaleWithUi(&statusLabel, 1);

   if (sdHintsTwoRows) {
      // Reserve the maximum row height so pressing L2 only changes text/icon size,
      // not the absolute positions of the footer or grid.
      int maxGlyphHeight = (int)(HINT_GLYPH_H * SD_MAX_SCALE + 0.5f);
      initButtonHints(&hints, &uiFont,
                      home.screenH - SD_HINTS_BOTTOM - maxGlyphHeight - SD_HINT_ROW_GAP,
                      HINT_GLYPH_H, HINT_TEXT, activeTheme->textSecondary);
      addButtonHint(&hints, getConsoleGlyph(GLYPH_CROSS),    "Play");
      addButtonHint(&hints, getConsoleGlyph(GLYPH_SQUARE),   "Watch Later");
      addButtonHint(&hints, getConsoleGlyph(GLYPH_TRIANGLE), "Channel");
      addButtonHint(&hints, getConsoleGlyph(GLYPH_R3),       "Download");
      setButtonHintsStableLayout(&hints, 1);

      initButtonHints(&hintsSecondRow, &uiFont, home.screenH - SD_HINTS_BOTTOM,
                      HINT_GLYPH_H, HINT_TEXT, activeTheme->textSecondary);
      addButtonHint(&hintsSecondRow, getConsoleGlyph(GLYPH_START), "Search");
      accountHint = addButtonHint(&hintsSecondRow, getConsoleGlyph(GLYPH_SELECT), isSignedIn() ? "Account" : "Sign in");
      addButtonHint(&hintsSecondRow, getConsoleGlyph(GLYPH_L1), "");
      addButtonHint(&hintsSecondRow, getConsoleGlyph(GLYPH_R1), "Category");
      scaleHint = addButtonHint(&hintsSecondRow, getConsoleGlyph(GLYPH_L2), "Scale 1.5x");
      setButtonHintsStableLayout(&hintsSecondRow, 1);
      refreshScaleHint();
   } else {
      initButtonHints(&hints, &uiFont, home.screenH - HINTS_BOTTOM, HINT_GLYPH_H, HINT_TEXT, activeTheme->textSecondary);
      addButtonHint(&hints, getConsoleGlyph(GLYPH_CROSS),    "Play");
      addButtonHint(&hints, getConsoleGlyph(GLYPH_SQUARE),   "Watch Later");
      addButtonHint(&hints, getConsoleGlyph(GLYPH_TRIANGLE), "Channel");
      addButtonHint(&hints, getConsoleGlyph(GLYPH_R3),       "Download");
      addButtonHint(&hints, getConsoleGlyph(GLYPH_START),    "Search");
      accountHint = addButtonHint(&hints, getConsoleGlyph(GLYPH_SELECT), isSignedIn() ? "Account" : "Sign in");
      addButtonHint(&hints, getConsoleGlyph(GLYPH_L1),       "");
      addButtonHint(&hints, getConsoleGlyph(GLYPH_R1),       "Category");
   }

   int panelY = (home.screenH - PANEL_H) / 2;
   initLabel(&panelTitle,  &uiFont, 0, 0, PANEL_W, AUTO, 30, activeTheme->textPrimary,   TEXT_NOWRAP, "");
   initLabel(&panelLine1,  &uiFont, 0, 0, PANEL_W, AUTO, 22, activeTheme->textSecondary, TEXT_NOWRAP, "");
   initLabel(&panelLine2,  &uiFont, 0, 0, PANEL_W, AUTO, 22, activeTheme->textPrimary,   TEXT_NOWRAP, "");
   initLabel(&panelCode,   &uiFont, 0, 0, PANEL_W, AUTO, 56, activeTheme->textPrimary,   TEXT_NOWRAP, "");
   initLabel(&panelFooter, &uiFont, 0, 0, PANEL_W, AUTO, 20, activeTheme->textSecondary, TEXT_NOWRAP, "");
   initButtonHints(&panelHints, &uiFont, panelY + PANEL_H - 48, HINT_GLYPH_H, HINT_TEXT, activeTheme->textSecondary);
   panelConfirmHint = addButtonHint(&panelHints, getConsoleGlyph(GLYPH_CROSS),  "Try again");
   panelBackHint    = addButtonHint(&panelHints, getConsoleGlyph(GLYPH_CIRCLE), "Close");

   int gridHeight;
   if (sdHintsTwoRows) {
      int maxGlyphHeight = (int)(HINT_GLYPH_H * SD_MAX_SCALE + 0.5f);
      gridHeight = home.screenH - gridTop - SD_HINTS_BOTTOM - maxGlyphHeight - SD_HINT_ROW_GAP - SD_GRID_BOTTOM_GAP;
   } else {
      gridHeight = home.screenH - gridTop - HINTS_BOTTOM - HINT_GLYPH_H - GRID_BOTTOM_GAP;
   }
   initVideoGrid(&home.grid, &uiFont, MARGIN_X, gridTop, home.screenW - 2 * MARGIN_X, gridHeight);
   setGridWatchedPredicate(&home.grid, isWatched);
   setGridWatchLaterPredicate(&home.grid, isWatchLater);

   subsRevisionSeen = getSubscriptionsRevision();
   watchLaterRevisionSeen = getWatchLaterRevision();
   accountRevisionSeen = getAccountRevision();
   // signed in, start on the account's Recommended feed; otherwise on Subscriptions, as before
   home.category = isSignedIn() ? HOME_ACCOUNT + ACCOUNT_FEED_HOME : HOME_SUBS;
   loadCategory();
}

static void updateHome(void)
{
   updateDownloadOverlay();
   updateSignIn();
   if (getAccountRevision() != accountRevisionSeen) applyAccountChange();
   if (home.accountOpen) { updateAccountPanel(); return; }
   if (oskInputActive()) return;

   // Cycle and persist the SD readability scale; thumbnails keep their size.
   if (isPadButtonPressed(PAD_BTN_L2) && isGfxSdVideoOutput()) {
      cycleSdScale();
      refreshScaleHint();
      return;
   }

   if (isPadButtonPressed(PAD_BTN_START)) oskInputBegin("Search YouTube", "", onQueryEntered);
   if (isPadButtonPressed(PAD_BTN_SELECT)) { openAccountPanel(); return; }

   // L1/R1 stay responsive even mid-load (the source swap is non-blocking)
   if      (isPadButtonPressed(PAD_BTN_R1)) { switchCategory(1);  return; }
   else if (isPadButtonPressed(PAD_BTN_L1)) { switchCategory(-1); return; }

   updateVideoGrid(&home.grid);
   if (gridStage(&home.grid) != GRID_READY) return;

   const SearchResult *selected = gridSelected(&home.grid);
   if (!selected) return;
   if (isPadButtonPressed(PAD_BTN_SQUARE)) {
      toggleWatchLater(selected);
      watchLaterRevisionSeen = getWatchLaterRevision();
      categoryCached[HOME_WATCHLATER] = 0;
      if (home.category == HOME_WATCHLATER) loadCategory();   // reflect the add/remove immediately
      return;
   }
   if (isPadButtonPressed(PAD_BTN_R3)) { if (!selected->isLive) enqueueDownload(selected); return; }   // live can't be downloaded
   // Recommended tiles don't say which channel they are from
   if (isPadButtonPressed(PAD_BTN_TRIANGLE)) {
      logInfo("[diag-triangle] home page=%d selected=%d videoId=%s channelIdLen=%d authorLen=%d\n",
         home.category, home.grid.selected, selected->videoId,
         (int)strlen(selected->channelId), (int)strlen(selected->author));
      SearchResult channelItem = *selected;
      if (!channelItem.channelId[0]) {
         logInfo("[home] Triangle: resolving channel for video=%s\n", channelItem.videoId);
         if (getVideoChannelId(channelItem.videoId, channelItem.channelId, sizeof channelItem.channelId) != 0) {
            logError("[home] Triangle: channel lookup failed; navigation skipped video=%s\n", channelItem.videoId);
            return;
         }
         logInfo("[home] Triangle: resolved channelId=%s\n", channelItem.channelId);
      }

      logInfo("[diag-triangle] home cacheCurrent begin\n");
      cacheCurrent();
      logInfo("[diag-triangle] home cacheCurrent done; openChannelFor begin\n");
      openChannelFor(&channelItem);
      logInfo("[diag-triangle] home openChannelFor returned\n");
      return;
   }
   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      markWatched(selected->videoId);
      stopVideoGrid(&home.grid);   // no per-call http fetch may overlap the http media stream during playback
      playVideo(selected->videoId);
   }
}

static void drawHome(void)
{
   // Explicit, change-only refresh: never allocate/rebuild text textures from
   // inside drawLabel itself.
   refreshLabelScale(&titleLabel);
   refreshLabelScale(&statusLabel);
   fillGfxRectangle(0, 0, home.screenW, home.screenH, activeTheme->appBg);
   drawLabel(&titleLabel);
   if (gridStage(&home.grid) == GRID_READY) {
      drawVideoGrid(&home.grid);
   } else {
      setLabelText(&statusLabel, gridStage(&home.grid) == GRID_EMPTY ? "No results" : "Loading...");
      drawLabel(&statusLabel);
   }
   drawButtonHints(&hints, home.screenW);
   if (sdHintsTwoRows) drawButtonHints(&hintsSecondRow, home.screenW);
   drawDownloadOverlay(home.screenW);
   if (home.accountOpen) drawAccountPanel();
}

static void suspendHome(void) { stopVideoGrid(&home.grid); }   // free the http pools for the pushed screen

// returning from search/play: if the load was interrupted mid-flight (worker stopped), restart it; an
// already-loaded grid is kept as-is (instant, thumbnails intact).
static void resumeHome(void)
{
   // a subscribe/unsubscribe while we were away makes the cached Subscriptions feed stale - drop it and,
   // if it's the current tab, refetch now.
   if (getSubscriptionsRevision() != subsRevisionSeen) {
      subsRevisionSeen = getSubscriptionsRevision();
      categoryCached[HOME_SUBS] = 0;
      if (home.category == HOME_SUBS) { loadCategory(); return; }
   }
   if (getWatchLaterRevision() != watchLaterRevisionSeen) {
      watchLaterRevisionSeen = getWatchLaterRevision();
      categoryCached[HOME_WATCHLATER] = 0;
      if (home.category == HOME_WATCHLATER) { loadCategory(); return; }
   }
   if (gridStage(&home.grid) != GRID_READY) loadCategory();
}

static void termHome(void)
{
   termVideoGrid(&home.grid);
   termButtonHints(&hints);
   termButtonHints(&hintsSecondRow);
   termButtonHints(&panelHints);
   freeLabel(&titleLabel);
   freeLabel(&statusLabel);
   freeLabel(&panelTitle);
   freeLabel(&panelLine1);
   freeLabel(&panelLine2);
   freeLabel(&panelCode);
   freeLabel(&panelFooter);
   closeFont(&uiFont);
}

void openHome(void) { changeScreen(&homeScreen); }

// Screen vtable order: init, resume, update, draw, suspend, term, status.
Screen homeScreen = { initHome, resumeHome, updateHome, drawHome, suspendHome, termHome, SCREEN_TERMINATED };
