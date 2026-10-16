/* See netmsg.h. */

#include "netmsg.h"
#include "netmsglog.h"

#include "forksession.h"
#include "netcount.h"
#include "ui.h"

#include "state.h"

#include <cstdio>

namespace {

/* kLsw2MsgOwner, THE SLOT BASE AND THE STRIDE ALL COME FROM netmsglog.h.
 *
 * They were declared here too, as kLsw2MsgOwner, kLsw2MsgSlotBase and
 * kLsw2MsgStationStride - the same three ROM facts under a second set of names.
 * That mattered more than the usual duplication: netmsglog.h's copies are
 * PINNED by tools/sweep_rom_constants.py against the instructions they came
 * from, and these were not, so a correction to either side would have left
 * the other silently disagreeing with the gate's blessing.
 *
 * THE SEQUENCE BASE AND STRIDE CAME FROM THERE TOO, later: this file kept
 * kLsw2MsgSeqBase and kLsw2MsgSeqStride under that very comment, so the note above was
 * describing a collapse it had only half done. */

/* The header is four bytes and the pump steps in words, so a payload is padded
 * up to a word. Ten slots exist; this only ever uses the first, because the port
 * sends one message at a time and leaving the rest terminated is simpler to
 * reason about than sharing a ring with nobody. */
constexpr int kHeaderBytes = 4;
constexpr int kMaxPayload = 60;               /* comfortably inside a slot   */

constexpr uint32_t kRamLow = 0x02000000u;
constexpr uint32_t kRamHigh = 0x02400000u;

uint8_t read_byte(uint32_t a) {
    return uint8_t((bus_device_read32(0, a & ~3u) >> ((a & 3u) * 8u)) & 0xFFu);
}

void write_byte(uint32_t a, uint8_t v) {
    const uint32_t base = a & ~3u;
    const uint32_t sh = (a & 3u) * 8u;
    const uint32_t w = bus_device_read32(0, base);
    bus_device_write32(0, base, (w & ~(0xFFu << sh)) | (uint32_t(v) << sh));
}

}  // namespace

bool lsw2_netmsg_deliver(int station, uint8_t msg, const uint8_t* payload,
                         int len) {
    if (!ui_fork_build()) return false;
    if (station < 0 || station > 3) return false;
    if (msg == 0) return false;                 /* 0 means "skip" to the pump */
    if (len < 0 || len > kMaxPayload) return false;
    if (len > 0 && !payload) return false;
    if (kLsw2MsgOwner < kRamLow || kLsw2MsgOwner >= kRamHigh) return false;

    /* THE PUMP MUST BE RUNNING OR THIS IS A WRITE INTO A BUFFER NOBODY READS.
     * Refusing is the whole difference between "the message was not sent" and
     * "the message was sent and did nothing", and only one of those is
     * debuggable. */
    if (lsw2_netcount_guest() <= 1) return false;

    const uint32_t pkt =
        kLsw2MsgOwner + kLsw2MsgSlotBase + uint32_t(station) * kLsw2MsgStationStride;
    const uint32_t seq_at =
        kLsw2MsgOwner + uint32_t(station) * kLsw2MsgSeqStride + kLsw2MsgSeqBase;

    /* SIZE IN WORDS, rounded up - it is the pump's step to the next packet, so
     * getting it short would make the pump read the tail of this packet as the
     * next one's header. */
    const int bytes = kHeaderBytes + len;
    const int words = (bytes + 3) / 4;

    /* A SEQUENCE THAT DIFFERS FROM THE SLOT'S LAST. Anything different
     * dispatches; +1 keeps it moving so two sends in a row are both seen. */
    const uint8_t last = read_byte(seq_at);
    const uint8_t seq = uint8_t(last + 1u);

    /* PAYLOAD FIRST, THEN THE HEADER. The pump can run between any two of these
     * writes - it is the guest's own tick and the port is not synchronised with
     * it - so the message id and the sequence, which are what make a packet
     * live, go in last. Written the other way round, the pump could dispatch a
     * packet whose payload was still half the previous one's. */
    for (int i = 0; i < len; ++i) write_byte(pkt + uint32_t(kHeaderBytes + i),
                                             payload[i]);
    /* Zero the padding so a short payload does not carry stale bytes. */
    for (int i = kHeaderBytes + len; i < words * 4; ++i)
        write_byte(pkt + uint32_t(i), 0);

    /* AND TERMINATE THE LIST, so the pump stops after this packet rather than
     * walking into whatever the buffer held before. */
    write_byte(pkt + uint32_t(words) * 4u, 0);

    write_byte(pkt + 0u, uint8_t(words));
    write_byte(pkt + 2u, seq);
    write_byte(pkt + 3u, 0);
    write_byte(pkt + 1u, msg);      /* LAST: a non-zero id is what arms it */

    return true;
}

bool lsw2_netmsg_start_round(uint16_t seed) {
    /* `{u16 seed at +4, u8 selector at +6}` - confirmed field for
     * field against the game's own sender at `0x0209CC08` (`STRH` the seed to
     * `sp+4`, `STRB` the selector to `sp+6`) and its reader at `0x020B15A8`
     * (`LDRH [r4,#8]`, `LDRB [r4,#0x0A]` - the same two fields four bytes on,
     * which is the packet header).
     *
     * THE SELECTOR IS ALWAYS THE CHOOSER, AND IT USED TO DEPEND ON THE ROSTER.
     *
     * Reading the game's own round-start senders showed the flow is
     * a CHAIN rather than a choice:
     *
     *   selector 1 -> the map/gamemode/time chooser, state 0x36
     *   the chooser, on confirm, itself broadcasts 0x0A with SELECTOR 3
     *   selector 3 -> the hunter statue select, state 0x29, and no chooser
     *
     * So sending 3 for a Hunters round did not "start with the hunter select"
     * - it started with the hunter select INSTEAD OF letting the host pick the
     * map. The required order: the lobby timer runs out first, and the host
     * then picks map, game mode and time through the cartridge's usual
     * chooser; after that, when the Character setting is Hunters, the hunter
     * statue select follows with its usual 30s timer.
     *
     * The chooser first, then the select. Sending 1 gets both, because the
     * game's own chooser sends the 3. */
    uint8_t payload[3];
    payload[0] = uint8_t(seed & 0xFFu);
    payload[1] = uint8_t((seed >> 8) & 0xFFu);
    payload[2] = kLsw2ArenaNoSelect;

    /* THE HALF THAT IS LOCATED AND NOT BUILT - AND IT IS BUILT NOW.
     *
     * This used to print, every non-Hunters round: "the statue select is not
     * suppressed yet - the chooser sends its own selector 3". The first half of
     * that is still true and the conclusion is not.
     *
     * The chooser DOES broadcast its own 3 at `0x0209E6D8` after the host
     * confirms, so the select cannot be prevented by choosing a selector here.
     * selectskip.cpp does not try: it lets the select be created and then
     * fills in the game's own completion bookkeeping - `+0x2B14[slot] = 1` per
     * player and `+0x2B06` = the player count - so the state finishes itself on
     * its next update. Same shape as the level warp and the page bounds: write
     * the game's data and let the game's code act on it. lsw2_selectskip_tick
     * runs from the fork tick and reports what it marked.
     *
     * THE MESSAGE WAS THE WORST PART OF IT. A stale comment misleads whoever
     * reads the file; a stale fprintf misleads the PLAYER, once per round, about
     * a feature that works. Overriding 0x0209E338 was considered and rejected -
     * its six callers are all in 0x0209Exxx, so the recompiler emits direct
     * calls and an override there could never fire; and 0x020B04F0, the
     * broadcast itself, has 73 callers across the whole game. */

    const int station = lsw2_fork_session().local_station();
    const bool ok = lsw2_netmsg_deliver(station, kLsw2MsgRoundStart, payload,
                                        int(sizeof payload));
    std::fprintf(stderr,
                 "[fork] round start %s: selector %u, seed %u, station %d\n",
                 ok ? "delivered" : "REFUSED (pump idle or state unreachable)",
                 unsigned(payload[2]), unsigned(seed), station);
    return ok;
}
