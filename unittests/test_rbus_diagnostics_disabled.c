#include <assert.h>
#include <stdio.h>

#include "../src/rbus/rbus_diagnostics.h"

/*
 * This target intentionally compiles without ENABLE_RBUS_DIAGNOSTICS. It
 * verifies the cheap disabled-state guard before an RBus operation is timed.
 */
int main(void)
{
    rbusDiagnosticsTimer_t timer = rbusDiagnostics_TimerStart();

    assert(timer.enabled == 0);
    assert(rbusDiagnostics_TimerElapsedMicroseconds(&timer) == 0U);

    puts("rbus diagnostics disabled tests passed");
    return 0;
}
