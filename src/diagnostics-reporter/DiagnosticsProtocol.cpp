#include "DiagnosticsProtocol.h"

#include <arpa/inet.h>
#include <limits.h>

namespace rbusdiagnostics {
namespace {

void AppendU16(std::vector<uint8_t>* output, uint16_t value)
{
    value = htons(value);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    output->insert(output->end(), bytes, bytes + sizeof(value));
}

void AppendU32(std::vector<uint8_t>* output, uint32_t value)
{
    value = htonl(value);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    output->insert(output->end(), bytes, bytes + sizeof(value));
}

void AppendU64(std::vector<uint8_t>* output, uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8) {
        output->push_back(static_cast<uint8_t>(value >> shift));
    }
}

bool ReadU16(const uint8_t** cursor, size_t* remaining, uint16_t* value)
{
    if (*remaining < 2) {
        return false;
    }
    *value = static_cast<uint16_t>((*cursor)[0] << 8 | (*cursor)[1]);
    *cursor += 2;
    *remaining -= 2;
    return true;
}

bool ReadU32(const uint8_t** cursor, size_t* remaining, uint32_t* value)
{
    if (*remaining < 4) {
        return false;
    }
    *value = (static_cast<uint32_t>((*cursor)[0]) << 24) |
        (static_cast<uint32_t>((*cursor)[1]) << 16) |
        (static_cast<uint32_t>((*cursor)[2]) << 8) |
        static_cast<uint32_t>((*cursor)[3]);
    *cursor += 4;
    *remaining -= 4;
    return true;
}

bool ReadU64(const uint8_t** cursor, size_t* remaining, uint64_t* value)
{
    if (*remaining < 8) {
        return false;
    }
    *value = 0;
    for (int index = 0; index < 8; ++index) {
        *value = (*value << 8) | (*cursor)[index];
    }
    *cursor += 8;
    *remaining -= 8;
    return true;
}

bool IsValidUtf8(const std::string& value)
{
    for (size_t index = 0; index < value.size();) {
        const uint8_t byte = static_cast<uint8_t>(value[index]);
        size_t continuationCount = 0;
        if (byte <= 0x7F) {
            continuationCount = 0;
        } else if (byte >= 0xC2 && byte <= 0xDF) {
            continuationCount = 1;
        } else if (byte >= 0xE0 && byte <= 0xEF) {
            continuationCount = 2;
        } else if (byte >= 0xF0 && byte <= 0xF4) {
            continuationCount = 3;
        } else {
            return false;
        }

        if (index + continuationCount >= value.size()) {
            return false;
        }
        for (size_t offset = 1; offset <= continuationCount; ++offset) {
            if ((static_cast<uint8_t>(value[index + offset]) & 0xC0) != 0x80) {
                return false;
            }
        }
        index += continuationCount + 1;
    }
    return true;
}

bool ReadIdentifier(const uint8_t** cursor, size_t* remaining, std::string* value)
{
    uint16_t length = 0;
    if (!ReadU16(cursor, remaining, &length) || length > kMaximumIdentifierBytes ||
        *remaining < length) {
        return false;
    }
    value->assign(reinterpret_cast<const char*>(*cursor), length);
    *cursor += length;
    *remaining -= length;
    return IsValidUtf8(*value);
}

bool IsValidOperation(uint8_t operation)
{
    return operation < static_cast<uint8_t>(Operation::Count);
}

bool IsValidOutcome(uint8_t outcome)
{
    return outcome < static_cast<uint8_t>(Outcome::Count);
}

bool BeginFrame(
    MessageType type,
    std::vector<uint8_t>* encoded,
    size_t maximumBytes)
{
    if (encoded == NULL) {
        return false;
    }

    encoded->clear();
    AppendU16(encoded, kProtocolVersion);
    AppendU16(encoded, static_cast<uint16_t>(type));
    AppendU32(encoded, 0);
    return encoded->size() <= maximumBytes;
}

bool FinishFrame(std::vector<uint8_t>* encoded, size_t maximumBytes)
{
    if (encoded->size() > maximumBytes || encoded->size() > UINT32_MAX) {
        encoded->clear();
        return false;
    }

    const uint32_t size = htonl(static_cast<uint32_t>(encoded->size()));
    (*encoded)[4] = reinterpret_cast<const uint8_t*>(&size)[0];
    (*encoded)[5] = reinterpret_cast<const uint8_t*>(&size)[1];
    (*encoded)[6] = reinterpret_cast<const uint8_t*>(&size)[2];
    (*encoded)[7] = reinterpret_cast<const uint8_t*>(&size)[3];
    return true;
}

bool BeginDecodingFrame(
    const uint8_t* data,
    size_t length,
    MessageType expectedType,
    const uint8_t** cursor,
    size_t* remaining)
{
    uint16_t version = 0;
    uint16_t type = 0;
    uint32_t declaredSize = 0;

    if (data == NULL || length > kMaximumObservationBytes) {
        return false;
    }

    *cursor = data;
    *remaining = length;
    return ReadU16(cursor, remaining, &version) &&
        ReadU16(cursor, remaining, &type) &&
        ReadU32(cursor, remaining, &declaredSize) &&
        version == kProtocolVersion &&
        type == static_cast<uint16_t>(expectedType) &&
        declaredSize == length;
}

} // namespace

bool EncodeObservation(const Observation& observation, std::vector<uint8_t>* encoded)
{
    if (observation.path.size() > kMaximumIdentifierBytes ||
        observation.provider.size() > kMaximumIdentifierBytes ||
        !IsValidUtf8(observation.path) || !IsValidUtf8(observation.provider) ||
        !IsValidOperation(static_cast<uint8_t>(observation.operation)) ||
        !IsValidOutcome(static_cast<uint8_t>(observation.outcome)) ||
        !BeginFrame(MessageType::Observation, encoded, kMaximumObservationBytes)) {
        return false;
    }

    AppendU64(encoded, observation.publisherInstanceId);
    AppendU64(encoded, observation.sequence);
    encoded->push_back(static_cast<uint8_t>(observation.operation));
    encoded->push_back(static_cast<uint8_t>(observation.outcome));
    AppendU32(encoded, static_cast<uint32_t>(observation.rbusError));
    AppendU64(encoded, observation.durationMicroseconds);
    AppendU64(encoded, observation.completionEpochMicroseconds);
    AppendU16(encoded, static_cast<uint16_t>(observation.path.size()));
    encoded->insert(encoded->end(), observation.path.begin(), observation.path.end());
    AppendU16(encoded, static_cast<uint16_t>(observation.provider.size()));
    encoded->insert(encoded->end(), observation.provider.begin(), observation.provider.end());
    return FinishFrame(encoded, kMaximumObservationBytes);
}

bool EncodeProviderLifecycle(
    const ProviderLifecycle& lifecycle,
    std::vector<uint8_t>* encoded)
{
    if (lifecycle.provider.empty() ||
        lifecycle.provider.size() > kMaximumIdentifierBytes ||
        !IsValidUtf8(lifecycle.provider) ||
        !BeginFrame(MessageType::ProviderLifecycle, encoded, kMaximumObservationBytes)) {
        return false;
    }

    AppendU64(encoded, lifecycle.publisherInstanceId);
    AppendU64(encoded, lifecycle.sequence);
    encoded->push_back(lifecycle.registered ? 1 : 0);
    AppendU16(encoded, static_cast<uint16_t>(lifecycle.provider.size()));
    encoded->insert(encoded->end(), lifecycle.provider.begin(), lifecycle.provider.end());
    return FinishFrame(encoded, kMaximumObservationBytes);
}

bool EncodePublisherStatus(
    const PublisherStatus& status,
    std::vector<uint8_t>* encoded)
{
    if (!BeginFrame(MessageType::PublisherStatus, encoded, kMaximumObservationBytes)) {
        return false;
    }

    AppendU64(encoded, status.publisherInstanceId);
    AppendU64(encoded, status.sequence);
    AppendU64(encoded, status.cumulativeLocalDrops);
    return FinishFrame(encoded, kMaximumObservationBytes);
}

bool DecodeObservation(const uint8_t* data, size_t length, Observation* observation)
{
    const uint8_t* cursor = NULL;
    size_t remaining = 0;
    uint8_t operation = 0;
    uint8_t outcome = 0;
    uint32_t rbusError = 0;

    if (observation == NULL ||
        !BeginDecodingFrame(
            data, length, MessageType::Observation, &cursor, &remaining) ||
        !ReadU64(&cursor, &remaining, &observation->publisherInstanceId) ||
        !ReadU64(&cursor, &remaining, &observation->sequence) || remaining < 2) {
        return false;
    }

    operation = *cursor++;
    outcome = *cursor++;
    remaining -= 2;
    if (!IsValidOperation(operation) || !IsValidOutcome(outcome) ||
        !ReadU32(&cursor, &remaining, &rbusError) ||
        !ReadU64(&cursor, &remaining, &observation->durationMicroseconds) ||
        !ReadU64(&cursor, &remaining, &observation->completionEpochMicroseconds) ||
        !ReadIdentifier(&cursor, &remaining, &observation->path) ||
        !ReadIdentifier(&cursor, &remaining, &observation->provider) || remaining != 0) {
        return false;
    }

    observation->operation = static_cast<Operation>(operation);
    observation->outcome = static_cast<Outcome>(outcome);
    observation->rbusError = static_cast<int32_t>(rbusError);
    return true;
}

bool DecodeProviderLifecycle(
    const uint8_t* data,
    size_t length,
    ProviderLifecycle* lifecycle)
{
    const uint8_t* cursor = NULL;
    size_t remaining = 0;
    uint8_t registered = 0;

    if (lifecycle == NULL ||
        !BeginDecodingFrame(
            data, length, MessageType::ProviderLifecycle, &cursor, &remaining) ||
        !ReadU64(&cursor, &remaining, &lifecycle->publisherInstanceId) ||
        !ReadU64(&cursor, &remaining, &lifecycle->sequence) || remaining < 1) {
        return false;
    }

    registered = *cursor++;
    --remaining;
    if (registered > 1 ||
        !ReadIdentifier(&cursor, &remaining, &lifecycle->provider) ||
        lifecycle->provider.empty() || remaining != 0) {
        return false;
    }

    lifecycle->registered = registered != 0;
    return true;
}

bool DecodePublisherStatus(
    const uint8_t* data,
    size_t length,
    PublisherStatus* status)
{
    const uint8_t* cursor = NULL;
    size_t remaining = 0;

    return status != NULL &&
        BeginDecodingFrame(
            data, length, MessageType::PublisherStatus, &cursor, &remaining) &&
        ReadU64(&cursor, &remaining, &status->publisherInstanceId) &&
        ReadU64(&cursor, &remaining, &status->sequence) &&
        ReadU64(&cursor, &remaining, &status->cumulativeLocalDrops) &&
        remaining == 0;
}

bool DecodeControlRequest(const uint8_t* data, size_t length, ControlRequest* request)
{
    if (data == NULL || request == NULL || length > kMaximumControlBytes) {
        return false;
    }

    const uint8_t* cursor = data;
    size_t remaining = length;
    uint16_t version = 0;
    uint16_t type = 0;
    uint32_t declaredSize = 0;
    if (!ReadU16(&cursor, &remaining, &version) || !ReadU16(&cursor, &remaining, &type) ||
        !ReadU32(&cursor, &remaining, &declaredSize) ||
        !ReadU64(&cursor, &remaining, &request->requestId) ||
        version != kProtocolVersion || declaredSize != length) {
        return false;
    }

    request->type = static_cast<MessageType>(type);
    request->limit = 0;
    request->provider.clear();

    switch (request->type) {
    case MessageType::Snapshot:
    case MessageType::Reset:
        return remaining == 0;
    case MessageType::ProviderMetrics:
        return ReadIdentifier(&cursor, &remaining, &request->provider) &&
            !request->provider.empty() && remaining == 0;
    case MessageType::ProviderList:
    case MessageType::SlowRequestList:
        return ReadU16(&cursor, &remaining, &request->limit) &&
            request->limit > 0 && request->limit <= 128 && remaining == 0;
    default:
        return false;
    }
}

bool EncodeControlReply(const ControlReply& reply, std::vector<uint8_t>* encoded)
{
    if (reply.payload.size() > kMaximumControlBytes - 24 ||
        !IsValidUtf8(reply.payload) ||
        (reply.status != ControlReplyStatus::Success &&
         reply.status != ControlReplyStatus::ProviderNotFound) ||
        !BeginFrame(reply.type, encoded, kMaximumControlBytes)) {
        return false;
    }

    AppendU64(encoded, reply.requestId);
    AppendU16(encoded, static_cast<uint16_t>(reply.status));
    AppendU16(encoded, 0);
    AppendU32(encoded, static_cast<uint32_t>(reply.payload.size()));
    encoded->insert(encoded->end(), reply.payload.begin(), reply.payload.end());
    return FinishFrame(encoded, kMaximumControlBytes);
}

bool DecodeControlReply(const uint8_t* data, size_t length, ControlReply* reply)
{
    if (data == NULL || reply == NULL || length > kMaximumControlBytes) {
        return false;
    }

    const uint8_t* cursor = data;
    size_t remaining = length;
    uint16_t version = 0;
    uint16_t type = 0;
    uint16_t status = 0;
    uint16_t reserved = 0;
    uint32_t declaredSize = 0;
    uint32_t payloadLength = 0;
    if (!ReadU16(&cursor, &remaining, &version) ||
        !ReadU16(&cursor, &remaining, &type) ||
        !ReadU32(&cursor, &remaining, &declaredSize) ||
        !ReadU64(&cursor, &remaining, &reply->requestId) ||
        !ReadU16(&cursor, &remaining, &status) ||
        !ReadU16(&cursor, &remaining, &reserved) ||
        !ReadU32(&cursor, &remaining, &payloadLength) ||
        version != kProtocolVersion || declaredSize != length || reserved != 0 ||
        payloadLength != remaining ||
        (status != static_cast<uint16_t>(ControlReplyStatus::Success) &&
         status != static_cast<uint16_t>(ControlReplyStatus::ProviderNotFound))) {
        return false;
    }

    switch (static_cast<MessageType>(type)) {
    case MessageType::Snapshot:
    case MessageType::Reset:
    case MessageType::ProviderMetrics:
    case MessageType::ProviderList:
    case MessageType::SlowRequestList:
        break;
    default:
        return false;
    }

    reply->type = static_cast<MessageType>(type);
    reply->status = static_cast<ControlReplyStatus>(status);
    reply->payload.assign(reinterpret_cast<const char*>(cursor), remaining);
    return IsValidUtf8(reply->payload);
}

} // namespace rbusdiagnostics
