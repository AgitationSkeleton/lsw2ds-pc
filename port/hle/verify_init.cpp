/* C++ shim: bring the runtime up the way the runner does.
 *
 * The harness itself is C, because the generated banks are C and calling them
 * from C++ mangles the names. But the runner's initialisation entry points -
 * nds_io_reset(), nds_register_dispatch() - are declared with C++ linkage in
 * runner/src/*.h, so they are unreachable from a C translation unit. This file
 * is the one-function bridge.
 *
 * WHY INITIALISE AT ALL. A lifted function's first line is
 *
 *     if (runtime_should_yield()) return;
 *
 * and that consults nds_cpu_halted(), nds_dma_cpu_stalled() and
 * active_static_code_changed(). Outside a booted runner those read
 * uninitialised globals, so the function silently returns having executed
 * nothing - and a comparison against "nothing" looks exactly like a real
 * divergence. The first version of this harness skipped initialisation and
 * produced results that changed when two independent test cases were reordered.
 */

#include "io.h"
#include "state.h"
#include "runtime_arm.h"
#include "scheduler.h"
#include <cstdio>

extern "C" {

extern const NdsDispatchEntry g_dispatch_lsw2_arm9[];
extern const unsigned g_dispatch_lsw2_arm9_len;

void hle_verify_runtime_init(void) {
    nds_io_reset();
    /* The scheduler owns the event queue that runtime_should_yield() consults
     * via nds_event_break_hit(). Without scheduler_init() its events read as
     * perpetually due, so every guest instruction yields before executing -
     * which is exactly what the harness was seeing (halted=0 dma_stalled=0
     * insn_stop=0 terminal=0, and still unwinding). */
    scheduler_init();
    runtime_init(nullptr);
    runtime_trace_reset();
    /* Only the ARM9 title bank is needed: every routine under verification
     * lives in it, and registering a bank we are not calling into would add
     * state without adding coverage. */
    nds_register_dispatch(NDS_ARM9, g_dispatch_lsw2_arm9,
                          g_dispatch_lsw2_arm9_len, 0x02000000u);
}

}  /* extern "C" */

/* Which of runtime_should_yield()'s conditions is firing? Guessing cost two
 * iterations already; this reports it. nds_cpu_halted and nds_dma_cpu_stalled
 * are already declared by io.h with C++ linkage - redeclaring them here as
 * extern "C" is a conflict, so only the shim itself is exported. */
extern "C" void hle_verify_report_yield_state(void) {
    std::printf("  yield state: halted=%d dma_stalled=%d insn_stop=%u "
                "terminal=%d\n",
                (int)nds_cpu_halted(NDS_ARM9), (int)nds_dma_cpu_stalled(NDS_ARM9),
                0u, 0);
}
