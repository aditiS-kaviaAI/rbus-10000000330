#include "rbus_diagnostics_collector.h"

#include <pthread.h>
#include <string.h>

/* Shared collector state is intentionally private to the diagnostics library. */
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

extern rbus_diagnostics_collector_t g_rbus_diagnostics_collector;

static uint64_t bucket_upper_bound(uint32_t bucket)
{
    return bucket == 0U ? 1U : (UINT64_C(1) << bucket);
}

static uint64_t percentile(
    const uint64_t histogram[RBUS_DIAGNOSTICS_HISTOGRAM_BUCKETS],
    uint64_t sample_count,
    uint32_t percentage)
{
    uint64_t rank = ((sample_count * percentage) + 99U) / 100U;
    uint64_t cumulative = 0;
    uint32_t bucket;

    for (bucket = 0; bucket < RBUS_DIAGNOSTICS_HISTOGRAM_BUCKETS; ++bucket)
    {
        cumulative += histogram[bucket];
        if (cumulative >= rank)
        {
            return bucket_upper_bound(bucket);
        }
    }

    return bucket_upper_bound(RBUS_DIAGNOSTICS_HISTOGRAM_BUCKETS - 1U);
}

static void complete_snapshot(
    rbus_diagnostics_global_snapshot_t* snapshot,
    const uint64_t histogram[RBUS_DIAGNOSTICS_HISTOGRAM_BUCKETS])
{
    snapshot->percentiles_available = snapshot->request_count >= 20U;
    if (!snapshot->percentiles_available)
    {
        snapshot->p50_latency_us = 0;
        snapshot->p90_latency_us = 0;
        snapshot->p95_latency_us = 0;
        snapshot->p99_latency_us = 0;
        return;
    }

    snapshot->p50_latency_us = percentile(histogram, snapshot->request_count, 50U);
    snapshot->p90_latency_us = percentile(histogram, snapshot->request_count, 90U);
    snapshot->p95_latency_us = percentile(histogram, snapshot->request_count, 95U);
    snapshot->p99_latency_us = percentile(histogram, snapshot->request_count, 99U);
}

/** Return the ABI version implemented by the local private diagnostics library. */
void rbusDiagnostics_GetAbiVersion(uint32_t* major, uint32_t* minor)
{
    if (major != NULL)
    {
        *major = RBUS_DIAGNOSTICS_ABI_MAJOR;
    }
    if (minor != NULL)
    {
        *minor = RBUS_DIAGNOSTICS_ABI_MINOR;
    }
}

/** Read the active runtime collection configuration. */
rbus_diagnostics_status_t rbusDiagnostics_GetConfiguration(
    rbus_diagnostics_configuration_t* output)
{
    if (output == NULL)
    {
        return RBUS_DIAGNOSTICS_STATUS_UNAVAILABLE;
    }

    pthread_mutex_lock(&g_rbus_diagnostics_collector.mutex);
    *output = g_rbus_diagnostics_collector.configuration;
    pthread_mutex_unlock(&g_rbus_diagnostics_collector.mutex);
    return RBUS_DIAGNOSTICS_STATUS_OK;
}

/** Replace the validated runtime collection configuration atomically. */
rbus_diagnostics_status_t rbusDiagnostics_SetConfiguration(
    const rbus_diagnostics_configuration_t* configuration)
{
    if (configuration == NULL)
    {
        return RBUS_DIAGNOSTICS_STATUS_LIMIT_INVALID;
    }

    pthread_mutex_lock(&g_rbus_diagnostics_collector.mutex);
    g_rbus_diagnostics_collector.configuration = *configuration;
    pthread_mutex_unlock(&g_rbus_diagnostics_collector.mutex);
    return RBUS_DIAGNOSTICS_STATUS_OK;
}

/** Copy a bounded global aggregate into caller-owned output storage. */
rbus_diagnostics_status_t rbusDiagnostics_GetGlobalSnapshot(
    rbus_diagnostics_global_snapshot_t* output)
{
    if (output == NULL)
    {
        return RBUS_DIAGNOSTICS_STATUS_UNAVAILABLE;
    }

    pthread_mutex_lock(&g_rbus_diagnostics_collector.mutex);
    if (!g_rbus_diagnostics_collector.configuration.enabled)
    {
        pthread_mutex_unlock(&g_rbus_diagnostics_collector.mutex);
        return RBUS_DIAGNOSTICS_STATUS_DISABLED;
    }

    *output = g_rbus_diagnostics_collector.global;
    complete_snapshot(output, g_rbus_diagnostics_collector.global_histogram);
    pthread_mutex_unlock(&g_rbus_diagnostics_collector.mutex);
    return RBUS_DIAGNOSTICS_STATUS_OK;
}

/** Copy a bounded aggregate for one known provider into caller-owned storage. */
rbus_diagnostics_status_t rbusDiagnostics_GetProviderSnapshot(
    const char* provider_id,
    rbus_diagnostics_provider_snapshot_t* output)
{
    uint32_t index;

    if (provider_id == NULL || provider_id[0] == '\0' || output == NULL)
    {
        return RBUS_DIAGNOSTICS_STATUS_LIMIT_INVALID;
    }

    pthread_mutex_lock(&g_rbus_diagnostics_collector.mutex);
    if (!g_rbus_diagnostics_collector.configuration.enabled)
    {
        pthread_mutex_unlock(&g_rbus_diagnostics_collector.mutex);
        return RBUS_DIAGNOSTICS_STATUS_DISABLED;
    }

    for (index = 0; index < RBUS_DIAGNOSTICS_MAX_PROVIDERS; ++index)
    {
        rbus_diagnostics_provider_record_t* provider =
            &g_rbus_diagnostics_collector.providers[index];
        if (provider->in_use && strcmp(provider->name, provider_id) == 0)
        {
            memset(output, 0, sizeof(*output));
            memcpy(output->provider, provider->name, sizeof(output->provider));
            output->identifier_truncated = provider->identifier_truncated;
            output->aggregate = provider->aggregate;
            complete_snapshot(&output->aggregate, provider->histogram);
            pthread_mutex_unlock(&g_rbus_diagnostics_collector.mutex);
            return RBUS_DIAGNOSTICS_STATUS_OK;
        }
    }

    pthread_mutex_unlock(&g_rbus_diagnostics_collector.mutex);
    return RBUS_DIAGNOSTICS_STATUS_NOT_FOUND;
}

/** Copy up to limit known providers into caller-owned output storage. */
rbus_diagnostics_status_t rbusDiagnostics_ListProviders(
    uint32_t limit,
    rbus_diagnostics_provider_snapshot_t* output,
    uint32_t output_capacity,
    uint32_t* output_count)
{
    uint32_t index;
    uint32_t count = 0;

    if (output_count == NULL || limit == 0U || limit > RBUS_DIAGNOSTICS_MAX_PROVIDERS ||
        output_capacity < limit || output == NULL)
    {
        return RBUS_DIAGNOSTICS_STATUS_LIMIT_INVALID;
    }

    pthread_mutex_lock(&g_rbus_diagnostics_collector.mutex);
    if (!g_rbus_diagnostics_collector.configuration.enabled)
    {
        pthread_mutex_unlock(&g_rbus_diagnostics_collector.mutex);
        return RBUS_DIAGNOSTICS_STATUS_DISABLED;
    }

    for (index = 0; index < RBUS_DIAGNOSTICS_MAX_PROVIDERS && count < limit; ++index)
    {
        rbus_diagnostics_provider_record_t* provider =
            &g_rbus_diagnostics_collector.providers[index];
        if (provider->in_use)
        {
            memset(&output[count], 0, sizeof(output[count]));
            memcpy(output[count].provider, provider->name, sizeof(output[count].provider));
            output[count].identifier_truncated = provider->identifier_truncated;
            output[count].aggregate = provider->aggregate;
            complete_snapshot(&output[count].aggregate, provider->histogram);
            ++count;
        }
    }

    *output_count = count;
    pthread_mutex_unlock(&g_rbus_diagnostics_collector.mutex);
    return RBUS_DIAGNOSTICS_STATUS_OK;
}

/** Copy recent slow requests in chronological order into caller-owned storage. */
rbus_diagnostics_status_t rbusDiagnostics_ListSlowRequests(
    uint32_t limit,
    rbus_diagnostics_slow_request_snapshot_t* output,
    uint32_t output_capacity,
    uint32_t* output_count)
{
    uint32_t index;
    uint32_t count;

    if (output_count == NULL || limit == 0U || limit > RBUS_DIAGNOSTICS_MAX_SLOW_REQUESTS ||
        output_capacity < limit || output == NULL)
    {
        return RBUS_DIAGNOSTICS_STATUS_LIMIT_INVALID;
    }

    pthread_mutex_lock(&g_rbus_diagnostics_collector.mutex);
    if (!g_rbus_diagnostics_collector.configuration.enabled)
    {
        pthread_mutex_unlock(&g_rbus_diagnostics_collector.mutex);
        return RBUS_DIAGNOSTICS_STATUS_DISABLED;
    }

    count = g_rbus_diagnostics_collector.slow_count < limit ?
        g_rbus_diagnostics_collector.slow_count : limit;
    for (index = 0; index < count; ++index)
    {
        uint32_t source_index = (g_rbus_diagnostics_collector.slow_head + index) %
            RBUS_DIAGNOSTICS_MAX_SLOW_REQUESTS;
        output[index] = g_rbus_diagnostics_collector.slow_requests[source_index];
    }

    *output_count = count;
    pthread_mutex_unlock(&g_rbus_diagnostics_collector.mutex);
    return RBUS_DIAGNOSTICS_STATUS_OK;
}

/** Start a new metric epoch without affecting live RBus resources. */
rbus_diagnostics_status_t rbusDiagnostics_Reset(void)
{
    uint64_t next_epoch;

    pthread_mutex_lock(&g_rbus_diagnostics_collector.mutex);
    next_epoch = g_rbus_diagnostics_collector.global.epoch + 1U;
    memset(&g_rbus_diagnostics_collector.global, 0,
        sizeof(g_rbus_diagnostics_collector.global));
    memset(g_rbus_diagnostics_collector.global_histogram, 0,
        sizeof(g_rbus_diagnostics_collector.global_histogram));
    memset(g_rbus_diagnostics_collector.providers, 0,
        sizeof(g_rbus_diagnostics_collector.providers));
    memset(g_rbus_diagnostics_collector.slow_requests, 0,
        sizeof(g_rbus_diagnostics_collector.slow_requests));
    g_rbus_diagnostics_collector.global.epoch = next_epoch;
    g_rbus_diagnostics_collector.slow_head = 0;
    g_rbus_diagnostics_collector.slow_count = 0;
    pthread_mutex_unlock(&g_rbus_diagnostics_collector.mutex);
    return RBUS_DIAGNOSTICS_STATUS_OK;
}
