#pragma once

#include <stdio.h>

/*
 * Minimal shared test harness: CHECK records failures without aborting, and
 * FLOW_TEST_RESULT reports and converts them to a process exit code.
 */

static int g_flow_test_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            g_flow_test_failures++;                                          \
        }                                                                    \
    } while (0)

#define FLOW_TEST_RESULT(name)                                                     \
    (g_flow_test_failures                                                          \
         ? (fprintf(stderr, "%s: %d check(s) failed\n", name, g_flow_test_failures), 1) \
         : (printf("%s: all passed\n", name), 0))
