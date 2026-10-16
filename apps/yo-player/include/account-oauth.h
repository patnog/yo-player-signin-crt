#pragma once

// account-oauth - the YouTube TV sign-in protocol (OAuth 2.0 device flow), as request builders and
// response readers with no i/o, so the host test can drive them against Google directly. account.c does
// the requests, the polling thread and the saved token.
//
// The flow: ask the device-code endpoint for a short user code, show it, and poll the token endpoint
// every `interval` seconds until the user approves it at the verification url on another device. That
// yields an access token (~1 day) and a refresh token (until revoked), which renews the access token.
//
// The client id and secret are the YouTube TV app's own, the same ones every TV build ships. They are
// not a credential of the user's: the user's consent is the code they approve.

#define OAUTH_CLIENT_ID     "861556708454-d6dlm3lh05idd8npek18k6be8ba3oc68.apps.googleusercontent.com"
#define OAUTH_CLIENT_SECRET "SboVhoG9s0rNafixCSGGKXAT"
#define OAUTH_SCOPE         "http://gdata.youtube.com https://www.googleapis.com/auth/youtube"

#define OAUTH_DEVICE_CODE_URL "https://www.youtube.com/o/oauth2/device/code"
#define OAUTH_TOKEN_URL       "https://oauth2.googleapis.com/token"
#define OAUTH_REVOKE_URL      "https://oauth2.googleapis.com/revoke"

#define OAUTH_ACCESS_TOKEN_MAX  1024
#define OAUTH_REFRESH_TOKEN_MAX 512

typedef struct {
   char deviceCode[256];        // secret: what the poll sends back; never shown
   char userCode[32];           // what the user types, e.g. "WHC-PWK-QFQR"
   char verificationUrl[96];    // where they type it, e.g. "https://www.google.com/device"
   int  expiresIn;              // seconds the code stays valid
   int  interval;               // seconds to wait between polls
} DeviceCode;

typedef struct {
   char accessToken[OAUTH_ACCESS_TOKEN_MAX];
   char refreshToken[OAUTH_REFRESH_TOKEN_MAX];   // only present on the first grant, not on a refresh
   int  expiresIn;                               // seconds the access token stays valid
} OAuthTokens;

typedef enum {
   OAUTH_OK,              // tokens filled
   OAUTH_PENDING,         // not approved yet: poll again after the interval
   OAUTH_SLOW_DOWN,       // polled too fast: poll again with a longer interval
   OAUTH_DENIED,          // the user refused on the consent screen
   OAUTH_EXPIRED,         // the code timed out before it was approved
   OAUTH_INVALID_GRANT,   // the refresh token was revoked or expired: the account must sign in again
   OAUTH_ERROR            // anything else (network, unreadable answer)
} OAuthResult;

// request bodies. each returns the body length, or -1 if it did not fit cap.
// deviceId is any stable random id for this console (the TV app sends a uuid).
int buildDeviceCodeBody(char *out, int cap, const char *deviceId);        // json, for OAUTH_DEVICE_CODE_URL
int buildTokenPollBody(char *out, int cap, const char *deviceCode);       // form, for OAUTH_TOKEN_URL
int buildRefreshBody(char *out, int cap, const char *refreshToken);       // form, for OAUTH_TOKEN_URL
int buildRevokeBody(char *out, int cap, const char *token);               // form, for OAUTH_REVOKE_URL

// 0 if resp held a usable device code, else -1.
int parseDeviceCode(const char *resp, int respLen, DeviceCode *out);

// reads a token endpoint answer (poll or refresh). out is cleared first.
OAuthResult parseTokenResponse(const char *resp, int respLen, OAuthTokens *out);
