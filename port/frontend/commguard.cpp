/* See commguard.h. */

#include "commguard.h"

#include "viewmask.h"

#include "state.h"
#include "runtime_arm.h"

#include <cstdio>

extern "C" void lsw2_arm9_FUN_0206ef0c(void);

namespace {

int g_refusals = 0;

/* SAY IT A FEW TIMES, NOT EVERY FRAME. The check runs on the netgame path, so
 * an unconditional line would bury the log the moment a session ends - and the
 * interesting thing is that it fired at all, not how often. */
constexpr int kSaidLimit = 4;
int g_said = 0;

}  // namespace

void lsw2_commguard_consistency_check() {
    /* THE ONE TEST, and it is the netgame tick's own first act rather than a
     * new rule: with no view manager there is no session to be inconsistent
     * with. See commguard.h for the disassembly of the three unguarded
     * dereferences this stands in front of. */
    const uint32_t manager = bus_device_read32(0, kLsw2ViewManagerPtr);
    if (manager == 0) {
        ++g_refusals;
        if (g_said < kSaidLimit) {
            ++g_said;
            std::fprintf(stderr,
                         "[fork] comm guard: the cross-player consistency "
                         "check was called with the view manager at %08X torn "
                         "down (null). Not running it - it would read address "
                         "zero as an object and branch through the result, "
                         "which is the crash after Shut Down Lobby. Refusal "
                         "%d\n",
                         unsigned(kLsw2ViewManagerPtr), g_refusals);
        }
        return;
    }

    /* WHAT THE SAFETY SWEEP SAID, asked before this was written. It reports
     *
     *     FUN_0206ef0c -> FUN_02072cd0 -> FUN_02072bc8
     *
     * and FUN_02072bc8 is the cartridge's session teardown, which polls the WH
     * object's vtable+0x50 in a do-while - the known spin-on-return that CANNOT
     * be host-called.
     *
     * That is a reason this must stay an override and never become a guest
     * call. An override body runs on the GUEST'S OWN THREAD inside a dispatch,
     * where the event queue is live and such a poll completes normally - the
     * same property the gear hop and the session end were built to obtain.
     * Calling it from the host between frames would hang on an interrupt that
     * can never be raised. The guard also makes the null case skip that chain
     * entirely, which is strictly safer than the cartridge's own behaviour.
     *
     * THE GAME'S OWN CHECK, unchanged and complete, whenever there IS a
     * session - so a genuine desync still raises a genuine Communication
     * Error. R[15] names the entry because the lifted function uses it to pick
     * a landing pad; R[14] is left alone so it returns to its caller. */
    g_cpu.R[15] = kLsw2NetConsistencyCheck;
    lsw2_arm9_FUN_0206ef0c();
}
