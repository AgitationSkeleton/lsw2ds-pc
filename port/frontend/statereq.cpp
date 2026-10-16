/* See statereq.h. Name the state and the code that asked for it. */

#include "statereq.h"

#include "freeplaypick.h"   /* the ALL rule's pick holds the round start */

#include "state.h"
#include "statelog.h"

#include <cstdio>


extern "C" void lsw2_arm9_FUN_0205ecc4(void);

namespace {

/* ONE LINE PER DISTINCT (state, caller), because the interesting thing is the
 * SET of pairs, not the traffic. The game changes screens constantly; printing
 * every request would bury the handful that matter under the level
 * transitions, and this project has already made the string decoder a
 * frame-rate problem once by logging something that runs constantly. */
constexpr int kMaxPairs = 64;

struct Pair {
    uint32_t state;
    uint32_t caller;
};

Pair g_seen[kMaxPairs];
int g_count = 0;

bool remember(uint32_t state, uint32_t caller) {
    for (int i = 0; i < g_count; ++i) {
        if (g_seen[i].state == state && g_seen[i].caller == caller) return false;
    }
    if (g_count < kMaxPairs) {
        g_seen[g_count].state = state;
        g_seen[g_count].caller = caller;
        ++g_count;
        return true;
    }
    /* FULL, AND SAY SO ONCE. A cap that silently stops reporting reads exactly
     * like a game that stopped changing screens. A flag, never g_count past
     * the cap: see markerhook.cpp's remember() for the crash that idiom
     * caused. */
    static bool said_full = false;
    if (!said_full) {
        said_full = true;
        std::fprintf(stderr, "[state] request log full at %d pairs - later "
                             "distinct requests are NOT shown\n", kMaxPairs);
    }
    return false;
}

/* NAME THE MODE, because an unnamed one cost this project nine documents.
 *
 * `0x0205ECC4`'s **r3** becomes `[state+0x8]`, which `FUN_0208c888` branches on
 * seven times a frame and which decides whether entering a level SPAWNS the
 * players. Every arm is in statelog.h. This log printed the state id and the
 * caller and never this number, so "the arena is entered in a mode that only
 * places players" was invisible in a log that ran on every single run.
 *
 * A switch rather than a lookup so that adding a mode to the enum makes the
 * compiler point here. */
const char* mode_name(uint32_t mode) {
    switch (static_cast<Lsw2LevelMode>(mode)) {
        case Lsw2LevelMode::Spawn:      return "Spawn";
        case Lsw2LevelMode::Ordinary:   return "Ordinary";
        case Lsw2LevelMode::CoopEnter:  return "CoopEnter";
        case Lsw2LevelMode::Arena:      return "Arena";
        case Lsw2LevelMode::Walkaround: return "Walkaround";
        case Lsw2LevelMode::SpawnAlt:   return "SpawnAlt";
    }
    return "?";
}

}  // namespace

bool lsw2_state_request_in_slot(uint32_t slot, uint32_t state, uint32_t mode,
                                uint32_t level, uint32_t arg) {
    if (slot > kLsw2ScreenSlot) return false;
    const uint32_t base = kLsw2StateManager + slot * kLsw2SlotStride + kLsw2SlotArgs;
    const uint32_t words[4] = {state, mode, level, arg};
    for (uint32_t i = 0; i < kLsw2StateRequestWords; ++i)
        bus_device_write32(9, base + i * 4u, i < 4u ? words[i] : 0u);
    std::fprintf(stderr, "[state] asked slot %u for state %u (mode 0x%02X, level %u) "
                 "the way 0x0205EC4C asks - the manager tears the old one down\n",
                 unsigned(slot), unsigned(state), unsigned(mode), unsigned(level));
    return true;
}

void lsw2_statereq_override() {
    const uint32_t state = g_cpu.R[1];
    const uint32_t caller = g_cpu.R[14];
    /* r3 is the mode and [sp+0] is the level - the two arguments the factory
     * stores at +0x08 and +0x0C. Read at entry, before the prologue moves r13. */
    const uint32_t mode = g_cpu.R[3];
    const uint32_t level = bus_device_read32(9, g_cpu.R[13]);

    /* Fold the mode into the key so the SAME caller asking for the same state
     * in a DIFFERENT mode still reports - which is the whole point. */
    if (remember(state, caller ^ (mode << 24))) {
        if (state == kLsw2LevelStateKind) {
            std::fprintf(stderr,
                         "[state] request %u from %08X  level %u  mode 0x%02X %s\n",
                         unsigned(state), unsigned(caller), unsigned(level),
                         unsigned(mode), mode_name(mode));
        } else {
            std::fprintf(stderr, "[state] request %u from %08X\n",
                         unsigned(state), unsigned(caller));
        }
    }

    /* The lobby's exit door is NOT refused here. An earlier version skipped this
     * requester for a level-kind request to a non-lobby, non-arena level while
     * in the lobby - and it was wrong twice: it did not stop the warp (the door
     * still reached level 14, so the door does not post through this path in the
     * frame we can gate on), and it broke things when it DID fire - a joining
     * peer legitimately loads level 14 during the session bring-up ("left the
     * lobby (level 14)" in the two-console logs), and refusing the posted request
     * mid-transition left the fade in flight with nothing to complete it, which
     * showed in play as a freeze/crash. The door stays handled reactively by
     * lsw2_cheats_pin_level + the warp-back backstop until it is disabled at the
     * ENTITY/trigger rather than on the shared level-request path. */

    /* THE ALL RULE'S FREE PLAY PICK (freeplaypick.h): at the round start the
     * Walkaround world request is rewritten to CoopEnter and the select's 41
     * to 48 - the lobby return's own pair - so the pick happens in a reloaded
     * lobby, where the grid lives. Fork, session and ALL only; every other
     * request, and every other round, passes through untouched. */
    lsw2_freeplaypick_round_start_request();

    /* THE GAME'S OWN CODE, unchanged. R[15] names the entry because the lifted
     * function uses it to pick a landing pad; R[14] is left alone so it returns
     * to the caller we just named. */
    g_cpu.R[15] = kLsw2StateRequester;
    lsw2_arm9_FUN_0205ecc4();
}
