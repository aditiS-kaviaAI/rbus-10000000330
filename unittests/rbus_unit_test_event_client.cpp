/*
  * If not stated otherwise in this file or this component's Licenses.txt file
  * the following copyright and licenses apply:
  *
  * Copyright 2016 RDK Management
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
/******************************************************
Test Case : Testing rbus communications from client end
*******************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
extern "C" {
#include "rbuscore.h"
#include "rbus.h"

}
#include "gtest_app.h"
#include <chrono>
#include <condition_variable>
#include <mutex>

#define DEFAULT_RESULT_BUFFERSIZE 128

#define MAX_CLIENT_NAME 20


typedef struct
{
   char name[100];
   int age;
   bool work_status;
}test_struct_t;


char eventserver_kill[] = "killall -9 rbus_event_server";
#ifdef BUILD_FOR_DESKTOP
char eventserver_create[] = "./unittests/rbus_event_server alpha > /tmp/outp.txt  2>&1 &";
#else
char eventserver_create[] = "/usr/bin/rbus_event_server alpha > /tmp/outp.txt  2>&1 &";
#endif

class EventClientAPIs : public ::testing::Test{

protected:

static void SetUpTestCase()
{
    system(eventserver_create);
    sleep(2);
    printf("Set up done Successfully for EventClient\n");
}

static void TearDownTestCase()
{
    system(eventserver_kill);
    printf("Clean up done Successfully for EventClient\n");
}

};

static bool CALL_RBUS_OPEN_BROKER_CONNECTION(char* client_name)
{
    bool result = false;
    rbusCoreError_t err = RBUSCORE_SUCCESS;

    if((err = rbus_openBrokerConnection(client_name)) == RBUSCORE_SUCCESS)
    {
         printf("Successfully connected to bus.\n");
         result = true;
    }
    EXPECT_EQ(err, RBUSCORE_SUCCESS) << "rbus_openBrokerConnection failed";
    return result;
}

static bool CALL_RBUS_CLOSE_BROKER_CONNECTION()
{
    bool result = false;
    rbusCoreError_t err = RBUSCORE_SUCCESS;
    if((err = rbus_closeBrokerConnection()) == RBUSCORE_SUCCESS)
    {
        printf("Successfully disconnected from bus.\n");
        result = true;
    }
    EXPECT_EQ(err, RBUSCORE_SUCCESS) << "rbus_openBrokerConnection failed";
    return result;
}

static bool CALL_RBUS_PULL_OBJECT(char* expected_data, char* server_obj)
{
    bool result = false;
    rbusCoreError_t err = RBUSCORE_SUCCESS;
    rbusMessage response;
    if((err = rbus_pullObj(server_obj, 1000, &response)) == RBUSCORE_SUCCESS)
    {
        const char* buff = NULL;
        printf("Received object %s\n", server_obj);
        rbusMessage_GetString(response, &buff);
        printf("Payload: %s\n", buff);
        EXPECT_STREQ(buff, expected_data) << "rbus_pullObj failed to procure the server's initial string -init init init- ";
        rbusMessage_Release(response);
        result = true;
    }
    else
    {
        printf("Could not pull object %s\n", server_obj);
    }
    EXPECT_EQ(err, RBUSCORE_SUCCESS) << "rbus_pullObj failed";
    return result;
}

static int event_callback(const char * object_name,  const char * event_name, rbusMessage message, void * user_data) 
{
     (void) user_data;
     char* buff = NULL;
    uint32_t buff_length = 0;
    printf("In event callback for object %s, event %s.\n", object_name, event_name);
    rbusMessage_ToDebugString(message, &buff, &buff_length);
    printf("dumpMessage: %.*s\n", buff_length, buff);
    free(buff);
    return 0;
}

struct RbusEventCallbackReentryContext
{
    const char* eventName;
    std::mutex mutex;
    std::condition_variable callbackComplete;
    bool callbackEntered;
    bool callbackCompleted;
    bool eventSnapshotValid;
    bool subscriptionSnapshotValid;
    bool absentAfterUnsubscribe;
    bool presentAfterSubscribe;
    rbusError_t unsubscribeError;
    rbusError_t subscribeError;
};

/*
 * The callback deliberately re-enters RBUS using the same consumer handle.
 * Its recorded state is asserted by the test thread after a bounded wait so
 * that a lock re-entry regression cannot pass merely because no event arrived.
 */
static void rbus_event_callback_reentry_handler(
    rbusHandle_t handle,
    rbusEvent_t const* event,
    rbusEventSubscription_t* subscription)
{
    RbusEventCallbackReentryContext* context =
        static_cast<RbusEventCallbackReentryContext*>(subscription ? subscription->userData : NULL);

    if (context == NULL)
        return;

    {
        std::lock_guard<std::mutex> lock(context->mutex);
        context->callbackEntered = true;
        context->eventSnapshotValid = event != NULL && event->name != NULL &&
            strcmp(event->name, context->eventName) == 0;
        /* Capture callback-owned subscription details before mutating it. */
        context->subscriptionSnapshotValid = subscription->handle == handle &&
            subscription->eventName != NULL &&
            strcmp(subscription->eventName, context->eventName) == 0 &&
            subscription->userData == context;
    }
    context->callbackComplete.notify_all();

    context->unsubscribeError = rbusEvent_Unsubscribe(handle, context->eventName);
    context->absentAfterUnsubscribe =
        !rbusEvent_IsSubscriptionExist(handle, context->eventName, NULL);

    context->subscribeError = rbusEvent_Subscribe(
        handle,
        context->eventName,
        rbus_event_callback_reentry_handler,
        context,
        0);
    context->presentAfterSubscribe =
        rbusEvent_IsSubscriptionExist(handle, context->eventName, NULL);

    {
        std::lock_guard<std::mutex> lock(context->mutex);
        context->callbackCompleted = true;
    }
    context->callbackComplete.notify_all();
}

TEST(RbusEventCallbackReentry, SubscribeAndUnsubscribeOnSameHandleDoNotDeadlock)
{
    static const char eventName[] = "Device.RbusEventCallbackReentry.Event!";
    static const char providerName[] = "RbusEventCallbackReentryProvider";
    static const char consumerName[] = "RbusEventCallbackReentryConsumer";
    const std::chrono::seconds callbackDeadline(5);

    rbusHandle_t providerHandle = NULL;
    rbusHandle_t consumerHandle = NULL;
    rbusDataElement_t eventElement = {
        (char*)eventName,
        RBUS_ELEMENT_TYPE_EVENT,
        {NULL, NULL, NULL, NULL, NULL, NULL}
    };
    RbusEventCallbackReentryContext context = {
        eventName,
        std::mutex(),
        std::condition_variable(),
        false,
        false,
        false,
        false,
        false,
        false,
        RBUS_ERROR_BUS_ERROR,
        RBUS_ERROR_BUS_ERROR
    };

    ASSERT_EQ(rbus_open(&providerHandle, providerName), RBUS_ERROR_SUCCESS);
    ASSERT_EQ(rbus_regDataElements(providerHandle, 1, &eventElement), RBUS_ERROR_SUCCESS);
    ASSERT_EQ(rbus_open(&consumerHandle, consumerName), RBUS_ERROR_SUCCESS);
    ASSERT_EQ(
        rbusEvent_Subscribe(
            consumerHandle,
            eventName,
            rbus_event_callback_reentry_handler,
            &context,
            0),
        RBUS_ERROR_SUCCESS);

    rbusEvent_t event = {eventName, RBUS_EVENT_GENERAL, NULL};
    ASSERT_EQ(rbusEvent_Publish(providerHandle, &event), RBUS_ERROR_SUCCESS);

    bool callbackEntered = false;
    bool callbackCompleted = false;
    {
        std::unique_lock<std::mutex> lock(context.mutex);
        callbackEntered = context.callbackComplete.wait_for(
            lock,
            callbackDeadline,
            [&context] { return context.callbackEntered; });
        callbackCompleted = callbackEntered && context.callbackComplete.wait_for(
            lock,
            callbackDeadline,
            [&context] { return context.callbackCompleted; });
    }

    ASSERT_TRUE(callbackEntered) << "The published event did not reach the callback.";
    /*
     * Do not attempt connection teardown after this deadline failure: teardown
     * may wait on the same lock held by a regressed callback implementation.
     */
    ASSERT_TRUE(callbackCompleted)
        << "Nested same-handle unsubscribe/subscribe did not complete before the deadline.";

    EXPECT_TRUE(context.eventSnapshotValid);
    EXPECT_TRUE(context.subscriptionSnapshotValid);
    EXPECT_EQ(context.unsubscribeError, RBUS_ERROR_SUCCESS);
    EXPECT_TRUE(context.absentAfterUnsubscribe);
    EXPECT_EQ(context.subscribeError, RBUS_ERROR_SUCCESS);
    EXPECT_TRUE(context.presentAfterSubscribe);

    EXPECT_EQ(rbusEvent_Unsubscribe(consumerHandle, eventName), RBUS_ERROR_SUCCESS);
    EXPECT_EQ(rbus_close(consumerHandle), RBUS_ERROR_SUCCESS);
    EXPECT_EQ(rbus_unregDataElements(providerHandle, 1, &eventElement), RBUS_ERROR_SUCCESS);
    EXPECT_EQ(rbus_close(providerHandle), RBUS_ERROR_SUCCESS);
}

TEST_F(EventClientAPIs, sample_test)
{
    EXPECT_EQ(1, 1);
}

TEST_F(EventClientAPIs, rbus_subscribeToEvent_test1)
{
    char client_name[MAX_CLIENT_NAME] = "Event_Client_1";
    char obj_name[20] = "alpha.obj1";
    bool conn_status = false;
    rbusCoreError_t err = RBUSCORE_SUCCESS;
    printf("*********************  CREATING CLIENT : %s \n", client_name);
    conn_status = CALL_RBUS_OPEN_BROKER_CONNECTION(client_name);
    ASSERT_EQ(conn_status, true) << "RBUS_OPEN_BROKER_CONNECTION failed";
    //Test with invalid objname passed
     err = rbus_subscribeToEvent(NULL, "event_1", &event_callback, NULL, NULL, NULL);
     ASSERT_EQ(err,RBUSCORE_ERROR_ENTRY_NOT_FOUND) << "rbus_subscribeToEvent failed";
    //Test with the event name  to be NULL
     err = rbus_subscribeToEvent(obj_name, "NULL", &event_callback, NULL, NULL, NULL);
     ASSERT_EQ(err,RBUSCORE_ERROR_GENERAL) << "rbus_subscribeToEvent failed";
    //Test with the callback to be NULL
     err = rbus_subscribeToEvent(obj_name, "event_1",NULL, NULL, NULL, NULL);
     ASSERT_EQ(err,RBUSCORE_ERROR_INVALID_PARAM) << "rbus_subscribeToEvent failed";
    //Test with the valid Event name, objname, Callback
     err = rbus_subscribeToEvent(obj_name, "event_1",&event_callback, NULL, NULL, NULL);
     ASSERT_EQ(err,RBUSCORE_SUCCESS) << "rbus_subscribeToEvent failed";
     printf("Subscribed Events with Event name: event_1 \n");
    //Test with the already subscribed Event
     err = rbus_subscribeToEvent(obj_name, "event_1",&event_callback, NULL, NULL, NULL);
     ASSERT_EQ(err,RBUSCORE_SUCCESS) << "rbus_subscribeToEvent failed";
    conn_status = CALL_RBUS_CLOSE_BROKER_CONNECTION();
    ASSERT_EQ(conn_status, true) << "RBUS_CLOSE_BROKER_CONNECTION failed";
}

TEST_F(EventClientAPIs, rbus_subscribeToEvent_test2)
{
    char client_name[MAX_CLIENT_NAME] = "Event_Client_1";
    char obj_name[MAX_OBJECT_NAME_LENGTH + 1] = "0";
    bool conn_status = false;
    rbusCoreError_t err = RBUSCORE_SUCCESS;
    printf("*********************  CREATING CLIENT : %s \n", client_name);
    conn_status = CALL_RBUS_OPEN_BROKER_CONNECTION(client_name);
    ASSERT_EQ(conn_status, true) << "RBUS_OPEN_BROKER_CONNECTION failed";
    //Boundary Test with MAX_OBJECT_NAME_LENGTH
    memset(obj_name, 't', (sizeof(obj_name)- 1));
    err = rbus_subscribeToEvent(obj_name, "event_1",&event_callback, NULL, NULL, NULL);
    ASSERT_EQ(err,RBUSCORE_ERROR_INVALID_PARAM) << "rbus_subscribeToEvent failed";
    conn_status = CALL_RBUS_CLOSE_BROKER_CONNECTION();
    ASSERT_EQ(conn_status, true) << "RBUS_CLOSE_BROKER_CONNECTION failed";

}

TEST_F(EventClientAPIs, rbus_subscribeToEventTimeout_test1)
{
    char client_name[MAX_CLIENT_NAME] = "Event_Client_1";
    char obj_name[20] = "alpha.obj1";
    bool conn_status = false;
    rbusCoreError_t err = RBUSCORE_SUCCESS;
    printf("*********************  CREATING CLIENT : %s \n", client_name);
    conn_status = CALL_RBUS_OPEN_BROKER_CONNECTION(client_name);
    ASSERT_EQ(conn_status, true) << "RBUS_OPEN_BROKER_CONNECTION failed";
    err = rbus_subscribeToEventTimeout(obj_name, "event_1",&event_callback, NULL, NULL, NULL, 1000, false, NULL, false);
    ASSERT_EQ(err,RBUSCORE_SUCCESS) << "rbus_subscribeToEventTimeout failed";
    err = rbus_subscribeToEventTimeout(obj_name, "event_1",&event_callback, NULL, NULL, NULL, 1000, false, NULL, false);
    ASSERT_EQ(err,RBUSCORE_SUCCESS) << "rbus_subscribeToEventTimeout failed";
    printf("********************Subscribed Events with Event name: event_1 in 1000 ms \n");
    conn_status = CALL_RBUS_CLOSE_BROKER_CONNECTION();
    ASSERT_EQ(conn_status, true) << "RBUS_CLOSE_BROKER_CONNECTION failed";
}

TEST_F(EventClientAPIs, rbus_subscribeToEventTimeout_test2)
{
    char client_name[MAX_CLIENT_NAME] = "Event_Client_1";
    char obj_name[20] = "alpha.obj1";
    bool conn_status = false;
    rbusCoreError_t err = RBUSCORE_SUCCESS;
    printf("*********************  CREATING CLIENT : %s \n", client_name);
    conn_status = CALL_RBUS_OPEN_BROKER_CONNECTION(client_name);
    ASSERT_EQ(conn_status, true) << "RBUS_OPEN_BROKER_CONNECTION failed";
    //Test with invalid objname passed
    err = rbus_subscribeToEventTimeout(NULL, "event_1", &event_callback, NULL, NULL, NULL, 1000, false, NULL, false);
    ASSERT_EQ(err,RBUSCORE_ERROR_ENTRY_NOT_FOUND) << "rbus_subscribeToEventTimeout failed";
    //Test with the event name  to be NULL
    err = rbus_subscribeToEventTimeout(obj_name, "NULL", &event_callback, NULL, NULL, NULL, 1000, false, NULL, false);
    ASSERT_EQ(err,RBUSCORE_ERROR_GENERAL) << "rbus_subscribeToEventTimeout failed";
    //Test with the callback to be NULL
    err = rbus_subscribeToEventTimeout(obj_name, "event_1",NULL, NULL, NULL, NULL, 1000, false, NULL, false);
    ASSERT_EQ(err,RBUSCORE_ERROR_INVALID_PARAM) << "rbus_subscribeToEventTimeout failed";
    //Test with the valid Event name, objname, Callback
    err = rbus_subscribeToEventTimeout(obj_name, "event_1",&event_callback, NULL, NULL, NULL, 1000, false, NULL, false);
    ASSERT_EQ(err,RBUSCORE_SUCCESS) << "rbus_subscribeToEventTimeout failed";
    printf("Subscribed Events with Event name: event_1 \n");
    //Test with the already subscribed Event
    err = rbus_subscribeToEventTimeout(obj_name, "event_1",&event_callback, NULL, NULL, NULL, 1000, false, NULL, false);
    ASSERT_EQ(err,RBUSCORE_SUCCESS) << "rbus_subscribeToEventTimeout failed";
    conn_status = CALL_RBUS_CLOSE_BROKER_CONNECTION();
    ASSERT_EQ(conn_status, true) << "RBUS_CLOSE_BROKER_CONNECTION failed";
}

TEST_F(EventClientAPIs, rbus_subscribeToEventTimeout_test3)
{
    char client_name[MAX_CLIENT_NAME] = "Event_Client_1";
    char obj_name[MAX_OBJECT_NAME_LENGTH + 1] = "0";
    bool conn_status = false;
    rbusCoreError_t err = RBUSCORE_SUCCESS;
    printf("*********************  CREATING CLIENT : %s \n", client_name);
    conn_status = CALL_RBUS_OPEN_BROKER_CONNECTION(client_name);
    ASSERT_EQ(conn_status, true) << "RBUS_OPEN_BROKER_CONNECTION failed";
    //Boundary Test with MAX_OBJECT_NAME_LENGTH
    memset(obj_name, 't', (sizeof(obj_name)- 1));
    err = rbus_subscribeToEventTimeout(obj_name, "event_1",&event_callback, NULL, NULL, NULL, 1000, false, NULL, false);
    ASSERT_EQ(err,RBUSCORE_ERROR_INVALID_PARAM) << "rbus_subscribeToEventTimeout failed";
    conn_status = CALL_RBUS_CLOSE_BROKER_CONNECTION();
    ASSERT_EQ(conn_status, true) << "RBUS_CLOSE_BROKER_CONNECTION failed";
}

TEST_F(EventClientAPIs, rbus_unsubscribeFromEvent_test1)
{
    char client_name[MAX_CLIENT_NAME] = "Event_Client_1";
    char obj_name[20] = "alpha.obj1";
    bool conn_status = false;
    rbusCoreError_t err = RBUSCORE_SUCCESS;
    printf("*********************  CREATING CLIENT : %s \n", client_name);
    conn_status = CALL_RBUS_OPEN_BROKER_CONNECTION(client_name);
    ASSERT_EQ(conn_status, true) << "RBUS_OPEN_BROKER_CONNECTION failed";
    //Test with objname to be NULL
    err = rbus_unsubscribeFromEvent(NULL, "event_1", NULL, false);
    ASSERT_EQ(err,RBUSCORE_ERROR_ENTRY_NOT_FOUND) << "rbus_unsubscribeFromEvent failed";
   //Test with the event name  to be NULL
    err = rbus_unsubscribeFromEvent(obj_name, NULL, NULL, false);
    ASSERT_EQ(err,RBUSCORE_ERROR_GENERAL) << "rbus_unsubscribeFromEvent failed";
   //Test with the valid Event name, objname, Callback
    err = rbus_unsubscribeFromEvent(obj_name, "event_1", NULL, false);
    ASSERT_EQ(err,RBUSCORE_SUCCESS) << "rbus_unsubscribeFromEvent failed";
    //Test with the already unsubscribed Event
    err = rbus_unsubscribeFromEvent(obj_name, "event_1", NULL, false);
    ASSERT_EQ(err,RBUSCORE_SUCCESS) << "rbus_unsubscribeFromEvent failed";
    conn_status = CALL_RBUS_CLOSE_BROKER_CONNECTION();
    ASSERT_EQ(conn_status, true) << "RBUS_CLOSE_BROKER_CONNECTION failed";
}

TEST_F(EventClientAPIs, rbus_unsubscribeFromEvent_test2)
{
    char client_name[MAX_CLIENT_NAME] = "Event_Client_1";
    char obj_name[MAX_OBJECT_NAME_LENGTH + 1] = "0";
    bool conn_status = false;
    rbusCoreError_t err = RBUSCORE_SUCCESS;
    printf("*********************  CREATING CLIENT : %s \n", client_name);
    conn_status = CALL_RBUS_OPEN_BROKER_CONNECTION(client_name);
    ASSERT_EQ(conn_status, true) << "RBUS_OPEN_BROKER_CONNECTION failed";
    //Boundary Test with MAX_OBJECT_NAME_LENGTH
    memset(obj_name, 't', (sizeof(obj_name)- 1));
    err = rbus_subscribeToEvent(obj_name, "event_1",&event_callback, NULL, NULL, NULL);
    ASSERT_EQ(err,RBUSCORE_ERROR_INVALID_PARAM) << "rbus_subscribeToEvent failed";
    conn_status = CALL_RBUS_CLOSE_BROKER_CONNECTION();
    ASSERT_EQ(conn_status, true) << "RBUS_CLOSE_BROKER_CONNECTION failed";
}

TEST_F(EventClientAPIs, Data_eventPushPull_test1)
{
    char client_name[MAX_CLIENT_NAME] = "Event_Client_1";
    char obj_name[20] = "alpha.obj1";
    bool conn_status = false;
    rbusCoreError_t err = RBUSCORE_SUCCESS;
    char test_string[] = "alpha init init init";
    printf("*********************  CREATING CLIENT : %s \n", client_name);
   conn_status = CALL_RBUS_OPEN_BROKER_CONNECTION(client_name);
    ASSERT_EQ(conn_status, true) << "RBUS_OPEN_BROKER_CONNECTION failed";
    //Test with the valid Event name, objname, Callback
    err = rbus_subscribeToEvent(obj_name, "event_1",&event_callback, NULL, NULL, NULL);
    ASSERT_EQ(err,RBUSCORE_SUCCESS) << "rbus_subscribeToEvent failed";
    printf("Subscribed Events with Event name: event_1 \n");
    CALL_RBUS_PULL_OBJECT(test_string,obj_name);
    conn_status = CALL_RBUS_CLOSE_BROKER_CONNECTION();
    ASSERT_EQ(conn_status, true) << "RBUS_CLOSE_BROKER_CONNECTION failed";
}
