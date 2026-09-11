#include "DiagnosticsProtocol.h"
#include "DiagnosticsStore.h"
#include "ReporterServer.h"

#include <assert.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <unistd.h>

#include <string>
#include <thread>
#include <vector>

using namespace rbusdiagnostics;

namespace {

Observation CreateObservation(uint64_t publisherInstanceId, uint64_t sequence)
{
    Observation observation = {};
    observation.publisherInstanceId = publisherInstanceId;
    observation.sequence = sequence;
    observation.operation = Operation::Get;
    observation.outcome = Outcome::Success;
    observation.durationMicroseconds = 20;
    observation.completionEpochMicroseconds = 100;
    observation.path = "Device.Test.";
    observation.provider = "org.rdk.Test";
    return observation;
}

void SendObservationFromPublisher(
    const std::string& socketPath,
    uint64_t publisherInstanceId,
    uint64_t sequence)
{
    std::vector<uint8_t> frame;
    assert(EncodeObservation(CreateObservation(publisherInstanceId, sequence), &frame));

    const int descriptor = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    assert(descriptor >= 0);

    struct sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    assert(socketPath.size() < sizeof(address.sun_path));
    address.sun_path[socketPath.copy(address.sun_path, sizeof(address.sun_path) - 1)] = '\0';

    assert(connect(
        descriptor,
        reinterpret_cast<const struct sockaddr*>(&address),
        sizeof(address)) == 0);
    assert(send(descriptor, frame.data(), frame.size(), MSG_NOSIGNAL) ==
        static_cast<ssize_t>(frame.size()));
    close(descriptor);
}

void VerifyTwoIndependentPublishers()
{
    const std::string runtimeDirectory =
        std::string("/tmp/rbus-diagnostics-reporter-test-") + std::to_string(getpid());
    const std::string observationSocketPath = runtimeDirectory + "/observations.sock";

    DiagnosticsStore store(12, 34);
    ReporterServer server(
        &store,
        static_cast<uint32_t>(getegid()),
        static_cast<uint32_t>(geteuid()),
        runtimeDirectory.c_str());
    assert(server.Start());

    std::thread serverThread(&ReporterServer::Run, &server);

    const pid_t firstPublisher = fork();
    assert(firstPublisher >= 0);
    if (firstPublisher == 0) {
        SendObservationFromPublisher(observationSocketPath, 1001, 1);
        _exit(0);
    }

    const pid_t secondPublisher = fork();
    assert(secondPublisher >= 0);
    if (secondPublisher == 0) {
        SendObservationFromPublisher(observationSocketPath, 1002, 1);
        _exit(0);
    }

    int status = 0;
    assert(waitpid(firstPublisher, &status, 0) == firstPublisher);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(waitpid(secondPublisher, &status, 0) == secondPublisher);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    for (int attempt = 0; attempt < 100 && store.Snapshot().operations[0].requestCount != 2; ++attempt) {
        usleep(10000);
    }
    assert(store.Snapshot().operations[0].requestCount == 2);

    server.Stop();
    serverThread.join();
    assert(rmdir(runtimeDirectory.c_str()) == 0 || errno == ENOENT);
}

void VerifyBoundedDetailedDiagnostics()
{
    DiagnosticsStore store(12, 34);
    Observation observation = CreateObservation(99, 1);
    observation.durationMicroseconds = kSlowRequestThresholdMicroseconds;
    store.RecordObservation(observation);
    assert(store.Snapshot().slowRequestCount == 0);

    observation.durationMicroseconds = kSlowRequestThresholdMicroseconds + 1;
    for (size_t index = 0; index < kMaximumSlowRequests + 1; ++index) {
        observation.sequence = index + 2;
        observation.path = "Device.Slow." + std::to_string(index);
        store.RecordObservation(observation);
    }
    DiagnosticsSnapshot snapshot = store.Snapshot();
    assert(snapshot.slowRequestCount == kMaximumSlowRequests);
    assert(snapshot.slowRequestOverwrites == 1);

    for (size_t index = 0; index < kMaximumProviderRecords + 1; ++index) {
        observation.sequence++;
        observation.provider = "org.rdk.Provider" + std::to_string(index);
        store.RecordObservation(observation);
    }
    snapshot = store.Snapshot();
    assert(snapshot.operations[static_cast<size_t>(Operation::Get)].requestCount ==
        kMaximumSlowRequests + kMaximumProviderRecords + 3);
    assert(snapshot.providerCapacityOverflows == 2);

    store.RecordProviderLifecycle("org.rdk.Lifecycle", true);
    snapshot = store.Snapshot();
    for (size_t index = 0; index < snapshot.providers.size(); ++index) {
        if (snapshot.providers[index].inUse &&
            snapshot.providers[index].provider == "org.rdk.Lifecycle") {
            assert(snapshot.providers[index].state == ProviderState::Starting);
        }
    }
    store.RecordProviderLifecycle("org.rdk.Lifecycle", false);
    snapshot = store.Snapshot();
    for (size_t index = 0; index < snapshot.providers.size(); ++index) {
        if (snapshot.providers[index].inUse &&
            snapshot.providers[index].provider == "org.rdk.Lifecycle") {
            assert(snapshot.providers[index].state == ProviderState::Stopped);
        }
    }

    store.RecordPublisherStatus(700, 10, 3);
    store.RecordPublisherStatus(700, 10, 2);
    store.RecordPublisherStatus(700, 8, 8);
    store.RecordPublisherStatus(700, 15, 4);
    snapshot = store.Snapshot();
    assert(snapshot.publishers[1].inUse || snapshot.publishers[0].inUse);
    bool foundPublisher = false;
    for (size_t index = 0; index < snapshot.publishers.size(); ++index) {
        if (snapshot.publishers[index].inUse &&
            snapshot.publishers[index].publisherInstanceId == 700) {
            foundPublisher = true;
            assert(snapshot.publishers[index].lastSequence == 15);
            assert(snapshot.publishers[index].inferredLostRecords == 4);
            // Cumulative drops never regress when duplicate or out-of-order
            // status records report a lower value.
            assert(snapshot.publishers[index].cumulativeLocalDrops == 8);
        }
    }
    assert(foundPublisher);

    assert(store.Reset() == 1);
    snapshot = store.Snapshot();
    assert(snapshot.slowRequestCount == 0);
    assert(snapshot.providerCapacityOverflows == 0);
    assert(!snapshot.publishers[0].inUse);
}

void VerifyLifecycleAndStatusProtocol()
{
    ProviderLifecycle lifecycle = {};
    lifecycle.publisherInstanceId = 400;
    lifecycle.sequence = 41;
    lifecycle.registered = true;
    lifecycle.provider = "org.rdk.LifecycleProtocol";

    std::vector<uint8_t> frame;
    assert(EncodeProviderLifecycle(lifecycle, &frame));
    ProviderLifecycle decodedLifecycle = {};
    assert(DecodeProviderLifecycle(frame.data(), frame.size(), &decodedLifecycle));
    assert(decodedLifecycle.publisherInstanceId == lifecycle.publisherInstanceId);
    assert(decodedLifecycle.sequence == lifecycle.sequence);
    assert(decodedLifecycle.registered);
    assert(decodedLifecycle.provider == lifecycle.provider);

    PublisherStatus status = {};
    status.publisherInstanceId = 400;
    status.sequence = 45;
    status.cumulativeLocalDrops = 7;
    assert(EncodePublisherStatus(status, &frame));
    PublisherStatus decodedStatus = {};
    assert(DecodePublisherStatus(frame.data(), frame.size(), &decodedStatus));
    assert(decodedStatus.publisherInstanceId == status.publisherInstanceId);
    assert(decodedStatus.sequence == status.sequence);
    assert(decodedStatus.cumulativeLocalDrops == status.cumulativeLocalDrops);

    frame[3] = 99;
    assert(!DecodePublisherStatus(frame.data(), frame.size(), &decodedStatus));

    DiagnosticsStore store(56, 78);
    store.RecordProviderLifecycle(decodedLifecycle.provider, decodedLifecycle.registered);
    store.RecordPublisherStatus(status.publisherInstanceId, status.sequence, status.cumulativeLocalDrops);
    const DiagnosticsSnapshot snapshot = store.Snapshot();
    bool foundProvider = false;
    bool foundPublisher = false;
    for (size_t index = 0; index < snapshot.providers.size(); ++index) {
        if (snapshot.providers[index].inUse &&
            snapshot.providers[index].provider == lifecycle.provider) {
            foundProvider = true;
            assert(snapshot.providers[index].state == ProviderState::Starting);
        }
    }
    for (size_t index = 0; index < snapshot.publishers.size(); ++index) {
        if (snapshot.publishers[index].inUse &&
            snapshot.publishers[index].publisherInstanceId == status.publisherInstanceId) {
            foundPublisher = true;
            assert(snapshot.publishers[index].lastSequence == status.sequence);
            assert(snapshot.publishers[index].cumulativeLocalDrops == status.cumulativeLocalDrops);
        }
    }
    assert(foundProvider);
    assert(foundPublisher);
}

} // namespace

int main()
{
    Observation observation = CreateObservation(7, 1);

    std::vector<uint8_t> frame;
    assert(EncodeObservation(observation, &frame));

    Observation decoded = {};
    assert(DecodeObservation(frame.data(), frame.size(), &decoded));
    frame[7] ^= 1;
    assert(!DecodeObservation(frame.data(), frame.size(), &decoded));

    DiagnosticsStore store(12, 34);
    store.RecordObservation(observation);
    DiagnosticsSnapshot beforeReset = store.Snapshot();
    assert(beforeReset.measurementGeneration == 0);
    assert(beforeReset.operations[0].requestCount == 1);
    assert(beforeReset.operations[0].histogram.size() == 64);

    beforeReset.operations[0].requestCount = 99;
    assert(store.Snapshot().operations[0].requestCount == 1);

    assert(store.Reset() == 1);
    DiagnosticsSnapshot afterReset = store.Snapshot();
    assert(afterReset.measurementGeneration == 1);
    assert(afterReset.operations[0].requestCount == 0);

    store.RecordObservation(observation);
    assert(store.Snapshot().operations[0].requestCount == 1);

    VerifyTwoIndependentPublishers();
    VerifyBoundedDetailedDiagnostics();
    VerifyLifecycleAndStatusProtocol();
    return 0;
}
