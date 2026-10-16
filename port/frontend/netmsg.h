/* Handing the cartridge one of its own messages.
 *
 * THE WHOLE POINT, in one sentence: the game dispatches messages out of a plain
 * memory buffer with a sequence byte, every frame, from its own state-manager
 * tick - so the port can deliver one by writing it, and never has to call into
 * guest code.
 *
 * Both ends have been read. The pump `0x020B0410` walks the stations, and for
 * each packet compares `[packet + 0x02]` against the slot's last-seen sequence;
 * anything different, with a non-zero message id, goes to the dispatcher
 * `0x020B1248` and thence to its 58-handler table.
 *
 *     obj = 0x0210794C
 *     packets   obj + 0xA3C + station*0x344,  ten slots
 *     sequences obj + station*0x0A + 0x40,    one byte per slot
 *
 *     +0x00  size in WORDS, and the step to the next packet; 0 ends the list
 *     +0x01  message id; 0 means "skip this one"
 *     +0x02  sequence, halfword
 *     +0x04  the payload, exactly where the dispatcher hands it on
 *
 * WHY THIS IS THE RIGHT SHAPE and not a hack. Every effect the message has -
 * the state requests, the RNG seed, the save writes, the screen it lands on - is
 * the cartridge's own code doing the cartridge's own thing in the cartridge's own
 * order. The port supplies the fact that a message arrived and nothing else. It
 * is the same preference as the level warp, the pending-character
 * request and the RNG seed: fill in the game's own data
 * and let the game act.
 *
 * IT REQUIRES THE PUMP TO BE RUNNING, which means more than one player - see
 * netcount.h. A send while the count is 1 would sit in the buffer unread, so
 * this refuses rather than pretending.
 */

#ifndef LSW2_NETMSG_H
#define LSW2_NETMSG_H

#include <cstdint>

/* The message ids this port knows by meaning, from the dispatcher's handler
 * table and the handler bodies. Named so call sites do not carry bare numbers.
 *
 * OBSERVED IN REAL TRAFFIC, 2026-08-30, which these ids had never been.
 *
 * out/work/archive/gpudump_3matches records three matches on both machines.
 * Applying netmsglog_policy.cpp's own walk to the station buffers - base
 * 0x0210794C + station*0x344 + 0xA3C, 4-byte entries, low byte a delta that
 * ends the list at zero, second byte the type - across all 6868 frames gives a
 * complete timeline of what was sent.
 *
 * Cross-referenced against when the arena HUD's slot column exists in the same
 * capture's oam_sub, which brackets each match exactly:
 *
 *     HUD column appears   f015220        f034750        f053720
 *     HUD column gone      f033150        f052670        f064470
 *
 * TYPE 0x0A IS WHERE IT SHOULD BE. It occurs once, at f052800 - after the
 * second match's HUD vanished at f052670 and before the third's appeared at
 * f053720 - and on station 0, the host. That is the between-rounds window and
 * the host is who sends it, which is exactly what this id is documented to
 * mean. It was inferred from a table and handler bodies; it now has traffic
 * behind it.
 *
 * TYPES 57 AND 62 ARRIVE TOGETHER, ON BOTH STATIONS, on precisely the frames
 * the HUD column appears - f015220 and f053720. Whatever they are, they
 * accompany a round actually beginning, as distinct from 0x0A which asks for
 * one ~920 frames earlier with the statue select and map chooser in between.
 * They are NOT named here, because "arrives when the round starts"
 * is not the same as knowing what a handler does with it.
 *
 * THE THIRD MATCH START AT f034750 HAS NO RECORDED MESSAGE, and that is a
 * limit of the capture rather than a counter-example: frames are 10 apart and
 * a slot list that fills and drains inside ten frames is invisible to this
 * sampling. Every message above was seen in exactly one sampled frame.
 *
 * The rest of the timeline, recorded without interpretation: type 20 on both
 * stations at f014510 (before any match), type 3 at f015160, type 23 at
 * f031320 / f043370 / f062930, type 53 at f049030 / f050830 / f060830 /
 * f062630, and a cluster of 18, 10, 9 at f052630 / f052800 / f053620 between
 * the second and third matches. */
constexpr uint8_t kLsw2MsgRoundStart = 0x0Au;   /* {u16 seed, u8 selector}  */
constexpr uint8_t kLsw2MsgQuit = 0x0Bu;
constexpr uint8_t kLsw2MsgContinue = 0x0Du;

/* The arena-flow selector message 0x0A carries. 3 is the only value
 * that reaches the hunter statue select; everything else non-zero goes to the
 * map/gamemode/time chooser.
 *
 * THE TWO ARE A CHAIN, NOT A CHOICE. The game's own between-round
 * menu at `0x0209CB94` branches on `[this+0x8C]` and sends 1 from one row and 3
 * from another, and state `0x36` - the chooser those two names - broadcasts its
 * OWN 3 at `0x0209E6D8` once the host confirms. So:
 *
 *     1  ->  chooser  ->  (host confirms)  ->  3  ->  statue select  ->  match
 *     3  ->  statue select  ->  match, with whatever settings already stood
 *
 * The fork sends 1, because the fork's round flow shows both screens in that
 * order.
 *
 * `kLsw2ArenaWithSelect` therefore has NO CALLER, deliberately, and is kept
 * rather than deleted: it is half of a protocol description that would be a
 * bare `3` the moment the non-Hunters suppression needs it, and the suppression
 * is located (that same `0x0209E6D8`) rather than speculative. The orphan sweep
 * checks functions, not constants, so this note is the only thing that stops it
 * reading as an oversight. */
constexpr uint8_t kLsw2ArenaWithSelect = 3u;
constexpr uint8_t kLsw2ArenaNoSelect = 1u;

/* Deliver `msg` with `len` bytes of payload to the local guest, as though it had
 * arrived from `station`.
 *
 * Returns false - and writes nothing - when guest state is unreachable, when the
 * payload does not fit a packet slot, or when the receive pump is not running.
 * A caller that gets false has not sent anything. */
bool lsw2_netmsg_deliver(int station, uint8_t msg, const uint8_t* payload,
                         int len);

/* The session's round start, as the cartridge does it.
 *
 * `select_hunters` chose between the two arena flows the host settings
 * distinguish: setting Character to anything besides Hunters skips the
 * character select phase. The seed rides along because the handler feeds it
 * straight to the game's own RNG seeding - which answers the RNG-seed concern
 * through the cartridge rather than around it. */
/* THE ROSTER IS NOT A PARAMETER, and used to be. `select_hunters` decided
 * nothing: the payload's selector is always kLsw2ArenaNoSelect, because
 * the game's own chooser broadcasts the statue select's 3 itself. The flag
 * fed one warning line, and when that line turned out to be stale the
 * parameter had no reader left. selectskip.cpp owns the skip. */
bool lsw2_netmsg_start_round(uint16_t seed);

#endif /* LSW2_NETMSG_H */
