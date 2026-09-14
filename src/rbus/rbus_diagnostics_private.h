#ifndef RBUS_DIAGNOSTICS_PRIVATE_H
#define RBUS_DIAGNOSTICS_PRIVATE_H

#include "rbus_diagnostics_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    RBUS_DIAGNOSTICS_STATUS_OK = 0,
    RBUS_DIAGNOSTICS_STATUS_DISABLED,
    RBUS_DIAGNOSTICS_STATUS_NOT_FOUND,
    RBUS_DIAGNOSTICS_STATUS_LIMIT_INVALID,
    RBUS_DIAGNOSTICS_STATUS_UNAVAILABLE,
    RBUS_DIAGNOSTICS_STATUS_ABI_MISMATCH
} rbus_diagnostics_status_t;

/** Return the ABI version implemented by the local private diagnostics library. */
void rbusDiagnostics_GetAbiVersion(uint32_t* major, uint32_t* minor);

/** Read the active runtime collection configuration. */
rbus_diagnostics_status_t rbusDiagnostics_GetConfiguration(
    rbus_diagnostics_configuration_t* output);

/** Replace the validated runtime collection configuration atomically. */
rbus_diagnostics_status_t rbusDiagnostics_SetConfiguration(
    const rbus_diagnostics_configuration_t* configuration);

/** Copy a bounded global aggregate into caller-owned output storage. */
rbus_diagnostics_status_t rbusDiagnostics_GetGlobalSnapshot(
    rbus_diagnostics_global_snapshot_t* output);

/** Copy a bounded aggregate for one known provider into caller-owned storage. */
rbus_diagnostics_status_t rbusDiagnostics_GetProviderSnapshot(
    const char* provider_id,
    rbus_diagnostics_provider_snapshot_t* output);

/** Copy up to limit known providers into caller-owned output storage. */
rbus_diagnostics_status_t rbusDiagnostics_ListProviders(
    uint32_t limit,
    rbus_diagnostics_provider_snapshot_t* output,
    uint32_t output_capacity,
    uint32_t* output_count);

/** Copy recent slow requests in chronological order into caller-owned storage. */
rbus_diagnostics_status_t rbusDiagnostics_ListSlowRequests(
    uint32_t limit,
    rbus_diagnostics_slow_request_snapshot_t* output,
    uint32_t output_capacity,
    uint32_t* output_count);

/** Start a new metric epoch without affecting live RBus resources. */
rbus_diagnostics_status_t rbusDiagnostics_Reset(void);

#ifdef __cplusplus
}
#endif

#endif
