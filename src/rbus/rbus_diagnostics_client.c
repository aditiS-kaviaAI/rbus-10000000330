#include "rbus_diagnostics_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define RBUS_DIAGNOSTICS_PROTOCOL_VERSION 1U
#define RBUS_DIAGNOSTICS_MESSAGE_OBSERVATION 1U
#define RBUS_DIAGNOSTICS_MESSAGE_PROVIDER_LIFECYCLE 4U
#define RBUS_DIAGNOSTICS_MESSAGE_PUBLISHER_STATUS 5U
#define RBUS_DIAGNOSTICS_MAX_IDENTIFIER_BYTES 256U
#define RBUS_DIAGNOSTICS_MAX_OBSERVATION_BYTES 512U
#define RBUS_DIAGNOSTICS_OBSERVATION_SOCKET \
    "/run/rbus-diagnostics/observations.sock"

static uint64_t g_publisherInstanceId;
static uint64_t g_nextSequence;
static uint64_t g_localDropCount;

#ifdef RBUS_DIAGNOSTICS_TESTING
/*
 * This is intentionally process-local and test-only. It observes the same
 * final-result publisher invocation used by rbus.c without opening a socket.
 */
static rbusDiagnosticsTestPublishHook_t g_testPublishHook;
static rbusDiagnosticsTestLifecycleHook_t g_testLifecycleHook;
static bool g_testAsyncSchedulingFailure;
static bool g_testRemovalFailure;

void rbusDiagnostics_SetTestPublishHook(
    rbusDiagnosticsTestPublishHook_t hook)
{
    g_testPublishHook = hook;
}

void rbusDiagnostics_SetTestLifecycleHook(
    rbusDiagnosticsTestLifecycleHook_t hook)
{
    g_testLifecycleHook = hook;
}

void rbusDiagnostics_SetTestAsyncSchedulingFailure(bool enabled)
{
    g_testAsyncSchedulingFailure = enabled;
}

bool rbusDiagnostics_ShouldFailAsyncSchedulingForTest(void)
{
    return g_testAsyncSchedulingFailure;
}

void rbusDiagnostics_SetTestRemovalFailure(bool enabled)
{
    g_testRemovalFailure = enabled;
}

bool rbusDiagnostics_ShouldFailRemovalForTest(void)
{
    const bool shouldFail = g_testRemovalFailure;

    g_testRemovalFailure = false;
    return shouldFail;
}
#endif

/* The reporter protocol uses explicit network-byte-order integer fields. */
static void rbusDiagnostics_AppendU16(uint8_t** cursor, uint16_t value)
{
    value = htons(value);
    memcpy(*cursor, &value, sizeof(value));
    *cursor += sizeof(value);
}

static void rbusDiagnostics_AppendU32(uint8_t** cursor, uint32_t value)
{
    value = htonl(value);
    memcpy(*cursor, &value, sizeof(value));
    *cursor += sizeof(value);
}

static void rbusDiagnostics_AppendU64(uint8_t** cursor, uint64_t value)
{
    int shift;

    for (shift = 56; shift >= 0; shift -= 8) {
        *(*cursor)++ = (uint8_t)(value >> shift);
    }
}

static uint64_t rbusDiagnostics_PublisherInstanceId(void)
{
    struct timespec now;

    if (g_publisherInstanceId != 0) {
        return g_publisherInstanceId;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        now.tv_sec = 0;
        now.tv_nsec = 0;
    }

    g_publisherInstanceId = ((uint64_t)(uint32_t)getpid() << 32) ^
        ((uint64_t)now.tv_sec << 16) ^ (uint64_t)now.tv_nsec;
    if (g_publisherInstanceId == 0) {
        g_publisherInstanceId = 1;
    }

    return g_publisherInstanceId;
}

static uint64_t rbusDiagnostics_EpochMicroseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_REALTIME, &now) != 0 || now.tv_sec < 0) {
        return 0;
    }

    if ((uint64_t)now.tv_sec > UINT64_MAX / 1000000U) {
        return UINT64_MAX;
    }

    return ((uint64_t)now.tv_sec * 1000000U) + ((uint64_t)now.tv_nsec / 1000U);
}

static void rbusDiagnostics_RecordDrop(void)
{
    if (g_localDropCount != UINT64_MAX) {
        ++g_localDropCount;
    }
}

static bool rbusDiagnostics_SendRecord(
    const uint8_t* record,
    size_t recordLength)
{
    struct sockaddr_un address;
    int socketFd;
    ssize_t sent;

    socketFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0);
    if (socketFd < 0) {
        return false;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, RBUS_DIAGNOSTICS_OBSERVATION_SOCKET,
        sizeof(address.sun_path) - 1U);

    if (connect(socketFd, (struct sockaddr*)&address, sizeof(address)) != 0 &&
        errno != EISCONN) {
        close(socketFd);
        return false;
    }

    sent = send(socketFd, record, recordLength, MSG_DONTWAIT | MSG_NOSIGNAL);
    close(socketFd);
    return sent == (ssize_t)recordLength;
}

static bool rbusDiagnostics_SendPublisherStatus(uint64_t sequence)
{
    uint8_t record[32];
    uint8_t* cursor = record;

    rbusDiagnostics_AppendU16(&cursor, RBUS_DIAGNOSTICS_PROTOCOL_VERSION);
    rbusDiagnostics_AppendU16(&cursor, RBUS_DIAGNOSTICS_MESSAGE_PUBLISHER_STATUS);
    rbusDiagnostics_AppendU32(&cursor, sizeof(record));
    rbusDiagnostics_AppendU64(&cursor, rbusDiagnostics_PublisherInstanceId());
    rbusDiagnostics_AppendU64(&cursor, sequence);
    rbusDiagnostics_AppendU64(&cursor, g_localDropCount);
    return rbusDiagnostics_SendRecord(record, sizeof(record));
}

static bool rbusDiagnostics_IsBoundedIdentifier(
    const char* value,
    size_t* length)
{
    *length = strnlen(value, RBUS_DIAGNOSTICS_MAX_IDENTIFIER_BYTES + 1U);
    return *length <= RBUS_DIAGNOSTICS_MAX_IDENTIFIER_BYTES;
}

void rbusDiagnostics_Publish(
    const rbusDiagnosticsTimer_t* timer,
    rbusDiagnosticsOperation_t operation,
    const char* path,
    rbusError_t result)
{
#ifdef ENABLE_RBUS_DIAGNOSTICS
    uint8_t record[RBUS_DIAGNOSTICS_MAX_OBSERVATION_BYTES];
    uint8_t* cursor = record;
    size_t pathLength;
    size_t recordLength;
    const uint64_t sequence = ++g_nextSequence;

    if (timer == NULL || !timer->enabled || path == NULL) {
        return;
    }

#ifdef RBUS_DIAGNOSTICS_TESTING
    if (g_testPublishHook != NULL) {
        g_testPublishHook(operation, path, result);
        return;
    }
#endif

    if (!rbusDiagnostics_IsBoundedIdentifier(path, &pathLength)) {
        rbusDiagnostics_RecordDrop();
        return;
    }

    /*
     * Status is deliberately best-effort and independent: a failed status
     * frame is never recursively reported as another diagnostics record.
     */
    (void)rbusDiagnostics_SendPublisherStatus(sequence);

    rbusDiagnostics_AppendU16(&cursor, RBUS_DIAGNOSTICS_PROTOCOL_VERSION);
    rbusDiagnostics_AppendU16(&cursor, RBUS_DIAGNOSTICS_MESSAGE_OBSERVATION);
    rbusDiagnostics_AppendU32(&cursor, 0);
    rbusDiagnostics_AppendU64(&cursor, rbusDiagnostics_PublisherInstanceId());
    rbusDiagnostics_AppendU64(&cursor, sequence);
    *cursor++ = (uint8_t)operation;
    *cursor++ = (uint8_t)rbusDiagnostics_OutcomeFromError(result);
    rbusDiagnostics_AppendU32(&cursor, (uint32_t)result);
    rbusDiagnostics_AppendU64(&cursor, rbusDiagnostics_TimerElapsedMicroseconds(timer));
    rbusDiagnostics_AppendU64(&cursor, rbusDiagnostics_EpochMicroseconds());
    rbusDiagnostics_AppendU16(&cursor, (uint16_t)pathLength);
    memcpy(cursor, path, pathLength);
    cursor += pathLength;
    rbusDiagnostics_AppendU16(&cursor, 0);

    recordLength = (size_t)(cursor - record);
    if (recordLength > RBUS_DIAGNOSTICS_MAX_OBSERVATION_BYTES) {
        rbusDiagnostics_RecordDrop();
        return;
    }

    record[4] = (uint8_t)(recordLength >> 24);
    record[5] = (uint8_t)(recordLength >> 16);
    record[6] = (uint8_t)(recordLength >> 8);
    record[7] = (uint8_t)recordLength;

    if (!rbusDiagnostics_SendRecord(record, recordLength)) {
        rbusDiagnostics_RecordDrop();
    }
#else
    (void)timer;
    (void)operation;
    (void)path;
    (void)result;
#endif
}

void rbusDiagnostics_PublishProviderLifecycle(
    const char* provider,
    bool registered)
{
#ifdef ENABLE_RBUS_DIAGNOSTICS
    uint8_t record[RBUS_DIAGNOSTICS_MAX_OBSERVATION_BYTES];
    uint8_t* cursor = record;
    size_t providerLength;
    size_t recordLength;
    const uint64_t sequence = ++g_nextSequence;

    if (provider == NULL ||
        !rbusDiagnostics_IsBoundedIdentifier(provider, &providerLength) ||
        providerLength == 0) {
        return;
    }

#ifdef RBUS_DIAGNOSTICS_TESTING
    if (g_testLifecycleHook != NULL) {
        g_testLifecycleHook(provider, registered);
        return;
    }
#endif

    (void)rbusDiagnostics_SendPublisherStatus(sequence);

    rbusDiagnostics_AppendU16(&cursor, RBUS_DIAGNOSTICS_PROTOCOL_VERSION);
    rbusDiagnostics_AppendU16(&cursor, RBUS_DIAGNOSTICS_MESSAGE_PROVIDER_LIFECYCLE);
    rbusDiagnostics_AppendU32(&cursor, 0);
    rbusDiagnostics_AppendU64(&cursor, rbusDiagnostics_PublisherInstanceId());
    rbusDiagnostics_AppendU64(&cursor, sequence);
    *cursor++ = registered ? 1U : 0U;
    rbusDiagnostics_AppendU16(&cursor, (uint16_t)providerLength);
    memcpy(cursor, provider, providerLength);
    cursor += providerLength;

    recordLength = (size_t)(cursor - record);
    record[4] = (uint8_t)(recordLength >> 24);
    record[5] = (uint8_t)(recordLength >> 16);
    record[6] = (uint8_t)(recordLength >> 8);
    record[7] = (uint8_t)recordLength;

    if (!rbusDiagnostics_SendRecord(record, recordLength)) {
        rbusDiagnostics_RecordDrop();
    }
#else
    (void)provider;
    (void)registered;
#endif
}
