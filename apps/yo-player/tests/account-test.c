// account-test - runs account.c (the sign-in thread, saved token and renewal) against a scripted Google.
// fetchHttp, readFile/writeFile/deleteFile and the threads are host stand-ins (see host-include/), so the
// state machine itself is the real PS3 code. Takes ~15 s: the poll waits are real, at the 1 s minimum.

#include "account.h"
#include "account-oauth.h"
#include "http.h"
#include "vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

int hostVerbose;
static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

// ---- the one-file store ----
static char     stored[512];
static int      storedLength = -1;   // -1 = file absent
int readFile(const char *path, char *buf, int cap)
{
   (void)path;
   if (storedLength < 0) return -1;
   int n = storedLength < cap - 1 ? storedLength : cap - 1;
   memcpy(buf, stored, n);
   buf[n] = 0;
   return n;
}
int writeFile(const char *path, const char *data, uint64_t len)
{
   (void)path;
   memcpy(stored, data, len);
   storedLength = (int)len;
   return 0;
}
int deleteFile(const char *path) { (void)path; storedLength = -1; return 0; }

// ---- the scripted google ----
// token endpoint answers come off a queue; every request is also counted by kind
#define MAX_SCRIPT 16
static const char     *tokenScript[MAX_SCRIPT];
static int             tokenScriptLength, tokenScriptNext;
static int             deviceCodeCalls, tokenCalls, revokeCalls;
static int             deviceCodeExpires = 1800;
static char            lastTokenBody[1024], lastRevokeBody[1024];
static pthread_mutex_t scriptLock = PTHREAD_MUTEX_INITIALIZER;

static void script(const char *first, ...)
{
   pthread_mutex_lock(&scriptLock);
   tokenScriptLength = tokenScriptNext = 0;
   const char *answer = first;
   __builtin_va_list args;
   __builtin_va_start(args, first);
   while (answer && tokenScriptLength < MAX_SCRIPT) { tokenScript[tokenScriptLength++] = answer; answer = __builtin_va_arg(args, const char *); }
   __builtin_va_end(args);
   pthread_mutex_unlock(&scriptLock);
}

static const char *PENDING = "{\"error\":\"authorization_pending\"}";
static const char *SLOW    = "{\"error\":\"slow_down\"}";
static const char *DENIED  = "{\"error\":\"access_denied\"}";
static const char *REVOKED = "{\"error\":\"invalid_grant\"}";
static const char *GRANT   = "{\"access_token\":\"AT1\",\"expires_in\":3600,\"refresh_token\":\"RT/1\"}";
static const char *RENEWED = "{\"access_token\":\"AT2\",\"expires_in\":3600}";
static const char *SHORT   = "{\"access_token\":\"ATS\",\"expires_in\":60}";   // inside the renew margin

int fetchHttp(const char *method, const char *url, const HttpHeader *headers, int headerCount,
              const void *body, int bodyLen, char *out, int cap, int *outLen, int *status)
{
   (void)method; (void)headers; (void)headerCount;
   const char *answer = "{}";
   char text[1024] = "";
   if (body && bodyLen > 0 && bodyLen < (int)sizeof text) { memcpy(text, body, bodyLen); text[bodyLen] = 0; }

   pthread_mutex_lock(&scriptLock);
   if (strstr(url, "/device/code")) {
      deviceCodeCalls++;
      static char deviceAnswer[256];
      snprintf(deviceAnswer, sizeof deviceAnswer, "{\"device_code\":\"DEV\",\"user_code\":\"ABC-DEF-GHIJ\",\"expires_in\":%d,"
               "\"interval\":1,\"verification_url\":\"https://www.google.com/device\"}", deviceCodeExpires);
      answer = deviceAnswer;
   } else if (strstr(url, "/revoke")) {
      revokeCalls++;
      snprintf(lastRevokeBody, sizeof lastRevokeBody, "%s", text);
   } else {
      tokenCalls++;
      snprintf(lastTokenBody, sizeof lastTokenBody, "%s", text);
      if (tokenScriptNext < tokenScriptLength) answer = tokenScript[tokenScriptNext++];
      else answer = PENDING;
   }
   pthread_mutex_unlock(&scriptLock);

   snprintf(out, cap, "%s", answer);
   *outLen = (int)strlen(out);
   *status = 200;
   return 0;
}

// ---- helpers ----
static void reset(void)
{
   script(NULL);
   deviceCodeCalls = tokenCalls = revokeCalls = 0;
   deviceCodeExpires = 1800;
   lastTokenBody[0] = lastRevokeBody[0] = 0;
}

static SignInStage waitForStage(SignInStage want, int timeoutMs)
{
   SignInStatus status;
   for (int waited = 0; waited < timeoutMs; waited += 50) {
      getSignInStatus(&status);
      if (status.stage == want) return status.stage;
      updateSignIn();
      usleep(50 * 1000);
   }
   getSignInStatus(&status);
   return status.stage;
}

static void signedOutStart(void)
{
   cancelSignIn();
   storedLength = -1;
   initAccount();   // reloads from the (empty) store: signed out
}

int main(void)
{
   hostVerbose = getenv("VERBOSE") != NULL;

   // a saved token means signed in at launch, with no network
   reset();
   memcpy(stored, "1//saved-refresh\n", 17); storedLength = 17;
   initAccount();
   CHECK(isSignedIn());
   CHECK(tokenCalls == 0);

   // ...and its first access token costs one refresh, whose body carries the (encoded) saved token
   char token[OAUTH_ACCESS_TOKEN_MAX];
   script(RENEWED, NULL);
   CHECK(getAccessToken(token, sizeof token, 0) == 0 && strcmp(token, "AT2") == 0);
   CHECK(strstr(lastTokenBody, "refresh_token=1%2F%2Fsaved-refresh&grant_type=refresh_token") != NULL);   // newline trimmed
   CHECK(tokenCalls == 1);
   CHECK(getAccessToken(token, sizeof token, 0) == 0 && tokenCalls == 1);   // still fresh: no new request
   script(RENEWED, NULL);
   CHECK(getAccessToken(token, sizeof token, 1) == 0 && tokenCalls == 2);   // forced renewal (a 401)

   // a token about to expire is renewed on the next ask
   script(SHORT, RENEWED, NULL);
   int before = tokenCalls;
   CHECK(getAccessToken(token, sizeof token, 1) == 0 && strcmp(token, "ATS") == 0);
   CHECK(getAccessToken(token, sizeof token, 0) == 0 && strcmp(token, "AT2") == 0 && tokenCalls == before + 2);

   // Google says the token was revoked: signed out, file gone, revision moved
   int revision = getAccountRevision();
   script(REVOKED, NULL);
   CHECK(getAccessToken(token, sizeof token, 1) == -1);
   CHECK(!isSignedIn() && storedLength == -1 && getAccountRevision() == revision + 1);
   CHECK(getAccessToken(token, sizeof token, 0) == -1);   // signed out: no request
   int callsAfter = tokenCalls;
   getAccessToken(token, sizeof token, 0);
   CHECK(tokenCalls == callsAfter);

   // sign-in: pending, then approved
   signedOutStart();
   reset();
   script(PENDING, GRANT, NULL);
   revision = getAccountRevision();
   beginSignIn();
   CHECK(waitForStage(SIGNIN_WAITING, 3000) == SIGNIN_WAITING);
   SignInStatus status;
   getSignInStatus(&status);
   CHECK(strcmp(status.userCode, "ABC-DEF-GHIJ") == 0 && strstr(status.verificationUrl, "google.com/device"));
   CHECK(status.secondsLeft > 1700 && status.secondsLeft <= 1800);
   CHECK(waitForStage(SIGNIN_DONE, 6000) == SIGNIN_DONE);
   CHECK(isSignedIn() && getAccountRevision() == revision + 1);
   CHECK(storedLength == 4 && memcmp(stored, "RT/1", 4) == 0);   // saved without a newline
   CHECK(strstr(lastTokenBody, "code=DEV&") != NULL);
   CHECK(tokenCalls == 2);
   updateSignIn();
   CHECK(getAccessToken(token, sizeof token, 0) == 0 && strcmp(token, "AT1") == 0 && tokenCalls == 2);   // grant's token reused

   // sign out: revoke goes out with the encoded token, and everything is forgotten
   revision = getAccountRevision();
   signOut();
   CHECK(!isSignedIn() && storedLength == -1 && getAccountRevision() == revision + 1);
   for (int i = 0; i < 40 && revokeCalls == 0; i++) usleep(50 * 1000);
   CHECK(revokeCalls == 1 && strcmp(lastRevokeBody, "token=RT%2F1") == 0);

   // denied
   signedOutStart();
   reset();
   script(DENIED, NULL);
   beginSignIn();
   CHECK(waitForStage(SIGNIN_DENIED, 4000) == SIGNIN_DENIED);
   CHECK(!isSignedIn());

   // expired by the clock
   signedOutStart();
   reset();
   deviceCodeExpires = 2;
   beginSignIn();
   CHECK(waitForStage(SIGNIN_EXPIRED, 6000) == SIGNIN_EXPIRED);
   CHECK(!isSignedIn());

   // slow_down: the next poll comes later, then it still completes
   signedOutStart();
   reset();
   script(SLOW, GRANT, NULL);
   beginSignIn();
   CHECK(waitForStage(SIGNIN_DONE, 12000) == SIGNIN_DONE && isSignedIn());
   signOut();
   for (int i = 0; i < 40 && revokeCalls == 0; i++) usleep(50 * 1000);

   // cancel while waiting returns to idle without signing in, and a retry starts clean
   signedOutStart();
   reset();
   beginSignIn();
   CHECK(waitForStage(SIGNIN_WAITING, 3000) == SIGNIN_WAITING);
   cancelSignIn();
   getSignInStatus(&status);
   CHECK(status.stage == SIGNIN_IDLE && !isSignedIn());
   script(GRANT, NULL);
   beginSignIn();
   CHECK(waitForStage(SIGNIN_DONE, 6000) == SIGNIN_DONE && isSignedIn());
   CHECK(deviceCodeCalls == 2);   // a fresh code for the retry

   shutdownAccount();
   printf("%d checks, %d failed\n", checks, failures);
   return failures ? 1 : 0;
}
