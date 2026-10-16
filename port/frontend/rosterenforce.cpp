/* The guest-touching half of the host settings menu's Roster rule.
 *
 * SEPARATE SO roster.cpp STAYS TESTABLE, the same split lasthunter_observe.cpp
 * and customization_policy.cpp already make.
 *
 * WHAT IT DOES, AND WHY IT IS A LOOP RATHER THAN AN EVENT.
 *
 * The random rosters reassign the character every time the player spawns, and
 * the obvious implementation is to detect a spawn and act on it. This does not do
 * that, because "how does this cartridge signal a respawn" is not established -
 * and a rule that fires on a signal nobody has read is a rule that silently
 * does nothing.
 *
 * What IS established is the state the rule describes: while a roster rule is
 * in force, the player IS the character it names. So this enforces the
 * invariant continuously - if the player is not who the rule says, ask the game
 * to change them - and a respawn is then just one of the moments the invariant
 * is briefly violated. It corrects itself without ever needing to know how
 * spawning works.
 *
 * The spawn counter is the ACTOR POINTER changing, not a frame count. A new
 * actor is a new appearance by definition, and the pointer is already read by
 * lsw2_cheats_party_actor(). That keeps the counter tied to something the
 * simulation does rather than to wall time.
 *
 * WHAT THIS DOES NOT YET DO, stated plainly because the previous pass claimed
 * more than it built.
 *
 * roster.h argues the choice is deterministic so every machine agrees, and the
 * FORMULA is - same station, same spawn, same character, anywhere. But this
 * file only ever assigns the LOCAL player: `lsw2_cheats_party_actor(0)` is this
 * machine's own party slot, and the spawn counter it keeps counts this
 * machine's own actor changes. Nothing here tells the other machines that
 * station 2 just became Bossk, and nothing here counts station 2's spawns -
 * a remote player's actor is not locally observable that way.
 *
 * So today: each machine holds ITSELF to the rule, correctly and consistently.
 * Agreement across machines needs the character id to travel in the lockstep
 * share alongside the block - which is where remote avatars (Phase C) need it
 * anyway, and is the right place for it rather than a second channel.
 *
 * The determinism is not wasted work: it means that when the id does travel,
 * every machine can VERIFY what it is told rather than trust it, and a
 * disagreement is a bug report rather than a silent divergence.
 */

#include "roster.h"
#include "vwh.h"

#include "cheats.h"
#include "forksession.h"
#include "lasthunter.h"
#include "levels.h"
#include "scorescreen.h"
#include "ui.h"

#include <cstdio>

namespace {

/* The last actor we saw in slot 0, so a new one can be recognised. */
uint32_t g_last_actor = 0;

/* How many distinct actors this station has had. The random rosters index on
 * it, so it must count appearances rather than frames. */
int g_spawn = 0;

/* What the rule last asked for, so the log says something only when the answer
 * changes. */
int g_requested = -1;

/* Whether a round is under way, latched across the level-unloaded gap between
 * the match ending and the score screen appearing. See lsw2_roster_match_state.
 */
bool g_match_latched = false;

}  // namespace

void lsw2_roster_match_state() {
    /* THE GUEST READ forksession.cpp MUST NOT DO. Being on an arena map is
     * observable now; owning the ROUND is Phase D. Telling the session the
     * plainer fact activates the behaviours already written for InMatch -
     * the heartbeat and beacon keep announcing so a match stays joinable, and
     * the status line stops claiming the host is merely "Hosting". */
    if (!ui_fork_build()) return;
    /* THE SCORE SCREEN IS STILL THE MATCH - MEASURED.
     *
     * This was `set_in_match(lsw2_cheats_in_arena_match())` alone, and that
     * predicate is `level == 4 || level == 5`. The game unloads the level
     * BEFORE it enters the score screen, so the flag dropped one step early:
     * the log shows "match ended, back to the lobby" and the lobby countdown
     * going COUNTING *above* the "[state] entered 44" line that is the score
     * screen appearing.
     *
     * The required order is: the score screen stays up for its 15 s timer,
     * and only AFTER that does everyone go back to the lobby, where the lobby
     * timer restarts.
     * Dropping the flag early inverts it, and lets a 30-second lobby
     * timer run out while the score screen is still up - which is what a run
     * measured, with Game Settings re-opening on a console still at level 0.
     *
     * cheats.cpp already says this predicate is not the question anyone wants:
     * "This says the player is ON an arena map, which is not the same as a
     * round being in progress ... the score screen [happens] elsewhere". So the
     * missing half is added here rather than by widening a function whose whole
     * comment is about staying an honest approximation. */
    /* AND A LATCH, BECAUSE THE TRANSITION PASSES THROUGH A GAP WHERE NEITHER
     * TEST HOLDS - MEASURED.
     *
     * The OR above was correct in principle and still dropped the flag one
     * frame early, because the game unloads the level BEFORE it builds the
     * score state. That frame reads, in the log, exactly like this:
     *
     *     JOIN GAP (HOST) state -1 level 0      <- no level AND no state
     *     match ended, back to the lobby
     *     lobby_seconds 30 -> COUNTING
     *     [state] entered 44                    <- the score screen, next frame
     *
     * So a predicate built only from "where am I now" can always be caught in
     * the doorway. The real question is not where the player is but whether
     * the round is over, and the required flow says when that is: everyone is
     * back in the lobby and the lobby timer has restarted. The match
     * ends when the players are BACK IN THE LOBBY.
     *
     * So the flag latches on entering an arena or the score screen, and clears
     * only on arriving at the arena lobby - or when the session ends, which
     * set_in_match already ignores but the latch must not outlive. */
    const bool in_arena = lsw2_cheats_in_arena_match();
    const bool on_score = lsw2_score_screen_is_up();

    if (in_arena || on_score) {
        g_match_latched = true;
    } else if (lsw2_cheats_current_level() == kLsw2ArenaLobbyLevel ||
               lsw2_fork_session().state() == Lsw2SessionState::Offline) {
        g_match_latched = false;
    }

    lsw2_fork_session().set_in_match(in_arena || on_score || g_match_latched);
}

void lsw2_roster_publish() {
    /* THE GUEST READ THAT forksession.cpp MUST NOT DO. It runs everywhere the
     * fork does - lobby included - because remote avatars need to know who the
     * others are before a match starts, not only during one. */
    if (!ui_fork_build()) return;
    lsw2_fork_session().publish_character(lsw2_cheats_party_character(0));
}

void lsw2_roster_sync_peers() {
    /* LOCKSTEP (vwh.h lsw2_vwh_lockstep): the game's own netgame carries every player's character; a swap from here lands at a different step on each console. */
    if (lsw2_vwh_lockstep()) return;
    /* MAKE THIS MACHINE'S COPY OF EVERY OTHER STATION MATCH WHAT THEY SAY.
     *
     * The previous correction to this file said the roster rules were local
     * only, and that agreement needed the character on the wire. It is there
     * now - byte 6 of every block - and the game's character-change poller
     * turns out to be per-player: the pending byte is at `+0x92E + player`, one for each of the
     * four. So a machine can ask the game to change ANY station, and what was
     * missing was never the lever, only the knowledge of who to change to.
     *
     * WHAT THIS IS FOR is remote avatars: a joiner must be able to see the
     * other connected players, the host included, and a player the local game thinks
     * is Greedo while their own machine thinks they are Bossk is not seeing
     * them, it is seeing somebody else standing where they are.
     *
     * ONLY WHAT THEY CLAIM ABOUT THEMSELVES. peer_character() is fed from the
     * sender's own station byte, stamped by the transport, so a station can
     * only ever describe itself. This applies that claim locally; it does not
     * let one player dress another. */
    if (!ui_fork_build()) return;
    if (!lsw2_cheats_available()) return;

    const Lsw2ForkSession& session = lsw2_fork_session();
    if (session.state() == Lsw2SessionState::Offline) return;

    for (int station = 0; station < int(kLsw2PartySlots); ++station) {
        if (station == session.local_station()) continue;   /* ours already */

        const int want = session.peer_character(station);
        if (want < 0) continue;                  /* they have not said */

        if (lsw2_cheats_party_character(station) == want) continue;
        lsw2_request_character_for(station, want);
    }
}

void lsw2_roster_enforce() {
    /* LOCKSTEP (vwh.h lsw2_vwh_lockstep): a per-console character request is a lockstep divergence; roster rules need the dataset path. */
    if (lsw2_vwh_lockstep()) return;
    if (!ui_fork_build()) return;

    /* IN A MATCH, AND IN A SESSION. The Roster row is a match rule the host
     * sets; offline it means nothing, and in the lobby the player picks their
     * own hunter through the Free Play tab. Enforcing there would fight the
     * hunters-only Free Play roster, which is the lobby's own character
     * picker. */
    if (!lsw2_cheats_in_arena_match()) return;

    const Lsw2ForkSession& session = lsw2_fork_session();
    if (session.state() == Lsw2SessionState::Offline) return;

    const Lsw2HostSettings& rules = session.settings();
    if (!lsw2_roster_assigns_character(rules.roster)) return;

    const uint32_t actor = lsw2_cheats_party_actor(0);
    if (!actor) return;

    if (actor != g_last_actor) {
        g_last_actor = actor;
        ++g_spawn;
        g_requested = -1;          /* a new actor may need a new answer */
    }

    const int want = lsw2_roster_character(rules, session.local_station(),
                                           g_spawn);
    if (want < 0) return;

    const int now = lsw2_cheats_party_character(0);
    if (now == want) return;

    /* THE GAME'S OWN REQUEST, not a byte written over the actor. A poller in
     * the game turns this into the cartridge's own character change. Writing
     * actor+0x6BB directly would rename an actor the game had already dressed. */
    if (!lsw2_request_character(want)) return;

    if (g_requested != want) {
        g_requested = want;
        std::fprintf(stderr,
                     "[fork] roster rule %d: station %d spawn %d should be "
                     "character %d (was %d)\n",
                     int(rules.roster), session.local_station(), g_spawn, want,
                     now);
    }
}

void lsw2_roster_enforce_reset() {
    g_last_actor = 0;
    g_spawn = 0;
    g_requested = -1;
}
