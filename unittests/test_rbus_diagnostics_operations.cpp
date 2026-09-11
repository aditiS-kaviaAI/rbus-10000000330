#include "gtest/gtest.h"

#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>

extern "C" {
#include <rbus.h>
#include "rbuscore.h"
#include "rbus_diagnostics_client.h"
}

#include "rbusProviderConsumer.h"

namespace {

enum EventType {
    kObserverEvent = 'O',
    kCallbackEvent = 'C'
};

struct TestEvent {
    char type;
    int operation;
    int result;
};

int g_eventFd = -1;
pthread_mutex_t g_callbackMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_callbackCondition = PTHREAD_COND_INITIALIZER;
bool g_callbackReceived = false;
rbusError_t g_callbackResult = RBUS_ERROR_BUS_ERROR;
int g_lifecycleEventCount = 0;
bool g_lifecycleRegistered = false;
char g_lifecycleProvider[RBUS_MAX_NAME_LENGTH + 1] = {};

void lifecycleObserver(const char* provider, bool registered)
{
    ++g_lifecycleEventCount;
    g_lifecycleRegistered = registered;
    std::strncpy(g_lifecycleProvider, provider,
        sizeof(g_lifecycleProvider) - 1);
    g_lifecycleProvider[sizeof(g_lifecycleProvider) - 1] = '\0';
}

void resetLifecycleObserver()
{
    g_lifecycleEventCount = 0;
    g_lifecycleRegistered = false;
    g_lifecycleProvider[0] = '\0';
}

void writeEvent(char type, rbusDiagnosticsOperation_t operation,
    rbusError_t result)
{
    const TestEvent event = {type, static_cast<int>(operation),
        static_cast<int>(result)};
    ssize_t written;

    do {
        written = write(g_eventFd, &event, sizeof(event));
    } while (written < 0 && errno == EINTR);

    /* A failed test-side pipe write must not affect the RBus operation. */
}

void publishObserver(rbusDiagnosticsOperation_t operation, const char* path,
    rbusError_t result)
{
    (void)path;
    writeEvent(kObserverEvent, operation, result);
}

void asyncCallback(rbusHandle_t handle, char const* methodName,
    rbusError_t result, rbusObject_t outParams)
{
    (void)handle;
    (void)methodName;
    (void)outParams;

    pthread_mutex_lock(&g_callbackMutex);
    g_callbackReceived = true;
    g_callbackResult = result;
    pthread_cond_signal(&g_callbackCondition);
    pthread_mutex_unlock(&g_callbackMutex);

    /* The observer must already have executed when this callback is reached. */
    writeEvent(kCallbackEvent, RBUS_DIAGNOSTICS_OPERATION_METHOD, result);
}

int runConsumerOperations()
{
    const char* getName = "Device.rbusProvider.Param1";
    const char* setName = "Device.rbusProvider.Param2";
    const char* methodName = "Device.rbusProvider.Method()";
    const char* asyncMethodName = "Device.rbusProvider.MethodAsync1()";
    rbusHandle_t handle = NULL;
    rbusValue_t value = NULL;
    rbusValue_t getValue = NULL;
    rbusObject_t inParams = NULL;
    rbusObject_t outParams = NULL;
    int result = RBUS_ERROR_BUS_ERROR;
    struct timespec deadline;

    rbusDiagnostics_SetTestPublishHook(publishObserver);

    result = rbus_open(&handle, "rbusDiagnosticsOperationConsumer");
    if (result != RBUS_ERROR_SUCCESS) {
        return result;
    }

    result = rbus_get(handle, getName, &getValue);
    if (result != RBUS_ERROR_SUCCESS) {
        goto cleanup;
    }

    rbusValue_Init(&value);
    rbusValue_SetString(value, "diagnostics-operation-test");
    result = rbus_set(handle, setName, value, NULL);
    if (result != RBUS_ERROR_SUCCESS) {
        goto cleanup;
    }

    rbusObject_Init(&inParams, NULL);
    result = rbusMethod_Invoke(handle, methodName, inParams, &outParams);
    if (result != RBUS_ERROR_SUCCESS) {
        goto cleanup;
    }

    pthread_mutex_lock(&g_callbackMutex);
    g_callbackReceived = false;
    g_callbackResult = RBUS_ERROR_BUS_ERROR;
    pthread_mutex_unlock(&g_callbackMutex);

    result = rbusMethod_InvokeAsync(handle, asyncMethodName, inParams,
        asyncCallback, 0);
    if (result != RBUS_ERROR_SUCCESS) {
        goto cleanup;
    }

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 8;

    pthread_mutex_lock(&g_callbackMutex);
    while (!g_callbackReceived) {
        const int waitResult = pthread_cond_timedwait(&g_callbackCondition,
            &g_callbackMutex, &deadline);
        if (waitResult == ETIMEDOUT) {
            pthread_mutex_unlock(&g_callbackMutex);
            result = RBUS_ERROR_TIMEOUT;
            goto cleanup;
        }
        if (waitResult != 0) {
            pthread_mutex_unlock(&g_callbackMutex);
            result = RBUS_ERROR_BUS_ERROR;
            goto cleanup;
        }
    }
    result = g_callbackResult;
    pthread_mutex_unlock(&g_callbackMutex);
    if (result != RBUS_ERROR_SUCCESS) {
        goto cleanup;
    }

    /*
     * Do not use thread exhaustion: the internal testing seam takes only the
     * existing pthread_create failure branch and must preserve its API result.
     */
    rbusDiagnostics_SetTestAsyncSchedulingFailure(true);
    result = rbusMethod_InvokeAsync(handle, asyncMethodName, inParams,
        asyncCallback, 0);
    rbusDiagnostics_SetTestAsyncSchedulingFailure(false);
    if (result != RBUS_ERROR_BUS_ERROR) {
        goto cleanup;
    }
    /* The expected injected error was verified; cleanup must report test success. */
    result = RBUS_ERROR_SUCCESS;

cleanup:
    rbusDiagnostics_SetTestAsyncSchedulingFailure(false);
    rbusDiagnostics_SetTestPublishHook(NULL);
    if (outParams != NULL) {
        rbusObject_Release(outParams);
    }
    if (inParams != NULL) {
        rbusObject_Release(inParams);
    }
    if (getValue != NULL) {
        rbusValue_Release(getValue);
    }
    if (value != NULL) {
        rbusValue_Release(value);
    }
    if (handle != NULL) {
        const rbusError_t closeResult = rbus_close(handle);
        if (result == RBUS_ERROR_SUCCESS) {
            result = closeResult;
        }
    }

    return result;
}

void expectEvent(const TestEvent& event, char type,
    rbusDiagnosticsOperation_t operation, rbusError_t result)
{
    EXPECT_EQ(event.type, type);
    EXPECT_EQ(event.operation, static_cast<int>(operation));
    EXPECT_EQ(event.result, static_cast<int>(result));
}

} // namespace

TEST(RbusDiagnosticsOperations, ObservesLiveTerminalPathsBeforeAsyncCallback)
{
    int eventPipe[2];
    int consumerStatus = 0;
    TestEvent events[6];
    ssize_t bytesRead = 0;
    pid_t consumerPid;

    ASSERT_EQ(pipe(eventPipe), 0);

    /*
     * This internal test seam bypasses only `/etc/rbus_client.conf`; it keeps
     * the broker endpoint aligned with the temporary rtrouted instance started
     * by the STEP-03 validation command. The setting is inherited by both
     * forked processes and is not compiled into production RBus builds.
     */
    rbuscore_SetTestBrokerAddress("unix:///tmp/rtrouted");

    consumerPid = fork();
    ASSERT_GE(consumerPid, 0);

    if (consumerPid == 0) {
        close(eventPipe[0]);
        g_eventFd = eventPipe[1];

        /*
         * The provider registers before waiting for this consumer. This short
         * delay mirrors the existing functional test startup sequencing.
         */
        usleep(100000);
        const int consumerResult = runConsumerOperations();

        close(eventPipe[1]);
        _exit(consumerResult == RBUS_ERROR_SUCCESS ? 0 : 1);
    }

    close(eventPipe[1]);

    /*
     * Reuse the established provider fixture so each assertion exercises the
     * public rbus_get, rbus_set, rbusMethod_Invoke, and InvokeAsync paths.
     */
    EXPECT_EQ(rbusProvider(RBUS_GTEST_METHOD_ASYNC, consumerPid,
        &consumerStatus), 0);
    ASSERT_TRUE(WIFEXITED(consumerStatus));
    EXPECT_EQ(WEXITSTATUS(consumerStatus), 0);

    while (bytesRead < static_cast<ssize_t>(sizeof(events))) {
        const ssize_t readResult = read(eventPipe[0],
            reinterpret_cast<char*>(events) + bytesRead,
            sizeof(events) - static_cast<size_t>(bytesRead));
        if (readResult == 0) {
            break;
        }
        ASSERT_GT(readResult, 0);
        bytesRead += readResult;
    }
    close(eventPipe[0]);

    ASSERT_EQ(bytesRead, static_cast<ssize_t>(sizeof(events)));
    expectEvent(events[0], kObserverEvent, RBUS_DIAGNOSTICS_OPERATION_GET,
        RBUS_ERROR_SUCCESS);
    expectEvent(events[1], kObserverEvent, RBUS_DIAGNOSTICS_OPERATION_SET,
        RBUS_ERROR_SUCCESS);
    expectEvent(events[2], kObserverEvent, RBUS_DIAGNOSTICS_OPERATION_METHOD,
        RBUS_ERROR_SUCCESS);
    expectEvent(events[3], kObserverEvent, RBUS_DIAGNOSTICS_OPERATION_METHOD,
        RBUS_ERROR_SUCCESS);
    expectEvent(events[4], kCallbackEvent, RBUS_DIAGNOSTICS_OPERATION_METHOD,
        RBUS_ERROR_SUCCESS);
    expectEvent(events[5], kObserverEvent, RBUS_DIAGNOSTICS_OPERATION_METHOD,
        RBUS_ERROR_BUS_ERROR);
}

TEST(RbusDiagnosticsLifecycle, EmitsOnlyVerifiedRegistrationAndRemoval)
{
    const char* componentName = "rbusDiagnosticsLifecycleProvider";
    char registeredName[] = "Device.rbusDiagnosticsLifecycle.Value";
    char verifiedName[] = "Device.rbusDiagnosticsLifecycle.VerifiedValue";
    rbusDataElement_t registeredElement = {
        registeredName, RBUS_ELEMENT_TYPE_PROPERTY, {NULL, NULL, NULL, NULL,
            NULL, NULL}};
    rbusDataElement_t verifiedElement = {
        verifiedName, RBUS_ELEMENT_TYPE_PROPERTY, {NULL, NULL, NULL, NULL,
            NULL, NULL}};
    rbusHandle_t handle = NULL;

    rbuscore_SetTestBrokerAddress("unix:///tmp/rtrouted");
    resetLifecycleObserver();
    rbusDiagnostics_SetTestLifecycleHook(lifecycleObserver);

    ASSERT_EQ(rbus_open(&handle, componentName), RBUS_ERROR_SUCCESS);
    ASSERT_EQ(rbus_regDataElements(handle, 1, &registeredElement),
        RBUS_ERROR_SUCCESS);
    EXPECT_EQ(g_lifecycleEventCount, 1);
    EXPECT_TRUE(g_lifecycleRegistered);
    EXPECT_STREQ(g_lifecycleProvider, componentName);

    /* Duplicate registration fails its transaction and emits no new frame. */
    EXPECT_NE(rbus_regDataElements(handle, 1, &registeredElement),
        RBUS_ERROR_SUCCESS);
    EXPECT_EQ(g_lifecycleEventCount, 1);

    /*
     * Do not depend on broker treatment of missing names. Force only the
     * existing core-removal failure branch and verify that the legacy API keeps
     * its success return while diagnostics emits no stopped lifecycle record.
     */
    rbusDiagnostics_SetTestRemovalFailure(true);
    EXPECT_EQ(rbus_unregDataElements(handle, 1, &registeredElement),
        RBUS_ERROR_SUCCESS);
    rbusDiagnostics_SetTestRemovalFailure(false);
    EXPECT_EQ(g_lifecycleEventCount, 1);

    /*
     * The injected failure leaves the provider element registered. A following
     * ordinary removal proves that only a complete verified request publishes
     * the stopped lifecycle transition.
     */
    ASSERT_EQ(rbus_unregDataElements(handle, 1, &registeredElement),
        RBUS_ERROR_SUCCESS);
    EXPECT_EQ(g_lifecycleEventCount, 2);
    EXPECT_FALSE(g_lifecycleRegistered);
    EXPECT_STREQ(g_lifecycleProvider, componentName);

    rbusDiagnostics_SetTestLifecycleHook(NULL);
    rbusDiagnostics_SetTestRemovalFailure(false);
    EXPECT_EQ(rbus_close(handle), RBUS_ERROR_SUCCESS);
}
