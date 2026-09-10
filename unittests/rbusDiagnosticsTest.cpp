#include "gtest/gtest.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

extern "C" {
#include "rbus_diagnostics.h"
#include "rbus_diagnostics_client.h"
}

namespace {

class DiagnosticsDatagramReceiver {
public:
    DiagnosticsDatagramReceiver()
        : socketFd_(-1)
    {
        std::snprintf(
            endpoint_,
            sizeof(endpoint_),
            "/tmp/rbus-diagnostics-gtest-%ld.sock",
            static_cast<long>(getpid()));

        socketFd_ = socket(AF_UNIX, SOCK_DGRAM, 0);
        ASSERT_GE(socketFd_, 0);
        unlink(endpoint_);

        struct sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, endpoint_, sizeof(address.sun_path) - 1U);
        ASSERT_EQ(
            0,
            bind(
                socketFd_,
                reinterpret_cast<const struct sockaddr*>(&address),
                sizeof(address)));
    }

    ~DiagnosticsDatagramReceiver()
    {
        if (socketFd_ >= 0) {
            close(socketFd_);
        }
        unlink(endpoint_);
    }

    const char* endpoint() const
    {
        return endpoint_;
    }

    bool receive(rbusDiagnosticsObservation_t* observation, int timeoutMs = 100)
    {
        uint8_t frame[RBUS_DIAGNOSTICS_MAX_FRAME_SIZE];
        struct pollfd descriptor = {socketFd_, POLLIN, 0};

        if (poll(&descriptor, 1, timeoutMs) != 1) {
            return false;
        }

        const ssize_t received = recv(socketFd_, frame, sizeof(frame), 0);
        return received > 0 &&
            rbusDiagnosticsDecodeObservation(
                frame,
                static_cast<size_t>(received),
                observation);
    }

private:
    int socketFd_;
    char endpoint_[sizeof(((struct sockaddr_un*)0)->sun_path)];
};

rbusDiagnosticsObservation_t makeObservation(
    rbusDiagnosticsOperation_t operation,
    rbusDiagnosticsOutcome_t outcome)
{
    rbusDiagnosticsObservation_t observation = {};
    observation.monotonicEndUs = 100;
    observation.durationUs = 25;
    observation.operation = operation;
    observation.outcome = outcome;
    return observation;
}

class RbusDiagnosticsTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        rbusDiagnosticsClientResetForTest();
    }

    void TearDown() override
    {
        rbusDiagnosticsClientResetForTest();
    }
};

} // namespace

TEST_F(RbusDiagnosticsTest, ClassifiesApprovedTerminalOutcomes)
{
    EXPECT_EQ(
        RBUS_DIAGNOSTICS_OUTCOME_SUCCESS,
        rbusDiagnosticsClassifyOutcome(RBUS_ERROR_SUCCESS));
    EXPECT_EQ(
        RBUS_DIAGNOSTICS_OUTCOME_TIMEOUT,
        rbusDiagnosticsClassifyOutcome(RBUS_ERROR_TIMEOUT));
    EXPECT_EQ(
        RBUS_DIAGNOSTICS_OUTCOME_PROVIDER_UNAVAILABLE,
        rbusDiagnosticsClassifyOutcome(RBUS_ERROR_DESTINATION_NOT_FOUND));
    EXPECT_EQ(
        RBUS_DIAGNOSTICS_OUTCOME_INVALID_REQUEST,
        rbusDiagnosticsClassifyOutcome(RBUS_ERROR_INVALID_INPUT));
    EXPECT_EQ(
        RBUS_DIAGNOSTICS_OUTCOME_PERMISSION_DENIED,
        rbusDiagnosticsClassifyOutcome(RBUS_ERROR_NOT_WRITABLE));
    EXPECT_EQ(
        RBUS_DIAGNOSTICS_OUTCOME_UNKNOWN_ERROR,
        rbusDiagnosticsClassifyOutcome(RBUS_ERROR_ASYNC_RESPONSE));
}

TEST_F(RbusDiagnosticsTest, ReporterAbsenceDoesNotPreventSubmissionAttempt)
{
    const rbusDiagnosticsObservation_t observation = makeObservation(
        RBUS_DIAGNOSTICS_OPERATION_GET,
        RBUS_DIAGNOSTICS_OUTCOME_SUCCESS);

    /*
     * The production endpoint is deliberately absent. Submission failure is
     * contained in the publisher and cannot change a caller-owned RBus result.
     */
    EXPECT_FALSE(rbusDiagnosticsClientSubmit(&observation));
    EXPECT_EQ(UINT64_C(0), rbusDiagnosticsClientGetDroppedObservationsForTest());
}

TEST_F(RbusDiagnosticsTest, DisabledCollectionAvoidsTimingAndPublication)
{
    DiagnosticsDatagramReceiver receiver;
    const rbusDiagnosticsObservation_t observation = makeObservation(
        RBUS_DIAGNOSTICS_OPERATION_GET,
        RBUS_DIAGNOSTICS_OUTCOME_SUCCESS);

    ASSERT_TRUE(rbusDiagnosticsClientSetEndpointForTest(receiver.endpoint()));
    rbusDiagnosticsClientSetEnabledForTest(false);

    EXPECT_FALSE(rbusDiagnosticsClientIsEnabled());
    EXPECT_EQ(UINT64_C(0), rbusDiagnosticsCaptureStartUs());
    EXPECT_FALSE(rbusDiagnosticsClientSubmit(&observation));

    rbusDiagnosticsObservation_t received = {};
    EXPECT_FALSE(receiver.receive(&received));
}

TEST_F(RbusDiagnosticsTest, CompletedResultIsPreservedInExactlyOneObservation)
{
    DiagnosticsDatagramReceiver receiver;
    rbusDiagnosticsObservation_t received = {};

    ASSERT_TRUE(rbusDiagnosticsClientSetEndpointForTest(receiver.endpoint()));

    /*
     * PublishCompleted is the terminal-path adapter: its return-free contract
     * leaves the application result untouched while emitting one final outcome.
     */
    const rbusError_t result = RBUS_ERROR_TIMEOUT;
    const uint64_t startUs = rbusDiagnosticsCaptureStartUs();
    ASSERT_NE(UINT64_C(0), startUs);

    rbusDiagnosticsPublishCompleted(
        RBUS_DIAGNOSTICS_OPERATION_ASYNC_METHOD,
        startUs,
        result);

    EXPECT_EQ(RBUS_ERROR_TIMEOUT, result);
    ASSERT_TRUE(receiver.receive(&received));
    EXPECT_EQ(RBUS_DIAGNOSTICS_OPERATION_ASYNC_METHOD, received.operation);
    EXPECT_EQ(RBUS_DIAGNOSTICS_OUTCOME_TIMEOUT, received.outcome);
    EXPECT_GE(received.monotonicEndUs, startUs);
    EXPECT_GE(received.durationUs, UINT64_C(0));

    /*
     * A terminal publication creates one datagram only.  This mirrors the
     * asynchronous completion boundary, where rbus.c publishes before invoking
     * the application callback.
     */
    EXPECT_FALSE(receiver.receive(&received));
}

TEST_F(RbusDiagnosticsTest, EventAndSubscriptionResultsUseBoundedOperationCategories)
{
    DiagnosticsDatagramReceiver receiver;
    rbusDiagnosticsObservation_t received = {};
    const uint64_t startUs = rbusDiagnosticsCaptureStartUs();

    ASSERT_NE(UINT64_C(0), startUs);
    ASSERT_TRUE(rbusDiagnosticsClientSetEndpointForTest(receiver.endpoint()));

    rbusDiagnosticsPublishCompletedForPath(
        RBUS_DIAGNOSTICS_OPERATION_EVENT_PUBLISH, startUs,
        RBUS_ERROR_NOSUBSCRIBERS, "Device.WiFi.SSID.1.Status");
    ASSERT_TRUE(receiver.receive(&received));
    EXPECT_EQ(RBUS_DIAGNOSTICS_OPERATION_EVENT_PUBLISH, received.operation);
    EXPECT_EQ(RBUS_DIAGNOSTICS_OUTCOME_UNKNOWN_ERROR, received.outcome);
    EXPECT_STREQ("Device.WiFi.SSID.1.Status", received.path);

    rbusDiagnosticsPublishCompletedForPath(
        RBUS_DIAGNOSTICS_OPERATION_EVENT_SUBSCRIBE, startUs,
        RBUS_ERROR_SUCCESS, "Device.WiFi.SSID.1.Status");
    ASSERT_TRUE(receiver.receive(&received));
    EXPECT_EQ(RBUS_DIAGNOSTICS_OPERATION_EVENT_SUBSCRIBE, received.operation);
    EXPECT_EQ(RBUS_DIAGNOSTICS_OUTCOME_SUCCESS, received.outcome);

    rbusDiagnosticsPublishCompletedForPath(
        RBUS_DIAGNOSTICS_OPERATION_EVENT_UNSUBSCRIBE, startUs,
        RBUS_ERROR_INVALID_OPERATION, "Device.WiFi.SSID.1.Status");
    ASSERT_TRUE(receiver.receive(&received));
    EXPECT_EQ(RBUS_DIAGNOSTICS_OPERATION_EVENT_UNSUBSCRIBE, received.operation);
    EXPECT_EQ(RBUS_DIAGNOSTICS_OUTCOME_INVALID_REQUEST, received.outcome);

    rbusDiagnosticsPublishCompletedForPath(
        RBUS_DIAGNOSTICS_OPERATION_TABLE, startUs,
        RBUS_ERROR_TIMEOUT, "Device.WiFi.AccessPoint.2147483647.");
    ASSERT_TRUE(receiver.receive(&received));
    EXPECT_EQ(RBUS_DIAGNOSTICS_OPERATION_TABLE, received.operation);
    EXPECT_EQ(RBUS_DIAGNOSTICS_OUTCOME_TIMEOUT, received.outcome);
    EXPECT_STREQ("Device.WiFi.AccessPoint.2147483647.", received.path);

    /*
     * Null validation paths retain the existing invalid-input result while
     * diagnostics safely omit an unavailable table or row identity.
     */
    rbusDiagnosticsPublishCompletedForPath(
        RBUS_DIAGNOSTICS_OPERATION_TABLE, startUs,
        RBUS_ERROR_INVALID_INPUT, nullptr);
    ASSERT_TRUE(receiver.receive(&received));
    EXPECT_EQ(RBUS_DIAGNOSTICS_OPERATION_TABLE, received.operation);
    EXPECT_EQ(RBUS_DIAGNOSTICS_OUTCOME_INVALID_REQUEST, received.outcome);
    EXPECT_STREQ("", received.path);
}

TEST_F(RbusDiagnosticsTest, SaturationCountsOnlyObservationsBeyondBoundedQueue)
{
    const rbusDiagnosticsObservation_t observation = makeObservation(
        RBUS_DIAGNOSTICS_OPERATION_SET,
        RBUS_DIAGNOSTICS_OUTCOME_SUCCESS);

    /*
     * Keep the endpoint absent so each submission remains queued. The fixed
     * publisher queue holds 32 records; the 33rd reports one local loss rather
     * than allocating, blocking, or changing the RBus operation result.
     */
    for (size_t index = 0; index < 33U; ++index) {
        EXPECT_FALSE(rbusDiagnosticsClientSubmit(&observation));
    }

    EXPECT_EQ(
        UINT64_C(1),
        rbusDiagnosticsClientGetDroppedObservationsForTest());
}
