#ifndef RBUS_DIAGNOSTICS_TYPES_H
#define RBUS_DIAGNOSTICS_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#include "rbus.h"

/*
 * These limits bound all collector-owned memory. Identifiers are copied as
 * metadata only; request and response payloads are never retained.
 */
#define RBUS_DIAGNOSTICS_ABI_MAJOR 1U
#define RBUS_DIAGNOSTICS_ABI_MINOR 0U
#define RBUS_DIAGNOSTICS_HISTOGRAM_BUCKETS 64U
#define RBUS_DIAGNOSTICS_MAX_PROVIDERS 64U
#define RBUS_DIAGNOSTICS_MAX_SLOW_REQUESTS 100U
#define RBUS_DIAGNOSTICS_MAX_IDENTIFIER_LENGTH 128U

typedef enum
{
    RBUS_DIAGNOSTICS_OPERATION_GET = 0,
    RBUS_DIAGNOSTICS_OPERATION_SET,
    RBUS_DIAGNOSTICS_OPERATION_METHOD_SYNC,
    RBUS_DIAGNOSTICS_OPERATION_TABLE
} rbus_diagnostics_operation_kind_t;

typedef enum
{
    RBUS_DIAGNOSTICS_OUTCOME_SUCCESS = 0,
    RBUS_DIAGNOSTICS_OUTCOME_TIMEOUT,
    RBUS_DIAGNOSTICS_OUTCOME_PROVIDER_UNAVAILABLE,
    RBUS_DIAGNOSTICS_OUTCOME_INVALID_REQUEST,
    RBUS_DIAGNOSTICS_OUTCOME_INVALID_RESPONSE,
    RBUS_DIAGNOSTICS_OUTCOME_PERMISSION_DENIED,
    RBUS_DIAGNOSTICS_OUTCOME_INTERNAL_ERROR,
    RBUS_DIAGNOSTICS_OUTCOME_UNKNOWN
} rbus_diagnostics_outcome_t;

typedef struct
{
    rbus_diagnostics_operation_kind_t operation;
    rbusError_t rbus_result;
    uint64_t started_monotonic_us;
    uint64_t completed_monotonic_us;
    uint64_t completed_epoch_ms;
    const char* operation_name;
    const char* provider_name;
} rbus_diagnostics_observation_t;

typedef struct
{
    uint64_t request_count;
    uint64_t success_count;
    uint64_t error_count;
    uint64_t timeout_count;
    uint64_t min_latency_us;
    uint64_t max_latency_us;
    uint64_t average_latency_us;
    uint64_t epoch;
    bool latency_available;
    bool percentiles_available;
    uint64_t p50_latency_us;
    uint64_t p90_latency_us;
    uint64_t p95_latency_us;
    uint64_t p99_latency_us;
} rbus_diagnostics_global_snapshot_t;

typedef struct
{
    char provider[RBUS_DIAGNOSTICS_MAX_IDENTIFIER_LENGTH];
    bool identifier_truncated;
    rbus_diagnostics_global_snapshot_t aggregate;
} rbus_diagnostics_provider_snapshot_t;

typedef struct
{
    rbus_diagnostics_operation_kind_t operation;
    rbus_diagnostics_outcome_t outcome;
    char operation_name[RBUS_DIAGNOSTICS_MAX_IDENTIFIER_LENGTH];
    char provider_name[RBUS_DIAGNOSTICS_MAX_IDENTIFIER_LENGTH];
    bool operation_name_truncated;
    bool provider_name_truncated;
    uint64_t latency_us;
    uint64_t completed_epoch_ms;
    uint64_t epoch;
} rbus_diagnostics_slow_request_snapshot_t;

typedef struct
{
    bool enabled;
    uint64_t slow_request_threshold_us;
} rbus_diagnostics_configuration_t;

#endif
