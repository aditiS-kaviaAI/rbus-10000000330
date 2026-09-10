#ifndef RBUS_DIAGNOSTICS_H
#define RBUS_DIAGNOSTICS_H

#include <stdint.h>

#include "../diagnostics-reporter/diagnostics_protocol.h"
#include <rbus.h>

/*
 * These helpers are internal to librbus.  They construct only fixed-size
 * protocol observations and deliberately do not retain request data.
 */

/** Returns the current monotonic clock value in microseconds. */
uint64_t rbusDiagnosticsCaptureStartUs(void);

/**
 * Maps an application-visible RBus result to the approved diagnostic outcome.
 * The original result is never modified by diagnostics collection.
 */
rbusDiagnosticsOutcome_t rbusDiagnosticsClassifyOutcome(rbusError_t result);

/**
 * Publishes one completed operation observation on a best-effort basis.
 * This call never waits for reporter acknowledgement or changes RBus results.
 */
void rbusDiagnosticsPublishCompleted(
    rbusDiagnosticsOperation_t operation,
    uint64_t startUs,
    rbusError_t result);

/**
 * Publishes one completed operation observation with a bounded normalized
 * identity. The identity is copied only into the fixed-size protocol record;
 * request and response payloads are never retained.
 */
void rbusDiagnosticsPublishCompletedForPath(
    rbusDiagnosticsOperation_t operation,
    uint64_t startUs,
    rbusError_t result,
    const char* path);

/* PUBLIC_INTERFACE */
/**
 * Publishes a successful provider lifecycle transition without changing RBus
 * registration behavior. Provider identity is bounded by the private protocol.
 */
void rbusDiagnosticsPublishProviderLifecycle(
    const char* provider,
    rbusDiagnosticsLifecycleAction_t action);

#endif
