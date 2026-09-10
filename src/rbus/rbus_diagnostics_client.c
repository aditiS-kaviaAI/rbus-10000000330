#define _POSIX_C_SOURCE 200809L

#include "rbus_diagnostics.h"
#include "rbus_diagnostics_client.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define RBUS_DIAGNOSTICS_OBSERVATION_SOCKET \
    "/run/rbus-diagnostics/observations.sock"
#define RBUS_DIAGNOSTICS_QUEUE_CAPACITY 32U
#define RBUS_DIAGNOSTICS_STATUS_INTERVAL 32U

typedef struct
{
    int socketFd;
    uint64_t publisherInstanceId;
    uint64_t nextSequence;
    uint64_t cumulativeDroppedObservations;
    uint64_t submissionsSinceStatus;
    bool enabledHint;
    char endpoint[sizeof(((struct sockaddr_un*)0)->sun_path)];
    rbusDiagnosticsObservation_t queue[RBUS_DIAGNOSTICS_QUEUE_CAPACITY];
    size_t queueHead;
    size_t queueCount;
} rbusDiagnosticsPublisher_t;

/*
 * All publisher state is process-local and bounded. The try-lock prevents a
 * congested diagnostics path from waiting behind another RBus request.
 */
static rbusDiagnosticsPublisher_t sPublisher = {
    .socketFd = -1,
    .enabledHint = true,
    .endpoint = RBUS_DIAGNOSTICS_OBSERVATION_SOCKET
};
static volatile int sPublisherLock = 0;

static bool publisherTryLock(void)
{
    return __sync_lock_test_and_set(&sPublisherLock, 1) == 0;
}

static void publisherUnlock(void)
{
    __sync_lock_release(&sPublisherLock);
}

static uint64_t createPublisherInstanceId(void)
{
    struct timespec timestamp;
    uint64_t value = (uint64_t)getpid();

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) == 0) {
        value ^= ((uint64_t)timestamp.tv_sec << 32);
        value ^= (uint64_t)timestamp.tv_nsec;
    }

    return value == 0 ? 1 : value;
}

static bool ensureSocket(rbusDiagnosticsPublisher_t* publisher)
{
    if (publisher->socketFd >= 0) {
        return true;
    }

    publisher->socketFd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    return publisher->socketFd >= 0;
}

static bool sendObservation(
    rbusDiagnosticsPublisher_t* publisher,
    const rbusDiagnosticsObservation_t* observation)
{
    struct sockaddr_un endpoint;
    uint8_t frame[RBUS_DIAGNOSTICS_MAX_FRAME_SIZE];
    size_t frameLength;
    ssize_t sent;

    if (!rbusDiagnosticsEncodeObservation(
            observation, frame, sizeof(frame), &frameLength) ||
        !ensureSocket(publisher)) {
        return false;
    }

    memset(&endpoint, 0, sizeof(endpoint));
    endpoint.sun_family = AF_UNIX;
    if (strlen(publisher->endpoint) >= sizeof(endpoint.sun_path)) {
        return false;
    }

    memcpy(endpoint.sun_path, publisher->endpoint, strlen(publisher->endpoint) + 1U);
    sent = sendto(
        publisher->socketFd,
        frame,
        frameLength,
        MSG_DONTWAIT | MSG_NOSIGNAL,
        (const struct sockaddr*)&endpoint,
        sizeof(endpoint));

    if (sent == (ssize_t)frameLength) {
        return true;
    }

    /*
     * A missing reporter can leave a datagram socket in a failed state.
     * Recreate it on a later best-effort attempt, never in a retry loop.
     */
    if (errno == ECONNREFUSED || errno == ENOENT || errno == ENOTCONN) {
        close(publisher->socketFd);
        publisher->socketFd = -1;
    }

    return false;
}

static void flushOneQueuedObservation(rbusDiagnosticsPublisher_t* publisher)
{
    rbusDiagnosticsObservation_t* queued;

    if (publisher->queueCount == 0) {
        return;
    }

    queued = &publisher->queue[publisher->queueHead];
    if (!sendObservation(publisher, queued)) {
        return;
    }

    publisher->queueHead =
        (publisher->queueHead + 1U) % RBUS_DIAGNOSTICS_QUEUE_CAPACITY;
    --publisher->queueCount;
}

static void initializeObservationIdentity(
    rbusDiagnosticsPublisher_t* publisher,
    rbusDiagnosticsObservation_t* observation)
{
    if (publisher->publisherInstanceId == 0) {
        publisher->publisherInstanceId = createPublisherInstanceId();
        publisher->nextSequence = 1;
    }

    observation->publisherInstanceId = publisher->publisherInstanceId;
    observation->publisherSequence = publisher->nextSequence++;
    observation->cumulativeDroppedObservations =
        publisher->cumulativeDroppedObservations;
}

static void sendPublisherStatus(rbusDiagnosticsPublisher_t* publisher)
{
    rbusDiagnosticsObservation_t status;

    if (++publisher->submissionsSinceStatus < RBUS_DIAGNOSTICS_STATUS_INTERVAL) {
        return;
    }

    publisher->submissionsSinceStatus = 0;
    memset(&status, 0, sizeof(status));
    status.operation = RBUS_DIAGNOSTICS_OPERATION_GET;
    status.outcome = RBUS_DIAGNOSTICS_OUTCOME_SUCCESS;
    status.messageType = RBUS_DIAGNOSTICS_MESSAGE_PUBLISHER_STATUS;
    status.lifecycleAction = RBUS_DIAGNOSTICS_LIFECYCLE_NONE;
    initializeObservationIdentity(publisher, &status);

    /*
     * A status is best effort only: failure neither enters the observation
     * queue nor increases the drop count, preventing recursive loss reports.
     */
    (void)sendObservation(publisher, &status);
}

uint64_t rbusDiagnosticsCaptureStartUs(void)
{
    struct timespec timestamp;

    if (!rbusDiagnosticsClientIsEnabled() ||
        clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
        return 0;
    }

    return ((uint64_t)timestamp.tv_sec * UINT64_C(1000000)) +
        ((uint64_t)timestamp.tv_nsec / UINT64_C(1000));
}

rbusDiagnosticsOutcome_t rbusDiagnosticsClassifyOutcome(rbusError_t result)
{
    switch (result) {
        case RBUS_ERROR_SUCCESS:
            return RBUS_DIAGNOSTICS_OUTCOME_SUCCESS;
        case RBUS_ERROR_TIMEOUT:
            return RBUS_DIAGNOSTICS_OUTCOME_TIMEOUT;
        case RBUS_ERROR_DESTINATION_NOT_FOUND:
        case RBUS_ERROR_DESTINATION_NOT_REACHABLE:
            return RBUS_DIAGNOSTICS_OUTCOME_PROVIDER_UNAVAILABLE;
        case RBUS_ERROR_INVALID_RESPONSE_FROM_DESTINATION:
            return RBUS_DIAGNOSTICS_OUTCOME_INVALID_RESPONSE;
        case RBUS_ERROR_ACCESS_NOT_ALLOWED:
        case RBUS_ERROR_NOT_WRITABLE:
        case RBUS_ERROR_NOT_READABLE:
            return RBUS_DIAGNOSTICS_OUTCOME_PERMISSION_DENIED;
        case RBUS_ERROR_INVALID_INPUT:
        case RBUS_ERROR_INVALID_OPERATION:
        case RBUS_ERROR_INVALID_EVENT:
        case RBUS_ERROR_INVALID_HANDLE:
        case RBUS_ERROR_INVALID_CONTEXT:
        case RBUS_ERROR_INVALID_METHOD:
        case RBUS_ERROR_INVALID_NAMESPACE:
        case RBUS_ERROR_INVALID_PARAMETER_TYPE:
        case RBUS_ERROR_INVALID_PARAMETER_VALUE:
        case RBUS_ERROR_ELEMENT_NAME_MISSING:
            return RBUS_DIAGNOSTICS_OUTCOME_INVALID_REQUEST;
        case RBUS_ERROR_BUS_ERROR:
        case RBUS_ERROR_NOT_INITIALIZED:
        case RBUS_ERROR_OUT_OF_RESOURCES:
        case RBUS_ERROR_DESTINATION_RESPONSE_FAILURE:
        case RBUS_ERROR_SESSION_ALREADY_EXIST:
        case RBUS_ERROR_COMPONENT_NAME_DUPLICATE:
        case RBUS_ERROR_ELEMENT_NAME_DUPLICATE:
        case RBUS_ERROR_COMPONENT_DOES_NOT_EXIST:
        case RBUS_ERROR_ELEMENT_DOES_NOT_EXIST:
        case RBUS_ERROR_DIRECT_CON_NOT_EXIST:
        case RBUS_ERROR_SUBSCRIPTION_ALREADY_EXIST:
            return RBUS_DIAGNOSTICS_OUTCOME_INTERNAL_ERROR;
        case RBUS_ERROR_ASYNC_RESPONSE:
        case RBUS_ERROR_NOSUBSCRIBERS:
        default:
            return RBUS_DIAGNOSTICS_OUTCOME_UNKNOWN_ERROR;
    }
}

bool rbusDiagnosticsClientIsEnabled(void)
{
    return __atomic_load_n(&sPublisher.enabledHint, __ATOMIC_RELAXED);
}

void rbusDiagnosticsClientResetForTest(void)
{
    if (!publisherTryLock()) {
        return;
    }

    if (sPublisher.socketFd >= 0) {
        close(sPublisher.socketFd);
    }

    memset(&sPublisher, 0, sizeof(sPublisher));
    sPublisher.socketFd = -1;
    sPublisher.enabledHint = true;
    memcpy(
        sPublisher.endpoint,
        RBUS_DIAGNOSTICS_OBSERVATION_SOCKET,
        sizeof(RBUS_DIAGNOSTICS_OBSERVATION_SOCKET));
    publisherUnlock();
}

void rbusDiagnosticsClientSetEnabledForTest(bool enabled)
{
    __atomic_store_n(&sPublisher.enabledHint, enabled, __ATOMIC_RELAXED);
}

bool rbusDiagnosticsClientSetEndpointForTest(const char* endpoint)
{
    bool configured = false;

    if (endpoint == NULL || strlen(endpoint) >= sizeof(sPublisher.endpoint) ||
        !publisherTryLock()) {
        return false;
    }

    if (sPublisher.socketFd >= 0) {
        close(sPublisher.socketFd);
        sPublisher.socketFd = -1;
    }
    memcpy(sPublisher.endpoint, endpoint, strlen(endpoint) + 1U);
    configured = true;
    publisherUnlock();
    return configured;
}

uint64_t rbusDiagnosticsClientGetDroppedObservationsForTest(void)
{
    return __atomic_load_n(
        &sPublisher.cumulativeDroppedObservations, __ATOMIC_RELAXED);
}

bool rbusDiagnosticsClientSubmit(
    const rbusDiagnosticsObservation_t* observation)
{
    size_t queueTail;
    bool sent = false;
    rbusDiagnosticsObservation_t submitted;

    if (observation == NULL || !rbusDiagnosticsClientIsEnabled()) {
        return false;
    }

    if (!publisherTryLock()) {
        __sync_fetch_and_add(&sPublisher.cumulativeDroppedObservations, 1ULL);
        return false;
    }

    /*
     * A single opportunistic flush bounds work on the request path. No normal
     * RBus operation waits for the reporter or performs synchronous retries.
     */
    flushOneQueuedObservation(&sPublisher);
    submitted = *observation;
    initializeObservationIdentity(&sPublisher, &submitted);
    sent = sendObservation(&sPublisher, &submitted);

    if (!sent && sPublisher.queueCount < RBUS_DIAGNOSTICS_QUEUE_CAPACITY) {
        queueTail = (sPublisher.queueHead + sPublisher.queueCount) %
            RBUS_DIAGNOSTICS_QUEUE_CAPACITY;
        sPublisher.queue[queueTail] = submitted;
        ++sPublisher.queueCount;
    } else if (!sent) {
        ++sPublisher.cumulativeDroppedObservations;
    }

    if (submitted.messageType != RBUS_DIAGNOSTICS_MESSAGE_PUBLISHER_STATUS) {
        sendPublisherStatus(&sPublisher);
    }

    publisherUnlock();
    return sent;
}

void rbusDiagnosticsPublishCompleted(
    rbusDiagnosticsOperation_t operation,
    uint64_t startUs,
    rbusError_t result)
{
    rbusDiagnosticsPublishCompletedForPath(operation, startUs, result, NULL);
}

void rbusDiagnosticsPublishCompletedForPath(
    rbusDiagnosticsOperation_t operation,
    uint64_t startUs,
    rbusError_t result,
    const char* path)
{
    rbusDiagnosticsObservation_t observation;
    uint64_t endUs;

    if (startUs == 0 || !rbusDiagnosticsClientIsEnabled()) {
        return;
    }

    endUs = rbusDiagnosticsCaptureStartUs();
    if (endUs == 0) {
        return;
    }

    memset(&observation, 0, sizeof(observation));
    observation.monotonicEndUs = endUs;
    observation.durationUs = endUs >= startUs ? endUs - startUs : 0;
    observation.operation = operation;
    observation.outcome = rbusDiagnosticsClassifyOutcome(result);
    observation.messageType = RBUS_DIAGNOSTICS_MESSAGE_OBSERVATION;
    observation.lifecycleAction = RBUS_DIAGNOSTICS_LIFECYCLE_NONE;
    if (path != NULL) {
        strncpy(observation.path, path, sizeof(observation.path) - 1U);
    }
    (void)rbusDiagnosticsClientSubmit(&observation);
}

void rbusDiagnosticsPublishProviderLifecycle(
    const char* provider,
    rbusDiagnosticsLifecycleAction_t action)
{
    rbusDiagnosticsObservation_t observation;

    if (provider == NULL || provider[0] == '\0' || !rbusDiagnosticsClientIsEnabled() ||
        action == RBUS_DIAGNOSTICS_LIFECYCLE_NONE) {
        return;
    }

    memset(&observation, 0, sizeof(observation));
    observation.operation = RBUS_DIAGNOSTICS_OPERATION_GET;
    observation.outcome = RBUS_DIAGNOSTICS_OUTCOME_SUCCESS;
    observation.messageType = RBUS_DIAGNOSTICS_MESSAGE_PROVIDER_LIFECYCLE;
    observation.lifecycleAction = action;
    strncpy(observation.provider, provider, sizeof(observation.provider) - 1U);
    (void)rbusDiagnosticsClientSubmit(&observation);
}
