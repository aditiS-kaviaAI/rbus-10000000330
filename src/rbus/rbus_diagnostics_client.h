#ifndef RBUS_DIAGNOSTICS_CLIENT_H
#define RBUS_DIAGNOSTICS_CLIENT_H

#include "rbus_diagnostics.h"

/**
 * Best-effort delivery of one completed, compact RBus observation.
 *
 * This function never changes an RBus result, retries synchronously, waits for
 * a reporter acknowledgement, or logs application payloads.
 */
void rbusDiagnostics_Publish(
    const rbusDiagnosticsTimer_t* timer,
    rbusDiagnosticsOperation_t operation,
    const char* path,
    rbusError_t result);

/**
 * Best-effort publication of a verified provider registration state change.
 *
 * This private helper is called only once the registration transaction or all
 * requested removals have completed successfully. It never affects the
 * caller's RBus result or waits for reporter acknowledgement.
 */
void rbusDiagnostics_PublishProviderLifecycle(
    const char* provider,
    bool registered);

#ifdef RBUS_DIAGNOSTICS_TESTING
/**
 * Test-only observer for the internal final-observation boundary.
 *
 * Focused tests install this observer to assert the operation, final result,
 * and ordering of the call made by rbus.c before exercising socket delivery.
 * It is intentionally unavailable from public RBus headers and production
 * builds.
 */
typedef void (*rbusDiagnosticsTestPublishHook_t)(
    rbusDiagnosticsOperation_t operation,
    const char* path,
    rbusError_t result);

void rbusDiagnostics_SetTestPublishHook(
    rbusDiagnosticsTestPublishHook_t hook);

/**
 * Test-only observer for verified provider lifecycle publication.
 *
 * Focused tests use this private hook to distinguish a complete lifecycle
 * transition from a failed registration or incomplete removal. It is excluded
 * from public headers and production builds.
 */
typedef void (*rbusDiagnosticsTestLifecycleHook_t)(
    const char* provider,
    bool registered);

void rbusDiagnostics_SetTestLifecycleHook(
    rbusDiagnosticsTestLifecycleHook_t hook);

/**
 * Forces only the asynchronous method worker-scheduling decision to fail.
 *
 * This process-local control exists exclusively in diagnostics test builds so
 * tests can validate the existing submission-failure terminal boundary without
 * depending on host thread-resource exhaustion.
 */
void rbusDiagnostics_SetTestAsyncSchedulingFailure(bool enabled);

/**
 * Returns whether the diagnostics test build must bypass pthread_create.
 *
 * The function is private to the RBus diagnostics test seam and is never
 * compiled into production RBus builds.
 */
bool rbusDiagnostics_ShouldFailAsyncSchedulingForTest(void);

/**
 * Forces one provider-element core-removal result to fail in diagnostics tests.
 *
 * This process-local control lets focused lifecycle coverage enter the
 * existing incomplete-removal branch without relying on broker-specific
 * handling of nonexistent element names. It is unavailable in public headers
 * and production builds.
 */
void rbusDiagnostics_SetTestRemovalFailure(bool enabled);

/**
 * Returns whether the next diagnostics test-build core removal must fail.
 *
 * A true result consumes the configured one-shot injection so subsequent
 * removals execute through the ordinary core implementation.
 */
bool rbusDiagnostics_ShouldFailRemovalForTest(void);
#endif

#endif
