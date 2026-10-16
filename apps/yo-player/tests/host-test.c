// host-test - checks the sign-in protocol and the TV feed reader on a PC (see tests/run.sh).
//
//   host-test                    synthetic cases only
//   host-test feed <file>...     also parse saved TVHTML5 browse responses and print what was read
//
// The synthetic cases are shaped like real responses but hold no account data.

#include "account-oauth.h"
#include "youtube-tv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static SearchResults results;   // large: keep off the stack

// ---- oauth ----

static void testOAuth(void)
{
   char body[1024];

   CHECK(buildDeviceCodeBody(body, sizeof body, "abc-123") > 0);
   CHECK(strstr(body, "\"device_id\":\"abc-123\"") != NULL);
   CHECK(strstr(body, "\"client_id\":\"" OAUTH_CLIENT_ID "\"") != NULL);
   CHECK(buildDeviceCodeBody(body, 20, "abc") == -1);   // does not fit

   CHECK(buildTokenPollBody(body, sizeof body, "4/0AbC_d-e.f~g") > 0);
   CHECK(strstr(body, "&code=4%2F0AbC_d-e.f~g&") != NULL);   // '/' is escaped, unreserved kept
   CHECK(strstr(body, "grant_type=http%3A%2F%2Foauth.net%2Fgrant_type%2Fdevice%2F1.0") != NULL);
   CHECK(buildTokenPollBody(body, 40, "x") == -1);

   CHECK(buildRefreshBody(body, sizeof body, "1//0gRefresh+Token=") > 0);
   CHECK(strstr(body, "refresh_token=1%2F%2F0gRefresh%2BToken%3D&grant_type=refresh_token") != NULL);
   CHECK(buildRevokeBody(body, sizeof body, "a b") > 0 && strcmp(body, "token=a%20b") == 0);

   DeviceCode code;
   const char *good = "{\n \"device_code\": \"AH-1Ng\",\n \"user_code\": \"WHC-PWK-QFQR\",\n"
                      " \"expires_in\": 1800,\n \"interval\": 5,\n \"verification_url\": \"https://www.google.com/device\"\n}";
   CHECK(parseDeviceCode(good, (int)strlen(good), &code) == 0);
   CHECK(strcmp(code.userCode, "WHC-PWK-QFQR") == 0 && strcmp(code.deviceCode, "AH-1Ng") == 0);
   CHECK(code.expiresIn == 1800 && code.interval == 5);
   CHECK(strcmp(code.verificationUrl, "https://www.google.com/device") == 0);

   const char *noInterval = "{\"device_code\":\"d\",\"user_code\":\"u\",\"expires_in\":600}";
   CHECK(parseDeviceCode(noInterval, (int)strlen(noInterval), &code) == 0);
   CHECK(code.interval == 5);                                  // spec default
   CHECK(strstr(code.verificationUrl, "google.com/device"));   // default url
   const char *noUser = "{\"device_code\":\"d\",\"expires_in\":600}";
   CHECK(parseDeviceCode(noUser, (int)strlen(noUser), &code) == -1);
   CHECK(parseDeviceCode("not json", 8, &code) == -1);

   OAuthTokens tokens;
   const char *grant = "{\"access_token\":\"ya29.A0\",\"expires_in\":86399,\"refresh_token\":\"1//0gR\","
                       "\"scope\":\"x\",\"token_type\":\"Bearer\"}";
   CHECK(parseTokenResponse(grant, (int)strlen(grant), &tokens) == OAUTH_OK);
   CHECK(strcmp(tokens.accessToken, "ya29.A0") == 0 && strcmp(tokens.refreshToken, "1//0gR") == 0);
   CHECK(tokens.expiresIn == 86399);

   const char *refreshed = "{\"access_token\":\"ya29.B1\",\"expires_in\":83937,\"scope\":\"x\",\"token_type\":\"Bearer\"}";
   CHECK(parseTokenResponse(refreshed, (int)strlen(refreshed), &tokens) == OAUTH_OK);
   CHECK(tokens.refreshToken[0] == 0);   // a refresh does not hand out a new refresh token

   struct { const char *error; OAuthResult expected; } errors[] = {
      { "authorization_pending", OAUTH_PENDING },  { "slow_down", OAUTH_SLOW_DOWN },
      { "access_denied", OAUTH_DENIED },           { "expired_token", OAUTH_EXPIRED },
      { "invalid_grant", OAUTH_INVALID_GRANT },    { "invalid_client", OAUTH_ERROR },
   };
   for (unsigned i = 0; i < sizeof errors / sizeof errors[0]; i++) {
      char answer[160];
      snprintf(answer, sizeof answer, "{\n  \"error\": \"%s\",\n  \"error_description\": \"x\"\n}", errors[i].error);
      CHECK(parseTokenResponse(answer, (int)strlen(answer), &tokens) == errors[i].expected);
   }
   CHECK(parseTokenResponse("{}", 2, &tokens) == OAUTH_ERROR);
   CHECK(parseTokenResponse("", 0, &tokens) == OAUTH_ERROR);
}

// ---- tv feed ----

static void testBodies(void)
{
   char body[1024];
   CHECK(buildTvBrowseBody(body, sizeof body, ACCOUNT_FEED_HOME, NULL) > 0);
   CHECK(strstr(body, "\"clientName\":\"TVHTML5\"") && strstr(body, "\"browseId\":\"FEwhat_to_watch\""));
   CHECK(buildTvBrowseBody(body, sizeof body, ACCOUNT_FEED_WATCH_LATER, NULL) > 0 && strstr(body, "\"browseId\":\"VLWL\""));
   CHECK(buildTvBrowseBody(body, sizeof body, ACCOUNT_FEED_HISTORY, "TOKEN") > 0);
   CHECK(strstr(body, "\"continuation\":\"TOKEN\"") && !strstr(body, "browseId"));
   CHECK(buildTvBrowseBody(body, sizeof body, ACCOUNT_FEED_COUNT, NULL) == -1);
   CHECK(buildTvBrowseBody(body, 30, ACCOUNT_FEED_HOME, NULL) == -1);
}

// two shelves: a video tile (runs-style channel, simpleText title), a tile with a runs-style title and a
// live overlay, an ad tile (no contentType), a non-video tile, a repeat, and the shelf + page tokens.
static const char *HOME_SAMPLE =
   "{\"contents\":{\"sectionListRenderer\":{\"contents\":["
   "{\"shelfRenderer\":{\"content\":{\"horizontalListRenderer\":{\"items\":["
   "{\"tileRenderer\":{\"style\":\"TILE_STYLE_YTLR_DEFAULT\",\"header\":{\"tileHeaderRenderer\":{\"thumbnailOverlays\":["
   "{\"thumbnailOverlayTimeStatusRenderer\":{\"text\":{\"simpleText\":\"57:59\"},\"style\":\"DEFAULT\"}}]}},"
   "\"metadata\":{\"tileMetadataRenderer\":{\"title\":{\"simpleText\":\"Eat \\\"Everything\\\" In A Store\"},\"lines\":["
   "{\"lineRenderer\":{\"items\":[{\"lineItemRenderer\":{\"text\":{\"runs\":[{\"text\":\"MrBeast\"}]}}}]}},"
   "{\"lineRenderer\":{\"items\":[{\"lineItemRenderer\":{\"text\":{\"simpleText\":\"199M views\"}}},"
   "{\"lineItemRenderer\":{\"text\":{\"simpleText\":\"\\u2022\"}}},"
   "{\"lineItemRenderer\":{\"text\":{\"simpleText\":\"6 days ago\"}}}]}}]}},"
   "\"onSelectCommand\":{\"watchEndpoint\":{\"videoId\":\"plN7JMbadRg\"}},"
   "\"contentId\":\"plN7JMbadRg\",\"contentType\":\"TILE_CONTENT_TYPE_VIDEO\"}},"
   "{\"tileRenderer\":{\"header\":{\"tileHeaderRenderer\":{\"thumbnailOverlays\":["
   "{\"thumbnailOverlayTimeStatusRenderer\":{\"style\":\"LIVE\"}}]}},"
   "\"metadata\":{\"tileMetadataRenderer\":{\"title\":{\"runs\":[{\"text\":\"Live \"},{\"text\":\"News\"}]},\"lines\":["
   "{\"lineRenderer\":{\"items\":[{\"lineItemRenderer\":{\"text\":{\"simpleText\":\"News Channel\"}}}]}},"
   "{\"lineRenderer\":{\"items\":[{\"lineItemRenderer\":{\"text\":{\"simpleText\":\"12K watching\"}}}]}}]}},"
   "\"contentId\":\"liveLiveLive\",\"contentType\":\"TILE_CONTENT_TYPE_VIDEO\"}},"
   "{\"adSlotRenderer\":{\"fulfillmentContent\":{\"tileRenderer\":{\"metadata\":{\"tileMetadataRenderer\":"
   "{\"title\":{\"simpleText\":\"Hands-free messaging keeps the chat flowing.\"}}},\"contentId\":\"adAdAdAdAdA\"}}}},"
   "{\"tileRenderer\":{\"metadata\":{\"tileMetadataRenderer\":{\"title\":{\"simpleText\":\"A Playlist\"}}},"
   "\"contentId\":\"PLxxxx\",\"contentType\":\"TILE_CONTENT_TYPE_PLAYLIST\"}},"
   "{\"tileRenderer\":{\"metadata\":{\"tileMetadataRenderer\":{\"title\":{\"simpleText\":\"Repeat\"}}},"
   "\"contentId\":\"plN7JMbadRg\",\"contentType\":\"TILE_CONTENT_TYPE_VIDEO\"}}],"
   "\"continuations\":[{\"nextContinuationData\":{\"continuation\":\"SHELF_TOKEN\"}}]}}}},"
   "{\"shelfRenderer\":{\"content\":{\"horizontalListRenderer\":{\"items\":[]}}}}"
   "],\"continuations\":[{\"nextContinuationData\":{\"continuation\":\"PAGE_TOKEN\",\"clickTrackingParams\":\"x\"}}]}}}";

static const char *HISTORY_SAMPLE =
   "{\"gridRenderer\":{\"items\":[{\"tileRenderer\":{\"header\":{},\"metadata\":{\"tileMetadataRenderer\":"
   "{\"title\":{\"simpleText\":\"Old Video\"},\"lines\":[{\"lineRenderer\":{\"items\":[{\"lineItemRenderer\":"
   "{\"text\":{\"runs\":[{\"text\":\"Some Channel\",\"navigationEndpoint\":{\"browseEndpoint\":"
   "{\"browseId\":\"UCabcdefghijklmnopqrstuv\"}}}]}}}]}}]}},\"contentId\":\"oldOldOldOl\","
   "\"contentType\":\"TILE_CONTENT_TYPE_VIDEO\"}}],"
   "\"continuations\":[{\"nextContinuationData\":{\"continuation\":\"GRID_TOKEN\"}}]}}";

static void testSamples(void)
{
   parseTvTiles(HOME_SAMPLE, (int)strlen(HOME_SAMPLE), &results);
   CHECK(results.count == 2);   // the ad, the playlist and the repeat are dropped
   CHECK(strcmp(results.items[0].videoId, "plN7JMbadRg") == 0);
   CHECK(strcmp(results.items[0].title, "Eat \"Everything\" In A Store") == 0);
   CHECK(strcmp(results.items[0].author, "MrBeast") == 0);
   CHECK(strcmp(results.items[0].views, "199M views") == 0);
   CHECK(strcmp(results.items[0].published, "6 days ago") == 0);
   CHECK(strcmp(results.items[0].duration, "57:59") == 0 && !results.items[0].isLive);
   CHECK(results.items[0].channelId[0] == 0);
   CHECK(strcmp(results.items[1].videoId, "liveLiveLive") == 0);
   CHECK(strcmp(results.items[1].title, "Live ") == 0 || strcmp(results.items[1].title, "Live News") == 0);
   CHECK(results.items[1].isLive && results.items[1].duration[0] == 0);
   CHECK(strcmp(results.items[1].author, "News Channel") == 0 && strcmp(results.items[1].views, "12K watching") == 0);
   CHECK(strcmp(results.continuation, "PAGE_TOKEN") == 0);   // the page token, not the shelf's

   parseTvTiles(HISTORY_SAMPLE, (int)strlen(HISTORY_SAMPLE), &results);
   CHECK(results.count == 1);
   CHECK(strcmp(results.items[0].channelId, "UCabcdefghijklmnopqrstuv") == 0);
   CHECK(strcmp(results.items[0].author, "Some Channel") == 0);
   CHECK(strcmp(results.continuation, "GRID_TOKEN") == 0);

   parseTvTiles("{}", 2, &results);
   CHECK(results.count == 0 && results.continuation[0] == 0);
   parseTvTiles("", 0, &results);
   CHECK(results.count == 0);
}

// ---- saved real responses ----

static int parseFile(const char *path)
{
   FILE *file = fopen(path, "rb");
   if (!file) { printf("cannot open %s\n", path); return 1; }
   fseek(file, 0, SEEK_END);
   long size = ftell(file);
   fseek(file, 0, SEEK_SET);
   char *text = malloc(size + 1);
   if (!text) { fclose(file); return 1; }
   text[fread(text, 1, size, file)] = 0;
   fclose(file);

   parseTvTiles(text, (int)size, &results);
   printf("%s: %d videos, continuation %s\n", path, results.count, results.continuation[0] ? "yes" : "no");
   for (int i = 0; i < results.count && i < 6; i++) {
      const SearchResult *item = &results.items[i];
      printf("  %-11s | %-48.48s | %-18.18s | %-10s | %-9s | %s%s\n", item->videoId, item->title, item->author,
             item->views, item->duration, item->published, item->isLive ? " LIVE" : "");
   }
   free(text);
   return results.count > 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
   testOAuth();
   testBodies();
   testSamples();
   printf("%d checks, %d failed\n", checks, failures);

   int unread = 0;
   if (argc > 2 && strcmp(argv[1], "feed") == 0)
      for (int i = 2; i < argc; i++) unread += parseFile(argv[i]);
   return failures || unread ? 1 : 0;
}
