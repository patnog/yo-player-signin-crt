#pragma once
// host stand-in for the SDK's <cell/rtc.h>: just the names string-utilities.h mentions, so the
// platform-free sources can compile on a PC. nothing here is ever called by the tests.
#include <stdint.h>
#include <time.h>

typedef struct { uint16_t year, month, day, hour, minute, second; uint32_t microsecond; } CellRtcDateTime;
typedef struct { uint64_t tick; } CellRtcTick;

int cellRtcSetTime_t(CellRtcDateTime *dateTime, time_t unixTime);
int cellRtcGetTick(const CellRtcDateTime *dateTime, CellRtcTick *tick);
int cellRtcSetTick(CellRtcDateTime *dateTime, const CellRtcTick *tick);
int cellRtcConvertUtcToLocalTime(const CellRtcTick *utc, CellRtcTick *local);
