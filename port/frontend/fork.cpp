/* See fork.h. */

#include "fork.h"
#include "forkpopup.h"

#include "arenahud.h"
#include "diagnostics.h"
#include "customization.h"
#include "sessiondress.h"
#include "sessionroster.h"
#include "lasthunter.h"
#include "characterpick.h"
#include "freeplaypick.h"
#include "sessionpause.h"
#include "musicpick.h"
#include "rngsync.h"
#include "cartroster.h"
#include "coopjoin.h"
#include "dsname.h"
#include "countboot.h"
#include "netcount.h"
#include "netmsglog.h"
#include "lobbyrespawn.h"
#include "arrowhide.h"
#include "markerhook.h"
#include "partapply.h"
#include "partslot.h"
#include "freezewatch.h"
#include "gearapply.h"
#include "headfix.h"
#include "menubackdrop.h"
#include "menuhost.h"
#include "hudportrait.h"
#include "partyportrait.h"
#include "slotcolour.h"
#include "pauseredraw.h"
#include "levelhud.h"
#include "ringcolour.h"
#include "statelog.h"
#include "viewmask.h"
#include "netmsg.h"
#include "peerparts.h"
#include "scorescreen.h"
#include "selectskip.h"
#include "selectwidget.h"
#include "roster.h"
#include "forksession.h"

#include "cheats.h"
#include "enhanced.h"
#include "extrasapply.h"
#include "guestcall.h"
#include "levels.h"
#include "gearhop.h"
#include "probehooks.h"
#include "roundfollow.h"
#include "viewmanager.h"
#include "whsession.h"
#include "vwh.h"
#include "lobbypin.h"
#include "ui.h"

#include "state.h"
#include "scheduler.h"   /* scheduler_cpu_terminal_halted - leaving a session once the game halts */

#include <cstdio>
/* std::getenv, for the LSW2_TEST_CONSOLE instrument below. It compiled without
 * this through a transitive include, which is luck rather than a contract. */
#include <cstdlib>

namespace {

/* The fork's boot destination, the Cantina arena lobby. Confirmed by observation, not derived from asset
 * names: port/frontend/levels.h records id 9 as "Cantina Arena Lobby" with
 * source "confirmed", and the derivation-based names around it are explicitly
 * marked otherwise. Warping there and looking is how it was confirmed, which is
 * the same check this code performs every time it runs. */
/* That destination is levels.h's kLsw2ArenaLobbyLevel, EMITTED from the
 * generated table by name. This file used to declare it again. */

/* HOW LONG TO WAIT BEFORE GIVING UP AND SHOWING THE GAME.
 *
 * The warp cannot fire until the game has built a level state - the request
 * copies two of its arguments from the live one, and firing into a
 * half-constructed state machine is what crashed the guest during the scripted
 * level sweep. So this waits, and waiting needs a bound.
 *
 * Thirty seconds at 60fps. Raised from ten once the boot stopped being a wait
 * and became a drive: the auto-press below has to sit through splash
 * animations and a save-slot screen, each of which takes as long as it takes.
 * The cost of waiting too long is a black screen; the cost of giving up too
 * early is the fork silently behaving like the baseline. If it expires, the
 * port says so and shows the game - a fork that drops the player at the title
 * screen with an explanation is better than one that hides a failure. */
constexpr int kBootFrameBudget = 1800;

enum class Phase {
    Waiting,     /* boot is running; watching for a level state to exist   */
    Requested,   /* the cold warp is in flight                             */
    Reload,      /* re-entering the level the ordinary way, to clear the
                    splash's leftover 2D setup                              */
    Done,        /* the player is where they belong, or we gave up         */
};

Phase g_phase = Phase::Waiting;
int g_frames = 0;
const char* g_status = "Starting...";

/* THE FORK PRESSES NOTHING. Two earlier versions did - one marched the player
 * through the title, the save select and the Cantina; a later one pressed Start
 * on arrival to skip the lobby's opening camera. The second was worse than it
 * looked: by the time the touch screen was fixed, that Start was landing in
 * gameplay and opening the PAUSE MENU, which is what the lobby's touch screen
 * then showed.
 *
 * Kept as a zero so the key-mask expression in frontend.cpp does not need a
 * special case, and so the seam is still there if a real need for it appears. */
uint16_t g_boot_keys = 0;

/* How long can_warp() must hold before the fork acts on it. One second at
 * 60fps - long enough for a level's assets to finish arriving, short enough
 * that the player does not notice a pause on a black screen. */
constexpr int kSettleFrames = 60;

/* How long to spend trying to skip the lobby's opening cutscene. Two seconds:
 * long enough for several Start presses to land, short enough that if there is
 * no cutscene to skip the player barely waits. */
constexpr int kCutsceneFrames = 120;

/* How long the state machine must be alive and idle before the cold warp fires.
 * Three seconds. Measured against the symptom rather than picked: at ~1 second
 * the boot splash's 2D state was still installed and bled into the lobby. */
/* The boot splash's state class, measured: slot 0 reads kind 6 from power-on
 * and changes as soon as the splash is dismissed. */
constexpr int kSplashKind = 6;

/* How long to let the splash's teardown finish before asking for a level. */
constexpr int kPostSplashFrames = 20;
int g_settled = 0;

/* Set once the warp has been requested, so the arrival check knows what it is
 * waiting for rather than accepting whatever level happens to appear. */
bool g_reached = false;

/* How long to leave the escape backstop alone after it fires. Two seconds at
 * 60fps - comfortably longer than a level load, so the check never sees its own
 * warp still in flight and asks for another. */
constexpr int kEscapeCooldownFrames = 120;
int g_escape_cooldown = 0;

/* The last non-negative value lsw2_state_current() returned. A level load
 * reports -1 for both the level AND the state, so a rule that must know whether
 * a match is running needs the state that was live before the gap opened. */
int g_last_real_state = -1;

/* A client lost its host while in the match and is owed the lobby; served once
 * the wireless has come down (see the tick). */
bool g_stranded = false;

}  // namespace

void lsw2_fork_enforce_party_of_one();
void lsw2_fork_remove_companions(uint16_t members);
void lsw2_fork_report_view_index();
static void lsw2_match_signal_tick();

/* THE ROUND STARTS, on this machine and on every other one.
 *
 * ONE FUNCTION FOR BOTH TRIGGERS, which the design's two ways in already
 * claimed and did not have. With Lobby Time set to Off the host starts the
 * match manually with Start Game; otherwise the lobby timer starts it - so a
 * round begins either when the lobby timer expires or when the host picks
 * Start Game, and until now
 * only the second of those warped anybody. A lobby that ran its clock out sent
 * message 0x0A and stayed in the lobby.
 */
/* ROUND-STARTED LATCH. The pre-round setup states - the game-rules chooser
 * (54) and the hunter statue/character select (41) - run on the SAME Cantina level as
 * the lobby (48), so the fork's level-based "in match" stays false through them. A
 * transient host bounce during setup (54->41->-1->48, from the joiner's WH churn) then
 * lets the lobby timer's fired-latch reset and RE-FIRE begin_round, which re-requests
 * the rules chooser = the host is asked for the game rules twice, and Start Game into
 * a dying WH session is the crash. This latches on the first round start and refuses
 * re-entry until the round genuinely reaches the arena (state 42, cleared in the tick)
 * or the session ends - so a bounce cannot produce a second rules prompt. */
bool g_round_begun = false;

void lsw2_fork_begin_round(Lsw2ForkSession& s) {
    if (g_round_begun) {
        static int said = 0;
        if (said < 3) {
            ++said;
            std::fprintf(stderr,
                         "[fork] begin_round: a round is already starting - ignoring "
                         "this re-trigger so the host is not prompted for the game rules "
                         "again\n");
        }
        return;
    }
    g_round_begun = true;

    /* THE CARTRIDGE'S OWN ROUTE FIRST, WHEN IT IS RUNNING.
     *
     * The players have to be connected and present in the cartridge's real
     * arena multiplayer system, not just warped to the map. Everything below
     * this line IS the warp. With the real wireless session up, the host's
     * Start Game should instead bring up the cartridge's session and open the
     * cartridge's own Game Settings, which is what lsw2_wh_console_start does.
     *
     * It returns true when it has taken the round start, and the warp is then
     * skipped entirely. When the real-WH route is off it returns false and
     * everything below runs exactly as before - the baseline, the tests, and
     * the fork's own lobby all keep the behaviour they had. */
    if (lsw2_wh_console_start()) return;

    /* THE CARTRIDGE'S OWN ROUND START, for the local guest. Message
     * 0x0A with `{seed, selector}` enters the arena flow, seeds the game's RNG
     * through 0x02062A40 and picks the screen. The seed is the SESSION'S, which
     * every machine in the lobby already agreed, so the round is
     * deterministic across them rather than merely seeded on each.
     *
     * IT REACHES NOBODY ELSE, despite the name. It writes message 0x0A into the
     * LOCAL guest's receive buffer and sends nothing over the network - a
     * comment here used to say "the message still goes out, for the OTHER
     * machines" and that was simply false. What crosses the wire is the
     * announcement below. */
    lsw2_netmsg_start_round(uint16_t(s.match_seed()));

    /* THE HOST'S DECISION. Play begins on whichever arena the host picked,
     * Jabba's Tower or Mos Eisley Streets - so the destination is the Map row of the host settings menu, and a client
     * uses the host's value because it adopted the host's settings. */
    int level = s.settings().map;
    if (level != kLsw2ArenaJabbaLevel && level != kLsw2ArenaMosEisleyLevel) {
        /* A hand-edited settings.json or an older peer's record. The clamp
         * catches this too; this is the second line of it, because the value
         * about to be handed to the level loader is the one that took two days
         * to find when it was wrong. */
        level = kLsw2ArenaJabbaLevel;
    }

    /* AND THIS MACHINE GOES TO THE MAP.
     *
     * THE WHOLE BLOCKER WAS AN ARGUMENT BEING PASSED HERE. Entering state 54
     * does not crash - it starts a LEVEL TRANSITION, and the level it
     * transitions to comes from the state-request record's third word.
     * lsw2_cheats_warp writes that same record ([kind][arg_a][LEVEL][arg_c] at
     * manager + 0x48), and the port was passing 0, so the game loaded level 0 -
     * Endor - and fell over running a map it had never set up. */
    if (!lsw2_cheats_warp(level)) {
        std::fprintf(stderr, "[fork] round start: the warp to level %d was "
                             "refused - the level state was not ready\n", level);
        return;
    }

    /* AND EVERY CLIENT FOLLOWS. The announcement rides the settings payload
     * clients already adopt; roundfollow.cpp watches it and warps to match.
     * Only after the local warp is accepted, so a refusal here does not send
     * the rest of the lobby somewhere this machine is not. */
    s.announce_round(level);
}

namespace {

/* JSON-safe text for a name: quotes and backslashes escaped, controls dropped. */
std::string json_text(const char* s) {
    std::string out;
    for (; s && *s; ++s) {
        const unsigned char c = static_cast<unsigned char>(*s);
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c >= 0x20) out += char(c);
    }
    return out;
}

/* THE MASTER SERVER'S VIEW OF THIS LOBBY: "lobby" or "match", and the rules a
 * game runs with - so its log can say when a game started and with what
 * settings. Refreshed twice a second; only the host's
 * heartbeat carries it.
 *
 * THE MAP AND MODE ARE THE CARTRIDGE'S OWN: Game Settings (state 54) writes
 * them into the rules object, *[0x0209DEC8] - the mode at +0x34 (0 Kills -
 * Deaths, 1 Capture, 2 Deathmatch, 3 Deathmatch with points, 4 Points -
 * Deaths) and the level at +0x40 (4 Jabba's Tower, 5 Mos Eisley Streets). */
void master_status_tick() {
    static int countdown = 0;
    if (--countdown > 0) return;
    countdown = 30;

    Lsw2ForkSession& session = lsw2_fork_session();
    if (!session.is_host()) return;

    const int screen = lsw2_state_current();
    const int level = lsw2_cheats_current_level();
    const bool match = (screen == kLsw2MatchState || screen == kLsw2PreGameState) &&
                       lsw2_level_is_arena(level);

    const Lsw2HostSettings& s = session.settings();
    static const char* const kRoster[] = {"Hunters", "All", "Random Hunter",
                                          "Random All", "One"};
    static const char* const kCustom[] = {"Off", "Hats", "All"};
    static const char* const kLobbyMusic[] = {"Hunter", "Cantina", "Random"};
    static const char* const kLobbyTime[] = {"Off", "30s", "60s", "300s"};
    static const char* const kModes[] = {"Kills - Deaths", "Capture the Flag",
                                         "Deathmatch", "Deathmatch (points)",
                                         "Points - Deaths"};

    std::string cfg = "{";
    cfg += "\"roster\":\"" + std::string(kRoster[int(s.roster) % 5]) + "\"";
    if (s.roster == Lsw2Roster::One) {
        char name[48];
        cfg += ",\"character\":\"" +
               json_text(lsw2_character_name(s.character, name, sizeof name)) + "\"";
    }
    cfg += ",\"customization\":\"" + std::string(kCustom[int(s.customization) % 3]) + "\"";
    cfg += ",\"controls\":\"" + std::string(s.controls == Lsw2HostControls::Tank
                                                ? "Tank" : "Enhanced") + "\"";
    cfg += ",\"music\":\"" + std::string(s.music == Lsw2HostMusic::Random
                                             ? "Random" : "Default") + "\"";
    cfg += ",\"lobby_music\":\"" + std::string(kLobbyMusic[int(s.lobby_music) % 3]) + "\"";
    cfg += ",\"lobby_time\":\"" + std::string(kLobbyTime[int(s.lobby_time) % 4]) + "\"";
    cfg += ",\"extras\":[";
    bool first = true;
    for (int i = 0; i < kLsw2ExtrasCount; ++i) {
        if (!s.extras[i]) continue;
        if (!first) cfg += ",";
        first = false;
        cfg += "\"" + json_text(lsw2_extra_name(i)) + "\"";
    }
    cfg += "]";
    if (match) {
        cfg += ",\"map\":\"" + std::string(level == kLsw2ArenaJabbaLevel
                                               ? "Jabba's Tower"
                                               : "Mos Eisley Streets") + "\"";
        const uint32_t holder = bus_device_read32(0, 0x0209DEC8u);
        const uint32_t rules = (holder >= 0x02000000u && holder < 0x02400000u)
                                   ? bus_device_read32(0, holder) : 0u;
        if (rules >= 0x02000000u && rules < 0x02400000u) {
            const uint32_t mode = bus_device_read32(0, rules + 0x34u);
            if (mode < 5u) cfg += ",\"mode\":\"" + std::string(kModes[mode]) + "\"";
        }
    }
    cfg += "}";
    session.set_master_status(match ? "match" : "lobby", cfg);
}

}  // namespace

void lsw2_fork_tick() {
    master_status_tick();
    /* FREE PLAY CUSTOMIZATION. Runs every frame regardless of the boot phase:
     * customization must carry across level and arena transitions, not only
     * inside the lobby, and it is a no-op in the baseline. */
    lsw2_customization_tick();
    /* ...and IN A SHARED SESSION, through the game's own customize messages -
     * the tick above is gated off there (sessiondress.h). */
    lsw2_sessiondress_tick();
    /* The host's Roster setting, applied in a session - see sessionroster.h. */
    lsw2_sessionroster_tick();


    /* THE SESSION PUMPS EVERY FRAME, host or client, menu open or not.
     *
     * Sockets do not wait for a convenient moment: a joiner knocking while the
     * host is reading a settings menu still has to be accepted, and a peer that
     * drops mid-menu still has to leave the kick list. Putting this before the
     * boot-phase early exit is deliberate for the same reason. */
    /* BUT NOT FROM A CONSOLE WHOSE GAME HAS STOPPED: a
     * terminal halt of the ARM9 (a dispatch miss) leaves the window
     * and this tick running, so the session would go on announcing a lobby
     * nobody can play in and holding its peers' sockets open. It leaves
     * instead, once, without touching the guest - see abandon_session. */
    {
        /* TEST ONLY - LSW2_TEST_HALT=<seconds>: that many seconds into a match
         * (state 42), stop this console's CPUs the way a crash leaves them, so
         * a drive can watch the session leave and the peers carry on. */
        static const char* halt_spec = std::getenv("LSW2_TEST_HALT");
        static int halt_frames = 0;
        static bool halted_once = false;
        if (halt_spec && !halted_once && lsw2_state_current() == 42 &&
            ++halt_frames >= std::atoi(halt_spec) * 60) {
            halted_once = true;
            std::fprintf(stderr, "[fork] TEST: halting this console's CPUs, as "
                                 "a crash would (LSW2_TEST_HALT)\n");
            scheduler_terminal_halt_all("test halt");
        }
    }
    /* EITHER CPU: a halted ARM7 takes the wireless and the sound with it, which
     * is as dead as a game gets (a Capture the Flag crash ended this way). */
    if (lsw2_fork_session().state() != Lsw2SessionState::Offline &&
        (scheduler_cpu_terminal_halted(0) || scheduler_cpu_terminal_halted(1)))
        lsw2_fork_session().abandon_session(
            scheduler_cpu_terminal_halted(0) ? scheduler_cpu_halt_reason(0)
                                             : scheduler_cpu_halt_reason(1));
    lsw2_fork_session().tick();
    /* The host-level WH's guest side: a joiner becomes a member when
     * its START arrives, the browse row stays alive. No-op when it is off
     * (LSW2_VWH=0, or the baseline - see lsw2_vwh_enabled). */
    lsw2_vwh_tick();

    /* The Multiplayer tab's player name, handed to the cartridge as the DS
     * nickname it already reads for its beacon, its browse row and its peer
     * slots - BOTH builds (the baseline's online co-op has the same tab).
     * Before the fork-only return, and before the wireless layer can start,
     * which is why it sits this high. Idempotent after the first frame. See
     * dsname.h. */
    lsw2_dsname_tick();

    /* The baseline's co-op lobby list (master server and LAN lobbies shown in
     * the cartridge's own list), and it must run BEFORE the fork-only early
     * return below because it is the BASELINE's feature: the cartridge's lobby
     * list draws whatever is in its host table, and this is what puts the
     * directory's and the LAN's lobbies there. Refreshed every frame because
     * the browse phase ages each row's TTL. See coopjoin.h. */
    lsw2_coopjoin_publish();




    /* THE FREE PLAY PORTRAIT FIX (a Free Play character change updates the
     * party portrait), in BOTH builds and therefore above the fork-only return.
     * The Free Play screen exists in both and the portrait bug is the
     * cartridge's. Idempotent - four byte reads a frame until the party
     * actually changes. See partyportrait.h. */
    lsw2_partyportrait_tick();

    /* THE PORTRAIT FIX'S OTHER HALF - the TOP screen's character icon.
     *
     * A separate module because it is a separate widget with a separate bug:
     * the call above maintains the touch screen's Free Play select page and
     * provably never touches the top screen. This one
     * rebuilds the HUD portrait the game only ever builds once.
     *
     * Idempotent - three byte reads a frame until the player's character
     * actually changes. See hudportrait.h. */
    lsw2_hudportrait_tick();

    /* THE SESSION'S PLAYER ROSTER, fed from the cartridge's own seat record. On
     * the preferred route - a peer admitted through the cartridge's
     * wireless - the fork's session learns no peers from its socket, so the
     * join popup, the kick list and the directory count are all empty. This reads the cartridge's peer-name record and feeds the
     * session, fork build only, doing nothing on a pure-TCP session. See
     * cartroster.h. */
    lsw2_cartroster_tick();

    /* THE HOP'S RETIRE TICK, above the fork-only return, because the two modules above now ask the hop in both builds and an arm
     * nobody claims must be retired in the baseline too. It used to sit with
     * the fork-only probes below; there it would have left a stale arm - and
     * the wild branch its own comment warns of - standing in lsw2.exe. */
    lsw2_gearhop_tick();

    /* THE PORTRAIT RING IS DRAWN IN THE PLAYER'S SLOT COLOUR, BOTH BUILDS - the
     * fork's hunter lobby and the baseline's
     * co-op netgame want the same ring colour, because it is the same HUD.
     * Silent offline and for the host. See slotcolour.h. */
    lsw2_slotcolour_tick();
    /* And the top-screen ring itself, which is a 3D quad, not a sprite. */
    lsw2_ringcolour_tick();

    /* The freeze measurement, silent until the console is interacted with and
     * silent for good after one report. See freezewatch.h. */
    lsw2_freezewatch_tick();

    /* WHICH STATE THE GAME IS IN, reported when it changes, in BOTH builds -
     * which is why it sits above the fork-only return rather than with the
     * other probes below it. The game has 58 states and names none: an
     * arm gives a constructor, not a label. The two states the blocked clauses
     * need are reached by walking to the Cantina console and by choosing the
     * pause wireless row, and the fork has removed both - its boot warp skips
     * the Cantina, and its pause menu hides the row. The baseline is where this has to watch. See
     * statelog.h. */
    lsw2_statelog_tick();

    /* LSW2_TEST_SWITCH_CHARACTER=<id> - a TEST HOOK to change the party
     * character without the touch screen (the Free Play select is a composite
     * keys can't reach). It calls lsw2_request_character_for, which is
     * baseline-safe (it needs only lsw2_cheats_available()), so this sits ABOVE
     * the fork-only return: it lets the BASELINE's portrait fix / customization be driven
     * and verified, which is otherwise impossible (the fork's other test hooks
     * are fork-only, so raw input is the only baseline driver). Env-gated, so an
     * ordinary run of EITHER build does nothing. Moved here from the
     * fork-only section below. LSW2_TEST_SWITCH_AFTER delays it (default 600
     * ticks) so a drive sees a settled BEFORE and a clean AFTER. */
    if (const char* who = std::getenv("LSW2_TEST_SWITCH_CHARACTER")) {
        static int delay = [] {
            if (const char* d = std::getenv("LSW2_TEST_SWITCH_AFTER")) {
                const int v = std::atoi(d);
                if (v > 0) return v;
            }
            return 600;
        }();
        static bool fired = false;
        if (!fired && delay > 0) --delay;
        if (!fired && delay <= 0) {
            fired = true;
            const int id = std::atoi(who);
            const bool asked = lsw2_request_character_for(0, id);
            std::fprintf(stderr,
                         "[fork] LSW2_TEST_SWITCH_CHARACTER: asked for character "
                         "%d on station 0 - %s (works in both builds)\n",
                         id, asked ? "ACCEPTED" : "REFUSED");
        }
    }

    /* LSW2_MASTER_HOST[:/LSW2_MASTER_PORT] - point the master client at a specific
     * directory at boot, once, without opening the imgui MP tab (which is the only
     * other place session.configure runs). Lets a launcher or a test use a local
     * self-host; an ordinary run leaves the compiled default. */
    {
        static bool s_master_env_done = false;
        if (!s_master_env_done) {
            s_master_env_done = true;
            if (const char* mh = std::getenv("LSW2_MASTER_HOST")) {
                if (*mh) {
                    const char* mp = std::getenv("LSW2_MASTER_PORT");
                    const int mport = (mp && *mp) ? std::atoi(mp) : 28553;
                    lsw2_fork_session().configure(nullptr, mh, mport, 0);
                    std::fprintf(stderr,
                                 "[fork] master directory set from env: %s:%d\n",
                                 mh, mport);
                }
            }
        }
    }

    /* LSW2_TEST_HOST_WIRELESS=1 - a TEST HOOK to trigger the baseline co-op host
     * without the finicky pause-menu nav. It calls the same session.start_lobby()
     * that coopjoin's "Host Wireless" (0x020B10A8) override calls in both builds,
     * which is pure host-side TCP setup (no guest context), so it is safe above
     * the fork-only return and lets the BASELINE's co-op host (a lobby broadcast to
     * the master server) be driven directly.
     * One shot on a delay (LSW2_TEST_HOST_AFTER, default 900 ticks) so a level
     * is reached first. Env-gated: an ordinary run of either build does nothing. */
    if (std::getenv("LSW2_TEST_HOST_WIRELESS")) {
        static int hdelay = [] {
            if (const char* d = std::getenv("LSW2_TEST_HOST_AFTER")) {
                const int v = std::atoi(d);
                if (v > 0) return v;
            }
            return 900;
        }();
        static bool hfired = false;
        if (!hfired && hdelay > 0) --hdelay;
        if (!hfired && hdelay <= 0) {
            hfired = true;
            lsw2_fork_session().start_lobby();
            std::fprintf(stderr,
                         "[fork] LSW2_TEST_HOST_WIRELESS: start_lobby() called "
                         "(works in both builds)\n");
        }
    }

    /* LSW2_TEST_JOIN_WIRELESS=host:port - the JOIN counterpart, calling the same
     * join_address() coopjoin's connect override uses (host-side TCP client, no
     * guest). Above the fork-only return so a BASELINE joiner can be driven, to
     * verify the baseline's co-op join over the fork transport. Env-gated. */
    if (const char* hp = std::getenv("LSW2_TEST_JOIN_WIRELESS")) {
        static int jdelay = [] {
            if (const char* d = std::getenv("LSW2_TEST_JOIN_AFTER")) {
                const int v = std::atoi(d);
                if (v > 0) return v;
            }
            return 900;
        }();
        static bool jfired = false;
        if (!jfired && jdelay > 0) --jdelay;
        if (!jfired && jdelay <= 0) {
            jfired = true;
            std::string spec(hp);
            const size_t colon = spec.rfind(':');
            const std::string host = (colon == std::string::npos) ? spec : spec.substr(0, colon);
            const int port = (colon == std::string::npos) ? 0 : std::atoi(spec.c_str() + colon + 1);
            const bool ok = lsw2_fork_session().join_address(host.c_str(), port);
            std::fprintf(stderr,
                         "[fork] LSW2_TEST_JOIN_WIRELESS: join_address(%s,%d) -> %s\n",
                         host.c_str(), port, ok ? "OK" : "FAILED");
            /* Bring the joiner's view manager up (console_join runs the hop that
             * builds [0x021092F4]) - the viewmask-driven count derivation is gated
             * on a non-null manager. console_join now SKIPS its
             * DS-WM browse-list request by default (that parks the joiner in state
             * 34, a detour off the PC-networking path); the browse list
             * is opened only under LSW2_JOIN_WM=1. So the joiner builds the view
             * manager and stays in the arena lobby (48), where the mask published
             * by lsw2_viewmask_publish (fed by the host now counted as a peer)
             * derives the player count to 2. */
            if (ok) lsw2_wh_console_join();
        }
    }

    /* LSW2_TEST_JOIN_RELAY=host:port - the internet JOIN over the real WH
     * carried by the relay, with NO Path A. Mirrors LSW2_TEST_JOIN_WIRELESS but
     * calls join_relay (relay + WH-console-join, no Lsw2TcpTransport, no
     * take_joined bounce). join_relay already drives the WH-console-join, so this
     * does not call it again. Env-gated. */
    if (const char* hp = std::getenv("LSW2_TEST_JOIN_RELAY")) {
        static int jdelay = [] {
            if (const char* d = std::getenv("LSW2_TEST_JOIN_AFTER")) {
                const int v = std::atoi(d);
                if (v > 0) return v;
            }
            return 300;
        }();
        static bool jfired = false;
        if (!jfired && jdelay > 0) --jdelay;
        if (!jfired && jdelay <= 0) {
            jfired = true;
            std::string spec(hp);
            const size_t colon = spec.rfind(':');
            const std::string host = (colon == std::string::npos) ? spec : spec.substr(0, colon);
            const int port = (colon == std::string::npos) ? 0 : std::atoi(spec.c_str() + colon + 1);
            const bool ok = lsw2_fork_session().join_relay(host.c_str(), port);
            std::fprintf(stderr,
                         "[fork] LSW2_TEST_JOIN_RELAY: join_relay(%s,%d) -> %s\n",
                         host.c_str(), port, ok ? "OK" : "FAILED");
        }
    }

    /* LSW2_TEST_JOIN_CODE=XXXX - the FULL in-game internet join: resolve the room
     * code via the master server, then (under LSW2_RELAY_AUTO) join_relay to the
     * resolved peer. Exercises register->resolve->join_relay->match end
     * to end, the way "Join by Room Code" does. Env-gated. */
    if (const char* code = std::getenv("LSW2_TEST_JOIN_CODE")) {
        static int jdelay = [] {
            if (const char* d = std::getenv("LSW2_TEST_JOIN_AFTER")) {
                const int v = std::atoi(d);
                if (v > 0) return v;
            }
            return 300;
        }();
        static bool jfired = false;
        if (!jfired && jdelay > 0) --jdelay;
        if (!jfired && jdelay <= 0) {
            jfired = true;
            const bool ok = lsw2_fork_session().join_code(code);
            std::fprintf(stderr,
                         "[fork] LSW2_TEST_JOIN_CODE: join_code(%s) -> %s\n",
                         code, ok ? "OK" : "FAILED");
        }
    }

    /* LSW2_TEST_REFRESH_LOBBIES=1 - browse the directory + LAN once a second and
     * report the row count, to test the baseline's lobby discovery (a joiner finding a host in
     * the list) without the finicky menu nav. Above the fork-only return so the
     * baseline browses. Env-gated. */
    if (std::getenv("LSW2_TEST_REFRESH_LOBBIES")) {
        static int rframes = 0;
        if (++rframes >= 60) {
            rframes = 0;
            lsw2_fork_session().refresh_lobbies(/*lan_first=*/true);
            std::fprintf(stderr, "[fork] LSW2_TEST_REFRESH_LOBBIES: %d lobby row(s)\n",
                         lsw2_fork_session().list_row_count());
        }
    }

    /* LSW2_TEST_ROSTER=<n> - pin the host roster (0 Hunters, 1 All, 2 Random
     * Hunter, 3 Random ALL, 4 One) so a drive can reach the match with a chosen
     * roster and watch selectskip. Pinned every frame so the settings menu /
     * rosterenforce cannot drift it. Env-gated; the host Roster is a fork feature. */
    if (const char* rv = std::getenv("LSW2_TEST_ROSTER")) {
        const int n = std::atoi(rv);
        if (n >= 0 && n <= 4) {
            lsw2_fork_session().settings().roster = Lsw2Roster(n);
            static int said = -1;
            if (said != n) { said = n;
                std::fprintf(stderr, "[fork] LSW2_TEST_ROSTER: host roster pinned to %d\n", n); }
        }
    }

    /* LSW2_TEST_MUSIC=1 - pin the host music to RANDOM so a drive can reach the
     * arena and watch the round-start music pick ("Music: RANDOM ... picked slot
     * N"). Env-gated; a fork host setting. */
    if (std::getenv("LSW2_TEST_MUSIC")) {
        lsw2_fork_session().settings().music = Lsw2HostMusic::Random;
        static bool said = false;
        if (!said) { said = true;
            std::fprintf(stderr, "[fork] LSW2_TEST_MUSIC: host music pinned to RANDOM\n"); }
    }

    if (!ui_fork_build()) return;

    /* THE HOST'S LOBBY TIME: the between-match countdown. The session owns the rules; the
     * clock reading is here because forksession.cpp is deliberately guest-free,
     * the same split rosterenforce.cpp makes.
     *
     * IT IS THE GAME'S SECONDS COUNTER, not a frame count - the same word the
     * statue select's 30s timer and the score screen's 15s timer read, so all
     * three lengths mean the same thing to a player.
     *
     * WHAT EXPIRY DOES IS PHASE D's, and saying so in the log is better than a
     * silent no-op: when the timer runs out the host should get the cartridge's
     * usual map / game mode / time choice, and which
     * state that is has not been read. The countdown itself is complete and correct; only its
     * destination is missing. */
    if (lsw2_cheats_available()) {
        const uint32_t now = bus_device_read32(0, 0x02109258u);

        /* TELL THE SESSION WHAT THE CARTRIDGE HAS SEATED, because it cannot ask.
         *
         * Measured: two consoles joined through the
         * cartridge's own wireless system and the host reported `players 2`, and
         * the lobby countdown never started - so the round never began, and the
         * rule that the timer begins once a second player joins did not happen
         * at all.
         *
         * The timer was reading `player_count()`, which counts the FORK's own
         * TCP peers. A player admitted by the cartridge is not one of those, so
         * the count stayed 1 and the countdown's precondition never held. The
         * same wrong authority was fixed elsewhere earlier; this is another place
         * it lived.
         *
         * HERE rather than inside the session, because forksession.cpp is
         * deliberately guest-free - the same split the clock read above makes,
         * and what lets the host unit test link it without a ROM. */
        const Lsw2ViewManagerState vm_now = lsw2_viewmanager_state();
        lsw2_fork_session().set_cartridge_players(vm_now.players);

        /* AND SAY SO WHEN THE TWO DISAGREE. The countdown not starting was
         * completely silent - no line said "players 1" from the authority that
         * decided it while another line said "players 2" a few rows above. Once
         * per change, so a long lobby does not fill the log. */
        {
            /* EVERY INPUT THE COUNTDOWN DEPENDS ON, not just the count.
             *
             * The count was the first thing found wrong and printing it was
             * what found it - but `wants_countdown` also needs `hosting`,
             * `!in_match` and a non-zero `lobby_seconds`, and with only the
             * count on screen a false value in any of those looks exactly like
             * the bug that was already fixed. Printing one of four inputs is
             * how a second silent cause hides behind the first. */
            static int said_fork = -1, said_cart = -1, said_host = -1;
            static int said_match = -1, said_secs = -1;
            const Lsw2ForkSession& s = lsw2_fork_session();
            const int f = s.player_count();
            const int host = int(s.is_host());
            const int inmatch = int(s.state() == Lsw2SessionState::InMatch);
            const int secs = s.settings().lobby_seconds();
            if (f != said_fork || vm_now.players != said_cart ||
                host != said_host || inmatch != said_match ||
                secs != said_secs) {
                said_fork = f;
                said_cart = vm_now.players;
                said_host = host;
                said_match = inmatch;
                said_secs = secs;
                std::fprintf(stderr,
                             "[fork] lobby countdown inputs: players %d (fork "
                             "%d, cartridge %d), hosting %d, in_match %d, "
                             "lobby_seconds %d -> %s\n",
                             s.players_present(), f, vm_now.players, host,
                             inmatch, secs,
                             (host && !inmatch && secs > 0 &&
                              s.players_present() >= 2)
                                 ? "COUNTING"
                                 : "not counting");
            }
        }

        /* AND WHAT THE COUNTDOWN IS ACTUALLY COUNTING.
         *
         * The inputs above said COUNTING and the round still never started, so
         * the next silent thing is the arithmetic: `now` is read from the
         * game's own counter at 0x02109258, and nothing has ever printed it
         * beside the `started` it is being compared against. A countdown that
         * cannot say how far along it is cannot be told apart from one that is
         * not running. Once a second while it is counting, and it stops on its
         * own when `remaining` reaches zero. */
        {
            static int beat = 0;
            const int left = lsw2_fork_session().lobby_remaining(now);
            if (left >= 0 && ++beat >= 60) {
                beat = 0;
                std::fprintf(stderr,
                             "[fork] lobby countdown: now %u, %d to go\n",
                             unsigned(now), left);
            }
        }

        /* CLEAR THE ROUND-STARTED LATCH once the round genuinely reaches
         * the arena (state 42 = 0x2a) - from there the in-match latch guards re-entry
         * so the next round's timer fires normally - or the session has ended. A
         * transient bounce back to the lobby that never reached 42 leaves it latched,
         * which is exactly what stops the second rules prompt. */
        if (g_round_begun &&
            (lsw2_state_current() == 42 ||
             lsw2_fork_session().state() == Lsw2SessionState::Offline)) {
            g_round_begun = false;
        }

        /* NOT WHILE A JOINER IS BEING SEATED (vwh.h lsw2_vwh_seating): the
         * lobby reloads for every join, and a round started under it would
         * leave that player out or break the reload. The countdown keeps
         * running on the game's clock, so the round starts as soon as the
         * seat is done.
         *
         * A HOLD, NOT A SKIPPED TICK. This was
         * `!lsw2_vwh_seating() && lobby_tick(now)`, which stopped the timer
         * seeing anything while a joiner waited - and one who connects
         * mid-round waits through the whole match. The timer never saw the
         * match that re-arms it, its fired latch outlived the round, and the
         * next lobby counted "0 to go" without ever starting round two
         * (conn_allpick_jip). */
        if (lsw2_fork_session().lobby_tick(now, lsw2_vwh_seating())) {
            /* AND THIS IS WHERE THE ROUND STARTS. Message 0x0A with
             * `{seed, selector}` is the cartridge's own round start - it enters
             * the arena flow, seeds the game's RNG through `0x02062A40`, and
             * picks the screen. The selector follows the Roster rule: the
             * arena flow's character select runs only when Roster is Hunters
             * and is skipped for every other value.
             *
             * THE SEED IS THE SESSION'S, not a fresh draw. Every machine in the
             * lobby already agreed one, and handing that to the
             * game's own seeding is what makes the round deterministic across
             * them rather than merely seeded on each. */
            Lsw2ForkSession& s = lsw2_fork_session();
            /* THROUGH THE PREDICATE, not `roster == Hunters`.
             *
             * Both sites open-coded that comparison while `roster.cpp` held
             * `lsw2_roster_skips_select()` - the tested statement of this exact
             * rule, with five checks on it and no caller. Three ways to
             * ask one question, two of them a negation apart, is how a future
             * edit flips one and not the others; it is the shape this project
             * deleted `lsw2_menuhost_open_p()` for and deduplicated
             * `lsw2_cheats_in_arena_match()` onto.
             *
             * Note it is NOT the same question as
             * `lsw2_roster_assigns_character()`, which is a caller away in
             * rosterenforce.cpp: roster ALL skips the select and assigns
             * nothing. Collapsing the two would break exactly that case. */
            lsw2_fork_begin_round(s);
        }

        /* THE OTHER WAY IN: with Lobby Time set to Off, the host starts the
         * match manually through the console entity's Start Game option. Same
         * round start, same
         * selector - one implementation, two triggers, so they cannot drift. */
        if (!lsw2_vwh_seating() && lsw2_fork_session().take_start_request()) {
            /* SAY THAT THE PRESS WAS PICKED UP. The other end of the same blind
             * spot: when Start Game produced no visible effect there was no way
             * to tell whether this trigger had fired at all, or whether the
             * round start had run and stopped somewhere further in. */
            std::fprintf(stderr,
                         "[fork] Start Game: the console's round-start request "
                         "was taken - running the round start now\n");
            lsw2_fork_begin_round(lsw2_fork_session());
        }
    }

    /* AND A CLIENT FOLLOWS THE HOST INTO THE ARENA. Outside the
     * `lsw2_cheats_available()` block above on purpose - that block is the
     * HOST's lobby machinery, and this is the half that runs on the machines
     * that did not press the button. See roundfollow.h for what was measured.
     *
     * It is also how a match in progress can be joined: a joiner adopts the
     * host's settings on arrival, and the announced level is in them. */
    lsw2_roundfollow_tick();

    /* THE BOOKKEEPING THE CARTRIDGE WOULD HAVE BUILT, and why this is a
     * read and not a build.
     *
     * The view manager was measured null and the receive pump idle in the
     * arena lobby; the same two numbers were measured in the MATCH too, so it
     * was never a property of the lobby - coopjoin.cpp
     * replaces the function whose tail allocates the object.
     *
     * REPRODUCING THAT TAIL FROM HERE WAS TRIED AND IT CORRUPTED THE GUEST.
     * The allocation works; the constructor does not return, and the reason is
     * in the disassembly at the address it stops on:
     *
     *     020D4BF0  ldr r0,[r12,#0x0]      ; r12 = 0x021174F8
     *     020D4BF4  cmp r0,#0x1
     *     020D4BF8  beq 0x020d4bf0         ; spin while the flag is 1
     *
     * a busy-wait on a flag something asynchronous clears. Between frames
     * nothing does, so the call is preempted at that backward branch every
     * time, and retrying it left the host on "[bus] ARM9 unmapped read"
     * followed by a dispatch miss. See viewmanager.h; the build is opt-in
     * behind LSW2_BUILD_VIEWMANAGER and is not on this path.
     *
     * What runs here is the READ, which costs three loads and is what makes
     * the next attempt answerable with evidence. */
    lsw2_viewmanager_report();

    /* AND THE LAYER THAT WOULD MAKE IT NON-NULL. The cartridge's
     * multiplayer is Nintendo's WH, its state table is at 0x020FFD20, and in
     * the fork it reads STOP because coopjoin.cpp replaces the session start.
     * The bring-up asks for the real one to run; it happens on the guest
     * thread, and it is ON unless LSW2_REAL_WH=0. See
     * whsession.h for why the default changed. */
    lsw2_wh_report();
    lsw2_wh_console_start_tick();
    /* Auto-answer the host's join prompt (state 35). A joining
     * peer parks the host on the cartridge's "let them join? Yes/No" and nobody
     * was answering it - so the peer was never really admitted (signal red, no
     * one visible) while the countdown ran on the WM's provisional count, and a
     * loading chain driven without admission could stall the host to black.
     * Answering YES completes the connection. See whsession.cpp. */
    lsw2_wh_answer_join_prompt_tick();
    /* And none of the stock join screens is SHOWN (34, 35, 39): their text
     * layer is left out of the touch screen, so a fork join looks like the
     * loading screen it becomes. Presentation only. See whsession.cpp. */
    lsw2_wh_hide_stock_join_screens_tick();
    /* FOUR PLAYERS: the host closes its beacon to new children after
     * the first one though maxEntry is 7. Re-opening entry is now driven
     * from netgame_tick_probe (dispatch_overrides.cpp) instead of here, because a WM
     * FIFO command issued from this between-frames tick dropped the first child; the
     * override runs within guest execution where it is non-disruptive. */
    /* THE JOINER'S HALF: once the child is SCANNING with its browse
     * list open, the scan finds the host but nothing selects it, so the connect
     * never fires. This presses A on the highlighted host row for the joiner, the
     * mirror of the host's auto-answer above. See whsession.cpp. */
    lsw2_wh_pick_browse_host_tick();
    /* THE MATCH-START BLOCKER. The fork's lobby is a level, and loading it arms
     * the cartridge's cross-player consistency check without re-seeding the
     * shared generator - so the check fails the moment the match starts and
     * both consoles are sent to Communication Error. This keeps it disarmed
     * while the lobby is up, on whichever console is sitting in it. It has to
     * run per frame rather than at the arming site because the arm happens
     * during a level load, when no state object exists to hook. */
    lsw2_net_check_disarm_prematch();
    /* Retire a session-end hop whose return never arrives - which is
     * the joiner's case, measured. It has to live here rather than in the hook
     * that armed it, because that hook stops being called. */
    lsw2_wh_session_end_tick();
    /* Which addresses actually run every frame. A
     * measurement, off unless LSW2_PROBE_FRAMEPCS=1. See probehooks.h. */
    lsw2_probe_frame_pcs();
    /* The gear-hop retire tick used to be here; it now runs
     * above the fork-only return, with the portrait modules that feed it. */
    lsw2_wh_probe_chooser();
    lsw2_wh_probe_select_wireless();
    lsw2_wh_local_index_experiment();
    lsw2_wh_nodetable_trace();
    lsw2_wh_join_gap_report();
    lsw2_wh_reopen_chooser_when_joined();   /* off unless LSW2_PROBE_CHOOSER=1 */
    lsw2_wh_probe_dump_trace();

    /* Every host call goes through lsw2_guest_ready, and the
     * ARM9-active requirement it gained refuses more
     * often than what it replaced. See guestcall.h. */
    lsw2_guest_ready_report();

    /* SEAT CONNECTED PEERS before party-of-one, so the peer's actor exists in
     * its slot when party-of-one decides which slots are companions. The lobby
     * builds its actor list once before any peer, so this re-runs the game's own
     * slot spawn pass from the (peer-populated) container when a peer connects -
     * the remote-avatar seat. Free until a peer is seated. */
    lsw2_lobby_seat_peers_tick();

    /* PARTY OF ONE IS ENFORCED FOR THE WHOLE SESSION, not just during boot, so
     * it runs BEFORE the phase machine's early exit. The companion respawns on
     * every level transition and the fork moves between the lobby and the arena
     * constantly; a one-shot fix on arrival would hold until the first match.
     *
     * IN LOCKSTEP IT RUNS FROM THE EXCHANGE INSTEAD (vwh.cpp), at a dataset
     * boundary, so every console removes a companion at the same simulation
     * step - from here the removal would land at a different step on each. */
    if (!lsw2_vwh_lockstep()) lsw2_fork_enforce_party_of_one();

    /* The Enhanced controls' measurement. Cheap, and it answers the last open
     * question about the movement mode the first time a match is reachable. */
    lsw2_fork_report_view_index();

    /* The boot warp spawns the player as the Bounty Hunter they last played,
     * or as Greedo; this is the last-played half. Watches who the player
     * is while they are somewhere that answer means something, and remembers
     * it across runs. Runs after the party fix-up so it reads the seat the fork
     * actually leaves the player in. */
    lsw2_last_hunter_tick();

    /* The host's Roster rule. Held continuously rather than fired on a spawn
     * signal nobody has read - see rosterenforce.cpp. */
    /* One play session answers six questions. Cheap when off. */
    lsw2_diag_tick();

    /* LSW2_TEST_SWITCH_CHARACTER=<id> - CHANGE CHARACTER WITHOUT THE TOUCH SCREEN.
     *
     * WHY THIS EXISTS. Customization has to auto-apply on subsequent Free
     * Play switches and character changes. The half that works was measured -
     * a saved customization loads and is pushed into the game at boot - and
     * the half that matters was left unmeasured, because testing it needs a
     * CHARACTER CHANGE and the Free Play select is a touch UI the port
     * composites, so injected keys never reach it (fork.cpp's own note on
     * LSW2_TEST_MENU_PICK).
     *
     * So this asks for the change the way the roster rules already do -
     * `lsw2_request_character_for`, the same call rosterenforce uses - and lets
     * customization.cpp narrate what happens. It measures the port's own
     * behaviour on a character change; it does NOT prove the touch screen
     * drives that path, and this comment is here so nobody later reads it as
     * having done so.
     *
    /* (LSW2_TEST_SWITCH_CHARACTER moved above the fork-only return so
     * it works in the baseline too - see the note there.) */

    /* LSW2_TEST_CONSOLE=1 - WALK INTO THE CONSOLE WITHOUT WALKING.
     *
     * A TEST INSTRUMENT, off unless the variable is set, and it fires the
     * game's own path rather than standing in for it: this is the same
     * lsw2_fork_console_interact() the script-command override calls when the
     * cartridge raises its console refusal, so whatever it opens is exactly
     * what a player walking into the hologram gets.
     *
     * WHY IT EARNS ITS PLACE. The console is POSITIONAL, and reaching it over
     * the debug socket means driving a walkaround character across a Cantina
     * blind, a d-pad hold at a time, with a screenshot after each to see where
     * it went. That was slow and succeeded once out
     * of two attempts - and the bug behind it, the arena menu opening
     * INVISIBLE and swallowing all input, is the thing standing between this
     * port and the first bounty hunter match. A defect that takes twenty
     * minutes of luck to reproduce does not get fixed.
     *
     * ONCE, AND ONLY WHEN THE LOBBY IS REALLY UP. Firing it repeatedly would
     * re-open a menu under the player every frame, which is the flicker
     * menuhost.cpp already had to fix; firing it before the level exists would
     * open a menu over a half-built state. */
    if (const char* t = std::getenv("LSW2_TEST_CONSOLE")) {
        /* THE VALUE IS A BUDGET, not a flag. "1" fires once; "4" allows four.
         *
         * ONE WAS NOT ENOUGH and the flow says why: the console is visited
         * TWICE on the way to a match. Offline it opens the Arena menu, where
         * the host picks Host Internet Game and Start Lobby - and only once a
         * lobby is up does the same console offer "Start Game", which is the
         * option that is the manual round start. A one-shot trigger
         * can reach the first of those and never the second. */
        static int budget = -1;
        if (budget < 0) budget = std::atoi(t);

        /* NOT WHILE A MENU IS OPEN, and not immediately after one closes.
         *
         * Re-opening a menu that is already open rebuilds it under the player,
         * which is the flicker menuhost.cpp had to fix; firing the instant a
         * menu closes would re-open the one the player just dismissed. Two
         * seconds is long enough to be a deliberate second visit. */
        static int cooldown = 0;
        if (cooldown > 0) --cooldown;

        /* AND IT SAYS WHY IT IS NOT FIRING. Four conditions, and when the
         * instrument sat silent for five minutes in the arena lobby there was
         * no way to tell which one was false - so it prints the whole gate
         * once a second while it has budget left. An instrument that cannot
         * explain its own inaction is the thing that costs the most time. */
        static int said = 0;
        if (budget > 0 && --said <= 0) {
            said = 60;
            std::fprintf(stderr,
                         "[fork] LSW2_TEST_CONSOLE gate: budget %d cooldown %d "
                         "menu_open %d cheats %d level %d (want %d)\n",
                         budget, cooldown, int(lsw2_menuhost_open_now()),
                         int(lsw2_cheats_available()),
                         lsw2_cheats_current_level(), kLsw2ArenaLobbyLevel);
        }

        if (budget > 0 && cooldown == 0 && !lsw2_menuhost_open_now() &&
            lsw2_cheats_available() &&
            lsw2_cheats_current_level() == kLsw2ArenaLobbyLevel) {
            --budget;
            /* HOW LONG UNTIL THE NEXT VISIT, and why it is settable.
             *
             * Two seconds is right for driving the console twice in a row on
             * one console. It is wrong for the thing being tested now: the
             * intended order is that players GATHER in the lobby and the host
             * starts the match afterwards, so the host's "Start Game" visit has
             * to land AFTER the joiner has actually seated - which takes about
             * twenty seconds of the joiner booting, scanning and connecting.
             * With a fixed two-second gap the host always started the match
             * before anyone had arrived, which is not the intended sequence
             * and not the one worth measuring.
             *
             * LSW2_TEST_CONSOLE_GAP is in frames at 60fps; default unchanged. */
            static const int gap = [] {
                if (const char* g = std::getenv("LSW2_TEST_CONSOLE_GAP")) {
                    const int v = std::atoi(g);
                    if (v > 0) return v;
                }
                return 120;
            }();
            cooldown = gap;
            std::fprintf(stderr,
                         "[fork] LSW2_TEST_CONSOLE: driving the console "
                         "interaction the script would (%d left)\n", budget);
            lsw2_fork_console_interact();
        }
    }

    /* AND A ROW TO PICK ONCE IT IS OPEN, for the same reason.
     *
     * LSW2_TEST_MENU_PICK is a comma-separated list of row indices, one used
     * per menu that opens: "0,0" picks row 0 on the arena menu (Host Internet
     * Game) and row 0 again on the console menu that a second visit brings up
     * (Start Game). That is exactly the intended route to a match - walk to
     * the console, host, walk back, start - and it replaced a boot that
     * dropped each console onto a different screen.
     *
     * WHY THIS EXISTS. The fork's menus are composited by the port, so a socket
     * screenshot shows only the starfield behind them, and reaching one by hand
     * means driving a walkaround character across a Cantina blind - the cost
     * that earned LSW2_TEST_CONSOLE its place. It gets the player to the
     * console without the walk.
     *
     * CORRECTED. This used to add "and they read the
     * port's own input, so injected keys never arrive". **That is false.**
     * frontend.cpp assembles
     *
     *     const uint16_t held = g_keys_host | g_keys_pad | g_keys_stick |
     *                           g_keys_injected | ...
     *     lsw2_menuhost_feed_keys(held);
     *
     * and `g_keys_injected` is pilot.py's channel, so INJECTED KEYS DO REACH
     * THESE MENUS. `pilot press Down` moves the cursor and `pilot press Right`
     * changes a value row, which is how Show Room Code was finally seen: the room-code
     * row toggled OFF -> ON on screen and "NO CODE YET" appeared beneath it.
     *
     * The false half cost real time. It is why every in-game menu measurement
     * went through LSW2_TEST_MENU_PICK, which can ACTIVATE a row but cannot
     * express "move the cursor and change a value" - so anything that needed a
     * cursor move sat unplayed. A comment that shapes how tests are written
     * earns the same scrutiny as the code it sits beside.
     *
     * ONE PICK PER OPENING, tracked by the menu going from closed to open, so a
     * held budget cannot re-activate the same row every frame. */
    if (const char* picks = std::getenv("LSW2_TEST_MENU_PICK")) {
        static const char* cursor = picks;
        static int gap = 0;

        /* PACED, NOT EDGE-TRIGGERED. The first version fired one pick each time
         * a menu went from closed to open, and the route needs three: arena row
         * 0 (Host Internet Game) opens Host Settings, row 10 there (Start
         * Lobby) brings the lobby up, and a second console visit offers row 0
         * (Start Game). Moving between the fork's own screens never closes the
         * menu, so two of those three never fired.
         *
         * A fixed gap is enough for a test driver and cannot stall: each pick
         * happens on its own frame, a screen that ignores it simply consumes an
         * entry, and the list runs out. */
        if (*cursor && lsw2_menuhost_open_now() && --gap <= 0) {
            gap = 45;
            const int row = std::atoi(cursor);
            while (*cursor && *cursor != ',') ++cursor;
            if (*cursor == ',') ++cursor;
            const bool ok = lsw2_fork_session().test_activate_row(row);
            std::fprintf(stderr,
                         "[fork] LSW2_TEST_MENU_PICK: activating row %d - %s\n",
                         row, ok ? "accepted" : "REFUSED (no menu, or no such "
                                                "row)");
        }
    }

    /* Four machines seeded from their own clocks diverge on the
     * first draw. Applied on entering a match, which is a moment they agree on. */
    /* The host's Music setting (Default or Random). Keeps the arena maps' level
     * records naming the shared match seed's pick, so every machine's own
     * arena load plays the same track. See musicpick.h. */
    lsw2_music_tick();
    lsw2_lobby_music_tick();

    lsw2_rng_sync_tick();

    lsw2_roster_match_state();
    lsw2_roster_publish();
    lsw2_roster_sync_peers();
    lsw2_roster_enforce();

    /* THE MATCH-START SIGNAL over the fork transport. The host tells
     * the joiner it has left the lobby for the arena, since the DataSharing
     * lockstep the block/char rides only steps once both are already in the
     * match - a joiner idle at state 48 would otherwise never learn to follow. */
    lsw2_match_signal_tick();

    /* OTHER PLAYERS' CUSTOMIZATIONS, visible on this console. Refused twice for
     * good reason - a remote player's parts had nowhere to go - and buildable
     * now that the per-player record and its reader have been found. See
     * peerparts.h. */
    lsw2_peerparts_tick();

    /* The arena HUD overlay (other players' icons and scores, and the radar,
     * with the touch screen hidden) owns the touch screen's starfield layer while it is up.
     * This is the half that has a side effect; the drawing happens in the
     * present path. Both are needed - the composite alone would mirror the
     * starfield it exists to remove. */
    lsw2_arena_hud_tick();

    /* The co-op arrow, gone from the lobby. A DELIBERATE
     * DIVERGENCE from the ROM rather than a repair - the marker is the
     * cartridge's own level-9 content and retail with two players would show it
     * too. See arrowhide.h for why the lever is the VALUE at +0x0C and not the
     * MASK four bytes later that an earlier attempt cleared to no effect. */
    lsw2_arrowhide_tick();

    /* The fifteen-second score screen before the return to the lobby. Silent outside state 0x2C, so
     * it costs two reads a frame everywhere else. See scorescreen.h for why the
     * timer is the port's to add rather than the ROM's to find. */
    lsw2_score_tick();

    /* THE BYTE THE CARTRIDGE'S WHOLE MESSAGE LAYER TURNS ON.
     *
     * CORRECTED. The sentence that stood here - "the
     * receive pump only runs when the game believes there is more than one
     * player, and mirroring the session's count is what makes the round start
     * reach the game" - was wrong twice over: the pump reads a different word,
     * and every candidate word is popcounted out of the view mask by the game
     * itself. Both calls below are now DIAGNOSTICS. Neither enables anything,
     * and saying so here matters because this comment is what made the feature
     * look finished. */
    lsw2_netcount_tick();

    /* The value all of those are derived from, reported when it changes. What
     * the arena presents here decides whether the networked match's message
     * layer can run at all, and no run has yet reported it. See viewmask.h. */
    lsw2_viewmask_tick();

    /* AND THE ONE WRITE THAT IS THE PORT'S TO MAKE. `[0x0210B320 + 0xE]` is the
     * wireless connection layer's own report of which stations are present, and
     * on a PC that layer is this program - the handlers at `0x020AEAA8` and
     * `0x020AEAE8` that would maintain it have no radio to fire them. Putting
     * the session's real station numbers there lets the game derive its own
     * player count, input count and camera configuration, instead of the port
     * writing three conclusions the game recomputes. See viewmask.h. */
    lsw2_viewmask_publish();

    /* Customization's measurement, not its fix. The six customize
     * slots are read only for character 72, which the fork's Bounty Hunter
     * pages never contain - so the substitution that would work has to go
     * through the gear applier, and that design needs to know what the arena's
     * hunters actually wear. See partslot.h. */
    lsw2_partslot_probe();

    /* CUSTOMIZATION'S FIRST GUEST CALL, armed from the Cheats tab and otherwise silent.
     * This is the safe point for one: the guest is settled between frames, so
     * a call it never made cannot land in the middle of its own. See
     * partapply.h for why this is a probe and not the feature. */
    lsw2_partapply_tick();

    /* THE LOBBY RESPAWN, and the same safe point for the same reason: the guest is
     * settled between frames here. start_lobby() only ARMS the request -
     * called there directly, guestcall refused it outright and the request
     * was abandoned, which is how the first armed run failed to test
     * anything at all. Silent and free unless LSW2_LOBBY_RESPAWN is set. */
    lsw2_lobby_respawn_tick();

    /* NAMING THE CARTRIDGE'S OWN MESSAGES. 70 types are mapped, but not
     * which one is the round start; one match with the types printed answers
     * it, where reading 64 handler bodies would take days. Read-only - the
     * dispatcher has a single caller, so overriding it would mean
     * reimplementing the whole protocol. See netmsglog.h. */
    lsw2_netmsglog_tick();

    /* THE NETWORKED MATCH'S ROUTE 2, behind a button. The game's own two-player
     * bootstrap with the fork's real count - which of the three routes is right
     * depends on whether the derivation runs, and the session's first log line
     * answers that. See countboot.h. */
    lsw2_countboot_tick();

    /* THE ENHANCED CONTROLS' movement half. One byte on the arena player's actor turns
     * the game's own tank movement into camera-relative movement, and the game
     * does the rest - see enhanced.h. Silent outside an arena and in the
     * baseline build, and a no-op if the arena turns out to hand out real view
     * indices already, which is the one number not yet measured. */
    /* THE ENHANCED CONTROLS' right stick, applied after the guest frame so the write
     * lands after 0x0205ED08's. See lsw2_enhanced_camera_step. */
    lsw2_enhanced_camera_step();

    lsw2_enhanced_tick();

    /* THE PAUSE MENU'S HIGHLIGHT is no longer redrawn from here: the rebuild
     * is a gear hop pinned to the pause menu's own update now, because from
     * this tick the guest was usually halted and the rebuild ran seconds late
     * - after the player had opened Settings, which crashed. See
     * pauseredraw.h. */

    /* CUSTOMIZATION, ACTUALLY DRESSING THE ACTOR. The staging writes above are
     * what the customize SCREEN reads; this is the half that puts the parts on
     * the character, using the applier 0x02006220 uses for stock appearances
     * rather than asking the game to re-dress through a mode-gated path that
     * offline never runs. Idempotent - a settled character costs five reads and
     * no guest calls. See gearapply.h. */
    lsw2_gearapply_tick();

    /* THE HELMET-HEAD FIX, per frame. Reveals Boba/Jango's (and the other seven covered
     * heads') head sub-part whenever they wear a non-covering hat - catching the
     * first application, which the apply-time reveal can miss before the head
     * model is registered. Idempotent and a no-op for anyone else. */
    lsw2_headfix_tick();

    /* THE HOST'S Extras, actually switched on. The host menu, the persistence
     * and the wire format were all there; nothing wrote the game's enabled
     * array, so a host could turn on Disco, have it remembered, have it sent to
     * clients, and play a match with no Disco. See extrasapply.h. */
    lsw2_extras_tick();

    /* The fork's hidden pause rows (Extras, Host Wireless, Save Game), kept
     * hidden while the player scrolls. Without this the icon came back and the
     * cursor still landed on them, because the corrections only ran while the menu was being BUILT. */
    lsw2_pause_tick();

    /* THE FORK'S MENUS OWN THE TOUCH SCREEN while they are up, the way the
     * vanilla lobby menus do - rows on the starfield, tabs and portraits gone.
     * See menubackdrop.h. */
    lsw2_menubackdrop_tick();

    /* A Roster other than Hunters skips the character select phase. Not a
     * selector choice - the chooser broadcasts its own selector 3 after the
     * host confirms, so the skip has to act on the select itself. See
     * selectskip.h. */
    /* THE HOST'S CHARACTER PICKER (Roster: One), read off the cartridge's own
     * widget.
     *
     * The picker is the Free Play touch-screen menu with its normal controls
     * and logic. So there is no input to intercept: while the host has the
     * Character row open, whatever the Free Play select is showing IS their
     * choice, and the row's text follows it, e.g. "Character: Gonk Droid".
     *
     * ONLY WHILE THAT SCREEN IS OPEN. The widget belongs to the player the
     * rest of the time - the hunters-only Free Play tab puts it on the lobby's
     * touch screen for them to pick their own hunter - and taking their taps
     * as the host's setting would rewrite the match's character every time
     * anybody browsed.
     *
     * -1 IS NORMAL AND IGNORED: the host has opened the screen and not tapped
     * yet, or is sitting on a padding slot. Leaving the previous choice
     * standing is what keeps the default (Greedo) a default. */
    /* THE PAGE RANGE, MAINTAINED RATHER THAN SET ONCE.
     *
     * The hunters-only clamp lives at the Free Play constructor's return and
     * wrote the bounds exactly once. That is correct for the player's own
     * hunter select and it made the host's One picker impossible: the widget
     * is built when the lobby's touch screen appears, so it is already cut to
     * pages 6 and 7 by the time the host opens the Character row, and R5-D4
     * (page 8) and Gonk Droid (page 10) - examples of the host's freedom to
     * pick any playable character, however absurd - could not be reached at
     * all. ui.cpp standing the hunters-only flag down on that screen
     * did nothing, because nothing re-read the flag after construction.
     *
     * So the flag is asked here, every frame, and the widget follows it. That
     * also puts the range back when the host leaves the screen, which a
     * one-shot widen at open would not: lsw2_selectwidget_set_pages pulls the
     * page inside the narrowed range, so a host who wandered to page 10 does
     * not leave the player's select stranded there.
     *
     * FREE, AND SILENT, WHEN NOTHING CHANGES: it returns false after three
     * reads when the range is already applied, so the log line below is one
     * line per actual transition rather than one per frame. */
    {
        unsigned low = 0, high = 0;
        if (lsw2_hle_fork_select_range(&low, &high) &&
            lsw2_selectwidget_set_pages(low, high))
            std::fprintf(stderr,
                         "[fork] free play select pages %u-%u (%s)\n",
                         low, high - 1,
                         high - low == 2 ? "hunters only"
                                         : "every page");
    }

    /* WHAT THE FREE PLAY SELECT IS SHOWING, whenever it changes.
     *
     * The portrait fix - a stale Greedo portrait after a Free Play tap - can only be
     * tested by making a character switch happen, and from outside the process
     * there is no way to see whether a tap landed. The widget is rebuilt at a
     * new address each time it opens, so the address in an older log line is a
     * dead object: reading it gave page 8440 and entry 0xFF, which looks like
     * data and is not.
     *
     * The port knows the live one. Two accessors that already exist, printed
     * only on a change, make "did that tap select anything" answerable from the
     * log instead of from a screenshot of a highlight that may not be drawn. */
    {
        static int last_page = -2, last_entry = -2;
        const int page = lsw2_selectwidget_page();
        const int entry = lsw2_selectwidget_entry();
        if (page != last_page || entry != last_entry) {
            last_page = page;
            last_entry = entry;
            if (page >= 0)
                std::fprintf(stderr,
                             "[fork] free play select: page %d, entry %d\n",
                             page, entry);
        }
    }

    /* THE HOST'S ONE CHARACTER, ON THE GRID (freeplaypick.h). The Character
     * screen opens no menu of its own; the picker follows it, and a
     * confirmed pick - a double tap, or A on a highlighted portrait - or a B
     * ends it. A highlight alone no longer counts as the pick: that read took
     * every browsing tap as the host's decision. */
    if (lsw2_fork_session().screen() == Lsw2ForkScreen::Character) {
        if (!lsw2_freeplaypick_host_active()) lsw2_freeplaypick_host_begin();
        const int picked = lsw2_freeplaypick_host_take();
        if (picked >= 0) {
            std::fprintf(stderr, "[fork] host picked character %d on the free "
                         "play grid\n", picked);
            lsw2_freeplaypick_host_end();
            lsw2_fork_session().finish_character_pick(picked);
        } else if (lsw2_freeplaypick_host_cancelled()) {
            lsw2_freeplaypick_host_end();
            lsw2_fork_session().finish_character_pick(-1);
        }
    } else if (lsw2_freeplaypick_host_active()) {
        lsw2_freeplaypick_host_end();
    }

    /* THE ALL RULE'S PICK: the host ends it (freeplaypick.h). */
    lsw2_freeplaypick_tick();

    /* THE ONLINE PAUSE: a menu a transition closed still has to be unmarked
     * on every console (sessionpause.h). */
    lsw2_sessionpause_tick();

    /* GAME SETTINGS OVER THE LOBBY: the level's HUD steps aside (levelhud.h). */
    lsw2_levelhud_tick();

    lsw2_selectskip_tick();

    /* THE LAST STATE THAT ACTUALLY EXISTED, sampled EVERY FRAME.
     *
     * This has to live out here rather than beside the check that uses it, and
     * that mistake cost a whole A/B. The level branches below are exclusive:
     * while the player is in the lobby the FIRST one runs, so a latch placed in
     * the last one only ever samples during the gap it is meant to see through -
     * where the state is already -1 and there is nothing left to remember. It
     * never once observed state 41, and the session was still torn down.
     *
     * Sampled here it sees 41 for the whole match state, and carries it across
     * the `41 -> -1 -> 42` gap the arena load opens. See lobbypin.h. */
    g_last_real_state = lsw2_lobbypin_settle_state(lsw2_state_current(),
                                                   g_last_real_state);

    /* A CLIENT WHOSE HOST VANISHED IN THE MATCH GOES BACK TO THE LOBBY
     * (as seen when a host crashed in a Jabba's Tower Capture the Flag
     * match). The game leaves the arena
     * itself: its netgame tick answers a broken link with world 15 and the
     * communication-error screen 38 (0x020B0B84, 0x020B0BAC), and the session
     * end now lets it (lsw2_wh_session_end_hop). What the cartridge does not
     * do is come back: from 38 it heads for the front end. So from 38 - and
     * only from there - the fork asks for the lobby with the round end's own
     * pair, once the wireless is down and nothing else is pending.
     *
     * NEVER FROM THE ARENA ITSELF. Asked for over a live arena (conn_halt9),
     * the same pair crashed the load: a callback list broadcast (0x020627D4,
     * list 1) walked a node read out of code. From 38 it landed cleanly
     * (conn_halt10). If the game never reacts, the client plays on alone,
     * which is what it did before this, and no worse. */
    if (lsw2_fork_session().take_host_lost() &&
        lsw2_lobbypin_match_bound(g_last_real_state, lsw2_state_pending()))
        g_stranded = true;
    if (g_stranded) {
        const int now = lsw2_state_current();
        if (now == kLsw2CommErrorScreen && lsw2_wh_state() <= 1 &&
            lsw2_state_pending() < 0) {
            g_stranded = false;
            std::fprintf(stderr,
                         "[fork] the host left during the match and the game "
                         "raised its communication error - returning this "
                         "console to the arena lobby offline\n");
            lsw2_lobby_return_pair();
        } else if (now >= 0 && now != kLsw2CommErrorScreen &&
                   !lsw2_lobbypin_match_bound(now, lsw2_state_pending())) {
            g_stranded = false;   /* it left the match some other way */
        }
    }

    /* THE LOBBY'S DISABLED DOOR (the player cannot leave the room), and it must run AFTER the boot is done rather than
     * during it - the fork's own arrival is a level change to the lobby, and
     * pinning while that is in flight would fight the warp it depends on.
     *
     * Only while the player is in the lobby. A match is a level change the
     * fork WANTS, so pinning during one would trap the player in the arena
     * exactly as surely as the door traps them in the room. */
    if (g_phase == Phase::Done && g_reached) {
        if (lsw2_cheats_current_level() == kLsw2ArenaLobbyLevel) {
            g_escape_cooldown = 0;
            if (lsw2_cheats_pin_level(kLsw2ArenaLobbyLevel))
                std::fprintf(stderr,
                             "[fork] refused a transition out of the lobby\n");
        } else if (lsw2_level_is_arena(lsw2_cheats_current_level()) &&
                   !lsw2_lobbypin_should_warp_back(
                        lsw2_cheats_current_level(), kLsw2ArenaLobbyLevel,
                        /*is_arena=*/true, /*cooling=*/false)) {
            /* AN ARENA IS WHERE THE MATCH HAPPENS - never warp out of it.
             *
             * The pin above was given this exemption when the round start was
             * built; THIS BACKSTOP WAS NOT, and it fires on any level that is
             * not the lobby. So the first round to actually start would have
             * dragged the player straight back out of Jabba's Tower, once per
             * cooldown, for as long as the match lasted.
             *
             * It had never been reachable to notice: the console entity refused
             * to open until the flag-11 fix, so no match could begin. That is
             * the same shape as the pause fixups that could not fire - code
             * that is correct only because the situation that breaks it does
             * not exist yet, and the situation was about to exist. */
            g_escape_cooldown = 0;
        } else if (g_escape_cooldown > 0) {
            --g_escape_cooldown;
        } else {
            /* THE PAUSE MENU'S Disconnect, from the other end.
             *
             * While connected, the pause menu's Quit Game becomes Disconnect;
             * once confirmed, it takes the player back to the lobby, offline.
             *
             * The row itself is not intercepted - see pause_quit_relabel() for
             * why - so the quit is recognised by where it LANDS. Somewhere that
             * is neither the lobby nor an arena, while a session is up, is the
             * player having left through the pause menu. Dropping the session
             * here is what makes "offline" true; the warp below is what makes
             * "returns the player to the lobby" true.
             *
             * Host or client, the design gives the same outcome by two names -
             * "Shut Down Lobby" for a host, "Disconnect" otherwise - and
             * shut_down_lobby() is the session's one way back to Offline. */
            /* ONLY WHEN WE ARE NOT ON A LEVEL AT ALL, which is what separates
             * the quit from the door.
             *
             * The door leads to another LEVEL - the Cantina - and this backstop
             * exists because play reached it. Disconnecting a host for
             * walking through a door would be a worse bug than the one being
             * fixed. Quit Game does not go to a level: it goes to the front
             * end, and `lsw2_cheats_current_level()` answers -1 there because
             * the context's index is not a level index.
             *
             * So a negative level is the pause menu's quit, and a valid one is
             * the door - which keeps the behaviour it already had. */
            /* SNAPSHOT THE ACTOR BEFORE ANYTHING ELSE, so the diff a second
             * from now is against the state the interaction found rather than
             * the state the fork's own menu left. See freezewatch.h. */
            lsw2_freezewatch_arm();

            Lsw2ForkSession& session = lsw2_fork_session();
            /* THE FRONT END BY ITS KIND, NOT BY ITS LEVEL NUMBER.
             * The rule below was written as "Quit Game lands off-level, where
             * the level reads -1" - but lsw2_cheats_current_level() reads
             * +0x0C of whatever state object the world slot holds, and the
             * front end's (0x11) carries a 0 there: a valid level index.
             * Measured (pause_explore): after Quit Game the log read "state 15
             * level 0", this rule never fired, and the player was left on the
             * LSW2 main menu - the very bug c9c81ded set out to fix.
             * Not "any world that is not a level": the score screen's world
             * is 43, and that is the round ending, not a quit.
             *
             * AND ONLY THE FRONT END IS OFF-LEVEL for the quit rule below. A
             * reload's gap also reads level -1 for a sampled frame, and that
             * was the rule's whole exposure: an earlier fix closed it for
             * the round start by reading the pending 41, and ALL's pre-game
             * pick reopened it - the pick rewrites that 41 into 48, so its
             * lobby reload's gap looked like a quit and the host shut its own
             * lobby down at round two's pick (conn_allpick_jip2: "state -1
             * level -1", then "disconnected through the pause menu (shut
             * down the lobby)"). The front end's kind is the quit's own
             * signature - Quit Game's request (0x0208EFD4) - so a gap that is
             * not the front end is never one. `cur` stays the level for the
             * door's backstop, which needs a live level state to act at all. */
            const bool at_front_end =
                lsw2_cheats_state_kind(int(kLsw2WorldSlot)) ==
                int(kLsw2FrontEndWorldKind);
            const int level_now = lsw2_cheats_current_level();
            const int cur = at_front_end ? -1 : (level_now < 0 ? 0 : level_now);

            /* AND NOT WHILE A MATCH IS LOADING. The arena transition is
             * off-level for about a second and a half, and this runs every
             * frame, so without this the fork shut its own match down in
             * roughly half of all runs. See lobbypin.cpp.
             *
             * NOR WHILE ONE IS STARTING: the round start unloads the lobby
             * while the screen still reads 48 or 54, and only the pending
             * request says 41 is next. */
            const bool match_running =
                lsw2_lobbypin_match_bound(g_last_real_state,
                                          lsw2_state_pending());

            /* THE PAUSE MENU'S Disconnect, AND THE FIX FOR IT QUITTING TO THE
             * LSW2 MAIN MENU.
             *
             * Quit Game / Disconnect tears the lobby down and lands at the
             * FRONT END - a menu state, not a level - which is `cur < 0`. The
             * warm warp used here before (lsw2_cheats_warp) CANNOT return from
             * there: lsw2_cheats_can_warp() requires the live state to already
             * be a level state, so it refused every frame and the player was
             * left sitting on the LSW2 main menu, although Disconnect should
             * land back in a freshly loaded Bounty Hunter arena lobby.
             *
             * So the front-end quit is handled by RESTARTING THE BOOT WARP -
             * the same cold-warp-then-clean-reload sequence that reached the
             * arena lobby at startup, which works precisely because it does NOT
             * need a live level state (Phase::Waiting cold-warps from an idle
             * manager). That is the intended outcome under both names
             * (Disconnect and Shut Down Lobby): the player returns to the lobby
             * offline. The session is dropped
             * first so the return is offline, with no lobby left open.
             *
             * match_running still guards it, because the arena load is briefly
             * off-level too and must not be mistaken for a quit.
             *
             * lsw2_lobbypin_is_quit is the tested discriminator for the CONNECTED
             * disconnect (its own in_session guard); the second clause extends
             * the same "off-level front end while not mid-match" rule to an
             * OFFLINE quit, which should also land back in the lobby rather than
             * on the LSW2 main menu. */
            const bool front_end_quit =
                lsw2_lobbypin_is_quit(cur, session.in_session(), match_running) ||
                (!session.in_session() && cur < 0 && !match_running);
            if (front_end_quit) {
                if (session.in_session()) {
                    std::fprintf(stderr,
                                 "[fork] disconnected through the pause menu "
                                 "(%s) - returning to the arena lobby offline\n",
                                 session.is_host() ? "shut down the lobby"
                                                   : "left the host's game");
                    session.shut_down_lobby();
                } else {
                    std::fprintf(stderr,
                                 "[fork] quit to the front end - returning to "
                                 "the arena lobby\n");
                }
                g_reached = false;
                g_settled = 0;
                g_frames = 0;
                g_status = "Returning to the arena lobby...";
                g_phase = Phase::Waiting;
                return;
            }

            /* THE DOOR BACKSTOP, AND IT IS THE CRUDE ONE ON PURPOSE.
             *
             * The door is now refused at the source (statereq.cpp turns the
             * lobby door's exit transition off before it is ever posted), so this should no
             * longer fire for it. It stays as the backstop for a transition
             * begun some way that never reaches that override - a real level
             * that is neither the lobby nor an arena. A warm warp works here
             * because `cur` IS a live level state.
             *
             * THE COOLDOWN IS NOT POLITENESS, it is the difference between one
             * warp and one per frame. The request takes several frames to be
             * acted on, and during those frames the current level still reads
             * as the far level - so an uncooled version would post a new request
             * every frame into a state machine that is already building one,
             * which is a known way to crash the guest. */
            if (lsw2_cheats_warp(kLsw2ArenaLobbyLevel)) {
                std::fprintf(stderr,
                             "[fork] left the lobby (level %d) - warping back\n",
                             cur);
                g_escape_cooldown = kEscapeCooldownFrames;
            }
        }
    }

    if (g_phase == Phase::Done) return;

    ++g_frames;

    switch (g_phase) {
    case Phase::Waiting: {
        /* WARP THE INSTANT THE STATE MACHINE IS ALIVE. No splash, no title, no
         * save select, and no synthetic key presses at all.
         *
         * The requirement is explicit - the warp happens the moment the game is
         * running, with neither the splash titles of the boot sequence nor the
         * save select shown first - and an earlier attempt to let the splash end itself first
         * measured TWENTY-ONE SECONDS before the lobby appeared. That is not a
         * skip by any reading. */
        if (lsw2_cheats_manager_idle() &&
            lsw2_cheats_warp_cold(kLsw2ArenaLobbyLevel)) {
            g_settled = 0;
            g_phase = Phase::Requested;
        }
        break;
    }

    case Phase::Requested: {
        if (!lsw2_cheats_can_warp() ||
            lsw2_cheats_current_level() != kLsw2ArenaLobbyLevel) {
            g_settled = 0;
        } else if (++g_settled >= kSettleFrames) {
            /* THEN LOAD IT AGAIN, FROM A PROPER LEVEL STATE.
             *
             * The cold warp jumps out of the splash without the splash ever
             * tearing down its own 2D setup - its sub-screen layers, VRAM banks
             * and palettes stay installed, which is the magenta starfield
             * through the lobby floor and the copyright text on the touch
             * screen. The level entry configures what it needs and does not
             * clear what it does not.
             *
             * A SECOND warp is the game's ordinary path: it leaves a real level
             * state, so the transition runs the teardown the cold jump skipped,
             * and the lobby is then built over a clean 2D configuration rather
             * than over the splash's leftovers.
             *
             * RETRACTED IN PART: "the transition runs the
             * teardown". It did not - both warps set the slot's status byte
             * themselves, which builds over the old state without exiting or
             * destroying it, the first lobby included. Both now write the
             * game's own request (cheats.cpp), so the teardown this paragraph
             * counted on is real, for the splash as well as the first lobby.
             *
             * It costs one more load and no host-side poking at hardware
             * registers, which is the alternative and a far worse bet. */
            g_settled = 0;
            g_status = "Reloading cleanly...";
            if (lsw2_cheats_warp(kLsw2ArenaLobbyLevel)) {
                g_phase = Phase::Reload;
            } else {
                g_status = nullptr;
                g_phase = Phase::Done;   /* nothing to reload from */
            }
        }
        break;
    }

    case Phase::Reload: {
        if (!lsw2_cheats_can_warp() ||
            lsw2_cheats_current_level() != kLsw2ArenaLobbyLevel) {
            g_settled = 0;
        } else if (++g_settled >= kSettleFrames) {
            /* The main screen is clean by now; the TOUCH screen is not,
             * because slot 1 still holds the BOOT UI. Swap it for the in-game
             * one - kind 48, measured against a real level. */
            lsw2_cheats_install_game_ui();

            /* EVERYTHING UNLOCKED (characters, customization parts, weapons and
             * extras), applied once the lobby is actually up.
             *
             * WHY HERE AND NOT AT BOOT. The flag block is the RUNTIME progress
             * structure - the game fills it from the save while loading, and a
             * write made before that is overwritten by the load. Waiting until
             * the level has settled is the same reasoning that made the warp
             * itself wait, and it is cheap: one pass over 75 ids.
             *
             * The fork only. The baseline offers this as a button in the cheats
             * tab instead, because the baseline stays vanilla-esque. */
            if (lsw2_cheats_unlock_everything())
                std::fprintf(stderr, "[fork] unlocked the roster and extras\n");

            g_reached = true;
            g_settled = 0;
            g_status = nullptr;
            g_phase = Phase::Done;
        }
        break;
    }

    case Phase::Done:
        break;
    }

    if (g_phase != Phase::Done && g_frames >= kBootFrameBudget) {
        /* SAY WHAT HAPPENED. A fork that quietly falls back to baseline
         * behaviour is a fork that looks broken for reasons nobody can see. */
        std::fprintf(stderr,
            "[fork] gave up waiting to reach the Arena Lobby after %d frames "
            "(phase %d). Showing the game as it is.\n",
            g_frames, int(g_phase));
        g_phase = Phase::Done;
        g_status = nullptr;
    }
}

void lsw2_fork_enforce_party_of_one() {
    /* THE PARTY OF ONE: the player 2 companion character is disabled.
     *
     * EVERY FRAME, not once on arrival. The companion is spawned by the level,
     * so it comes back on every level transition - and the fork moves between
     * the lobby and the arena constantly. Enforcing continuously costs one read
     * of a null pointer in the settled case, which is nothing.
     *
     * BUT NOT A SLOT A CONNECTED PLAYER OWNS, and the comment that used to sit
     * here said the opposite:
     *
     *     "Slots 2 and 3 are included because the rule is party of ONE, and
     *      a network session that seats more players would otherwise leave them."
     *
     * That reasoning inverts the networked lobby. The party of one is about
     * the level's player 2 COMPANION, and the lobby asks for the opposite of
     * removing everyone else: a joiner spawns in the Cantina lobby and sees the
     * other connected players, the host among them, and the host sees the
     * other players walking around the lobby.
     *
     * So this loop, running every frame, would have deleted every remote
     * player's actor the instant Phase C spawned one - and it would have looked
     * like Phase C failing to spawn them rather than the party-of-one rule
     * removing them. Two requirements in genuine conflict, resolved the wrong
     * way, and invisible only because no remote player has ever existed to
     * delete.
     *
     * THE RULE: a slot is the companion's unless someone is sitting in it. */
    if (!ui_fork_build() || !lsw2_cheats_available()) return;

    Lsw2ForkSession& session = lsw2_fork_session();
    const int local = session.local_station();

    for (int slot = 1; slot < int(kLsw2PartySlots); ++slot) {
        /* A SEATED PEER, or this machine's own seat if it is not slot 0. The
         * station-to-slot mapping is not established - all that is known is
         * that other players are indices 1 to 3 - so both are checked rather
         * than assuming station equals slot. Erring towards keeping an actor is
         * the safe direction: a companion that lingers is a visible bug, a
         * teammate deleted every frame is an unexplainable one. */
        if (slot == local) continue;
        if (session.peer_row_for_station(slot) >= 0) continue;

        /* AND A SEAT THE CARTRIDGE'S OWN SESSION HAS FILLED.
         *
         * The symptom: other players were not visible in the lobby. The guard above
         * asks the FORK'S session who is seated, and the fork's session is the
         * TCP/master-server one. It knows nothing about a peer that joined
         * through the cartridge's own wireless system, which is now the way
         * peers actually arrive.
         *
         * MEASURED, not reasoned: "[fork] removed party slot 1 (party of 1)"
         * appeared in every two-console run, while both consoles
         * reported players 2. So the fork has been deleting the remote player's
         * actor every frame - the exact failure the comment above predicted and
         * then guarded against with the wrong signal.
         *
         * The cartridge's own input-player count is the right authority here:
         * it is what the game itself uses to decide how many players exist. A
         * slot below that count belongs to a player, not to the companion. */
        const Lsw2ViewManagerState vm = lsw2_viewmanager_state();
        if (slot < vm.players) continue;

        if (lsw2_cheats_remove_party_member(slot)) {
            std::fprintf(stderr,
                         "[fork] removed party slot %d (party of 1)\n", slot);
        }
    }
}

/* THE PARTY OF ONE IN LOCKSTEP: the dataset's members decide.
 *
 * Called from the exchange, at a dataset boundary, with that dataset's member
 * bitmap - the one input every console holds identically at the same step.
 * The netgame's party slots ARE its AIDs, so a slot that is not a member is
 * the level's companion and a member's slot is a player. The tick version
 * above asks this console's own station and the fork's TCP peer list, which
 * differ by console - measured in a four-player match: consoles 2,
 * 3 and 4 each deleted a DIFFERENT real player's actor. */
void lsw2_fork_remove_companions(uint16_t members) {
    if (!ui_fork_build() || !lsw2_cheats_available()) return;
    for (int slot = 1; slot < int(kLsw2PartySlots); ++slot) {
        if ((members >> slot) & 1u) continue;
        if (lsw2_cheats_remove_party_member(slot))
            std::fprintf(stderr, "[fork] removed party slot %d (party of 1: "
                         "not a member of this dataset)\n", slot);
    }
}

/* THE ENHANCED CONTROLS' ONE UNMEASURED NUMBER, reported when it changes.
 *
 * Tank-versus-camera-relative movement was traced down to `actor+0x5DA`
 * and left one thing unread: which value the arena assigns, because both
 * construction sites compute it rather than passing a literal. "The arena passes
 * 4" is an inference from how the controls feel, and the Enhanced controls' implementation route
 * depends on it being right.
 *
 * ON CHANGE, NOT EVERY FRAME. The value is stable for the life of an actor, so a
 * per-frame line would be sixty identical lines a second; a line per distinct
 * value is the whole signal. Costs one byte read per frame otherwise. */
void lsw2_fork_report_view_index() {
    static int last = -2;
    const int index = lsw2_cheats_party_view_index(0);
    if (index == last) return;
    last = index;
    if (index < 0) return;
    std::fprintf(stderr,
                 "[fork] player view index (actor+0x5DA) = %d%s  [level %d]\n",
                 index,
                 index == 4 ? "  (4 = no view of its own -> tank controls)" : "",
                 lsw2_cheats_current_level());
}

/* THE MATCH-START SIGNAL, host and joiner halves.
 *
 * The fork's PC transport reaches the lobby but the MATCH stalled: the host
 * pressed Start Game and entered the arena flow (state 40/41) while the joiner
 * sat at state 48, because the DataSharing lockstep only steps once both are IN
 * the match, so the joiner never learned the host had left. This carries that
 * one fact over the transport's lobby-active control channel.
 *
 * HOST: when its own state enters the arena flow (40/41/42), send the signal
 * once; re-arm when it returns to the lobby (48/54). JOINER: take the signal and
 * report it. The joiner does NOT yet drive itself into the match here - that is
 * the next step (behind LSW2_JOINER_ENTER_MATCH), now that its view manager is
 * built so FUN_02072cf4's tail can no longer null-deref. This
 * increment proves the signal crosses and the host-side detection fires. */
static void lsw2_match_signal_tick() {
    if (!ui_fork_build()) return;

    Lsw2ForkSession& session = lsw2_fork_session();
    if (session.state() == Lsw2SessionState::Offline) return;

    const int st = lsw2_state_current();

    if (session.is_host()) {
        static bool signaled = false;
        if (st == 40 || st == 41 || st == 42) {
            if (!signaled) {
                signaled = true;
                /* map/rule/seconds are wired to the chooser's real values in the
                 * joiner-drive step; 0 is a placeholder the signal test ignores. */
                session.send_start_match(0, 0, 0);
                std::fprintf(stderr,
                             "[fork] match signal: host entered the arena flow "
                             "(state %d) - sent start-match to peers\n", st);
            }
        } else if (st == 48 || st == 54) {
            signaled = false;   /* back in the lobby: re-arm for the next match */
        }
    } else {
        unsigned char map = 0, rule = 0;
        unsigned short seconds = 0;
        if (session.take_start_match(&map, &rule, &seconds)) {
            std::fprintf(stderr,
                         "[fork] match signal: RECEIVED host start-match "
                         "(map %u rule %u sec %u) while at joiner state %d - "
                         "arming the arena entry\n",
                         unsigned(map), unsigned(rule), unsigned(seconds), st);
            lsw2_wh_arm_joiner_enter_match();
        }
        /* Fire the armed entry once the guest is awake and settled in the lobby.
         * No-op unless armed and LSW2_JOINER_ENTER_MATCH is set. */
        lsw2_wh_joiner_enter_match();
    }

    /* Bounded probe: force 41 -> 42 (both roles). No-op unless LSW2_FORCE_MATCH. */
    lsw2_wh_force_match_state();
}

void lsw2_fork_console_interact() {
    if (!ui_fork_build()) return;

    /* ALREADY UP MEANS LEAVE IT ALONE.
     *
     * The reported symptom was the console menu appearing and vanishing, and
     * the log showed "opened the arena menu" TWICE from what looked like one
     * approach, with the backdrop going down, restoring, and going down again.
     *
     * THREE PLACES DECLINE THE CONSOLE'S REFUSAL and all three call this: the
     * script command, the state message, and the queue. That redundancy is
     * deliberate - the refusal can surface by any of those routes and the fork
     * has to catch it wherever it does. What was NOT deliberate is that each
     * site guards only its own LOG LINE with a `static bool said`, not the
     * call. So a refusal that recurs - and it recurs for as long as the player
     * stands on the console - re-entered here every frame while the log
     * cheerfully reported it once.
     *
     * Re-opening a menu that is already open rebuilds it under the player,
     * which is what made it flicker. Opening is an EDGE, not a state: the
     * console says "the player asked", and the answer is only interesting when
     * nothing is already answering it. */
    if (lsw2_menuhost_open_now()) return;

    /* NOR WHILE A FORK SCREEN WITH NO MENU IS UP - the One character's Free
     * Play grid (Lsw2ForkScreen::Character) closes the menu host and answers on
     * the touch screen. The console standing under the host would otherwise
     * take the grid's place. */
    if (lsw2_fork_session().screen() != Lsw2ForkScreen::None) return;

    /* SAID EVERY TIME IT OPENS, with where the game is. The three refusal
     * hooks each log their first decline only, so a console that opened during
     * a match left no line saying when. */
    {
        static int said = 0;
        if (said < 40) {
            ++said;
            std::fprintf(stderr,
                         "[fork] console interaction: screen state %d, world "
                         "kind %d, level %d\n", lsw2_state_current(),
                         lsw2_cheats_state_kind(int(kLsw2WorldSlot)),
                         lsw2_cheats_current_level());
        }
    }

    Lsw2ForkSession& session = lsw2_fork_session();

    /* OFFLINE OPENS THE ARENA MENU, not the console's six options.
     *
     * The designed flow reaches the console entity only after a lobby is up -
     * Start Lobby puts the host, on foot, into the arena lobby, and the
     * console entity is used only from there. Offline,
     * four of those six options have no lobby to act on.
     *
     * What belongs here instead is the menu that comes first: "Host
     * Internet Game" / "Join by IP/Room Code" / "View Master Server". An
     * earlier version opened the host settings menu directly, which skipped the
     * step where the player chooses to host rather than to join - and joining is
     * half of what that screen is for. */
    if (session.state() == Lsw2SessionState::Offline) {
        session.open_arena_menu();
        /* SAY WHETHER THE MENU IS ACTUALLY UP, because the freeze report has
         * two mutually exclusive explanations and this line separates them.
         *
         * The symptom: the arena lobby console entity froze the player until
         * they jumped. If a menu is open the guest sees NO
         * keys at all - lsw2_menuhost_guest_keys returns 0 - so the player
         * could not jump either, and the freeze would be an invisible menu.
         * If no menu is open the swallow is not happening and the lock is the
         * guest's own interaction state, left set because the fork declined
         * the script command that would have ended it.
         *
         * Those need opposite fixes, and one line decides which. */
        /* THE VERDICT WAS REMOVED FROM THIS LINE.
         *
         * It used to report the holding case as "IS holding input - an
         * invisible menu", which was an INFERENCE printed as an observation,
         * and it was wrong. Captured through the port's own present path
         * (presentshot.h), the arena menu draws correctly - title, three rows,
         * cursor highlight, on the starfield. The "invisible" half came from
         * `pilot shot`, which reads the framework's framebuffer and cannot see
         * anything the port composites; a reader of this log had no way to
         * know the label was a guess.
         *
         * Holding input is simply what an open menu does. So the line says
         * which of the two states it is in and stops there. */
        std::fprintf(stderr,
                     "[fork] console: opened the arena menu (menu host %s)\n",
                     lsw2_menuhost_open_now()
                         ? "is holding input, as an open menu does"
                         : "is NOT holding input - the guest is locked");
        return;
    }

    if (!session.open_console())
        std::fprintf(stderr, "[fork] console: a client has no options here\n");
}

bool lsw2_fork_hiding_boot() {
    return ui_fork_build() && g_phase != Phase::Done;
}

bool lsw2_fork_hiding_join() {
    bool hide = false;
    const Lsw2SessionState st = lsw2_fork_session().state();
    /* THE LOBBY AND THE JOIN'S OWN SCREEN STATES. The stock join screens
     * (browse 34, "Waiting for Host" 39) and the loading screen (40) are the
     * touch screen's: the top keeps drawing the lobby, with the camera on
     * slot 0 - measured, a cover that stopped at 34 left the host's hunter on
     * the top screen for the whole join. */
    const int state = lsw2_state_current();
    if (ui_fork_build() &&
        (st == Lsw2SessionState::Joining || st == Lsw2SessionState::Joined) &&
        (state == kLsw2ArenaLobbyState || state == kLsw2BrowseState ||
         state == kLsw2JoinWaitState || state == kLsw2StateSessionStart)) {
        /* SEATED: the game's station byte is this console's session AID and
         * the multi-view bit is up - the seat reload has put this player in
         * its slot. */
        const uint32_t a = kLsw2NetCountObject + kLsw2NetLocalStation;
        const uint8_t station =
            uint8_t((bus_device_read32(0, a & ~3u) >> ((a & 3u) * 8u)) & 0xFFu);
        const bool seated =
            lsw2_vwh_multiview() && int(station) == lsw2_vwh_local_aid();
        /* AND ONLY ONCE SLOT 0 IS SOMEBODY ELSE. Until the join's rebuild,
         * slot 0 is still this player in their own lobby - a second joiner
         * waiting its turn to be seated sits there for seconds and must see
         * it. */
        const int slot0 = lsw2_cheats_party_character(0);
        hide = !seated && slot0 >= 0 && slot0 != lsw2_last_hunter();
    }
    static bool was = false;
    if (hide != was) {
        was = hide;
        std::fprintf(stderr, "[fork] join cover %s\n",
                     hide ? "ON: the join made slot 0 somebody else - the top "
                            "screen is not shown"
                          : "off");
    }
    return hide;
}

const char* lsw2_fork_boot_status() {
    return lsw2_fork_hiding_boot() ? g_status : nullptr;
}

uint16_t lsw2_fork_boot_keys() {
    /* Only while the fork is still getting there. Once it has arrived the pad
     * belongs to the player, and a stray synthetic press would look like the
     * game pressing its own buttons. */
    return lsw2_fork_hiding_boot() ? g_boot_keys : 0;
}
