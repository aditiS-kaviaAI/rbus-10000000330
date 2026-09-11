#ifndef RBUS_DIAGNOSTICS_PROTOCOL_H
#define RBUS_DIAGNOSTICS_PROTOCOL_H

#include <stdint.h>

#include <string>
#include <vector>

namespace rbusdiagnostics {

static const uint16_t kProtocolVersion = 1;
static const size_t kMaximumObservationBytes = 512;
static const size_t kMaximumControlBytes = 16384;
static const size_t kMaximumIdentifierBytes = 256;

enum class MessageType : uint16_t {
    Observation = 1,
    Snapshot = 2,
    Reset = 3,
    ProviderLifecycle = 4,
    PublisherStatus = 5,
    ProviderMetrics = 6,
    ProviderList = 7,
    SlowRequestList = 8
};

enum class Operation : uint8_t {
    Get = 0,
    Set = 1,
    Method = 2,
    Other = 3,
    Count = 4
};

enum class Outcome : uint8_t {
    Success = 0,
    Timeout = 1,
    PermissionDenied = 2,
    InvalidRequest = 3,
    InvalidResponse = 4,
    ProviderUnavailable = 5,
    UnknownError = 6,
    Count = 7
};

struct Observation {
    uint64_t publisherInstanceId;
    uint64_t sequence;
    Operation operation;
    int32_t rbusError;
    Outcome outcome;
    uint64_t durationMicroseconds;
    uint64_t completionEpochMicroseconds;
    std::string path;
    std::string provider;
};

struct ProviderLifecycle {
    uint64_t publisherInstanceId;
    uint64_t sequence;
    bool registered;
    std::string provider;
};

struct PublisherStatus {
    uint64_t publisherInstanceId;
    uint64_t sequence;
    uint64_t cumulativeLocalDrops;
};

struct ControlRequest {
    MessageType type;
    uint64_t requestId;
    uint16_t limit;
    std::string provider;
};

enum class ControlReplyStatus : uint16_t {
    Success = 0,
    ProviderNotFound = 1
};

struct ControlReply {
    MessageType type;
    uint64_t requestId;
    ControlReplyStatus status;
    std::string payload;
};

/**
 * Encodes a bounded observation using versioned network-byte-order fields.
 * The encoded format contains no native C or C++ structure representation.
 */
bool EncodeObservation(const Observation& observation, std::vector<uint8_t>* encoded);

/**
 * Encodes a bounded provider registration or verified-removal lifecycle frame.
 */
bool EncodeProviderLifecycle(
    const ProviderLifecycle& lifecycle,
    std::vector<uint8_t>* encoded);

/**
 * Encodes a bounded publisher sequence and cumulative-drop status frame.
 */
bool EncodePublisherStatus(
    const PublisherStatus& status,
    std::vector<uint8_t>* encoded);

/**
 * Validates and decodes a complete SOCK_SEQPACKET observation record.
 */
bool DecodeObservation(const uint8_t* data, size_t length, Observation* observation);

/**
 * Validates and decodes a complete provider lifecycle record.
 */
bool DecodeProviderLifecycle(
    const uint8_t* data,
    size_t length,
    ProviderLifecycle* lifecycle);

/**
 * Validates and decodes a complete publisher-status record.
 */
bool DecodePublisherStatus(
    const uint8_t* data,
    size_t length,
    PublisherStatus* status);

/**
 * Validates and decodes a complete bounded control request record.
 */
bool DecodeControlRequest(const uint8_t* data, size_t length, ControlRequest* request);

/**
 * Encodes a bounded versioned control reply. Payloads are UTF-8 JSON objects
 * built only from the reporter's already bounded snapshot collections.
 */
bool EncodeControlReply(const ControlReply& reply, std::vector<uint8_t>* encoded);

/**
 * Validates and decodes a bounded versioned control reply.
 */
bool DecodeControlReply(const uint8_t* data, size_t length, ControlReply* reply);

} // namespace rbusdiagnostics

#endif
