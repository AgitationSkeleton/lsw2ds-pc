/* The multiplayer match's determinism problem.
 *
 * THE PROBLEM. A lockstep session is only deterministic if every machine draws
 * the same random numbers in the same order. The game's generator is MT19937
 * (`0x02062D10`, tempering masks `0x9D2C5680` and `0xEFC60000`) seeded ONCE at
 * boot from the real-time clock - so four machines are not at risk of holding
 * different states, they are guaranteed to. No amount of correct block delivery
 * fixes it: it is a property of the simulation, not of the transport.
 *
 * THE FIX IS TWO WORDS, and it is the game's own path rather than the port's.
 *
 * The refill at `0x02062BB4` reads the state block and, when the remaining count
 * `+0x08` is BELOW -1, takes the seed from `+0x1C` and re-initialises from it.
 * The exact condition is the `BGE` at `0x02062BD8` and it matters - see
 * kRandForceReseed below, where writing -1 instead of -2 would have made this
 * whole file a no-op that logged success.
 *
 * So the port writes the shared seed to `+0x1C`, writes -2 to `+0x08`, and the
 * next draw re-seeds through the cartridge's own expansion. Nothing here
 * reimplements MT, and nothing depends on matching what the game's own seeder
 * would have produced - what matters is that all four machines land on the SAME
 * state, not on any particular one.
 *
 * Same shape as the pending-character request and the level warp:
 * fill in the game's own data and let the game's own code act on it.
 *
 * WHEN. On entering an arena match while in a session. That is a moment every
 * machine reaches and can agree on, and it is late enough that the menus,
 * effects and boot draws that happened beforehand - which differ per machine and
 * do not matter - are behind it.
 */

#include "rngsync.h"
#include "vwh.h"

#include "cheats.h"
#include "forksession.h"
#include "ui.h"

#include "state.h"

#include <cstdio>

namespace {

/* See rngsync.h. */
constexpr uint32_t kRandRemaining = 0x08u;
constexpr uint32_t kRandSeed = 0x1Cu;

/* WHAT TO WRITE INTO THE REMAINING COUNT, AND WHY IT IS NOT -1.
 *
 * The refill's branch is a SIGNED comparison against -1, and it skips the seed
 * on greater-or-equal:
 *
 *     02062BBC  mvn  r1,#0            r1 = -1
 *     02062BC4  ldr  r3,[r2,#8]       the remaining count
 *     02062BCC  cmp  r3,r1
 *     02062BD8  bge  0x02062BE4       remaining >= -1 -> ordinary refill
 *     02062BDC  ldr  r0,[r2,#0x1C]    remaining <  -1 -> RE-SEED from +0x1C
 *
 * So -1 means "exhausted, regenerate from the state I already have" - which is
 * what the count reaches on its own by counting down, and what the game's own
 * seeder leaves behind after doing its expansion by hand. Only a value BELOW -1
 * asks for a re-seed.
 *
 * The first version of this file wrote -1. It would have regenerated from each
 * machine's existing state, left them exactly as divergent as before, and
 * printed a line saying the seed had been applied - a silent no-op wearing a
 * success message, which is the worst shape a bug can take. One `BGE` settled
 * it, and it was found by re-reading rather than by anything failing. */
constexpr uint32_t kRandForceReseed = 0xFFFFFFFEu;   /* -2 */

/* Was the last frame in a match? Entering one is the trigger; being in one is
 * not, or the seed would be re-applied every frame and the generator would
 * never advance. */
bool g_was_in_match = false;

/* What we last applied, so a repeat of the same seed in the same match is not
 * mistaken for a new round. */
uint32_t g_applied = 0;
bool g_have_applied = false;

}  // namespace

void lsw2_rng_apply_seed(uint32_t seed) {
    if (!lsw2_cheats_available()) return;

    /* ORDER MATTERS. The seed has to be in place before the count says "refill
     * me", or a draw landing between the two writes would expand whatever was
     * in +0x1C from the previous session. */
    bus_device_write32(0, kLsw2RandState + kRandSeed, seed);
    bus_device_write32(0, kLsw2RandState + kRandRemaining, kRandForceReseed);

    g_applied = seed;
    g_have_applied = true;
    std::fprintf(stderr, "[fork] rng reseeded to %08X for this match\n",
                 unsigned(seed));
}

void lsw2_rng_sync_tick() {
    /* LOCKSTEP (vwh.h lsw2_vwh_lockstep): the cartridge seeds the shared stream itself (message 0x0A, FUN_02098b6c); a reseed from here races it. */
    if (lsw2_vwh_lockstep()) return;
    if (!ui_fork_build()) return;

    const bool in_match = lsw2_cheats_in_arena_match();
    const bool entering = in_match && !g_was_in_match;
    g_was_in_match = in_match;

    if (!in_match) {
        /* Leaving a match forgets what was applied, so the next one reseeds
         * even if the host happens to send the same number again. */
        g_have_applied = false;
        return;
    }
    if (!entering) return;

    Lsw2ForkSession& session = lsw2_fork_session();
    if (session.state() == Lsw2SessionState::Offline) return;

    const uint32_t seed = session.match_seed();
    if (!seed) return;                     /* the host has not sent one */
    if (g_have_applied && seed == g_applied) return;

    lsw2_rng_apply_seed(seed);
}
