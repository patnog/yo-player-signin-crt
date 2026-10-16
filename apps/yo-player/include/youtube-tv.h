#pragma once

// youtube-tv - reads the TVHTML5 client's browse responses (the signed-in account feeds: Recommended,
// Subscriptions, History, Watch Later). The TV client is the only one YouTube accepts an OAuth bearer
// token from, and it answers in its own layout - tileRenderer items inside shelves (home) or a grid
// (history, playlists) - which the WEB parser in youtube.c does not recognise.
//
// Pure text scanning with no platform calls, so the host test can run it against saved responses.

#include "extractor.h"   // SearchResults, AccountFeed

#define TV_CLIENT_NAME_ID  "7"   // X-YouTube-Client-Name for TVHTML5
#define TV_CLIENT_VERSION  "7.20250923.13.00"
#define TV_USER_AGENT      "Mozilla/5.0 (SMART-TV; Linux; Tizen 6.0) AppleWebKit/538.1 (KHTML, like Gecko) Version/6.0 TV Safari/538.1"

// the browse request body for an account feed's first page (continuation NULL) or a later page.
// returns the length, or -1 if it did not fit cap.
int buildTvBrowseBody(char *out, int cap, AccountFeed feed, const char *continuation);

// fill out->items from every playable video tile in resp (ads and non-video tiles are skipped, repeats
// dropped) and out->continuation from the page-level "load more" token. out is cleared first.
void parseTvTiles(const char *resp, int respLen, SearchResults *out);
