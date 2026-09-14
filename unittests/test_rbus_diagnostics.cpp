/*
 * If not stated otherwise in this file or this component's Licenses.txt file
 * the following copyright and licenses apply:
 *
 * Copyright 2025 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "gtest/gtest.h"

extern "C"
{
#include "rbus_diagnostics_collector.h"
}

#include <cstdio>
#include <cstring>

namespace
{

uint64_t g_test_clock_us = 0;
uint32_t g_broker_get_calls = 0;
uint32_t g_broker_set_calls = 0;
uint32_t g_broker_method_calls = 0;
uint32_t g_broker_table_add_calls = 0;
uint32_t g_broker_table_remove_calls = 0;

const char* const kBrokerProperty = "Device.DiagnosticsCompatibility.Value";
/* Providers register a row wildcard; consumers address the concrete table. */
const char* const kBrokerTableRegistration = "Device.DiagnosticsCompatibility.Table.{i}.";
const char* const kBrokerTable = "Device.DiagnosticsCompatibility.Table.";
const char* const kBrokerMethod = "Device.DiagnosticsCompatibility.Method()";

uint64_t deterministic_clock()
{
    return g_test_clock_us;
}

/*
 * These handlers deliberately do no deferred work.  The compatibility test
 * checks their counters immediately after each caller-side return so that a
 * diagnostics hook cannot change synchronous terminal-path sequencing.
 */
rbusError_t BrokerGetHandler(
    rbusHandle_t handle,
    rbusProperty_t property,
    rbusGetHandlerOptions_t* options)
{
    rbusValue_t value = NULL;

    (void)handle;
    (void)options;
    ++g_broker_get_calls;
    rbusValue_Init(&value);
    rbusValue_SetInt32(value, 42);
    rbusProperty_SetValue(property, value);
    rbusValue_Release(value);
    return RBUS_ERROR_SUCCESS;
}

rbusError_t BrokerSetHandler(
    rbusHandle_t handle,
    rbusProperty_t property,
    rbusSetHandlerOptions_t* options)
{
    (void)handle;
    (void)property;
    (void)options;
    ++g_broker_set_calls;
    return RBUS_ERROR_SUCCESS;
}

rbusError_t BrokerMethodHandler(
    rbusHandle_t handle,
    char const* method_name,
    rbusObject_t in_params,
    rbusObject_t out_params,
    rbusMethodAsyncHandle_t async_handle)
{
    rbusValue_t value = NULL;

    (void)handle;
    (void)method_name;
    (void)in_params;
    (void)async_handle;
    ++g_broker_method_calls;
    rbusValue_Init(&value);
    rbusValue_SetString(value, "completed");
    rbusObject_SetValue(out_params, "result", value);
    rbusValue_Release(value);
    return RBUS_ERROR_SUCCESS;
}

rbusError_t BrokerTableAddHandler(
    rbusHandle_t handle,
    char const* table_name,
    char const* alias_name,
    uint32_t* instance_number)
{
    (void)handle;
    (void)table_name;
    (void)alias_name;
    ++g_broker_table_add_calls;
    *instance_number = 1;
    return RBUS_ERROR_SUCCESS;
}

rbusError_t BrokerTableRemoveHandler(
    rbusHandle_t handle,
    char const* row_name)
{
    (void)handle;
    (void)row_name;
    ++g_broker_table_remove_calls;
    return RBUS_ERROR_SUCCESS;
}

class RBusDiagnosticsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        rbus_diagnostics_configuration_t configuration = {};

        ASSERT_EQ(RBUS_DIAGNOSTICS_STATUS_OK, rbusDiagnostics_Reset());
        configuration.enabled = true;
        configuration.slow_request_threshold_us = 10;
        ASSERT_EQ(
            RBUS_DIAGNOSTICS_STATUS_OK,
            rbusDiagnostics_SetConfiguration(&configuration));

        g_test_clock_us = 1234;
        rbusDiagnosticsClock_SetTestClock(deterministic_clock);
    }

    void TearDown() override
    {
        rbus_diagnostics_configuration_t configuration = {};

        rbusDiagnosticsClock_SetTestClock(NULL);
        rbusDiagnostics_Reset();
        configuration.enabled = false;
        configuration.slow_request_threshold_us = 10;
        rbusDiagnostics_SetConfiguration(&configuration);
    }

    void Record(
        rbusError_t result,
        uint64_t latency_us,
        const char* provider_name = NULL,
        const char* operation_name = "Device.Diagnostics.Test")
    {
        rbus_diagnostics_observation_t observation = {};

        observation.operation = RBUS_DIAGNOSTICS_OPERATION_GET;
        observation.rbus_result = result;
        observation.started_monotonic_us = 100;
        observation.completed_monotonic_us = 100 + latency_us;
        observation.completed_epoch_ms = 500;
        observation.operation_name = operation_name;
        observation.provider_name = provider_name;
        rbusDiagnosticsCollector_RecordCompleted(&observation);
    }

    rbus_diagnostics_global_snapshot_t GlobalSnapshot()
    {
        rbus_diagnostics_global_snapshot_t snapshot = {};

        EXPECT_EQ(
            RBUS_DIAGNOSTICS_STATUS_OK,
            rbusDiagnostics_GetGlobalSnapshot(&snapshot));
        return snapshot;
    }
};

TEST_F(RBusDiagnosticsTest, RecordsSuccessErrorTimeoutAndUnknownWithoutChangingResults)
{
    rbus_diagnostics_slow_request_snapshot_t slow_requests[4] = {};
    uint32_t slow_count = 0;
    rbus_diagnostics_global_snapshot_t snapshot;

    Record(RBUS_ERROR_SUCCESS, 11, "provider");
    Record(RBUS_ERROR_TIMEOUT, 12, "provider");
    Record(RBUS_ERROR_INVALID_INPUT, 13, "provider");
    Record(static_cast<rbusError_t>(999), 14, "provider");

    snapshot = GlobalSnapshot();
    EXPECT_EQ(4U, snapshot.request_count);
    EXPECT_EQ(1U, snapshot.success_count);
    EXPECT_EQ(3U, snapshot.error_count);
    EXPECT_EQ(1U, snapshot.timeout_count);
    EXPECT_EQ(11U, snapshot.min_latency_us);
    EXPECT_EQ(14U, snapshot.max_latency_us);

    ASSERT_EQ(
        RBUS_DIAGNOSTICS_STATUS_OK,
        rbusDiagnostics_ListSlowRequests(4, slow_requests, 4, &slow_count));
    ASSERT_EQ(4U, slow_count);
    EXPECT_EQ(RBUS_DIAGNOSTICS_OUTCOME_SUCCESS, slow_requests[0].outcome);
    EXPECT_EQ(RBUS_DIAGNOSTICS_OUTCOME_TIMEOUT, slow_requests[1].outcome);
    EXPECT_EQ(RBUS_DIAGNOSTICS_OUTCOME_INVALID_REQUEST, slow_requests[2].outcome);
    EXPECT_EQ(RBUS_DIAGNOSTICS_OUTCOME_UNKNOWN, slow_requests[3].outcome);
}

TEST_F(RBusDiagnosticsTest, MakesPercentilesAvailableAtTwentySamplesUsingBucketUpperBounds)
{
    rbus_diagnostics_global_snapshot_t snapshot;
    uint32_t index;

    for (index = 0; index < 19; ++index)
    {
        Record(RBUS_ERROR_SUCCESS, 1);
    }

    snapshot = GlobalSnapshot();
    EXPECT_FALSE(snapshot.percentiles_available);
    EXPECT_EQ(0U, snapshot.p50_latency_us);
    EXPECT_EQ(0U, snapshot.p90_latency_us);
    EXPECT_EQ(0U, snapshot.p95_latency_us);
    EXPECT_EQ(0U, snapshot.p99_latency_us);

    /* Ranks 10, 18, 19, and 20 select bounds 1, 4, 16, and 32 respectively. */
    for (index = 0; index < 9; ++index)
    {
        Record(RBUS_ERROR_SUCCESS, 3);
    }
    Record(RBUS_ERROR_SUCCESS, 9);
    Record(RBUS_ERROR_SUCCESS, 17);

    snapshot = GlobalSnapshot();
    ASSERT_EQ(30U, snapshot.request_count);
    ASSERT_TRUE(snapshot.percentiles_available);
    EXPECT_EQ(1U, snapshot.p50_latency_us);
    EXPECT_EQ(4U, snapshot.p90_latency_us);
    EXPECT_EQ(16U, snapshot.p95_latency_us);
    EXPECT_EQ(32U, snapshot.p99_latency_us);
}

TEST_F(RBusDiagnosticsTest, RetainsBoundedProvidersAndTruncatesIdentifiers)
{
    rbus_diagnostics_provider_snapshot_t providers[RBUS_DIAGNOSTICS_MAX_PROVIDERS] = {};
    rbus_diagnostics_provider_snapshot_t provider = {};
    uint32_t provider_count = 0;
    char provider_name[32];
    char long_provider[RBUS_DIAGNOSTICS_MAX_IDENTIFIER_LENGTH + 20] = {};

    for (uint32_t index = 0; index < RBUS_DIAGNOSTICS_MAX_PROVIDERS; ++index)
    {
        std::snprintf(provider_name, sizeof(provider_name), "provider-%03u", index);
        Record(RBUS_ERROR_SUCCESS, 1, provider_name);
    }

    Record(RBUS_ERROR_SUCCESS, 1, "provider-over-capacity");
    ASSERT_EQ(
        RBUS_DIAGNOSTICS_STATUS_OK,
        rbusDiagnostics_ListProviders(
            RBUS_DIAGNOSTICS_MAX_PROVIDERS,
            providers,
            RBUS_DIAGNOSTICS_MAX_PROVIDERS,
            &provider_count));
    EXPECT_EQ(RBUS_DIAGNOSTICS_MAX_PROVIDERS, provider_count);
    EXPECT_EQ(
        RBUS_DIAGNOSTICS_STATUS_NOT_FOUND,
        rbusDiagnostics_GetProviderSnapshot("provider-over-capacity", &provider));

    std::memset(long_provider, 'p', sizeof(long_provider) - 1);
    Record(RBUS_ERROR_SUCCESS, 1, long_provider);
    EXPECT_EQ(
        RBUS_DIAGNOSTICS_STATUS_NOT_FOUND,
        rbusDiagnostics_GetProviderSnapshot(long_provider, &provider));
}

TEST_F(RBusDiagnosticsTest, RetainsSlowRequestsStrictlyAboveThresholdAndOverwritesChronologically)
{
    rbus_diagnostics_slow_request_snapshot_t slow_requests[RBUS_DIAGNOSTICS_MAX_SLOW_REQUESTS] = {};
    uint32_t slow_count = 0;
    char operation_name[32];

    Record(RBUS_ERROR_SUCCESS, 10, NULL, "at-threshold");
    ASSERT_EQ(
        RBUS_DIAGNOSTICS_STATUS_OK,
        rbusDiagnostics_ListSlowRequests(1, slow_requests, 1, &slow_count));
    EXPECT_EQ(0U, slow_count);

    for (uint32_t index = 0; index <= RBUS_DIAGNOSTICS_MAX_SLOW_REQUESTS; ++index)
    {
        std::snprintf(operation_name, sizeof(operation_name), "request-%03u", index);
        Record(RBUS_ERROR_SUCCESS, 11, NULL, operation_name);
    }

    ASSERT_EQ(
        RBUS_DIAGNOSTICS_STATUS_OK,
        rbusDiagnostics_ListSlowRequests(
            RBUS_DIAGNOSTICS_MAX_SLOW_REQUESTS,
            slow_requests,
            RBUS_DIAGNOSTICS_MAX_SLOW_REQUESTS,
            &slow_count));
    ASSERT_EQ(RBUS_DIAGNOSTICS_MAX_SLOW_REQUESTS, slow_count);
    EXPECT_STREQ("request-001", slow_requests[0].operation_name);
    EXPECT_STREQ("request-100", slow_requests[RBUS_DIAGNOSTICS_MAX_SLOW_REQUESTS - 1].operation_name);
}

TEST_F(RBusDiagnosticsTest, ResetStartsANewEpochAndClearsSnapshotState)
{
    rbus_diagnostics_global_snapshot_t before_reset;
    rbus_diagnostics_global_snapshot_t after_reset;

    Record(RBUS_ERROR_SUCCESS, 20, "provider");
    before_reset = GlobalSnapshot();

    ASSERT_EQ(RBUS_DIAGNOSTICS_STATUS_OK, rbusDiagnostics_Reset());
    after_reset = GlobalSnapshot();

    EXPECT_EQ(before_reset.epoch + 1U, after_reset.epoch);
    EXPECT_EQ(0U, after_reset.request_count);
    EXPECT_EQ(0U, after_reset.success_count);
    EXPECT_EQ(0U, after_reset.error_count);
    EXPECT_FALSE(after_reset.latency_available);
    EXPECT_FALSE(after_reset.percentiles_available);
}

TEST_F(RBusDiagnosticsTest, ReturnsDisabledWithoutRecordingAndValidatesCallerOwnedBuffers)
{
    rbus_diagnostics_configuration_t configuration = {};
    rbus_diagnostics_global_snapshot_t snapshot = {};
    rbus_diagnostics_provider_snapshot_t providers[1] = {};
    uint32_t output_count = 0;

    configuration.enabled = false;
    configuration.slow_request_threshold_us = 10;
    ASSERT_EQ(
        RBUS_DIAGNOSTICS_STATUS_OK,
        rbusDiagnostics_SetConfiguration(&configuration));

    Record(RBUS_ERROR_SUCCESS, 100, "provider");
    EXPECT_EQ(
        RBUS_DIAGNOSTICS_STATUS_DISABLED,
        rbusDiagnostics_GetGlobalSnapshot(&snapshot));

    configuration.enabled = true;
    ASSERT_EQ(
        RBUS_DIAGNOSTICS_STATUS_OK,
        rbusDiagnostics_SetConfiguration(&configuration));
    snapshot = GlobalSnapshot();
    EXPECT_EQ(0U, snapshot.request_count);

    EXPECT_EQ(
        RBUS_DIAGNOSTICS_STATUS_LIMIT_INVALID,
        rbusDiagnostics_ListProviders(0, providers, 1, &output_count));
    EXPECT_EQ(
        RBUS_DIAGNOSTICS_STATUS_LIMIT_INVALID,
        rbusDiagnostics_ListProviders(1, providers, 0, &output_count));
    EXPECT_EQ(
        RBUS_DIAGNOSTICS_STATUS_LIMIT_INVALID,
        rbusDiagnostics_ListSlowRequests(1, NULL, 1, &output_count));
}

TEST_F(RBusDiagnosticsTest, BrokerRoutedTerminalPathsPreserveResultsWhenDiagnosticsIsEnabled)
{
    rbus_diagnostics_configuration_t configuration = {};
    rbus_diagnostics_global_snapshot_t snapshot = {};
    rbusDataElement_t elements[] = {
        {const_cast<char*>(kBrokerProperty), RBUS_ELEMENT_TYPE_PROPERTY,
            {BrokerGetHandler, BrokerSetHandler, NULL, NULL, NULL, NULL}},
        {const_cast<char*>(kBrokerTableRegistration), RBUS_ELEMENT_TYPE_TABLE,
            {NULL, NULL, BrokerTableAddHandler, BrokerTableRemoveHandler, NULL, NULL}},
        {const_cast<char*>(kBrokerMethod), RBUS_ELEMENT_TYPE_METHOD,
            {NULL, NULL, NULL, NULL, NULL, reinterpret_cast<void*>(BrokerMethodHandler)}}
    };
    rbusHandle_t provider = NULL;
    rbusHandle_t consumer = NULL;
    rbusValue_t value = NULL;
    rbusObject_t out_params = NULL;
    uint32_t instance_number = 0;
    rbusError_t result;

    /*
     * RBus unit-test deployments supply the broker independently.  Do not
     * convert a desktop-only build without that service into an unrelated
     * failure; a broker-enabled run executes every assertion below.
     */
    result = rbus_open(&provider, "DiagnosticsCompatibilityProvider");
    if (result != RBUS_ERROR_SUCCESS)
        GTEST_SKIP() << "An external RBus broker is required for terminal-path compatibility coverage.";

    ASSERT_EQ(RBUS_ERROR_SUCCESS, rbus_regDataElements(provider, 3, elements));
    result = rbus_open(&consumer, "DiagnosticsCompatibilityConsumer");
    if (result != RBUS_ERROR_SUCCESS)
    {
        rbus_unregDataElements(provider, 3, elements);
        rbus_close(provider);
        GTEST_SKIP() << "An external RBus broker is required for terminal-path compatibility coverage.";
    }

    g_broker_get_calls = 0;
    g_broker_set_calls = 0;
    g_broker_method_calls = 0;
    g_broker_table_add_calls = 0;
    g_broker_table_remove_calls = 0;

    configuration.enabled = false;
    configuration.slow_request_threshold_us = 10;
    ASSERT_EQ(RBUS_DIAGNOSTICS_STATUS_OK, rbusDiagnostics_SetConfiguration(&configuration));

    ASSERT_EQ(RBUS_ERROR_SUCCESS, rbus_get(consumer, kBrokerProperty, &value));
    ASSERT_EQ(1U, g_broker_get_calls);
    ASSERT_EQ(42, rbusValue_GetInt32(value));
    rbusValue_Release(value);
    value = NULL;

    rbusValue_Init(&value);
    rbusValue_SetInt32(value, 7);
    ASSERT_EQ(RBUS_ERROR_SUCCESS, rbus_set(consumer, kBrokerProperty, value, NULL));
    ASSERT_EQ(1U, g_broker_set_calls);
    rbusValue_Release(value);
    value = NULL;

    ASSERT_EQ(RBUS_ERROR_SUCCESS, rbusMethod_Invoke(consumer, kBrokerMethod, NULL, &out_params));
    ASSERT_EQ(1U, g_broker_method_calls);
    ASSERT_STREQ("completed", rbusValue_GetString(rbusObject_GetValue(out_params, "result"), NULL));
    rbusObject_Release(out_params);
    out_params = NULL;

    ASSERT_EQ(RBUS_ERROR_SUCCESS, rbusTable_addRow(consumer, kBrokerTable, NULL, &instance_number));
    ASSERT_EQ(1U, g_broker_table_add_calls);
    ASSERT_EQ(1U, instance_number);
    ASSERT_EQ(RBUS_ERROR_SUCCESS, rbusTable_removeRow(consumer, "Device.DiagnosticsCompatibility.Table.1"));
    ASSERT_EQ(1U, g_broker_table_remove_calls);
    EXPECT_EQ(RBUS_DIAGNOSTICS_STATUS_DISABLED, rbusDiagnostics_GetGlobalSnapshot(&snapshot));

    ASSERT_EQ(RBUS_DIAGNOSTICS_STATUS_OK, rbusDiagnostics_Reset());
    configuration.enabled = true;
    ASSERT_EQ(RBUS_DIAGNOSTICS_STATUS_OK, rbusDiagnostics_SetConfiguration(&configuration));

    ASSERT_EQ(RBUS_ERROR_SUCCESS, rbus_get(consumer, kBrokerProperty, &value));
    EXPECT_EQ(2U, g_broker_get_calls);
    EXPECT_EQ(42, rbusValue_GetInt32(value));
    rbusValue_Release(value);
    value = NULL;

    rbusValue_Init(&value);
    rbusValue_SetInt32(value, 7);
    ASSERT_EQ(RBUS_ERROR_SUCCESS, rbus_set(consumer, kBrokerProperty, value, NULL));
    EXPECT_EQ(2U, g_broker_set_calls);
    rbusValue_Release(value);
    value = NULL;

    ASSERT_EQ(RBUS_ERROR_SUCCESS, rbusMethod_Invoke(consumer, kBrokerMethod, NULL, &out_params));
    EXPECT_EQ(2U, g_broker_method_calls);
    EXPECT_STREQ("completed", rbusValue_GetString(rbusObject_GetValue(out_params, "result"), NULL));
    rbusObject_Release(out_params);
    out_params = NULL;

    ASSERT_EQ(RBUS_ERROR_SUCCESS, rbusTable_addRow(consumer, kBrokerTable, NULL, &instance_number));
    EXPECT_EQ(2U, g_broker_table_add_calls);
    EXPECT_EQ(1U, instance_number);
    ASSERT_EQ(RBUS_ERROR_SUCCESS, rbusTable_removeRow(consumer, "Device.DiagnosticsCompatibility.Table.1"));
    EXPECT_EQ(2U, g_broker_table_remove_calls);

    ASSERT_EQ(RBUS_DIAGNOSTICS_STATUS_OK, rbusDiagnostics_GetGlobalSnapshot(&snapshot));
    EXPECT_EQ(5U, snapshot.request_count);
    EXPECT_EQ(5U, snapshot.success_count);
    EXPECT_EQ(0U, snapshot.error_count);

    EXPECT_EQ(RBUS_ERROR_SUCCESS, rbus_close(consumer));
    EXPECT_EQ(RBUS_ERROR_SUCCESS, rbus_unregDataElements(provider, 3, elements));
    EXPECT_EQ(RBUS_ERROR_SUCCESS, rbus_close(provider));
}

TEST_F(RBusDiagnosticsTest, ExposesTheApprovedPrivateAbiAndDeterministicClockSeam)
{
    uint32_t major = 0;
    uint32_t minor = 0;

    rbusDiagnostics_GetAbiVersion(&major, &minor);
    EXPECT_EQ(RBUS_DIAGNOSTICS_ABI_MAJOR, major);
    EXPECT_EQ(RBUS_DIAGNOSTICS_ABI_MINOR, minor);
    EXPECT_EQ(1234U, rbusDiagnosticsClock_NowMonotonicUs());

    /*
     * Only the four approved synchronous operation categories are represented
     * by the internal operation enum. Deferred DEC-02 paths have no measured
     * operation kind and cannot be fabricated by the collector.
     */
    EXPECT_EQ(0, RBUS_DIAGNOSTICS_OPERATION_GET);
    EXPECT_EQ(3, RBUS_DIAGNOSTICS_OPERATION_TABLE);
}

} // namespace
