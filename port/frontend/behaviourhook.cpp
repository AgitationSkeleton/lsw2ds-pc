/* See behaviourhook.h. Name the caller of each behaviour hook, and say how
 * often each one runs. */

#include "behaviourhook.h"

#include "state.h"
#include "cheats.h"
#include "statelog.h"

#include <cstdio>

extern "C" void lsw2_arm9_FUN_0204b450(void);
extern "C" void lsw2_arm9_FUN_0204a938(void);
extern "C" void lsw2_arm9_FUN_02057bc8(void);
extern "C" void lsw2_arm9_FUN_0208c888(void);
extern "C" void lsw2_arm9_FUN_02057a28(void);
extern "C" void lsw2_arm9_FUN_0207641c(void);

namespace {

/* ONE LINE PER DISTINCT (hook, caller, key), not per call.
 *
 * `FUN_0204b450` writes a status byte into its wrapper before the my-slot
 * check, which is the shape of a per-frame predicate rather than an event - so
 * it may well run every frame for every actor. Printing each call would bury
 * the answer and would repeat this project's own mistake of making a log a
 * frame-rate problem. The interesting thing is the SET of call sites. */
constexpr int kMaxPairs = 48;

struct Pair {
    uint32_t caller;
    int key;
    int hook;
};

Pair g_seen[kMaxPairs];
int g_count = 0;

/* disable, enable, setbyte */
long g_calls[3] = {0, 0, 0};

bool remember(int hook, uint32_t caller, int key) {
    for (int i = 0; i < g_count; ++i) {
        if (g_seen[i].hook == hook && g_seen[i].caller == caller &&
            g_seen[i].key == key) {
            return false;
        }
    }
    if (g_count < kMaxPairs) {
        g_seen[g_count].hook = hook;
        g_seen[g_count].caller = caller;
        g_seen[g_count].key = key;
        ++g_count;
        return true;
    }
    /* FULL, AND SAY SO ONCE - a cap that silently stops reporting reads exactly
     * like a hook that stopped being called, which is the answer this file
     * exists to distinguish. A flag, never g_count past the cap: see
     * markerhook.cpp's remember() for the crash that idiom caused. */
    static bool said_full = false;
    if (!said_full) {
        said_full = true;
        std::fprintf(stderr, "[behaviour] call-site log full at %d pairs - "
                             "later distinct callers are NOT shown\n", kMaxPairs);
    }
    return false;
}

bool in_ram(uint32_t a) { return a >= 0x02000000u && a < 0x02400000u; }

uint8_t read_byte(uint32_t addr) {
    const uint32_t w = bus_device_read32(0, addr & ~3u);
    return uint8_t((w >> ((addr & 3u) * 8u)) & 0xFFu);
}

/* The ACTOR, not the wrapper. r0 here is a wrapper and the actor is
 * [r0 + 4]; reading +0x5DA off the wrapper would be a value from the wrong
 * object, a mistake that has cost this project a stride before. */
void report(int hook, const char* name) {
    const uint32_t wrapper = g_cpu.R[0];
    const uint32_t caller = g_cpu.R[14];
    const int state = lsw2_state_current();

    ++g_calls[hook];

    if (!remember(hook, caller, state)) return;

    uint32_t actor = 0;
    unsigned slot = 0xFFu;
    if (in_ram(wrapper)) {
        actor = bus_device_read32(0, wrapper + 4u);
        /* +0x5DA is a BYTE - caveat (g). */
        if (in_ram(actor)) slot = read_byte(actor + 0x5DAu);
    }

    std::fprintf(stderr,
                 "[behaviour] %s from %08X   state=%d actor=%08X slot=%u "
                 "(calls: disable=%ld enable=%ld setbyte=%ld)\n",
                 name, unsigned(caller), state, unsigned(actor), slot,
                 g_calls[0], g_calls[1], g_calls[2]);
}

}  // namespace

void lsw2_behaviourhook_disable_override() {
    report(0, "DISABLE FUN_0204b450 (broadcasts 9)");
    /* THE GAME'S OWN CODE, UNCHANGED. R[15] names the entry because the lifted
     * function uses it to pick a landing pad; R[14] is left alone so it returns
     * to the caller just named. Same contract as lsw2_statereq_override. */
    g_cpu.R[15] = kLsw2BehaviourDisable;
    lsw2_arm9_FUN_0204b450();
}

void lsw2_behaviourhook_enable_override() {
    report(1, "ENABLE  FUN_0204a938 (broadcasts 0xA)");
    g_cpu.R[15] = kLsw2BehaviourEnable;
    lsw2_arm9_FUN_0204a938();
}

/* THE VALUE IS THE POINT HERE, not the actor. This one takes a POINTER TO A
 * BYTE in r1 and sets the local player's behaviour to whatever it holds, so
 * what it is about to write is the whole question - and the value is folded
 * into the dedup key so the same call site setting a DIFFERENT value still
 * reports. */
void lsw2_behaviourhook_setbyte_override() {
    const uint32_t src = g_cpu.R[1];
    const uint32_t caller = g_cpu.R[14];
    const int state = lsw2_state_current();

    ++g_calls[2];

    unsigned value = 0xFFFFu;
    if (in_ram(src)) value = read_byte(src);

    if (remember(2, caller, state * 256 + int(value & 0xFFu))) {
        std::fprintf(stderr,
                     "[behaviour] SETBYTE FUN_02057bc8 from %08X   state=%d "
                     "value=0x%02X src=%08X "
                     "(calls: disable=%ld enable=%ld setbyte=%ld)\n",
                     unsigned(caller), state, value, unsigned(src),
                     g_calls[0], g_calls[1], g_calls[2]);
    }

    g_cpu.R[15] = kLsw2BehaviourSetByte;
    lsw2_arm9_FUN_02057bc8();
}

/* THE STEP IS THE WHOLE POINT. This machine drives the match start, and the
 * open question is what it does ACROSS the arena load - whether it
 * ever reaches step 4 or 0x12 again once the level exists, since those are the
 * two arms that reach the spawn dispatcher.
 *
 * Reported per distinct (step, level) so a machine that ticks every frame on
 * one step cannot bury the one transition that matters, while the SAME step
 * reached again on a different level still reports. */
void lsw2_behaviourhook_matchstep_override() {
    const uint32_t self = g_cpu.R[0];
    const int level = lsw2_cheats_current_level();

    unsigned step = 0xFFFFu;
    if (in_ram(self)) step = bus_device_read32(0, self + 8u) & 0xFFFFu;

    /* Fold the level into the key so the same step on a new level reports. */
    if (remember(3, self, int(step) * 256 + (level & 0xFF))) {
        std::fprintf(stderr,
                     "[behaviour] MATCHSTEP FUN_0208c888  step=0x%02X "
                     "level=%d state=%d self=%08X\n",
                     step, level, lsw2_state_current(), unsigned(self));
    }

    g_cpu.R[15] = kLsw2MatchStep;
    lsw2_arm9_FUN_0208c888();
}

/* THE TWO ENABLE ROUTES not yet examined. Either firing in the arena
 * would answer the open question of where the enable happens; neither firing narrows it
 * to "the enable is somewhere else again". Keyed by level so the same route
 * reaching a new level still reports. */
void lsw2_behaviourhook_enableplain_override() {
    const int level = lsw2_cheats_current_level();
    if (remember(4, g_cpu.R[14], level)) {
        std::fprintf(stderr,
                     "[behaviour] ENABLE-PLAIN FUN_02057a28 from %08X  "
                     "level=%d state=%d\n",
                     unsigned(g_cpu.R[14]), level, lsw2_state_current());
    }
    g_cpu.R[15] = kLsw2EnablePlain;
    lsw2_arm9_FUN_02057a28();
}

void lsw2_behaviourhook_enablearrive_override() {
    const int level = lsw2_cheats_current_level();
    if (remember(5, g_cpu.R[14], level)) {
        std::fprintf(stderr,
                     "[behaviour] ENABLE-ARRIVE FUN_0207641c from %08X  "
                     "level=%d state=%d self=%08X\n",
                     unsigned(g_cpu.R[14]), level, lsw2_state_current(),
                     unsigned(g_cpu.R[0]));
    }
    g_cpu.R[15] = kLsw2EnableArrive;
    lsw2_arm9_FUN_0207641c();
}
