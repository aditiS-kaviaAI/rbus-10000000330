#ifndef RBUS_DIAGNOSTICS_H
#define RBUS_DIAGNOSTICS_H

#include <stdint.h>
#include <time.h>

#include <rbus.h>

/*
 * This header is private to the rbus library.  It deliberately excludes
 * request and response values so diagnostics cannot retain application data.
 */
typedef enum
{
    RBUS_DIAGNOSTICS_OPERATION_GET = 0,
    RBUS_DIAGNOSTICS_OPERATION_SET = 1,
    RBUS_DIAGNOSTICS_OPERATION_METHOD = 2
} rbusDiagnosticsOperation_t;

typedef enum
{
    RBUS_DIAGNOSTICS_OUTCOME_SUCCESS = 0,
    RBUS_DIAGNOSTICS_OUTCOME_TIMEOUT = 1,
    RBUS_DIAGNOSTICS_OUTCOME_PERMISSION_DENIED = 2,
    RBUS_DIAGNOSTICS_OUTCOME_INVALID_REQUEST = 3,
    RBUS_DIAGNOSTICS_OUTCOME_INVALID_RESPONSE = 4,
    RBUS_DIAGNOSTICS_OUTCOME_PROVIDER_UNAVAILABLE = 5,
    RBUS_DIAGNOSTICS_OUTCOME_UNKNOWN_ERROR = 6
} rbusDiagnosticsOutcome_t;

typedef struct
{
    struct timespec monotonicStart;
    int enabled;
} rbusDiagnosticsTimer_t;

/**
 * Starts timing only when diagnostics support is compiled into this library.
 */
static inline rbusDiagnosticsTimer_t rbusDiagnostics_TimerStart(void)
{
    rbusDiagnosticsTimer_t timer = {{0, 0}, 0};

#ifdef ENABLE_RBUS_DIAGNOSTICS
    timer.enabled = (clock_gettime(CLOCK_MONOTONIC, &timer.monotonicStart) == 0);
#endif
    return timer;
}

/**
 * Returns a saturating elapsed duration in microseconds.
 */
static inline uint64_t rbusDiagnostics_TimerElapsedMicroseconds(
    const rbusDiagnosticsTimer_t* timer)
{
    struct timespec end;
    uint64_t seconds;
    uint64_t nanoseconds;

    if (timer == NULL || !timer->enabled ||
        clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        return 0;
    }

    if (end.tv_nsec < timer->monotonicStart.tv_nsec) {
        --end.tv_sec;
        end.tv_nsec += 1000000000L;
    }

    if (end.tv_sec < timer->monotonicStart.tv_sec) {
        return 0;
    }

    seconds = (uint64_t)(end.tv_sec - timer->monotonicStart.tv_sec);
    nanoseconds = (uint64_t)(end.tv_nsec - timer->monotonicStart.tv_nsec);
    if (seconds > (UINT64_MAX - (nanoseconds / 1000U)) / 1000000U) {
        return UINT64_MAX;
    }

    return (seconds * 1000000U) + (nanoseconds / 1000U);
}

/**
 * Maps an unmodified RBus terminal result to its diagnostics-only category.
 */
static inline rbusDiagnosticsOutcome_t rbusDiagnostics_OutcomeFromError(
    rbusError_t error)
{
    switch (error) {
        case RBUS_ERROR_SUCCESS:
            return RBUS_DIAGNOSTICS_OUTCOME_SUCCESS;
        case RBUS_ERROR_TIMEOUT:
            return RBUS_DIAGNOSTICS_OUTCOME_TIMEOUT;
        case RBUS_ERROR_ACCESS_NOT_ALLOWED:
        case RBUS_ERROR_NOT_WRITABLE:
        case RBUS_ERROR_NOT_READABLE:
            return RBUS_DIAGNOSTICS_OUTCOME_PERMISSION_DENIED;
        case RBUS_ERROR_INVALID_INPUT:
        case RBUS_ERROR_INVALID_OPERATION:
        case RBUS_ERROR_INVALID_EVENT:
        case RBUS_ERROR_INVALID_HANDLE:
        case RBUS_ERROR_INVALID_METHOD:
        case RBUS_ERROR_INVALID_NAMESPACE:
        case RBUS_ERROR_INVALID_PARAMETER_TYPE:
        case RBUS_ERROR_INVALID_PARAMETER_VALUE:
            return RBUS_DIAGNOSTICS_OUTCOME_INVALID_REQUEST;
        case RBUS_ERROR_INVALID_RESPONSE_FROM_DESTINATION:
            return RBUS_DIAGNOSTICS_OUTCOME_INVALID_RESPONSE;
        default:
            return RBUS_DIAGNOSTICS_OUTCOME_UNKNOWN_ERROR;
    }
}

#endif
