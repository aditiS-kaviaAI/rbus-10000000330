#ifndef RBUS_DIAGNOSTICS_PROTOCOL_H
#define RBUS_DIAGNOSTICS_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RBUS_DIAGNOSTICS_PROTOCOL_VERSION 1U
#define RBUS_DIAGNOSTICS_MAX_FRAME_SIZE 12288U
#define RBUS_DIAGNOSTICS_OPERATION_COUNT 8U
#define RBUS_DIAGNOSTICS_HISTOGRAM_BUCKETS 64U
#define RBUS_DIAGNOSTICS_MAX_PROVIDER_NAME 64U
#define RBUS_DIAGNOSTICS_MAX_PATH_NAME 128U
#define RBUS_DIAGNOSTICS_MAX_CONTROL_LIST_ENTRIES 32U

typedef enum {
    RBUS_DIAGNOSTICS_MESSAGE_OBSERVATION = 1,
    RBUS_DIAGNOSTICS_MESSAGE_GET_GLOBAL = 2,
    RBUS_DIAGNOSTICS_MESSAGE_RESET = 3,
    RBUS_DIAGNOSTICS_MESSAGE_GLOBAL_SNAPSHOT = 4,
    RBUS_DIAGNOSTICS_MESSAGE_RESET_RESULT = 5,
    RBUS_DIAGNOSTICS_MESSAGE_PROVIDER_LIFECYCLE = 6,
    RBUS_DIAGNOSTICS_MESSAGE_PUBLISHER_STATUS = 7,
    RBUS_DIAGNOSTICS_MESSAGE_GET_PROVIDER = 8,
    RBUS_DIAGNOSTICS_MESSAGE_LIST_PROVIDERS = 9,
    RBUS_DIAGNOSTICS_MESSAGE_LIST_SLOW_REQUESTS = 10,
    RBUS_DIAGNOSTICS_MESSAGE_PROVIDER_SNAPSHOT = 11,
    RBUS_DIAGNOSTICS_MESSAGE_PROVIDER_LIST = 12,
    RBUS_DIAGNOSTICS_MESSAGE_SLOW_REQUEST_LIST = 13,
    RBUS_DIAGNOSTICS_MESSAGE_PROVIDER_NOT_FOUND = 14
} rbusDiagnosticsMessageType_t;

typedef enum {
    RBUS_DIAGNOSTICS_OPERATION_GET = 0,
    RBUS_DIAGNOSTICS_OPERATION_SET = 1,
    RBUS_DIAGNOSTICS_OPERATION_METHOD = 2,
    RBUS_DIAGNOSTICS_OPERATION_ASYNC_METHOD = 3,
    RBUS_DIAGNOSTICS_OPERATION_EVENT_PUBLISH = 4,
    RBUS_DIAGNOSTICS_OPERATION_EVENT_SUBSCRIBE = 5,
    RBUS_DIAGNOSTICS_OPERATION_EVENT_UNSUBSCRIBE = 6,
    /*
     * The synchronous table consumer APIs share one deliberately bounded
     * category: add row, remove row, and get row names. The record contains
     * only the table or row path, never aliases or returned row data.
     */
    RBUS_DIAGNOSTICS_OPERATION_TABLE = 7
} rbusDiagnosticsOperation_t;

typedef enum {
    RBUS_DIAGNOSTICS_OUTCOME_SUCCESS = 0,
    RBUS_DIAGNOSTICS_OUTCOME_TIMEOUT = 1,
    RBUS_DIAGNOSTICS_OUTCOME_PROVIDER_UNAVAILABLE = 2,
    RBUS_DIAGNOSTICS_OUTCOME_INVALID_REQUEST = 3,
    RBUS_DIAGNOSTICS_OUTCOME_INVALID_RESPONSE = 4,
    RBUS_DIAGNOSTICS_OUTCOME_PERMISSION_DENIED = 5,
    RBUS_DIAGNOSTICS_OUTCOME_INTERNAL_ERROR = 6,
    RBUS_DIAGNOSTICS_OUTCOME_UNKNOWN_ERROR = 7
} rbusDiagnosticsOutcome_t;

typedef enum {
    RBUS_DIAGNOSTICS_LIFECYCLE_NONE = 0,
    RBUS_DIAGNOSTICS_LIFECYCLE_REGISTERED = 1,
    RBUS_DIAGNOSTICS_LIFECYCLE_REMOVED = 2
} rbusDiagnosticsLifecycleAction_t;

typedef struct {
    uint64_t monotonicEndUs;
    uint64_t durationUs;
    uint64_t publisherInstanceId;
    uint64_t publisherSequence;
    uint64_t cumulativeDroppedObservations;
    rbusDiagnosticsOperation_t operation;
    rbusDiagnosticsOutcome_t outcome;
    rbusDiagnosticsMessageType_t messageType;
    rbusDiagnosticsLifecycleAction_t lifecycleAction;
    char provider[RBUS_DIAGNOSTICS_MAX_PROVIDER_NAME];
    char path[RBUS_DIAGNOSTICS_MAX_PATH_NAME];
} rbusDiagnosticsObservation_t;

typedef struct {
    uint64_t requestCount;
    uint64_t successCount;
    uint64_t timeoutCount;
    uint64_t errorCount;
    uint64_t durationSumUs;
    uint64_t minimumDurationUs;
    uint64_t maximumDurationUs;
    uint64_t histogram[RBUS_DIAGNOSTICS_HISTOGRAM_BUCKETS];
} rbusDiagnosticsOperationMetrics_t;

typedef struct {
    uint64_t reporterInstanceId;
    uint64_t reporterStartupEpochUs;
    uint64_t measurementGeneration;
    uint64_t observationsReceived;
    uint64_t malformedObservationsRejected;
    uint64_t providerCapacityOverflows;
    uint64_t pathPrefixCapacityOverflows;
    uint64_t slowRequestOverwrites;
    uint64_t publisherDropDelta;
    uint64_t publisherStatusCapacityOverflows;
    uint64_t duplicateOrOutOfOrderPublisherStatuses;
    bool durationSumSaturated;
    bool histogramSaturated;
    bool publisherDropCompletenessPartial;
    rbusDiagnosticsOperationMetrics_t operations[RBUS_DIAGNOSTICS_OPERATION_COUNT];
} rbusDiagnosticsGlobalSnapshot_t;

typedef struct {
    rbusDiagnosticsMessageType_t type;
    uint32_t limit;
    char provider[RBUS_DIAGNOSTICS_MAX_PROVIDER_NAME];
} rbusDiagnosticsControlRequest_t;

/* Encodes or decodes only explicitly sized wire fields; C struct layout is never sent. */
bool rbusDiagnosticsEncodeObservation(const rbusDiagnosticsObservation_t* observation,
    uint8_t* output, size_t outputCapacity, size_t* outputLength);
bool rbusDiagnosticsDecodeObservation(const uint8_t* input, size_t inputLength,
    rbusDiagnosticsObservation_t* observation);

/* PUBLIC_INTERFACE */
/** Encodes a bounded authenticated-control request payload. */
bool rbusDiagnosticsEncodeControlRequest(const rbusDiagnosticsControlRequest_t* request,
    uint8_t* output, size_t outputCapacity, size_t* outputLength);

/* PUBLIC_INTERFACE */
/** Decodes and validates a bounded authenticated-control request payload. */
bool rbusDiagnosticsDecodeControlRequest(const uint8_t* input, size_t inputLength,
    rbusDiagnosticsControlRequest_t* request);

bool rbusDiagnosticsEncodeSnapshot(rbusDiagnosticsMessageType_t type,
    const rbusDiagnosticsGlobalSnapshot_t* snapshot, uint8_t* output,
    size_t outputCapacity, size_t* outputLength);

#endif
