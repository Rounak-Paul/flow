#pragma once

#include <stdint.h>

/* Broken-down local wall-clock time. */
typedef struct Fl_WallTime {
    int16_t year;
    uint8_t month;        /* 1..12  */
    uint8_t day;          /* 1..31  */
    uint8_t hour;         /* 0..23  */
    uint8_t minute;       /* 0..59  */
    uint8_t second;       /* 0..60  */
    uint16_t millisecond; /* 0..999 */
} Fl_WallTime;

/*
 * Monotonic time since an unspecified epoch, unaffected by wall-clock changes.
 *
 * Returns  Nanoseconds; never decreases within a process.
 */
uint64_t fl_clock_monotonic_ns(void);

/*
 * Current local wall-clock time, for human-facing timestamps only. Engine
 * evaluation must never depend on it.
 *
 * Returns  Broken-down local time; all fields zero if the OS query fails.
 */
Fl_WallTime fl_clock_wall_now(void);
