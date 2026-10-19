#pragma once
// host stand-in for simple-lib-core/dbg.h
#include <stdio.h>
extern int hostVerbose;
#define logInfo(...)  do { if (hostVerbose) printf(__VA_ARGS__); } while (0)
#define logError(...) do { if (hostVerbose) printf(__VA_ARGS__); } while (0)
