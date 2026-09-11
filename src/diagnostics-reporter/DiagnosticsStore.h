#ifndef RBUS_DIAGNOSTICS_STORE_H
#define RBUS_DIAGNOSTICS_STORE_H

#include "DiagnosticsProtocol.h"

#include <array>
#include <mutex>
#include <stdint.h>
#include <string>

namespace rbusdiagnostics {

static const size_t kMaximumProviderRecords = 64;
static const size_t kMaximumPathPrefixRecords = 128;
static const size_t kMaximumPublisherStatusRecords = 64;
static const size_t kMaximumSlowRequests = 128;
static const uint64_t kSlowRequestThresholdMicroseconds = 1000000;

enum class ProviderState : uint8_t {
    Unknown = 0,
    Starting,
    Healthy,
    Degraded,
    Unavailable,
    Stopped
};

struct OperationMetrics {
    uint64_t requestCount;
    uint64_t successCount;
    uint64_t errorCount;
    uint64_t timeoutCount;
    uint64_t durationSumMicroseconds;
    uint64_t minimumDurationMicroseconds;
    uint64_t maximumDurationMicroseconds;
    std::array<uint64_t, 64> histogram;
};

struct ProviderRecord {
    bool inUse;
    std::string provider;
    ProviderState state;
    OperationMetrics metrics;
};

struct PathPrefixRecord {
    bool inUse;
    std::string prefix;
    uint64_t requestCount;
};

struct SlowRequestRecord {
    Observation observation;
};

struct PublisherStatusRecord {
    bool inUse;
    uint64_t publisherInstanceId;
    uint64_t lastSequence;
    uint64_t inferredLostRecords;
    uint64_t cumulativeLocalDrops;
};

struct DiagnosticsSnapshot {
    uint64_t reporterInstanceId;
    uint64_t startupEpochMicroseconds;
    uint64_t measurementGeneration;
    uint64_t rejectedFrames;
    uint64_t providerCapacityOverflows;
    uint64_t pathCapacityOverflows;
    uint64_t publisherCapacityOverflows;
    uint64_t slowRequestOverwrites;
    std::array<OperationMetrics, static_cast<size_t>(Operation::Count)> operations;
    std::array<ProviderRecord, kMaximumProviderRecords> providers;
    std::array<PathPrefixRecord, kMaximumPathPrefixRecords> pathPrefixes;
    std::array<SlowRequestRecord, kMaximumSlowRequests> slowRequests;
    size_t slowRequestCount;
    std::array<PublisherStatusRecord, kMaximumPublisherStatusRecords> publishers;
};

/**
 * Owns bounded device-wide diagnostic state. Snapshot results are copies and
 * never expose mutable store collections to control clients.
 */
class DiagnosticsStore {
public:
    DiagnosticsStore(uint64_t reporterInstanceId, uint64_t startupEpochMicroseconds);

    void RecordObservation(const Observation& observation);
    void RecordProviderLifecycle(const std::string& provider, bool registered);
    void RecordPublisherStatus(
        uint64_t publisherInstanceId,
        uint64_t sequence,
        uint64_t cumulativeLocalDrops);
    void RecordRejectedFrame();
    DiagnosticsSnapshot Snapshot() const;
    uint64_t Reset();

private:
    static size_t HistogramBucket(uint64_t durationMicroseconds);
    static OperationMetrics EmptyMetrics();
    static std::string NormalizePathPrefix(const std::string& path);
    static ProviderState EvaluateProviderState(const OperationMetrics& metrics);
    static void UpdateMetrics(OperationMetrics* metrics, const Observation& observation);

    ProviderRecord* FindOrCreateProvider(const std::string& provider);
    PathPrefixRecord* FindOrCreatePathPrefix(const std::string& prefix);
    PublisherStatusRecord* FindOrCreatePublisher(uint64_t publisherInstanceId);
    void RetainSlowRequest(const Observation& observation);

    const uint64_t reporterInstanceId_;
    const uint64_t startupEpochMicroseconds_;
    mutable std::mutex mutex_;
    uint64_t measurementGeneration_;
    uint64_t rejectedFrames_;
    uint64_t providerCapacityOverflows_;
    uint64_t pathCapacityOverflows_;
    uint64_t publisherCapacityOverflows_;
    uint64_t slowRequestOverwrites_;
    size_t slowRequestCount_;
    size_t nextSlowRequestIndex_;
    std::array<OperationMetrics, static_cast<size_t>(Operation::Count)> operations_;
    std::array<ProviderRecord, kMaximumProviderRecords> providers_;
    std::array<PathPrefixRecord, kMaximumPathPrefixRecords> pathPrefixes_;
    std::array<SlowRequestRecord, kMaximumSlowRequests> slowRequests_;
    std::array<PublisherStatusRecord, kMaximumPublisherStatusRecords> publishers_;
};

} // namespace rbusdiagnostics

#endif
