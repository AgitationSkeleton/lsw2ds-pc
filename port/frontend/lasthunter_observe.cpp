/* The half of lasthunter.h that reads guest memory.
 *
 * SEPARATE SO THE REST CAN BE TESTED, which is the same split
 * customization_policy.cpp made for the same reason. Everything in
 * lasthunter.cpp - the roster, the membership test, the load path - is pure and
 * links into a test on its own; this file drags in the whole guest bus through
 * cheats.h, and there is nothing in it a test could assert that reading the two
 * lines does not already tell you.
 */

#include "lasthunter.h"
#include "vwh.h"
#include "levels.h"
#include "statelog.h"

#include "cheats.h"
#include "roster.h"
#include "forksession.h"
#include "freeplaypick.h"   /* lsw2_freeplaypick_pregame_active */
#include "gearhop.h"        /* lsw2_gearhop_request_send - message 7 */
#include "netcount.h"       /* kLsw2NetCountObject, kLsw2NetLocalStation */
#include "whsession.h"      /* kLsw2ArenaLobbyState */

#include "state.h"

#include <cstdint>
#include <cstdio>

namespace {

/* levels.h id 9. The two arena maps come from lsw2_cheats_in_arena_match(). */
/* The direct boot's destination is levels.h's kLsw2ArenaLobbyLevel, EMITTED from the
 * generated table by name. This file used to declare it again. */

/* The level root, as cheats.cpp already knows it. The poller at 0x02091604
 * reads its actor table at `+player*4 + 0x4C`, which is byte-for-byte what
 * lsw2_cheats_party_actor() reads - so the manager it is handed and the root
 * this port already uses are the same object. That is what makes the pending
 * fields addressable without a new pointer chain. */
/* THE LEVEL ROOT, from the GENERATED levels.h as kLsw2LevelRootAddr - one
 * copy, and the right one: extract_level_table.py regenerates it from the
 * ROM, so a re-extraction updates every consumer instead of leaving
 * hand-written copies behind. */

/* WHAT THE PLAYER WAS WHEN WE LAST LOOKED, so arriving in the lobby can be
 * distinguished from being in it. -1 means "we have not looked yet", which is
 * also the state after a level change. */
int g_last_level = -1;

/* One request per arrival. The poller clears the byte itself when it acts, so
 * re-writing it every frame would keep re-arming a change the game had already
 * made - and the ten-frame countdown exists precisely to stop that. */
bool g_requested_here = false;

/* The session's join-in-progress rule, and the one fact it needs:
 *
 *   A match already under way may be joined, and a player who arrives as a
 *   round begins is given, automatically, the hunter they had in the lobby.
 *
 * The distinction that matters is whether THIS MACHINE picked a hunter for this
 * round. A player who sat through the statue select chose one and the fork must
 * not overrule them; a player who arrived on an arena map without ever seeing
 * that screen never chose, and the rule is to give them their lobby hunter.
 *
 * The select is state `0x29`, readable every frame from
 * `[[0x02102900] + 0x04] + 0x04`. So "did we pick" is "did we see 0x29 since we
 * were last in the lobby" - no new protocol, no message, and it is true for both
 * ways of skipping the select: joining late, and a Roster setting that turns it
 * off. */
/* The state manager, its two +4 hops and the state ids are ALL statelog.h's
 * now - kLsw2StateManager, kLsw2StateCurrent, kLsw2StateId and the ids. This
 * file used to declare the offsets and the ids again under its own names,
 * directly beneath a comment saying the manager was the one copy. */

constexpr uint32_t kRamLow = 0x02000000u;
constexpr uint32_t kRamHigh = 0x02400000u;

bool g_saw_select = false;      /* since the last lobby arrival */
bool g_applied_on_arena = false;/* once per arena arrival       */

/* THE SESSION LOBBY'S HUNTER, restored by the game's own switch message.
 *
 * Joining a lobby used to replace the player's character with Greedo rather
 * than keeping the one they had before choosing the lobby. Measured
 * (conn_join7, host 34, joiners 7 and 13): every session lobby reload builds
 * the players from the level's data, Greedo, and nothing put anybody back -
 * the host went 34 -> 20 and the first joiner 7 -> 20, on every console alike.
 * The offline lobby builds slot 0 as the last hunter (the spawn-as override),
 * but a session's slots are its stations and none of them is rewritten there.
 *
 * So each console asks for ITS OWN station's hunter the way a player does it by
 * hand: message 7, the Free Play grid's switch (sender 0x02068A18, payload
 * u16 character then the sender's station byte; arm 0x020B1524 switches that
 * station's actor through FUN_020917c8). Every console handles it at the same
 * exchange, so the lockstep holds. Once per lobby load, after the actor has
 * settled, and only once the game's station byte is this console's session
 * AID with the multi-view bit up - mid-join the byte still reads the host's 0.
 *
 * AND THE RECORD WAITS FOR IT. The last hunter used to be read from party slot
 * 0, which on a joiner is the HOST - so a joiner saved the host's character,
 * or the level's Greedo, as its own. In a session it is read from this
 * console's station, and not until the restore is done: the actor the reload
 * built is the level's Greedo, not a choice. A Free Play switch made after
 * that is recorded as the new hunter, as offline. */
constexpr uint8_t kMsgSwitchCharacter = 7u;
constexpr int kRestoreSettle = 30;     /* hudportrait.cpp's settling window */
constexpr int kRestoreDeadline = 180;
enum class Restore { Waiting, Sent, Done };
Restore g_restore = Restore::Done;
uint32_t g_restore_actor = 0;
int g_restore_frames = 0;
int g_restore_want = -1;
/* Slots the lobby builder built during a session since the tick last looked
 * (lsw2_last_hunter_note_build) - a rebuild the tick samples no level change
 * for, measured: Start Lobby's own reload of the host's lobby. */
unsigned g_built_mask = 0u;

/* IN A SESSION: the fork's session is up (hosting, joining, in a match), or
 * the lockstep is. Known before the lockstep is - the join's first frames
 * and the host's seating reload happen while the fork session is up and the
 * datasets are not yet flowing. */
bool in_session() {
    return lsw2_fork_session().state() != Lsw2SessionState::Offline ||
           lsw2_vwh_lockstep();
}

bool ram_ptr(uint32_t v) { return v >= kRamLow && v < kRamHigh; }

/* ONE DEREFERENCE TOO MANY, CONFIRMED BY MEASUREMENT. The manager
 * constant is a BASE ADDRESS; dereferencing it first and then adding the offset
 * landed on `00000004`, which fails ram_ptr(), so this returned -1 on every
 * call. See coopjoin.cpp for the side-by-side reading. */
int running_state_id() {
    const uint32_t st = lsw2_state_current_object();
    if (!st) return -1;
    return int(bus_device_read32(0, st + kLsw2StateId) & 0xFFu);
}

/* Read and write one byte through the 32-bit bus helpers.
 *
 * THE BUS OFFERS NO BYTE WRITE - state.h has write32 and write16 only - so a
 * byte store is a read-modify-write on the containing word. Safe here because
 * the port touches guest memory between frames, never alongside the guest; but
 * it is worth naming, because the three neighbouring bytes are the OTHER THREE
 * PLAYERS' pending characters and a careless word store would cancel their
 * requests. */
uint8_t read_byte(uint32_t addr) {
    const uint32_t word = bus_device_read32(0, addr & ~3u);
    return uint8_t((word >> ((addr & 3u) * 8u)) & 0xFFu);
}

void write_byte(uint32_t addr, uint8_t value) {
    const uint32_t aligned = addr & ~3u;
    const unsigned shift = (addr & 3u) * 8u;
    uint32_t word = bus_device_read32(0, aligned);
    word &= ~(0xFFu << shift);
    word |= uint32_t(value) << shift;
    bus_device_write32(0, aligned, word);
}

}  // namespace

bool lsw2_request_character_for(int player, int character_id) {
    if (player < 0 || player >= int(kLsw2PartySlots)) return false;
    if (character_id < 0) return false;
    if (!lsw2_cheats_available()) return false;

    const int kPlayer = player;

    const int now = lsw2_cheats_party_character(kPlayer);
    if (now < 0) return false;                  /* no actor yet */
    if (now == character_id) return false;      /* already them */

    const uint32_t pending = kLsw2LevelRootAddr + kLsw2PendingCharacter + kPlayer;
    if (int8_t(read_byte(pending)) >= 0) {
        /* Somebody - the select screen, or a previous request - is mid-change.
         * Overwriting it would make the game act on ours instead of theirs, and
         * theirs is the player's.
         *
         * `>= 0`, NOT `!= -1`, because that is the poller's own test:
         *
         *     02091640  ldrsb r2,[r0,#0x2E]
         *     02091644  cmp   r2,#0
         *     02091648  blt   <skip>          ANY negative is idle
         *
         * -1 is merely the value the game writes when it clears the field; the
         * CONDITION is the sign. Testing for the value rather than the condition
         * is the mistake that once made the RNG reseed inert,
         * and it is the same mistake here in a milder form - a field left at,
         * say, -2 would look busy to the port and idle to the game, and the
         * port would silently stop issuing requests. */
        return false;
    }

    write_byte(pending, uint8_t(character_id));
    return true;
}

bool lsw2_request_character(int character_id) {
    /* SLOT 0 IS THE LOCAL PLAYER. The fork enforces a party of one offline, and
     * in a session the guest seats this machine's own station there. */
    return lsw2_request_character_for(0, character_id);
}

void lsw2_last_hunter_note_build(int slot) {
    if (slot >= 0 && slot < int(kLsw2PartySlots)) g_built_mask |= 1u << slot;
}

bool lsw2_last_hunter_apply() {
    const int now = lsw2_cheats_party_character(0);
    if (!lsw2_request_character(lsw2_last_hunter())) return false;
    std::fprintf(stderr, "[fork] asked the game to make the player their "
                         "last hunter, id %d (was %d)\n",
                 lsw2_last_hunter(), now);
    return true;
}

void lsw2_last_hunter_tick() {
    /* WHERE THE ANSWER MEANS SOMETHING. The arena lobby is where the player
     * picks a hunter and the two arena maps are where they play as one; a
     * character id read anywhere else is somebody from a story level, and
     * remembering it would boot the fork in as Chewbacca. */
    const int level = lsw2_cheats_current_level();

    /* ARRIVAL IS A LEVEL CHANGE, not a frame count. Tracking the previous level
     * means the request fires once when the player enters the lobby and again
     * if they come back to it after a match - which is what is wanted,
     * since a match can leave them as somebody else. */
    if (level != g_last_level) {
        g_last_level = level;
        g_requested_here = false;
        /* A lobby load in a session rebuilds this console's actor from the
         * level's data: its hunter has to be asked for again. */
        if (level == kLsw2ArenaLobbyLevel && in_session()) {
            g_restore = Restore::Waiting;
            g_restore_actor = 0;
        }
        /* A NEW ARENA ARRIVAL GETS ONE CHANCE to apply the lobby hunter, and
         * returning to the lobby forgets that we ever saw the select - the next
         * round is a fresh question. */
        g_applied_on_arena = false;
        if (level == kLsw2ArenaLobbyLevel) g_saw_select = false;
    }

    /* WATCHED EVERY FRAME, not only in the two places below, because the statue
     * select runs between the lobby and the arena and the level id during it is
     * whichever of the two the game has loaded. Missing it would make a player
     * who DID pick look like one who never had the chance. */
    if (running_state_id() == kLsw2StateSelect) g_saw_select = true;

    if (level != kLsw2ArenaLobbyLevel && !lsw2_cheats_in_arena_match()) return;

    /* The join-in-progress rule: a player who arrives as a round begins is
     * given, automatically, the hunter they had in the lobby.
     *
     * ON AN ARENA MAP, HAVING NEVER SEEN THE SELECT. That is precisely the
     * player the sentence describes - and it is also the player whose host set
     * Roster to something that skips the select, who wants the same thing for
     * the same reason: they never got to choose here, so the choice they made
     * in the lobby stands.
     *
     * NOT WHEN THE ROSTER RULE ASSIGNS ONE. Random Hunter and Random All pick a
     * character every spawn (rosterenforce.cpp), and applying the lobby hunter
     * over that would fight the host's own setting on the frame after it acted. */
    /* NOT IN LOCKSTEP (vwh.h lsw2_vwh_lockstep): a character request from one
     * console's tick is a divergence, and party slot 0 is the HOST's player on a
     * joiner - the netgame's slots are AIDs. */
    if (lsw2_cheats_in_arena_match() && !g_saw_select && !g_applied_on_arena &&
        !lsw2_vwh_lockstep()) {
        g_applied_on_arena = true;
        const Lsw2ForkSession& session = lsw2_fork_session();
        if (session.state() != Lsw2SessionState::Offline &&
            !lsw2_roster_assigns_character(session.settings().roster)) {
            std::fprintf(stderr,
                         "[fork] joined without a hunter select - applying the "
                         "lobby hunter\n");
            lsw2_last_hunter_apply();
        }
    }

    /* The direct boot's last-played-hunter half applies in the lobby only. An
     * arena match is where a hunter is PLAYED, and changing the player's
     * character during one would be the fork overruling the match. */
    if (level == kLsw2ArenaLobbyLevel && !g_requested_here &&
        !lsw2_vwh_lockstep()) {
        g_requested_here = true;
        lsw2_last_hunter_apply();
    }

    /* WHICH SLOT IS THIS PLAYER. Offline, and before a session exists, slot 0;
     * in a lockstep session, this console's station - and only once the game
     * agrees it is (the station byte equals the session AID, multi-view up). */
    int mine = 0;
    if (in_session()) {
        /* NOTHING IS READ OR RECORDED IN A SESSION UNTIL THIS CONSOLE'S SEAT IS
         * REAL: measured (conn_join7b), recording while the session was up but
         * the lockstep not yet, every console saved slot 0 - the host's Greedo
         * - as its own hunter. */
        const uint8_t station =
            read_byte(kLsw2NetCountObject + kLsw2NetLocalStation);
        if (!lsw2_vwh_lockstep() || station >= kLsw2PartySlots ||
            int(station) != lsw2_vwh_local_aid() || !lsw2_vwh_multiview())
            return;
        mine = int(station);

        /* THIS CONSOLE'S ACTOR WAS REBUILT FROM THE LEVEL'S DATA. */
        if (g_built_mask & (1u << mine)) {
            g_built_mask = 0u;
            g_restore = Restore::Waiting;
            g_restore_actor = 0;
        }

        /* THE RESTORE, in the lobby only, never during ALL's pre-game pick
         * (the grid is that pick's) and never in a match. */
        if (g_restore != Restore::Done) {
            if (level != kLsw2ArenaLobbyLevel ||
                lsw2_state_current() != kLsw2ArenaLobbyState ||
                lsw2_freeplaypick_pregame_active())
                return;
            const uint32_t actor = lsw2_cheats_party_actor(mine);
            if (!ram_ptr(actor)) return;
            if (g_restore == Restore::Waiting) {
                if (actor != g_restore_actor) {
                    g_restore_actor = actor;
                    g_restore_frames = kRestoreSettle;
                    return;
                }
                if (g_restore_frames > 0) { --g_restore_frames; return; }
                const int now = lsw2_cheats_party_character(mine);
                g_restore_want = lsw2_last_hunter();
                if (g_restore_want < 0 || g_restore_want == int(kLsw2CharacterNone) ||
                    now == g_restore_want) {
                    g_restore = Restore::Done;
                } else if (lsw2_gearhop_request_send(
                               kMsgSwitchCharacter,
                               uint32_t(g_restore_want) | (uint32_t(mine) << 16),
                               0u)) {
                    g_restore = Restore::Sent;
                    g_restore_frames = kRestoreDeadline;
                    std::fprintf(stderr,
                                 "[fork] session lobby: station %d was rebuilt as "
                                 "%d - sending the game's own switch (message 7) "
                                 "back to this player's hunter %d\n", mine, now,
                                 g_restore_want);
                }
                return;
            }
            /* SENT: done when the switch has reached this console too. */
            if (lsw2_cheats_party_character(mine) == g_restore_want) {
                g_restore = Restore::Done;
                std::fprintf(stderr, "[fork] session lobby: station %d is hunter "
                             "%d again\n", mine, g_restore_want);
            } else if (--g_restore_frames <= 0) {
                g_restore = Restore::Done;
                std::fprintf(stderr, "[fork] session lobby: the switch of station "
                             "%d to %d did not arrive within %d frames - left as "
                             "is\n", mine, g_restore_want, kRestoreDeadline);
            }
            return;
        }
    }

    const int who = lsw2_cheats_party_character(mine);
    if (who < 0) return;                 /* between levels, or no party yet */

    if (!lsw2_set_last_hunter(who)) return;

    /* Once per change, not once per frame. The player switching hunters is a
     * thing worth seeing in a log; sixty lines a second of the same id is not.
     *
     * THE PAGE COMES FROM THE PAGE TABLE, not from the roster index. A first
     * version computed it as `index < 8 ? 1 : 2`, which is what an earlier
     * reading inferred and what reading 0x020EDDB2 disproved: both orders hold the same
     * ten hunters and they are not the same order, so that line named the wrong
     * page for six of the ten. */
    int page = -1, slot = -1;
    lsw2_hunter_page_slot(who, &page, &slot);
    std::fprintf(stderr, "[fork] last-played hunter is now id %d (roster entry "
                         "%d, select page %d slot %d)\n",
                 who, lsw2_hunter_index(who), page, slot);
}
