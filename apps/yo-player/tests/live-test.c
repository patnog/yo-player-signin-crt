// live-test - runs the real sign-in request builders and feed reader against Google, from a PC.
// Not part of run.sh (it needs the network and an account). Uses curl for transport, since the PS3's
// http layer is not available here; everything else is the code the PS3 runs.
//
//   live-test refresh  <refresh_token>            renew an access token, print only its length
//   live-test feed     <refresh_token> <feed 0-3> read a feed through the TV client, print titles
//
// The refresh token comes from the command line, never from this file, and no token is printed.

#include "account-oauth.h"
#include "youtube-tv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SearchResults results;

// POST body to url with extra curl args; the answer lands in out. curl reads the body from a file so it
// never shows up in a process listing.
static int post(const char *url, const char *contentType, const char *body, const char *extra, char *out, int cap)
{
   char path[] = "/tmp/yo-live-body-XXXXXX";
   int fd = mkstemp(path);
   if (fd < 0) return -1;
   FILE *file = fdopen(fd, "w");
   fputs(body, file);
   fclose(file);

   char command[2048];
   snprintf(command, sizeof command, "curl -s -X POST '%s' -H 'Content-Type: %s' %s --data-binary @%s", url, contentType, extra, path);
   FILE *pipe = popen(command, "r");
   int length = 0;
   if (pipe) { length = (int)fread(out, 1, cap - 1, pipe); pclose(pipe); }
   out[length] = 0;
   remove(path);
   return length;
}

static int refresh(const char *refreshToken, OAuthTokens *tokens)
{
   static char resp[8192];
   char body[1024];
   int bodyLen = buildRefreshBody(body, sizeof body, refreshToken);
   if (bodyLen < 0) return -1;
   int length = post(OAUTH_TOKEN_URL, "application/x-www-form-urlencoded", body, "", resp, sizeof resp);
   OAuthResult result = parseTokenResponse(resp, length, tokens);
   printf("refresh -> result %d, access token %d chars, expires_in %d\n", (int)result, (int)strlen(tokens->accessToken), tokens->expiresIn);
   return result == OAUTH_OK ? 0 : -1;
}

int main(int argc, char **argv)
{
   if (argc < 3) { fprintf(stderr, "usage: live-test refresh|feed <refresh_token> [feed]\n"); return 2; }
   OAuthTokens tokens;
   if (refresh(argv[2], &tokens) != 0) return 1;
   if (strcmp(argv[1], "refresh") == 0) return 0;

   AccountFeed feed = (AccountFeed)atoi(argc > 3 ? argv[3] : "0");
   static char body[MAX_CONTINUATION + 512]; char extra[1600];
   int bodyLen = buildTvBrowseBody(body, sizeof body, feed, NULL);
   if (bodyLen < 0) return 1;
   snprintf(extra, sizeof extra, "-H 'Authorization: Bearer %s' -H 'User-Agent: %s' -H 'X-YouTube-Client-Name: %s' -H 'X-YouTube-Client-Version: %s'",
            tokens.accessToken, TV_USER_AGENT, TV_CLIENT_NAME_ID, TV_CLIENT_VERSION);

   static char resp[2 * 1024 * 1024];
   int length = post("https://www.youtube.com/youtubei/v1/browse?prettyPrint=false", "application/json", body, extra, resp, sizeof resp);
   parseTvTiles(resp, length, &results);
   printf("feed %d -> %d bytes, %d videos, continuation %s\n", (int)feed, length, results.count, results.continuation[0] ? "yes" : "no");

   if (results.continuation[0]) {   // and one more page, to prove the token pages
      static SearchResults page1;
      page1 = results;
      bodyLen = buildTvBrowseBody(body, sizeof body, feed, page1.continuation);
      if (bodyLen < 0) { printf("continuation body did not fit\n"); return 1; }
      length = post("https://www.youtube.com/youtubei/v1/browse?prettyPrint=false", "application/json", body, extra, resp, sizeof resp);
      parseTvTiles(resp, length, &results);
      printf("page 2 -> %d bytes, %d videos, first: %.60s\n", length, results.count, results.count ? results.items[0].title : "");
      results = page1;
   }
   for (int i = 0; i < results.count && i < 5; i++) printf("  %s | %.60s\n", results.items[i].videoId, results.items[i].title);
   return results.count > 0 ? 0 : 1;
}
