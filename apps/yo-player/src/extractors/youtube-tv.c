// youtube-tv - TVHTML5 browse response reader (see youtube-tv.h).
//
// A tile looks like (trimmed):
//   "tileRenderer":{ "header":{ ...thumbnailOverlayTimeStatusRenderer {"simpleText":"57:59","style":"DEFAULT"} },
//                    "metadata":{"tileMetadataRenderer":{ "title":{"simpleText":"..."},
//                        "lines":[ {lineItemRenderer "MrBeast"}, {lineItemRenderer "199M views"}, "•", "6 days ago" ] }},
//                    "onSelectCommand":{...}, "contentId":"plN7JMbadRg", "contentType":"TILE_CONTENT_TYPE_VIDEO" }
// Ad tiles have the same shape but no contentType, which is what drops them.

#include "youtube-tv.h"
#include "json.h"               // decodeJsonString
#include "string-utilities.h"   // strCopy

#include <string.h>
#include <stdio.h>

#define TV_CONTEXT \
   "{\"context\":{\"client\":{\"clientName\":\"TVHTML5\",\"clientVersion\":\"" TV_CLIENT_VERSION "\",\"hl\":\"en\",\"gl\":\"US\"}}"

// order matches AccountFeed
static const char *ACCOUNT_FEED_IDS[ACCOUNT_FEED_COUNT] = { "FEwhat_to_watch", "FEsubscriptions", "FEhistory", "VLWL" };

int buildTvBrowseBody(char *out, int cap, AccountFeed feed, const char *continuation)
{
   if (feed < 0 || feed >= ACCOUNT_FEED_COUNT) return -1;
   int length = continuation ? snprintf(out, cap, TV_CONTEXT ",\"continuation\":\"%s\"}", continuation)
                             : snprintf(out, cap, TV_CONTEXT ",\"browseId\":\"%s\"}", ACCOUNT_FEED_IDS[feed]);
   return (length < 0 || length >= cap) ? -1 : length;
}

// copy the json string value of "key" found within [from,end). 1 if found.
static int jsonString(const char *from, const char *end, const char *key, char *out, int cap)
{
   char pat[48];
   int patLen = snprintf(pat, sizeof pat, "\"%s\":\"", key);
   const char *p = from ? strstr(from, pat) : NULL;
   if (!p || p >= end) { if (cap) out[0] = 0; return 0; }
   p += patLen;
   decodeJsonString(p, (int)(end - p), out, cap);
   return 1;
}

// a text object is either {"simpleText":"..."} or {"runs":[{"text":"..."}]}: read whichever comes first
// inside [from,end), so a runs-style title can't pick up a later simpleText.
static int readText(const char *from, const char *end, char *out, int cap)
{
   const char *simple = strstr(from, "\"simpleText\":\"");
   const char *runs   = strstr(from, "\"text\":\"");
   if (simple && simple >= end) simple = NULL;
   if (runs && runs >= end) runs = NULL;
   if (simple && (!runs || simple < runs)) return jsonString(simple, end, "simpleText", out, cap);
   if (runs) return jsonString(runs, end, "text", out, cap);
   if (cap) out[0] = 0;
   return 0;
}

static int endsWith(const char *text, const char *suffix)
{
   int textLen = (int)strlen(text), suffixLen = (int)strlen(suffix);
   return textLen >= suffixLen && strcmp(text + textLen - suffixLen, suffix) == 0;
}

// the tile's metadata lines: the first item is the channel, later ones are views and age with "•"
// separators between them.
static void readLines(const char *meta, const char *end, SearchResult *item)
{
   const char *lines = strstr(meta, "\"lines\":");
   if (!lines || lines >= end) return;

   const char *scan = lines;
   int index = 0;
   while ((scan = strstr(scan, "\"lineItemRenderer\":")) != NULL && scan < end) {
      const char *next = strstr(scan + 1, "\"lineItemRenderer\":");
      const char *itemEnd = (next && next < end) ? next : end;
      char text[64];
      if (readText(scan, itemEnd, text, sizeof text)) {
         if (index == 0)                                              strCopy(item->author, sizeof item->author, text);
         else if (endsWith(text, " views") || endsWith(text, " watching")) strCopy(item->views, sizeof item->views, text);
         else if (endsWith(text, " ago"))                             strCopy(item->published, sizeof item->published, text);
         index++;
      }
      scan = itemEnd;
   }
}

// fill one result from a tile block [block,end). 1 if it is a playable video with a title.
static int parseTile(const char *block, const char *end, SearchResult *item)
{
   const char *videoType = strstr(block, "\"contentType\":\"TILE_CONTENT_TYPE_VIDEO\"");
   if (!videoType || videoType >= end) return 0;
   if (!jsonString(block, end, "contentId", item->videoId, sizeof item->videoId) || !item->videoId[0]) return 0;

   const char *meta = strstr(block, "\"tileMetadataRenderer\":");
   if (!meta || meta >= end) return 0;
   const char *title = strstr(meta, "\"title\":");
   const char *lines = strstr(meta, "\"lines\":");
   if (title && title < end) readText(title, (lines && lines < end) ? lines : end, item->title, sizeof item->title);
   readLines(meta, end, item);

   const char *overlay = strstr(block, "\"thumbnailOverlayTimeStatusRenderer\":");
   if (overlay && overlay < meta) {
      const char *live = strstr(overlay, "\"style\":\"LIVE\"");
      item->isLive = live && live < meta;
      if (!item->isLive) jsonString(overlay, meta, "simpleText", item->duration, sizeof item->duration);
   }

   // history and playlist tiles name their channel; home tiles don't (left empty, so Triangle is skipped)
   const char *channel = strstr(block, "\"browseId\":\"UC");
   if (channel && channel < end) jsonString(channel, end, "browseId", item->channelId, sizeof item->channelId);

   return item->title[0] != 0;
}

static int isDuplicate(const SearchResults *out, const char *videoId)
{
   for (int i = 0; i < out->count; i++) if (strcmp(out->items[i].videoId, videoId) == 0) return 1;
   return 0;
}

void parseTvTiles(const char *resp, int respLen, SearchResults *out)
{
   static const char TILE[] = "\"tileRenderer\":{";
   memset(out, 0, sizeof *out);
   const char *end = resp + respLen;

   const char *block = strstr(resp, TILE);
   while (block && block < end && out->count < MAX_SEARCH_RESULTS) {
      const char *next = strstr(block + 1, TILE);
      const char *blockEnd = (next && next < end) ? next : end;
      SearchResult *item = &out->items[out->count];
      memset(item, 0, sizeof *item);
      if (parseTile(block, blockEnd, item) && !isDuplicate(out, item->videoId)) out->count++;
      block = next;
   }

   // every shelf carries its own sideways "load more" token; the page-level one (sectionList on home,
   // the grid on history/playlists) closes the response, so the last token is the one that pages down.
   static const char MORE[] = "\"nextContinuationData\":{\"continuation\":\"";
   const char *last = NULL;
   for (const char *hit = strstr(resp, MORE); hit && hit < end; hit = strstr(hit + 1, MORE)) last = hit;
   if (last) jsonString(last, end, "continuation", out->continuation, sizeof out->continuation);
}
