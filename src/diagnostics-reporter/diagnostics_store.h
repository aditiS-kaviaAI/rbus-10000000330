#ifndef RBUS_DIAGNOSTICS_STORE_H
#define RBUS_DIAGNOSTICS_STORE_H

#include "diagnostics_protocol.h"

#include <pthread.h>
#include <stdint.h>

#define RBUS_DIAGNOSTICS_MAX_PROVIDERS 32U
#define RBUS_DIAGNOSTICS_MAX_PATH_PREFIXES 64U
#define RBUS_DIAGNOSTICS_MAX_SLOW_REQUESTS 32U
#define RBUS_DIAGNOSTICS_MAX_PUBLISHER_STATUSES 32U
#define RBUS_DIAGNOSTICS_SLOW_REQUEST_THRESHOLD_US 1000000U

typedef enum {
    RBUS_DIAGNOSTICS_PROVIDER_UNKNOWN = 0,
    RBUS_DIAGNOSTICS_PROVIDER_STARTING,
    RBUS_DIAGNOSTICS_PROVIDER_HEALTHY,
    RBUS_DIAGNOSTICS_PROVIDER_DEGRADED,
    RBUS_DIAGNOSTICS_PROVIDER_UNAVAILABLE,
    RBUS_DIAGNOSTICS_PROVIDER_STOPPED
} rbusDiagnosticsProviderState_t;

typedef struct {
    bool inUse;
    char provider[RBUS_DIAGNOSTICS_MAX_PROVIDER_NAME];
    rbusDiagnosticsProviderState_t state;
    rbusDiagnosticsLifecycleAction_t registrationState;
    uint64_t restartCount;
    rbusDiagnosticsOperationMetrics_t metrics;
} rbusDiagnosticsProviderRecord_t;

typedef struct {
    bool inUse;
    rbusDiagnosticsOperation_t operation;
    char prefix[RBUS_DIAGNOSTICS_MAX_PATH_NAME];
    rbusDiagnosticsOperationMetrics_t metrics;
} rbusDiagnosticsPathPrefixRecord_t;

typedef struct {
    rbusDiagnosticsObservation_t observation;
    uint64_t measurementGeneration;
} rbusDiagnosticsSlowRequest_t;

typedef struct {
    bool inUse;
    uint64_t publisherInstanceId;
    uint64_t lastAcceptedSequence;
    uint64_t lastCumulativeDrops;
} rbusDiagnosticsPublisherStatus_t;

typedef struct {
    char provider[RBUS_DIAGNOSTICS_MAX_PROVIDER_NAME];
    rbusDiagnosticsProviderState_t state;
    rbusDiagnosticsLifecycleAction_t registrationState;
    uint64_t restartCount;
    rbusDiagnosticsOperationMetrics_t metrics;
} rbusDiagnosticsProviderSnapshot_t;

typedef struct {
    pthread_mutex_t lock;
    rbusDiagnosticsGlobalSnapshot_t state;
    rbusDiagnosticsProviderRecord_t providers[RBUS_DIAGNOSTICS_MAX_PROVIDERS];
    rbusDiagnosticsPathPrefixRecord_t pathPrefixes[RBUS_DIAGNOSTICS_MAX_PATH_PREFIXES];
    rbusDiagnosticsSlowRequest_t slowRequests[RBUS_DIAGNOSTICS_MAX_SLOW_REQUESTS];
    rbusDiagnosticsPublisherStatus_t publishers[RBUS_DIAGNOSTICS_MAX_PUBLISHER_STATUSES];
    uint32_t slowRequestHead;
    uint32_t slowRequestCount;
} rbusDiagnosticsStore_t;

/* PUBLIC_INTERFACE */
/** Initializes fixed-size reporter state and bounded attribution collections. */
int rbusDiagnosticsStoreInitialize(
    rbusDiagnosticsStore_t* store,
    uint64_t reporterInstanceId,
    uint64_t reporterStartupEpochUs);

/* PUBLIC_INTERFACE */
/** Releases the store synchronization primitive. */
void rbusDiagnosticsStoreDestroy(rbusDiagnosticsStore_t* store);

/* PUBLIC_INTERFACE */
/** Accepts a decoded bounded observation without retaining payload data. */
void rbusDiagnosticsStoreAccept(rbusDiagnosticsStore_t* store,
    const rbusDiagnosticsObservation_t* observation);

/* PUBLIC_INTERFACE */
/** Records a rejected malformed observation. */
void rbusDiagnosticsStoreRejectMalformed(rbusDiagnosticsStore_t* store);

/* PUBLIC_INTERFACE */
/** Copies a consistent immutable global snapshot. */
void rbusDiagnosticsStoreSnapshot(const rbusDiagnosticsStore_t* store,
    rbusDiagnosticsGlobalSnapshot_t* snapshot);

/* PUBLIC_INTERFACE */
/** Resets interval metrics while retaining known provider registration baselines. */
void rbusDiagnosticsStoreReset(rbusDiagnosticsStore_t* store,
    rbusDiagnosticsGlobalSnapshot_t* snapshot);

/* PUBLIC_INTERFACE */
/** Copies one retained provider snapshot, returning false when it is unknown. */
bool rbusDiagnosticsStoreProviderSnapshot(const rbusDiagnosticsStore_t* store,
    const char* provider, rbusDiagnosticsGlobalSnapshot_t* global,
    rbusDiagnosticsProviderSnapshot_t* providerSnapshot);

/* PUBLIC_INTERFACE */
/** Copies retained providers in deterministic fixed-array order. */
uint32_t rbusDiagnosticsStoreProviderList(const rbusDiagnosticsStore_t* store,
    uint32_t limit, rbusDiagnosticsGlobalSnapshot_t* global,
    rbusDiagnosticsProviderSnapshot_t* providers);

/* PUBLIC_INTERFACE */
/** Copies newest-first retained slow requests without exposing arbitrary payloads. */
uint32_t rbusDiagnosticsStoreSlowRequestList(const rbusDiagnosticsStore_t* store,
    uint32_t limit, rbusDiagnosticsGlobalSnapshot_t* global,
    rbusDiagnosticsSlowRequest_t* requests);

/* PUBLIC_INTERFACE */
/** Encodes a bounded provider result or deterministic provider-list response. */
bool rbusDiagnosticsEncodeProviderResponse(rbusDiagnosticsMessageType_t type,
    const rbusDiagnosticsGlobalSnapshot_t* snapshot,
    const rbusDiagnosticsProviderSnapshot_t* providers, uint32_t providerCount,
    uint8_t* output, size_t outputCapacity, size_t* outputLength);

/* PUBLIC_INTERFACE */
/** Encodes newest-first bounded slow-request records for reporter control clients. */
bool rbusDiagnosticsEncodeSlowRequestResponse(
    const rbusDiagnosticsGlobalSnapshot_t* snapshot,
    const rbusDiagnosticsSlowRequest_t* requests, uint32_t requestCount,
    uint8_t* output, size_t outputCapacity, size_t* outputLength);

#endif
