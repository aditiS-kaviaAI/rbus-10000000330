#ifndef RBUS_DIAGNOSTICS_CLIENT_H
#define RBUS_DIAGNOSTICS_CLIENT_H

#include <stdbool.h>

#include "../diagnostics-reporter/diagnostics_protocol.h"

/**
 * Submits an observation through the local reporter datagram endpoint without
 * waiting for a response.  A failed submission is intentionally ignored by
 * the caller so it cannot interfere with normal RBus behavior.
 */
bool rbusDiagnosticsClientSubmit(
    const rbusDiagnosticsObservation_t* observation);

/**
 * Returns the process-local collection hint without I/O or allocation.
 * A false value keeps normal RBus operations off the diagnostics path.
 */
bool rbusDiagnosticsClientIsEnabled(void);

/*
 * Test-only controls keep diagnostics publisher tests independent from the
 * production runtime directory. They are internal to librbus and are not
 * installed as part of the public RBus interface.
 */

/* PUBLIC_INTERFACE */
/** Resets process-local publisher state before an isolated unit test. */
void rbusDiagnosticsClientResetForTest(void);

/* PUBLIC_INTERFACE */
/** Selects whether the test publisher should collect observations. */
void rbusDiagnosticsClientSetEnabledForTest(bool enabled);

/* PUBLIC_INTERFACE */
/** Overrides the datagram endpoint for an isolated unit test. */
bool rbusDiagnosticsClientSetEndpointForTest(const char* endpoint);

/* PUBLIC_INTERFACE */
/** Returns the cumulative local observation-loss count for a unit test. */
uint64_t rbusDiagnosticsClientGetDroppedObservationsForTest(void);

#endif
