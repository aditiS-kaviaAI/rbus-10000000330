#include "DiagnosticsStore.h"
#include "ReporterServer.h"

#include <grp.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

uint64_t EpochMicroseconds()
{
    struct timeval now = {};
    gettimeofday(&now, NULL);
    return static_cast<uint64_t>(now.tv_sec) * 1000000ULL + now.tv_usec;
}

uint64_t InstanceId()
{
    return (EpochMicroseconds() << 16) ^ static_cast<uint64_t>(getpid());
}

} // namespace

int main()
{
    const struct group* diagnosticsGroup = getgrnam("rbusdiag");
    if (diagnosticsGroup == NULL) {
        fprintf(stderr, "rbus-diagnostics-reporter: missing rbusdiag group\n");
        return 1;
    }

    rbusdiagnostics::DiagnosticsStore store(InstanceId(), EpochMicroseconds());
    rbusdiagnostics::ReporterServer server(
        &store,
        static_cast<uint32_t>(diagnosticsGroup->gr_gid),
        static_cast<uint32_t>(geteuid()));

    if (!server.Start()) {
        fprintf(stderr, "rbus-diagnostics-reporter: endpoint setup failed\n");
        return 1;
    }
    server.Run();
    return 0;
}
