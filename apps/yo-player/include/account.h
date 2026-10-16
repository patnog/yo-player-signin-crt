#pragma once

// account - the signed-in YouTube account: TV-style sign-in with a code, the saved refresh token, and a
// fresh access token for the account feeds (see account-oauth.h for the protocol).
//
// Signing in is optional. Signed out, the app behaves exactly as before. Signed in, the home screen gains
// the account's own feeds. Playback never uses the account - it stays on the anonymous player client.
//
// The refresh token is kept in /dev_hdd0/tmp/yo-player/account.txt. Anyone with that file can read the
// account's feeds until it is revoked (sign out here, or Google Account > Security > Third-party connections).

void initAccount(void);       // load a saved sign-in; call once at startup, after initStorage
void shutdownAccount(void);   // stop a sign-in in progress; call at app exit

int  isSignedIn(void);
// bumped whenever the account signs in or out (including a revoked token found on refresh). screens that
// cache account feeds compare it against the value they last loaded at.
int  getAccountRevision(void);

// a valid access token for an Authorization header, renewed when it is about to expire (or always, with
// forceRefresh - for a request that just came back 401). 0 ok; -1 when signed out or the renewal failed.
// blocking (it may make a request): call from a worker thread, not the frame loop.
int  getAccessToken(char *out, int cap, int forceRefresh);

void signOut(void);           // forget the saved token and revoke it with Google (best effort)

// ---- sign-in (runs on its own thread; the UI polls getSignInStatus each frame) ----

typedef enum {
   SIGNIN_IDLE,       // nothing started
   SIGNIN_STARTING,   // asking Google for a code
   SIGNIN_WAITING,    // code on screen; waiting for the user to approve it
   SIGNIN_DONE,       // approved and saved: isSignedIn() is now 1
   SIGNIN_DENIED,     // the user refused on the consent screen
   SIGNIN_EXPIRED,    // the code timed out
   SIGNIN_FAILED      // network or protocol error
} SignInStage;

typedef struct {
   SignInStage stage;
   char        userCode[32];          // valid in SIGNIN_WAITING
   char        verificationUrl[96];   // valid in SIGNIN_WAITING
   int         secondsLeft;           // until the code expires, in SIGNIN_WAITING
} SignInStatus;

void beginSignIn(void);                     // (re)start: fetches a code, then polls until approved
void cancelSignIn(void);                    // stop polling and return to SIGNIN_IDLE
void getSignInStatus(SignInStatus *out);
void updateSignIn(void);                    // frame-loop housekeeping: reaps the finished sign-in thread
