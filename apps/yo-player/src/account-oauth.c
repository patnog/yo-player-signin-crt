// account-oauth - YouTube TV device-flow requests and answers (see account-oauth.h).

#include "account-oauth.h"
#include "json.h"               // readJsonObject, getJsonText, getJsonNumber
#include "string-utilities.h"   // strCopy

#include <string.h>
#include <stdio.h>

// percent-encode a form value. tokens are mostly url-safe already, but a refresh token carries '/'
// and nothing promises the rest stays inside the unreserved set.
static int appendFormValue(char *out, int at, int cap, const char *value)
{
   static const char HEX[] = "0123456789ABCDEF";
   for (; *value; value++) {
      unsigned char c = (unsigned char)*value;
      int plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_' || c == '.' || c == '~';
      if (at + (plain ? 1 : 3) >= cap) return -1;
      if (plain) out[at++] = (char)c;
      else { out[at++] = '%'; out[at++] = HEX[c >> 4]; out[at++] = HEX[c & 15]; }
   }
   out[at] = 0;
   return at;
}

// "<prefix><encoded value><suffix>", or -1 if it did not fit
static int buildForm(char *out, int cap, const char *prefix, const char *value, const char *suffix)
{
   int at = snprintf(out, cap, "%s", prefix);
   if (at < 0 || at >= cap) return -1;
   at = appendFormValue(out, at, cap, value);
   if (at < 0) return -1;
   int tail = snprintf(out + at, cap - at, "%s", suffix);
   return (tail < 0 || tail >= cap - at) ? -1 : at + tail;
}

int buildDeviceCodeBody(char *out, int cap, const char *deviceId)
{
   int length = snprintf(out, cap,
      "{\"client_id\":\"" OAUTH_CLIENT_ID "\",\"scope\":\"" OAUTH_SCOPE "\","
      "\"device_id\":\"%s\",\"device_model\":\"ytlr::\"}", deviceId);
   return (length < 0 || length >= cap) ? -1 : length;
}

int buildTokenPollBody(char *out, int cap, const char *deviceCode)
{
   return buildForm(out, cap,
      "client_id=" OAUTH_CLIENT_ID "&client_secret=" OAUTH_CLIENT_SECRET "&code=", deviceCode,
      "&grant_type=http%3A%2F%2Foauth.net%2Fgrant_type%2Fdevice%2F1.0");
}

int buildRefreshBody(char *out, int cap, const char *refreshToken)
{
   return buildForm(out, cap,
      "client_id=" OAUTH_CLIENT_ID "&client_secret=" OAUTH_CLIENT_SECRET "&refresh_token=", refreshToken,
      "&grant_type=refresh_token");
}

int buildRevokeBody(char *out, int cap, const char *token)
{
   return buildForm(out, cap, "token=", token, "");
}

// the members of the answer's top-level object; 0 and its inner bounds, or -1
static int topObject(const char *resp, int respLen, const char **inner, int *innerLen)
{
   int start = 0, end = 0;
   if (readJsonObject(resp, respLen, 0, &start, &end) == 0) return -1;
   *inner = resp + start;
   *innerLen = end - start;
   return 0;
}

int parseDeviceCode(const char *resp, int respLen, DeviceCode *out)
{
   memset(out, 0, sizeof *out);
   const char *object;
   int length;
   if (topObject(resp, respLen, &object, &length) != 0) return -1;

   getJsonText(object, length, "device_code", out->deviceCode, sizeof out->deviceCode);
   getJsonText(object, length, "user_code", out->userCode, sizeof out->userCode);
   getJsonText(object, length, "verification_url", out->verificationUrl, sizeof out->verificationUrl);
   out->expiresIn = (int)getJsonNumber(object, length, "expires_in");
   out->interval  = (int)getJsonNumber(object, length, "interval");
   if (out->interval <= 0) out->interval = 5;   // the spec's default when the server leaves it out
   if (!out->verificationUrl[0]) strCopy(out->verificationUrl, sizeof out->verificationUrl, "https://www.google.com/device");

   return (out->deviceCode[0] && out->userCode[0] && out->expiresIn > 0) ? 0 : -1;
}

OAuthResult parseTokenResponse(const char *resp, int respLen, OAuthTokens *out)
{
   memset(out, 0, sizeof *out);
   const char *object;
   int length;
   if (topObject(resp, respLen, &object, &length) != 0) return OAUTH_ERROR;

   char error[48] = "";
   if (getJsonText(object, length, "error", error, sizeof error) == 0 && error[0]) {
      if (strcmp(error, "authorization_pending") == 0) return OAUTH_PENDING;
      if (strcmp(error, "slow_down") == 0)             return OAUTH_SLOW_DOWN;
      if (strcmp(error, "access_denied") == 0)         return OAUTH_DENIED;
      if (strcmp(error, "expired_token") == 0)         return OAUTH_EXPIRED;
      if (strcmp(error, "invalid_grant") == 0)         return OAUTH_INVALID_GRANT;
      return OAUTH_ERROR;
   }

   getJsonText(object, length, "access_token", out->accessToken, sizeof out->accessToken);
   getJsonText(object, length, "refresh_token", out->refreshToken, sizeof out->refreshToken);
   out->expiresIn = (int)getJsonNumber(object, length, "expires_in");
   return out->accessToken[0] ? OAUTH_OK : OAUTH_ERROR;
}
