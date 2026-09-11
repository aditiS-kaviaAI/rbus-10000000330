#ifndef RBUS_DIAGNOSTICS_REPORTER_SERVER_H
#define RBUS_DIAGNOSTICS_REPORTER_SERVER_H

#include "DiagnosticsStore.h"

#include <stdint.h>

namespace rbusdiagnostics {

static const char kDiagnosticsRuntimeDirectory[] = "/run/rbus-diagnostics";
static const char kObservationSocketPath[] = "/run/rbus-diagnostics/observations.sock";
static const char kControlSocketPath[] = "/run/rbus-diagnostics/control.sock";

/**
 * Hosts the private diagnostics endpoints and validates peer credentials before
 * decoding frames or accessing the global diagnostics store.
 */
class ReporterServer {
public:
    ReporterServer(
        DiagnosticsStore* store,
        uint32_t publisherGroupId,
        uint32_t controlUserId,
        const char* runtimeDirectory = kDiagnosticsRuntimeDirectory);
    ~ReporterServer();

    bool Start();
    void Run();
    void Stop();

private:
    bool CreateEndpoint(const char* path, int* descriptor);
    bool IsAuthorizedPeer(int descriptor, bool controlEndpoint) const;
    void HandleObservation();
    void HandleControl();
    void SendControlReply(int descriptor, const ControlReply& reply);
    ControlReply BuildControlReply(const ControlRequest& request);

    DiagnosticsStore* const store_;
    const uint32_t publisherGroupId_;
    const uint32_t controlUserId_;
    const char* const runtimeDirectory_;
    std::string observationSocketPath_;
    std::string controlSocketPath_;
    int observationDescriptor_;
    int controlDescriptor_;
    bool running_;
};

} // namespace rbusdiagnostics

#endif
