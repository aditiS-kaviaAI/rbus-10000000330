#include "diagnostics_protocol.h"
#include "diagnostics_store.h"

#include <arpa/inet.h>
#include <string.h>

#define OBSERVATION_FRAME_SIZE 256U
#define CONTROL_FRAME_SIZE 80U
#define SNAPSHOT_HEADER_SIZE 128U
#define OPERATION_METRICS_WIRE_SIZE 64U
#define PROVIDER_RESPONSE_ENTRY_SIZE 144U
#define SLOW_REQUEST_RESPONSE_ENTRY_SIZE 224U

static uint64_t hostToNetwork64(uint64_t value)
{
    uint32_t high = htonl((uint32_t)(value >> 32));
    uint32_t low = htonl((uint32_t)value);
    return ((uint64_t)low << 32) | high;
}

static uint64_t networkToHost64(uint64_t value)
{
    uint32_t high = ntohl((uint32_t)(value >> 32));
    uint32_t low = ntohl((uint32_t)value);
    return ((uint64_t)low << 32) | high;
}

static void writeU16(uint8_t* output, uint16_t value)
{
    value = htons(value);
    memcpy(output, &value, sizeof(value));
}

static void writeU32(uint8_t* output, uint32_t value)
{
    value = htonl(value);
    memcpy(output, &value, sizeof(value));
}

static void writeU64(uint8_t* output, uint64_t value)
{
    value = hostToNetwork64(value);
    memcpy(output, &value, sizeof(value));
}

static uint16_t readU16(const uint8_t* input)
{
    uint16_t value;
    memcpy(&value, input, sizeof(value));
    return ntohs(value);
}

static uint32_t readU32(const uint8_t* input)
{
    uint32_t value;
    memcpy(&value, input, sizeof(value));
    return ntohl(value);
}

static uint64_t readU64(const uint8_t* input)
{
    uint64_t value;
    memcpy(&value, input, sizeof(value));
    return networkToHost64(value);
}

static bool isValidMessageType(uint16_t type)
{
    return type == RBUS_DIAGNOSTICS_MESSAGE_OBSERVATION ||
        type == RBUS_DIAGNOSTICS_MESSAGE_PROVIDER_LIFECYCLE ||
        type == RBUS_DIAGNOSTICS_MESSAGE_PUBLISHER_STATUS;
}

bool rbusDiagnosticsEncodeObservation(
    const rbusDiagnosticsObservation_t* observation,
    uint8_t* output,
    size_t outputCapacity,
    size_t* outputLength)
{
    if (observation == NULL || output == NULL || outputLength == NULL ||
        outputCapacity < OBSERVATION_FRAME_SIZE ||
        observation->operation >= RBUS_DIAGNOSTICS_OPERATION_COUNT ||
        observation->outcome > RBUS_DIAGNOSTICS_OUTCOME_UNKNOWN_ERROR ||
        !isValidMessageType((uint16_t)observation->messageType) ||
        observation->lifecycleAction > RBUS_DIAGNOSTICS_LIFECYCLE_REMOVED ||
        memchr(observation->provider, '\0', sizeof(observation->provider)) == NULL ||
        memchr(observation->path, '\0', sizeof(observation->path)) == NULL) {
        return false;
    }

    memset(output, 0, OBSERVATION_FRAME_SIZE);
    writeU16(output, RBUS_DIAGNOSTICS_PROTOCOL_VERSION);
    writeU16(output + 2, (uint16_t)observation->messageType);
    writeU32(output + 4, OBSERVATION_FRAME_SIZE);
    writeU64(output + 8, observation->monotonicEndUs);
    writeU64(output + 16, observation->durationUs);
    writeU64(output + 24, observation->publisherInstanceId);
    writeU64(output + 32, observation->publisherSequence);
    writeU64(output + 40, observation->cumulativeDroppedObservations);
    writeU16(output + 48, (uint16_t)observation->operation);
    writeU16(output + 50, (uint16_t)observation->outcome);
    writeU16(output + 52, (uint16_t)observation->lifecycleAction);
    memcpy(output + 56, observation->provider, sizeof(observation->provider));
    memcpy(output + 120, observation->path, sizeof(observation->path));
    *outputLength = OBSERVATION_FRAME_SIZE;
    return true;
}

bool rbusDiagnosticsDecodeObservation(
    const uint8_t* input,
    size_t inputLength,
    rbusDiagnosticsObservation_t* observation)
{
    uint16_t type;
    uint16_t operation;
    uint16_t outcome;
    uint16_t lifecycleAction;

    if (input == NULL || observation == NULL || inputLength != OBSERVATION_FRAME_SIZE ||
        readU16(input) != RBUS_DIAGNOSTICS_PROTOCOL_VERSION ||
        readU32(input + 4) != OBSERVATION_FRAME_SIZE) {
        return false;
    }

    type = readU16(input + 2);
    operation = readU16(input + 48);
    outcome = readU16(input + 50);
    lifecycleAction = readU16(input + 52);
    if (!isValidMessageType(type) || operation >= RBUS_DIAGNOSTICS_OPERATION_COUNT ||
        outcome > RBUS_DIAGNOSTICS_OUTCOME_UNKNOWN_ERROR ||
        lifecycleAction > RBUS_DIAGNOSTICS_LIFECYCLE_REMOVED ||
        memchr(input + 56, '\0', RBUS_DIAGNOSTICS_MAX_PROVIDER_NAME) == NULL ||
        memchr(input + 120, '\0', RBUS_DIAGNOSTICS_MAX_PATH_NAME) == NULL) {
        return false;
    }

    memset(observation, 0, sizeof(*observation));
    observation->monotonicEndUs = readU64(input + 8);
    observation->durationUs = readU64(input + 16);
    observation->publisherInstanceId = readU64(input + 24);
    observation->publisherSequence = readU64(input + 32);
    observation->cumulativeDroppedObservations = readU64(input + 40);
    observation->operation = (rbusDiagnosticsOperation_t)operation;
    observation->outcome = (rbusDiagnosticsOutcome_t)outcome;
    observation->messageType = (rbusDiagnosticsMessageType_t)type;
    observation->lifecycleAction = (rbusDiagnosticsLifecycleAction_t)lifecycleAction;
    memcpy(observation->provider, input + 56, sizeof(observation->provider));
    memcpy(observation->path, input + 120, sizeof(observation->path));
    return true;
}

bool rbusDiagnosticsEncodeControlRequest(
    const rbusDiagnosticsControlRequest_t* request,
    uint8_t* output,
    size_t outputCapacity,
    size_t* outputLength)
{
    if (request == NULL || output == NULL || outputLength == NULL ||
        outputCapacity < CONTROL_FRAME_SIZE ||
        request->limit > RBUS_DIAGNOSTICS_MAX_CONTROL_LIST_ENTRIES ||
        memchr(request->provider, '\0', sizeof(request->provider)) == NULL ||
        (request->type != RBUS_DIAGNOSTICS_MESSAGE_GET_GLOBAL &&
         request->type != RBUS_DIAGNOSTICS_MESSAGE_RESET &&
         request->type != RBUS_DIAGNOSTICS_MESSAGE_GET_PROVIDER &&
         request->type != RBUS_DIAGNOSTICS_MESSAGE_LIST_PROVIDERS &&
         request->type != RBUS_DIAGNOSTICS_MESSAGE_LIST_SLOW_REQUESTS) ||
        (request->type == RBUS_DIAGNOSTICS_MESSAGE_GET_PROVIDER &&
            request->provider[0] == '\0')) {
        return false;
    }

    writeU16(output, RBUS_DIAGNOSTICS_PROTOCOL_VERSION);
    writeU16(output + 2, (uint16_t)request->type);
    writeU32(output + 4, CONTROL_FRAME_SIZE);
    writeU32(output + 8, request->limit);
    memcpy(output + 12, request->provider, sizeof(request->provider));
    *outputLength = CONTROL_FRAME_SIZE;
    return true;
}

bool rbusDiagnosticsDecodeControlRequest(
    const uint8_t* input,
    size_t inputLength,
    rbusDiagnosticsControlRequest_t* request)
{
    uint16_t messageType;

    if (input == NULL || request == NULL || inputLength != CONTROL_FRAME_SIZE ||
        readU16(input) != RBUS_DIAGNOSTICS_PROTOCOL_VERSION ||
        readU32(input + 4) != CONTROL_FRAME_SIZE) {
        return false;
    }

    messageType = readU16(input + 2);
    if ((messageType != RBUS_DIAGNOSTICS_MESSAGE_GET_GLOBAL &&
         messageType != RBUS_DIAGNOSTICS_MESSAGE_RESET &&
         messageType != RBUS_DIAGNOSTICS_MESSAGE_GET_PROVIDER &&
         messageType != RBUS_DIAGNOSTICS_MESSAGE_LIST_PROVIDERS &&
         messageType != RBUS_DIAGNOSTICS_MESSAGE_LIST_SLOW_REQUESTS) ||
        readU32(input + 8) > RBUS_DIAGNOSTICS_MAX_CONTROL_LIST_ENTRIES ||
        memchr(input + 12, '\0', RBUS_DIAGNOSTICS_MAX_PROVIDER_NAME) == NULL) {
        return false;
    }

    memset(request, 0, sizeof(*request));
    request->type = (rbusDiagnosticsMessageType_t)messageType;
    request->limit = readU32(input + 8);
    memcpy(request->provider, input + 12, sizeof(request->provider));
    if (request->type == RBUS_DIAGNOSTICS_MESSAGE_GET_PROVIDER &&
        request->provider[0] == '\0') {
        return false;
    }
    return true;
}

bool rbusDiagnosticsEncodeSnapshot(
    rbusDiagnosticsMessageType_t type,
    const rbusDiagnosticsGlobalSnapshot_t* snapshot,
    uint8_t* output,
    size_t outputCapacity,
    size_t* outputLength)
{
    size_t offset;
    size_t index;
    const size_t frameSize = SNAPSHOT_HEADER_SIZE +
        (RBUS_DIAGNOSTICS_OPERATION_COUNT * OPERATION_METRICS_WIRE_SIZE);

    if (snapshot == NULL || output == NULL || outputLength == NULL ||
        outputCapacity < frameSize ||
        (type != RBUS_DIAGNOSTICS_MESSAGE_GLOBAL_SNAPSHOT &&
         type != RBUS_DIAGNOSTICS_MESSAGE_RESET_RESULT)) {
        return false;
    }

    memset(output, 0, frameSize);
    writeU16(output, RBUS_DIAGNOSTICS_PROTOCOL_VERSION);
    writeU16(output + 2, (uint16_t)type);
    writeU32(output + 4, (uint32_t)frameSize);
    writeU64(output + 8, snapshot->reporterInstanceId);
    writeU64(output + 16, snapshot->reporterStartupEpochUs);
    writeU64(output + 24, snapshot->measurementGeneration);
    writeU64(output + 32, snapshot->observationsReceived);
    writeU64(output + 40, snapshot->malformedObservationsRejected);
    writeU64(output + 48, snapshot->providerCapacityOverflows);
    writeU64(output + 56, snapshot->pathPrefixCapacityOverflows);
    writeU64(output + 64, snapshot->slowRequestOverwrites);
    writeU64(output + 72, snapshot->publisherDropDelta);
    writeU64(output + 80, snapshot->publisherStatusCapacityOverflows);
    writeU64(output + 88, snapshot->duplicateOrOutOfOrderPublisherStatuses);
    writeU32(output + 96, snapshot->durationSumSaturated ? 1U : 0U);
    writeU32(output + 100, snapshot->histogramSaturated ? 1U : 0U);
    writeU32(output + 104, snapshot->publisherDropCompletenessPartial ? 1U : 0U);

    offset = SNAPSHOT_HEADER_SIZE;
    for (index = 0; index < RBUS_DIAGNOSTICS_OPERATION_COUNT; ++index) {
        const rbusDiagnosticsOperationMetrics_t* metrics = &snapshot->operations[index];

        writeU64(output + offset, metrics->requestCount);
        writeU64(output + offset + 8, metrics->successCount);
        writeU64(output + offset + 16, metrics->timeoutCount);
        writeU64(output + offset + 24, metrics->errorCount);
        writeU64(output + offset + 32, metrics->durationSumUs);
        writeU64(output + offset + 40, metrics->minimumDurationUs);
        writeU64(output + offset + 48, metrics->maximumDurationUs);
        writeU64(output + offset + 56, metrics->histogram[0]);
        offset += OPERATION_METRICS_WIRE_SIZE;
    }

    *outputLength = frameSize;
    return true;
}

bool rbusDiagnosticsEncodeProviderResponse(
    rbusDiagnosticsMessageType_t type,
    const rbusDiagnosticsGlobalSnapshot_t* snapshot,
    const rbusDiagnosticsProviderSnapshot_t* providers,
    uint32_t providerCount,
    uint8_t* output,
    size_t outputCapacity,
    size_t* outputLength)
{
    size_t frameSize;
    uint32_t index;

    if (snapshot == NULL || output == NULL || outputLength == NULL ||
        providers == NULL || providerCount > RBUS_DIAGNOSTICS_MAX_CONTROL_LIST_ENTRIES ||
        (type != RBUS_DIAGNOSTICS_MESSAGE_PROVIDER_SNAPSHOT &&
         type != RBUS_DIAGNOSTICS_MESSAGE_PROVIDER_LIST)) {
        return false;
    }

    frameSize = SNAPSHOT_HEADER_SIZE + ((size_t)providerCount * PROVIDER_RESPONSE_ENTRY_SIZE);
    if (outputCapacity < frameSize) {
        return false;
    }
    if (!rbusDiagnosticsEncodeSnapshot(RBUS_DIAGNOSTICS_MESSAGE_GLOBAL_SNAPSHOT,
            snapshot, output, outputCapacity, outputLength)) {
        return false;
    }

    memset(output, 0, frameSize);
    writeU16(output, RBUS_DIAGNOSTICS_PROTOCOL_VERSION);
    writeU16(output + 2, (uint16_t)type);
    writeU32(output + 4, (uint32_t)frameSize);
    writeU64(output + 8, snapshot->reporterInstanceId);
    writeU64(output + 16, snapshot->reporterStartupEpochUs);
    writeU64(output + 24, snapshot->measurementGeneration);
    writeU64(output + 32, snapshot->observationsReceived);
    writeU64(output + 40, snapshot->malformedObservationsRejected);
    writeU64(output + 48, snapshot->providerCapacityOverflows);
    writeU64(output + 56, snapshot->pathPrefixCapacityOverflows);
    writeU64(output + 64, snapshot->slowRequestOverwrites);
    writeU64(output + 72, snapshot->publisherDropDelta);
    writeU64(output + 80, snapshot->publisherStatusCapacityOverflows);
    writeU64(output + 88, snapshot->duplicateOrOutOfOrderPublisherStatuses);
    writeU32(output + 96, snapshot->durationSumSaturated ? 1U : 0U);
    writeU32(output + 100, snapshot->histogramSaturated ? 1U : 0U);
    writeU32(output + 104, snapshot->publisherDropCompletenessPartial ? 1U : 0U);
    writeU32(output + 108, providerCount);

    for (index = 0; index < providerCount; ++index) {
        const rbusDiagnosticsProviderSnapshot_t* provider = &providers[index];
        uint8_t* entry = output + SNAPSHOT_HEADER_SIZE +
            ((size_t)index * PROVIDER_RESPONSE_ENTRY_SIZE);

        memcpy(entry, provider->provider, sizeof(provider->provider));
        writeU32(entry + 64, (uint32_t)provider->state);
        writeU32(entry + 68, (uint32_t)provider->registrationState);
        writeU64(entry + 72, provider->restartCount);
        writeU64(entry + 80, provider->metrics.requestCount);
        writeU64(entry + 88, provider->metrics.successCount);
        writeU64(entry + 96, provider->metrics.timeoutCount);
        writeU64(entry + 104, provider->metrics.errorCount);
        writeU64(entry + 112, provider->metrics.durationSumUs);
        writeU64(entry + 120, provider->metrics.minimumDurationUs);
        writeU64(entry + 128, provider->metrics.maximumDurationUs);
    }

    *outputLength = frameSize;
    return true;
}

bool rbusDiagnosticsEncodeSlowRequestResponse(
    const rbusDiagnosticsGlobalSnapshot_t* snapshot,
    const rbusDiagnosticsSlowRequest_t* requests,
    uint32_t requestCount,
    uint8_t* output,
    size_t outputCapacity,
    size_t* outputLength)
{
    size_t frameSize;
    uint32_t index;

    if (snapshot == NULL || output == NULL || outputLength == NULL ||
        requests == NULL || requestCount > RBUS_DIAGNOSTICS_MAX_CONTROL_LIST_ENTRIES) {
        return false;
    }

    frameSize = SNAPSHOT_HEADER_SIZE + ((size_t)requestCount * SLOW_REQUEST_RESPONSE_ENTRY_SIZE);
    if (outputCapacity < frameSize) {
        return false;
    }
    memset(output, 0, frameSize);
    writeU16(output, RBUS_DIAGNOSTICS_PROTOCOL_VERSION);
    writeU16(output + 2, RBUS_DIAGNOSTICS_MESSAGE_SLOW_REQUEST_LIST);
    writeU32(output + 4, (uint32_t)frameSize);
    writeU64(output + 8, snapshot->reporterInstanceId);
    writeU64(output + 16, snapshot->reporterStartupEpochUs);
    writeU64(output + 24, snapshot->measurementGeneration);
    writeU64(output + 32, snapshot->observationsReceived);
    writeU64(output + 40, snapshot->malformedObservationsRejected);
    writeU64(output + 48, snapshot->providerCapacityOverflows);
    writeU64(output + 56, snapshot->pathPrefixCapacityOverflows);
    writeU64(output + 64, snapshot->slowRequestOverwrites);
    writeU64(output + 72, snapshot->publisherDropDelta);
    writeU32(output + 108, requestCount);

    for (index = 0; index < requestCount; ++index) {
        const rbusDiagnosticsObservation_t* observation = &requests[index].observation;
        uint8_t* entry = output + SNAPSHOT_HEADER_SIZE +
            ((size_t)index * SLOW_REQUEST_RESPONSE_ENTRY_SIZE);

        writeU32(entry, (uint32_t)observation->operation);
        writeU32(entry + 4, (uint32_t)observation->outcome);
        writeU64(entry + 8, observation->monotonicEndUs);
        writeU64(entry + 16, observation->durationUs);
        writeU64(entry + 24, requests[index].measurementGeneration);
        memcpy(entry + 32, observation->provider, sizeof(observation->provider));
        memcpy(entry + 96, observation->path, sizeof(observation->path));
    }

    *outputLength = frameSize;
    return true;
}
