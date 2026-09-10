#define _GNU_SOURCE
#include "diagnostics_protocol.h"
#include "diagnostics_store.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define RBUS_DIAGNOSTICS_RUNTIME_DIRECTORY "/run/rbus-diagnostics"

typedef struct {
    char runtimeDirectory[sizeof(((struct sockaddr_un*)0)->sun_path)];
    char observationSocket[sizeof(((struct sockaddr_un*)0)->sun_path)];
    char controlSocket[sizeof(((struct sockaddr_un*)0)->sun_path)];
} rbusDiagnosticsRuntimePaths_t;

static volatile sig_atomic_t running = 1;

static bool initializeRuntimePaths(rbusDiagnosticsRuntimePaths_t* paths)
{
    const char* runtimeDirectory = RBUS_DIAGNOSTICS_RUNTIME_DIRECTORY;

#ifdef RBUS_DIAGNOSTICS_TEST_RUNTIME_DIRECTORY_OVERRIDE
    /*
     * This override exists only in the test reporter executable. Production
     * builds always retain the deployment contract's /run path.
     */
    const char* testRuntimeDirectory = getenv("RBUS_DIAGNOSTICS_TEST_RUNTIME_DIRECTORY");
    if (testRuntimeDirectory != NULL && testRuntimeDirectory[0] != '\0') {
        runtimeDirectory = testRuntimeDirectory;
    }
#endif

    return snprintf(paths->runtimeDirectory, sizeof(paths->runtimeDirectory), "%s",
               runtimeDirectory) < (int)sizeof(paths->runtimeDirectory) &&
        snprintf(paths->observationSocket, sizeof(paths->observationSocket), "%s/observations.sock",
            paths->runtimeDirectory) < (int)sizeof(paths->observationSocket) &&
        snprintf(paths->controlSocket, sizeof(paths->controlSocket), "%s/control.sock",
            paths->runtimeDirectory) < (int)sizeof(paths->controlSocket);
}

static void stopReporter(int signalNumber)
{
    (void)signalNumber;
    running = 0;
}

static uint64_t epochUs(void)
{
    struct timespec value;
    clock_gettime(CLOCK_REALTIME, &value);
    return ((uint64_t)value.tv_sec * 1000000U) + ((uint64_t)value.tv_nsec / 1000U);
}

static int makeServerSocket(const char* path, int type)
{
    int descriptor;
    struct sockaddr_un address;

    descriptor = socket(AF_UNIX, type | SOCK_CLOEXEC, 0);
    if (descriptor < 0) {
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
    unlink(path);
    if (bind(descriptor, (const struct sockaddr*)&address, sizeof(address)) != 0) {
        close(descriptor);
        return -1;
    }
    if (chmod(path, 0660) != 0) {
        close(descriptor);
        return -1;
    }
    if (type == SOCK_SEQPACKET && listen(descriptor, 4) != 0) {
        close(descriptor);
        unlink(path);
        return -1;
    }
    return descriptor;
}

static bool isAuthorizedObservationPeer(const struct ucred* credentials, const char* observationSocket)
{
    /*
     * Filesystem access is the primary group policy. Root may publish; the
     * socket's rbusdiag group grants publisher access. SO_PASSCRED supplies
     * the effective credentials used for the secondary validation.
     */
    struct stat metadata;

    if (credentials == NULL || stat(observationSocket, &metadata) != 0) {
        return false;
    }
    return credentials->uid == 0 || credentials->uid == geteuid() ||
        credentials->gid == metadata.st_gid;
}

static bool isAuthorizedControlPeer(int descriptor, uid_t thunderUid)
{
    struct ucred credentials;
    socklen_t length = sizeof(credentials);

    if (getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0) {
        return false;
    }
    return credentials.uid == 0 || credentials.uid == thunderUid;
}

static void receiveObservation(int descriptor, rbusDiagnosticsStore_t* store,
    const char* observationSocket)
{
    uint8_t frame[RBUS_DIAGNOSTICS_MAX_FRAME_SIZE];
    char control[CMSG_SPACE(sizeof(struct ucred))];
    struct iovec iov;
    struct msghdr message;
    struct cmsghdr* header;
    struct ucred* credentials = NULL;
    ssize_t received;
    rbusDiagnosticsObservation_t observation;

    memset(&message, 0, sizeof(message));
    memset(control, 0, sizeof(control));
    iov.iov_base = frame;
    iov.iov_len = sizeof(frame);
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);

    received = recvmsg(descriptor, &message, 0);
    if (received <= 0 || (message.msg_flags & MSG_TRUNC) != 0) {
        rbusDiagnosticsStoreRejectMalformed(store);
        return;
    }

    for (header = CMSG_FIRSTHDR(&message); header != NULL;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_CREDENTIALS) {
            credentials = (struct ucred*)CMSG_DATA(header);
            break;
        }
    }

    if (!isAuthorizedObservationPeer(credentials, observationSocket) ||
        !rbusDiagnosticsDecodeObservation(frame, (size_t)received, &observation)) {
        rbusDiagnosticsStoreRejectMalformed(store);
        return;
    }
    rbusDiagnosticsStoreAccept(store, &observation);
}

static void serviceControl(int listener, rbusDiagnosticsStore_t* store, uid_t thunderUid)
{
    int client;
    uint8_t request[RBUS_DIAGNOSTICS_MAX_FRAME_SIZE];
    uint8_t response[RBUS_DIAGNOSTICS_MAX_FRAME_SIZE];
    ssize_t received;
    size_t responseLength;
    rbusDiagnosticsControlRequest_t requestFrame;
    rbusDiagnosticsGlobalSnapshot_t snapshot;
    rbusDiagnosticsProviderSnapshot_t providers[RBUS_DIAGNOSTICS_MAX_PROVIDERS];
    rbusDiagnosticsSlowRequest_t slowRequests[RBUS_DIAGNOSTICS_MAX_SLOW_REQUESTS];

    client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    if (client < 0) {
        return;
    }
    if (!isAuthorizedControlPeer(client, thunderUid)) {
        close(client);
        return;
    }

    received = recv(client, request, sizeof(request), 0);
    if (received > 0 && rbusDiagnosticsDecodeControlRequest(request, (size_t)received,
            &requestFrame)) {
        if (requestFrame.type == RBUS_DIAGNOSTICS_MESSAGE_RESET) {
            rbusDiagnosticsStoreReset(store, &snapshot);
            (void)rbusDiagnosticsEncodeSnapshot(RBUS_DIAGNOSTICS_MESSAGE_RESET_RESULT,
                &snapshot, response, sizeof(response), &responseLength);
        } else if (requestFrame.type == RBUS_DIAGNOSTICS_MESSAGE_GET_GLOBAL) {
            rbusDiagnosticsStoreSnapshot(store, &snapshot);
            (void)rbusDiagnosticsEncodeSnapshot(RBUS_DIAGNOSTICS_MESSAGE_GLOBAL_SNAPSHOT,
                &snapshot, response, sizeof(response), &responseLength);
        } else if (requestFrame.type == RBUS_DIAGNOSTICS_MESSAGE_GET_PROVIDER) {
            if (!rbusDiagnosticsStoreProviderSnapshot(store, requestFrame.provider, &snapshot,
                    &providers[0])) {
                memset(response, 0, 8);
                response[0] = 0;
                response[1] = RBUS_DIAGNOSTICS_PROTOCOL_VERSION;
                response[2] = 0;
                response[3] = RBUS_DIAGNOSTICS_MESSAGE_PROVIDER_NOT_FOUND;
                response[7] = 8;
                (void)send(client, response, 8, MSG_NOSIGNAL);
                close(client);
                return;
            }
            (void)rbusDiagnosticsEncodeProviderResponse(
                RBUS_DIAGNOSTICS_MESSAGE_PROVIDER_SNAPSHOT, &snapshot, providers, 1,
                response, sizeof(response), &responseLength);
        } else if (requestFrame.type == RBUS_DIAGNOSTICS_MESSAGE_LIST_PROVIDERS) {
            uint32_t providerCount = rbusDiagnosticsStoreProviderList(store, requestFrame.limit,
                &snapshot, providers);
            (void)rbusDiagnosticsEncodeProviderResponse(
                RBUS_DIAGNOSTICS_MESSAGE_PROVIDER_LIST, &snapshot, providers, providerCount,
                response, sizeof(response), &responseLength);
        } else {
            uint32_t slowRequestCount = rbusDiagnosticsStoreSlowRequestList(store,
                requestFrame.limit, &snapshot, slowRequests);
            (void)rbusDiagnosticsEncodeSlowRequestResponse(&snapshot, slowRequests,
                slowRequestCount, response, sizeof(response), &responseLength);
        }
        (void)send(client, response, responseLength, MSG_NOSIGNAL);
    }
    close(client);
}

int main(int argc, char** argv)
{
    int observationSocket;
    int controlSocket;
    int enabled = 1;
    struct pollfd pollDescriptors[2];
    rbusDiagnosticsStore_t store;
    struct group* diagnosticsGroup;
    uid_t thunderUid = 0;
    uint64_t instanceId;
    rbusDiagnosticsRuntimePaths_t paths;

    if (argc == 3 && strcmp(argv[1], "--thunder-uid") == 0) {
        thunderUid = (uid_t)strtoul(argv[2], NULL, 10);
    } else if (argc != 1) {
        fprintf(stderr, "Usage: %s [--thunder-uid UID]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (!initializeRuntimePaths(&paths)) {
        fprintf(stderr, "diagnostics runtime socket path is too long\n");
        return EXIT_FAILURE;
    }
    if (mkdir(paths.runtimeDirectory, 0750) != 0 && errno != EEXIST) {
        perror("mkdir diagnostics runtime directory");
        return EXIT_FAILURE;
    }
    diagnosticsGroup = getgrnam("rbusdiag");
    if (diagnosticsGroup != NULL) {
        (void)chown(paths.runtimeDirectory, 0, diagnosticsGroup->gr_gid);
    }
    (void)chmod(paths.runtimeDirectory, 0750);

    observationSocket = makeServerSocket(paths.observationSocket, SOCK_DGRAM);
    controlSocket = makeServerSocket(paths.controlSocket, SOCK_SEQPACKET);
    if (observationSocket < 0 || controlSocket < 0 ||
        setsockopt(observationSocket, SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) != 0) {
        perror("create diagnostics sockets");
        if (observationSocket >= 0) {
            close(observationSocket);
        }
        if (controlSocket >= 0) {
            close(controlSocket);
        }
        return EXIT_FAILURE;
    }

    if (diagnosticsGroup != NULL) {
        (void)chown(paths.observationSocket, 0, diagnosticsGroup->gr_gid);
        (void)chown(paths.controlSocket, 0, diagnosticsGroup->gr_gid);
    }

    instanceId = epochUs() ^ ((uint64_t)getpid() << 32);
    if (rbusDiagnosticsStoreInitialize(&store, instanceId, epochUs()) != 0) {
        close(observationSocket);
        close(controlSocket);
        return EXIT_FAILURE;
    }

    signal(SIGINT, stopReporter);
    signal(SIGTERM, stopReporter);
    pollDescriptors[0].fd = observationSocket;
    pollDescriptors[0].events = POLLIN;
    pollDescriptors[1].fd = controlSocket;
    pollDescriptors[1].events = POLLIN;

    while (running) {
        int result = poll(pollDescriptors, 2, 500);
        if (result > 0) {
            if ((pollDescriptors[0].revents & POLLIN) != 0) {
                receiveObservation(observationSocket, &store, paths.observationSocket);
            }
            if ((pollDescriptors[1].revents & POLLIN) != 0) {
                serviceControl(controlSocket, &store, thunderUid);
            }
        }
    }

    rbusDiagnosticsStoreDestroy(&store);
    close(observationSocket);
    close(controlSocket);
    unlink(paths.observationSocket);
    unlink(paths.controlSocket);
    return EXIT_SUCCESS;
}
