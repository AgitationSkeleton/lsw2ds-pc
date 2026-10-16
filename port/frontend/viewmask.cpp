/* The guest half of viewmask.h: read the mask, say when it changes. */

#include "viewmask.h"

#include "consolemenu.h"
#include "forksession.h"
#include "ui.h"

#include "state.h"

#include <cstdio>
#include <cstdlib>

namespace {

constexpr uint32_t kRamLow = 0x02000000u;
constexpr uint32_t kRamHigh = 0x02400000u;

int g_last = -2;   /* -2 so the first observation always prints, -1 included */

}  // namespace

int lsw2_viewmask_bound() {
    /* The same word `0x02073044` returns: `[0x020FEB88 + 4]`.
     *
     * A SANITY CEILING, because this is read before the game has necessarily
     * initialised it. The bound is a slot count - four stations at most - so
     * anything large is uninitialised memory rather than a number, and
     * reporting -1 for it is more honest than printing 0x4F3A2B10 as a "bound". */
    const uint32_t v = bus_device_read32(0, kLsw2PumpCountWord + kLsw2PumpBound);
    return v > 0x10u ? -1 : int(v);
}

int lsw2_viewmask_guest() {
    const uint32_t mgr = bus_device_read32(0, kLsw2ViewManagerPtr);
    if (mgr < kRamLow || mgr >= kRamHigh) return -1;

    /* RECORD 0, because that is where the resolver reads it. `0x020AF780` calls
     * slot 0 with index 0 and then `LDRH [r0, #0x66]` - so the mask that
     * resolves every other view lives in the first record, and the copies in
     * other records (if any) are not what the game consults.
     *
     * The count derivation at `0x020720D0` reads `+0x66` from a record whose
     * index is whatever `r4` holds on that path, which this reading did not
     * establish. If those ever disagree, the resolver's record 0 is the one
     * that decides which view a positive index means. */
    const uint32_t addr = mgr + kLsw2ViewArray + kLsw2ViewMask;

    /* A HALFWORD through its word - `+0x66` is even but not word aligned, the
     * same discipline the yaw at `+0x6A` needs. */
    const uint32_t w = bus_device_read32(0, addr & ~3u);
    return int((w >> ((addr & 2u) * 8u)) & 0xFFFFu);
}

void lsw2_viewmask_tick() {
    if (!ui_fork_build()) return;

    const int now = lsw2_viewmask_guest();
    if (now == g_last) return;
    g_last = now;

    if (now < 0) {
        std::fprintf(stderr, "[fork] view mask: manager not allocated\n");
        return;
    }

    /* WHAT THIS LINE IS FOR. If the arena presents a mask of 1 while four
     * players are connected, then the cartridge believes it is a single-player
     * session no matter what any port module writes downstream, and the online
     * lobby's message layer cannot run. If it presents something else, that is the
     * value the fork has to produce. One session answers it. */
    /* THE GAME'S OWN BOUND, printed beside the mask because it can defeat it.
     * `0x020720EC` is a BLE: a bound of zero skips the popcount and stores a
     * player count of zero however many bits the mask has. Reporting the port's
     * own constant 4 here instead would have looked correct while the game
     * computed nothing. */
    const int bound = lsw2_viewmask_bound();

    std::fprintf(stderr,
                 "[fork] view mask = 0x%X  (%d set of %d slots; game bound %d; "
                 "view1 -> record %d)\n",
                 unsigned(now), lsw2_viewmask_count(unsigned(now),
                                                    kLsw2ViewRecords),
                 kLsw2ViewRecords, bound,
                 lsw2_viewmask_resolve(unsigned(now), 1));

    if (bound <= 0) {
        std::fprintf(stderr,
                     "   the bound is %d, so the game's popcount is SKIPPED and "
                     "its player count stays 0 whatever the mask holds - "
                     "publishing the mask alone cannot be enough\n",
                     bound);
    }
}

namespace {

/* WHAT WE FOUND, so we can put it back. A saved-and-restored value is the
 * difference between standing in for the connection layer and permanently
 * altering a field the game may later use for something we did not predict.
 * -1 means "we have not written, so there is nothing to restore". */
int g_saved = -1;
unsigned g_published = 0;

/* AUTO BACK-OFF LATCH. Once the local WH has reached a real DATASHARING
 * association, the game's own datasharing pump owns the authority mask / bound /
 * view record, and the fork re-asserting them every frame RACES and corrupts the
 * real netcode - which is what tripped the host to state 38. Latch it (not a
 * per-frame read) so the arena transition's WH flap does not re-enable the writes
 * mid-match. Reset when the session ends. A joiner that never associates (the
 * abandoned fork-transport path) never reaches DATASHARING, so it still gets the
 * stand-in; and the early lobby (before DATASHARING) still gets it, which keeps the
 * lobby player count working. */
bool g_real_pump_seen = false;

void write_mask(uint16_t v) {
    /* `0x0210B32E` is halfword aligned and NOT word aligned, so the store is a
     * read-modify-write of the containing word - `+0xC` is its neighbour and is
     * not ours to clear. */
    const uint32_t addr = kLsw2SessionAuthority + kLsw2AuthorityMask;
    const uint32_t base = addr & ~3u;
    const uint32_t sh = (addr & 2u) * 8u;
    const uint32_t w = bus_device_read32(0, base);
    bus_device_write32(0, base, (w & ~(0xFFFFu << sh)) | (uint32_t(v) << sh));
}

int read_mask() {
    const uint32_t addr = kLsw2SessionAuthority + kLsw2AuthorityMask;
    const uint32_t w = bus_device_read32(0, addr & ~3u);
    return int((w >> ((addr & 2u) * 8u)) & 0xFFFFu);
}

/* THE BOUND, saved and restored on the same terms as the mask.
 *
 * A PLAIN WORD, not a read-modify-write: `0x020FEB8C` is word aligned, unlike
 * the mask's `0x0210B32E`.
 *
 * The saved value is a uint32_t and not an int because the idle bound is
 * 0xFFFFFFFF - the capture's -1 - and restoring it has to put those bits back
 * exactly. `g_saved_bound_valid` carries the "have we written" flag instead of
 * the -1 sentinel the mask uses, because -1 is a REAL value of this field. */
uint32_t g_saved_bound = 0;
bool g_saved_bound_valid = false;

/* THE VIEW RECORD MASK, at record 0's +0x66.
 *
 * On the HOST the cartridge's own datasharing pump copies the session-authority
 * mask (0x0210B32E) into this record every frame, because the host's radio is
 * in WH_SYSSTATE_DATASHARING. On a JOINER the radio never reaches DATASHARING -
 * it sits in MEASURECHANNEL, because a child does not start a session of its
 * own - so that pump never runs and the record stays -1 no matter
 * what the authority mask holds. The count derivation at 0x020720D0 reads THIS
 * record, not the authority word, so a joiner's player count is stuck at 1
 * until the fork stands in for the absent pump and writes the record itself.
 * That is the same standing-in-for-the-connection-layer the authority mask
 * already is. */
int g_saved_record = -1;   /* -1: not written, so nothing to restore */

int read_view_record_mask() {
    const uint32_t mgr = bus_device_read32(0, kLsw2ViewManagerPtr);
    if (mgr < kRamLow || mgr >= kRamHigh) return -1;
    const uint32_t addr = lsw2_viewmask_record(mgr, 0) + kLsw2ViewMask;
    const uint32_t w = bus_device_read32(0, addr & ~3u);
    return int((w >> ((addr & 2u) * 8u)) & 0xFFFFu);
}

bool write_view_record_mask(uint16_t v) {
    const uint32_t mgr = bus_device_read32(0, kLsw2ViewManagerPtr);
    if (mgr < kRamLow || mgr >= kRamHigh) return false;
    /* Same halfword-through-its-word discipline as write_mask: +0x66 is even
     * but not word aligned, so the store is a read-modify-write. */
    const uint32_t addr = lsw2_viewmask_record(mgr, 0) + kLsw2ViewMask;
    const uint32_t base = addr & ~3u;
    const uint32_t sh = (addr & 2u) * 8u;
    const uint32_t w = bus_device_read32(0, base);
    bus_device_write32(0, base, (w & ~(0xFFFFu << sh)) | (uint32_t(v) << sh));
    return true;
}

}  // namespace

void lsw2_viewmask_publish() {
    if (!ui_fork_build()) return;

    const Lsw2ForkSession& session = lsw2_fork_session();
    const Lsw2ConsoleState& console = session.console();

    /* A SESSION OF ONE IS NOT A SESSION. The cartridge's own handlers never run
     * for a lone machine either, and writing a lone bit here would be the port
     * announcing a wireless game to a game that is not in one - which is how
     * the console came to say "You can't do that during a Wireless game!". */
    const bool live = session.state() != Lsw2SessionState::Offline &&
                      session.player_count() >= 2;

    if (!live) {
        /* PUT IT BACK, once. */
        if (g_saved >= 0) {
            write_mask(uint16_t(g_saved));
            std::fprintf(stderr,
                         "[fork] view mask released, restored to 0x%X\n",
                         unsigned(g_saved));
            g_saved = -1;
            g_published = 0;
        }
        if (g_saved_bound_valid) {
            bus_device_write32(0, kLsw2PumpCountWord + kLsw2PumpBound,
                               g_saved_bound);
            std::fprintf(stderr,
                         "[fork] slot bound released, restored to %d\n",
                         int(g_saved_bound));
            g_saved_bound_valid = false;
        }
        if (g_saved_record >= 0) {
            write_view_record_mask(uint16_t(g_saved_record));
            g_saved_record = -1;
        }
        g_real_pump_seen = false;   /* new session next time: re-arm the stand-in */
        return;
    }

    /* A/B LEVER: these force-writes (authority mask, slot bound, and
     * the joiner's view-record mask) were built when the joiner NEVER reached
     * WH_SYSSTATE_DATASHARING, so its real datasharing pump never ran and the fork
     * stood in for it. cap-128 (patch 0013) changed that: the joiner now DOES reach
     * DATASHARING, so its own pump runs - and the fork re-asserting the record every
     * frame then RACES the real pump, which can publish an inconsistent record and
     * trip the host's cross-player consistency check (state 38). LSW2_NO_FORCE_MASKS
     * disables all of these writes so the real association drives the netcode
     * unaided, to A/B whether the fork's standing-in is now the cause of the desync. */
    static const bool no_force_masks = [] {
        const char* e = std::getenv("LSW2_NO_FORCE_MASKS");
        return e && *e == '1';
    }();

    /* Latch the real datasharing pump: WH state 5 (DATASHARING) at
     * kLsw2SessionAuthority+kLsw2WhSystemState means the game's own pump is now in
     * charge of these fields. Read as the low byte of the word (state is 0..9). */
    if (!g_real_pump_seen) {
        const uint32_t wh = bus_device_read32(
            0, kLsw2SessionAuthority + kLsw2WhSystemState);
        if ((wh & 0xFFu) == 5u) {   /* WH_SYSSTATE_DATASHARING */
            g_real_pump_seen = true;
            std::fprintf(stderr,
                         "[fork] view mask: WH reached DATASHARING - the game's own "
                         "pump owns the mask/bound/record now; the fork backs off "
                         "its force-writes\n");
        }
    }

    if (no_force_masks || g_real_pump_seen) return;

    const unsigned want = lsw2_viewmask_compose(
        session.local_station(), console.peer_station, console.peer_count);

    /* STAND IN FOR THE DATASHARING PUMP ON A JOINER. See write_view_record_mask:
     * a client's radio never reaches DATASHARING, so the cartridge never copies
     * the authority mask into the view record the count derivation reads, and
     * the joiner's player count stays 1 while the host derives 2. Assert the
     * composed mask into the record every frame while joined. The host is left
     * entirely to its own pump, which owns the record - the fork must not race
     * it. Re-asserted each frame (not gated by g_published) because there is no
     * pump to hold the value if the game clears it. */
    if (!session.is_host()) {
        const int rec = read_view_record_mask();
        if (rec >= 0 && unsigned(rec) != want &&
            write_view_record_mask(uint16_t(want))) {
            if (g_saved_record < 0) g_saved_record = rec;
            static unsigned last_rec = 0xFFFFFFFFu;
            if (last_rec != want) {
                last_rec = want;
                std::fprintf(stderr,
                             "[fork] view record populated 0x%X on the joiner "
                             "(no datasharing pump to copy the authority mask) "
                             "- the count derivation reads this record\n",
                             want);
            }
        }

        /* THE WH CONNECTED-STATION COUNT CAP. The joiner's player
         * count (view-manager vtable+0x1C = FUN_020af7f4) counts view-record
         * +0x66 mask bits only UP TO WHmgr->vtable[0x2c] = *(u16*)0x0210B328 (the
         * WH context 0x0210B320 + 8). The joiner's radio never associates, so that
         * field stays 1 and caps the count at 1 even with the record mask 0x3.
         * Assert it to self+peers while joined. ENV-GATED (LSW2_FORCE_WHCOUNT):
         * faking the SIBLING netgame counter 0x021027F8+0x29 crashed by walking
         * absent station records - this tests whether THIS field is safe.
         * 0x0210B328 is word-aligned; its low halfword is the count. */
        static const bool force_whcount = [] {
            const char* e = std::getenv("LSW2_FORCE_WHCOUNT");
            return e && *e == '1';
        }();
        if (force_whcount) {
            const uint32_t addr = 0x0210B328u;
            const uint16_t want_ct = uint16_t(console.peer_count + 1);
            const uint32_t w = bus_device_read32(0, addr);
            if ((w & 0xFFFFu) != want_ct) {
                bus_device_write32(0, addr, (w & 0xFFFF0000u) | want_ct);
                static unsigned last_ct = 0xFFFFFFFFu;
                if (last_ct != want_ct) {
                    last_ct = want_ct;
                    std::fprintf(stderr,
                                 "[fork] WH connected-station count forced to %u "
                                 "at 0x0210B328 (LSW2_FORCE_WHCOUNT) - caps the "
                                 "joiner's vtable+0x1C player count\n",
                                 unsigned(want_ct));
                }
            }
        }
    }

    /* THE BOUND FIRST, BEFORE THE MASK, because that is the order the cartridge
     * does it in and the order is load-bearing.
     *
     * The arena capture caught the join: at frame 4620 the bound was already 4
     * while the mask still read 0x1 and the count was still -1; the mask went
     * to 0x3 and the count to 2 only at 4740. The bound is not derived from the
     * mask - it arrives ahead of it.
     *
     * And it has to. The popcount at `0x020720E4` reads this word, and
     * `CMP r0,#0 / BLE` skips the whole derivation when it is not positive -
     * so publishing a mask while the bound is still the idle -1 stores a
     * player count of ZERO no matter how many bits the mask has. That was the
     * "necessary and may not be sufficient" the header used to leave open.
     *
     * WRITTEN ONCE, and only when it is not already the capacity, so a session
     * where the game has set it itself is left alone. */
    if (!g_saved_bound_valid) {
        const uint32_t addr = kLsw2PumpCountWord + kLsw2PumpBound;
        const uint32_t had = bus_device_read32(0, addr);
        if (had != uint32_t(kLsw2PumpBoundValue)) {
            g_saved_bound = had;
            g_saved_bound_valid = true;
            bus_device_write32(0, addr, uint32_t(kLsw2PumpBoundValue));
            std::fprintf(stderr,
                         "[fork] slot bound published %d (was %d) - the "
                         "popcount is skipped entirely below 1\n",
                         kLsw2PumpBoundValue, int(had));
        }
    }

    if (g_saved < 0) {
        const int had = read_mask();
        if (had < 0) return;
        g_saved = had;
    }

    if (want == g_published) return;
    g_published = want;

    write_mask(uint16_t(want));
    std::fprintf(stderr,
                 "[fork] view mask published 0x%X (%d seated) - the game "
                 "derives its player count from this\n",
                 want, lsw2_viewmask_count(want, kLsw2ViewRecords));
}
