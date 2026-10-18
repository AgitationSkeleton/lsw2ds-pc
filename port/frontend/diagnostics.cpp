/* See diagnostics.h. */

#include "diagnostics.h"
#include "coopjoin.h"
#include "selectskip.h"
#include "lasthunter.h"
#include "rngsync.h"
#include "netcount.h"
#include "netmsglog.h"
#include "statelog.h"

#include "cheats.h"
#include "enhanced.h"
#include "levels.h"
#include "selectwidget.h"
#include "ui.h"

#include "state.h"

#include "musicpick.h"

#include <cstdio>
#include <cstdlib>

namespace {

/* The slot the game currently has playing. The request function reads
 * this very byte to decide whether a request is a no-op, so it is the game's own
 * answer to "what is playing" rather than a guess from the request side. */
constexpr uint32_t kMusicSlotField = 9u;

/* THE GAME'S RANDOM NUMBER GENERATOR, watched as a desync canary.
 *
 * `0x02062D10` is MT19937 - the tempering constants `0x9D2C5680` and
 * `0xEFC60000` are unmistakable - with its state block at `0x02104344`. `+8` is
 * the count of numbers left before the state is regenerated, and it decrements
 * on every single draw.
 *
 * That makes it one word that changes whenever anything asks for a random
 * number, which is exactly what a lockstep session needs to compare. Two
 * machines in the same match must draw the same numbers in the same order; if
 * this word differs between them at the same frame, they have already diverged
 * and everything after is a symptom. */
/* See rngsync.h - kLsw2RandState is the one copy. */
constexpr uint32_t kRandRemaining = 8u;

/* The per-player pending-character request the poller acts on. Watched
 * so a request the port makes can be seen being taken up, which is the only way
 * to tell "the write did not land" from "the game declined it". */
/* THE LEVEL ROOT, from the GENERATED levels.h as kLsw2LevelRootAddr - one
 * copy, and the right one: extract_level_table.py regenerates it from the
 * ROM, so a re-extraction updates every consumer instead of leaving
 * hand-written copies behind. */
/* lasthunter.h's kLsw2PendingCharacter - this was the THIRD copy of 0x92E. */

/* THE RUNNING STATE, from a static root.
 *
 * The netplay dispatcher asks this question before every message it routes -
 * `ldr r0,=0x02102900; ldr r0,[r0,#4]; ldrne r1,[r0,#4]` - so it is the game's
 * own notion of "which screen is this", not a reconstruction. An earlier
 * investigation spent five attempts trying to name the state that draws the
 * arena menu by what the screen does; this prints the id the game itself
 * branches on, and one run that walks lobby -> select -> match names all three.
 *
 * THE ID IS THE REQUEST KIND. The factory at `0x0205F578` switches on
 * its `kind` argument, and two kinds line up with two dispatcher ids by
 * independent routes - kind 0x29 allocates 0x2B20 bytes, exactly bounding the
 * select state's field cluster measured at `this + 0x2B00..0x2B1C`, and kind
 * 0x36's constructor sits in the same module as the message handler found for
 * state 0x36. So a kind names a state without a live run.
 *
 * NOT PROVEN IN GENERAL, and the difference matters: `+0x04` is filled from a
 * constructor ARGUMENT, and that argument has not been traced back to the kind.
 * Two agreements are evidence, not a mechanism. */
/* The state manager, its two +4 hops and the state ids are ALL statelog.h's
 * now - kLsw2StateManager, kLsw2StateCurrent, kLsw2StateId and the ids. This
 * file used to declare the offsets and the ids again under its own names,
 * directly beneath a comment saying the manager was the one copy. */

/* The select state's own fields, valid only while the id above says 0x29.
 * The cluster was mapped first; the four fields that carry the flow were read
 * out of it afterwards. */
/* selectskip.h's kLsw2SelPicked: how many players have chosen. */
constexpr uint32_t kSelSpinner = 0x2B07u;  /* s8, +0x32 is the statue type  */

/* statelog.h's kLsw2SequenceCounter - a sequence counter message 3 resyncs,
 * not the once-per-round value this comment used to promise. */

/* THE PER-PLAYER APPEARANCE RECORD, four slots, stride 0x40.
 *
 * Showing each player the other players' customizations was not put on the
 * wire at first, because the customization block at `0x02107890 + 0xA6` is a
 * single global set with nowhere to put a remote player's parts - the
 * per-player structure had to be found first. This is it.
 *
 *   0x02106FD8  character id, halfword, 100 (0x64) meaning "nobody"
 *   0x02106FDA  five part slots, halfwords, 0xFA meaning "default"
 *   0x02106FF3  a byte set to 1 when the slot is filled
 *
 * Slot n adds n * 0x40. The addresses are named rather than framed as offsets
 * from a record head, because the head is NOT here: `0x02098D3C` reads three
 * halfwords at `+0x2240..+0x2244`, before the character, and what they are has
 * not been read. Naming the character field "the record" would repeat exactly
 * the mistake made with the global block - naming a structure after the way in.
 *
 * THE SIXTH PART IS THE OPEN QUESTION. The global block has six (a correction
 * to its first reading, which counted five); this array's initialiser writes
 * five. `+0x0C` is printed alongside them precisely so one run says whether it
 * moves - the
 * five-versus-six mistake has already cost this project a slot once, and
 * guessing the same way twice would be careless rather than unlucky. */
/* THE APPEARANCE RECORD ARRAY - see cheats.h for kLsw2AppearanceRecord and
 * kLsw2AppearanceStride. Both were declared here too, as kLsw2AppearanceRecord and
 * kLsw2AppearanceStride, and again in the other of these two files: one array, three
 * files, two spellings each. */
constexpr int kAppearanceSlotCount = 4;
constexpr int kSlotParts = 6;   /* five confirmed, the sixth under test */
constexpr uint16_t kSlotNobody = 100u;

/* DOES THE CARTRIDGE THINK A WIRELESS SESSION IS UP?
 *
 * One byte, three consumers, all read in the same pass:
 *   - the pause menu BUILDER renames row 1 from "Host Wireless" to "Shut Off
 *     Wireless" and clears the DS icon when it is set (0x0208E8B8);
 *   - row 1's ACTION starts a session when it is clear and ends one when it is
 *     set (0x0208EAF0);
 *   - Quit Game's leave path tests it before tearing down (0x0208EFA4).
 *
 * It is therefore the game's own answer to "am I in a multiplayer game", and it
 * gates both the online pause menu's Quit Game turning into Disconnect and the
 * master server's pause-menu entry point. Watched rather than written: whether the fork's
 * session should set it is a decision that needs to know what it currently does
 * while the fork is connected, which is exactly what this line reports. */
/* See coopjoin.h - the wireless-owner word and its field live there. */
constexpr uint32_t kWirelessUpField = 6u;

/* IS THE CARTRIDGE'S MESSAGE LAYER ALIVE IN THE FORK?
 *
 * The receive pump `0x020B0410` runs every frame from the state manager's tick,
 * but only when `[0x021027F8 + 0x29] > 1`. Everything worked out about
 * delivering messages rests on that guard passing - write a packet, bump a
 * sequence, and the game dispatches it - so if this byte stays at 0 or 1 while a
 * fork lobby is up, none of it runs and the port would have to drive the
 * dispatcher itself.
 *
 * DECODED SINCE, and the port now writes it. `0x0205CE5C` is a one-instruction
 * setter for this byte and `0x0206F870` guards it with `cmp r1,#1 / bls skip`
 * against a stored count - so it is the player count, not a reading of one.
 * `netcount.cpp` mirrors the session's count into it, because the fork's peers
 * arrive over a transport the cartridge knows nothing about and none of the
 * game's own callers ever fire.
 *
 * STILL WATCHED, for the opposite reason it was added: not to learn what the
 * byte is, but to see that the write lands and that nothing puts it back. The
 * local station's sequence byte rides alongside - if it ever moves, packets are
 * really being dispatched. */
/* See netcount.h - the object and both offsets live there. */
/* See netmsglog.h - kLsw2MsgOwner, kLsw2MsgSeqBase and kLsw2MsgSeqStride
 * are the one copy. The last two used to be declared here as kLsw2MsgSeqBase
 * and kLsw2MsgSeqStride, directly under this comment. */

constexpr uint32_t kRamLow = 0x02000000u;
constexpr uint32_t kRamHigh = 0x02400000u;
bool ram_pointer(uint32_t v) { return v >= kRamLow && v < kRamHigh; }

uint8_t read_byte(uint32_t addr) {
    const uint32_t word = bus_device_read32(0, addr & ~3u);
    return uint8_t((word >> ((addr & 3u) * 8u)) & 0xFFu);
}

/* Every watched value, and what it was when we last said so. -2 is "never
 * looked", which is distinct from -1 "not available right now" - a field that
 * goes unavailable is worth one line, and then silence. */
struct Watched {
    int level = -2;
    int music = -2;
    int view_index = -2;
    int camera_yaw = -2;
    int character = -2;
    int pending = -2;
    int state_id = -2;
    int wireless = -2;
    int netplayers = -2;
    int netseq = -2;
    int picked = -2;
    int spinner = -2;
    long round = -2;
    /* Not printed on change - it changes on every draw. Sampled on a level
     * change instead, which is a moment two machines can be compared at. */
    unsigned rand_remaining = 0;
    /* One line per slot, printed when that slot's record changes. `seen` keeps
     * the first read from being suppressed by a lucky match against zero. */
    uint16_t slot[kAppearanceSlotCount][1 + kSlotParts] = {{0}};
    bool slot_seen[kAppearanceSlotCount] = {false};
};

Watched g_last;
bool g_on = false;
bool g_checked = false;

bool env_set(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && *v != '0';
}

void say(const char* what, int was, int now, const char* note) {
    std::fprintf(stderr, "[diag] %-14s %d -> %d%s%s\n", what, was, now,
                 note && *note ? "   " : "", note ? note : "");
}

}  // namespace

bool lsw2_diag_enabled() {
    if (!g_checked) {
        g_checked = true;
        g_on = env_set("LSW2_DIAG") || env_set("LSW2_TRACE_ENQUEUE") ||
               env_set("LSW2_TRACE_MUSIC");
    }
    return g_on;
}

void lsw2_diag_announce() {
    if (!lsw2_diag_enabled()) return;
    std::fprintf(stderr,
        "[diag] watching, on change only. What each line answers:\n"
        "[diag]   level        which map - the lobby is 9, the arenas 4 and 5\n"
        "[diag]   music        which slot is playing. Play a match and\n"
        "[diag]                note which slots appear during COMBAT - that is\n"
        "[diag]                the 'battle track' list, which the ROM does not\n"
        "[diag]                encode.\n"
        "[diag]   view_index   4 means tank controls. ENHANCED NOW WRITES\n"
        "[diag]                THIS, so in a match with Enhanced on expect a 4\n"
        "[diag]                followed at once by a 0 - that pair IS the feature\n"
        "[diag]                working. A first reading already in 0-3 means the\n"
        "[diag]                arena hands out real views and there was nothing to\n"
        "[diag]                change; the port then leaves it alone on purpose.\n"
        "[diag]                A 4 in the lobby proves nothing either way.\n"
        "[diag]   netplayers   READ THIS ONE FIRST. The cartridge's\n"
        "[diag]                receive pump only runs while this is > 1, so it\n"
        "[diag]                gates the ROUND START, the pick, the score screen\n"
        "[diag]                and every other message the fork delivers. The\n"
        "[diag]                fork writes it from the session's peer count. If\n"
        "[diag]                it never leaves 1 with a peer connected, nothing\n"
        "[diag]                downstream can work and that is the first thing\n"
        "[diag]                to fix - every other question below is secondary.\n"
        "[diag]   netseq       the local station's sequence byte, which the pump\n"
        "[diag]                bumps as it consumes packets. Moving means the\n"
        "[diag]                game is READING what the fork delivered; frozen\n"
        "[diag]                while netplayers > 1 means it is being written\n"
        "[diag]                somewhere the pump does not look.\n"
        "[diag]   camera_yaw   Enhanced's camera half. Every earlier pass\n"
        "[diag]                recorded this as needing a virtual call the port\n"
        "[diag]                cannot make; the call turns out to be a plain\n"
        "[diag]                address sum, so the port reads it directly. -1\n"
        "[diag]                means the multi-view array is not allocated, which\n"
        "[diag]                is normal outside a match. A PLAUSIBLE, MOVING\n"
        "[diag]                value in a match proves the address and makes the\n"
        "[diag]                right stick a write away.\n"
        "[diag]   character    who the player is, from actor+0x6BB\n"
        "[diag]   pending      the request byte. -1 is idle; a value\n"
        "[diag]                appearing and then returning to -1 is the game\n"
        "[diag]                taking up a change the port asked for.\n"
        "[diag]   state        the id the netplay dispatcher itself\n"
        "[diag]                branches on. Walk lobby -> statue select -> a\n"
        "[diag]                match and the three lines name all three states;\n"
        "[diag]                only 0x29 and 0x36 are known so far. Naming them\n"
        "[diag]                from the screen alone failed five times.\n"
        "[diag]   picked       while 0x29: how many players have chosen.\n"
        "[diag]   spinner      while 0x29: the local cursor.\n"
        "[diag]   round        message 3's counter. It ticks once per\n"
        "[diag]                round start and must MATCH on both machines.\n"
        "[diag]   wireless     the cartridge's own 'a session is up'\n"
        "[diag]                byte. Host a fork lobby and watch whether it\n"
        "[diag]                moves. Staying 0 means the fork's session is\n"
        "[diag]                INVISIBLE to the pause menu, and the 'Quit Game\n"
        "[diag]                becomes Disconnect' has to be driven by the fork\n"
        "[diag]                rather than by setting this.\n"
        "[diag]   slot N       the per-player appearance record -\n"
        "[diag]                character and parts, one per station. Watch a\n"
        "[diag]                REMOTE slot fill in: that is what showing other\n"
        "[diag]                players' customizations needs, and what had\n"
        "[diag]                to be found first. If [+0x0C] ever moves,\n"
        "[diag]                the record has six parts like the global block\n"
        "[diag]                does; if it never does, it has five.\n"
        "[diag] Also useful in the same run:\n"
        "[diag]   LSW2_TRACE_ENQUEUE=1  names the opcode the pause menu raises\n"
        "[diag]                         - open the pause menu once.\n"
        "[diag]   For Settings: change something in the cartridge's own Settings\n"
        "[diag]   screen, quit, restart, and see whether it survived.\n"
        "[diag]   For the master server's entry point, in the BASELINE build:\n"
        "[diag]     LSW2_HLE=1 LSW2_TRACE_TEXT=Wireless  port/launch.sh\n"
        "[diag]   then walk up to the console in the main Cantina. The fork\n"
        "[diag]   found its own console entity exactly this way -\n"
        "[diag]   the id is built at runtime and no static search can produce\n"
        "[diag]   it. 0x2D6 \"Do you want to use DS Wireless?\" is the\n"
        "[diag]   candidate; the trace says whether it is the one.\n"
        "[diag]   LSW2_HLE=1 is needed because the baseline installs no\n"
        "[diag]   overrides by default, and the trace lives in one of them.\n");
}

void lsw2_diag_tick() {
    if (!lsw2_diag_enabled()) return;
    if (!lsw2_cheats_available()) return;

    const int level = lsw2_cheats_current_level();
    if (level != g_last.level) {
        /* levels.h is a table rather than a lookup, so the name is found here.
         * Worth the four lines: a log saying "level 9" makes a reader go and
         * find the table; one saying "Cantina Arena Lobby" does not. */
        const char* name = "?";
        for (const Lsw2Level& l : kLsw2Levels)
            if (l.id == level) { name = l.name; break; }
        /* THE RNG COUNTER RIDES THE LEVEL LINE. It changes on every draw, so
         * printing it on change would be a flood; a level change is a moment
         * two machines in the same match can be compared at, and if this
         * differs between them they have already diverged. */
        const unsigned draws = bus_device_read32(0, kLsw2RandState + kRandRemaining);
        std::fprintf(stderr, "[diag] %-14s %d -> %d   %s   (rng draws left %u)\n",
                     "level", g_last.level, level, name, draws);
        g_last.rand_remaining = draws;
        g_last.level = level;
    }

    const int music = int(read_byte(kLsw2MusicContext + kMusicSlotField));
    if (music != g_last.music) {
        /* THE NOTE IS THE WHOLE POINT OF THIS LINE. Whoever reads the log needs
         * to know that a slot seen during combat is the answer and a slot seen
         * on a menu is not. */
        say("music", g_last.music, music,
            lsw2_cheats_in_arena_match() ? "IN A MATCH - candidate battle track"
                                         : "not in a match");
        g_last.music = music;
    }

    const int view = lsw2_cheats_party_view_index(0);
    if (view != g_last.view_index) {
        say("view_index", g_last.view_index, view,
            view == 4 ? "4 = no view of its own -> tank controls"
                      : "not 4 -> camera-relative movement");
        g_last.view_index = view;
    }

    /* The camera half of Enhanced controls, now that it is reachable. The
     * manager is only allocated in multi-view, so -1 outside a match is the
     * normal answer and not a failure. */
    const int cam = lsw2_enhanced_camera_yaw(0);
    if (cam != g_last.camera_yaw) {
        say("camera_yaw", g_last.camera_yaw, cam,
            cam < 0 ? "no multi-view camera array yet"
                    : "the view yaw the port can now read directly");
        g_last.camera_yaw = cam;
    }

    const int who = lsw2_cheats_party_character(0);
    if (who != g_last.character) {
        say("character", g_last.character, who, "");
        g_last.character = who;
    }

    const int pending = int(int8_t(read_byte(kLsw2LevelRootAddr + kLsw2PendingCharacter)));
    if (pending != g_last.pending) {
        say("pending", g_last.pending, pending,
            pending < 0 ? "idle" : "a character change is queued");
        g_last.pending = pending;
    }

    /* THE STATE, AND ONLY THEN ITS FIELDS.
     *
     * Two dereferences, each checked. The manager is a static that exists from
     * boot, but the slot it points at is empty between states - reading a
     * state's fields through a null or stale pointer would print a number that
     * looks like an answer, which is worse than printing nothing. -1 means "no
     * state right now", which is a real transition and worth one line. */
    /* ONE DEREFERENCE TOO MANY, CONFIRMED BY MEASUREMENT. This read
     * the manager constant as a POINTER and then added the offset; it is a BASE
     * ADDRESS, and the wrong chain lands on `00000004`. So `state` was always 0
     * here and this diagnostic reported "no state right now" forever - which is
     * worse than useless in a file whose whole job is to say what changed. */
    const uint32_t state = lsw2_state_current_object();
    const int state_id =
        state ? int(bus_device_read32(0, state + kLsw2StateId) & 0xFFu) : -1;
    if (state_id != g_last.state_id) {
        say("state", g_last.state_id, state_id,
            state_id == kLsw2StateSelect ? "0x29 = the hunter statue select"
            : state_id == 0x36             ? "0x36 = the map/gamemode/time chooser"
            : state_id == kLsw2StateScore      ? "0x2C = THE SCORE SCREEN - the 15s "
                                             "timer belongs here"
            : state_id == 0x2B             ? "0x2B = raised right after the score "
                                             "screen at match end"
            : state_id == 0x2A             ? "0x2A = raised with the round start"
                                           : "unnamed - request kinds map to ids");
        g_last.state_id = state_id;
        /* The two fields below belong to 0x29 alone, so a state change makes
         * whatever was remembered about them meaningless rather than stale. */
        g_last.picked = -2;
        g_last.spinner = -2;
    }

    if (state && state_id == kLsw2StateSelect) {
        const int picked = int(read_byte(state + kLsw2SelPicked));
        if (picked != g_last.picked) {
            say("picked", g_last.picked, picked,
                "players who have chosen; the round starts when this reaches "
                "the player count");
            g_last.picked = picked;
        }
        const int spinner = int(int8_t(read_byte(state + kSelSpinner)));
        if (spinner != g_last.spinner) {
            say("spinner", g_last.spinner, spinner,
                "the local cursor; +0x32 is the statue type");
            g_last.spinner = spinner;
        }
    }

    /* THE CARTRIDGE'S OWN "a wireless session is up" BYTE. Two dereferences and
     * the pointer is checked: the owning object does not exist before the game builds
     * it, and reading through null would report a flag that is not there yet as
     * though it were clear. */
    int wireless = -1;
    {
        const uint32_t owner =
            bus_device_read32(0, kLsw2WirelessOwner + kLsw2WirelessOwnerField);
        if (ram_pointer(owner))
            wireless = int(read_byte(owner + kWirelessUpField));
    }
    if (wireless != g_last.wireless) {
        say("wireless", g_last.wireless, wireless,
            wireless > 0 ? "a session is up - pause row 1 reads Shut Off Wireless"
                         : "no session - pause row 1 reads Host Wireless");
        g_last.wireless = wireless;
    }

    /* THE MESSAGE LAYER'S GUARD, and the local station's first sequence byte.
     * See the note above kLsw2NetCountObject: this one byte decides whether the whole
     * write-a-packet route exists. */
    const int netplayers = int(read_byte(kLsw2NetCountObject + kLsw2NetPlayerCount));
    if (netplayers != g_last.netplayers) {
        say("netplayers", g_last.netplayers, netplayers,
            netplayers > 1 ? "> 1: the receive pump RUNS - packets can be "
                             "delivered by writing them"
                           : "the pump is skipped; netcount.cpp should have "
                             "raised this if a session is up");
        g_last.netplayers = netplayers;
    }
    {
        const int station = int(read_byte(kLsw2NetCountObject + kLsw2NetLocalStation));
        if (station >= 0 && station < 4) {
            const int seq = int(read_byte(kLsw2MsgOwner +
                                          uint32_t(station) * kLsw2MsgSeqStride +
                                          kLsw2MsgSeqBase));
            if (seq != g_last.netseq) {
                say("netseq", g_last.netseq, seq,
                    "the local station's slot-0 sequence - if this moves, "
                    "packets really are being dispatched");
                g_last.netseq = seq;
            }
        }
    }

    /* THE FOUR APPEARANCE RECORDS. Halfwords, read as halfwords - the whole
     * record is 0x40 bytes and reading it as words would report the neighbour
     * as part of the value, which has been this project's most repeated
     * mistake. */
    for (int slot = 0; slot < kAppearanceSlotCount; ++slot) {
        const uint32_t rec = kLsw2AppearanceRecord + uint32_t(slot) * kLsw2AppearanceStride;
        uint16_t now[1 + kSlotParts];
        for (int f = 0; f <= kSlotParts; ++f) {
            const uint32_t a = rec + uint32_t(f) * 2u;
            now[f] = uint16_t((bus_device_read32(0, a & ~3u) >>
                               ((a & 2u) * 8u)) & 0xFFFFu);
        }
        bool same = g_last.slot_seen[slot];
        for (int f = 0; same && f <= kSlotParts; ++f)
            same = (now[f] == g_last.slot[slot][f]);
        if (same) continue;

        std::fprintf(stderr, "[diag] slot %d       char %u%s  parts",
                     slot, unsigned(now[0]),
                     now[0] == kSlotNobody ? " (nobody)" : "");
        for (int f = 1; f < kSlotParts; ++f)
            std::fprintf(stderr, " %u", unsigned(now[f]));
        /* The candidate sixth, marked so nobody reads it as established. */
        std::fprintf(stderr, "   [+0x0C=%u, sixth?]\n", unsigned(now[kSlotParts]));

        for (int f = 0; f <= kSlotParts; ++f) g_last.slot[slot][f] = now[f];
        g_last.slot_seen[slot] = true;
    }

    /* THE ROUND COUNTER. Message 3 writes it on every machine from the same
     * word, so it is both "a round began" and a one-word agreement check. */
    const long round = long(bus_device_read32(0, kLsw2SequenceCounter));
    if (round != g_last.round) {
        std::fprintf(stderr, "[diag] %-14s %ld -> %ld   %s\n", "round",
                     g_last.round, round,
                     "message 3 raised a round");
        g_last.round = round;
    }
}
