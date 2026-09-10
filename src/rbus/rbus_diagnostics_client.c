#include "rbus_diagnostics_client.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#ifdef RBUS_DIAGNOSTICS_ENABLED
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#ifndef RBUS_DIAGNOSTICS_OBSERVATION_SOCKET
#define RBUS_DIAGNOSTICS_OBSERVATION_SOCKET "/tmp/rbus-diagnostics-observations.sock"
#endif

static rbusDiagnosticsOutcome_t rbusDiagnostics_ClassifyResult(const rbusError_t result)
{
    switch (result) {
    case RBUS_ERROR_SUCCESS:
    case RBUS_ERROR_NOSUBSCRIBERS:
        return RBUS_DIAGNOSTICS_OUTCOME_SUCCESS;
    case RBUS_ERROR_TIMEOUT:
        return RBUS_DIAGNOSTICS_OUTCOME_TIMEOUT;
    case RBUS_ERROR_DESTINATION_NOT_FOUND:
    case RBUS_ERROR_DESTINATION_NOT_REACHABLE:
    case RBUS_ERROR_COMPONENT_DOES_NOT_EXIST:
    case RBUS_ERROR_ELEMENT_DOES_NOT_EXIST:
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
    case RBUS_ERROR_NOT_READABLE:
    case RBUS_ERROR_NOT_WRITABLE:
        return RBUS_DIAGNOSTICS_OUTCOME_PERMISSION_DENIED;
    case RBUS_ERROR_BUS_ERROR:
    case RBUS_ERROR_NOT_INITIALIZED:
    case RBUS_ERROR_OUT_OF_RESOURCES:
        return RBUS_DIAGNOSTICS_OUTCOME_INTERNAL_ERROR;
    default:
        return RBUS_DIAGNOSTICS_OUTCOME_UNKNOWN_ERROR;
    }
}

uint64_t rbusDiagnostics_MonotonicTimeUs(void)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
        return 0;
    }

    return ((uint64_t)timestamp.tv_sec * UINT64_C(1000000))
        + ((uint64_t)timestamp.tv_nsec / UINT64_C(1000));
}

void rbusDiagnostics_RecordOperation(
    const rbusDiagnosticsOperation_t operation,
    const char* component,
    const char* operationName,
    const uint64_t durationUs,
    const rbusError_t result)
{
#ifdef RBUS_DIAGNOSTICS_ENABLED
    int socketFd;
    struct sockaddr_un address;
    rbusDiagnosticsObservation_t observation;

    if (operationName == NULL) {
        return;
    }

    memset(&observation, 0, sizeof(observation));
    observation.protocolVersion = RBUS_DIAGNOSTICS_PROTOCOL_VERSION;
    observation.operation = (uint16_t)operation;
    observation.outcome = (uint16_t)rbusDiagnostics_ClassifyResult(result);
    observation.durationUs = durationUs;

    if (component != NULL) {
        strncpy(observation.component, component, sizeof(observation.component) - 1);
    }
    strncpy(observation.operationName, operationName, sizeof(observation.operationName) - 1);

    socketFd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (socketFd < 0) {
        return;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, RBUS_DIAGNOSTICS_OBSERVATION_SOCKET, sizeof(address.sun_path) - 1);

    /*
     * Publishing is deliberately fire-and-forget. A full socket, missing reporter,
     * or any other send failure must never affect the RBus caller.
     */
    (void)sendto(
        socketFd,
        &observation,
        sizeof(observation),
        MSG_DONTWAIT,
        (const struct sockaddr*)&address,
        sizeof(address));
    (void)close(socketFd);
#else
    (void)operation;
    (void)component;
    (void)operationName;
    (void)durationUs;
    (void)result;
#endif
}
