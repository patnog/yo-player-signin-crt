#pragma once
// host stand-in for simple-lib-core/http.h: the test supplies fetchHttp (a scripted Google).
typedef struct { const char *name; const char *value; } HttpHeader;
int fetchHttp(const char *method, const char *url, const HttpHeader *headers, int headerCount,
              const void *body, int bodyLen, char *out, int cap, int *outLen, int *status);
