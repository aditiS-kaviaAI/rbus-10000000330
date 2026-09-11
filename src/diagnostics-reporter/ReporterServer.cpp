#include "ReporterServer.h"

#include "DiagnosticsProtocol.h"

#include <algorithm>
#include <errno.h>
#include <grp.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace rbusdiagnostics {
namespace {

std::string EscapeJson(const std::string& value)
{
    std::ostringstream escaped;
    for (size_t index = 0; index < value.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(value[index]);
        switch (character) {
        case '"':
            escaped << "\\\"";
            break;
        case '\\':
            escaped << "\\\\";
            break;
        case '\b':
            escaped << "\\b";
            break;
        case '\f':
            escaped << "\\f";
            break;
        case '\n':
            escaped << "\\n";
            break;
        case '\r':
            escaped << "\\r";
            break;
        case '\t':
            escaped << "\\t";
            break;
        default:
            if (character < 0x20) {
                static const char hexadecimal[] = "0123456789abcdef";
                escaped << "\\u00" << hexadecimal[character >> 4] << hexadecimal[character & 0x0F];
            } else {
                escaped << value[index];
            }
            break;
        }
    }
    return escaped.str();
}

void AppendMetrics(std::ostringstream* payload, const OperationMetrics& metrics)
{
    *payload << "{\"requestCount\":" << metrics.requestCount
             << ",\"successCount\":" << metrics.successCount
             << ",\"errorCount\":" << metrics.errorCount
             << ",\"timeoutCount\":" << metrics.timeoutCount
             << ",\"durationSumMicroseconds\":" << metrics.durationSumMicroseconds
             << ",\"minimumDurationMicroseconds\":";
    if (metrics.requestCount == 0) {
        *payload << "null";
    } else {
        *payload << metrics.minimumDurationMicroseconds;
    }
    *payload << ",\"maximumDurationMicroseconds\":" << metrics.maximumDurationMicroseconds
             << "}";
}

void AppendProvider(std::ostringstream* payload, const ProviderRecord& provider)
{
    *payload << "{\"provider\":\"" << EscapeJson(provider.provider)
             << "\",\"state\":" << static_cast<unsigned int>(provider.state)
             << ",\"metrics\":";
    AppendMetrics(payload, provider.metrics);
    *payload << "}";
}

} // namespace

ReporterServer::ReporterServer(
    DiagnosticsStore* store,
    uint32_t publisherGroupId,
    uint32_t controlUserId,
    const char* runtimeDirectory)
    : store_(store),
      publisherGroupId_(publisherGroupId),
      controlUserId_(controlUserId),
      runtimeDirectory_(runtimeDirectory),
      observationSocketPath_(std::string(runtimeDirectory) + "/observations.sock"),
      controlSocketPath_(std::string(runtimeDirectory) + "/control.sock"),
      observationDescriptor_(-1),
      controlDescriptor_(-1),
      running_(false)
{
}

ReporterServer::~ReporterServer()
{
    Stop();
}

bool ReporterServer::CreateEndpoint(const char* path, int* descriptor)
{
    *descriptor = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (*descriptor < 0) {
        return false;
    }

    unlink(path);
    struct sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(address.sun_path)) {
        close(*descriptor);
        *descriptor = -1;
        return false;
    }
    strcpy(address.sun_path, path);
    if (bind(*descriptor, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0 ||
        chmod(path, 0660) != 0 || listen(*descriptor, 16) != 0) {
        close(*descriptor);
        *descriptor = -1;
        unlink(path);
        return false;
    }
    return true;
}

bool ReporterServer::Start()
{
    if (mkdir(runtimeDirectory_, 0750) != 0 && errno != EEXIST) {
        return false;
    }
    if (chmod(runtimeDirectory_, 0750) != 0 ||
        !CreateEndpoint(observationSocketPath_.c_str(), &observationDescriptor_) ||
        !CreateEndpoint(controlSocketPath_.c_str(), &controlDescriptor_)) {
        Stop();
        return false;
    }
    running_ = true;
    return true;
}

void ReporterServer::Stop()
{
    running_ = false;
    if (observationDescriptor_ >= 0) {
        close(observationDescriptor_);
        observationDescriptor_ = -1;
    }
    if (controlDescriptor_ >= 0) {
        close(controlDescriptor_);
        controlDescriptor_ = -1;
    }
    unlink(observationSocketPath_.c_str());
    unlink(controlSocketPath_.c_str());
}

bool ReporterServer::IsAuthorizedPeer(int descriptor, bool controlEndpoint) const
{
    struct ucred credentials = {};
    socklen_t credentialsLength = sizeof(credentials);
    if (getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &credentialsLength) != 0) {
        return false;
    }
    return credentials.gid == static_cast<gid_t>(publisherGroupId_) &&
        (!controlEndpoint || credentials.uid == static_cast<uid_t>(controlUserId_));
}

void ReporterServer::HandleObservation()
{
    const int client = accept(observationDescriptor_, NULL, NULL);
    if (client < 0) {
        return;
    }

    uint8_t frame[kMaximumObservationBytes];
    const ssize_t received = recv(client, frame, sizeof(frame), 0);
    if (!IsAuthorizedPeer(client, false) || received <= 0) {
        store_->RecordRejectedFrame();
    } else {
        Observation observation = {};
        ProviderLifecycle lifecycle = {};
        PublisherStatus status = {};
        if (DecodeObservation(frame, static_cast<size_t>(received), &observation)) {
            store_->RecordObservation(observation);
        } else if (DecodeProviderLifecycle(frame, static_cast<size_t>(received), &lifecycle)) {
            store_->RecordProviderLifecycle(lifecycle.provider, lifecycle.registered);
        } else if (DecodePublisherStatus(frame, static_cast<size_t>(received), &status)) {
            store_->RecordPublisherStatus(
                status.publisherInstanceId,
                status.sequence,
                status.cumulativeLocalDrops);
        } else {
            store_->RecordRejectedFrame();
        }
    }
    close(client);
}

void ReporterServer::SendControlReply(int descriptor, const ControlReply& reply)
{
    std::vector<uint8_t> encoded;
    if (EncodeControlReply(reply, &encoded)) {
        (void)send(descriptor, encoded.data(), encoded.size(), MSG_NOSIGNAL);
    }
}

ControlReply ReporterServer::BuildControlReply(const ControlRequest& request)
{
    ControlReply reply = {};
    reply.type = request.type;
    reply.requestId = request.requestId;
    reply.status = ControlReplyStatus::Success;

    const DiagnosticsSnapshot snapshot = store_->Snapshot();
    std::ostringstream payload;
    payload << "{\"reporterInstanceId\":" << snapshot.reporterInstanceId
            << ",\"startupEpochMicroseconds\":" << snapshot.startupEpochMicroseconds
            << ",\"measurementGeneration\":" << snapshot.measurementGeneration;

    if (request.type == MessageType::Snapshot) {
        payload << ",\"quality\":{\"rejectedFrames\":" << snapshot.rejectedFrames
                << ",\"providerCapacityOverflows\":" << snapshot.providerCapacityOverflows
                << ",\"pathCapacityOverflows\":" << snapshot.pathCapacityOverflows
                << ",\"publisherCapacityOverflows\":" << snapshot.publisherCapacityOverflows
                << ",\"slowRequestOverwrites\":" << snapshot.slowRequestOverwrites
                << "},\"operations\":[";
        for (size_t index = 0; index < snapshot.operations.size(); ++index) {
            if (index != 0) {
                payload << ",";
            }
            AppendMetrics(&payload, snapshot.operations[index]);
        }
        payload << "]";
    } else if (request.type == MessageType::ProviderMetrics ||
        request.type == MessageType::ProviderList) {
        payload << ",\"providers\":[";
        const size_t maximum = request.type == MessageType::ProviderMetrics ? 1 : request.limit;
        size_t emitted = 0;
        for (size_t index = 0; index < snapshot.providers.size() && emitted < maximum; ++index) {
            const ProviderRecord& provider = snapshot.providers[index];
            if (!provider.inUse ||
                (request.type == MessageType::ProviderMetrics &&
                 provider.provider != request.provider)) {
                continue;
            }
            if (emitted++ != 0) {
                payload << ",";
            }
            AppendProvider(&payload, provider);
        }
        payload << "]";
        if (request.type == MessageType::ProviderMetrics && emitted == 0) {
            reply.status = ControlReplyStatus::ProviderNotFound;
        }
    } else if (request.type == MessageType::SlowRequestList) {
        payload << ",\"slowRequests\":[";
        const size_t count = std::min(
            static_cast<size_t>(request.limit),
            snapshot.slowRequestCount);
        for (size_t index = 0; index < count; ++index) {
            const Observation& observation = snapshot.slowRequests[index].observation;
            if (index != 0) {
                payload << ",";
            }
            payload << "{\"operation\":" << static_cast<unsigned int>(observation.operation)
                    << ",\"outcome\":" << static_cast<unsigned int>(observation.outcome)
                    << ",\"rbusError\":" << observation.rbusError
                    << ",\"durationMicroseconds\":" << observation.durationMicroseconds
                    << ",\"completionEpochMicroseconds\":" << observation.completionEpochMicroseconds
                    << ",\"path\":\"" << EscapeJson(observation.path)
                    << "\",\"provider\":\"" << EscapeJson(observation.provider)
                    << "\"}";
        }
        payload << "]";
    }

    payload << "}";
    reply.payload = payload.str();
    return reply;
}

void ReporterServer::HandleControl()
{
    const int client = accept(controlDescriptor_, NULL, NULL);
    if (client < 0) {
        return;
    }

    uint8_t frame[kMaximumControlBytes];
    const ssize_t received = recv(client, frame, sizeof(frame), 0);
    ControlRequest request = {};
    if (!IsAuthorizedPeer(client, true) || received <= 0 ||
        !DecodeControlRequest(frame, static_cast<size_t>(received), &request)) {
        store_->RecordRejectedFrame();
    } else if (request.type == MessageType::Reset) {
        ControlReply reply = {};
        reply.type = request.type;
        reply.requestId = request.requestId;
        reply.status = ControlReplyStatus::Success;
        std::ostringstream payload;
        payload << "{\"measurementGeneration\":" << store_->Reset() << "}";
        reply.payload = payload.str();
        SendControlReply(client, reply);
    } else {
        SendControlReply(client, BuildControlReply(request));
    }
    close(client);
}

void ReporterServer::Run()
{
    while (running_) {
        struct pollfd descriptors[2] = {
            {observationDescriptor_, POLLIN, 0},
            {controlDescriptor_, POLLIN, 0}
        };
        const int result = poll(descriptors, 2, 1000);
        if (result <= 0) {
            continue;
        }
        if ((descriptors[0].revents & POLLIN) != 0) {
            HandleObservation();
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            HandleControl();
        }
    }
}

} // namespace rbusdiagnostics
