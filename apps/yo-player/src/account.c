// account - signed-in YouTube account state (see account.h).

#include "account.h"
#include "account-oauth.h"
#include "http.h"               // fetchHttp
#include "vfs.h"                // readFile / writeFile / deleteFile
#include "thread.h"             // lwmutex, spawnJoinableThread, sleepMs, getTimeUs
#include "string-utilities.h"   // strCopy
#include "dbg.h"                // logInfo / logError (never the tokens themselves)

#include <stdio.h>
#include <string.h>

#define ACCOUNT_PATH      "/dev_hdd0/tmp/yo-player/account.txt"
#define RESP_CAP          4096
#define BODY_CAP          1024
#define RENEW_MARGIN_US   (5 * 60 * 1000000ULL)   // renew this long before the access token runs out
#define POLL_SLICE_MS     250                      // how often a waiting poll checks for cancel

static sys_lwmutex_t accountLock;   // guards everything below except the volatile flags
static int           lockReady;

static char     refreshToken[OAUTH_REFRESH_TOKEN_MAX];
static char     accessToken[OAUTH_ACCESS_TOKEN_MAX];
static uint64_t accessExpiresUs;    // getTimeUs() at which accessToken stops working; 0 = none held

static volatile int signedIn;
static volatile int revision;

static SignInStatus     status;            // what getSignInStatus reports
static sys_ppu_thread_t signInTid;
static int              signInActive;      // a sign-in thread exists and has not been joined
static volatile int     signInFinished;    // that thread has returned
static volatile int     signInCancel;      // asks it to stop

static const HttpHeader FORM_HEADERS[] = { { "Content-Type", "application/x-www-form-urlencoded" } };
static const HttpHeader JSON_HEADERS[] = { { "Content-Type", "application/json" } };

// POST body to url, then read the answer as a token response. the token endpoint answers a pending poll
// with a 4xx and an error member, so the body is read whatever the status was.
static OAuthResult postForTokens(const char *body, int bodyLen, OAuthTokens *tokens)
{
   char resp[RESP_CAP];
   int respLen = 0, httpStatus = 0;
   if (fetchHttp("POST", OAUTH_TOKEN_URL, FORM_HEADERS, 1, body, bodyLen, resp, sizeof resp, &respLen, &httpStatus) != 0) {
      logError("[account] token request failed (status %d)\n", httpStatus);
      return OAUTH_ERROR;
   }
   return parseTokenResponse(resp, respLen, tokens);
}

// adopt a new access token (and refresh token, when the grant carried one). caller holds the lock.
static void storeTokens(const OAuthTokens *tokens)
{
   strCopy(accessToken, sizeof accessToken, tokens->accessToken);
   int lifetime = tokens->expiresIn > 0 ? tokens->expiresIn : 3600;
   accessExpiresUs = getTimeUs() + (uint64_t)lifetime * 1000000ULL;
   if (tokens->refreshToken[0]) strCopy(refreshToken, sizeof refreshToken, tokens->refreshToken);
}

// drop every token held. caller holds the lock.
static void forgetTokens(void)
{
   memset(refreshToken, 0, sizeof refreshToken);
   memset(accessToken, 0, sizeof accessToken);
   accessExpiresUs = 0;
   deleteFile(ACCOUNT_PATH);
   if (signedIn) { signedIn = 0; revision++; }
}

void initAccount(void)
{
   if (createLock(&accountLock) == 0) lockReady = 1;
   status.stage = SIGNIN_IDLE;

   int length = readFile(ACCOUNT_PATH, refreshToken, sizeof refreshToken);
   while (length > 0 && (refreshToken[length - 1] == '\n' || refreshToken[length - 1] == '\r')) refreshToken[--length] = 0;
   signedIn = length > 0;
   logInfo("[account] %s\n", signedIn ? "signed in (saved token)" : "signed out");
}

int isSignedIn(void)         { return signedIn; }
int getAccountRevision(void) { return revision; }

int getAccessToken(char *out, int cap, int forceRefresh)
{
   if (!lockReady || !signedIn) return -1;
   lock(&accountLock);

   int ok = 0;
   if (!refreshToken[0]) ok = -1;
   else if (forceRefresh || !accessToken[0] || getTimeUs() + RENEW_MARGIN_US >= accessExpiresUs) {
      char body[BODY_CAP];
      int bodyLen = buildRefreshBody(body, sizeof body, refreshToken);
      OAuthTokens tokens;
      OAuthResult result = bodyLen < 0 ? OAUTH_ERROR : postForTokens(body, bodyLen, &tokens);
      if (result == OAUTH_OK) storeTokens(&tokens);
      else if (result == OAUTH_INVALID_GRANT) { logInfo("[account] token revoked; signed out\n"); forgetTokens(); ok = -1; }
      else ok = -1;   // network trouble: stay signed in and try again on the next request
   }
   if (ok == 0) strCopy(out, cap, accessToken);

   unlock(&accountLock);
   return ok;
}

// revoking is a courtesy to the user's account page, so a failure only gets logged
static void revokeWorker(uint64_t arg)
{
   char body[BODY_CAP];
   int bodyLen = buildRevokeBody(body, sizeof body, (const char *)(uintptr_t)arg);
   char resp[256];
   int respLen = 0, httpStatus = 0;
   if (bodyLen > 0)
      fetchHttp("POST", OAUTH_REVOKE_URL, FORM_HEADERS, 1, body, bodyLen, resp, sizeof resp, &respLen, &httpStatus);
   logInfo("[account] revoke status %d\n", httpStatus);
   exitThread();
}

void signOut(void)
{
   if (!lockReady) return;
   cancelSignIn();
   static char revoking[OAUTH_REFRESH_TOKEN_MAX];   // static: the revoke thread reads it after we return
   lock(&accountLock);
   strCopy(revoking, sizeof revoking, refreshToken);
   forgetTokens();
   status.stage = SIGNIN_IDLE;
   unlock(&accountLock);

   sys_ppu_thread_t tid;
   if (revoking[0]) spawnThread(&tid, revokeWorker, (uint64_t)(uintptr_t)revoking, THREAD_PRIORITY_LOW, THREAD_STACK_SIZE_64KB, "yt-revoke");
   logInfo("[account] signed out\n");
}

// ---- sign-in thread ----

static void setStage(SignInStage stage)
{
   lock(&accountLock);
   status.stage = stage;
   unlock(&accountLock);
}

// any unique id serves: the TV app sends a uuid, and Google only uses it to name the device
static void makeDeviceId(char *out, int cap)
{
   uint64_t seed = getTimeUs() ^ 0x9E3779B97F4A7C15ULL;
   snprintf(out, cap, "%08x-%04x-4%03x-a%03x-%012llx",
            (unsigned)(seed >> 32), (unsigned)(seed >> 16) & 0xFFFF, (unsigned)seed & 0xFFF,
            (unsigned)(seed >> 20) & 0xFFF, (unsigned long long)(seed * 0x2545F4914F6CDD1DULL) & 0xFFFFFFFFFFFFULL);
}

static void signInWorker(uint64_t arg)
{
   (void)arg;
   static char resp[RESP_CAP];
   char body[BODY_CAP], deviceId[40];
   DeviceCode code;

   makeDeviceId(deviceId, sizeof deviceId);
   int bodyLen = buildDeviceCodeBody(body, sizeof body, deviceId);
   int respLen = 0, httpStatus = 0;
   if (bodyLen < 0 ||
       fetchHttp("POST", OAUTH_DEVICE_CODE_URL, JSON_HEADERS, 1, body, bodyLen, resp, sizeof resp, &respLen, &httpStatus) != 0 ||
       parseDeviceCode(resp, respLen, &code) != 0) {
      logError("[account] no sign-in code (status %d)\n", httpStatus);
      setStage(SIGNIN_FAILED);
      goto done;
   }

   uint64_t deadlineUs = getTimeUs() + (uint64_t)code.expiresIn * 1000000ULL;
   int intervalMs = code.interval * 1000;
   lock(&accountLock);
   strCopy(status.userCode, sizeof status.userCode, code.userCode);
   strCopy(status.verificationUrl, sizeof status.verificationUrl, code.verificationUrl);
   status.secondsLeft = code.expiresIn;
   status.stage = SIGNIN_WAITING;
   unlock(&accountLock);

   bodyLen = buildTokenPollBody(body, sizeof body, code.deviceCode);
   for (;;) {
      for (int waited = 0; waited < intervalMs && !signInCancel; waited += POLL_SLICE_MS) {
         sleepMs(POLL_SLICE_MS);
         uint64_t now = getTimeUs();
         lock(&accountLock);
         status.secondsLeft = now < deadlineUs ? (int)((deadlineUs - now) / 1000000ULL) : 0;
         unlock(&accountLock);
      }
      if (signInCancel) goto done;
      if (getTimeUs() >= deadlineUs) { setStage(SIGNIN_EXPIRED); goto done; }

      OAuthTokens tokens;
      OAuthResult result = bodyLen < 0 ? OAUTH_ERROR : postForTokens(body, bodyLen, &tokens);
      if (result == OAUTH_PENDING)   continue;
      if (result == OAUTH_SLOW_DOWN) { intervalMs += 5000; continue; }
      if (result == OAUTH_ERROR)     continue;   // a dropped request: keep polling until the code expires
      if (result == OAUTH_DENIED)    { setStage(SIGNIN_DENIED);  goto done; }
      if (result == OAUTH_EXPIRED)   { setStage(SIGNIN_EXPIRED); goto done; }
      if (result != OAUTH_OK || !tokens.refreshToken[0]) { setStage(SIGNIN_FAILED); goto done; }

      if (writeFile(ACCOUNT_PATH, tokens.refreshToken, strlen(tokens.refreshToken)) != 0)
         logError("[account] could not save the sign-in; it lasts until the app closes\n");
      lock(&accountLock);
      storeTokens(&tokens);
      signedIn = 1;
      revision++;
      status.stage = SIGNIN_DONE;
      unlock(&accountLock);
      logInfo("[account] signed in\n");
      goto done;
   }

done:
   __sync_synchronize();
   signInFinished = 1;
   exitThread();
}

static void reapSignIn(void)
{
   if (signInActive) { joinThread(signInTid); signInActive = 0; }
}

void beginSignIn(void)
{
   if (!lockReady) return;
   cancelSignIn();
   lock(&accountLock);
   memset(&status, 0, sizeof status);
   status.stage = SIGNIN_STARTING;
   unlock(&accountLock);

   signInCancel = 0;
   signInFinished = 0;
   signInActive = spawnJoinableThread(&signInTid, signInWorker, 0, THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-signin") == 0;
   if (!signInActive) setStage(SIGNIN_FAILED);
}

// joins the thread, which can take up to one poll request: the wait between polls checks the flag every
// POLL_SLICE_MS, but a request already in flight runs to its end.
void cancelSignIn(void)
{
   if (!lockReady) return;
   signInCancel = 1;
   reapSignIn();
   lock(&accountLock);
   if (status.stage == SIGNIN_STARTING || status.stage == SIGNIN_WAITING) status.stage = SIGNIN_IDLE;
   unlock(&accountLock);
}

void getSignInStatus(SignInStatus *out)
{
   if (!lockReady) { memset(out, 0, sizeof *out); return; }
   lock(&accountLock);
   *out = status;
   unlock(&accountLock);
}

void updateSignIn(void)
{
   if (signInActive && signInFinished) reapSignIn();
}

void shutdownAccount(void)
{
   cancelSignIn();
}
