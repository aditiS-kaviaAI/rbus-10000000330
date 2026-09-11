#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../src/rbus/rbus_diagnostics_client.h"

/*
 * This focused test embeds the private publisher implementation so its
 * process-local socket boundary can be replaced with deterministic fakes.
 * Production code remains unmodified and no application payload is retained.
 */
static int g_socketCalls;
static int g_connectCalls;
static int g_sendCalls;
static int g_closeCalls;
static int g_socketResult;
static int g_connectResult;
static int g_connectErrno;
static ssize_t g_sendResult;
static int g_sendErrno;
static uint8_t g_lastRecord[512];
static size_t g_lastRecordLength;
static int g_testHookCalls;
static rbusDiagnosticsOperation_t g_testHookOperation;
static const char* g_testHookPath;
static rbusError_t g_testHookResult;

static int test_socket(int domain, int type, int protocol)
{
    assert(domain == AF_UNIX);
    assert((type & SOCK_SEQPACKET) == SOCK_SEQPACKET);
    assert((type & SOCK_NONBLOCK) == SOCK_NONBLOCK);
    assert(protocol == 0);
    ++g_socketCalls;
    return g_socketResult;
}

static int test_connect(int socketFd, const struct sockaddr* address,
    socklen_t addressLength)
{
    const struct sockaddr_un* unixAddress =
        (const struct sockaddr_un*)address;

    assert(socketFd == g_socketResult);
    assert(addressLength == sizeof(*unixAddress));
    assert(unixAddress->sun_family == AF_UNIX);
    ++g_connectCalls;

    if (g_connectResult != 0) {
        errno = g_connectErrno;
    }

    return g_connectResult;
}

static ssize_t test_send(int socketFd, const void* buffer, size_t length,
    int flags)
{
    assert(socketFd == g_socketResult);
    assert((flags & MSG_DONTWAIT) != 0);
    assert((flags & MSG_NOSIGNAL) != 0);
    assert(length <= sizeof(g_lastRecord));

    ++g_sendCalls;
    memcpy(g_lastRecord, buffer, length);
    g_lastRecordLength = length;

    if (g_sendResult < 0) {
        errno = g_sendErrno;
        return -1;
    }

    return g_sendResult == 0 ? (ssize_t)length : g_sendResult;
}

static int test_close(int socketFd)
{
    assert(socketFd == g_socketResult);
    ++g_closeCalls;
    return 0;
}

#define socket test_socket
#define connect test_connect
#define send test_send
#define close test_close
#include "../src/rbus/rbus_diagnostics_client.c"
#undef close
#undef send
#undef connect
#undef socket

static void test_publish_hook(rbusDiagnosticsOperation_t operation,
    const char* path, rbusError_t result)
{
    ++g_testHookCalls;
    g_testHookOperation = operation;
    g_testHookPath = path;
    g_testHookResult = result;
}

static uint16_t read_u16(const uint8_t* value)
{
    uint16_t networkValue;

    memcpy(&networkValue, value, sizeof(networkValue));
    return ntohs(networkValue);
}

static uint32_t read_u32(const uint8_t* value)
{
    uint32_t networkValue;

    memcpy(&networkValue, value, sizeof(networkValue));
    return ntohl(networkValue);
}

static uint64_t read_u64(const uint8_t* value)
{
    uint64_t result = 0;
    size_t index;

    for (index = 0; index < sizeof(result); ++index) {
        result = (result << 8) | value[index];
    }

    return result;
}

static void reset_fake_publisher(void)
{
    g_socketCalls = 0;
    g_connectCalls = 0;
    g_sendCalls = 0;
    g_closeCalls = 0;
    g_socketResult = 41;
    g_connectResult = 0;
    g_connectErrno = 0;
    g_sendResult = 0;
    g_sendErrno = 0;
    g_lastRecordLength = 0;
    memset(g_lastRecord, 0, sizeof(g_lastRecord));
    g_testHookCalls = 0;
    g_testHookOperation = RBUS_DIAGNOSTICS_OPERATION_GET;
    g_testHookPath = NULL;
    g_testHookResult = RBUS_ERROR_SUCCESS;
    rbusDiagnostics_SetTestPublishHook(NULL);

    /* The embedded implementation gives each test a fresh publisher state. */
    g_publisherInstanceId = 0;
    g_nextSequence = 0;
    g_localDropCount = 0;
}

static rbusDiagnosticsTimer_t enabled_timer(void)
{
    rbusDiagnosticsTimer_t timer = rbusDiagnostics_TimerStart();

    assert(timer.enabled != 0);
    return timer;
}

static void assert_observation(rbusDiagnosticsOperation_t operation,
    rbusError_t result, const char* path, uint64_t expectedSequence)
{
    const size_t pathLength = strlen(path);

    assert(g_lastRecordLength == 50U + pathLength);
    assert(read_u16(g_lastRecord) == 1U);
    assert(read_u16(g_lastRecord + 2U) == 1U);
    assert(read_u32(g_lastRecord + 4U) == g_lastRecordLength);
    assert(read_u64(g_lastRecord + 8U) != 0U);
    assert(read_u64(g_lastRecord + 16U) == expectedSequence);
    assert(g_lastRecord[24] == (uint8_t)operation);
    assert(g_lastRecord[25] ==
        (uint8_t)rbusDiagnostics_OutcomeFromError(result));
    assert(read_u32(g_lastRecord + 26U) == (uint32_t)result);
    assert(read_u64(g_lastRecord + 30U) < UINT64_MAX);
    assert(read_u64(g_lastRecord + 38U) != 0U);
    assert(read_u16(g_lastRecord + 46U) == pathLength);
    assert(memcmp(g_lastRecord + 48U, path, pathLength) == 0);
    assert(read_u16(g_lastRecord + 48U + pathLength) == 0U);
}

static void test_completed_observations_are_emitted_once(void)
{
    rbusDiagnosticsTimer_t timer;

    reset_fake_publisher();

    timer = enabled_timer();
    rbusDiagnostics_Publish(&timer, RBUS_DIAGNOSTICS_OPERATION_GET,
        "Device.Test.Value", RBUS_ERROR_SUCCESS);
    assert(g_socketCalls == 2);
    assert(g_connectCalls == 2);
    assert(g_sendCalls == 2);
    assert(g_closeCalls == 2);
    assert_observation(RBUS_DIAGNOSTICS_OPERATION_GET, RBUS_ERROR_SUCCESS,
        "Device.Test.Value", 1U);

    timer = enabled_timer();
    rbusDiagnostics_Publish(&timer, RBUS_DIAGNOSTICS_OPERATION_SET,
        "Device.Test.Value", RBUS_ERROR_TIMEOUT);
    assert(g_socketCalls == 4);
    assert(g_sendCalls == 4);
    assert_observation(RBUS_DIAGNOSTICS_OPERATION_SET, RBUS_ERROR_TIMEOUT,
        "Device.Test.Value", 2U);

    timer = enabled_timer();
    rbusDiagnostics_Publish(&timer, RBUS_DIAGNOSTICS_OPERATION_METHOD,
        "Device.Test.Method", RBUS_ERROR_INVALID_INPUT);
    assert(g_socketCalls == 6);
    assert(g_sendCalls == 6);
    assert_observation(RBUS_DIAGNOSTICS_OPERATION_METHOD,
        RBUS_ERROR_INVALID_INPUT, "Device.Test.Method", 3U);
    assert(g_localDropCount == 0U);
}

static void test_internal_hook_observes_completed_boundary(void)
{
    rbusDiagnosticsTimer_t timer;

    reset_fake_publisher();
    rbusDiagnostics_SetTestPublishHook(test_publish_hook);

    timer = enabled_timer();
    rbusDiagnostics_Publish(&timer, RBUS_DIAGNOSTICS_OPERATION_METHOD,
        "Device.Test.AsyncMethod", RBUS_ERROR_TIMEOUT);

    assert(g_testHookCalls == 1);
    assert(g_testHookOperation == RBUS_DIAGNOSTICS_OPERATION_METHOD);
    assert(strcmp(g_testHookPath, "Device.Test.AsyncMethod") == 0);
    assert(g_testHookResult == RBUS_ERROR_TIMEOUT);
    assert(g_socketCalls == 0);
    assert(g_connectCalls == 0);
    assert(g_sendCalls == 0);
    assert(g_closeCalls == 0);
}

static void test_unavailable_endpoint_drops_without_sending(void)
{
    rbusDiagnosticsTimer_t timer;

    reset_fake_publisher();
    g_connectResult = -1;
    g_connectErrno = ENOENT;

    timer = enabled_timer();
    rbusDiagnostics_Publish(&timer, RBUS_DIAGNOSTICS_OPERATION_GET,
        "Device.Test.AbsentReporter", RBUS_ERROR_SUCCESS);

    /*
     * Publisher status and the observation are independent best-effort frames.
     * Both connection attempts fail, but only the observation increments the
     * local observation-drop counter.
     */
    assert(g_socketCalls == 2);
    assert(g_connectCalls == 2);
    assert(g_sendCalls == 0);
    assert(g_closeCalls == 2);
    assert(g_localDropCount == 1U);
}

static void test_saturated_socket_drops_without_retry(void)
{
    rbusDiagnosticsTimer_t timer;

    reset_fake_publisher();
    g_sendResult = -1;
    g_sendErrno = EAGAIN;

    timer = enabled_timer();
    rbusDiagnostics_Publish(&timer, RBUS_DIAGNOSTICS_OPERATION_METHOD,
        "Device.Test.Saturated", RBUS_ERROR_SUCCESS);

    /* Status-send failure is not recursively reported; the observation also
     * attempts one nonblocking send and records its local drop. */
    assert(g_socketCalls == 2);
    assert(g_connectCalls == 2);
    assert(g_sendCalls == 2);
    assert(g_closeCalls == 2);
    assert(g_localDropCount == 1U);
}

int main(void)
{
    test_completed_observations_are_emitted_once();
    test_internal_hook_observes_completed_boundary();
    test_unavailable_endpoint_drops_without_sending();
    test_saturated_socket_drops_without_retry();

    puts("rbus diagnostics publisher tests passed");
    return 0;
}
