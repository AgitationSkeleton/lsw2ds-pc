/* THE CARTRIDGE'S OWN ROSTER, fed into the fork's session object.
 *
 * THE GOAL, IN PLAIN ENGLISH. Three online-lobby features - the "<name> has joined the
 * game!" popup, the host's Kick Player list, and (its joiner half) the pause
 * row reading "Disconnect" - are all fed by `Lsw2ForkSession`'s peer list, and
 * on the preferred route (the cartridge's own radio join) that list is EMPTY. Measured: a
 * joiner admitted through the cartridge's own browse list is invisible to the
 * fork's session, which only ever learns peers from its own TCP transport.
 * `players 2 (fork 1, cartridge 2)` - the cartridge seated two, the fork knew
 * one.
 *
 * WHERE THE PEERS ACTUALLY ARE, read live from RAM. The cartridge
 * keeps a record whose pointer sits at `[kLsw2WirelessOwner + 0xAD0]`; its four
 * 24-byte name slots at `+0x51 + 24*slot` held "Maclunkey" for both seated
 * players in an admitted session, and the net-count object's `+0x29` byte read
 * 2 - the seat count. `+0x2A` is the LOCAL station, so the local player's own
 * slot is known and the others are the remote peers.
 *
 * SO THIS READS THAT RECORD AND FEEDS THE SESSION. On the host it calls the
 * session's existing `peer_joined` / `peer_left_at` for each remote slot that
 * appears or vanishes - the same calls the TCP path makes, so the popup, the
 * kick list and the directory's player count all come true without any of them
 * knowing the peer came from the radio rather than a socket. It invents
 * nothing: the names are the cartridge's own, sanitised the way every peer
 * name is (lsw2_name.h).
 *
 * WHY IT DOES NOT FIGHT THE TCP PATH. On a fork-hosted TCP session the peers
 * ARE in console_.peers already, and this would double-count them. So it acts
 * only when the fork's own seat path has NOT seated anyone (its list is empty)
 * and the cartridge's has - which is exactly the radio-route case and no
 * other. When both are somehow populated the cartridge reading is ignored and
 * a one-time line says so.
 *
 * THE JOINER HALF - marking the session "in a session" so the pause row
 * relabels - is a separate change in forksession and is NOT done here; this
 * module is the host-side roster feed, testable on its own. */

#ifndef LSW2_CARTROSTER_H
#define LSW2_CARTROSTER_H

#include <cstdint>

/* The record pointer lives at the wireless owner object plus this. Read
 * live; the wireless owner object's base is coopjoin.h's kLsw2WirelessOwner. */
constexpr uint32_t kLsw2CartRosterPtrOffset = 0xAD0u;

/* Inside the record: four names, 24 bytes each, starting here (the earlier
 * static reading "settingsObject + 0x51 + 24*slot", confirmed by the live read). */
constexpr uint32_t kLsw2CartNameOffset = 0x51u;
constexpr uint32_t kLsw2CartNameStride = 24u;
constexpr int kLsw2CartMaxSlots = 4;

/* Once a frame from the fork tick, fork build only. Does nothing unless the
 * cartridge has seated more than the local player and the fork's own list is
 * empty - the radio route. See cartroster.h's header. */
void lsw2_cartroster_tick();

/* ------------------------------------------------------------- the policy
 *
 * Split out so test_cartroster.cpp can pin "which slots are remote peers, and
 * what changed since last frame" without a ROM or a guest. */

/* One slot's state, as read from the record. */
struct Lsw2CartSlot {
    bool present;                 /* the name slot is non-empty */
    char name[kLsw2CartNameStride + 1];
};

/* Given the four slots, the local station, and the seat count, decide which
 * remote slots should be peers. Writes up to 3 names into `out` (the session
 * holds at most three peers besides the local player) with their slot indices
 * in `out_station`, and returns how many. A slot equal to `local_station` is
 * never a peer; an empty slot is never a peer; nothing beyond `seat_count`
 * total seats is trusted. Pure. */
int lsw2_cartroster_remote_peers(const Lsw2CartSlot slots[kLsw2CartMaxSlots],
                                 int local_station, int seat_count,
                                 const char* out[3], int out_station[3]);

#endif /* LSW2_CARTROSTER_H */
