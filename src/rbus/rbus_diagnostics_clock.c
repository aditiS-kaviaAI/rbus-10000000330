#include "rbus_diagnostics_collector.h"

#include <time.h>

static uint64_t (*g_test_clock)(void) = NULL;

/** Return the current monotonic timestamp in microseconds. */
uint64_t rbusDiagnosticsClock_NowMonotonicUs(void)
{
    struct timespec timestamp;

    if (g_test_clock != NULL)
    {
        return g_test_clock();
    }

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0)
    {
        return 0;
    }

    return ((uint64_t)timestamp.tv_sec * UINT64_C(1000000)) +
        ((uint64_t)timestamp.tv_nsec / UINT64_C(1000));
}

/** Override the clock for deterministic tests; a NULL callback restores production timing. */
void rbusDiagnosticsClock_SetTestClock(uint64_t (*clock_function)(void))
{
    g_test_clock = clock_function;
}
