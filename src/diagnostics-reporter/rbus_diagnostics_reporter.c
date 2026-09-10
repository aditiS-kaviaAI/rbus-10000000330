#include "../rbus/rbus_diagnostics_client.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef RBUS_DIAGNOSTICS_OBSERVATION_SOCKET
#define RBUS_DIAGNOSTICS_OBSERVATION_SOCKET "/tmp/rbus-diagnostics-observations.sock"
#endif

#ifndef RBUS_DIAGNOSTICS_CONTROL_SOCKET
#define RBUS_DIAGNOSTICS_CONTROL_SOCKET "/tmp/rbus-diagnostics-control.sock"
#endif

#define RBUS_DIAGNOSTICS_OPERATION_COUNT 3U
#define RBUS_DIAGNOSTICS_CONTROL_REQUEST_MAX 64U
#define RBUS_DIAGNOSTICS_CONTROL_RESPONSE_MAX 1024U
#define RBUS_DIAGNOSTICS_CONTROL_CLIENT_IDLE_TIMEOUT_US UINT64_C(30000000)

typedef struct {
    uint64_t requestCount;
    uint64_t successCount;
    uint64_t errorCount;
    uint64_t timeoutCount;
    uint64_t totalLatencyUs;
    uint64_t minLatencyUs;
    uint64_t maxLatencyUs;
} metrics_t;

typedef struct {
    uint64_t generation;
    uint64_t startupEpochUs;
    uint64_t observationsReceived;
    uint64_t malformedObservationsRejected;
    metrics_t operations[RBUS_DIAGNOSTICS_OPERATION_COUNT];
} reporterStore_t;

typedef struct {
    int fd;
    char request[RBUS_DIAGNOSTICS_CONTROL_REQUEST_MAX];
    size_t requestLength;
    char response[RBUS_DIAGNOSTICS_CONTROL_RESPONSE_MAX];
    size_t responseLength;
    size_t responseSent;
    bool responseReady;
    uint64_t lastActivityMonotonicUs;
} controlClient_t;

static volatile sig_atomic_t keepRunning = 1;

static void stopReporter(int signalNumber)
{
    (void)signalNumber;
    keepRunning = 0;
}

static uint64_t epochUs(void)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_REALTIME, &timestamp) != 0) {
        return 0;
    }

    return ((uint64_t)timestamp.tv_sec * UINT64_C(1000000))
        + ((uint64_t)timestamp.tv_nsec / UINT64_C(1000));
}

/*
 * Uses a monotonic clock so control-client expiry is unaffected by wall-clock
 * adjustments while the reporter is running.
 */
static uint64_t monotonicUs(void)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
        return 0;
    }

    return ((uint64_t)timestamp.tv_sec * UINT64_C(1000000))
        + ((uint64_t)timestamp.tv_nsec / UINT64_C(1000));
}

static void markControlClientActivity(controlClient_t* client)
{
    client->lastActivityMonotonicUs = monotonicUs();
}

/*
 * Queues a bounded response for incremental delivery. The event loop sends it
 * only when the client is writable, so a slow peer never blocks aggregation.
 */
static void queueControlResponse(controlClient_t* client, const char* response)
{
    int written = snprintf(client->response, sizeof(client->response), "%s", response);

    if (written < 0) {
        client->responseLength = 0;
    } else if ((size_t)written >= sizeof(client->response)) {
        client->responseLength = sizeof(client->response) - 1;
    } else {
        client->responseLength = (size_t)written;
    }

    client->responseSent = 0;
    client->responseReady = true;
}

static void resetStore(reporterStore_t* store)
{
    uint64_t startupEpochUs = store->startupEpochUs;
    uint64_t generation = store->generation + 1;

    memset(store, 0, sizeof(*store));
    store->startupEpochUs = startupEpochUs;
    store->generation = generation;
}

static void processObservation(reporterStore_t* store, const rbusDiagnosticsObservation_t* observation)
{
    metrics_t* metrics;
    uint16_t operationIndex;

    if (observation->protocolVersion != RBUS_DIAGNOSTICS_PROTOCOL_VERSION
        || observation->operation < RBUS_DIAGNOSTICS_OPERATION_GET
        || observation->operation > RBUS_DIAGNOSTICS_OPERATION_METHOD
        || observation->outcome > RBUS_DIAGNOSTICS_OUTCOME_UNKNOWN_ERROR) {
        store->malformedObservationsRejected++;
        return;
    }

    operationIndex = (uint16_t)(observation->operation - RBUS_DIAGNOSTICS_OPERATION_GET);
    metrics = &store->operations[operationIndex];
    metrics->requestCount++;
    metrics->totalLatencyUs += observation->durationUs;

    if (metrics->requestCount == 1 || observation->durationUs < metrics->minLatencyUs) {
        metrics->minLatencyUs = observation->durationUs;
    }
    if (observation->durationUs > metrics->maxLatencyUs) {
        metrics->maxLatencyUs = observation->durationUs;
    }

    if (observation->outcome == RBUS_DIAGNOSTICS_OUTCOME_SUCCESS) {
        metrics->successCount++;
    } else if (observation->outcome == RBUS_DIAGNOSTICS_OUTCOME_TIMEOUT) {
        metrics->timeoutCount++;
    } else {
        metrics->errorCount++;
    }

    store->observationsReceived++;
}

static void queueGlobalSnapshot(controlClient_t* client, const reporterStore_t* store)
{
    uint64_t requestCount = 0;
    uint64_t successCount = 0;
    uint64_t errorCount = 0;
    uint64_t timeoutCount = 0;
    uint64_t totalLatencyUs = 0;
    uint64_t averageLatencyUs;
    uint64_t minimumLatencyUs = 0;
    uint64_t maximumLatencyUs = 0;
    char response[RBUS_DIAGNOSTICS_CONTROL_RESPONSE_MAX];
    size_t index;

    for (index = 0; index < RBUS_DIAGNOSTICS_OPERATION_COUNT; ++index) {
        const metrics_t* metrics = &store->operations[index];

        requestCount += metrics->requestCount;
        successCount += metrics->successCount;
        errorCount += metrics->errorCount;
        timeoutCount += metrics->timeoutCount;
        totalLatencyUs += metrics->totalLatencyUs;

        if (metrics->requestCount != 0) {
            if (minimumLatencyUs == 0 || metrics->minLatencyUs < minimumLatencyUs) {
                minimumLatencyUs = metrics->minLatencyUs;
            }
            if (metrics->maxLatencyUs > maximumLatencyUs) {
                maximumLatencyUs = metrics->maxLatencyUs;
            }
        }
    }

    averageLatencyUs = requestCount == 0 ? 0 : totalLatencyUs / requestCount;
    (void)snprintf(
        response,
        sizeof(response),
        "{\"reporterInstanceId\":\"rbus-diagnostics-reporter\","
        "\"reporterStartupEpochUs\":%llu,\"measurementGeneration\":%llu,"
        "\"availability\":\"AVAILABLE\",\"requestCount\":%llu,\"successCount\":%llu,"
        "\"errorCount\":%llu,\"timeoutCount\":%llu,\"minimumLatencyUs\":%llu,"
        "\"maximumLatencyUs\":%llu,\"averageLatencyUs\":%llu,"
        "\"p95LatencyUs\":null,\"p99LatencyUs\":null,"
        "\"dataQuality\":{\"observationsReceived\":%llu,"
        "\"malformedObservationsRejected\":%llu}}",
        (unsigned long long)store->startupEpochUs,
        (unsigned long long)store->generation,
        (unsigned long long)requestCount,
        (unsigned long long)successCount,
        (unsigned long long)errorCount,
        (unsigned long long)timeoutCount,
        (unsigned long long)minimumLatencyUs,
        (unsigned long long)maximumLatencyUs,
        (unsigned long long)averageLatencyUs,
        (unsigned long long)store->observationsReceived,
        (unsigned long long)store->malformedObservationsRejected);
    queueControlResponse(client, response);
}

static void queueResetResponse(controlClient_t* client, reporterStore_t* store)
{
    char response[256];

    resetStore(store);
    (void)snprintf(
        response,
        sizeof(response),
        "{\"reporterInstanceId\":\"rbus-diagnostics-reporter\","
        "\"reporterStartupEpochUs\":%llu,\"measurementGeneration\":%llu,"
        "\"success\":true}",
        (unsigned long long)store->startupEpochUs,
        (unsigned long long)store->generation);
    queueControlResponse(client, response);
}

static int bindUnixSocket(const char* path, const int socketType)
{
    int socketFd;
    struct sockaddr_un address;

    socketFd = socket(AF_UNIX, socketType, 0);
    if (socketFd < 0) {
        return -1;
    }

    (void)unlink(path);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);

    if (bind(socketFd, (const struct sockaddr*)&address, sizeof(address)) != 0) {
        (void)close(socketFd);
        return -1;
    }

    if (socketType == SOCK_STREAM && listen(socketFd, 8) != 0) {
        (void)close(socketFd);
        return -1;
    }

    if (socketType == SOCK_STREAM) {
        int socketFlags = fcntl(socketFd, F_GETFL, 0);

        if (socketFlags == -1 || fcntl(socketFd, F_SETFL, socketFlags | O_NONBLOCK) == -1) {
            (void)close(socketFd);
            return -1;
        }
    }

    (void)chmod(path, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    return socketFd;
}

static void closeControlClient(controlClient_t* client)
{
    if (client->fd >= 0) {
        (void)close(client->fd);
    }

    memset(client, 0, sizeof(*client));
    client->fd = -1;
}

static void processControlRequest(controlClient_t* client, reporterStore_t* store)
{
    if (strcmp(client->request, "GET_GLOBAL\n") == 0) {
        queueGlobalSnapshot(client, store);
    } else if (strcmp(client->request, "RESET\n") == 0) {
        queueResetResponse(client, store);
    } else {
        queueControlResponse(client, "{\"error\":\"INVALID_REQUEST\"}");
    }
}

int main(void)
{
    int observationFd;
    int controlFd;
    reporterStore_t store;
    controlClient_t client;

    memset(&store, 0, sizeof(store));
    memset(&client, 0, sizeof(client));
    client.fd = -1;
    store.generation = 1;
    store.startupEpochUs = epochUs();

    signal(SIGINT, stopReporter);
    signal(SIGTERM, stopReporter);

    observationFd = bindUnixSocket(RBUS_DIAGNOSTICS_OBSERVATION_SOCKET, SOCK_DGRAM);
    controlFd = bindUnixSocket(RBUS_DIAGNOSTICS_CONTROL_SOCKET, SOCK_STREAM);
    if (observationFd < 0 || controlFd < 0) {
        fprintf(stderr, "Unable to create RBus diagnostics sockets: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    while (keepRunning != 0) {
        fd_set readDescriptors;
        fd_set writeDescriptors;
        struct timeval timeout;
        struct timeval* timeoutPointer = NULL;
        int maxFd = observationFd > controlFd ? observationFd : controlFd;
        int ready;
        uint64_t nowMonotonicUs;

        FD_ZERO(&readDescriptors);
        FD_ZERO(&writeDescriptors);
        FD_SET(observationFd, &readDescriptors);

        if (client.fd >= 0) {
            nowMonotonicUs = monotonicUs();
            if (nowMonotonicUs != 0 && client.lastActivityMonotonicUs != 0
                && nowMonotonicUs - client.lastActivityMonotonicUs
                    >= RBUS_DIAGNOSTICS_CONTROL_CLIENT_IDLE_TIMEOUT_US) {
                closeControlClient(&client);
            }
        }

        if (client.fd >= 0) {
            uint64_t remainingUs =
                RBUS_DIAGNOSTICS_CONTROL_CLIENT_IDLE_TIMEOUT_US;

            nowMonotonicUs = monotonicUs();
            if (nowMonotonicUs != 0 && client.lastActivityMonotonicUs != 0
                && nowMonotonicUs > client.lastActivityMonotonicUs) {
                uint64_t elapsedUs = nowMonotonicUs - client.lastActivityMonotonicUs;

                remainingUs = elapsedUs >= RBUS_DIAGNOSTICS_CONTROL_CLIENT_IDLE_TIMEOUT_US
                    ? 0
                    : RBUS_DIAGNOSTICS_CONTROL_CLIENT_IDLE_TIMEOUT_US - elapsedUs;
            }

            timeout.tv_sec = (time_t)(remainingUs / UINT64_C(1000000));
            timeout.tv_usec = (suseconds_t)(remainingUs % UINT64_C(1000000));
            timeoutPointer = &timeout;

            if (client.responseReady) {
                FD_SET(client.fd, &writeDescriptors);
            } else {
                FD_SET(client.fd, &readDescriptors);
            }
            if (client.fd > maxFd) {
                maxFd = client.fd;
            }
        }
        /*
         * Always observe the listener. If the sole active-client slot is in
         * use, one pending connection is accepted and immediately closed below
         * so callers fail promptly rather than waiting in the listen backlog.
         */
        FD_SET(controlFd, &readDescriptors);

        ready = select(
            maxFd + 1,
            &readDescriptors,
            &writeDescriptors,
            NULL,
            timeoutPointer);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        /*
         * Process observations first whenever both input sources are ready.
         * Control work is limited to one nonblocking recv/send per iteration.
         */
        if (FD_ISSET(observationFd, &readDescriptors)) {
            rbusDiagnosticsObservation_t observation;
            ssize_t received = recv(observationFd, &observation, sizeof(observation), 0);

            if (received == (ssize_t)sizeof(observation)) {
                processObservation(&store, &observation);
            } else if (received >= 0) {
                store.malformedObservationsRejected++;
            }
        }

        if (FD_ISSET(controlFd, &readDescriptors)) {
            int clientFd = accept(controlFd, NULL, NULL);

            if (clientFd >= 0) {
                if (client.fd >= 0) {
                    /*
                     * Preserve the single active-client policy without
                     * allowing queued callers to consume the idle timeout.
                     * Only one listener accept is attempted per iteration.
                     */
                    (void)close(clientFd);
                } else {
                    int clientFlags = fcntl(clientFd, F_GETFL, 0);

                    if (clientFlags == -1
                        || fcntl(clientFd, F_SETFL, clientFlags | O_NONBLOCK) == -1) {
                        (void)close(clientFd);
                    } else {
                        client.fd = clientFd;
                        markControlClientActivity(&client);
                    }
                }
            }
        }

        if (client.fd >= 0 && !client.responseReady
            && FD_ISSET(client.fd, &readDescriptors)) {
            ssize_t received = recv(
                client.fd,
                client.request + client.requestLength,
                sizeof(client.request) - 1 - client.requestLength,
                0);

            if (received > 0) {
                client.requestLength += (size_t)received;
                client.request[client.requestLength] = '\0';
                markControlClientActivity(&client);

                if (strchr(client.request, '\n') != NULL
                    || client.requestLength == sizeof(client.request) - 1) {
                    processControlRequest(&client, &store);
                }
            } else if (received == 0) {
                /*
                 * Commands are newline framed. EOF is not a frame terminator:
                 * an unfinished EOF-terminated command is explicitly rejected.
                 */
                if (client.requestLength > 0) {
                    queueControlResponse(&client, "{\"error\":\"INVALID_REQUEST\"}");
                } else {
                    closeControlClient(&client);
                }
            } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                closeControlClient(&client);
            }
        }

        if (client.fd >= 0 && client.responseReady
            && FD_ISSET(client.fd, &writeDescriptors)) {
            ssize_t sent = send(
                client.fd,
                client.response + client.responseSent,
                client.responseLength - client.responseSent,
                MSG_DONTWAIT | MSG_NOSIGNAL);

            if (sent > 0) {
                client.responseSent += (size_t)sent;
                markControlClientActivity(&client);
                if (client.responseSent == client.responseLength) {
                    closeControlClient(&client);
                }
            } else if (sent < 0 && errno != EINTR
                && errno != EAGAIN && errno != EWOULDBLOCK) {
                closeControlClient(&client);
            }
        }
    }

    closeControlClient(&client);
    (void)close(observationFd);
    (void)close(controlFd);
    (void)unlink(RBUS_DIAGNOSTICS_OBSERVATION_SOCKET);
    (void)unlink(RBUS_DIAGNOSTICS_CONTROL_SOCKET);
    return EXIT_SUCCESS;
}
