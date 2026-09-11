#include "DiagnosticsStore.h"

#include <limits>

namespace rbusdiagnostics {

OperationMetrics DiagnosticsStore::EmptyMetrics()
{
    OperationMetrics metrics = {};
    metrics.minimumDurationMicroseconds = std::numeric_limits<uint64_t>::max();
    return metrics;
}

DiagnosticsStore::DiagnosticsStore(uint64_t reporterInstanceId, uint64_t startupEpochMicroseconds)
    : reporterInstanceId_(reporterInstanceId),
      startupEpochMicroseconds_(startupEpochMicroseconds),
      measurementGeneration_(0),
      rejectedFrames_(0),
      providerCapacityOverflows_(0),
      pathCapacityOverflows_(0),
      publisherCapacityOverflows_(0),
      slowRequestOverwrites_(0),
      slowRequestCount_(0),
      nextSlowRequestIndex_(0)
{
    for (size_t index = 0; index < operations_.size(); ++index) {
        operations_[index] = EmptyMetrics();
    }
    for (size_t index = 0; index < providers_.size(); ++index) {
        providers_[index].inUse = false;
        providers_[index].state = ProviderState::Unknown;
        providers_[index].metrics = EmptyMetrics();
    }
    for (size_t index = 0; index < pathPrefixes_.size(); ++index) {
        pathPrefixes_[index].inUse = false;
    }
    for (size_t index = 0; index < publishers_.size(); ++index) {
        publishers_[index].inUse = false;
    }
}

size_t DiagnosticsStore::HistogramBucket(uint64_t durationMicroseconds)
{
    size_t bucket = 0;
    while (durationMicroseconds > 1 && bucket < 63) {
        durationMicroseconds >>= 1;
        ++bucket;
    }
    return bucket;
}

void DiagnosticsStore::UpdateMetrics(OperationMetrics* metrics, const Observation& observation)
{
    ++metrics->requestCount;
    if (observation.outcome == Outcome::Success) {
        ++metrics->successCount;
    } else {
        ++metrics->errorCount;
    }
    if (observation.outcome == Outcome::Timeout) {
        ++metrics->timeoutCount;
    }

    if (std::numeric_limits<uint64_t>::max() - metrics->durationSumMicroseconds <
        observation.durationMicroseconds) {
        metrics->durationSumMicroseconds = std::numeric_limits<uint64_t>::max();
    } else {
        metrics->durationSumMicroseconds += observation.durationMicroseconds;
    }

    if (observation.durationMicroseconds < metrics->minimumDurationMicroseconds) {
        metrics->minimumDurationMicroseconds = observation.durationMicroseconds;
    }
    if (observation.durationMicroseconds > metrics->maximumDurationMicroseconds) {
        metrics->maximumDurationMicroseconds = observation.durationMicroseconds;
    }
    ++metrics->histogram[HistogramBucket(observation.durationMicroseconds)];
}

std::string DiagnosticsStore::NormalizePathPrefix(const std::string& path)
{
    if (path.empty()) {
        return std::string();
    }

    const std::string::size_type separator = path.find('.', 0);
    if (separator == std::string::npos) {
        return path;
    }

    const std::string::size_type secondSeparator = path.find('.', separator + 1);
    return path.substr(0, secondSeparator == std::string::npos ? path.size() : secondSeparator + 1);
}

ProviderState DiagnosticsStore::EvaluateProviderState(const OperationMetrics& metrics)
{
    if (metrics.requestCount < 20) {
        return ProviderState::Starting;
    }

    const bool degradedTimeouts = metrics.timeoutCount * 100 > metrics.requestCount * 5;
    const bool degradedErrors = metrics.errorCount * 100 > metrics.requestCount * 10;
    return degradedTimeouts || degradedErrors ? ProviderState::Degraded : ProviderState::Healthy;
}

ProviderRecord* DiagnosticsStore::FindOrCreateProvider(const std::string& provider)
{
    for (size_t index = 0; index < providers_.size(); ++index) {
        if (providers_[index].inUse && providers_[index].provider == provider) {
            return &providers_[index];
        }
    }
    for (size_t index = 0; index < providers_.size(); ++index) {
        if (!providers_[index].inUse) {
            providers_[index].inUse = true;
            providers_[index].provider = provider;
            providers_[index].state = ProviderState::Starting;
            providers_[index].metrics = EmptyMetrics();
            return &providers_[index];
        }
    }
    ++providerCapacityOverflows_;
    return NULL;
}

PathPrefixRecord* DiagnosticsStore::FindOrCreatePathPrefix(const std::string& prefix)
{
    for (size_t index = 0; index < pathPrefixes_.size(); ++index) {
        if (pathPrefixes_[index].inUse && pathPrefixes_[index].prefix == prefix) {
            return &pathPrefixes_[index];
        }
    }
    for (size_t index = 0; index < pathPrefixes_.size(); ++index) {
        if (!pathPrefixes_[index].inUse) {
            pathPrefixes_[index].inUse = true;
            pathPrefixes_[index].prefix = prefix;
            pathPrefixes_[index].requestCount = 0;
            return &pathPrefixes_[index];
        }
    }
    ++pathCapacityOverflows_;
    return NULL;
}

PublisherStatusRecord* DiagnosticsStore::FindOrCreatePublisher(uint64_t publisherInstanceId)
{
    for (size_t index = 0; index < publishers_.size(); ++index) {
        if (publishers_[index].inUse &&
            publishers_[index].publisherInstanceId == publisherInstanceId) {
            return &publishers_[index];
        }
    }
    for (size_t index = 0; index < publishers_.size(); ++index) {
        if (!publishers_[index].inUse) {
            publishers_[index].inUse = true;
            publishers_[index].publisherInstanceId = publisherInstanceId;
            publishers_[index].lastSequence = 0;
            publishers_[index].inferredLostRecords = 0;
            publishers_[index].cumulativeLocalDrops = 0;
            return &publishers_[index];
        }
    }
    ++publisherCapacityOverflows_;
    return NULL;
}

void DiagnosticsStore::RetainSlowRequest(const Observation& observation)
{
    if (observation.durationMicroseconds <= kSlowRequestThresholdMicroseconds) {
        return;
    }

    if (slowRequestCount_ == kMaximumSlowRequests) {
        ++slowRequestOverwrites_;
    } else {
        ++slowRequestCount_;
    }
    slowRequests_[nextSlowRequestIndex_].observation = observation;
    nextSlowRequestIndex_ = (nextSlowRequestIndex_ + 1) % kMaximumSlowRequests;
}

void DiagnosticsStore::RecordObservation(const Observation& observation)
{
    const size_t operationIndex = static_cast<size_t>(observation.operation);
    if (operationIndex >= operations_.size()) {
        RecordRejectedFrame();
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    UpdateMetrics(&operations_[operationIndex], observation);

    PublisherStatusRecord* publisher = FindOrCreatePublisher(observation.publisherInstanceId);
    if (publisher != NULL && observation.sequence > publisher->lastSequence) {
        if (publisher->lastSequence != 0 && observation.sequence > publisher->lastSequence + 1) {
            publisher->inferredLostRecords += observation.sequence - publisher->lastSequence - 1;
        }
        publisher->lastSequence = observation.sequence;
    }

    const std::string prefix = NormalizePathPrefix(observation.path);
    if (!prefix.empty()) {
        PathPrefixRecord* pathRecord = FindOrCreatePathPrefix(prefix);
        if (pathRecord != NULL) {
            ++pathRecord->requestCount;
        }
    }

    if (!observation.provider.empty()) {
        ProviderRecord* provider = FindOrCreateProvider(observation.provider);
        if (provider != NULL) {
            UpdateMetrics(&provider->metrics, observation);
            if (observation.outcome == Outcome::ProviderUnavailable) {
                provider->state = ProviderState::Unavailable;
            } else if (provider->state != ProviderState::Stopped) {
                provider->state = EvaluateProviderState(provider->metrics);
            }
        }
    }

    RetainSlowRequest(observation);
}

void DiagnosticsStore::RecordProviderLifecycle(const std::string& provider, bool registered)
{
    if (provider.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ProviderRecord* record = FindOrCreateProvider(provider);
    if (record != NULL) {
        record->state = registered ? EvaluateProviderState(record->metrics) : ProviderState::Stopped;
    }
}

void DiagnosticsStore::RecordPublisherStatus(
    uint64_t publisherInstanceId,
    uint64_t sequence,
    uint64_t cumulativeLocalDrops)
{
    std::lock_guard<std::mutex> lock(mutex_);
    PublisherStatusRecord* publisher = FindOrCreatePublisher(publisherInstanceId);
    if (publisher == NULL) {
        return;
    }

    if (sequence > publisher->lastSequence) {
        if (publisher->lastSequence != 0 && sequence > publisher->lastSequence + 1) {
            publisher->inferredLostRecords += sequence - publisher->lastSequence - 1;
        }
        publisher->lastSequence = sequence;
    }
    if (cumulativeLocalDrops > publisher->cumulativeLocalDrops) {
        publisher->cumulativeLocalDrops = cumulativeLocalDrops;
    }
}

void DiagnosticsStore::RecordRejectedFrame()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++rejectedFrames_;
}

DiagnosticsSnapshot DiagnosticsStore::Snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    DiagnosticsSnapshot snapshot = {};
    snapshot.reporterInstanceId = reporterInstanceId_;
    snapshot.startupEpochMicroseconds = startupEpochMicroseconds_;
    snapshot.measurementGeneration = measurementGeneration_;
    snapshot.rejectedFrames = rejectedFrames_;
    snapshot.providerCapacityOverflows = providerCapacityOverflows_;
    snapshot.pathCapacityOverflows = pathCapacityOverflows_;
    snapshot.publisherCapacityOverflows = publisherCapacityOverflows_;
    snapshot.slowRequestOverwrites = slowRequestOverwrites_;
    snapshot.operations = operations_;
    snapshot.providers = providers_;
    snapshot.pathPrefixes = pathPrefixes_;
    snapshot.slowRequests = slowRequests_;
    snapshot.slowRequestCount = slowRequestCount_;
    snapshot.publishers = publishers_;
    return snapshot;
}

uint64_t DiagnosticsStore::Reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++measurementGeneration_;
    rejectedFrames_ = 0;
    providerCapacityOverflows_ = 0;
    pathCapacityOverflows_ = 0;
    publisherCapacityOverflows_ = 0;
    slowRequestOverwrites_ = 0;
    slowRequestCount_ = 0;
    nextSlowRequestIndex_ = 0;
    for (size_t index = 0; index < operations_.size(); ++index) {
        operations_[index] = EmptyMetrics();
    }
    for (size_t index = 0; index < providers_.size(); ++index) {
        providers_[index].inUse = false;
        providers_[index].provider.clear();
        providers_[index].state = ProviderState::Unknown;
        providers_[index].metrics = EmptyMetrics();
    }
    for (size_t index = 0; index < pathPrefixes_.size(); ++index) {
        pathPrefixes_[index].inUse = false;
        pathPrefixes_[index].prefix.clear();
        pathPrefixes_[index].requestCount = 0;
    }
    for (size_t index = 0; index < publishers_.size(); ++index) {
        publishers_[index].inUse = false;
        publishers_[index].lastSequence = 0;
        publishers_[index].inferredLostRecords = 0;
        publishers_[index].cumulativeLocalDrops = 0;
    }
    return measurementGeneration_;
}

} // namespace rbusdiagnostics
