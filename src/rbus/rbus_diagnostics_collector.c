#include "rbus_diagnostics_collector.h"

#include <pthread.h>
#include <string.h>

typedef struct
{
    bool in_use;
    char name[RBUS_DIAGNOSTICS_MAX_IDENTIFIER_LENGTH];
    bool identifier_truncated;
    rbus_diagnostics_global_snapshot_t aggregate;
    uint64_t histogram[RBUS_DIAGNOSTICS_HISTOGRAM_BUCKETS];
} rbus_diagnostics_provider_record_t;

typedef struct
{
    pthread_mutex_t mutex;
    rbus_diagnostics_configuration_t configuration;
    rbus_diagnostics_global_snapshot_t global;
    uint64_t global_histogram[RBUS_DIAGNOSTICS_HISTOGRAM_BUCKETS];
    rbus_diagnostics_provider_record_t providers[RBUS_DIAGNOSTICS_MAX_PROVIDERS];
    rbus_diagnostics_slow_request_snapshot_t slow_requests[RBUS_DIAGNOSTICS_MAX_SLOW_REQUESTS];
    uint32_t slow_head;
    uint32_t slow_count;
} rbus_diagnostics_collector_t;

rbus_diagnostics_collector_t g_rbus_diagnostics_collector = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .configuration = {
        .enabled = false,
        .slow_request_threshold_us = UINT64_C(100000)
    },
    .global = {
        .epoch = 1
    }
};

static void copy_identifier(
    char destination[RBUS_DIAGNOSTICS_MAX_IDENTIFIER_LENGTH],
    bool* truncated,
    const char* source)
{
    size_t source_length;

    if (source == NULL)
    {
        destination[0] = '\0';
        *truncated = false;
        return;
    }

    source_length = strlen(source);
    *truncated = source_length >= RBUS_DIAGNOSTICS_MAX_IDENTIFIER_LENGTH;
    if (*truncated)
    {
        source_length = RBUS_DIAGNOSTICS_MAX_IDENTIFIER_LENGTH - 1U;
    }

    memcpy(destination, source, source_length);
    destination[source_length] = '\0';
}

static uint32_t histogram_bucket(uint64_t latency_us)
{
    uint32_t bucket = 0;

    if (latency_us <= 1U)
    {
        return 0;
    }

    --latency_us;
    while (latency_us > 0U && bucket < (RBUS_DIAGNOSTICS_HISTOGRAM_BUCKETS - 1U))
    {
        ++bucket;
        latency_us >>= 1U;
    }

    return bucket;
}

static rbus_diagnostics_outcome_t classify_result(rbusError_t result)
{
    switch (result)
    {
        case RBUS_ERROR_SUCCESS:
            return RBUS_DIAGNOSTICS_OUTCOME_SUCCESS;
        case RBUS_ERROR_TIMEOUT:
            return RBUS_DIAGNOSTICS_OUTCOME_TIMEOUT;
        case RBUS_ERROR_DESTINATION_NOT_FOUND:
        case RBUS_ERROR_DESTINATION_NOT_REACHABLE:
            return RBUS_DIAGNOSTICS_OUTCOME_PROVIDER_UNAVAILABLE;
        case RBUS_ERROR_INVALID_INPUT:
        case RBUS_ERROR_INVALID_HANDLE:
        case RBUS_ERROR_INVALID_OPERATION:
        case RBUS_ERROR_INVALID_METHOD:
        case RBUS_ERROR_INVALID_PARAMETER_TYPE:
        case RBUS_ERROR_INVALID_PARAMETER_VALUE:
            return RBUS_DIAGNOSTICS_OUTCOME_INVALID_REQUEST;
        case RBUS_ERROR_INVALID_RESPONSE_FROM_DESTINATION:
        case RBUS_ERROR_DESTINATION_RESPONSE_FAILURE:
            return RBUS_DIAGNOSTICS_OUTCOME_INVALID_RESPONSE;
        case RBUS_ERROR_ACCESS_NOT_ALLOWED:
            return RBUS_DIAGNOSTICS_OUTCOME_PERMISSION_DENIED;
        case RBUS_ERROR_BUS_ERROR:
        case RBUS_ERROR_OUT_OF_RESOURCES:
            return RBUS_DIAGNOSTICS_OUTCOME_INTERNAL_ERROR;
        default:
            return RBUS_DIAGNOSTICS_OUTCOME_UNKNOWN;
    }
}

static void update_aggregate(
    rbus_diagnostics_global_snapshot_t* aggregate,
    uint64_t histogram[RBUS_DIAGNOSTICS_HISTOGRAM_BUCKETS],
    rbusError_t result,
    uint64_t latency_us)
{
    ++aggregate->request_count;
    if (result == RBUS_ERROR_SUCCESS)
    {
        ++aggregate->success_count;
    }
    else
    {
        ++aggregate->error_count;
    }

    if (result == RBUS_ERROR_TIMEOUT)
    {
        ++aggregate->timeout_count;
    }

    if (!aggregate->latency_available || latency_us < aggregate->min_latency_us)
    {
        aggregate->min_latency_us = latency_us;
    }
    if (!aggregate->latency_available || latency_us > aggregate->max_latency_us)
    {
        aggregate->max_latency_us = latency_us;
    }

    aggregate->latency_available = true;
    aggregate->average_latency_us =
        ((aggregate->average_latency_us * (aggregate->request_count - 1U)) + latency_us) /
        aggregate->request_count;
    ++histogram[histogram_bucket(latency_us)];
}

static rbus_diagnostics_provider_record_t* find_provider(const char* provider_name)
{
    uint32_t index;
    rbus_diagnostics_provider_record_t* empty_record = NULL;

    if (provider_name == NULL || provider_name[0] == '\0')
    {
        return NULL;
    }

    for (index = 0; index < RBUS_DIAGNOSTICS_MAX_PROVIDERS; ++index)
    {
        rbus_diagnostics_provider_record_t* record =
            &g_rbus_diagnostics_collector.providers[index];

        if (record->in_use && strcmp(record->name, provider_name) == 0)
        {
            return record;
        }
        if (!record->in_use && empty_record == NULL)
        {
            empty_record = record;
        }
    }

    if (empty_record != NULL)
    {
        memset(empty_record, 0, sizeof(*empty_record));
        empty_record->in_use = true;
        empty_record->aggregate.epoch = g_rbus_diagnostics_collector.global.epoch;
        copy_identifier(
            empty_record->name,
            &empty_record->identifier_truncated,
            provider_name);
    }

    return empty_record;
}

static void record_slow_request(
    const rbus_diagnostics_observation_t* observation,
    uint64_t latency_us)
{
    uint32_t index;
    rbus_diagnostics_slow_request_snapshot_t* record;

    if (latency_us <= g_rbus_diagnostics_collector.configuration.slow_request_threshold_us)
    {
        return;
    }

    index = (g_rbus_diagnostics_collector.slow_head +
        g_rbus_diagnostics_collector.slow_count) % RBUS_DIAGNOSTICS_MAX_SLOW_REQUESTS;
    if (g_rbus_diagnostics_collector.slow_count == RBUS_DIAGNOSTICS_MAX_SLOW_REQUESTS)
    {
        index = g_rbus_diagnostics_collector.slow_head;
        g_rbus_diagnostics_collector.slow_head =
            (g_rbus_diagnostics_collector.slow_head + 1U) %
            RBUS_DIAGNOSTICS_MAX_SLOW_REQUESTS;
    }
    else
    {
        ++g_rbus_diagnostics_collector.slow_count;
    }

    record = &g_rbus_diagnostics_collector.slow_requests[index];
    memset(record, 0, sizeof(*record));
    record->operation = observation->operation;
    record->outcome = classify_result(observation->rbus_result);
    record->latency_us = latency_us;
    record->completed_epoch_ms = observation->completed_epoch_ms;
    record->epoch = g_rbus_diagnostics_collector.global.epoch;
    copy_identifier(
        record->operation_name,
        &record->operation_name_truncated,
        observation->operation_name);
    copy_identifier(
        record->provider_name,
        &record->provider_name_truncated,
        observation->provider_name);
}

/** Return whether runtime collection is enabled without exposing collector state. */
bool rbusDiagnosticsCollector_IsEnabled(void)
{
    return g_rbus_diagnostics_collector.configuration.enabled;
}

/** Record one completed metadata-only observation on a best-effort basis. */
void rbusDiagnosticsCollector_RecordCompleted(
    const rbus_diagnostics_observation_t* observation)
{
    uint64_t latency_us;
    rbus_diagnostics_provider_record_t* provider;

    if (observation == NULL)
    {
        return;
    }

    /*
     * The disabled branch intentionally occurs before clock work or locking.
     * Callers may still provide timing values, but no collector state is read.
     */
    if (!g_rbus_diagnostics_collector.configuration.enabled)
    {
        return;
    }

    latency_us = observation->completed_monotonic_us >= observation->started_monotonic_us ?
        observation->completed_monotonic_us - observation->started_monotonic_us : 0U;

    pthread_mutex_lock(&g_rbus_diagnostics_collector.mutex);
    if (!g_rbus_diagnostics_collector.configuration.enabled)
    {
        pthread_mutex_unlock(&g_rbus_diagnostics_collector.mutex);
        return;
    }

    update_aggregate(
        &g_rbus_diagnostics_collector.global,
        g_rbus_diagnostics_collector.global_histogram,
        observation->rbus_result,
        latency_us);

    provider = find_provider(observation->provider_name);
    if (provider != NULL)
    {
        update_aggregate(
            &provider->aggregate,
            provider->histogram,
            observation->rbus_result,
            latency_us);
    }

    record_slow_request(observation, latency_us);
    pthread_mutex_unlock(&g_rbus_diagnostics_collector.mutex);
}
