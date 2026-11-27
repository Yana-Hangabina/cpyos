#pragma once

// Host stub: the prune executor logs progress and failures; the test does not need them,
// but it must compile with the same macro names the firmware uses.
inline void fakeLog(const char*, const char*, ...) {}
#define LOG_ERR(tag, ...) fakeLog(tag, __VA_ARGS__)
#define LOG_DBG(tag, ...) fakeLog(tag, __VA_ARGS__)
#define LOG_INF(tag, ...) fakeLog(tag, __VA_ARGS__)
