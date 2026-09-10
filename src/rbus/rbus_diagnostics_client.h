#ifndef RBUS_DIAGNOSTICS_CLIENT_H
#define RBUS_DIAGNOSTICS_CLIENT_H

#include <stdint.h>

#include <rbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * This private protocol intentionally contains operation metadata only. It never
 * serializes RBus request values, response values, or raw trace-context values.
 */
#define RBUS_DIAGNOSTICS_PROTOCOL_VERSION 1U
#define RBUS_DIAGNOSTICS_NAME_MAX 256U
#define RBUS_DIAGNOSTICS_COMPONENT_MAX 128U
#define RBUS_DIAGNOSTICS_SOCKET_PATH_MAX 108U

typedef enum {
    RBUS_DIAGNOSTICS_OPERATION_GET = 1,
    RBUS_DIAGNOSTICS_OPERATION_SET,
    RBUS_DIAGNOSTICS_OPERATION_METHOD
} rbusDiagnosticsOperation_t;

typedef enum {
    RBUS_DIAGNOSTICS_OUTCOME_SUCCESS = 0,
    RBUS_DIAGNOSTICS_OUTCOME_TIMEOUT,
    RBUS_DIAGNOSTICS_OUTCOME_PROVIDER_UNAVAILABLE,
    RBUS_DIAGNOSTICS_OUTCOME_INVALID_REQUEST,
    RBUS_DIAGNOSTICS_OUTCOME_INVALID_RESPONSE,
    RBUS_DIAGNOSTICS_OUTCOME_PERMISSION_DENIED,
    RBUS_DIAGNOSTICS_OUTCOME_INTERNAL_ERROR,
    RBUS_DIAGNOSTICS_OUTCOME_UNKNOWN_ERROR
} rbusDiagnosticsOutcome_t;

typedef struct {
    uint16_t protocolVersion;
    uint16_t operation;
    uint16_t outcome;
    uint16_t reserved;
    uint64_t durationUs;
    char component[RBUS_DIAGNOSTICS_COMPONENT_MAX];
    char operationName[RBUS_DIAGNOSTICS_NAME_MAX];
} rbusDiagnosticsObservation_t;

/**
 * Records one completed operation using a nonblocking best-effort datagram send.
 *
 * This function never changes the caller's RBus result and silently drops an
 * observation when the optional reporter is absent or saturated.
 */
void rbusDiagnostics_RecordOperation(
    rbusDiagnosticsOperation_t operation,
    const char* component,
    const char* operationName,
    uint64_t durationUs,
    rbusError_t result);

/**
 * Returns the current monotonic timestamp in microseconds for request timing.
 */
uint64_t rbusDiagnostics_MonotonicTimeUs(void);

#ifdef __cplusplus
}
#endif

#endif
