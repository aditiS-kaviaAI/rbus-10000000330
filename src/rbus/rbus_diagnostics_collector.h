#ifndef RBUS_DIAGNOSTICS_COLLECTOR_H
#define RBUS_DIAGNOSTICS_COLLECTOR_H

#include "rbus_diagnostics_private.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Return the current monotonic timestamp in microseconds. */
uint64_t rbusDiagnosticsClock_NowMonotonicUs(void);

/** Override the clock for deterministic tests; a NULL callback restores production timing. */
void rbusDiagnosticsClock_SetTestClock(uint64_t (*clock_function)(void));

/** Return whether runtime collection is enabled without exposing collector state. */
bool rbusDiagnosticsCollector_IsEnabled(void);

/** Record one completed metadata-only observation on a best-effort basis. */
void rbusDiagnosticsCollector_RecordCompleted(
    const rbus_diagnostics_observation_t* observation);

#ifdef __cplusplus
}
#endif

#endif
