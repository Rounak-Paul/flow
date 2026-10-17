#include "fl_clock.h"

#if defined(_WIN32)

#include <windows.h>

uint64_t fl_clock_monotonic_ns(void)
{
    static LARGE_INTEGER frequency;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    const uint64_t ticks = (uint64_t)counter.QuadPart;
    const uint64_t freq = (uint64_t)frequency.QuadPart;
    return (ticks / freq) * 1000000000ull + ((ticks % freq) * 1000000000ull) / freq;
}

Fl_WallTime fl_clock_wall_now(void)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    return (Fl_WallTime){
        .year = (int16_t)st.wYear, .month = (uint8_t)st.wMonth, .day = (uint8_t)st.wDay,
        .hour = (uint8_t)st.wHour, .minute = (uint8_t)st.wMinute, .second = (uint8_t)st.wSecond,
        .millisecond = (uint16_t)st.wMilliseconds,
    };
}

#else

#include <time.h>

uint64_t fl_clock_monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

Fl_WallTime fl_clock_wall_now(void)
{
    struct timespec ts;
    struct tm local;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || !localtime_r(&ts.tv_sec, &local))
        return (Fl_WallTime){0};
    return (Fl_WallTime){
        .year = (int16_t)(local.tm_year + 1900), .month = (uint8_t)(local.tm_mon + 1),
        .day = (uint8_t)local.tm_mday, .hour = (uint8_t)local.tm_hour,
        .minute = (uint8_t)local.tm_min, .second = (uint8_t)local.tm_sec,
        .millisecond = (uint16_t)(ts.tv_nsec / 1000000),
    };
}

#endif
