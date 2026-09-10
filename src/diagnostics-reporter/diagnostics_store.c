#include "diagnostics_store.h"

#include <limits.h>
#include <string.h>

static uint64_t saturatingAdd(uint64_t current, uint64_t increment, bool* saturated)
{
    if (UINT64_MAX - current < increment) {
        *saturated = true;
        return UINT64_MAX;
    }
    return current + increment;
}

static uint32_t histogramBucket(uint64_t durationUs)
{
    uint32_t bucket = 0;

    while (durationUs > 0 && bucket < RBUS_DIAGNOSTICS_HISTOGRAM_BUCKETS - 1U) {
        ++bucket;
        durationUs >>= 1;
    }
    return bucket;
}

static void updateMetrics(rbusDiagnosticsOperationMetrics_t* metrics,
    const rbusDiagnosticsObservation_t* observation, bool* saturated,
    bool* histogramSaturated)
{
    uint32_t bucket;

    metrics->requestCount = saturatingAdd(metrics->requestCount, 1, saturated);
    metrics->durationSumUs =
        saturatingAdd(metrics->durationSumUs, observation->durationUs, saturated);
    if (metrics->requestCount == 1 || observation->durationUs < metrics->minimumDurationUs) {
        metrics->minimumDurationUs = observation->durationUs;
    }
    if (observation->durationUs > metrics->maximumDurationUs) {
        metrics->maximumDurationUs = observation->durationUs;
    }

    if (observation->outcome == RBUS_DIAGNOSTICS_OUTCOME_SUCCESS) {
        metrics->successCount = saturatingAdd(metrics->successCount, 1, saturated);
    } else if (observation->outcome == RBUS_DIAGNOSTICS_OUTCOME_TIMEOUT) {
        metrics->timeoutCount = saturatingAdd(metrics->timeoutCount, 1, saturated);
    } else {
        metrics->errorCount = saturatingAdd(metrics->errorCount, 1, saturated);
    }

    bucket = histogramBucket(observation->durationUs);
    if (metrics->histogram[bucket] == UINT64_MAX) {
        *histogramSaturated = true;
    } else {
        ++metrics->histogram[bucket];
    }
}

static void normalizePathPrefix(const char* path, char* prefix, size_t capacity)
{
    size_t output = 0;
    size_t componentLength = 0;
    unsigned int depth = 0;
    bool numericComponent = true;
    const char* cursor = path;

    prefix[0] = '\0';
    while (*cursor != '\0' && output + 1U < capacity && depth < 4U) {
        const char* component = cursor;
        size_t length = 0;

        while (cursor[length] != '\0' && cursor[length] != '.') {
            if (cursor[length] < '0' || cursor[length] > '9') {
                numericComponent = false;
            }
            ++length;
        }
        componentLength = length;
        if (depth != 0 && output + 1U < capacity) {
            prefix[output++] = '.';
        }
        if (numericComponent && componentLength != 0) {
            static const char tableIndex[] = "{i}";
            if (output + sizeof(tableIndex) >= capacity) {
                break;
            }
            memcpy(prefix + output, tableIndex, sizeof(tableIndex) - 1U);
            output += sizeof(tableIndex) - 1U;
        } else if (output + componentLength < capacity) {
            memcpy(prefix + output, component, componentLength);
            output += componentLength;
        } else {
            break;
        }

        ++depth;
        if (cursor[length] == '\0') {
            break;
        }
        cursor += length + 1U;
        numericComponent = true;
    }
    prefix[output] = '\0';
}

static rbusDiagnosticsProviderRecord_t* findProvider(rbusDiagnosticsStore_t* store,
    const char* provider, bool create)
{
    uint32_t index;
    rbusDiagnosticsProviderRecord_t* available = NULL;

    if (provider[0] == '\0') {
        return NULL;
    }
    for (index = 0; index < RBUS_DIAGNOSTICS_MAX_PROVIDERS; ++index) {
        rbusDiagnosticsProviderRecord_t* record = &store->providers[index];

        if (record->inUse && strcmp(record->provider, provider) == 0) {
            return record;
        }
        if (!record->inUse && available == NULL) {
            available = record;
        }
    }
    if (!create || available == NULL) {
        if (create) {
            ++store->state.providerCapacityOverflows;
        }
        return NULL;
    }

    memset(available, 0, sizeof(*available));
    available->inUse = true;
    available->state = RBUS_DIAGNOSTICS_PROVIDER_UNKNOWN;
    strncpy(available->provider, provider, sizeof(available->provider) - 1U);
    return available;
}

static void updateProvider(rbusDiagnosticsStore_t* store,
    const rbusDiagnosticsObservation_t* observation)
{
    rbusDiagnosticsProviderRecord_t* provider =
        findProvider(store, observation->provider, true);

    if (provider == NULL) {
        return;
    }

    if (observation->messageType == RBUS_DIAGNOSTICS_MESSAGE_PROVIDER_LIFECYCLE) {
        if (observation->lifecycleAction == RBUS_DIAGNOSTICS_LIFECYCLE_REGISTERED) {
            if (provider->registrationState == RBUS_DIAGNOSTICS_LIFECYCLE_REMOVED ||
                provider->state == RBUS_DIAGNOSTICS_PROVIDER_UNAVAILABLE) {
                ++provider->restartCount;
            }
            provider->registrationState = RBUS_DIAGNOSTICS_LIFECYCLE_REGISTERED;
            provider->state = RBUS_DIAGNOSTICS_PROVIDER_STARTING;
        } else if (observation->lifecycleAction == RBUS_DIAGNOSTICS_LIFECYCLE_REMOVED) {
            provider->registrationState = RBUS_DIAGNOSTICS_LIFECYCLE_REMOVED;
            provider->state = RBUS_DIAGNOSTICS_PROVIDER_STOPPED;
        }
        return;
    }

    updateMetrics(&provider->metrics, observation, &store->state.durationSumSaturated,
        &store->state.histogramSaturated);
    if (observation->outcome == RBUS_DIAGNOSTICS_OUTCOME_PROVIDER_UNAVAILABLE) {
        provider->state = RBUS_DIAGNOSTICS_PROVIDER_UNAVAILABLE;
    } else if (observation->outcome == RBUS_DIAGNOSTICS_OUTCOME_SUCCESS &&
        provider->registrationState == RBUS_DIAGNOSTICS_LIFECYCLE_REGISTERED) {
        provider->state = RBUS_DIAGNOSTICS_PROVIDER_HEALTHY;
    }
}

static void updatePathPrefix(rbusDiagnosticsStore_t* store,
    const rbusDiagnosticsObservation_t* observation)
{
    char prefix[RBUS_DIAGNOSTICS_MAX_PATH_NAME];
    rbusDiagnosticsPathPrefixRecord_t* available = NULL;
    uint32_t index;

    if (observation->path[0] == '\0') {
        return;
    }
    normalizePathPrefix(observation->path, prefix, sizeof(prefix));
    if (prefix[0] == '\0') {
        return;
    }

    for (index = 0; index < RBUS_DIAGNOSTICS_MAX_PATH_PREFIXES; ++index) {
        rbusDiagnosticsPathPrefixRecord_t* record = &store->pathPrefixes[index];

        if (record->inUse && record->operation == observation->operation &&
            strcmp(record->prefix, prefix) == 0) {
            updateMetrics(&record->metrics, observation, &store->state.durationSumSaturated,
                &store->state.histogramSaturated);
            return;
        }
        if (!record->inUse && available == NULL) {
            available = record;
        }
    }
    if (available == NULL) {
        ++store->state.pathPrefixCapacityOverflows;
        return;
    }

    memset(available, 0, sizeof(*available));
    available->inUse = true;
    available->operation = observation->operation;
    strncpy(available->prefix, prefix, sizeof(available->prefix) - 1U);
    updateMetrics(&available->metrics, observation, &store->state.durationSumSaturated,
        &store->state.histogramSaturated);
}

static void retainSlowRequest(rbusDiagnosticsStore_t* store,
    const rbusDiagnosticsObservation_t* observation)
{
    uint32_t slot;

    if (observation->durationUs <= RBUS_DIAGNOSTICS_SLOW_REQUEST_THRESHOLD_US) {
        return;
    }
    slot = (store->slowRequestHead + store->slowRequestCount) %
        RBUS_DIAGNOSTICS_MAX_SLOW_REQUESTS;
    if (store->slowRequestCount == RBUS_DIAGNOSTICS_MAX_SLOW_REQUESTS) {
        slot = store->slowRequestHead;
        store->slowRequestHead = (store->slowRequestHead + 1U) %
            RBUS_DIAGNOSTICS_MAX_SLOW_REQUESTS;
        ++store->state.slowRequestOverwrites;
    } else {
        ++store->slowRequestCount;
    }
    store->slowRequests[slot].observation = *observation;
    store->slowRequests[slot].measurementGeneration = store->state.measurementGeneration;
}

static void processPublisherStatus(rbusDiagnosticsStore_t* store,
    const rbusDiagnosticsObservation_t* observation)
{
    uint32_t index;
    rbusDiagnosticsPublisherStatus_t* available = NULL;

    for (index = 0; index < RBUS_DIAGNOSTICS_MAX_PUBLISHER_STATUSES; ++index) {
        rbusDiagnosticsPublisherStatus_t* status = &store->publishers[index];

        if (status->inUse &&
            status->publisherInstanceId == observation->publisherInstanceId) {
            if (observation->publisherSequence <= status->lastAcceptedSequence) {
                ++store->state.duplicateOrOutOfOrderPublisherStatuses;
                return;
            }
            if (observation->cumulativeDroppedObservations >= status->lastCumulativeDrops) {
                store->state.publisherDropDelta = saturatingAdd(
                    store->state.publisherDropDelta,
                    observation->cumulativeDroppedObservations - status->lastCumulativeDrops,
                    &store->state.durationSumSaturated);
            } else {
                ++store->state.duplicateOrOutOfOrderPublisherStatuses;
                return;
            }
            status->lastAcceptedSequence = observation->publisherSequence;
            status->lastCumulativeDrops = observation->cumulativeDroppedObservations;
            return;
        }
        if (!status->inUse && available == NULL) {
            available = status;
        }
    }

    if (available == NULL) {
        ++store->state.publisherStatusCapacityOverflows;
        store->state.publisherDropCompletenessPartial = true;
        return;
    }
    available->inUse = true;
    available->publisherInstanceId = observation->publisherInstanceId;
    available->lastAcceptedSequence = observation->publisherSequence;
    available->lastCumulativeDrops = observation->cumulativeDroppedObservations;
}

int rbusDiagnosticsStoreInitialize(
    rbusDiagnosticsStore_t* store,
    uint64_t reporterInstanceId,
    uint64_t reporterStartupEpochUs)
{
    if (store == NULL) {
        return -1;
    }

    memset(store, 0, sizeof(*store));
    if (pthread_mutex_init(&store->lock, NULL) != 0) {
        return -1;
    }
    store->state.reporterInstanceId = reporterInstanceId;
    store->state.reporterStartupEpochUs = reporterStartupEpochUs;
    store->state.measurementGeneration = 1;
    return 0;
}

void rbusDiagnosticsStoreDestroy(rbusDiagnosticsStore_t* store)
{
    if (store != NULL) {
        pthread_mutex_destroy(&store->lock);
    }
}

void rbusDiagnosticsStoreAccept(rbusDiagnosticsStore_t* store,
    const rbusDiagnosticsObservation_t* observation)
{
    rbusDiagnosticsOperationMetrics_t* metrics;

    if (store == NULL || observation == NULL ||
        observation->operation >= RBUS_DIAGNOSTICS_OPERATION_COUNT) {
        return;
    }

    pthread_mutex_lock(&store->lock);
    if (observation->messageType == RBUS_DIAGNOSTICS_MESSAGE_PUBLISHER_STATUS) {
        processPublisherStatus(store, observation);
        pthread_mutex_unlock(&store->lock);
        return;
    }
    if (observation->messageType == RBUS_DIAGNOSTICS_MESSAGE_PROVIDER_LIFECYCLE) {
        updateProvider(store, observation);
        pthread_mutex_unlock(&store->lock);
        return;
    }

    metrics = &store->state.operations[observation->operation];
    store->state.observationsReceived =
        saturatingAdd(store->state.observationsReceived, 1,
            &store->state.durationSumSaturated);
    updateMetrics(metrics, observation, &store->state.durationSumSaturated,
        &store->state.histogramSaturated);
    updateProvider(store, observation);
    updatePathPrefix(store, observation);
    retainSlowRequest(store, observation);
    pthread_mutex_unlock(&store->lock);
}

void rbusDiagnosticsStoreRejectMalformed(rbusDiagnosticsStore_t* store)
{
    if (store == NULL) {
        return;
    }

    pthread_mutex_lock(&store->lock);
    store->state.malformedObservationsRejected =
        saturatingAdd(store->state.malformedObservationsRejected, 1,
            &store->state.durationSumSaturated);
    pthread_mutex_unlock(&store->lock);
}

void rbusDiagnosticsStoreSnapshot(const rbusDiagnosticsStore_t* store,
    rbusDiagnosticsGlobalSnapshot_t* snapshot)
{
    rbusDiagnosticsStore_t* mutableStore = (rbusDiagnosticsStore_t*)store;

    if (store == NULL || snapshot == NULL) {
        return;
    }

    pthread_mutex_lock(&mutableStore->lock);
    *snapshot = store->state;
    pthread_mutex_unlock(&mutableStore->lock);
}

void rbusDiagnosticsStoreReset(rbusDiagnosticsStore_t* store,
    rbusDiagnosticsGlobalSnapshot_t* snapshot)
{
    uint32_t index;
    uint64_t instanceId;
    uint64_t startupEpochUs;
    uint64_t nextGeneration;

    if (store == NULL) {
        return;
    }

    pthread_mutex_lock(&store->lock);
    instanceId = store->state.reporterInstanceId;
    startupEpochUs = store->state.reporterStartupEpochUs;
    nextGeneration = store->state.measurementGeneration == UINT64_MAX ?
        UINT64_MAX : store->state.measurementGeneration + 1U;

    memset(&store->state, 0, sizeof(store->state));
    memset(store->pathPrefixes, 0, sizeof(store->pathPrefixes));
    memset(store->slowRequests, 0, sizeof(store->slowRequests));
    store->slowRequestHead = 0;
    store->slowRequestCount = 0;
    store->state.reporterInstanceId = instanceId;
    store->state.reporterStartupEpochUs = startupEpochUs;
    store->state.measurementGeneration = nextGeneration;

    for (index = 0; index < RBUS_DIAGNOSTICS_MAX_PROVIDERS; ++index) {
        rbusDiagnosticsProviderRecord_t* provider = &store->providers[index];

        if (!provider->inUse) {
            continue;
        }
        memset(&provider->metrics, 0, sizeof(provider->metrics));
        provider->restartCount = 0;
        provider->state =
            provider->registrationState == RBUS_DIAGNOSTICS_LIFECYCLE_REMOVED ?
            RBUS_DIAGNOSTICS_PROVIDER_STOPPED :
            RBUS_DIAGNOSTICS_PROVIDER_STARTING;
    }

    if (snapshot != NULL) {
        *snapshot = store->state;
    }
    pthread_mutex_unlock(&store->lock);
}

bool rbusDiagnosticsStoreProviderSnapshot(const rbusDiagnosticsStore_t* store,
    const char* provider, rbusDiagnosticsGlobalSnapshot_t* global,
    rbusDiagnosticsProviderSnapshot_t* providerSnapshot)
{
    rbusDiagnosticsStore_t* mutableStore = (rbusDiagnosticsStore_t*)store;
    uint32_t index;

    if (store == NULL || provider == NULL || providerSnapshot == NULL ||
        provider[0] == '\0') {
        return false;
    }

    pthread_mutex_lock(&mutableStore->lock);
    if (global != NULL) {
        *global = store->state;
    }
    for (index = 0; index < RBUS_DIAGNOSTICS_MAX_PROVIDERS; ++index) {
        const rbusDiagnosticsProviderRecord_t* record = &store->providers[index];
        if (record->inUse && strcmp(record->provider, provider) == 0) {
            memset(providerSnapshot, 0, sizeof(*providerSnapshot));
            strncpy(providerSnapshot->provider, record->provider,
                sizeof(providerSnapshot->provider) - 1U);
            providerSnapshot->state = record->state;
            providerSnapshot->registrationState = record->registrationState;
            providerSnapshot->restartCount = record->restartCount;
            providerSnapshot->metrics = record->metrics;
            pthread_mutex_unlock(&mutableStore->lock);
            return true;
        }
    }
    pthread_mutex_unlock(&mutableStore->lock);
    return false;
}

uint32_t rbusDiagnosticsStoreProviderList(const rbusDiagnosticsStore_t* store,
    uint32_t limit, rbusDiagnosticsGlobalSnapshot_t* global,
    rbusDiagnosticsProviderSnapshot_t* providers)
{
    rbusDiagnosticsStore_t* mutableStore = (rbusDiagnosticsStore_t*)store;
    uint32_t index;
    uint32_t count = 0;

    if (store == NULL || providers == NULL) {
        return 0;
    }
    if (limit > RBUS_DIAGNOSTICS_MAX_PROVIDERS) {
        limit = RBUS_DIAGNOSTICS_MAX_PROVIDERS;
    }

    pthread_mutex_lock(&mutableStore->lock);
    if (global != NULL) {
        *global = store->state;
    }
    for (index = 0; index < RBUS_DIAGNOSTICS_MAX_PROVIDERS && count < limit; ++index) {
        const rbusDiagnosticsProviderRecord_t* record = &store->providers[index];
        if (!record->inUse) {
            continue;
        }
        memset(&providers[count], 0, sizeof(providers[count]));
        strncpy(providers[count].provider, record->provider,
            sizeof(providers[count].provider) - 1U);
        providers[count].state = record->state;
        providers[count].registrationState = record->registrationState;
        providers[count].restartCount = record->restartCount;
        providers[count].metrics = record->metrics;
        ++count;
    }
    pthread_mutex_unlock(&mutableStore->lock);
    return count;
}

uint32_t rbusDiagnosticsStoreSlowRequestList(const rbusDiagnosticsStore_t* store,
    uint32_t limit, rbusDiagnosticsGlobalSnapshot_t* global,
    rbusDiagnosticsSlowRequest_t* requests)
{
    rbusDiagnosticsStore_t* mutableStore = (rbusDiagnosticsStore_t*)store;
    uint32_t count;
    uint32_t index;

    if (store == NULL || requests == NULL) {
        return 0;
    }
    if (limit > RBUS_DIAGNOSTICS_MAX_SLOW_REQUESTS) {
        limit = RBUS_DIAGNOSTICS_MAX_SLOW_REQUESTS;
    }

    pthread_mutex_lock(&mutableStore->lock);
    if (global != NULL) {
        *global = store->state;
    }
    count = store->slowRequestCount < limit ? store->slowRequestCount : limit;
    for (index = 0; index < count; ++index) {
        uint32_t slot = (store->slowRequestHead + store->slowRequestCount - 1U - index) %
            RBUS_DIAGNOSTICS_MAX_SLOW_REQUESTS;
        requests[index] = store->slowRequests[slot];
    }
    pthread_mutex_unlock(&mutableStore->lock);
    return count;
}
