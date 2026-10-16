/* See markerhook.h. Name the sprite the co-op arrow is, and the code that asks
 * for it. */

#include "markerhook.h"

#include "cheats.h"
#include "state.h"

#include <cstdio>
#include <cstdlib>

extern "C" void lsw2_arm9_FUN_0209f640(void);

namespace {

constexpr int kArm9 = 9;

/* ONE LINE PER DISTINCT (id, caller, level), not per call.
 *
 * Markers are attached constantly - this runs for every sprite in the accepted
 * range on every actor that gets one - and printing each call would bury the
 * answer under its own traffic. This project has already made a log a
 * frame-rate problem once by printing something that runs every frame. The
 * interesting thing is the SET of (what, who, where). */
constexpr int kMaxSeen = 64;

struct Seen {
    uint32_t id;
    uint32_t caller;
    int level;
};

Seen g_seen[kMaxSeen];
int g_count = 0;
long g_calls = 0;

bool remember(uint32_t id, uint32_t caller, int level) {
    for (int i = 0; i < g_count; ++i) {
        if (g_seen[i].id == id && g_seen[i].caller == caller &&
            g_seen[i].level == level) {
            return false;
        }
    }
    if (g_count < kMaxSeen) {
        g_seen[g_count].id = id;
        g_seen[g_count].caller = caller;
        g_seen[g_count].level = level;
        ++g_count;
        return true;
    }
    /* FULL, AND SAY SO ONCE. A cap that silently stops reporting reads exactly
     * like a marker that stopped being attached, which is one of the two answers
     * this file exists to tell apart.
     *
     * A FLAG, NOT ++g_count - THAT WAS A REAL CRASH. "Said once" used to be
     * g_count stepped to kMaxSeen + 1. The
     * loop above reads g_seen[i] for every i < g_count, so the compiler may
     * assume g_count <= kMaxSeen, and it folded `g_count < kMaxSeen` into
     * `g_count != kMaxSeen`: at 65 the write went ahead, one element past the
     * array per new site. A build on Jabba's Palace made the 65th, 66th and 67th
     * distinct sites; the 67th's level word landed on menuhost's g_menu, and
     * the next frame's menu composite read through (Lsw2Menu*)4. */
    static bool said_full = false;
    if (!said_full) {
        said_full = true;
        std::fprintf(stderr, "[marker] log full at %d distinct sites - later "
                             "ones are NOT shown\n", kMaxSeen);
    }
    return false;
}

bool in_ram(uint32_t a) { return a >= 0x02000000u && a < 0x02400000u; }

/* WHICH ID TO WITHHOLD, from `LSW2_MARKER_SUPPRESS`. Accepts hex with or
 * without the 0x, because typing it either way and getting silence would be
 * indistinguishable from "the id was not the arrow" - which is the exact answer
 * this test exists to give, so it must not be forgeable by a typo.
 *
 * A value that parses to nothing suppresses NOTHING and says so once, rather
 * than defaulting to some id and reporting a result about it. */
long suppress_id() {
    static long cached = -2;
    if (cached == -2) {
        const char* v = std::getenv("LSW2_MARKER_SUPPRESS");
        cached = -1;
        if (v && *v) {
            char* end = nullptr;
            const long parsed = std::strtol(v, &end, 0);
            if (end && end != v) {
                cached = parsed;
            } else {
                std::fprintf(stderr,
                             "[marker] LSW2_MARKER_SUPPRESS=\"%s\" is not a "
                             "number - suppressing NOTHING (a typo must not read "
                             "as 'that id was not the arrow')\n", v);
            }
        }
    }
    return cached;
}

/* AND ONLY ON ONE LEVEL. `LSW2_MARKER_SUPPRESS_LEVEL`, default 9 - the cantina
 * the fork uses as its lobby, which is the level whose marker table declares
 * the 0x8A descriptor.
 *
 * THE FIRST VERSION SUPPRESSED EVERYWHERE, AND THAT BROKE THE BOOT. The run
 * withheld id 0x8A at LEVEL 0 - the boot level attaches it too - and the console
 * died there without ever reaching the cantina, so the test measured nothing
 * about the lobby at all:
 *
 *     [marker] SUPPRESSED id=0x8A at level=0 ... console Responding=False
 *
 * That is worth keeping as a finding rather than only as a bug in the harness:
 * the marker this test withholds is LOAD-BEARING during boot. Whatever the
 * lobby's copy turns out to be, "just stop attaching id 0x8A" is not a fix that
 * could ever ship unscoped. */
long suppress_level() {
    static long cached = -2;
    if (cached == -2) {
        const char* v = std::getenv("LSW2_MARKER_SUPPRESS_LEVEL");
        cached = 9;
        if (v && *v) {
            char* end = nullptr;
            const long parsed = std::strtol(v, &end, 0);
            if (end && end != v) cached = parsed;
        }
    }
    return cached;
}

bool suppressed(uint32_t id, int level) {
    const long want = suppress_id();
    return want >= 0 && uint32_t(want) == id && level == int(suppress_level());
}

/* DROP THE SECOND DESCRIPTOR, FROM INSIDE THE LOOP THAT READS IT.
 *
 * The fork-tick version of this had a race it could not win: the count lives in
 * the ARM9 image, which is not resident when the fork's first tick runs (it
 * reads 0), and by the time it IS resident the level-9 markers may already have
 * been built in the same frame. A patch that has to beat a level load is a
 * patch that sometimes does not.
 *
 * There is no race here, because the loop re-reads its own bound every pass:
 *
 *     0208F280  BLX r12              ; attach this descriptor  <- we are here
 *     0208F284  LDRH r0,[r5,#0x6]    ; the count, RE-READ each iteration
 *     0208F288  ADD  r9,r9,#0x1
 *     0208F290  CMP  r9,r0
 *
 * Level 9's two descriptors are attached in order - id 0x0000 first (which the
 * range check rejects anyway), then id 0x008A. Lowering the count to 1 during
 * the FIRST call means the loop's own re-read ends it, and the 0x8A descriptor
 * is never built. The game stops the loop itself; nothing is skipped or
 * interrupted, which is the mistake an earlier attempt made (returning early
 * from the attach override hung the guest).
 *
 * Identified by `lr` rather than by guessing: only the marker loop calls from
 * `0208F284`, so a call from anywhere else is left alone. */
constexpr uint32_t kLsw2MarkerLoopReturn = 0x0208F284u;

void maybe_drop_descriptor(int level, uint32_t caller) {
    static bool done = false;
    if (done) return;
    const char* v = std::getenv("LSW2_MARKER_DROP_DESC");
    if (!(v && *v && *v != '0')) { done = true; return; }
    if (level != kLsw2MarkerDropLevel || caller != kLsw2MarkerLoopReturn) return;

    const uint32_t addr = kLsw2Level9MarkerEntry + kLsw2MarkerEntryCount;
    const uint32_t word = bus_device_read32(kArm9, addr & ~3u);
    const uint16_t count = uint16_t((addr & 2u) ? (word >> 16) : (word & 0xFFFFu));
    if (count != kLsw2Level9CountAsShipped) {
        std::fprintf(stderr,
                     "[marker] descriptor-count patch REFUSED: %08X reads %u, "
                     "not the %u the ROM ships - not writing blind\n",
                     unsigned(addr), unsigned(count),
                     unsigned(kLsw2Level9CountAsShipped));
        done = true;
        return;
    }

    const uint16_t want = uint16_t(kLsw2Level9CountAsShipped - 1u);
    const uint32_t merged = (addr & 2u)
        ? ((word & 0x0000FFFFu) | (uint32_t(want) << 16))
        : ((word & 0xFFFF0000u) | want);
    bus_device_write32(kArm9, addr & ~3u, merged);

    const uint32_t after = bus_device_read32(kArm9, addr & ~3u);
    const uint16_t now = uint16_t((addr & 2u) ? (after >> 16) : (after & 0xFFFFu));
    std::fprintf(stderr,
                 "[marker] level %d descriptor count %08X: %u -> %u (read back %u)"
                 " - the loop re-reads this, so id 0x8A should never be built\n",
                 level, unsigned(addr), unsigned(count), unsigned(want),
                 unsigned(now));
    done = true;
}

}  // namespace

void lsw2_markerhook_attach_override() {
    const uint32_t owner = g_cpu.R[0];
    const uint32_t descriptor = g_cpu.R[1];
    const uint32_t caller = g_cpu.R[14];
    const int level = lsw2_cheats_current_level();

    ++g_calls;

    /* THE ID IS A HALFWORD at [r1+0] - read widths matter. Reading
     * it as a word here would fold the next field in and report ids that do not
     * exist. */
    uint32_t id = 0xFFFFFFFFu;
    if (in_ram(descriptor)) {
        const uint32_t w = bus_device_read32(kArm9, descriptor & ~3u);
        id = (descriptor & 2u) ? (w >> 16) : (w & 0xFFFFu);
    }

    const bool accepted = id >= kLsw2MarkerIdLow && id <= kLsw2MarkerIdHigh;

    /* BEFORE DELEGATING, and only from the marker loop on the target level.
     * The loop re-reads its bound after this call returns, so lowering it here
     * ends the loop by the game's own logic rather than by skipping anything. */
    maybe_drop_descriptor(level, caller);

    if (remember(id, caller, level)) {
        std::fprintf(stderr,
                     "[marker] ATTACH id=0x%02X %s  level=%d owner=%08X "
                     "from %08X  (calls so far %ld)\n",
                     unsigned(id),
                     accepted ? "IN RANGE - a marker" : "out of range - ignored",
                     level, unsigned(owner), unsigned(caller), g_calls);
    }

    /* THE IDENTIFICATION TEST, and it is the only thing here that can change
     * what the player sees.
     *
     * Reading the level table established that level 9 declares exactly one
     * marker descriptor in range and its id is 0x8A, and that the live attach at
     * level 9 uses that id. What it could NOT establish is that 0x8A is the
     * GREEN ARROW seen in the lobby -
     * "the only candidate in the data" is not "seen on screen", and this project
     * has been wrong before by treating the most plausible reading of a structure
     * as a measurement.
     *
     * So: with `LSW2_MARKER_SUPPRESS=0x8A` set, this one id is not attached, and
     * a screenshot says whether the arrow went with it. One run, one difference,
     * and the answer is visible rather than argued.
     *
     * OFF BY DEFAULT. Unset, the cartridge's routine runs for every id exactly
     * as before, so the shipped build is unchanged - and even armed this is an
     * IDENTIFICATION test, not the fix. Suppressing a marker the ROM declares is
     * a deliberate divergence and should be decided on its own terms once we
     * know what we are suppressing. */
    if (suppressed(id, level)) {
        if (remember(id, caller, level + 0x10000)) {
            std::fprintf(stderr,
                         "[marker] SUPPRESSED id=0x%02X at level=%d (owner %08X, "
                         "from %08X) - LSW2_MARKER_SUPPRESS is set; this is the "
                         "identification test, not a fix\n",
                         unsigned(id), level, unsigned(owner), unsigned(caller));
        }
        return;
    }

    /* THE GAME'S OWN CODE, UNCHANGED. R[15] names the entry because the lifted
     * function uses it to pick a landing pad; R[14] is left alone so it returns
     * to the caller just named. Same contract as the behaviour hooks. */
    g_cpu.R[15] = kLsw2MarkerAttach;
    lsw2_arm9_FUN_0209f640();
}
