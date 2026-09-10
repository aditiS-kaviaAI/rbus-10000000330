#include "diagnostics_protocol.h"
#include "diagnostics_store.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SNAPSHOT_HEADER_SIZE 128U
#define OPERATION_METRICS_WIRE_SIZE 64U

static char runtimeDirectory[sizeof(((struct sockaddr_un*)0)->sun_path)];
static char observationSocket[sizeof(((struct sockaddr_un*)0)->sun_path)];
static char controlSocket[sizeof(((struct sockaddr_un*)0)->sun_path)];

static uint64_t readNetworkU64(const uint8_t* input);

static void initializeTestRuntimeDirectory(void)
{
    char template[] = "/tmp/rbus-diagnostics-test-XXXXXX";

    assert(mkdtemp(template) != NULL);
    assert(snprintf(runtimeDirectory, sizeof(runtimeDirectory), "%s", template) <
        (int)sizeof(runtimeDirectory));
    assert(snprintf(observationSocket, sizeof(observationSocket), "%s/observations.sock",
        runtimeDirectory) < (int)sizeof(observationSocket));
    assert(snprintf(controlSocket, sizeof(controlSocket), "%s/control.sock",
        runtimeDirectory) < (int)sizeof(controlSocket));
    assert(setenv("RBUS_DIAGNOSTICS_TEST_RUNTIME_DIRECTORY", runtimeDirectory, 1) == 0);
}

static rbusDiagnosticsObservation_t observation(
    rbusDiagnosticsOperation_t operation,
    rbusDiagnosticsOutcome_t outcome,
    uint64_t durationUs)
{
    rbusDiagnosticsObservation_t value;

    value.monotonicEndUs = 100;
    value.durationUs = durationUs;
    value.operation = operation;
    value.outcome = outcome;
    value.messageType = RBUS_DIAGNOSTICS_MESSAGE_OBSERVATION;
    value.lifecycleAction = RBUS_DIAGNOSTICS_LIFECYCLE_NONE;
    return value;
}

static void testBoundedDetailedDiagnostics(void)
{
    rbusDiagnosticsStore_t store;
    rbusDiagnosticsGlobalSnapshot_t snapshot;
    rbusDiagnosticsObservation_t value =
        observation(RBUS_DIAGNOSTICS_OPERATION_GET,
            RBUS_DIAGNOSTICS_OUTCOME_SUCCESS,
            RBUS_DIAGNOSTICS_SLOW_REQUEST_THRESHOLD_US);

    assert(rbusDiagnosticsStoreInitialize(&store, 1, 2) == 0);
    strcpy(value.provider, "provider-a");
    strcpy(value.path, "Device.WiFi.SSID.1.Enable");
    rbusDiagnosticsStoreAccept(&store, &value);
    assert(store.slowRequestCount == 0);
    value.durationUs = RBUS_DIAGNOSTICS_SLOW_REQUEST_THRESHOLD_US + 1U;
    rbusDiagnosticsStoreAccept(&store, &value);
    assert(store.slowRequestCount == 1);
    assert(strcmp(store.pathPrefixes[0].prefix, "Device.WiFi.SSID.{i}") == 0);

    value.messageType = RBUS_DIAGNOSTICS_MESSAGE_PROVIDER_LIFECYCLE;
    value.lifecycleAction = RBUS_DIAGNOSTICS_LIFECYCLE_REGISTERED;
    rbusDiagnosticsStoreAccept(&store, &value);
    assert(store.providers[0].state == RBUS_DIAGNOSTICS_PROVIDER_STARTING);
    value.messageType = RBUS_DIAGNOSTICS_MESSAGE_OBSERVATION;
    value.lifecycleAction = RBUS_DIAGNOSTICS_LIFECYCLE_NONE;
    rbusDiagnosticsStoreAccept(&store, &value);
    assert(store.providers[0].state == RBUS_DIAGNOSTICS_PROVIDER_HEALTHY);
    value.outcome = RBUS_DIAGNOSTICS_OUTCOME_TIMEOUT;
    rbusDiagnosticsStoreAccept(&store, &value);
    assert(store.providers[0].state != RBUS_DIAGNOSTICS_PROVIDER_UNAVAILABLE);
    value.outcome = RBUS_DIAGNOSTICS_OUTCOME_PROVIDER_UNAVAILABLE;
    rbusDiagnosticsStoreAccept(&store, &value);
    assert(store.providers[0].state == RBUS_DIAGNOSTICS_PROVIDER_UNAVAILABLE);

    value.messageType = RBUS_DIAGNOSTICS_MESSAGE_PUBLISHER_STATUS;
    value.publisherInstanceId = 7;
    value.publisherSequence = 1;
    value.cumulativeDroppedObservations = 10;
    rbusDiagnosticsStoreAccept(&store, &value);
    value.publisherSequence = 2;
    value.cumulativeDroppedObservations = 13;
    rbusDiagnosticsStoreAccept(&store, &value);
    value.publisherSequence = 2;
    rbusDiagnosticsStoreAccept(&store, &value);
    rbusDiagnosticsStoreSnapshot(&store, &snapshot);
    assert(snapshot.publisherDropDelta == 3);
    assert(snapshot.duplicateOrOutOfOrderPublisherStatuses == 1);

    rbusDiagnosticsStoreReset(&store, &snapshot);
    assert(snapshot.publisherDropDelta == 0);
    assert(store.providers[0].state == RBUS_DIAGNOSTICS_PROVIDER_STARTING);
    rbusDiagnosticsStoreDestroy(&store);
}

static void testBoundedControlResponses(void)
{
    rbusDiagnosticsStore_t store;
    rbusDiagnosticsGlobalSnapshot_t global;
    rbusDiagnosticsProviderSnapshot_t providers[RBUS_DIAGNOSTICS_MAX_PROVIDERS];
    rbusDiagnosticsSlowRequest_t slowRequests[RBUS_DIAGNOSTICS_MAX_SLOW_REQUESTS];
    rbusDiagnosticsObservation_t value = observation(RBUS_DIAGNOSTICS_OPERATION_METHOD,
        RBUS_DIAGNOSTICS_OUTCOME_SUCCESS,
        RBUS_DIAGNOSTICS_SLOW_REQUEST_THRESHOLD_US + 1U);
    uint8_t frame[RBUS_DIAGNOSTICS_MAX_FRAME_SIZE];
    size_t length = 0;
    uint32_t providerCount;
    uint32_t slowRequestCount;

    assert(rbusDiagnosticsStoreInitialize(&store, 101, 202) == 0);
    strcpy(value.provider, "provider-control");
    strcpy(value.path, "Device.Diagnostics.Control");
    rbusDiagnosticsStoreAccept(&store, &value);

    providerCount = rbusDiagnosticsStoreProviderList(&store, 1, &global, providers);
    assert(providerCount == 1);
    assert(rbusDiagnosticsEncodeProviderResponse(
        RBUS_DIAGNOSTICS_MESSAGE_PROVIDER_LIST, &global, providers, providerCount,
        frame, sizeof(frame), &length));
    assert(length == SNAPSHOT_HEADER_SIZE + 144U);
    assert(readNetworkU64(frame + 8) == 101);
    assert(strcmp((const char*)(frame + SNAPSHOT_HEADER_SIZE), "provider-control") == 0);

    slowRequestCount = rbusDiagnosticsStoreSlowRequestList(&store, 1, &global, slowRequests);
    assert(slowRequestCount == 1);
    assert(rbusDiagnosticsEncodeSlowRequestResponse(&global, slowRequests, slowRequestCount,
        frame, sizeof(frame), &length));
    assert(length == SNAPSHOT_HEADER_SIZE + 224U);
    assert(readNetworkU64(frame + SNAPSHOT_HEADER_SIZE + 16) ==
        RBUS_DIAGNOSTICS_SLOW_REQUEST_THRESHOLD_US + 1U);
    rbusDiagnosticsStoreDestroy(&store);
}

static void testControlRequestBoundsAndOperations(void)
{
    rbusDiagnosticsControlRequest_t request;
    rbusDiagnosticsControlRequest_t decoded;
    uint8_t frame[RBUS_DIAGNOSTICS_MAX_FRAME_SIZE];
    size_t length = 0;

    memset(&request, 0, sizeof(request));
    request.type = RBUS_DIAGNOSTICS_MESSAGE_GET_GLOBAL;
    assert(rbusDiagnosticsEncodeControlRequest(&request, frame, sizeof(frame), &length));
    assert(rbusDiagnosticsDecodeControlRequest(frame, length, &decoded));
    assert(decoded.type == RBUS_DIAGNOSTICS_MESSAGE_GET_GLOBAL);

    request.type = RBUS_DIAGNOSTICS_MESSAGE_RESET;
    assert(rbusDiagnosticsEncodeControlRequest(&request, frame, sizeof(frame), &length));
    assert(rbusDiagnosticsDecodeControlRequest(frame, length, &decoded));
    assert(decoded.type == RBUS_DIAGNOSTICS_MESSAGE_RESET);

    request.type = RBUS_DIAGNOSTICS_MESSAGE_GET_PROVIDER;
    strcpy(request.provider, "provider-control");
    assert(rbusDiagnosticsEncodeControlRequest(&request, frame, sizeof(frame), &length));
    assert(rbusDiagnosticsDecodeControlRequest(frame, length, &decoded));
    assert(decoded.type == RBUS_DIAGNOSTICS_MESSAGE_GET_PROVIDER);
    assert(strcmp(decoded.provider, "provider-control") == 0);

    memset(&request, 0, sizeof(request));
    request.type = RBUS_DIAGNOSTICS_MESSAGE_LIST_PROVIDERS;
    request.limit = RBUS_DIAGNOSTICS_MAX_CONTROL_LIST_ENTRIES;
    assert(rbusDiagnosticsEncodeControlRequest(&request, frame, sizeof(frame), &length));
    assert(rbusDiagnosticsDecodeControlRequest(frame, length, &decoded));
    assert(decoded.type == RBUS_DIAGNOSTICS_MESSAGE_LIST_PROVIDERS);
    assert(decoded.limit == RBUS_DIAGNOSTICS_MAX_CONTROL_LIST_ENTRIES);

    request.type = RBUS_DIAGNOSTICS_MESSAGE_LIST_SLOW_REQUESTS;
    request.limit = 1;
    assert(rbusDiagnosticsEncodeControlRequest(&request, frame, sizeof(frame), &length));
    assert(rbusDiagnosticsDecodeControlRequest(frame, length, &decoded));
    assert(decoded.type == RBUS_DIAGNOSTICS_MESSAGE_LIST_SLOW_REQUESTS);
    assert(decoded.limit == 1);

    request.limit = RBUS_DIAGNOSTICS_MAX_CONTROL_LIST_ENTRIES + 1U;
    assert(!rbusDiagnosticsEncodeControlRequest(&request, frame, sizeof(frame), &length));
    request.type = RBUS_DIAGNOSTICS_MESSAGE_GET_PROVIDER;
    request.limit = 0;
    assert(!rbusDiagnosticsEncodeControlRequest(&request, frame, sizeof(frame), &length));

    memset(&request, 0, sizeof(request));
    request.type = RBUS_DIAGNOSTICS_MESSAGE_GET_GLOBAL;
    assert(rbusDiagnosticsEncodeControlRequest(&request, frame, sizeof(frame), &length));
    frame[3] = 99;
    assert(!rbusDiagnosticsDecodeControlRequest(frame, length, &decoded));
}

static void acceptEncoded(rbusDiagnosticsStore_t* store,
    const rbusDiagnosticsObservation_t* value)
{
    uint8_t frame[RBUS_DIAGNOSTICS_MAX_FRAME_SIZE];
    size_t length = 0;
    rbusDiagnosticsObservation_t decoded;

    assert(rbusDiagnosticsEncodeObservation(value, frame, sizeof(frame), &length));
    assert(rbusDiagnosticsDecodeObservation(frame, length, &decoded));
    rbusDiagnosticsStoreAccept(store, &decoded);
}

static uint64_t readNetworkU64(const uint8_t* input)
{
    uint32_t high;
    uint32_t low;

    memcpy(&high, input, sizeof(high));
    memcpy(&low, input + sizeof(high), sizeof(low));
    return ((uint64_t)ntohl(high) << 32) | ntohl(low);
}

static void testProtocolRejectsMalformedAndIncompatible(void)
{
    rbusDiagnosticsObservation_t decoded;
    rbusDiagnosticsObservation_t value =
        observation(RBUS_DIAGNOSTICS_OPERATION_GET, RBUS_DIAGNOSTICS_OUTCOME_SUCCESS, 1);
    uint8_t frame[RBUS_DIAGNOSTICS_MAX_FRAME_SIZE];
    size_t length = 0;

    assert(rbusDiagnosticsEncodeObservation(&value, frame, sizeof(frame), &length));
    assert(!rbusDiagnosticsDecodeObservation(frame, length - 1, &decoded));
    frame[1] = 2;
    assert(!rbusDiagnosticsDecodeObservation(frame, length, &decoded));
}

static void testSnapshotsAndReset(void)
{
    rbusDiagnosticsStore_t store;
    rbusDiagnosticsGlobalSnapshot_t before;
    rbusDiagnosticsGlobalSnapshot_t after;
    rbusDiagnosticsObservation_t value =
        observation(RBUS_DIAGNOSTICS_OPERATION_GET, RBUS_DIAGNOSTICS_OUTCOME_TIMEOUT, 4);

    assert(rbusDiagnosticsStoreInitialize(&store, 11, 22) == 0);
    acceptEncoded(&store, &value);
    rbusDiagnosticsStoreSnapshot(&store, &before);
    assert(before.reporterInstanceId == 11);
    assert(before.measurementGeneration == 1);
    assert(before.operations[RBUS_DIAGNOSTICS_OPERATION_GET].requestCount == 1);
    assert(before.operations[RBUS_DIAGNOSTICS_OPERATION_GET].timeoutCount == 1);

    rbusDiagnosticsStoreReset(&store, &after);
    assert(after.reporterInstanceId == 11);
    assert(after.measurementGeneration == 2);
    assert(after.operations[RBUS_DIAGNOSTICS_OPERATION_GET].requestCount == 0);
    assert(before.operations[RBUS_DIAGNOSTICS_OPERATION_GET].requestCount == 1);
    rbusDiagnosticsStoreDestroy(&store);
}

static void testTwoIndependentPublishers(void)
{
    int pipeDescriptors[2];
    pid_t first;
    pid_t second;
    rbusDiagnosticsStore_t store;
    rbusDiagnosticsObservation_t value;
    rbusDiagnosticsGlobalSnapshot_t snapshot;

    assert(pipe(pipeDescriptors) == 0);
    assert(rbusDiagnosticsStoreInitialize(&store, 33, 44) == 0);
    first = fork();
    assert(first >= 0);
    if (first == 0) {
        value = observation(RBUS_DIAGNOSTICS_OPERATION_GET,
            RBUS_DIAGNOSTICS_OUTCOME_SUCCESS, 8);
        (void)write(pipeDescriptors[1], &value, sizeof(value));
        _exit(0);
    }
    second = fork();
    assert(second >= 0);
    if (second == 0) {
        value = observation(RBUS_DIAGNOSTICS_OPERATION_METHOD,
            RBUS_DIAGNOSTICS_OUTCOME_INTERNAL_ERROR, 16);
        (void)write(pipeDescriptors[1], &value, sizeof(value));
        _exit(0);
    }

    close(pipeDescriptors[1]);
    assert(read(pipeDescriptors[0], &value, sizeof(value)) == (ssize_t)sizeof(value));
    rbusDiagnosticsStoreAccept(&store, &value);
    assert(read(pipeDescriptors[0], &value, sizeof(value)) == (ssize_t)sizeof(value));
    rbusDiagnosticsStoreAccept(&store, &value);
    assert(waitpid(first, NULL, 0) == first);
    assert(waitpid(second, NULL, 0) == second);

    rbusDiagnosticsStoreSnapshot(&store, &snapshot);
    assert(snapshot.observationsReceived == 2);
    assert(snapshot.operations[RBUS_DIAGNOSTICS_OPERATION_GET].requestCount == 1);
    assert(snapshot.operations[RBUS_DIAGNOSTICS_OPERATION_METHOD].requestCount == 1);
    rbusDiagnosticsStoreDestroy(&store);
}

static void waitForSocket(const char* path)
{
    struct stat metadata;
    struct timespec pause = { 0, 10000000L };
    unsigned int attempt;

    for (attempt = 0; attempt < 500; ++attempt) {
        if (stat(path, &metadata) == 0 && S_ISSOCK(metadata.st_mode)) {
            return;
        }
        nanosleep(&pause, NULL);
    }
    assert(!"timed out waiting for reporter socket");
}

static void sendObservationToReporter(const rbusDiagnosticsObservation_t* value)
{
    int descriptor;
    struct sockaddr_un address;
    uint8_t frame[RBUS_DIAGNOSTICS_MAX_FRAME_SIZE];
    size_t length = 0;

    assert(rbusDiagnosticsEncodeObservation(value, frame, sizeof(frame), &length));
    descriptor = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    assert(descriptor >= 0);

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, observationSocket, sizeof(address.sun_path) - 1);
    assert(sendto(descriptor, frame, length, 0, (const struct sockaddr*)&address,
        sizeof(address)) == (ssize_t)length);
    close(descriptor);
}

static void publishFromChild(rbusDiagnosticsOperation_t operation,
    rbusDiagnosticsOutcome_t outcome, uint64_t durationUs)
{
    pid_t publisher = fork();

    assert(publisher >= 0);
    if (publisher == 0) {
        rbusDiagnosticsObservation_t value = observation(operation, outcome, durationUs);

        sendObservationToReporter(&value);
        _exit(0);
    }
    assert(waitpid(publisher, NULL, 0) == publisher);
}

static void testLiveReporterSocketAggregation(const char* reporterPath)
{
    int descriptor;
    struct sockaddr_un address;
    char authorizedUid[32];
    uint8_t request[RBUS_DIAGNOSTICS_MAX_FRAME_SIZE];
    uint8_t response[RBUS_DIAGNOSTICS_MAX_FRAME_SIZE];
    size_t requestLength = 0;
    ssize_t responseLength;
    pid_t reporter;
    uint64_t instanceId;
    uint64_t generation;
    rbusDiagnosticsControlRequest_t globalRequest;
    const size_t getOffset = SNAPSHOT_HEADER_SIZE +
        (RBUS_DIAGNOSTICS_OPERATION_GET * OPERATION_METRICS_WIRE_SIZE);
    const size_t methodOffset = SNAPSHOT_HEADER_SIZE +
        (RBUS_DIAGNOSTICS_OPERATION_METHOD * OPERATION_METRICS_WIRE_SIZE);

    assert(reporterPath != NULL && reporterPath[0] != '\0');
    initializeTestRuntimeDirectory();
    assert(snprintf(authorizedUid, sizeof(authorizedUid), "%lu",
        (unsigned long)getuid()) < (int)sizeof(authorizedUid));

    reporter = fork();
    assert(reporter >= 0);
    if (reporter == 0) {
        execl(reporterPath, reporterPath, "--thunder-uid", authorizedUid, (char*)NULL);
        _exit(127);
    }

    waitForSocket(observationSocket);
    waitForSocket(controlSocket);
    publishFromChild(RBUS_DIAGNOSTICS_OPERATION_GET,
        RBUS_DIAGNOSTICS_OUTCOME_SUCCESS, 8);
    publishFromChild(RBUS_DIAGNOSTICS_OPERATION_METHOD,
        RBUS_DIAGNOSTICS_OUTCOME_INTERNAL_ERROR, 16);

    memset(&globalRequest, 0, sizeof(globalRequest));
    globalRequest.type = RBUS_DIAGNOSTICS_MESSAGE_GET_GLOBAL;
    assert(rbusDiagnosticsEncodeControlRequest(&globalRequest, request, sizeof(request),
        &requestLength));
    descriptor = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    assert(descriptor >= 0);

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, controlSocket, sizeof(address.sun_path) - 1);
    assert(connect(descriptor, (const struct sockaddr*)&address, sizeof(address)) == 0);
    assert(send(descriptor, request, requestLength, MSG_NOSIGNAL) == (ssize_t)requestLength);
    responseLength = recv(descriptor, response, sizeof(response), 0);
    assert(responseLength >= (ssize_t)(SNAPSHOT_HEADER_SIZE +
        (RBUS_DIAGNOSTICS_OPERATION_COUNT * OPERATION_METRICS_WIRE_SIZE)));
    close(descriptor);

    assert(ntohs(*(uint16_t*)response) == RBUS_DIAGNOSTICS_PROTOCOL_VERSION);
    instanceId = readNetworkU64(response + 8);
    generation = readNetworkU64(response + 24);
    assert(instanceId != 0);
    assert(generation == 1);
    assert(readNetworkU64(response + 32) == 2);
    assert(readNetworkU64(response + getOffset) == 1);
    assert(readNetworkU64(response + methodOffset) == 1);

    assert(kill(reporter, SIGTERM) == 0);
    assert(waitpid(reporter, NULL, 0) == reporter);
    unlink(observationSocket);
    unlink(controlSocket);
    assert(rmdir(runtimeDirectory) == 0);
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    testProtocolRejectsMalformedAndIncompatible();
    testSnapshotsAndReset();
    testTwoIndependentPublishers();
    testBoundedDetailedDiagnostics();
    testBoundedControlResponses();
    testControlRequestBoundsAndOperations();
    testLiveReporterSocketAggregation(argv[1]);
    return 0;
}
